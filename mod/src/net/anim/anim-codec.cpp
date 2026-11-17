// ============================================================================
//  anim-codec.cpp -- the pure AnimCmdList bitstream codec. WIRE-LOCKED: the
//  encode/decode grammar and AnimListValid must stay bit-exact against the
//  peer's, so nothing here is a local change.
// ============================================================================
#include "anim-internal.hpp"

// ============================================================================
//  ANIMCMDLIST WIRE CODEC  --  ~1424 B/frame raw  ->  ~250 B packed, BIT-EXACT.
//
//  Nothing here is lossy. Every float (phase, blend weight) crosses as its exact
//  32-bit pattern; every id crosses exactly. The saving is pure redundancy:
//    - dword0 holds a 4-bit opcode + a size that is a PURE FUNCTION of the opcode
//      -> 32 bits become 2..5, and the size is never sent at all
//    - pose slots are 0..31 -> 5 bits, not 32
//    - srcSkelId is identical on every clip -> hoisted to the packet header
//    - a clip's 64-bit nameId repeats every frame -> a 10-bit session dictionary index
//    - blend weight pinned at exactly 1.0 or 0.0 -> carried by the opcode, 0 bits
//    - feather's d6..d9 are always zero (the receiver re-derives the mask) -> 0 bits
//
//  SAFETY: the packer refuses to guess. Anything it cannot express exactly becomes a
//  RAW escape (the command's literal dwords), and if the whole list defeats it the
//  packet falls back to a raw dword dump. Decode failure rejects the packet, which
//  leaves the puppet on its last good pose. So the worst case is the unpacked size,
//  never a wrong pose.
//
//  This is the ND design (FillAnimCmdBitStream @0x1409E1FB0 / NetSnapshot::Preparse
//  @0x1409C76A0) minus their fidelity sacrifices: they drop the feather mask entirely,
//  squash blend mode to 2 bits, and quantize weight to 8 bits. We keep all three exact.
//  We also beat them on clip identity: their dictionary is per-packet (64 bits per
//  distinct clip EVERY frame, ~63% of their payload) because they must interoperate
//  with shipped clients; ours is per-session because both peers are the same build.
// ============================================================================

