/**
 * rrc_lcd.cpp — the on-device RRC (Reticulum Relay Chat) app (LVGL).
 *
 *   list ──tap room──▶ room ──tap header──▶ room info ──tap member──▶ member card
 *     │                  ▲                                               │
 *     └──tap hub───▶ hub ┘ (Join opens the room once its record exists)   │
 *                         ◀──────────────── Mention ──────────────────────┘
 *
 * Five screens on the one app layer, one shown at a time:
 *   - List (launcher entry): two tabs. Rooms is one flat list of every room on
 *     every hub, newest activity first; a row is the room and a time over the
 *     hub and the firmware's preview line, with a blue unread pill and an amber
 *     @ for a mention. A room whose hub is not live shows the hub's status in
 *     place of the preview. Hubs lists the saved hubs (status dot, room count,
 *     round trip), then the hubs heard announcing that are not saved (hops,
 *     age), then a field to add one by its 32-hex hash. With no rooms the Rooms
 *     tab says so and lists the hubs inline, so the first visit is one tap and
 *     one button. Rows select (focus); a tap or the centre button opens.
 *   - Hub: name, hash in fours, who we are there, the status sentence as the
 *     firmware words it, the greeting, the room directory from /list (a row
 *     selects; Join is a button), a field to join or create a room by name,
 *     Connect / Disconnect and Forget (which asks first, naming the hub). When
 *     the hub needs to know who to chat as, the LXMF identities and the node
 *     itself are offered and "Connect as" commits the choice. While this
 *     screen is in the foreground rrc.hubs.<hub>.watch is 1, so the node
 *     measures the round trip.
 *   - Room: IRC lines, not bubbles. A narrow grey time gutter carries the time
 *     on the first line of each minute; the nick is bold in the person's
 *     colour (lxmf's peerAvatar hue of their LXMF address when the roster
 *     knows it, else of the 12-hex identity prefix — the same colour as their
 *     avatar in LXMF); the text wraps under itself, clear of the gutter.
 *     Actions read "* nick text", notices are dimmed, runs of joins and leaves
 *     fold into one grey line when s.rrc.presence is collapsed, a mention has
 *     an amber bar at its left edge, and our own lines end in ✓ (relayed), a
 *     circling arrow (queued) or ✗ (failed — tap the line to retry). An
 *     "N new" divider sits where the room was read up to when it was opened.
 *     The newest ~40 lines are drawn; reaching the top draws 40 more. The
 *     composer sits at the bottom; Enter sends. On a board with keys it holds
 *     the focus, so typing goes straight in, and the trackball's up/down
 *     scrolls the log. The room's error (or its hub's status) shows under it.
 *   - Room info: topic, hub with hash and round trip, who we are, Notify
 *     (All · Mentions · Off), the member list, and Leave (asks first, naming
 *     the room). While it is in front of somebody the node may refresh the
 *     member list (rrc.cmd.watch).
 *   - Member card: avatar, nick, identity, LXMF address, the other rooms they
 *     are in, when LXMF last heard them; Message on LXMF (hands the address to
 *     the LXMF app through lxmf.url_lcd), Mention (puts "@nick " in the room's
 *     composer), Ignore / Unignore.
 *
 * Storage is the API (README.md has the contract). Read: the room directory
 * s.rrc.rooms, the open room's log s.rrc.log.<rid> (a record store; reads route
 * to it), saved hubs s.rrc.hubs, RAM state rrc.hubs.*, rrc.heard.*,
 * rrc.rooms.*, s.rrc.presence, s.rrc.ignore.*, LXMF identities for the chooser
 * and lxmf.announces.<hash> for the card. Written: rrc.cmd.{add,connect,
 * disconnect,forget,join,part,send,retry,read,notify,ignore,watch},
 * rrc.hubs.<hub>.watch and lxmf.url_lcd. The firmware publishes finished
 * strings (status, preview, error); they are shown as given, through
 * printable().
 *
 * Everything runs on the lcd task: the subscriptions are made from it, so
 * their callbacks may touch LVGL. They only mark what changed; one debounced
 * timer folds a burst into one refresh, and the open room's log is patched
 * line by line (append, or redraw the one line whose delivery state moved)
 * rather than rebuilt — a rebuild on the lcd task stalls the T-Deck keyboard
 * poll, and its C3 holds one key, so keys typed meanwhile would be lost.
 */
#include "lcd.h"
#include "lcd_app.h"
#include "lcd_input_box.h"
#include "rrc_app.h"
#include "storage.h"
#include "compat.h"
#include "esp_timer.h"

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

/* ---- look & feel (the LCD stylesheet and the mockups) ---- */

const uint32_t C_BG      = 0x101418;   /* app background */
const uint32_t C_SUB     = 0x9098A0;   /* secondary text */
const uint32_t C_ACCENT  = 0x2563A0;   /* pills, primary buttons, section titles */
const uint32_t C_AMBER   = 0xE8A33A;   /* mention @, mention bar, "N new", errors */
const uint32_t C_RULE    = 0x232B33;   /* row and bar rules */
const uint32_t C_OK      = 0x36C06A;   /* live dot, relayed tick, Connected */
const uint32_t C_SEL     = 0x1B2A3C;   /* selected / focused row */
const uint32_t C_BAR     = 0x0C1014;   /* composer bar */
const uint32_t C_FIELD   = 0x2F3B47;   /* field and ghost-button borders */
const uint32_t C_GUTTER  = 0x5F6A75;   /* time gutter */
const uint32_t C_PRES    = 0x6C7783;   /* joins / leaves, system lines, placeholders */
const uint32_t C_NOTE    = 0x8FA3B6;   /* notices */
const uint32_t C_ACT     = 0xCFD8E3;   /* action text, ghost-button text, chevrons */
const uint32_t C_HOLLOW  = 0x8A96A3;   /* a hub trying to connect */
const uint32_t C_DANGER  = 0x7A2F2F;   /* Leave / Forget border */
const uint32_t C_DANGERT = 0xF0A0A0;   /* Leave / Forget text */
const uint32_t C_FAIL    = 0xE06060;   /* failed line mark */
const uint32_t C_NEWLINE = 0x6A4A1A;   /* "N new" rules */
const uint32_t C_CHEV    = 0x1F2A35;   /* scroll-to-bottom chevron */
const uint32_t C_DIALOG  = 0x20262E;   /* confirm box */

/* Fonts at the platform zoom, re-resolved at the top of every build (a zoom
 * change frees the old ones). 14 px is the text size; 11 px is the secondary
 * line, the gutter and the section titles, as on lxmf's screens. */
const lv_font_t* kFont      = &lv_font_montserrat_12_latin;
const lv_font_t* kBold      = &lv_font_montserrat_12_latin;
const lv_font_t* kItalic    = &lv_font_montserrat_12_latin;
const lv_font_t* kSmall     = &lv_font_montserrat_12_latin;
const lv_font_t* kSmallBold = &lv_font_montserrat_12_latin;
const lv_font_t* kBtn       = &lv_font_montserrat_12_latin;
const lv_font_t* kBig       = &lv_font_montserrat_12_latin;
void refreshFont() {
    kFont      = lcdFont(LcdFace::UI,        lcdPx(14));
    kBold      = lcdFont(LcdFace::UI_BOLD,   lcdPx(14));
    kItalic    = lcdFont(LcdFace::UI_ITALIC, lcdPx(14));
    kSmall     = lcdFont(LcdFace::UI,        lcdPx(11));
    kSmallBold = lcdFont(LcdFace::UI_BOLD,   lcdPx(11));
    kBtn       = lcdFont(LcdFace::UI_BOLD,   lcdPx(12));
    kBig       = lcdFont(LcdFace::UI_BOLD,   lcdPx(16));
}

int hdrH()    { return lcdPx(26); }   /* screen header bar */
int gutterW() { return lcdPx(36); }   /* the room log's time gutter */

/* Log pagination: the newest PAGE lines are drawn on open, PAGE more each time
 * the top is reached. An open with more unread than that reaches back to the
 * "N new" divider, up to FIRST_MAX lines. */
const size_t PAGE = 40, FIRST_MAX = 200;

/* Debounce: storage changes are folded into one refresh this long after the
 * first of a burst. The log's is short, so a line lands promptly; the rest
 * (directory, hubs, roster) can wait a little longer. */
const uint32_t LOG_DEBOUNCE_MS = 60, META_DEBOUNCE_MS = 250;

/* Up to this many identity slots are offered in the chooser (lxmf's slots). */
const int LXMF_SLOTS = 10;

/* Log record kinds and delivery states, as rrc.cpp writes them. */
enum { KIND_MSG = 0, KIND_ACTION = 1, KIND_NOTICE = 2, KIND_PRESENCE = 3, KIND_SYSTEM = 4 };
enum { ST_QUEUED = 0, ST_RELAYED = 1, ST_FAILED = 2, ST_RECEIVED = 3 };

/* Codepoints that only modify their neighbours: combining marks, zero-width
 * joiners and spaces, variation selectors, skin tones, tag characters. */
static bool zeroWidth(uint32_t cp) {
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xE0000 && cp <= 0xE007F);
}

/* Make network text drawable in the UI font: strip C0/C1 controls and
 * zero-width modifiers, and show each run of codepoints with no glyph (emoji,
 * CJK, …) as one '?', so it never leaves LVGL's placeholder boxes and a name
 * written only in such characters still shows as something.
 * `oneLine` folds CR/LF/TAB to a space; otherwise newlines stay. */
std::string printable(std::string_view in, bool oneLine) {
    std::string out;
    out.reserve(in.size());
    bool inRun = false;
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t b = (uint8_t)in[i];
        uint32_t cp; size_t len;
        if      (b < 0x80)           { cp = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > n) break;
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) { i++; continue; }
        size_t at = i;
        i += len;
        if (cp == '\n' || cp == '\r' || cp == '\t') { out += oneLine ? ' ' : (char)cp; inRun = false; continue; }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || zeroWidth(cp)) continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(kFont, &g, cp, 0) || g.is_placeholder) {
            if (!inRun) out += '?';
            inRun = true;
            continue;
        }
        inRun = false;
        out.append(in.data() + at, len);
    }
    return out;
}

/* ---- small text helpers ---- */

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
bool isHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}
/* A room name the way the directory stores it: no leading '#', lowercased. */
std::string normRoom(const std::string& r) {
    std::string s = trim(r);
    while (!s.empty() && s[0] == '#') s.erase(0, 1);
    return lower(trim(s));
}
/* A room as people read it. */
std::string roomLabel(const std::string& name) { return "#" + printable(normRoom(name), true); }

/* Hex grouped in fours for eye comparison: "1303 a636 cf76 …". */
std::string fours(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 4) {
        if (i) out += ' ';
        out += hex.substr(i, 4);
    }
    return out;
}

std::string nonce() { return std::to_string((unsigned)millis()); }

/* ---- colour: lxmf's peerAvatar hue, so a person is one colour in both apps ---- */

/* lxmf/browser/src/modules/lxmf.ts peerAvatar(): h = h*31 + charCode over the
 * first 8 characters, unsigned 32-bit, mod 360. */
int peerHue(const std::string& peer) {
    uint32_t h = 0;
    for (size_t i = 0; i < peer.size() && i < 8; i++) h = h * 31u + (uint8_t)peer[i];
    return (int)(h % 360u);
}

