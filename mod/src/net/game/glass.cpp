// ported from tools/ext/glass.c

#include <winsock2.h>
#include <windows.h>

#include "glass.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/ptr.hpp"
#include "engine/script.hpp"
#include "net-common.hpp"
#include "net-gui2.hpp"
#include "net-player.hpp"
#include "net-protocol.hpp"
#include "net-session.hpp"

#include <cstdint>
#include <cstring>

namespace net::glass {

    namespace {

        // ---- wire (ext MSG_GLASS) ----------------------------------------------
        constexpr uint8_t  kMsgGlass = 0x7A;                  // pinned to the ext's byte
        constexpr uint64_t kBreak    = 0x2ccc09d462355d58ULL; // the engine's break event hash

        // ---- registry (ext GL_REG_MAX / GL_HASH / g_gl_fast) -------------------
        constexpr int kRegMax   = 2048;   // panes the hook has seen
        constexpr int kHashSize = 8192;   // open-addressing index over g_reg (by object address)
        constexpr int kFastN    = 64;     // direct-mapped cache: the hot path of the hook
        constexpr int kJoinMax  = 20;     // panes per joiner message (ext: 20 entries a message)

        // ---- exe RVAs (tlou-ii.exe 1.6.0 PC) -----------------------------------
        constexpr uint32_t kRvaHandleEvent = 0x84a820;   // SimpleBreakable / BigBreakable HandleEvent
        constexpr uint32_t kRvaEventSender = 0x15734d0;  // the engine's own event sender
        constexpr uint32_t kRvaFindPane    = 0x1b0a970;  // pane lookup by obj+0xc0 id

        using fn_find   = uint8_t* (__fastcall*)(uint64_t id);
        using fn_sendev = void (__fastcall*)(uint8_t* out, uint8_t* sender, uint64_t sid,
                                             uint8_t* target, uint8_t* a0, uint8_t* a1,
                                             uint8_t* a2, uint8_t* a3);

        // A pane the hook has seen. `done` is a bit mask of pieces 0..255 that got their
        // break event here; `piece` is the last one (the joiner fallback when no bit is set).
        struct Reg {
            uint8_t* obj = nullptr;
            uint64_t id = 0;
            uint64_t vt = 0;
            uint64_t done[4] = {};
            uint32_t piece = 0xffffffffu;
        };
        Reg     g_reg[kRegMax];
        int16_t g_hash[kHashSize] = {};   // index + 1 into g_reg, 0 = empty
        int     g_nreg = 0;

        struct Fast {
            uint8_t* obj = nullptr;
            uint64_t id = 0;
            uint64_t vt = 0;
        };
        Fast g_fast[kFastN];

        uint8_t  g_was[kMaxNetPlayers] = {};   // last frame's active state, per slot (ext was[])
        int      g_replay = 0;                 // delivering a peer's break: the hook must not echo it
        bool     g_on = false;                 // hook installed and not opted out
        bool     g_tried = false;              // install attempted (never re-checked)

        fn_find   g_find = nullptr;
        fn_sendev g_sendEv = nullptr;
        SafetyHookInline g_hkHandleEvent;

        uint32_t g_nsent = 0, g_nrecv = 0, g_npurge = 0;

        bool LogCap(int& c, int n) { return c++ < n; }

        inline bool Readable(const void* p, size_t n)
        {
            return ::engine::ptr::Readable(reinterpret_cast<uintptr_t>(p), n);
        }

        // ---- registry (ext GlHidx / GlDone* / GlLive) --------------------------
        unsigned Hidx(const uint8_t* obj)
        {
            return static_cast<unsigned>(
                (((reinterpret_cast<uintptr_t>(obj) >> 4) * 2654435761u) >> 7) & (kHashSize - 1));
        }

        void DoneClear(int i) { memset(g_reg[i].done, 0, sizeof g_reg[i].done); }

        bool DoneHas(int i, uint32_t piece)
        {
            return piece < 256 && ((g_reg[i].done[piece >> 6] >> (piece & 63)) & 1) != 0;
        }

