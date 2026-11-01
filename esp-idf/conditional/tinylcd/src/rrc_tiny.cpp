/**
 * rrc_tiny.cpp — the RRC status page on the tiny OLED.
 *
 *   RRC                      ●● ○
 *   4          @1
 *   unread     mentions
 *   #lora · kc1awv
 *
 * Storage is rrc's API surface (see rrc.h). A hub the node is holding or
 * trying to hold is a dot: filled while rrc.hubs.<hub>.state is live, hollow
 * while it is linking or retrying. The totals are rrc.unread and rrc.mentions
 * as published. The last line names the room with the newest mention (the
 * directory's last_ts among rooms with mentions) and who it was from, found by
 * reading that room's log back from its newest line. All of it is read at draw
 * time, which only runs while the page is on screen; the subscriptions turn
 * any change into a redraw request that tinylcd drops unless the page shows.
 */
#include "rrc_tiny.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "storage.h"
#include "tinylcd.h"

namespace {

tinylcd_page_t s_page = TINYLCD_NO_PAGE;

const int MAX_DOTS = 8;          /* fits the title band right of "RRC" */
const int MENTION_LOOKBACK = 40; /* log lines read back for the mention's nick */

/* Accumulators for the storageForEach walks (draw runs on the tinylcd task
 * only, so file statics are safe). */
std::vector<bool> s_dots;        /* true = live (filled), false = trying (hollow) */

struct MentionRoom { std::string rid, name; long seq = 0, ts = 0; int mentions = 0; };
std::vector<MentionRoom> s_rooms;

void hubLeaf(const char* key, const char* val)
{
    /* rrc.hubs.<32 hex>.state */
    static const size_t PFX = sizeof("rrc.hubs.") - 1;
    size_t n = strlen(key);
    if (n != PFX + 32 + 6 || strcmp(key + n - 6, ".state") != 0 || !val) return;
    if ((int)s_dots.size() >= MAX_DOTS) return;
    if (!strcmp(val, "live")) s_dots.push_back(true);
    else if (!strcmp(val, "linking") || !strcmp(val, "hello") || !strcmp(val, "retry"))
        s_dots.push_back(false);
}

void roomLeaf(const char* key, const char* val)
{
    /* s.rrc.rooms.<rid>.<field> — fields of one record arrive together. */
    const char* tail = key + sizeof("s.rrc.rooms.") - 1;
    const char* dot = strchr(tail, '.');
    if (!dot || !val) return;
    std::string rid(tail, dot - tail);
    const char* f = dot + 1;
    if (s_rooms.empty() || s_rooms.back().rid != rid) {
        s_rooms.emplace_back();
        s_rooms.back().rid = rid;
    }
    MentionRoom& r = s_rooms.back();
    if      (!strcmp(f, "name"))     r.name = val;
    else if (!strcmp(f, "seq"))      r.seq = atol(val);
    else if (!strcmp(f, "last_ts"))  r.ts = atol(val);
    else if (!strcmp(f, "mentions")) r.mentions = atoi(val);
}

/* "#room · nick" of the newest mention, or "" when there is none. */
std::string newestMention()
{
    s_rooms.clear();
    storageForEach("s.rrc.rooms.", roomLeaf);
    const MentionRoom* best = nullptr;
    for (auto& r : s_rooms)
        if (r.mentions > 0 && (!best || r.ts > best->ts)) best = &r;
    if (!best) return "";
    std::string out = "#" + best->name;
    for (long seq = best->seq; seq > 0 && seq > best->seq - MENTION_LOOKBACK; --seq) {
        char base[48];
        snprintf(base, sizeof base, "s.rrc.log.%s.%08ld.", best->rid.c_str(), seq);
        if (storageGetInt((std::string(base) + "mention").c_str(), 0) != 1) continue;
        std::string nick = storageGetStr((std::string(base) + "nick").c_str(), "");
        if (!nick.empty()) out += " \xC2\xB7 " + nick;   /* U+00B7 middle dot */
        break;
    }
    return out;
}

/* Drop whole UTF-8 characters off the end until `s` fits `w` pixels. */
void fitWidth(u8g2_t* g, std::string& s, int w)
{
    while (!s.empty() && (int)u8g2_GetUTF8Width(g, s.c_str()) > w) {
        size_t n = s.size() - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s.erase(n);
    }
}

bool drawRrcPage(tinylcd_page_t, u8g2_t* g, tinylcd_ev_t)
{
    u8g2_SetFont(g, u8g2_font_7x13B_tr);
    u8g2_DrawStr(g, 0, TINYLCD_TITLE_Y, "RRC");

    /* Hub dots, right-aligned in the title band (rows 7..13, clear of the
     * page-indicator strip and of the yellow/blue seam). */
    s_dots.clear();
    storageForEach("rrc.hubs.", hubLeaf);
    int x = 124;
    for (int i = (int)s_dots.size() - 1; i >= 0; --i, x -= 9) {
        if (s_dots[i]) u8g2_DrawDisc(g, x, 10, 3, U8G2_DRAW_ALL);
        else           u8g2_DrawCircle(g, x, 10, 3, U8G2_DRAW_ALL);
    }

    int unread = storageGetInt("rrc.unread", 0);
    int ment   = storageGetInt("rrc.mentions", 0);
    if (unread < 0) unread = 0;
    if (unread > 9999) unread = 9999;
    if (ment < 0) ment = 0;
    if (ment > 999) ment = 999;

    char num[12];
    u8g2_SetFont(g, u8g2_font_logisoso20_tr);
    snprintf(num, sizeof num, "%d", unread);
    u8g2_DrawStr(g, 0, 37, num);
    snprintf(num, sizeof num, "@%d", ment);
    u8g2_DrawStr(g, 66, 37, num);

    u8g2_SetFont(g, u8g2_font_5x8_tr);
    u8g2_DrawStr(g, 0, 46, "unread");
    u8g2_DrawStr(g, 66, 46, ment == 1 ? "mention" : "mentions");

    std::string where = newestMention();
    if (!where.empty()) {
        u8g2_SetFont(g, u8g2_font_6x10_tf);
        fitWidth(g, where, 128);
        u8g2_DrawUTF8(g, 0, 55, where.c_str());
    }
    return false;   /* draw-only page: no events handled */
}

}  // namespace

void RrcTinyPage::onInit()
{
    s_page = tinylcdAddPage("rrc", drawRrcPage);
    tinylcdRun(ON_TINYLCD {
        /* On the tinylcd task. The totals move with every counted line and
         * every read; a hub's dot moves with rrc.hubs.<hub>.state. */
        storageSubscribeChanges("rrc.unread",   ON_CHANGE { tinylcdDraw(s_page); });
        storageSubscribeChanges("rrc.mentions", ON_CHANGE { tinylcdDraw(s_page); });
        storageSubscribeChanges("rrc.hubs.",    ON_CHANGE { tinylcdDraw(s_page); });
    });
}