/* CSS hsl(h s% l%), as the browser draws the same hue. */
lv_color_t hsl(int h, int s, int l) {
    float S = s / 100.0f, L = l / 100.0f;
    float C = (1.0f - fabsf(2.0f * L - 1.0f)) * S;
    float Hp = (float)(h % 360) / 60.0f;
    float X = C * (1.0f - fabsf(fmodf(Hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if      (Hp < 1) { r = C; g = X; }
    else if (Hp < 2) { r = X; g = C; }
    else if (Hp < 3) { g = C; b = X; }
    else if (Hp < 4) { g = X; b = C; }
    else if (Hp < 5) { r = X; b = C; }
    else             { r = C; b = X; }
    float m = L - C / 2.0f;
    auto u8 = [](float v) { int x = (int)lroundf(v * 255.0f); return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); };
    return lv_color_make(u8(r + m), u8(g + m), u8(b + m));
}

/* The avatar glyph peerAvatar() draws: the initials of up to two words of the
 * name, or the first two characters of the hash. */
std::string avatarGlyph(const std::string& name, const std::string& hash) {
    std::string out;
    bool named = !name.empty() && name[0] != '(';
    if (named) {
        size_t i = 0;
        int words = 0;
        while (i < name.size() && words < 2) {
            while (i < name.size() && isspace((unsigned char)name[i])) i++;
            if (i >= name.size()) break;
            size_t len = 1;
            uint8_t b = (uint8_t)name[i];
            if      ((b & 0xE0) == 0xC0) len = 2;
            else if ((b & 0xF0) == 0xE0) len = 3;
            else if ((b & 0xF8) == 0xF0) len = 4;
            std::string ch = name.substr(i, len);
            if (len == 1) ch[0] = (char)toupper((unsigned char)ch[0]);
            out += ch;
            words++;
            while (i < name.size() && !isspace((unsigned char)name[i])) i++;
        }
    } else {
        for (size_t i = 0; i < hash.size() && i < 2; i++) out += (char)toupper((unsigned char)hash[i]);
    }
    return out.empty() ? "?" : out;
}

/* ---- time ---- */

bool clockValid() { return time(nullptr) > 1600000000; }
long monoS() { return (long)(esp_timer_get_time() / 1000000); }

std::string hhmm(uint32_t ts) {
    if (!ts) return "";
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return b;
}

/* A list row's time: the clock today, the weekday this week, else the date. */
std::string listTime(uint32_t ts) {
    if (!ts) return "";
    if (!clockValid()) return hhmm(ts);
    time_t now = time(nullptr), t = (time_t)ts;
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return hhmm(ts);
    static const char* wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    long d = (long)(now - t);
    char buf[16];
    if (d >= 0 && d < 6 * 86400L) return wd[a.tm_wday % 7];
    snprintf(buf, sizeof buf, "%d %s", a.tm_mday, mo[a.tm_mon % 12]);
    return buf;
}

std::string age(long secs) {
    if (secs < 0) secs = 0;
    char b[16];
    if      (secs < 60)     snprintf(b, sizeof b, "%ld s", secs);
    else if (secs < 3600)   snprintf(b, sizeof b, "%ld min", secs / 60);
    else if (secs < 86400)  snprintf(b, sizeof b, "%ld h", secs / 3600);
    else                    snprintf(b, sizeof b, "%ld d", secs / 86400);
    return b;
}

/* A round trip in ms as "0.8 s", without floating-point printf. */
std::string rttText(int ms) {
    if (ms <= 0) return "";
    char b[16];
    snprintf(b, sizeof b, "%d.%d s", ms / 1000, (ms % 1000) / 100);
    return b;
}

/* ---- model (lcd task only) ---- */

struct RoomRec {
    std::string rid, hub, name, preview;
    uint32_t seq = 0, read_seq = 0, unread = 0, mentions = 0, last_ts = 0;
    int joined = 1, notify = 1;
};
std::map<std::string, RoomRec> g_rooms;          /* rid → directory record */

struct Heard { std::string hub, name; long mono = 0; int hops = 0; };
std::vector<Heard> g_heard;                      /* newest first */

std::vector<std::string> g_saved;                /* saved hub hashes */

struct Who { std::string nick, full, lxmf; };
std::map<std::string, Who> g_roster;             /* id12 → member of the open room */

struct Line {
    uint32_t seq = 0, ts = 0;
    int kind = KIND_MSG, state = ST_RECEIVED;
    bool mention = false, own = false;
    std::string src, id, nick, text;
};
std::map<uint32_t, Line> g_lines;                /* the open room's log, by seq */

/* Accumulators for the storageForEach walks (function-pointer callbacks). */
std::map<std::string, RoomRec>* s_roomAcc = nullptr;
void roomLeaf(const char* key, const char* val) {
    if (!s_roomAcc || !key) return;
    const char* tail = key + sizeof("s.rrc.rooms.") - 1;
    const char* dot = strchr(tail, '.');
    if (!dot) return;
    std::string rid(tail, dot - tail);
    const char* f = dot + 1;
    if (strchr(f, '.')) return;
    RoomRec& r = (*s_roomAcc)[rid];
    r.rid = rid;
    std::string v = val ? val : "";
    if      (!strcmp(f, "hub"))      r.hub = v;
    else if (!strcmp(f, "name"))     r.name = v;
    else if (!strcmp(f, "preview"))  r.preview = v;
    else if (!strcmp(f, "seq"))      r.seq = (uint32_t)strtoul(v.c_str(), nullptr, 10);
    else if (!strcmp(f, "read_seq")) r.read_seq = (uint32_t)strtoul(v.c_str(), nullptr, 10);
    else if (!strcmp(f, "unread"))   r.unread = (uint32_t)strtoul(v.c_str(), nullptr, 10);
    else if (!strcmp(f, "mentions")) r.mentions = (uint32_t)strtoul(v.c_str(), nullptr, 10);
    else if (!strcmp(f, "last_ts"))  r.last_ts = (uint32_t)strtoul(v.c_str(), nullptr, 10);
    else if (!strcmp(f, "joined"))   r.joined = atoi(v.c_str());
    else if (!strcmp(f, "notify"))   r.notify = atoi(v.c_str());
}
void loadRooms() {
    std::map<std::string, RoomRec> acc;
    s_roomAcc = &acc;
    storageForEach("s.rrc.rooms.", roomLeaf);
    s_roomAcc = nullptr;
    for (auto it = acc.begin(); it != acc.end();) {
        if (!isHex32(it->second.hub) || it->second.name.empty()) it = acc.erase(it);
        else ++it;
    }
    g_rooms.swap(acc);
}

std::vector<std::string>* s_savedAcc = nullptr;
void savedLeaf(const char* key, const char*) {
    if (!s_savedAcc || !key) return;
    const char* tail = key + sizeof("s.rrc.hubs.") - 1;
    const char* dot = strchr(tail, '.');
    if (!dot || strcmp(dot + 1, "auto")) return;   /* every saved hub has `auto` */
    std::string hub(tail, dot - tail);
    if (isHex32(hub)) s_savedAcc->push_back(hub);
}
void loadSaved() {
    std::vector<std::string> acc;
    s_savedAcc = &acc;
    storageForEach("s.rrc.hubs.", savedLeaf);
    s_savedAcc = nullptr;
    g_saved.swap(acc);
}
bool isSaved(const std::string& hub) {
    return std::find(g_saved.begin(), g_saved.end(), hub) != g_saved.end();
}

std::vector<Heard>* s_heardAcc = nullptr;
void heardLeaf(const char* key, const char* val) {
    if (!s_heardAcc || !key || !val) return;
    const char* tail = key + sizeof("rrc.heard.") - 1;
    if (strchr(tail, '.')) return;
    std::string v = val;                        /* "<mono s>|<hops>|<name>" */
    size_t p1 = v.find('|');
    size_t p2 = p1 == std::string::npos ? p1 : v.find('|', p1 + 1);
    if (p2 == std::string::npos) return;
    Heard h;
    h.hub = tail;
    h.mono = atol(v.c_str());
    h.hops = atoi(v.c_str() + p1 + 1);
    h.name = v.substr(p2 + 1);
    if (isHex32(h.hub)) s_heardAcc->push_back(std::move(h));
}
void loadHeard() {
    std::vector<Heard> acc;
    s_heardAcc = &acc;
    storageForEach("rrc.heard.", heardLeaf);
    s_heardAcc = nullptr;
    std::sort(acc.begin(), acc.end(), [](const Heard& a, const Heard& b) { return a.mono > b.mono; });
    g_heard.swap(acc);
}
const Heard* heardOf(const std::string& hub) {
    for (auto& h : g_heard) if (h.hub == hub) return &h;
    return nullptr;
}

std::string hubRam(const std::string& hub, const char* f) { return "rrc.hubs." + hub + "." + f; }
std::string hubState(const std::string& hub)  { return storageGetStr(hubRam(hub, "state").c_str(), ""); }
std::string hubStatus(const std::string& hub) {
    return printable(storageGetStr(hubRam(hub, "status").c_str(), "Not connected"), true);
}
bool hubActive(const std::string& st) {
    return st == "live" || st == "linking" || st == "hello" || st == "retry";
}

/* The saved name, else the hub's own from WELCOME, else the announced one,
 * else the head of the hash — the firmware's own order. */
std::string hubName(const std::string& hub) {
    std::string n = storageGetStr(("s.rrc.hubs." + hub + ".name").c_str(), "");
    if (n.empty()) n = storageGetStr(hubRam(hub, "name").c_str(), "");
    if (n.empty()) if (const Heard* h = heardOf(hub)) n = h->name;
    n = printable(n, true);
    return n.empty() ? hub.substr(0, 8) : n;
}

/* Who we chat as on a hub that is not connected: the saved choice, worded. */
std::string identLabel(const std::string& sel) {
    if (sel.rfind("lxmf", 0) == 0) {
        std::string n = sel.substr(4);
        std::string name = storageGetStr(("s.lxmf.id." + n + ".display_name").c_str(), "");
        return name.empty() ? "LXMF identity " + n : printable(name, true);
    }
    if (sel == "node") return "this node";
    return "";
}

std::string findRid(const std::string& hub, const std::string& name) {
    std::string n = normRoom(name);
    for (auto& [rid, r] : g_rooms) if (r.hub == hub && normRoom(r.name) == n) return rid;
    return "";
}

std::string s_rosterPfx;
std::map<std::string, Who>* s_rosterAcc = nullptr;
void rosterLeaf(const char* key, const char* val) {
    if (!s_rosterAcc || !key) return;
    if (strncmp(key, s_rosterPfx.c_str(), s_rosterPfx.size())) return;
    std::string id12 = key + s_rosterPfx.size();
    if (id12.size() != 12) return;
    std::string v = val ? val : "";             /* "<nick>|<identity>|<lxmf>" */
    Who w;
    size_t p1 = v.find('|');
    w.nick = v.substr(0, p1);
    if (p1 != std::string::npos) {
        size_t p2 = v.find('|', p1 + 1);
        w.full = v.substr(p1 + 1, p2 == std::string::npos ? std::string::npos : p2 - p1 - 1);
        if (p2 != std::string::npos) w.lxmf = v.substr(p2 + 1);
    }
    (*s_rosterAcc)[id12] = w;
}
std::map<std::string, Who> readRoster(const std::string& rid) {
    std::map<std::string, Who> acc;
    s_rosterPfx = "rrc.rooms." + rid + ".who.";
    s_rosterAcc = &acc;
    storageForEach(s_rosterPfx.c_str(), rosterLeaf);
    s_rosterAcc = nullptr;
    return acc;
}

/* A speaker's colour: the hue of their LXMF address when the roster has it,
 * else of their 12-hex identity prefix. */
lv_color_t nickColor(const std::string& src12) {
    if (src12.empty()) return lv_color_hex(C_ACT);
    auto it = g_roster.find(src12);
    const std::string& basis = (it != g_roster.end() && !it->second.lxmf.empty()) ? it->second.lxmf : src12;
    return hsl(peerHue(basis), 70, 72);
}

/* ---- widgets ---- */

lv_obj_t* s_layer = nullptr;

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f ? f : kFont, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}

/* A one-line label that ellipsizes inside whatever width the flex row gives it. */
lv_obj_t* mkFill(lv_obj_t* row, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(row, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, 1);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_height(l, lv_font_get_line_height(f ? f : kFont));
    return l;
}

/* A wrapping label that takes the rest of a flex row. */
lv_obj_t* mkWrap(lv_obj_t* row, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(row, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, 1);
    lv_obj_set_flex_grow(l, 1);
    return l;
}

/* A plain container. Not clickable: a press on it falls through to whatever
 * clickable parent it sits in (a row button), which is where taps belong. */
lv_obj_t* mkBox(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t* mkRowBox(lv_obj_t* parent, int gap) {
    lv_obj_t* o = mkBox(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

/* A full-size screen on the app layer: header, body, bar in a column. */
lv_obj_t* mkScreen() {
    lv_obj_t* o = lv_obj_create(s_layer);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* A scrolling column that takes the screen's remaining height. */
lv_obj_t* mkBody(lv_obj_t* screen) {
    lv_obj_t* b = lv_obj_create(screen);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(b, lcdPx(6), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_row(b, lcdPx(2), 0);
    return b;
}

/* The bar of buttons along a screen's bottom edge, right-aligned. */
lv_obj_t* mkActionBar(lv_obj_t* screen) {
    lv_obj_t* bar = mkRowBox(screen, lcdPx(6));
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, lcdPx(8), 0);
    lv_obj_set_style_pad_ver(bar, lcdPx(5), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    return bar;
}

void focusRing(lv_obj_t* o) {
    lv_obj_set_style_outline_width(o, lcdPx(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(o, lv_color_hex(0x7FA6FF), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(o, 1, LV_STATE_FOCUS_KEY);
}

enum BtnStyle { BTN_PRIMARY, BTN_GHOST, BTN_DANGER };

lv_obj_t* mkButton(lv_obj_t* parent, const std::string& text, BtnStyle st,
                   lv_event_cb_t cb, void* ud = nullptr, lv_obj_t** lblOut = nullptr) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_hor(b, lcdPx(10), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    uint32_t tc = 0xFFFFFF;
    if (st == BTN_PRIMARY) {
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(st == BTN_DANGER ? C_DANGER : C_FIELD), 0);
        tc = st == BTN_DANGER ? C_DANGERT : C_ACT;
    }
    lv_obj_set_style_bg_color(b, lv_color_hex(st == BTN_PRIMARY ? 0x1D4F80 : C_SEL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    focusRing(b);
    lv_obj_t* l = mkLabel(b, text, tc, kBtn);
    if (lblOut) *lblOut = l;
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

/* A list row: a button whose selection (keypad focus, a press, or being the
 * chosen one of a set) is the darker blue band. */
lv_obj_t* mkRow(lv_obj_t* parent) {
    lv_obj_t* r = lv_button_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, lcdPx(2), 0);
    lv_obj_set_style_pad_hor(r, lcdPx(6), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(C_RULE), 0);
    for (lv_state_t s : { (lv_state_t)LV_STATE_FOCUSED, (lv_state_t)LV_STATE_PRESSED, (lv_state_t)LV_STATE_CHECKED }) {
        lv_obj_set_style_bg_color(r, lv_color_hex(C_SEL), s);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, s);
    }
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), r);
    return r;
}

enum { DOT_NONE = -1, DOT_HOLLOW = 0, DOT_LIVE = 1, DOT_ASK = 2 };
lv_obj_t* mkDot(lv_obj_t* parent, int kind) {
    lv_obj_t* d = mkBox(parent);
    int sz = lcdPx(7);
    lv_obj_set_size(d, sz, sz);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    if (kind == DOT_LIVE) {
        lv_obj_set_style_bg_color(d, lv_color_hex(C_OK), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    } else if (kind == DOT_HOLLOW || kind == DOT_ASK) {
        lv_obj_set_style_border_width(d, lcdPx(1) < 1 ? 1 : lcdPx(1), 0);
        lv_obj_set_style_border_color(d, lv_color_hex(kind == DOT_ASK ? C_AMBER : C_HOLLOW), 0);
    }
    return d;
}

lv_obj_t* mkPill(lv_obj_t* parent, int n, bool dim) {
    lv_obj_t* p = mkBox(parent);
    lv_obj_set_style_bg_color(p, lv_color_hex(dim ? C_FIELD : C_ACCENT), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(p, lcdPx(5), 0);
    char b[12];
    snprintf(b, sizeof b, "%d", n > 999 ? 999 : n);
    mkLabel(p, b, 0xFFFFFF, kSmallBold);
    return p;
}

lv_obj_t* mkSection(lv_obj_t* parent, const std::string& text) {
    lv_obj_t* l = mkLabel(parent, text, C_ACCENT, kSmallBold);
    lv_obj_set_style_pad_top(l, lcdPx(5), 0);
    lv_obj_set_style_pad_left(l, lcdPx(6), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

/* "Key   value" on a page; returns the value label. */
lv_obj_t* mkKv(lv_obj_t* body, const char* key, const std::string& val, uint32_t color, const lv_font_t* f) {
    lv_obj_t* row = mkRowBox(body, lcdPx(4));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t* k = mkLabel(row, key, C_SUB, kSmall);
    lv_obj_set_width(k, lcdPx(56));
    lv_obj_set_style_pad_top(k, lcdPx(2), 0);
    return mkWrap(row, val, color, f);
}

/* A spangroup that holds one line and ellipsizes it: a bold title with a
 * smaller grey tail ("#mesh · MichMesh"). */
struct TitleLine { lv_obj_t* sg = nullptr; lv_span_t* title = nullptr; lv_span_t* sub = nullptr; };
TitleLine mkTitleLine(lv_obj_t* row) {
    TitleLine t;
    t.sg = lv_spangroup_create(row);
    lv_obj_remove_flag(t.sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(t.sg, 1);
    lv_obj_set_flex_grow(t.sg, 1);
    lv_obj_set_height(t.sg, lv_font_get_line_height(kBold));
    lv_spangroup_set_max_lines(t.sg, 1);
    lv_spangroup_set_overflow(t.sg, LV_SPAN_OVERFLOW_ELLIPSIS);
    lv_obj_set_style_text_font(t.sg, kBold, 0);
    t.title = lv_spangroup_add_span(t.sg);
    lv_style_set_text_color(lv_span_get_style(t.title), lv_color_white());
    lv_style_set_text_font(lv_span_get_style(t.title), kBold);
    lv_spangroup_set_span_text(t.sg, t.title, "");
    t.sub = lv_spangroup_add_span(t.sg);
    lv_style_set_text_color(lv_span_get_style(t.sub), lv_color_hex(C_SUB));
    lv_style_set_text_font(lv_span_get_style(t.sub), kSmall);
    lv_spangroup_set_span_text(t.sg, t.sub, "");
    return t;
}
void setTitleLine(TitleLine& t, const std::string& title, const std::string& sub) {
    if (!t.sg) return;
    lv_spangroup_set_span_text(t.sg, t.title, title.c_str());
    lv_spangroup_set_span_text(t.sg, t.sub, sub.c_str());
}

/* Header bar: back chevron, title line, a small grey note at the right. With
 * `onTap` the bar itself is a tap target too (the chevron wins its own). */
struct Header { lv_obj_t* bar = nullptr; lv_obj_t* back = nullptr; TitleLine line; lv_obj_t* right = nullptr; };
Header mkHeader(lv_obj_t* screen, lv_event_cb_t onBack, lv_event_cb_t onTap) {
    Header h;
    h.bar = mkRowBox(screen, lcdPx(6));
    lv_obj_set_height(h.bar, hdrH());
    lv_obj_set_style_pad_hor(h.bar, lcdPx(6), 0);
    lv_obj_set_style_border_side(h.bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(h.bar, 1, 0);
    lv_obj_set_style_border_color(h.bar, lv_color_hex(C_RULE), 0);
    if (onTap) {
        lv_obj_add_flag(h.bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(h.bar, onTap, LV_EVENT_CLICKED, nullptr);
    }
    h.back = mkLabel(h.bar, LV_SYMBOL_LEFT, C_ACT, kFont);
    lv_obj_add_flag(h.back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(h.back, lcdPx(12));
    lv_obj_add_event_cb(h.back, onBack, LV_EVENT_CLICKED, nullptr);
    focusRing(h.back);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), h.back);
    h.line = mkTitleLine(h.bar);
    h.right = mkLabel(h.bar, "", C_SUB, kSmall);
    return h;
}

/* Each clickable row carries its own target string, freed with the widget, so
 * a reused or reordered row never points at a stale index. */
void onFreeString(lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); }
void bindString(lv_obj_t* o, lv_event_cb_t cb, const std::string& s) {
    auto* p = new std::string(s);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(o, onFreeString, LV_EVENT_DELETE, p);
}
const std::string& boundString(lv_event_t* e) { return *static_cast<std::string*>(lv_event_get_user_data(e)); }

/* ---- deferred focus ----
 * A launcher tile (or a tapped row) takes the input group's focus on
 * click-release, after our handlers have run, so focus is placed from a
 * one-shot timer once that has settled. A text field gets it only where the
 * board has keys: there focus means "start typing"; where the keys are on the
 * panel it would only stand for a keyboard nobody asked for. */
lv_obj_t* g_focusTarget = nullptr;
bool onScreenNow(lv_obj_t* o) {
    for (; o; o = lv_obj_get_parent(o)) if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}
void focusTimerCb(lv_timer_t*) {
    /* A screen change in the meantime (a link opening a room) wins. */
    if (g_focusTarget && lv_obj_is_valid(g_focusTarget) && onScreenNow(g_focusTarget) && lcdInputGroup())
        lv_group_focus_obj(g_focusTarget);
    g_focusTarget = nullptr;
}
void deferFocus(lv_obj_t* o) {
    if (!o) return;
    if (lcdKeyboardOnScreen() && lv_obj_check_type(o, &lv_textarea_class)) return;
    g_focusTarget = o;
    lv_timer_t* t = lv_timer_create(focusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

/* ---- confirm dialog: says what goes, then the verb or Cancel ---- */

lv_obj_t* s_confirm = nullptr;
void closeConfirm() {
    if (!s_confirm) return;
    lv_obj_t* o = s_confirm;
    s_confirm = nullptr;
    lv_obj_delete(o);
}
void showConfirm(const std::string& text, const char* verb, lv_event_cb_t onOk) {
    closeConfirm();
    s_confirm = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_size(s_confirm, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_50, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_confirm);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(85));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_DIALOG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, lcdPx(6), 0);
    lv_obj_set_style_pad_all(box, lcdPx(10), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, lcdPx(8), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* msg = mkLabel(box, text, 0xFFFFFF, kFont);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(msg, lv_pct(100));

    lv_obj_t* btns = mkRowBox(box, lcdPx(8));
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mkButton(btns, verb, BTN_DANGER, onOk);
    lv_obj_t* cancel = mkButton(btns, "Cancel", BTN_GHOST, [](lv_event_t*) { closeConfirm(); });

    lcdModalTrack(s_confirm, [](void*) { closeConfirm(); });
    deferFocus(cancel);   /* the safe answer sits under the cursor */
}

/* ---- screens ---- */

enum Screen { SC_LIST, SC_HUB, SC_ROOM, SC_INFO, SC_CARD };
Screen g_screen = SC_LIST;
bool   g_shown = false;                 /* the app is in the foreground */

lv_obj_t* s_list = nullptr;             /* list screen (built once) */
lv_obj_t* s_room = nullptr;             /* room screen (built once, reused per room) */
lv_obj_t* s_hub  = nullptr;             /* hub screen (rebuilt per open) */
lv_obj_t* s_info = nullptr;             /* room info (rebuilt per open) */
lv_obj_t* s_card = nullptr;             /* member card (rebuilt per open) */

void rebuildList();
void openHub(const std::string& hub, const std::string& room = "");
void openRoom(const std::string& rid, bool focus);
void openInfo();
void openCard(const std::string& id12, bool fromInfo);
void leaveRoom();
void refreshHub();
void refreshRoomHeader();
void refreshRoomNote();
void refreshInfo();
void maybeMarkRead();
void scheduleMeta();

/* ---- list: two tabs ---- */

lv_obj_t* s_tabBtnR = nullptr;          /* "Rooms" tab */
lv_obj_t* s_tabBtnH = nullptr;          /* "Hubs" tab */
lv_obj_t* s_tabRooms = nullptr;         /* Rooms rows (scrolls) */
lv_obj_t* s_tabHubs = nullptr;          /* Hubs rows (scrolls) */
lv_obj_t* s_addTa = nullptr;            /* the add-by-hash field, last row of Hubs */
int  g_tab = 0;                         /* 0 Rooms, 1 Hubs */
bool g_listStale = false;               /* changed while another screen was up */

/* Rows are reconciled, not rebuilt: each live row keeps a key (the rid or hub,
 * or a "\x01" sentinel for decoration) and a signature of what it shows. A
 * refresh reuses every row whose key and signature still match, recreates the
 * ones that changed, drops the ones that went and reorders the rest — so a new
 * line in one room touches one row, and the add field (and its text) stays. */
enum RowKind { RK_ROOM, RK_HUB, RK_HEARD, RK_SECT, RK_EMPTY, RK_ADD };
struct RowSpec {
    RowKind kind = RK_EMPTY;
    std::string key, sig, target, title, right, sub;
    int unread = 0, dot = DOT_NONE;
    bool mention = false, dim = false, pillDim = false;
};
struct ListRow { std::string key, sig; lv_obj_t* obj = nullptr; };
std::vector<ListRow> g_rowsR, g_rowsH;

void onRoomRow(lv_event_t* e) { openRoom(boundString(e), true); }
void onHubRow(lv_event_t* e)  { openHub(boundString(e)); }

void onAddHub(lv_event_t*) {
    if (!s_addTa) return;
    std::string h = lower(lv_textarea_get_text(s_addTa));
    h.erase(std::remove(h.begin(), h.end(), ' '), h.end());
    if (h.size() == 64 && isHex32(h.substr(0, 32)) && isHex32(h.substr(32))) h.resize(32);
    if (!isHex32(h)) {
        /* Not a hub hash yet: the field says so in amber until it is edited. */
        lv_obj_set_style_border_color(s_addTa, lv_color_hex(C_AMBER), 0);
        return;
    }
    storageSet("rrc.cmd.add", h.c_str());
    lv_textarea_set_text(s_addTa, "");
    openHub(h);
}

lv_obj_t* buildRow(lv_obj_t* list, const RowSpec& s) {
    switch (s.kind) {
    case RK_SECT:
        return mkSection(list, s.title);
    case RK_EMPTY: {
        lv_obj_t* l = mkLabel(list, s.title, C_SUB, kFont);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_pad_all(l, lcdPx(6), 0);
        return l;
    }
    case RK_ADD: {
        lv_obj_t* row = mkRowBox(list, lcdPx(6));
        lv_obj_set_style_pad_all(row, lcdPx(6), 0);
        s_addTa = lv_textarea_create(row);
        lv_textarea_set_one_line(s_addTa, true);
        lv_textarea_set_placeholder_text(s_addTa, "+ Add by hash (32 hex)");
        lv_textarea_set_accepted_chars(s_addTa, "0123456789abcdefABCDEF ");
        lv_textarea_set_max_length(s_addTa, 128);
        lv_obj_set_style_text_font(s_addTa, kFont, 0);
        lv_obj_set_style_bg_opa(s_addTa, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_addTa, lv_color_white(), 0);
        lv_obj_set_style_text_color(s_addTa, lv_color_hex(0x7FA6FF), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_border_width(s_addTa, 1, 0);
        lv_obj_set_style_border_color(s_addTa, lv_color_hex(C_FIELD), 0);
        lv_obj_set_style_radius(s_addTa, lcdPx(4), 0);
        lv_obj_set_style_pad_ver(s_addTa, lcdPx(3), 0);
        lv_obj_set_style_pad_hor(s_addTa, lcdPx(6), 0);
        lv_obj_set_width(s_addTa, 1);
        lv_obj_set_flex_grow(s_addTa, 1);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_addTa);
        lcdKeyboardAttach(s_addTa);
        lv_obj_add_event_cb(s_addTa, onAddHub, LV_EVENT_READY, nullptr);
        lv_obj_add_event_cb(s_addTa, [](lv_event_t*) {
            if (s_addTa) lv_obj_set_style_border_color(s_addTa, lv_color_hex(C_FIELD), 0);
        }, LV_EVENT_VALUE_CHANGED, nullptr);
        mkButton(row, "Add", BTN_PRIMARY, onAddHub);
        return row;
    }
    case RK_ROOM: {
        lv_obj_t* row = mkRow(list);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        bindString(row, onRoomRow, s.target);
        lv_obj_t* l1 = mkRowBox(row, lcdPx(4));
        mkFill(l1, s.title, s.dim ? C_SUB : 0xFFFFFF, s.dim ? kFont : kBold);
        if (s.dot != DOT_NONE) mkDot(l1, s.dot);
        if (s.mention) mkLabel(l1, "@", C_AMBER, kBold);
        if (!s.right.empty()) mkLabel(l1, s.right, C_SUB, kSmall);
        if (s.unread > 0) mkPill(l1, s.unread, s.pillDim);
        lv_obj_t* l2 = mkLabel(row, s.sub, C_SUB, kSmall);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l2, lv_pct(100));
        lv_obj_set_height(l2, lv_font_get_line_height(kSmall));
        return row;
    }
    case RK_HUB:
    case RK_HEARD: {
        lv_obj_t* row = mkRow(list);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, lcdPx(6), 0);
        lv_obj_set_style_pad_ver(row, lcdPx(4), 0);
        bindString(row, onHubRow, s.target);
        mkDot(row, s.dot);
        mkFill(row, s.title, 0xFFFFFF, s.kind == RK_HUB ? kBold : kFont);
        if (!s.right.empty()) mkLabel(row, s.right, C_SUB, kSmall);
        return row;
    }
    }
    return mkBox(list);
}

void applyRows(lv_obj_t* list, std::vector<ListRow>& model, const std::vector<RowSpec>& target) {
    std::map<std::string, size_t> have;
    for (size_t i = 0; i < model.size(); i++) if (model[i].obj) have.emplace(model[i].key, i);
    std::vector<char> used(model.size(), 0);
    std::vector<ListRow> out;
    out.reserve(target.size());
    for (auto& t : target) {
        lv_obj_t* obj = nullptr;
        auto it = have.find(t.key);
        if (it != have.end() && !used[it->second]) {
            used[it->second] = 1;
            ListRow& ex = model[it->second];
            if (ex.sig == t.sig) obj = ex.obj;
            else {
                if (s_addTa && lv_obj_get_parent(s_addTa) == ex.obj) s_addTa = nullptr;
                lv_obj_delete(ex.obj);
            }
        }
        out.push_back({ t.key, t.sig, obj });
    }
    for (size_t i = 0; i < model.size(); i++) {
        if (used[i] || !model[i].obj) continue;
        if (s_addTa && lv_obj_get_parent(s_addTa) == model[i].obj) s_addTa = nullptr;
        lv_obj_delete(model[i].obj);
    }
    model.swap(out);
    int32_t sy = lv_obj_get_scroll_y(list);
    for (size_t i = 0; i < model.size(); i++)
        if (!model[i].obj) model[i].obj = buildRow(list, target[i]);
    int32_t idx = 0;
    for (auto& r : model) if (r.obj) lv_obj_move_to_index(r.obj, idx++);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, sy, LV_ANIM_OFF);
}

RowSpec sectSpec(const std::string& t) {
    RowSpec s; s.kind = RK_SECT; s.key = "\x01.s." + t; s.sig = t; s.title = t; return s;
}
RowSpec emptySpec(const std::string& t) {
    RowSpec s; s.kind = RK_EMPTY; s.key = "\x01.e." + t; s.sig = t; s.title = t; return s;
}

int roomCount(const std::string& hub) {
    int n = 0;
    for (auto& [rid, r] : g_rooms) if (r.hub == hub) n++;
    return n;
}

/* Saved hubs, then the heard ones that are not saved, then (on the Hubs tab)
 * the add field. */
void hubSpecs(std::vector<RowSpec>& out, bool withAdd) {
    std::vector<std::pair<std::string, std::string>> saved;   /* name, hub */
    for (auto& h : g_saved) saved.push_back({ lower(hubName(h)), h });
    std::sort(saved.begin(), saved.end());
    if (!saved.empty()) out.push_back(sectSpec("SAVED"));
    for (auto& [lname, hub] : saved) {
        std::string st = hubState(hub);
        RowSpec s;
        s.kind = RK_HUB; s.key = "h." + hub; s.target = hub;
        s.title = hubName(hub);
        s.dot = st == "live" ? DOT_LIVE : st == "need_ident" ? DOT_ASK : hubActive(st) ? DOT_HOLLOW : DOT_NONE;
        if (st == "live") {
            int n = roomCount(hub);
            std::string rt = rttText(storageGetInt(hubRam(hub, "rtt").c_str(), 0));
            s.right = std::to_string(n) + (n == 1 ? " room" : " rooms") + (rt.empty() ? "" : " \xC2\xB7 " + rt);
        } else {
            s.right = hubStatus(hub);
        }
        s.sig = "H|" + s.title + "|" + s.right + "|" + std::to_string(s.dot);
        out.push_back(std::move(s));
    }
    bool heardHdr = false;
    long now = monoS();
    for (auto& h : g_heard) {
        if (isSaved(h.hub)) continue;
        if (!heardHdr) { out.push_back(sectSpec("ON THE MESH")); heardHdr = true; }
        RowSpec s;
        s.kind = RK_HEARD; s.key = "m." + h.hub; s.target = h.hub;
        std::string nm = printable(h.name, true);
        s.title = nm.empty() ? h.hub.substr(0, 8) : nm;
        s.right = std::to_string(h.hops) + (h.hops == 1 ? " hop" : " hops") + " \xC2\xB7 " + age(now - h.mono);
        s.sig = "M|" + s.title + "|" + std::to_string(h.hops);   /* age alone never churns a row */
        out.push_back(std::move(s));
    }
    if (saved.empty() && !heardHdr)
        out.push_back(emptySpec(withAdd ? "No hubs saved or heard yet. Hubs appear here as they announce."
                                        : "No hubs heard yet. They appear here as they announce."));
    if (withAdd) {
        RowSpec a; a.kind = RK_ADD; a.key = "\x01.add"; a.sig = "add";
        out.push_back(std::move(a));
    }
}

void rebuildList() {
    if (!s_tabRooms || !s_tabHubs) return;
    refreshFont();
    g_listStale = false;

    std::vector<const RoomRec*> rs;
    for (auto& [rid, r] : g_rooms) rs.push_back(&r);
    std::sort(rs.begin(), rs.end(), [](const RoomRec* a, const RoomRec* b) {
        if (a->last_ts != b->last_ts) return a->last_ts > b->last_ts;
        return a->name < b->name;
    });

    std::vector<RowSpec> tR, tH;
    for (auto* r : rs) {
        std::string st = hubState(r->hub);
        bool live = st == "live";
        RowSpec s;
        s.kind = RK_ROOM; s.key = "r." + r->rid; s.target = r->rid;
        s.title = roomLabel(r->name);
        s.right = live ? listTime(r->last_ts) : "";
        s.sub = hubName(r->hub) + " \xC2\xB7 " + (live ? printable(r->preview, true) : hubStatus(r->hub));
        s.dot = live ? DOT_NONE : st == "need_ident" ? DOT_ASK : DOT_HOLLOW;
        s.unread = (int)r->unread;
        s.mention = r->mentions > 0;
        s.dim = r->joined == 0;
        s.pillDim = r->notify == 2;
        s.sig = "R|" + s.title + "|" + s.right + "|" + s.sub + "|" + std::to_string(s.unread) + "|" +
                (s.mention ? "@" : "") + "|" + std::to_string(s.dot) + (s.dim ? "|d" : "") + (s.pillDim ? "|o" : "");
        tR.push_back(std::move(s));
    }
    if (rs.empty()) {
        tR.push_back(emptySpec("No rooms yet. Choose a hub to join a room or start one."));
        hubSpecs(tR, false);
    }
    hubSpecs(tH, true);

    applyRows(s_tabRooms, g_rowsR, tR);
    applyRows(s_tabHubs, g_rowsH, tH);
}

void styleTab(lv_obj_t* b, bool on) {
    if (!b) return;
    lv_obj_set_style_border_width(b, on ? lcdPx(2) : 0, 0);
    lv_obj_t* l = lv_obj_get_child(b, 0);
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(on ? 0xFFFFFF : C_SUB), 0);
}

void setTab(int n) {
    g_tab = n;
    if (s_tabRooms) { if (n == 0) lv_obj_remove_flag(s_tabRooms, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_tabRooms, LV_OBJ_FLAG_HIDDEN); }
    if (s_tabHubs)  { if (n == 1) lv_obj_remove_flag(s_tabHubs,  LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_tabHubs,  LV_OBJ_FLAG_HIDDEN); }
    styleTab(s_tabBtnR, n == 0);
    styleTab(s_tabBtnH, n == 1);
}

lv_obj_t* mkTab(lv_obj_t* bar, const char* text, int idx) {
    lv_obj_t* b = lv_button_create(bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_style_pad_hor(b, lcdPx(10), 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    focusRing(b);
    mkLabel(b, text, C_SUB, kFont);
    lv_obj_add_event_cb(b, [](lv_event_t* e) { setTab((int)(intptr_t)lv_event_get_user_data(e)); },
                        LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

lv_obj_t* mkTabPage(lv_obj_t* screen) {
    lv_obj_t* p = lv_obj_create(screen);
    lv_obj_remove_style_all(p);
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    return p;
}

void buildList() {
    s_list = mkScreen();
    lv_obj_t* bar = mkRowBox(s_list, 0);
    lv_obj_set_height(bar, lcdPx(24));
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    s_tabBtnR = mkTab(bar, "Rooms", 0);
    s_tabBtnH = mkTab(bar, "Hubs", 1);
    s_tabRooms = mkTabPage(s_list);
    s_tabHubs  = mkTabPage(s_list);
    setTab(g_tab);
}

/* ---- navigation ---- */

void setWatch(const std::string& hub, bool on) {
    if (hub.empty()) return;
    storageSet(hubRam(hub, "watch").c_str(), on ? 1 : 0);
}

std::string g_hub;                      /* hub on the hub screen */

void roomWatchUpdate();

void showScreen(Screen sc) {
    if (g_screen == SC_HUB && sc != SC_HUB) setWatch(g_hub, false);
    g_screen = sc;
    lv_obj_t* want = sc == SC_LIST ? s_list : sc == SC_HUB ? s_hub : sc == SC_ROOM ? s_room
                   : sc == SC_INFO ? s_info : s_card;
    for (lv_obj_t* o : { s_list, s_hub, s_room, s_info, s_card }) {
        if (!o) continue;
        if (o == want) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    roomWatchUpdate();
}

void dropPage(lv_obj_t*& o) {
    if (o) lv_obj_delete(o);
    o = nullptr;
}

void closeInfoPages();

void goList() {
    closeConfirm();
    leaveRoom();
    closeInfoPages();
    dropPage(s_hub);
    showScreen(SC_LIST);
    if (g_listStale) rebuildList();
}

/* ---- hub screen ---- */

Header    g_hubHdr;
lv_obj_t* s_hubAs = nullptr;            /* "As" value */
lv_obj_t* s_hubStatus = nullptr;        /* status sentence */
lv_obj_t* s_hubMotdBox = nullptr;       /* greeting section (hidden when empty) */
lv_obj_t* s_hubMotd = nullptr;
lv_obj_t* s_hubPick = nullptr;          /* who-to-chat-as chooser (need_ident only) */
lv_obj_t* s_hubPickGo = nullptr;        /* "Connect as …" */
lv_obj_t* s_hubPickGoLbl = nullptr;
lv_obj_t* s_hubDir = nullptr;           /* room directory rows */
lv_obj_t* s_hubJoinTa = nullptr;        /* join-by-name field */
lv_obj_t* s_hubConn = nullptr;          /* Connect / Disconnect */
lv_obj_t* s_hubConnLbl = nullptr;
lv_obj_t* s_hubForget = nullptr;
lv_obj_t* s_hubJoinSel = nullptr;       /* "Join #room" for the directory selection */
lv_obj_t* s_hubJoinSelLbl = nullptr;
std::string g_hubDir = "\x01";          /* rooms_dir text the rows were built from */
std::string g_hubJoinedSig;             /* the hub's joined rooms when the rows were built */
std::string g_hubSel;                   /* selected directory room (normalised name) */
int         g_hubPickShown = -1;        /* the chooser is built (1) / not (0) / unknown */
std::string g_pickSel;                  /* chosen identity: lxmf<n> | node */
std::map<std::string, std::string> g_pickLabels;

/* A join waiting for its directory record, so the room opens once it exists. */
std::string g_pendingHub, g_pendingRoom;

void joinRoom(const std::string& hub, const std::string& input) {
    std::string in = trim(input);
    std::string room = in.substr(0, in.find(' '));
    std::string key = in.find(' ') == std::string::npos ? "" : trim(in.substr(in.find(' ') + 1));
    std::string norm = normRoom(room);
    if (norm.empty()) return;
    std::string rid = findRid(hub, norm);
    if (!rid.empty() && g_rooms[rid].joined && key.empty()) { openRoom(rid, true); return; }
    std::string v = hub + "|" + norm + (key.empty() ? "" : "|" + key);
    storageSet("rrc.cmd.join", v.c_str());
    g_pendingHub = hub;
    g_pendingRoom = norm;
    if (!rid.empty()) { g_pendingHub.clear(); g_pendingRoom.clear(); openRoom(rid, true); }
}

void hubBack(lv_event_t*) { goList(); }

void onHubConn(lv_event_t*) {
    if (g_hub.empty()) return;
    bool active = hubActive(hubState(g_hub));
    storageSet(active ? "rrc.cmd.disconnect" : "rrc.cmd.connect", g_hub.c_str());
}

void onHubForget(lv_event_t*) {
    if (g_hub.empty()) return;
    showConfirm("Forget " + hubName(g_hub) + "? Its rooms and what was said in them go from this device too.",
                "Forget", [](lv_event_t*) {
        std::string hub = g_hub;
        closeConfirm();
        storageSet("rrc.cmd.forget", hub.c_str());
        g_tab = 1;
        setTab(1);
        goList();
    });
}

void onHubJoinSel(lv_event_t*) {
    if (!g_hub.empty() && !g_hubSel.empty()) joinRoom(g_hub, g_hubSel);
}

void onHubJoinTyped(lv_event_t*) {
    if (!s_hubJoinTa || g_hub.empty()) return;
    std::string t = lv_textarea_get_text(s_hubJoinTa);
    if (trim(t).empty()) return;
    lv_textarea_set_text(s_hubJoinTa, "");
    joinRoom(g_hub, t);
}

void reflectJoinSel() {
    if (!s_hubJoinSel || !s_hubJoinSelLbl) return;
    if (g_hubSel.empty()) { lv_obj_add_flag(s_hubJoinSel, LV_OBJ_FLAG_HIDDEN); return; }
    std::string rid = findRid(g_hub, g_hubSel);
    bool in = !rid.empty() && g_rooms[rid].joined;
    lv_label_set_text(s_hubJoinSelLbl, ((in ? "Open " : "Join ") + roomLabel(g_hubSel)).c_str());
    lv_obj_remove_flag(s_hubJoinSel, LV_OBJ_FLAG_HIDDEN);
}

void onDirRow(lv_event_t* e) {
    g_hubSel = boundString(e);
    if (s_hubDir) {
        uint32_t n = lv_obj_get_child_count(s_hubDir);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* c = lv_obj_get_child(s_hubDir, (int32_t)i);
            if (c == lv_event_get_current_target_obj(e)) lv_obj_add_state(c, LV_STATE_CHECKED);
            else lv_obj_remove_state(c, LV_STATE_CHECKED);
        }
    }
    reflectJoinSel();
}

void buildHubDir(const std::string& dir, bool live) {
    if (!s_hubDir) return;
    lv_obj_clean(s_hubDir);
    size_t at = 0;
    int rows = 0;
    while (at < dir.size()) {
        size_t e = dir.find('\n', at);
        std::string line = dir.substr(at, e == std::string::npos ? std::string::npos : e - at);
        at = e == std::string::npos ? dir.size() : e + 1;
        if (trim(line).empty()) continue;
        size_t bar = line.find('|');
        std::string name = normRoom(line.substr(0, bar));
        std::string topic = bar == std::string::npos ? "" : printable(line.substr(bar + 1), true);
        if (name.empty()) continue;
        lv_obj_t* row = mkRow(s_hubDir);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, lcdPx(6), 0);
        bindString(row, onDirRow, name);
        mkLabel(row, roomLabel(name), 0xFFFFFF, kBold);
        mkFill(row, topic, C_SUB, kSmall);
        std::string rid = findRid(g_hub, name);
        if (!rid.empty() && g_rooms[rid].joined) mkLabel(row, "joined", C_ACCENT, kSmallBold);
        if (name == g_hubSel) lv_obj_add_state(row, LV_STATE_CHECKED);
        rows++;
    }
    if (!rows) {
        lv_obj_t* l = mkLabel(s_hubDir, live ? "This hub lists no public rooms. Join one by name below."
                                             : "Its public rooms are listed once it is connected.",
                              C_SUB, kSmall);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_pad_all(l, lcdPx(6), 0);
    }
    /* A selection the directory does not (or no longer) lists stays: a room
     * named by a link joins by name all the same. */
    reflectJoinSel();
}

void onPickRow(lv_event_t* e) {
    g_pickSel = boundString(e);
    if (s_hubPick) {
        uint32_t n = lv_obj_get_child_count(s_hubPick);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* c = lv_obj_get_child(s_hubPick, (int32_t)i);
            if (c == lv_event_get_current_target_obj(e)) lv_obj_add_state(c, LV_STATE_CHECKED);
            else lv_obj_remove_state(c, LV_STATE_CHECKED);
        }
    }
    if (s_hubPickGo && s_hubPickGoLbl) {
        lv_label_set_text(s_hubPickGoLbl, ("Connect as " + g_pickLabels[g_pickSel]).c_str());
        lv_obj_remove_flag(s_hubPickGo, LV_OBJ_FLAG_HIDDEN);
    }
}

void buildPicker(bool show) {
    if (!s_hubPick) return;
    lv_obj_clean(s_hubPick);
    s_hubPickGo = nullptr;
    s_hubPickGoLbl = nullptr;
    g_pickSel.clear();
    g_pickLabels.clear();
    g_hubPickShown = show ? 1 : 0;
    if (!show) { lv_obj_add_flag(s_hubPick, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(s_hubPick, LV_OBJ_FLAG_HIDDEN);
    mkSection(s_hubPick, "CHAT AS");
    lv_obj_t* cap = mkLabel(s_hubPick, "The hub knows you by this identity, and whoever is in the room can "
                                       "message you back on it. It is kept for this hub.", C_SUB, kSmall);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(cap, lv_pct(100));
    lv_obj_set_style_pad_hor(cap, lcdPx(6), 0);

    auto addChoice = [](const std::string& sel, const std::string& label, const std::string& sub) {
        lv_obj_t* row = mkRow(s_hubPick);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        bindString(row, onPickRow, sel);
        mkLabel(row, label, 0xFFFFFF, kBold);
        mkLabel(row, sub, C_SUB, kSmall);
        g_pickLabels[sel] = label;
    };
    for (int n = 0; n < LXMF_SLOTS; n++) {
        char k[48];
        snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
        std::string dest = storageGetStr(k, "");
        if (dest.empty()) continue;
        snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
        if (storageGetStr(k, "") == "group") continue;
        std::string sel = "lxmf" + std::to_string(n);
        addChoice(sel, identLabel(sel), "LXMF " + fours(dest.substr(0, 16)) + " \xE2\x80\xA6");
    }
    addChoice("node", "This node", "the node's own identity, kept apart from LXMF");

    lv_obj_t* bar = mkRowBox(s_hubPick, 0);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(bar, lcdPx(4), 0);
    s_hubPickGo = mkButton(bar, "Connect as", BTN_PRIMARY, [](lv_event_t*) {
        if (g_hub.empty() || g_pickSel.empty()) return;
        storageSet("rrc.cmd.connect", (g_hub + "|" + g_pickSel).c_str());
    }, nullptr, &s_hubPickGoLbl);
    lv_obj_add_flag(s_hubPickGo, LV_OBJ_FLAG_HIDDEN);   /* until a choice is made */
}

void refreshHub() {
    if (!s_hub || g_hub.empty()) return;
    std::string st = hubState(g_hub);
    bool live = st == "live";

    int hops = live ? storageGetInt(hubRam(g_hub, "hops").c_str(), 0) : 0;
    if (!hops) if (const Heard* h = heardOf(g_hub)) hops = h->hops;
    setTitleLine(g_hubHdr.line, hubName(g_hub),
                 hops > 0 ? " \xC2\xB7 " + std::to_string(hops) + (hops == 1 ? " hop" : " hops") : "");

    std::string as = printable(storageGetStr(hubRam(g_hub, "as").c_str(), ""), true);
    std::string nick = printable(storageGetStr(hubRam(g_hub, "nick").c_str(), ""), true);
    if (as.empty()) as = identLabel(storageGetStr(("s.rrc.hubs." + g_hub + ".ident").c_str(), ""));
    std::string asLine = as.empty() ? "Chosen at the first connect" : as + (nick.empty() ? "" : " \xC2\xB7 " + nick);
    if (s_hubAs) lv_label_set_text(s_hubAs, asLine.c_str());

    if (s_hubStatus) {
        lv_label_set_text(s_hubStatus, hubStatus(g_hub).c_str());
        lv_obj_set_style_text_color(s_hubStatus, lv_color_hex(live ? C_OK : st == "need_ident" ? C_AMBER : 0xFFFFFF), 0);
    }

    std::string motd = printable(storageGetStr(hubRam(g_hub, "motd").c_str(), ""), false);
    if (s_hubMotdBox && s_hubMotd) {
        lv_label_set_text(s_hubMotd, motd.c_str());
        if (motd.empty()) lv_obj_add_flag(s_hubMotdBox, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_remove_flag(s_hubMotdBox, LV_OBJ_FLAG_HIDDEN);
    }

    bool ask = st == "need_ident";
    if (g_hubPickShown != (ask ? 1 : 0)) buildPicker(ask);

    std::string dir = storageGetStr(hubRam(g_hub, "rooms_dir").c_str(), "");
    if (dir != g_hubDir) { g_hubDir = dir; buildHubDir(dir, live); }
    else reflectJoinSel();

    if (s_hubConn && s_hubConnLbl) {
        if (ask) lv_obj_add_flag(s_hubConn, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(s_hubConn, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_hubConnLbl, hubActive(st) ? "Disconnect" : "Connect");
        }
    }
    if (s_hubForget) {
        if (isSaved(g_hub)) lv_obj_remove_flag(s_hubForget, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(s_hubForget, LV_OBJ_FLAG_HIDDEN);
    }
}

void openHub(const std::string& hub, const std::string& room) {
    refreshFont();
    closeConfirm();
    leaveRoom();
    closeInfoPages();
    if (g_screen == SC_HUB && hub != g_hub) setWatch(g_hub, false);
    dropPage(s_hub);
    g_hub = hub;
    g_hubDir = "\x01";
    g_hubJoinedSig.clear();
    g_hubSel = normRoom(room);
    g_hubPickShown = -1;
    g_pickSel.clear();

    s_hub = mkScreen();
    g_hubHdr = mkHeader(s_hub, hubBack, nullptr);
    lv_obj_t* body = mkBody(s_hub);

    lv_obj_t* hash = mkKv(body, "Hash", fours(hub), C_SUB, kSmall);
    (void)hash;
    s_hubAs = mkKv(body, "As", "", 0xFFFFFF, kFont);
    s_hubStatus = mkKv(body, "Status", "", 0xFFFFFF, kFont);

    s_hubMotdBox = mkBox(body);
    lv_obj_set_width(s_hubMotdBox, lv_pct(100));
    lv_obj_set_flex_flow(s_hubMotdBox, LV_FLEX_FLOW_COLUMN);
    mkSection(s_hubMotdBox, "MESSAGE OF THE DAY");
    s_hubMotd = mkLabel(s_hubMotdBox, "", C_ACT, kSmall);
    lv_label_set_long_mode(s_hubMotd, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_hubMotd, lv_pct(100));
    lv_obj_set_style_pad_hor(s_hubMotd, lcdPx(6), 0);

    s_hubPick = mkBox(body);
    lv_obj_set_width(s_hubPick, lv_pct(100));
    lv_obj_set_flex_flow(s_hubPick, LV_FLEX_FLOW_COLUMN);

    mkSection(body, "ROOMS ON THIS HUB");
    s_hubDir = mkBox(body);
    lv_obj_set_width(s_hubDir, lv_pct(100));
    lv_obj_set_flex_flow(s_hubDir, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* jr = mkRowBox(body, lcdPx(6));
    lv_obj_set_style_pad_ver(jr, lcdPx(4), 0);
    s_hubJoinTa = lv_textarea_create(jr);
    lv_textarea_set_one_line(s_hubJoinTa, true);
    lv_textarea_set_placeholder_text(s_hubJoinTa, "Join or start a room by name");
    lv_obj_set_style_text_font(s_hubJoinTa, kFont, 0);
    lv_obj_set_style_bg_opa(s_hubJoinTa, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_hubJoinTa, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_hubJoinTa, lv_color_hex(C_PRES), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(s_hubJoinTa, 1, 0);
    lv_obj_set_style_border_color(s_hubJoinTa, lv_color_hex(C_FIELD), 0);
    lv_obj_set_style_radius(s_hubJoinTa, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(s_hubJoinTa, lcdPx(3), 0);
    lv_obj_set_style_pad_hor(s_hubJoinTa, lcdPx(6), 0);
    lv_obj_set_width(s_hubJoinTa, 1);
    lv_obj_set_flex_grow(s_hubJoinTa, 1);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_hubJoinTa);
    lcdKeyboardAttach(s_hubJoinTa);
    lv_obj_add_event_cb(s_hubJoinTa, onHubJoinTyped, LV_EVENT_READY, nullptr);
    mkButton(jr, "Join", BTN_PRIMARY, onHubJoinTyped);
    if (!g_hubSel.empty()) lv_textarea_set_text(s_hubJoinTa, roomLabel(g_hubSel).c_str());

    lv_obj_t* bar = mkActionBar(s_hub);
    s_hubForget = mkButton(bar, "Forget", BTN_DANGER, onHubForget);
    lv_obj_t* spacer = mkBox(bar);
    lv_obj_set_flex_grow(spacer, 1);
    s_hubConn = mkButton(bar, "Connect", BTN_GHOST, onHubConn, nullptr, &s_hubConnLbl);
    s_hubJoinSel = mkButton(bar, "Join", BTN_PRIMARY, onHubJoinSel, nullptr, &s_hubJoinSelLbl);
    lv_obj_add_flag(s_hubJoinSel, LV_OBJ_FLAG_HIDDEN);

    showScreen(SC_HUB);
    if (g_shown) setWatch(hub, true);
    refreshHub();
    deferFocus(g_hubHdr.back);
}

/* ---- room screen ---- */

std::string g_rid;                      /* the open room */

/* The room whose member list (its info page) is in front of somebody: the app
 * shown, the screen out of standby, input within WATCH_IDLE_MS. The node
 * refreshes a stale roster only for such a room, so it is told, as a lease it
 * drops after 45 s: rrc.cmd.watch "lcd|<rid>", renewed every WATCH_RENEW_MS
 * by g_watchTimer, and "lcd|" when that ends. */
const uint32_t WATCH_IDLE_MS  = 5 * 60 * 1000;
const int64_t  WATCH_RENEW_MS = 15000;
std::string g_watchRid;
int64_t     g_watchSentMs = 0;
lv_timer_t* g_watchTimer = nullptr;

void roomWatchUpdate() {
    std::string rid = s_layer && g_shown && g_screen == SC_INFO && !storageGetInt("sys.standby", 0) &&
                      lcdIdleMs() < WATCH_IDLE_MS ? g_rid : "";
    int64_t now = esp_timer_get_time() / 1000;
    if (rid == g_watchRid && (rid.empty() || now - g_watchSentMs < WATCH_RENEW_MS)) return;
    g_watchRid = rid;
    g_watchSentMs = now;
    storageSet("rrc.cmd.watch", ("lcd|" + rid).c_str());
}

void watchTimerCb(lv_timer_t*) { roomWatchUpdate(); }

std::string g_logScope;                 /* its log subscription, "s.rrc.log.<rid>" */
Header    g_roomHdr;
lv_obj_t* s_log = nullptr;              /* the log's scroll container */
lv_obj_t* s_logCol = nullptr;           /* the line rows, in order */
lv_obj_t* s_loading = nullptr;          /* placeholder while the log loads */
lv_obj_t* s_chev = nullptr;             /* scroll-to-bottom chevron */
lv_obj_t* s_compose = nullptr;          /* the composer (lcdInputBox) */
lv_obj_t* s_count = nullptr;            /* bytes left when near the hub's limit */
lv_obj_t* s_note = nullptr;             /* the room's error, or its hub's status */
lv_timer_t* g_loadTimer = nullptr;
std::map<std::string, std::string> g_drafts;   /* rid → unsent composer text, this session */

enum RowT { R_LINE, R_PRES, R_DIV };
struct LRow { RowT t; uint32_t first, last; bool showTime; lv_obj_t* obj; };
std::vector<LRow> g_lrows;              /* the drawn rows, top to bottom */
uint32_t g_winFrom = 0;                 /* oldest seq in the drawn window */
uint32_t g_divAt = 0;                   /* the room was read up to here when opened */
uint32_t g_divCount = 0;                /* …with this many unread after it */
bool     g_divPlaced = false;
long     g_lastMinute = -1;             /* minute of the last line drawn with a time */
int32_t  g_textW = 0;                   /* width of a line's text column */
uint32_t g_readSent = 0;                /* the newest seq already reported read */
bool     g_collapse = true;             /* s.rrc.presence == collapsed */
bool     g_hidePresence = false;        /* s.rrc.presence == hidden */
void readPresenceMode() {
    std::string p = storageGetStr("s.rrc.presence", "collapsed");
    g_collapse = p == "collapsed";
    g_hidePresence = p == "hidden";
}

std::set<uint32_t> g_dirtySeqs;         /* log lines changed since the last patch */
bool g_logReload = false;               /* the log changed wholesale (cleared, dropped) */

std::string s_logPfx;
void logLeaf(const char* key, const char* val) {
    if (!key || strncmp(key, s_logPfx.c_str(), s_logPfx.size())) return;
    const char* t = key + s_logPfx.size();
    char* end = nullptr;
    unsigned long seq = strtoul(t, &end, 10);
    if (end == t || *end != '.') return;
    const char* f = end + 1;
    if (strchr(f, '.')) return;
    Line& l = g_lines[(uint32_t)seq];
    l.seq = (uint32_t)seq;
    const char* v = val ? val : "";
    if      (!strcmp(f, "ts"))      l.ts = (uint32_t)strtoul(v, nullptr, 10);
    else if (!strcmp(f, "kind"))    l.kind = atoi(v);
    else if (!strcmp(f, "state"))   l.state = atoi(v);
    else if (!strcmp(f, "mention")) l.mention = atoi(v) != 0;
    else if (!strcmp(f, "own"))     l.own = atoi(v) != 0;
    else if (!strcmp(f, "src"))     l.src = v;
    else if (!strcmp(f, "id"))      l.id = v;
    else if (!strcmp(f, "nick"))    l.nick = v;
    else if (!strcmp(f, "text"))    l.text = v;
}

/* Joins and leaves are never logged: the node keeps the latest in a RAM ring
 * per room (rrc.rooms.<rid>.pres.<n> = "<ts>|<seq>|<j|l>|<id12>|<nick>",
 * pres_next counting every one written), each at a seq the log leaves empty.
 * The open room collects those that arrive while it is open into g_pres,
 * merged into g_lines, and drops them when it closes. */
const int PRES_RING = 32;
std::map<uint32_t, Line> g_pres;
int g_presSeen = 0;

int presNext() { return storageGetInt(("rrc.rooms." + g_rid + ".pres_next").c_str(), 0); }

void loadLog() {
    g_lines.clear();
    if (g_rid.empty()) return;
    s_logPfx = "s.rrc.log." + g_rid + ".";
    storageForEach(s_logPfx.c_str(), logLeaf);
    for (auto& [seq, l] : g_pres) g_lines.emplace(seq, l);
}

std::string lineBase(uint32_t seq) {
    char b[16];
    snprintf(b, sizeof b, "%08u", (unsigned)seq);
    return "s.rrc.log." + g_rid + "." + b + ".";
}

bool readLine(uint32_t seq, Line& l) {
    std::string base = lineBase(seq);
    if (!storageExists((base + "ts").c_str())) return false;
    l.seq = seq;
    l.ts = (uint32_t)storageGetInt((base + "ts").c_str(), 0);
    l.kind = storageGetInt((base + "kind").c_str(), KIND_MSG);
    l.state = storageGetInt((base + "state").c_str(), ST_RECEIVED);
    l.mention = storageGetInt((base + "mention").c_str(), 0) != 0;
    l.own = storageGetInt((base + "own").c_str(), 0) != 0;
    l.src = storageGetStr((base + "src").c_str(), "");
    l.id = storageGetStr((base + "id").c_str(), "");
    l.nick = storageGetStr((base + "nick").c_str(), "");
    l.text = storageGetStr((base + "text").c_str(), "");
    return true;
}

bool logAtBottom() { return s_log && lv_obj_get_scroll_bottom(s_log) <= 4; }
bool lcdAwake()    { return storageGetInt("sys.standby", 0) == 0; }

void updateChevron() {
    if (!s_chev || !s_log) return;
    if (lv_obj_get_scroll_bottom(s_log) <= 4) lv_obj_add_flag(s_chev, LV_OBJ_FLAG_HIDDEN);
    else                                      lv_obj_remove_flag(s_chev, LV_OBJ_FLAG_HIDDEN);
}

void scrollBottom(bool anim) {
    if (!s_log) return;
    lv_obj_update_layout(s_log);
    lv_obj_scroll_to_y(s_log, LV_COORD_MAX, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    updateChevron();
}

lv_span_t* addSpan(lv_obj_t* sg, const std::string& t, lv_color_t c, const lv_font_t* f) {
    if (t.empty()) return nullptr;
    lv_span_t* sp = lv_spangroup_add_span(sg);
    lv_style_t* st = lv_span_get_style(sp);
    lv_style_set_text_color(st, c);
    if (f) lv_style_set_text_font(st, f);
    lv_spangroup_set_span_text(sg, sp, t.c_str());
    return sp;
}

std::string nickOf(const Line& l) {
    std::string n = printable(l.nick, true);
    return n.empty() ? l.src.substr(0, 8) : n;
}

void onRetry(lv_event_t* e) {
    uint32_t seq = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (g_rid.empty()) return;
    storageSet("rrc.cmd.retry", (g_rid + "|" + std::to_string(seq)).c_str());
}

/* One row of the log: the gutter, then a spangroup at the text column whose
 * wrapped lines all start at that column — the hanging indent. */
lv_obj_t* newLogRow(lv_obj_t** sgOut, bool mention) {
    lv_obj_t* row = lv_obj_create(s_logCol);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* sg = lv_spangroup_create(row);
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(sg, gutterW(), 0);
    lv_obj_set_width(sg, g_textW);
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(sg, kFont, 0);
    lv_obj_set_style_text_color(sg, lv_color_white(), 0);
    lv_obj_set_style_pad_left(sg, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(sg, 1, 0);
    if (mention) {
        lv_obj_set_style_border_side(sg, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(sg, lcdPx(2), 0);
        lv_obj_set_style_border_color(sg, lv_color_hex(C_AMBER), 0);
    }
    *sgOut = sg;
    return row;
}

void gutterTime(lv_obj_t* row, uint32_t ts) {
    lv_obj_t* t = mkLabel(row, hhmm(ts), C_GUTTER, kSmall);
    lv_obj_set_width(t, gutterW());
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);
    int dy = (lv_font_get_line_height(kFont) - lv_font_get_line_height(kSmall)) * 2 / 3 + 1;
    lv_obj_set_pos(t, 0, dy > 0 ? dy : 0);
}

lv_obj_t* buildLineRow(const Line& l, bool showTime) {
    lv_obj_t* sg = nullptr;
    lv_obj_t* row = newLogRow(&sg, l.mention);
    if (showTime && l.ts) gutterTime(row, l.ts);
    std::string text = printable(l.text, false);
    lv_color_t white = lv_color_white();
    switch (l.kind) {
    case KIND_ACTION:
        addSpan(sg, "* ", lv_color_hex(C_ACT), kItalic);
        addSpan(sg, nickOf(l), nickColor(l.src), kBold);
        addSpan(sg, " " + text, lv_color_hex(C_ACT), kItalic);
        break;
    case KIND_NOTICE:
        if (!l.nick.empty()) addSpan(sg, nickOf(l) + " ", lv_color_hex(C_NOTE), kSmallBold);
        addSpan(sg, text, lv_color_hex(C_NOTE), kSmall);
        break;
    case KIND_PRESENCE:
        addSpan(sg, nickOf(l) + " " + text, lv_color_hex(C_PRES), kSmall);
        break;
    case KIND_SYSTEM:
        addSpan(sg, text, lv_color_hex(C_PRES), kSmall);
        break;
    default:
        addSpan(sg, nickOf(l), nickColor(l.src), kBold);
        addSpan(sg, " " + text, white, kFont);
        break;
    }
    if (l.own && (l.kind == KIND_MSG || l.kind == KIND_ACTION)) {
        if (l.state == ST_RELAYED)     addSpan(sg, " " LV_SYMBOL_OK, lv_color_hex(C_OK), kSmall);
        else if (l.state == ST_QUEUED) addSpan(sg, " " LV_SYMBOL_REFRESH, lv_color_hex(C_PRES), kSmall);
        else if (l.state == ST_FAILED) {
            addSpan(sg, " " LV_SYMBOL_CLOSE " not sent, tap to retry", lv_color_hex(C_FAIL), kSmall);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, onRetry, LV_EVENT_CLICKED, (void*)(uintptr_t)l.seq);
        }
    }
    lv_spangroup_refresh(sg);
    return row;
}

/* A run of joins and leaves as one line: "dana and 2 others joined". */
std::string presenceRun(uint32_t first, uint32_t last) {
    std::vector<std::string> joined, left;
    auto addUnique = [](std::vector<std::string>& v, const std::string& n) {
        if (std::find(v.begin(), v.end(), n) == v.end()) v.push_back(n);
    };
    for (auto it = g_lines.lower_bound(first); it != g_lines.end() && it->first <= last; ++it) {
        const Line& l = it->second;
        if (l.kind != KIND_PRESENCE) continue;
        addUnique(l.text == "left" ? left : joined, nickOf(l));
    }
    auto phrase = [](const std::vector<std::string>& v) {
        if (v.size() == 1) return v[0];
        if (v.size() == 2) return v[0] + " and " + v[1];
        return v[0] + " and " + std::to_string(v.size() - 1) + " others";
    };
    std::string out;
    if (!joined.empty()) out = phrase(joined) + " joined";
    if (!left.empty()) out += (out.empty() ? "" : ", ") + phrase(left) + " left";
    return out;
}

lv_obj_t* buildPresRow(uint32_t first, uint32_t last) {
    lv_obj_t* sg = nullptr;
    lv_obj_t* row = newLogRow(&sg, false);
    addSpan(sg, presenceRun(first, last), lv_color_hex(C_PRES), kSmall);
    lv_spangroup_refresh(sg);
    return row;
}

lv_obj_t* buildDivider() {
    lv_obj_t* row = mkRowBox(s_logCol, lcdPx(6));
    lv_obj_set_style_pad_left(row, gutterW(), 0);
    lv_obj_set_style_pad_right(row, lcdPx(6), 0);
    lv_obj_set_style_pad_ver(row, lcdPx(2), 0);
    auto rule = [](lv_obj_t* p) {
        lv_obj_t* r = mkBox(p);
        lv_obj_set_height(r, 1);
        lv_obj_set_width(r, 1);
        lv_obj_set_flex_grow(r, 1);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_NEWLINE), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    };
    rule(row);
    char b[24];
    snprintf(b, sizeof b, "%u new", (unsigned)g_divCount);
    mkLabel(row, b, C_AMBER, kSmall);
    rule(row);
    return row;
}

/* Append rows for every line from `from` on, folding presence runs and placing
 * the divider (on a full draw only). */
void renderFrom(uint32_t from, bool allowDiv) {
    for (auto it = g_lines.lower_bound(from); it != g_lines.end();) {
        const Line& l = it->second;
        if (allowDiv && !g_divPlaced && g_divAt && l.seq > g_divAt) {
            g_lrows.push_back({ R_DIV, l.seq, l.seq, false, buildDivider() });
            g_divPlaced = true;
        }
        if (l.kind == KIND_PRESENCE && g_hidePresence) { ++it; continue; }
        if (l.kind == KIND_PRESENCE && g_collapse) {
            uint32_t first = l.seq, last = l.seq;
            auto j = it;
            for (++j; j != g_lines.end() && j->second.kind == KIND_PRESENCE; ++j) last = j->first;
            g_lrows.push_back({ R_PRES, first, last, false, buildPresRow(first, last) });
            it = j;
            continue;
        }
        bool timed = l.kind != KIND_PRESENCE;
        long minute = (long)(l.ts / 60);
        bool show = timed && l.ts && minute != g_lastMinute;
        if (show) g_lastMinute = minute;
        g_lrows.push_back({ R_LINE, l.seq, l.seq, show, buildLineRow(l, show) });
        ++it;
    }
}

void measureText() {
    if (!s_logCol) return;
    lv_obj_update_layout(s_room);
    g_textW = lv_obj_get_content_width(s_logCol) - gutterW();
    if (g_textW < lcdPx(80)) g_textW = lcdPx(80);
}

void renderAll() {
    if (!s_logCol) return;
    lv_obj_clean(s_logCol);
    g_lrows.clear();
    g_lastMinute = -1;
    g_divPlaced = false;
    measureText();
    renderFrom(g_winFrom, true);
}

/* New lines past the last drawn one; a presence line continuing a folded run
 * redraws that run. */
void appendNew() {
    uint32_t from = g_winFrom;
    if (!g_lrows.empty()) {
        LRow& b = g_lrows.back();
        from = b.last + 1;
        if (b.t == R_PRES && g_collapse) {
            auto nx = g_lines.lower_bound(from);
            if (nx != g_lines.end() && nx->second.kind == KIND_PRESENCE) {
                from = b.first;
                lv_obj_delete(b.obj);
                g_lrows.pop_back();
            }
        }
    }
    renderFrom(from, false);
}

void rerenderLine(uint32_t seq) {
    for (auto& r : g_lrows) {
        if (r.t != R_LINE || r.first != seq) continue;
        auto it = g_lines.find(seq);
        if (it == g_lines.end()) return;
        int32_t idx = lv_obj_get_index(r.obj);
        lv_obj_t* n = buildLineRow(it->second, r.showTime);
        lv_obj_move_to_index(n, idx);
        lv_obj_delete(r.obj);
        r.obj = n;
        return;
    }
}

/* Hold what is at the top of the view across a redraw. */
uint32_t g_anchorSeq = 0;
int32_t  g_anchorOff = 0;
void captureAnchor() {
    g_anchorSeq = 0;
    if (!s_log || g_lrows.empty()) return;
    lv_obj_update_layout(s_log);
    int32_t top = lv_obj_get_scroll_y(s_log);
    int32_t base = lv_obj_get_y(s_logCol);
    for (auto& r : g_lrows) {
        int32_t y = base + lv_obj_get_y(r.obj);
        if (y + lv_obj_get_height(r.obj) > top) { g_anchorSeq = r.first; g_anchorOff = y - top; return; }
    }
}
void restoreAnchor() {
    if (!s_log) return;
    lv_obj_update_layout(s_log);
    if (!g_anchorSeq) { scrollBottom(false); return; }
    int32_t base = lv_obj_get_y(s_logCol);
    for (auto& r : g_lrows) {
        if (r.t == R_DIV || r.last < g_anchorSeq) continue;
        lv_obj_scroll_to_y(s_log, base + lv_obj_get_y(r.obj) - g_anchorOff, LV_ANIM_OFF);
        updateChevron();
        return;
    }
    scrollBottom(false);
}

/* Draw PAGE older lines above the window, holding the view still. */
void loadEarlier() {
    if (g_lines.empty() || g_lines.begin()->first >= g_winFrom) return;
    captureAnchor();
    auto it = g_lines.lower_bound(g_winFrom);
    for (size_t n = 0; n < PAGE && it != g_lines.begin(); n++) --it;
    g_winFrom = it->first;
    renderAll();
    restoreAnchor();
}

/* Tell the node this room has been read: only with its newest line in view at
 * the bottom of the log, this screen in the foreground and the screen awake. */
void maybeMarkRead() {
    if (g_screen != SC_ROOM || !g_shown || g_rid.empty() || g_lines.empty()) return;
    if (g_loadTimer || !logAtBottom() || !lcdAwake()) return;
    uint32_t newest = g_lines.rbegin()->first;
    if (newest == g_readSent) return;
    std::string base = "s.rrc.rooms." + g_rid + ".";
    uint32_t rs = (uint32_t)storageGetInt((base + "read_seq").c_str(), 0);
    int unread = storageGetInt((base + "unread").c_str(), 0);
    if (newest <= rs && unread == 0) return;
    g_readSent = newest;
    storageSet("rrc.cmd.read", (g_rid + "|" + std::to_string(newest)).c_str());
}

void onLogScrollEnd(lv_event_t*) {
    updateChevron();
    if (s_log && lv_obj_get_scroll_top(s_log) <= 2) loadEarlier();
    maybeMarkRead();
}

void loadTimerCb(lv_timer_t*) {
    g_loadTimer = nullptr;
    if (g_rid.empty() || !s_logCol) return;
    g_dirtySeqs.clear();
    g_logReload = false;
    loadLog();
    /* The newest PAGE, reaching back to the divider when there is more unread
     * than that (up to FIRST_MAX). */
    g_winFrom = 0;
    if (!g_lines.empty()) {
        auto it = g_lines.end();
        for (size_t n = 0; n < PAGE && it != g_lines.begin(); n++) --it;
        g_winFrom = it->first;
        if (g_divAt) {
            auto d = g_lines.upper_bound(g_divAt);
            if (d != g_lines.end() && d->first < g_winFrom) {
                auto lim = g_lines.end();
                for (size_t n = 0; n < FIRST_MAX && lim != g_lines.begin(); n++) --lim;
                g_winFrom = std::max(d->first, lim->first);
            }
        }
    }
    if (s_loading) lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
    renderAll();
    scrollBottom(false);
    /* More unread than fits: start at the divider instead of the bottom. */
    for (auto& r : g_lrows) {
        if (r.t != R_DIV) continue;
        lv_obj_update_layout(s_log);
        int32_t y = lv_obj_get_y(s_logCol) + lv_obj_get_y(r.obj);
        if (y < lv_obj_get_scroll_y(s_log)) lv_obj_scroll_to_y(s_log, y, LV_ANIM_OFF);
        updateChevron();
        break;
    }
    maybeMarkRead();
}

/* Patch the drawn log with what changed: new lines append, a line whose
 * delivery state moved is redrawn alone, anything else redraws the window. */
void patchLog() {
    if (g_rid.empty() || !s_logCol || g_loadTimer) return;
    bool bottom = logAtBottom();
    if (g_logReload) {
        g_logReload = false;
        g_dirtySeqs.clear();
        captureAnchor();
        loadLog();
        if (!g_lines.empty() && g_winFrom < g_lines.begin()->first) g_winFrom = g_lines.begin()->first;
        renderAll();
        if (bottom) scrollBottom(false); else restoreAnchor();
        maybeMarkRead();
        return;
    }
    uint32_t lastDrawn = g_lrows.empty() ? (g_winFrom ? g_winFrom - 1 : 0) : g_lrows.back().last;
    bool structural = false, grew = false;
    std::vector<uint32_t> redraw;
    for (uint32_t seq : g_dirtySeqs) {
        Line l;
        bool exists = readLine(seq, l);
        auto it = g_lines.find(seq);
        if (!exists) {
            if (it != g_lines.end()) {
                g_lines.erase(it);
                if (seq >= g_winFrom && seq <= lastDrawn) structural = true;
            }
            continue;
        }
        if (it == g_lines.end()) {
            g_lines[seq] = l;
            if (seq > lastDrawn || g_lrows.empty()) grew = true;
            else if (seq >= g_winFrom) structural = true;
            continue;
        }
        bool changed = it->second.state != l.state || it->second.text != l.text ||
                       it->second.nick != l.nick || it->second.mention != l.mention;
        it->second = l;
        if (changed && seq >= g_winFrom && seq <= lastDrawn) redraw.push_back(seq);
    }
    g_dirtySeqs.clear();
    if (structural) {
        captureAnchor();
        if (!g_lines.empty() && g_winFrom < g_lines.begin()->first) g_winFrom = g_lines.begin()->first;
        renderAll();
        if (bottom) scrollBottom(false); else restoreAnchor();
    } else {
        for (uint32_t s : redraw) rerenderLine(s);
        if (grew) appendNew();
        if (grew || !redraw.empty()) {
            if (bottom) scrollBottom(false);
            else updateChevron();
        }
    }
    maybeMarkRead();
}

/* Joins and leaves written to the ring since we last looked: into the open
 * room, drawn at the bottom or, if one lands among drawn lines, by a redraw. */
void presCollect() {
    if (g_rid.empty()) return;
    int next = presNext();
    if (next < g_presSeen) g_presSeen = next;          /* the node restarted its count */
    int from = std::max(g_presSeen, next - PRES_RING);
    g_presSeen = next;
    if (from >= next) return;
    uint32_t lastDrawn = g_lrows.empty() ? (g_winFrom ? g_winFrom - 1 : 0) : g_lrows.back().last;
    bool grew = false, structural = false;
    for (int n = from; n < next; n++) {
        std::string raw = storageGetStr(("rrc.rooms." + g_rid + ".pres." + std::to_string(n % PRES_RING)).c_str(), "");
        size_t a = raw.find('|'), b = a == std::string::npos ? a : raw.find('|', a + 1);
        size_t c = b == std::string::npos ? b : raw.find('|', b + 1);
        size_t d = c == std::string::npos ? c : raw.find('|', c + 1);
        if (d == std::string::npos) continue;
        Line l;
        l.ts = (uint32_t)strtoul(raw.c_str(), nullptr, 10);
        l.seq = (uint32_t)strtoul(raw.c_str() + a + 1, nullptr, 10);
        l.kind = KIND_PRESENCE;
        l.text = raw.compare(b + 1, 1, "j") == 0 ? "joined" : "left";
        l.src = raw.substr(c + 1, d - c - 1);
        l.nick = raw.substr(d + 1);
        if (!l.seq || g_lines.count(l.seq)) continue;
        g_pres[l.seq] = l;
        g_lines[l.seq] = l;
        if (l.seq > lastDrawn || g_lrows.empty()) grew = true;
        else if (l.seq >= g_winFrom) structural = true;
    }
    if (!s_logCol || g_loadTimer) return;
    bool bottom = logAtBottom();
    if (structural) {
        captureAnchor();
        renderAll();
        if (bottom) scrollBottom(false); else restoreAnchor();
    } else if (grew) {
        appendNew();
        if (bottom) scrollBottom(false); else updateChevron();
    }
}

void onSend(lv_event_t*) {
    if (!s_compose || g_rid.empty()) return;
    std::string t = lcdInputBoxText(s_compose);
    if (t.empty()) return;
    storageSet("rrc.cmd.send", (g_rid + "|" + nonce() + "|" + t).c_str());
    lv_textarea_set_text(s_compose, "");
    g_drafts.erase(g_rid);
    lcdKeyboardClose();
    deferFocus(s_compose);
    scrollBottom(false);
}

/* Bytes left against the hub's body limit, shown once within 40 of it: a line
 * that is too long is refused whole, never split. */
void reflectCount() {
    if (!s_count || !s_compose) return;
    std::string hub = g_rooms.count(g_rid) ? g_rooms[g_rid].hub : "";
    int limit = hub.empty() ? 350 : storageGetInt(hubRam(hub, "limit_body").c_str(), 350);
    if (limit <= 0) limit = 350;
    int left = limit - (int)strlen(lv_textarea_get_text(s_compose));
    if (left > 40) { lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN); return; }
    lv_label_set_text_fmt(s_count, "%d", left);
    lv_obj_set_style_text_color(s_count, lv_color_hex(left < 0 ? C_FAIL : C_AMBER), 0);
    lv_obj_remove_flag(s_count, LV_OBJ_FLAG_HIDDEN);
}

/* The trackball's up/down (arrow keys while the composer's caret is lit) scroll
 * the log while the composer holds a single line; a multi-line draft keeps
 * them for moving the caret. */
void onComposeKey(lv_event_t* e) {
    uint32_t* kp = (uint32_t*)lv_event_get_param(e);
    if (!kp || !s_log || !s_compose) return;
    if (*kp != LV_KEY_UP && *kp != LV_KEY_DOWN) return;
    if (strchr(lv_textarea_get_text(s_compose), '\n')) return;
    int step = lv_font_get_line_height(kFont) * 3;
    if (*kp == LV_KEY_UP) {
        if (lv_obj_get_scroll_top(s_log) <= 2) loadEarlier();
        lv_obj_scroll_by_bounded(s_log, 0, step, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_by_bounded(s_log, 0, -step, LV_ANIM_OFF);
    }
    *kp = 0;
    updateChevron();
    maybeMarkRead();
}

void roomBack(lv_event_t*) { goList(); }

void buildRoomShell() {
    s_room = mkScreen();
    g_roomHdr = mkHeader(s_room, roomBack, [](lv_event_t*) { if (!g_rid.empty()) openInfo(); });

    s_log = lv_obj_create(s_room);
    lv_obj_remove_style_all(s_log);
    lv_obj_set_width(s_log, lv_pct(100));
    lv_obj_set_flex_grow(s_log, 1);
    lv_obj_set_style_pad_top(s_log, lcdPx(2), 0);
    lv_obj_set_style_pad_bottom(s_log, lcdPx(2), 0);
    lv_obj_set_style_pad_right(s_log, lcdPx(4), 0);
    lv_obj_add_event_cb(s_log, [](lv_event_t*) { updateChevron(); }, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(s_log, onLogScrollEnd, LV_EVENT_SCROLL_END, nullptr);

    s_logCol = lv_obj_create(s_log);
    lv_obj_remove_style_all(s_logCol);
    lv_obj_set_size(s_logCol, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_logCol, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_logCol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_logCol, LV_OBJ_FLAG_CLICKABLE);

    s_loading = mkLabel(s_log, "Loading the room\xE2\x80\xA6", C_PRES, kFont);
    lv_obj_add_flag(s_loading, LV_OBJ_FLAG_FLOATING);
    lv_obj_center(s_loading);
    lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);

    /* Floating, so it stays put while the log scrolls under it. */
    s_chev = lv_button_create(s_log);
    lv_obj_remove_style_all(s_chev);
    lv_obj_add_flag(s_chev, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_chev, lcdPx(22), lcdPx(22));
    lv_obj_set_style_radius(s_chev, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_chev, lv_color_hex(C_CHEV), 0);
    lv_obj_set_style_bg_opa(s_chev, LV_OPA_COVER, 0);
    lv_obj_align(s_chev, LV_ALIGN_BOTTOM_RIGHT, -lcdPx(4), -lcdPx(4));
    lv_obj_set_ext_click_area(s_chev, lcdPx(8));
    lv_obj_center(mkLabel(s_chev, LV_SYMBOL_DOWN, C_ACT, kSmall));
    lv_obj_add_event_cb(s_chev, [](lv_event_t*) { scrollBottom(true); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_chev, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* bar = mkRowBox(s_room, lcdPx(6));
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    lv_obj_set_style_pad_all(bar, lcdPx(4), 0);
    /* The whole bar is the field's target, not just its text. */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, [](lv_event_t*) { if (s_compose) lcdInputBoxActivate(s_compose); },
                        LV_EVENT_CLICKED, nullptr);

    s_compose = lcdInputBoxCreate(bar, 1, 3);
    lv_obj_set_style_text_font(s_compose, kFont, 0);
    lv_obj_set_style_bg_opa(s_compose, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_compose, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_compose, lv_color_hex(C_PRES), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(s_compose, 1, 0);
    lv_obj_set_style_border_color(s_compose, lv_color_hex(C_FIELD), 0);
    lv_obj_set_style_radius(s_compose, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(s_compose, lcdPx(2), 0);
    lv_obj_set_style_pad_hor(s_compose, lcdPx(6), 0);
    lv_obj_set_width(s_compose, 1);
    lv_obj_set_flex_grow(s_compose, 1);
    lv_obj_add_flag(s_compose, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_compose);
    lv_obj_add_event_cb(s_compose, onSend, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) { reflectCount(); }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_compose, onComposeKey, (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), nullptr);

    s_count = mkLabel(bar, "", C_AMBER, kSmall);
    lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);
    mkButton(bar, "Send", BTN_PRIMARY, onSend);

    s_note = mkLabel(s_room, "", C_AMBER, kSmall);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_note, lv_pct(100));
    lv_obj_set_style_pad_hor(s_note, lcdPx(6), 0);
    lv_obj_set_style_pad_bottom(s_note, lcdPx(2), 0);
    lv_obj_set_style_bg_color(s_note, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(s_note, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}

void refreshRoomHeader() {
    if (!s_room || g_rid.empty()) return;
    auto it = g_rooms.find(g_rid);
    std::string name = it != g_rooms.end() ? it->second.name : "";
    std::string hub = it != g_rooms.end() ? it->second.hub : "";
    setTitleLine(g_roomHdr.line, roomLabel(name), hub.empty() ? "" : " \xC2\xB7 " + hubName(hub));
    int m = storageGetInt(("rrc.rooms." + g_rid + ".members").c_str(), 0);
    lv_label_set_text(g_roomHdr.right, m > 0 ? (std::to_string(m) + (m == 1 ? " member" : " members")).c_str() : "");
    if (s_compose) lv_textarea_set_placeholder_text(s_compose, ("Message " + roomLabel(name)).c_str());
}

/* Under the composer: why a join or a send was refused, else why the room is
 * not live (its hub's status, or that the join is under way). */
void refreshRoomNote() {
    if (!s_note || g_rid.empty()) return;
    std::string err = printable(storageGetStr(("rrc.rooms." + g_rid + ".error").c_str(), ""), true);
    std::string hub = g_rooms.count(g_rid) ? g_rooms[g_rid].hub : "";
    std::string text;
    uint32_t col = C_AMBER;
    if (!err.empty()) text = err;
    else if (!hub.empty() && hubState(hub) != "live") { text = hubName(hub) + ": " + hubStatus(hub); col = C_SUB; }
    else if (storageGetStr(("rrc.rooms." + g_rid + ".state").c_str(), "") == "joining") { text = "Joining\xE2\x80\xA6"; col = C_SUB; }
    lv_label_set_text(s_note, text.c_str());
    lv_obj_set_style_text_color(s_note, lv_color_hex(col), 0);
    if (text.empty()) lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
}

void onLogChange(const char* key, const char*);

void saveDraft() {
    if (g_rid.empty() || !s_compose) return;
    const char* t = lv_textarea_get_text(s_compose);
    if (t && *t) g_drafts[g_rid] = t;
    else         g_drafts.erase(g_rid);
}

/* Close the open room: its subscription, its rows and its window. */
void leaveRoom() {
    if (g_rid.empty()) return;
    saveDraft();
    if (!g_logScope.empty()) storageUnsubscribeCb(g_logScope.c_str(), onLogChange);
    g_logScope.clear();
    g_rid.clear();
    g_lines.clear();
    g_pres.clear();
    g_lrows.clear();
    g_roster.clear();
    g_dirtySeqs.clear();
    g_logReload = false;
    if (g_loadTimer) { lv_timer_delete(g_loadTimer); g_loadTimer = nullptr; }
    if (s_logCol) lv_obj_clean(s_logCol);
    if (s_compose) lv_textarea_set_text(s_compose, "");
}

void openRoom(const std::string& rid, bool focus) {
    refreshFont();
    closeConfirm();
    closeInfoPages();
    if (g_screen == SC_HUB) setWatch(g_hub, false);
    dropPage(s_hub);
    if (!s_room) buildRoomShell();
    if (rid != g_rid) {
        leaveRoom();
        g_rid = rid;
        g_readSent = 0;
        g_pres.clear();
        g_presSeen = presNext();
        readPresenceMode();
        g_roster = readRoster(rid);
        /* Where the room was read up to now: the "N new" divider stays there
         * for as long as the room is open. */
        auto it = g_rooms.find(rid);
        g_divAt = 0;
        g_divCount = 0;
        if (it != g_rooms.end() && it->second.unread > 0 && it->second.seq > it->second.read_seq) {
            g_divAt = it->second.read_seq;
            g_divCount = it->second.unread;
        }
        g_logScope = "s.rrc.log." + rid;
        storageSubscribeChanges(g_logScope.c_str(), onLogChange);
        auto d = g_drafts.find(rid);
        lv_textarea_set_text(s_compose, d != g_drafts.end() ? d->second.c_str() : "");
        showScreen(SC_ROOM);
        refreshRoomHeader();
        refreshRoomNote();
        reflectCount();
        if (s_chev) lv_obj_add_flag(s_chev, LV_OBJ_FLAG_HIDDEN);
        if (s_loading) lv_obj_remove_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(nullptr);   /* paint the shell before the log is read */
        g_loadTimer = lv_timer_create(loadTimerCb, 20, nullptr);
        lv_timer_set_repeat_count(g_loadTimer, 1);
    } else {
        showScreen(SC_ROOM);
        refreshRoomHeader();
        refreshRoomNote();
        maybeMarkRead();
    }
    if (focus) deferFocus(s_compose);
}

/* ---- room info ---- */

Header    g_infoHdr;
lv_obj_t* s_infoTopic = nullptr;
lv_obj_t* s_infoHub = nullptr;
lv_obj_t* s_infoYou = nullptr;
lv_obj_t* s_infoSeg[3] = { nullptr, nullptr, nullptr };
lv_obj_t* s_infoMemSect = nullptr;
lv_obj_t* s_infoMembers = nullptr;
std::string g_infoRosterSig;            /* roster the member rows were built from ("\x01" = none yet) */
lv_obj_t*   s_cardIgnoreLbl = nullptr;  /* the card's Ignore / Unignore label */

void closeInfoPages() {
    dropPage(s_info);
    dropPage(s_card);
    s_infoTopic = s_infoHub = s_infoYou = s_infoMemSect = s_infoMembers = nullptr;
    s_infoSeg[0] = s_infoSeg[1] = s_infoSeg[2] = nullptr;
    s_cardIgnoreLbl = nullptr;
    g_infoRosterSig = "\x01";
}

void backToRoom() {
    closeConfirm();
    closeInfoPages();
    if (g_rid.empty()) { goList(); return; }
    openRoom(g_rid, true);
}

void onNotify(lv_event_t* e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    static const char* v[] = { "all", "mentions", "off" };
    if (g_rid.empty() || n < 0 || n > 2) return;
    storageSet("rrc.cmd.notify", (g_rid + "|" + v[n]).c_str());
    for (int i = 0; i < 3; i++)
        if (s_infoSeg[i]) { if (i == n) lv_obj_add_state(s_infoSeg[i], LV_STATE_CHECKED);
                            else        lv_obj_remove_state(s_infoSeg[i], LV_STATE_CHECKED); }
}

void onMemberRow(lv_event_t* e) { openCard(boundString(e), true); }

void fillMembers() {
    if (!s_infoMembers) return;
    std::string sig;
    for (auto& [id, w] : g_roster) sig += id + w.nick + w.lxmf + ";";
    if (sig == g_infoRosterSig) return;
    g_infoRosterSig = sig;
    lv_obj_clean(s_infoMembers);
    std::vector<std::pair<std::string, std::string>> order;   /* lower nick, id12 */
    for (auto& [id, w] : g_roster) order.push_back({ lower(w.nick.empty() ? id : w.nick), id });
    std::sort(order.begin(), order.end());
    for (auto& [ln, id] : order) {
        const Who& w = g_roster[id];
        lv_obj_t* row = mkRow(s_infoMembers);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, lcdPx(6), 0);
        bindString(row, onMemberRow, id);
        std::string nick = printable(w.nick, true);
        lv_obj_t* n = mkLabel(row, nick.empty() ? id.substr(0, 8) : nick, 0xFFFFFF, kBold);
        lv_obj_set_style_text_color(n, nickColor(id), 0);
        mkFill(row, fours(id), C_SUB, kSmall);
    }
    if (order.empty()) {
        lv_obj_t* l = mkLabel(s_infoMembers, "The member list arrives once the room is joined.", C_SUB, kSmall);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_pad_all(l, lcdPx(6), 0);
    }
    if (s_infoMemSect) lv_label_set_text_fmt(s_infoMemSect, "MEMBERS \xC2\xB7 %u", (unsigned)order.size());
}

void refreshInfo() {
    if (!s_info || g_rid.empty()) return;
    auto it = g_rooms.find(g_rid);
    if (it == g_rooms.end()) return;
    const RoomRec& r = it->second;
    setTitleLine(g_infoHdr.line, roomLabel(r.name), "");
    std::string topic = printable(storageGetStr(("rrc.rooms." + g_rid + ".topic").c_str(), ""), true);
    if (s_infoTopic) lv_label_set_text(s_infoTopic, topic.empty() ? "No topic set" : topic.c_str());
    std::string hubLine = hubName(r.hub);
    std::string rt = rttText(storageGetInt(hubRam(r.hub, "rtt").c_str(), 0));
    int hops = storageGetInt(hubRam(r.hub, "hops").c_str(), 0);
    if (hubState(r.hub) == "live") {
        if (!rt.empty()) hubLine += " \xC2\xB7 " + rt;
        if (hops > 0) hubLine += " \xC2\xB7 " + std::to_string(hops) + (hops == 1 ? " hop" : " hops");
    } else {
        hubLine += " \xC2\xB7 " + hubStatus(r.hub);
    }
    if (s_infoHub) lv_label_set_text(s_infoHub, hubLine.c_str());
    std::string nick = printable(storageGetStr(hubRam(r.hub, "nick").c_str(), ""), true);
    std::string as = printable(storageGetStr(hubRam(r.hub, "as").c_str(), ""), true);
    if (as.empty()) as = identLabel(storageGetStr(("s.rrc.hubs." + r.hub + ".ident").c_str(), ""));
    std::string you = nick.empty() ? as : nick + (as.empty() ? "" : " \xC2\xB7 as " + as);
    if (s_infoYou) lv_label_set_text(s_infoYou, you.c_str());
    for (int i = 0; i < 3; i++)
        if (s_infoSeg[i]) { if (i == r.notify) lv_obj_add_state(s_infoSeg[i], LV_STATE_CHECKED);
                            else               lv_obj_remove_state(s_infoSeg[i], LV_STATE_CHECKED); }
    fillMembers();
}

void openInfo() {
    if (g_rid.empty()) return;
    refreshFont();
    saveDraft();
    closeInfoPages();
    s_info = mkScreen();
    g_infoHdr = mkHeader(s_info, [](lv_event_t*) { backToRoom(); }, nullptr);
    lv_obj_t* body = mkBody(s_info);
    s_infoTopic = mkKv(body, "Topic", "", 0xFFFFFF, kFont);
    s_infoHub = mkKv(body, "Hub", "", 0xFFFFFF, kFont);
    std::string hub = g_rooms.count(g_rid) ? g_rooms[g_rid].hub : "";
    mkKv(body, "", fours(hub), C_SUB, kSmall);
    s_infoYou = mkKv(body, "You", "", 0xFFFFFF, kFont);

    lv_obj_t* nrow = mkRowBox(body, lcdPx(4));
    lv_obj_t* nk = mkLabel(nrow, "Notify", C_SUB, kSmall);
    lv_obj_set_width(nk, lcdPx(56));
    lv_obj_t* seg = mkRowBox(nrow, 0);
    lv_obj_set_width(seg, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(seg, 1, 0);
    lv_obj_set_style_border_color(seg, lv_color_hex(C_FIELD), 0);
    lv_obj_set_style_radius(seg, lcdPx(4), 0);
    lv_obj_set_style_clip_corner(seg, true, 0);
    static const char* names[] = { "All", "Mentions", "Off" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* b = lv_button_create(seg);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(b, lcdPx(8), 0);
        lv_obj_set_style_pad_ver(b, lcdPx(2), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
        focusRing(b);
        mkLabel(b, names[i], C_ACT, kSmall);
        lv_obj_add_event_cb(b, onNotify, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
        s_infoSeg[i] = b;
    }

    s_infoMemSect = mkSection(body, "MEMBERS");
    s_infoMembers = mkBox(body);
    lv_obj_set_width(s_infoMembers, lv_pct(100));
    lv_obj_set_flex_flow(s_infoMembers, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* bar = mkActionBar(s_info);
    std::string label = "Leave " + roomLabel(g_rooms.count(g_rid) ? g_rooms[g_rid].name : "");
    mkButton(bar, label, BTN_DANGER, [](lv_event_t*) {
        if (g_rid.empty()) return;
        std::string name = roomLabel(g_rooms.count(g_rid) ? g_rooms[g_rid].name : "");
        showConfirm("Leave " + name + "? What was said there goes from this device too.", "Leave",
                    [](lv_event_t*) {
            std::string rid = g_rid;
            closeConfirm();
            storageSet("rrc.cmd.part", rid.c_str());
            g_drafts.erase(rid);
            goList();
        });
    });

    showScreen(SC_INFO);
    refreshInfo();
    deferFocus(g_infoHdr.back);
}

/* ---- member card ---- */

Header    g_cardHdr;
std::string g_cardId;                   /* id12 on the card */
Who         g_cardWho;                  /* as the roster had them when opened */
bool        g_cardFromInfo = false;

std::string s_alsoId;
std::vector<std::string>* s_alsoAcc = nullptr;
void alsoLeaf(const char* key, const char*) {
    if (!s_alsoAcc || !key) return;
    std::string k = key;                /* rrc.rooms.<rid>.who.<id12> */
    std::string tail = ".who." + s_alsoId;
    if (k.size() <= tail.size() || k.compare(k.size() - tail.size(), tail.size(), tail)) return;
    std::string rid = k.substr(sizeof("rrc.rooms.") - 1, k.size() - tail.size() - (sizeof("rrc.rooms.") - 1));
    s_alsoAcc->push_back(rid);
}

void reflectIgnore() {
    if (!s_cardIgnoreLbl || g_cardId.empty()) return;
    bool ig = storageGetInt(("s.rrc.ignore." + g_cardId).c_str(), 0) != 0;
    lv_label_set_text(s_cardIgnoreLbl, ig ? "Unignore" : "Ignore");
}

void cardBack() {
    if (g_cardFromInfo) { dropPage(s_card); openInfo(); }
    else backToRoom();
}

void openCard(const std::string& id12, bool fromInfo) {
    refreshFont();
    g_cardId = id12;
    g_cardFromInfo = fromInfo;
    auto it = g_roster.find(id12);
    g_cardWho = it != g_roster.end() ? it->second : Who{};
    dropPage(s_card);
    s_cardIgnoreLbl = nullptr;

    std::string nick = printable(g_cardWho.nick, true);
    if (nick.empty()) nick = id12.substr(0, 8);
    std::string room = roomLabel(g_rooms.count(g_rid) ? g_rooms[g_rid].name : "");
    const std::string& basis = g_cardWho.lxmf.empty() ? id12 : g_cardWho.lxmf;
    int hue = peerHue(basis);

    s_card = mkScreen();
    g_cardHdr = mkHeader(s_card, [](lv_event_t*) { cardBack(); }, nullptr);
    setTitleLine(g_cardHdr.line, nick, " \xC2\xB7 " + room);
    lv_obj_t* body = mkBody(s_card);

    lv_obj_t* top = mkRowBox(body, lcdPx(10));
    lv_obj_set_style_pad_ver(top, lcdPx(6), 0);
    lv_obj_t* av = mkBox(top);
    lv_obj_set_size(av, lcdPx(34), lcdPx(34));
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(av, hsl(hue, 45, 32), 0);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    lv_obj_t* gl = mkLabel(av, printable(avatarGlyph(nick, basis), true), 0xFFFFFF, kBold);
    lv_obj_set_style_text_color(gl, hsl(hue, 70, 86), 0);
    lv_obj_center(gl);
    lv_obj_t* col = mkBox(top);
    lv_obj_set_width(col, 1);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* nl = mkLabel(col, nick, 0xFFFFFF, kBig);
    lv_obj_set_style_text_color(nl, hsl(hue, 70, 72), 0);
    lv_obj_t* il = mkLabel(col, fours(g_cardWho.full.empty() ? id12 : g_cardWho.full)
                                + (g_cardWho.full.empty() ? " \xE2\x80\xA6" : ""), C_SUB, kSmall);
    lv_label_set_long_mode(il, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(il, lv_pct(100));

    mkKv(body, "LXMF", g_cardWho.lxmf.empty() ? "Known once they say something here" : fours(g_cardWho.lxmf),
         g_cardWho.lxmf.empty() ? C_SUB : 0xFFFFFF, kSmall);

    std::vector<std::string> also;
    s_alsoId = id12;
    s_alsoAcc = &also;
    storageForEach("rrc.rooms.", alsoLeaf);
    s_alsoAcc = nullptr;
    std::string alsoLine;
    for (auto& rid : also) {
        if (rid == g_rid || !g_rooms.count(rid)) continue;
        if (!alsoLine.empty()) alsoLine += ", ";
        alsoLine += roomLabel(g_rooms[rid].name) + " \xC2\xB7 " + hubName(g_rooms[rid].hub);
    }
    if (!alsoLine.empty()) mkKv(body, "Also in", alsoLine, 0xFFFFFF, kFont);

    if (!g_cardWho.lxmf.empty()) {
        std::string base = "lxmf.announces." + g_cardWho.lxmf + ".";
        long last = storageGetInt((base + "last").c_str(), 0);
        if (last > 0) {
            int hops = storageGetInt((base + "hops").c_str(), 0);
            mkKv(body, "Heard", std::to_string(hops) + (hops == 1 ? " hop" : " hops") + " \xC2\xB7 " +
                 age(monoS() - last) + " ago", 0xFFFFFF, kFont);
        }
    }

    lv_obj_t* bar = mkActionBar(s_card);
    lv_obj_t* first = nullptr;
    if (!g_cardWho.lxmf.empty())
        first = mkButton(bar, "Message on LXMF", BTN_PRIMARY, [](lv_event_t*) {
            if (g_cardWho.lxmf.empty()) return;
            storageSet("lxmf.url_lcd", (g_cardWho.lxmf + ":" + nonce()).c_str());
        });
    lv_obj_t* men = mkButton(bar, "Mention", BTN_GHOST, [](lv_event_t*) {
        std::string nick = printable(g_cardWho.nick, true);
        if (nick.empty()) return;
        std::string rid = g_rid;
        backToRoom();
        if (s_compose && g_rid == rid) {
            lv_textarea_add_text(s_compose, ("@" + nick + " ").c_str());
            reflectCount();
        }
    });
    if (!first) first = men;
    mkButton(bar, "Ignore", BTN_GHOST, [](lv_event_t*) {
        if (g_cardId.empty()) return;
        bool ig = storageGetInt(("s.rrc.ignore." + g_cardId).c_str(), 0) != 0;
        storageSet("rrc.cmd.ignore", (g_cardId + (ig ? "|0" : "|1")).c_str());
    }, nullptr, &s_cardIgnoreLbl);
    reflectIgnore();

    showScreen(SC_CARD);
    deferFocus(first);
}

/* ---- live updates ---- */

/* What a burst of changes dirtied; the debounced timer does the work once. */
bool g_dRooms = false, g_dSaved = false, g_dHeard = false, g_dHubs = false;
bool g_dRoomRt = false, g_dRoster = false, g_dIgnore = false, g_dPresence = false;
bool g_dPres = false;                   /* the open room's join/leave ring moved */
lv_timer_t* g_metaTimer = nullptr;
lv_timer_t* g_logTimer = nullptr;

void metaTimerCb(lv_timer_t*) {
    g_metaTimer = nullptr;
    if (!s_layer) return;
    bool rooms = g_dRooms, saved = g_dSaved, heard = g_dHeard, hubs = g_dHubs;
    bool roomRt = g_dRoomRt, roster = g_dRoster, ignore = g_dIgnore, presence = g_dPresence, pres = g_dPres;
    g_dRooms = g_dSaved = g_dHeard = g_dHubs = g_dRoomRt = g_dRoster = g_dIgnore = g_dPresence = g_dPres = false;
    refreshFont();
    if (rooms) loadRooms();
    if (saved) loadSaved();
    if (heard) loadHeard();

    /* A room joined from the hub screen opens once its record exists. */
    if (rooms && !g_pendingRoom.empty()) {
        std::string rid = findRid(g_pendingHub, g_pendingRoom);
        if (!rid.empty()) {
            g_pendingHub.clear();
            g_pendingRoom.clear();
            if (g_screen == SC_HUB) openRoom(rid, true);
        }
    }

    if (rooms || saved || heard || hubs) {
        if (g_screen == SC_LIST) rebuildList();
        else g_listStale = true;
    }
    if (g_screen == SC_HUB && (rooms || saved || heard || hubs)) {
        /* The "joined" tags follow which rooms are joined, not every new line. */
        std::string sig;
        for (auto& [rid, r] : g_rooms) if (r.hub == g_hub && r.joined) sig += r.name + "\n";
        if (sig != g_hubJoinedSig) { g_hubJoinedSig = sig; g_hubDir = "\x01"; }
        refreshHub();
    }

    if (!g_rid.empty()) {
        if (roster) {
            auto fresh = readRoster(g_rid);
            /* Someone's LXMF address turning up changes their colour: redraw. */
            bool recolour = false;
            for (auto& [id, w] : fresh) {
                auto o = g_roster.find(id);
                if (!w.lxmf.empty() && (o == g_roster.end() || o->second.lxmf != w.lxmf)) { recolour = true; break; }
            }
            g_roster.swap(fresh);
            if (recolour) presence = true;   /* same redraw as a presence-setting change */
        }
        if (presence && !g_loadTimer) {
            readPresenceMode();
            bool bottom = logAtBottom();
            captureAnchor();
            renderAll();
            if (bottom) scrollBottom(false); else restoreAnchor();
        }
        if (pres) presCollect();
        if (rooms || hubs || roomRt) { refreshRoomHeader(); refreshRoomNote(); }
        if (rooms) maybeMarkRead();
    }
    if (g_screen == SC_INFO && (rooms || hubs || roomRt || roster || saved)) refreshInfo();
    if (g_screen == SC_CARD && ignore) reflectIgnore();
}

void scheduleMeta() {
    if (g_metaTimer) return;
    g_metaTimer = lv_timer_create(metaTimerCb, META_DEBOUNCE_MS, nullptr);
    lv_timer_set_repeat_count(g_metaTimer, 1);
}

void logTimerCb(lv_timer_t*) {
    g_logTimer = nullptr;
    if (!s_layer) return;
    refreshFont();
    patchLog();
}

bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

void onStorageChange(const char* key, const char*) {
    if (!s_layer || !key) return;
    if (startsWith(key, "s.rrc.rooms")) g_dRooms = true;
    else if (startsWith(key, "s.rrc.hubs")) g_dSaved = true;
    else if (startsWith(key, "s.rrc.ignore")) g_dIgnore = true;
    else if (startsWith(key, "s.rrc.presence")) g_dPresence = true;
    else if (startsWith(key, "rrc.heard.")) g_dHeard = true;
    else if (startsWith(key, "rrc.hubs.")) {
        /* Notices and command replies are the web window's; the watch flag is
         * our own write echoing back. */
        if (strstr(key, ".notes") || strstr(key, ".watch")) return;
        g_dHubs = true;
    } else if (startsWith(key, "rrc.rooms.")) {
        if (!g_rid.empty()) {
            std::string room = "rrc.rooms." + g_rid + ".";
            if (startsWith(key, (room + "pres").c_str())) { g_dPres = true; scheduleMeta(); return; }
            if (startsWith(key, (room + "who").c_str())) g_dRoster = true;
        }
        g_dRoomRt = true;
    } else return;
    scheduleMeta();
}

/* s.rrc.log.<rid>[.<seq>[.<field>]] for the open room: a seq is patched; a
 * change to the store as a whole reloads it. */
void onLogChange(const char* key, const char*) {
    if (!s_layer || !key || g_logScope.empty()) return;
    if (strncmp(key, g_logScope.c_str(), g_logScope.size())) return;
    const char* t = key + g_logScope.size();
    if (*t != '.') {
        if (*t) return;                 /* another room's id sharing the prefix */
        g_logReload = true;
    } else {
        char* end = nullptr;
        unsigned long seq = strtoul(t + 1, &end, 10);
        if (end == t + 1) g_logReload = true;
        else g_dirtySeqs.insert((uint32_t)seq);
    }
    if (g_logTimer) return;
    g_logTimer = lv_timer_create(logTimerCb, LOG_DEBOUNCE_MS, nullptr);
    lv_timer_set_repeat_count(g_logTimer, 1);
}

/* The screen woke: a room open at its newest line has now been read. */
void onStandbyChange(const char*, const char* val) {
    if (!s_layer) return;
    if (val && atoi(val) == 0) maybeMarkRead();
    roomWatchUpdate();
}

/* Another app asked to open a hub or a room: "<hub>[/<room>]|<nonce>". Runs on
 * the lcd task (subscribed through lcdRun in appInit). The hub is connected if
 * it isn't and opens; a named room is joined and opens once it exists. */
void onLcdOpenUrl(const char*, const char* val) {
    if (!val || !*val) return;
    std::string v = val;
    size_t bar = v.rfind('|');
    if (bar != std::string::npos) v.erase(bar);
    std::string hub = v, room;
    size_t sl = v.find('/');
    if (sl != std::string::npos) { hub = v.substr(0, sl); room = v.substr(sl + 1); }
    hub = lower(trim(hub));
    if (!isHex32(hub)) return;
    lcdShowProgram("RRC");
    if (!s_layer) return;
    loadRooms();
    std::string st = hubState(hub);
    if (!hubActive(st) && st != "need_ident") storageSet("rrc.cmd.connect", hub.c_str());
    openHub(hub);
    if (!room.empty()) joinRoom(hub, room);
}

bool g_subscribed = false;

/* ---- entry (lcd task, on a fresh layer) ---- */

void rrcApp(lv_obj_t* root) {
    refreshFont();
    s_layer = root;
    /* A fresh layer: every handle and every guard starts over, so a reopen
     * after a stop or an eviction rebuilds instead of trusting state whose
     * widgets are gone. */
    s_list = s_room = s_hub = s_info = s_card = nullptr;
    s_tabBtnR = s_tabBtnH = s_tabRooms = s_tabHubs = s_addTa = nullptr;
    g_rowsR.clear(); g_rowsH.clear();
    g_listStale = false;
    g_tab = 0;
    s_confirm = nullptr;
    g_hub.clear();
    g_hubHdr = Header{};
    s_hubAs = s_hubStatus = s_hubMotdBox = s_hubMotd = s_hubPick = s_hubPickGo = s_hubPickGoLbl = nullptr;
    s_hubDir = s_hubJoinTa = s_hubConn = s_hubConnLbl = s_hubForget = s_hubJoinSel = s_hubJoinSelLbl = nullptr;
    g_hubDir = "\x01"; g_hubSel.clear(); g_hubPickShown = -1; g_pickSel.clear(); g_pickLabels.clear();
    g_pendingHub.clear(); g_pendingRoom.clear();
    g_rid.clear(); g_logScope.clear();
    g_roomHdr = Header{};
    s_log = s_logCol = s_loading = s_chev = s_compose = s_count = s_note = nullptr;
    g_loadTimer = nullptr;
    g_lines.clear(); g_lrows.clear(); g_roster.clear(); g_dirtySeqs.clear();
    g_logReload = false; g_readSent = 0; g_winFrom = 0; g_divAt = 0; g_divCount = 0;
    g_infoHdr = Header{};
    s_infoTopic = s_infoHub = s_infoYou = s_infoMemSect = s_infoMembers = nullptr;
    s_infoSeg[0] = s_infoSeg[1] = s_infoSeg[2] = nullptr;
    g_infoRosterSig = "\x01";
    g_cardHdr = Header{};
    g_cardId.clear(); s_cardIgnoreLbl = nullptr;
    g_metaTimer = nullptr; g_logTimer = nullptr;
    g_dRooms = g_dSaved = g_dHeard = g_dHubs = g_dRoomRt = g_dRoster = g_dIgnore = g_dPresence = g_dPres = false;
    g_pres.clear();
    g_focusTarget = nullptr;
    g_screen = SC_LIST;
    g_shown = true;

    /* Subscribe before the first read, so nothing published in between is
     * missed. The subscriptions outlive the layer (each callback tests
     * s_layer first), so they are made once. */
    if (!g_subscribed) {
        storageSubscribeChanges("s.rrc.rooms",    onStorageChange);
        storageSubscribeChanges("s.rrc.hubs",     onStorageChange);
        storageSubscribeChanges("s.rrc.ignore",   onStorageChange);
        storageSubscribeChanges("s.rrc.presence", onStorageChange);
        storageSubscribeChanges("rrc.hubs.",      onStorageChange);
        storageSubscribeChanges("rrc.heard.",     onStorageChange);
        storageSubscribeChanges("rrc.rooms.",     onStorageChange);
        storageSubscribeChanges("sys.standby",    onStandbyChange);
        g_subscribed = true;
    }

    if (!g_watchTimer) g_watchTimer = lv_timer_create(watchTimerCb, 5000, nullptr);

    loadRooms();
    loadSaved();
    loadHeard();
    buildList();
    showScreen(SC_LIST);
    rebuildList();
    if (!g_rowsR.empty() && g_rowsR.front().obj && lv_obj_check_type(g_rowsR.front().obj, &lv_button_class))
        deferFocus(g_rowsR.front().obj);
}

}  // namespace

RrcApp::RrcApp() : LcdApp({ .name = "RRC", .iconBasename = "rrc" }) {}

void RrcApp::onCreate(lv_obj_t* root) { rrcApp(root); }

bool RrcApp::onBack()
{
    if (!s_layer) return false;
    if (s_confirm) { closeConfirm(); return true; }
    switch (g_screen) {
    case SC_LIST: return false;
    case SC_HUB:
    case SC_ROOM: goList(); return true;
    case SC_INFO: backToRoom(); return true;
    case SC_CARD: cardBack(); return true;
    }
    return false;
}

void RrcApp::onShow()
{
    g_shown = true;
    if (!s_layer) return;
    if (g_screen == SC_HUB) setWatch(g_hub, true);
    if (g_screen == SC_ROOM) maybeMarkRead();
    roomWatchUpdate();
}

void RrcApp::onHide()
{
    g_shown = false;
    if (!s_layer) return;
    if (g_screen == SC_HUB) setWatch(g_hub, false);
    roomWatchUpdate();
    saveDraft();
}

void RrcApp::onClose()
{
    /* The layer first: it is what every subscription callback tests. */
    if (g_screen == SC_HUB) setWatch(g_hub, false);
    saveDraft();
    if (!g_logScope.empty()) storageUnsubscribeCb(g_logScope.c_str(), onLogChange);
    g_logScope.clear();
    g_rid.clear();
    s_layer = nullptr;
    roomWatchUpdate();
    if (g_watchTimer) { lv_timer_delete(g_watchTimer); g_watchTimer = nullptr; }
    s_list = s_room = s_hub = s_info = s_card = nullptr;
    s_tabBtnR = s_tabBtnH = s_tabRooms = s_tabHubs = s_addTa = nullptr;
    g_rowsR.clear(); g_rowsH.clear();
    s_confirm = nullptr;
    g_hubHdr = Header{}; g_roomHdr = Header{}; g_infoHdr = Header{}; g_cardHdr = Header{};
    s_hubAs = s_hubStatus = s_hubMotdBox = s_hubMotd = s_hubPick = s_hubPickGo = s_hubPickGoLbl = nullptr;
    s_hubDir = s_hubJoinTa = s_hubConn = s_hubConnLbl = s_hubForget = s_hubJoinSel = s_hubJoinSelLbl = nullptr;
    s_log = s_logCol = s_loading = s_chev = s_compose = s_count = s_note = nullptr;
    s_infoTopic = s_infoHub = s_infoYou = s_infoMemSect = s_infoMembers = nullptr;
    s_infoSeg[0] = s_infoSeg[1] = s_infoSeg[2] = nullptr;
    s_cardIgnoreLbl = nullptr;
    g_lines.clear(); g_lrows.clear(); g_roster.clear(); g_dirtySeqs.clear();
    g_focusTarget = nullptr;
    /* Pending one-shot timers would touch freed widgets. */
    if (g_loadTimer) { lv_timer_delete(g_loadTimer); g_loadTimer = nullptr; }
    if (g_metaTimer) { lv_timer_delete(g_metaTimer); g_metaTimer = nullptr; }
    if (g_logTimer)  { lv_timer_delete(g_logTimer);  g_logTimer = nullptr; }
    g_screen = SC_LIST;
}

/* RrcApp::appInit — the boot-task half of bring-up. Another app (nomad's
 * rrc:// and rrc@ links) writes rrc.url_lcd; subscribing on the lcd task means
 * the callback may touch LVGL, and doing it here means it works whether or not
 * RRC was ever opened. */
void RrcApp::appInit()
{
    lcdRun([](void*) { storageSubscribeChanges("rrc.url_lcd", onLcdOpenUrl); });
}
