// Native home for the post-match / menu latches that hold requeue's settle.
// Producers ported from tools/ext/t2r-ext.c (g_pm.pending / g_ev.pending via
// net/online/clan-state.*, GEAR UP g_loState, g_mmBack's party + Steam gates),
// clan13.c (Cl13PostActive) and tools/ext/requeue.c's post-match screen gate.

#include "postmatch-screens.hpp"

#include "log.hpp"           // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/script.hpp" // MENU2 / MenuStack / DC::Call
#include "engine/widget.hpp" // PageId
#include "net/match/match-liveness.hpp"
#include "net/online/clan-state.hpp"

namespace net::ui::postmatch {

    namespace {

        // ext: `if (g_mmBack && ++g_mmBack > 3) { g_mmBack = 0; ... pop ... }` -- a
        // refused FIND MATCH holds for a few frames, then the search page is popped.
        constexpr int  kMmBackFrames = 3;
        // The mod's own find-match flow (net-gui2.cpp) already pops the page it
        // refuses; popping it again here would eat the page underneath. The ext's
        // menu2-pop-page call is kept as a gated seam for a future cutover.
        constexpr bool kPopOnMmBack  = false;
        constexpr uint64_t kLegacyFmPage = 0x9E33B6D92E193B4DULL; // ext's old matchmaking page

        struct State {
            // cached page ids (stable for the session; 0 until the first Tick)
            StringId64 cl13x = 0, postmatch = 0, event = 0;
            StringId64 gearup13 = 0, loadoutMenu = 0;
            StringId64 pickers[4]{};
            StringId64 findMatch = 0, mainMenu = 0;

            // sampled latches
            bool cl13 = false, post = false, eventUp = false;
            LoState lo = LoState::None;
            int  mmBack = 0;
        } g;

        void Init()
        {
            if (g.cl13x)
                return;
            g.cl13x       = engine::widget::PageId("clan13x");
            g.postmatch   = engine::widget::PageId("clan-postmatch");
            g.event       = engine::widget::PageId("clan-event");
            g.gearup13    = engine::widget::PageId("gearup13");
            g.loadoutMenu = engine::widget::PageId("loadout-menu");
            g.pickers[0]  = engine::widget::PageId("picker-skill");
            g.pickers[1]  = engine::widget::PageId("picker-primary");
            g.pickers[2]  = engine::widget::PageId("picker-secondary");
            g.pickers[3]  = engine::widget::PageId("picker-purchase");
            g.findMatch   = engine::widget::PageId("find-match-menu");
            g.mainMenu    = engine::widget::PageId("main-menu");
        }

        bool OnStack(StringId64 page)
        {
            return page && MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, page);
        }

        bool GearPagesUp()
        {
            return OnStack(g.gearup13) || OnStack(g.loadoutMenu)
                || OnStack(g.pickers[0]) || OnStack(g.pickers[1])
                || OnStack(g.pickers[2]) || OnStack(g.pickers[3]);
        }

    } // namespace

    void Tick()
    {
        Init();

        // The page handlers' bools are now backed by the native clan state machine;
        // the stack sample only covers the widget pages the ext still owns.
        g.cl13    = OnStack(g.cl13x);
        g.post    = OnStack(g.postmatch);
        g.eventUp = OnStack(g.event);

        // ext g_loState: GuEnter sets LO_LIST on the menu (never in substate 6);
        // the mid-match CHANGE LOADOUT sets LO_MATCH, the countdown sets LO_CHOOSE,
        // the editor sets LO_EDIT. The explicit transitions come from net-gui2;
        // this fallback only handles a page the engine pushed by itself.
        const bool gear = GearPagesUp();
        if (gear && g.lo == LoState::None) {
            g.lo = net::match::liveness::Live() ? LoState::Match
                                                : LoState::List;
        } else if (!gear && (g.lo == LoState::List || g.lo == LoState::Edit)) {
            g.lo = LoState::None;
        }

        if (g.mmBack && ++g.mmBack > kMmBackFrames) {
            const StringId64 top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
            g.mmBack = 0;
            if (kPopOnMmBack && (top == g.findMatch || top == kLegacyFmPage))
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            TO1_LOG("postmatch: refused find-match latch cleared, top %llx%s\n",
                static_cast<unsigned long long>(top), kPopOnMmBack ? "" : " [page pop gated]");
        }

        // ext EvOffer: on the MP menu, with no GEAR UP navigation and no post-match
        // page due, decide whether the event screen is wanted. The ext still pushes
        // the widget page; the decision and latch are native.
        const uint64_t top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
        const bool menuTop = top == g.findMatch || top == g.mainMenu
            || OnStack(g.mainMenu);
        net::clan::Offer(menuTop, g.lo != LoState::None, GetTickCount64());
    }

    void Reset()
    {
        g.cl13 = g.post = g.eventUp = false;
        g.lo = LoState::None;
        g.mmBack = 0;
    }

    bool Cl13PostActive()  { return g.cl13; }
    bool ClanPostActive()  { return net::clan::PostPending() || g.post; }
    bool ClanEventActive() { return net::clan::EventPending() || g.eventUp; }
    bool GearUpNavActive() { return g.lo != LoState::None; }
    LoState Lo()           { return g.lo; }
    bool MmBackActive()    { return g.mmBack != 0; }

    bool ScreensDone()
    {
        return !g.cl13 && !net::clan::PostPending() && !g.post
            && !net::clan::EventPending() && !g.eventUp
            && g.lo == LoState::None && g.mmBack == 0;
    }

    void NoteLoadoutOpen(bool inMatch)
    {
        if (g.lo == LoState::None)
            g.lo = inMatch ? LoState::Match : LoState::List;
    }
    void NoteLoadoutEdit()
    {
        if (g.lo == LoState::List)
            g.lo = LoState::Edit;
    }
    void NoteLoadoutApplied()
    {
        if (g.lo == LoState::Match)
            g.lo = LoState::None;
    }
    void NoteLoadoutClose()
    {
        if (g.lo != LoState::Match)      // a mid-match pick stays pending to next spawn
            g.lo = LoState::None;
    }
    void NoteCountdownChoose(int phase, int secondsLeft)
    {
        if (phase == 2 && secondsLeft >= 100 && secondsLeft < 130) {
            if (g.lo == LoState::None || g.lo == LoState::List)
                g.lo = LoState::Choose;
        } else if (g.lo == LoState::Choose) {
            g.lo = LoState::None;
        }
    }

    void NoteMmRefused()
    {
        if (!g.mmBack)
            TO1_LOG("postmatch: find match refused (party gate) - hold requeue's settle for a few frames\n");
        g.mmBack = 1;
    }

    void NoteSteamRefused()
    {
        if (!g.mmBack)
            TO1_LOG("postmatch: find match refused (Steam helper not connected) - g_mmBack set\n");
        g.mmBack = 1;
    }

} // namespace net::ui::postmatch
