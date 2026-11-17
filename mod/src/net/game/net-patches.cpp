#include "net-patches.hpp"
#include "net-protocol.hpp"
#include "net-debug.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <safetyhook.hpp>
#include "engine/memory.hpp"

uintptr_t g_base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));

void Patch8(uintptr_t base, uint32_t rva, uint8_t val)
{
    DWORD old; uint8_t* p = reinterpret_cast<uint8_t*>(base + rva);
    VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old);
    *p = val;
    VirtualProtect(p, 1, old, &old);
}

static void PatchBytes(uintptr_t base, uint32_t rva, const uint8_t* bytes, size_t n)
{
    DWORD old; uint8_t* p = reinterpret_cast<uint8_t*>(base + rva);
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
}

namespace net::patch {
    namespace {

        SafetyHookInline g_hkProcLink, g_hkProcUnlink; // not needed anymore, no longer using players as puppets
        uintptr_t        g_singletonNode = 0;

        __int64 __fastcall hk_ProcLink(__int64 a1, __int64 a2) {
            if ((uintptr_t)a2 == g_singletonNode
                && *reinterpret_cast<uintptr_t*>(a2 + 8) != 0)
                return a1 + 0x481568;
            return g_hkProcLink.call<__int64>(a1, a2);
        }

        __int64 __fastcall hk_ProcUnlink(__int64 a1, __int64 a2) {
            if ((uintptr_t)a2 == g_singletonNode
                && *reinterpret_cast<uintptr_t*>(a2 + 8) == 0)
                return 0;
            return g_hkProcUnlink.call<__int64>(a1, a2);
        }

        void GuardProcessListLinks(uintptr_t base) {
            g_singletonNode = base + 0x380A398;   // off_14380A398
            g_hkProcLink   = safetyhook::create_inline((void*)(base + 0x1A52C90), (void*)hk_ProcLink);
            g_hkProcUnlink = safetyhook::create_inline((void*)(base + 0x1A53630), (void*)hk_ProcUnlink);
        }
    }
}

SafetyHookInline g_hkWidgetFirstChild;
__int64 __fastcall hk_WidgetFirstChild(__int64 a1)
{
    if (!a1) return 0;
    return g_hkWidgetFirstChild.call<__int64>(a1);
}

SafetyHookMid g_hkHudRootGuard;
static void hk_HudRootGuard(SafetyHookContext& ctx)
{
    if (ctx.rax == 0)
        ctx.rip = g_base + 0x87E2C3;
}

SafetyHookInline g_hkRestoreStats;

static char hk_RestoreStats(void* statsState, unsigned int resetType) {
    return 1;   // apply nothing
}

static SafetyHookInline g_hkApplySaveData;
static __int64 __fastcall hk_ApplySaveData(uintptr_t a1, uintptr_t a2, int* a3, unsigned int a4) {
    return 0;   // nothing applied
}

static SafetyHookInline g_hkDrawItem;   // sub_14129D890
static __int64 __fastcall hk_DrawItem(uintptr_t* a1, char a2, uintptr_t a3, uintptr_t a4) {
    if (!a1 || !*a1) return 0;          // null drawable -> skip it
    return g_hkDrawItem.unsafe_fastcall<__int64>(a1, a2, a3, a4);
}

// AddMaterialToQueue fix

namespace net::patch {
    namespace {
        constexpr uint32_t kRvaMatRingLoad = 0x1395664;   // mov r12,[r15+r14*8+38h]
        constexpr int      kRingMaxNodes   = 64;          // walk bound; real rings are tiny
        constexpr uint32_t kRingMaxParams  = 256;         // sub_14184C910's reflection copy cap

        SafetyHookMid g_hkMatRing;
        long          g_dRingSevered = 0;

