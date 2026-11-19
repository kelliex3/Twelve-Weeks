// ported from tools/ext/t2r-ext.c (XbxLoc / GvisDc / ObjLook / LookName / SetObjLoc)

#include "object.hpp"

#include "ptr.hpp"
#include "script.hpp"
#include "transform.hpp"
#include "local-player.hpp"

#include "log.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine::object {

    namespace {

        // handle -> process, the ext's kFnEntityFromHandle: the mod's route is the same
        // lookup the rest of the net layer uses (DcManager::LookupObject).
        uintptr_t ProcOf(StringId64 handle)
        {
            if (!handle)
                return 0;
            return static_cast<uintptr_t>(DcManager::LookupObject(handle));
        }

    }

    bool EntityWorld(StringId64 handle, float out[8])
    {
        return engine::transform::ProcessWorld(ProcOf(handle), out);
    }

    bool LocalPlayerWorld(float out[8])
    {
        return engine::transform::ProcessWorld(engine::local_player::Ptr(), out);
    }

    bool JointLocator(StringId64 entity, const char* joint, float p[3], float q[4])
    {
        if (!entity || !joint || !p)
            return false;
        const uintptr_t jl = LOCATOR::GET_JOINT_LOCATOR(entity, SID(joint));
        if (!jl || jl == ~static_cast<uintptr_t>(0))
            return false;
        const uintptr_t jp = LOCATOR::GET_LOCATOR_POSITION_PTR(jl);
        if (!jp || jp == ~static_cast<uintptr_t>(0) || !engine::ptr::Readable(jp, 12))
            return false;
        std::memcpy(p, reinterpret_cast<const void*>(jp), 12);
        if (!std::isfinite(p[0] + p[1] + p[2]) || std::fabs(p[0]) > 1e5f || std::fabs(p[2]) > 1e5f)
            return false;
        if (q) {
            uintptr_t jr = LOCATOR::GET_LOCATOR_POSITION_PTR(jl) ? 0 : 0;   // (unused; kept for parity)
            (void)jr;
            const Quaternion r = LOCATOR::GET_LOCATOR_ROTATION(jl);
            q[0] = r.x; q[1] = r.y; q[2] = r.z; q[3] = r.w;
            const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            if (!(n > 0.25f && n < 4.0f))
                return false;
        }
        return true;
    }

    bool JointFinite(StringId64 chr, StringId64 jsid)
    {
        const uintptr_t loc = LOCATOR::GET_JOINT_LOCATOR(chr, jsid);
        if (!loc || loc == ~static_cast<uintptr_t>(0) || !engine::ptr::Readable(loc, 16))
            return true;        // no such joint: the spawn natives deal with that as before
        const uintptr_t p = LOCATOR::GET_LOCATOR_POSITION_PTR(loc);
        if (!p || p == ~static_cast<uintptr_t>(0) || !engine::ptr::Readable(p, 12))
            return true;
        float t[3];
        std::memcpy(t, reinterpret_cast<const void*>(p), sizeof t);
        for (int i = 0; i < 3; i++)
            if (!std::isfinite(t[i]) || std::fabs(t[i]) > 1e6f)
                return false;
        return true;
    }

    uint8_t* DrawControl(StringId64 handle)
    {
        const uintptr_t p = handle == SID("player") ? engine::local_player::Ptr() : ProcOf(handle);
        return DrawControlOfProc(p);
    }

    uint8_t* DrawControlOfProc(uintptr_t p)
    {
        if (!p || !engine::ptr::Readable(p, 0x1b8))
            return nullptr;
        const uint64_t vt = *reinterpret_cast<const uint64_t*>(p);
        if (vt < 0x100000000ULL || !engine::ptr::Readable(static_cast<uintptr_t>(vt), 8))
            return nullptr;     // released block (the ext's dead-puppet guard)
        uint8_t* dc = *reinterpret_cast<uint8_t**>(p + 0x1b0);
        return dc && engine::ptr::Readable(reinterpret_cast<uintptr_t>(dc), 0x18) ? dc : nullptr;
    }

    // ---- draw-control mesh visibility (ext BpDcFind / BpDcHidden / BpDcHide / BpDcShow) --

    namespace {

        uintptr_t ExeBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

    }

    int MeshIndex(uint8_t* dc, StringId64 sid)
    {
        if (!dc || !sid)
            return -1;
        using Fn = int(__fastcall*)(void*, uint64_t);
        return reinterpret_cast<Fn>(ExeBase() + 0x1482160)(dc, sid);
    }

    bool MeshHidden(uint8_t* dc, int idx)
    {
        if (!dc || idx < 0)
            return false;
        using Fn = char(__fastcall*)(void*, int, int);
        return reinterpret_cast<Fn>(ExeBase() + 0x1483960)(dc, idx, -1) != 0;
    }

    void HideMesh(uint8_t* dc, StringId64 sid)
    {
        if (!dc || !sid)
            return;
        using Fn = void(__fastcall*)(void*, uint64_t, int, uint64_t);
        reinterpret_cast<Fn>(ExeBase() + 0x1482a30)(dc, sid, -1, 0);
    }

    void ShowMesh(uint8_t* dc, StringId64 sid)
    {
        if (!dc || !sid)
            return;
        using Fn = void(__fastcall*)(void*, uint64_t, int, uint64_t);
        reinterpret_cast<Fn>(ExeBase() + 0x1488380)(dc, sid, -1, 0);
    }

    uint64_t Look(StringId64 object)
    {
        if (!object)
            return 0;
        return DC::Call<"get-object-look">(object, 0);
    }

    void SetLocator(StringId64 handle, const float pos[3], const float quat[4])
    {
        if (!handle || !pos || !quat)
            return;
        const float qn = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3];
        if (!std::isfinite(pos[0] + pos[1] + pos[2] + qn) || !(qn > 0.25f && qn < 4.0f))
            return;
        for (int i = 0; i < 3; i++)
            if (!(std::fabs(pos[i]) < 1e5f))
                return;
        LOCATOR::SET_OBJECT_LOCATOR(handle, MEMORY::ALLOC_BOUND_FRAME(
            Vector3{ pos[0], pos[1], pos[2] }, Quaternion{ quat[0], quat[1], quat[2], quat[3] }));
    }

    // ---- diagnostic look names (ext LooksLoad / LookName) --------------------------------

    namespace {

        constexpr int kMaxLooks = 1400;
        char     g_lookName[kMaxLooks][64];
        uint64_t g_lookHash[kMaxLooks];
        int      g_nLooks;

        void LooksLoad()
        {
            if (g_nLooks)
                return;
            char pth[MAX_PATH];
            GetModuleFileNameA(nullptr, pth, MAX_PATH);
            char* sl = strrchr(pth, '\\');
            if (!sl)
                return;
            strcpy(sl + 1, "tlou-ii-mp\\t2r-looks.txt");
            FILE* f = fopen(pth, "r");
            if (!f)
                return;
            char ln[128];
            while (g_nLooks < kMaxLooks && fgets(ln, sizeof ln, f)) {
                size_t n = strlen(ln);
                while (n && (ln[n - 1] == '\n' || ln[n - 1] == '\r' || ln[n - 1] == ' '))
                    ln[--n] = 0;
                if (!n || n >= 64)
                    continue;
                strcpy(g_lookName[g_nLooks], ln);
                g_lookHash[g_nLooks++] = SID(ln);
            }
            fclose(f);
        }

    }

    const char* LookName(uint64_t sid)
    {
        LooksLoad();
        for (int i = 0; i < g_nLooks; i++)
            if (g_lookHash[i] == sid)
                return g_lookName[i];
        return "?";
    }

}