        void DoneSet(int i, uint32_t piece)
        {
            if (piece < 256)
                g_reg[i].done[piece >> 6] |= 1ULL << (piece & 63);
        }

        bool Live(const uint8_t* obj, uint64_t id)
        {
            return obj && Readable(obj, 0x700)
                && *reinterpret_cast<const uint64_t*>(obj + 0xc0) == id
                && *reinterpret_cast<const uint64_t*>(obj) != 0;
        }

        // The old registry was a 256-entry table that was never cleared; the level has far
        // more breakables than that, so after a few matches it was full and the hook sent
        // NOTHING any more. Now it is hashed by object address, 2048 entries, and when it
        // fills up only the live BROKEN panes (the joiner list needs them) are kept --
        // unbroken ones are re-learned from their next event.
        void RegPurge()
        {
            int k = 0;
            for (int i = 0; i < g_nreg; i++) {
                uint8_t* o = g_reg[i].obj;
                if (Live(o, g_reg[i].id) && *reinterpret_cast<uint64_t*>(o) == g_reg[i].vt
                    && (o[0x640] & 1)) {
                    if (k != i)
                        g_reg[k] = g_reg[i];
                    k++;
                }
            }
            TO1_LOG("glass: registry full (%d panes) - kept the %d live broken ones\n", g_nreg, k);
            g_nreg = k;
            g_npurge++;
            memset(g_hash, 0, sizeof g_hash);
            for (int i = 0; i < k; i++) {
                unsigned h = Hidx(g_reg[i].obj);
                while (g_hash[h])
                    h = (h + 1) & (kHashSize - 1);
                g_hash[h] = static_cast<int16_t>(i + 1);
            }
        }

        // Registry entry of a pane object (created on first sight); a recycled address
        // (other id / vtable) starts fresh.
        int RegGet(uint8_t* obj, bool create)
        {
            const uint64_t id = *reinterpret_cast<uint64_t*>(obj + 0xc0);
            const uint64_t vt = *reinterpret_cast<uint64_t*>(obj);
            unsigned h = Hidx(obj);
            while (g_hash[h]) {
                const int i = g_hash[h] - 1;
                if (g_reg[i].obj == obj) {
                    if (g_reg[i].id != id || g_reg[i].vt != vt) {
                        g_reg[i].id = id;
                        g_reg[i].vt = vt;
                        DoneClear(i);
                        g_reg[i].piece = 0xffffffffu;
                    }
                    return i;
                }
                h = (h + 1) & (kHashSize - 1);
            }
            if (!create)
                return -1;
            if (g_nreg >= kRegMax) {
                // Full: purge keeps the live broken panes (the joiner list needs them).
                // If that frees nothing (every slot is a live broken pane) there is no room
                // -- the caller still sends the break, it just cannot track the piece.
                const int before = g_nreg;
                RegPurge();
                if (g_nreg == before)
                    return -1;
                return RegGet(obj, create);
            }
            const int i = g_nreg++;
            g_reg[i].obj = obj;
            g_reg[i].id = id;
            g_reg[i].vt = vt;
            DoneClear(i);
            g_reg[i].piece = 0xffffffffu;
            g_hash[h] = static_cast<int16_t>(i + 1);
            return i;
        }

        // ---- event arg tags (ext GlArgInt / GlTagScalar / GlTagInt) ------------
        // An event arg as the engine's own reader (0x1414cf020) reads it: tag 1 bool,
        // 2-5 / 10 u32, 6 float.
        uint32_t ArgInt(const uint8_t* a)
        {
            const uint16_t tag = *reinterpret_cast<const uint16_t*>(a + 0xc);
            if (tag == 1)
                return a[0] != 0;
            if (tag == 6) {
                float f;
                memcpy(&f, a, 4);
                return static_cast<uint32_t>(static_cast<int32_t>(f));
            }
            return *reinterpret_cast<const uint32_t*>(a);
        }

