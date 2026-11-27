// ported from tools/ext/immersion.c (NEAR MISS: MSG_SHOTRAY send/recv + ImmNmTick whizz).
// All rules live in near-miss-core.hpp; engine / audio / net seams stay here.

#include "near-miss.hpp"

#include "near-miss-pcm.hpp"

#include "alertsting.hpp"

#include "boot.hpp"
#include "engine/backpack-prop.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/object.hpp"
#include "engine/pcm.hpp"
#include "engine/ptr.hpp"
#include "engine/raycast.hpp"
#include "engine/render-camera.hpp"
#include "engine/script.hpp"
#include "log.hpp"

#include "net/base/net-common.hpp"
#include "net/game/player-status.hpp"
#include "net/match/match.hpp"
#include "net/match/net-player.hpp"
#include "net/online/net-session.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>

namespace net::audio::near_miss {

    namespace {

        namespace core = net::audio::near_miss::core;
        namespace sink = net::audio::near_miss::pcm;

        core::State g;

        bool SenderOn() noexcept
        {
            return !boot::ExtLoaded() && engine::flag::Absent("t2r-no-shotray.txt");
        }

        bool ReceiverOn() noexcept
        {
            return !boot::ExtLoaded() && engine::flag::Absent("t2r-no-near-miss.txt");
        }

        // ext SfxSpatial's listener: final render camera first, then the reticle pair,
        // then our own body at eye height (never drop a sound just for lack of a camera).
        bool Listener(float cp[3], float cd[3]) noexcept
        {
            if (engine::render_camera::Get(cp, cd))
                return true;
            Vector3 p;
            if (engine::raycast::CameraPosition(p)) {
                cp[0] = p.x; cp[1] = p.y; cp[2] = p.z;
                cd[0] = cd[1] = cd[2] = 0.0f;
                const Vector3* d = DC::Call<"camera-get-dir-through-reticle", const Vector3>();
                if (d && std::isfinite(d->x) && std::isfinite(d->z)) {
                    cd[0] = d->x; cd[1] = d->y; cd[2] = d->z;
                }
                return true;
            }
            float w[8] = {};
            if (!engine::object::LocalPlayerWorld(w)
                || !std::isfinite(w[0] + w[1] + w[2]))
                return false;
            cp[0] = w[0]; cp[1] = w[1] + core::kLosEyeUpM; cp[2] = w[2];
            cd[0] = cd[1] = cd[2] = 0.0f;
            return true;
        }

        uint64_t KickRay(const float from[3], const float to[3]) noexcept
        {
            return engine::raycast::KickWorldRay(
                Vector3(from[0], from[1], from[2]),
                Vector3(to[0], to[1], to[2]),
                engine::raycast::kMaskStaticWorld, SID("player"));
        }

        float RayT(uint64_t job) noexcept { return engine::raycast::RayT(job); }

        // ---- ext's shot source: clip-decrement poll + raw aim helper -------------------

        constexpr uintptr_t kOffAimHelper = 0x3278;   // player -> PlayerAimController
        constexpr size_t    kPlayerBytes  = 0x5530;
        constexpr size_t    kAimBytes     = 0x40;
        constexpr uintptr_t kMinAimObj    = 0x10000;

        bool LogCapped(int& n, int cap) noexcept
        {
            return n < cap ? (++n, true) : false;
        }

