// ported from tools/ext/launchnote.c. The translated text pieces are already in
// engine/loc_table_pl.hpp; writes go through engine::locale::Tr like every other
// native menu text.

#include "launch-notice.hpp"
#include "launch-notice-core.hpp"

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "menu/loadseq.hpp"
#include "log.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#define LN_EM "<font color=\"#ECE9E2\">"   // tools/ext/launchnote.c:15

namespace net::ui::launch_notice {

    namespace {

        using core::Stage;

        struct StateData {
            Stage    st = Stage::Unknown;
            int      pushes = 0;
            uint64_t pushedAt = 0;
            uint64_t holdAt = 0;
            uint64_t page = 0;
            uint64_t lastTop = 0;
        } g;

        struct PageState {
            uint64_t t0 = 0, readAt = 0, outAt = 0;
            int      focus = 0, ticked = 0, gone = 0, refill = 0;
            int      lastLeft = -1, hovLast = -1, never = 0;
            bool     hSel = true, hBack = true, hUp = true, hDown = true;
            bool     hLeft = true, hRight = true;
            float    aCheck = -1.0f, aBtn0 = -1.0f, aBtn1 = -1.0f;
            float    aFrame = -1.0f, aGroup = -1.0f, aDim = -1.0f, aTimer = -1.0f;
            int      vTick = -1, vFo0 = -1, vFo1 = -1, vIcon = -1;
        } gPage;

        void Init()
        {
            if (g.st != Stage::Unknown) return;
            if (!g.page) g.page = engine::widget::PageId("launch-notice");
            FILE* f = engine::exe_side::Open("t2r-notice.txt", "r");
            int v = 0;
            if (f) {
                if (std::fscanf(f, "seen %d", &v) != 1) v = 0;
                std::fclose(f);
            }
            if (core::Seen(v)) {
                g.st = Stage::Done;
                TO1_LOG("notice: not shown (t2r-notice.txt: seen, version %d)\n", core::kVersion);
            }
            else {
                g.st = Stage::Due;
                TO1_LOG("notice: due this launch (version %d)\n", core::kVersion);
            }
        }

        void Text(const char* path, const char* text)
        {
            const uint64_t w = engine::widget::Page(path);
            if (!w) return;
            char b[1536];
            std::snprintf(b, sizeof b, "%s", engine::locale::Tr(text ? text : ""));
            engine::widget::SetText(w, b);
        }

        void TextDb(const char* path, const char* text)
        {
            Text(path, text);
            char twin[160];
            if (!path || std::strlen(path) + 1 >= sizeof twin) return;
            std::snprintf(twin, sizeof twin, "%sB", path);
            Text(twin, text);
        }

        void Put(const char* path, float& last, float a)
        {
            const bool force = engine::frame::Count() % 30 == 0;
            if (last == a && !force) return;
            last = a;
            engine::widget::Alpha(engine::widget::Page(path), a);
        }

        void Vis(const char* path, int& last, int on)
        {
            const bool force = engine::frame::Count() % 30 == 0;
            if (last == on && !force) return;
            last = on;
            engine::widget::Visible(engine::widget::Page(path), on != 0);
        }

        void Sfx(const char* name)
        {
            AUDIO::PLAY_SOUND(SID(name));
        }

        void PopPage()
        {
            if (g.page && MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack) == g.page)
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
        }

        void Close(int never)
        {
            if (!never) {
                TO1_LOG("notice: closed\n");
                return;
            }
            FILE* f = engine::exe_side::Open("t2r-notice.txt", "w");
            if (f) {
                std::fprintf(f, "seen %d\n", core::kVersion);
                std::fclose(f);
                engine::exe_side::Forget("t2r-notice.txt");
            }
            TO1_LOG("notice: closed - don't show again (t2r-notice.txt %s)\n",
                f ? "written" : "NOT written");
        }

