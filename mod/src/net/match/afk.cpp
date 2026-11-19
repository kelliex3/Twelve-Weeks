// ported from tools/ext/afk.c

#include "afk.hpp"

#include <winsock2.h>        // daemon-rpc.hpp uses sockaddr-in types

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/draw.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/pad.hpp"
#include "engine/script.hpp"
#include "engine/window.hpp"
#include "daemon-rpc.hpp"
#include "net-gui2.hpp"
#include "player-status.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace net::afk {

    namespace {

        // ---- constants (ext AFK_*) ----------------------------------------------
        constexpr uint64_t kWarnMs       = 60000;   // warning after ~60 s idle
        constexpr uint64_t kKickMs       = 90000;   // removal at ~90 s (drives the countdown only)
        constexpr uint64_t kPadWindowMs  = 250;     // ext: any pad activity within 250 ms
        constexpr int      kFlagFrames   = 300;     // ext: re-check the sidecar every ~5 s

        struct State {
            int      off = -1;                      // -1 unknown, 0 on, 1 off
            uint64_t prev = 0, idle = 0, warnAt = 0, loggedS = 0, vis = 0;
            uint32_t lii = 0;                       // last GetLastInputInfo tick
            uint32_t btn = 0, st = 0;               // last virtual-pad injection counters
            int      counting = 0, warn = 0, deadC = 0;
            uint64_t deadF = 0;
            uint8_t  wasLive = 0;
        };
        State g_afk;

        int AfkOff()
        {
            // ext re-reads the sidecar every 5 s; engine::flag caches per frame count, so
            // asking it every frame is the same cheap re-check. Log on change only.
            const int o = engine::flag::Present("t2r-no-afk.txt", kFlagFrames) ? 1 : 0;
            if (o != g_afk.off) {
                g_afk.off = o;
                TO1_LOG("afk: %s\n",
                    o ? "off (t2r-no-afk.txt)"
                      : "on (warning at 60 s idle, the server removes at 90 s)");
            }
            // ext: g_srvTestFlags & 0x40 -- the test server removes nobody for inactivity
            // (T2R_AFK_KICK_S=0); the flag arrives in the trailing byte of the 0xE5 JIP
            // reply and is published by net::daemon::TestFlags().
            return o || (net::daemon::TestFlags() & 0x40) != 0;
        }

        int BpOwnDead()
        {
            // ext BpOwnDead(): character-is-dead? on the local player, low byte. -1 (the
            // native is missing) counts as not dead, like the ext's ~0ULL guard.
            const uint64_t d = DC::Call<"character-is-dead?">(SID("player"));
            return d != static_cast<uint64_t>(-1) && (d & 0xff);
        }

        int AfkCountable()
        {
            // ext: MatchSubstate() == 6 and the match is not over (LobbyObj()[0x2ca4]).
            if (!net::player_status::MatchLive())
                return 0;
            // ext: g_downed || DuDeadNow() (downed / death screen / spectating). Downed is
            // the shared stub in player-status; HP <= 0 approximates the death screen and
            // the fresh kill-signal window (DuDeadNow's phase 1 / 1.5 s edge), which the
            // mod's death UI does not expose separately.
            if (net::player_status::LocalDowned() || !net::player_status::LocalAlive())
                return 0;
            // ext: a native character-is-dead? call, every 15 frames.
            if (engine::frame::Count() - g_afk.deadF >= 15) {
                g_afk.deadF = engine::frame::Count();
                g_afk.deadC = BpOwnDead();
            }
            if (g_afk.deadC)
                return 0;
            // ext: g_tabOpen || g_gu13Up || PlayerMenuActive() == 1 -- menus that are part
            // of play. GearUpOpen() checks the page name only (the gearup13 feature is
            // still unported), which is all the ext's g_gu13Up latch reads.
            if (net::gui2::BackpackOpen() || net::gui2::GearUpOpen()
                || net::gui2::PlayerMenuActive() == 1)
                return 0;
            return 1;
        }

        uint64_t AfkRepMs()
        {
            // The idle time that counts toward the removal: the warning must have been
            // visible (see the header), so before the warning it is idle and after it is
            // kWarnMs + the time the warning was actually on screen.
            if (!g_afk.warn)
                return g_afk.idle;
            const uint64_t r = kWarnMs + g_afk.vis;
            return r < g_afk.idle ? r : g_afk.idle;
        }

    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        uint64_t dt = g_afk.prev && now > g_afk.prev ? now - g_afk.prev : 0;
        g_afk.prev = now;
        if (dt > 1000)
            dt = 1000;              // a hitch / load stall counts at most 1 s

        const int live = net::gui2::InMatch();
        if (live && !g_afk.wasLive) {
            g_afk.idle = 0;
            g_afk.warn = 0;
            g_afk.vis = 0;
            g_afk.loggedS = 0;
            // Seed the injection counters so a press queued before the match does not
            // read as player activity on the first counted frame.
            g_afk.btn = engine::pad::Applied();
            g_afk.st = engine::pad::StickApplied();
        }                           // a new match starts at 0
        g_afk.wasLive = static_cast<uint8_t>(live);

        if (AfkOff() || !engine::pad::Ready()) {
            g_afk.idle = 0;
            g_afk.warn = 0;
            g_afk.vis = 0;
            g_afk.counting = 0;
            return;
        }

        int input = 0;
        uint32_t lii = 0;
        if (engine::window::LastInputTick(&lii)) {
            if (lii != g_afk.lii && engine::window::Focused())
                input = 1;          // keyboard / mouse while the game has the focus
            g_afk.lii = lii;
        }
        const uint64_t padAct = engine::pad::ActivityMs();
        if (padAct && now - padAct < kPadWindowMs)
            input = 1;              // any pad
        // The virtual pad now reports its own applied-button / applied-stick counters.
        // A change means the engine applied an injected press / stick this frame, which
        // the ext counted as player activity (a bot that plays must not be kicked). The
        // real-pad stamp above was taken before injection, so an injected press never
        // counts as a *real* pad via that path.
        const uint32_t injBtn = engine::pad::Applied();
        const uint32_t injSt = engine::pad::StickApplied();
        if (injBtn != g_afk.btn || injSt != g_afk.st)
            input = 1;
        g_afk.btn = injBtn;
        g_afk.st = injSt;

        if (input) {
            if (g_afk.warn)
                TO1_LOG("afk: input after %.0f s idle - warning cleared\n",
                    g_afk.idle / 1000.0);
            else if (g_afk.idle >= 30000)
                TO1_LOG("afk: input after %.0f s idle\n", g_afk.idle / 1000.0);
            g_afk.idle = 0;
            g_afk.warn = 0;
            g_afk.vis = 0;
            g_afk.loggedS = 0;
        }

        g_afk.counting = !input && AfkCountable();
        if (g_afk.counting)
            g_afk.idle += dt;
        if (g_afk.counting && g_afk.warn && !net::gui2::MenuUp())
            g_afk.vis += dt;
        if (!g_afk.warn && g_afk.idle >= kWarnMs) {
            g_afk.warn = 1;
            g_afk.warnAt = now;
            g_afk.vis = 0;
            TO1_LOG("afk: no input for %.0f s - warning shown (the server removes us at %d s)\n",
                g_afk.idle / 1000.0, static_cast<int>(kKickMs / 1000));
        }
        if (g_afk.warn && g_afk.idle / 10000 != g_afk.loggedS) {
            g_afk.loggedS = g_afk.idle / 10000;
            TO1_LOG("afk: idle %.0f s, warning visible %.0f s%s\n",
                g_afk.idle / 1000.0, g_afk.vis / 1000.0,
                AfkRepMs() >= kKickMs ? " - removal due, waiting for the server" : "");
        }
    }

    void ReqTail(uint8_t* p)
    {
        // ext AfkReqTail(): [0xAF][u16 idle s, 0xFFFF = unknown / off][flags].
        const int known = !AfkOff() && engine::pad::Ready() && net::gui2::InMatch();
        uint64_t s = AfkRepMs() / 1000;
        if (s > 0xFFFE)
            s = 0xFFFE;
        const uint16_t v = known ? static_cast<uint16_t>(s) : 0xFFFF;
        p[0] = 0xAF;
        p[1] = static_cast<uint8_t>(v);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>((g_afk.counting ? 1 : 0) | (g_afk.warn ? 2 : 0));
    }

    void Draw()
    {
        if (!g_afk.warn || !g_afk.counting || !net::gui2::InMatch() || net::gui2::MenuUp())
            return;                 // (dead / downed / in the store: the count is frozen, nothing shown)

        int64_t left = (static_cast<int64_t>(kKickMs) - static_cast<int64_t>(AfkRepMs()) + 999) / 1000;
        if (left < 0)
            left = 0;

        float a = static_cast<float>(GetTickCount64() - g_afk.warnAt) / 300.0f;
        if (a > 1.0f)
            a = 1.0f;               // 0.3 s fade in
        const uint32_t al = static_cast<uint32_t>(a * 255.0f + 0.5f) << 24;

        char l2[128];
        const char* h = engine::locale::TranslateDraw("YOU ARE INACTIVE");
        snprintf(l2, sizeof l2,
            "MOVE OR YOU WILL BE REMOVED FROM THE MATCH IN %d", static_cast<int>(left));
        const char* t2 = engine::locale::TranslateDraw(l2);
        const char* L[2] = { h, t2 };
        const float S[2] = { 1.2f, 0.9f };
        const float Y[2] = { 236.0f, 266.0f };
        const uint32_t C[2] = { 0x00E1B23Cu, 0x00DCE6EAu };
        for (int i = 0; i < 2; i++) {
            const vec2 wh = DebugDrawCommon::GetTextWidthHeight(L[i], S[i]);
            const float w = wh.x;
            if (!(w > 0.0f && w < 1280.0f))
                continue;
            DebugDrawCommon::DrawTextPrintV(640.0f - w * 0.5f, Y[i], S[i],
                al | C[i], "%s", L[i]);
        }
    }

    void Reset()
    {
        const int off = g_afk.off;
        g_afk = State{};
        g_afk.off = off;
    }

}
