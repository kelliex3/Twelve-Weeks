// ported from tools/ext/t2r-ext.c (Cl13PostActive / g_pm.pending / g_ev.pending /
// g_loState / g_mmBack) and tools/ext/requeue.c's post-match screen gate

#include "postmatch-screens.hpp"

#include "log.hpp"           // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/script.hpp" // MENU2 / MenuStack / DC::Call
#include "engine/widget.hpp" // PageId

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
            StringId64 findMatch = 0;

            // sampled latches
            bool cl13 = false, post = false, eventUp = false, gear = false;
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
        }

        bool OnStack(StringId64 page)
        {
            return page && MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, page);
        }

    } // namespace

    void Tick()
    {
        Init();

        // The ext's latches are bools owned by the page handlers (they stay set from
        // "page due" until the page closes). Their observable equivalent is the page
        // being on the HUD menu stack; requeue re-checks every frame and only arms
        // after a full 1.5 s settled, so a page popping in does not lose an arm.
        g.cl13    = OnStack(g.cl13x);
        g.post    = OnStack(g.postmatch);
        g.eventUp = OnStack(g.event);
        // ext g_loState != LO_NONE covers the loadout list / edit views and the
        // picker pages (LO_LIST / LO_EDIT / LO_CHOOSE / LO_MATCH). With the 2013 page
        // one page holds both list and edit; the stock flow uses loadout-menu + the
        // four picker pages. Any of them up means GEAR UP navigation is in progress.
        g.gear    = OnStack(g.gearup13) || OnStack(g.loadoutMenu)
                 || OnStack(g.pickers[0]) || OnStack(g.pickers[1])
                 || OnStack(g.pickers[2]) || OnStack(g.pickers[3]);

        if (g.mmBack && ++g.mmBack > kMmBackFrames) {
            const StringId64 top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
            g.mmBack = 0;
            if (kPopOnMmBack && (top == g.findMatch || top == kLegacyFmPage))
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            TO1_LOG("postmatch: refused find-match latch cleared, top %llx%s\n",
                static_cast<unsigned long long>(top), kPopOnMmBack ? "" : " [page pop gated]");
        }
    }

    void Reset()
    {
        g.cl13 = g.post = g.eventUp = g.gear = false;
        g.mmBack = 0;
    }

    bool Cl13PostActive()  { return g.cl13; }
    bool ClanPostActive()  { return g.post; }
    bool ClanEventActive() { return g.eventUp; }
    bool GearUpNavActive() { return g.gear; }
    bool MmBackActive()    { return g.mmBack != 0; }

    bool ScreensDone()
    {
        return !g.cl13 && !g.post && !g.eventUp && !g.gear && g.mmBack == 0;
    }

    void NoteMmRefused()
    {
        if (!g.mmBack)
            TO1_LOG("postmatch: find match refused (party gate) - hold requeue's settle for a few frames\n");
        g.mmBack = 1;
    }

} // namespace net::ui::postmatch
