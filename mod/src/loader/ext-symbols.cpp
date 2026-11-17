#include "ext-symbols.hpp"

#include "sid/sid.hpp"                 // SID(): FNV-1a 64, the same id the extension hashes
#include "net/match/net-player.hpp"    // the engine entry points the extension calls
#include "log.hpp"

#include <windows.h>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// The ".t2rsym" manifest
//
//   struct { uint32 magic; uint32 version; uint32 count; uint32 reserved;
//            struct { uint64 sid; uint64 rva; } entries[count]; };
//
// magic 'T2RS' (0x53523254), version 1.  rva is relative to this module's base,
// which is the module the extension adds it to.  tools/ext/t2r-engine.c looks
// the section up by name and matches each of its handles by hashing the name in
// tools/ext/t2r-engine-symbols.def with FNV-1a 64.  Anything not published here
// falls back to the reference build's address, so a partial manifest is safe.
// ---------------------------------------------------------------------------
#pragma section(".t2rsym", read, write)

namespace {

	constexpr uint32_t kSymMagic    = 0x53523254u;  // 'T2RS'
	constexpr uint32_t kSymVersion  = 1u;
	constexpr uint32_t kSymCapacity = 512;

#pragma pack(push, 1)
	struct ExtSymEntry {
		uint64_t sid;
		uint64_t rva;
	};
	struct ExtSymManifest {
		uint32_t magic;
		uint32_t version;
		uint32_t count;
		uint32_t reserved;
		ExtSymEntry entries[kSymCapacity];
	};
#pragma pack(pop)

	__declspec(allocate(".t2rsym")) ExtSymManifest g_extSymManifest = {
		kSymMagic, kSymVersion, 0u, 0u, {}
	};

	// The canonical name list, shared with the extension (single source of
	// truth).  Included so InitExtSymbols can reject a binding whose name is not
	// in the contract - the names can never silently drift apart.
	struct ContractName { const char* name; uint64_t legacy; };
	const ContractName kContract[] = {
#define X(kind, name, va) { #name, static_cast<uint64_t>(va) },
#include TO1_EXT_SYMBOLS_DEF
#undef X
	};

	bool InContract(const char* name) {
		for (const ContractName& c : kContract)
			if (std::strcmp(c.name, name) == 0) return true;
		return false;
	}

	// -----------------------------------------------------------------------
	// What the mod publishes to the extension.
	//
	// Each entry maps a contract name (tools/ext/t2r-engine-symbols.def) to the
	// mod symbol the extension should call.  The entries below are the engine
	// entry points that are unambiguous by name; they are NOT yet verified
	// against the extension's expected record layout, because the source build
	// does not ship yet.  Grow this list as the source build's engine ABI is
	// pinned down - every name left out simply keeps the extension on the
	// reference build's address.
	// -----------------------------------------------------------------------
	struct Binding { const char* name; const void* symbol; };

	const Binding kBindings[] = {
		{ "kFnBySlot",                  reinterpret_cast<const void*>(&::NetPlayers::BySlot) },
		{ "kFnNetPlayersLocal",         reinterpret_cast<const void*>(&::NetPlayers::Local) },
		{ "kFnNetPlayersFromEnginePtr", reinterpret_cast<const void*>(&::NetPlayers::FromEnginePtr) },
	};

}

void loader::InitExtSymbols()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&InitExtSymbols), &self);
	const uintptr_t base = reinterpret_cast<uintptr_t>(self);

	uint32_t n = 0;
	for (const Binding& b : kBindings) {
		if (!b.symbol || n >= kSymCapacity) continue;
		if (!InContract(b.name)) {
			TO1_LOG("[ext-symbols] '%s' is not in t2r-engine-symbols.def - not published\n", b.name);
			continue;
		}
		g_extSymManifest.entries[n].sid = SID(b.name);
		g_extSymManifest.entries[n].rva = reinterpret_cast<uintptr_t>(b.symbol) - base;
		n++;
	}
	g_extSymManifest.count = n;
	g_extSymManifest.magic = kSymMagic;
	g_extSymManifest.version = kSymVersion;
	TO1_LOG("[ext-symbols] published %u engine symbols to .t2rsym\n", n);
}
