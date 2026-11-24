#pragma once

// Pure core ported from tools/ext/padact.h (build 199). No engine / Windows calls: the
// mod/tests/padact_core_test.cpp harness exercises it standalone.
//
// Why it exists: in pad mode the engine reads commands on the Steam Input path and never
// calls the command-reader hook, so the old "primary joypad" activity stamp learned nothing
// and a pad player was kicked for inactivity. Every Joypad::Update call is now a source.
// A (joypad, Pad) pair is one source; its first call only sets a baseline, then a changed
// button mask or a stick past kStick counts as input. Keying on the pair (not the joypad
// alone) keeps two Pads feeding one joypad on alternate frames from looking like a changed
// mask every frame. Eight slots; a full table reuses the oldest one.

#include <cmath>
#include <cstdint>

namespace engine::pad::activity {

    constexpr int    kSources = 8;
    constexpr float  kStick   = 0.3f;

    struct Source {
        const void* jp = nullptr;
        const void* pad = nullptr;
        uint64_t    buttons = 0;
        uint64_t    seen = 0;              // call sequence number; 0 = unused slot
    };

    struct Sources {
        Source   slots[kSources]{};
        uint64_t seq = 0;                  // call counter (oldest-slot reuse)
        uint64_t activityMs = 0;           // last activity, 0 = none yet
        int      activitySlot = -1;        // slot of the last activity
        uint32_t activityCount = 0;        // activity calls (a source that never rests shows here)
    };

    // One Joypad::Update call: buttons = Pad+0x528, stick = the four stick floats
    // (left x, y, right x, y). 1 = input on this call. A timestamp of 0 still marks
    // activity (activityMs 0 means "none"), so the caller must map 0 to a sentinel.
    inline bool Note(Sources& t, const void* jp, const void* pad,
                     uint64_t buttons, const float stick[4], uint64_t nowMs)
    {
        ++t.seq;
        int found = -1;
        int oldest = 0;
        for (int i = 0; i < kSources; ++i) {
            if (t.slots[i].jp == jp && t.slots[i].pad == pad && t.slots[i].seen) {
                found = i;
                break;
            }
            if (t.slots[i].seen < t.slots[oldest].seen)
                oldest = i;
        }

        bool active = false;
        if (found < 0) {
            found = oldest;
            t.slots[found].jp = jp;
            t.slots[found].pad = pad;
            t.slots[found].buttons = buttons;   // a new source: baseline only
        } else if (buttons != t.slots[found].buttons) {
            active = true;
            t.slots[found].buttons = buttons;
        }
        t.slots[found].seen = t.seq;

        for (int i = 0; i < 4; ++i) {
            // std::fabs(NaN) compares false, just like the ext's fabsf check.
            if (std::fabs(stick[i]) > kStick)
                active = true;
        }
        if (active) {
            t.activityMs = nowMs ? nowMs : 1;
            t.activitySlot = found;
            ++t.activityCount;
        }
        return active;
    }

    // Steam Input command path: one watched command's answer on one joypad. Two joypads
    // are kept per command (the engine alternates two on the reader); the first answer on
    // a joypad is its baseline. 1 = the answer changed (a button pressed or released).
    struct AnswerSlot {
        const void* jp = nullptr;
        int         last = 0;
    };

    inline bool Answer(AnswerSlot (&slots)[2], const void* jp, int answer)
    {
        const int slot = slots[0].jp == jp ? 0
                       : slots[1].jp == jp ? 1
                       : !slots[0].jp     ? 0
                       : !slots[1].jp     ? 1
                                          : -1;
        if (slot < 0 || !jp)
            return false;                  // a third joypad: not tracked (no false flips)
        const int a = answer ? 1 : 0;
        if (slots[slot].jp != jp) {
            slots[slot].jp = jp;
            slots[slot].last = a;
            return false;                  // baseline
        }
        if (slots[slot].last == a)
            return false;
        slots[slot].last = a;
        return true;
    }

} // namespace engine::pad::activity
