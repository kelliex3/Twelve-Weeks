// ported from tools/ext/t2r-ext.c's particle helpers

#include "vfx.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/script.hpp"
#include "engine/ptr.hpp"
#include "engine/object.hpp"
#include "engine/frame.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace engine::vfx {

    uint64_t SpawnParticlesAtJoint(StringId64 effect, uintptr_t process, StringId64 joint,
                                   uint64_t a3, uint64_t flags, StringId64 root,
                                   uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9)
    {
        return DC::Call<"spawn-particles-at-joint">(
            effect, process, joint, a3, flags, root, a6, a7, a8, a9);
    }

    void KillParticles(uint64_t handle)
    {
        if (!handle)
            return;
        DC::Call<"kill-particles">(handle, 0);
    }

    void SetEffectAlpha(uint64_t handle, float alpha)
    {
        if (!handle)
            return;
        DC::Call<"set-effect-alpha">(handle, alpha);
    }

    // ---- GPU-compute particle entries (ext CFX_* / Cfx* / ArmorFxSpawn) ------------------

    namespace {

        uint8_t* ExeBase()
        {
            static uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            return base;
        }

        // The short-lived "dressing" groups whose compute entry may be ended to make room.
        // ext CFX_DRESSING.
        const char* const kDressing[] = {
            "dcl-bullet-drip-spawner", "dcl-gore-drip-spawner",
            "dcl-bullet-gore-02-skin-ribbon",
            "dcl-bullet-gore-02-skin-ribbon-neck",
            "dcl-bullet-gore-02-skin-ribbon-shambler", "gore-cap-entry-wound",
            "gore-cap-entry-wound-bloater", "group-hit-meat-ribbon",
            "group-hit-meat-graze-player", "melee-blood-ribbon",
            "melee-blood-ribbon-dense", "el-blood-ribbon",
            "blood-ribbon-slash-sys",
            "blood-ribbon-splurt-sys", "blood-ribbon-stream-sys",
            "blood-ribbon-stream-eff", "blood-drip-screen-space",
            "gushing-blood", "auto-drips",
            "bm-dcl-blood-slice", "bm-dynamic-blood-slash", "bm-headshot-player",
            "bm-knife-wound-cloth", "bm-knife-wound-skin",
            "bm-blood-restore-knife",
            "bm-blood-restore-shot", "bm-arrow-left-shoulder-skin-eff",
            "bm-arrow-right-shoulder-skin-eff", "bm-ellie-head-right",
            "squib-brick-pistol", "squib-concrete-pistol", "squib-wood-pistol",
            "squib-wood-tommy-sniper", "squib-water-pistol",
            "squib-water-tommy-sniper",
            "pistol-muzzleflash-eff", "pistol-muzzleflash-player",
            "pistol-muzzleflash-silencer-bottle-player",
            "pistol-muzzleflash-silencer-can-player",
            "revolver-muzzleflash-player", "rifle-bolt-action-muzzleflash-player",
            "rifle-muzzleflash-player", "tracer-smoke-basic",
            "tracer-smoke-tommy"
        };

        // Compute groups only logged by name (never ended). ext CFX_KEEP.
        const char* const kKeep[] = {
            "blood-ribbon-pulse-eff", "blood-ribbon-splurt-eff",
            "dynamic-blood-box", "dynamic-water-box",
            "nav-water-dynamic-drips-back",
            "nav-water-dynamic-drips-head", "nav-water-dynamic-drips-shoulder",
            "auto-drips-water", "gushing-water", "lm-scent-trail",
            "rain-drip-screen-space", "rain-drip-screen-space-fg",
            "rain-drip-screen-space-fg-blood-pickup", "snow-sticky-compute",
            "spore-swirl-emitter", "glass-car-window-decal-gpu",
            "glass-shards-decal-dispatcher"
        };

        const char* GroupName(uint64_t s)
        {
            for (const char* n : kDressing)
                if (SID(n) == s)
                    return n;
            for (const char* n : kKeep)
                if (SID(n) == s)
                    return n;
            return nullptr;
        }

        bool IsDressing(uint64_t s)
        {
            for (const char* n : kDressing)
                if (SID(n) == s)
                    return true;
            return false;
        }

        // Group name sid of entry i's owner (0 = entry free / owner gone / layout check failed).
        uint64_t Owner(const uint8_t* m, int i)
        {
            const uint8_t* e = m + 0x5840 + static_cast<size_t>(i) * 0x240;
            if (!e[0x30])
                return 0;
            uint8_t** hp = *reinterpret_cast<uint8_t***>(const_cast<uint8_t*>(e));
            const uint32_t id = *reinterpret_cast<const uint32_t*>(e + 8);
            if (!hp || !id || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(hp), 8))
                return 0;
            uint8_t* root = *hp;
            if (!root || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(root), 0x50)
                || *reinterpret_cast<const uint32_t*>(root) != id)
                return 0;
            const uint64_t s = *reinterpret_cast<const uint64_t*>(root + 0x30);
            uint8_t* grp = *reinterpret_cast<uint8_t**>(root + 0x48);
            if (!grp || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(grp), 0x10)
                || *reinterpret_cast<const uint64_t*>(grp + 8) != s)
                return 0;
            return s;
        }

        void LogEntries(const uint8_t* m, const char* what)
        {
            uint64_t sid[kComputeEntries];
            int cnt[kComputeEntries], ng = 0, gone = 0;
            for (int i = 0; i < kComputeEntries; i++) {
                if (!m[0x5870 + i * 0x240])
                    continue;
                const uint64_t s = Owner(m, i);
                if (!s) { gone++; continue; }
                int k = 0;
                while (k < ng && sid[k] != s)
                    k++;
                if (k == ng) { sid[ng] = s; cnt[ng++] = 0; }
                cnt[k]++;
            }
            char buf[900] = "";
            for (int k = 0; k < ng; k++) {
                const char* nm = GroupName(sid[k]);
                char one[96];
                if (nm)
                    snprintf(one, sizeof one, " %s x%d%s", nm, cnt[k],
                        IsDressing(sid[k]) ? "" : " (kept)");
                else
                    snprintf(one, sizeof one, " %016llx x%d",
                        static_cast<unsigned long long>(sid[k]), cnt[k]);
                strncat(buf, one, sizeof buf - strlen(buf) - 1);
            }
            TO1_LOG("[VFX] particle compute entries: %s - %d/%d in use:%s%s\n", what,
                Used(m), kComputeEntries, ng ? buf : " (no live owner read)",
                gone ? " + owner gone / unreadable" : "");
        }

        // A stream spawn that did not happen: SP's slash ribbon + drop burst at the same joint.
        void Fallback(uint64_t particle, uint64_t chr, const char* joint, const char* why)
        {
            static int nl;
            const uint64_t r = SpawnRaw(SID("blood-ribbon-slash-eff"), chr, joint);
            SpawnRaw(SID("blood-drops-burst-eff"), chr, joint);
            if (nl < 20) {
                nl++;
                TO1_LOG("[VFX] blood stream %s at %s: %s - slash ribbon + drops instead -> %llx\n",
                    particle == 0xdc2d31c03c7cded8ULL ? "blood-ribbon-pulse-eff" : "blood-ribbon-splurt-eff",
                    joint, why, static_cast<unsigned long long>(r));
            }
        }

        // stream spawns waiting for the entry an ended dressing effect gives back
        struct PendingStream {
            uint64_t particle, chr, until;
            const char* joint;
        };
        PendingStream g_q[4];

    }

    bool Stream(uint64_t particle)
    {
        return particle == 0xdc2d31c03c7cded8ULL      // pulse
            || particle == 0x4fac22f779791381ULL;     // splurt
    }

    uint8_t* Rtmgr()
    {
        static int ok = -1;
        uint8_t* exe = ExeBase();
        if (ok < 0) {
            static const uint8_t lea_inuse[7] = { 0x49, 0x8d, 0x87, 0x70, 0x58, 0x00, 0x00 };   // 141b44933
            static const uint8_t cmp_n[4]     = { 0x48, 0x83, 0xfe, 0x17 };                     // 141b44951
            static const uint8_t kill_pro[8]  = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48 }; // 141b59010
            static const uint8_t call_cce[5]  = { 0xe8, 0xab, 0x3b, 0xfe, 0xff };               // 141b60d30
            ok = (!std::memcmp(exe + 0x1b44933, lea_inuse, sizeof lea_inuse)
                  && !std::memcmp(exe + 0x1b44951, cmp_n, sizeof cmp_n)
                  && !std::memcmp(exe + 0x1b59010, kill_pro, sizeof kill_pro)
                  && !std::memcmp(exe + 0x1b60d30, call_cce, sizeof call_cce)) ? 1 : 0;
            if (!ok)
                TO1_LOG("[VFX] particle compute entries: exe code differs - table not read, "
                    "streams keep the slash-ribbon fallback\n");
        }
        return ok ? exe + 0x37dee90 : nullptr;
    }

    int Used(const uint8_t* m)
    {
        if (!m)
            return 0;
        int n = 0;
        for (int i = 0; i < kComputeEntries; i++)
            n += m[0x5870 + i * 0x240] != 0;
        return n;
    }

    bool MakeRoom(uint8_t* m)
    {
        if (!m)
            return false;
        using KillFn = void(__fastcall*)(void*, const void*, char);
        uint8_t* exe = ExeBase();
        for (int i = kComputeEntries - 1; i >= 0; i--) {
            const uint64_t s = Owner(m, i);
            if (!s || !IsDressing(s))
                continue;
            uint8_t h[16];
            std::memcpy(h, m + 0x5840 + static_cast<size_t>(i) * 0x240, 16);
            reinterpret_cast<KillFn>(exe + 0x1b59010)(exe + 0x92d9720, h, 1);
            TO1_LOG("[VFX] particle compute entries: ended %s (entry %d) to free an entry for "
                "the blood stream\n", GroupName(s), i);
            return true;
        }
        return false;
    }

    uint64_t SpawnRaw(uint64_t particle, uint64_t chr, const char* joint)
    {
        static const char* const kSafe[] = { "spineb", "pelvis", "root" };
        if (!joint)
            return 0;
        if (!engine::object::JointFinite(chr, SID(joint))) {
            const char* alt = nullptr;
            for (int k = 0; k < 3 && !alt; k++)
                if (strcmp(joint, kSafe[k]) && engine::object::JointFinite(chr, SID(kSafe[k])))
                    alt = kSafe[k];
            static int nlj;
            if (nlj < 24) {
                nlj++;
                TO1_LOG("[VFX] particle spawn: particle %016llx joint '%s' of %016llx reads "
                    "non-finite - %s\n", static_cast<unsigned long long>(particle), joint,
                    static_cast<unsigned long long>(chr), alt ? alt : "spawn skipped");
            }
            if (!alt)
                return 0;
            joint = alt;
        }
        const uintptr_t loc = LOCATOR::GET_JOINT_LOCATOR(chr, SID(joint));
        uint64_t r = 0;
        if (loc && loc != ~static_cast<uintptr_t>(0)) {
            r = DC::Call<"spawn-particles-at-bound-frame">(particle, static_cast<uint64_t>(loc),
                SID("none"));
        }
        if (!r || r == ~0ULL) {
            r = DC::Call<"spawn-particles-at-joint">(particle, chr, SID(joint), 0ull, 2ull,
                SID("root"), 2ull, 0ull, 2ull, 0ull);
        }
        return r;
    }

    uint64_t ArmorFxSpawn(uint64_t particle, uint64_t chr, const char* joint)
    {
        const uint64_t r = SpawnRaw(particle, chr, joint);
        if ((r && r != ~0ULL) || !Stream(particle))
            return r;
        static int nl;
        uint8_t* m = Rtmgr();
        if (!m) {
            Fallback(particle, chr, joint, "compute table not readable");
            return 0;
        }
        if (Used(m) < kComputeEntries) {
            if (nl < 12) {
                nl++;
                LogEntries(m, "stream spawn failed with free entries (not the compute budget: "
                    "missing group / root overflow?)");
            }
            Fallback(particle, chr, joint, "spawn failed with compute entries free");
            return 0;
        }
        if (nl < 12) {
            nl++;
            LogEntries(m, "stream spawn failed: compute-entry table full (CreateComputeEntry "
                "141b448e0 -> -1)");
        }
        int q = 0;
        while (q < 4 && g_q[q].particle)
            q++;
        if (q == 4 || !MakeRoom(m)) {
            Fallback(particle, chr, joint,
                q == 4 ? "retry queue full" : "table full of kept effects - nothing ended");
            return 0;
        }
        g_q[q].particle = particle;
        g_q[q].chr = chr;
        g_q[q].joint = joint;
        g_q[q].until = engine::frame::T60() + 15;   // 0.25 s
        return 1;   // queued (spawns on a following frame): not a failure for the caller
    }

    void RetryTick()
    {
        static int nl;
        for (int q = 0; q < 4; q++) {
            if (!g_q[q].particle)
                continue;
            const uint64_t p = g_q[q].particle, c = g_q[q].chr;
            const char* j = g_q[q].joint;
            const uint64_t r = SpawnRaw(p, c, j);
            if (r && r != ~0ULL) {
                g_q[q].particle = 0;
                if (nl < 20) {
                    nl++;
                    TO1_LOG("[VFX] blood stream %s at %s spawned after the entry came back -> %llx\n",
                        p == 0xdc2d31c03c7cded8ULL ? "blood-ribbon-pulse-eff" : "blood-ribbon-splurt-eff",
                        j, static_cast<unsigned long long>(r));
                }
            } else if (engine::frame::T60() >= g_q[q].until) {
                g_q[q].particle = 0;
                if (uint8_t* m = Rtmgr())
                    LogEntries(m, "stream still not spawned 0.25 s after ending a holder");
                Fallback(p, c, j, "no compute entry came back");
            }
        }
    }

}
