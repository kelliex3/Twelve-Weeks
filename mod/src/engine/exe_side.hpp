#pragma once

#include <cstdio>

// Ported from tools/ext/t2r-ext.c (ExeSideOpen + the fa_poll "not there" cache).
//
// A file that lives next to the game exe -- the t2r-*.txt opt-in / opt-out sidecars
// every feature reads -- is opened without paying a file-system call on every frame:
// a read open of an absent file is answered from a ~1.5 s cache. A write open drops
// the entry so the next read sees the new file. Reusable by any feature that reads
// or writes a sidecar in the game folder.

namespace engine::exe_side {

    // The game exe's folder (trailing separator), cached. "" if it could not be found.
    const char* Dir();

    // `name` resolved inside the exe folder. false (and out[0] = 0) when it would not fit.
    bool FullPath(const char* name, char* out, int cap);

    // Read-only cached existence check (no fopen).
    bool Exists(const char* name);

    // fopen() of `name` next to the exe. Read opens consult the presence cache; write
    // opens invalidate it. nullptr on failure (missing file / unbuildable path).
    FILE* Open(const char* name, const char* mode);

    // Drop the cached presence of `name` (e.g. after writing it yourself).
    void Forget(const char* name);

    // Drop every cached entry (test / level-reload helper).
    void Reset();

}
