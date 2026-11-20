// Native port of tools/ext/friends13.c -> the in-game FRIENDS / PARTY panel.
// The ext module is the behavioural reference; this is native C++ in the mod's idiom.

#include "friends13.hpp"

#include <winsock2.h>
#include <windows.h>

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
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
        constexpr uint8_t kOpReq = 0xD8;
        constexpr uint8_t kOpRep = 0xD9;
        constexpr uint64_t kFlagMs = 5000;
        constexpr uint64_t kActionCoolMs = 600;
        constexpr uint64_t kOpenCoolMs = 1200;
        constexpr uint64_t kAutoWindowMs = 8000;
        constexpr uint64_t kFakeSearchMs = 9000;

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
        };

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

        bool FtHover()
        {
            float x = 0.0f, y = 0.0f;
            if (!engine::window::CursorCanvas(&x, &y)) return false;
            return x >= 2480.0f && x <= 3720.0f && y >= 1760.0f && y <= 1940.0f;
        }

        void FtCardText()
        {
            if (!g_toast.st) return;
            const ToastItem& it = g_toast.it;
            const char* head = it.kind == kToastRequest ? Tr("FRIEND REQUEST", "ZAPROSZENIE DO ZNAJOMYCH")
                : it.kind == kToastInvite ? Tr("PARTY INVITE", "ZAPROSZENIE DO GRUPY")
                : it.kind == kToastGood ? Tr("DONE", "GOTOWE")
                : it.kind == kToastBad ? Tr("COULD NOT", "NIE UDAŁO SIĘ") : Tr("FRIENDS", "ZNAJOMI");
            char body[220];
            if (it.name[0] && it.text[0])
                std::snprintf(body, sizeof body, "%s\n%s", it.name, it.text);
            else
                std::snprintf(body, sizeof body, "%s", it.name[0] ? it.name : it.text);
            Fit("anim-group.frToast.tHead", head, 1100.0f, 42.0f);
            Fit("anim-group.frToast.tName", body, 1100.0f, 38.0f);
            Fit("anim-group.frToast.tYes", FtActionable(it.kind)
                ? (it.kind == kToastRequest ? Tr("ACCEPT", "PRZYJMIJ") : Tr("JOIN", "DOŁĄCZ")) : "",
                232.0f, 38.0f);
            Fit("anim-group.frToast.tNo", FtActionable(it.kind) ? Tr("DECLINE", "ODRZUĆ") : "",
                232.0f, 38.0f);
            char more[16];
            std::snprintf(more, sizeof more, "+%d", g_queueN);
            UiText("anim-group.frToast.tMore", g_queueN > 0 ? more : "");
        }

        void FtTick(uint64_t now, bool panelUp, bool menuVisible)
        {
            float dt = g_toast.last ? static_cast<float>(now - g_toast.last) : 16.0f;
            g_toast.last = now;
            if (dt > 100.0f) dt = 100.0f;

            if (!g_toast.st && g_queueN && menuVisible && !panelUp && now >= g_queue[0].readyAt
                && now >= g_toast.cool) {
                g_toast.it = g_queue[0];
                std::memmove(g_queue, g_queue + 1,
                    static_cast<size_t>(g_queueN - 1) * sizeof(g_queue[0]));
                g_queueN--;
                g_toast.st = 1;
                g_toast.t0 = now;
                g_toast.leftMs = g_toast.totalMs = itMs(g_toast.it.secs);
            }
            if (g_toast.st == 1 && now - g_toast.t0 >= 260) { g_toast.st = 2; g_toast.tOut = now; }
            if (g_toast.st == 2) {
                if (!FtPending(g_toast.it)) { g_toast.st = 3; g_toast.tOut = now; }
                else if (!FtHover()) {
                    g_toast.leftMs -= dt;
                    if (g_toast.leftMs <= 0.0f) { g_toast.st = 3; g_toast.tOut = now; }
                }
            }
            if (g_toast.st == 3 && now - g_toast.tOut >= 220) {
                g_toast.st = 0;
                g_toast.cool = now + 180;
            }
            const bool visible = g_toast.st != 0 && menuVisible && !panelUp;
            UiVisible("anim-group.frToast", visible);
            if (!visible) return;

            FtCardText();
            float p = g_toast.st == 1
                ? static_cast<float>(now - g_toast.t0) / 260.0f
                : g_toast.st == 3 ? 1.0f - static_cast<float>(now - g_toast.tOut) / 220.0f : 1.0f;
            if (p < 0.0f) p = 0.0f;
            if (p > 1.0f) p = 1.0f;
            const float ease = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
            UiAlpha("anim-group.frToast", ease);
            engine::widget::Scale(engine::widget::Page("anim-group.frToast"),
                1.0f - (1.0f - ease) * 0.06f, 1.0f);
            const float line = g_toast.totalMs > 0.0f ? g_toast.leftMs / g_toast.totalMs : 0.0f;
            engine::widget::Scale(engine::widget::Page("anim-group.frToast.tBar"),
                line < 0.0f ? 0.0f : line > 1.0f ? 1.0f : line, 1.0f);

            // Y accepts / joins, N declines (a click on the chip is the pad/menu path).
            if (g_toast.st == 2 && FtActionable(g_toast.it.kind)) {
                if (KeyEdge('Y')) {
                    Action a = g_toast.it.kind == kToastRequest ? Action::Accept : Action::Join;
                    QueueAction(a, a == Action::Accept ? 4 : 7, g_toast.it.id);
                    g_toast.st = 3; g_toast.tOut = now;
                } else if (KeyEdge('N')) {
                    QueueAction(g_toast.it.kind == kToastRequest ? Action::DeclineRequest
                        : Action::DeclineParty, 5, g_toast.it.id);
                    g_toast.st = 3; g_toast.tOut = now;
                }
            }
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
            const char* st, const char* tag, const char* ac, bool foc, bool hov)
        {
            char g[96], p[160];
            std::snprintf(g, sizeof g, "anim-group.%s.%cRow%d", col ? "rt" : "lf",
                col ? 'r' : 'l', i);
            UiVisible(g, vis);
            if (!vis) return;
            for (int k = 0; k < 5; k++) {
                std::snprintf(p, sizeof p, "%s.%cBd%d_%d", g, col ? 'r' : 'l', k, i);
                UiVisible(p, k == tone);
            }
            // TODO(friends13): rank badges (0xBF / rankbadge.c) are not ported; the ext
            // draws a badge before a player row. The name / status / action text still
            // fills its box, just without the badge's 120-unit reservation.
            RowPath(p, sizeof p, col, i, "Nm");  Fit(p, name, col ? 920.0f : 680.0f, 47.0f);
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
                        social::ActionLabel(&g_state, pri), foc, false);
                } else if (i == g_state.nm) {
                    SetRow(0, i, true, 0, Tr("READY UP", "GOTOWY"), "", "",
                        social::ActionLabel(&g_state, pri), foc, false);
                } else {
                    SetRow(0, i, true, 0, g_state.meLeader ? Tr("DISBAND PARTY", "ROZWIĄŻ GRUPĘ")
                        : Tr("LEAVE PARTY", "OPUŚĆ GRUPĘ"), "", "",
                        social::ActionLabel(&g_state, pri), foc, false);
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
                if (r.kind == RowKind::Add) {
                    name = Tr("ADD A FRIEND", "DODAJ ZNAJOMEGO");
                    tag = Tr("TYPE A NAME OR A STEAM ID", "WPISZ NAZWĘ LUB ID STEAM");
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
                SetRow(1, k, true, tone, name, st, tag, d, foc, false);
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
            bool click = false;   // TODO(friends13): click-to-nav needs UiMouseOver's widget bounds

            if (g_panel.typing) {
                TypingTick(now);
                if (esc || back) { TypingStop(); esc = back = false; }
                else if (KeyEdge(VK_RETURN)) SendTyped();
                back = false;
                selEdge = upEdge = downEdge = leftEdge = rightEdge = 0;
                prev = next = act = false;
            }
            if (esc) { Close(); return; }
            if (back) g_panel.backFrame = engine::frame::Count() ? engine::frame::Count() : 1;
            if (g_panel.backFrame && engine::frame::Count() >= g_panel.backFrame + 6) {
                g_panel.backFrame = 0;
                Close();
                return;
            }
            if (KeyEdge('P')) { Close(); return; }

            Row rows[1 + social::kMaxInvites + 2 * kRightRows + social::kMaxFriends];
            int nr = RightRows(rows);
            int ln = LeftN();
            bool x = KeyEdge('X') || KeyEdge(VK_DELETE);
            bool r = KeyEdge('R'), l = KeyEdge('L');
            bool sw = KeyEdge(VK_TAB) || KeyEdge('Q') || KeyEdge('E');
            if (!g_net.have) { selEdge = upEdge = downEdge = x = r = l = 0; }
            int col = g_panel.col, n = col ? nr : ln, foc = g_panel.foc[col];
            if (upEdge && n) { foc = (foc + n - 1) % n; Sfx("sfx-cycle"); }
            if (downEdge && n) { foc = (foc + 1) % n; Sfx("sfx-cycle"); }
            if (sw || prev || next || leftEdge || rightEdge) {
                if (ln) { col = !col; g_panel.col = col; n = col ? nr : ln; foc = g_panel.foc[col]; Sfx("sfx-cycle"); }
            }
            if (foc < 0) foc = 0;
            if (n && foc >= n) foc = n - 1;
            g_panel.foc[col] = foc;
            Action pri = Action::None, sec = Action::None;
            social::RowActions(&g_state, col, foc, rows, nr, pri, sec);
            if (r) { Action ap, as; social::RowActions(&g_state, 0, 0, rows, nr, ap, as); Do(ap, 0, 0, rows, nr); }
            if (l) { Action ap, as; social::RowActions(&g_state, 0, g_state.nm + 1, rows, nr, ap, as); Do(ap, 0, g_state.nm + 1, rows, nr); }
            if (selEdge || act) Do(pri, col, foc, rows, nr);
            if (x || click) Do(sec, col, foc, rows, nr);
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

        // TODO(friends13): the raw lobby object / kPartyStatus / g_srchTab / g_srchName
        // producers are still ext-owned. The daemon snapshot gives us the party state,
        // ready bits and the leader's search flag, so the follow rule itself is native;
        // the match-found team tabs and the searchers' table are gated until those
        // producers exist in mod/.
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
            // ext FrLobbyTabs / FrLobbyPartyRows: the party READY badges are derivable
            // from the daemon snapshot; the match-found blue/red team tabs need the
            // lobby roster (lobby object +0x1d / game slot teams) which mod does not
            // expose yet. Those tabs stay hidden instead of guessing a team.
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
        g_hSel = g_hUp = g_hDown = g_hLeft = g_hRight = false;
        g_hBack = g_hPrev = g_hNext = g_hAct = g_hP = g_hPadNext = false;
        g_repUp = g_repDown = 0;
        std::memset(g_keyCur, 0, sizeof g_keyCur);
        std::memset(g_keyPrev, 0, sizeof g_keyPrev);
        g_net.actSub = 0;
        g_net.actCode = Action::None;
    }

}
