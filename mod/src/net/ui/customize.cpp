// Native owner for build-191 CUSTOMIZE (tools/ext/customize.c). Profile/file/wire
// rules live in customize-core.hpp; this owner drives page state, the shared
// in-memory profile, the atomic save and NEVT_CUSTOM_LOOK. It yields while
// t2r-ext.dll is loaded -- the ext path is deprecated, not a dependency.

#include "customize.hpp"
#include "challenges-core.hpp"
#include "customize-compose.hpp"
#include "customize-stage.hpp"

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "log.hpp"             // TO1_LOG must precede engine/script.hpp
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "menu/loadseq.hpp"
#include "net/match/match.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "sid/sid.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::customize {

    namespace ui {

        uint64_t W(const char* rel) { return engine::widget::Page(rel); }

        void Vis(const char* rel, bool on)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Visible(w, on);
        }

        void Alpha(const char* rel, float a)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Alpha(w, a);
        }

        void Text(const char* rel, const char* text)
        {
            RawText(rel, engine::locale::Tr(text ? text : ""));
        }

        void RawText(const char* rel, const char* text)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::SetText(w, text ? text : "");
        }

        void Image(const char* rel, const char* path)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::SetImage(w, path ? path : "");
        }

        void Pivot(const char* rel, float x, float y)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Pivot(w, x, y);
        }

        void Scale(const char* rel, float sx, float sy)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Scale(w, sx, sy);
        }

        void Rule(const char* rel, float x0, float y, float x1, float baseWidth)
        {
            const uint64_t w = W(rel);
            if (!w) return;
            if (x0 > x1 - 40.0f) x0 = x1 - 40.0f;
            engine::widget::Pivot(w, x0, y);
            engine::widget::Scale(w, (x1 - x0) / baseWidth, 1.0f);
        }

        void Bar(const char* rel, float x0, float x1, float y, float frac)
        {
            const uint64_t w = W(rel);
            if (!w) return;
            frac = std::clamp(frac, 0.0f, 1.0f);
            engine::widget::Visible(w, frac > 0.004f);
            if (frac <= 0.004f) return;
            engine::widget::Pivot(w, x0, y);
            engine::widget::Scale(w, (x1 - x0) * frac / 1000.0f, 1.0f);
        }

        void Fit(const char* rel, const char* text, float box, float pt)
        {
            RawText(rel, text);
            const uint64_t w = W(rel);
            if (!w) return;
            const float e = TextWidth(text, pt);
            const float s = e > box && e > 0.0f ? std::max(0.6f, box / e) : 1.0f;
            engine::widget::Scale(w, s, s);
        }

        float TextWidth(const char* text, float pt)
        {
            return engine::widget::TextWidth(engine::locale::Tr(text ? text : ""),
                                             pt / 34.0f);
        }

        void Sfx(const char* name)
        {
            AUDIO::PLAY_SOUND(SID(name));
        }

    } // namespace ui

    namespace {

        using namespace core;

        constexpr const char* kFactionName[CZ_NFAC] = { "WLF", "SERAPHITES" };
        constexpr const char* kFactionLogo[CZ_NFAC] = {
            "streaming/photo-mode/logos/wlf-wolf-logo.png",
            "streaming/photo-mode/logos/scar-logo.png",
        };
        constexpr const char* kIcon[CZ_NCAT] = {
            "cz-hat", "cz-torso", "cz-lower", "cz-backpack", "cz-face",
            "cz-melee", "cz-mask", "cz-sex",
        };
        constexpr uint64_t kOpenIn = 160, kOpenHold = 850, kOpenShow = 1010;
        constexpr uint64_t kDenyMs = 480, kNoteMs = 2500;
        constexpr uint64_t kEquipGateMs = 300, kBackFrames = 6;

        struct Runtime {
            Profile p{};
            bool loaded = false, on = false, closing = false, quiet = false;
            bool tipUp = false;
            uint64_t enterMs = 0, showAt = 0, openAt = 0, lastWayIn = 0;
            uint64_t msgUntil = 0, denyUntil = 0, lastTop = 0;
            uint64_t backFrame = 0, matchPopAt = 0;
            uint64_t matchPoppedTop = 0;
            uint64_t key = 0;
            uint32_t gen = 0;              // bumped on progress changes (ext g_cz_gen)
            uint64_t tUp = ~uint64_t{ 0 }, tDown = ~uint64_t{ 0 };
            uint64_t tLeft = ~uint64_t{ 0 }, tRight = ~uint64_t{ 0 };
            int fac = 0, cat = CZ_TORSO;
            int foc[CZ_NCAT]{}, off[CZ_NCAT]{};
            int hvCard = -1, hvTab = -1;
            bool hvBack = false, hvSel = false, hvSw = false, hvF = false;
            bool hvB = false, hvQ = false, hvE = false, hvCh = false;
            bool hvZm = false, hvTip = false, hvUo = false;
            bool hUp = true, hDown = true, hLeft = true, hRight = true;
            bool hSel = true, hBack = true, hEsc = true, hPrev = true, hNext = true;
            bool hAct = true, hQ = true, hE = true, hF = true, hC = true, hIns = true;
            bool hZ = true, hR3 = true, hU = true, hL3 = true, hWay = true;
            bool zoom = false;
            float zoomU = 1.0f;
            uint8_t counter = 0;
            bool netDirty = false;
            uint64_t lightRepAt = 0;
            bool cineWas = false;
            char msg[128]{};
        } g;

        bool Disabled() { return boot::ExtLoaded() || engine::flag::Present("t2r-no-customize.txt"); }

        uint64_t Page() { return engine::widget::PageId("customize"); }
        uint64_t ChallengesPage() { return engine::widget::PageId("customize-challenges"); }

        int Sex(int fac)
        {
            return fac >= 0 && fac < CZ_NFAC && g.p.sex[fac] < CZ_NSEX ? g.p.sex[fac] : 0;
        }

        int CurrentFaction()
        {
            const uint8_t team = engine::lobby::OwnTeam();
            return team == 0xFF ? -1 : (team != 0 ? 1 : 0);
        }

        bool MenuIdle(uint64_t now)
        {
            if (engine::lobby::MatchPhase() == 2) return false;
            if (menu::loadseq::HoldActive()) return false;
            if (net::match::Get().GetPhase() == net::match::Phase::FrontEndLoading) return false;
            const uint64_t ready = DC::Call<"task-want-load-levels-fully-spawned?">();
            if (ready != ~uint64_t{ 0 } && ready == 0) return false;
            if (HUD::GET_BLACK_FADE() > 0.02f) return false;
            (void)now;
            return true;
        }

        void ResetHeld()
        {
            g.hUp = g.hDown = g.hLeft = g.hRight = true;
            g.hSel = g.hBack = g.hEsc = g.hPrev = g.hNext = true;
            g.hAct = g.hQ = g.hE = g.hF = g.hC = g.hIns = true;
            g.hZ = g.hR3 = g.hU = g.hL3 = true;
            g.tUp = g.tDown = g.tLeft = g.tRight = ~uint64_t{ 0 };
            g.key = 0;
        }

        void LoadOnce()
        {
            if (g.loaded) return;
            g.loaded = true;
            if (FILE* f = engine::exe_side::Open("t2r-customize.txt", "r")) {
                char buf[16384];
                const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
                buf[n] = 0;
                std::fclose(f);
                if (!Parse(g.p, buf))
                    Defaults(g.p);
            } else {
                Defaults(g.p);
            }
            if (!StoryMasksShown()) (void)StoryMaskFallback(g.p);
            TO1_LOG("customize: profile loaded (table %04x)\n", TableId());
        }

        void SaveImpl()
        {
            LoadOnce();
            char body[16384];
            const int n = Format(g.p, body, static_cast<int>(sizeof body) - 80);
            if (n <= 0) return;
            std::snprintf(body + n, 80, "\n");
            char tmp[MAX_PATH], final[MAX_PATH];
            if (!engine::exe_side::FullPath("t2r-customize.txt.tmp", tmp, MAX_PATH)
                || !engine::exe_side::FullPath("t2r-customize.txt", final, MAX_PATH))
                return;
            FILE* f = engine::exe_side::Open("t2r-customize.txt.tmp", "wb");
            if (!f) {
                TO1_LOG("customize: cannot write t2r-customize.txt\n");
                return;
            }
            const size_t len = std::strlen(body);
            const bool wrote = std::fwrite(body, 1, len, f) == len && std::fflush(f) == 0;
            std::fclose(f);
            if (!wrote || !MoveFileExA(tmp, final, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                TO1_LOG("customize: atomic save failed (%lu)\n", GetLastError());
                DeleteFileA(tmp);
            }
        }

        void ApplyChange()
        {
            ++g.counter;
            g.netDirty = true;
            SaveImpl();
        }

        void SendIfDirty(uint64_t now)
        {
            (void)now;
            if (!g.netDirty) return;
            if (compose::WearOff()) return;   // read-only opt-out suppresses sends, not the save
            uint8_t body[kMsgLen];
            const int n = Msg(g.p, g.counter, body);
            if (n == kMsgLen)
                net::session::SendEventRaw(static_cast<uint8_t>(NEVT_CUSTOM_LOOK),
                                           body, static_cast<uint8_t>(n));
            g.netDirty = false;
        }

        void Note(const char* text, uint64_t now)
        {
            std::snprintf(g.msg, sizeof g.msg, "%s", engine::locale::Tr(text));
            g.msgUntil = now + kNoteMs;
            g.key = 0;
        }

        void Deny(uint64_t now) { g.denyUntil = now + kDenyMs; g.key = 0; }

        // The core's TabStep() already walks kTabOrder; keep this local helper
        // out of overload resolution with a distinct name.
        int CzTabStep(int cat, int d)
        {
            const int i = (TabPos(cat) + d) % CZ_NCAT;
            return CZ_TAB_ORDER[i < 0 ? i + CZ_NCAT : i];
        }

        int RowsIn(int cat)
        {
            return Listed(g.p, g.fac, Sex(g.fac), cat,
                          IsKellie(), StoryMasksShown());
        }

        int Rows() { return RowsIn(g.cat); }

        // Page row -> table key and back, in any category. The UNLOCKED ONLY
        // toggle uses these to keep every category's focus on its item when
        // the filter changes what is listed.
        int RowKey(int cat)
        {
            return R2K(g.p, g.fac, Sex(g.fac), cat, g.foc[cat],
                       IsKellie(), StoryMasksShown());
        }

        int KeyRow(int cat, int k)
        {
            return K2R(g.p, g.fac, Sex(g.fac), cat, k,
                       IsKellie(), StoryMasksShown());
        }

        bool RowFiltered(int cat, int k)
        {
            return RowOut(g.p, g.fac, Sex(g.fac), cat, k,
                          IsKellie(), StoryMasksShown());
        }

        int EqRow(int cat)
        {
            return EquippedRow(g.p, g.fac, Sex(g.fac), cat,
                               IsKellie(), StoryMasksShown());
        }

        void KeepVisible()
        {
            KeepVisibleRows(g.foc[g.cat], g.off[g.cat], Rows());
        }

        void SetCat(int cat)
        {
            cat = (cat % CZ_NCAT + CZ_NCAT) % CZ_NCAT;
            if (cat == g.cat) return;
            g.cat = cat;
            g.foc[cat] = EqRow(cat);
            KeepVisible();
            ui::Sfx("sfx-cycle");
            TO1_LOG("customize: category %s\n", CZ_CAT_LABEL[cat]);
        }

        void SetFac(int fac)
        {
            fac = (fac % CZ_NFAC + CZ_NFAC) % CZ_NFAC;
            if (fac == g.fac) return;
            g.fac = fac;
            for (int c = 0; c < CZ_NCAT; ++c) {
                g.foc[c] = EqRow(c);
                g.off[c] = 0;
            }
            ui::Sfx("sfx-select");
            TO1_LOG("customize: editing %s\n", kFactionName[fac]);
        }

        void Equip(int row, uint64_t now)
        {
            const int fac = g.fac, sex = Sex(fac), cat = g.cat;
            const int k = R2K(g.p, fac, sex, cat, row,
                              IsKellie(), StoryMasksShown());
            const cz_item_t* it = k >= 0 ? Item(fac, sex, cat, k) : nullptr;
            if (!it) return;
            if (!Unlocked(g.p, it)) {
                char need[160];
                LockLine(g.p, it, need, static_cast<int>(sizeof need));
                Note(need, now);
                Deny(now);
                return;
            }
            if (it->tier > 1) { Note("NOT IN MULTIPLAYER YET", now); return; }
            if (cat != CZ_SEX) {
                const int cc = ConflictCat(fac, sex, g.p.it[fac][sex], cat, k);
                const cz_item_t* worn = cc >= 0 ? Item(fac, sex, cc, g.p.it[fac][sex][cc]) : nullptr;
                if (worn) {
                    const char* tpl = engine::locale::Tr("CONFLICTS WITH %s");
                    const char* at = std::strstr(tpl, "%s");
                    if (at)
                        std::snprintf(g.msg, sizeof g.msg, "%.*s%s%s",
                                      static_cast<int>(at - tpl), tpl,
                                      engine::locale::Tr(worn->label), at + 2);
                    else
                        std::snprintf(g.msg, sizeof g.msg, "%s %s", tpl,
                                      engine::locale::Tr(worn->label));
                    g.msgUntil = now + kNoteMs;
                    g.key = 0;
                    return;
                }
            }
            if (cat == CZ_SEX) {
                if (g.p.sex[fac] == k) return;
                g.p.sex[fac] = static_cast<uint8_t>(k);
                for (int c = 0; c < CZ_NCAT; ++c) {
                    g.foc[c] = EqRow(c);
                    g.off[c] = 0;
                }
            } else {
                if (g.p.it[fac][sex][cat] == k) return;
                g.p.it[fac][sex][cat] = static_cast<uint8_t>(k);
            }
            ApplyChange();
            ui::Sfx("sfx-select");
            TO1_LOG("customize: %s %s = %s\n", kFactionName[fac], CZ_CAT_LABEL[cat], it->label);
        }

        // ext cz_set_uonly(): every category's rows change at once, so keep
        // each focus on the same table item (or on what is worn when that item
        // is now hidden), restart the scroll, and follow the focus into view.
        void SetUonly(bool on, const char* why)
        {
            if (!kUonlyEnabled) return;
            const bool was = (g.p.flags & kFlagUonly) != 0;
            if (on == was) return;
            const int fac = g.fac, sex = Sex(fac);
            int keys[CZ_NCAT];
            for (int c = 0; c < CZ_NCAT; ++c) keys[c] = RowKey(c);
            if (on) g.p.flags |= kFlagUonly;
            else g.p.flags &= ~kFlagUonly;
            for (int c = 0; c < CZ_NCAT; ++c) {
                g.foc[c] = keys[c] >= 0 && !RowFiltered(c, keys[c])
                         ? KeyRow(c, keys[c]) : EqRow(c);
                g.off[c] = 0;
            }
            for (int c = 0; c < CZ_NCAT; ++c)
                KeepVisibleRows(g.foc[c], g.off[c], RowsIn(c));
            SaveImpl();
            g.key = 0;
            ui::Sfx("sfx-cycle");
            TO1_LOG("customize: unlocked only %s (%s; %s: %d of %d rows)\n",
                    on ? "on" : "off", why ? why : "toggle",
                    CZ_CAT_LABEL[g.cat], RowsIn(g.cat), Count(fac, sex, g.cat));
        }

        void Scan()
        {
            g.hvCard = g.hvTab = -1;
            g.hvBack = g.hvSel = g.hvSw = g.hvF = g.hvB = false;
            g.hvQ = g.hvE = g.hvCh = g.hvZm = g.hvTip = g.hvUo = false;
            float cx = 0.0f, cy = 0.0f;
            const bool have = engine::window::CursorCanvas(&cx, &cy);
            if (!have || engine::menu_input::PadMode()) return;
            if (cy >= kBarCy - 55.0f && cy < kBarCy + 55.0f)
                for (int i = 0; i < CZ_NCAT; ++i) {
                    const float x = kBarIcX0 + kBarCell * 0.5f
                                  + i * (kBarCell + kBarGap);
                    if (cx >= x - (kBarCell + kBarGap) * 0.5f
                        && cx < x + (kBarCell + kBarGap) * 0.5f)
                        g.hvTab = CZ_TAB_ORDER[i];
                }
            if (cx >= kGridX && cy >= kGridY) {
                const int col = static_cast<int>((cx - kGridX) / (kCardW + kCardGap));
                const int row = static_cast<int>((cy - kGridY) / (kCardH + kCardGap));
                const float ix = cx - kGridX - col * (kCardW + kCardGap);
                const float iy = cy - kGridY - row * (kCardH + kCardGap);
                if (col >= 0 && col < kCols && row >= 0 && row < kRows
                    && ix < kCardW && iy < kCardH
                    && g.off[g.cat] + row * kCols + col < Rows())
                    g.hvCard = row * kCols + col;
            }
            const uint64_t page = engine::widget::TopPage();
            const auto over = [page](const char* rel) {
                const uint64_t w = engine::widget::Page(rel);
                return w && engine::widget::MouseOver(w, true);
            };
            (void)page;
            g.hvBack = over("anim-group.pBackHit");
            g.hvSel = over("anim-group.pSelHit");
            g.hvSw = over("anim-group.pSwHit");
            g.hvF = over("anim-group.czHitF");
            g.hvB = over("anim-group.czHitB");
            g.hvQ = over("anim-group.tabs.tabQHit");
            g.hvE = over("anim-group.tabs.tabEHit");
            g.hvCh = over("anim-group.pChHit");
            g.hvZm = over("anim-group.pZmHit");
            g.hvTip = over("anim-group.czTip.tipHit");
            g.hvUo = over("anim-group.tabs.tabUoHit");
        }

        void CardLabel(const char* label, char* out, size_t cap)
        {
            std::snprintf(out, cap, "%s", engine::locale::Tr(label ? label : ""));
            if (ui::TextWidth(out, 24.0f) <= kCardTextW) return;
            // Match ext cz_card_label: break at the space/hyphen nearest the
            // middle, not at an arbitrary wrap point. A hyphen stays on the
            // first line; a space is replaced by the newline.
            const size_t n = std::strlen(out);
            const size_t mid = n / 2;
            size_t best = n;
            for (size_t k = 0; k < n; ++k) {
                if (out[k] != ' ' && out[k] != '-') continue;
                const size_t d = k < mid ? mid - k : k - mid;
                const size_t bd = best < n ? (best < mid ? mid - best : best - mid) : n;
                if (d < bd) best = k;
            }
            if (best == n) return;
            if (out[best] == ' ') {
                out[best] = '\n';
                return;
            }
            if (n + 1 < cap) {
                std::memmove(out + best + 2, out + best + 1, n - best);
                out[best + 1] = '\n';
            }
        }

        void FillReq(const Profile& p, int fac, int sex, int cat, int k, bool show,
                     uint64_t now)
        {
            const cz_item_t* it = k >= 0 ? Item(fac, sex, cat, k) : nullptr;
            const LockParts lp = LockTextFor(p, it);
            if (!show || !it || (!lp.title[0] && !lp.line[0])) {
                ui::Vis("anim-group.req", false);
                return;
            }
            const bool open = Unlocked(p, it);
            ui::Vis("anim-group.req", true);
            ui::Vis("anim-group.req.reqOk", open);
            ui::Vis("anim-group.req.reqLk", !open);
            char line[160];
            std::snprintf(line, sizeof line, "%s", engine::locale::Tr(lp.line));
            const bool plain = lp.title[0] && PlainKind(it->uk < CZ_NCHAL
                                                         ? CZ_CHAL[it->uk].kind : -1);
            ui::Text("anim-group.req.reqTi", open && !plain ? "UNLOCKED" : lp.title);
            ui::Vis("anim-group.req.reqDs", !open || plain);
            ui::RawText("anim-group.req.reqDs", line);
            const int lines = std::min(kReqMaxLines,
                                       ReqLines(line, kReqPt,
                                                kCols * kCardW
                                                + (kCols - 1) * kCardGap));
            ui::Vis("anim-group.req.reqBT", !open && !plain);
            ui::Vis("anim-group.req.reqN", !open && !plain);
            if (open) {
                ui::Vis("anim-group.req.reqBF", false);
            } else if (!plain) {
                char count[32];
                std::snprintf(count, sizeof count, "%u / %u", lp.done, lp.need);
                ui::RawText("anim-group.req.reqN", count);
                const float y = kReqDsY + static_cast<float>(lines) * kReqLineH
                              + 22.0f + kReqBarH * 0.5f;
                ui::Pivot("anim-group.req.reqN", kGridX + kCols * kCardW
                          + (kCols - 1) * kCardGap, y);
                const float x1 = kGridX + kCols * kCardW + (kCols - 1) * kCardGap
                               - engine::widget::TextWidth(count, kReqNPt / 34.0f) - 36.0f;
                ui::Vis("anim-group.req.reqBT", true);
                ui::Bar("anim-group.req.reqBT", kGridX, x1, y, 1.0f);
                ui::Bar("anim-group.req.reqBF", kGridX, x1, y,
                        lp.need ? static_cast<float>(lp.done) / static_cast<float>(lp.need) : 0.0f);
            } else {
                ui::Vis("anim-group.req.reqBF", false);
            }
            if (now < g.denyUntil) {
                const float f = std::fabs(std::cos(static_cast<float>(g.denyUntil - now)
                                                   / static_cast<float>(kDenyMs) * 6.2831853f));
                ui::Alpha("anim-group.req", 0.3f + 0.7f * f);
            } else {
                ui::Alpha("anim-group.req", 1.0f);
            }
        }

        void FillTip(bool on)
        {
            static const char* const kHide[] = {
                "grid", "tabs", "title", "titleDiv", "titleSub", "titleRule",
                "secName", "secRule", "mdlName", "pHint", "req", "logoF", "logoB",
                "chkF", "chkB", "pBackIc", "pBackTx", "pSelIc", "pSelTx",
                "pSwIc", "pSwTx", "pChKb", "pChKt", "pChTx", "pZmKb", "pZmKt", "pZmTx",
            };
            ui::Vis("anim-group.czTip", on);
            for (const char* rel : kHide) {
                char path[96];
                std::snprintf(path, sizeof path, "anim-group.%s", rel);
                ui::Vis(path, !on);
            }
            if (!on) return;
            for (int f = 0; f < CZ_NFAC; ++f) {
                int done = 0, total = 0;
                challenges::core::Count(g.p, f, true, StoryMasksShown(), done, total);
                char path[96], buf[32];
                std::snprintf(path, sizeof path, "anim-group.czTip.tipC%d.tipC%dCt", f, f);
                std::snprintf(buf, sizeof buf, "%d | %d", done, total);
                ui::RawText(path, buf);
                std::snprintf(path, sizeof path, "anim-group.czTip.tipC%d.tipC%dNm", f, f);
                ui::Text(path, kFactionName[f]);
            }
            ui::Text("anim-group.czTip.tipTitle", "CHALLENGES");
            ui::Text("anim-group.czTip.tipLine1",
                     "Completing <font color=\"#ffffff\">Challenges</font> will permanently unlock new outfits, headwear, faces and more.");
            ui::Text("anim-group.czTip.tipLine2",
                     "You can view your <font color=\"#ffffff\">Challenges</font> any time from the <font color=\"#ffffff\">CUSTOMIZE</font> page.");
            ui::Text("anim-group.czTip.tipCont", "CONTINUE");
            const float lw = ui::TextWidth("CONTINUE", 42.0f);
            ui::Pivot("anim-group.czTip.tipKey", 2870.0f - lw - 22.0f - 32.0f, 1816.0f);
        }

        void Fill(uint64_t now)
        {
            KeepVisible();
            const int fac = g.fac, sex = Sex(fac), cat = g.cat;
            const int n = Rows(), foc = g.foc[cat], off = g.off[cat];
            const int eq = Equipped(g.p, fac, sex, cat);
            const int cur = CurrentFaction();
            const bool msg = now < g.msgUntil;
            const bool uo = (g.p.flags & kFlagUonly) != 0;
            const uint64_t key = static_cast<uint64_t>(fac)
                | static_cast<uint64_t>(sex) << 2 | static_cast<uint64_t>(cat) << 4
                | static_cast<uint64_t>(foc & 0xff) << 8
                | static_cast<uint64_t>(off & 0xff) << 16
                | static_cast<uint64_t>(eq & 0xff) << 24
                | static_cast<uint64_t>(cur + 1) << 32
                | static_cast<uint64_t>(g.hvCard + 1) << 34
                | static_cast<uint64_t>(g.hvTab + 1) << 39
                | static_cast<uint64_t>(msg) << 44
                | static_cast<uint64_t>(engine::menu_input::PadMode()) << 45
                | static_cast<uint64_t>(uo) << 46
                | static_cast<uint64_t>(g.zoom) << 47
                | static_cast<uint64_t>(g.tipUp) << 48
                | static_cast<uint64_t>(g.hvUo) << 49
                | static_cast<uint64_t>(g.gen & 0xffu) << 50
                | (uint64_t{ 1 } << 63);
            if (key == g.key) return;
            g.key = key;

            char path[96], buf[256];
            const char* title = engine::locale::Tr("CUSTOMIZE");
            const char* sub = engine::locale::Tr(CZ_CAT_LABEL[cat]);
            float hx = kHeadX + ui::TextWidth(title, kHeadPt) + 65.0f;
            ui::Pivot("anim-group.titleDiv", hx - 1.0f, kHeadCy - 25.0f);
            hx += 65.0f; // clang-format off
            ui::Pivot("anim-group.titleSub", hx, kHeadCy); // clang-format on
            ui::RawText("anim-group.titleSub", sub);
            const float rx = hx + ui::TextWidth(sub, kHeadPt) + 65.0f;
            ui::Rule("anim-group.titleRule", rx, kHeadCy - 1.0f, kHeadRuleX1,
                     kHeadRuleX1 - 1240.0f);

            for (int i = 0; i < CZ_NCAT; ++i) {
                std::snprintf(path, sizeof path, "anim-group.tabs.tabIc%d", i);
                const uint64_t w = ui::W(path);
                if (w) engine::widget::Alpha(w, i == cat ? 1.0f
                                                  : i == g.hvTab ? 0.6f : kTabAlphaOff);
            }
            if (kUonlyEnabled) {
                ui::Text("anim-group.tabs.tabUoKt",
                         engine::menu_input::PadMode() ? "L3" : "U");
                ui::Text("anim-group.tabs.tabUoTx", uo ? "SHOW ALL" : "UNLOCKED ONLY");
                const uint64_t w = ui::W("anim-group.tabs.tabUoTx");
                if (w) engine::widget::Alpha(w, uo || g.hvUo ? 1.0f : 0.8f);
            }

            const int fk = R2K(g.p, fac, sex, cat, foc,
                               IsKellie(), StoryMasksShown());
            const cz_item_t* fi = fk >= 0 ? Item(fac, sex, cat, fk) : nullptr;
            const char* nm = fi ? engine::locale::Tr(fi->label) : "";
            ui::RawText("anim-group.secName", nm);
            ui::Rule("anim-group.secRule",
                     kItemX + engine::widget::TextWidth(nm, kItemPt / 34.0f) + 60.0f,
                     kItemCy - 1.0f, kItemRuleX1, kItemRuleX1 - kItemX - 500.0f);

            for (int i = 0; i < kCards; ++i) {
                const int k = R2K(g.p, fac, sex, cat, off + i,
                                  IsKellie(), StoryMasksShown());
                const cz_item_t* it = k >= 0 ? Item(fac, sex, cat, k) : nullptr;
                std::snprintf(path, sizeof path, "anim-group.grid.cd%d", i);
                ui::Vis(path, it != nullptr);
                if (!it) continue;
                const bool focused = off + i == foc, hovered = i == g.hvCard;
                const bool locked = !Unlocked(g.p, it);
                const bool conflict = cat != CZ_SEX
                    && ConflictCat(fac, sex, g.p.it[fac][sex], cat, k) >= 0;
                const char* pic = it->pic && std::strlen(it->pic) <= 96 ? it->pic : nullptr;
                const char* icon = cat == CZ_MELEE ? (k ? "cz-knife" : "cz-fists")
                                 : cat == CZ_SEX ? (k ? "cz-female" : "cz-male")
                                 : kIcon[cat];
                char leaf[96];
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdPic%d", i, i);
                ui::Vis(leaf, pic != nullptr);
                if (pic) {
                    std::snprintf(buf, sizeof buf, "streaming/t2r/icons/%s.png", pic);
                    ui::Image(leaf, buf);
                    ui::Alpha(leaf, conflict ? 0.3f : focused ? 1.0f : hovered ? 0.85f : 0.7f);
                }
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdIc%d", i, i);
                ui::Vis(leaf, pic == nullptr);
                if (!pic) {
                    std::snprintf(buf, sizeof buf, "streaming/t2r/icons/%s.png", icon);
                    ui::Image(leaf, buf);
                    ui::Alpha(leaf, focused ? 1.0f : hovered ? 0.8f : 0.5f);
                }
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdNm%d", i, i);
                CardLabel(it->label, buf, sizeof buf);
                ui::RawText(leaf, buf);
                ui::Alpha(leaf, conflict ? 0.4f : (focused || hovered) ? 1.0f : 0.7f);
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdFill%d", i, i);
                ui::Vis(leaf, focused || hovered);
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdTop%d", i, i);
                ui::Vis(leaf, focused);
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdBot%d", i, i);
                ui::Vis(leaf, focused);
                const bool og = it->uk && it->uk < CZ_NCHAL
                    && CZ_CHAL[it->uk].kind == CZK_OG && ui::W("anim-group.grid.cd0.cdEqU0");
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdEq%d", i, i);
                ui::Vis(leaf, k == eq && !og);
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdEqU%d", i, i);
                ui::Vis(leaf, k == eq && og);
                std::snprintf(leaf, sizeof leaf, "anim-group.grid.cd%d.cdLock%d", i, i);
                ui::Vis(leaf, locked);
            }
            const bool sb = n > kCards;
            ui::Vis("anim-group.grid.sbTrack", sb);
            ui::Vis("anim-group.grid.sbThumb", sb);
            if (sb) {
                const float gh = kRows * kCardH + (kRows - 1) * kCardGap;
                ui::Scale("anim-group.grid.sbThumb", 1.0f,
                          static_cast<float>(kCards) / static_cast<float>(n));
                ui::Pivot("anim-group.grid.sbThumb", kSbX,
                          kGridY + gh * static_cast<float>(off) / static_cast<float>(n));
            }

            ui::Image("anim-group.logoF", kFactionLogo[fac]);
            ui::Image("anim-group.logoB", kFactionLogo[!fac]);
            const bool zoomed = g.zoom && cat != CZ_SEX;
            ui::Vis("anim-group.logoF", !zoomed);
            ui::Vis("anim-group.logoB", !zoomed);
            ui::Vis("anim-group.mdlName", !zoomed);
            ui::Vis("anim-group.chkF", cur == fac && !zoomed);
            ui::Vis("anim-group.chkB", cur == !fac && !zoomed);
            ui::Text("anim-group.mdlName", kFactionName[fac]);
            if (ui::W("anim-group.pZmTx")) {
                ui::RawText("anim-group.pZmKt", engine::menu_input::PadMode() ? "R3" : "Z");
                ui::Text("anim-group.pZmTx", g.zoom ? "ZOOM OUT" : "ZOOM IN");
            }
            ui::Text("anim-group.pSelTx", "EQUIP");
            ui::Text("anim-group.pSwTx", "SWITCH FACTION");
            ui::RawText("anim-group.pChKt", engine::menu_input::PadMode() ? "Y" : "C");
            ui::Text("anim-group.pChTx", "CHALLENGES");
            ui::RawText("anim-group.pHint", msg ? g.msg : "");
            if (ui::W("anim-group.facBox")) {
                const bool note = cur >= 0 && cur != fac;
                ui::Text("anim-group.facBox.facNote",
                         note ? "NOT YOUR CURRENT FACTION - WORN ONCE YOU SWITCH SIDES" : "");
                ui::Alpha("anim-group.facBox", note ? 1.0f : 0.0f);
                if (note) {
                    const char* nt = engine::locale::Tr(
                        "NOT YOUR CURRENT FACTION - WORN ONCE YOU SWITCH SIDES");
                    int nch = 0;
                    for (const char* q = nt; *q; ++q)
                        if ((*q & 0xC0) != 0x80) ++nch;
                    const float x1 = kHeadRuleX1;
                    const float x0 = x1 - ((ui::TextWidth(nt, 30.0f)
                                            + 2.0f * static_cast<float>(nch)) * 0.88f
                                           + 2.0f * kFacPad);
                    ui::Rule("anim-group.facBox.facBg", x0, kFacY, x1, kFacW);
                    ui::Rule("anim-group.facBox.facFrT", x0, kFacY, x1, kFacW);
                    ui::Rule("anim-group.facBox.facFrB", x0, kFacY + kFacH - 2.0f, x1, kFacW);
                    ui::Pivot("anim-group.facBox.facFrL", x0, kFacY);
                    ui::Pivot("anim-group.facBox.facNote", (x0 + x1) * 0.5f,
                              kFacY + kFacH * 0.5f);
                }
            } else if (ui::W("anim-group.facNote")) {
                ui::Text("anim-group.facNote",
                         cur >= 0 && cur != fac
                             ? "NOT YOUR CURRENT FACTION - WORN ONCE YOU SWITCH SIDES" : "");
            }
            FillReq(g.p, fac, sex, cat, fk, fi && !msg && !g.tipUp, now);
        }

        void Fade(uint64_t now)
        {
            const uint64_t t0 = g.showAt > g.enterMs && g.showAt - g.enterMs < 3000
                              ? g.showAt : g.enterMs;
            const float f = now <= t0 ? 0.0f
                : std::min(1.0f, static_cast<float>(now - t0)
                                 / static_cast<float>(t0 == g.enterMs ? 220 : 320));
            const int q = static_cast<int>(f * 24.0f + 0.5f);
            ui::Alpha("anim-group", static_cast<float>(q) / 24.0f);
        }

        void Enter(uint64_t now)
        {
            g.on = true;
            g.closing = g.quiet = false;
            g.enterMs = now;
            ResetHeld();
            const int cur = CurrentFaction();
            g.fac = cur >= 0 ? cur : 0;
            g.cat = CZ_TORSO;
            g.zoom = false;
            g.zoomU = 1.0f;
            for (int c = 0; c < CZ_NCAT; ++c) {
                g.foc[c] = EqRow(c);
                g.off[c] = 0;
            }
            KeepVisible();
            ui::Alpha("anim-group", 0.0f);
            if (challenges::core::TipWanted(g.p)) {
                g.tipUp = true;
                FillTip(true);
            }
            TO1_LOG("customize: page up (editing %s, side %d)\n", kFactionName[g.fac], cur);
        }

        void Close(const char* how, uint64_t now)
        {
            (void)how;
            if (g.closing || g.quiet) return;
            ui::Alpha("anim-group", 1.0f);
            g.quiet = true;
            const uint64_t frame = engine::frame::Count();
            g.backFrame = frame ? frame : 1;
            g.key = 0;
        }

        void Pop(uint64_t top)
        {
            if (top != Page()) return;
            DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            g.closing = true;
            g.backFrame = 0;
            TO1_LOG("customize: closed\n");
        }

        void WayIn(uint64_t now)
        {
            if (g.openAt) {
                if (now < g.openAt) return;
                g.openAt = 0;
                MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/customize");
                return;
            }
            const uint64_t top = engine::widget::TopPage();
            if (top == Page() || top == ChallengesPage()) return;
            if (!MenuIdle(now) || now - g.lastWayIn < 1500) return;
            const uint64_t row = MENU2::GET_PATH_IN_CURRENT_PAGE(
                "anim-group.menuList.Customize", MenuStack::hudMenuStack);
            const bool byKey = engine::menu_input::CommandEdge(SID("menu-select"), g.hWay)
                && row && DC::Call<"menu2-get-focused-component">() == row;
            const bool byClick = engine::menu_input::ClickEdge() && row
                && engine::widget::MouseOver(engine::widget::Page(
                       "anim-group.menuList.Customize"), true);
            if (!byKey && !byClick) return;
            g.lastWayIn = now;
            g.openAt = now + kOpenIn + 20;
            g.showAt = now + kOpenShow;
            DC::Call<"fade-to-black">(0.16f, 0.85f, 0.40f);
            ui::Sfx("sfx-select");
            TO1_LOG("customize: opening (%s)\n", byClick ? "click" : "select");
        }

        void MatchFoundClose(uint64_t top, uint64_t now)
        {
            const bool ours = top == Page() || top == ChallengesPage();
            if (!ours || engine::lobby::MatchPhase() != 2) {
                if (g.matchPoppedTop && top != g.matchPoppedTop) {
                    g.matchPoppedTop = 0;
                    g.matchPopAt = 0;
                }
                return;
            }
            if (g.matchPoppedTop == top) {
                if (g.matchPopAt && engine::frame::Count() - g.matchPopAt > 120) {
                    g.matchPopAt = 0;
                    g.matchPoppedTop = 0;   // try once more on the next tick
                }
                return;
            }
            g.matchPoppedTop = top;
            g.matchPopAt = engine::frame::Count();
            ui::Alpha("anim-group", 1.0f);
            DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            TO1_LOG("customize: match found - closing %llx before the map load\n",
                    static_cast<unsigned long long>(top));
        }

    } // namespace

    bool StoryMasksShown()
    {
#if defined(TO1_RELEASE) || defined(T2R_RELEASE)
        return false;
#else
        return engine::flag::Present("t2r-cz-story-masks.txt");
#endif
    }

    bool IsKellie()
    {
        static int value = -1;
        static uint64_t at = 0;
        const uint64_t now = ::GetTickCount64();
        if (value >= 0 && now - at < 15000) return value != 0;
        at = now;
        value = 0;
        DWORD activeUser = 0, size = sizeof activeUser;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess",
                         "ActiveUser", RRF_RT_REG_DWORD, nullptr,
                         &activeUser, &size) == ERROR_SUCCESS && activeUser) {
            char token[64];
            std::snprintf(token, sizeof token, "t2r-kellie-1118:%llu",
                          76561197960265728ULL
                              + static_cast<unsigned long long>(activeUser));
            value = SID(token) == 0x20644379c4ec6b9fULL ? 1 : 0;
        }
        return value != 0;
    }

    bool Active() { return !Disabled() && g.on; }
    void EnsureLoaded() { LoadOnce(); }
    core::Profile& GetProfile() { return g.p; }
    int EditingFaction() { return g.fac; }
    int EditingCategory() { return g.cat; }
    int Sex(int fac) { return net::ui::customize::Sex(fac); }
    int CurrentSide() { return CurrentFaction(); }
    bool TipUp() { return g.tipUp; }
    uint64_t ShowAt() { return g.showAt; }
    bool NetChanged() { return g.netDirty; }

    bool PageUp()
    {
        if (Disabled()) return false;
        const uint64_t top = engine::widget::TopPage();
        return top && (top == Page() || top == ChallengesPage())
            && !g.closing && !g.quiet;
    }

    int FocusedKey(int fac, int sex, int cat)
    {
        if (!g.on || cat < 0 || cat >= CZ_NCAT || cat == CZ_SEX) return -1;
        return core::R2K(g.p, fac, sex, cat, g.foc[cat], true, StoryMasksShown());
    }

    bool Zoomed() { return g.zoom && g.cat != CZ_SEX; }
    float ZoomUse() { return g.zoomU; }
    bool FrontHovered() { return g.hvF; }

    uint8_t ConsumeNetChange()
    {
        g.netDirty = false;
        return g.counter;
    }

    void SaveProfile() { SaveImpl(); }
    void Changed() { ApplyChange(); }

    void ProgressChanged()
    {
        ++g.gen;
        g.key = 0;
        SaveImpl();
    }

    uint64_t ProgressGeneration() { return g.gen; }

    void TipDone()
    {
        if (!g.tipUp) return;
        g.tipUp = false;
        g.p.flags |= kFlagChTip;
        g.key = 0;
        FillTip(false);
        SaveImpl();
        ui::Sfx("sfx-select");
        TO1_LOG("customize: CHALLENGES tip dismissed\n");
    }

    void ChallengesLeft()
    {
        ResetHeld();
        g.key = 0;
    }

    void Tick(uint64_t now)
    {
        if (Disabled()) {
            g.on = false;
            g.openAt = 0;
            return;
        }
        LoadOnce();
        WayIn(now);
        const uint64_t top = engine::widget::TopPage();
        MatchFoundClose(top, now);
        SendIfDirty(now);
        if (top != Page() || !top) {
            const bool fromChallenges = g.lastTop == ChallengesPage();
            g.on = false;
            if (fromChallenges) ChallengesLeft();
            g.lastTop = top;
            return;
        }
        if (!g.on) Enter(now);
        if (g.closing) { g.lastTop = top; return; }
        if (g.lastTop == ChallengesPage()) ChallengesLeft();
        g.lastTop = top;
        if (g.quiet) {
            const uint64_t frame = engine::frame::Count();
            if (g.backFrame && frame >= g.backFrame + kBackFrames)
                Pop(top);
            Fade(now);
            return;
        }

        bool sel = engine::menu_input::CommandEdge(SID("menu-select"), g.hSel);
        const bool back = engine::menu_input::CommandEdge(SID("menu-back"), g.hBack);
        const bool escNow = engine::window::KeyDown(VK_ESCAPE);
        const bool esc = escNow && !g.hEsc;
        g.hEsc = escNow;
        const bool up = engine::menu_input::CommandRepeat(SID("menu-up"), g.hUp, g.tUp, now);
        const bool down = engine::menu_input::CommandRepeat(SID("menu-down"), g.hDown, g.tDown, now);
        const bool left = engine::menu_input::CommandRepeat(SID("menu-left"), g.hLeft, g.tLeft, now);
        const bool right = engine::menu_input::CommandRepeat(SID("menu-right"), g.hRight, g.tRight, now);
        const bool prev = engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
        const bool next = engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);
        const bool fNow = engine::window::KeyDown('F');
        bool sw = (fNow && !g.hF)
            || engine::menu_input::CommandEdge(SID("menu-action"), g.hAct);
        g.hF = fNow;
        const bool qNow = engine::window::KeyDown('Q'), eNow = engine::window::KeyDown('E');
        const bool tabL = (qNow && !g.hQ) || prev;
        const bool tabR = (eNow && !g.hE) || next;
        g.hQ = qNow;
        g.hE = eNow;
        const bool cNow = engine::window::KeyDown('C');
        const bool chal = (cNow && !g.hC)
            || engine::menu_input::CommandEdge(SID("menu-inspect"), g.hIns);
        g.hC = cNow;
        const bool zNow = engine::window::KeyDown('Z');
        const uint16_t pad = engine::menu_input::PadButtons();
        const bool r3 = (pad & 0x0080) && !(pad & 0x0040);
        const bool zoom = (zNow && !g.hZ) || (r3 && !g.hR3);
        g.hZ = zNow;
        g.hR3 = r3;
        const bool uNow = engine::window::KeyDown('U');
        const bool l3 = (pad & 0x0040) && !(pad & 0x0080);
        const bool uonly = (uNow && !g.hU) || (l3 && !g.hL3);
        g.hU = uNow;
        g.hL3 = l3;
        const int wheel = engine::menu_input::WheelNotches();
        Scan();
        bool click = engine::menu_input::ClickEdge();

        // The stage freecam owns all input while it flies. Its Esc (sampled in
        // stage::Tick) must not also close the page when the key is released.
        if (stage::FreecamActive()) {
            g.hEsc = true;
            Fade(now);
            return;
        }
        if (g.tipUp) {
            if (now - g.enterMs > 400
                && (sel || back || esc || (click && g.hvTip)))
                TipDone();
            Fade(now);
            return;
        }
        if (chal || (click && g.hvCh)) {
            MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/customize-challenges");
            ui::Sfx("sfx-select");
            return;
        }
        // Live stage lighting, owned by the stage but driven from the page's
        // already-sampled keyboard state (ext customize.c: [ / ] repeat at 110 ms,
        // L cycles YOUR LAYOUT -> A..E for the stage on screen).
        if (engine::window::Focused()) {
            const bool dn = engine::window::KeyDown(VK_OEM_4);
            const bool br = engine::window::KeyDown(VK_OEM_6);
            if ((dn || br) && now >= g.lightRepAt) {
                g.lightRepAt = now + 110;
                stage::LightStep(br ? 1 : -1);
                std::snprintf(g.msg, sizeof g.msg, "STAGE LIGHT %d%%",
                              static_cast<int>(stage::LightPercent() * 100.0f + 0.5f));
                g.msgUntil = now + 2500;
                g.key = 0;
            }
            const bool lNow = engine::window::KeyDown('L');
            if (lNow && !g.cineWas) {
                const char* nm = stage::CineStep();
                std::snprintf(g.msg, sizeof g.msg, "LIGHTING: %s  %d%%", nm ? nm : "",
                              static_cast<int>(stage::LightPercent() * 100.0f + 0.5f));
                g.msgUntil = now + 3500;
                g.key = 0;
                TO1_LOG("customize: lighting setup -> %s (L)\n", nm ? nm : "");
            }
            g.cineWas = lNow;
        }
        if (zoom || (click && g.hvZm)) {
            g.zoom = !g.zoom;
            g.zoomU = 1.0f;
            g.key = 0;
            click = false;
            ui::Sfx("sfx-cycle");
        }
        if (g.zoom) {
            float rx = 0.0f, ry = 0.0f;
            if (wheel && g.hvCard < 0) {
                g.zoomU = std::clamp(g.zoomU + 0.12f * static_cast<float>(wheel),
                                     0.5f, 2.2f);
            }
            if (engine::menu_input::RightStick(rx, ry) && std::fabs(ry) > 0.25f)
                g.zoomU = std::clamp(g.zoomU + ry * 0.035f, 0.5f, 2.2f);
        }
        if (back || esc || (click && g.hvBack))
            Close(esc ? "Esc" : back ? "menu-back" : "BACK clicked", now);
        if (g.quiet) { Fade(now); return; }
        // The filter runs before navigation: the keys below move the remapped
        // focus, and a click on the toggle is consumed here.
        if (uonly || (click && g.hvUo)) {
            SetUonly((g.p.flags & kFlagUonly) == 0,
                     uonly ? (l3 ? "pad L3" : "U") : "toggle clicked");
            click = false;
        }

        const int n = Rows();
        int& foc = g.foc[g.cat];
        if (up && foc - kCols >= 0) { foc -= kCols; ui::Sfx("sfx-cycle"); }
        if (down && foc + kCols < n) { foc += kCols; ui::Sfx("sfx-cycle"); }
        else if (down && foc / kCols < (n - 1) / kCols) {
            foc = n - 1;
            ui::Sfx("sfx-cycle");
        }
        if (left && foc % kCols > 0) { --foc; ui::Sfx("sfx-cycle"); }
        if (right && foc % kCols < kCols - 1 && foc + 1 < n) {
            ++foc;
            ui::Sfx("sfx-cycle");
        }
        if (wheel && n > kCards) {
            const int max = ((n - 1) / kCols - kRows + 1) * kCols;
            const int off = std::clamp(g.off[g.cat] - wheel * kCols, 0, max);
            if (off != g.off[g.cat]) {
                g.off[g.cat] = off;
                if (foc < off) foc = off;
                if (foc >= off + kCards) foc = off + kCards - 1;
            }
        }
        KeepVisible();
        if (tabL) SetCat(CzTabStep(g.cat, -1));
        if (tabR) SetCat(CzTabStep(g.cat, 1));
        if (click) {
            if (g.hvTab >= 0) SetCat(g.hvTab);
            else if (g.hvQ) SetCat(CzTabStep(g.cat, -1));
            else if (g.hvE) SetCat(CzTabStep(g.cat, 1));
            else if (g.hvCard >= 0) {
                foc = g.off[g.cat] + g.hvCard;
                Equip(foc, now);
            } else if (g.hvSel) sel = true;
            else if (g.hvSw || g.hvB) sw = true;
        }
        static int lastHover = -2;
        if (g.hvCard != lastHover) {
            lastHover = g.hvCard;
            if (g.hvCard >= 0 && !engine::menu_input::PadMode())
                foc = g.off[g.cat] + g.hvCard;
        }
        if (sel && now - g.enterMs > kEquipGateMs) Equip(foc, now);
        if (sw && now - g.enterMs > kEquipGateMs) SetFac(!g.fac);
        Fill(now);
        Fade(now);
    }

    void Reset()
    {
        g = Runtime{};
    }

} // namespace net::ui::customize
