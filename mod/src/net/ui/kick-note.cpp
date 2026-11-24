// ported from tools/ext/kicknote.c (build-201 working tree plus the packet-loss
// branch from origin/port-201). The kick text still comes from the already-ported
// notice 0xBB producer; this module owns the MP-menu page that must be closed.

#include "kick-note.hpp"
#include "kick-note-core.hpp"

#include "boot.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "engine/script.hpp"
#include "engine/lobby.hpp"
#include "net/ui/notice.hpp"
#include "menu/loadseq.hpp"
#include "net/ui/launch-notice.hpp"
#include "sid/sid.hpp"
#include "log.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::kick_note {

    namespace {

        using core::State;

        struct StateData {
            core::State  st = State::None;
            char         text[300]{};
            char         msg[400]{};
            const char*  title = "KICKED FROM THE MATCH";
            uint64_t     setAt = 0;
            uint64_t     pushedAt = 0;
            int          pushes = 0;
            uint64_t     page = 0;
            uint64_t     lastTop = 0;
        } g;

        struct PageState {
            uint64_t t0 = 0, outAt = 0;
            bool     gone = false;
            int      hovLast = -1;
            int      refill = 0;
            bool     hSel = true, hBack = true;
            float    aGrp = -1.0f, aDim = -1.0f, aBtn = -1.0f;
        } gPage;

        void Text(const char* path, const char* text)
        {
            const uint64_t w = engine::widget::Page(path);
            if (!w) return;
            char b[1024];
            std::snprintf(b, sizeof b, "%s", engine::locale::Tr(text ? text : ""));
            engine::widget::SetText(w, b);
        }

        void Alpha(const char* path, float a)
        {
            engine::widget::Alpha(engine::widget::Page(path), a);
        }

        void Put(const char* path, float& last, float a)
        {
            const bool force = engine::frame::Count() % core::kRefillFrames == 0;
            if (last == a && !force) return;
            last = a;
            Alpha(path, a);
        }

        void Sfx(const char* name)
        {
            AUDIO::PLAY_SOUND(SID(name));
        }

        void PopPage()
        {
            if (!g.page) return;
            if (MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack) == g.page)
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
        }

        bool OnStack(uint64_t page)
        {
            return page && MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, page);
        }

        void Refill()
        {
            Text("anim-group.knBox.knTitle", g.title);
            Text("anim-group.knBox.knMsg", g.msg);
            Text("anim-group.knBox.knBtn.knBtnTx", "CLOSE");
            Text("anim-group.knHint.knHintTx", "CLOSE");
        }

        void PageTick(bool entered, uint64_t now)
        {
            if (entered) {
                g.st = State::Up;
                gPage.t0 = now;
                gPage.outAt = 0;
                gPage.gone = false;
                gPage.hovLast = -1;
                gPage.refill = core::kRefillFrames;
                gPage.hSel = gPage.hBack = true;  // held keys on entry are not an action
                gPage.aGrp = gPage.aDim = gPage.aBtn = -1.0f;
                Refill();
                TO1_LOG("kick note: page up - \"%.120s\"\n", g.msg);
            }
            else if (gPage.refill && !--gPage.refill)
                Refill();

            if (gPage.gone) {
                Alpha("anim-group", 0.0f);
                Alpha("knDimG", 0.0f);
                return;
            }

            const uint64_t hoverW = engine::widget::Page("anim-group.knBox.knBtn");
            const bool locked = gPage.t0 && now - gPage.t0 < core::kLockMs;
            const bool hov = !locked && engine::widget::MouseOver(hoverW, true);
            if (hov && gPage.hovLast < 0 && !locked) Sfx("sfx-cycle");
            gPage.hovLast = hov ? 0 : -1;

            const bool sel = engine::menu_input::CommandEdge(SID("menu-select"), gPage.hSel);
            const bool back = engine::menu_input::CommandEdge(SID("menu-back"), gPage.hBack);
            const bool click = engine::menu_input::ClickEdge();

            bool close = false;
            if (!locked && ((click && hov) || back))
                close = true;
            if (!locked && sel && !engine::menu_input::SelectIsMouse())
                close = true;
            if (engine::flag::Present("t2r-test-autofind.txt") && gPage.t0
                && now - gPage.t0 > core::kAutoFindMs) {
                close = true;
                TO1_LOG("test autofind: kick notice closed\n");
            }

            if (close && !gPage.outAt) {
                gPage.outAt = now;
                Sfx("sfx-select");
            }

            if (gPage.outAt) {
                float f = 1.0f - static_cast<float>(now - gPage.outAt) / static_cast<float>(core::kFadeMs);
                if (f < 0.0f) f = 0.0f;
                Alpha("anim-group", f);
                Alpha("knDimG", f);
                if (now - gPage.outAt >= core::kFadeMs) {
                    gPage.gone = true;
                    g.st = State::None;
                    PopPage();
                    TO1_LOG("kick note: closed\n");
                }
                return;
            }

            const float fin = core::Ramp(now, gPage.t0, core::kFadeMs);
            Put("anim-group", gPage.aGrp, fin);
            Put("knDimG", gPage.aDim, fin);
            Put("anim-group.knBox.knBtn", gPage.aBtn, locked ? 0.5f : 1.0f);
        }

    } // namespace

    bool Set(const char* text)
    {
        if (!text || core::IsMove(text)) return false;
        if (boot::ExtLoaded()) return false;   // legacy ext owns this page while loaded
        if (!g.page) g.page = engine::widget::PageId("kick-notice");

        const uint64_t now = GetTickCount64();
        if (core::Duplicate(g.text, text, g.setAt, now))
            return true;   // the 0x11 note and the notice-poll line are one kick

        core::Parsed p = core::Parse(text);
        std::snprintf(g.text, sizeof g.text, "%s", text);
        std::snprintf(g.msg, sizeof g.msg, "%s", p.msg);
        switch (p.title) {
        case core::Title::Ping:        g.title = "KICKED FOR HIGH PING"; break;
        case core::Title::PacketLoss:  g.title = "REMOVED FOR PACKET LOSS"; break;
        default:                       g.title = "KICKED FROM THE MATCH"; break;
        }
        g.setAt = now;
        g.pushedAt = 0;
        g.pushes = 0;
        if (g.st != State::Up) g.st = State::Due;
        TO1_LOG("kick note: \"%.120s\" -> due (substate %d)\n", text,
            static_cast<int>(engine::lobby::Substate()));
        return true;
    }

    void Tick(uint64_t now)
    {
        if (boot::ExtLoaded() || g.st == State::None) {
            if (g.st == State::None && engine::widget::TopPage() == g.page && g.page) {
                Alpha("anim-group", 0.0f);
                Alpha("knDimG", 0.0f);
            }
            return;
        }
        if (!g.page) g.page = engine::widget::PageId("kick-notice");

        const uint64_t top = engine::widget::TopPage();
        const bool entered = top != g.lastTop;
        g.lastTop = top;

        if (top == g.page) {
            PageTick(entered || g.st != State::Up, now);
            return;
        }
        if (g.st == State::Up) {
            g.st = State::None;
            TO1_LOG("kick note: page gone (closed another way)\n");
            return;
        }
        if (top != engine::widget::PageId("main-menu")) return;
        // The entry title and the launch notice both sit above this box.
        if (menu::loadseq::HoldActive()) return;
        if (launch_notice::Busy()) return;

        if (core::DropPushes(g.st, now, g.pushedAt, g.pushes)) {
            g.st = State::None;
            TO1_LOG("kick note: to1/kick-notice never came up - dropped\n");
            return;
        }
        if (!core::ShouldPush(g.st, now, g.pushedAt, g.pushes)) return;

        if (!OnStack(g.page))
            MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/kick-notice");
        g.st = State::Pushed;
        ++g.pushes;
        g.pushedAt = now;
        TO1_LOG("kick note: to1/kick-notice pushed (try %d)\n", g.pushes);
    }

    bool Busy()
    {
        const int s = static_cast<int>(g.st);
        return s >= static_cast<int>(State::Due) && s <= static_cast<int>(State::Up);
    }

    bool ClickHit()
    {
        if (boot::ExtLoaded()) return false;
        return engine::widget::MouseOver(engine::widget::Page("anim-group.knBox.knBtn"), true);
    }

    void InstallNoticeHooks()
    {
        net::notice::Hooks h{};
        h.kickSet = &Set;
        net::notice::SetHooks(h);
    }

    void Reset()
    {
        g = StateData{};
        gPage = PageState{};
    }

} // namespace net::ui::kick_note