namespace animcodec {

// ---- wire grammar ----------------------------------------------------------
//  [1] format: 1 = packed, 0 = raw dwords
//  packed: [32] hoisted srcSkelId, then commands until sub=END:
//     tag [2]: 0 = Blend(explicit weight) | 1 = Blend(weight==1.0f) | 2 = Clip
//              3 = escape -> sub [3]
//     sub [3]: 0 Feather | 1 END(terminator) | 2 Header | 3 Flip(0x0A)
//              4 Op0B | 5 Op0D | 6 Blend(weight==0.0f) | 7 RAW
enum : uint32_t { TAG_BLENDW = 0, TAG_BLEND1 = 1, TAG_CLIP = 2, TAG_ESC = 3 };
enum : uint32_t { SUB_FEATHER = 0, SUB_END = 1, SUB_HEADER = 2, SUB_FLIP = 3,
                  SUB_0B = 4, SUB_0D = 5, SUB_BLEND0 = 6, SUB_RAW = 7 };

static inline uint32_t F2B(float f)  { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline bool     IsExactly(uint32_t bits, float v) { return bits == F2B(v); }
static inline float    B2F(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

// ---------------------------------------------------------------------------
//  Field codecs -- ND parity (quantized scalars + top-relative pose slots).
//  RE'd from ND's own codec: FillAnimCmdBitStream 0x9E1FB0 / NetSnapshot::Preparse
//  0x9C76A0. ND spends 8 bits on a weight, 16 on a phase, and ~0 on pose slots (an
//  implicit instance stack). Spending a flat 32/32/5 on those three is ~60% of the
//  payload and breaches MTU (peak 1777 B > 1472), which fragments the packet.
//  EVERY codec below is SELF-VALIDATING: the compact form is emitted only when the
//  value provably round-trips, otherwise a 1-bit flag selects the exact 32-bit form.
//  A value outside the modelled range therefore costs BITS, never fidelity -- we never
//  clamp and never wrap. (ND's own encoder does `movzx ax` on the quantized phase and
//  silently WRAPS an out-of-range clip to a wildly wrong time; we refuse instead.)
// ---------------------------------------------------------------------------

// WEIGHT -- normalized [0,1] -> 16 bits. Error 1.5e-5 vs ND's 8-bit 3.9e-3 (~0.35 deg at
// a 90 deg joint delta), so we are 256x finer than ND for 8 more bits. Anything outside
// [0,1] (additive layers can exceed it) or NaN falls to the exact 32-bit form.
static inline void PutWeight(BitW& w, uint32_t bits) {
    const float f = B2F(bits);
    if (f >= 0.0f && f <= 1.0f) { w.put(1, 1); w.put((uint32_t)(f * 65535.0f + 0.5f), 16); return; }
    w.put(0, 1); w.put(bits, 32);
}
static inline uint32_t GetWeight(BitR& r) {
    if (r.get(1)) return F2B((float)r.get(16) * (1.0f / 65535.0f));
    return (uint32_t)r.get(32);
}

// PHASE -- the CLIP d5 field is in FRAMES, not normalized. (Confirmed twice in IDA: ND's
// encoder divides d5 by (numFrames-1) before quantizing @0x1409E2E6B, and its decoder
// re-multiplies by the same at 0x1408F6613 -- so their round-trip is symmetric and d5 is a
// frame index on both ends.) We deliberately do NOT copy their normalize-by-numFrames:
// it needs an ArtItemAnim deref on BOTH sides, and our receiver can resolve a clip via the
// retarget fallback to an anim with a DIFFERENT numFrames -- which would silently rescale
// the pose. Q8.8 fixed point instead: no deref anywhere, and 1/256-frame precision
// actually BEATS ND's (numFrames-1)/65535 on any clip longer than 256 frames.
static inline void PutPhase(BitW& w, uint32_t bits) {
    const float f = B2F(bits);
    if (f >= 0.0f && f < 255.99f) { w.put(1, 1); w.put((uint32_t)(f * 256.0f + 0.5f), 16); return; }
    w.put(0, 1); w.put(bits, 32);                    // >=256 frames, negative, or NaN -> exact
}
static inline uint32_t GetPhase(BitR& r) {
    if (r.get(1)) return F2B((float)r.get(16) * (1.0f / 256.0f));
    return (uint32_t)r.get(32);
}

// BLEND mode d4 -- 1 bit for the overwhelmingly common literal 1, else a 2-bit code over
// ND's observed alphabet {2,6} with a 32-bit escape. Keeping the 1-bit fast path is why
// this beats ND's flat 2-bit code, which would cost +1 bit on every ordinary blend.
static inline void PutMode(BitW& w, uint32_t d4) {
    if (d4 == 1u) { w.put(1, 1); return; }
    w.put(0, 1);
    if      (d4 == 2u) w.put(0u, 2);
    else if (d4 == 6u) w.put(1u, 2);
    else             { w.put(2u, 2); w.put(d4, 32); }
}
static inline uint32_t GetMode(BitR& r) {
    if (r.get(1)) return 1u;
    const uint32_t c = (uint32_t)r.get(2);
    if (c == 0u) return 2u;
    if (c == 1u) return 6u;
    return (uint32_t)r.get(32);                      // c>=2: escape (encoder only ever writes 2)
}

// POSE SLOT -- top-relative, ND's implicit-instance-stack idea but with an always-available
// literal. `top` = one past the highest slot written so far, recomputed IDENTICALLY on both
// sides from DECODED values only (never from engine semantics), so the two can't drift.
// A 2-bit code covers top..top-2, which is where nearly every reference lands; anything
// else is a 6-bit literal. Crucially the literal means a bad prediction costs BITS, never
// correctness -- unlike ND, whose scheme leans on structural asserts (dst in {srcA,srcB},
// |srcA-srcB|<=4, top-max(src) in [0,3]) and bails the WHOLE frame to an empty pose when
// they fail -- the exact bind-pose snap we already identified as a visible flicker.
//   SYMMETRY RULE (load-bearing): `top` is advanced ONLY by packed commands, never across a
// RAW escape, on both sides. Otherwise the decoder would have to re-parse raw dwords to
// track it and would desync. The literal is 6 bits so it spans 0..63, the whole range
// AnimListValid permits: a narrower one would push slots >=32 onto a RAW escape, which
// costs 13+32*sz bits, i.e. MORE than sending the command uncompressed.
static inline void PutSlot(BitW& w, uint32_t slot, uint32_t top) {
    const int32_t d = (int32_t)top - (int32_t)slot;
    if (d >= 0 && d <= 2) { w.put((uint32_t)d, 2); return; }
    w.put(3u, 2); w.put(slot, 6);                    // caller guarantees slot < 64
}
static inline uint32_t GetSlot(BitR& r, uint32_t top) {
    const uint32_t c = (uint32_t)r.get(2);
    if (c <= 2) return top - c;   // honest stream guarantees top>=c; a malformed one underflows
    return (uint32_t)r.get(6);    // to a huge slot, which AnimListValid then rejects.
}

} // namespace animcodec

// Encode a dword AnimCmdList. Returns BITS written, 0 on failure.
uint32_t NetAnimEncode(const uint32_t* cmd, uint32_t n,
                              uint8_t* out, uint32_t capBytes, uint8_t* outEpoch)
{
    using namespace animcodec;
    *outEpoch = g_txEpoch;
    BitW w(out, capBytes);

    // Hoist srcSkelId: identical on every clip in practice, so send it once. If a
    // clip disagrees we fall back to a RAW escape for that one command.
    uint32_t hoisted = 0;
    for (uint32_t i = 0; i < n; ) {
        const uint32_t sz = CmdSize(cmd[i]);
        if (sz == 0 || i + sz > n) return 0;
        if (CmdOp(cmd[i]) == OP_CLIP) { if (sz < 2) return 0; hoisted = cmd[i + 1]; break; }
        i += sz;
    }

    TxPendBegin();            // definitions are charged on transmit, not here
    w.put(1, 1);              // format = packed
    w.put(hoisted, 32);

    // Emit one command verbatim as a RAW escape (always exact, never a guess).
    auto raw = [&](const uint32_t* c, uint32_t sz) {
        if (sz > 255) { w.ok = false; return; }
        w.put(TAG_ESC, 2); w.put(SUB_RAW, 3); w.put(sz, 8);
        for (uint32_t k = 0; k < sz; ++k) w.put(c[k], 32);
    };
    // PutSlot's literal is 6 bits and AnimListValid allows 0..63, so every in-range slot
    // packs. Forcing a [32,63] slot onto a RAW escape would be BIGGER than the raw dwords.
    auto slotOk = [](uint32_t s) { return s < 64u; };

    // Running pose-slot cursor for top-relative coding. Mirrored EXACTLY in the decoder:
    // advanced only by packed commands (never by a RAW escape), always from the slot values
    // themselves, so both sides compute the identical sequence. See PutSlot.
    uint32_t top = 0;

    bool sawEnd = false;
    for (uint32_t i = 0; i < n && w.ok && !sawEnd; ) {
        const uint32_t sz = CmdSize(cmd[i]);
        const uint32_t op = CmdOp(cmd[i]);
        if (sz == 0 || i + sz > n) return 0;
        const uint32_t* c = &cmd[i];

        switch (op) {
        case OP_CLIP: {
            // {op, srcSkelId, nameLo, nameHi, slot, phase}
            if (sz != 6 || !slotOk(c[4])) { raw(c, sz); break; }
            w.put(TAG_CLIP, 2);
            // srcSkelId is USUALLY the hoisted value but not always. Carry a 1-bit
            // flag with a 32-bit escape rather than raw-escaping the whole command:
            // a RAW escape costs sz*32 + 13 bits, i.e. MORE than the unpacked dwords.
            const bool skelSame = (c[1] == hoisted);
            w.put(skelSame ? 1u : 0u, 1);
            if (!skelSame) w.put(c[1], 32);
            PutRef(w, *reinterpret_cast<const uint64_t*>(&c[2]));
            PutSlot(w, c[4], top);                            // top-relative: 2 bits, else 8
            PutPhase(w, c[5]);                                // Q8.8: 17 bits, else 33 exact
            top = c[4] + 1;                                   // dest written -> advance cursor
            break;
        }

        case OP_BLEND: {
            // {op, srcA, srcB, dst, mode, weight}
            if (sz != 6 || !slotOk(c[1]) || !slotOk(c[2]) || !slotOk(c[3])) {
                raw(c, sz); break;
            }
            const bool w1 = IsExactly(c[5], 1.0f);
            const bool w0 = IsExactly(c[5], 0.0f);
            if (w1)      w.put(TAG_BLEND1, 2);
            else if (w0) { w.put(TAG_ESC, 2); w.put(SUB_BLEND0, 3); }
            else         w.put(TAG_BLENDW, 2);
            // All three coded against the SAME pre-command `top` (decoder does likewise).
            PutSlot(w, c[1], top); PutSlot(w, c[2], top); PutSlot(w, c[3], top);
            PutMode(w, c[4]);                                 // 1 bit for d4==1, else 3 (+32 escape)
            if (!w1 && !w0) PutWeight(w, c[5]);               // 17 bits, else 33 exact
            top = c[3] + 1;                                   // d3 is the destination slot
            break;
        }

        case OP_FEATHER: {
            // {op,_,slotA,slotB,mode,weight, 0,0, 0,0, fbSidLo,fbSidHi}
            const bool zeroed = (*reinterpret_cast<const uint64_t*>(&c[6]) == 0ull &&
                                 *reinterpret_cast<const uint64_t*>(&c[8]) == 0ull);
            if (sz != 12 || !slotOk(c[2]) || !slotOk(c[3]) || c[4] >= 16u || !zeroed) {
                raw(c, sz); break;
            }
            w.put(TAG_ESC, 2); w.put(SUB_FEATHER, 3);
            w.put(c[1] == 0u ? 1u : 0u, 1); if (c[1]) w.put(c[1], 32);
            PutSlot(w, c[2], top); PutSlot(w, c[3], top);
            w.put(c[4], 4);                                   // mode, exact (ND squashes to 2)
            { const bool w1 = IsExactly(c[5], 1.0f);
              w.put(w1 ? 1u : 0u, 1); if (!w1) PutWeight(w, c[5]); }
            PutRef(w, *reinterpret_cast<const uint64_t*>(&c[10]));
            top = c[3] + 1;                                   // d3 aliases the blend's dest slot
            break;
        }

        // Header/terminator carry a 1-bit width flag rather than a hard limit: an
        // out-of-range value must not force the WHOLE frame onto the raw fallback
        // forever (these widths are assumptions about engine fields, not guarantees).
        case OP_HEADER: {
            if (sz != 2) { raw(c, sz); break; }
            const bool isSmall = c[1] < 64u;
            w.put(TAG_ESC, 2); w.put(SUB_HEADER, 3);
            w.put(isSmall ? 1u : 0u, 1); w.put(c[1], isSmall ? 6 : 32);
            break;
        }

        case OP_TERM: {
            if (sz != 2) { raw(c, sz); break; }
            const bool isSmall = c[1] < 256u;
            w.put(TAG_ESC, 2); w.put(SUB_END, 3);
            w.put(isSmall ? 1u : 0u, 1); w.put(c[1], isSmall ? 8 : 32);
            sawEnd = true;                                    // terminator ends the stream
            break;
        }

        case OP_FLIP: case OP_MIRROR: case OP_EMPTY:
            if (sz != 2 || !slotOk(c[1])) { raw(c, sz); break; }
            w.put(TAG_ESC, 2);
            w.put(op == OP_FLIP ? SUB_FLIP : (op == OP_MIRROR ? SUB_0B : SUB_0D), 3);
            PutSlot(w, c[1], top);
            top = c[1] + 1;
            break;

        default:
            raw(c, sz);                                       // never guess
            break;
        }
        i += sz;
    }

    if (!w.ok || !sawEnd) return 0;                           // caller falls back to raw
    return w.pos;
}

// Decode back to the dword list the replay path expects. Returns dwords, 0 on failure.
// senderSlot selects that sender's private clip dictionary (per-author index space).
uint32_t NetAnimDecode(int senderSlot, const uint8_t* data, uint32_t nBits, uint8_t epoch,
                              uint32_t* out, uint32_t maxDwords)
{
    using namespace animcodec;
    RxDictEpoch(senderSlot, epoch);
    BitR r(data, nBits);

    uint32_t o = 0;
    auto room = [&](uint32_t k) { return o + k <= maxDwords; };

    if (r.get(1) == 0) {                                      // raw dword passthrough
        const uint32_t n = (uint32_t)r.get(16);
        if (!r.ok || n == 0 || n > maxDwords) return 0;
        for (uint32_t k = 0; k < n; ++k) out[k] = (uint32_t)r.get(32);
        return r.ok ? n : 0;
    }

    const uint32_t hoisted = (uint32_t)r.get(32);
    if (!r.ok) return 0;

    // Mirror of the encoder's pose-slot cursor. MUST be advanced by exactly the same
    // commands, in the same order, from the same decoded values -- and NEVER across a RAW
    // escape (the decoder can't re-parse raw dwords, so both sides simply skip them).
    uint32_t top = 0;

    for (;;) {
        const uint32_t tag = (uint32_t)r.get(2);
        if (!r.ok) return 0;

        if (tag == TAG_CLIP) {
            const uint32_t skelSame = (uint32_t)r.get(1);
            const uint32_t srcSkel  = skelSame ? hoisted : (uint32_t)r.get(32);
            uint64_t key = 0;
            if (!r.ok || !GetRef(senderSlot, r, key)) return 0;
            const uint32_t slot = GetSlot(r, top);
            const uint32_t ph   = GetPhase(r);
            if (!r.ok || !room(6)) return 0;
            out[o++] = MakeCmd(OP_CLIP, 6); out[o++] = srcSkel;
            *reinterpret_cast<uint64_t*>(&out[o]) = key; o += 2;
            out[o++] = slot; out[o++] = ph;
            top = slot + 1;
            continue;
        }
        if (tag == TAG_BLENDW || tag == TAG_BLEND1) {
            // Declarator initialisers are sequenced left-to-right, so these three reads
            // happen in wire order against the same pre-command `top` (as the encoder wrote).
            const uint32_t a = GetSlot(r, top), b = GetSlot(r, top), d = GetSlot(r, top);
            const uint32_t md = GetMode(r);
            const uint32_t wt = (tag == TAG_BLEND1) ? F2B(1.0f) : GetWeight(r);
            if (!r.ok || !room(6)) return 0;
            out[o++] = MakeCmd(OP_BLEND, 6);
            out[o++] = a; out[o++] = b; out[o++] = d; out[o++] = md; out[o++] = wt;
            top = d + 1;
            continue;
        }

        const uint32_t sub = (uint32_t)r.get(3);
        if (!r.ok) return 0;

        if (sub == SUB_END) {
            const uint32_t isSmall = (uint32_t)r.get(1);
            const uint32_t flags = (uint32_t)r.get(isSmall ? 8 : 32);
            if (!r.ok || !room(2)) return 0;
            out[o++] = MakeCmd(OP_TERM, 2); out[o++] = flags;
            return o;                                         // terminator is always last
        }
        if (sub == SUB_BLEND0) {
            const uint32_t a = GetSlot(r, top), b = GetSlot(r, top), d = GetSlot(r, top);
            const uint32_t md = GetMode(r);
            if (!r.ok || !room(6)) return 0;
            out[o++] = MakeCmd(OP_BLEND, 6);
            out[o++] = a; out[o++] = b; out[o++] = d; out[o++] = md; out[o++] = F2B(0.0f);
            top = d + 1;
            continue;
        }
        if (sub == SUB_FEATHER) {
            const uint32_t d1z = (uint32_t)r.get(1);
            const uint32_t d1  = d1z ? 0u : (uint32_t)r.get(32);
            const uint32_t sa  = GetSlot(r, top), sb = GetSlot(r, top);
            const uint32_t md  = (uint32_t)r.get(4);
            const uint32_t w1  = (uint32_t)r.get(1);
            const uint32_t wt  = w1 ? F2B(1.0f) : GetWeight(r);
            uint64_t fb = 0;
            if (!r.ok || !GetRef(senderSlot, r, fb) || !room(12)) return 0;
            out[o++] = MakeCmd(OP_FEATHER, 12); out[o++] = d1;
            out[o++] = sa; out[o++] = sb; out[o++] = md; out[o++] = wt;
            *reinterpret_cast<uint64_t*>(&out[o]) = 0ull; o += 2;   // counts  (re-derived)
            *reinterpret_cast<uint64_t*>(&out[o]) = 0ull; o += 2;   // factors (re-derived)
            *reinterpret_cast<uint64_t*>(&out[o]) = fb;   o += 2;
            top = sb + 1;
            continue;
        }
        if (sub == SUB_HEADER) {
            const uint32_t isSmall = (uint32_t)r.get(1);
            const uint32_t cnt   = (uint32_t)r.get(isSmall ? 6 : 32);
            if (!r.ok || !room(2)) return 0;
            out[o++] = MakeCmd(OP_HEADER, 2); out[o++] = cnt;
            continue;
        }
        if (sub == SUB_FLIP || sub == SUB_0B || sub == SUB_0D) {
            const uint32_t slot = GetSlot(r, top);
            if (!r.ok || !room(2)) return 0;
            const uint32_t op = (sub == SUB_FLIP) ? OP_FLIP : (sub == SUB_0B ? OP_MIRROR : OP_EMPTY);
            out[o++] = MakeCmd(op, 2); out[o++] = slot;
            top = slot + 1;
            continue;
        }
        if (sub == SUB_RAW) {
            const uint32_t sz = (uint32_t)r.get(8);
            if (!r.ok || sz == 0 || !room(sz)) return 0;
            for (uint32_t k = 0; k < sz; ++k) out[o++] = (uint32_t)r.get(32);
            if (!r.ok) return 0;
            continue;
        }
        return 0;                                             // unreachable sub-opcode
    }
}

// Structural validation of a received list. A malformed count or an out-of-range
// slot is a REMOTE MEMORY-CORRUPTION PRIMITIVE, not just a bad pose: the engine
// bounds-checks nothing, and a count past ~85 overruns its fixed 4KB context.
// Anything that fails here is rejected WHOLE -- we never execute part of a list.
bool AnimListValid(const uint32_t* c, uint32_t n)
{
    if (n < 4 || n > (uint32_t)kMaxAnimCmdDwords) return false;
    if (CmdOp(c[0]) != OP_HEADER) return false;

    const uint32_t count = c[1];                     // pose-slot count from the header
    if (count < 1 || count > kMaxPoseSlots) return false;

    uint32_t headers = 0, terms = 0, i = 0, maxSlot = 0;
    bool writesSlot0 = false, termLast = false;

    // Two distinct predicates. `writesSlot0` gates the whole list (the terminator publishes
    // slot 0 unconditionally, so SOMETHING must produce it), and only a command that WRITES
    // a slot may satisfy it -- counting a mere READ of slot 0 would accept a list with no
    // producer for it and hand the executor stale bump-allocator memory. That distinction is
    // why the read operands below use slotRead, including OP_BLEND's two sources.
    auto slotRead  = [&](uint32_t s) { if (s >= count) return false; if (s > maxSlot) maxSlot = s;
                                       return true; };
    auto slotWrite = [&](uint32_t s) { if (!slotRead(s)) return false;
                                       if (s == 0) writesSlot0 = true; return true; };

    while (i < n) {
        uint32_t sz = CmdSize(c[i]), op = CmdOp(c[i]);
        if (sz < 1 || i + sz > n) return false;       // must tile the buffer exactly
        termLast = false;
        switch (op) {
        case OP_HEADER:  if (++headers != 1 || i != 0) return false; break;
        case OP_TERM:    if (++terms   != 1) return false; termLast = true; break;
        case OP_CLIP:    if (sz != 6  || !slotWrite(c[i + 4])) return false; break;
        case OP_BLEND:   if (sz != 6  || !slotRead(c[i + 1]) || !slotRead(c[i + 2])
                                      || !slotWrite(c[i + 3])) return false; break;
        // d1 MUST be bounded. The engine routes 0x08 and 0x09 through the SAME handler
        // (jump-table entries for opcode 8 and 9 are both 0x14169B778, verified), and that
        // handler uses [cmd+4] as a pose-slot index: `imul edx,r8d; shl rdx,4; add rdx,rdi`,
        // with no bound of its own. SUB_RAW can synthesise any dword, so an unchecked d1 is a
        // remote wild-write primitive -- the same hole 0x0F closes below.
        case OP_FEATHER: if (sz != 12 || !slotRead(c[i + 1]) || !slotRead(c[i + 2])
                                      || !slotWrite(c[i + 3])) return false; break;
        case OP_FLIP: case OP_MIRROR: case OP_EMPTY:
                         if (sz != 2  || !slotWrite(c[i + 1])) return false; break;
        // 0x0F's operands are bounded for the same reason: with SUB_RAW able to synthesise
        // arbitrary dwords, a hostile/corrupt packet would otherwise hand the executor slot
        // indices like 0xFFFFFFFF, which it never bounds-checks -> wild write. They are
        // read-only operands, so they must NOT satisfy writesSlot0.
        case OP_0F:      if (sz  < 3  || !slotRead(c[i + 1]) || !slotRead(c[i + 2])) return false; break;
        // Plugins are SID-allowlisted, not merely size-checked. Size 10 alone is not safe:
        // joint-spring is also 10 dwords and packs a raw AnimSnapshotNode* into d6:d7, so a
        // corrupt or hostile packet could hand the executor an arbitrary pointer to dereference
        // (SUB_RAW can synthesise any dwords it likes). Only the SIDs we ourselves ship -- whose
        // payload is provably pointer-free once d8:d9 is zeroed -- are accepted.
        case OP_PLUGIN: {
            if (sz != 10) return false;
            const uint64_t psid = *reinterpret_cast<const uint64_t*>(&c[i + 2]);
            if (psid == SID_LEGFIXIK) {
                // d8:d9 is an optional raw float*; we always send 0 and must never accept
                // anything else, or the handler dereferences a sender-side address.
                if (*reinterpret_cast<const uint64_t*>(&c[i + 8]) != 0) return false;
            } else if (kCarryAnkleBlendIk && psid == SID_ANKLEBLENDIK) {
                if (!slotRead(c[i + 9])) return false;          // d9 is a slot on this one; d8 is a float
            } else {
                return false;                                   // everything else, incl. joint-spring
            }
            if (!slotRead(c[i + 6]) || !slotRead(c[i + 7])) return false;
            break;
        }
        default:         return false;                // allowlist only
        }
        i += sz;
    }
    // exactly one header + one terminator, terminator last, and slot 0 must be
    // produced (the terminator publishes it unconditionally, mask unchecked).
    return i == n && headers == 1 && terms == 1 && termLast && writesSlot0;
}
