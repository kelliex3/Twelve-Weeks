#pragma once

#include <Windows.h>

namespace engine::archive {

	extern volatile long g_archiveGateEnabled;

	bool Init(HMODULE gameModule, HMODULE selfModule);
}
