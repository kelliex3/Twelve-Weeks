// native MusicSlotSeen / MusicSlotCos producer -- faithful port of ext's sampler seams

#include "music-sight.hpp"

#include "music-tier.hpp"

#include "engine/flag.hpp"
#include "log.hpp"                 // TO1_LOG must precede engine/script.hpp (template helper)
#include "engine/object.hpp"
#include "engine/ptr.hpp"
#include "engine/raycast.hpp"
#include "engine/render-camera.hpp" // ext's primary camera source (final render camera)
#include "engine/script.hpp"
#include "net/game/player-status.hpp"
#include "net/match/match.hpp"
#include "net/match/net-player.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace net::audio::music_sight {

    namespace {

        // ---- ext constants ------------------------------------------------------------
        constexpr float kChestUpM     = 1.1f;   // ext MusicSlotSeen / MusicSlotCos: locator + 1.1 m
        constexpr float kConeCosMin   = 0.3f;   // ext close-quarters consumer

        // ext tag body visibility (t2r-ext.c BodyVisTick; the default when g_noBodyVis is off)
        constexpr int   kBvN          = 12;
        constexpr int   kBvCadence    = 6;      // ext: kick for a slot every 6th frame
        constexpr int   kBvOffFrames  = 2;      // ext: two hidden samples before turning hidden
        constexpr float kBvFarM       = 200.0f; // ext: skip a puppet beyond 200 m

        // ext BV_PT: joint-name fallbacks, root offsets (h, lat), weight and joint-Y nudge.
        struct BvPt {
            const char* joint[2];
            float h, lat, w, up;
        };
        static const BvPt kBvPt[kBvN] = {
            { { "head",       "headb"     }, 1.85f,  0.00f, 1, 0.12f },
            { { "head",       "headb"     }, 1.68f,  0.00f, 2, 0.00f },
            { { "spined",     "spinec"    }, 1.40f,  0.00f, 2, 0.00f },
            { { "spinea",     "spineb"    }, 1.20f,  0.00f, 1, 0.00f },
            { { "pelvis",     "spina"     }, 1.00f,  0.00f, 2, 0.00f },
            { { "l_shoulder", "l_clavicle"}, 1.50f, -0.22f, 1, 0.00f },
            { { "r_shoulder", "r_clavicle"}, 1.50f,  0.22f, 1, 0.00f },
            { { "l_hand",     "l_wrist"   }, 0.95f, -0.42f, 1, 0.00f },
            { { "r_hand",     "r_wrist"   }, 0.95f,  0.42f, 1, 0.00f },
            { { "l_knee",     "l_calf"    }, 0.50f, -0.12f, 1, 0.00f },
            { { "r_knee",     "r_calf"    }, 0.50f,  0.12f, 1, 0.00f },
            { { "l_ankle",    "l_foot"    }, 0.08f, -0.14f, 1, 0.00f }
        };

        // ---- sampler + tag state --------------------------------------------------------
        bool     g_seen[kMaxNetPlayers] = {};
        float    g_cos[kMaxNetPlayers]  = {};

        bool     g_tagHidden[kMaxNetPlayers] = {};   // ext g_tagHidden (= !g_bodyVis)
        bool     g_bodyVis[kMaxNetPlayers]   = {};
        uint64_t g_job[kMaxNetPlayers][kBvN]  = {};
        uint64_t g_jh[kMaxNetPlayers]         = {};  // handle the jobs were kicked for
        float    g_jd[kMaxNetPlayers][kBvN]   = {};  // camera -> point distance at kick time
        int      g_offCnt[kMaxNetPlayers]     = {};

        // ext BvJoint's jsid / jtry / jbad: which joint name answered, resolution attempts,
        // and consecutive read failures. -1 = no joint name resolved for this point yet.
        int      g_jpick[kBvN] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        int      g_jtry[kBvN]  = {};
        int      g_jbad[kBvN]  = {};

        uint32_t g_tick = 0;
        Vector3  g_camPos{};
        Vector3  g_camDir{};
        bool     g_camOk = false;

        // ext hud_geom / HudX0 / HudX1 / HudProjX (t2r-ext.c ~71740), ported exactly so the
        // on-screen box matches ext at any aspect ratio.
        int   g_hudW = 1280, g_hudH = 720;
        float g_hudAsp = 16.0f / 9.0f;
        float g_textXoff = 0.0f;

        void ReadHudGeom()
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            const int32_t* pw = reinterpret_cast<const int32_t*>(base + 0x3614600);
            const int32_t* ph = reinterpret_cast<const int32_t*>(base + 0x3614604);
            int w = 0, h = 0;
            if (engine::ptr::Readable(reinterpret_cast<uintptr_t>(pw), 8)
                && *pw >= 320 && *pw <= 16384 && *ph >= 200 && *ph <= 8192) {
                w = *pw;
                h = *ph;
            }
            else {
                HWND hw = GetForegroundWindow();
                DWORD pid = 0;
                RECT rc;
                if (hw) {
                    GetWindowThreadProcessId(hw, &pid);
                    if (pid == GetCurrentProcessId() && GetClientRect(hw, &rc)
                        && rc.right > 0 && rc.bottom > 0) {
                        w = static_cast<int>(rc.right);
                        h = static_cast<int>(rc.bottom);
                    }
                }
            }
            if (w > 0 && h > 0 && (w != g_hudW || h != g_hudH)) {
                float a = static_cast<float>(w) / static_cast<float>(h);
                if (a > 16.0f / 9.0f - 0.01f && a < 16.0f / 9.0f + 0.01f)
                    a = 16.0f / 9.0f;
                if (a < 1.0f)
                    a = 1.0f;
                g_hudW = w;
                g_hudH = h;
                g_hudAsp = a;
                g_textXoff = a > 16.0f / 9.0f ? (720.0f * a - 1280.0f) * 0.5f : 0.0f;
            }
        }

        // ext TagScreen: the engine attachable projection it prefers (FUN_141393b00 with the
        // camera from FUN_1413904d0(0), the same one the mark triangle uses) when its signature
        // matches and t2r-no-tag-attach-proj.txt is absent, else the world-space-to-screen-space
        // native. Both map into the fixed 1280x720 space; then HudProjX + the MusicSlotSeen box.
        int g_tagProjMode = -1;                    // -1 unresolved, 1 attachable, 0 native

        bool TagScreen(const Vector3& p, float& x, float& y)
        {
            const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if (g_tagProjMode < 0) {
                // ext's exact byte signatures for the two funcs and their call site.
                static const uint8_t s_cam[12] = {
                    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0xF9, 0x83, 0xF9, 0x02, 0x72
                };
                static const uint8_t s_prj[9] = {
                    0x48, 0x83, 0xEC, 0x48, 0x0F, 0xB6, 0x44, 0x24, 0x78
                };
                static const uint8_t s_site[4] = { 0x8B, 0xCF, 0xE8, 0x00 };
                const bool sig =
                    engine::ptr::Readable(exe + 0x13904d0, 12)
                    && engine::ptr::Readable(exe + 0x1393b00, 9)
                    && engine::ptr::Readable(exe + 0x13af1a4, 0x28)
                    && std::memcmp(reinterpret_cast<const void*>(exe + 0x13904d0), s_cam, 12) == 0
                    && std::memcmp(reinterpret_cast<const void*>(exe + 0x1393b00), s_prj, 9) == 0
                    && std::memcmp(reinterpret_cast<const void*>(exe + 0x13af1a4), s_site, 3) == 0
                    && *reinterpret_cast<const int32_t*>(exe + 0x13af1a7)
                       == static_cast<int32_t>(0x13904d0 - 0x13af1ab)
                    && *reinterpret_cast<const uint8_t*>(exe + 0x13af1b1) == 0x28
                    && *reinterpret_cast<const uint8_t*>(exe + 0x13af1b2) == 0x01
                    && *reinterpret_cast<const uint8_t*>(exe + 0x13af1bb) == 0x20
                    && *reinterpret_cast<const uint8_t*>(exe + 0x13af1bc) == 0x01
                    && *reinterpret_cast<const int32_t*>(exe + 0x13af1c6)
                       == static_cast<int32_t>(0x1393b00 - 0x13af1ca);
                g_tagProjMode = (sig && engine::flag::Absent("t2r-no-tag-attach-proj.txt"))
                    ? 1 : 0;
            }

            float sx = 0.0f, sy = 0.0f;
            if (g_tagProjMode) {
                using FnCam = void* (__fastcall*)(uint32_t);
                using FnProj = uint64_t (__fastcall*)(void*, float*, float*,
                                                      const float*, uint8_t, uint8_t);
                void* cam = reinterpret_cast<FnCam>(exe + 0x13904d0)(0);
                if (!cam)
                    return false;
                alignas(16) const float q[4] = { p.x, p.y, p.z, 1.0f };  // ext: aligned(16)
                const uint64_t r = reinterpret_cast<FnProj>(exe + 0x1393b00)(
                    cam, &sx, &sy, q, 1, 1);
                if (!(r & 0xff))                     // ext: behind the camera
                    return false;
            }
            else {
                const Vector2* r = DC::Call<"world-space-to-screen-space", const Vector2>(
                    0, MEMORY::ALLOC_VECTOR(p));
                if (!r)
                    return false;
                sx = r->x;
                sy = r->y;
            }
            ReadHudGeom();
            const float k = g_hudAsp > 16.0f / 9.0f ? g_hudAsp / (16.0f / 9.0f) : 1.0f;
            x = 640.0f + (sx - 640.0f) * k;          // ext HudProjX
            y = sy;
            return std::isfinite(x) && std::isfinite(y);
        }

        bool MusicSlotOnScreen(const Vector3& chest)
        {
            float x = 0.0f, y = 0.0f;
            return TagScreen(chest, x, y) && TagOnScreen(x, y, g_textXoff);
        }

        // ext BvJoint: resolve a joint name once per point, then read its world position.
        bool BvJoint(StringId64 h, int k, float out[3])
        {
            if (g_jtry[k] >= 3 && g_jpick[k] < 0)
                return false;
            if (g_jpick[k] < 0) {
                for (int i = 0; i < 2; ++i) {
                    float t[3];
                    if (engine::object::JointLocator(h, kBvPt[k].joint[i], t, nullptr)) {
                        g_jpick[k] = i;
                        break;
                    }
                }
                if (g_jpick[k] < 0) {
                    ++g_jtry[k];
                    return false;
                }
            }
            float t[3];
            if (engine::object::JointLocator(h, kBvPt[k].joint[g_jpick[k]], t, nullptr)
                && std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2])) {
                g_jbad[k] = 0;
                out[0] = t[0];
                out[1] = t[1];
                out[2] = t[2];
                return true;
            }
            if (++g_jbad[k] > 30) {     // ext: give up on a joint that keeps reading bad
                g_jpick[k] = -1;
                g_jtry[k] = 3;
            }
            return false;
        }

        bool Enabled() noexcept
        {
            return kEnabled && engine::flag::Absent("t2r-no-music-sight.txt");
        }

        // ext CamPosDir(): render camera first, then the real reticle-locator + dir pair, then
        // the ext's 0.5 < n < 1.5 normalization gate. Both paths share one g_camPos/g_camDir.
        bool ResolveCamera()
        {
            float cp[3], cd[3];
            bool ok = engine::render_camera::Get(cp, cd);
            if (!ok) {
                Vector3 p;
                if (engine::raycast::CameraPosition(p)) {
                    const Vector3* d = DC::Call<"camera-get-dir-through-reticle", const Vector3>();
                    if (d && std::isfinite(d->x) && std::isfinite(d->y) && std::isfinite(d->z)) {
                        cp[0] = p.x; cp[1] = p.y; cp[2] = p.z;
                        cd[0] = d->x; cd[1] = d->y; cd[2] = d->z;
                        ok = true;
                    }
                }
            }
            if (!ok)
                return false;
            const float n = std::sqrt(cd[0] * cd[0] + cd[1] * cd[1] + cd[2] * cd[2]);
            if (!(n > 0.5f && n < 1.5f))
                return false;
            g_camPos = { cp[0], cp[1], cp[2] };
            g_camDir = { cd[0] / n, cd[1] / n, cd[2] / n };
            return true;
        }

        void ClearJobs(int slot)
        {
            for (int k = 0; k < kBvN; ++k) {
                if (g_job[slot][k]) {
                    engine::raycast::RayT(g_job[slot][k]);   // ext: drain outstanding answers
                    g_job[slot][k] = 0;
                }
            }
        }

        void ClearSlot(int slot)
        {
            g_seen[slot] = false;
            g_cos[slot] = -2.0f;
            g_tagHidden[slot] = false;
            g_bodyVis[slot] = false;
            g_offCnt[slot] = 0;
            g_jh[slot] = 0;
            ClearJobs(slot);
        }

        void ClearAll()
        {
            for (int slot = 0; slot < kMaxNetPlayers; ++slot)
                ClearSlot(slot);
        }

        // ext BodyVisTick: maintain g_tagHidden[slot] from 12 weighted static-world rays.
        // Absent / invalid puppets get an immediate all-clear (ext's !valid branch).
        void TagBodyVisTick()
        {
            if (!net::player_status::MatchLive()) {     // ext: MatchSubstate() != 6 clears the tags
                ClearAll();
                return;
            }

            const NetPlayer* local = NetPlayers::Local();
            const int me = local ? local->netSlot : -1;

            for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
                const NetPlayer& p = ::net::match::Get().players[slot];
                const bool valid = slot != me && !p.isLocal && p.active && p.hasState
                    && p.spawned && !p.dead && p.npcSid
                    && !NetPlayers::SlotIsFriendly(slot);

                bool have = false;
                for (int k = 0; k < kBvN; ++k)
                    if (g_job[slot][k]) { have = true; break; }

                if (!valid) {
                    if (have)
                        ClearJobs(slot);
                    g_bodyVis[slot] = false;
                    g_tagHidden[slot] = false;
                    g_offCnt[slot] = 0;
                    g_jh[slot] = 0;
                    continue;
                }

                if (have) {                              // read last kick's answers
                    float cw = 0.0f, tw = 0.0f;
                    int nv = 0;
                    for (int k = 0; k < kBvN; ++k) {
                        if (!g_job[slot][k])
                            continue;
                        const float t = engine::raycast::RayT(g_job[slot][k]);
                        g_job[slot][k] = 0;
                        if (g_jh[slot] != p.npcSid)
                            continue;
                        const float w = kBvPt[k].w;
                        ++nv;
                        tw += w;
                        if (!BodyVisClear(t, g_jd[slot][k]))
                            cw += w;
                    }
                    if (nv && g_jh[slot] == p.npcSid) {
                        if (BodyVisFraction(cw, tw)) {
                            g_bodyVis[slot] = true;
                            g_offCnt[slot] = 0;
                        }
                        else if (++g_offCnt[slot] >= kBvOffFrames) {
                            g_bodyVis[slot] = false;
                        }
                        g_tagHidden[slot] = !g_bodyVis[slot];
                    }
                    continue;
                }

                if ((g_tick + static_cast<uint32_t>(slot)) % kBvCadence || !g_camOk)
                    continue;                            // ext: every 6th frame per slot

                float w[8];
                if (!engine::object::EntityWorld(p.npcSid, w))
                    continue;
                const float dx = w[0] - g_camPos.x, dz = w[2] - g_camPos.z;
                const float dl = std::sqrt(dx * dx + dz * dz);
                if (dl > kBvFarM)
                    continue;
                float rx = 1.0f, rz = 0.0f;
                if (dl > 0.1f) { rx = dz / dl; rz = -dx / dl; }

                // ext lying = g_peerDowned != 0 || peer[0x86] != 0; crouch = peer[0x85].
                // Both stance bytes ARE on our wire: PlayerNetState.isProne / .isCrouched are
                // sampled from the sender's player-is-prone? / player-is-crouched? in
                // net-replication.cpp. Down/dying comes from PuppetDowned. Exact ext terms.
                const PlayerNetState& s = p.State();
                const bool lying = net::player_status::PuppetDowned(slot) || s.isProne != 0;
                const bool crouch = s.isCrouched != 0;

                float fx = rx, fz = rz;
                if (lying) {                             // ext: use the body's own forward axis
                    const float qx = w[4], qy = w[5], qz = w[6], qw = w[7];
                    const float ax = 2.0f * (qx * qz + qw * qy);
                    const float az = 1.0f - 2.0f * (qx * qx + qy * qy);
                    const float al = std::sqrt(ax * ax + az * az);
                    if (al > 0.1f && al < 2.0f) { fx = ax / al; fz = az / al; }
                }

                g_jh[slot] = p.npcSid;
                for (int k = 0; k < kBvN; ++k) {
                    float pt[3];
                    if (!BvJoint(p.npcSid, k, pt)) {
                        const float hh = kBvPt[k].h, lat = kBvPt[k].lat;
                        if (lying) {
                            pt[0] = w[0] + fx * hh * 0.85f + rx * lat * 0.6f;
                            pt[1] = w[1] + 0.25f;
                            pt[2] = w[2] + fz * hh * 0.85f + rz * lat * 0.6f;
                        }
                        else {
                            const float sc = crouch ? 0.7f : 1.0f;   // ext: crouch body scale
                            pt[0] = w[0] + rx * lat;
                            pt[1] = w[1] + hh * sc;
                            pt[2] = w[2] + rz * lat;
                        }
                    }
                    else {
                        pt[1] += kBvPt[k].up;
                    }
                    const float ex = pt[0] - g_camPos.x, ey = pt[1] - g_camPos.y,
                                ez = pt[2] - g_camPos.z;
                    g_jd[slot][k] = std::sqrt(ex * ex + ey * ey + ez * ez);
                    g_job[slot][k] = engine::raycast::KickWorldRay(
                        g_camPos, Vector3{ pt[0], pt[1], pt[2] },
                        engine::raycast::kMaskStaticWorld, SID("player"));
                }
            }
        }

        bool SightFn(int slot, void*)
        {
            return slot >= 0 && slot < kMaxNetPlayers && g_seen[slot];
        }

        bool ConeFn(int slot, void*)
        {
            return slot >= 0 && slot < kMaxNetPlayers && g_cos[slot] > kConeCosMin;
        }

    }

    void Install()
    {
        if (!Enabled())
            return;
        music_tier::SetSightSampler(&SightFn, nullptr);
        music_tier::SetConeSampler(&ConeFn, nullptr);
    }

    void Tick()
    {
        ++g_tick;
        if (!Enabled()) {
            ClearAll();
            return;
        }

        g_camOk = ResolveCamera();
        if (!g_camOk) {
            ClearAll();                                  // ext: no camera -> no visibility rays
            return;
        }

        TagBodyVisTick();                                // ext maintains g_tagHidden each frame

        const NetPlayer* local = NetPlayers::Local();
        for (int slot = 0; slot < kMaxNetPlayers; ++slot) {
            const NetPlayer& p = ::net::match::Get().players[slot];
            if (slot == (local ? local->netSlot : -1) || !p.active || !p.hasState
                || NetPlayers::SlotIsFriendly(slot)) {
                g_seen[slot] = false;
                g_cos[slot] = -2.0f;
                continue;
            }

            // ext MusicSlotSeen / MusicSlotCos: exact puppet locator chest (exe+0x133DB30 via
            // EntityFromHandle), not the replicated net position. No locator -> no sample.
            float w[8];
            if (!engine::object::EntityWorld(p.npcSid, w)
                || !std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) {
                g_seen[slot] = false;
                g_cos[slot] = -2.0f;
                continue;
            }
            const Vector3 chest{ w[0], w[1] + kChestUpM, w[2] };

            const Vector3 v = chest - g_camPos;
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            const float depth = v.x * g_camDir.x + v.y * g_camDir.y + v.z * g_camDir.z;

            // ext MusicSlotCos: len < 0.1 m -> 1.0, else depth / len.
            g_cos[slot] = len > 0.1f ? depth / len : 1.0f;

            // ext MusicSlotSeen: !g_tagHidden + depth/cone gate + TagScreen on-screen box.
            g_seen[slot] = !g_tagHidden[slot]
                && SeenFrustumOk(depth, len)
                && MusicSlotOnScreen(chest);
        }
    }

    void Reset()
    {
        ClearAll();
        g_camOk = false;
        g_tick = 0;
    }

} // namespace net::audio::music_sight
