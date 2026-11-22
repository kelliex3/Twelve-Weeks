// Native port of tools/ext/friends13.c -> the in-game FRIENDS / PARTY panel.
// The ext module is the behavioural reference; this is native C++ in the mod's idiom.

#include "friends13.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "net-protocol.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/lobby.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "daemon-rpc.hpp"
#include "net-account.hpp"
#include "net-gui2.hpp"
#include "net-matchmaking.hpp"
#include "net/match/match-liveness.hpp"
#include "notify.hpp"
#include "notice.hpp"
#include "friends-state.hpp"
#include "rank-badge.hpp"
#include "search-rows.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::friends13 {

    namespace {

        using social::Action;
        using social::Row;
        using social::RowKind;
        using social::State;

        constexpr int    kLeftRows = 6;
        constexpr int    kRightRows = 7;
        constexpr int    kToastN = 6;
        constexpr size_t kTokenLen = 64;
        constexpr size_t kArgMax = 100;
        constexpr size_t kRepCap = 24000;
        constexpr size_t kReqLen = 64 + 3 + kArgMax;
        constexpr uint8_t kOpReq = static_cast<uint8_t>(NetDaemonOp::kFriendsRequest);
        constexpr uint8_t kOpRep = static_cast<uint8_t>(NetDaemonOp::kFriendsReply);
        constexpr uint64_t kFlagMs = 5000;
        constexpr uint64_t kActionCoolMs = 600;
        constexpr uint64_t kOpenCoolMs = 1200;
        constexpr uint64_t kAutoWindowMs = 8000;
        constexpr uint64_t kFakeSearchMs = 9000;
        constexpr float    kRankRes46 = 120.0f;   // ext RKB_RES46 (reserved badge width before a 46 pt name)
        constexpr float    kNamePivotX = 36.0f;   // authored pivot of a row's name widget
        constexpr float    kNamePivotY = 40.0f;

        enum ToastKind { kToastRequest, kToastInvite, kToastGood, kToastBad, kToastInfo };

        struct ToastItem {
            int kind = kToastInfo;
            char id[40]{};
            char name[36]{};
            char text[160]{};
            float secs = 0.0f;
            uint64_t readyAt = 0;
        };

        struct NetState {
            bool  have = false;
            int   ver = 0;
            int   fails = 0, okOnce = 0, dead = 0;
            uint64_t nextPoll = 0, okAt = 0;
            bool  actPending = false;
            int   actSub = 0;
            bool  actInflight = false;
            char  actArg[64]{};
            Action actCode = Action::None;
            int   nfBefore = 0;
            uint64_t tseen[32]{};
            int   ntseen = 0;
            char  msg[200]{};
            uint64_t msgUntil = 0;
            int   lastNf = 0, lastNi = 0, lastNm = 0, lastNpi = 0;
        };

        struct PanelState {
            bool up = false, closing = false, dirty = true, typing = false;
            int  col = 0, foc[2]{}, off[2]{};
            int  blen = 0, caret = 0;
            char buf[40]{};
            uint64_t enterMs = 0, closeMs = 0, typMs = 0, editMs = 0;
            uint64_t backFrame = 0;
        };

        struct ToastState {
            int st = 0;                    // 0 idle, 1 in, 2 hold, 3 out
            ToastItem it;
            uint64_t t0 = 0, tOut = 0, cool = 0;
            float leftMs = 0.0f, totalMs = 0.0f;
            uint64_t last = 0;
            int off = -1, missing = 0;
            uint64_t gw = 0;               // ext T13.gw: the card group's handle
            int qa = -1, qx = -1, qb = -1; // ext T13.qa/qx/qb: quantised write dedup
            float fy = 0.0f, fn = 0.0f;    // ext GuFade's tFillY / tFillN alpha
            int   qfy = -1, qfn = -1;      // ext GuFade's quantised write dedup
            uint64_t gfMs = 0;             // ext g_gfMs: GuFade's last tick
        };

        // ext friends13.c FT_* (the bottom-right toast card's keyframes).
        constexpr float kFtX = 2480.0f, kFtY = 1760.0f;
        constexpr float kFtW = 1240.0f, kFtH = 180.0f;
        constexpr float kFtInMs = 260.0f, kFtOutMs = 220.0f;

        struct MenuHeld {
            bool sel = false, up = false, down = false, left = false, right = false;
            bool back = false, prev = false, next = false, act = false;
        };

        State          g_state;
        NetState       g_net;
        PanelState     g_panel;
        ToastState     g_toast;
        ToastItem      g_queue[kToastN];
        int            g_queueN = 0;

        std::atomic<int> g_busy{ 0 }, g_have{ 0 };
        std::atomic<int> g_repLen{ 0 };
        uint8_t        g_job[kReqLen]{};
        int            g_jobN = 0;
        char           g_rep[kRepCap + 16]{};
        char           g_tok[kTokenLen + 1]{};

        int            g_off = -1, g_offChk = 0;
        MenuHeld       g_held;
        bool           g_hSel = false, g_hUp = false, g_hDown = false;
        bool           g_hLeft = false, g_hRight = false, g_hBack = false;
        bool           g_hPrev = false, g_hNext = false, g_hAct = false;
        bool           g_hP = false, g_hPadNext = false;
        uint64_t       g_repUp = 0, g_repDown = 0;
        bool           g_keyCur[256]{}, g_keyPrev[256]{};
        uint64_t       g_autoPush = 0, g_openAt = 0, g_lastMemberPress = 0;
        bool           g_armedOpen = false;

        // Click-to-nav (ext F13.hv_col / hv_row / hv_back / hv_sel / hv_act and g_clickEdge).
        // The left-button edge is sampled every frame in Tick() -- even while the panel is
        // closed -- so a button held across the page opening never counts as a click. The
        // hover boxes are the ext's pinned friends13_widgets.py canvas geometry, read through
        // the native engine::window::CursorCanvas (ext UiCursorCanvas). t2r-no-mouse-nav.txt
        // opts the whole mouse path out, like engine::widget::MouseOver's gate.
        bool           g_lbHeld = false, g_click = false;
        uint64_t       g_clickFrame = 0;          // ext F13.click_f (drop the click's menu-select)
        int            g_hvCol = -1, g_hvRow = -1;
        bool           g_hvBack = false, g_hvSel = false, g_hvAct = false;

        struct RecentLine { char text[300]{}; uint64_t at = 0; };
        RecentLine     g_recent[8];
        int            g_recentN = 0;

        // ---- helpers ----------------------------------------------------------------
        const char* Tr(const char* en, const char* pl) { return social::Tr(en, pl); }

        uint64_t Now() { return GetTickCount64(); }

        bool Pl() { return engine::locale::IsPolish(); }

        void Sfx(const char* name) { AUDIO::PLAY_SOUND(SID(name)); }

        uint64_t PageId(const char* page);

        bool FeatureEnabled()
        {
            const uint64_t now = Now();
            if (g_off < 0 || now - g_offChk > kFlagMs) {
                g_offChk = now;
                const int o = engine::flag::Absent("t2r-no-ingame-friends.txt",
                    static_cast<int>(kFlagMs * 60 / 1000)) ? 0 : 1;
                if (g_off != o) {
                    g_off = o;
                    TO1_LOG("friendsui: in-game FRIENDS panel %s\n",
                        o ? "on" : "off (t2r-no-ingame-friends.txt)");
                }
            }
            return g_off == 0;
        }

        bool ToastCardEnabled()
        {
            if (g_toast.off < 0)
                g_toast.off = engine::flag::Absent("t2r-no-friend-toast.txt") ? 0 : 1;
            if (g_toast.off || g_toast.missing) return false;
            // The card lives in tools/features/friends13_widgets.py. Until those defs
            // are in the mod's pak the handle is 0: fall back to the plain toast path
            // exactly like the ext's t2r-no-friend-toast.txt does.
            const uint64_t top = engine::widget::TopPage();
            if (!g_toast.missing && (top == PageId("main-menu") || top == PageId("find-match-menu"))
                && !engine::widget::Page("anim-group.frToast")) {
                g_toast.missing = 1;
                TO1_LOG("friendsui: frToast widgets absent - plain toasts instead\n");
            }
            return !g_toast.missing;
        }

        // ext UiText / UiVisible / WScale on a page-relative path. A missing widget
        // resolves to 0 and every write is inert, which is the honest gate for the
        // widget definitions that live in tools/features/friends13_widgets.py.
        void UiText(const char* path, const char* text)
        {
            engine::widget::SetText(engine::widget::Page(path), text ? text : "");
        }
        void UiVisible(const char* path, bool on)
        {
            engine::widget::Visible(engine::widget::Page(path), on);
        }
        void UiAlpha(const char* path, float a)
        {
            engine::widget::Alpha(engine::widget::Page(path), a);
        }
        void Fit(const char* path, const char* text, float box, float pt)
        {
            const char* t = engine::locale::Translate(text ? text : "");
            const uint64_t w = engine::widget::Page(path);
            if (!w) return;
            engine::widget::SetText(w, t);
            float est = engine::widget::TextWidth(t, 1.0f) * pt;
            if (est <= 0.0f) est = 1.0f;
            float sc = est > box ? box / est : 1.0f;
            if (sc < 0.6f) sc = 0.6f;
            engine::widget::Scale(w, sc, sc);
        }

        uint64_t PageId(const char* page) { return engine::widget::PageId(page); }

        void PushPage(const char* page)
        {
            if (!MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, PageId(page)))
                MENU2::PUSH_PAGE(MenuStack::hudMenuStack, page);
        }
        void PopPage()
        {
            DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
        }

        void SampleKeys()
        {
            for (int i = 0; i < 256; i++) {
                g_keyPrev[i] = g_keyCur[i];
                g_keyCur[i] = engine::window::KeyDown(i);
            }
        }
        bool KeyDown(int vk) { return g_keyCur[vk & 0xff]; }
        bool KeyEdge(int vk) { return g_keyCur[vk & 0xff] && !g_keyPrev[vk & 0xff]; }

        void SampleMenu()
        {
            MenuHeld h;
            h.sel   = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-select"));
            h.up    = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-up"));
            h.down  = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-down"));
            h.left  = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-left"));
            h.right = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-right"));
            h.back  = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-back"));
            h.prev  = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-prev"));
            h.next  = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-next"));
            h.act   = DC::JOYPAD_COMMAND_ACTIVE(SID("menu-action"));
            g_held = h;
        }
        bool MenuEdge(bool now, bool& held) { bool e = now && !held; held = now; return e; }

        // ext GuRep: menu edge + key repeat (400 ms then 110 ms).
        bool Repeat(bool down, bool& held, uint64_t& next, uint64_t now)
        {
            bool fire = false;
            if (down && !held) { fire = true; next = now + 400; }
            else if (down && held && now >= next) { fire = true; next = now + 110; }
            held = down;
            return fire;
        }

        // ---- plain / card toasts -----------------------------------------------------
        static float itMs(float secs) { return secs * 1000.0f; }

        void QueueAction(Action code, int sub, const char* arg);

        void PlainToast(const char* text, float secs)
        {
            if (!text || !text[0]) return;
            const float s = secs > 0.0f ? secs : 5.0f;
            if (!net::notify::Push(net::notify::NTF_SERVER, text, s))
                HUD::SHOW_NOTIFICATION(text, s, 1);
        }

        void FtPush(int kind, const char* id, const char* name, const char* text,
            float secs, uint64_t delayMs)
        {
            if (!ToastCardEnabled()) {
                if (kind == kToastGood || kind == kToastBad || kind == kToastInfo)
                    PlainToast(text, secs);
                return;
            }
            id = id ? id : "";
            name = name ? name : "";
            text = text ? text : "";
            for (int i = 0; i < g_queueN; i++)
                if (g_queue[i].kind == kind
                    && (id[0] ? !std::strcmp(g_queue[i].id, id)
                              : !std::strcmp(g_queue[i].text, text))) return;
            if (g_toast.st && g_toast.it.kind == kind
                && (id[0] ? !std::strcmp(g_toast.it.id, id)
                          : !std::strcmp(g_toast.it.text, text))) return;
            if (g_queueN >= kToastN) {
                int k = 0;
                for (int i = 0; i < g_queueN; i++)
                    if (g_queue[i].kind != kToastRequest && g_queue[i].kind != kToastInvite) {
                        k = i; break;
                    }
                std::memmove(g_queue + k, g_queue + k + 1,
                    static_cast<size_t>(g_queueN - k - 1) * sizeof(g_queue[0]));
                g_queueN--;
            }
            ToastItem& it = g_queue[g_queueN++];
            it = ToastItem{};
            it.kind = kind;
            std::snprintf(it.id, sizeof it.id, "%s", id);
            std::snprintf(it.name, sizeof it.name, "%s", name);
            std::snprintf(it.text, sizeof it.text, "%s", text);
            it.secs = secs;
            it.readyAt = Now() + delayMs;
        }

        bool FtActionable(int kind) { return kind == kToastRequest || kind == kToastInvite; }

        bool FtPending(const ToastItem& it)
        {
            if (!FtActionable(it.kind) || !g_net.have) return true;
            if (it.kind == kToastRequest) {
                for (int i = 0; i < g_state.ni; i++)
                    if (!std::strcmp(g_state.in[i].id, it.id)) return true;
            } else {
                for (int i = 0; i < g_state.npi; i++)
                    if (!std::strcmp(g_state.pinv[i].id, it.id)) return true;
            }
            return false;
        }

        // ext FtP: "anim-group.frToast.<leaf>" (a rotating static buffer like the
        // ext's 4-slot one; every caller consumes the returned pointer immediately).
        const char* FtP(const char* leaf)
        {
            static char b[4][64];
            static int n = 0;
            char* o = b[n++ & 3];
            std::snprintf(o, 64, "anim-group.frToast.%s", leaf);
            return o;
        }

        float FtEase(float k)
        {
            if (k < 0.0f) k = 0.0f;
            if (k > 1.0f) k = 1.0f;
            return 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
        }

        // ext FtCursor: the cursor relative to the card's RESTING place (its pivot
        // slides during the tween, but hover is measured against the final spot).
        bool FtCursor(float& rx, float& ry)
        {
            float cx = 0.0f, cy = 0.0f;
            if (!engine::window::CursorCanvas(&cx, &cy)) return false;
            rx = cx - kFtX;
            ry = cy - kFtY;
            return true;
        }

        // ext FtFill: fill every leaf of the card once (on show / page rebuild).
        // The widget leaves are tKick / tName / tSub / tInfo / tAc0..4 / tIc /
        // tKe[YN] / tKt[YN] / tLb[YN] / tMore / tBar (friends13_widgets.py).
        void FtCardFill()
        {
            if (!g_toast.st) return;
            const ToastItem& it = g_toast.it;
            const bool act = FtActionable(it.kind);
            for (int k = 0; k < 5; k++) {
                char l[8];
                std::snprintf(l, sizeof l, "tAc%d", k);
                UiVisible(FtP(l), k == it.kind);
            }
            const char* icon = it.kind == kToastRequest
                ? "streaming/t2r/icons/notify-revive.png"
                : it.kind == kToastBad ? "streaming/t2r/icons/notify-warn.png"
                : "streaming/t2r/icons/notify-info.png";
            engine::widget::SetImage(engine::widget::Page(FtP("tIc")), icon);
            UiText(FtP("tKick"), it.kind == kToastRequest
                ? Tr("FRIEND REQUEST", "ZAPROSZENIE DO ZNAJOMYCH")
                : it.kind == kToastInvite ? Tr("PARTY INVITE", "ZAPROSZENIE DO GRUPY")
                : Tr("FRIENDS", "ZNAJOMI"));
            UiVisible(FtP("tName"), act);
            UiVisible(FtP("tSub"), act);
            UiVisible(FtP("tInfo"), !act);
            if (act) {
                Fit(FtP("tName"), it.name, 640.0f, 54.0f);
                Fit(FtP("tSub"), it.kind == kToastRequest
                    ? Tr("WANTS TO BE YOUR FRIEND", "CHCE ZOSTAĆ TWOIM ZNAJOMYM")
                    : Tr("INVITES YOU TO A PARTY", "ZAPRASZA CIĘ DO GRUPY"), 650.0f, 33.0f);
                UiText(FtP("tInfo"), "");
            } else {
                UiText(FtP("tName"), "");
                UiText(FtP("tSub"), "");
                UiText(FtP("tInfo"), it.text);
            }
            for (int c = 0; c < 2; c++) {
                const char key = c ? 'N' : 'Y';
                char l[8];
                std::snprintf(l, sizeof l, "tKe%c", key); UiVisible(FtP(l), act);
                std::snprintf(l, sizeof l, "tKt%c", key); UiVisible(FtP(l), act);
                std::snprintf(l, sizeof l, "tLb%c", key); UiVisible(FtP(l), act);
                const char* lab = !act ? "" : c == 0
                    ? (it.kind == kToastRequest ? Tr("ACCEPT", "PRZYJMIJ") : Tr("JOIN", "DOŁĄCZ"))
                    : Tr("DECLINE", "ODRZUĆ");
                Fit(FtP(l), lab, 232.0f, 38.0f);
            }
            char m[16];
            std::snprintf(m, sizeof m, "+%d", g_queueN);
            UiText(FtP("tMore"), g_queueN > 0 ? m : "");
            engine::widget::Scale(engine::widget::Page(FtP("tBar")), 1.0f, 1.0f);
            g_toast.qa = g_toast.qx = g_toast.qb = -1;
            g_toast.qfy = g_toast.qfn = -1;
            g_toast.gfMs = 0;
        }

        void FtTick(uint64_t now, bool panelUp, bool menuVisible)
        {
            float dt = g_toast.last ? static_cast<float>(now - g_toast.last) : 16.0f;
            g_toast.last = now;
            if (dt > 100.0f) dt = 100.0f;

            // ext FtTick's FRIENDS-page branch: the page shows everything, so the
            // pending actionable cards are dropped and a showing one is ended.
            if (panelUp) {
                int k = 0;
                for (int i = 0; i < g_queueN; i++)
                    if (!FtActionable(g_queue[i].kind))
                        g_queue[k++] = g_queue[i];
                g_queueN = k;
                if (g_toast.st && FtActionable(g_toast.it.kind)) {
                    g_toast.st = 3;
                    g_toast.tOut = now;
                }
            }
            const bool ok = !panelUp && menuVisible;
            if (g_toast.st && !ok) {
                // the menu stopped showing: the card leaves at once (its remaining
                // time stays with the queued item), as in the ext.
                g_toast.st = 0;
                g_toast.cool = now + 350;
                UiVisible("anim-group.frToast", false);
                return;
            }
            const uint64_t gw = engine::widget::Page("anim-group.frToast");
            if (!g_toast.st) {
                if (!ok || now < g_toast.cool || !gw) return;
                int pick = -1;
                for (int i = 0; i < g_queueN && pick < 0; i++)
                    if (now >= g_queue[i].readyAt && FtPending(g_queue[i])) pick = i;
                if (pick < 0) return;
                g_toast.it = g_queue[pick];
                std::memmove(g_queue + pick, g_queue + pick + 1,
                    static_cast<size_t>(g_queueN - pick - 1) * sizeof(g_queue[0]));
                g_queueN--;
                g_toast.st = 1;
                g_toast.gw = gw;
                g_toast.t0 = now;
                g_toast.tOut = 0;
                g_toast.leftMs = g_toast.totalMs = itMs(g_toast.it.secs);
                g_toast.fy = g_toast.fn = 0.0f;
                FtCardFill();
                UiAlpha("anim-group.frToast", 0.0f);
                UiVisible("anim-group.frToast", true);
                return;
            }
            if (!gw) { g_toast.st = 0; return; }
            if (gw != g_toast.gw) {          // the page was rebuilt under us
                g_toast.gw = gw;
                FtCardFill();
                UiVisible("anim-group.frToast", true);
            }

            const bool act = FtActionable(g_toast.it.kind);
            float cx = 0.0f, cy = 0.0f;
            const bool cur = FtCursor(cx, cy);
            const bool hov = cur && cx >= 0.0f && cx <= kFtW && cy >= 0.0f && cy <= kFtH;
            const bool hy = act && hov && cx >= 852.0f && cx <= 852.0f + 340.0f
                && cy >= 26.0f && cy <= 88.0f;
            const bool hn = act && hov && cx >= 852.0f && cx <= 852.0f + 340.0f
                && cy >= 98.0f && cy <= 160.0f;
            // ext GuFade: exponential smoothing (tau 45 ms, dt capped 50), target
            // 0/1, quantised to 1/24, topped out at the widget's baked Fill alpha
            // (14/255, GuFadeMax): a hover only brightens the chip slightly.
            const float gfdt = g_toast.gfMs ? static_cast<float>(now - g_toast.gfMs) : 16.0f;
            g_toast.gfMs = now;
            const float gfk = 1.0f - std::exp(-(gfdt > 50.0f ? 50.0f : gfdt) / 45.0f);
            g_toast.fy += ((hy ? 1.0f : 0.0f) - g_toast.fy) * gfk;
            g_toast.fn += ((hn ? 1.0f : 0.0f) - g_toast.fn) * gfk;
            constexpr float kFillMax = 14.0f / 255.0f;
            const int qfy = static_cast<int>(g_toast.fy * 24.0f + 0.5f);
            const int qfn = static_cast<int>(g_toast.fn * 24.0f + 0.5f);
            if (qfy != g_toast.qfy) {
                g_toast.qfy = qfy;
                UiAlpha(FtP("tFillY"), static_cast<float>(qfy) / 24.0f * kFillMax);
            }
            if (qfn != g_toast.qfn) {
                g_toast.qfn = qfn;
                UiAlpha(FtP("tFillN"), static_cast<float>(qfn) / 24.0f * kFillMax);
            }

            if (g_toast.st != 3) {
                if (!hov)
                    g_toast.leftMs -= dt;     // the cursor on the card stops the clock
                if (g_toast.st == 1 && static_cast<float>(now - g_toast.t0) >= kFtInMs) {
                    g_toast.st = 2;
                    g_toast.tOut = now;
                }
                bool ky = KeyEdge('Y'), kn = KeyEdge('N');
                if (g_click && act && (hy || hn)) { ky = hy; kn = hn; }
                if (act && (ky || kn)) {
                    Action a = ky
                        ? (g_toast.it.kind == kToastRequest ? Action::Accept : Action::Join)
                        : (g_toast.it.kind == kToastRequest ? Action::DeclineRequest
                            : Action::DeclineParty);
                    const int sub = ky ? (g_toast.it.kind == kToastRequest ? 4 : 7)
                        : (g_toast.it.kind == kToastRequest ? 5 : 8);
                    QueueAction(a, sub, g_toast.it.id);
                    g_toast.st = 3;
                    g_toast.tOut = now;
                } else if (g_toast.leftMs <= 0.0f || (act && !FtPending(g_toast.it))) {
                    g_toast.st = 3;
                    g_toast.tOut = now;
                }
            }

            // ext FtTick's keyframes: slide in from +170 (ease-out cubic) + fade
            // over 260 ms, hold, then slide +70 (ease-in, p*p) + fade over 220 ms.
            // Alpha is quantised to 1/32 and the slide to 0.5 canvas units, so a
            // rebuilt page's re-assert only writes when the value actually changed.
            float a = 1.0f, dx = 0.0f;
            if (g_toast.st == 1) {
                const float e = FtEase(static_cast<float>(now - g_toast.t0) / kFtInMs);
                a = e;
                dx = (1.0f - e) * 170.0f;
            } else if (g_toast.st == 3) {
                const float p = static_cast<float>(now - g_toast.tOut) / kFtOutMs;
                if (p >= 1.0f) {
                    g_toast.st = 0;
                    g_toast.cool = now + 350;
                    UiVisible("anim-group.frToast", false);
                    return;
                }
                a = 1.0f - p;
                dx = p * p * 70.0f;
            }
            const int qa = static_cast<int>(a * 32.0f + 0.5f);
            const int qx = static_cast<int>(dx * 2.0f + 0.5f);
            if (qa != g_toast.qa) {
                g_toast.qa = qa;
                UiAlpha("anim-group.frToast", static_cast<float>(qa) / 32.0f);
            }
            if (qx != g_toast.qx) {
                g_toast.qx = qx;
                engine::widget::Pivot(gw, kFtX + static_cast<float>(qx) * 0.5f, kFtY);
            }
            const float frac = g_toast.totalMs > 0.0f ? g_toast.leftMs / g_toast.totalMs : 0.0f;
            const int qb = static_cast<int>((frac < 0.0f ? 0.0f : frac) * 200.0f + 0.5f);
            if (qb != g_toast.qb) {
                g_toast.qb = qb;
                const float f = static_cast<float>(qb) / 200.0f;
                engine::widget::Scale(engine::widget::Page(FtP("tBar")),
                    f > 0.002f ? f : 0.002f, 1.0f);
            }
            if (!(engine::frame::Count() % 90))
                g_toast.qa = g_toast.qx = -1;  // re-asserted twice a second (ext FtTick)
        }

        uint64_t Hash(const char* kind, const char* id)
        {
            uint64_t h = 1469598103934665603ULL;
            for (const char* p = kind; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
            for (const char* p = id; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
            return h;
        }
        bool SeenBefore(uint64_t h)
        {
            for (int i = 0; i < 32 && i < g_net.ntseen; i++)
                if (g_net.tseen[i] == h) return true;
            return false;
        }
        bool Noted(const char* name)
        {
            const uint64_t now = Now();
            for (const RecentLine& l : g_recent)
                if (l.text[0] && now - l.at < 600000 && name[0] && std::strstr(l.text, name))
                    return true;
            return false;
        }

        void ToastNewRequests(const State* d, bool panelUp)
        {
            int shown = 0;
            for (int k = 0; k < 2; k++) {
                const int n = k ? d->npi : d->ni;
                for (int i = 0; i < n; i++) {
                    const social::Request* r = k ? &d->pinv[i] : &d->in[i];
                    const uint64_t h = Hash(k ? "i" : "r", r->id);
                    if (SeenBefore(h)) continue;
                    g_net.tseen[g_net.ntseen++ & 31] = h;
                    if (panelUp) continue;
                    if (ToastCardEnabled()) {
                        if (shown >= 3) continue;
                        FtPush(k ? kToastInvite : kToastRequest, r->id, r->name, "",
                            k ? 16.0f : 12.0f, 0);
                        shown++;
                        continue;
                    }
                    if (shown >= 2 || Noted(r->name)) continue;
                    char m[200];
                    if (k)
                        std::snprintf(m, sizeof m, Tr("PARTY: %s invited you to a party. Press P to open FRIENDS and join.",
                            "GRUPA: %s zaprasza cię do grupy. Wciśnij P, aby otworzyć ZNAJOMYCH i dołączyć."), r->name);
                    else
                        std::snprintf(m, sizeof m, Tr("%s sent you a friend request. Press P to open FRIENDS.",
                            "%s wysyła ci zaproszenie do znajomych. Wciśnij P, aby otworzyć ZNAJOMYCH."), r->name);
                    PlainToast(m, 9.0f);
                    shown++;
                }
            }
        }

        // ---- RPC --------------------------------------------------------------------
        DWORD WINAPI Worker(LPVOID)
        {
            uint8_t job[kReqLen];
            std::memcpy(job, g_job, static_cast<size_t>(g_jobN));
            int res = 3;
            sockaddr_in a{};
            if (net::daemon::Addr(&a)) {
                uint8_t frame[2 + 1 + kReqLen];
                const int n = g_jobN + 1;
                frame[0] = static_cast<uint8_t>(n);
                frame[1] = static_cast<uint8_t>(n >> 8);
                frame[2] = kOpReq;
                std::memcpy(frame + 3, job, static_cast<size_t>(g_jobN));
                char rep[sizeof g_rep + 16];
                int sent = 0;
                const int got = net::daemon::Rpc(a, frame, n + 2, rep,
                    static_cast<int>(sizeof rep) - 1, 2500, 3500, 3500, &sent);
                if (sent) res = 2;
                const int ln = got >= 3
                    ? (rep[0] & 0xff) + ((rep[1] & 0xff) << 8) : 0;
                if (ln >= 2 && got >= ln + 2 && static_cast<uint8_t>(rep[2]) == kOpRep
                    && static_cast<size_t>(ln - 1) < kRepCap) {
                    std::memcpy(g_rep, rep + 3, static_cast<size_t>(ln - 1));
                    g_rep[ln - 1] = 0;
                    g_repLen.store(ln - 1);
                    res = 1;
                    net::daemon::MarkAlive();
                }
            }
            g_have.store(res);
            g_busy.store(0);
            return 0;
        }

        bool Start(int sub, const char* arg)
        {
            if (g_busy.load()) return false;
            if (!g_tok[0]) {
                const char* tok = net::account::Token();
                if (!tok || std::strlen(tok) < kTokenLen) return false;
                std::memcpy(g_tok, tok, kTokenLen);
                g_tok[kTokenLen] = 0;
            }
            std::memset(g_job, 0, sizeof g_job);
            std::memcpy(g_job, g_tok, kTokenLen);
            g_job[kTokenLen] = 1;                     // ver
            g_job[kTokenLen + 1] = static_cast<uint8_t>(Pl() ? 1 : 0);
            g_job[kTokenLen + 2] = static_cast<uint8_t>(sub);
            size_t al = arg ? std::strlen(arg) : 0;
            if (al > kArgMax) al = kArgMax;
            if (al) std::memcpy(g_job + kTokenLen + 3, arg, al);
            g_jobN = static_cast<int>(kTokenLen + 3 + al);

            g_busy.store(1);
            HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
            if (!h) { g_busy.store(0); return false; }
            CloseHandle(h);
            return true;
        }

        void Msg(const char* text, bool good)
        {
            std::snprintf(g_net.msg, sizeof g_net.msg, "<font color=\"#%s\">%s</font>",
                good ? "7CB494" : "F2625A", text ? text : "");
            g_net.msgUntil = Now() + 6000;
            g_panel.dirty = true;
        }

        void QueueAction(Action code, int sub, const char* arg)
        {
            g_net.actSub = sub;
            std::snprintf(g_net.actArg, sizeof g_net.actArg, "%s", arg ? arg : "");
            g_net.actCode = code;
            g_net.actPending = true;
            g_net.nfBefore = g_state.nf;
            Sfx("sfx-select");
            TO1_LOG("friendsui: action %d (sub %d %s)\n", static_cast<int>(code), sub,
                sub == 3 ? "<typed>" : g_net.actArg);
        }

        void NetTick(uint64_t now, bool panelUp, bool menuOk)
        {
            const int have = g_have.exchange(0);
            if (have == 1) {
                State tmp;
                if (social::Parse(&tmp, g_rep, static_cast<size_t>(g_repLen.load()))) {
                    if (g_net.actInflight) {
                        if (!tmp.ok) {
                            Msg(social::ErrorText(tmp.err), false);
                            Sfx("sfx-menu-error");
                            if (!panelUp && ToastCardEnabled())
                                FtPush(kToastBad, "", "", social::ErrorText(tmp.err), 5.5f, 0);
                        } else {
                            const char* ok = social::OkText(g_net.actCode, tmp.nf > g_net.nfBefore);
                            Msg(ok, true);
                            if (!panelUp && ToastCardEnabled() && ok[0])
                                FtPush(kToastGood, "", "", ok, 4.5f, 0);
                        }
                        g_net.actInflight = false;
                    }
                    g_state = tmp;
                    g_net.have = true;
                    ++g_net.ver;
                    g_net.okAt = now;
                    g_net.fails = 0;
                    g_net.okOnce = 1;
                    if (g_state.nf != g_net.lastNf || g_state.nm != g_net.lastNm
                        || g_state.ni != g_net.lastNi || g_state.npi != g_net.lastNpi) {
                        g_net.lastNf = g_state.nf;
                        g_net.lastNm = g_state.nm;
                        g_net.lastNi = g_state.ni;
                        g_net.lastNpi = g_state.npi;
                        TO1_LOG("friendsui: state: %d friends, %d requests in, %d out, %d party invites, party of %d%s\n",
                            g_state.nf, g_state.ni, g_state.no, g_state.npi, g_state.nm,
                            g_state.meLeader ? " (leader)" : "");
                    }
                    ToastNewRequests(&g_state, panelUp);
                    g_panel.dirty = true;
                } else {
                    g_net.actInflight = false;
                    g_net.nextPoll = now + 10000;
                }
            } else if (have == 2 || have == 3) {
                g_net.fails++;
                if (g_net.actInflight) {
                    Msg(Tr("NO ANSWER FROM THE SERVER", "SERWER NIE ODPOWIADA"), false);
                    if (!panelUp && ToastCardEnabled())
                        FtPush(kToastBad, "", "", Tr("NO ANSWER FROM THE SERVER", "SERWER NIE ODPOWIADA"),
                            5.5f, 0);
                    g_net.actInflight = false;
                }
                g_net.nextPoll = now + (g_net.fails < 3 ? 6000 : 20000);
                if (have == 2 && net::daemon::AliveWithin(60000) && g_net.fails >= 3
                    && !g_net.okOnce) {
                    g_net.dead = 1;
                    TO1_LOG("friendsui: the server does not answer op 0xD8 (older server) - not asking again\n");
                }
                g_panel.dirty = true;
            }
            if (g_net.dead || g_busy.load()) return;
            if (g_net.actPending) {
                if (Start(g_net.actSub, g_net.actArg)) {
                    g_net.actPending = false;
                    g_net.actInflight = true;
                }
                return;
            }
            if (!menuOk || now < g_net.nextPoll) return;
            if (Start(1, ""))
                g_net.nextPoll = now + (panelUp ? 3000 : g_state.hasParty ? 2500 : 15000);
        }

        // ---- panel ------------------------------------------------------------------
        int LeftN() { return g_state.hasParty ? g_state.nm + 2 : 0; }

        void RowPath(char* out, size_t n, int col, int i, const char* leaf)
        {
            std::snprintf(out, n, "anim-group.%s.%cRow%d.%c%s%d",
                col ? "rt" : "lf", col ? 'r' : 'l', i, col ? 'r' : 'l', leaf, i);
        }

        void SetRow(int col, int i, bool vis, int tone, const char* name,
            const char* st, const char* tag, const char* ac, bool foc, bool hov,
            bool playerRow = true)
        {
            char g[96], p[160], rp[160];
            std::snprintf(g, sizeof g, "anim-group.%s.%cRow%d", col ? "rt" : "lf",
                col ? 'r' : 'l', i);
            UiVisible(g, vis);
            if (!vis) return;
            for (int k = 0; k < 5; k++) {
                std::snprintf(p, sizeof p, "%s.%cBd%d_%d", g, col ? 'r' : 'l', k, i);
                UiVisible(p, k == tone);
            }
            // rank badge (ext FrSetRow): a PLAYER row shows the 0xBF badge and its
            // name moves right by the reserved width; an action row ("ADD A FRIEND"
            // ...) keeps the plain layout (no rank-0 fallback). rRk/rRn leaves come
            // from rank_badge_widgets.py; absent widgets make every write inert.
            RowPath(p, sizeof p, col, i, "Rk");
            RowPath(rp, sizeof rp, col, i, "Rn");
            const bool hasBadge = ::net::rank_badge::DrawBadge(p, rp, name, playerRow);
            RowPath(p, sizeof p, col, i, "Nm");
            engine::widget::Pivot(engine::widget::Page(p),
                kNamePivotX + (hasBadge ? kRankRes46 : 0.0f), kNamePivotY);
            Fit(p, name, (col ? 920.0f : 680.0f) - (hasBadge ? kRankRes46 : 0.0f), 47.0f);
            RowPath(p, sizeof p, col, i, "St");  Fit(p, st, col ? 850.0f : 620.0f, 33.0f);
            RowPath(p, sizeof p, col, i, "Tg");  Fit(p, tag, 540.0f, 33.0f);
            RowPath(p, sizeof p, col, i, "Ac");  Fit(p, ac, 560.0f, 33.0f);
            RowPath(p, sizeof p, col, i, "Fill"); UiVisible(p, foc || hov);
            RowPath(p, sizeof p, col, i, "Frame"); UiVisible(p, foc);
        }

        void Window(int n)
        {
            if (n <= 0) { g_panel.foc[1] = g_panel.off[1] = 0; return; }
            if (g_panel.foc[1] >= n) g_panel.foc[1] = n - 1;
            if (g_panel.foc[1] < 0) g_panel.foc[1] = 0;
            if (g_panel.foc[1] < g_panel.off[1]) g_panel.foc[1] = g_panel.off[1];
            if (g_panel.foc[1] >= g_panel.off[1] + kRightRows)
                g_panel.off[1] = g_panel.foc[1] - kRightRows + 1;
            if (g_panel.off[1] > n - kRightRows) g_panel.off[1] = n - kRightRows;
            if (g_panel.off[1] < 0) g_panel.off[1] = 0;
        }

        int RightRows(Row* r)
        {
            return social::Rows(&g_state, r,
                1 + social::kMaxInvites + 2 * kRightRows + social::kMaxFriends);
        }

        // ext FrScan: hit-test the panel's rows and footer prompts in the 3840x2160
        // canvas. The boxes are pinned to friends13_widgets.py (ext FR_NL=6 /
        // FR_RROWS=7 -- the same rows Fill() draws); no widget lookup is needed, so a
        // missing GUI pak only hides the highlight, it never faults. A right-list row
        // scrolled out of the window is not hoverable (ext's off[1] + row >= rows).
        void ScanHover(int ln, int nr)
        {
            g_hvCol = g_hvRow = -1;
            g_hvBack = g_hvSel = g_hvAct = false;
            if (!engine::flag::Absent("t2r-no-mouse-nav.txt")) return;
            float cx = 0.0f, cy = 0.0f;
            if (!engine::window::CursorCanvas(&cx, &cy)) return;
            constexpr int kLY[kLeftRows] = { 540, 710, 880, 1050, 1260, 1410 };
            constexpr int kLH[kLeftRows] = { 150, 150, 150, 150, 130, 130 };
            if (cx >= 300.0f && cx <= 1700.0f)
                for (int i = 0; i < ln && i < kLeftRows; i++)
                    if (cy >= kLY[i] && cy <= kLY[i] + kLH[i])
                        g_hvCol = 0, g_hvRow = i;
            if (cx >= 1900.0f && cx <= 3540.0f)
                for (int i = 0; i < kRightRows; i++)
                    if (cy >= 540.0f + 160.0f * i && cy <= 680.0f + 160.0f * i)
                        g_hvCol = 1, g_hvRow = i;
            if (g_hvCol == 1 && g_panel.off[1] + g_hvRow >= nr)
                g_hvCol = g_hvRow = -1;
            g_hvBack = cx >= 250.0f && cx <= 730.0f && cy >= 1880.0f && cy <= 1970.0f;
            g_hvSel  = cx >= 740.0f && cx < 1500.0f && cy >= 1880.0f && cy <= 1970.0f;
            g_hvAct  = cx >= 1500.0f && cx < 2260.0f && cy >= 1880.0f && cy <= 1970.0f;
        }

        Action RowPrimary(int col, const Row* rows, int nr);
        Action RowSecondary(int col, const Row* rows, int nr);

        void Fill()
        {
            char a[240], b[240], c[240], d[240];
            UiText("anim-group.hd.hdKick", Tr("PARTY AND FRIENDS", "GRUPA I ZNAJOMI"));
            UiText("anim-group.hd.hdTitle", Tr("FRIENDS", "ZNAJOMI"));
            const bool show = g_net.have;
            const char* ld = !show
                ? (!g_busy.load() && g_net.fails >= 2
                    ? Tr("THE SERVER DID NOT ANSWER", "SERWER NIE ODPOWIADA")
                    : Tr("LOADING YOUR FRIENDS", "WCZYTYWANIE ZNAJOMYCH")) : "";
            UiText("anim-group.ldTx", ld);
            UiVisible("anim-group.ldTx", !show);
            UiVisible("anim-group.lf", show);
            UiVisible("anim-group.rt", show);
            UiText("anim-group.pBackTx", Tr("BACK", "WSTECZ"));
            if (!show) return;
            Fit("anim-group.hd.hdMe", g_state.meName, 1300.0f, 47.0f);
            std::snprintf(a, sizeof a, Tr("YOUR FRIEND ID  %s", "TWOJE ID  %s"), g_state.meId);
            Fit("anim-group.hd.hdId", a, 1400.0f, 33.0f);
            int ln = LeftN();
            if (g_panel.foc[0] >= ln) g_panel.foc[0] = ln ? ln - 1 : 0;
            if (!ln && g_panel.col == 0) g_panel.col = 1;
            UiText("anim-group.lf.lTitle", Tr("PARTY", "GRUPA"));
            if (g_state.hasParty) {
                std::snprintf(a, sizeof a, "%d / %d%s%s", g_state.nm,
                    g_state.pmax ? g_state.pmax : 4,
                    g_state.meLeader ? Tr("  -  YOU ARE THE LEADER", "  -  JESTEŚ LIDEREM") : "",
                    g_state.searching ? Tr("  -  SEARCHING", "  -  SZUKA MECZU") : "");
                Fit("anim-group.lf.lSub", a, 980.0f, 33.0f);
            } else UiText("anim-group.lf.lSub", "");
            UiText("anim-group.lf.lNone", Tr("NO PARTY YET. INVITE A FRIEND FROM THE LIST ON THE RIGHT - A PARTY QUEUES TOGETHER AND PLAYS ON ONE TEAM.",
                "JESZCZE NIE MASZ GRUPY. ZAPROŚ ZNAJOMEGO Z LISTY PO PRAWEJ - GRUPA SZUKA MECZU RAZEM I GRA W JEDNEJ DRUŻYNIE."));
            UiVisible("anim-group.lf.lNone", !ln);

            Row rows[1 + social::kMaxInvites + 2 * kRightRows + social::kMaxFriends];
            int nr = RightRows(rows);
            Window(nr);
            for (int i = 0; i < kLeftRows; i++) {
                const bool foc = g_panel.col == 0 && g_panel.foc[0] == i;
                if (i >= ln) { SetRow(0, i, false, 0, "", "", "", "", false, false); continue; }
                Action pri = Action::None, sec = Action::None;
                social::RowActions(&g_state, 0, i, rows, nr, pri, sec);
                if (i < g_state.nm) {
                    const social::Member& m = g_state.m[i];
                    std::snprintf(a, sizeof a, "%s%s", m.name, m.me ? Tr("  (YOU)", "  (TY)") : "");
                    const char* word = social::StatusWord(m.status, b, sizeof b);
                    std::snprintf(c, sizeof c, "%s%s%s%s",
                        m.leader ? "<font color=\"#3FA9F5\">" : "",
                        i == 0 && g_state.hasParty ? Tr("LEADER  ", "LIDER  ") : "",
                        m.ready ? Tr("READY  ", "GOTOWY  ") : "", word);
                    SetRow(0, i, true, social::Tone(social::Presence(m.status)), a, c,
                        i == 0 && g_state.hasParty ? Tr("PARTY", "GRUPA") : "",
                        social::ActionLabel(&g_state, pri), foc, g_hvCol == 0 && g_hvRow == i);
                } else if (i == g_state.nm) {
                    SetRow(0, i, true, 0, Tr("READY UP", "GOTOWY"), "", "",
                        social::ActionLabel(&g_state, pri), foc,
                        g_hvCol == 0 && g_hvRow == i, false);
                } else {
                    SetRow(0, i, true, 0, g_state.meLeader ? Tr("DISBAND PARTY", "ROZWIĄŻ GRUPĘ")
                        : Tr("LEAVE PARTY", "OPUŚĆ GRUPĘ"), "", "",
                        social::ActionLabel(&g_state, pri), foc,
                        g_hvCol == 0 && g_hvRow == i, false);
                }
            }
            for (int k = 0; k < kRightRows; k++) {
                const int i = g_panel.off[1] + k;
                const bool foc = g_panel.col == 1 && g_panel.foc[1] == i;
                if (i >= nr) { SetRow(1, k, false, 0, "", "", "", "", false, false); continue; }
                const Row& r = rows[i];
                Action pri = Action::None, sec = Action::None;
                social::RowActions(&g_state, 1, i, rows, nr, pri, sec);
                const char* name = "", *st = "", *tag = "";
                int tone = 0;
                bool playerRow = true;   // ADD A FRIEND is an action row (no rank fallback)
                if (r.kind == RowKind::Add) {
                    name = Tr("ADD A FRIEND", "DODAJ ZNAJOMEGO");
                    tag = Tr("TYPE A NAME OR A STEAM ID", "WPISZ NAZWĘ LUB ID STEAM");
                    playerRow = false;
                } else if (r.kind == RowKind::PartyInvite) {
                    const social::Request& q = g_state.pinv[r.idx];
                    name = q.name; tag = Tr("PARTY INVITE", "ZAPROSZENIE DO GRUPY");
                } else if (r.kind == RowKind::Request) {
                    const social::Request& q = g_state.in[r.idx];
                    name = q.name; tag = Tr("FRIEND REQUEST", "ZAPROSZENIE DO ZNAJOMYCH");
                } else if (r.kind == RowKind::Friend) {
                    const social::Friend& f = g_state.friends[r.idx];
                    name = f.name;
                    st = social::StatusWord(f.status, b, sizeof b);
                    tone = social::Tone(social::Presence(f.status));
                } else if (r.kind == RowKind::Outgoing) {
                    name = g_state.out[r.idx].name;
                    tag = Tr("REQUEST SENT", "ZAPROSZENIE WYSŁANE");
                }
                std::snprintf(d, sizeof d, "%s", social::ActionLabel(&g_state, pri));
                SetRow(1, k, true, tone, name, st, tag, d, foc,
                    g_hvCol == 1 && g_hvRow == k, playerRow);
            }
            if (Now() < g_net.msgUntil) Fit("anim-group.msgTx", g_net.msg, 1700.0f, 33.0f);
            else UiText("anim-group.msgTx", "");
            Fit("anim-group.pSelTx", social::ActionLabel(&g_state, RowPrimary(g_panel.col, rows, nr)), 900.0f, 33.0f);
            Fit("anim-group.pActTx", social::ActionLabel(&g_state, RowSecondary(g_panel.col, rows, nr)), 900.0f, 33.0f);
        }

        Action RowPrimary(int col, const Row* rows, int nr)
        {
            Action pri = Action::None, sec = Action::None;
            const int foc = col ? g_panel.foc[1] : g_panel.foc[0];
            if (foc >= 0 && (col ? foc < nr : true))
                social::RowActions(&g_state, col, foc, rows, nr, pri, sec);
            return pri;
        }
        Action RowSecondary(int col, const Row* rows, int nr)
        {
            Action pri = Action::None, sec = Action::None;
            const int foc = col ? g_panel.foc[1] : g_panel.foc[0];
            if (foc >= 0 && (col ? foc < nr : true))
                social::RowActions(&g_state, col, foc, rows, nr, pri, sec);
            return sec;
        }

        void PageAlpha(float f)
        {
            UiAlpha("anim-group", f);
        }

        void Enter()
        {
            g_panel = PanelState{};
            g_panel.up = true;
            g_panel.enterMs = Now();
            g_hSel = g_hUp = g_hDown = g_hLeft = g_hRight = false;
            g_hBack = g_hPrev = g_hNext = g_hAct = false;
            g_repUp = g_repDown = 0;
            Sfx("sfx-open");
        }
        void Close()
        {
            g_panel.closing = true;
            g_panel.closeMs = Now();
        }

        // ---- text entry -------------------------------------------------------------
        void Edited() { g_panel.editMs = Now(); g_panel.dirty = true; }

        int PrevCh(int i)
        {
            if (i > 0) i--;
            while (i > 0 && (static_cast<unsigned char>(g_panel.buf[i]) & 0xC0) == 0x80) i--;
            return i;
        }
        int NextCh(int i)
        {
            if (i < g_panel.blen) i++;
            while (i < g_panel.blen && (static_cast<unsigned char>(g_panel.buf[i]) & 0xC0) == 0x80) i++;
            return i;
        }
        void Cut(int from, int to)
        {
            if (from < 0 || to > g_panel.blen || from >= to) return;
            std::memmove(g_panel.buf + from, g_panel.buf + to,
                static_cast<size_t>(g_panel.blen - to + 1));
            g_panel.blen -= to - from;
            g_panel.caret = from;
            Edited();
        }
        bool Allowed(unsigned char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_'
                || c == '.' || c == ':' || c == '[' || c == ']' || c >= 0x80;
        }
        void Insert(const char* t, int n)
        {
            char f[128];
            int fn = 0;
            for (int i = 0; i < n && fn < static_cast<int>(sizeof f); i++)
                if (Allowed(static_cast<unsigned char>(t[i]))) f[fn++] = t[i];
            if (fn <= 0 || g_panel.blen + fn > 39) return;
            std::memmove(g_panel.buf + g_panel.caret + fn, g_panel.buf + g_panel.caret,
                static_cast<size_t>(g_panel.blen - g_panel.caret + 1));
            std::memcpy(g_panel.buf + g_panel.caret, f, static_cast<size_t>(fn));
            g_panel.caret += fn;
            g_panel.blen += fn;
            g_panel.buf[g_panel.blen] = 0;
            Edited();
        }
        int VkChar(int vk, bool shift)
        {
            if (vk >= 'A' && vk <= 'Z') return shift ? vk : vk + 32;
            if (vk >= '0' && vk <= '9') return vk;
            if (vk == VK_SPACE) return ' ';
            if (vk == VK_OEM_MINUS) return shift ? '_' : '-';
            if (vk == VK_OEM_PERIOD) return shift ? ':' : '.';
            if (vk == VK_OEM_1) return shift ? ':' : ';';
            if (vk == VK_OEM_4) return shift ? '{' : '[';
            if (vk == VK_OEM_6) return shift ? '}' : ']';
            return 0;
        }
        void Paste()
        {
            if (!OpenClipboard(nullptr)) return;
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) {
                const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
                if (w) {
                    char utf8[128];
                    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof utf8, nullptr, nullptr);
                    if (n > 0) Insert(utf8, n - 1);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        void TypingTick(uint64_t now)
        {
            if (KeyEdge(VK_LEFT))  g_panel.caret = PrevCh(g_panel.caret);
            if (KeyEdge(VK_RIGHT)) g_panel.caret = NextCh(g_panel.caret);
            if (KeyEdge(VK_HOME))  g_panel.caret = 0;
            if (KeyEdge(VK_END))   g_panel.caret = g_panel.blen;
            if (KeyEdge(VK_BACK))  Cut(PrevCh(g_panel.caret), g_panel.caret);
            if (KeyEdge(VK_DELETE)) Cut(g_panel.caret, NextCh(g_panel.caret));
            if (KeyDown(VK_CONTROL) && KeyEdge('V')) Paste();
            else {
                const bool shift = KeyDown(VK_SHIFT);
                for (int vk = '0'; vk <= 'Z'; vk++) {
                    if (!KeyEdge(vk)) continue;
                    int c = VkChar(vk, shift);
                    if (c) { char b[2] = { static_cast<char>(c), 0 }; Insert(b, 1); }
                }
                const int oem[] = { VK_OEM_MINUS, VK_OEM_PERIOD, VK_OEM_1, VK_OEM_4, VK_OEM_6 };
                for (int vk : oem) {
                    if (!KeyEdge(vk)) continue;
                    int c = VkChar(vk, shift);
                    if (c) { char b[2] = { static_cast<char>(c), 0 }; Insert(b, 1); }
                }
            }
            Fit("anim-group.rt.inp.inpLbl", Tr("ADD A FRIEND  -  PLAYER NAME OR STEAM ID",
                "DODAJ ZNAJOMEGO  -  NAZWA GRACZA LUB ID STEAM"), 1060.0f, 33.0f);
            Fit("anim-group.rt.inp.inpPh", g_panel.blen ? "" :
                Tr("TYPE A NAME OR A STEAM ID", "WPISZ NAZWĘ LUB ID STEAM"), 1060.0f, 47.0f);
            UiText("anim-group.rt.inp.inpTx", g_panel.buf);
            const bool caretOn = now - g_panel.editMs < 500;
            UiVisible("anim-group.rt.inp.inpCar", caretOn);
        }
        void TypingStart()
        {
            g_panel.typing = true;
            g_panel.blen = g_panel.caret = 0;
            g_panel.buf[0] = 0;
            g_panel.typMs = g_panel.editMs = Now();
            Edited();
        }
        void TypingStop() { g_panel.typing = false; }
        void SendTyped()
        {
            while (g_panel.blen > 0 && g_panel.buf[g_panel.blen - 1] == ' ') g_panel.buf[--g_panel.blen] = 0;
            if (g_panel.blen < 2) {
                Msg(Tr("TYPE AT LEAST 2 CHARACTERS", "WPISZ CO NAJMNIEJ 2 ZNAKI"), false);
                Sfx("sfx-menu-error");
                g_panel.caret = g_panel.blen;
                Edited();
                return;
            }
            char t[40];
            std::snprintf(t, sizeof t, "%s", g_panel.buf);
            QueueAction(Action::Add, 3, t);
            TypingStop();
        }

        void Do(Action code, int col, int idx, const Row* rows, int nr)
        {
            if (code == Action::None) return;
            char id[44] = "";
            if (col == 0 && idx < g_state.nm)
                std::snprintf(id, sizeof id, "%s", g_state.m[idx].id);
            else if (col == 1 && idx >= 0 && idx < nr) {
                const Row& r = rows[idx];
                const char* src = r.kind == RowKind::PartyInvite ? g_state.pinv[r.idx].id
                    : r.kind == RowKind::Request ? g_state.in[r.idx].id
                    : r.kind == RowKind::Friend ? g_state.friends[r.idx].id
                    : r.kind == RowKind::Outgoing ? g_state.out[r.idx].id : "";
                std::snprintf(id, sizeof id, "%s", src);
            }
            switch (code) {
            case Action::Type:    TypingStart(); Sfx("sfx-select"); break;
            case Action::Invite:  QueueAction(code, 6, id); break;
            case Action::Accept:  QueueAction(code, 4, id); break;
            case Action::Join:    QueueAction(code, 7, id); break;
            case Action::DeclineParty:
            case Action::DeclineRequest:
            case Action::Remove:
            case Action::Cancel:  QueueAction(code, 5, id); break;
            case Action::Promote: QueueAction(code, 11, id); break;
            case Action::Kick:    QueueAction(code, 10, id); break;
            case Action::Ready:   QueueAction(code, 12, social::MyReady(&g_state) ? "0" : "1"); break;
            case Action::Leave:   QueueAction(code, 9, ""); break;
            default: break;
            }
        }

        void PageTick(uint64_t now, uint64_t top)
        {
            (void) top;
            if (!g_panel.up) Enter();
            if (g_panel.closing) {
                if (g_panel.closeMs && now - g_panel.closeMs >= 170) {
                    PopPage();
                    g_panel.closing = false;
                    g_panel.up = false;
                    g_panel.closeMs = 0;
                    return;
                }
                PageAlpha(1.0f - static_cast<float>(now - g_panel.closeMs) / 170.0f);
                return;
            }
            bool selEdge = MenuEdge(g_held.sel, g_hSel);
            bool upEdge = Repeat(g_held.up, g_hUp, g_repUp, now);
            bool downEdge = Repeat(g_held.down, g_hDown, g_repDown, now);
            bool leftEdge = MenuEdge(g_held.left, g_hLeft);
            bool rightEdge = MenuEdge(g_held.right, g_hRight);
            bool back = MenuEdge(g_held.back, g_hBack);
            bool prev = MenuEdge(g_held.prev, g_hPrev);
            bool next = MenuEdge(g_held.next, g_hNext);
            bool act = MenuEdge(g_held.act, g_hAct);
            bool esc = KeyEdge(VK_ESCAPE);

            // Click-to-nav (ext FrScan + the click_f gate): a row click ONLY moves the
            // selection -- a mouse-sourced menu-select always belongs to the engine's
            // focused row, so it is dropped for a few frames after any click. An action
            // then needs the footer prompt (hv_sel / hv_act) or a key / pad button.
            bool click = g_click;
            if (click) g_clickFrame = engine::frame::Count() ? engine::frame::Count() : 1;

            Row rows[1 + social::kMaxInvites + 2 * kRightRows + social::kMaxFriends];
            int nr = RightRows(rows);
            int ln = LeftN();
            ScanHover(ln, nr);

            if (g_panel.typing) {
                TypingTick(now);
                if (esc || back) { TypingStop(); esc = back = false; }
                else if (KeyEdge(VK_RETURN)) SendTyped();
                else if (click && !g_hvBack
                    && !(g_hvCol == 1 && g_hvRow == 0 && g_panel.off[1] == 0))
                    TypingStop();                  // a click elsewhere leaves the field
                back = false;
                if (!g_hvBack) click = false;      // BACK still closes while typing
                selEdge = upEdge = downEdge = leftEdge = rightEdge = 0;
                prev = next = act = false;
            }
            if (esc || (click && g_hvBack)) { Close(); return; }
            if (back) g_panel.backFrame = engine::frame::Count() ? engine::frame::Count() : 1;
            if (g_panel.backFrame && engine::frame::Count() >= g_panel.backFrame + 6) {
                g_panel.backFrame = 0;
                Close();
                return;
            }
            if (KeyEdge('P')) { Close(); return; }

            bool x = KeyEdge('X') || KeyEdge(VK_DELETE);
            bool r = KeyEdge('R'), l = KeyEdge('L');
            bool sw = KeyEdge(VK_TAB) || KeyEdge('Q') || KeyEdge('E');
            if (!g_net.have) { selEdge = upEdge = downEdge = x = r = l = 0; }
            if (selEdge && g_clickFrame && engine::frame::Count() - g_clickFrame <= 8)
                selEdge = false;                   // the engine's select rode on the click
            int col = g_panel.col, n = col ? nr : ln, foc = g_panel.foc[col];
            if (upEdge && n) { foc = (foc + n - 1) % n; Sfx("sfx-cycle"); }
            if (downEdge && n) { foc = (foc + 1) % n; Sfx("sfx-cycle"); }
            if (sw || prev || next || leftEdge || rightEdge) {
                if (ln) { col = !col; g_panel.col = col; n = col ? nr : ln; foc = g_panel.foc[col]; Sfx("sfx-cycle"); }
            }
            bool clkRow = false;
            if (click && g_hvRow >= 0) {
                const int ci = g_hvCol, ri = g_hvCol ? g_panel.off[1] + g_hvRow : g_hvRow;
                col = ci; g_panel.col = col;       // hovered row becomes the focus
                foc = ri;
                n = col ? nr : ln;
                Sfx("sfx-cycle");
                clkRow = true;
            }
            if (click && !clkRow && g_hvSel && !g_panel.typing) selEdge = true;
            if (click && !clkRow && g_hvAct && !g_panel.typing) act = true;
            if (foc < 0) foc = 0;
            if (n && foc >= n) foc = n - 1;
            g_panel.foc[col] = foc;
            Action pri = Action::None, sec = Action::None;
            social::RowActions(&g_state, col, foc, rows, nr, pri, sec);
            if (r) { Action ap, as; social::RowActions(&g_state, 0, 0, rows, nr, ap, as); Do(ap, 0, 0, rows, nr); }
            if (l) { Action ap, as; social::RowActions(&g_state, 0, g_state.nm + 1, rows, nr, ap, as); Do(ap, 0, g_state.nm + 1, rows, nr); }
            if (selEdge) Do(pri, col, foc, rows, nr);
            if (x || act) Do(sec, col, foc, rows, nr);
            PageAlpha(1.0f);
            Fill();
        }

        // ---- party rule -------------------------------------------------------------
        bool InPartyState() { return g_net.have && g_state.hasParty && g_state.nm >= 2; }
        bool IsMemberState() { return InPartyState() && !g_state.meLeader; }
        bool ReadyState() { return social::MyReady(&g_state) != 0; }

        bool GateSearch()
        {
            if (!g_net.have || !g_state.hasParty || g_state.nm < 2) return false;
            const uint64_t now = Now();
            if (g_state.meLeader) {
                for (int i = 0; i < g_state.nm; i++)
                    if (!g_state.m[i].me && !g_state.m[i].leader && !g_state.m[i].ready)
                        TO1_LOG("friendsui: leader searches, %s is not ready yet (the daemon waits for it)\n",
                            g_state.m[i].name);
                return false;
            }
            if (g_autoPush && now - g_autoPush < kAutoWindowMs) {
                g_autoPush = 0;
                return false;
            }
            TO1_LOG("friendsui: a member's search request outside the follow-the-leader push - page popped, ready state unchanged\n");
            return true;
        }
        void NoteAutoPushState() { g_autoPush = Now(); }

        void ReadyPress()
        {
            const uint64_t now = Now();
            if (now - g_lastMemberPress < kActionCoolMs) return;
            g_lastMemberPress = now;
            const bool r = ReadyState();
            QueueAction(Action::Ready, 12, r ? "0" : "1");
            TO1_LOG("friendsui: member's FIND MATCH = %s%s\n", r ? "UNREADY" : "READY UP",
                !r && g_state.lsearch ? " (the leader searches: we follow)" : "");
        }

        // The daemon snapshot gives us the party state, ready bits and the leader's
        // search flag, so the follow rule itself is native; kPartyStatus (ext's raw
        // session byte) is mapped onto net::matchmaking::State. MATCH FOUND team tabs
        // / names now come from engine::lobby (RosterSorted / RosterName) in
        // LobbyDecorations. Only the SEARCHERS' table stays gated: mod has no producer
        // for the daemon's relative [T1]/[T2] markers (ext g_srchTab / g_srchName).
        int PartyStatus()
        {
            const auto st = net::matchmaking::GetState();
            if (st == net::matchmaking::State::Matched) return 3;
            if (st == net::matchmaking::State::Searching) return 2;
            return 1;
        }

        bool MenuVisible()
        {
            if (net::match::liveness::Live() || net::match::liveness::MatchEndHold()) return false;
            const uint64_t top = engine::widget::TopPage();
            return top == PageId("main-menu") || top == PageId("find-match-menu");
        }

        void PartyTick(uint64_t now, uint64_t top, uint64_t pMain)
        {
            static int lat = 0, inSearch = 0, prevReady = 0;
            static uint64_t pushedAt = 0, fakeAt = 0;
            static bool labelSet = false;
            const bool mem = IsMemberState() && MenuVisible();
            if (mem || labelSet) {
                const char* t = mem ? (ReadyState() ? Tr("UNREADY", "NIEGOTOWY") : Tr("READY UP", "GOTOWY"))
                                    : "FIND MATCH";
                if (top == pMain) {
                    UiText("anim-group.menuList.FindMatch.label0", t);
                    labelSet = mem;
                }
            }
            int mePresence = -1;
            for (int i = 0; i < g_state.nm; i++)
                if (g_state.m[i].me) mePresence = social::Presence(g_state.m[i].status);
            const uint64_t pFm = PageId("find-match-menu");
            if (InPartyState() && top == pFm && mePresence >= 0 && mePresence < 3
                && now - g_autoPush > kFakeSearchMs) {
                if (!fakeAt) fakeAt = now;
                else if (now - fakeAt > kFakeSearchMs) {
                    fakeAt = 0; lat = inSearch = 0;
                    if (MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, pFm)) PopPage();
                }
            } else fakeAt = 0;

            const int r = ReadyState() ? 1 : 0;
            if (r && !prevReady) lat = 0;
            prevReady = r;
            const bool ls = mem && g_state.lsearch != 0;
            if (!ls) { lat = inSearch = 0; return; }
            const int st = PartyStatus();
            if (inSearch) {
                if (top == pMain && st < 3 && now - pushedAt > 4000) {
                    inSearch = 0;
                    QueueAction(Action::Ready, 12, "0");
                }
                return;
            }
            if (lat || !r || top != pMain || st != 2) return;
            lat = inSearch = 1;
            pushedAt = g_autoPush = now;
            PushPage("to1/find-match-menu");
            TO1_LOG("friendsui: the leader searches and we are ready - search page pushed (the party queues together)\n");
        }

        void LobbyDecorations(uint64_t top)
        {
            // ext FrLobbyTabs / FrLobbyNames / FrLobbyPartyRows. The MATCH FOUND
            // roster is derivable from engine::lobby (RosterSorted / RosterName):
            // row 0 = us (blue), rows 1.. = our team first (blue), the enemy after
            // (red). The SEARCHING rows' [T1]/[T2] / "not searching" markers come
            // from the read-only g_srchTab / g_srchName mirror (search-rows.*),
            // observed on the mod's own text writes -- no competing settext hook.
            static const char* const kTab[5] =
                { "", "tabGrey", "tabGreen", "tabBlue", "tabRed" };
            int idx[engine::lobby::kRosterMax], en[engine::lobby::kRosterMax];
            int found = -1;
            if (PartyStatus() >= 3 && top != PageId("main-menu")
                && engine::lobby::Ready())
                found = engine::lobby::RosterSorted(idx, en);
            if (found >= 0) {
                for (int r = 0; r < 8; r++) {
                    const int tab = r == 0 ? 3 : (r - 1 < found ? (en[r - 1] ? 4 : 3) : 0);
                    for (int k = 1; k <= 4; k++) {
                        char p[112];
                        std::snprintf(p, sizeof p, "anim-group.lobbyPanel.lobbyList.Player%d.%s", r, kTab[k]);
                        UiVisible(p, tab == k);
                    }
                    char p[112];
                    std::snprintf(p, sizeof p, "anim-group.lobbyPanel.lobbyList.Player%d.partyBar", r);
                    UiVisible(p, false);
                    std::snprintf(p, sizeof p, "anim-group.lobbyPanel.lobbyList.Player%d.pmReady", r);
                    UiVisible(p, false);
                    if (r > 0 && r - 1 < found) {   // row 0's name stays the engine's (us)
                        char nm[49];
                        if (engine::lobby::RosterName(idx[r - 1], nm, sizeof nm) && nm[0]) {
                            std::snprintf(p, sizeof p, "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", r);
                            UiText(p, nm);
                        }
                    }
                }
                return;
            }
            bool haveSearch = false;
            for (int r = 0; r < 8; r++)
                if (net::social::search_rows::Tab(r) || net::social::search_rows::Name(r)[0])
                    haveSearch = true;
            if (haveSearch && found < 0
                && (top == PageId("main-menu") || top == PageId("find-match-menu"))) {
                // SEARCHING rows (ext fr_lobby_tabs / fr_lobby_names): 1 = [T1]
                // our team (blue), 2 = [T2] enemy (red), 3 = party mate not
                // searching (grey). Row names are the captured shown names.
                // With no captured write (the engine owns those text writes while
                // the ext's pinned settext hook is the bridge) the engine's own
                // rows are left untouched -- an honest gate, not a wrong tab.
                for (int r = 0; r < 8; r++) {
                    const int s = net::social::search_rows::Tab(r);
                    const int tab = s == 1 ? 3 : s == 2 ? 4 : s == 3 ? 1 : 0;
                    for (int k = 1; k <= 4; k++) {
                        char p[112];
                        std::snprintf(p, sizeof p,
                            "anim-group.lobbyPanel.lobbyList.Player%d.%s", r, kTab[k]);
                        UiVisible(p, tab == k);
                    }
                    const char* nm = net::social::search_rows::Name(r);
                    if (nm && nm[0]) {
                        char p[112];
                        std::snprintf(p, sizeof p,
                            "anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", r);
                        UiText(p, nm);
                    }
                }
            }
            if (top == PageId("main-menu") || top == PageId("find-match-menu")) {
                if (g_net.have && g_state.hasParty) {
                    for (int i = 0; i < social::kMaxMembers; i++) {
                        char p[112];
                        std::snprintf(p, sizeof p, "anim-group.lobbyPanel.lobbyList.Player%d.partyBar", i);
                        UiVisible(p, i < g_state.nm);
                        std::snprintf(p, sizeof p, "anim-group.lobbyPanel.lobbyList.Player%d.pmReady", i);
                        UiVisible(p, i < g_state.nm && g_state.m[i].ready);
                    }
                }
            }
        }

        void ButtonTick(uint64_t now)
        {
            static bool heldPad = false;
            // ext: never open on the first look (a pad button held on arrival).
            if (!g_armedOpen) {
                g_armedOpen = true;
                heldPad = g_held.next;
                return;
            }
            const bool kp = KeyEdge('P');
            const bool pad = MenuEdge(g_held.next, heldPad);
            if (!(kp || pad) || now - g_openAt < kOpenCoolMs) return;
            g_openAt = now;
            TO1_LOG("friendsui: %s on the MP menu - pushing to1/friends13\n",
                kp ? "P" : "menu-next (pad)");
            PushPage("to1/friends13");
        }

        void ButtonDecorations(uint64_t top)
        {
            if (top != PageId("main-menu") && top != PageId("find-match-menu")) return;
            if (!g_net.have) { UiText("anim-group.friendsBtn.badge", ""); return; }
            const int badge = social::Badge(&g_state);
            char b[32];
            std::snprintf(b, sizeof b, "%d", badge);
            UiText("anim-group.friendsBtn.badge", badge > 0 ? b : "");
            if (g_state.hasParty)
                std::snprintf(b, sizeof b, Tr("PARTY %d/%d", "GRUPA %d/%d"),
                    g_state.nm, g_state.pmax ? g_state.pmax : 4);
            else
                std::snprintf(b, sizeof b, Tr("%d ONLINE", "%d ONLINE"), g_state.no);
            UiText("anim-group.friendsBtn.status", b);
        }

    } // namespace

    // ---- public API ----------------------------------------------------------------
    bool Enabled() { return FeatureEnabled(); }

    void Tick()
    {
        if (!FeatureEnabled()) return;
        SampleKeys();
        SampleMenu();

        // ext UiClickGateTick: the left-button rising edge, sampled every frame even
        // with the panel closed so a button held across the page opening is not a click.
        const bool lb = engine::window::KeyDown(VK_LBUTTON);
        g_click = lb && !g_lbHeld;
        g_lbHeld = lb;

        const uint64_t now = Now();
        const uint64_t top = engine::widget::TopPage();
        const uint64_t pMain = PageId("main-menu");
        const bool panel = top == PageId("friends13");
        const bool menuVisible = MenuVisible();

        NetTick(now, panel, menuVisible);
        FtTick(now, panel, menuVisible);

        if (!panel) {
            if (g_panel.up) {
                g_panel.up = false;
                g_panel.typing = false;
            }
            if (menuVisible) {
                if (top == pMain || top == PageId("find-match-menu")) ButtonDecorations(top);
                ButtonTick(now);
                PartyTick(now, top, pMain);
                LobbyDecorations(top);
            }
            return;
        }
        PageTick(now, top);
    }

    bool InParty() { return InPartyState(); }
    bool IsMember() { return IsMemberState(); }
    bool Ready() { return ReadyState(); }
    void MemberReadyPress() { ReadyPress(); }
    bool SearchGate() { return GateSearch(); }
    void NoteAutoPush() { NoteAutoPushState(); }
    bool PanelOpen() { return g_panel.up; }

    bool Toast(const char* text, float secs)
    {
        if (!FeatureEnabled() || !ToastCardEnabled()) return false;
        if (!MenuVisible()) return false;
        FtPush(kToastInfo, "", "", text ? text : "", secs > 0.0f ? secs : 5.0f, 0);
        return true;
    }
    void NoteSeen(const char* text)
    {
        RecentLine& l = g_recent[g_recentN++ & 7];
        std::snprintf(l.text, sizeof l.text, "%s", text ? text : "");
        l.at = Now();
    }

    void InstallNoticeHooks()
    {
        net::notice::Hooks h{};
        h.friendsToast = &net::friends13::Toast;
        h.friendsNoteSeen = &net::friends13::NoteSeen;
        h.friendsEnabled = &net::friends13::Enabled;
        net::notice::SetHooks(h);
    }

    void Reset()
    {
        g_state = State{};
        g_net = NetState{};
        g_panel = PanelState{};
        g_toast = ToastState{};
        g_queueN = 0;
        g_busy.store(0);
        g_have.store(0);
        g_repLen.store(0);
        g_tok[0] = 0;
        g_autoPush = g_openAt = g_lastMemberPress = 0;
        g_armedOpen = false;
        g_lbHeld = g_click = false;
        g_clickFrame = 0;
        g_hvCol = g_hvRow = -1;
        g_hvBack = g_hvSel = g_hvAct = false;
        g_hSel = g_hUp = g_hDown = g_hLeft = g_hRight = false;
        g_hBack = g_hPrev = g_hNext = g_hAct = g_hP = g_hPadNext = false;
        g_repUp = g_repDown = 0;
        std::memset(g_keyCur, 0, sizeof g_keyCur);
        std::memset(g_keyPrev, 0, sizeof g_keyPrev);
        g_net.actSub = 0;
        g_net.actCode = Action::None;
    }

}
