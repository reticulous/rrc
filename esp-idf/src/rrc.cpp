/**
 * rrc — Reticulum Relay Chat client task.
 *
 * The node is the client. One task holds a Link to each connected hub (a
 * "slot", at most RRC_MAX_SLOTS), keeps the room logs, counts what is unread
 * and paces what it sends; the web window, the LCD app and the CLI are views
 * over the storage it publishes and writers of the commands it consumes.
 *
 *   session     rnsdLinkOpen(hub, "rrc.hub") → LINKIDENTIFY (queued at once,
 *               rnsd sends it ahead of our data) → on rnsd.links.rrc<N>.state
 *               "active" a HELLO, resent every 4 s up to 5 times → WELCOME →
 *               rejoin every room wanted on that hub → live. A room is in on
 *               our own JOINED or on the "room <r>: …" notice that follows
 *               it, since a hub drops a JOINED whose member list outgrows
 *               the Link MDU. A lost Link is a
 *               new session after a backoff of 5 s · 15 s · 45 s · 2 min ·
 *               5 min (±20 %).
 *   pacing      everything sent to a hub goes through a token bucket that
 *               mirrors the hub's own (full at start, refilled at the WELCOME
 *               rate), holding a reserve for PONG and HELLO. ERROR "rate
 *               limited" names no packet, so it empties ours and re-queues
 *               every line the hub has not echoed yet.
 *   delivery    a line is queued, then relayed when the hub's echo carries its
 *               id back, or failed when no echo comes within max(15 s, 4 × the
 *               round trip) or the Link drops first.
 *   quiet cmds  /who and /list are sent for the member pane and the room
 *               directory; the reply is parsed and not shown. /list goes
 *               after WELCOME, /who on entering a room and on rrc.cmd.who.
 *               A JOINED or PARTED that names the identity, or a PARTED
 *               whose nick is one member's, settles the roster itself; any
 *               other leaves it stale, and the /who that refreshes it waits
 *               until the room has been quiet for RRC_WHO_SETTLE_MS, a
 *               surface holds a rrc.cmd.watch lease on it (its member list
 *               in front of somebody), and RRC_WHO_FLOOR_MS has passed
 *               since the room's last /who. A reply too long for one packet may come as
 *               several NOTICEs (each line its own, a long line cut into
 *               fixed-size pieces anywhere); what follows its first line
 *               within RRC_CONT_MS is joined to it. rrcd's JOINED carries the
 *               joiner's nick but not their identity, so /who is also what
 *               turns a nick into the 12-hex identity prefix the roster keys on.
 *   announces   rrc.hub announces, app_data {"proto":"rrc","v":1,"hub":name},
 *               feed rrc.heard.<hub>.
 *
 * Storage map: see rrc.h. Everything here runs on the rrc task: storage
 * subscriptions and ITS callbacks dispatch through its one itsPoll.
 */
#include "rrc.h"
#include "rrc_cbor.h"

#include "spangap.h"
#include "ports.h"
#include "rnsd.h"
#include "mem.h"
#include "fs.h"
#include "storage_db.h"
#if CONFIG_STRADDLE_AUDIO
#include "audio.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <map>
#include <algorithm>

using rrccbor::Value;
using rrccbor::Writer;

static const char* TAG = "rrc";

#define RRC_ASPECT         "rrc.hub"
#define RRC_CLIENT_NAME    "reticulous"
#define RRC_CLIENT_VER     "0.1.0"
#define RRC_TAG_PFX        "rrc"            /* link tags rrc0..rrc7 */
#define RRC_LXMF_SLOTS     10
#define RRC_HEARD_MAX      64
#define RRC_NOTES          16               /* hub notice ring per hub */
#define RRC_PRES_RING      32               /* join/leave ring per room */
#define RRC_DEDUPE         64
#define RRC_HELLO_EVERY_MS 4000
#define RRC_HELLO_TRIES    5
#define RRC_QUIET_MS       10000
#define RRC_CONT_MS        3000
#define RRC_WHO_SETTLE_MS  10000            /* quiet after a join/leave before /who */
#define RRC_WHO_FLOOR_MS   60000            /* at most one presence /who a minute */
#define RRC_WATCH_LEASE_MS 45000
#define RRC_RESERVE        4.0
#define RRC_TICK_MS        500

/* Message types and envelope keys (RRC spec + rrcd's extensions). */
enum : uint64_t {
    K_V = 0, K_T = 1, K_ID = 2, K_TS = 3, K_SRC = 4, K_ROOM = 5, K_BODY = 6, K_NICK = 7,
};
enum : uint64_t {
    T_HELLO = 1, T_WELCOME = 2, T_JOIN = 10, T_JOINED = 11, T_PART = 12, T_PARTED = 13,
    T_MSG = 20, T_NOTICE = 21, T_ACTION = 22, T_PING = 30, T_PONG = 31, T_ERROR = 40,
    T_RESOURCE_ENVELOPE = 50,
};

/* Log record kinds and delivery states (s.rrc.log.<rid>.<seq>.kind/.state). */
enum : int { KIND_MSG = 0, KIND_ACTION = 1, KIND_NOTICE = 2, KIND_PRESENCE = 3, KIND_SYSTEM = 4 };
enum : int { ST_QUEUED = 0, ST_RELAYED = 1, ST_FAILED = 2, ST_RECEIVED = 3 };
enum : int { NOTIFY_ALL = 0, NOTIFY_MENTIONS = 1, NOTIFY_OFF = 2 };

/* ─────────────── small helpers ─────────────── */

static int64_t nowMs()   { return esp_timer_get_time() / 1000; }
static int     monoS()   { return (int)(nowMs() / 1000); }
static uint32_t wallS()
{
    time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;
}

static std::string hexOf(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}
static std::string hexOf(const std::string& b) { return hexOf((const uint8_t*)b.data(), b.size()); }

static bool hexTo(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned x;
        if (!std::isxdigit((unsigned char)s[2 * i]) || !std::isxdigit((unsigned char)s[2 * i + 1])) return false;
        if (std::sscanf(s.c_str() + 2 * i, "%2x", &x) != 1) return false;
        out[i] = (uint8_t)x;
    }
    return true;
}

/* Typed hex may be grouped by spaces anywhere, and a hash may be given in full
 * (64 hex), whose leading 32 are the truncated hash. */
static std::string unspaced(std::string s)
{
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
    if (s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); }))
        s.resize(32);
    return s;
}

static bool isHubHash(const std::string& s)
{
    uint8_t b[16];
    return hexTo(s, b, 16);
}

static std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string trim(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

/* Network text in the log and in storage: no C0 controls but newline/tab (a
 * NOTICE may be several lines), and never a NUL. */
static std::string clean(std::string_view in, bool oneLine = false)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c == '\n' || c == '\t') out += oneLine ? ' ' : (char)c;
        else if (c < 0x20 || c == 0x7f) continue;
        else out += (char)c;
    }
    return out;
}

/* Storage values may not hold '|' where we pack fields with it. */
static std::string noBar(std::string s)
{
    for (char& c : s) if (c == '|') c = '/';
    return s;
}

/* A room's wire name: trimmed, no leading '#', lowercased — what NomadNet, the
 * Go hub and the mobile app all send, so "#Mesh" typed here is the room they
 * call mesh. Frontends show it with the '#'. */
static std::string normRoom(const std::string& r)
{
    std::string s = trim(r);
    while (!s.empty() && s[0] == '#') s.erase(0, 1);
    return lower(trim(s));
}

/* <rid>: the first four bytes of sha256(hub ‖ lowercased room), in hex. */
static std::string roomId(const std::string& hubHex, const std::string& room)
{
    uint8_t hub[16];
    if (!hexTo(hubHex, hub, 16)) return "";
    std::string n = normRoom(room);
    std::vector<uint8_t> in(hub, hub + 16);
    in.insert(in.end(), n.begin(), n.end());
    uint8_t h[32];
    rnsdSha256(in.data(), in.size(), h);
    return hexOf(h, 4);
}

static std::string seqKey(uint32_t seq)
{
    char b[12];
    std::snprintf(b, sizeof b, "%08u", (unsigned)seq);
    return b;
}

/* Whole-word, case-insensitive: does `text` mention `nick`? A leading @ is
 * allowed and is not part of the word. */
static bool mentions(const std::string& text, const std::string& nick)
{
    if (nick.empty()) return false;
    std::string t = lower(text), n = lower(nick);
    auto word = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-' || (unsigned char)c >= 0x80; };
    for (size_t at = t.find(n); at != std::string::npos; at = t.find(n, at + 1)) {
        bool l = at == 0 || !word(t[at - 1]);
        size_t e = at + n.size();
        bool r = e >= t.size() || !word(t[e]);
        if (l && r) return true;
    }
    return false;
}

