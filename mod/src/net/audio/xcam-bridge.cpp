// native xcam audio bridge: paired-move producer -> pure route -> exec music / shiv sting

#include "xcam-bridge.hpp"

#include "boot.hpp"
#include "exec-beg.hpp"
#include "exec-music.hpp"
#include "exec-sp.hpp"
#include "explus.hpp"
#include "shivsting.hpp"
#include "xcam.hpp"

#include "gameplay/melee2013.hpp"
#include "log.hpp"
#include "net/anim/paired.hpp"

#include <windows.h>

#include <cstdint>
#include <random>

namespace net::audio::xcam_bridge {

    namespace {

        bool ExtOwns() noexcept { return boot::ExtLoaded(); }

        struct Bridge final : xcam::Sink {
            void Exec(float killS) override
            {
                static std::mt19937 rng{ std::random_device{}() };
                const int pick = static_cast<int>(rng() % static_cast<uint32_t>(exec_music::kCues));
                exec_music::Start(GetTickCount64(), killS, pick);
            }
            void Shiv(const char* name) override
            {
                shivsting::ShivMusicStart(name);   // ext: ShivdrumStart first
                shivsting::Start(name);            // then ShivstingStart
            }
            void Upgraded(const char* name) override
            {
                (void)name;                    // log-only when TO1_DEBUG is off
                TO1_LOG("[XCamBridge] %s - upgraded melee one-shot, no execution music\n",
                        name ? name : "?");
            }
            void Log(const char* line) override
            {
                (void)line;                    // log-only when TO1_DEBUG is off
                TO1_LOG("[XCamBridge] %s\n", line);
            }
        };

        Bridge g_bridge;

        // The paired state keeps `uid` after End(), so a rising uid is the once-per-move edge
        // (ext g_gx start / xcam_start). Reset() clears it so a new match cannot replay a cue.
        uint32_t g_lastUid = 0;

        // Reconstruct the ext `name` / `killS` / upgraded flag from the native paired state.
        // melee2013's table is the same MELEE2013 array the ext indexed at its xcam_start
        // call, so the strings it hands Route() are byte-identical for those moves.
        void DispatchPaired(const net::anim::paired::State& ps)
        {
            const char* name = "melee-finisher";   // ext's fallback for an un-named move
            float killS = ps.killS;
            bool upgraded = false;
            int  pair = -1;
            bool bluntOk = false;
            if (ps.kind == net::anim::paired::kCtxdown) {
                name = "ctxdown";
            }
            else {
                const int mi = gameplay::melee2013::ByMove(ps.move);
                if (mi >= 0) {
                    const gameplay::melee2013::Move& mv = gameplay::melee2013::At(mi);
                    name = mv.name;
                    pair = mi;
                    // Coarse stand-in for ext mhfx_class(): the 2013 table's
                    // pipe quadrant is the only blunt-class node this producer
                    // carries. A future weapon-class producer can refine it.
                    bluntOk = (mv.kind == gameplay::melee2013::kPipe);
                    upgraded = (mv.kind == gameplay::melee2013::kPipeMod);
                    // The native victim bridge passes 0; the ext used the authored kill frame.
                    if (killS <= 0.0f)
                        killS = mv.killS;
                }
            }
            // The same once-per-move edge feeds the victim's plea state. When
            // execsp has authored EST_BEG rows, exec_beg opens the voice run
            // but leaves the plea timing to exec_sp's scheduled feeds.
            exec_beg::PairedStart plea;
            plea.uid = ps.uid;
            plea.role = ps.role;
            plea.kind = ps.kind;
            plea.attSlot = ps.att;
            plea.vicSlot = ps.vic;
            plea.vicEnt = ps.entVic;
            plea.move = ps.move;
            plea.killS = ps.killS;
            exec_beg::OnPairedStart(plea, !exec_sp::HasPleas(name));
            if (ps.kind == net::anim::paired::kMeleeExec && pair >= 0) {
                exec_sp::MeleeFeed feed;
                feed.uid = ps.uid;
                feed.pair = pair;
                feed.name = name;
                feed.attEnt = ps.entAtt;
                feed.vicEnt = ps.entVic;
                feed.t0 = net::anim::paired::Ms();
                feed.moveMs = feed.t0;
                feed.bluntOk = bluntOk;
                exec_sp::OnPairedStart(feed);
            }
            // The gun-exec rows need ext's GUNEXEC_PAIRS / GUNEXEC_AIM producer,
            // which has no native equivalent yet; with pair = -1 Begin() declines
            // those rows instead of guessing a timeline.
            if (ps.kind == net::anim::paired::kGunExec
                || ps.kind == net::anim::paired::kMeleeExec) {
                explus::PairFeed plus;
                plus.kind = ps.kind == net::anim::paired::kGunExec ? 0 : 1;
                plus.pair = pair;
                plus.name = name;
                plus.t0 = net::anim::paired::Ms();
                plus.uid = ps.uid;
                plus.attEnt = ps.entAtt;
                plus.vicEnt = ps.entVic;
                explus::OnPairedStart(plus);
            }
            OnCameraStart(ps.role, name, killS, upgraded);
        }

    }

    void Init()
    {
        if (kEnabled && !ExtOwns())
            xcam::SetSink(&g_bridge);
    }

    void Tick()
    {
        if (!kEnabled || ExtOwns())
            return;
        const net::anim::paired::State& ps = net::anim::paired::Get();
        if (ps.role == 0) {
            g_lastUid = 0;
            return;
        }
        if (ps.role != static_cast<int>(xcam::Role::kVictim) &&
            ps.role != static_cast<int>(xcam::Role::kAttacker))
            return;
        if (ps.uid == 0 || ps.uid == g_lastUid)
            return;
        g_lastUid = ps.uid;
        DispatchPaired(ps);   // Route() drops the attacker role; keep the edge for both.
    }

    void OnCameraStart(int role, const char* name, float killS, bool upgradedOneShot)
    {
        if (!kEnabled || ExtOwns())
            return;
        xcam::Dispatch(role, name, killS, upgradedOneShot);
    }

    void Reset()
    {
        g_lastUid = 0;
        xcam::Reset();
    }

} // namespace net::audio::xcam_bridge
