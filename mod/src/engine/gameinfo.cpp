#include "engine/gameinfo.hpp"
#include "engine/patterns.hpp"
#include "engine/memory.hpp"
#include "loader/config.hpp"
#include "log.hpp"

#include <safetyhook.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine {

	static uintptr_t** s_ComponentTable{};
	static constexpr size_t kNdGameInfoIndex = 0;

	static NdGameInfo* GetNdGameInfo() {
		if (!s_ComponentTable) return nullptr;
		return (NdGameInfo*)(s_ComponentTable[kNdGameInfoIndex]);
	}

	NdGameInfo* GetGameInfo() {
		return GetNdGameInfo();
	}

	void* GetEngineComponent(size_t index) {
		if (!s_ComponentTable) return nullptr;
		return s_ComponentTable[index];
	}

	static safetyhook::MidHook s_GameInitHook{};
	static safetyhook::MidHook s_GameInitReturnHook{};
	static SafetyHookInline s_StdHandleHook{};
	static Utility::memory::Patch::Ptr s_DevModePatch{};

	// The NIXXES std-handle probe rejects our AllocConsole'd handles and kills
	// stdio; forcing its bool result true (al=1) keeps console output alive.
	extern "C" bool StdHandle_ReturnTrue() {
		return true;
	}

	bool InitGameInfo(HMODULE gameModule)
	{
		// EngineComponents table (NdGameInfo is entry 0).
		s_ComponentTable = (uintptr_t**)Utility::ReadLEA32(gameModule, Patterns::EngineComponents_s_table, 3, 7);

		if (!s_ComponentTable) {
			TO1_LOG("[gameinfo] FATAL: EngineComponents_s_table not found.\n");
			return false;
		}

		// GameInit entry + return mid-hooks.
		constexpr size_t GameInitReturnHook_Offset = 0x13;

		auto GameInitJMP = (void*)Utility::FindAndPrintPattern(gameModule, Patterns::CommonGame_GameInit);
		auto GameInitReturnJMP = (void*)Utility::FindAndPrintPattern(gameModule, Patterns::CommonGame_GameInit, GameInitReturnHook_Offset);

		if (!GameInitJMP ||
			!GameInitReturnJMP) {
			TO1_LOG("[gameinfo] FATAL: CommonGame_GameInit pattern not found.\n");
			return false;
		}

		s_GameInitHook = Utility::MakeMidHook(GameInitJMP,
			[](SafetyHookContext& ctx)
			{
				auto* ndgameinfo = GetNdGameInfo();
				if (ndgameinfo) {
					// Rollback original cfg path to game path (Like it was in T1X)
					strcpy(ndgameinfo->m_CfgPath, ndgameinfo->m_GamePath);
					ndgameinfo->m_DevConfig = true;
				}

			}, Patterns::CommonGame_GameInit.name, TO1_NAME("GameInitJMP"));

		s_GameInitReturnHook = Utility::MakeMidHook(GameInitReturnJMP,
			[](SafetyHookContext& ctx)
			{
				auto* ndgameinfo = GetNdGameInfo();
				if (ndgameinfo) {
					strcpy(ndgameinfo->m_DiscUser, PROJECT_NAME);
					strcpy(ndgameinfo->m_StatsBuild, PROJECT_NAME);
					ndgameinfo->m_DisableFpsStat = true; // disable on boot, but for some reason also gets rid of active task display permanently.

#ifdef TO1_RELEASE
					ndgameinfo->m_DevMode = false; // disable on boot
#endif
				}

			}, Patterns::CommonGame_GameInit.name, TO1_NAME("GameInitReturnJMP"));

		if (!s_GameInitHook ||
			!s_GameInitReturnHook) {
			TO1_LOG("[gameinfo] FATAL: failed to hook CommonGame_GameInit.\n");
			return false;
		}

		// (3) StdHandle fix -- warn-not-fail: without it only the logs suffer.
		auto StdHandleTarget = (void*)Utility::FindAndPrintPattern(gameModule, Patterns::CommonGame_NIXXES_StdHandle);

		if (StdHandleTarget) {
			s_StdHandleHook = safetyhook::create_inline(StdHandleTarget, (void*)&StdHandle_ReturnTrue);
		}
		if (!s_StdHandleHook) {
			TO1_LOG("[gameinfo] WARNING: failed to patch %s! Logs may not work!\n",
				TOSTRING(Patterns::CommonGame_NIXXES_StdHandle));
		}

		constexpr unsigned char m_DevMode_1[] = { 0xb2, 0x01, 0x90 };

		s_DevModePatch = Utility::WritePatchPattern(gameModule, Patterns::NdDevMenu_GameConfig_DevMode, m_DevMode_1, sizeof(m_DevMode_1));

		if (!s_DevModePatch) {
			TO1_LOG("[gameinfo] WARNING: DevMode patch failed — dev config env may not boot.\n");
		}

		const char* game_ver_text = "BUILD_NUMBER=";
		uintptr_t version_number = (uintptr_t)Utility::memory::char_Scan(gameModule, game_ver_text);

		if (version_number) {
			long GameVersion = strtol((const char*)version_number + strlen(game_ver_text), nullptr, 10);
			TO1_LOG("[gameinfo] Game version: %ld\n", GameVersion);
		}
		else {
			TO1_LOG("[gameinfo] WARNING: BUILD_NUMBER string not found.\n");
		}

		return true;
	}
}