static size_t utf8Prefix(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s.size();
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

/* ─────────────── identities ─────────────── */

static std::string identKeyForSel(const std::string& sel)
{
    if (sel.size() >= 5 && sel.rfind("lxmf", 0) == 0 && std::isdigit((unsigned char)sel[4])) {
        int n = std::atoi(sel.c_str() + 4);
        if (n >= 0 && n < RRC_LXMF_SLOTS) {
            char k[48];
            std::snprintf(k, sizeof k, "secrets.lxmf.id.%d.privkey", n);
            return k;
        }
    }
    return "secrets.rnsd.identity";
}

static bool lxmfSlotLive(int n)
{
    char k[48];
    std::snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
    if (storageGetStr(k, "") == "group") return false;
    std::snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
    return isHubHash(storageGetStr(k, ""));
}

/** The one LXMF identity to use when there is exactly one; "node" when there
 *  are none; "" when there are several, which is the operator's to choose. */
static std::string identDefault()
{
    int found = -1;
    for (int n = 0; n < RRC_LXMF_SLOTS; ++n) {
        if (!lxmfSlotLive(n)) continue;
        if (found >= 0) return "";
        found = n;
    }
    if (found < 0) return "node";
    return std::string("lxmf") + (char)('0' + found);
}

static std::string identLabel(const std::string& sel)
{
    if (sel.rfind("lxmf", 0) == 0) {
        std::string n = sel.substr(4);
        std::string name = storageGetStr(("s.lxmf.id." + n + ".display_name").c_str(), "");
        return name.empty() ? "LXMF identity " + n : name;
    }
    return "this node";
}

/* ─────────────── room directory (RAM mirror of s.rrc.rooms) ─────────────── */

struct Room {
    std::string hub, name;
    uint32_t    seq = 0, read_seq = 0, unread = 0, mentions = 0, last_ts = 0;
    uint32_t    logSeq = 0;        /* newest logged line; seq runs ahead by joins/leaves */
    int         joined = 0, notify = NOTIFY_MENTIONS;
    /* RAM only */
    std::string state = "out";     /* out | joining | in */
    uint32_t    floor = 0;         /* lowest seq the log may still hold; 0 = not yet known */
};
static std::map<std::string, Room> s_rooms;   /* rid → Room */

static std::string roomKey(const std::string& rid, const char* field)
{
    return "s.rrc.rooms." + rid + "." + field;
}

static void roomSave(const std::string& rid, const Room& r)
{
    storageBegin();
    storageSet(roomKey(rid, "hub").c_str(), r.hub.c_str());
    storageSet(roomKey(rid, "name").c_str(), r.name.c_str());
    storageSet(roomKey(rid, "seq").c_str(), (int)r.logSeq);
    storageSet(roomKey(rid, "read_seq").c_str(), (int)r.read_seq);
    storageSet(roomKey(rid, "unread").c_str(), (int)r.unread);
    storageSet(roomKey(rid, "mentions").c_str(), (int)r.mentions);
    storageSet(roomKey(rid, "last_ts").c_str(), (int)r.last_ts);
    storageSet(roomKey(rid, "joined").c_str(), r.joined);
    storageSet(roomKey(rid, "notify").c_str(), r.notify);
    storageEnd();
}

static void roomPublishState(const std::string& rid, const char* state)
{
    s_rooms[rid].state = state;
    storageSet(("rrc.rooms." + rid + ".state").c_str(), state);
}

static void totalsPublish()
{
    int unread = 0, ment = 0;
    for (auto& [rid, r] : s_rooms) {
        if (r.notify == NOTIFY_OFF) continue;
        unread += (int)r.unread;
        ment   += (int)r.mentions;
    }
    storageBegin();
    storageSet("rrc.unread", unread);
    storageSet("rrc.mentions", ment);
    storageEnd();
}

static std::map<std::string, Room>* s_loadInto = nullptr;
static void roomLoadLeaf(const char* key, const char* val)
{
    if (!s_loadInto || !key) return;
    const char* tail = key + sizeof("s.rrc.rooms.") - 1;
    const char* dot = std::strchr(tail, '.');
    if (!dot) return;
    std::string rid(tail, dot - tail), f = dot + 1;
    Room& r = (*s_loadInto)[rid];
    std::string v = val ? val : "";
    if (f == "hub") r.hub = v;
    else if (f == "name") r.name = v;
    else if (f == "seq") r.seq = r.logSeq = (uint32_t)std::atoll(v.c_str());
    else if (f == "read_seq") r.read_seq = (uint32_t)std::atoll(v.c_str());
    else if (f == "unread") r.unread = (uint32_t)std::atoll(v.c_str());
    else if (f == "mentions") r.mentions = (uint32_t)std::atoll(v.c_str());
    else if (f == "last_ts") r.last_ts = (uint32_t)std::atoll(v.c_str());
    else if (f == "joined") r.joined = std::atoi(v.c_str());
    else if (f == "notify") r.notify = std::atoi(v.c_str());
}

static void roomsLoad()
{
    s_loadInto = &s_rooms;
    storageForEach("s.rrc.rooms.", roomLoadLeaf);
    s_loadInto = nullptr;
    for (auto it = s_rooms.begin(); it != s_rooms.end();) {
        if (!isHubHash(it->second.hub) || it->second.name.empty()) it = s_rooms.erase(it);
        else ++it;
    }
    for (auto& [rid, r] : s_rooms) storageSet(("rrc.rooms." + rid + ".state").c_str(), "out");
    totalsPublish();
}

/* ─────────────── the log ─────────────── */

struct LogLine {
    int         kind = KIND_MSG, state = ST_RECEIVED;
    bool        mention = false, own = false;
    std::string src12, id, nick, text;
    uint32_t    ts = 0;
};

static void playChirp();

static uint32_t s_oldest = 0;
static size_t   s_oldestPfx = 0;
static void oldestLeaf(const char* key, const char*)
{
    if (!key || std::strlen(key) <= s_oldestPfx) return;
    uint32_t seq = (uint32_t)std::strtoul(key + s_oldestPfx, nullptr, 10);
    if (seq && (!s_oldest || seq < s_oldest)) s_oldest = seq;
}

/** The lowest seq in a room's log; 0 when it is empty. */
static uint32_t logOldest(const std::string& rid)
{
    std::string pfx = "s.rrc.log." + rid + ".";
    s_oldest = 0;
    s_oldestPfx = pfx.size();
    storageForEach(pfx.c_str(), oldestLeaf);
    return s_oldest;
}

/** Append a line to a room's log, prune past s.rrc.keep, and keep the
 *  directory's counters and preview current. Returns the line's seq. */
static uint32_t logAppend(const std::string& rid, const LogLine& l)
{
    auto it = s_rooms.find(rid);
    if (it == s_rooms.end()) return 0;
    Room& r = it->second;
    uint32_t seq = ++r.seq;
    r.logSeq = seq;
    std::string base = "s.rrc.log." + rid + "." + seqKey(seq) + ".";
    uint32_t ts = l.ts ? l.ts : wallS();

    bool counts = !l.own && (l.kind == KIND_MSG || l.kind == KIND_ACTION || l.kind == KIND_NOTICE);
    if (!r.floor) r.floor = logOldest(rid);
    storageBegin();
    storageSet((base + "ts").c_str(), (int)ts);
    storageSet((base + "kind").c_str(), l.kind);
    storageSet((base + "state").c_str(), l.state);
    storageSet((base + "mention").c_str(), l.mention ? 1 : 0);
    storageSet((base + "own").c_str(), l.own ? 1 : 0);
    storageSet((base + "src").c_str(), l.src12.c_str());
    storageSet((base + "id").c_str(), l.id.c_str());
    storageSet((base + "nick").c_str(), l.nick.c_str());
    storageSet((base + "text").c_str(), l.text.c_str());

    /* Every line at or below seq − keep goes: the one that just left the
     * window, and all a lowered s.rrc.keep left behind it. Seqs have holes
     * (a join or leave takes one and is never logged), so the lowest line
     * still kept is tracked, found by a scan on the room's first line after
     * boot. */
    int keep = storageGetInt("s.rrc.keep", 500);
    if (keep > 0 && seq > (uint32_t)keep) {
        uint32_t top = seq - (uint32_t)keep;
        for (uint32_t old = r.floor; old && old <= top; ++old)
            storageUnset(("s.rrc.log." + rid + "." + seqKey(old)).c_str());
        if (r.floor <= top) r.floor = top + 1;
    }

    if (l.kind != KIND_PRESENCE && l.kind != KIND_SYSTEM) {
        r.last_ts = ts;
        std::string pv;
        if (l.kind == KIND_ACTION) pv = "* " + l.nick + " " + l.text;
        else if (l.nick.empty()) pv = l.text;
        else pv = l.nick + ": " + l.text;
        pv = clean(pv, true);
        pv.resize(utf8Prefix(pv, 80));
        storageSet(roomKey(rid, "preview").c_str(), pv.c_str());
        storageSet(roomKey(rid, "last_ts").c_str(), (int)ts);
    }
    if (l.own) r.read_seq = seq;
    if (counts) {
        r.unread++;
        if (l.mention) r.mentions++;
        storageSet(roomKey(rid, "unread").c_str(), (int)r.unread);
        storageSet(roomKey(rid, "mentions").c_str(), (int)r.mentions);
    }
    storageSet(roomKey(rid, "seq").c_str(), (int)seq);
    storageSet(roomKey(rid, "read_seq").c_str(), (int)r.read_seq);
    storageEnd();

    if (counts) {
        totalsPublish();
        if (r.notify == NOTIFY_ALL || (r.notify == NOTIFY_MENTIONS && l.mention)) {
            std::string snd = storageGetStr("s.rrc.sound", "mentions");
            if (snd == "all" || (snd == "mentions" && l.mention)) playChirp();
        }
    }
    return seq;
}

/** A join or leave. It takes the next seq, as a line would, but is never
 *  logged and never reaches flash: it goes to a RAM ring per room,
 *  rrc.rooms.<rid>.pres.<n> = "<ts>|<seq>|<j|l>|<id12>|<nick>", with pres_next
 *  counting every one written. A frontend shows those that arrive while it
 *  has the room open, in the log's hole at their seq. */
static void presenceNote(const std::string& rid, bool joined, const std::string& nick,
                         const std::string& id12)
{
    auto it = s_rooms.find(rid);
    if (it == s_rooms.end()) return;
    uint32_t seq = ++it->second.seq;
    std::string base = "rrc.rooms." + rid + ".";
    int next = storageGetInt((base + "pres_next").c_str(), 0);
    char head[48];
    std::snprintf(head, sizeof head, "%u|%u|%c|", (unsigned)wallS(), (unsigned)seq,
                  joined ? 'j' : 'l');
    storageBegin();
    storageSet((base + "pres." + std::to_string(next % RRC_PRES_RING)).c_str(),
               (head + id12 + "|" + clean(nick, true)).c_str());
    storageSet((base + "pres_next").c_str(), next + 1);
    storageEnd();
}

static void logSetState(const std::string& rid, uint32_t seq, int state)
{
    std::string k = "s.rrc.log." + rid + "." + seqKey(seq);
    if (!storageExists((k + ".ts").c_str())) return;
    storageSet((k + ".state").c_str(), state);
}

static void playChirp()
{
#if CONFIG_STRADDLE_AUDIO
    bool lcdAwake = storageExists("sys.standby") && storageGetInt("sys.standby", 0) == 0;
    audioPlayWav(FS_FIXED "/rrc/chirp.wav", lcdAwake ? 50 : 100);
#endif
}

/* ─────────────── hubs and slots ─────────────── */

struct Outbound {
    std::vector<uint8_t> bytes;
    bool                 reserve = false;   /* PONG / HELLO: may spend the reserve */
    std::string          rid;               /* chat lines: where the line is logged */
    uint32_t             seq = 0;
    std::string          id;                /* message id hex */
};

struct Pending {                            /* sent, waiting for the hub's echo */
    std::string rid, id;
    uint32_t    seq;
    int64_t     sentMs;
    std::vector<uint8_t> bytes;
};

struct Quiet { std::string what; int64_t until; };  /* "who:<room>" | "list" */

struct ResExpect { std::string id, kind, sha, room; bool hasRoom = false; uint32_t size = 0; int64_t until = 0; };

enum SlotState { SL_FREE, SL_LINKING, SL_HELLO, SL_LIVE };

struct Slot {
    SlotState   st = SL_FREE;
    std::string hub;
    int         handle = -1;
    std::string sel, identKey;
    uint8_t     src[16] = {};
    std::string nick;
    int         helloTries = 0;
    int64_t     helloNextMs = 0;
    /* hub limits from WELCOME */
    int         limBody = 350, limNick = 32, limRoom = 64, limRooms = 32, limRate = 240;
    double      tokens = 240;
    int64_t     refillMs = 0;
    int64_t     pauseUntil = 0;
    std::deque<Outbound> outq;
    std::vector<Pending> pending;
    std::vector<Quiet>   quiet;
    std::deque<std::string> seen;           /* last RRC_DEDUPE message ids */
    std::vector<ResExpect>  res;
    bool        motdTaken = false;
    int64_t     motdUntil = 0;
    std::string lastCmdRid;                 /* where a typed /command came from */
    int64_t     lastCmdUntil = 0;
    int         rttMs = 0;
    std::string pingId;
    int64_t     pingSentMs = 0, lastPingMs = 0;
    std::map<std::string, int64_t> whoDue;  /* rid → roster stale; /who no sooner than */
    std::map<std::string, int64_t> whoLast; /* rid → when /who last went */
    /* a /who or /list reply that may still be arriving in pieces */
    std::string contKind, contText;         /* "who" | "list" | "" */
    int64_t     contUntil = 0;
    int         contNote = -1;              /* its hub note; -1 = asked quietly */
    std::string contNoteRid;
};

static Slot s_slots[RRC_MAX_SLOTS];

struct Retry { int attempt = 0; int64_t at = 0; };
static std::map<std::string, Retry> s_retry;   /* hub → next reconnect */
static std::map<std::string, std::string> s_held;   /* hub → why (ban) */

static std::string hubKey(const std::string& hub, const char* f) { return "s.rrc.hubs." + hub + "." + f; }
static std::string hubRam(const std::string& hub, const char* f) { return "rrc.hubs." + hub + "." + f; }

static Slot* slotFor(const std::string& hub)
{
    for (auto& s : s_slots) if (s.st != SL_FREE && s.hub == hub) return &s;
    return nullptr;
}

static int slotIndex(const Slot* s) { return (int)(s - s_slots); }

static std::string slotTag(const Slot* s)
{
    return RRC_TAG_PFX + std::to_string(slotIndex(s));
}

static std::string hubName(const std::string& hub)
{
    std::string n = storageGetStr(hubKey(hub, "name").c_str(), "");
    if (n.empty()) n = storageGetStr(hubRam(hub, "name").c_str(), "");
    if (n.empty()) {
        std::string h = storageGetStr(("rrc.heard." + hub).c_str(), "");
        size_t p1 = h.find('|'), p2 = p1 == std::string::npos ? p1 : h.find('|', p1 + 1);
        if (p2 != std::string::npos) n = h.substr(p2 + 1);
    }
    return n.empty() ? hub.substr(0, 8) : n;
}

static void hubStatus(const std::string& hub, const char* state, const std::string& status)
{
    storageBegin();
    storageSet(hubRam(hub, "state").c_str(), state);
    storageSet(hubRam(hub, "status").c_str(), status.c_str());
    storageSet(hubRam(hub, "label").c_str(), hubName(hub).c_str());
    storageEnd();
}

/** One line in the hub's notice ring. `rid` ties it to the room it answers,
 *  "" for the hub as a whole. RAM only. `at` rewrites that slot rather than
 *  taking the next; the slot written is returned. */
static int hubNote(const std::string& hub, const std::string& rid, const std::string& text, int at = -1)
{
    static std::map<std::string, int> next;
    int n = at >= 0 ? at : next[hub]++ % RRC_NOTES;
    char v[16];
    std::snprintf(v, sizeof v, "%u|", (unsigned)wallS());
    std::string val = std::string(v) + rid + "|" + clean(text);
    storageSet((hubRam(hub, "notes.") + std::to_string(n)).c_str(), val.c_str());
    storageSet(hubRam(hub, "notes_next").c_str(), next[hub]);
    return n;
}

static std::string nickFor(const std::string& hub, const std::string& sel)
{
    std::string n = storageGetStr(hubKey(hub, "nick").c_str(), "");
    if (n.empty()) n = storageGetStr("s.rrc.nick", "");
    if (n.empty() && sel.rfind("lxmf", 0) == 0)
        n = storageGetStr(("s.lxmf.id." + sel.substr(4) + ".display_name").c_str(), "");
    if (n.empty()) n = "reticulous";
    n = clean(n, true);
    for (char& c : n) if (c == ' ') c = '_';
    n.resize(utf8Prefix(n, 32));
    return n;
}

/* ─────────────── envelopes and the outbound queue ─────────────── */

static std::string newId()
{
    uint8_t b[8];
    esp_fill_random(b, sizeof b);
    return std::string((const char*)b, 8);
}

/* Build an envelope. `body` writes key 6's value (nullptr = no body). */
template <typename BodyFn>
static std::vector<uint8_t> envelope(const Slot& s, uint64_t type, const std::string& id,
                                     const std::string* room, BodyFn body, bool withBody)
{
    Writer w;
    size_t n = 5 + (room ? 1 : 0) + (withBody ? 1 : 0) + (s.nick.empty() ? 0 : 1);
    w.map(n);
    w.uint(K_V);   w.uint(1);
    w.uint(K_T);   w.uint(type);
    w.uint(K_ID);  w.bytes((const uint8_t*)id.data(), id.size());
    uint64_t ts = wallS() ? (uint64_t)wallS() * 1000 : (uint64_t)nowMs();
    w.uint(K_TS);  w.uint(ts);
    w.uint(K_SRC); w.bytes(s.src, 16);
    if (room) { w.uint(K_ROOM); w.text(*room); }
    if (withBody) { w.uint(K_BODY); body(w); }
    if (!s.nick.empty()) { w.uint(K_NICK); w.text(s.nick); }
    return std::move(w.out);
}

static void slotRefill(Slot& s)
{
    int64_t now = nowMs();
    double per = (double)std::max(1, s.limRate);
    s.tokens = std::min(per, s.tokens + (now - s.refillMs) * per / 60000.0);
    s.refillMs = now;
}

static void slotDrain(Slot& s)
{
    if (s.handle < 0 || s.st == SL_LINKING) return;
    if (nowMs() < s.pauseUntil) return;
    slotRefill(s);
    while (!s.outq.empty()) {
        Outbound& o = s.outq.front();
        if (s.st != SL_LIVE && !o.reserve) break;           /* only HELLO before WELCOME */
        double need = o.reserve ? 1.0 : 1.0 + RRC_RESERVE;
        if (s.tokens < need) break;
        if (itsSend(s.handle, o.bytes.data(), o.bytes.size(), 0) == 0) break;   /* ITS full: next tick */
        s.tokens -= 1.0;
        if (!o.id.empty() && !o.rid.empty())
            s.pending.push_back({ o.rid, o.id, o.seq, nowMs(), std::move(o.bytes) });
        s.outq.pop_front();
    }
}

static void slotSend(Slot& s, std::vector<uint8_t> bytes, bool reserve,
                     const std::string& rid = "", uint32_t seq = 0, const std::string& idHex = "")
{
    Outbound o;
    o.bytes = std::move(bytes);
    o.reserve = reserve;
    o.rid = rid; o.seq = seq; o.id = idHex;
    if (reserve) s.outq.push_front(std::move(o));
    else s.outq.push_back(std::move(o));
    slotDrain(s);
}

static void sendText(Slot& s, uint64_t type, const std::string& room, const std::string& text)
{
    std::string id = newId();
    auto b = envelope(s, type, id, &room, [&](Writer& w) { w.text(text); }, true);
    slotSend(s, std::move(b), false);
}

/** A slash command sent for our own bookkeeping; its answer is consumed. */
static void sendQuiet(Slot& s, const std::string& what, const std::string& cmd)
{
    s.quiet.push_back({ what, nowMs() + RRC_QUIET_MS });
    std::string id = newId();
    auto b = envelope(s, T_MSG, id, nullptr, [&](Writer& w) { w.text(cmd); }, true);
    slotSend(s, std::move(b), false);
}

/* The room whose member list each surface (web, LCD) has in front of somebody,
 * as a lease the surface renews (rrc.cmd.watch). */
struct Watch { std::string rid; int64_t until = 0; };
static Watch s_watch[2];

static bool roomWatched(const std::string& rid)
{
    for (auto& w : s_watch)
        if (w.rid == rid && nowMs() < w.until) return true;
    return false;
}

static void sendWho(Slot& s, const std::string& rid)
{
    s.whoDue.erase(rid);
    s.whoLast[rid] = nowMs();
    sendQuiet(s, "who:" + rid, "/who " + s_rooms[rid].name);
}

static void sendJoin(Slot& s, const std::string& rid)
{
    Room& r = s_rooms[rid];
    std::string key = storageGetStr(("secrets.rrc.keys." + rid).c_str(), "");
    std::string id = newId();
    auto b = envelope(s, T_JOIN, id, &r.name, [&](Writer& w) { w.text(key); }, !key.empty());
    roomPublishState(rid, "joining");
    storageSet(("rrc.rooms." + rid + ".error").c_str(), "");
    slotSend(s, std::move(b), false);
}

static void sendHello(Slot& s)
{
    std::string id = newId();
    auto b = envelope(s, T_HELLO, id, nullptr, [&](Writer& w) {
        w.map(3);
        w.uint(0); w.text(RRC_CLIENT_NAME);
        w.uint(1); w.text(RRC_CLIENT_VER);
        w.uint(2); w.map(2); w.uint(0); w.boolean(true); w.uint(1); w.boolean(true);
    }, true);
    s.helloTries++;
    s.helloNextMs = nowMs() + RRC_HELLO_EVERY_MS;
    slotSend(s, std::move(b), true);
}

/* ─────────────── connect / disconnect ─────────────── */

static void scheduleRetry(const std::string& hub);

static void publishWho(const std::string& rid);

static void slotFree(Slot& s, bool lost)
{
    std::string hub = s.hub;
    if (s.handle >= 0) { itsDisconnect(s.handle); s.handle = -1; }
    /* Lines the hub never echoed failed with the Link. */
    for (auto& p : s.pending) logSetState(p.rid, p.seq, ST_FAILED);
    for (auto& o : s.outq) if (!o.rid.empty()) logSetState(o.rid, o.seq, ST_FAILED);
    for (auto& [rid, r] : s_rooms) {
        if (r.hub != hub) continue;
        roomPublishState(rid, "out");
        storageDeleteTree(("rrc.rooms." + rid + ".who").c_str());
        storageSet(("rrc.rooms." + rid + ".members").c_str(), 0);
    }
    s = Slot{};
    if (lost && storageGetInt(hubKey(hub, "auto").c_str(), 0) && !s_held.count(hub))
        scheduleRetry(hub);
}

static void onLinkRecv(int handle, size_t);
static void onLinkDisc(int ref);

static int connectedCount()
{
    int n = 0;
    for (auto& s : s_slots) if (s.st != SL_FREE) n++;
    return n;
}

static bool slotOpen(const std::string& hub)
{
    if (slotFor(hub)) return true;
    int maxHubs = std::clamp(storageGetInt("s.rrc.max_hubs", 4), 1, RRC_MAX_SLOTS);
    if (connectedCount() >= maxHubs) {
        hubStatus(hub, "idle", "Not connected: " + std::to_string(maxHubs) +
                  (maxHubs == 1 ? " hub is" : " hubs are") + " connected already");
        return false;
    }
    Slot* s = nullptr;
    for (auto& c : s_slots) if (c.st == SL_FREE) { s = &c; break; }
    if (!s) return false;

    std::string sel = storageGetStr(hubKey(hub, "ident").c_str(), "");
    if (sel.empty()) {
        sel = identDefault();
        if (sel.empty()) {
            hubStatus(hub, "need_ident", "Choose who to chat as");
            return false;
        }
        storageSet(hubKey(hub, "ident").c_str(), sel.c_str());
    }
    std::string ik = identKeyForSel(sel);
    uint8_t src[16];
    if (!rnsdIdentityHash(ik.c_str(), src)) {
        hubStatus(hub, "idle", "Not connected: the identity " + identLabel(sel) + " is not available");
        return false;
    }
    uint8_t dh[16];
    hexTo(hub, dh, 16);

    *s = Slot{};
    s->hub = hub;
    s->sel = sel;
    s->identKey = ik;
    std::memcpy(s->src, src, 16);
    s->nick = nickFor(hub, sel);
    s->refillMs = nowMs();
    std::string tag = slotTag(s);
    int h = rnsdLinkOpen(dh, RRC_ASPECT, "", tag.c_str(), 0, 0, slotIndex(s), onLinkRecv, onLinkDisc);
    if (h < 0) {
        *s = Slot{};
        hubStatus(hub, "idle", "Not connected: no free link");
        scheduleRetry(hub);
        return false;
    }
    s->handle = h;
    s->st = SL_LINKING;
    /* rnsd holds the identify and sends it at establishment, ahead of the
     * HELLO the "active" state below triggers. rrcd drops every packet of a
     * Link that has not identified. No hub proves the packets it receives,
     * so rnsd is told not to wait for proofs. */
    if (!rnsdLinkExpectProofs(tag.c_str(), false))
        warn("rrc %s: proof expectation could not be queued", hub.c_str());
    if (!rnsdLinkIdentify(tag.c_str(), ik == "secrets.rnsd.identity" ? "" : ik.c_str()))
        warn("rrc %s: identify could not be queued", hub.c_str());
    s_retry.erase(hub);
    storageBegin();
    storageSet(hubRam(hub, "as").c_str(), identLabel(sel).c_str());
    storageSet(hubRam(hub, "nick").c_str(), s->nick.c_str());
    storageSet(hubRam(hub, "self").c_str(), hexOf(src, 6).c_str());
    storageEnd();
    hubStatus(hub, "linking", "Connecting");
    info("rrc %s: linking as %s (%s)", hub.c_str(), sel.c_str(), s->nick.c_str());
    return true;
}

static void scheduleRetry(const std::string& hub)
{
    static const int ladder[] = { 5, 15, 45, 120, 300 };
    Retry& r = s_retry[hub];
    int base = ladder[std::min(r.attempt, 4)];
    int jitter = (int)(esp_random() % (unsigned)(base * 40 + 1)) - base * 20;   /* ±20 % in 1/100 s */
    r.at = nowMs() + base * 1000 + jitter * 10;
    r.attempt++;
    hubStatus(hub, "retry", "Retrying in " + std::to_string(base) + " s");
}

/* ─────────────── inbound ─────────────── */

static const std::string* textOf(const Value* v) { return v && v->isText() ? &v->s : nullptr; }

static std::string ridFor(const Slot& s, const std::string& room)
{
    std::string rid = roomId(s.hub, room);
    auto it = s_rooms.find(rid);
    return (it != s_rooms.end() && it->second.hub == s.hub) ? rid : "";
}

static bool seenBefore(Slot& s, const std::string& id)
{
    if (id.empty()) return false;
    if (std::find(s.seen.begin(), s.seen.end(), id) != s.seen.end()) return true;
    s.seen.push_back(id);
    if (s.seen.size() > RRC_DEDUPE) s.seen.pop_front();
    return false;
}

static void rosterSet(const std::string& rid, const std::string& id12, const std::string& nick,
                      const std::string& full)
{
    std::string k = "rrc.rooms." + rid + ".who." + id12;
    std::string old = storageGetStr(k.c_str(), "");
    std::string oldFull, oldLx;
    size_t p1 = old.find('|');
    if (p1 != std::string::npos) {
        size_t p2 = old.find('|', p1 + 1);
        oldFull = old.substr(p1 + 1, p2 == std::string::npos ? std::string::npos : p2 - p1 - 1);
        if (p2 != std::string::npos) oldLx = old.substr(p2 + 1);
    }
    std::string f = full.empty() ? oldFull : full;
    std::string lx = oldLx;
    if (lx.empty() && f.size() == 32) {
        uint8_t ih[16], d[16];
        if (hexTo(f, ih, 16) && rnsdDestinationHashFromIdentityHash(ih, "lxmf", "delivery", d))
            lx = hexOf(d, 16);
    }
    storageSet(k.c_str(), (noBar(nick) + "|" + f + "|" + lx).c_str());
}

static std::vector<std::pair<std::string, std::string>>* s_whoCollect = nullptr;
static void whoLeaf(const char* key, const char* val)
{
    if (!s_whoCollect || !key) return;
    const char* tail = std::strrchr(key, '.');
    if (tail) s_whoCollect->push_back({ tail + 1, val ? val : "" });
}

static std::vector<std::pair<std::string, std::string>> rosterOf(const std::string& rid)
{
    std::vector<std::pair<std::string, std::string>> out;
    s_whoCollect = &out;
    storageForEach(("rrc.rooms." + rid + ".who.").c_str(), whoLeaf);
    s_whoCollect = nullptr;
    return out;
}

static void publishWho(const std::string& rid)
{
    storageSet(("rrc.rooms." + rid + ".members").c_str(), (int)rosterOf(rid).size());
}

/* "members in <room>: nick (hex12), nick (hex12), <fullhex>, …" — rrcd's
 * /who reply. The list replaces the roster. */
static bool parseWho(Slot& s, const std::string& text, std::string& ridOut)
{
    static const char* P = "members in ";
    if (text.rfind(P, 0) != 0) return false;
    size_t colon = text.find(": ", std::strlen(P));
    if (colon == std::string::npos) return false;
    std::string room = text.substr(std::strlen(P), colon - std::strlen(P));
    std::string rid = ridFor(s, room);
    ridOut = rid;
    if (rid.empty()) return true;
    std::string list = text.substr(colon + 2);
    std::map<std::string, std::pair<std::string, std::string>> fresh;   /* id12 → nick, full */
    if (list != "(none)") {
        size_t at = 0;
        while (at < list.size()) {
            size_t e = list.find(", ", at);
            std::string item = list.substr(at, e == std::string::npos ? std::string::npos : e - at);
            at = e == std::string::npos ? list.size() : e + 2;
            item = trim(item);
            if (item.size() == 32 && isHubHash(item)) {
                fresh[item.substr(0, 12)] = { "", item };
                continue;
            }
            size_t lp = item.rfind(" (");
            if (lp != std::string::npos && item.back() == ')') {
                std::string id12 = item.substr(lp + 2, item.size() - lp - 3);
                if (id12.size() == 12) fresh[lower(id12)] = { item.substr(0, lp), "" };
            }
        }
    }
    auto old = rosterOf(rid);
    storageBegin();
    for (auto& [id12, v] : old)
        if (!fresh.count(id12)) storageUnset(("rrc.rooms." + rid + ".who." + id12).c_str());
    for (auto& [id12, nf] : fresh) rosterSet(rid, id12, nf.first, nf.second);
    storageEnd();
    publishWho(rid);
    return true;
}

/* "Registered public rooms:\n  #r - topic\n  #s" or "No public rooms registered". */
static bool parseList(Slot& s, const std::string& text)
{
    if (text.rfind("No public rooms registered", 0) == 0) {
        storageSet(hubRam(s.hub, "rooms_dir").c_str(), "");
        return true;
    }
    if (text.rfind("Registered public rooms:", 0) != 0) return false;
    std::string dir;
    size_t at = text.find('\n');
    while (at != std::string::npos) {
        size_t e = text.find('\n', at + 1);
        std::string line = trim(text.substr(at + 1, e == std::string::npos ? std::string::npos : e - at - 1));
        at = e;
        if (line.empty()) continue;
        size_t dash = line.find(" - ");
        std::string name = dash == std::string::npos ? line : line.substr(0, dash);
        std::string topic = dash == std::string::npos ? "" : line.substr(dash + 3);
        dir += noBar(name) + "|" + clean(topic, true) + "\n";
    }
    storageSet(hubRam(s.hub, "rooms_dir").c_str(), dir.c_str());
    return true;
}

static void roomEntered(Slot& s, const std::string& rid)
{
    Room& r = s_rooms[rid];
    roomPublishState(rid, "in");
    rosterSet(rid, hexOf(s.src, 6), s.nick, hexOf(s.src, 16));
    info("rrc %s: in %s", s.hub.c_str(), r.name.c_str());
    sendWho(s, rid);
}

/* "room <r>: registered; mode=+nt; topic=<t>" after a JOIN, and
 * "topic for <r> is now: <t>" when it changes. */
static bool parseRoomInfo(Slot& s, const std::string& text)
{
    if (text.rfind("room ", 0) == 0) {
        size_t c = text.find(": ");
        if (c == std::string::npos) return false;
        std::string rid = ridFor(s, text.substr(5, c - 5));
        if (rid.empty()) return false;
        if (s_rooms[rid].state == "joining") roomEntered(s, rid);
        size_t m = text.find("mode=", c), t = text.find("topic=", c);
        std::string mode = m == std::string::npos ? "" : text.substr(m + 5, text.find(';', m) - m - 5);
        std::string topic = t == std::string::npos ? "" : text.substr(t + 6);
        if (topic == "(none)") topic = "";
        if (mode == "(none)") mode = "";
        storageBegin();
        storageSet(("rrc.rooms." + rid + ".topic").c_str(), clean(topic, true).c_str());
        storageSet(("rrc.rooms." + rid + ".mode").c_str(), mode.c_str());
        storageEnd();
        return true;
    }
    if (text.rfind("topic for ", 0) == 0) {
        size_t c = text.find(" is now: ");
        if (c == std::string::npos) return false;
        std::string rid = ridFor(s, text.substr(10, c - 10));
        if (rid.empty()) return false;
        std::string topic = text.substr(c + 9);
        if (topic == "(cleared)") topic = "";
        storageSet(("rrc.rooms." + rid + ".topic").c_str(), clean(topic, true).c_str());
        return false;   /* also shown: someone changed it */
    }
    return false;
}

static bool takeQuiet(Slot& s, const std::string& what)
{
    for (auto it = s.quiet.begin(); it != s.quiet.end(); ++it)
        if (it->what == what) { s.quiet.erase(it); return true; }
    return false;
}

/* A NOTICE, whether it came as a packet or as a Resource. */
static void onNoticeText(Slot& s, const std::string* room, const std::string& src12,
                         const std::string& nick, const std::string& text)
{
    if (room) {
        std::string rid = ridFor(s, *room);
        if (rid.empty()) return;
        if (parseRoomInfo(s, text)) return;
        LogLine l;
        l.kind = KIND_NOTICE;
        l.src12 = src12;
        l.nick = nick;
        l.text = clean(text);
        logAppend(rid, l);
        return;
    }
    /* A /who or /list reply too long for one packet comes as several
     * NOTICEs: each line its own, a long line cut into fixed-size pieces
     * anywhere. A piece after the first line within RRC_CONT_MS is more of
     * it (a new /list line goes on a newline, any other piece straight on),
     * and the reply so far is parsed again; one we typed grows in its note. */
    bool head = text.rfind("members in ", 0) == 0 || text.rfind("Registered public rooms:", 0) == 0 ||
                text.rfind("No public rooms registered", 0) == 0;
    if (!head && !s.contKind.empty() && nowMs() < s.contUntil) {
        if (s.contKind == "list" && text.rfind("  ", 0) == 0) s.contText += '\n';
        s.contText += text;
        s.contUntil = nowMs() + RRC_CONT_MS;
        std::string whoRid;
        if (s.contKind == "who") parseWho(s, s.contText, whoRid);
        else parseList(s, s.contText);
        if (s.contNote >= 0) hubNote(s.hub, s.contNoteRid, s.contText, s.contNote);
        return;
    }
    s.contKind.clear();
    std::string whoRid;
    bool isWho = parseWho(s, text, whoRid);
    if (isWho || parseList(s, text)) {
        s.contKind = isWho ? "who" : "list";
        s.contText = text;
        s.contUntil = nowMs() + RRC_CONT_MS;
        s.contNote = -1;
        if (takeQuiet(s, isWho ? "who:" + whoRid : "list")) return;
        s.contNoteRid = nowMs() < s.lastCmdUntil ? s.lastCmdRid : "";
        s.contNote = hubNote(s.hub, s.contNoteRid, text);
        return;
    }
    /* The first notice after WELCOME that answers nothing we asked is the
     * hub's greeting. */
    if (!s.motdTaken && nowMs() < s.motdUntil) {
        s.motdTaken = true;
        storageSet(hubRam(s.hub, "motd").c_str(), clean(text).c_str());
    }
    std::string rid = nowMs() < s.lastCmdUntil ? s.lastCmdRid : "";
    hubNote(s.hub, rid, text);
}

static void onNotice(Slot& s, const Value& env, const std::string& text)
{
    std::string src12, nick;
    const Value* src = env.at(K_SRC);
    if (src && src->isBytes() && src->s.size() == 16) src12 = hexOf(src->s).substr(0, 12);
    if (const std::string* n = textOf(env.at(K_NICK))) nick = clean(*n, true);
    onNoticeText(s, textOf(env.at(K_ROOM)), src12, nick, text);
}

static void onWelcome(Slot& s, const Value& env)
{
    const Value* b = env.at(K_BODY);
    std::string name;
    if (b && b->isMap()) {
        if (const std::string* n = textOf(b->at(0))) name = clean(*n, true);
        if (const Value* lim = b->at(3)) {
            auto get = [&](uint64_t k, int def) {
                const Value* v = lim->at(k);
                return v && v->isUint() && v->u > 0 && v->u < 100000 ? (int)v->u : def;
            };
            s.limNick = get(0, 32); s.limRoom = get(1, 64); s.limBody = get(2, 350);
            s.limRooms = get(3, 32); s.limRate = get(4, 240);
        }
    }
    s.st = SL_LIVE;
    s.tokens = s.limRate;   /* the hub's bucket starts full; so does ours */
    s.refillMs = nowMs();
    s.motdUntil = nowMs() + 15000;
    s_retry.erase(s.hub);
    storageBegin();
    if (!name.empty()) storageSet(hubRam(s.hub, "name").c_str(), name.c_str());
    if (!name.empty() && storageGetStr(hubKey(s.hub, "name").c_str(), "").empty())
        storageSet(hubKey(s.hub, "name").c_str(), name.c_str());
    storageSet(hubRam(s.hub, "limit_body").c_str(), s.limBody);
    storageEnd();
    hubStatus(s.hub, "live", "Connected");
    info("rrc %s: welcomed by \"%s\" (body %d B, %d/min)", s.hub.c_str(), name.c_str(), s.limBody, s.limRate);

    for (auto& [rid, r] : s_rooms)
        if (r.hub == s.hub && r.joined) sendJoin(s, rid);
    sendQuiet(s, "list", "/list");
}

static void onMessage(Slot& s, const Value& env, uint64_t type)
{
    const std::string* room = textOf(env.at(K_ROOM));
    if (!room) return;
    std::string rid = ridFor(s, *room);
    if (rid.empty()) return;
    const Value* idv = env.at(K_ID);
    std::string id = idv && idv->isBytes() ? hexOf(idv->s) : "";

    /* Our own line coming back: relayed. */
    for (auto it = s.pending.begin(); it != s.pending.end(); ++it) {
        if (it->id == id) {
            logSetState(it->rid, it->seq, ST_RELAYED);
            s.pending.erase(it);
            seenBefore(s, id);
            return;
        }
    }
    if (seenBefore(s, id)) return;

    const std::string* body = textOf(env.at(K_BODY));
    if (!body) return;
    const Value* src = env.at(K_SRC);
    std::string full = src && src->isBytes() && src->s.size() == 16 ? hexOf(src->s) : "";
    std::string id12 = full.substr(0, std::min<size_t>(12, full.size()));
    std::string nick;
    if (const std::string* n = textOf(env.at(K_NICK))) nick = clean(*n, true);
    if (!id12.empty()) {
        rosterSet(rid, id12, nick, full);
        publishWho(rid);
    }
    if (!id12.empty() && storageGetInt(("s.rrc.ignore." + id12).c_str(), 0)) return;

    LogLine l;
    l.kind = type == T_ACTION ? KIND_ACTION : KIND_MSG;
    l.src12 = id12;
    l.id = id;
    l.nick = nick.empty() ? id12 : nick;
    l.text = clean(*body);
    l.own = id12 == hexOf(s.src, 6);
    l.state = l.own ? ST_RELAYED : ST_RECEIVED;
    l.mention = !l.own && mentions(l.text, s.nick);
    logAppend(rid, l);
}

static void onPresence(Slot& s, const Value& env, bool joined)
{
    const std::string* room = textOf(env.at(K_ROOM));
    if (!room) return;
    std::string rid = ridFor(s, *room);
    if (rid.empty()) return;
    Room& r = s_rooms[rid];
    const std::string* nick = textOf(env.at(K_NICK));
    const Value* body = env.at(K_BODY);

    if (joined && !nick && r.state == "joining") {
        /* Our own JOINED: it carries no nick. */
        roomEntered(s, rid);
        return;
    }
    if (!joined && r.state == "parting") return;

    std::string who = nick ? clean(*nick, true) : "";
    std::string full;
    if (body && body->isArray() && !body->items.empty() && body->items[0].isBytes())
        full = hexOf(body->items[0].s);
    else if (body && body->isBytes())
        full = hexOf(body->s);
    std::string id12 = full.substr(0, 12);
    if (who.empty() && !id12.empty()) who = id12;
    if (who.empty()) who = "someone";
    if (id12 == hexOf(s.src, 6)) return;

    if (storageGetStr("s.rrc.presence", "collapsed") != "hidden")
        presenceNote(rid, joined, who, id12);
    /* The roster knows people by identity. A JOINED or PARTED that carries it
     * settles the roster at once, and a PARTED whose nick is one member's
     * does too. Otherwise the roster is stale until a quiet /who, sent once
     * the room has been quiet for RRC_WHO_SETTLE_MS and while somebody is
     * looking at its member list. */
    if (id12.size() == 12) {
        if (joined) rosterSet(rid, id12, nick ? *nick : "", full.size() == 32 ? full : "");
        else storageUnset(("rrc.rooms." + rid + ".who." + id12).c_str());
        publishWho(rid);
        return;
    }
    if (!joined && nick) {
        std::string gone;
        int n = 0;
        for (auto& [k, v] : rosterOf(rid))
            if (v.substr(0, v.find('|')) == noBar(*nick)) { gone = k; n++; }
        if (n == 1) {
            storageUnset(("rrc.rooms." + rid + ".who." + gone).c_str());
            publishWho(rid);
            return;
        }
    }
    s.whoDue[rid] = nowMs() + RRC_WHO_SETTLE_MS;
}

static void requeueUnechoed(Slot& s)
{
    /* Re-send oldest first, ahead of anything not yet sent. */
    for (auto it = s.pending.rbegin(); it != s.pending.rend(); ++it) {
        Outbound o;
        o.bytes = std::move(it->bytes);
        o.rid = it->rid; o.seq = it->seq; o.id = it->id;
        logSetState(it->rid, it->seq, ST_QUEUED);
        s.outq.push_front(std::move(o));
    }
    s.pending.clear();
}

static void onError(Slot& s, const Value& env, const std::string& text)
{
    const std::string* room = textOf(env.at(K_ROOM));
    std::string rid = room ? ridFor(s, *room) : "";
    warn("rrc %s: ERROR \"%s\"%s%s", s.hub.c_str(), clean(text, true).c_str(),
         room ? " in " : "", room ? room->c_str() : "");
    if (text == "rate limited") {
        s.tokens = 0;
        s.refillMs = nowMs();
        requeueUnechoed(s);
        return;
    }
    std::string low = lower(text);
    if (!room && (low.find("banned") != std::string::npos || low.find("k-line") != std::string::npos ||
                  low.find("kline") != std::string::npos)) {
        s_held[s.hub] = clean(text, true);
        std::string hub = s.hub;
        slotFree(s, false);
        hubStatus(hub, "held", "Refused by the hub: " + clean(text, true));
        return;
    }
    if (!rid.empty()) {
        Room& r = s_rooms[rid];
        if (r.state == "joining") {
            roomPublishState(rid, "out");
            std::string why = text == "bad key (+k)" ? "This room needs a key"
                            : text == "invite-only (+i)" ? "This room is invite-only"
                            : text == "banned from room" ? "You are banned from this room"
                            : clean(text, true);
            storageSet(("rrc.rooms." + rid + ".error").c_str(), why.c_str());
            return;
        }
        LogLine l;
        l.kind = KIND_SYSTEM;
        l.text = clean(text, true);
        logAppend(rid, l);
        return;
    }
    hubNote(s.hub, nowMs() < s.lastCmdUntil ? s.lastCmdRid : "", "Error: " + text);
}

/* rrcd's RESOURCE_ENVELOPE: the next Resource on this Link is a notice or
 * the MOTD, of this size and SHA-256. */
static void onResourceEnvelope(Slot& s, const Value& env)
{
    const Value* b = env.at(K_BODY);
    if (!b || !b->isMap()) return;
    ResExpect e;
    if (const Value* v = b->at(0); v && v->isBytes()) e.id = v->s;
    if (const std::string* k = textOf(b->at(1))) e.kind = *k;
    if (const Value* v = b->at(2); v && v->isUint()) e.size = (uint32_t)v->u;
    if (const Value* v = b->at(3); v && v->isBytes()) e.sha = v->s;
    if (const std::string* r = textOf(env.at(K_ROOM))) { e.hasRoom = true; e.room = *r; }
    if (e.size == 0 || e.size > 16384) return;
    e.until = nowMs() + 30000;
    if (s.res.size() >= 8) s.res.erase(s.res.begin());
    s.res.push_back(e);
}

static void handleEnvelope(Slot& s, const uint8_t* p, size_t n)
{
    Value env;
    if (!rrccbor::decode(p, n, env) || !env.isMap()) {
        verb("rrc %s: undecodable packet (%u B)", s.hub.c_str(), (unsigned)n);
        return;
    }
    const Value* t = env.at(K_T);
    if (!t || !t->isUint()) return;
    const std::string* body = textOf(env.at(K_BODY));
    switch (t->u) {
    case T_WELCOME:  if (s.st == SL_HELLO) onWelcome(s, env); break;
    case T_MSG: case T_ACTION: onMessage(s, env, t->u); break;
    case T_NOTICE:   if (body) onNotice(s, env, *body); break;
    case T_JOINED:   onPresence(s, env, true); break;
    case T_PARTED:   onPresence(s, env, false); break;
    case T_ERROR:    if (body) onError(s, env, *body); break;
    case T_PING: {
        const Value* pb = env.at(K_BODY);
        std::string id = newId();
        std::string echo = pb && (pb->isBytes() || pb->isText()) ? pb->s : "";
        bool isText = pb && pb->isText();
        auto b = envelope(s, T_PONG, id, nullptr, [&](Writer& w) {
            if (isText) w.text(echo); else w.bytes((const uint8_t*)echo.data(), echo.size());
        }, pb != nullptr);
        slotSend(s, std::move(b), true);
        break;
    }
    case T_PONG: {
        const Value* pb = env.at(K_BODY);
        if (pb && pb->isBytes() && !s.pingId.empty() && pb->s == s.pingId) {
            s.rttMs = (int)(nowMs() - s.pingSentMs);
            s.pingId.clear();
            storageSet(hubRam(s.hub, "rtt").c_str(), s.rttMs);
        }
        break;
    }
    case T_RESOURCE_ENVELOPE: onResourceEnvelope(s, env); break;
    default: break;
    }
}

static void onLinkRecv(int handle, size_t)
{
    PSRAM_BSS static uint8_t buf[1024];
    for (;;) {
        size_t n = itsRecv(handle, buf, sizeof buf, 0);
        if (n == 0) return;
        for (auto& s : s_slots)
            if (s.st != SL_FREE && s.handle == handle) { handleEnvelope(s, buf, n); break; }
    }
}

static void onLinkDisc(int ref)
{
    if (ref < 0 || ref >= RRC_MAX_SLOTS) return;
    Slot& s = s_slots[ref];
    if (s.st == SL_FREE) return;
    info("rrc %s: link gone", s.hub.c_str());
    s.handle = -1;
    slotFree(s, true);
}

/* rnsd.links.rrc<N>.* — the Link coming up, or failing. */
static void onLinkState(const char* key, const char* val)
{
    const char* tail = key + sizeof("rnsd.links." RRC_TAG_PFX) - 1;
    if (tail[0] < '0' || tail[0] > '9' || tail[1] != '.') return;
    int i = tail[0] - '0';
    if (i >= RRC_MAX_SLOTS) return;
    Slot& s = s_slots[i];
    if (s.st == SL_FREE || !val) return;
    std::string field = tail + 2;
    if (field == "state") {
        if (!std::strcmp(val, "awaiting_path")) hubStatus(s.hub, "linking", "Connecting: looking for a path");
        else if (!std::strcmp(val, "establishing")) hubStatus(s.hub, "linking", "Connecting");
        else if (!std::strcmp(val, "active") && s.st == SL_LINKING) {
            s.st = SL_HELLO;
            std::string tag = slotTag(&s);
            storageBegin();
            storageSet(hubRam(s.hub, "rtt").c_str(), storageGetInt(("rnsd.links." + tag + ".rtt_ms").c_str(), 0));
            storageSet(hubRam(s.hub, "hops").c_str(), storageGetInt(("rnsd.links." + tag + ".hops").c_str(), 0));
            storageSet(hubRam(s.hub, "iface").c_str(), storageGetStr(("rnsd.links." + tag + ".iface").c_str(), "").c_str());
            storageEnd();
            hubStatus(s.hub, "hello", "Saying hello");
            sendHello(s);
        } else if (!std::strcmp(val, "failed") || !std::strcmp(val, "closed")) {
            std::string err = storageGetStr(("rnsd.links." + slotTag(&s) + ".last_error").c_str(), "");
            info("rrc %s: link %s%s%s", s.hub.c_str(), val, err.empty() ? "" : ": ", err.c_str());
            slotFree(s, true);
        }
    }
}

/* ─────────────── resources (rnsd → aux 101) ─────────────── */

static void onResourceAux(TaskHandle_t, const void* data, size_t len)
{
    if (len < sizeof(rnsd_link_resource_done_t)) return;
    rnsd_link_resource_done_t d;
    std::memcpy(&d, data, sizeof d);
    if (d.opcode != RNSD_LINK_RESOURCE_INBOUND_DONE) {
        if (d.buf) rnsdResourceRelease(d.buf);
        return;
    }
    std::string tag = storageGetStr(("rnsd.links.byid." + hexOf(d.link_id, 16)).c_str(), "");
    Slot* s = nullptr;
    for (auto& c : s_slots) if (c.st != SL_FREE && slotTag(&c) == tag) { s = &c; break; }
    if (s && d.buf) {
        uint8_t sha[32];
        rnsdSha256((const uint8_t*)d.buf, d.len, sha);
        std::string shaS((const char*)sha, 32);
        for (auto it = s->res.begin(); it != s->res.end(); ++it) {
            if (it->size != d.len || nowMs() > it->until) continue;
            if (!it->sha.empty() && it->sha != shaS) continue;
            ResExpect e = *it;
            s->res.erase(it);
            std::string text((const char*)d.buf, d.len);
            if (e.kind == "motd") {
                s->motdTaken = true;
                storageSet(hubRam(s->hub, "motd").c_str(), clean(text).c_str());
                hubNote(s->hub, "", clean(text));
            } else if (e.kind == "notice") {
                onNoticeText(*s, e.hasRoom ? &e.room : nullptr, "", "", text);
            } else {
                hubNote(s->hub, "", clean(text));
            }
            break;
        }
    }
    if (d.buf) rnsdResourceRelease(d.buf);
}

/* ─────────────── announces ─────────────── */

static int s_annHandle = -1;
constexpr size_t ANN_HDR = 1 + 16 + 16 + 64 + 32;

struct HeardOldest { int n = 0; int oldest = 0; std::string key; };
static HeardOldest* s_heardCtx = nullptr;
static void heardLeaf(const char* key, const char* val)
{
    if (!s_heardCtx || !key || !val) return;
    const char* tail = key + sizeof("rrc.heard.") - 1;
    if (std::strchr(tail, '.')) return;
    s_heardCtx->n++;
    int t = std::atoi(val);
    if (s_heardCtx->key.empty() || t < s_heardCtx->oldest) { s_heardCtx->oldest = t; s_heardCtx->key = key; }
}

static void onAnnounce(int handle, size_t)
{
    if (handle != s_annHandle) return;
    PSRAM_BSS static uint8_t buf[ANN_HDR + 512];
    size_t n = itsRecv(handle, buf, sizeof buf, 0);
    if (n < ANN_HDR) return;
    int hops = buf[0];
    std::string hub = hexOf(buf + 1, 16);
    Value app;
    std::string name;
    if (rrccbor::decode(buf + ANN_HDR, n - ANN_HDR, app) && app.isMap()) {
        const std::string* proto = textOf(app.atText("proto"));
        if (proto && *proto != "rrc") return;
        if (const std::string* h = textOf(app.atText("hub"))) name = clean(*h, true);
    }
    std::string key = "rrc.heard." + hub;
    if (!storageExists(key.c_str())) {
        HeardOldest c;
        s_heardCtx = &c;
        storageForEach("rrc.heard.", heardLeaf);
        s_heardCtx = nullptr;
        if (c.n >= RRC_HEARD_MAX && !c.key.empty()) storageUnset(c.key.c_str());
    }
    char v[32];
    std::snprintf(v, sizeof v, "%d|%d|", monoS(), hops);
    storageSet(key.c_str(), (std::string(v) + noBar(name)).c_str());
    verb("rrc: heard hub %s \"%s\" %d hops", hub.c_str(), name.c_str(), hops);
}

static void onAnnounceDisc(int) { s_annHandle = -1; }

static void connectAnnounces()
{
    if (s_annHandle >= 0) return;
    rnsd_announces_connect_t req = {};
    safeStrncpy(req.aspect, RRC_ASPECT, sizeof req.aspect);
    int h = itsConnect("rnsd", RNSD_PORT_ANNOUNCES, &req, sizeof req, pdMS_TO_TICKS(2000), 0,
                       onAnnounce, onAnnounceDisc);
    if (h >= 0) s_annHandle = h;
}

/* ─────────────── commands ─────────────── */

static std::vector<std::string> split(const std::string& v, size_t maxParts)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (out.size() + 1 < maxParts) {
        size_t b = v.find('|', at);
        if (b == std::string::npos) break;
        out.push_back(v.substr(at, b - at));
        at = b + 1;
    }
    out.push_back(v.substr(at));
    return out;
}