        bool TagScalar(uint16_t t)
        {
            return t == 1 || t == 2 || t == 3 || t == 6 || t == 10;
        }

        bool TagInt(uint16_t t)
        {
            return t == 2 || t == 3 || t == 4 || t == 5 || t == 10;
        }

        // ---- pane lookup -------------------------------------------------------
        uint8_t* FindPane(uint64_t id, int* viaReg)
        {
            *viaReg = 0;
            uint8_t* o = g_find ? g_find(id) : nullptr;
            if (Live(o, id))
                return o;
            for (int i = 0; i < g_nreg; i++)
                if (g_reg[i].id == id && Live(g_reg[i].obj, id)
                    && *reinterpret_cast<uint64_t*>(g_reg[i].obj) == g_reg[i].vt) {
                    *viaReg = 1;
                    return g_reg[i].obj;
                }
            return nullptr;
        }

        // Replay a peer's break on the pane through the engine's event sender (target handle
        // = {obj+0x80 slot ptr, obj+0x94 id}, as the coat reboot does).
        bool Replay(uint8_t* obj, uint32_t piece, const uint8_t* args, int nargs)
        {
            if (!g_sendEv || !Readable(obj + 0x80, 8) || !Readable(obj + 0x94, 4))
                return false;
            uint8_t* slot = *reinterpret_cast<uint8_t**>(obj + 0x80);
            if (!slot || !Readable(slot, 16) || *reinterpret_cast<uint8_t**>(slot + 8) != obj)
                return false;

            alignas(32) uint8_t out[0x100] = {};
            alignas(16) uint8_t snd[16] = {};
            alignas(16) uint8_t tgt[16] = {};
            alignas(16) uint8_t al[4][16] = {};
            memcpy(tgt, &slot, 8);
            memcpy(tgt + 8, obj + 0x94, 4);

            for (int i = 0; i < nargs && i < 3; i++) {
                const uint8_t* a = args + 16 * i;
                const uint16_t tag = *reinterpret_cast<const uint16_t*>(a + 0xc);
                if (i == 2) {           // the piece index: the sender's tag when it is an int kind, else a plain int
                    const uint16_t t = TagInt(tag) ? tag : 2;
                    const uint64_t v = piece;
                    memcpy(al[i], &v, 8);
                    memcpy(al[i] + 0xc, &t, 2);
                } else if (TagScalar(tag)) {
                    memcpy(al[i], a, 16);       // scalars travel; anything pointer-like becomes int 0
                } else {
                    const uint16_t t = 2;
                    memcpy(al[i] + 0xc, &t, 2);
                }
            }
            for (int i = nargs < 0 ? 0 : nargs; i < 3; i++) {
                const uint16_t t = 2;           // missing args: int 0 (the piece goes in arg 2 below)
                memcpy(al[i] + 0xc, &t, 2);
            }
            if (nargs < 3) {
                const uint64_t v = piece;
                memcpy(al[2], &v, 8);
            }

            g_replay = 1;
            g_sendEv(out, snd, kBreak, tgt, al[0], al[1], al[2], al[3]);
            g_replay = 0;
            return true;
        }