        void Fill(int leftSec)
        {
            Text("anim-group.lnBox.lnP0",
                "TWELVE WEEKS IS OUT, BUT IT IS STILL " LN_EM "UNFINISHED</font>. "
                "THIS IS AN EARLY, WORK-IN-PROGRESS RELEASE, " LN_EM "NOT THE FINISHED GAME</font>.");
            Text("anim-group.lnBox.lnHead", "WHAT THAT MEANS");
            Text("anim-group.lnBox.lnB0",
                "PERFORMANCE IS " LN_EM "NOT FINAL</font>: STUTTERS, LONG LOADS AND FRAME DROPS CAN HAPPEN.");
            Text("anim-group.lnBox.lnB1",
                "GAMEPLAY IS " LN_EM "NOT FINAL</font>: WEAPONS, MELEE, MAPS AND BALANCE STILL CHANGE FROM UPDATE TO UPDATE.");
            Text("anim-group.lnBox.lnB2",
                "EXPECT " LN_EM "BUGS AND CRASHES</font>, DISCONNECTS, BROKEN ANIMATIONS AND ROUGH EDGES.");
            Text("anim-group.lnBox.lnB3",
                "SOME FEATURES ARE " LN_EM "MISSING OR PLACEHOLDER</font>.");
            Text("anim-group.lnBox.lnP1",
                "BY PLAYING, YOU AGREE TO REPORT THE BUGS YOU FIND WITH " LN_EM "REPORT A BUG</font> "
                "IN THE LAUNCHER. SAY WHAT HAPPENED AND WHERE. EVERY REPORT HELPS.");
            char b[160];
            if (leftSec > 0) {
                std::snprintf(b, sizeof b, "I ACKNOWLEDGE (%d)", leftSec);
                TextDb("anim-group.lnBox.lnCheck.lnAck", b);
            }
            else
                TextDb("anim-group.lnBox.lnCheck.lnAck", "I ACKNOWLEDGE");
            Text("anim-group.lnBox.lnBtn0.lnBtn0Tx", "CLOSE");
            Text("anim-group.lnBox.lnBtn1.lnBtn1Tx", "CLOSE, DON'T SHOW AGAIN");
        }

