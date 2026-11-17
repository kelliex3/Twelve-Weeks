// t2r-engine.h - DLL-agnostic engine-symbol layer for the T2R extension.
//
// The extension used to reach winmm (the mod) through hard-coded image offsets,
// the VMA(0x180...) macro.  A rebuilt winmm moves every one of them, so the
// extension broke on anything but the exact reference build.  This layer replaces
// those offsets with named handles that are resolved at load time:
//
//   T2R_SYM(kFnLocalPlayer)     - an engine function or global.  The mod publishes
//                                 it by name in its ".t2rsym" manifest; the
//                                 reference build's address is the fallback.
//   T2R_CODE(kCodeKillfeedBody) - an instruction address used to install or verify
//                                 a hook / patch.  Located by signature scan; the
//                                 reference build's address is the fallback.
//
// The manifest is a ".t2rsym" PE section in winmm:
//
//   { uint32 magic; uint32 version; uint32 count; uint32 reserved;
//     { uint64 sid; uint64 rva; } entries[count]; }
//
// Names are the tokens in t2r-engine-symbols.def, hashed with FNV-1a 64 - the
// same StringId the engine's own sidbase uses.  See mod/src/loader/ext-symbols.cpp
// for the publishing side.
#pragma once
#include <windows.h>
#include <stdint.h>

// Image base of the reference winmm build the legacy VAs are expressed against.
#define T2R_IMAGE_BASE 0x180000000ULL

// A raw winmm-relative offset, resolved against the loaded image.  This is the
// escape hatch for the few sites that carry a runtime offset (a site table
// entry, a computed RVA) rather than a fixed address; it replaces the old
// VMA(0x180000000 + off) form.  g_winmm is the unity build's loaded module.
#define T2R_LEGACY_RVA(off) ((uintptr_t)g_winmm + (uintptr_t)(off))

// The old VMA() semantics, for the few call sites that hold a reference-build
// absolute VA in a variable or table (a code-site list, a runtime parameter).
// Maps a reference VA to the loaded address.  Prefer T2R_SYM/T2R_CODE; use this
// only where the address is genuinely dynamic.
#define T2R_LEGACY_VA(va) ((uintptr_t)g_winmm + ((uintptr_t)(va) - T2R_IMAGE_BASE))

// Every published symbol, in the order of t2r-engine-symbols.def.  The enum
// constants are exactly the names from that file, so a call site reads
// T2R_SYM(kFnLocalPlayer) and the index is kFnLocalPlayer.
enum {
#define X(kind, name, va) name,
#include "t2r-engine-symbols.def"
#undef X
    T2R_SYMBOL_COUNT
};

// Resolved absolute addresses (filled by t2r_engine_init; all zero until then).
extern uintptr_t g_t2rSymbol[T2R_SYMBOL_COUNT];

// Resolve every symbol against a loaded winmm.  Safe to call more than once.
// Falls back to the reference address for anything winmm does not publish.
void t2r_engine_init(HMODULE winmm);

#define T2R_SYM(name)  g_t2rSymbol[name]
#define T2R_CODE(name) g_t2rSymbol[name]
