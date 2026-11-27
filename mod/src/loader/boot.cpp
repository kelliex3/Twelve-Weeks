#include "boot.hpp"
#include "config.hpp"

#include "engine/patterns.hpp"
#include "engine/memory.hpp"
#include "engine/pad.hpp"
#include "engine/gameinfo.hpp"
#include "engine/archive.hpp"
#include "engine/script.hpp"
#include "engine/draw.hpp"
#include "engine/debugdraw.hpp"
#include "engine/framecap.hpp"
#include "engine/frame.hpp"
#include "engine/text-input.hpp"
#include "engine/menu-input.hpp"
#include "engine/render-camera.hpp"
#include "engine/vfx.hpp"
#include "sid/sidbase.hpp"
#include "menu/net-menu.hpp"
#include "menu/loadseq.hpp"
#include "menu/sendoff.hpp"
#include "menu/titlebg.hpp"
#include "gameplay/barrel-check.hpp"
#include "gameplay/stun-lock.hpp"
#include "net/ui/notice.hpp"
#include "net/ui/kick-note.hpp"
#include "net/ui/launch-notice.hpp"
#include "net/ui/kill-feed-icons.hpp"
#include "net/ui/prompt-anim.hpp"
#include "net/ui/nrui.hpp"
#include "net/ui/mfnr.hpp"
#include "net/ui/pmunlock.hpp"
#include "net/ui/sbteam.hpp"
#include "net/ui/scopeglint.hpp"
#include "net/ui/notify.hpp"
#include "net/ui/denynote.hpp"
#include "net/ui/death-ui.hpp"
#include "net/game/glass.hpp"
#include "engine/pcm.hpp"
#include "net/audio/shivsting.hpp"
#include "net/audio/alertsting.hpp"
#include "net/audio/stun-tone.hpp"
#include "net/audio/crossbow-hud.hpp"
#include "net/audio/stun-cough.hpp"
#include "net/audio/downed-vox.hpp"
#include "net/audio/exec-beg.hpp"
#include "net/audio/bomb-roll.hpp"
#include "net/audio/death-theme.hpp"
#include "net/audio/downed-music.hpp"
#include "net/audio/music-tier.hpp"
#include "net/audio/music-sight.hpp"
#include "net/audio/near-miss.hpp"
#include "net/audio/exec-music.hpp"
#include "net/audio/xcam-bridge.hpp"
#include "net/audio/bladesfx.hpp"
#include "net/game/player-status.hpp"
#include "net/match/net-player.hpp"
#include "net/match/afk.hpp"
#include "net/match/kick-state.hpp"
#include "net/match/melee-bridge.hpp"
#include "net/match/melee-pullout-bridge.hpp"
#include "net/match/downed-bridge.hpp"
#include "net/gift/gift.hpp"
#include "net/match/match-liveness.hpp"
#include "net/social/friends13.hpp"
#include "net/chat/chat.hpp"
#include "net/social/rank-badge.hpp"
#include "net/social/name-rim.hpp"
#include "net/social/search-rows.hpp"
#include "net/social/clan-page-data.hpp"
#include "net/online/clan-state.hpp"
#include "net/ui/postmatch-screens.hpp"
#include "net/ui/achieve.hpp"
#include "net/ui/choose.hpp"
#include "net/ui/customize.hpp"
#include "net/ui/customize-challenges.hpp"
#include "net/ui/customize-compose.hpp"
#include "net/ui/customize-stage.hpp"
#include "net/game/net-direct.hpp"
#include "net/online/jip-report.hpp"
#include "net/online/requeue.hpp"
#include "net/match/backpack.hpp"
#include "net/match/bloodbuild.hpp"
#include "hwid.hpp"
#include "log.hpp"