/* Commands are consumed: read, unset, act. */
#define TAKE(val) \
    if (!val || !*val) return; \
    std::string v = val; \
    storageUnset(key)

static void saveHub(const std::string& hub, const std::string& name)
{
    if (!storageExists(hubKey(hub, "auto").c_str())) {
        storageBegin();
        storageSet(hubKey(hub, "name").c_str(), name.empty() ? "" : noBar(name).c_str());
        storageSet(hubKey(hub, "auto").c_str(), 0);
        storageEnd();
    } else if (!name.empty()) {
        storageSet(hubKey(hub, "name").c_str(), noBar(name).c_str());
    }
}

/* rrc.cmd.add = "<hub>[|<name>]" — save a hub without connecting. */
static void onCmdAdd(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 2);
    std::string hub = lower(unspaced(p[0]));
    if (!isHubHash(hub)) { warn("rrc: not a hub hash: %s", hub.c_str()); return; }
    saveHub(hub, p.size() > 1 ? trim(p[1]) : "");
    if (!storageExists(hubRam(hub, "state").c_str())) hubStatus(hub, "idle", "Not connected");
}

/* rrc.cmd.connect = "<hub>[|<sel>]" — sel = lxmf<n> | node chooses who to
 * chat as, and is remembered for the hub. */