        static bool RingNodeSane(uintptr_t node, uintptr_t defEntry)
        {
            if (!node || (node & 15u)) return false;                 // ring nodes are 16-B aligned
            if (node < 0x10000ull || node > 0x7FFFFFFFFFFFull) return false;
            __try {
                if (*reinterpret_cast<uintptr_t*>(node + 0x10) != defEntry) return false;
                if (*reinterpret_cast<uint32_t*>(node + 0x20) > kRingMaxParams) return false;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            return true;
        }

        static bool RingIsSane(uintptr_t head, uintptr_t defEntry)
        {
            uintptr_t n = head;
            for (int i = 0; i < kRingMaxNodes; ++i) {
                if (!RingNodeSane(n, defEntry)) return false;
                uintptr_t next = 0;
                __try { next = *reinterpret_cast<uintptr_t*>(n); }
                __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
                if (next == head) return true;                        // closed the circle cleanly
                n = next;
            }
            return false;                                             // unterminated -> corrupt
        }

        void hk_MatRingGuard(SafetyHookContext& ctx)
        {
            const uintptr_t defEntry = ctx.r15 + ctx.r14 * 8;
            uintptr_t head = 0;
            __try { head = *reinterpret_cast<uintptr_t*>(defEntry + 0x38); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return; }
            if (!head) return;                                        // nothing to walk anyway

            if (!RingIsSane(head, defEntry)) {
                // Sever it. Leaving it linked means every later call re-walks the same dead ring,
                // so this both stops the crash and stops it recurring for this table.
                __try { *reinterpret_cast<uintptr_t*>(defEntry + 0x38) = 0; }
                __except (EXCEPTION_EXECUTE_HANDLER) { return; }
                if ((++g_dRingSevered % 32) == 1)
                    TO1_LOG("[MATRING] severed a dangling material-param ring (%ld total)\n",
                            g_dRingSevered);
            }
        }
    }
}

namespace net::patch {
    namespace {
        constexpr uint32_t kRvaMatPatchCount = 0x824CBD8;   // g_numMaterialPatches
        constexpr uint32_t kRvaMatAddAssert  = 0x13949D6;   // site 1 assert block
        constexpr uint32_t kRvaMatTblAssert  = 0x13956C9;   // site 2 assert block
        constexpr uint32_t kRvaMatDrain      = 0x1394CA0;   // the per-frame drain

        uint32_t* g_matPatchCount = nullptr;
        SafetyHookInline g_hkMatDrain;
        int              g_matPeak = 0;

        void* hk_MatDrain(uintptr_t frameParams)
        {
            if (g_matPatchCount) {
                const int n = static_cast<int>(*g_matPatchCount);
                if (n > g_matPeak) {
                    g_matPeak = n;
                    if (n >= 256)
                        TO1_LOG("[MATQ] material-patch queue high-water %d/512\n", n);
                }
            }
            return g_hkMatDrain.unsafe_fastcall<void*>(frameParams);
        }

        void ClampMaterialPatchQueue(uintptr_t base)
        {
            static const uint8_t kSite1[] = { 0x41, 0xBA, 0xFF, 0x01, 0x00, 0x00, 0xEB, 0x7D };
            static const uint8_t kSite2[] = { 0xB9, 0xFF, 0x01, 0x00, 0x00, 0xEB, 0x7E, 0x90 };

            PatchBytes(base, kRvaMatAddAssert, kSite1, sizeof(kSite1));
            PatchBytes(base, kRvaMatTblAssert, kSite2, sizeof(kSite2));

            g_matPatchCount = reinterpret_cast<uint32_t*>(base + kRvaMatPatchCount);
            g_hkMatDrain = safetyhook::create_inline(base + kRvaMatDrain, hk_MatDrain);

            TO1_LOG("net-patch: material-patch queue clamped (AddMaterialToQueue assert).\n");
        }
    }

