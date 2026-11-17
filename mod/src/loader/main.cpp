#include "config.hpp"
#include "boot.hpp"
#include "winmm-proxy.hpp"

#include "engine/memory.hpp"
#include "net-patches.hpp"
#include "log.hpp"

#include <windows.h>
#include <cstdio>

static void OpenConsole() {
#ifdef TO1_DEBUG
	if (!AllocConsole()) return;
	FILE* f{};
	freopen_s(&f, "CONOUT$", "w", stdout);
	freopen_s(&f, "CONOUT$", "w", stderr);
	freopen_s(&f, "CONIN$", "r", stdin);
	SetConsoleTitleA(PROJECT_NAME);
	to1log::Init();
#endif
}

static BOOL Initialize(HMODULE self) {
	HMODULE game = GetModuleHandleA(nullptr);

	if (Utility::memory::get_module_name(game) != GAME_MODULE_NAME)
		return FALSE;

	g_base = reinterpret_cast<uintptr_t>(game);

	OpenConsole();
	TO1_LOG("%s loading...\n", PROJECT_NAME);

	if (!boot::InitCore(game, self))
		return FALSE;

	NetPatch();

	TO1_LOG("%s initialized.\n", PROJECT_NAME);
	return TRUE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hModule);
		winmm_proxy::ResolveReal();
		Initialize(hModule);
	}
	return TRUE;
}