static void onCmdConnect(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 2);
    std::string hub = lower(unspaced(p[0]));
    if (!isHubHash(hub)) { warn("rrc: not a hub hash: %s", hub.c_str()); return; }
    saveHub(hub, "");
    if (p.size() > 1 && !p[1].empty()) {
        std::string sel = trim(p[1]);
        if (sel != storageGetStr(hubKey(hub, "ident").c_str(), "")) {
            storageSet(hubKey(hub, "ident").c_str(), sel.c_str());
            if (Slot* s = slotFor(hub)) slotFree(*s, false);   /* a different person: a new session */
        }
    }
    storageSet(hubKey(hub, "auto").c_str(), 1);
    s_held.erase(hub);
    s_retry.erase(hub);
    slotOpen(hub);
}

static void onCmdDisconnect(const char* key, const char* val)
{
    TAKE(val);
    std::string hub = lower(unspaced(v));
    if (!isHubHash(hub)) return;
    storageSet(hubKey(hub, "auto").c_str(), 0);
    s_retry.erase(hub);
    if (Slot* s = slotFor(hub)) slotFree(*s, false);
    hubStatus(hub, "idle", "Not connected");
}

static void roomForget(const std::string& rid)
{
    s_rooms.erase(rid);
    storageDbDropInstance(("s.rrc.log." + rid).c_str());
    storageUnset(("s.rrc.rooms." + rid).c_str());
    storageUnset(("secrets.rrc.keys." + rid).c_str());
    storageDeleteTree(("rrc.rooms." + rid).c_str());
    totalsPublish();
}

