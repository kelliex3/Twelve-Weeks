// ported from tools/ext/padinject.c (the whole virtual-pad path, not just its activity stamp)

#include <winsock2.h>       // keep before windows.h (pulled in by engine/memory.hpp et al.)

#include "pad.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)

#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/memory.hpp"
#include "engine/script.hpp"
#include "sid/sid.hpp"

#include <windows.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace engine::pad {

        constexpr int kMaxEntries = 8;

        // Watch table from ext padinj_w. Defined at namespace scope so the public
        // Watch() API's forward declaration in pad.hpp resolves to the same type.
        struct CommandWatch {
            uint64_t                  sid = 0;
            char                      name[24]{};
            std::atomic<uint64_t>     mask{ 0 };
            uint32_t                  kind = 0;
            std::atomic<void*>        joypad{ nullptr };
            std::atomic<uint32_t>     readsTrue{ 0 };
            std::atomic<uint32_t>     readsAll{ 0 };
            uint64_t                  learnedMs = 0;
            std::atomic<uint64_t>     seenMs{ 0 };
            std::atomic<int>          learnLog{ 0 };
            uint32_t                  entryCount = 0;
            uint32_t                  entries[kMaxEntries][4]{};
        };

    namespace {

        constexpr uint32_t kExeTimestamp   = 0x687DF976u;   // tlou-ii.exe 1.6.10721
        constexpr uint32_t kExeSizeOfImage = 0x09A68000u;
        constexpr uint32_t kNoFocusRva     = 0x08E03F65u;   // "Enable Joypad w/o Focus"

        // Pinned function RVAs from the ext (1.6.10721). Preferred when their prologue
        // still matches; the prologue patterns are the fallback for a different build.
        constexpr uint32_t kUpdateRva = 0x1593350u;
        constexpr uint32_t kReaderRva = 0x158EB70u;
        constexpr uint8_t kUpdatePrologue[] = {
            0x40, 0x53, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00 };
        constexpr uint8_t kReaderPrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };

        constexpr size_t kPadSize      = 0x540;
        constexpr size_t kBtnOffset    = 0x528;
        constexpr float  kStick        = 0.3f;
        constexpr int    kMaxPads      = 8;
        constexpr int    kMaxWatches   = 48;
        constexpr int    kMaxSlots     = 8;
        constexpr uint64_t kMinPressMs = 20;
        constexpr uint64_t kPressGraceMs = 600;
        constexpr uint64_t kRawFrameWindow = 30;
        // Bit 63 is excluded from real input masks by Learn(), so it is a safe
        // "claimed but not armed yet" sentinel for a slot.
        constexpr uint64_t kSlotClaimed = 1ull << 63;

        // Pending injections from ext padinj_slot.
        struct InjectSlot {
            std::atomic<uint64_t> until{ 0 };
            std::atomic<uint64_t> hardUntil{ 0 };
            std::atomic<uint64_t> mask{ 0 };
            std::atomic<int>      phase{ 0 };
            std::atomic<uint32_t> applied{ 0 };
            int                   watch = -1;
            bool                  blockable = false;
        };

        struct PadTrack {
            const void* pad = nullptr;
            uint64_t    btn = 0;
        };

        CommandWatch     g_watches[kMaxWatches];
        InjectSlot       g_slots[kMaxSlots];
        PadTrack         g_tracks[kMaxPads];

        std::atomic<int>      g_state{ 0 };       // 0 untried, 1 installed, -1 failed
        uint8_t*              g_exe = nullptr;
        uint8_t*              g_updateFn = nullptr;
        uint8_t*              g_readerFn = nullptr;

        std::vector<safetyhook::MidHook> g_updateHooks;
        std::vector<safetyhook::MidHook> g_readerHooks;
        std::vector<safetyhook::MidHook> g_readerReturnHooks;

        CommandWatch*             g_meleeWatch = nullptr;
        std::atomic<CommandWatch*> g_dodgeWatches[3]{};

        std::atomic<uint64_t> g_actMs{ 0 };
        std::atomic<void*>    g_primaryJoypad{ nullptr };
        std::atomic<uint64_t> g_primaryJoypadMs{ 0 };
        std::atomic<uint64_t> g_activityUntil{ 0 };

        std::atomic<float>   g_stick[4]{};
        std::atomic<int>     g_stickSwap{ 0 };
        std::atomic<uint64_t> g_stickUntil{ 0 };

        std::atomic<uint64_t> g_blockMask{ 0 };
        std::atomic<uint64_t> g_blockUntil{ 0 };
        std::atomic<int>      g_rawBlock{ 0 };

        std::atomic<uint32_t> g_updatePrimary{ 0 };
        std::atomic<uint32_t> g_updateOther{ 0 };
        std::atomic<uint32_t> g_buttonApplied{ 0 };
        std::atomic<uint32_t> g_stickApplied{ 0 };
        std::atomic<uint32_t> g_blockApplied{ 0 };

        std::atomic<float>    g_rawLx{ 0.0f };
        std::atomic<float>    g_rawLy{ 0.0f };
        std::atomic<uint64_t> g_rawStickFrame{ 0 };

        std::atomic<int>      g_rawMelee{ 0 };
        std::atomic<uint32_t> g_meleePresses{ 0 };
        std::atomic<uint64_t> g_rawMeleeFrame{ 0 };
        std::atomic<uint64_t> g_rawMeleeMs{ 0 };

        std::atomic<uint64_t> g_dodgeMask{ 0 };
        std::atomic<int>      g_rawDodge{ 0 };
        std::atomic<uint64_t> g_rawDodgeFrame{ 0 };

        std::atomic<uint64_t> g_lastTickFrame{ 0 };
        std::atomic<uint64_t> g_lastHeartbeatMs{ 0 };
        std::atomic<uint32_t> g_lastHeartbeatCount{ 0 };

        uint8_t       g_noFocusPrev = 0;
        std::atomic<int> g_noFocusSet{ 0 };

        thread_local CommandWatch* t_pendingReadWatch = nullptr;

        uint32_t LoadU32(const uint8_t* p)
        {
            uint32_t v{};
            std::memcpy(&v, p, sizeof v);
            return v;
        }

        uint64_t LoadU64(const uint8_t* p)
        {
            uint64_t v{};
            std::memcpy(&v, p, sizeof v);
            return v;
        }

        bool Readable(const void* p, size_t n)
        {
            return p && !IsBadReadPtr(p, n);
        }

        bool Executable(DWORD protect)
        {
            return (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        }

        bool IsExecutablePtr(const void* p)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof mbi))
                return false;
            return mbi.State == MEM_COMMIT && Executable(mbi.Protect);
        }

        // Find every E8/E9 rel32 in executable image pages whose target is `fn`. This is
        // the pattern-scan replacement for the ext's five hardcoded update-site RVAs and
        // two reader-site RVAs.
        std::vector<uint8_t*> FindCallSites(uint8_t* fn)
        {
            std::vector<uint8_t*> sites;
            if (!g_exe || !fn)
                return sites;

            const auto size = Utility::memory::get_module_size(reinterpret_cast<HMODULE>(g_exe)).value_or(0);
            uint8_t* base = g_exe;
            uint8_t* end = base + size;

            for (uintptr_t addr = reinterpret_cast<uintptr_t>(base);
                 addr < reinterpret_cast<uintptr_t>(end);) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi))
                    break;
                const auto regionSize = mbi.RegionSize;
                if (!regionSize)
                    break;

                uint8_t* region = static_cast<uint8_t*>(mbi.BaseAddress);
                uint8_t* begin = region > base ? region : base;
                uint8_t* stop = region + regionSize < end ? region + regionSize : end;
                if (mbi.State == MEM_COMMIT && Executable(mbi.Protect) && stop > begin) {
                    for (uint8_t* p = begin; p + 5 <= stop; ++p) {
                        if (*p != 0xE8 && *p != 0xE9)
                            continue;
                        int32_t rel{};
                        std::memcpy(&rel, p + 1, sizeof rel);
                        const auto target = reinterpret_cast<uintptr_t>(p + 5) +
                                            static_cast<int64_t>(rel);
                        if (target == reinterpret_cast<uintptr_t>(fn))
                            sites.push_back(p);
                    }
                }
                addr = reinterpret_cast<uintptr_t>(region + regionSize);
            }

            std::sort(sites.begin(), sites.end());
            sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
            return sites;
        }

        void WatchDefaults()
        {
            g_meleeWatch = Watch("melee-attack");
            g_dodgeWatches[0].store(Watch("evade"));
            g_dodgeWatches[1].store(Watch("dodge"));
            g_dodgeWatches[2].store(Watch("evade-start"));

            static const char* const kNames[] = {
                "evade", "sprint", "aim", "fire", "crouch-press", "prone-toggle",
                "reload", "interact", "jump", "melee-attack-shiv",
                "weapon-cross-up", "weapon-cross-down", "weapon-cross-left",
                "weapon-cross-right", "fire-press", "fire-hold", "aim-hold"
            };
            for (const char* name : kNames)
                Watch(name);
        }

        // Learn the input bit of a command from the engine's own command table. Entries:
        // u32 kind, float threshold, u64 input mask. Press kinds win, then alternate press
        // kinds, then held kinds; the lowest real bit of the first match is the one we
        // inject. Bit 63 marks a mask that must not be injected.
        void Learn(CommandWatch* w, void* jp, uint32_t count, const uint8_t* entries)
        {
            if (!w || count < 1 || count > kMaxEntries || !Readable(entries, count * 16))
                return;

            uint64_t best = 0;
            uint32_t kind = 0;
            for (int pass = 0; pass < 3 && !best; ++pass) {
                for (uint32_t i = 0; i < count; ++i) {
                    const uint8_t* e = entries + static_cast<size_t>(i) * 16;
                    const uint32_t k = LoadU32(e);
                    const uint64_t m = LoadU64(e + 8);
                    const bool want = pass == 0 ? k == 8
                                    : pass == 1 ? k == 0x20
                                                : (k == 2 || k == 4);
                    if (!want || !m || (m & (1ull << 63)))
                        continue;
                    best = m & (~m + 1);   // lowest set bit
                    kind = k;
                    break;
                }
            }

            if (best == w->mask.load(std::memory_order_relaxed) &&
                w->joypad.load(std::memory_order_relaxed))
                return;   // keep the first joypad on A/B alternation

            w->entryCount = count;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* e = entries + static_cast<size_t>(i) * 16;
                for (int k = 0; k < 4; ++k)
                    w->entries[i][k] = LoadU32(e + static_cast<size_t>(k) * 4);
            }
            w->mask.store(best, std::memory_order_relaxed);
            w->kind = kind;
            w->joypad.store(jp, std::memory_order_relaxed);
            w->learnedMs = GetTickCount64();
            w->learnLog.store(1, std::memory_order_relaxed);
            if (best) {
                g_primaryJoypad.store(jp, std::memory_order_relaxed);
                g_primaryJoypadMs.store(w->learnedMs, std::memory_order_relaxed);
            }
        }

        bool SawInput(const void* pad)
        {
            if (!pad)
                return false;
            const uint64_t bm = LoadU64(static_cast<const uint8_t*>(pad) + kBtnOffset);
            const float* f = static_cast<const float*>(pad);
            const bool moved =
                std::fabs(f[0]) > kStick || std::fabs(f[1]) > kStick ||
                std::fabs(f[2]) > kStick || std::fabs(f[3]) > kStick;

            PadTrack* free = nullptr;
            for (PadTrack& t : g_tracks) {
                if (t.pad == pad) {
                    const bool edge = bm != t.btn;
                    t.btn = bm;
                    return edge || moved;
                }
                if (!t.pad && !free)
                    free = &t;
            }
            (free ? free : &g_tracks[0])->pad = pad;
            (free ? free : &g_tracks[0])->btn = bm;
            return moved;
        }

        uint64_t DodgeEntryMask(CommandWatch* w)
        {
            uint64_t out = 0;
            if (!w)
                return out;
            for (uint32_t i = 0; i < w->entryCount && i < kMaxEntries; ++i) {
                const uint32_t kind = w->entries[i][0];
                const uint64_t mask = static_cast<uint64_t>(w->entries[i][2]) |
                                      (static_cast<uint64_t>(w->entries[i][3]) << 32);
                if ((kind == 8 || kind == 0x20 || kind == 2 || kind == 4) &&
                    !(mask & (1ull << 63)))
                    out |= mask;
            }
            return out;
        }

        // Read the engine's real device state BEFORE any injection. Kept separate from
        // Apply so an injected press can never masquerade as player activity.
        void ObserveReal(void* jp, uint8_t* pad, uint64_t now)
        {
            if (!jp || jp != g_primaryJoypad.load(std::memory_order_relaxed) ||
                !Readable(pad, kPadSize))
                return;

            const uint64_t bm = LoadU64(pad + kBtnOffset);
            const float* f = reinterpret_cast<const float*>(pad);
            g_rawLx.store(f[0], std::memory_order_relaxed);
            g_rawLy.store(f[1], std::memory_order_relaxed);
            g_rawStickFrame.store(engine::frame::Count(), std::memory_order_relaxed);

            if (g_meleeWatch) {
                const uint64_t mm = g_meleeWatch->mask.load(std::memory_order_relaxed);
                const int on = mm && (bm & mm) != 0;
                if (on && !g_rawMelee.load(std::memory_order_relaxed)) {
                    g_meleePresses.fetch_add(1, std::memory_order_relaxed);
                    g_rawMeleeMs.store(now, std::memory_order_relaxed);
                }
                g_rawMelee.store(on, std::memory_order_relaxed);
                g_rawMeleeFrame.store(engine::frame::Count(), std::memory_order_relaxed);
            }

            uint64_t dm = 0;
            for (auto& slot : g_dodgeWatches)
                dm |= DodgeEntryMask(slot.load(std::memory_order_relaxed));
            if (dm != g_dodgeMask.load(std::memory_order_relaxed)) {
                g_dodgeMask.store(dm, std::memory_order_relaxed);
                static int logged = 0;
                if (logged++ < 12)
                    TO1_LOG("[padinj] dodge input bits %016llx (all keyboard/pad entries)\n",
                        static_cast<unsigned long long>(dm));
            }
            if (dm) {
                g_rawDodge.store((bm & dm) != 0, std::memory_order_relaxed);
                g_rawDodgeFrame.store(engine::frame::Count(), std::memory_order_relaxed);
            }
        }

        void Apply(void* jp, uint8_t* pad, uint64_t now)
        {
            if (!jp || jp != g_primaryJoypad.load(std::memory_order_relaxed) ||
                !Readable(pad, kPadSize))
                return;
            g_updatePrimary.fetch_add(1, std::memory_order_relaxed);

            uint64_t* btn = reinterpret_cast<uint64_t*>(pad + kBtnOffset);
            const uint64_t block = g_blockMask.load(std::memory_order_relaxed);
            const uint64_t blockUntil = g_blockUntil.load(std::memory_order_relaxed);
            if (block && now < blockUntil) {
                g_rawBlock.store((*btn & block) != 0, std::memory_order_relaxed);
                if (g_rawBlock.load(std::memory_order_relaxed)) {
                    *btn &= ~block;
                    g_blockApplied.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                g_rawBlock.store(0, std::memory_order_relaxed);
            }

            for (InjectSlot& s : g_slots) {
                uint64_t mask = s.mask.load(std::memory_order_relaxed);
                if (!mask || (mask & kSlotClaimed))
                    continue;   // free, or claimed by Press but not armed yet
                const uint64_t until = s.until.load(std::memory_order_relaxed);
                const uint64_t hard = s.hardUntil.load(std::memory_order_relaxed);
                if ((now >= until && s.applied.load(std::memory_order_relaxed) >= 2) ||
                    now >= hard) {
                    s.mask.store(0, std::memory_order_relaxed);
                    continue;
                }

                int phase = s.phase.load(std::memory_order_relaxed);
                if (phase == 0) {
                    phase = (*btn & mask) ? 1 : 2;
                    s.phase.store(phase, std::memory_order_relaxed);
                    if (phase == 1) {
                        *btn &= ~mask;   // force a fresh edge if the button was held
                        s.applied.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                } else if (phase == 1) {
                    s.phase.store(2, std::memory_order_relaxed);
                }
                *btn |= mask;
                s.applied.fetch_add(1, std::memory_order_relaxed);
                g_buttonApplied.fetch_add(1, std::memory_order_relaxed);
                if (s.blockable && block && now < blockUntil && (mask & block)) {
                    g_rawBlock.store(1, std::memory_order_relaxed);
                    *btn &= ~(mask & block);
                    g_blockApplied.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (now < g_stickUntil.load(std::memory_order_relaxed)) {
                float* f = reinterpret_cast<float*>(pad);
                float lx = g_stick[0].load(std::memory_order_relaxed);
                float ly = g_stick[1].load(std::memory_order_relaxed);
                float rx = g_stick[2].load(std::memory_order_relaxed);
                float ry = g_stick[3].load(std::memory_order_relaxed);
                if (g_stickSwap.load(std::memory_order_relaxed)) {
                    std::swap(lx, rx);
                    std::swap(ly, ry);
                }
                f[0] = f[4] = lx;
                f[1] = f[5] = ly;
                f[2] = f[6] = rx;
                f[3] = f[7] = ry;
                g_stickApplied.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void UpdateCallHook(SafetyHookContext& ctx)
        {
            void* jp = reinterpret_cast<void*>(ctx.rcx);
            uint8_t* pad = reinterpret_cast<uint8_t*>(ctx.rdx);
            if (SawInput(pad))
                g_actMs.store(GetTickCount64(), std::memory_order_relaxed);

            const uint64_t now = GetTickCount64();
            ObserveReal(jp, pad, now);
            if (jp != g_primaryJoypad.load(std::memory_order_relaxed))
                g_updateOther.fetch_add(1, std::memory_order_relaxed);

            if (now < g_activityUntil.load(std::memory_order_relaxed))
                Apply(jp, pad, now);
            else
                g_activityUntil.store(0, std::memory_order_relaxed);
        }

        void ReaderCallHook(SafetyHookContext& ctx)
        {
            const uint64_t sid = static_cast<uint64_t>(ctx.r8);
            const uint32_t count = static_cast<uint32_t>(ctx.r9);
            void* jp = reinterpret_cast<void*>(ctx.rcx);
            const uint8_t* entries =
                reinterpret_cast<const uint8_t*>(*reinterpret_cast<uintptr_t*>(ctx.rsp + 0x20));

            CommandWatch* found = nullptr;
            for (CommandWatch& w : g_watches) {
                if (w.sid == sid) { found = &w; break; }
            }
            t_pendingReadWatch = found;
            if (!found)
                return;

            found->readsAll.fetch_add(1, std::memory_order_relaxed);
            const uint64_t now = GetTickCount64();
            if (now - found->seenMs.load(std::memory_order_relaxed) > 1000 ||
                jp != found->joypad.load(std::memory_order_relaxed)) {
                found->seenMs.store(now, std::memory_order_relaxed);
                Learn(found, jp, count, entries);
            }
        }

        // The call hook runs before the reader; this one runs at the next instruction and
        // sees its return value in RAX. If the site was a tail jmp it never fires, which
        // only costs the diagnostic readsTrue counter.
        void ReaderReturnHook(SafetyHookContext& ctx)
        {
            CommandWatch* w = t_pendingReadWatch;
            if (!w)
                return;
            t_pendingReadWatch = nullptr;
            if (ctx.rax)
                w->readsTrue.fetch_add(1, std::memory_order_relaxed);
        }

        uint8_t* NoFocusAddress()
        {
            if (!g_exe)
                return nullptr;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != kExeTimestamp ||
                nt->OptionalHeader.SizeOfImage != kExeSizeOfImage) {
                static int logged = 0;
                if (!logged++) {
                    logged = 1;
                    TO1_LOG("[padinj] unsupported exe version - unfocused input unavailable\n");
                }
                return nullptr;
            }
            return g_exe + kNoFocusRva;
        }

    }

    CommandWatch* Watch(const char* name)
    {
        if (!name || !*name)
            return nullptr;
        const uint64_t sid = ToStringId64(name);
        for (CommandWatch& w : g_watches)
            if (w.sid == sid)
                return &w;
        for (CommandWatch& w : g_watches) {
            if (!w.sid) {
                std::snprintf(w.name, sizeof w.name, "%s", name);
                w.sid = sid;
                return &w;
            }
        }
        return nullptr;
    }

    bool Install()
    {
        if (g_state.load(std::memory_order_relaxed))
            return g_state.load(std::memory_order_relaxed) == 1;
        g_state.store(-1, std::memory_order_relaxed);

        if (!engine::flag::Absent("t2r-no-pad-inject.txt")) {
            TO1_LOG("[padinj] off (t2r-no-pad-inject.txt) - no virtual pad\n");
            return false;
        }

        g_exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_exe)
            return false;

        // Prefer the pinned RVA when its prologue still matches (exactly the ext's
        // version check), then fall back to a whole-image prologue scan.
        auto resolve = [](uint32_t rva, const uint8_t* pro, size_t proLen,
                          const Patterns::Pattern& pat) -> uint8_t* {
            uint8_t* pinned = g_exe + rva;
            if (Readable(pinned, proLen) && std::memcmp(pinned, pro, proLen) == 0 &&
                IsExecutablePtr(pinned))
                return pinned;
            Utility::memory::Pattern p(pat);
            return p.find(reinterpret_cast<HMODULE>(g_exe));
        };
        g_updateFn = resolve(kUpdateRva, kUpdatePrologue, sizeof kUpdatePrologue,
                             Patterns::Joypad_Update);
        g_readerFn = resolve(kReaderRva, kReaderPrologue, sizeof kReaderPrologue,
                             Patterns::Joypad_CommandReader);
        if (!g_updateFn || !g_readerFn || !IsExecutablePtr(g_updateFn) ||
            !IsExecutablePtr(g_readerFn)) {
            TO1_LOG("[padinj] Joypad::Update / command reader prologue not found - virtual pad off\n");
            return false;
        }

        const auto updateSites = FindCallSites(g_updateFn);
        const auto readerSites = FindCallSites(g_readerFn);
        TO1_LOG("[padinj] %s at %p, %zu call sites; reader at %p, %zu call sites\n",
            "Joypad::Update", static_cast<void*>(g_updateFn), updateSites.size(),
            static_cast<void*>(g_readerFn), readerSites.size());

        if (updateSites.size() < 5 || readerSites.empty()) {
            TO1_LOG("[padinj] not enough call sites (need 5 update + 1 reader) - virtual pad off\n");
            return false;
        }

        WatchDefaults();

        int readerHooks = 0;
        for (uint8_t* site : readerSites) {
            auto hook = safetyhook::create_mid(site, ReaderCallHook);
            if (!hook) {
                TO1_LOG("[padinj] reader call-site hook failed at %p\n", static_cast<void*>(site));
                continue;
            }
            g_readerHooks.push_back(std::move(hook));
            ++readerHooks;

            auto ret = safetyhook::create_mid(site + 5, ReaderReturnHook);
            if (ret)
                g_readerReturnHooks.push_back(std::move(ret));
        }

        int updateHooks = 0;
        for (uint8_t* site : updateSites) {
            auto hook = safetyhook::create_mid(site, UpdateCallHook);
            if (!hook) {
                TO1_LOG("[padinj] Joypad::Update call-site hook failed at %p\n", static_cast<void*>(site));
                continue;
            }
            g_updateHooks.push_back(std::move(hook));
            ++updateHooks;
        }

        if (updateHooks < 5 || readerHooks < 1) {
            TO1_LOG("[padinj] incomplete install (%d/%zu update, %d/%zu reader) - virtual pad off\n",
                updateHooks, updateSites.size(), readerHooks, readerSites.size());
            g_updateHooks.clear();
            g_readerHooks.clear();
            g_readerReturnHooks.clear();
            return false;
        }

        g_state.store(1, std::memory_order_relaxed);
        TO1_LOG("[padinj] installed - %d Joypad::Update sites, %d reader sites (+%zu return probes)\n",
            updateHooks, readerHooks, g_readerReturnHooks.size());
        return true;
    }

    bool Ready()
    {
        Install();
        return g_state.load(std::memory_order_relaxed) == 1;
    }

    void Tick()
    {
        Install();
        const uint64_t frame = engine::frame::Count();
        if (g_lastTickFrame.load(std::memory_order_relaxed) == frame)
            return;
        g_lastTickFrame.store(frame, std::memory_order_relaxed);

        for (CommandWatch& w : g_watches) {
            if (!w.learnLog.exchange(0, std::memory_order_relaxed))
                continue;
            char buf[400];
            int q = std::snprintf(buf, sizeof buf, "[padinj] learned '%s' on joypad %p: %u entr%s",
                w.name, w.joypad.load(std::memory_order_relaxed), w.entryCount,
                w.entryCount == 1 ? "y" : "ies");
            if (q < 0) q = 0;
            if (q > static_cast<int>(sizeof buf) - 1) q = static_cast<int>(sizeof buf) - 1;
            for (uint32_t i = 0; i < w.entryCount && q < static_cast<int>(sizeof buf) - 60; ++i) {
                const float threshold = std::bit_cast<float>(w.entries[i][1]);
                q += std::snprintf(buf + q, sizeof buf - static_cast<size_t>(q),
                    " [kind %u thr %.2f mask %08x%08x]", w.entries[i][0], threshold,
                    w.entries[i][3], w.entries[i][2]);
                if (q < 0) q = 0;
            }
            std::snprintf(buf + q, sizeof buf - static_cast<size_t>(q),
                " -> injects input bit %016llx",
                static_cast<unsigned long long>(w.mask.load(std::memory_order_relaxed)));
            static int logged = 0;
            if (logged++ < 20)
                TO1_LOG("%s\n", buf);
        }

        const uint64_t now = GetTickCount64();
        if (g_activityUntil.load(std::memory_order_relaxed) &&
            now - g_lastHeartbeatMs.load(std::memory_order_relaxed) >= 1500) {
            g_lastHeartbeatMs.store(now, std::memory_order_relaxed);
            const uint32_t primary = g_updatePrimary.load(std::memory_order_relaxed);
            static int logged = 0;
            if (logged++ < 40) {
                TO1_LOG("[padinj] heartbeat - primary Joypad::Update %u (+%u / 1.5 s), other %u, buttons %u, sticks %u\n",
                    primary, primary - g_lastHeartbeatCount.load(std::memory_order_relaxed),
                    g_updateOther.load(std::memory_order_relaxed),
                    g_buttonApplied.load(std::memory_order_relaxed),
                    g_stickApplied.load(std::memory_order_relaxed));
            }
            g_lastHeartbeatCount.store(primary, std::memory_order_relaxed);
        }
    }

    uint64_t ActivityMs()
    {
        return g_actMs.load(std::memory_order_relaxed);
    }

    uint32_t Applied()
    {
        return g_buttonApplied.load(std::memory_order_relaxed);
    }

    uint32_t StickApplied()
    {
        return g_stickApplied.load(std::memory_order_relaxed);
    }

    bool NofocusOn()
    {
        return g_noFocusSet.load(std::memory_order_relaxed) != 0;
    }

    void SetNofocus(bool on)
    {
        uint8_t* p = NoFocusAddress();
        if (!p || IsBadWritePtr(p, 1))
            return;
        const int set = g_noFocusSet.load(std::memory_order_relaxed);
        if (on && !set) {
            g_noFocusPrev = *p;
            DWORD old{};
            if (!VirtualProtect(p, 1, PAGE_READWRITE, &old))
                return;
            *p = 1;
            VirtualProtect(p, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 1);
            g_noFocusSet.store(1, std::memory_order_relaxed);
            TO1_LOG("[padinj] unfocused-window input ON (rva 0x%X, was %u)\n",
                kNoFocusRva, g_noFocusPrev);
        } else if (!on && set) {
            DWORD old{};
            if (!VirtualProtect(p, 1, PAGE_READWRITE, &old))
                return;
            *p = g_noFocusPrev;
            VirtualProtect(p, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 1);
            g_noFocusSet.store(0, std::memory_order_relaxed);
            TO1_LOG("[padinj] unfocused-window input restored\n");
        }
    }

    void ProbeRead(const char* name)
    {
        if (!name || !*name)
            return;
        DC::Call<"joypad-command-active?">(ToStringId64(name));
    }

    uint32_t ReadsTrue(const char* name)
    {
        CommandWatch* w = Watch(name);
        return w ? w->readsTrue.load(std::memory_order_relaxed) : 0;
    }

    namespace {
        bool QueuePress(const char* name, int ms, bool blockable);
    }

    bool Press(const char* name, int ms)
    {
        return QueuePress(name, ms, false);
    }

    bool BotPress(const char* name, int ms)
    {
        return QueuePress(name, ms, true);
    }

    namespace {
    bool QueuePress(const char* name, int ms, bool blockable)
    {
        if (g_state.load(std::memory_order_relaxed) != 1)
            return false;
        CommandWatch* w = Watch(name);
        if (!w)
            return false;
        if (!w->mask.load(std::memory_order_relaxed) ||
            !g_primaryJoypad.load(std::memory_order_relaxed)) {
            ProbeRead(name);
            if (!w->mask.load(std::memory_order_relaxed))
                return false;
        }

        const uint64_t now = GetTickCount64();
        const uint64_t until = now + static_cast<uint64_t>(ms < 20 ? kMinPressMs : ms);
        const uint64_t hard = until + kPressGraceMs;
        for (InjectSlot& s : g_slots) {
            // Claim a free slot, but arm `mask` LAST (after every other field is set),
            // exactly like the ext: the Apply hook runs on the game thread and must
            // never see a non-zero mask paired with stale timing fields.
            uint64_t expected = 0;
            if (!s.mask.compare_exchange_strong(expected, kSlotClaimed))
                continue;
            s.phase.store(0, std::memory_order_relaxed);
            s.applied.store(0, std::memory_order_relaxed);
            s.watch = static_cast<int>(w - g_watches);
            s.blockable = blockable;
            s.until.store(until, std::memory_order_relaxed);
            s.hardUntil.store(hard, std::memory_order_relaxed);
            w->readsTrue.store(0, std::memory_order_relaxed);
            w->readsAll.store(0, std::memory_order_relaxed);
            if (hard > g_activityUntil.load(std::memory_order_relaxed))
                g_activityUntil.store(hard, std::memory_order_relaxed);
            s.mask.store(w->mask.load(std::memory_order_relaxed),
                         std::memory_order_release);   // armed last
            return true;
        }
        return false;
    }
    }

    void Stick(float lx, float ly, float rx, float ry, int ms, bool swap)
    {
        if (g_state.load(std::memory_order_relaxed) != 1)
            return;
        g_stick[0].store(lx, std::memory_order_relaxed);
        g_stick[1].store(ly, std::memory_order_relaxed);
        g_stick[2].store(rx, std::memory_order_relaxed);
        g_stick[3].store(ry, std::memory_order_relaxed);
        g_stickSwap.store(swap ? 1 : 0, std::memory_order_relaxed);
        const uint64_t until = GetTickCount64() +
            static_cast<uint64_t>(ms < 20 ? kMinPressMs : ms);
        g_stickUntil.store(until, std::memory_order_relaxed);
        if (until > g_activityUntil.load(std::memory_order_relaxed))
            g_activityUntil.store(until, std::memory_order_relaxed);
    }

    void StickOff()
    {
        g_stickUntil.store(0, std::memory_order_relaxed);
    }

    bool LeftStick(float& x, float& y)
    {
        if (g_state.load(std::memory_order_relaxed) != 1)
            return false;
        const uint64_t seen = g_rawStickFrame.load(std::memory_order_relaxed);
        if (!seen || engine::frame::Count() - seen > kRawFrameWindow)
            return false;
        x = g_rawLx.load(std::memory_order_relaxed);
        y = g_rawLy.load(std::memory_order_relaxed);
        return true;
    }

    bool SetBlock(const char* name, uint64_t untilMs)
    {
        if (g_state.load(std::memory_order_relaxed) != 1)
            return false;
        if (!untilMs) {
            g_blockUntil.store(0, std::memory_order_relaxed);
            g_rawBlock.store(0, std::memory_order_relaxed);
            return true;
        }
        CommandWatch* w = Watch(name);
        if (!w || !w->mask.load(std::memory_order_relaxed) ||
            !g_primaryJoypad.load(std::memory_order_relaxed))
            return false;
        g_blockMask.store(w->mask.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_blockUntil.store(untilMs, std::memory_order_relaxed);
        if (untilMs > g_activityUntil.load(std::memory_order_relaxed))
            g_activityUntil.store(untilMs, std::memory_order_relaxed);
        return true;
    }

    bool BlockedRaw()
    {
        const uint64_t until = g_blockUntil.load(std::memory_order_relaxed);
        if (!until || GetTickCount64() >= until)
            return false;
        if (g_meleeWatch && g_blockMask.load(std::memory_order_relaxed) ==
                            g_meleeWatch->mask.load(std::memory_order_relaxed))
            return MeleeHeld();
        return g_rawBlock.load(std::memory_order_relaxed) != 0;
    }

    bool MeleeHeld()
    {
        const uint64_t seen = g_rawMeleeFrame.load(std::memory_order_relaxed);
        return g_state.load(std::memory_order_relaxed) == 1 &&
               g_rawMelee.load(std::memory_order_relaxed) &&
               seen && engine::frame::Count() - seen <= kRawFrameWindow;
    }

    uint32_t MeleePresses()
    {
        return g_meleePresses.load(std::memory_order_relaxed);
    }

    bool DodgeHeld()
    {
        static int old = -1;
        static uint64_t checked = 0;
        const uint64_t now = GetTickCount64();
        if (old < 0 || now - checked > 2000) {
            checked = now;
            const int v = !engine::flag::Absent("t2r-dodge-pad-ps53-old.txt") ? 1 : 0;
            if (v != old) {
                old = v;
                TO1_LOG("[padinj] raw dodge input %s\n",
                    v ? "off (t2r-dodge-pad-ps53-old.txt): engine commands only"
                      : "on: every evade / dodge / evade-start input bit");
            }
        }
        const uint64_t seen = g_rawDodgeFrame.load(std::memory_order_relaxed);
        return !old && g_state.load(std::memory_order_relaxed) == 1 &&
               g_rawDodge.load(std::memory_order_relaxed) && seen &&
               engine::frame::Count() - seen <= kRawFrameWindow;
    }

    int MeleeEdge(MeleeEdgeState& state, uint64_t now, bool keyboardDown,
                  bool botDown, const char** src)
    {
        const uint32_t seq = MeleePresses();
        const bool pad = state.init && seq != state.seq;
        const bool keyEdge = keyboardDown && !state.keyboard;
        const bool botEdge = botDown && !state.bot;
        state.seq = seq;
        state.keyboard = keyboardDown;
        state.bot = botDown;
        state.init = true;
        if (!pad && !keyEdge && !botEdge)
            return 0;
        if (src)
            *src = pad ? (keyEdge ? "pad bit + keyboard F" : "pad / engine bit")
                       : keyEdge ? "keyboard F / engine melee command" : "bot";
        if (state.lastMs && now - state.lastMs < 90)
            return 2;
        state.lastMs = now;
        return 1;
    }

}