    void FlushMaterialPatchQueue()
    {
        if (g_matPatchCount && *g_matPatchCount) {
            TO1_LOG("[MATQ] flushing %u stale material patches on teardown\n",
                    *g_matPatchCount);
            *g_matPatchCount = 0;
        }
        g_matPeak = 0;
    }
}

constexpr uint32_t kRvaOwnerVCall = 0x15A1315;
constexpr uint32_t kRvaOwnerNull = 0x15A132D;
constexpr uint32_t kRvaPurecall = 0x287143C;
constexpr uint32_t kRvaTextBegin = 0x0001000;
constexpr uint32_t kRvaTextEnd = 0x2A8C000;
constexpr uint32_t kImageEnd = 0x9A68000;

SafetyHookMid g_hkOwnerVCall;
long g_dDeadOwner = 0;

static bool OwnerIsLive(uintptr_t obj)
{
    if (!obj || (obj & 7u)) return false;
    __try {
        const uintptr_t vptr = *reinterpret_cast<uintptr_t*>(obj);
        if (vptr & 7u) return false;
        if (vptr <= g_base || vptr >= g_base + kImageEnd) return false;

        const uintptr_t fn = *reinterpret_cast<uintptr_t*>(vptr + 0x30);
        if (fn == g_base + kRvaPurecall) return false; 
        if (fn < g_base + kRvaTextBegin) return false;
        if (fn >= g_base + kRvaTextEnd)  return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

void hk_OwnerVCallGuard(SafetyHookContext& ctx)
{
    if (OwnerIsLive(ctx.r15)) return;
    ctx.rip = g_base + kRvaOwnerNull;
}

constexpr uint32_t kRvaAbort = 0x28AFC14;
constexpr uint32_t kRvaOwnerTail = 0x15A13D3;
constexpr uint32_t kRvaOwnerTailEnd = 0x15A13DE;

SafetyHookInline g_hkPurecall;
SafetyHookMid g_hkOwnerTail;

static uintptr_t hk_Purecall()
{
    return 0;
}

void hk_OwnerTailGuard(SafetyHookContext& ctx)
{
    if (OwnerIsLive(ctx.r15)) return;
    __try { *reinterpret_cast<uintptr_t*>(ctx.rsi + 0x98) = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    ctx.rip = g_base + kRvaOwnerTailEnd;
}

extern "C" void NetPatch() {
    using namespace net::patch;

    GuardProcessListLinks(g_base);
    ClampMaterialPatchQueue(g_base); // AddMaterialToQueue assert on leaving a match (see the block above)
    g_hkMatRing = safetyhook::create_mid(g_base + kRvaMatRingLoad, hk_MatRingGuard);

    g_hkWidgetFirstChild = safetyhook::create_inline(g_base + 0x1340B10, hk_WidgetFirstChild); // untested. HUD2::Root is momentarily returning null causing the crash, this should fix it. ------ no longer needed since using NPCs as clients
    g_hkHudRootGuard = safetyhook::create_mid(g_base + 0x87E2AE, hk_HudRootGuard);

    // Increase level defs (pak68.txt)
    Utility::memory::StaticMemoryMapPatch(MemoryMapId::ALLOCATION_LEVEL_DEFS, 4608000ull + 4ull * 1024 * 1024);
    Utility::memory::StaticMemoryMapPatch(MemoryMapId::ALLOCATION_APP_CPU_UNACCOUNTED, 7557120ull - 4ull * 1024 * 1024);
    TO1_LOG("net-patch: Increased ALLOCATION_LEVEL_DEFS, ALLOCATION_APP_CPU_UNACCOUNTED.\n"); // for pak68.txt level defs


    g_hkRestoreStats = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x342130), reinterpret_cast<void*>(hk_RestoreStats));
    TO1_LOG("net-patch: Stats hook installed.\n"); // stops player from being applied data from save game

    // another crash fix
    g_hkDrawItem = safetyhook::create_inline(g_base + 0x129D890, hk_DrawItem);

    // block save files
    g_hkApplySaveData = safetyhook::create_inline(g_base + 0x120E7E0, hk_ApplySaveData);


    // gui2 crash fix
    g_hkOwnerVCall = safetyhook::create_mid(g_base + kRvaOwnerVCall, hk_OwnerVCallGuard);
    g_hkOwnerTail  = safetyhook::create_mid(g_base + kRvaOwnerTail, hk_OwnerTailGuard);
    g_hkPurecall = safetyhook::create_inline(g_base + kRvaPurecall, hk_Purecall);
    TO1_LOG("net-patch: gui2 crash fixes applied.\n");

    TO1_LOG("Successfully initialised tlou-ii-mp.\n");
}