        bool IsCodePtr(const void* p) noexcept
        {
            if (!p) return false;
            MEMORY_BASIC_INFORMATION mi{};
            return VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT
                && (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                                  | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        }

        // ext base_weapon(): "<base>-supp" clones map back to their base, and Dragon's
        // Buckshot maps to the Double Barrel. SID() computes the same FNV-1a at compile time.
        constexpr StringId64 kFnvOffset = 0xCBF29CE484222325ULL;
        constexpr StringId64 kFnvPrime  = 0x100000001B3ULL;

        constexpr StringId64 SidSupp(const char* base) noexcept
        {
            StringId64 h = kFnvOffset;
            for (const char* p = base; *p; ++p)
                h = kFnvPrime * (h ^ static_cast<unsigned char>(*p));
            const char suffix[] = "-supp";
            for (const char* p = suffix; *p; ++p)
                h = kFnvPrime * (h ^ static_cast<unsigned char>(*p));
            return h;
        }

        struct ClonePair { StringId64 clone, base; };

        uint64_t BaseWeapon(uint64_t wid) noexcept
        {
            // The clone table is built from the same base names as ext CLONED_BASE[].
            static const ClonePair clones[] = {
                { SidSupp("rifle-vepr"),                 SID("rifle-vepr") },
                { SidSupp("rifle-remington-bolt"),       SID("rifle-remington-bolt") },
                { SidSupp("pistol-beretta"),              SID("pistol-beretta") },
                { SidSupp("pistol-glock"),                SID("pistol-glock") },
                { SidSupp("rifle-mpx5"),                  SID("rifle-mpx5") },
                { SidSupp("rifle-m14-semi"),              SID("rifle-m14-semi") },
                { SidSupp("pistol-cz75-enforcer"),        SID("pistol-cz75-enforcer") },
                { SidSupp("shotgun-tactical"),            SID("shotgun-tactical") },
                { SidSupp("rifle-burst"),                 SID("rifle-burst") },
                { SidSupp("rifle-frontier"),              SID("rifle-frontier") },
            };
            for (const ClonePair& c : clones)
                if (wid == c.clone) return c.base;
            if (wid == SID("shotgun-dragon"))
                return SID("shotgun-baserri-over-under");
            return wid;
        }

        bool BaseWeaponIsGun(uint64_t wid) noexcept
        {
            // ext base_weapon_is_gun()'s G[] verbatim; the five macro ids are the ext's
            // ASSAULT_ID / EXPBOW_ID / FLAME_ID / VINTAGE_ID / LAUNCHER_ID strings.
            static constexpr StringId64 kGuns[] = {
                SID("rifle-vepr"), SID("rifle-remington-bolt"), SID("rifle-mpx5"),
                SID("rifle-m14-semi"), SID("rifle-burst"), SID("shotgun-remington-pump"),
                SID("shotgun-baserri-over-under"), SID("shotgun-tactical"), SID("bow-ellie"),
                SID("crossbow"), SID("pistol-beretta"), SID("pistol-glock"),
                SID("pistol-revolver-taurus"), SID("pistol-cz75-enforcer"), SID("rifle-galil"),
                SID("pistol-xcaliber"), SID("pistol-el-diablo"), SID("shotgun-shorty"),
                SID("rifle-frontier"), SID("rifle-milsniper"), SID("pistol-snubby"),
                SID("rifle-assault-firefly-ellie-player"), SID("bow-explosive"),
                SID("flamethrower"), SID("rifle-vintage"), SID("t2r-launcher"),
            };
            const uint64_t b = BaseWeapon(wid);
            for (StringId64 g : kGuns)
                if (g == b) return true;
            return false;
        }

        // ext held_weapon()/player_weapon(): character vtable +0xD38, with the controller
        // fallback (player+0x2F60 -> exe+0xBDC1F0) the ext uses for the local player.
        uint8_t* HeldWeapon(uint8_t* chr) noexcept
        {
            const uintptr_t p = reinterpret_cast<uintptr_t>(chr);
            if (!p || !engine::ptr::Readable(p, 8)) return nullptr;
            void** vt = *reinterpret_cast<void***>(p);
            if (!vt || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(vt) + 0xD38 / 8, 8))
                return nullptr;
            const void* fn = vt[0xD38 / 8];
            if (!IsCodePtr(fn)) return nullptr;
            return reinterpret_cast<uint8_t* (*)(void*)>(fn)(chr);
        }

        uint8_t* PlayerWeapon(uint8_t* player) noexcept
        {
            const uintptr_t p = reinterpret_cast<uintptr_t>(player);
            if (!p || !engine::ptr::Readable(p + 0x2F60, 8)) return nullptr;
            uint8_t* v3 = *reinterpret_cast<uint8_t**>(p + 0x2F60);
            if (!v3 || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(v3), 8))
                return nullptr;
            return GameFn<uint8_t* (*)(void*)>(0xBDC1F0)(v3);
        }

