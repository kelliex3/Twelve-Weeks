// Native owner for build-191 CUSTOMIZE | CHALLENGES. The grid / focus / equip
// rules live in challenges-core.hpp. All widget paths come from
// tools/features/customize_challenges_widgets.py; missing widgets are no-ops.

#include "customize-challenges.hpp"

#include "customize.hpp"
#include "customize-compose.hpp"
#include "customize-stage.hpp"

#include "boot.hpp"
#include "denynote.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "log.hpp"             // TO1_LOG must precede engine/script.hpp
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "sid/sid.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::customize::challenges {

    namespace {

        using namespace core;

        constexpr float kRuleW0     = 1000.0f;   // nr_style.RULE_W0
        constexpr float kPanelW0    = 100.0f;    // nr_style.CHIP_W0 / focus rule
        constexpr float kPBW0       = 400.0f;    // nr_style.PB_W0
        constexpr float kNR1        = 3700.0f;
        constexpr float kL0         = 148.0f;
        constexpr float kHeadPt     = 54.0f;
        constexpr float kTabPt      = 36.0f;
        constexpr float kTabY       = 270.0f;
        constexpr float kSecPt      = 36.0f;     // nr_style.pt(24)
        constexpr float kRightPt    = 33.0f;
        constexpr float kPromptPt   = 36.0f;
        constexpr float kPromptKeyPt = 24.0f;
        constexpr float kPromptY    = 2000.0f;
        constexpr float kPromptH    = 88.0f;
        constexpr float kChipPt     = 30.0f;

        constexpr char kTab[CZ_NFAC][12] = { "WLF", "SERAPHITES" };

        struct PromptItem {
            const char* key;
            const char* label;
            bool on;
        };

        struct Runtime {
            State state;
            bool entered = false;
            uint64_t backFrame = 0;
            uint64_t key = 0;
            DenyFeedback deny;
            float pbX0[3]{}, pbX1[3]{};
            int pbN = 0;
            int lastHoverR = -2, lastHoverC = -2;
            bool hUp = true, hDown = true, hLeft = true, hRight = true;
            bool hSel = true, hBack = true, hEsc = true;
            bool hPrev = true, hNext = true, hQ = true, hE = true;
            uint64_t tUp = ~uint64_t{ 0 }, tDown = ~uint64_t{ 0 };
            uint64_t tLeft = ~uint64_t{ 0 }, tRight = ~uint64_t{ 0 };
        } g;

        bool Disabled() { return boot::ExtLoaded() || engine::flag::Present("t2r-no-customize.txt"); }
        uint64_t Page() { return engine::widget::PageId("customize-challenges"); }

        void ResetHeld()
        {
            g.hUp = g.hDown = g.hLeft = g.hRight = true;
            g.hSel = g.hBack = g.hEsc = g.hPrev = g.hNext = true;
            g.hQ = g.hE = true;
            g.tUp = g.tDown = g.tLeft = g.tRight = ~uint64_t{ 0 };
            g.lastHoverR = g.lastHoverC = -2;
        }

        // ---- small nrui.c helpers (using the shared CUSTOMIZE widget seam) ---------

        void Header()
        {
            const char* parent = engine::locale::Tr("CUSTOMIZE");
            const char* page = engine::locale::Tr("CHALLENGES");
            ui::RawText("anim-group.hdPar", parent);
            const float x = kL0 + ui::TextWidth(parent, kHeadPt) + 52.0f;
            ui::Pivot("anim-group.hdDiv", x, 168.0f - 34.0f);
            ui::Pivot("anim-group.hdPage", x + 52.0f, 168.0f);
            ui::RawText("anim-group.hdPage", page);
            const float x2 = x + 52.0f + ui::TextWidth(page, kHeadPt) + 52.0f;
            ui::Rule("anim-group.hdRule", x2, 168.0f, kNR1, kRuleW0);
        }

        void Tabs(int fac, int hover)
        {
            float x = kL0 + 108.0f, ax = x, aw = 100.0f;
            char path[64], buf[128];
            for (int k = 0; k < 8; ++k) {
                std::snprintf(path, sizeof path, "anim-group.tabs.tb%d", k);
                ui::Vis(path, k < CZ_NFAC);
                std::snprintf(path, sizeof path, "anim-group.tabs.tbHit%d", k);
                ui::Vis(path, k < CZ_NFAC);
                if (k >= CZ_NFAC) continue;
                const char* lb = engine::locale::Tr(kTab[k]);
                const float w = ui::TextWidth(lb, kTabPt) + 8.0f;
                if (k == fac || k == hover)
                    std::snprintf(buf, sizeof buf, "%s", lb);
                else
                    std::snprintf(buf, sizeof buf, "<font color=\"#8C8C8C\">%s</font>", lb);
                std::snprintf(path, sizeof path, "anim-group.tabs.tb%d", k);
                ui::RawText(path, buf);
                ui::Pivot(path, x, kTabY);
                std::snprintf(path, sizeof path, "anim-group.tabs.tbHit%d", k);
                ui::Pivot(path, x - 30.0f, kTabY - 60.0f);
                if (k == fac) { ax = x; aw = w; }
                x += w + 80.0f;
            }
            const float ex = x - 80.0f + 28.0f;
            ui::Pivot("anim-group.tabs.tbE", ex + 30.0f, kTabY);
            ui::Pivot("anim-group.tabs.tbEHit", ex + 30.0f, kTabY);
            ui::Pivot("anim-group.tabs.tbU", ax, kTabY + 38.0f);
            ui::Scale("anim-group.tabs.tbU", aw / kPanelW0, 1.0f);
        }

        // The challenge line: translated, sentence case, map names keep their
        // capitals, and a full stop (nrui.c czc_line).
        void Lower(char* s)
        {
            for (unsigned char* q = reinterpret_cast<unsigned char*>(s); *q;) {
                if (*q >= 'A' && *q <= 'Z') { *q = static_cast<unsigned char>(*q + 32); ++q; continue; }
                if (q[1] && ((q[0] == 0xC4 && (q[1] == 0x84 || q[1] == 0x86 || q[1] == 0x98))
                    || (q[0] == 0xC5 && (q[1] == 0x81 || q[1] == 0x83 || q[1] == 0x9A || q[1] == 0xB9 || q[1] == 0xBB)))) {
                    ++q[1]; q += 2; continue;
                }
                if (q[0] == 0xC3 && q[1] == 0x93) { q[1] = 0xB3; q += 2; continue; }
                ++q;
            }
        }

        void TitleCase(char* s, size_t n)
        {
            for (size_t i = 0; i < n; ++i)
                if ((i == 0 || s[i - 1] == ' ') && s[i] >= 'a' && s[i] <= 'z')
                    s[i] = static_cast<char>(s[i] - 32);
        }

        void ChallengeLine(const cz_item_t* it, char* out, int cap)
        {
            out[0] = 0;
            if (!it || !it->uk || it->uk >= CZ_NCHAL) return;
            const cz_chal_t& c = CZ_CHAL[it->uk];
            const char* map = c.kind == CZK_MAPWINS && it->um < CZ_NUMAP
                            ? CZ_UMAP_NAME[it->um] : nullptr;
            char t[160];
            if (c.kind == CZK_SAFES && it->un == 1)
                std::snprintf(t, sizeof t, "CRACK A SAFE IN INTERROGATION");
            else if (map && it->un == 1)
                std::snprintf(t, sizeof t, "WIN A MATCH ON %s", map);
            else if (c.kind == CZK_WEEKS && it->un == 1)
                std::snprintf(t, sizeof t, "SURVIVE A WEEK");
            else if (c.kind == CZK_FLAWLESS && it->un == 1)
                std::snprintf(t, sizeof t, "WIN A MATCH WITHOUT DYING");
            else if (map)
                std::snprintf(t, sizeof t, c.line, static_cast<unsigned>(it->un), map);
            else
                std::snprintf(t, sizeof t, c.line, static_cast<unsigned>(it->un));
            std::snprintf(out, static_cast<size_t>(cap), "%s", engine::locale::Tr(t));
            Lower(out);
            if (out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 32);
            if (map) {
                char m[64];
                std::snprintf(m, sizeof m, "%s", map);
                Lower(m);
                char* p = std::strstr(out, m);
                if (p) TitleCase(p, std::strlen(m));
            }
            const size_t n = std::strlen(out);
            if (n && n + 1 < static_cast<size_t>(cap) && out[n - 1] != '.') {
                out[n] = '.';
                out[n + 1] = 0;
            }
        }

        void ChipText(int cat, const cz_item_t* it, char* out, int cap)
        {
            const char* lb = it ? it->label : "";
            const char* cl = CZ_CAT_LABEL[cat];
            char w1[32];
            int n = 0;
            while (lb[n] && lb[n] != ' ' && n < 31) { w1[n] = lb[n]; ++n; }
            w1[n] = 0;
            int same = 0;
            for (const char* q = cl; *q && !same;) {
                const char* e = std::strchr(q, ' ');
                const size_t l = e ? static_cast<size_t>(e - q) : std::strlen(q);
                if (l == std::strlen(w1) && std::strncmp(q, w1, l) == 0) same = 1;
                q = e ? e + 1 : q + l;
            }
            char a[64];
            std::snprintf(a, sizeof a, "%s", engine::locale::Tr(cl));
            if (same)
                std::snprintf(out, static_cast<size_t>(cap), "%s", engine::locale::Tr(lb));
            else
                std::snprintf(out, static_cast<size_t>(cap), "%s - %s", a, engine::locale::Tr(lb));
        }

        void Chip(const char* base, const char* text, float x)
        {
            char path[96];
            const float w = ui::TextWidth(text, kChipPt) + 32.0f;
            std::snprintf(path, sizeof path, "%sTx", base);
            ui::RawText(path, text);
            float px = 0.0f, py = 0.0f;
            const uint64_t tw = ui::W(path);
            if (tw && engine::widget::PivotGet(tw, &px, &py))
                ui::Pivot(path, x + 16.0f, py);
            std::snprintf(path, sizeof path, "%sBg", base);
            const uint64_t bw = ui::W(path);
            if (bw && engine::widget::PivotGet(bw, &px, &py))
                ui::Pivot(path, x, py);
            ui::Scale(path, w / kPanelW0, 1.0f);
        }

        void HorizontalScroll(const char* base, float x0, float x1, float y,
                              int first, int shown, int n)
        {
            char path[96];
            std::snprintf(path, sizeof path, "%sT", base);
            ui::Vis(path, n > shown);
            std::snprintf(path, sizeof path, "%sH", base);
            ui::Vis(path, n > shown);
            if (n <= shown) return;
            const float a = static_cast<float>(first) / static_cast<float>(n);
            float w = (x1 - x0) * static_cast<float>(shown) / static_cast<float>(n);
            if (w < 8.0f) w = 8.0f;
            std::snprintf(path, sizeof path, "%sH", base);
            ui::Pivot(path, x0 + (x1 - x0) * a, y);
            ui::Scale(path, w / kRuleW0, 1.0f);
        }

        void FrameAt(const char* base, float x, float y, float w, float h, bool on)
        {
            static const char S[4] = { 'T', 'B', 'L', 'R' };
            char path[96];
            for (int k = 0; k < 4; ++k) {
                std::snprintf(path, sizeof path, "%s%c", base, S[k]);
                ui::Vis(path, on);
                if (!on) continue;
                if (k == 0) {
                    ui::Pivot(path, x, y + 2.0f);
                    ui::Scale(path, w / kPanelW0, 1.0f);
                } else if (k == 1) {
                    ui::Pivot(path, x, y + h - 2.0f);
                    ui::Scale(path, w / kPanelW0, 1.0f);
                } else {
                    ui::Pivot(path, k == 2 ? x : x + w - 4.0f, y);
                    ui::Scale(path, 1.0f, h / kPanelW0);
                }
            }
        }

        void VerticalScroll(float first, int shown, int n)
        {
            const float y0 = kRowY0 - 20.0f;
            const float y1 = kRowY0 + (kScreenRows - 1) * kRowPitch
                           + kTileDY + kBarDY + 8.0f;
            const bool on = n > shown && n > 0;
            ui::Vis("anim-group.rows.sbVT", on);
            ui::Vis("anim-group.rows.sbVH", on);
            if (!on) return;
            const float a = first / static_cast<float>(n);
            const float b = static_cast<float>(first + shown) / static_cast<float>(n);
            float len = (y1 - y0) * (b - a);
            if (len < 8.0f) len = 8.0f;
            ui::Pivot("anim-group.rows.sbVH", kSbX - 2.0f, y0 + (y1 - y0) * a);
            ui::Scale("anim-group.rows.sbVH", 1.0f, len / (y1 - y0));
        }

        // nr_prompts(): declared left to right, drawn right to left. The caller's
        // prompt index is the declaration index (BACK=0, EQUIP=1, SWITCH=2).
        void Prompts(const PromptItem* items, int n)
        {
            float x = kNR1;
            g.pbN = std::min(n, 3);
            char path[96], k1[48], k2[48];
            for (int k = 0; k < 3; ++k) {
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d", k);
                ui::Vis(path, k < g.pbN);
                if (k >= g.pbN) continue;
                const PromptItem& q = items[g.pbN - 1 - k];
                const char* key = q.key ? q.key : "";
                k1[0] = k2[0] = 0;
                const bool glyph = std::strncmp(key, "menu:", 5) == 0;
                if (glyph) {
                    const char* plus = std::strchr(key, '+');
                    if (plus) {
                        std::snprintf(k1, sizeof k1, "%.*s", static_cast<int>(plus - key), key);
                        std::snprintf(k2, sizeof k2, "%s", plus + 1);
                    } else {
                        std::snprintf(k1, sizeof k1, "%s", key);
                    }
                }
                float kw = glyph ? (k2[0] ? 136.0f : 64.0f)
                                 : std::max(60.0f, ui::TextWidth(key, kPromptKeyPt) + 28.0f);
                (void)kw;
                const float lw = ui::TextWidth(q.label, kPromptPt);
                const float tot = 18.0f + kw + 22.0f + lw + 26.0f;
                g.pbX0[k] = x - tot;
                g.pbX1[k] = x;
                ui::Pivot(path, x - tot, kPromptY - kPromptH * 0.5f);
                const float a = q.on ? 70.0f / 255.0f : 30.0f / 255.0f;
                static const char S4[4] = { 'T', 'B', 'L', 'R' };
                for (int s = 0; s < 4; ++s) {
                    std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%d%c", k, k, S4[s]);
                    ui::Vis(path, true);
                    ui::Alpha(path, a);
                    if (s < 2) ui::Scale(path, tot / kPBW0, 1.0f);
                    if (s == 3) ui::Pivot(path, tot - 4.0f, 0.0f);
                }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKg", k, k);
                ui::Vis(path, glyph);
                if (glyph) { ui::Image(path, k1); ui::Alpha(path, q.on ? 1.0f : 0.4f); }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKg2", k, k);
                ui::Vis(path, glyph && k2[0]);
                if (glyph && k2[0]) { ui::Image(path, k2); ui::Alpha(path, q.on ? 1.0f : 0.4f); }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKb", k, k);
                ui::Vis(path, !glyph);
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dKt", k, k);
                ui::Vis(path, !glyph);
                if (!glyph) {
                    ui::RawText(path, key);
                    ui::Pivot(path, 18.0f + kw * 0.5f - 50.0f, 16.0f);
                }
                std::snprintf(path, sizeof path, "anim-group.pbar.pb%d.pb%dTx", k, k);
                char lb[128];
                const char* label = engine::locale::Tr(q.label ? q.label : "");
                if (q.on)
                    std::snprintf(lb, sizeof lb, "%s", label);
                else
                    std::snprintf(lb, sizeof lb, "<font color=\"#585858\">%s</font>", label);
                ui::RawText(path, lb);
                ui::Pivot(path, 18.0f + kw + 22.0f, kPromptH * 0.5f);
                x -= tot + 28.0f;
            }
        }

        int PromptHover(float cx, float cy)
        {
            if (engine::menu_input::PadMode()) return -1;
            if (cy < kPromptY - kPromptH * 0.5f - 6.0f
                || cy > kPromptY + kPromptH * 0.5f + 6.0f) return -1;
            for (int k = 0; k < g.pbN; ++k)
                if (cx >= g.pbX0[k] && cx <= g.pbX1[k]) return g.pbN - 1 - k;
            return -1;
        }

        // ---- page fill -------------------------------------------------------------

        void Draw(uint64_t now)
        {
            State& s = g.state;
            Profile& p = customize::GetProfile();
            const int fac = s.fac, sex = Sex(p, fac);
            uint64_t key = static_cast<uint64_t>(fac)
                | static_cast<uint64_t>(s.fr) << 2
                | static_cast<uint64_t>(s.fc & 0xff) << 6
                | static_cast<uint64_t>(s.row0) << 14
                | static_cast<uint64_t>(s.hvTab + 1) << 18
                | static_cast<uint64_t>((s.hvPb + 1) & 7) << 20
                | static_cast<uint64_t>(engine::menu_input::PadMode()) << 23
                | static_cast<uint64_t>(s.hvQ | (s.hvE << 1)) << 24
                | static_cast<uint64_t>(customize::ProgressGeneration() & 0x7fu) << 56;
            for (int r = 0; r < kWearCats; ++r)
                key ^= static_cast<uint64_t>(s.off[r] & 0x3f) << (28 + 4 * r % 24);
            for (int c = 0; c < kWearCats; ++c)
                key ^= static_cast<uint64_t>(p.it[fac][sex][c]) * (0x9E3779B97F4A7C15ULL + c);
            key |= uint64_t{ 1 } << 63;
            if (key == g.key) return;
            g.key = key;

            Header();
            Tabs(fac, s.hvTab);
            {
                int done = 0, total = 0;
                Count(p, fac, false, customize::StoryMasksShown(), done, total);
                char buf[96];
                std::snprintf(buf, sizeof buf, "%s %d | %d",
                              engine::locale::Tr("COMPLETED"), done, total);
                ui::Text("anim-group.chTot", buf);
            }

            for (int r = 0; r < kScreenRows; ++r) {
                const int ri = s.row0 + r;
                const bool on = RowOk(s, ri);
                char path[96];
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dTx", r);
                ui::Vis(path, on);
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dRt", r);
                ui::Vis(path, on);
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dRl", r);
                ui::Vis(path, on);
                if (!on) {
                    for (int c = 0; c < kScreenCols; ++c) {
                        std::snprintf(path, sizeof path, "anim-group.rows.t%d_%d", r, c);
                        ui::Vis(path, false);
                        std::snprintf(path, sizeof path, "anim-group.rows.t%d_%dBT", r, c);
                        ui::Vis(path, false);
                        std::snprintf(path, sizeof path, "anim-group.rows.t%d_%dBF", r, c);
                        ui::Vis(path, false);
                    }
                    continue;
                }
                const int cat = s.grid.rcat[ri], n = s.grid.rn[ri], off = s.off[ri];
                char right[32];
                int done = 0;
                for (int i = 0; i < n; ++i)
                    if (Unlocked(p, Item(fac, sex, cat, s.grid.rk[ri][i]))) ++done;
                std::snprintf(right, sizeof right, "%d | %d", done, n);
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dTx", r);
                ui::Text(path, CZ_CAT_LABEL[cat]);
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dRt", r);
                ui::RawText(path, right);
                std::snprintf(path, sizeof path, "anim-group.rows.rw%dRl", r);
                ui::Rule(path, kX0 + ui::TextWidth(CZ_CAT_LABEL[cat], kSecPt) + 40.0f,
                         kRowY0 + r * kRowPitch,
                         kX1 - ui::TextWidth(right, kRightPt) - 40.0f, kRuleW0);

                for (int c = 0; c < kScreenCols; ++c) {
                    const int k = off + c;
                    const bool ton = k < n;
                    std::snprintf(path, sizeof path, "anim-group.rows.t%d_%d", r, c);
                    ui::Vis(path, ton);
                    char base[96], child[128];
                    std::snprintf(base, sizeof base, "anim-group.rows.t%d_%d", r, c);
                    std::snprintf(path, sizeof path, "%sBT", base);
                    ui::Vis(path, ton);
                    std::snprintf(path, sizeof path, "%sBF", base);
                    ui::Vis(path, ton);
                    if (!ton) continue;
                    const int ki = s.grid.rk[ri][k];
                    const cz_item_t* it = Item(fac, sex, cat, ki);
                    if (!it) continue;
                    const bool lk = !Unlocked(p, it);
                    const bool foc = ri == s.fr && k == s.fc;
                    float x, y;
                    TileXY(r, c, x, y);
                    const uint32_t pr = Progress(p, it);
                    const float frac = it->un
                        ? static_cast<float>(std::min(pr, static_cast<uint32_t>(it->un)))
                          / static_cast<float>(it->un) : 1.0f;
                    std::snprintf(path, sizeof path, "%sBF", base);
                    ui::Bar(path, x, x + kTileW, y + kBarDY + 2.0f, frac);
                    ui::Alpha(path, foc ? 1.0f : 150.0f / 255.0f);
                    if (TileChildPath(child, sizeof child, r, c, "P")) {
                        std::snprintf(path, sizeof path, "%s", child);
                        ui::Vis(path, it->pic != nullptr);
                        if (it->pic) {
                            char pic[192];
                            const char* leaf = lk && std::strlen(it->pic) > 4 ? it->pic + 4 : it->pic;
                            std::snprintf(pic, sizeof pic, lk ? "streaming/t2r/icons/czg-%s.png"
                                                              : "streaming/t2r/icons/%s.png", leaf);
                            ui::Image(path, pic);
                            ui::Alpha(path, foc ? 1.0f : lk ? 200.0f / 255.0f : 150.0f / 255.0f);
                        }
                    }
                    if (TileChildPath(child, sizeof child, r, c, "I")) {
                        std::snprintf(path, sizeof path, "%s", child);
                        ui::Vis(path, it->pic == nullptr);
                        if (!it->pic) {
                            const char* ic = cat == CZ_MELEE
                                ? (std::strstr(it->label, "KNIFE") ? "cz-knife" : "cz-fists")
                                : kCatIcon[cat];
                            char pic[160];
                            std::snprintf(pic, sizeof pic, "streaming/t2r/icons/%s.png", ic);
                            ui::Image(path, pic);
                            ui::Alpha(path, foc ? 1.0f : lk ? 90.0f / 255.0f : 150.0f / 255.0f);
                        }
                    }
                    if (TileChildPath(child, sizeof child, r, c, "L")) {
                        std::snprintf(path, sizeof path, "%s", child);
                        ui::Vis(path, lk);
                        ui::Alpha(path, foc ? 1.0f : 170.0f / 255.0f);
                    }
                    if (TileChildPath(child, sizeof child, r, c, "Bg")) {
                        std::snprintf(path, sizeof path, "%s", child);
                        ui::Alpha(path, (foc ? 215.0f : 175.0f) / 255.0f);
                    }
                    if (TileChildPath(child, sizeof child, r, c, "N")) {
                        std::snprintf(path, sizeof path, "%s", child);
                        ui::Vis(path, false);     // NEW dots are not tracked by the native port
                    }
                }
                char rs[96];
                std::snprintf(rs, sizeof rs, "anim-group.rows.rs%d", r);
                HorizontalScroll(rs, kX0, kX1,
                    kRowY0 + r * kRowPitch + kTileDY + kBarDY + 30.0f + 1.0f,
                    off, kScreenCols, n);
            }

            bool focusOn = false;
            float fx = 0.0f, fy = 0.0f;
            if (RowOk(s, s.fr)) {
                const int fr = s.fr - s.row0;
                const int fc = s.fc - s.off[s.fr];
                focusOn = fr >= 0 && fr < kScreenRows && fc >= 0 && fc < kScreenCols;
                if (focusOn) TileXY(fr, fc, fx, fy);
            }
            FrameAt("anim-group.rows.foc", fx - 6.0f, fy - 6.0f,
                    kTileW + 12.0f, kTileH + 12.0f, focusOn);
            VerticalScroll(static_cast<float>(s.row0), kScreenRows, s.grid.nrow);

            bool eqOn = false;
            ui::Vis("anim-group.det", RowOk(s, s.fr));
            if (RowOk(s, s.fr)) {
                const int cat = s.grid.rcat[s.fr], ki = s.grid.rk[s.fr][s.fc];
                const cz_item_t* it = Item(fac, sex, cat, ki);
                if (!it) return;
                const bool lk = !Unlocked(p, it);
                const bool worn = p.it[fac][sex][cat] == ki;
                ui::Vis("anim-group.det.dArt", it->pic != nullptr);
                ui::Vis("anim-group.det.dIc", it->pic == nullptr);
                if (it->pic) {
                    char pic[160];
                    std::snprintf(pic, sizeof pic, "streaming/t2r/icons/%s.png", it->pic);
                    ui::Image("anim-group.det.dArt", pic);
                    ui::Alpha("anim-group.det.dArt", lk ? 150.0f / 255.0f : 1.0f);
                } else {
                    const char* ic = cat == CZ_MELEE
                        ? (std::strstr(it->label, "KNIFE") ? "cz-knife" : "cz-fists")
                        : kCatIcon[cat];
                    char pic[160];
                    std::snprintf(pic, sizeof pic, "streaming/t2r/icons/%s.png", ic);
                    ui::Image("anim-group.det.dIc", pic);
                }
                ui::Vis("anim-group.det.dLock", false);
                ui::Fit("anim-group.det.dTitle",
                        engine::locale::Tr(CZ_CHAL[it->uk].title),
                        kDX1 - kDX0, kHeadPt);
                char line[192];
                ChallengeLine(it, line, static_cast<int>(sizeof line));
                ui::Text("anim-group.det.dDesc", line);
                uint32_t pr = Progress(p, it);
                if (pr > it->un) pr = it->un;
                ui::Bar("anim-group.det.dBarF", kDX0, kDX1 - 260.0f,
                        kDetailTitleY + 210.0f + 3.0f,
                        it->un ? static_cast<float>(pr) / static_cast<float>(it->un) : 1.0f);
                char buf[64];
                std::snprintf(buf, sizeof buf, "%u | %u", static_cast<unsigned>(pr),
                              static_cast<unsigned>(it->un));
                ui::Text("anim-group.det.dCnt", buf);
                ui::Text("anim-group.det.dUnl", "Unlocks:");
                ChipText(cat, it, buf, static_cast<int>(sizeof buf));
                const float cx0 = kDX0 + ui::TextWidth("Unlocks:", 33.0f) + 30.0f;
                Chip("anim-group.det.dChip", buf, cx0);
                Chip("anim-group.det.dChipA", buf, cx0);
                ui::Vis("anim-group.det.dChipBg", lk);
                ui::Vis("anim-group.det.dChipTx", lk);
                ui::Vis("anim-group.det.dChipABg", !lk);
                ui::Vis("anim-group.det.dChipATx", !lk);
                const char* state = lk ? "LOCKED" : worn ? "EQUIPPED"
                    : it->tier > 1 ? "NOT IN MULTIPLAYER YET" : "UNLOCKED";
                ui::Text("anim-group.det.dState", state);
                eqOn = !lk && !worn && it->tier <= 1;
            }
            const PromptItem prompts[3] = {
                { "menu:prev+menu:next", "SWITCH FACTION", true },
                { "menu:select", "EQUIP", eqOn },
                { "menu:back", "BACK", true },
            };
            Prompts(prompts, 3);
            (void)now;
        }

        void Scan(float cx, float cy, bool haveCursor)
        {
            State& s = g.state;
            core::ScanHover(s, cx, cy, engine::menu_input::PadMode(), haveCursor);
            s.hvTab = -1;
            s.hvQ = s.hvE = 0;
            for (int k = 0; k < CZ_NFAC; ++k) {
                char path[64];
                std::snprintf(path, sizeof path, "anim-group.tabs.tbHit%d", k);
                const uint64_t w = ui::W(path);
                if (w && engine::widget::MouseOver(w, true)) s.hvTab = k;
            }
            const uint64_t q = ui::W("anim-group.tabs.tbQHit");
            const uint64_t e = ui::W("anim-group.tabs.tbEHit");
            s.hvQ = q && engine::widget::MouseOver(q, true) ? 1 : 0;
            s.hvE = e && engine::widget::MouseOver(e, true) ? 1 : 0;
            s.hvPb = PromptHover(cx, cy);
        }

        // ext ui_deny(): one guarded menu-error sound, plus an immediate notice
        // when the refusal has text. denynote::Show() falls back to notify::Push
        // on pages without the styled denial widgets (including this one).
        void Deny(const EquipResult& r, uint64_t now)
        {
            if (g.deny.AllowSound(now)) ui::Sfx("sfx-menu-error");
            if (r.denyText && r.denyText[0])
                (void)net::denynote::Show(engine::locale::Tr(r.denyText), 3.0f);
        }

        void Equip(const char* why, uint64_t now)
        {
            State& s = g.state;
            if (!RowOk(s, s.fr)) return;
            Profile& p = customize::GetProfile();
            const int fac = s.fac, sex = Sex(p, fac);
            const int cat = s.grid.rcat[s.fr], k = s.grid.rk[s.fr][s.fc];
            const EquipResult r = CanEquip(s, p, fac, cat, k);
            if (r.denied) {
                Deny(r, now);
                return;
            }
            if (!r.changed) return;
            p.it[fac][sex][cat] = static_cast<uint8_t>(k);
            customize::Changed();
            const uint8_t counter = customize::ConsumeNetChange();
            if (!compose::WearOff()) {
                uint8_t body[kMsgLen];
                if (Msg(p, counter, body) == kMsgLen)
                    net::session::SendEventRaw(static_cast<uint8_t>(NEVT_CUSTOM_LOOK),
                                               body, static_cast<uint8_t>(kMsgLen));
            }
            ui::Sfx("sfx-select");
            g.key = 0;
            TO1_LOG("customize: %s %s %s = %s (challenges page, %s)\n",
                    kFactionName[fac], CZ_SEX_KEY[sex], CZ_CAT_LABEL[cat],
                    Item(fac, sex, cat, k)->label, why);
            (void)now;
        }

        void Close(const char* how, uint64_t top)
        {
            if (g.state.closing || g.state.quiet) return;
            ui::Alpha("anim-group", 1.0f);
            g.state.quiet = true;
            g.state.key = 0;
            const uint64_t frame = engine::frame::Count();
            g.backFrame = frame ? frame : 1;
            if (std::strcmp(how, "menu-back") != 0 && top == Page()) {
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
                g.state.closing = true;
                g.backFrame = 0;
            }
            TO1_LOG("customize-challenges: back (%s)\n", how);
        }

    } // namespace

    bool Active() { return !Disabled() && g.entered; }

    void Tick(uint64_t now)
    {
        if (Disabled()) {
            g.entered = false;
            g.backFrame = 0;
            return;
        }
        const uint64_t top = engine::widget::TopPage();
        if (!top || top != Page()) {
            g.entered = false;
            g.backFrame = 0;
            return;
        }
        State& s = g.state;
        if (!g.entered) {
            Profile& p = customize::GetProfile();
            Enter(s, p, customize::EditingFaction(), customize::StoryMasksShown(),
                  customize::EditingCategory(), now);
            g.entered = true;
            g.key = 0;
            g.backFrame = 0;
            ui::Alpha("anim-group", 0.0f);
            ResetHeld();
            TO1_LOG("customize-challenges: page up (%s, %d rows)\n",
                    kFactionName[s.fac], s.grid.nrow);
        }
        if (s.closing) return;

        const bool selRaw = engine::menu_input::CommandEdge(SID("menu-select"), g.hSel);
        const bool up = engine::menu_input::CommandRepeat(SID("menu-up"), g.hUp, g.tUp, now);
        const bool down = engine::menu_input::CommandRepeat(SID("menu-down"), g.hDown, g.tDown, now);
        const bool left = engine::menu_input::CommandRepeat(SID("menu-left"), g.hLeft, g.tLeft, now);
        const bool right = engine::menu_input::CommandRepeat(SID("menu-right"), g.hRight, g.tRight, now);
        const bool back = engine::menu_input::CommandEdge(SID("menu-back"), g.hBack);
        const bool escNow = engine::window::KeyDown(VK_ESCAPE);
        const bool esc = escNow && !g.hEsc;
        g.hEsc = escNow;
        const bool qNow = engine::window::KeyDown('Q'), eNow = engine::window::KeyDown('E');
        const bool tabL = (qNow && !g.hQ) || engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
        const bool tabR = (eNow && !g.hE) || engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);
        g.hQ = qNow;
        g.hE = eNow;
        const int wheel = engine::menu_input::WheelNotches();
        float cx = 0.0f, cy = 0.0f;
        const bool haveCursor = engine::window::CursorCanvas(&cx, &cy);
        Scan(cx, cy, haveCursor);
        const bool click = engine::menu_input::ClickEdge();
        // Stage freecam owns all input; its Esc must not also pop this page.
        if (stage::FreecamActive()) {
            g.hEsc = true;
            return;
        }
        // The ext's shared click gate drops a mouse-derived menu-select on empty
        // space; explicit click edges on tiles and prompts remain the mouse path.
        const bool mouseSelect = engine::menu_input::SelectIsMouse();
        const bool sel = selRaw && !(mouseSelect && !core::ClickHit(s));

        if (back || esc || (click && s.hvPb == kPromptBack))
            Close(esc ? "Esc" : back ? "menu-back" : "BACK clicked", top);
        if (g.backFrame && engine::frame::Count() >= g.backFrame + kBackFrames) {
            g.backFrame = 0;
            if (engine::widget::TopPage() == Page() && !s.closing) {
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
                s.closing = true;
                TO1_LOG("customize-challenges: closed\n");
                return;
            }
        }
        if (s.quiet) return;

        if (tabL || (click && s.hvQ))
            SetFac(s, customize::GetProfile(), s.fac - 1, customize::StoryMasksShown());
        else if (tabR || (click && s.hvE) || (click && s.hvPb == kPromptSwitch))
            SetFac(s, customize::GetProfile(), s.fac + 1, customize::StoryMasksShown());
        else if (click && s.hvTab >= 0)
            SetFac(s, customize::GetProfile(), s.hvTab, customize::StoryMasksShown());

        if (RowOk(s, s.fr)) {
            if (up) { MoveRow(s, -1); ui::Sfx("sfx-cycle"); }
            if (down) { MoveRow(s, 1); ui::Sfx("sfx-cycle"); }
            if (left && s.fc > 0) { --s.fc; ui::Sfx("sfx-cycle"); }
            if (right && s.fc + 1 < s.grid.rn[s.fr]) { ++s.fc; ui::Sfx("sfx-cycle"); }
            if (left || right) {
                KeepVisible(s);
                s.want = s.fc - s.off[s.fr];
            }
            if (wheel) {
                if (s.grid.nrow > kScreenRows) {
                    ScrollRows(s, wheel);
                } else if (s.hvR >= 0 && s.grid.rn[s.hvR] > kScreenCols) {
                    ScrollCols(s, s.hvR, wheel);
                }
            }
            KeepVisible(s);
            if ((s.hvR != g.lastHoverR || s.hvC != g.lastHoverC)
                && s.hvR >= 0 && !engine::menu_input::PadMode()) {
                s.fr = s.hvR;
                s.fc = s.hvC;
                s.want = s.fc - s.off[s.fr];
            }
            g.lastHoverR = s.hvR;
            g.lastHoverC = s.hvC;
            if (click && s.hvR >= 0)
                Equip("clicked", now);
            else if ((sel || (click && s.hvPb == kPromptEquip))
                     && now - s.enterMs > kEquipGateMs)
                Equip(sel ? "select" : "EQUIP clicked", now);
        }

        Draw(now);
        const int q = FadeStep(s, now);
        if (now - s.enterMs < 40) s.key = 0;
        ui::Alpha("anim-group", static_cast<float>(q) / 24.0f);
    }

    void Reset()
    {
        g = Runtime{};
    }

} // namespace net::ui::customize::challenges
