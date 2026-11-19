// ported from tools/ext/clipfx.c (the fire path; the FireEffect watch hook is not ported)

#include "clipfx.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/script.hpp"
#include "engine/ptr.hpp"
#include "engine/local-player.hpp"
#include "engine/exe_side.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace engine::clipfx {

    namespace {

        constexpr uint64_t kName  = 0xc4bcadba8e631b86ULL;  // "name" param key
        constexpr uint64_t kJoint = 0x52e7fc8056bc65d7ULL;  // the event's joint key

        uint8_t* Exe()
        {
            static uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            return base;
        }

        int g_ok = -1;      // exe code verified

        // The joint's world position reads finite (ext CfxeJointSidOk). Missing / unreadable
        // counts as NOT finite here, so a bad joint is swapped / skipped.
        bool JointReadsFinite(uint64_t ent, uint64_t jsid)
        {
            const uintptr_t loc = LOCATOR::GET_JOINT_LOCATOR(ent, jsid);
            if (!loc || loc == ~static_cast<uintptr_t>(0) || !engine::ptr::Readable(loc, 16))
                return false;
            const uintptr_t p = LOCATOR::GET_LOCATOR_POSITION_PTR(loc);
            if (!p || p == ~static_cast<uintptr_t>(0) || !engine::ptr::Readable(p, 12))
                return false;
            float t[3];
            std::memcpy(t, reinterpret_cast<const void*>(p), sizeof t);
            for (int i = 0; i < 3; i++)
                if (!std::isfinite(t[i]) || std::fabs(t[i]) > 1e6f)
                    return false;
            return true;
        }

        // A clip event names a joint of the body. On a body posed by a paired clip a joint's
        // locator can read non-finite for a frame (the engine's nd-particle-tracker then
        // asserts IsFinite). Before such an event fires the joint is swapped for the nearest
        // anatomical joint that reads finite, in a persistent copy of the entry; when none
        // reads finite the event is skipped. ext CfxeSafeJoint.
        const Entry* SafeJoint(uint64_t ent, const Entry* e)
        {
            if (!e || !e->pairs)
                return e;
            uint32_t ji = ~0u;
            for (uint32_t i = 0; i < e->n && i < 64; i++)
                if (e->pairs[i][0] == kJoint) { ji = i; break; }
            if (ji == ~0u)
                return e;
            const uint64_t j = e->pairs[ji][1];
            if (JointReadsFinite(ent, j))
                return e;

            static const char* const kFallback[] = { "r_wrist", "neck", "spineb", "pelvis" };
            struct Cache { const Entry* src; uint64_t fb; Entry e; };
            static Cache  cache[32];
            static int    ncache;
            static uint64_t logged[16];
            static int    nlogged;

            uint64_t fb = 0;
            for (int k = 0; k < 4; k++) {
                const uint64_t c = SID(kFallback[k]);
                if (c != j && JointReadsFinite(ent, c)) { fb = c; break; }
            }

            bool seen = false;
            for (int i = 0; i < nlogged; i++)
                if (logged[i] == j) { seen = true; break; }
            if (!seen && nlogged < 16) {
                logged[nlogged++] = j;
                TO1_LOG("[CLIPFX] joint %016llx of an event on a body reads non-finite - %s\n",
                    static_cast<unsigned long long>(j),
                    fb ? "fired on the nearest finite joint instead" : "event skipped (no finite joint)");
            }
            if (!fb)
                return nullptr;

            for (int i = 0; i < ncache; i++)
                if (cache[i].src == e && cache[i].fb == fb)
                    return &cache[i].e;
            if (ncache >= 32)
                return nullptr;

            auto* p = static_cast<uint64_t(*)[2]>(std::calloc(e->n ? e->n : 1, 16));
            if (!p)
                return nullptr;
            for (uint32_t i = 0; i < e->n; i++) {
                p[i][0] = e->pairs[i][0];
                p[i][1] = e->pairs[i][1];
            }
            p[ji][1] = fb;
            cache[ncache].src = e;
            cache[ncache].fb  = fb;
            cache[ncache].e   = *e;
            cache[ncache].e.pairs = p;
            return &cache[ncache++].e;
        }

    }

    bool File(const char* name, int* cache, uint64_t* chk)
    {
        if (!name || !cache || !chk)
            return false;
        const uint64_t now = GetTickCount64();
        if (*cache < 0 || now - *chk > 2000) {
            *chk = now;
            *cache = engine::exe_side::Exists(name) ? 1 : 0;
        }
        return *cache != 0;
    }

    bool Off()
    {
        static int c = -1;
        static uint64_t k = 0;
        return File("t2r-clipfx-off.txt", &c, &k);
    }

    uint8_t* ObjOf(uint64_t ent)
    {
        if (!ent)
            return nullptr;
        if (ent == SID("player"))
            return reinterpret_cast<uint8_t*>(engine::local_player::Ptr());
        return reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(DcManager::LookupObject(ent)));
    }

    bool Check()
    {
        if (g_ok >= 0)
            return g_ok != 0;
        uint8_t* exe = Exe();
        static const uint8_t ctor[24] = {
            0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89,
            0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56
        };  // 141c2ec30 EffectAnimInfo ctor
        static const uint8_t fire[17] = {
            0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x81, 0xec, 0xb8, 0x04, 0x00
        };  // 14038a260 FireEffect
        static const uint8_t dflt[16] = {
            0x33, 0xd2, 0x48, 0x8d, 0x4d, 0x50, 0x41, 0xb8, 0xb0, 0x03, 0x00, 0x00,
            0xe8, 0x6b, 0xcc, 0xd6
        };  // 141b04824 NdGameObject::HandleEvent default case
        const uint64_t slot = *reinterpret_cast<const uint64_t*>(exe + 0x2b274d8);
        g_ok = (!std::memcmp(exe + 0x1c2ec30, ctor, sizeof ctor)
                && !std::memcmp(exe + 0x38a260, fire, sizeof fire)
                && !std::memcmp(exe + 0x1b04824, dflt, sizeof dflt)
                && slot != 0) ? 1 : 0;
        if (!g_ok)
            TO1_LOG("[CLIPFX] exe code differs (EffectAnimInfo / FireEffect / event handler) - "
                "clip events fall back to spawn-particles-at-joint\n");
        return g_ok != 0;
    }

    uint64_t Fire(uint8_t* obj, uint64_t clip, const Entry* e, bool ownBuilder)
    {
        if (!obj || !e || !Check())
            return 0;
        uint8_t* exe = Exe();

        static int base_ok = -1;
        if (base_ok < 0) {
            static const uint8_t base[14] = {
                0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x81, 0xc8, 0x01, 0x00, 0x00,
                0x4d, 0x8b, 0xd8
            };  // 141b0c160 base effect-info builder
            base_ok = !std::memcmp(exe + 0x1b0c160, base, sizeof base) ? 1 : 0;
            if (!base_ok)
                TO1_LOG("[CLIPFX] base effect-info builder 0x141b0c160 differs - puppets use "
                    "their own builder\n");
        }

        if (obj == reinterpret_cast<uint8_t*>(engine::local_player::Ptr())) {
            e = SafeJoint(SID("player"), e);
            if (!e)
                return 0;
        } else if (engine::ptr::Readable(reinterpret_cast<uintptr_t>(obj) + 0xc0, 8)
                   && *reinterpret_cast<const uint64_t*>(obj + 0xc0)) {
            e = SafeJoint(*reinterpret_cast<const uint64_t*>(obj + 0xc0), e);
            if (!e)
                return 0;
        }

        void** vt = *reinterpret_cast<void***>(obj);
        uint8_t* ec = *reinterpret_cast<uint8_t**>(obj + 0x1c8);
        if (!vt || !ec)
            return 0;
        if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(vt) + 0x508, 8))
            return 0;
        void* builder = (ownBuilder || !base_ok)
            ? vt[0x508 / 8]
            : reinterpret_cast<void*>(exe + 0x1b0c160);

        void** evt = *reinterpret_cast<void***>(ec);
        // The slot is the exe's FireEffect or a hook installed into it; either is callable.
        if (!evt || !evt[0x48 / 8])
            return 0;

        static __attribute__((aligned(32))) uint8_t info[0x400];
        struct Msg {
            uint64_t anim;
            const Entry* ev;
            uint64_t aux;
            uint8_t mirror;
            uint8_t pad[7];
        } msg = { clip, e, 0, 0, { 0 } };
        uint64_t out[2] = { 0, 0 };
        std::memset(info, 0, 0x3b0);

        using Ctor = void(__fastcall*)(void*);
        reinterpret_cast<Ctor>(exe + 0x1c2ec30)(info);
        using Builder = void(__fastcall*)(void*, void*, void*);
        reinterpret_cast<Builder>(builder)(obj, &msg, info);
        using FireFn = void*(__fastcall*)(void*, void*, void*);
        reinterpret_cast<FireFn>(evt[0x48 / 8])(ec, out, info);
        return out[0] ? out[0] : 1;
    }

}
