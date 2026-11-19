// ported from tools/ext/t2r-ext.c (feature_backpack_prop: BpMe / BpCtrlOf / BpState /
// BpWornSnapshot / BpWornOff / BpWornOn / BpEnterHook / BpLeaveHook / BpLeaveRunPending /
// InstallBpEngineHooks)

#include "backpack-prop.hpp"

#include "armor-rig.hpp"
#include "exe_side.hpp"
#include "frame.hpp"
#include "local-player.hpp"
#include "object.hpp"
#include "ptr.hpp"
#include "script.hpp"

#include "log.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::backpack_prop {

    namespace {

        // The engine's PlayerBackpackAnimCtrl (exe 0x140aff010 = EnterBackpack, 0x140aff400 =
        // LeaveBackpack), reached through Player+0x2f58. Its vtable at exe+0x2c99d98 has the
        // two swap moments at slots +0xb8 / +0xc0.
        constexpr uintptr_t kCtrlVtRva = 0x2c99d98;
        constexpr uintptr_t kVtEnter    = 0xb8;
        constexpr uintptr_t kVtLeave    = 0xc0;
        constexpr uintptr_t kRvaEnter   = 0xaff010;
        constexpr uintptr_t kRvaLeave   = 0xaff400;

        // Every worn bag part of every pool look (originals, player-style rigged copies,
        // armour twins' pulled copies). The engine's enter / leave hide / show only the look's
        // ORIGINAL bag sid; the copies are ours to switch on the same call.
        const char* const kParts[] = {
            "scar-male-backpack", "scar-t2r-bp-rigged", "scar-t2r-bp-armour",
            "militia-male-backpack-05", "militia-male-backpack-08",
            "militia-male-backpack-11", "militia-male-backpack-12",
            "militia-male-backpack-13", "militia-male-backpack-14",
            "militia-t2r-bp-rigged-11", "militia-t2r-bp-rigged-12",
            "militia-t2r-bp-armour-11", "militia-t2r-bp-armour-12",
            "militia-arm1-backpack-05", "militia-arm1-backpack-08",
            "militia-arm1-backpack-13", "militia-arm1-backpack-14",
            "scar-female-backpack", "scar-farm01-backpack",
            "militia-female-backpack-12", "militia-female-backpack-13",
            "militia-farm01-backpack-12", "militia-farm01-backpack-13",
        };
        constexpr int kNparts = static_cast<int>(sizeof kParts / sizeof *kParts);

        uint64_t g_partSid[kNparts];
        uint8_t  g_present[kNparts];
        uint8_t  g_wasVis[kNparts];
        volatile int g_snapOk = 0;

        volatile int      g_hid = 0;          // our worn bag parts are held off (prop is out)
        volatile uint64_t g_hidGen = 0;       // our life at the hide: a new own life voids it
        volatile int      g_wornHidden = 0;   // the armour rig keeps the bag pair off
        volatile int      g_inMatch = 0;      // the level is live (ext g_bp_inmatch)
        volatile int      g_inEnter = 0;      // inside our EnterBackpack hook

        volatile LONG     g_lock = 0;

        volatile uint32_t g_nEnter = 0, g_nLeave = 0;
        volatile double   g_tEnter = 0.0, g_tLeave = 0.0;
        volatile int      g_srcEnter = 0, g_srcLeave = 0;

        uint64_t g_spawnedFrame = 0;          // our life id (ext g_spawnedFrame)

        volatile int      g_hooks = 0;        // bit 0: enter / leave vtable hooks live

        // held leave
        volatile int      g_leavePend = 0;
        uint64_t          g_leaveArg = 0;
        double            g_leaveDue = 0.0;
        uint64_t          g_leaveGen = 0;

        typedef void (*fn_enter)(void*, uint64_t, uint64_t);
        typedef void (*fn_leave)(void*, uint64_t);
        fn_enter g_enterOrig = nullptr;
        fn_leave g_leaveOrig = nullptr;

        uintptr_t ExeBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        void Lock()
        {
            while (InterlockedCompareExchange(&g_lock, 1, 0))
                YieldProcessor();
        }

        void Unlock()
        {
            InterlockedExchange(&g_lock, 0);
        }

        int CtrlIsOwn(uint8_t* c, uint8_t** meOut)
        {
            uint8_t* me = Me();
            *meOut = me;
            return g_inMatch && c && me && CtrlOf(me) == c;
        }

        void EnterHook(void* ctrl, uint64_t a, uint64_t b)
        {
            uint8_t* c = static_cast<uint8_t*>(ctrl);
            uint8_t* me = nullptr;
            const int was = c[0x190];
            const int own = !was && CtrlIsOwn(c, &me);
            if (own) {
                Lock();
                if (!g_hid)
                    WornSnapshot(me);
                Unlock();
            }
            g_inEnter = own;
            g_enterOrig(ctrl, a, b);
            g_inEnter = 0;
            if (own && c[0x190]) {
                Lock();
                WornOff(me, 1);
                Unlock();
            }
        }

        void LeaveHook(void* ctrl, uint64_t a)
        {
            uint8_t* c = static_cast<uint8_t*>(ctrl);
            uint8_t* me = nullptr;
            const int was = c[0x190];
            const int own = was && CtrlIsOwn(c, &me);
            const int hold = own ? LeaveHoldMs() : 0;
            if (hold > 0) {
                if (!g_leavePend) {
                    g_leaveArg = a;
                    g_leaveGen = g_spawnedFrame;
                    g_leaveDue = engine::frame::NowMs() + hold;
                    g_leavePend = 1;
                    TO1_LOG("bp: own leave event held %d ms (prop bag finishes its outro, weapons ride it)\n", hold);
                }
                return;     // (a repeat event while held is swallowed)
            }
            g_leaveOrig(ctrl, a);
            if (own && !c[0x190]) {
                Lock();
                WornOn(me, 1);
                Unlock();
            }
        }

        // ==================================================================================
        //  Weapon reparent / attach (ext BpReparentHook / BpAttachHook / InstallBpEngineHooks)
        // ==================================================================================
        //
        // ReparentWeapons (exe+0xafc740, toBag 1 = the bag coming off) moves the stowed weapons
        // onto the TAB prop's points. The prop points carry the WORN stow itself (BP_PROP_C, the
        // model-space target the ext's backpack_slot2.py solved), so each gun is exactly where the
        // hand-off back will put it. The engine's own instant attach (exe+0x1cd2140) is turned into
        // the blended one (exe+0x1cd2210, 0.2 s) on our own bag-off, so the gun leaves the sling on
        // the very call the bag swaps. The ext hooked the 12 CALL SITES (4 ReparentWeapons + the 8
        // instant attaches inside it); the mod hooks the two FUNCTION BODIES instead -- every
        // caller is caught, and the attach hook is reached through the original reparent call. The
        // `ours` gate keeps every other call a pass-through. t2r-bp-weapons-body.txt = opt-in old
        // behaviour (the weapons stay on the body).

        constexpr uintptr_t kRvaReparent    = 0xafc740;
        constexpr uintptr_t kRvaAttach      = 0x1cd2140;
        constexpr uintptr_t kRvaAttachBlend = 0x1cd2210;
        constexpr float     kBlend          = 0.2f;

        // [0] melee (one_hand_attach), [1] targRifle, [2] targRifle2: the worn stow's spot on the
        // prop point (offset' = L^-1 * C_k * the engine's offset; L identity on the shipped props).
        const float kPropC[3][8] = {
            { -0.00218f, +0.01072f, -0.00196f, 1.0f, +0.00308f, +0.00065f, +0.00020f, +1.00000f },
            { +0.00668f, +0.01261f, +0.00756f, 1.0f, +0.02310f, +0.00635f, -0.02769f, +0.99933f },
            { +0.01664f, +0.12143f, +0.07954f, 1.0f, +0.31141f, -0.00968f, -0.03306f, +0.94965f },
        };

        void Qrotv(const float* q, const float* v, float* o)
        {                               // o = q * v * q^-1 (q = x y z w)
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            const float tx = 2 * (y * v[2] - z * v[1]);
            const float ty = 2 * (z * v[0] - x * v[2]);
            const float tz = 2 * (x * v[1] - y * v[0]);
            o[0] = v[0] + w * tx + (y * tz - z * ty);
            o[1] = v[1] + w * ty + (z * tx - x * tz);
            o[2] = v[2] + w * tz + (x * ty - y * tx);
        }

        void Qmul(const float* a, const float* b, float* o)
        {
            o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
            o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
            o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
            o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
        }

        void XfMul(const float* a, const float* b, float* o)
        {                               // {pos4, quat4}: o = a * b
            float t[3], q[4];
            Qrotv(a + 4, b, t);
            Qmul(a + 4, b + 4, q);
            o[0] = a[0] + t[0];
            o[1] = a[1] + t[1];
            o[2] = a[2] + t[2];
            o[3] = 1.0f;
            std::memcpy(o + 4, q, 16);
        }

        int IsMeleeAp(uint64_t ap)
        {
            return !(ap == SID("targRifle") || ap == SID("targRifle2")
                || ap == SID("targBow") || ap == SID("targFlashlight")
                || ap == SID("targSpinePropAnim"));
        }

        const char* ApName(uint64_t ap)
        {
            static const char* const N[] = {
                "targRifle", "targRifle2", "targBow", "targMelee1h",
                "targOneHandAttach", "targFlashlight", "targSpinePropAnim",
                "leftHand", "rightHand", "targLHand", "targRHand",
                "targLongGunSlotOne", "targLongGunSlotTwo", "targSpineProp",
                "targSpinePropAnimTwo", "thigh_pistol_holster", "gun_holster",
                "machete_holster", "t2r_fc_m", "t2r_fc_g1", "t2r_fc_g2",
                "t2r_fcf_m", "t2r_fcf_g1", "t2r_fcf_g2", "t2r_pt_f",
                "targPistolSlotTwo", "t2r_pistol_second", "t2r_pb_m",
                "t2r_pb_f", "t2r_pt_m", "switch_blade",
            };
            for (unsigned i = 0; i < sizeof N / sizeof *N; i++)
                if (SID(N[i]) == ap)
                    return N[i];
            // (ext fell back to PistGunName / PistSecondNameX -- the unported sidearm feature)
            return "?";
        }

        // {ptr, u32 id} handle pair -> process (ext TacHandleProc).
        uint8_t* TacHandleProc(uint8_t* hp)
        {
            if (!hp || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(hp), 12))
                return nullptr;
            const uint64_t p = *reinterpret_cast<uint64_t*>(hp);
            const uint32_t id = *reinterpret_cast<uint32_t*>(hp + 8);
            if (!p || !id || !engine::ptr::Readable(static_cast<uintptr_t>(p), 16))
                return nullptr;
            uint8_t* proc = *reinterpret_cast<uint8_t**>(p + 8);
            if (!proc || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(proc) + 0x94, 4)
                || *reinterpret_cast<uint32_t*>(proc + 0x94) != id)
                return nullptr;
            return proc;
        }

        // The prop's attach point def (attach system [proc+0x1f0]: +4 count, +8 entries of 0x40 =
        // {local pos4 quat4, +0x20 sid, ...}).
        const uint8_t* ApEntry(void* owner, uint64_t ap)
        {
            uint8_t* prop = owner ? TacHandleProc(static_cast<uint8_t*>(owner)) : nullptr;
            uint8_t* sys = (prop
                && engine::ptr::Readable(reinterpret_cast<uintptr_t>(prop) + 0x1f0, 8))
                ? *reinterpret_cast<uint8_t**>(prop + 0x1f0) : nullptr;
            if (!sys || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(sys), 16))
                return nullptr;
            const uint32_t n = *reinterpret_cast<uint32_t*>(sys + 4);
            uint8_t* e = *reinterpret_cast<uint8_t**>(sys + 8);
            if (!e || n > 512
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(e),
                    static_cast<size_t>(n) * 0x40))
                return nullptr;
            for (uint32_t i = 0; i < n; i++, e += 0x40)
                if (*reinterpret_cast<uint64_t*>(e + 0x20) == ap)
                    return e;
            return nullptr;
        }

        // Once per attach point: where the gun sits on the bag (log only).
        void LogApDef(void* owner, uint64_t ap)
        {
            static uint64_t seen[8];
            for (int i = 0; i < 8; i++) {
                if (seen[i] == ap)
                    return;
                if (!seen[i]) {
                    seen[i] = ap;
                    break;
                }
            }
            uint8_t* prop = owner ? TacHandleProc(static_cast<uint8_t*>(owner)) : nullptr;
            uint8_t* sys = (prop
                && engine::ptr::Readable(reinterpret_cast<uintptr_t>(prop) + 0x1f0, 8))
                ? *reinterpret_cast<uint8_t**>(prop + 0x1f0) : nullptr;
            if (!sys || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(sys), 16))
                return;
            const uint32_t n = *reinterpret_cast<uint32_t*>(sys + 4);
            uint8_t* e = *reinterpret_cast<uint8_t**>(sys + 8);
            if (!e || n > 512
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(e),
                    static_cast<size_t>(n) * 0x40))
                return;
            for (uint32_t i = 0; i < n; i++, e += 0x40) {
                if (*reinterpret_cast<uint64_t*>(e + 0x20) != ap)
                    continue;
                const float* l = reinterpret_cast<const float*>(e);
                TO1_LOG("bp: prop attach point %llx (#%u of %u): local pos %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f, +0x28 %08x %08x %08x %08x\n",
                    static_cast<unsigned long long>(ap), i, n, l[0], l[1], l[2], l[4], l[5], l[6], l[7],
                    *reinterpret_cast<const uint32_t*>(e + 0x28),
                    *reinterpret_cast<const uint32_t*>(e + 0x2c),
                    *reinterpret_cast<const uint32_t*>(e + 0x30),
                    *reinterpret_cast<const uint32_t*>(e + 0x34));
                return;
            }
            TO1_LOG("bp: prop attach point %llx is not among the prop's %u attach points - the engine's attach fails there\n",
                static_cast<unsigned long long>(ap), n);
        }

        typedef void     (*fn_reparent)(void*, void*, uint64_t, void*, uint64_t, uint64_t);
        typedef uint64_t (*fn_attach)(void*, const char*, int, const char*, void*, uint64_t, void*);
        typedef uint64_t (*fn_attach_bl)(void*, const char*, int, const char*, void*, uint64_t, void*, float, uint64_t);

        volatile int  g_perLook = 1;        // ext g_bpPerLook (default on)
        volatile int  g_wpnBody = 0;        // t2r-bp-weapons-body.txt: the weapons stay on the body
        volatile int  g_attachBlend = 0;    // inside our own toBag 1 reparent: instant attaches -> blended
        volatile int  g_inAttach = 0;       // re-entry guard: the blend path may reach the instant attach
        volatile int  g_wpnSkip = 0;        // body mode: this bag-off kept the weapons on the body
        volatile LONG g_wlog = 0;
        volatile LONG g_wblend = 0;
        volatile int  g_wblendLast = 0;

        SafetyHookInline g_reparentHook;
        SafetyHookInline g_attachHook;

        uint64_t __fastcall AttachHook(void* w, const char* file, int line, const char* func,
                                       void* owner, uint64_t ap, void* offset)
        {
            if (!g_attachBlend) {
                const uint64_t r = g_attachHook.fastcall<uint64_t>(w, file, line, func, owner, ap, offset);
                if (InterlockedIncrement(&g_wlog) <= 300)
                    TO1_LOG("bp: wpn attach (pass-through, not in our bag-off) weapon %p -> ap %llx %s line %d -> %llu\n",
                        w, static_cast<unsigned long long>(ap), ApName(ap), line,
                        static_cast<unsigned long long>(r & 0xff));
                return r;
            }
            if (g_inAttach)     // re-entry: the blend path reached the instant attach
                return g_attachHook.fastcall<uint64_t>(w, file, line, func, owner, ap, offset);
            float off[8];
            void* useOffset = offset;
            const int k = ap == SID("targRifle") ? 1 : ap == SID("targRifle2") ? 2
                : (ap != SID("targBow") && IsMeleeAp(ap)) ? 0 : -1;
            if (k >= 0 && offset
                && engine::ptr::Readable(reinterpret_cast<uintptr_t>(offset), 32)) {
                XfMul(kPropC[k], static_cast<const float*>(offset), off);
                const uint8_t* e = ApEntry(owner, ap);
                if (e) {    // the point's own locator on its joint: undo it (identity on the shipped props)
                    const float* l = reinterpret_cast<const float*>(e);
                    const float nq = l[4] * l[4] + l[5] * l[5] + l[6] * l[6] + l[7] * l[7];
                    if (nq > 0.98f && nq < 1.02f
                        && (std::fabs(l[0]) + std::fabs(l[1]) + std::fabs(l[2]) > 1e-5f
                            || std::fabs(l[7]) < 0.99999f)) {
                        float li[8], ip[3], t[8];
                        float qc[4] = { -l[4], -l[5], -l[6], l[7] };
                        Qrotv(qc, l, ip);
                        li[0] = -ip[0];
                        li[1] = -ip[1];
                        li[2] = -ip[2];
                        li[3] = 1.0f;
                        std::memcpy(li + 4, qc, 16);
                        XfMul(li, off, t);
                        std::memcpy(off, t, 32);
                    }
                }
                static volatile LONG nl;
                if (InterlockedIncrement(&nl) <= 30)
                    TO1_LOG("bp: prop %s ap %s: weapon on the worn stow's spot (offset %.3f %.3f %.3f -> %.3f %.3f %.3f)\n",
                        k ? (k == 1 ? "long gun one" : "long gun two") : "melee", ApName(ap),
                        static_cast<const float*>(offset)[0], static_cast<const float*>(offset)[1],
                        static_cast<const float*>(offset)[2], off[0], off[1], off[2]);
                useOffset = off;
            }
            auto blend = reinterpret_cast<fn_attach_bl>(ExeBase() + kRvaAttachBlend);
            g_inAttach = 1;
            const uint64_t r = blend(w, file, line, func, owner, ap, useOffset, kBlend, 0);
            g_inAttach = 0;
            TO1_LOG("bp: wpn attach (ours: blended %.2f s) weapon %p -> prop ap %llx %s line %d -> %llu\n",
                kBlend, w, static_cast<unsigned long long>(ap), ApName(ap), line,
                static_cast<unsigned long long>(r & 0xff));
            g_wblendLast = g_wblendLast + 1;
            g_wblend = g_wblend + 1;
            LogApDef(owner, ap);
            return r;
        }

        void __fastcall ReparentHook(void* ctrl, void* prop, uint64_t toBag, void* wpn,
                                     uint64_t f5, uint64_t f6)
        {
            uint8_t* me = nullptr;
            const int own = CtrlIsOwn(static_cast<uint8_t*>(ctrl), &me);
            // ours = OUR look has a per-look prop (read from our player; the ext read the last
            // spawn's player, which could be a puppet's)
            int ours = own && g_perLook && LookProp(PlayerLook(me)) >= 0;
            // ours only while the bag is really coming off: inside our EnterBackpack call (the
            // single-weapon path: the engine's IsInBackpack gate + [c+0x190]); anything else is
            // the engine's own and passes through untouched
            if (ours && static_cast<uint8_t>(toBag) && !g_inEnter
                && !static_cast<uint8_t*>(ctrl)[0x190])
                ours = 0;
            if (InterlockedIncrement(&g_wlog) <= 300
                || (static_cast<uint8_t>(toBag) && ours))
                TO1_LOG("bp: wpn reparent from exe+%llx toBag %d weapon %p flags %d %d own %d ours %d in-enter %d bag-off %d%s\n",
                    static_cast<unsigned long long>(
                        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - ExeBase()),
                    static_cast<int>(static_cast<uint8_t>(toBag)), wpn,
                    static_cast<int>(static_cast<uint8_t>(f5)),
                    static_cast<int>(static_cast<uint8_t>(f6)), own, ours, g_inEnter,
                    static_cast<int>(static_cast<uint8_t*>(ctrl)[0x190]),
                    static_cast<uint8_t>(toBag) && ours && !g_wpnBody
                        ? " -> blended attaches onto the bag" : " -> engine as is");
            if (g_wpnBody) {
                if (static_cast<uint8_t>(toBag)) {
                    if (ours) {
                        g_wpnSkip = 1;
                        return;
                    }
                    g_wpnSkip = 0;
                } else if (g_wpnSkip && own) {
                    g_wpnSkip = 0;
                    return;
                }
                g_reparentHook.fastcall<void>(ctrl, prop, toBag, wpn, f5, f6);
                return;
            }
            if (static_cast<uint8_t>(toBag) && ours) {
                if (!wpn)
                    g_wblendLast = 0;
                g_attachBlend = 1;
                g_reparentHook.fastcall<void>(ctrl, prop, toBag, wpn, f5, f6);
                g_attachBlend = 0;
                return;
            }
            g_reparentHook.fastcall<void>(ctrl, prop, toBag, wpn, f5, f6);
        }

        // ==================================================================================
        //  The per-look TAB prop table (ext BP_PROP_LOOKS / BpTables / BpLookProp)
        // ==================================================================================

        constexpr int kPropN = 16;
        const char* const kPropLooks[kPropN][2] = {   // look -> prop look (dc_data_patches.BACKPACK_PROPS)
            { "scar-male-13", "scar-t2r-tab-bprop" },
            { "scar-male-01", "scar-t2r-tab-bprop" },
            { "scar-male-12", "scar-t2r-tab-bprop" },
            { "scar-male-06", "scar-t2r-tab-bprop" },
            { "scar-male-09", "scar-t2r-tab-bprop" },
            { "scar-male-02", "scar-t2r-tab-bprop" },
            { "militia-male-11-backpack", "militia-t2r-tab-bprop-11" },
            { "militia-male-08-backpack", "militia-t2r-tab-bprop-08" },
            { "militia-male-12-backpack", "militia-t2r-tab-bprop-12" },
            { "militia-male-05-backpack", "militia-t2r-tab-bprop-05" },
            { "militia-male-13-backpack", "militia-t2r-tab-bprop-13" },
            { "militia-male-14-backpack", "militia-t2r-tab-bprop-14" },
            { "scar-female-11", "scar-t2r-tab-bprop-f" },
            { "scar-female-09", "scar-t2r-tab-bprop-f" },
            { "militia-female-13-backpack", "militia-t2r-tab-bprop-f-13" },
            { "militia-female-12-backpack", "militia-t2r-tab-bprop-f-12" },
        };

        // [0] the look sid, [1] its armoured twin, [2] the winter twin of the look, [3] the winter
        // look's armoured twin.
        uint64_t g_lookSid[kPropN][4];
        uint64_t g_propSid[kPropN];
        uint8_t  g_always[kPropN];
        int      g_tablesDone = 0;

        void Tables()
        {
            if (g_tablesDone)
                return;
            for (int i = 0; i < kPropN; i++) {
                char twin[64];
                std::snprintf(twin, sizeof twin, "%s-t2r-arm", kPropLooks[i][0]);
                g_lookSid[i][0] = SID(kPropLooks[i][0]);
                g_lookSid[i][1] = SID(twin);
                g_propSid[i] = SID(kPropLooks[i][1]);
                g_lookSid[i][2] = 0;
                g_lookSid[i][3] = 0;
                for (int t = 0; t < 2; t++)
                    for (int k = 0; k < armor_rig::kFlN; k++)
                        if (armor_rig::FactionLook(t, k) && armor_rig::FactionWinter(t, k)
                            && std::strcmp(armor_rig::FactionLook(t, k), kPropLooks[i][0]) == 0) {
                            g_lookSid[i][2] = SID(armor_rig::FactionWinter(t, k));
                            std::snprintf(twin, sizeof twin, "%s-t2r-arm",
                                armor_rig::FactionWinter(t, k));
                            g_lookSid[i][3] = SID(twin);
                        }
                g_always[i] = std::strcmp(kPropLooks[i][1], "scar-t2r-tab-bprop") == 0
                    || std::strcmp(kPropLooks[i][1], "militia-t2r-tab-bprop-11") == 0;
            }
            g_tablesDone = 1;
        }

        // ==================================================================================
        //  ps57 put-away refusal (ext g_bp57RefuseF / CurrentWeapon / IsCode)
        // ==================================================================================

        uint64_t g_refuseF = 0;     // ps57: last frame a duel keep / melee hold refused our put-away

        int IsCode(const void* p)
        {
            if (!p)
                return 0;
            MEMORY_BASIC_INFORMATION mi{};
            return (VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT
                && (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                    | PAGE_EXECUTE_WRITECOPY))) ? 1 : 0;
        }

    }

    // ---------------------------------------------------------------------------------------

    uint8_t* Me()
    {
        return reinterpret_cast<uint8_t*>(engine::local_player::Ptr());
    }

    uint8_t* CtrlOf(uint8_t* me)
    {
        if (!me || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(me) + 0x2f58, 8))
            return nullptr;
        uint8_t* c = *reinterpret_cast<uint8_t**>(me + 0x2f58);
        if (!c || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(c), 0x1f0))
            return nullptr;
        if (*reinterpret_cast<uintptr_t*>(c) != ExeBase() + kCtrlVtRva
            || *reinterpret_cast<uint8_t**>(c + 0x118) != me)
            return nullptr;
        return c;
    }

    uint8_t* CtrlOwn()
    {
        return CtrlOf(Me());
    }

    State Query()
    {
        State s{ 0, 0, -1, nullptr };
        uint8_t* c = CtrlOwn();
        if (!c || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(c) + 0x190, 0x20))
            return s;
        s.have = 1;
        s.ctrl = c;
        s.active = c[0x190] != 0;
        uint8_t* h = *reinterpret_cast<uint8_t**>(c + 0x198);
        const uint32_t id = *reinterpret_cast<uint32_t*>(c + 0x1a0);
        if (h && id && engine::ptr::Readable(reinterpret_cast<uintptr_t>(h), 16)) {
            uint8_t* p = *reinterpret_cast<uint8_t**>(h + 8);
            if (p && engine::ptr::Readable(reinterpret_cast<uintptr_t>(p), 0x618)
                && *reinterpret_cast<uint32_t*>(p + 0x94) == id)
                s.vis = p[0x611] != 0;
        }
        return s;
    }

    bool Hid()          { return g_hid != 0; }
    bool WornHidden()   { return g_wornHidden != 0; }

    uint64_t LifeId()   { return g_spawnedFrame; }
    void     BumpLife() { ++g_spawnedFrame; if (!g_spawnedFrame) g_spawnedFrame = 1; }

    uint32_t EnterCount() { return g_nEnter; }
    uint32_t LeaveCount() { return g_nLeave; }
    double   EnterMs()    { return g_tEnter; }
    double   LeaveMs()    { return g_tLeave; }
    int      EnterSrc()   { return g_srcEnter; }
    int      LeaveSrc()   { return g_srcLeave; }

    void SetInMatch(bool inMatch) { g_inMatch = inMatch ? 1 : 0; }

    int PartCount() { return kNparts; }

    StringId64 PartSid(int i)
    {
        if (i < 0 || i >= kNparts)
            return 0;
        if (!g_partSid[i])
            g_partSid[i] = SID(kParts[i]);
        return g_partSid[i];
    }

    bool HeldGenOk() { return g_hidGen == g_spawnedFrame; }

    void ClearHeld()
    {
        Lock();
        g_hid = 0;
        g_wornHidden = 0;
        Unlock();
    }

    void HideWornNow(uint8_t* me)
    {
        uint8_t* dc = engine::object::DrawControlOfProc(reinterpret_cast<uintptr_t>(me));
        if (!dc)
            return;
        for (int i = 0; i < kNparts; i++)
            if (g_present[i])
                engine::object::HideMesh(dc, PartSid(i));
    }

    void WornSnapshot(uint8_t* me)
    {
        uint8_t* dc = engine::object::DrawControlOfProc(reinterpret_cast<uintptr_t>(me));
        g_snapOk = 0;
        if (!dc)
            return;
        for (int i = 0; i < kNparts; i++) {
            if (!g_partSid[i])
                g_partSid[i] = SID(kParts[i]);
            const int idx = engine::object::MeshIndex(dc, g_partSid[i]);
            g_present[i] = idx >= 0 ? 1 : 0;
            g_wasVis[i] = (idx >= 0 && !engine::object::MeshHidden(dc, idx)) ? 1 : 0;
        }
        g_snapOk = 1;
    }

    void WornOff(uint8_t* me, int src)
    {
        uint8_t* dc = engine::object::DrawControlOfProc(reinterpret_cast<uintptr_t>(me));
        if (!dc || g_hid)
            return;
        if (src != 1) {
            WornSnapshot(me);
            g_snapOk = 0;   // the poll runs after the engine's enter: its own sid is hidden
        }
        int n = 0;
        for (int i = 0; i < kNparts; i++) {
            if (!g_present[i])
                continue;
            engine::object::HideMesh(dc, g_partSid[i]);
            n++;
        }
        g_hidGen = g_spawnedFrame;
        g_hid = 1;
        g_wornHidden = 1;
        g_tEnter = engine::frame::NowMs();
        g_srcEnter = src;
        g_nEnter = g_nEnter + 1;
    }

    void WornOn(uint8_t* me, int src)
    {
        if (!g_hid)
            return;
        g_hid = 0;
        g_wornHidden = 0;
        uint8_t* dc = engine::object::DrawControlOfProc(reinterpret_cast<uintptr_t>(me));
        if (!dc)
            return;
        uint8_t want[kNparts];
        for (int i = 0; i < kNparts; i++)
            want[i] = (g_present[i] && g_wasVis[i]) ? 1 : 0;
        // The armour rig's pushed / original pair wins where it is live: a copy bought in the
        // menu shows after the leave, not the part that was on at the enter (ext BpWornOn).
        const int li = armor_rig::OwnLook();
        const int rig = li >= 0 && li < armor_rig::kRigN
            && armor_rig::OwnProbed() && armor_rig::OwnFail() < 3;
        if (rig)
            for (int k = 0; k < armor_rig::kBodyN; k++) {
                const armor_rig::BodyPair* b = armor_rig::BodyAt(armor_rig::kUs, li, k);
                if (!b || !b->orig || !armor_rig::OwnBodyOk(k))
                    continue;
                const int on = (armor_rig::OwnWant() & b->mask) != 0;
                for (int i = 0; i < kNparts; i++) {
                    if (!g_present[i])
                        continue;
                    if (std::strcmp(kParts[i], b->copy) == 0)
                        want[i] = static_cast<uint8_t>(on);
                    else if (std::strcmp(kParts[i], b->orig) == 0)
                        want[i] = static_cast<uint8_t>(!on);
                }
            }
        int n = 0;
        for (int i = 0; i < kNparts; i++) {
            if (!g_present[i])
                continue;
            if (want[i]) {
                engine::object::ShowMesh(dc, g_partSid[i]);
                n++;
            } else if (rig || g_snapOk) {
                engine::object::HideMesh(dc, g_partSid[i]);   // the engine's leave shows the original even when the twin's copy is worn
            }
            // (no snapshot and no rig: what was on is unknown for the engine's own sid -- its
            // leave already showed it, left as is)
        }
        g_tLeave = engine::frame::NowMs();
        g_srcLeave = src;
        g_nLeave = g_nLeave + 1;
    }

    void ReapplyRig()
    {
        // ext BpReapplyRig: re-apply the armour rig's pushed / original pair of every body part
        // of our own twin from its last mask. A no-op while the rig is unprobed (the armour-visual
        // driver is not ported, so this stays inert).
        const int li = armor_rig::OwnLook();
        if (li < 0 || li >= armor_rig::kRigN || !armor_rig::OwnProbed()
            || armor_rig::OwnFail() >= 3)
            return;
        armor_rig::BodyApply(armor_rig::kUs, SID("player"), li, armor_rig::OwnWant(),
            armor_rig::OwnBodyOkArr());
    }

    // ---- the TAB prop per-look table (ext BP_PROP_LOOKS / BpTables / BpLookProp) -----------

    int PropLookCount() { return kPropN; }

    const char* PropLookName(int i)
    {
        return (i >= 0 && i < kPropN) ? kPropLooks[i][1] : nullptr;
    }

    StringId64 PropArtSid(int i)
    {
        if (i < 0 || i >= kPropN)
            return 0;
        Tables();
        return g_propSid[i];
    }

    int LookProp(uint64_t look)
    {
        Tables();
        for (int i = 0; look && i < kPropN; i++)
            if ((look == g_lookSid[i][0] || look == g_lookSid[i][1]
                 || (g_lookSid[i][2] && look == g_lookSid[i][2])
                 || (g_lookSid[i][3] && look == g_lookSid[i][3]))
                && (g_always[i] || armor_rig::FactionLooksOn()))
                return i;
        return -1;
    }

    uint64_t PlayerLook(uint8_t* player)
    {
        typedef uint64_t* (*fn_look)(void*, void*);
        uint8_t buf[128];
        if (!player || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(player), 8))
            return 0;
        uint8_t* vt = *reinterpret_cast<uint8_t**>(player);
        if (!vt || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(vt) + 0x390, 8))
            return 0;
        fn_look f = *reinterpret_cast<fn_look*>(vt + 0x390);
        if (!f)
            return 0;
        uint64_t* ls = f(player, buf);
        return ls ? *ls : 0;
    }

    bool PerLook() { return g_perLook != 0; }
    void SetPerLook(bool on) { g_perLook = on ? 1 : 0; }

    // ---- the weapon reparent / attach (ext BpReparentHook / BpAttachHook) ------------------

    bool WpnBody() { return g_wpnBody != 0; }
    int  WpnHooks() { return (g_hooks & 2) ? 2 : 0; }

    // ---- the ps57 put-away-refusal recovery (ext g_bp57RefuseF / CurrentWeapon) -------------

    void NoteRefuse(uint64_t frame) { g_refuseF = frame; }
    uint64_t RefuseFrame() { return g_refuseF; }

    uint64_t CurrentWeapon()
    {
        typedef uint64_t (*fn_w)(void*);
        typedef uint8_t* (*fn_me)();
        static const DcValue* rec;
        if (!rec)
            rec = DcManager::Lookup(SID("get-player-desired-or-current-weapon-id"));
        if (!rec)
            return ~0ULL;
        if (IsCode(reinterpret_cast<const void*>(rec->val[1]))) {
            return DC::Call<"get-player-desired-or-current-weapon-id">();
        }
        if (!IsCode(reinterpret_cast<const void*>(rec->val[2])))
            return ~0ULL;
        uint8_t* me = reinterpret_cast<uint8_t*>(engine::local_player::Ptr());
        return me ? reinterpret_cast<fn_w>(rec->val[2])(me) : ~0ULL;
    }

    int LeaveHoldMs()
    {
        static int v = -1;
        static uint64_t next = 0;
        const uint64_t now = GetTickCount64();
        if (v < 0 || now >= next) {
            next = now + 3000;
            int nv = 150;
            FILE* f = engine::exe_side::Open("t2r-bp-leave-delay.txt", "r");
            if (f) {
                int x;
                if (fscanf(f, "%d", &x) == 1 && x >= 0 && x <= 1500)
                    nv = x;
                fclose(f);
            }
            if (nv != v) {
                v = nv;
                TO1_LOG("bp: leave hand-off held back %d ms after the engine's leave event (t2r-bp-leave-delay.txt)\n", v);
            }
        }
        return v;
    }

    bool LeavePending() { return g_leavePend != 0; }

    void RunPendingLeave(bool menu)
    {
        if (!g_leavePend)
            return;
        uint8_t* me = Me();
        uint8_t* c = g_inMatch ? CtrlOf(me) : nullptr;
        if (g_leaveGen != g_spawnedFrame || !c) {
            g_leavePend = 0;
            TO1_LOG("bp: held leave dropped (%s)\n",
                !g_inMatch ? "level gone" : !c ? "no own backpack controller" : "new life");
            return;
        }
        if (!menu && engine::frame::NowMs() < g_leaveDue)
            return;
        g_leavePend = 0;
        const int was = c[0x190];
        g_leaveOrig(c, g_leaveArg);
        if (was && !c[0x190]) {
            Lock();
            WornOn(me, 1);
            Unlock();
        }
        TO1_LOG("bp: held leave ran (%s)\n", menu ? "menu reopened" : "due");
    }

    void Install()
    {
        uint8_t* exe = reinterpret_cast<uint8_t*>(ExeBase());
        // The controller vtable's enter / leave slots (idempotent, retried while it fails).
        if (!(g_hooks & 1)) {
            void** vt = reinterpret_cast<void**>(exe + kCtrlVtRva);
            if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(vt) + kVtLeave, 8)) {
                TO1_LOG("bp: backpack controller vtable unreadable - hooks NOT installed, per-frame poll only\n");
            } else if (vt[kVtEnter / 8] != exe + kRvaEnter
                || vt[kVtLeave / 8] != exe + kRvaLeave) {
                TO1_LOG("bp: backpack controller vtable +0x%x / +0x%x = exe+%llx / exe+%llx, not enter / leave - hooks NOT installed, per-frame poll only\n",
                    static_cast<unsigned>(kVtEnter), static_cast<unsigned>(kVtLeave),
                    static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(vt[kVtEnter / 8]) - exe),
                    static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(vt[kVtLeave / 8]) - exe));
            } else {
                g_enterOrig = reinterpret_cast<fn_enter>(exe + kRvaEnter);
                g_leaveOrig = reinterpret_cast<fn_leave>(exe + kRvaLeave);
                DWORD old = 0;
                VirtualProtect(&vt[kVtEnter / 8], 16, PAGE_READWRITE, &old);
                vt[kVtEnter / 8] = reinterpret_cast<void*>(&EnterHook);
                vt[kVtLeave / 8] = reinterpret_cast<void*>(&LeaveHook);
                VirtualProtect(&vt[kVtEnter / 8], 16, old, &old);
                FlushInstructionCache(GetCurrentProcess(), &vt[kVtEnter / 8], 16);
                g_hooks |= 1;
                TO1_LOG("bp: backpack controller vtable +0x%x / +0x%x hooked (enter / leave)\n",
                    static_cast<unsigned>(kVtEnter), static_cast<unsigned>(kVtLeave));
            }
        }
        // The weapon reparent / attach FUNCTION BODIES (ext InstallBpEngineHooks weapon sites).
        if (!(g_hooks & 2)) {
            g_wpnBody = engine::exe_side::Exists("t2r-bp-weapons-body.txt") ? 1 : 0;
            void* rp = reinterpret_cast<void*>(exe + kRvaReparent);
            void* at = reinterpret_cast<void*>(exe + kRvaAttach);
            if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(rp), 16)
                || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(at), 16)) {
                TO1_LOG("bp: weapon reparent / attach RVAs unreadable - not hooked, the engine's own instant attaches run\n");
            } else {
                g_reparentHook = safetyhook::create_inline(rp, reinterpret_cast<void*>(&ReparentHook));
                g_attachHook   = safetyhook::create_inline(at, reinterpret_cast<void*>(&AttachHook));
                if (static_cast<bool>(g_reparentHook) && static_cast<bool>(g_attachHook)) {
                    g_hooks |= 2;
                    TO1_LOG("bp: weapon reparent (exe+%llx) + attach (exe+%llx) hooked; slung weapons %s\n",
                        static_cast<unsigned long long>(kRvaReparent),
                        static_cast<unsigned long long>(kRvaAttach),
                        g_wpnBody
                            ? "stay on the body (t2r-bp-weapons-body.txt)"
                            : "go onto the bag as in SP, blended 0.2 s from the sling on the enter call");
                } else {
                    TO1_LOG("bp: weapon reparent / attach inline hook failed - the engine's own instant attaches run\n");
                }
            }
        }
    }

    void Reset()
    {
        Lock();
        g_hid = 0;
        g_hidGen = 0;
        g_wornHidden = 0;
        g_inMatch = 0;
        g_inEnter = 0;
        g_snapOk = 0;
        for (int i = 0; i < kNparts; i++) {
            g_present[i] = 0;
            g_wasVis[i] = 0;
        }
        g_nEnter = 0;
        g_nLeave = 0;
        g_tEnter = 0.0;
        g_tLeave = 0.0;
        g_srcEnter = 0;
        g_srcLeave = 0;
        g_leavePend = 0;
        g_leaveArg = 0;
        g_leaveDue = 0.0;
        g_leaveGen = 0;
        g_attachBlend = 0;
        g_inAttach = 0;
        g_wpnSkip = 0;
        g_refuseF = 0;
        Unlock();
    }

}