#include <safetyhook.hpp>
#include <cstdio>
#include <cstring>
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
	static bool s_extLoaded = false;   // set once by TickExt, read by ExtLoaded

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
			s_extLoaded = s_tick != nullptr;
			TO1_LOG("[boot] t2r-ext: dll %s, t2r_tick %s\n",
				s_ext ? "loaded" : "not found", s_tick ? "resolved" : "missing");
		}
		if (s_tick) s_tick();
	}

	bool ExtLoaded()
	{
		// TickExt() runs inside UpdateMenu (from NetTick) before every native
		// module tick, so this is the settled answer for the current frame.
		return s_extLoaded;
	}

	// The ext called stuntone_stunned() / the stun-cough own branch directly from
	// stun_local. The native stun producer publishes the same start through
	// gameplay::stun_lock::Until(); a rising edge is the once-per-lock confirmation.
	static void BridgeLocalStunAudio()
	{
		if (const NetPlayer* p = NetPlayers::Local()) {
			const char* look = p->LookOrDefault();
			net::audio::stun_cough::SetOwnFemale(look && std::strstr(look, "female") != nullptr);
		}
		static uint64_t lastUntil = 0;
		const uint64_t until = gameplay::stun_lock::Until();
		if (until && until != lastUntil) {
			net::audio::stun_tone::Stunned();
			net::audio::stun_cough::NoteOwnStun(
				until > gameplay::stun_lock::kTotalMs
					? until - gameplay::stun_lock::kTotalMs : 0);
		}
		lastUntil = until;
	}

	static void FrameTick(SafetyHookContext&) {
		engine::frame::Tick();    // g_frame / g_t60 clock (ported from tools/ext/t2r-ext.c)
		const uint64_t now = ::GetTickCount64();
		engine::text_input::BeginFrame();  // one hook-free keyboard sampler for chat + friends
		engine::menu_input::BeginFrame(now);  // one hook-free menu sampler (kick / launch notices)
		net::match::melee_bridge::Install();  // installs the wire / hit / dodge sinks (kEnabled == true)
		net::match::melee_bridge::Tick();     // hitrel / dodge / hitreact / exchange cores + local dodge press
		net::audio::xcam_bridge::Init();      // gated dispatch sink for xcam -> exec / shiv audio
		net::audio::music_sight::Install();   // gated native MusicSlotSeen / Cos samplers
		engine::pad::Tick();      // virtual pad: learned input bits + injection heartbeat (ported from tools/ext/padinject.c)
		net::ui::prompt_anim::Tick(now);    // one shared pad / key snapshot for self-drawn prompts
		net::match::liveness::Tick();  // one shared match-end latch (ext g_wasLive), before all consumers
		net::ui::pmunlock::Tick(now);  // match-start unlock snapshot (before the achievement reply)
		net::match::downed_bridge::Install(); // native sink; yields entirely while the ext is loaded
		net::match::downed_bridge::Tick();    // downed state + revive reach, after shared input is settled
		net::match::melee_pullout_bridge::Install(); // native family-5 pull-out owner (ext wins when loaded)
		net::match::melee_pullout_bridge::Tick();    // shared F / melee latch + counter edge + wire
		net::gift::Install();             // native gift stock / hand-out owner (ext wins when loaded)
		net::gift::Tick();                // stock broadcast, ACK, inventory and prompt feeds
		net::audio::bomb_roll::Install(); // one-shot throw-projectile hook for bomb impact / roll audio
		{
			static bool hooksInstalled = false;
			if (!hooksInstalled) {
				net::match::kick::InstallNoticeHooks();  // native g_kickMs: notice kick dedupe
				net::ui::kick_note::InstallNoticeHooks();  // the kick box takes over the toast line
				net::friends13::InstallNoticeHooks();  // route kind 'p' notice lines to friends13
				hooksInstalled = true;
			}
		}
#ifdef TO1_DEBUG
		NetMenuTick();            // net debug menu (toggle/input/render)
#endif
		NetTick();                // the entire MP driver (also ticks t2r-ext via UpdateMenu)
		net::match::melee_pullout_bridge::PostNetTick(); // SP camera / pair placement after replication
		net::audio::xcam_bridge::Tick(); // paired-move victim edge -> exec music / shiv sting (ext xcam_start routing)
		// Native menu / HUD owners. Each yields while the legacy ext is loaded so
		// the ext cannot push the same page or sample the same click twice.
		net::ui::launch_notice::Tick(now);  // once-per-run notice under the entry title
		net::ui::kick_note::Tick(now);      // admin / AFK / packet-loss kick box
		menu::loadseq::Tick(now);           // TWELVE WEEKS entry sequence (connecting page)
		net::ui::death_ui::Tick(now);       // victim death screen (ported from tools/ext/death_ui.c)
		net::audio::death_theme::Tick(now, net::ui::death_ui::Phase(), net::ui::death_ui::Final()); // death drone (cut on respawn / final)
		net::ui::killfeed::Tick(now);       // icon kill feed (hud-root widgets)
		net::ui::sbteam::Tick(now);         // place-coloured scoreboard bands (no block swap)
		net::direct::Tick();      // direct melee path via the Steam helper (ported from tools/ext/netdirect.c)
		net::notice::Tick();      // server notices (ported from tools/ext/notice.c)
		net::scopeglint::Tick();  // scope glint (ported from tools/ext/scopeglint.c)
		net::notify::Tick();      // styled HUD notification (ported from tools/ext/notify.c)
		net::denynote::Tick();    // picker refusal panel (ported from tools/ext/denynote.c)
		net::glass::Tick();       // synced breakable glass (ported from tools/ext/glass.c)
		net::audio::music_sight::Tick(); // gated native sight / cone samplers (before the tier consumes them)
		net::audio::music_tier::Tick(); // combat music tier STATE (ported from tools/ext/t2r-ext.c)
		gameplay::stun_lock::Tick(::GetTickCount64(), net::player_status::MatchLive()); // stun end / grace
		BridgeLocalStunAudio();  // native stun start -> confirmation tone + own cough cadence
		net::audio::shivsting::Tick();  // shiv victim shock sting + drums (ported from tools/ext/shiv_sting.c)
		net::audio::near_miss::Tick();  // immersion shot-ray whizz -> alert sting (before the sting's delayed play)
		net::audio::alertsting::Tick(); // alert sting (ported from tools/ext/alert_sting.c)
		net::audio::stun_tone::Tick();  // stun concussion bed / unconfirmed fade (alert_sting.c siblings)
		net::audio::bomb_roll::Tick();  // watched bomb positions -> impact / roll waves
		net::audio::crossbow_hud::Tick(); // bolt impact / pulse / pulled-out HUD sounds
		net::audio::stun_cough::Tick(); // our stun cough cadence + peer puppet coughs
		net::audio::downed_vox::Tick(); // downed groans / breaths + headshot gargles (after downed state)
		net::audio::exec_beg::Tick(); // victim pleas / shot cuts (ducks exec music before its tick)
		net::audio::exec_music::Tick(::GetTickCount64(), net::player_status::MatchLive()); // victim cue timing
		net::audio::downed_music::Tick(now); // adaptive downed layers / death tail / sting duck
		net::audio::bladesfx::PendingTick();  // blade cue post-delay (ported from tools/ext/blade_sfx.h)
		net::afk::Tick();         // anti-AFK watchdog (ported from tools/ext/afk.c)
		net::jip::Tick();         // JIP 0xE3 clock report (after afk: carries this frame's idle trailer)
		net::friends13::Tick();   // in-game FRIENDS / PARTY panel + party gate (after liveness, before requeue)
		net::chat::Tick(::GetTickCount64()); // in-game chat (0xC7 peer lines + 0xD8 chat subs)
		net::social::search_rows::Tick(); // g_srchTab / g_srchName mirror (read-only, observer on mod text writes)
		net::rank_badge::Tick(::GetTickCount64());  // async 0xBF rank table poll (only while a screen draws a badge)
		net::name_rim::Tick(::GetTickCount64());    // lead-dev pink name rim (role 4, 150 ms poll)
		net::clan::Tick();        // 2013 CLAN state (0xE0/0xE1) + g_pm.pending / g_ev.pending producers
		net::clan::page::Tick();  // 2013 CLAN page data (0xB6/0xB7 sections 1..6)
		net::ui::postmatch::Tick();  // post-match screen latches (ext clan13x / clan-postmatch / clan event / GEAR UP nav / g_mmBack)
		net::ui::achieve::Tick(now);     // 0xA9/0xAA/0xAB client half + clanach/cards (yields to ext)
		net::ui::choose::Tick(now);      // CHOOSE YOUR SIDE widget owner (camera/mixer seams inert)
		// The stage samples the settled page state first so its freecam can
		// suppress this frame's CUSTOMIZE / CHALLENGES input (ext czs_tick order).
		net::ui::customize::stage::Tick(now, net::ui::customize::Active()
			|| net::ui::customize::challenges::Active(),
			net::ui::customize::EditingFaction(), net::ui::customize::CurrentSide());
		net::ui::customize::Tick(now);   // CUSTOMIZE page owner (shared profile, atomic save + NEVT_CUSTOM_LOOK)
		net::ui::customize::challenges::Tick(now); // CUSTOMIZE CHALLENGES page owner (shared save/net/sfx)
		net::ui::customize::compose::Tick(now); // 0x59 outfit cache + composed look2 registry (yields to ext)
		net::ui::mfnr::Tick(now); // match flow: map vote / boosters / engine LOADING screen
		menu::sendoff::Tick(now);  // menu send-off: shared horn t=0, birds, fades and clips
		menu::titlebg::Tick(now);  // title background: Hillcrest camera, FOV and boot cover
		net::ui::nrui::Tick(now); // shared No Return frame glides (after page writers)
		net::requeue::Tick();     // post-match requeue 0xBC poll / WAITING FOR PLAYERS label
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

		// Read-only final render camera observer (ext icam capture). Hooks f+15, past the
		// ext's 15-byte prologue detour, so install order vs t2r-ext.dll does not matter;
		// idempotent inside Install().
		if (!engine::render_camera::Install())
			TO1_LOG("[boot] WARNING: render-camera observer not installed; audio/sight keep their fallback camera.\n");

		if (!engine::archive::Init(gameModule, selfModule)) {
			TO1_LOG("[boot] FATAL: embedded psarc init failed.\n");
			return false;
		}

		if (!engine::InitScript(gameModule)) {
			TO1_LOG("[boot] FATAL: script init failed.\n");
			return false;
		}

		// The stage's compaction hook is deferred until early ticks have let
		// ExtLoaded() settle; storing the module here keeps that first tick cheap.
		net::ui::customize::stage::Install(gameModule);

		// The match-flow LOADING hook uses the same settled-tick install; the
		// module is only stored here so it cannot detour an ext-loaded prologue.
		net::ui::mfnr::Install(gameModule);

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
