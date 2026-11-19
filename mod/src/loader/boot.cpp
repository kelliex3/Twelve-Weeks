#include "boot.hpp"
#include "config.hpp"

#include "engine/patterns.hpp"
#include "engine/memory.hpp"
#include "engine/gameinfo.hpp"
#include "engine/archive.hpp"
#include "engine/script.hpp"
#include "engine/draw.hpp"
#include "engine/debugdraw.hpp"
#include "engine/framecap.hpp"
#include "engine/frame.hpp"
#include "engine/vfx.hpp"
#include "sid/sidbase.hpp"
#include "menu/net-menu.hpp"
#include "gameplay/barrel-check.hpp"
#include "net/ui/notice.hpp"
#include "net/ui/scopeglint.hpp"
#include "net/ui/notify.hpp"
#include "net/ui/denynote.hpp"
#include "net/game/glass.hpp"
#include "engine/pcm.hpp"
#include "net/audio/shivsting.hpp"
#include "net/audio/alertsting.hpp"
#include "net/audio/bladesfx.hpp"
#include "net/match/afk.hpp"
#include "net/match/backpack.hpp"
#include "net/match/bloodbuild.hpp"
#include "hwid.hpp"
#include "log.hpp"

#include <safetyhook.hpp>
#include <cstdio>
#include <net-common.hpp>

void NetTick();

bool s_debugMenuOpen = false;
int  g_forcePlayerSkel = 0; // 0 default, 1 force ellie anims, 2 force abby anims

namespace boot {

	static SafetyHookInline gSkelType1Hook; // IsCharacterType1 (sub_140DB4030)
	static SafetyHookInline gSkelType2Hook; // IsCharacterType2 (sub_140DB3300)

	static bool __fastcall hk_IsCharacterType1(void* rcx) {
		switch (g_forcePlayerSkel) {
		case 1:  return true;    // ellie
		case 2:  return false;   // abby
		default: return gSkelType1Hook.fastcall<bool>(rcx);
		}
	}

	static bool __fastcall hk_IsCharacterType2(void* rcx) {
		switch (g_forcePlayerSkel) {
		case 1:  return false;   // ellie
		case 2:  return true;    // abby
		default: return gSkelType2Hook.fastcall<bool>(rcx);
		}
	}
	static SafetyHookInline gLevelResetHook;
	static void (__fastcall* gInteractiveLogoutLevel)(uintptr_t level) = nullptr;

	static __int64 __fastcall hk_LevelReset(uintptr_t level) {
		if (level && gInteractiveLogoutLevel && *reinterpret_cast<const uintptr_t*>(level + 0x130) != 0)
			gInteractiveLogoutLevel(level);
		return gLevelResetHook.fastcall<__int64>(level);
	}

	static SafetyHookInline gFreeBackgroundHook;

	struct LevelBackgroundView {
		uint8_t pad[0x130];
		uintptr_t pBackground;
	};

	static __int64 __fastcall hk_FreeBackground(uintptr_t levelMgr, uintptr_t background) {
		if (background && gInteractiveLogoutLevel) {
			LevelBackgroundView view{};
			view.pBackground = background;
			gInteractiveLogoutLevel(reinterpret_cast<uintptr_t>(&view));
		}
		return gFreeBackgroundHook.fastcall<__int64>(levelMgr, background);
	}

	static SafetyHookInline gPrepareLogoutHook;

	static char __fastcall hk_PreparePackageForLogout(uintptr_t packageMgr, uintptr_t package) {
		if (package) {
			const uintptr_t level = *reinterpret_cast<const uintptr_t*>(package + 0x9B0);
			if (level && *reinterpret_cast<const uintptr_t*>(level + 0x130) != 0)
				*reinterpret_cast<uint8_t*>(level + 0x275) = 1;
		}
		return gPrepareLogoutHook.fastcall<char>(packageMgr, package);
	}

	static constexpr size_t kPauseOnFocusLossOffset = 0x4574;
	static constexpr size_t kMuteOnFocusLossOffset = 0x4578;