/* rrc.cmd.forget = "<hub>" — the hub, its rooms and their logs. */
static void onCmdForget(const char* key, const char* val)
{
    TAKE(val);
    std::string hub = lower(unspaced(v));
    if (!isHubHash(hub)) return;
    if (Slot* s = slotFor(hub)) slotFree(*s, false);
    s_retry.erase(hub);
    s_held.erase(hub);
    std::vector<std::string> rids;
    for (auto& [rid, r] : s_rooms) if (r.hub == hub) rids.push_back(rid);
    for (auto& rid : rids) roomForget(rid);
    storageDeleteTree(("s.rrc.hubs." + hub).c_str());
    storageDeleteTree(("rrc.hubs." + hub).c_str());
    info("rrc: forgot hub %s", hub.c_str());
}

static void joinRoom(const std::string& hub, const std::string& roomIn, const std::string& keyIn)
{
    std::string room = normRoom(roomIn);
    if (room.empty()) return;
    std::string rid = roomId(hub, room);
    if (rid.empty()) return;
    saveHub(hub, "");
    bool fresh = !s_rooms.count(rid);
    Room& r = s_rooms[rid];
    r.hub = hub;
    r.name = room;
    r.joined = 1;
    if (fresh) {
        r.notify = NOTIFY_MENTIONS;
        r.last_ts = wallS();
    }
    roomSave(rid, r);
    if (fresh) storageSet(roomKey(rid, "preview").c_str(), "");
    if (!keyIn.empty()) storageSet(("secrets.rrc.keys." + rid).c_str(), keyIn.c_str());
    storageSet(("rrc.rooms." + rid + ".state").c_str(), r.state.c_str());
    Slot* s = slotFor(hub);
    if (s && s->st == SL_LIVE) sendJoin(*s, rid);
    else if (!s) {
        storageSet(hubKey(hub, "auto").c_str(), 1);
        slotOpen(hub);
    }
}

