#pragma once

#include <windows.h>

namespace boot {
	bool InitCore(HMODULE gameModule, HMODULE selfModule);

	// was tools/features/ext_loader.py: load t2r-ext.dll next to the game exe and call its
	// t2r_tick() once per frame. The byte-patch cave hooked the per-frame MP tick
	// (FUN_180063370 == net::gui2::UpdateMenu); in source we call this at the same point.
	void TickExt();

	// True only once t2r-ext.dll actually resolved t2r_tick. Every native module
	// that shares page / input ownership with a still-loaded ext yields while this
	// is set, so we never push the same page twice or handle the same click twice.
	// The answer is cached by TickExt (called once per frame from UpdateMenu) and
	// is false on the very first FrameTick, before UpdateMenu has had a chance to
	// resolve the ext.
	bool ExtLoaded();
}

extern bool s_debugMenuOpen;
extern int  g_forcePlayerSkel;
