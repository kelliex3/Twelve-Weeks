#pragma once

// Publishes the mod's engine entry points to the T2R extension by name.
//
// This replaces the extension's old hard-coded winmm offsets (the VMA(0x180...)
// macro; see tools/ext/t2r-engine.h). The manifest lives in a ".t2rsym" PE
// section; the extension reads it when it loads and resolves its
// T2R_SYM(name) / T2R_CODE(name) handles against it, falling back to the
// reference build's address for any name the mod does not publish.
//
// Call InitExtSymbols() before t2r-ext.dll is loaded (boot::TickExt does).
namespace loader {

	void InitExtSymbols();

}