/* rrc.cmd.join = "<hub>|<room>[|<key>]" */
static void onCmdJoin(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 3);
    if (p.size() < 2) return;
    std::string hub = lower(unspaced(p[0]));
    if (!isHubHash(hub)) return;
    joinRoom(hub, p[1], p.size() > 2 ? p[2] : "");
}

static void partRoom(const std::string& rid)
{
    auto it = s_rooms.find(rid);
    if (it == s_rooms.end()) return;
    Slot* s = slotFor(it->second.hub);
    if (s && s->st == SL_LIVE && it->second.state == "in") {
        std::string id = newId();
        auto b = envelope(*s, T_PART, id, &it->second.name, [](Writer&) {}, false);
        slotSend(*s, std::move(b), false);
    }
    info("rrc: left %s", it->second.name.c_str());
    roomForget(rid);
}

/* rrc.cmd.part = "<rid>" — leave the room; its log goes with it. */
static void onCmdPart(const char* key, const char* val)
{
    TAKE(val);
    partRoom(trim(v));
}

static void setNick(const std::string& hub, const std::string& nick)
{
    storageSet(hubKey(hub, "nick").c_str(), noBar(nick).c_str());
    Slot* s = slotFor(hub);
    if (!s) return;
    std::string was = s->nick;
    s->nick = nickFor(hub, s->sel);
    storageSet(hubRam(hub, "nick").c_str(), s->nick.c_str());
    if (s->nick == was) return;
    for (auto& [rid, r] : s_rooms) {
        if (r.hub != hub || r.state != "in") continue;
        rosterSet(rid, hexOf(s->src, 6), s->nick, hexOf(s->src, 16));
        LogLine l;
        l.kind = KIND_SYSTEM;
        l.text = "You are now known as " + s->nick;
        logAppend(rid, l);
    }
}