        // Apply one peer break / one joiner entry on the pane.
        void Apply(uint8_t slot, uint64_t id, uint32_t piece, const uint8_t* args, int nargs,
                   const char* what)
        {
            int via = 0;
            uint8_t* obj = FindPane(id, &via);
            if (!obj) {
                static int n;
                if (LogCap(n, 30))
                    TO1_LOG("glass: slot %u %s pane %016llx piece %d - NOT FOUND here (level object missing)\n",
                        slot, what, static_cast<unsigned long long>(id), static_cast<int>(static_cast<int32_t>(piece)));
                return;
            }
            const int ri = RegGet(obj, true);
            if (ri >= 0 && !(obj[0x640] & 1))
                DoneClear(ri);      // a reset pane (new round): forget old pieces
            if (ri >= 0 && DoneHas(ri, piece)) {
                static int n;
                if (LogCap(n, 60))
                    TO1_LOG("glass: slot %u %s pane %016llx piece %u - already broken here (skipped)\n",
                        slot, what, static_cast<unsigned long long>(id), piece);
                return;
            }
            if (ri < 0 && (obj[0x640] & 1)) {
                TO1_LOG("glass: slot %u %s pane %016llx - already broken here (skipped)\n",
                    slot, what, static_cast<unsigned long long>(id));
                return;
            }

            const bool ok = Replay(obj, piece, args, nargs);
            if (ri >= 0) {
                DoneSet(ri, piece);
                g_reg[ri].piece = piece;
            }
            g_nrecv++;
            TO1_LOG("glass: slot %u %s pane %016llx piece %d - %s (%p%s) broken flag now %d\n",
                slot, what, static_cast<unsigned long long>(id), static_cast<int>(static_cast<int32_t>(piece)),
                ok ? "replayed through the engine event path" : "REPLAY FAILED",
                static_cast<void*>(obj), via ? ", found via registry" : "", obj[0x640] & 1);
        }

        void Pre(uint8_t* obj, uint8_t* ev);

        void __fastcall HkHandleEvent(uint8_t* obj, uint8_t* ev)
        {
            // perf: this hook sits on the base handler of EVERY event sent to any breakable,
            // so the common case -- a pane we already know, an event that is not the break --
            // must cost a few loads: the 64-entry direct-mapped cache skips the two
            // readability probes and the registry scan.
            const unsigned fi = static_cast<unsigned>((reinterpret_cast<uintptr_t>(obj) >> 5) & (kFastN - 1));
            if (!(g_on && obj && ev && g_fast[fi].obj == obj
                  && *reinterpret_cast<uint64_t*>(ev) != kBreak
                  && *reinterpret_cast<uint64_t*>(obj + 0xc0) == g_fast[fi].id
                  && *reinterpret_cast<uint64_t*>(obj) == g_fast[fi].vt))
                Pre(obj, ev);
            g_hkHandleEvent.fastcall<void>(obj, ev);
        }

        void Pre(uint8_t* obj, uint8_t* ev)
        {
            if (!(g_on && obj && ev && Readable(ev, 0x130) && Readable(obj, 0x700)))
                return;

            if (*reinterpret_cast<uint64_t*>(ev) == kBreak) {
                const int ri = RegGet(obj, true);
                const uint64_t id = *reinterpret_cast<uint64_t*>(obj + 0xc0);
                const int16_t nargs = *reinterpret_cast<int16_t*>(ev + 0x128);
                const uint32_t piece = nargs >= 3 ? ArgInt(ev + 0x48) : 0xffffffffu;
                if (ri >= 0 && !(obj[0x640] & 1))
                    DoneClear(ri);      // a reset pane (new round): forget old pieces

                if (g_replay) {
                    static int n;
                    if (LogCap(n, 40))
                        TO1_LOG("glass: pane %016llx piece %u - the replayed peer break reached the pane's handler (not echoed)\n",
                            static_cast<unsigned long long>(id), piece);
                } else if (net::gui2::InMatch()) {
                    // The level breaks ~12-25 panes itself while it loads -- the same on every
                    // client, nothing to send. A pane without a registry slot is still sent
                    // (ri < 0 only if the table could not take it).
                    if (ri >= 0 && DoneHas(ri, piece)) {
                        // this piece's break was already handled here (a repeat of the physics event)
                    } else {
                        if (ri >= 0) {
                            DoneSet(ri, piece);
                            g_reg[ri].piece = piece;
                        }
                        g_nsent++;
                        static int nlog;
                        if (LogCap(nlog, 300)) {
                            TO1_LOG("glass: our break - pane %016llx %p piece %d args %d tags %u/%u/%u\n",
                                static_cast<unsigned long long>(id), static_cast<void*>(obj),
                                static_cast<int>(static_cast<int32_t>(piece)), static_cast<int>(nargs),
                                nargs > 0 ? *reinterpret_cast<uint16_t*>(ev + 0x28 + 0xc) : 0,
                                nargs > 1 ? *reinterpret_cast<uint16_t*>(ev + 0x38 + 0xc) : 0,
                                nargs > 2 ? *reinterpret_cast<uint16_t*>(ev + 0x48 + 0xc) : 0);
                        }
                        uint8_t m[1 + 8 + 4 + 1 + 48];
                        const int n3 = nargs > 3 ? 3 : nargs < 0 ? 0 : nargs;
                        m[0] = 1;
                        memcpy(m + 1, &id, 8);
                        memcpy(m + 9, &piece, 4);
                        m[13] = static_cast<uint8_t>(n3);
                        for (int i = 0; i < n3; i++)
                            memcpy(m + 14 + 16 * i, ev + 0x28 + 0x10 * i, 16);
                        net::session::SendEventRaw(kMsgGlass, m, static_cast<uint8_t>(14 + 16 * n3));
                    }
                }
            } else {
                RegGet(obj, true);      // learn the pane from any event it gets (the receiver fallback)
            }

            const int rf = RegGet(obj, false);
            if (rf >= 0) {
                const unsigned fi = static_cast<unsigned>((reinterpret_cast<uintptr_t>(obj) >> 5) & (kFastN - 1));
                g_fast[fi].id = g_reg[rf].id;
                g_fast[fi].vt = g_reg[rf].vt;
                g_fast[fi].obj = obj;
            }
        }

