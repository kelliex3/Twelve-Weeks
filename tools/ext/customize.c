// customize.c - the CUSTOMIZE page (doc re/customize-1117.md; widgets tools/features/customize_widgets.py, page to1/customize).
// Included by t2r-ext.c after clan13.c (uses the gearup13.c helpers gu_rep).
//
// The way in: the CUSTOMIZE row under CLAN on the MP menu list (customize_widgets.add_customize_row, the same native button as CLAN):
// select on it (Enter / pad A) or a click. t2r-no-customize.txt next to the exe hides the row.
//
// The page: category tabs (Q / E, the bumpers, a click), a 3 x 4 grid of the category's items for the faction + sex being edited
// (arrows / stick / d-pad, the wheel, hover), SELECT / a click = EQUIP (saved at once to t2r-customize.txt), F / menu-action / a click
// on the other model = SWITCH FACTION (the front model in the spotlight is the outfit being edited), U / pad L3 / a click on the
// tab bar's toggle = UNLOCKED ONLY (every category lists its unlocked items only; saved). BACK / Esc closes.
// Items whose parts are not in the MP data yet (customize_items tier 2 / 3) can be looked at but not equipped.
// First slice: the page, the lists and the saved choice. Not yet: the two live 3D models, wearing the outfit in a match,
// MSG_CUSTOM_LOOK (customize_core.h has the format; the send stays off until receivers can compose the look) - see the doc.
#include "customize_core.h"
#include <io.h>                      // _get_osfhandle (cz_og_tick: a file's creation time)

#define CZ_COLS 3
#define CZ_ROWS 4
#define CZ_NCARD (CZ_COLS * CZ_ROWS)
// geometry = customize_widgets.py (canvas 3840 x 2160; the No Return SKINS page's numbers)
#define CZ_HEAD_X 146.0f
#define CZ_HEAD_CY 166.0f
#define CZ_HEAD_PT 66.0f
#define CZ_HEAD_RULE_X1 3700.0f
#define CZ_FAC_Y (CZ_HEAD_CY + 20.0f)       // the other-faction note's box = customize_widgets FAC_Y / FAC_H / FAC_PAD / FAC_W
#define CZ_FAC_H 72.0f
#define CZ_FAC_PAD 32.0f
#define CZ_FAC_W 1240.0f
#define CZ_BAR_CY 361.0f
#define CZ_BAR_IC_X0 372.0f
#define CZ_BAR_CELL 88.0f
#define CZ_BAR_GAP 34.0f
#define CZ_TAB_A_OFF (84.0f / 255.0f)
#define CZ_ITEM_X 191.0f
#define CZ_ITEM_CY 497.0f
#define CZ_ITEM_PT 56.0f
#define CZ_ITEM_RULE_X1 1310.0f
#define CZ_X0 205.0f
#define CZ_GRID_Y 615.0f
#define CZ_CARD_W 300.0f
#define CZ_CARD_H 275.0f
#define CZ_GAP 32.0f
#define CZ_SB_X (CZ_X0 + CZ_COLS * CZ_CARD_W + (CZ_COLS - 1) * CZ_GAP + 30.0f)
#define CZ_CARD_TXT_W 190.0f                     // cdNm box width (two lines at most)
static const char *const CZ_ICON[CZ_NCAT] = {"cz-hat", "cz-torso", "cz-lower", "cz-backpack", "cz-face", "cz-melee", "cz-mask", "cz-sex"};
// the tab bar's order (customize_items.TAB_ORDER; the widgets tabIc<category> stand at these places): MASK is next to HEADWEAR on the
// bar, but last of the wearables in the saved order (an older file's numbers keep their meaning)
static const int CZ_TAB_ORDER[CZ_NCAT] = {CZ_HAT, CZ_MASK, CZ_TORSO, CZ_LOWER, CZ_BACKPACK, CZ_FACE, CZ_MELEE, CZ_SEX};
static int cz_tab_pos(int cat) { for (int i = 0; i < CZ_NCAT; i++) if (CZ_TAB_ORDER[i] == cat) return i; return 0; }
static int cz_tab_step(int cat, int d) { return CZ_TAB_ORDER[((cz_tab_pos(cat) + d) % CZ_NCAT + CZ_NCAT) % CZ_NCAT]; }
static const char *const CZ_LOGO[CZ_NFAC] = {"streaming/photo-mode/logos/wlf-wolf-logo.png", "streaming/photo-mode/logos/scar-logo.png"};
static const char *const CZ_FAC_NAME[CZ_NFAC] = {"WLF", "SERAPHITES"};

static cz_profile_t g_cz;
static int g_cz_loaded;
static struct {
    int up, quiet, closing, back_f;
    uint64_t enter_ms;
    int fac;                                     // the faction whose outfit is in front (being edited)
    int cat, foc[CZ_NCAT], off[CZ_NCAT];
    int hv_card, hv_tab, hv_back, hv_sel, hv_sw, hv_f, hv_b, hv_q, hv_e;
    int h_sel, h_u, h_d, h_l, h_r, h_back, h_esc, h_pv, h_nx, h_act, h_q, h_e, h_f, h_c, h_x;
    int hv_ch, hv_tip;                           // the CHALLENGES prompt / the tip's CONTINUE under the cursor
    int h_z, h_r3, hv_zm;                        // ZOOM: Z / pad R3 held, its prompt under the cursor
    int h_uk, h_l3, hv_uo;                       // UNLOCKED ONLY: U / pad L3 held, its toggle under the cursor
    uint64_t t_u, t_d, t_l, t_r;
    uint64_t key;                                // last filled state
    char msg[128]; uint64_t msg_until;
    uint64_t deny_until;                        // a locked item was pressed: the requirement card blinks until this tick
} CZ;