/* rrc.cmd.send = "<rid>|<nonce>|<text>" */
static void onCmdSend(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 3);
    if (p.size() < 3) return;
    std::string rid = p[0], text = p[2];
    auto it = s_rooms.find(rid);
    if (it == s_rooms.end()) return;
    Room& r = it->second;
    std::string errKey = "rrc.rooms." + rid + ".error";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (trim(text).empty()) return;

    uint64_t type = T_MSG;
    bool command = false;
    if (text.rfind("//", 0) == 0) text.erase(0, 1);
    else if (text.rfind("/me ", 0) == 0) { type = T_ACTION; text.erase(0, 4); }
    else if (text[0] == '/') {
        std::string cmd = text.substr(1, text.find(' ') == std::string::npos ? std::string::npos : text.find(' ') - 1);
        std::string arg = text.find(' ') == std::string::npos ? "" : trim(text.substr(text.find(' ') + 1));
        cmd = lower(cmd);
        if (cmd == "join") {
            std::string room = arg.substr(0, arg.find(' '));
            std::string k = arg.find(' ') == std::string::npos ? "" : trim(arg.substr(arg.find(' ') + 1));
            if (!room.empty()) joinRoom(r.hub, room, k);
            return;
        }
        if (cmd == "part" || cmd == "leave") { partRoom(rid); return; }
        if (cmd == "clear") {
            storageDbDropInstance(("s.rrc.log." + rid).c_str());
            return;
        }
        if (cmd == "nick") {
            setNick(r.hub, arg);
            return;
        }
        command = true;
    }

    Slot* s = slotFor(r.hub);
    if (!s || s->st != SL_LIVE || r.state != "in") {
        storageSet(errKey.c_str(), "Not in the room right now");
        return;
    }
    if ((int)text.size() > s->limBody) {
        storageSet(errKey.c_str(), ("Too long for this hub by " +
                    std::to_string(text.size() - s->limBody) + " bytes").c_str());
        return;
    }
    storageSet(errKey.c_str(), "");

    if (command) {
        s->lastCmdRid = rid;
        s->lastCmdUntil = nowMs() + RRC_QUIET_MS;
        hubNote(r.hub, rid, "> " + text);
        sendText(*s, T_MSG, r.name, text);
        return;
    }
    std::string id = newId();
    LogLine l;
    l.kind = type == T_ACTION ? KIND_ACTION : KIND_MSG;
    l.state = ST_QUEUED;
    l.own = true;
    l.src12 = hexOf(s->src, 6);
    l.id = hexOf(id);
    l.nick = s->nick;
    l.text = clean(text);
    uint32_t seq = logAppend(rid, l);
    auto b = envelope(*s, type, id, &r.name, [&](Writer& w) { w.text(text); }, true);
    slotSend(*s, std::move(b), false, rid, seq, hexOf(id));
}

/* rrc.cmd.retry = "<rid>|<seq>" — resend a failed line with the same id. */
static void onCmdRetry(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 2);
    if (p.size() < 2) return;
    std::string rid = p[0];
    uint32_t seq = (uint32_t)std::atoll(p[1].c_str());
    auto it = s_rooms.find(rid);
    if (it == s_rooms.end()) return;
    Slot* s = slotFor(it->second.hub);
    if (!s || s->st != SL_LIVE) return;
    std::string base = "s.rrc.log." + rid + "." + seqKey(seq) + ".";
    if (storageGetInt((base + "own").c_str(), 0) != 1) return;
    std::string idHex = storageGetStr((base + "id").c_str(), "");
    std::string text = storageGetStr((base + "text").c_str(), "");
    int kind = storageGetInt((base + "kind").c_str(), KIND_MSG);
    uint8_t idb[8];
    if (!hexTo(idHex, idb, 8)) return;
    std::string id((const char*)idb, 8);
    logSetState(rid, seq, ST_QUEUED);
    auto b = envelope(*s, kind == KIND_ACTION ? T_ACTION : T_MSG, id, &it->second.name,
                      [&](Writer& w) { w.text(text); }, true);
    slotSend(*s, std::move(b), false, rid, seq, idHex);
}

/* rrc.cmd.read = "<rid>|<seq>" — the newest line someone has seen. */
static void onCmdRead(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 2);
    auto it = s_rooms.find(p[0]);
    if (it == s_rooms.end()) return;
    Room& r = it->second;
    uint32_t seq = p.size() > 1 ? (uint32_t)std::atoll(p[1].c_str()) : r.seq;
    if (seq > r.seq) seq = r.seq;
    if (seq <= r.read_seq && r.unread == 0) return;
    r.read_seq = std::max(r.read_seq, seq);
    if (r.read_seq >= r.logSeq) { r.unread = 0; r.mentions = 0; }
    storageBegin();
    storageSet(roomKey(p[0], "read_seq").c_str(), (int)r.read_seq);
    storageSet(roomKey(p[0], "unread").c_str(), (int)r.unread);
    storageSet(roomKey(p[0], "mentions").c_str(), (int)r.mentions);
    storageEnd();
    totalsPublish();
}

/* rrc.cmd.notify = "<rid>|<all|mentions|off>" */
static void onCmdNotify(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 2);
    auto it = s_rooms.find(p[0]);
    if (it == s_rooms.end() || p.size() < 2) return;
    int n = p[1] == "all" ? NOTIFY_ALL : p[1] == "off" ? NOTIFY_OFF : NOTIFY_MENTIONS;
    it->second.notify = n;
    storageSet(roomKey(p[0], "notify").c_str(), n);
    totalsPublish();
}

/* rrc.cmd.nick = "<hub>|<nick>" ("" = the default) */
static void onCmdNick(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 2);
    std::string hub = lower(unspaced(p[0]));
    if (!isHubHash(hub)) return;
    setNick(hub, p.size() > 1 ? trim(p[1]) : "");
}

/* rrc.cmd.ignore = "<id12>|<0|1>" */
static void onCmdIgnore(const char* key, const char* val)
{
    TAKE(val);
    auto p = split(v, 2);
    std::string id = lower(unspaced(p[0]));
    if (id.size() != 12) return;
    if (p.size() > 1 && p[1] == "1") storageSet(("s.rrc.ignore." + id).c_str(), 1);
    else storageUnset(("s.rrc.ignore." + id).c_str());
}

/* rrc.cmd.who = "<rid>" — refresh the member list. */
static void onCmdWho(const char* key, const char* val)
{
    TAKE(val);
    auto it = s_rooms.find(trim(v));
    if (it == s_rooms.end()) return;
    Slot* s = slotFor(it->second.hub);
    if (s && s->st == SL_LIVE) sendWho(*s, it->first);
}

/* rrc.cmd.watch = "web|<rid>" or "lcd|<rid>" — that surface has the room's
 * member list in front of somebody, renewed within RRC_WATCH_LEASE_MS; an
 * empty rid ends it. */
static void onCmdWatch(const char* key, const char* val)
{
    TAKE(val);
    size_t bar = v.find('|');
    if (bar == std::string::npos) return;
    std::string who = v.substr(0, bar);
    if (who != "web" && who != "lcd") return;
    Watch& w = s_watch[who == "lcd" ? 1 : 0];
    w.rid = trim(v.substr(bar + 1));
    w.until = w.rid.empty() ? 0 : nowMs() + RRC_WATCH_LEASE_MS;
}

/* ─────────────── housekeeping tick ─────────────── */

static void tick()
{
    int64_t now = nowMs();
    for (auto& s : s_slots) {
        if (s.st == SL_FREE) continue;
        if (s.st == SL_HELLO && now >= s.helloNextMs) {
            if (s.helloTries >= RRC_HELLO_TRIES) {
                warn("rrc %s: no WELCOME after %d HELLOs", s.hub.c_str(), s.helloTries);
                slotFree(s, true);
                continue;
            }
            sendHello(s);
        }
        if (s.st != SL_LIVE) continue;
        /* Lines whose echo never came. */
        int64_t wait = std::max<int64_t>(15000, 4LL * s.rttMs);
        for (auto it = s.pending.begin(); it != s.pending.end();) {
            if (now - it->sentMs > wait) { logSetState(it->rid, it->seq, ST_FAILED); it = s.pending.erase(it); }
            else ++it;
        }
        for (auto it = s.quiet.begin(); it != s.quiet.end();) {
            if (now > it->until) it = s.quiet.erase(it); else ++it;
        }
        for (auto it = s.whoDue.begin(); it != s.whoDue.end();) {
            auto r = s_rooms.find(it->first);
            if (r == s_rooms.end() || r->second.state != "in") { it = s.whoDue.erase(it); continue; }
            auto last = s.whoLast.find(it->first);
            if (now >= it->second && roomWatched(it->first) &&
                (last == s.whoLast.end() || now - last->second >= RRC_WHO_FLOOR_MS)) {
                std::string rid = it->first;
                it = s.whoDue.erase(it);
                sendWho(s, rid);
            } else ++it;
        }
        /* Round trip, measured while somebody is looking at the hub. */
        if (storageGetInt(hubRam(s.hub, "watch").c_str(), 0) && now - s.lastPingMs > 60000) {
            s.lastPingMs = now;
            s.pingId = newId();
            s.pingSentMs = now;
            auto b = envelope(s, T_PING, s.pingId, nullptr,
                              [&](Writer& w) { w.bytes((const uint8_t*)s.pingId.data(), s.pingId.size()); }, true);
            slotSend(s, std::move(b), false);
        }
        slotDrain(s);
    }
    for (auto it = s_retry.begin(); it != s_retry.end();) {
        std::string hub = it->first;
        if (now >= it->second.at) {
            Retry r = it->second;
            it = s_retry.erase(it);
            if (!slotOpen(hub) && !s_retry.count(hub)) {
                s_retry[hub] = r;
                scheduleRetry(hub);
            } else if (s_retry.count(hub)) {
                s_retry[hub].attempt = r.attempt + 1;
            }
        } else {
            int left = (int)((it->second.at - now + 999) / 1000);
            if (left % 5 == 0 || left < 5)
                storageSet(hubRam(hub, "status").c_str(), ("Retrying in " + std::to_string(left) + " s").c_str());
            ++it;
        }
    }
}

/* Connect every hub set to auto-connect, after boot. */
static std::vector<std::string>* s_autoCollect = nullptr;
static void autoLeaf(const char* key, const char* val)
{
    if (!s_autoCollect || !key || !val) return;
    const char* tail = key + sizeof("s.rrc.hubs.") - 1;
    const char* dot = std::strchr(tail, '.');
    if (!dot) return;
    std::string hub(tail, dot - tail);
    if (!std::strcmp(dot + 1, "auto")) {
        if (std::atoi(val)) s_autoCollect->push_back(hub);
        else hubStatus(hub, "idle", "Not connected");
    }
}

static void autoConnect()
{
    std::vector<std::string> hubs;
    s_autoCollect = &hubs;
    storageForEach("s.rrc.hubs.", autoLeaf);
    s_autoCollect = nullptr;
    for (auto& h : hubs) slotOpen(h);
}

/* ─────────────── CLI ─────────────── */

static std::string ridByName(const std::string& name)
{
    std::string n = normRoom(name);
    for (auto& [rid, r] : s_rooms) if (r.name == n || rid == name) return rid;
    return "";
}

static void cliHubLeaf(const char* key, const char* val)
{
    const char* tail = key + sizeof("s.rrc.hubs.") - 1;
    const char* dot = std::strchr(tail, '.');
    if (!dot || std::strcmp(dot + 1, "auto")) return;
    std::string hub(tail, dot - tail);
    cliPrintf("%s  %-20s %s\n", hub.c_str(), hubName(hub).c_str(),
              storageGetStr(hubRam(hub, "status").c_str(), "Not connected").c_str());
    for (auto& [rid, r] : s_rooms) {
        if (r.hub != hub) continue;
        cliPrintf("    %s  %-20s %-7s %u unread%s\n", rid.c_str(), r.name.c_str(), r.state.c_str(),
                  (unsigned)r.unread, r.mentions ? ", mentioned" : "");
    }
    (void)val;
}