        bool Install()
        {
            if (g_tried)
                return g_on;
            g_tried = true;

            if (!::engine::flag::Absent("t2r-no-glass-sync.txt")) {
                TO1_LOG("glass: sync OFF (t2r-no-glass-sync.txt)\n");
                return false;
            }

            const uint8_t* exe = reinterpret_cast<const uint8_t*>(g_base);
            // mov [rsp+18h],rbx; push rbp,rsi,rdi,r12,r13,r14,r15 (16 bytes, no rip-relative operand)
            static const uint8_t kEvSig[16] = {
                0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
                0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
            };
            // SendEvent: mov [rsp+10h],rbx; mov [rsp+18h],rsi; push rdi; sub rsp,190h
            static const uint8_t kSendSig[16] = {
                0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                0x48, 0x81, 0xec, 0x90, 0x01
            };

            if (memcmp(exe + kRvaEventSender, kSendSig, 16) != 0) {
                TO1_LOG("glass: sync NOT installed - exe 0x%x (event sender) differs in this build\n",
                    kRvaEventSender);
                return false;
            }
            if (memcmp(exe + kRvaHandleEvent, kEvSig, 16) != 0) {
                TO1_LOG("glass: sync NOT installed - exe 0x%x (HandleEvent) differs in this build\n",
                    kRvaHandleEvent);
                return false;
            }

            g_sendEv = GameFn<fn_sendev>(kRvaEventSender);
            g_find = GameFn<fn_find>(kRvaFindPane);
            g_hkHandleEvent = safetyhook::create_inline(
                reinterpret_cast<void*>(g_base + kRvaHandleEvent),
                reinterpret_cast<void*>(HkHandleEvent));
            g_on = static_cast<bool>(g_hkHandleEvent);
            TO1_LOG("glass: sync %s (break event 0x%llx at exe 0x%x, replay through 0x%x, pane lookup 0x%x by obj+0xc0, MSG_GLASS 0x%02X)\n",
                g_on ? "on" : "NOT installed", static_cast<unsigned long long>(kBreak),
                kRvaHandleEvent, kRvaEventSender, kRvaFindPane, kMsgGlass);
            return g_on;
        }

    }

    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        if (!g_on || senderSlot >= kMaxNetPlayers || !data || size < 2)
            return;

