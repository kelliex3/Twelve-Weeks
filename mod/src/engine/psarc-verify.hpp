#pragma once

#include <cstdint>

// Integrity check for the GAME's own psarcs (bin/core/common/world-*/speech*...).
// A build-time manifest pins each archive's size and a sampled fingerprint; every
// mount of an archive under the game data root is checked against it, so a player
// cannot repack a shipped psarc or drop an extra one into the game folder.

namespace engine::verify {

	// 0 disables checking entirely (mounts are then allowed as before).
	extern volatile long g_enabled;

	// False when the build had no reference game install to hash.
	bool HasManifest();

	// relPath: lowercased, forward-slashed, relative to the game data root.
	// fullPath: absolute UTF-8 path to read.
	// ALWAYS returns true -- a bad archive must not fail the mount, or the check
	// becomes a one-frame-away breakpoint target. Failures only raise the flag
	// below; acting on it is the caller's job, far away in time and code.
	bool CheckArchive(const char* relPath, const char* fullPath);

	// True once any game archive failed to match the build-time manifest.
	bool Tampered();
}