        void PageTick(bool entered, uint64_t now)
        {
            if (entered) {
                g.st = Stage::Up;
                gPage = PageState{};
                gPage.t0 = now;
                gPage.refill = core::kRefillFrames;
                gPage.lastLeft = -1;
                gPage.hovLast = -1;
                Fill(static_cast<int>(core::kReadMs / 1000));
                TO1_LOG("notice: page up (%s)\n",
                    menu::loadseq::HoldActive() ? "under the TWELVE WEEKS title" : "over the MP menu");
            }
            else if (gPage.refill && !--gPage.refill)
                Fill(gPage.lastLeft > 0 ? gPage.lastLeft : 0);

            if (gPage.gone) {
                engine::widget::Alpha(engine::widget::Page("anim-group"), 0.0f);
                engine::widget::Alpha(engine::widget::Page("lnDimG"), 0.0f);
                return;
            }

            if (!gPage.readAt && !menu::loadseq::HoldActive()) {
                gPage.readAt = now;
                TO1_LOG("notice: on screen - %d s reading lock\n",
                    static_cast<int>(core::kReadMs / 1000));
            }
            const core::Lock lock = core::ReadLock(now, gPage.readAt);
            const bool locked = lock.locked;
            if (lock.leftSec != gPage.lastLeft) {
                gPage.lastLeft = lock.leftSec;
                Fill(lock.leftSec);
            }

            const bool sel = engine::menu_input::CommandEdge(SID("menu-select"), gPage.hSel);
            const bool back = engine::menu_input::CommandEdge(SID("menu-back"), gPage.hBack);
            const bool up = engine::menu_input::CommandEdge(SID("menu-up"), gPage.hUp);
            const bool down = engine::menu_input::CommandEdge(SID("menu-down"), gPage.hDown);
            const bool left = engine::menu_input::CommandEdge(SID("menu-left"), gPage.hLeft);
            const bool right = engine::menu_input::CommandEdge(SID("menu-right"), gPage.hRight);

            if (gPage.outAt) {
                float f = 1.0f - static_cast<float>(now - gPage.outAt) / static_cast<float>(core::kFadeMs);
                if (f < 0.0f) f = 0.0f;
                engine::widget::Alpha(engine::widget::Page("anim-group"), f);
                engine::widget::Alpha(engine::widget::Page("lnDimG"), f);
                if (now - gPage.outAt >= core::kFadeMs) {
                    gPage.gone = 1;
                    g.st = Stage::Done;
                    PopPage();
                    Close(gPage.never);
                }
                return;
            }

            int hov = -1;
            if (!locked) {
                if (engine::widget::MouseOver(engine::widget::Page("anim-group.lnBox.lnCheck"), true))
                    hov = 0;
                else if (gPage.ticked && engine::widget::MouseOver(
                    engine::widget::Page("anim-group.lnBox.lnBtn0"), true))
                    hov = 1;
                else if (gPage.ticked && engine::widget::MouseOver(
                    engine::widget::Page("anim-group.lnBox.lnBtn1"), true))
                    hov = 2;
            }
            if (hov >= 0 && hov != gPage.hovLast && hov != gPage.focus) {
                gPage.focus = hov;
                Sfx("sfx-cycle");
            }
            gPage.hovLast = hov;

            const bool click = engine::menu_input::ClickEdge();
            int act = -1;
            if (click && hov >= 0) act = hov;
            else if (sel && !engine::menu_input::SelectIsMouse()) act = gPage.focus;

            if (engine::flag::Present("t2r-test-autofind.txt") && !locked && gPage.readAt
                && now - gPage.readAt > core::kReadMs + 1000) {
                if (!gPage.ticked) act = 0;
                else {
                    act = 1;
                    TO1_LOG("test autofind: notice closed\n");
                }
            }

            const bool mouseSelect = engine::menu_input::SelectIsMouse();
            if (back || (sel && act < 0 && !mouseSelect)) Sfx("sfx-menu-error");

            if (act == 0) {
                if (locked) Sfx("sfx-menu-error");
                else {
                    gPage.ticked = !gPage.ticked;
                    gPage.focus = gPage.ticked ? 1 : 0;
                    Sfx("sfx-select");
                    TO1_LOG("notice: %s\n", gPage.ticked ? "acknowledged" : "unticked");
                }
            }
            else if (act == 1 || act == 2) {
                if (gPage.ticked) {
                    gPage.never = act == 2 ? 1 : 0;
                    gPage.outAt = now;
                    Sfx("sfx-select");
                }
                else Sfx("sfx-menu-error");
            }

            if (!locked && act < 0) {
                const int f0 = gPage.focus;
                gPage.focus = core::FocusMove(gPage.ticked != 0, gPage.focus,
                    up, down, left, right);
                if (gPage.focus != f0) Sfx("sfx-cycle");
            }

            const float fin = core::Ramp(now, gPage.t0, core::kFadeMs);
            Put("anim-group", gPage.aGroup, fin);
            Put("lnDimG", gPage.aDim, fin);
            Put("anim-group.lnBox.lnCheck", gPage.aCheck, locked ? 0.4f : 1.0f);
            Put("anim-group.lnBox.lnCheck.lnCheckFr", gPage.aFrame,
                locked ? 0.35f : gPage.focus == 0 ? 1.0f : 0.45f);
            Put("anim-group.lnBox.lnBtn0", gPage.aBtn0, gPage.ticked ? 1.0f : 0.35f);
            Put("anim-group.lnBox.lnBtn1", gPage.aBtn1, gPage.ticked ? 1.0f : 0.35f);
            Vis("anim-group.lnBox.lnCheck.lnTick", gPage.vTick, gPage.ticked);
            Vis("anim-group.lnBox.lnBtn0.lnBtn0Fo", gPage.vFo0,
                gPage.ticked && gPage.focus == 1);
            Vis("anim-group.lnBox.lnBtn1.lnBtn1Fo", gPage.vFo1,
                gPage.ticked && gPage.focus == 2);
            Vis("anim-group.lnHint.lnHintIcon", gPage.vIcon, !locked);
            TextDb("anim-group.lnHint.lnHintTx",
                locked ? "READ THE NOTICE" : gPage.ticked ? "SELECT" : "TICK");

            const float tm = core::TimerScale(lock);
            if (std::fabs(tm - gPage.aTimer) > 0.002f || (tm == 0.0f && gPage.aTimer != 0.0f)) {
                gPage.aTimer = tm;
                const uint64_t w = engine::widget::Page("anim-group.lnBox.lnTimer");
                if (tm > 0.0f) {
                    engine::widget::Visible(w, true);
                    engine::widget::Scale(w, tm, 1.0f);
                }
                else engine::widget::Visible(w, false);
            }
        }

    } // namespace

    void Tick(uint64_t now)
    {
        Init();
        if (boot::ExtLoaded()) return;

        const uint64_t top = engine::widget::TopPage();
        if (g.st == Stage::Done) {
            if (top == g.page) {
                engine::widget::Alpha(engine::widget::Page("anim-group"), 0.0f);
                engine::widget::Alpha(engine::widget::Page("lnDimG"), 0.0f);
            }
            return;
        }

        const bool entered = top != g.lastTop;
        g.lastTop = top;
        if (top == g.page) {
            PageTick(entered || g.st != Stage::Up, now);
            return;
        }
        if (g.st == Stage::Up) {
            g.st = Stage::Done;
            TO1_LOG("notice: page gone (closed another way)\n");
            return;
        }
        if (top != engine::widget::PageId("main-menu")) return;

        if (core::DropPushes(g.st, now, g.pushedAt, g.pushes)) {
            g.st = Stage::Done;
            TO1_LOG("notice: to1/launch-notice never came up - dropped\n");
            return;
        }
        if (!core::ShouldPush(g.st, now, g.pushedAt, g.pushes)) return;

        if (!MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, g.page))
            MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/launch-notice");
        g.st = Stage::Pushed;
        ++g.pushes;
        g.pushedAt = now;
        TO1_LOG("notice: to1/launch-notice pushed (try %d, entry %s)\n", g.pushes,
            menu::loadseq::HoldActive() ? "title still up" : "title gone");
    }

    bool Busy()
    {
        Init();
        return !boot::ExtLoaded() && core::Busy(g.st);
    }

    bool HoldFade(uint64_t now)
    {
        Init();
        if (boot::ExtLoaded()) return false;
        const bool held = core::HoldFade(g.st, now, g.holdAt);
        if (held && g.holdAt == now)
            TO1_LOG("notice: the title's hand-off fade waits for the notice\n");
        return held;
    }

    bool ClickHit()
    {
        if (boot::ExtLoaded() || g.st != Stage::Up) return false;
        return engine::widget::MouseOver(engine::widget::Page("anim-group.lnBox.lnCheck"), true)
            || engine::widget::MouseOver(engine::widget::Page("anim-group.lnBox.lnBtn0"), true)
            || engine::widget::MouseOver(engine::widget::Page("anim-group.lnBox.lnBtn1"), true);
    }

    void ResetRuntime()
    {
        // The notice is once per *process*: keep Done/Due/Up, drop only transient
        // retry / page-entry state so a new session cannot get a second push.
        g.pushedAt = 0;
        g.lastTop = 0;
        gPage = PageState{};
    }

    void ResetAll()
    {
        g = StateData{};
        gPage = PageState{};
    }

} // namespace net::ui::launch_notice