        if (data[0] == 1 && size >= 14) {
            const int n = data[13] > 3 ? 3 : data[13];
            if (size < 14 + 16 * n)
                return;
            uint32_t piece;
            memcpy(&piece, data + 9, 4);
            if (!(piece < 4096u || piece == 0xffffffffu)) {
                // TODO(trust): the ext gates this with PtDeny(PTK_glass_piece) -- log-only
                // there too, so the message is accepted here as well.
                TO1_LOG("glass: slot %u piece %u out of range (trust gate not ported - accepted)\n",
                    senderSlot, piece);
            }
            uint64_t id;
            memcpy(&id, data + 1, 8);
            Apply(senderSlot, id, piece, data + 14, n, "break");
        } else if (data[0] == 2 && size >= 2) {
            const int n = data[1];
            for (int k = 0; k < n && size >= 2 + 12 * (k + 1); k++) {
                uint32_t piece;
                memcpy(&piece, data + 10 + 12 * k, 4);
                if (!(piece < 4096u || piece == 0xffffffffu)) {
                    TO1_LOG("glass: slot %u piece %u out of range (trust gate not ported - accepted)\n",
                        senderSlot, piece);
                }
                uint64_t id;
                memcpy(&id, data + 2 + 12 * k, 8);
                Apply(senderSlot, id, piece, nullptr, 0, "list");
            }
        }
    }

    void Tick()
    {
        if (!Install())
            return;

        static uint64_t nextStats = 0;
        const uint64_t frame = ::engine::frame::Count();
        if (frame >= nextStats) {
            nextStats = frame + 3600;
            if (g_nsent || g_nrecv || g_npurge)
                TO1_LOG("glass: stats - sent %u breaks, replayed %u from peers, registry %d panes, %u purges\n",
                    g_nsent, g_nrecv, g_nreg, g_npurge);
        }

        if (frame % 30 != 0)
            return;

        // A joiner (slot turned active) gets the list of the panes broken on our screen.
        for (uint32_t s = 0; s < kMaxNetPlayers; s++) {
            const bool act = NetPlayers::BySlot(static_cast<int>(s)).IsActiveRemote();
            if (act && !g_was[s]) {
                uint8_t m[2 + 12 * kJoinMax];
                int n = 0, total = 0;
                for (int i = 0; i < g_nreg; i++) {
                    if (!Live(g_reg[i].obj, g_reg[i].id)
                        || *reinterpret_cast<uint64_t*>(g_reg[i].obj) != g_reg[i].vt
                        || !(g_reg[i].obj[0x640] & 1))
                        continue;
                    uint32_t pcs[257];
                    int np = 0;
                    for (uint32_t pc = 0; pc < 256; pc++)
                        if (DoneHas(i, pc))
                            pcs[np++] = pc;
                    if (!np)
                        pcs[np++] = g_reg[i].piece;
                    for (int k = 0; k < np; k++) {
                        memcpy(m + 2 + 12 * n, &g_reg[i].id, 8);
                        memcpy(m + 10 + 12 * n, &pcs[k], 4);
                        n++;
                        total++;
                        if (n == kJoinMax) {
                            m[0] = 2;
                            m[1] = kJoinMax;
                            net::session::SendEventRaw(kMsgGlass, m,
                                static_cast<uint8_t>(2 + 12 * n), static_cast<uint8_t>(s));
                            n = 0;
                        }
                    }
                }
                if (n) {
                    m[0] = 2;
                    m[1] = static_cast<uint8_t>(n);
                    net::session::SendEventRaw(kMsgGlass, m,
                        static_cast<uint8_t>(2 + 12 * n), static_cast<uint8_t>(s));
                }
                TO1_LOG("glass: slot %u is here - sent %d broken pane(s)\n", s, total);
            }
            g_was[s] = act ? 1 : 0;
        }
    }

    void Reset()
    {
        memset(g_hash, 0, sizeof g_hash);
        g_nreg = 0;
        memset(g_reg, 0, sizeof g_reg);
        memset(g_fast, 0, sizeof g_fast);
        memset(g_was, 0, sizeof g_was);
        g_nsent = g_nrecv = g_npurge = 0;
    }

}