static void cliHeardLeaf(const char* key, const char* val)
{
    const char* tail = key + sizeof("rrc.heard.") - 1;
    if (std::strchr(tail, '.')) return;
    std::string v = val ? val : "";
    size_t p1 = v.find('|'), p2 = p1 == std::string::npos ? p1 : v.find('|', p1 + 1);
    if (p2 == std::string::npos) return;
    cliPrintf("%s  %2d hops  %5ds ago  %s\n", tail, std::atoi(v.c_str() + p1 + 1),
              monoS() - std::atoi(v.c_str()), v.substr(p2 + 1).c_str());
}

static std::string s_cliLogPfx;
static std::map<std::string, std::map<std::string, std::string>>* s_cliRecs = nullptr;
static void cliLogLeaf(const char* key, const char* val)
{
    if (!s_cliRecs || !key) return;
    const char* tail = key + s_cliLogPfx.size();
    const char* dot = std::strchr(tail, '.');
    if (!dot) return;
    (*s_cliRecs)[std::string(tail, dot - tail)][dot + 1] = val ? val : "";
}

static void cliRrc(const char* args)
{
    if (args && std::strcmp(args, "help") == 0) {
        cliPrintf("%-*s Reticulum Relay Chat: hubs, rooms, say\n", CLI_HELP_COL, "rrc [...]");
        return;
    }
    if (args && cliWantsHelp(args)) {
        cliPrintf("rrc                              saved hubs and their rooms\n");
        cliPrintf("rrc heard                        hubs heard announcing\n");
        cliPrintf("rrc connect <hub> [lxmf<n>|node] connect (and remember who to chat as)\n");
        cliPrintf("rrc disconnect <hub>\n");
        cliPrintf("rrc forget <hub>                 the hub, its rooms and their logs\n");
        cliPrintf("rrc join <hub> <room> [key]\n");
        cliPrintf("rrc part <room>\n");
        cliPrintf("rrc say <room> <text>            /me, /who, /list… work too\n");
        cliPrintf("rrc log <room> [n]               the last n lines (default 20)\n");
        cliPrintf("rrc who <room>\n");
        cliPrintf("rrc notes <hub>                  hub notices and command replies\n");
        return;
    }
    std::string a = args ? trim(args) : "";
    std::string verb = a.substr(0, a.find(' '));
    std::string rest = a.find(' ') == std::string::npos ? "" : trim(a.substr(a.find(' ') + 1));
    auto first = [&](std::string& s) {
        std::string f = s.substr(0, s.find(' '));
        s = s.find(' ') == std::string::npos ? "" : trim(s.substr(s.find(' ') + 1));
        return f;
    };
    /* A hub hash may be typed in space-separated groups, and in full (64 hex). */
    auto hubArg = [&](std::string& s) {
        auto isHex = [](const std::string& t) {
            return !t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isxdigit(c); });
        };
        auto take = [&](size_t want, std::string& r) {
            std::string h;
            while (h.size() < want && !r.empty()) {
                std::string t = r.substr(0, r.find(' '));
                if (h.size() + t.size() > want || !isHex(t)) break;
                h += t;
                first(r);
            }
            return h;
        };
        std::string r = s, h = take(64, r);
        if (h.size() != 64) { r = s; h = take(32, r); }
        if (h.size() != 32 && h.size() != 64) return first(s);
        s = r;
        return h.substr(0, 32);
    };
    if (verb.empty()) {
        storageForEach("s.rrc.hubs.", cliHubLeaf);
        return;
    }
    if (verb == "heard") { storageForEach("rrc.heard.", cliHeardLeaf); return; }
    if (verb == "connect") {
        std::string hub = hubArg(rest);
        storageSet("rrc.cmd.connect", (hub + (rest.empty() ? "" : "|" + rest)).c_str());
        return;
    }
    if (verb == "disconnect") { storageSet("rrc.cmd.disconnect", rest.c_str()); return; }
    if (verb == "forget")     { storageSet("rrc.cmd.forget", rest.c_str()); return; }
    if (verb == "join") {
        std::string hub = hubArg(rest), room = first(rest);
        if (room.empty()) { cliPrintf("usage: rrc join <hub> <room> [key]\n"); return; }
        storageSet("rrc.cmd.join", (hub + "|" + room + (rest.empty() ? "" : "|" + rest)).c_str());
        return;
    }
    if (verb == "notes") {
        std::string hub = lower(hubArg(rest));
        for (int i = 0; i < RRC_NOTES; ++i) {
            std::string n = storageGetStr((hubRam(hub, "notes.") + std::to_string(i)).c_str(), "");
            if (!n.empty()) cliPrintf("%s\n", n.c_str());
        }
        return;
    }
    std::string room = first(rest);
    std::string rid = ridByName(room);
    if (rid.empty()) { cliPrintf("no room %s (see `rrc`)\n", room.c_str()); return; }
    if (verb == "part") { storageSet("rrc.cmd.part", rid.c_str()); return; }
    if (verb == "who") {
        for (auto& [id12, v] : rosterOf(rid)) cliPrintf("%s  %s\n", id12.c_str(), v.c_str());
        return;
    }
    if (verb == "say") {
        if (rest.empty()) { cliPrintf("usage: rrc say <room> <text>\n"); return; }
        char nonce[16];
        std::snprintf(nonce, sizeof nonce, "%u", (unsigned)xTaskGetTickCount());
        storageSet("rrc.cmd.send", (rid + "|" + nonce + "|" + rest).c_str());
        return;
    }
    if (verb == "log") {
        int n = rest.empty() ? 20 : std::max(1, std::atoi(rest.c_str()));
        std::map<std::string, std::map<std::string, std::string>> recs;
        s_cliLogPfx = "s.rrc.log." + rid + ".";
        s_cliRecs = &recs;
        storageForEach(s_cliLogPfx.c_str(), cliLogLeaf);
        s_cliRecs = nullptr;
        int skip = (int)recs.size() - n;
        static const char* states[] = { "…", "✓", "✗", "" };
        for (auto& [seq, f] : recs) {
            if (skip-- > 0) continue;
            time_t t = (time_t)std::atoll(f["ts"].c_str());
            struct tm tmv;
            localtime_r(&t, &tmv);
            int kind = std::atoi(f["kind"].c_str()), st = std::atoi(f["state"].c_str());
            const char* mk = f["mention"] == "1" ? "@" : " ";
            if (kind == KIND_ACTION)
                cliPrintf("%02d:%02d%s * %s %s %s\n", tmv.tm_hour, tmv.tm_min, mk, f["nick"].c_str(), f["text"].c_str(), states[st & 3]);
            else if (kind == KIND_PRESENCE || kind == KIND_SYSTEM)
                cliPrintf("%02d:%02d   -- %s %s\n", tmv.tm_hour, tmv.tm_min, f["nick"].c_str(), f["text"].c_str());
            else
                cliPrintf("%02d:%02d%s <%s> %s %s\n", tmv.tm_hour, tmv.tm_min, mk, f["nick"].c_str(), f["text"].c_str(), states[st & 3]);
        }
        return;
    }
    cliPrintf("unknown subcommand. try `rrc -h`\n");
}

/* ─────────────── record stores ─────────────── */

static const sdb_schema& roomSchema()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 0x5243;   /* "RC" */
        x.schema_ver = 1;
        x.u32("seq").u32("read_seq").u32("unread").u32("mentions").u32("last_ts")
         .u8("joined").u8("notify")
         .fixstr("hub", 33)
         .text("name").text("preview");
        return x;
    }();
    return s;
}

static const sdb_schema& logSchema()
{
    static const sdb_schema s = [] {
        sdb_schema x;
        x.schema_id = 0x524c;   /* "RL" */
        x.schema_ver = 1;
        x.u32("ts").u8("kind").u8("state").u8("mention").u8("own")
         .fixstr("src", 13).fixstr("id", 17)
         .text("nick").text("text");
        return x;
    }();
    return s;
}

/* ─────────────── task ─────────────── */

static TaskHandle_t  s_task = nullptr;
static volatile bool s_stop = false, s_parked = false;

static void rrcTaskMain(void*)
{
    info("[%s] task up", TAG);
    if (!itsServerInit()) err("rrc itsServerInit failed");
    itsServerPortOpen(RNSD_LINK_RESOURCE_AUX_PORT, false, 1, 0, 0);
    itsOnAux(RNSD_LINK_RESOURCE_AUX_PORT, onResourceAux);
    itsClientInit(RRC_MAX_SLOTS + 4);

    storageSubscribeChanges("rrc.cmd.add",        onCmdAdd);
    storageSubscribeChanges("rrc.cmd.connect",    onCmdConnect);
    storageSubscribeChanges("rrc.cmd.disconnect", onCmdDisconnect);
    storageSubscribeChanges("rrc.cmd.forget",     onCmdForget);
    storageSubscribeChanges("rrc.cmd.join",       onCmdJoin);
    storageSubscribeChanges("rrc.cmd.part",       onCmdPart);
    storageSubscribeChanges("rrc.cmd.send",       onCmdSend);
    storageSubscribeChanges("rrc.cmd.retry",      onCmdRetry);
    storageSubscribeChanges("rrc.cmd.read",       onCmdRead);
    storageSubscribeChanges("rrc.cmd.notify",     onCmdNotify);
    storageSubscribeChanges("rrc.cmd.nick",       onCmdNick);
    storageSubscribeChanges("rrc.cmd.ignore",     onCmdIgnore);
    storageSubscribeChanges("rrc.cmd.who",        onCmdWho);
    storageSubscribeChanges("rrc.cmd.watch",      onCmdWatch);
    storageSubscribeChanges("rnsd.links." RRC_TAG_PFX, onLinkState);

    roomsLoad();

    for (;;) {
        connectAnnounces();
        autoConnect();
        while (!s_stop) {
            itsPoll(pdMS_TO_TICKS(RRC_TICK_MS));
            tick();
            if (s_annHandle < 0) connectAnnounces();
        }
        for (auto& s : s_slots) if (s.st != SL_FREE) slotFree(s, false);
        if (s_annHandle >= 0) { itsDisconnect(s_annHandle); s_annHandle = -1; }
        s_parked = true;
        info("[%s] stopped", TAG);
        while (s_stop) itsPoll(portMAX_DELAY);
        s_parked = false;
    }
}

static void rrcStart()
{
    s_stop = false;
    if (!s_task) s_task = spawnTask(rrcTaskMain, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);
    else xTaskNotifyGive(s_task);
}

static void rrcStop()
{
    if (!s_task || s_stop) return;
    s_stop = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 300 && !s_parked; i++) delay(10);
    if (!s_parked) warn("[%s] stop timed out", TAG);
}

void RrcService::onInit()
{
    storage_db_opts ro;
    ro.persist = "rrc/rooms.db.gz";
    ro.evict = STORAGE_DB_RELOAD;
    ro.browserMirror = true;
    storageStructuredDB("rrc_rooms", "s.rrc.rooms", &roomSchema(), ro);

    storage_db_opts lo;
    lo.persist = "rrc/log/$1.db.gz";
    lo.evict = STORAGE_DB_RELOAD;
    storageStructuredDB("rrc_log", "s.rrc.log.$", &logSchema(), lo);

    storageBegin();
    storageDefault("s.rrc.nick", "");
    storageDefault("s.rrc.sound", "mentions");
    storageDefault("s.rrc.keep", 500);
    storageDefault("s.rrc.max_hubs", 4);
    storageDefault("s.rrc.presence", "collapsed");
    storageEnd();

    cliRegisterCmd("rrc", cliRrc);
    rnsServiceRegister(TAG, rrcStart, rrcStop);
}
