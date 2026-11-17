#pragma once

#include "sid/sid.hpp"


namespace NdGameSdk::Sidbase {

	typedef StringId64 StringId;

	extern bool g_bSidbaseAvailable;

	bool InitSidbase(const char* sid_path = "sidbase.bin");
	void ShutdownSidbase();
	const char* StringIdToStringInternal(StringId sid);
}
