#pragma once

#include <cstdint>

// Ported from tools/ext/padinject.c: the engine-level VIRTUAL PAD.
//
// XInput is not the gameplay input. The engine fills a `Pad`, then Joypad::Update copies
// it into the Joypad object every gameplay command reads. Setting a bit in Pad+0x528 (or
// the four stick pairs at Pad+0x00..0x1c) just before the real Joypad::Update gives the
// engine a genuine press edge / stick value, with its own pressed / held masks and
// timestamps. The command-reader hook learns which input bit a named command uses in the
// player's current control scheme, so remapped buttons work.
//
// The ext allocated near-image RWX thunks and re-pointed 5 call sites + 2 reader sites
// with hardcoded RVAs. This port keeps the behaviour but uses the mod's mechanisms:
// Joypad::Update and the reader are located by prologue pattern, their call sites are
// found by scanning executable pages for rel32 references, and safetyhook mid-hooks run
// at those sites. A version guard still protects the one pinned data address (the debug
// "Enable Joypad w/o Focus" byte) that cannot be pattern-matched.
//
// Activity tracking stays in the same hook and is read BEFORE injection, so an injected
// press never counts as real player input. Build 199's padact rework is included: every
// (joypad, Pad) pair is a source (not just the command-reader's learned joypad), and a
// changed answer on the Steam Input command path counts too. `t2r-no-pad-inject.txt`
// turns the whole module off; `t2r-dodge-pad-ps53-old.txt` selects the old dodge rule.

namespace engine::pad {

    struct CommandWatch;   // opaque; Watch() returns one of the fixed table entries

    // Snapshot of the padact facts consumed by the one AFK watchdog in net/match/afk.cpp.
    // lastMs covers both real-pad activity and Steam Input answer flips; the counters are
    // kept separate so a flip does not masquerade as a new pad source.
    struct PadActivity {
        uint64_t lastMs = 0;             // GetTickCount64() of the last real pad / SI input
        int      sourceSlot = -1;        // padact slot of the last real pad input
        uint32_t activityCount = 0;      // padact activity calls (button edges / sticks)
        uint32_t sourcesSeen = 0;        // distinct (joypad, Pad) pairs tracked
        uint32_t steamReads = 0;         // watched-command reads on the Steam Input path
        uint32_t steamFlips = 0;         // changed Steam Input command answers
        void*    steamJoypad = nullptr;  // first joypad seen on that path (diagnostics)
        bool     steamHooked = false;    // optional Steam Input inline hook installed
    };

    struct MeleeEdgeState {
        uint32_t seq = 0;
        bool     init = false;
        bool     keyboard = false;
        bool     bot = false;
        uint64_t lastMs = 0;
    };

    // Idempotent. Resolves + installs the call-site hooks once. false = a sidecar opt-out,
    // a missing pattern, an unsupported exe version, or a hook failure; every injection
    // call then no-ops and Ready() stays false.
    bool Install();
    bool Ready();

    // Once a frame from boot::FrameTick (after frame::Tick, before afk::Tick): logs newly
    // learned command bits and the pending-injection heartbeat.
    void Tick();

    // GetTickCount64() of the last REAL pad input (button edge / stick past 0.3 on ANY
    // joypad, or a changed Steam Input command answer), taken before injection.
    uint64_t ActivityMs();

    // The same stamp plus the padact / Steam Input diagnostic counters in one snapshot.
    PadActivity Activity();

    // Monotonic counters of injected button frames / stick frames. A change means an
    // injection was applied; the AFK watchdog uses that to treat a playing bot as active.
    uint32_t Applied();
    uint32_t StickApplied();

    // Dev A/B windows: let Joypad::Update run unfocused (the engine debug flag).
    bool NofocusOn();
    void SetNofocus(bool on);

    // Register / find a command. The reader hook learns its input bit when the engine
    // next reads it; ProbeRead forces that read for commands the game is not reading yet.
    CommandWatch* Watch(const char* name);
    void ProbeRead(const char* name);
    uint32_t ReadsTrue(const char* name);
    // True when the command's learned input bit is currently set in the primary pad's
    // real button mask (recent frame). Unlike ReadsTrue, this does not depend on the
    // engine reading the command this frame.
    bool Held(const char* name);

    // Queue a press for `ms` (minimum 20 ms, delivered for at least two Joypad::Update
    // calls). Returns false when the bit is unknown, the hooks are off, or all slots are
    // busy. BotPress is the same press but obeying an armed melee-attack block.
    bool Press(const char* name, int ms);
    bool BotPress(const char* name, int ms);

    // Stick axes in -1..1 for `ms`, refreshed by the caller while steering. `swap` is
    // for engines whose pad order is right-then-left. StickOff clears it immediately.
    void Stick(float lx, float ly, float rx, float ry, int ms, bool swap = false);
    void StickOff();

    // The primary pad's real left stick from the last 30 frames (x, y > 0 = forward).
    bool LeftStick(float& x, float& y);

    // Hide a command's input bit from the engine until an absolute GetTickCount64 time
    // (0 = stop). Used by melee pace locks so a buffered press cannot chain a combo.
    bool SetBlock(const char* name, uint64_t untilMs);
    bool BlockedRaw();     // the real button behind an armed block

    bool MeleeHeld();      // real melee-attack bit in the last 30 frames
    uint32_t MeleePresses();
    bool DodgeHeld();      // raw evade / dodge / evade-start bits (ps53 rule)

    // One press-edge reader shared by keyboard F, a bot flag and the pad's melee bit.
    // Call every frame; 1 = new press (*src = source), 2 = same press from a second
    // source within 90 ms, 0 = none.
    int MeleeEdge(MeleeEdgeState& state, uint64_t now, bool keyboardDown,
                  bool botDown, const char** src = nullptr);

}