	static void EnforceFocusLossSettings() {
		auto* info = reinterpret_cast<uint8_t*>(engine::GetGameInfo());
		if (!info) return;

		uint8_t& pauseOnFocusLoss = info[kPauseOnFocusLossOffset];
		uint8_t& muteOnFocusLoss = info[kMuteOnFocusLossOffset];

		static bool logged = false;
		if (!logged) {
			logged = true;
			TO1_LOG("[boot] focus-loss settings: pause=%u mute=%u\n",
				pauseOnFocusLoss, muteOnFocusLoss);
		}

		if (pauseOnFocusLoss) pauseOnFocusLoss = 0;
		if (muteOnFocusLoss) muteOnFocusLoss = 0;
	}

	static bool ApplyBootPatches(HMODULE module) {
		static const unsigned char kTestTasksTrue[] = { 0x40, 0xB6, 0x01 };
		auto testTasks = Utility::WritePatchPattern(module, Patterns::TestTasksMenu, kTestTasksTrue, sizeof(kTestTasksTrue));

		static const unsigned char kNop6[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
		auto levelDef = Utility::WritePatchPattern(module, Patterns::LevelDefDebugBreak, kNop6, sizeof(kNop6));

		if (!levelDef)
			TO1_LOG("[boot] WARNING: failed to patch pLevelDef debug break.\n");
		return testTasks && levelDef;
	}

	static safetyhook::MidHook gFrameHook;

	// was tools/features/ext_loader.py: the byte-patch cave hooked the per-frame MP tick
	// (FUN_180063370 == net::gui2::UpdateMenu) and loaded t2r-ext.dll + called t2r_tick.
	// In source, UpdateMenu calls this at the same point.
	void TickExt()
	{
		using TickFn = void (*)();
		static HMODULE s_ext = nullptr;
		static TickFn  s_tick = nullptr;
		static bool    s_done = false;
		if (!s_done) {
			s_done = true;
			s_ext = LoadLibraryW(L"t2r-ext.dll");
			if (s_ext)
				s_tick = reinterpret_cast<TickFn>(GetProcAddress(s_ext, "t2r_tick"));
			TO1_LOG("[boot] t2r-ext: dll %s, t2r_tick %s\n",
				s_ext ? "loaded" : "not found", s_tick ? "resolved" : "missing");
		}
		if (s_tick) s_tick();
	}

	static void FrameTick(SafetyHookContext&) {
		engine::frame::Tick();    // g_frame / g_t60 clock (ported from tools/ext/t2r-ext.c)
#ifdef TO1_DEBUG
		NetMenuTick();            // net debug menu (toggle/input/render)
#endif
		NetTick();                // the entire MP driver
		net::notice::Tick();      // server notices (ported from tools/ext/notice.c)
		net::scopeglint::Tick();  // scope glint (ported from tools/ext/scopeglint.c)
		net::notify::Tick();      // styled HUD notification (ported from tools/ext/notify.c)
		net::denynote::Tick();    // picker refusal panel (ported from tools/ext/denynote.c)
		net::glass::Tick();       // synced breakable glass (ported from tools/ext/glass.c)
		net::audio::shivsting::Tick();  // shiv victim shock sting (ported from tools/ext/shiv_sting.c)
		net::audio::alertsting::Tick(); // alert sting (ported from tools/ext/alert_sting.c)
		net::audio::bladesfx::PendingTick();  // blade cue post-delay (ported from tools/ext/blade_sfx.h)
		net::afk::Tick();         // anti-AFK watchdog (ported from tools/ext/afk.c)
		net::match::backpack::Tick();  // TAB backpack puppet state (ported from tools/ext/backpack.c)
		net::match::bloodbuild::Tick();  // per-life blood / dirt build-up (ported from tools/ext/bloodbuild.c)
		engine::vfx::RetryTick();     // queued compute-stream particle retries (ported from tools/ext/t2r-ext.c)
		engine::pcm::VoicesUpdate();  // live 3D voice follow (ported from tools/ext/t2r-ext.c)
#ifdef TO1_DEBUG
		engine::debugdraw::Tick();
#endif
		gameplay::barrelcheck::Tick();

		static bool initialized = false;
		if (!initialized) {
			gSkelType1Hook = safetyhook::create_inline(
				(void*)DcManager::SkeletonOverrideAddr1, (void*)hk_IsCharacterType1);
			gSkelType2Hook = safetyhook::create_inline(
				(void*)DcManager::SkeletonOverrideAddr2, (void*)hk_IsCharacterType2);

			DC::Call<"load-level">(MEMORY::ALLOC_STRING("starlight-mod-menu"));

			hwid::Get();

#ifdef TO1_DEBUG
			if (TO1_DEV_LOADDEVLEVEL)
			{
				DEBUG::PLAY_TASK(SID("starlight-levels"), SID("starlight-main-menu"), 0);
			}
#endif

			initialized = true;
		}

		EnforceFocusLossSettings();
		engine::framecap::Tick();
	}

	bool InitCore(HMODULE gameModule, HMODULE selfModule) {

		if (!engine::InitGameInfo(gameModule)) {
			TO1_LOG("[boot] FATAL: game-info init failed.\n");
			return false;
		}

		if (!engine::archive::Init(gameModule, selfModule)) {
			TO1_LOG("[boot] FATAL: embedded psarc init failed.\n");
			return false;
		}

		if (!engine::InitScript(gameModule)) {
			TO1_LOG("[boot] FATAL: script init failed.\n");
			return false;
		}

		if (!engine::InitDebugDraw(gameModule)) {
			TO1_LOG("[boot] FATAL: debug draw init failed.\n");
			return false;
		}

#ifdef TO1_DEBUG
		if (!engine::debugdraw::Init(gameModule))
			TO1_LOG("[boot] WARNING: engine debug-prim init-gate patch failed; 3D debug draw unavailable.\n");
#endif

		gPrepareLogoutHook = safetyhook::create_inline(
			reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(gameModule) + 0x1B98130),
			reinterpret_cast<void*>(hk_PreparePackageForLogout));
		if (!gPrepareLogoutHook)
			TO1_LOG("[boot] WARNING: PreparePackageForLogout hook failed; vegetation slot leak fix inactive.\n");

		gInteractiveLogoutLevel = reinterpret_cast<void(__fastcall*)(uintptr_t)>(
			reinterpret_cast<uintptr_t>(gameModule) + 0x13D75B0);
		gLevelResetHook = safetyhook::create_inline(
			reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(gameModule) + 0x1E99EA0),
			reinterpret_cast<void*>(hk_LevelReset));
		if (!gLevelResetHook)
			TO1_LOG("[boot] WARNING: Level reset hook failed; vegetation slot release on level free inactive.\n");
		gFreeBackgroundHook = safetyhook::create_inline(
			reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(gameModule) + 0x1A50620),
			reinterpret_cast<void*>(hk_FreeBackground));
		if (!gFreeBackgroundHook)
			TO1_LOG("[boot] WARNING: Background release hook failed; vegetation slot release on content-layer drop inactive.\n");

		if (!Utility::memory::InitMemoryMap(gameModule)) {
			TO1_LOG("[boot] FATAL: memory-map table not found.\n");
			return false;
		}

		ApplyBootPatches(gameModule);

		NdGameSdk::Sidbase::InitSidbase(PROJECT_DATA_DIR "/sidbase.bin");

		auto tick = (void*)Utility::FindAndPrintPattern(gameModule, Patterns::CommonGame_CommonGameLoop_GameDebugUpdate);
		if (!tick) {
			TO1_LOG("[boot] FATAL: GameDebugUpdate pattern not found.\n");
			return false;
		}
		gFrameHook = Utility::MakeMidHook(tick, FrameTick,
			Patterns::CommonGame_CommonGameLoop_GameDebugUpdate.name, TO1_NAME("FrameTick"));

		return static_cast<bool>(gFrameHook);
	}
}
