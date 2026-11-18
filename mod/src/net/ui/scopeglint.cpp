// ported from tools/ext/scopeglint.c

#include <winsock2.h>
#include <windows.h>

#include "scopeglint.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/raycast.hpp"
#include "engine/transform.hpp"
#include "engine/vfx.hpp"
#include "net-common.hpp"
#include "net-debug.hpp"
#include "net-protocol.hpp"
#include "net-session.hpp"
#include "net-gui2.hpp"
#include "net-player.hpp"
#include "net-engine.hpp"
#include "player-status.hpp"

#include "sid/sid.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace net::scopeglint {

    namespace {

        // ---- config (t2r-no-scope-glint.txt / t2r-scope-glint.txt) ----
        bool  g_off   = false;      // opt-out sidecar present: no message, no effect
        float g_alpha = 0.5f;       // max alpha at the cone centre
        float g_full  = 12.0f;      // fully bright within this angle (deg)
        float g_zero  = 60.0f;      // gone beyond this angle (deg)
        uint64_t g_cfgChk = 0;

        constexpr uint64_t kCfgInterval = 120;   // frames between config re-reads (ext: 120)

        // ---- sender state (the local player's own scoped state) ----
        bool     g_sending  = false;   // we are scoped and have told the others
        int      g_stopSends = 0;      // the stop sends still to do (ext: g_sgl_offs)
        uint64_t g_sendT60   = 0;      // t60 of the last message sent

        constexpr uint64_t kKeepAliveT60 = 30;   // 0.5 s
        constexpr uint64_t kStopGapT60   = 4;
        constexpr uint64_t kExpireT60    = 75;   // 1.25 s
        constexpr uint64_t kSpawnFailT60 = 180;  // back off this long after every joint failed
        constexpr uint64_t kRayNextFrames = 8;   // kick a line-of-sight ray at most every N frames

        constexpr uint64_t kInvalid   = static_cast<uint64_t>(-1);
        constexpr const char* kEffect = "sniper-scope-flare-1";      // the SP sniper flare

        // The SP joint first; the pistol / crossbow skeletons have no pump_mod.
        constexpr const char* kJoints[3] = { "pump_mod", "scope0", "root" };

        // alpha actually written per slot last frame (ext: static float last[] in SglFxTick)
        float g_lastAlpha[kMaxNetPlayers] = {};

        // ---- receiver state, one entry per slot (ext: g_sgl[]) ----
        struct Slot {
            bool     on = false;          // MSG_GLINT state of that slot
            uint64_t t60 = 0;             // t60 the last message arrived
            uint64_t fx = 0;              // the effect handle
            uint64_t h = 0;               // the puppet handle it was spawned for
            uint64_t wp = 0;              // the weapon process it was spawned for
            float    cur = 0.0f;          // smoothed alpha factor
            uint64_t ray = 0;             // the pending line-of-sight job
            uint64_t rayFrame = 0;        // the frame the job was kicked on
            uint64_t rayNext = 0;         // the frame the next job may be kicked on
            uint8_t  vis = 0;             // last line-of-sight answer
            uint64_t failUntil = 0;       // no more spawn attempts before this t60
            char     joint[12] = {};      // the joint the effect was spawned at (logging)
        };
        Slot g_slot[kMaxNetPlayers];

        void Cfg()
        {
            const uint64_t now = ::engine::frame::Count();
            if (g_cfgChk && now - g_cfgChk < kCfgInterval)
                return;
            g_cfgChk = now ? now : 1;

            // opt-out: t2r-no-scope-glint.txt next to the exe switches the whole feature off
            const bool off = !::engine::flag::Absent("t2r-no-scope-glint.txt", 120);
            if (off != g_off) {
                TO1_LOG("glint: %s (t2r-no-scope-glint.txt)\n",
                    off ? "OFF - no message, no effect" : "on");
                g_off = off;
            }

            // tunable: "max_alpha [cone_full_deg cone_zero_deg]"
            if (FILE* f = ::engine::exe_side::Open("t2r-scope-glint.txt", "r")) {
                float a, c0 = g_full, c1 = g_zero;
                const int n = fscanf(f, "%f %f %f", &a, &c0, &c1);
                if (n >= 1 && a >= 0.0f && a <= 1.0f && c0 >= 0.0f && c1 > c0 && c1 <= 179.0f) {
                    if (a != g_alpha || c0 != g_full || c1 != g_zero)
                        TO1_LOG("glint: t2r-scope-glint.txt - max alpha %.2f, full within %.0f deg, gone beyond %.0f deg\n",
                            a, c0, c1);
                    g_alpha = a;
                    g_full = c0;
                    g_zero = c1;
                }
                fclose(f);
            }
        }

        // send [u8 on] over the mod's own event transport (the ext's g_sendTramp).
        void Send(bool on)
        {
            const uint8_t m[1] = { static_cast<uint8_t>(on ? 1 : 0) };
            net::session::SendEventRaw(NEVT_SCOPE_GLINT, m, 1);
        }

        void KillEffect(uint32_t sl, const char* why)
        {
            if (!g_slot[sl].fx)
                return;
            (void)why;   // used by TO1_LOG, which is a no-op outside debug builds
            ::engine::vfx::KillParticles(g_slot[sl].fx);
            TO1_LOG("glint: slot %u effect %llx removed (%s)\n", sl,
                static_cast<unsigned long long>(g_slot[sl].fx), why);
            g_slot[sl].fx = 0;
            g_slot[sl].cur = 0.0f;
            g_slot[sl].ray = 0;
        }

        void KillAll(const char* why)
        {
            for (uint32_t sl = 0; sl < kMaxNetPlayers; sl++)
                KillEffect(sl, why);
        }

        // smoothstep, clamped (ext: SglSmooth)
        float Smooth(float x)
        {
            x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
            return x * x * (3.0f - 2.0f * x);
        }

        // spawn the flare on the weapon process, trying the joints in order (ext: SglSpawn).
        // Each joint is tried with flags 16 (the SP sniper flare's) then 2 (the plain fx spawns').
        uint64_t SpawnFlare(uint32_t sl, uint64_t wp)
        {
            for (int j = 0; j < 3; j++) {
                uint64_t r = ::engine::vfx::SpawnParticlesAtJoint(
                    SID(kEffect), wp, SID(kJoints[j]), 0, 16, SID("root"), 2, 0, 2, 0);
                if (!r || r == kInvalid) {
                    r = ::engine::vfx::SpawnParticlesAtJoint(
                        SID(kEffect), wp, SID(kJoints[j]), 0, 2, SID("root"), 2, 0, 2, 0);
                }
                TO1_LOG("glint: spawn %s at %s on slot %u's weapon process %llx -> %llx\n",
                    kEffect, kJoints[j], sl,
                    static_cast<unsigned long long>(wp),
                    static_cast<unsigned long long>(r));
                if (r && r != kInvalid) {
                    snprintf(g_slot[sl].joint, sizeof g_slot[sl].joint, "%s", kJoints[j]);
                    return r;
                }
            }
            return 0;
        }

        // ---- sender: our own scoped state -> MSG_GLINT --------------------------
        // The engine's own scoped state: aim component Player+0x2F68 byte +0x698, while aiming,
        // with a weapon in hand (ext: SglScoped). *wid = the held weapon id (logging only).
        bool Scoped(uint64_t* wid)
        {
            if (!::engine::local_player::ScopedIn())
                return false;
            if (!::engine::local_player::IsAiming())
                return false;
            const uint64_t w =
                net::engine::GetPlayerWeaponId(::engine::local_player::Ptr());
            if (!w || w == kInvalid)
                return false;
            *wid = w;
            return true;
        }

        // once per weapon id: the engine's scoped state next to the scope-upgrade native (never
        // a gate - bow / Vepr clones differ). Logging only (ext: SglLogWeapon).
        void LogWeapon(uint64_t w)
        {
            static uint64_t seen[16];
            static int n;
            for (int i = 0; i < n; i++)
                if (seen[i] == w)
                    return;
            if (n < 16)
                seen[n++] = w;
            const uint64_t r = DC::Call<"player-has-weapon-upgrade?">(w, SID("scope"));
            TO1_LOG("glint: scoped in with weapon %llx - player-has-weapon-upgrade?(.., scope) = %lld\n",
                static_cast<unsigned long long>(w),
                static_cast<long long>(r == kInvalid ? -1 : static_cast<int64_t>(r & 0xff)));
        }

        // SglTick's sender half: the edge + keep-alive + 3x-stop schedule over our own scoped
        // state. The scoped byte is read every 2nd frame; in between the state holds.
        void Sender()
        {
            const uint64_t f   = ::engine::frame::Count();
            const uint64_t t60 = ::engine::frame::T60();

            bool on = false;
            uint64_t w = 0;
            const bool gates = net::player_status::MatchLive()
                && !net::player_status::LocalDowned()
                && net::player_status::LocalAlive()
                && !net::player_status::LocalInPairedMove();
            if (gates && !(f & 1))
                on = Scoped(&w);
            else if (f & 1)
                on = g_sending;             // in between the read holds

            if (on != g_sending) {
                g_sending = on;
                if (on)
                    LogWeapon(w);
                TO1_LOG("glint: our scope %s\n",
                    on ? "IN - telling the others" : "out");
                Send(on);
                g_sendT60   = t60;
                g_stopSends = on ? 0 : 2;   // the stop 3x in all (this one + 2)
            }
            else if (on && t60 - g_sendT60 >= kKeepAliveT60) {
                Send(true);                 // keep-alive (the receiver expires it after 1.25 s)
                g_sendT60 = t60;
            }
            else if (!on && g_stopSends > 0 && t60 - g_sendT60 >= kStopGapT60) {
                g_stopSends--;
                g_sendT60 = t60;
                Send(false);
            }
        }

        // ---- receiver: a scoped puppet -> the flare effect ----------------------
        // SglFxTick: for each remote slot the flare lives on the puppet's gun while the sender
        // says "scoped", its alpha driven by max_alpha * facing * line-of-sight.
        void Receiver()
        {
            const uint64_t f   = ::engine::frame::Count();
            const uint64_t t60 = ::engine::frame::T60();
            const uint32_t me  = static_cast<uint32_t>(net::session::MySlot());

            bool camTried = false;      // ext: have_cam (-1 unknown, then resolved once)
            bool haveCam  = false;
            Vector3 cam{};

            for (uint32_t sl = 0; sl < kMaxNetPlayers; sl++) {
                if (sl == me)
                    continue;           // never on our own screen

                NetPlayer& p = NetPlayers::BySlot(sl);
                const uint64_t h = p.npcSid;    // the puppet handle (ext: peer+0x60)

                bool act = g_slot[sl].on && (t60 - g_slot[sl].t60) <= kExpireT60;
                if (g_slot[sl].on && !act)
                    g_slot[sl].on = false;      // expired

                if (!act || !h || !net::player_status::PuppetAlive(sl)
                    || net::player_status::PuppetDowned(sl)) {
                    if (g_slot[sl].fx)
                        KillEffect(sl, act ? "puppet gone / dead / downed" : "scope down");
                    continue;
                }

                const uintptr_t chr = p.Ptr();  // ext: EntityFromHandle(h)
                const uint64_t wp = DC::Call<"npc-get-weapon-process-id">(h);
                if (!chr || !wp || wp == kInvalid)
                    continue;
                if (g_slot[sl].fx && (g_slot[sl].h != h || g_slot[sl].wp != wp))
                    KillEffect(sl, "new puppet / weapon");

                // facing: the barrel (+z) against the direction to the viewer's camera
                const uintptr_t w = net::engine::GetNpcWeapon(chr);
                float t[8] = {};
                float tgt = 0.0f;
                if (w) {
                    if (!camTried) {
                        camTried = true;
                        haveCam = ::engine::raycast::CameraPosition(cam);
                    }
                    if (haveCam && ::engine::transform::ProcessWorld(w, t)) {
                        const Vector3 zf = Quaternion{ t[4], t[5], t[6], t[7] }
                            .rotate(Vector3{ 0.0f, 0.0f, 1.0f });
                        const Vector3 v{ cam.x - t[0], cam.y - t[1], cam.z - t[2] };
                        const float d = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                        if (d > 1.5f) {
                            const float c = (zf.x * v.x + zf.y * v.y + zf.z * v.z) / d;
                            const float cFull = std::cos(g_full * 0.01745329f);
                            const float cZero = std::cos(g_zero * 0.01745329f);
                            tgt = Smooth((c - cZero) / (cFull - cZero));
                            // line of sight: ray camera -> gun, kicked one frame, read a later one
                            if (tgt > 0.01f) {
                                if (!g_slot[sl].ray) {
                                    if (f >= g_slot[sl].rayNext && g_slot[sl].rayFrame != f) {
                                        g_slot[sl].ray = ::engine::raycast::KickWorldRay(
                                            cam, Vector3{ t[0], t[1], t[2] });
                                        g_slot[sl].rayFrame = f;
                                        g_slot[sl].rayNext  = f + kRayNextFrames;
                                        if (!g_slot[sl].ray)
                                            g_slot[sl].vis = 1;   // no job = refused: counts as clear
                                    }
                                }
                                else if (g_slot[sl].rayFrame != f) {
                                    const float rt = ::engine::raycast::RayT(g_slot[sl].ray);
                                    g_slot[sl].ray = 0;
                                    const int vis = !(rt >= 0.0f && rt < 0.95f);
                                    g_slot[sl].vis = static_cast<uint8_t>(vis);
                                }
                                if (!g_slot[sl].vis)
                                    tgt = 0.0f;
                            }
                            else
                                g_slot[sl].vis = 1;
                        }
                    }
                }

                g_slot[sl].cur += (tgt - g_slot[sl].cur) * 0.25f;
                if (g_slot[sl].cur < 0.004f)
                    g_slot[sl].cur = 0.0f;

                if (!g_slot[sl].fx) {
                    if (tgt < 0.03f || t60 < g_slot[sl].failUntil)
                        continue;
                    const uint64_t r = SpawnFlare(sl, wp);
                    if (!r) {
                        g_slot[sl].failUntil = t60 + kSpawnFailT60;
                        TO1_LOG("glint: spawn failed on every joint - is part-rifle-tommy (pak68) loaded in this build?\n");
                        continue;
                    }
                    g_slot[sl].fx = r;
                    g_slot[sl].h  = h;
                    g_slot[sl].wp = wp;
                    ::engine::vfx::SetEffectAlpha(r, 0.0f);
                }

                const float al = g_slot[sl].cur * g_alpha;
                if (std::fabs(al - g_lastAlpha[sl]) > 0.01f
                    || (al == 0.0f && g_lastAlpha[sl] != 0.0f)) {
                    ::engine::vfx::SetEffectAlpha(g_slot[sl].fx, al);
                    g_lastAlpha[sl] = al;
                }
            }
        }

    }

    void Tick()
    {
        Cfg();

        if (!net::gui2::InMatch()) {
            if (g_sending) {
                g_sending = false;
                TO1_LOG("glint: left the match\n");
            }
            Reset();
            return;
        }

        if (g_off) {
            if (g_sending) {
                g_sending = false;
                Send(false);
            }
            KillAll("t2r-no-scope-glint.txt");
            return;
        }

        Receiver();
        Sender();
    }

    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        if (senderSlot >= kMaxNetPlayers || !data || size < 1)
            return;

        Slot& s = g_slot[senderSlot];
        const bool on = data[0] != 0;
        if (s.on != on)
            TO1_LOG("glint: slot %u %s\n", senderSlot, on ? "scoped in" : "scope down");
        s.on = on;
        const uint64_t t60 = ::engine::frame::T60();
        s.t60 = t60 ? t60 : 1;
    }

    void Reset()
    {
        KillAll("reset");
        for (Slot& s : g_slot)
            s = Slot{};
        g_sending = false;
        g_stopSends = 0;
        g_sendT60 = 0;
    }

}