        // ext cant_gun_empty()'s raw ammo read (uses-ammo? vfunc +0xBF8, exe+0x1185160 /
        // +0x1185EC0 / +0x1185EA0). Returns -1 when the held gun cannot be read.
        void ReadClipRaw(uint8_t* player, int& clip, int& reserve) noexcept
        {
            clip = reserve = -1;
            uint8_t* w = HeldWeapon(player);
            if (!w) w = PlayerWeapon(player);
            if (!w) return;
            uint8_t* fw = GameFn<uint8_t* (*)(void*)>(0x1184B60)(w);
            if (!fw || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(fw), 0xB30)) return;
            void** vt = *reinterpret_cast<void***>(fw);
            if (!vt || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(vt) + 0xBF8 / 8, 8))
                return;
            const void* usesAmmo = vt[0xBF8 / 8];
            if (!IsCodePtr(usesAmmo)) return;
            if ((reinterpret_cast<int (*)(void*)>(usesAmmo)(fw) & 0xff) <= 0) return;
            uint64_t ammo = 0;
            GameFn<uint64_t* (*)(void*, uint64_t*)>(0x1185160)(fw, &ammo);
            void* inv = GameFn<void* (*)(void*)>(0xDAA1C0)(player);
            reserve = GameFn<int (*)(void*, uint64_t, void*)>(0x1185EC0)(fw, ammo, inv);
            clip = GameFn<int (*)(void*, uint64_t)>(0x1185EA0)(fw, ammo);
        }

        int HeldClipRounds() noexcept
        {
            // ext VMA(0x180050390)() is the local player object (g_base + 0x50390).
            uint8_t* me = GameFn<uint8_t* (*)()>(0x50390)();
            int clip = -1, reserve = -1;
            if (!me || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(me) + 0x2F60, 0x30))
                return -1;
            ReadClipRaw(me, clip, reserve);
            (void)reserve;
            if (clip >= 0) return clip;

            // ext's DC fallbacks: first plausible 0..500 wins, same call order.
            constexpr uint64_t kInvalid = ~0ULL;
            const uint64_t wid = engine::backpack_prop::CurrentWeapon();
            const uint64_t r1 = (wid && wid != kInvalid)
                ? DC::Call<"get-player-weapon-ammo-in-clip">(wid) : kInvalid;
            const uint64_t r0 = (r1 == kInvalid || static_cast<uint32_t>(r1) > 500u)
                ? DC::Call<"get-player-weapon-ammo-in-clip">() : kInvalid;
            const uint64_t r2 = DC::Call<"get-weapon-ammo-in-clip">(wid);
            if (r1 != kInvalid && static_cast<uint32_t>(r1) <= 500u)
                clip = static_cast<int>(static_cast<uint32_t>(r1));
            else if (r0 != kInvalid && static_cast<uint32_t>(r0) <= 500u)
                clip = static_cast<int>(static_cast<uint32_t>(r0));
            else if (r2 != kInvalid && static_cast<uint32_t>(r2) <= 500u)
                clip = static_cast<int>(static_cast<uint32_t>(r2));
            return clip;
        }

        int      s_prevClip = -1;
        uint64_t s_prevWeapon = 0;
        uint64_t s_lastFire = 0;
        int      s_nBad = 0, s_nSent = 0;

        void SendShotray(uint64_t now, uint64_t weapon, int clip)
        {
            // Only used by the debug build's capped log; keep release builds warning-clean.
            (void)weapon;
            (void)clip;
            if (!core::SendReady(g, now)) return;

            // ext exe+0xDABEA0 / +0xC72440: the aim helper the bow arc uses. Sending stays
            // here; no raw hook ever calls SendEvent.
            uint8_t* pl = GameFn<uint8_t* (*)()>(0xDABEA0)();
            if (!pl || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(pl), kPlayerBytes))
                return;
            uint8_t* aimo = *reinterpret_cast<uint8_t**>(pl + kOffAimHelper);
            if (!aimo || (reinterpret_cast<uintptr_t>(aimo) & 7)
                || reinterpret_cast<uintptr_t>(aimo) < kMinAimObj
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(aimo), kAimBytes)
                || *reinterpret_cast<uint8_t**>(aimo) != pl) {
                if (LogCapped(s_nBad, 4))
                    TO1_LOG("[NEARMISS] shotray: no aim helper (player+0x3278) - line not sent\n");
                return;
            }
            alignas(16) float out[12] = {};
            GameFn<void (*)(void*, void*)>(0xC72440)(aimo, out);
            const float* org = out + 4;
            const float* dir = out + 8;
            if (!core::AimRayValid(org, dir)) {
                if (LogCapped(s_nBad, 4))
                    TO1_LOG("[NEARMISS] shotray: camera ray invalid - line not sent\n");
                return;
            }
            float d[3], o[3];
            const float dd = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
            const float inv = 1.0f / std::sqrt(dd);
            for (int j = 0; j < 3; ++j) {
                d[j] = dir[j] * inv;
                o[j] = org[j];
            }
            float w[8] = {};
            if (engine::object::LocalPlayerWorld(w)
                && std::isfinite(w[0] + w[1] + w[2])) {
                const float chest[3] = { w[0], w[1] + core::kHeadUpM, w[2] };
                core::SlideOriginToChest(o, d, chest);
            }
            if (!core::ShotValid(o, d)) {
                if (LogCapped(s_nBad, 4))
                    TO1_LOG("[NEARMISS] shotray: slid origin invalid - line not sent\n");
                return;
            }

            ShotRayEvent evt{};
            evt.reserved = 0;
            for (int j = 0; j < 3; ++j) {
                evt.origin[j] = o[j];
                evt.dir[j] = core::QuantDir(d[j]);
            }
            evt.lenM = 0;          // unknown hit distance: the receiver uses 120 m
            net::session::SendEvent(NEVT_SHOTRAY, evt);
            core::NoteSent(g, now);
            if (LogCapped(s_nSent, 16))
                TO1_LOG("[NEARMISS] shotray: sent - weapon %llx clip %d, "
                        "origin (%.1f %.1f %.1f) dir (%.2f %.2f %.2f)\n",
                        static_cast<unsigned long long>(weapon), clip,
                        static_cast<double>(o[0]), static_cast<double>(o[1]),
                        static_cast<double>(o[2]), static_cast<double>(d[0]),
                        static_cast<double>(d[1]), static_cast<double>(d[2]));
        }

        void TickSender(uint64_t now, bool live)
        {
            if (!live || !SenderOn()) {
                s_prevClip = -1;
                return;
            }
            const uint64_t weapon = engine::backpack_prop::CurrentWeapon();
            if (!weapon || weapon == ~0ULL || !BaseWeaponIsGun(weapon)) {
                s_prevClip = -1;
                return;
            }
            const uint64_t fr = DC::Call<"player-is-firing?">();
            if (fr != ~0ULL && (fr & 0xff))
                s_lastFire = now;
            const int clip = HeldClipRounds();
            if (clip < 0) {
                s_prevClip = -1;
                return;
            }
            const bool shot = core::ClipDecrementShot(s_prevClip, weapon, s_prevWeapon,
                                                      clip, s_lastFire, now);
            s_prevClip = clip;
            s_prevWeapon = weapon;
            if (shot)
                SendShotray(now, weapon, clip);
        }

        void WarmLos(const float ourRoot[3], uint64_t t60, uint64_t frame)
        {
            const ::net::match::Match& m = ::net::match::Get();
            for (int s = 0; s < kMaxNetPlayers; ++s) {
                const NetPlayer& p = m.players[s];
                if (!p.active || !p.hasState || p.IsLocal()
                    || NetPlayers::SlotIsFriendly(s))
                    continue;
                const uint64_t handle = static_cast<uint64_t>(p.npcSid);
                if (!handle) continue;
                float tw[8] = {};
                if (!engine::object::EntityWorld(p.npcSid, tw)
                    || !std::isfinite(tw[0] + tw[1] + tw[2]))
                    continue;                   // ext skips a missing locator
                const float target[3] = { tw[0], tw[1], tw[2] };
                const float dx = target[0] - ourRoot[0], dz = target[2] - ourRoot[2];
                if (dx * dx + dz * dz > core::kLosRange * core::kLosRange)
                    continue;
                core::LosGate(g, handle, t60, frame, ourRoot, target, KickRay, RayT);
            }
        }

    } // namespace

    void OnLocalShot()
    {
        // Deprecated compatibility shim: ext does not use a shot hook. The true producer
        // is Tick()'s clip-decrement poll (TickSender), so this intentionally does nothing
        // until the caller in net-hooks.cpp is removed.
    }

    void OnShotRay(int slot, const ShotRayEvent& evt)
    {
        if (boot::ExtLoaded() || slot < 0 || slot >= kMaxNetPlayers)
            return;
        const float o[3] = { evt.origin[0], evt.origin[1], evt.origin[2] };
        const float d[3] = { core::DequantDir(evt.dir[0]),
                             core::DequantDir(evt.dir[1]),
                             core::DequantDir(evt.dir[2]) };
        if (!core::ShotValid(o, d))
            return;
        const float inv = 1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float unit[3] = { d[0] * inv, d[1] * inv, d[2] * inv };
        const float len = evt.lenM ? static_cast<float>(evt.lenM) : core::kMaxLen;
        core::StoreShot(g, static_cast<uint32_t>(slot), GetTickCount64(), o, unit, len);
    }

    void NoteHitFrom(int slot, uint64_t nowMs) noexcept
    {
        core::NoteHit(g, slot, nowMs);
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        const bool live = net::player_status::MatchLive();
        TickSender(now, live);                     // ext imm_sray_send_tick; independent of ReceiverOn

        if (!live || !ReceiverOn()) {
            core::DropPending(g, core::RingEnd(g));
            return;
        }
        const uint64_t t60 = engine::frame::T60();
        const uint64_t frame = engine::frame::Count();
        const uint32_t end = core::RingEnd(g);

        // Whizzes that waited out the hit check. A hit from that slot within 250 ms of the
        // line means that shot connected, so no whizz. The alert sting fires regardless of
        // whether the private PCM wave was available (ext behaviour).
        for (int i = 0; i < g.nPending;) {
            const core::Pending& p = g.pending[i];
            if (now < p.due) {
                ++i;
                continue;
            }
            if (core::ReadyToWhizz(g, now, p.slot, p.shot)) {
                float gain = 0.0f, pan = 0.0f, gl = 0.0f, gr = 0.0f;
                int wave = -1;
                bool played = false;
                if constexpr (sink::kHave) {
                    gain = core::WhizzGain(p.lat, p.along);
                    float lp[3], ld[3];
                    if (Listener(lp, ld))
                        pan = core::PanOf(p.cp, lp, ld);
                    else
                        pan = (core::NextRand(g.rng, now) & 1u) ? 0.7f : -0.7f;
                    wave = core::PickWave(g.rng, now, sink::kCount);
                    core::PanLr(gain, pan, gl, gr);
                    engine::pcm::Voice v;
                    played = sink::Play(wave, gl, gr, v);
                }
                core::NoteWhizz(g, now, played);
                net::audio::alertsting::Combat("near miss out of the quiet");
                TO1_LOG("[NEARMISS] slot %d, passed %.2f m at %.1f m, pan %+.2f gain %.2f "
                        "wave %d -> %s\n", p.slot, static_cast<double>(p.lat),
                        static_cast<double>(p.along), static_cast<double>(pan),
                        static_cast<double>(gain), wave,
                        !sink::kHave ? "data not built (TO1_BULLET_BY_PCM off)"
                                     : played ? "playing" : "mixer full");
            } else {
                TO1_LOG("[NEARMISS] slot %d dropped - hit within 250 ms or whizz cooldown\n",
                        g.pending[i].slot);
            }
            g.pending[i] = g.pending[--g.nPending];
        }

        float w[8] = {};
        const bool mpOk = engine::object::LocalPlayerWorld(w)
                       && std::isfinite(w[0] + w[1] + w[2]);
        if (!mpOk) {
            core::AdvanceSeen(g, end);
            return;
        }
        const float ourRoot[3] = { w[0], w[1], w[2] };
        const float head[3] = { w[0], w[1] + core::kHeadUpM, w[2] };

        if (core::WarmReady(g, now)) {
            WarmLos(ourRoot, t60, frame);        // ext warms the nearby verdicts every 300 ms
            core::NoteWarmed(g, now);
        }

        // New replicated shot lines. Iterate the ring by sequence, tolerating wraps / gaps.
        core::RewindSeen(g, end);
        const ::net::match::Match& m = ::net::match::Get();
        const NetPlayer* local = NetPlayers::Local();
        const int localSlot = local ? local->netSlot : -1;
        for (uint32_t q = g.seen + 1; q - 1 != end; ++q) {
            if (!core::RingLive(g, q))
                continue;
            const core::ShotRecord& r = core::At(g, q);
            const uint32_t slot = r.slot;
            if (slot >= static_cast<uint32_t>(kMaxNetPlayers)
                || static_cast<int>(slot) == localSlot)
                continue;
            const NetPlayer& p = m.players[slot];
            if (!p.active || p.IsLocal() || NetPlayers::SlotIsFriendly(static_cast<int>(slot)))
                continue;
            if (!core::QueueReady(g, now, r, true))
                continue;
            const float o[3] = { r.o[0], r.o[1], r.o[2] };
            const float d[3] = { r.d[0], r.d[1], r.d[2] };
            const core::ClosestApproach a = core::ClosestPointOnSegment(o, d, r.len, head);
            if (!core::ShotIsNear(a, core::kMinAlong, core::kMaxLat))
                continue;
            // LOS is cached by the puppet handle (ext peer+0x60), not by the net slot.
            const uint64_t handle = static_cast<uint64_t>(p.npcSid);
            if (handle) {
                float tw[8] = {};
                if (engine::object::EntityWorld(p.npcSid, tw)
                    && std::isfinite(tw[0] + tw[1] + tw[2])) {
                    const float target[3] = { tw[0], tw[1], tw[2] };
                    if (!core::LosGate(g, handle, t60, frame, ourRoot, target, KickRay, RayT))
                        continue;               // a wall between shooter and us
                }
            }                   // ext skips the LOS veto when the shooter has no live locator
            core::Queue(g, r, a, now);
        }
        core::AdvanceSeen(g, end);
    }

    void Reset()
    {
        core::Reset(g);
    }

} // namespace net::audio::near_miss