static uint64_t cz_page_id(void) { static uint64_t id; if (!id) id = page_id("customize"); return id; }
static int cz_enabled(void) {
    static int v = -1; static uint64_t t;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - t > 2000) { t = now; FILE *f = exe_side_open("t2r-no-customize.txt", "r"); v = !f; if (f) fclose(f); }
    return v;
}
// The story gas masks (DINA'S / ELLIE'S / ABBY'S / LEV'S / BONG MASK: wlf_gasmasks paks, card pictures "...-grp-t2r-gm-<sex>-<mask>") are
// out of the public lists for the release (the gas masks make the entire body disappear while the coats usually stay; the
// cause is not proven yet). They stay in the table (saved indices are append-only); the page lists the other rows only, and a saved look that
// wears one falls back to GAS MASK. Dev builds: t2r-cz-story-masks.txt next to the exe lists them again (the camera-hide test on A).
static int cz_story_mask(const cz_item_t *it) { return it && it->pic && strstr(it->pic, "-grp-t2r-gm-") != NULL; }
static int cz_story_masks_shown(void) {
#ifdef T2R_RELEASE
    return 0;
#else
    static int v = -1; static uint64_t t;
    uint64_t now = GetTickCount64();
    if (v < 0 || now - t > 2000) { t = now; v = GetFileAttributesA("t2r-cz-story-masks.txt") != INVALID_FILE_ATTRIBUTES; }
    return v;
#endif
}
static int cz_story_mask_off(const cz_item_t *it) { return cz_story_mask(it) && !cz_story_masks_shown(); }   // not offered: no card, no challenge row, no unlock line
static void cz_story_mask_fallback(cz_profile_t *s, const char *why) {
    if (cz_story_masks_shown()) return;
    for (int f = 0; f < CZ_NFAC; f++)
        for (int x = 0; x < CZ_NSEX; x++) {
            const cz_item_t *it = cz_item(f, x, CZ_MASK, s->it[f][x][CZ_MASK]);
            if (!cz_story_mask(it)) continue;
            int g = 0;
            for (int k = 0; k < cz_count(f, x, CZ_MASK); k++) { const cz_item_t *m = cz_item(f, x, CZ_MASK, k); if (m && !strcmp(m->label, "GAS MASK")) { g = k; break; } }
            xlog("customize: %s %s mask %s is not offered - worn as %s (%s)", CZ_FAC_NAME[f], CZ_SEX_KEY[x], it->label, cz_item(f, x, CZ_MASK, g)->label, why);
            s->it[f][x][CZ_MASK] = (uint8_t)g;
        }
}
#if __has_include("cz_seal.h")
#include "cz_seal.h"            // optional module, not in the public tree
#else
static void cz_seal_hex(const char *msg, size_t n, const char *tok, char hex[65]) { (void)msg; (void)n; (void)tok; memset(hex, '0', 64); hex[64] = 0; }
static int cz_seal_check(char *buf, const char **why) { (void)buf; *why = "no seal in this build"; return 1; }
#endif
static void cz_load(void) {
    if (g_cz_loaded) return;
    g_cz_loaded = 1;
    char buf[16384] = {0};
    FILE *f = exe_side_open("t2r-customize.txt", "r");
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    if (f) {
        const char *why = "";
        int trust = cz_seal_check(buf, &why);
#ifdef T2R_RELEASE
        if (!trust && cz_is_kellie()) { trust = 1; why = "owner account"; }
        if (!trust) cz_drop_progress(buf);
#else
        if (!trust) why = "(dev build: kept)";
        trust = 1;
#endif
        xlog("customize: unlock progress %s (%s)", trust ? "kept" : "DROPPED", why);
    }
    int ok = cz_parse(&g_cz, f ? buf : NULL);
    if (!ok) {                                   // first visit: each faction starts with the player's current sex choice (t2r-female.txt "always" = female)
        cz_defaults(&g_cz);
    }
    cz_story_mask_fallback(&g_cz, "saved look");
    xlog("customize: profile %s (table %04x, %d / %d / %d / %d items)", ok ? "loaded" : "defaults", cz_table_id(), cz_count(0, 0, CZ_TORSO), cz_count(0, 1, CZ_TORSO),
         cz_count(1, 0, CZ_TORSO), cz_count(1, 1, CZ_TORSO));
}
static void cz_save(void) {
    char buf[16384 + 80];
    int n = cz_format(&g_cz, buf, 16384);
    if (n < 0 || n >= 16384) return;
    char tok[65] = "", hex[65];
    cz_seal_hex(buf, (size_t)n, clan_token(tok) ? tok : NULL, hex);
    n += snprintf(buf + n, 80, "seal %s\n", hex);
    // whole file to a temp first, then swapped in: a save cut off half way must not leave a file whose seal no longer matches
    char p[MAX_PATH + 40], t[MAX_PATH + 40];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    char *sl = strrchr(p, 92);
    if (!sl) return;
    strcpy(sl + 1, "t2r-customize.txt");
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (!f) { xlog("customize: cannot write t2r-customize.txt"); return; }
    int okw = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    okw &= fflush(f) == 0;
    fclose(f);
    if (!okw || !MoveFileExA(t, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { xlog("customize: save failed (%lu)", GetLastError()); DeleteFileA(t); }
}
static int cz_cur_side(void) { int s = clan_side_now(); return s == 1 ? 0 : s == 2 ? 1 : -1; }     // the faction the player plays as (-1: none yet)
static int cz_sex(void) { return g_cz.sex[CZ.fac] < CZ_NSEX ? g_cz.sex[CZ.fac] : 0; }
// KELLIE rows (customize_items.KELLIE): the developer's own character. The owner = the local Steam account whose salted id 64 hashes
// to CZ_KELLIE_HASH (fnv64 of "t2r-kellie-1118:" + the decimal id; the id itself is not in the source). The account comes from the Steam
// client's registry entry (HKCU Software\Valve\Steam\ActiveProcess ActiveUser = the account number, id 64 = 76561197960265728 + it).
#define CZ_KELLIE_HASH 0x20644379c4ec6b9fULL
static int cz_is_kellie(void) {
    static int v = -1; static uint64_t at;
    uint64_t now = GetTickCount64();
    if (v >= 0 && now - at < 15000) return v;
    at = now; v = 0;
    DWORD au = 0, sz = sizeof au;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser", RRF_RT_REG_DWORD, NULL, &au, &sz) == ERROR_SUCCESS && au) {
        char t[64]; snprintf(t, sizeof t, "t2r-kellie-1118:%llu", 76561197960265728ULL + (unsigned long long)au);
        v = fnv64(t) == CZ_KELLIE_HASH;
    }
    return v;
}
static int cz_plain_kind(int kind) { return kind == CZK_OG || kind == CZK_KELLIE || kind == CZK_ACH; }      // no challenge: a plain line, no bar (customize_items.U_PLAIN)
// the rows of a list the page shows: hidden rows (hid: only their owner's page lists them; the story gas masks: nobody's, see cz_story_mask)
// are left out. The page works in ROW positions (CZ.foc / CZ.off / the cards); cz_r2k / cz_k2r convert to and from the table index that the
// profile, the compose and the network carry.
static int cz_unlisted(const cz_item_t *it) { return !it || (it->hid && !cz_is_kellie()) || cz_story_mask_off(it); }
// UNLOCKED ONLY (U / pad L3 / a click on the tab bar's toggle; saved as profile flag uonly): every category's grid leaves out the
// locked rows. The worn item always stays listed (the grid never loses its check), SEX is never filtered.
static int cz_locked(int fac, int sex, int cat, int k);
static int cz_row_out(int fac, int sex, int cat, int k) {
    if (cz_unlisted(cz_item(fac, sex, cat, k))) return 1;
    if (!(g_cz.flags & CZF_UONLY) || cat == CZ_SEX) return 0;
    if (k == g_cz.it[fac][sex < CZ_NSEX ? sex : 0][cat]) return 0;
    return cz_locked(fac, sex, cat, k);
}
static int cz_listed(int fac, int sex, int cat) {
    int n = 0, c = cz_count(fac, sex, cat);
    for (int k = 0; k < c; k++) n += !cz_row_out(fac, sex, cat, k);
    return n;
}
static int cz_r2k(int fac, int sex, int cat, int r) {          // row position -> table index (-1: past the listed rows)
    int c = cz_count(fac, sex, cat);
    if (r < 0) return -1;
    for (int k = 0; k < c; k++) if (!cz_row_out(fac, sex, cat, k) && r-- == 0) return k;
    return -1;
}
static int cz_k2r(int fac, int sex, int cat, int k) {          // table index -> row position (an unlisted index: row 0)
    int r = 0, c = cz_count(fac, sex, cat);
    for (int i = 0; i < c && i < k; i++) r += !cz_row_out(fac, sex, cat, i);
    return k >= 0 && k < c && !cz_row_out(fac, sex, cat, k) ? r : 0;
}
static int cz_rows(void) { return cz_listed(CZ.fac, cz_sex(), CZ.cat); }
static int cz_equipped(int cat) { return cat == CZ_SEX ? cz_sex() : g_cz.it[CZ.fac][cz_sex()][cat]; }      // table index
static int cz_eq_row(int cat) { return cz_k2r(CZ.fac, cz_sex(), cat, cz_equipped(cat)); }                    // its row position
static void cz_note(const char *m) { snprintf(CZ.msg, sizeof CZ.msg, "%s", loc_tr(m)); CZ.msg_until = GetTickCount64() + 2500; CZ.key = 0; }

// ------------------------------------------------------------------ unlocks (customize_items.py U_CHAL / U_POOL / assign_unlocks)
// The default of a category is free; every other item names one challenge (CZ_CHAL: a title + a line). Progress lives in t2r-customize.txt:
//   rank / weeks / survivors: the clan state (0xE1 rk / day / h + hu + s), latched at the best ever reached (a clan that shrinks or
//                             dies keeps what it unlocked) - cz_stats_tick, every 2 s while the MP menu runs
//   match result + counters:  when a match is over (cz_career_match, from feature_clan's match-over report): matches, wins, wins per map,
//                             flawless wins (won, no death), the best streak, and per counter id (g_mis_cnt: downs / executions / revives /
//                             downs + executions per gun / throwables / assists / revenge / ...) the career total and the best one match
//   safes:                    the enemy safe our team cracked in Interrogation (cz_career_safe, from int_lockbox_open)
// (Client-side counts: a server-side career op can replace them later - re/customize-1117.md "Unlocks".)
static uint32_t g_cz_gen;                        // bumped when progress changes (the page refills)
static const cz_item_t *cz_it(int fac, int sex, int cat, int k) { return cz_item(fac, sex, cat, k); }
static int g_cz_test_open;          // test file "unlock 1" (dev builds): every row counts as unlocked for this run - in-game checks of locked rows
static int cz_locked(int fac, int sex, int cat, int k) {
#ifndef T2R_RELEASE
    if (g_cz_test_open) return 0;
#endif
    const cz_item_t *it = cz_it(fac, sex, cat, k); return it && !cz_unlocked(&g_cz, it);
}
// The item's challenge in parts: the title (translated), the requirement line (the English template filled, then translated: the
// table's keys are the %d / %s templates; a lone safe reads singular), the progress (capped at the target) and the target. 0 = the
// item has no challenge.
static int cz_lock_parts(int fac, int sex, int cat, int k, const char **ti, char *line, int cap, unsigned *pr, unsigned *n) {
    const cz_item_t *it = cz_it(fac, sex, cat, k);
    line[0] = 0;
    if (!it || !it->uk || it->uk >= CZ_NCHAL) return 0;
    const cz_chal_t *c = &CZ_CHAL[it->uk];
    char t[112];
    *n = it->un;
    // (one of something reads singular: customize_items.U_SINGULAR)
    if (c->kind == CZK_SAFES && *n == 1) snprintf(t, sizeof t, "CRACK A SAFE IN INTERROGATION");
    else if (c->kind == CZK_MAPWINS && *n == 1) snprintf(t, sizeof t, "WIN A MATCH ON %s", it->um < CZ_NUMAP ? CZ_UMAP_NAME[it->um] : "?");
    else if (c->kind == CZK_WEEKS && *n == 1) snprintf(t, sizeof t, "SURVIVE A WEEK");
    else if (c->kind == CZK_FLAWLESS && *n == 1) snprintf(t, sizeof t, "WIN A MATCH WITHOUT DYING");
    else if (c->kind == CZK_MAPWINS) snprintf(t, sizeof t, c->line, *n, it->um < CZ_NUMAP ? CZ_UMAP_NAME[it->um] : "?");
    else snprintf(t, sizeof t, c->line, *n);
    snprintf(line, (size_t)cap, "%s", loc_tr(t));
    *ti = loc_tr(c->title);
    uint32_t p = cz_progress(&g_cz, it);
    *pr = (unsigned)(p < *n ? p : *n);
    return 1;
}
static void cz_lock_text(int fac, int sex, int cat, int k, char *out, int cap) {
    const char *ti = ""; char t[160]; unsigned pr = 0, n = 0;
    out[0] = 0;
    if (!cz_lock_parts(fac, sex, cat, k, &ti, t, sizeof t, &pr, &n)) return;
    snprintf(out, (size_t)cap, "%s:  %s  %u / %u", ti, t, pr, n);
}
// ---- the requirement card under the grid (tools/features/customize_widgets.py build_req: the approved card, so the unlock
// requirement text does not cut into other elements): lock + title (beige small caps), the description (multiline, grey), a thin bar with n / N on its
// right, placed under the description's last line. Unlocked: check + UNLOCKED, one line. No challenge (a default item, SEX): hidden.
#define CZ_REQ_X0 CZ_X0
#define CZ_REQ_X1 (CZ_X0 + CZ_COLS * CZ_CARD_W + (CZ_COLS - 1) * CZ_GAP)
#define CZ_REQ_DS_Y (CZ_GRID_Y + CZ_ROWS * CZ_CARD_H + (CZ_ROWS - 1) * CZ_GAP + 36.0f + 56.0f)   // REQ_Y + 56
#define CZ_REQ_DS_PT 32.0f            // nr.pt(21)
#define CZ_REQ_N_PT 28.0f             // nr.pt(19)
#define CZ_REQ_LINE_H 60.0f
#define CZ_REQ_MAX_LINES 3
#define CZ_REQ_BAR_H 6.0f
static float cz_req_cw(unsigned char c) { return c == ' ' ? 0.40f : (c >= 'a' && c <= 'z') ? 0.62f : 0.80f; }   // = customize_widgets._cw
static int cz_req_lines(const char *t, float pt, float w) {     // = customize_widgets.req_lines: greedy wrap on the estimated widths
    const float sp = cz_req_cw(' ') * pt * 1.06f, lim = w * 0.96f;
    float x = 0.0f;
    int lines = 1;
    const unsigned char *q = (const unsigned char *)(t ? t : "");
    while (*q) {
        while (*q == ' ') q++;
        if (!*q) break;
        float ww = 0.0f;
        for (; *q && *q != ' '; q++) if ((*q & 0xC0) != 0x80) ww += cz_req_cw(*q) * pt * 1.06f;    // (a UTF-8 letter counts once)
        if (x > 0.0f && x + sp + ww > lim) { lines++; x = ww; }
        else x += (x > 0.0f ? sp : 0.0f) + ww;
    }
    return lines;
}
#define CZ_DENY_MS 480
static void cz_deny_tick(void) {                // a locked item was pressed: two blinks of the requirement card
    static int on;
    uint64_t now = GetTickCount64(), w = ui_w("anim-group.req");
    if (now < CZ.deny_until) { if (w) w_alpha(w, 0.3f + 0.7f * fabsf(cosf((float)(CZ.deny_until - now) / (float)CZ_DENY_MS * 6.2831853f))); on = 1; }
    else if (on) { if (w) w_alpha(w, 1.0f); on = 0; }
}
static void cz_req_fill(int fac, int sex, int cat, int k, int show) {
    const char *ti = ""; char t[160], cnt[32]; unsigned pr = 0, n = 0;
    const cz_item_t *it = cz_it(fac, sex, cat, k);
    if (!show || !cz_lock_parts(fac, sex, cat, k, &ti, t, sizeof t, &pr, &n)) { ui_visible("anim-group.req", 0); return; }
    const int open = cz_unlocked(&g_cz, it);
    ui_visible("anim-group.req", 1);
    ui_visible("anim-group.req.reqOk", open); ui_visible("anim-group.req.reqLk", !open);
    if (cz_plain_kind(CZ_CHAL[it->uk].kind)) {          // not a challenge: title + the line, locked or not; no bar, no count
        ui_text("anim-group.req.reqTi", ti);
        ui_visible("anim-group.req.reqDs", 1); ui_text("anim-group.req.reqDs", t);
        ui_visible("anim-group.req.reqBT", 0); ui_visible("anim-group.req.reqBF", 0); ui_visible("anim-group.req.reqN", 0);
        return;
    }
    ui_text("anim-group.req.reqTi", open ? loc_tr("UNLOCKED") : ti);
    ui_visible("anim-group.req.reqDs", !open); ui_visible("anim-group.req.reqBT", !open); ui_visible("anim-group.req.reqN", !open);
    if (open) { ui_visible("anim-group.req.reqBF", 0); return; }
    ui_text("anim-group.req.reqDs", t);
    int lines = cz_req_lines(t, CZ_REQ_DS_PT, CZ_REQ_X1 - CZ_REQ_X0);
    if (lines > CZ_REQ_MAX_LINES) lines = CZ_REQ_MAX_LINES;
    const float y = CZ_REQ_DS_Y + lines * CZ_REQ_LINE_H + 22.0f + CZ_REQ_BAR_H * 0.5f;     // the bar's middle under the last line
    snprintf(cnt, sizeof cnt, "%u / %u", pr, n);
    ui_text("anim-group.req.reqN", cnt);
    uint64_t w = ui_w("anim-group.req.reqN");
    if (w) w_pivot(w, CZ_REQ_X1, y);
    const float x1 = CZ_REQ_X1 - nr_tw(cnt, CZ_REQ_N_PT) - 36.0f;           // the bar ends 36 before the drawn count
    ui_visible("anim-group.req.reqBT", 1);
    nr_bar("anim-group.req.reqBT", CZ_REQ_X0, x1, y, 1.0f);
    nr_bar("anim-group.req.reqBF", CZ_REQ_X0, x1, y, n ? (float)pr / (float)n : 0.0f);
}
// progress counts for the faction it was made as: fac 0 WLF / 1 Seraphites; -1 = both (OG: not a feat of a side)
static void cz_bump_f(int fac, int kind, uint32_t v, int latch, const char *why) {     // latch: keep the best (rank / weeks / survivors / streak); else add
    if (!cz_scalar(kind)) return;
    int ch = 0;
    for (int f = 0; f < CZ_NFAC; f++) {
        if (fac >= 0 && f != fac) continue;
        uint32_t o = g_cz.fs[f].st[kind], nv = latch ? (v > o ? v : o) : o + v;
        if (nv == o) continue;
        g_cz.fs[f].st[kind] = nv; ch = 1;
        xlog("customize: progress %s %s %u -> %u (%s)", CZ_FAC_KEY[f], CZK_KEY[kind], (unsigned)o, (unsigned)nv, why);
    }
    if (ch) { g_cz_gen++; cz_save(); }
}
static void cz_bump(int kind, uint32_t v, int latch, const char *why) {      // for the faction we play as now (none yet: not counted)
    int fac = cz_cur_side();
    if (fac >= 0) cz_bump_f(fac, kind, v, latch, why);
}
// OG (the bong mask is exclusive to the og role: anyone who played before the public release has it unlocked): the
// profile's stat og is set once, when this PC shows play before the public release - the clock is before CZ_OG_UNTIL, or one of the
// mod's own files next to the exe (written on a first run / first settings change) was created before it. CZ_OG_UNTIL = the release
// moment in UTC (the number below, confirmed). A later release moves it.
#define CZ_OG_UNTIL 1795089600LL
static void cz_og_tick(void) {
    static int done;
    if (done || (g_cz.fs[0].st[CZK_OG] && g_cz.fs[1].st[CZK_OG])) return;
    done = 1;
    const char *why = NULL;
    if ((long long)time(NULL) < CZ_OG_UNTIL) why = "running before the public release";
    static const char *const F[] = {"t2r-name.txt", "t2r-keybinds.txt", "t2r-sens.txt", "t2r-loadout-names.txt", "t2r-menu-hasside.txt"};
    for (int i = 0; !why && i < (int)(sizeof F / sizeof F[0]); i++) {
        FILE *f = exe_side_open(F[i], "r");
        if (!f) continue;
        FILETIME cr = {0};
        if (GetFileTime((HANDLE)_get_osfhandle(_fileno(f)), &cr, NULL, NULL)) {
            const long long ut = (long long)(((uint64_t)cr.dwHighDateTime << 32 | cr.dwLowDateTime) / 10000000ULL) - 11644473600LL;
            if (ut > 0 && ut < CZ_OG_UNTIL) why = F[i];
        }
        fclose(f);
    }
    if (why) cz_bump_f(-1, CZK_OG, 1, 1, why);
    else xlog("customize: no play before the public release found on this PC (OG items stay locked)");
}
// CLOTHING BY ACHIEVEMENT (some clothing is unlockable via achievements; customize_items.py U_ACH, customize_ach.h):
// an item of kind CZK_ACH has no counter of ours - the achievements code knows (achieve.c ach_earned: 1 earned, 0 not, -1 the table
// has not arrived yet). Once earned the item gets a "grant" line in the profile: it stays open at the next start, before the table
// is there, and cz_validate does not put a worn achievement item back to the default. Without the achievements code in the ext
// (T2R_ACH_EARNED undefined) these items stay locked.
#include "customize_ach.h"
static void cz_ach_tick(void) {
#ifdef T2R_ACH_EARNED
    int ch = 0;
    for (int i = 0; i < CZ_ACH_NUNLOCKS; i++)
        if (ach_earned(CZ_ACH_UNLOCKS[i].ach) == 1 && cz_grant_add(&g_cz, CZ_ACH_UNLOCKS[i].label)) {
            xlog("customize: achievement %s earned - %s unlocked", CZ_ACH_UNLOCKS[i].ach, CZ_ACH_UNLOCKS[i].label);
            ch = 1;
        }
    if (ch) { g_cz_gen++; cz_save(); }
#endif
}
static void cz_stats_tick(void) {
    static uint64_t t;
    uint64_t now = GetTickCount64();
    if (now - t < 2000) return;
    t = now;
    cz_load();
    cz_og_tick();
    cz_ach_tick();
    {   // the KELLIE kind follows the account, both ways (a copied profile file opens nothing)
        const uint32_t k = (uint32_t)cz_is_kellie();
        int ch = 0;
        for (int f = 0; f < CZ_NFAC; f++) if (g_cz.fs[f].st[CZK_KELLIE] != k) { g_cz.fs[f].st[CZK_KELLIE] = k; ch = 1; }
        if (ch) { g_cz_gen++; cz_clamp_locked(&g_cz); cz_save(); xlog("customize: KELLIE rows %s for this account", k ? "open" : "closed"); }
    }
    clan_t c = clan_get();
    if (!c.ok) return;
    cz_bump(CZK_RANK, (uint32_t)(c.rk + 1), 1, "clan rank");                         // the 2013 rank: 1 + journeys won
    cz_bump(CZK_WEEKS, c.st == 3 || c.rk >= 1 ? 12u : (uint32_t)(c.day / 7), 1, "clan weeks");   // (status 3 = the 12 weeks won)
    if (c.st == 1) cz_bump(CZK_SURVIVORS, (uint32_t)(c.h + c.hu + c.s), 1, "clan survivors");
}
static const char *mf_map_img(int i);
// a match is over (feature_clan's match-over report): won 1 = our team won; cnt = the match's per-counter counts (g_mis_cnt, n ids);
// streak = our best streak of the match (g_cw_best), deaths = our deaths in it
static void cz_career_match(int won, int map_idx, const int *cnt, int n, int streak, int deaths) {
    cz_load();
    char log[160]; int ln = 0;
    const int fac = cz_cur_side();              // the faction this match was played as: its items' progress only
    if (fac < 0) { xlog("customize: match over without a side - no progress counted"); return; }
    cz_fstat_t *q = &g_cz.fs[fac];
    q->st[CZK_MATCHES]++;
    if (won == 1) { q->st[CZK_WINS]++; if (deaths == 0) q->st[CZK_FLAWLESS]++; }
    if (streak > 0 && (uint32_t)streak > q->st[CZK_STREAK]) q->st[CZK_STREAK] = (uint32_t)streak;
    if (won == 1 && map_idx >= 0) {
        const char *img = mf_map_img(map_idx);
        for (int m = 0; m < CZ_NUMAP; m++) if (img && !strcmp(img, CZ_UMAP_KEY[m])) { q->mw[m]++; ln += snprintf(log + ln, sizeof log - (size_t)ln, " win on %s (%u)", CZ_UMAP_KEY[m], (unsigned)q->mw[m]); }
    }
    for (int i = 0; i < n && i < CZ_NLBL; i++) {
        if (!CZ_LBL_KEY[i] || cnt[i] <= 0) continue;
        uint32_t v = (uint32_t)cnt[i];
        q->cnt[i] = q->cnt[i] + v < q->cnt[i] ? 0xFFFFFFFFu : q->cnt[i] + v;
        if (v > q->best[i]) q->best[i] = v;
        if (ln < (int)sizeof log - 24) ln += snprintf(log + ln, sizeof log - (size_t)ln, " %s+%u", CZ_LBL_KEY[i], (unsigned)v);
    }
    g_cz_gen++;
    cz_save();
    xlog("customize: match over as %s (won %d, deaths %d, streak %d) - matches %u, wins %u, flawless %u;%s", CZ_FAC_KEY[fac], won, deaths, streak,
         (unsigned)q->st[CZK_MATCHES], (unsigned)q->st[CZK_WINS], (unsigned)q->st[CZK_FLAWLESS], log);
}
static void cz_career_safe(void) { cz_load(); cz_bump(CZK_SAFES, 1, 0, "Interrogation safe cracked"); }

// ------------------------------------------------------------------ the way in (called from feature_clan next to cl13_open_check)
// The open (the page showed over the menu, then the stage's fade ran, then the cards popped in one by one): the screen goes
// black FIRST, the page is pushed once it is black, the stage spawns and poses its models and the cards load under the black (the page
// itself stays at alpha 0: Gui2 draws over the fade), and at g_cz_show_at the black lifts and the page fades in with it.
#define CZ_OPEN_IN 0.16f
#define CZ_OPEN_HOLD 0.85f
#define CZ_OPEN_OUT 0.40f
static uint64_t g_cz_open_at, g_cz_show_at;
static void cz_open_start(const char *why) {
    uint64_t now = GetTickCount64();
    if (g_cz_open_at) return;
    fade_to_black(CZ_OPEN_IN, CZ_OPEN_HOLD, CZ_OPEN_OUT);
    g_cz_open_at = now + (uint64_t)(CZ_OPEN_IN * 1000.0f) + 20;
    g_cz_show_at = now + (uint64_t)((CZ_OPEN_IN + CZ_OPEN_HOLD) * 1000.0f);
    xlog("customize: CUSTOMIZE opening (%s) - black first, the page under it", why);
}
static void cz_open_check(void) {
    static int h_act, armed;
    int pad = 0;
    cz_stats_tick();
    if (g_cz_open_at && GetTickCount64() >= g_cz_open_at) { g_cz_open_at = 0; push_page("to1/customize"); }
    if (!cz_enabled()) { ui_visible("anim-group.menuList.Customize", 0); return; }
    int sel = menu_edge(0x6695c5b7921bb5ddULL /* menu-select */, &h_act);
    int clk = g_click_edge && ui_mouse_over("anim-group.menuList.Customize", 1);
    if (!armed) { armed = 1; sel = 0; }
    if (g_mdpk.open || g_mdpk.chosen) { sel = 0; clk = 0; }
    if ((sel && mk_focused("anim-group.menuList.Customize") && mk_sel_for("anim-group.menuList.Customize")) || clk) pad = 1;
    if (!pad || g_lo_state != LO_NONE) return;
    if (g_mf_code_now >= 100 || g_mh_load || g_mh_st || g_ls_own || !ms_on_menu() || g_pm.pending) return;
    static uint64_t last;
    uint64_t now = GetTickCount64();
    if (now - last < 1500) return;
    last = now;
    ui_sfx("sfx-select");
    cz_open_start(clk ? "click" : "select");
}

// ------------------------------------------------------------------ hover (canvas geometry = the widget file)
static void cz_scan(void) {
    float cx, cy;
    CZ.hv_card = CZ.hv_tab = -1; CZ.hv_back = CZ.hv_sel = CZ.hv_sw = CZ.hv_f = CZ.hv_b = CZ.hv_q = CZ.hv_e = 0;
    if (!ui_cursor_canvas(&cx, &cy)) return;
    if (cy >= CZ_BAR_CY - 55.0f && cy < CZ_BAR_CY + 55.0f)          // the tab bar: one cell per icon (centre +- half a cell + gap)
        for (int i = 0; i < CZ_NCAT; i++) {
            float x = CZ_BAR_IC_X0 + CZ_BAR_CELL / 2.0f + i * (CZ_BAR_CELL + CZ_BAR_GAP);        // (i = the place on the bar)
            if (cx >= x - (CZ_BAR_CELL + CZ_BAR_GAP) / 2.0f && cx < x + (CZ_BAR_CELL + CZ_BAR_GAP) / 2.0f) CZ.hv_tab = CZ_TAB_ORDER[i];
        }
    CZ.hv_q = ui_mouse_over("anim-group.tabs.tabQHit", 1);
    CZ.hv_e = ui_mouse_over("anim-group.tabs.tabEHit", 1);
    if (cx >= CZ_X0 && cy >= CZ_GRID_Y) {
        int col = (int)((cx - CZ_X0) / (CZ_CARD_W + CZ_GAP)), row = (int)((cy - CZ_GRID_Y) / (CZ_CARD_H + CZ_GAP));
        float ix = cx - CZ_X0 - col * (CZ_CARD_W + CZ_GAP), iy = cy - CZ_GRID_Y - row * (CZ_CARD_H + CZ_GAP);
        if (col < CZ_COLS && row < CZ_ROWS && ix < CZ_CARD_W && iy < CZ_CARD_H && CZ.off[CZ.cat] + row * CZ_COLS + col < cz_rows()) CZ.hv_card = row * CZ_COLS + col;
    }
    CZ.hv_back = ui_mouse_over("anim-group.pBackHit", 1);
    CZ.hv_sel = ui_mouse_over("anim-group.pSelHit", 1);
    CZ.hv_sw = ui_mouse_over("anim-group.pSwHit", 1);
    CZ.hv_f = ui_mouse_over("anim-group.czHitF", 1);
    CZ.hv_b = ui_mouse_over("anim-group.czHitB", 1);
    CZ.hv_ch = ui_mouse_over("anim-group.pChHit", 1);
    CZ.hv_zm = ui_mouse_over("anim-group.pZmHit", 1);            // (a page built before the prompt existed: no widget, 0)
    CZ.hv_uo = ui_mouse_over("anim-group.tabs.tabUoHit", 1);
    CZ.hv_tip = ui_mouse_over("anim-group.czTip.tipHit", 1);
}
static int g_czc_tip;                             // (customize_challenges.c) 1 = the first-visit tip is up
// ZOOM IN ON ITEM
// Z / pad R3 / a click on the prompt toggles it; customize_stage.c moves the page camera onto the
// part of the front model the shown category is about (and turns the model for BACKPACK), eased - never a cut.
static int g_cz_zoom;
// ... and how far
// 1 = the category's framing, CZ_ZOOM_MIN
// = half of the way to it (in fov ratio), CZ_ZOOM_MAX = closer. The wheel (cursor off the cards: over them it scrolls the list) and
// the pad's RIGHT stick up / down change it - the left stick already moves through the cards.
#define CZ_ZOOM_MIN 0.5f
#define CZ_ZOOM_MAX 2.2f
static float g_cz_zoom_u = 1.0f;
static int czc_click_hit(void);
static uint64_t czc_page_id(void);
static int cz_click_hit(void) {
    if (top_page() == czc_page_id()) return czc_click_hit();
    if (!CZ.up || CZ.quiet) return 0;
    cz_scan();
    if (g_czc_tip) return CZ.hv_tip;
    return CZ.hv_card >= 0 || CZ.hv_tab >= 0 || CZ.hv_back || CZ.hv_sel || CZ.hv_sw || CZ.hv_b || CZ.hv_q || CZ.hv_e || CZ.hv_ch || CZ.hv_zm || CZ.hv_uo;
}

// ------------------------------------------------------------------ text width / rules / card names
static float cz_text_w(const char *s, float pt) { return feed_text_w(s, pt / FEED_PT); }     // (the kill feed's per-glyph estimate)
// a 2 px rule rect (pivot at its left end, laid out base_w wide in the widget file) stretched to run from x0 to x1
static void cz_rule(const char *name, float x0, float y, float x1, float base_w) {
    uint64_t w = ui_w(name);
    if (!w) return;
    if (x0 > x1 - 40.0f) x0 = x1 - 40.0f;
    w_pivot(w, x0, y);
    w_scale(w, (x1 - x0) / base_w, 1.0f);
}
// a card name wider than its box breaks at the space nearest its middle (two lines; customize_widgets.card_label is the same rule)
static void cz_card_label(const char *s, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s", s);
    if (cz_text_w(s, 24.0f) <= CZ_CARD_TXT_W) return;
    int n = (int)strlen(out), best = -1;
    for (int k = 0; k < n; k++) if ((out[k] == ' ' || out[k] == '-') && (best < 0 || abs(k - n / 2) < abs(best - n / 2))) best = k;
    if (best < 0) return;
    if (out[best] == ' ') { out[best] = '\n'; return; }
    if (n + 1 < cap) { memmove(out + best + 2, out + best + 1, (size_t)(n - best)); out[best + 1] = '\n'; }      // a hyphen stays, the line ends after it
}

// ------------------------------------------------------------------ fill (only on a change of what is shown)
static void cz_fill(void) {
    const int fac = CZ.fac, sex = cz_sex(), cat = CZ.cat, n = cz_rows(), foc = CZ.foc[cat], off = CZ.off[cat], eq = cz_equipped(cat), cur = cz_cur_side();
    const int hv = CZ.hv_card;
    uint64_t now = GetTickCount64();
    int msg = now < CZ.msg_until;
    uint64_t key = (uint64_t)fac | (uint64_t)sex << 2 | (uint64_t)cat << 4 | (uint64_t)(foc & 0xff) << 8 | (uint64_t)(off & 0xff) << 16 | (uint64_t)(eq & 0xff) << 24 |
                   (uint64_t)(cur + 1) << 32 | (uint64_t)(hv + 1 & 0x1f) << 34 | (uint64_t)msg << 40 | (uint64_t)(g_pad_mode & 1) << 41 | (uint64_t)(g_cz_gen & 0xff) << 42 | (uint64_t)(g_cz_zoom & 1) << 50 |
                   (uint64_t)!!(g_cz.flags & CZF_UONLY) << 51 | (uint64_t)!!CZ.hv_uo << 52 | 1ULL << 63;
    if (key == CZ.key) return;
    CZ.key = key;
    char p[96], b[256];          // (b: an image path; a 128 byte one cut a long card name short and the tile kept another tile's picture)
    {   // header: CUSTOMIZE | <category> + rule, placed from the drawn widths (the stock flow group: 65 apart)
        const char *t = loc_tr("CUSTOMIZE"), *sub = loc_tr(CZ_CAT_LABEL[cat]);
        float x = CZ_HEAD_X + cz_text_w(t, CZ_HEAD_PT) + 65.0f;
        uint64_t w;
        if ((w = ui_w("anim-group.titleDiv"))) w_pivot(w, x - 1.0f, CZ_HEAD_CY - 25.0f);
        x += 65.0f;
        if ((w = ui_w("anim-group.titleSub"))) w_pivot(w, x, CZ_HEAD_CY);
        ui_text("anim-group.titleSub", sub);
        x += cz_text_w(sub, CZ_HEAD_PT) + 65.0f;
        cz_rule("anim-group.titleRule", x, CZ_HEAD_CY - 1.0f, CZ_HEAD_RULE_X1, CZ_HEAD_RULE_X1 - 1240.0f);
    }
    for (int i = 0; i < CZ_NCAT; i++) {          // the open tab white, a hovered one half, the rest at the stock fader alpha
        snprintf(p, sizeof p, "anim-group.tabs.tabIc%d", i); w_alpha(ui_w(p), i == cat ? 1.0f : i == CZ.hv_tab ? 0.6f : CZ_TAB_A_OFF);
    }
    const int fk = cz_r2k(fac, sex, cat, foc);       // (rows -> table: hidden rows are not in the page)
    const cz_item_t *fi = fk >= 0 ? cz_item(fac, sex, cat, fk) : NULL;
    {   // item row: the focused item's name + its rule from just past the name
        const char *nm = fi ? loc_tr(fi->label) : "";
        ui_text("anim-group.secName", nm);
        cz_rule("anim-group.secRule", CZ_ITEM_X + cz_text_w(nm, CZ_ITEM_PT) + 60.0f, CZ_ITEM_CY - 1.0f, CZ_ITEM_RULE_X1, CZ_ITEM_RULE_X1 - CZ_ITEM_X - 500.0f);
    }
    for (int i = 0; i < CZ_NCARD; i++) {
        const int k = cz_r2k(fac, sex, cat, off + i);
        const cz_item_t *it = k >= 0 ? cz_item(fac, sex, cat, k) : NULL;
        snprintf(p, sizeof p, "anim-group.grid.cd%d", i); ui_visible(p, it != NULL);
        if (!it) continue;
        const int f = off + i == foc, h = i == hv, lk = cz_locked(fac, sex, cat, k);
        const int cf = cat != CZ_SEX && cz_conflict_cat(fac, sex, g_cz.it[fac][sex], cat, k) >= 0;      // (cannot be worn with a worn item: dimmed)
        const char *ic = cat == CZ_MELEE ? (k ? "cz-knife" : "cz-fists") : cat == CZ_SEX ? (k ? "cz-female" : "cz-male") : CZ_ICON[cat];
        // (the engine's image setter ends the game without a crash report on a 224 character path - a six-pak item's card
        // name; customize_items.CARD_SLUG_MAX keeps the names under 96, a longer one here shows the category icon instead)
        const char *pic = it->pic && strlen(it->pic) <= 96 ? it->pic : NULL;
        snprintf(p, sizeof p, "anim-group.grid.cd%d.cdPic%d", i, i); ui_visible(p, pic != NULL);   // the item's own picture (customize_cards.py)
        if (pic) { snprintf(b, sizeof b, "streaming/t2r/icons/%s.png", pic); ui_image(p, b); w_alpha(ui_w(p), cf ? 0.3f : f ? 1.0f : h ? 0.85f : 0.7f); }
        snprintf(p, sizeof p, "anim-group.grid.cd%d.cdIc%d", i, i); ui_visible(p, pic == NULL);    // else the category icon
        if (!pic) { snprintf(b, sizeof b, "streaming/t2r/icons/%s.png", ic); ui_image(p, b); w_alpha(ui_w(p), f ? 1.0f : h ? 0.8f : 0.5f); }
        snprintf(p, sizeof p, "anim-group.grid.cd%d.cdNm%d", i, i); cz_card_label(loc_tr(it->label), b, sizeof b); ui_text(p, b);
        w_alpha(ui_w(p), cf ? 0.4f : f || h ? 1.0f : 0.7f);
        snprintf(p, sizeof p, "anim-group.grid.cd%d.cdFill%d", i, i); ui_visible(p, f || h);
        snprintf(p, sizeof p, "anim-group.grid.cd%d.cdTop%d", i, i); ui_visible(p, f);
        snprintf(p, sizeof p, "anim-group.grid.cd%d.cdBot%d", i, i); ui_visible(p, f);
        // (an OG item's card has its badge where the check sits: customize_cards.OG_BOX - there the check is the tile's second one,
        // top right (cdEqU; a page built before it existed keeps the bottom one). NOT by moving cdEq: a pivot write per tile
        // in one fill killed the game without a crash report at the 9th tile - the 15th transform write of the frame; this page's
        // writes are not deduped (ui_prop_due) and a fill already makes up to 13.)
        {
            char pu[96]; snprintf(pu, sizeof pu, "anim-group.grid.cd%d.cdEqU%d", i, i);
            const int up = it->uk && it->uk < CZ_NCHAL && CZ_CHAL[it->uk].kind == CZK_OG && ui_w(pu);     // (the OG card carries a badge there)
            snprintf(p, sizeof p, "anim-group.grid.cd%d.cdEq%d", i, i); ui_visible(p, k == eq && !up);
            ui_visible(pu, k == eq && up);
        }
        snprintf(p, sizeof p, "anim-group.grid.cd%d.cdLock%d", i, i); ui_visible(p, lk);
    }
    const int sb = n > CZ_NCARD;
    ui_visible("anim-group.grid.sbTrack", sb); ui_visible("anim-group.grid.sbThumb", sb);
    if (sb) {
        uint64_t th = ui_w("anim-group.grid.sbThumb");
        const float gh = CZ_ROWS * CZ_CARD_H + (CZ_ROWS - 1) * CZ_GAP;
        if (th) { w_scale(th, 1.0f, (float)CZ_NCARD / (float)n); w_pivot(th, CZ_SB_X, CZ_GRID_Y + gh * (float)off / (float)n); }
    }
    ui_image("anim-group.logoF", CZ_LOGO[fac]); ui_image("anim-group.logoB", CZ_LOGO[!fac]);
    {   // zoomed in: the faction marks drawn over the models' feet would lie across the framed part
        const int zm = g_cz_zoom && cat != CZ_SEX;
        ui_visible("anim-group.logoF", !zm); ui_visible("anim-group.logoB", !zm); ui_visible("anim-group.mdlName", !zm);
        ui_visible("anim-group.chkF", cur == fac && !zm); ui_visible("anim-group.chkB", cur == !fac && !zm);
    }
    ui_text("anim-group.mdlName", CZ_FAC_NAME[fac]);
    if (ui_w("anim-group.pZmTx")) { ui_text("anim-group.pZmKt", g_pad_mode ? "R3" : "Z"); ui_text("anim-group.pZmTx", loc_tr(g_cz_zoom ? "ZOOM OUT" : "ZOOM IN")); }
    if (ui_w("anim-group.tabs.tabUoTx")) {      // UNLOCKED ONLY: the label names what a press does; on = white, hovered brighter
        const int uo = (g_cz.flags & CZF_UONLY) != 0;
        ui_text("anim-group.tabs.tabUoKt", g_pad_mode ? "L3" : "U");
        ui_text("anim-group.tabs.tabUoTx", loc_tr(uo ? "SHOW ALL" : "UNLOCKED ONLY"));
        w_alpha(ui_w("anim-group.tabs.tabUoTx"), uo || CZ.hv_uo ? 1.0f : 0.8f);
    }
    ui_text("anim-group.pSelTx", loc_tr("EQUIP"));
    ui_text("anim-group.pSwTx", loc_tr("SWITCH FACTION"));
    ui_text("anim-group.pChKt", g_pad_mode ? "Y" : "C");
    ui_text("anim-group.pChTx", loc_tr("CHALLENGES"));
    ui_text("anim-group.pHint", msg ? CZ.msg : "");                // a refusal / note (the requirement card steps aside for it)
    // - a page built before the widget existed has none
    if (ui_w("anim-group.facBox")) {             // the note on its dark, framed box, as wide as the text (customize_widgets FAC_*)
        const char *nt = cur >= 0 && cur != fac ? loc_tr("NOT YOUR CURRENT FACTION - WORN ONCE YOU SWITCH SIDES") : "";
        ui_text("anim-group.facBox.facNote", nt);
        w_alpha(ui_w("anim-group.facBox"), nt[0] ? 1.0f : 0.0f);
        if (nt[0]) {
            int nch = 0;
            for (const char *q = nt; *q; q++) nch += (*q & 0xc0) != 0x80;               // characters (the tracking adds 2 units each)
            // (build #14: the box came out 13 % wider than the line - 688 px of estimate for 607 px of text at 1080p; the kill
            // feed's glyph estimate is generous for this face at 30 pt. The text is centred in the box, so what is left of the
            // error is equal padding on both sides.)
            const float x1 = CZ_HEAD_RULE_X1, x0 = x1 - ((cz_text_w(nt, 30.0f) + 2.0f * (float)nch) * 0.88f + 2.0f * CZ_FAC_PAD);
            uint64_t w;
            cz_rule("anim-group.facBox.facBg", x0, CZ_FAC_Y, x1, CZ_FAC_W);
            cz_rule("anim-group.facBox.facFrT", x0, CZ_FAC_Y, x1, CZ_FAC_W);
            cz_rule("anim-group.facBox.facFrB", x0, CZ_FAC_Y + CZ_FAC_H - 2.0f, x1, CZ_FAC_W);
            if ((w = ui_w("anim-group.facBox.facFrL"))) w_pivot(w, x0, CZ_FAC_Y);
            if ((w = ui_w("anim-group.facBox.facNote"))) w_pivot(w, (x0 + x1) * 0.5f, CZ_FAC_Y + CZ_FAC_H * 0.5f);      // (centred text, pivot at its middle)
        }
    } else if (ui_w("anim-group.facNote"))       // (a page built before the box: the plain line)
        ui_text("anim-group.facNote", cur >= 0 && cur != fac ? loc_tr("NOT YOUR CURRENT FACTION - WORN ONCE YOU SWITCH SIDES") : "");
    cz_req_fill(fac, sex, cat, fk, fi && !msg && !g_czc_tip);     // how to unlock the focused item (the card under the grid; the
                                                                    // CHALLENGES tip hides it, cz_fill runs every tick)
}

// ------------------------------------------------------------------ actions
static void cz_keep_visible(void) {
    int n = cz_rows(), *foc = &CZ.foc[CZ.cat], *off = &CZ.off[CZ.cat];
    if (*foc >= n) *foc = n ? n - 1 : 0;
    if (*foc < 0) *foc = 0;
    int row = *foc / CZ_COLS, top = *off / CZ_COLS;
    if (row < top) *off = row * CZ_COLS;
    else if (row >= top + CZ_ROWS) *off = (row - CZ_ROWS + 1) * CZ_COLS;
}
static void cz_set_cat(int c, const char *why) {
    c = (c % CZ_NCAT + CZ_NCAT) % CZ_NCAT;
    if (c == CZ.cat) return;
    CZ.cat = c;
    CZ.foc[c] = cz_eq_row(c);              // a tab opens on what is worn
    cz_keep_visible();
    ui_sfx("sfx-cycle");
    xlog("customize: category %s (%s)", CZ_CAT_LABEL[c], why);
}
static void cz_set_fac(int f, const char *why) {
    if (f == CZ.fac) return;
    CZ.fac = f;
    for (int c = 0; c < CZ_NCAT; c++) { CZ.foc[c] = cz_eq_row(c); CZ.off[c] = 0; }
    { int c = CZ.cat; CZ.cat = -1; cz_set_cat(c, why); }
    ui_sfx("sfx-select");
    xlog("customize: editing %s (%s)", CZ_FAC_NAME[f], why);
}
// UNLOCKED ONLY on / off: the rows of every category change, so each focus moves to where its item now stands (a hidden one:
// the worn item); the scroll starts over and follows the focus
static void cz_set_uonly(int on, const char *why) {
    on = on != 0;
    if (on == ((g_cz.flags & CZF_UONLY) != 0)) return;
    const int fac = CZ.fac, sex = cz_sex();
    int fk[CZ_NCAT];
    for (int c = 0; c < CZ_NCAT; c++) fk[c] = cz_r2k(fac, sex, c, CZ.foc[c]);
    if (on) g_cz.flags |= CZF_UONLY; else g_cz.flags &= ~CZF_UONLY;
    for (int c = 0; c < CZ_NCAT; c++) {
        CZ.foc[c] = fk[c] >= 0 && !cz_row_out(fac, sex, c, fk[c]) ? cz_k2r(fac, sex, c, fk[c]) : cz_eq_row(c);
        CZ.off[c] = 0;
    }
    cz_keep_visible();
    cz_save();
    CZ.key = 0;
    ui_sfx("sfx-cycle");
    xlog("customize: unlocked only %s (%s; %s: %d of %d rows)", on ? "on" : "off", why, CZ_CAT_LABEL[CZ.cat], cz_rows(), cz_count(fac, sex, CZ.cat));
}
static void cz_net_changed(void);                 // customize_compose.c: the outfit changed (next MSG_CUSTOM_LOOK carries a new counter)
static void cz_equip(int r, const char *why) {                // r: the ROW position on the page
    const int fac = CZ.fac, sex = cz_sex(), cat = CZ.cat, k = cz_r2k(fac, sex, cat, r);
    const cz_item_t *it = k >= 0 ? cz_item(fac, sex, cat, k) : NULL;
    if (!it) return;
    if (cz_locked(fac, sex, cat, k)) {             // still locked: the card stays and blinks
        char t[128]; cz_lock_text(fac, sex, cat, k, t, sizeof t);
        CZ.deny_until = GetTickCount64() + CZ_DENY_MS;
        xlog("customize: %s %s refused (locked: %s)", CZ_CAT_LABEL[cat], it->label, t);
        return;
    }
    if (it->tier > 1) { cz_note("NOT IN MULTIPLAYER YET"); xlog("customize: %s %s refused (tier %d)", CZ_CAT_LABEL[cat], it->label, it->tier); return; }
    {   // (the Seraphite hood rows are locked out while the worn top has no hood - customize_core.h cz_conflict_cat)
        const int cc = cat != CZ_SEX ? cz_conflict_cat(fac, sex, g_cz.it[fac][sex], cat, k) : -1;
        const cz_item_t *w = cc >= 0 ? cz_item(fac, sex, cc, g_cz.it[fac][sex][cc]) : NULL;
        if (w) {
            const char *tp = loc_tr("CONFLICTS WITH %s"), *at = strstr(tp, "%s");
            if (at) snprintf(CZ.msg, sizeof CZ.msg, "%.*s%s%s", (int)(at - tp), tp, loc_tr(w->label), at + 2);
            else snprintf(CZ.msg, sizeof CZ.msg, "%s %s", tp, loc_tr(w->label));
            CZ.msg_until = GetTickCount64() + 2500; CZ.key = 0;
            xlog("customize: %s %s refused (conflicts with the worn %s %s)", CZ_CAT_LABEL[cat], it->label, CZ_CAT_LABEL[cc], w->label);
            return;
        }
    }
    if (cat == CZ_SEX) {
        if (g_cz.sex[fac] == k) return;
        g_cz.sex[fac] = (uint8_t)k;
        for (int c = 0; c < CZ_NCAT; c++) if (c != CZ_SEX) { CZ.foc[c] = cz_eq_row(c); CZ.off[c] = 0; }
    } else {
        if (g_cz.it[fac][sex][cat] == k) return;
        g_cz.it[fac][sex][cat] = (uint8_t)k;
    }
    cz_save();
    cz_net_changed();
    ui_sfx("sfx-select");
    xlog("customize: %s %s %s = %s (%s)", CZ_FAC_NAME[fac], CZ_SEX_KEY[sex], CZ_CAT_LABEL[cat], it->label, why);
}

// ------------------------------------------------------------------ page lifecycle
static void czc_tip_show(int on);                 // customize_challenges.c
static void czc_tip_done(const char *how);
static void czc_open(const char *why);
static int g_czc_tip;
static void cz_enter(void) {
    memset(&CZ, 0, sizeof CZ);
    cz_load();
    CZ.up = 1; CZ.enter_ms = GetTickCount64();
    CZ.h_sel = CZ.h_u = CZ.h_d = CZ.h_l = CZ.h_r = CZ.h_back = CZ.h_esc = CZ.h_pv = CZ.h_nx = CZ.h_act = CZ.h_q = CZ.h_e = CZ.h_f = CZ.h_c = CZ.h_x = 1;   // held on arrival = not a press
    CZ.h_uk = CZ.h_l3 = 1;
    CZ.t_u = CZ.t_d = CZ.t_l = CZ.t_r = ~0ULL;
    int cur = cz_cur_side();
    CZ.fac = cur >= 0 ? cur : 0;                 // the faction the player plays as stands in front first
    CZ.cat = CZ_TORSO;
    for (int c = 0; c < CZ_NCAT; c++) CZ.foc[c] = cz_eq_row(c);
    cz_keep_visible();
    w_alpha(ui_w("anim-group"), 0.0f);
    xlog("customize: page up (editing %s, side %d)", CZ_FAC_NAME[CZ.fac], cur);
    czc_tip_show(!(g_cz.flags & CZF_CHTIP));     // the first visit: the CHALLENGES tip over the page (customize_challenges.c)
}
static void cz_close(const char *how) {
    if (CZ.closing || CZ.quiet) return;
    xlog("customize: back (%s)", how);
    w_alpha(ui_w("anim-group"), 1.0f);
    CZ.quiet = 1;
    CZ.back_f = g_frame ? (int)g_frame : 1;      // the engine pops a pushed page on menu-back itself; Esc / a click it does not (judged 6 frames later)
    if (strcmp(how, "menu-back") && top_page() == cz_page_id()) { pop_page(); CZ.closing = 1; CZ.back_f = 0; }
}
#include "customize_compose.c"       // the outfit worn in a match (t2r-cz-* records, spawn hooks, MSG_CUSTOM_LOOK)
#include "customize_challenges.c"   // CUSTOMIZE | CHALLENGES + the first-visit tip (uses CZ / g_cz / cz_* above)
#include "customize_stage.c"         // the stage: fade, two live models, the exchange, rotate (re/customize-1117.md "The stage")
// dev (A/B without input, not in release builds): t2r-cz-test.txt next to the exe = ONE command, the file is removed after it runs.
//   open             MP menu: open CUSTOMIZE (the menu row's push)
//   cat <0..6>       CUSTOMIZE: show that category          fac <0|1>   CUSTOMIZE: edit WLF / Seraphites
//   equip <k>        CUSTOMIZE: equip item k of the shown category (a locked one shows its refusal note)
//   chal             open CHALLENGES                         tip         dismiss the first-visit tip
//   unlock <0|1>     CUSTOMIZE: every row unlocked for this run (nothing saved as unlocked)
//   zoom <0|1>       CUSTOMIZE: ZOOM IN ON ITEM off / on
//   uonly <0|1>      CUSTOMIZE: UNLOCKED ONLY off / on (saved, as the toggle)
//   yaw <deg>        CUSTOMIZE: turn the front model         fov <deg> / cpitch <deg x 10> / cyaw <deg x 10>   page camera zoom + aim (close shots)
//   cfac <0|1>       CHALLENGES: faction tab                 crow <d>    CHALLENGES: move d rows      ccol <c>   CHALLENGES: column c
//   cequip           CHALLENGES: equip the focused tile
//   pad <k>          press a menu command on the engine's virtual pad (padinj, the pad path a real controller takes) for 150 ms:
//                    0 menu-prev-tab, 1 menu-next-tab, 2 menu-action, 3 menu-select, 4 menu-back, 5 menu-inspect, 6 menu-zoom
//   probe            learn the pad buttons of menu-inspect / menu-zoom / menu-toggle-player (padinject.c logs each command's entries)
static int padinj_press(const char *name, int ms);   // padinject.c (included later)
static void cz_test_tick(uint64_t top) {
#ifndef T2R_RELEASE
    static const char *const PADC[] = {"menu-prev-tab", "menu-next-tab", "menu-action", "menu-select", "menu-back", "menu-inspect", "menu-zoom"};
    static int probing;
    if (probing) { padinj_raw_down("menu-inspect"); padinj_raw_down("menu-zoom"); padinj_raw_down("menu-toggle-player"); }
    if (g_frame % 30) return;
    FILE *f = exe_side_open("t2r-cz-test.txt", "r");
    if (!f) return;
    char cmd[16] = ""; int v = 0;
    int n = fscanf(f, "%15s %d", cmd, &v);
    fclose(f);
    { char pth[MAX_PATH]; GetModuleFileNameA(NULL, pth, MAX_PATH); char *sl = strrchr(pth, '\\'); if (sl) { strcpy(sl + 1, "t2r-cz-test.txt"); DeleteFileA(pth); } }
    if (n < 1) return;
    int on_cz = top && top == cz_page_id() && CZ.up, on_ch = top && top == czc_page_id() && CZC.up;
    xlog("customize: test command \"%s %d\" (page %s)", cmd, v, on_ch ? "challenges" : on_cz ? "customize" : "other");
    if (!strcmp(cmd, "pad")) { if (v >= 0 && v < (int)(sizeof PADC / sizeof *PADC)) xlog("customize: test pad press %s -> %s", PADC[v], padinj_press(PADC[v], 150) ? "queued on the pad" : "NOT queued (input bit not learned)"); }
    else if (!strcmp(cmd, "probe")) probing = 1;
    else if (!strcmp(cmd, "tip")) { if (g_czc_tip) czc_tip_done("test file"); }
    else if (on_cz && !g_czc_tip && !strcmp(cmd, "cat")) cz_set_cat(v, "test file");
    else if (on_cz && !g_czc_tip && !strcmp(cmd, "fac")) cz_set_fac(v ? 1 : 0, "test file");
    else if (on_cz && !g_czc_tip && !strcmp(cmd, "equip")) { if (v >= 0 && v < cz_rows()) { CZ.foc[CZ.cat] = v; cz_keep_visible(); cz_equip(v, "test file"); } }
    else if (on_cz && !g_czc_tip && !strcmp(cmd, "chal")) czc_open("test file");
    else if (on_cz && !strcmp(cmd, "unlock")) { g_cz_test_open = v != 0; CZ.key = 0; }
    else if (on_cz && !strcmp(cmd, "zoom")) g_cz_zoom = v != 0;                         // ZOOM IN ON ITEM on / off
    else if (on_cz && !g_czc_tip && !strcmp(cmd, "uonly")) cz_set_uonly(v, "test file");   // UNLOCKED ONLY on / off
    else if (on_cz && !strcmp(cmd, "yaw")) CZS.user_yaw = (float)v;                    // the front model's turn (degrees)
    else if (on_cz && !strcmp(cmd, "fov")) g_czs_t_fov = (float)v;                     // page camera fov (0 = the stage's)
    else if (on_cz && !strcmp(cmd, "cpitch")) g_czs_t_pitch = (float)v / 10.0f;        // page camera pitch / yaw offsets, tenths of a degree
    else if (on_cz && !strcmp(cmd, "cyaw")) g_czs_t_yaw = (float)v / 10.0f;
    else if (on_ch && !strcmp(cmd, "cfac")) czc_set_fac(v, "test file");
    else if (on_ch && !strcmp(cmd, "crow")) czc_move_row(v);
    else if (on_ch && !strcmp(cmd, "ccol")) { if (czc_row_ok(CZC.fr) && v >= 0 && v < CZC.rn[CZC.fr]) { CZC.fc = v; CZC.want = v - CZC.off[CZC.fr]; czc_keep_visible(); } }
    else if (on_ch && !strcmp(cmd, "cequip")) czc_equip("test file");
    else if (!on_cz && !on_ch && !strcmp(cmd, "open")) {
        if (cz_enabled() && g_lo_state == LO_NONE && g_mf_code_now < 100 && !g_mh_load && !g_mh_st && !g_ls_own && ms_on_menu() && !g_pm.pending) cz_open_start("test file");
        else xlog("customize: test open refused (not on the idle MP menu)");
    }
    else xlog("customize: test command ignored (wrong page, or the tip is up)");
#else
    (void)top;
#endif
}
// A found match closes CUSTOMIZE / CHALLENGES before the map load. Each Gui2 page keeps its engine processes through the load, and the
// load adds about 2060 to the menu's ~7300 (private-lobby A/B, re/private-lobby-1117.md): a stacked mod page set took a client past
// the engine's 10000-process limit ("We have more than 10000 Processes!"). Match found = lobby phase (lobby object +0x1b4) 2, the same test
// as the match flow / gunlimit / rpcstate. CUSTOMIZE opens from the main menu, not as a clan extension tab (nr_clan_xtab_route), so the
// match flow's page close does not take these pages: this close runs with the flow on and off.
// One pop per page instance, the match flow's guard (feature_matchflow): the menu queue can hold a pop while the stack is busy, so
//   a pop still queued                          -> wait;
//   same top, g_ui_gen == at the request        -> the pop never ran -> pop again;
//   same top, g_ui_gen == request + 1           -> it ran, the stack has not shown the new top yet -> wait (2 s, then pop again with a log line);
//   a new top / g_ui_gen past that              -> the next page (CHALLENGES -> CUSTOMIZE under it) -> pop it.
static void cz_match_found_close(uint64_t top) {
    static uint64_t popped_top, ran_f; static unsigned req_gen; static int n_closed;
    int ours = top && (top == cz_page_id() || top == czc_page_id());
    uint8_t *o = ours ? lobby_obj() : NULL;
    if (!ours || !o || IsBadReadPtr(o + 0x1b4, 4) || *(int *)(o + 0x1b4) != 2) {
        if (popped_top && n_closed) xlog("customize: match found - %d page(s) closed before the map load", n_closed);
        popped_top = 0; ran_f = 0; n_closed = 0;
        return;
    }
    int queued = 0;
    for (int i = 0; i < g_mq_n; i++) if (g_mq[i].pop) queued = 1;
    if (queued) return;
    int ran_only = top == popped_top && g_ui_gen == req_gen + 1;
    if (!ran_only) ran_f = 0; else if (!ran_f) ran_f = g_frame;
    if (ran_only && g_frame - ran_f > 120) { xlog("customize: %s still on top 2 s after its pop ran - popped again", page_name(top)); ran_only = 0; ran_f = 0; }
    if (ran_only) return;
    if (top == popped_top && g_ui_gen == req_gen) xlog("customize: the pop of %s never ran (menu queue) - popped again", page_name(top));
    czs_off("match found", 0);
    if (top == cz_page_id()) { w_alpha(ui_w("anim-group"), 1.0f); CZ.closing = 1; }      // (as cz_close)
    else CZC.closing = 1;
    popped_top = top; req_gen = g_ui_gen;
    pop_page();
    n_closed++;
    xlog("customize: match found - closing %s before the map load", page_name(top));
}
static void feature_customize(void) {
    uint64_t top = top_page();
    cz_test_tick(top);
    cz_match_found_close(top);
    czs_tick(top && (top == cz_page_id() || top == czc_page_id()) && !CZ.closing && !CZ.quiet, CZ.fac);
    if (top && top == czc_page_id()) {               // the CHALLENGES page (pushed over CUSTOMIZE)
        czc_tick();
        return;
    }
    if (CZC.up) {                                    // back from CHALLENGES: what is held now (the BACK that closed it) is not a press here
        CZC.up = 0; CZ.key = 0;
        CZ.h_sel = CZ.h_u = CZ.h_d = CZ.h_l = CZ.h_r = CZ.h_back = CZ.h_esc = CZ.h_pv = CZ.h_nx = CZ.h_act = CZ.h_q = CZ.h_e = CZ.h_f = CZ.h_c = CZ.h_x = 1;
        CZ.h_uk = CZ.h_l3 = 1;
        CZ.t_u = CZ.t_d = CZ.t_l = CZ.t_r = ~0ULL;
        xlog("customize: challenges page left (top now %s)", page_name(top));
    }
    if (top != cz_page_id() || !top) {
        if (CZ.up) { CZ.up = 0; xlog("customize: page left (top now %s)", page_name(top)); }
        return;
    }
    if (!CZ.up) cz_enter();
    if (CZ.closing) return;
    uint64_t now = GetTickCount64();
    int sel = menu_edge(0x6695c5b7921bb5ddULL /* menu-select */, &CZ.h_sel);
    int up = gu_rep(&CZ.h_u, &CZ.t_u, fnv64("menu-up")), down = gu_rep(&CZ.h_d, &CZ.t_d, fnv64("menu-down"));
    int left = gu_rep(&CZ.h_l, &CZ.t_l, fnv64("menu-left")), right = gu_rep(&CZ.h_r, &CZ.t_r, fnv64("menu-right"));
    int back = menu_edge(fnv64("menu-back"), &CZ.h_back);
    int esc_now = game_focused() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0, esc = esc_now && !CZ.h_esc;
    CZ.h_esc = esc_now;
    int f_now = game_focused() && (GetAsyncKeyState('F') & 0x8000) != 0, sw = (f_now && !CZ.h_f) || menu_edge_pad("menu-action", &CZ.h_act);
    CZ.h_f = f_now;
    int kq = game_focused() && (GetAsyncKeyState('Q') & 0x8000) != 0, ke = game_focused() && (GetAsyncKeyState('E') & 0x8000) != 0;
    int tab_l = (kq && !CZ.h_q) || MENU_PREV_TAB(&CZ.h_pv), tab_r = (ke && !CZ.h_e) || MENU_NEXT_TAB(&CZ.h_nx);
    CZ.h_q = kq; CZ.h_e = ke;
    int wn = wheel_notches();
    cz_scan();
    int click = g_click_edge;
    // CHALLENGES: C on the keyboard, Y / Triangle (menu-inspect) on the pad, a click on the prompt
    int c_now = game_focused() && (GetAsyncKeyState('C') & 0x8000) != 0, x_now = 0;
    // (A: the pad X / Square is menu-action = SWITCH FACTION; CHALLENGES on the pad is menu-inspect, Y / Triangle - pad buttons read as menu_edge_pad)
    x_now = menu_edge_pad("menu-inspect", &CZ.h_x);
    int chal = (c_now && !CZ.h_c) || x_now;
    CZ.h_c = c_now;
    // ZOOM: Z on the keyboard, R3 on the pad (not with L3: that pair is the dev freecam), a click on the prompt
    int z_now = game_focused() && (GetAsyncKeyState('Z') & 0x8000) != 0, r3_now = 0;
    { WORD pb = 0; BYTE l, r; SHORT lx, ly; r3_now = pad_state(&pb, &l, &r, &lx, &ly) && (pb & 0x0080) && !(pb & 0x0040); }
    int zoom = (z_now && !CZ.h_z) || (r3_now && !CZ.h_r3);
    CZ.h_z = z_now; CZ.h_r3 = r3_now;
    // UNLOCKED ONLY: U on the keyboard, L3 on the pad (alone: L3 + R3 is the dev freecam, as ZOOM's R3), a click on the toggle
    int uk_now = game_focused() && (GetAsyncKeyState('U') & 0x8000) != 0, l3_now = 0;
    { WORD pb = 0; BYTE l, r; SHORT lx, ly; l3_now = pad_state(&pb, &l, &r, &lx, &ly) && (pb & 0x0040) && !(pb & 0x0080); }
    int uonly = (uk_now && !CZ.h_uk) || (l3_now && !CZ.h_l3);
    CZ.h_uk = uk_now; CZ.h_l3 = l3_now;
    if (g_czs_fc) {                                  // the stage freecam flies (customize_stage.c): no page input at all
        CZ.h_esc = 1;                                // (the Esc that ends the flight must not close the page on its release frame)
        goto fade;
    }
    if (g_czc_tip) {                                 // the first-visit tip is up: CONTINUE (select / a click) or BACK closes it, nothing else
        if (now - CZ.enter_ms > 400 && (sel || back || esc || (click && CZ.hv_tip))) czc_tip_done(back || esc ? "back" : click ? "CONTINUE clicked" : "select");
        if (!back && !esc) { sel = 0; click = 0; chal = 0; wn = 0; up = down = left = right = tab_l = tab_r = sw = uonly = 0; }
        if (!back && !esc) goto fade;
    }
    if (chal || (click && CZ.hv_ch)) { czc_open(chal ? (x_now ? "pad Y / Triangle" : "C") : "CHALLENGES clicked"); return; }
    {   // the stage light, live: [ darker, ] brighter (held: repeats)
        static uint64_t t_rep;
        const int dn = (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0, br = (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;
        if (game_focused() && (dn || br) && now >= t_rep) {
            t_rep = now + 110;
            czs_light_step(br ? 1 : -1);
            snprintf(CZ.msg, sizeof CZ.msg, "STAGE LIGHT %d%%", (int)(g_czs_light_k * 100.0f + 0.5f));
            CZ.msg_until = now + 2500; CZ.key = 0;
        }
    }
    {   // the lighting setups: L = the next one for the stage on screen (customize_stage.c CZS_CINE; 0 = the layout's own lights)
        static int was;
        const int dn = game_focused() && (GetAsyncKeyState('L') & 0x8000) != 0;
        if (dn && !was) {
            const char *nm = czs_cine_step();
            snprintf(CZ.msg, sizeof CZ.msg, "LIGHTING: %s  %d%%", nm, (int)(g_czs_light_k * 100.0f + 0.5f));
            CZ.msg_until = now + 3500; CZ.key = 0;
            xlog("customize: lighting setup -> %s (L)", nm);
        }
        was = dn;
    }
    if (g_cz_zoom) {
        float dz = 0.0f;
        if (wn && CZ.hv_card < 0) { dz += 0.12f * (float)wn; wn = 0; }
        { WORD pb = 0; BYTE l, r; SHORT lx, ly;
          if (pad_state(&pb, &l, &r, &lx, &ly)) { float y = (float)g_pad_rs[1] / 32767.0f; if (fabsf(y) > 0.25f) dz += y * 0.035f; } }
        if (dz != 0.0f) {
            g_cz_zoom_u += dz;
            if (g_cz_zoom_u < CZ_ZOOM_MIN) g_cz_zoom_u = CZ_ZOOM_MIN;
            if (g_cz_zoom_u > CZ_ZOOM_MAX) g_cz_zoom_u = CZ_ZOOM_MAX;
        }
    }
    if (zoom || (click && CZ.hv_zm)) {
        g_cz_zoom = !g_cz_zoom; click = 0;
        g_cz_zoom_u = 1.0f;
        ui_sfx("sfx-cycle");
        xlog("customize: zoom %s (%s, category %s)", g_cz_zoom ? "in" : "out", zoom ? (r3_now ? "pad R3" : "Z") : "ZOOM clicked", CZ_CAT_LABEL[CZ.cat]);
    }
    if (back || esc || (click && CZ.hv_back)) { cz_close(esc ? "Esc" : back ? "menu-back" : "BACK clicked"); }
    if (CZ.back_f && g_frame >= (uint64_t)CZ.back_f + 6) {
        CZ.back_f = 0;
        if (top_page() == cz_page_id() && !CZ.closing) { pop_page(); CZ.closing = 1; xlog("customize: closed"); return; }
    }
    if (CZ.quiet) return;
    if (uonly || (click && CZ.hv_uo)) {
        cz_set_uonly(!(g_cz.flags & CZF_UONLY), uonly ? (l3_now ? "pad L3" : "U") : "toggle clicked");
        click = 0;
    }
    const int n = cz_rows();
    int *foc = &CZ.foc[CZ.cat];
    if (up && *foc - CZ_COLS >= 0) { *foc -= CZ_COLS; ui_sfx("sfx-cycle"); }
    if (down && *foc + CZ_COLS < n) { *foc += CZ_COLS; ui_sfx("sfx-cycle"); }
    else if (down && *foc / CZ_COLS < (n - 1) / CZ_COLS) { *foc = n - 1; ui_sfx("sfx-cycle"); }      // (a short last row: down lands on its last card)
    if (left && *foc % CZ_COLS > 0) { (*foc)--; ui_sfx("sfx-cycle"); }
    if (right && *foc % CZ_COLS < CZ_COLS - 1 && *foc + 1 < n) { (*foc)++; ui_sfx("sfx-cycle"); }
    if (wn && n > CZ_NCARD) {                                    // the wheel scrolls the grid a row at a time (the focus follows into view)
        int o = CZ.off[CZ.cat] - wn * CZ_COLS, maxo = ((n - 1) / CZ_COLS - CZ_ROWS + 1) * CZ_COLS;
        if (o > maxo) o = maxo;
        if (o < 0) o = 0;
        if (o != CZ.off[CZ.cat]) {
            CZ.off[CZ.cat] = o;
            if (*foc < o) *foc = o;
            if (*foc >= o + CZ_NCARD) *foc = o + CZ_NCARD - 1;
        }
    }
    cz_keep_visible();
    if (tab_l) cz_set_cat(cz_tab_step(CZ.cat, -1), "previous");
    if (tab_r) cz_set_cat(cz_tab_step(CZ.cat, 1), "next");
    if (click) {
        if (CZ.hv_tab >= 0) cz_set_cat(CZ.hv_tab, "clicked");
        else if (CZ.hv_q) cz_set_cat(cz_tab_step(CZ.cat, -1), "Q clicked");
        else if (CZ.hv_e) cz_set_cat(cz_tab_step(CZ.cat, 1), "E clicked");
        else if (CZ.hv_card >= 0) { *foc = CZ.off[CZ.cat] + CZ.hv_card; cz_equip(*foc, "clicked"); }
        else if (CZ.hv_sel) sel = 1;
        else if (CZ.hv_sw || CZ.hv_b) sw = 1;
    }
    { static int lhv = -2; if (CZ.hv_card != lhv) { lhv = CZ.hv_card; if (CZ.hv_card >= 0 && g_pad_mode == 0) { *foc = CZ.off[CZ.cat] + CZ.hv_card; } } }   // hover = focus (mouse)
    if (sel && now - CZ.enter_ms > 300) cz_equip(CZ.foc[CZ.cat], "select");
    // (no second switch until the exchange on the stage is over, so there are no accidental double switches)
    if (sw && now - CZ.enter_ms > 300 && !(CZS.on && (CZS.swap_t0 || CZS.front != CZ.fac))) cz_set_fac(!CZ.fac, CZ.hv_b ? "back model clicked" : "switch");
    cz_fill();
    cz_deny_tick();
fade:
    {   // page fade-in: with the black lifting (an open through cz_open_start), else over 220 ms from the entry
        const uint64_t t0 = g_cz_show_at > CZ.enter_ms && g_cz_show_at - CZ.enter_ms < 3000 ? g_cz_show_at : CZ.enter_ms;
        float f = now <= t0 ? 0.0f : (float)(now - t0) / (t0 == CZ.enter_ms ? 220.0f : 320.0f);
        if (f > 1.0f) f = 1.0f;
        static int lq = -1; int q = (int)(f * 24.0f + 0.5f);
        if (now - CZ.enter_ms < 40) lq = -1;
        if (q != lq) { lq = q; w_alpha(ui_w("anim-group"), (float)q / 24.0f); }
    }
}
