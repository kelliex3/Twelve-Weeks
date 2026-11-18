#pragma once

// Ported from tools/ext/feel.c (FeelFlag), built on engine::exe_side.
//
// A cached "is this opt-in / opt-out sidecar present next to the exe" answer,
// re-read at most every `everyFrames` frames so a per-frame caller never touches
// the disk. The ext kept the (at, val) pair in a static per call site; here the
// cache is keyed by file name, so every caller of the same file shares one answer
// (opt-out files switch a feature off, opt-in files on -- the caller knows which).

namespace engine::flag {

    // Frames between re-reads (the ext's FeelFlag callers pass 120).
    inline constexpr int kDefaultInterval = 120;

    // Presence of `file` next to the exe, cached for `everyFrames` frames.
    bool Present(const char* file, int everyFrames = kDefaultInterval);

    // Convenience for an opt-out toggle: the feature is on when the file is absent.
    bool Absent(const char* file, int everyFrames = kDefaultInterval);

    // Drop every cached answer (test / level-reload helper).
    void Reset();

}
