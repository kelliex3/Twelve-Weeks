#pragma once

#include "engine/memory.hpp"
#include "sid/sid.hpp"
#include "structs.hpp"

#include <bit>
#include <cstdint>
#include <string>
#include <functional>
#include <type_traits>
#include <vector>

#define SCRIPT_FUNC_INDEX 1

#include <algorithm>
#include <cstddef>

template<std::size_t N>
struct fixed_string {
    char data[N]{};
    constexpr fixed_string(const char (&str)[N]) {
        std::copy_n(str, N, data);
    }
    constexpr operator const char*() const { return data; }
};

struct DcValue
{
	uint64_t val[64];
};

struct ScriptCFunc
{
	void** c_func_caller;
	void* c_script_func;
};

enum BoxedValueType : uint16_t {
	kInvalid = 0, // 0x00
	kBool = 1, // 0x01
	kI32 = 2, // 0x04
	kU32 = 3, // 0x04
	kI64 = 4, // 0x08
	kU64 = 5, // 0x08
	kF32 = 6, // 0x04
	kSymbol = 7, // 0x08
	kTimeFramePOD = 8, // 0x08
	// 9 unused
	kNetTrackerId = 10, // 0x04
	kProcessHandleMutable = 11, // 0x10
	kProcessHandle = 12, // 0x10
	kPtr = 13, // 0x08
	kPoint = 14, // 0x10
	kVector = 15, // 0x10
	kQuat = 16, // 0x10
	kLocator = 17, // 0x20
	kBoundFrame = 18, // 0x50
	kStringCopy = 19, // 0x08
	// 20 unused
	kArray = 21, // 0x08
	kColor = 22, // 0x10
	kSsAnimateParams = 23, // 0x1A0
	kNdAttackInfo = 24 // 0x20
};

enum Faction : uint64_t {
	Object = 0,
	Threat = 1,
	Player = 2,
	Militia = 3,
	Scar = 4,
	Slaver = 5,
	Infected = 6,
	Buddy = 7,
	Ally = 8,
	Civilian = 9,
	NetFfa = 10,
	NetTeam0 = 11,
	NetTeam1 = 12,
	NetTeam2 = 13,
	NetTeam3 = 14
};

enum LookAimMode : uint64_t {
	LookAimNone = 0,
	LookAimNatural = 1,
	LookAimBuddyPlayer = 2,
	LookAimBuddy = 3,
	LookAimCopyLookAt = 4,
	LookAimCurrentFacing = 5,
	// Invalid = 6, // "Use npc-set-look-aim-mode-object for type locatable"
	LookAimPath = 7,
	// Invalid = 8, // "Use npc-set-look-aim-mode-point for type point-ws"
	LookAimSearch = 9,
	LookAimSniper = 10,
	LookAimSplitBetweenAiEntities = 11,
	LookAimSplitBetweenRandomPointsInVisionCone = 12,
	LookAimTargetEntity = 13,
	LookAimTargetEntityOrPath = 15,
	// Invalid = 16, // "Use npc-set-look-aim-mode-vector for type vector-ws"
	LookAimWatch = 17,
};

// ProjectFEDRA\debug\lists\t2r-skills.txt
enum class NpcSkills : uint64_t {
	infected_explode = 0x01,
	infected_throw = 0x02,
	infected_ambush = 0x03,
	infected_investigate = 0x04,
	infected_chase = 0x05,
	infected_disengage = 0x06,
	infected_ranged_combat = 0x07,
	infected_search = 0x08,
	infected_wander = 0x09,
	infected_stir = 0x0A,
	infected_sleep = 0x0B,
	uncover = 0x0C,
	search = 0x0D,
	pursue = 0x0E,
	flank = 0x0F,
	surprise = 0x10,
	ellie_boss_throw_grenade = 0x11,
	ellie_boss_surprise = 0x12,
	ellie_boss_uncover = 0x13,
	investigate = 0x14,
	observe = 0x15,
	panic = 0x16,
	dog_panic = 0x17,
	dog_search = 0x18,
	handle_leash = 0x19,
	mourn = 0x1A,
	defend = 0x1B,
	defend_search = 0x1C,
	close_range = 0x1D,
	combat = 0x1E,
	melee = 0x1F,
	ride_horse_combat = 0x20,
	buddy_snowball = 0x21,
	buddy_combat = 0x22,
	buddy_scripted_throw = 0x23,
	sneak = 0x24,
	advance = 0x25,
	stand_ground = 0x26,
	throw_grenade = 0x27,
	avoid_vehicle = 0x28,
	stick_drive = 0x29,
	spline_network = 0x2A,
	turret = 0x2B,
	vehicle = 0x2C,
	heal = 0x2D,
	ellie_boss_sneak = 0x2E,
	ridden = 0x2F,
	horse_flee_fire = 0x30,
	horse_move_from_bad_spot = 0x31,
	heel_follow = 0x32,
	explore = 0x34,
	sniper_watch = 0x36,
	flee_infected = 0x37,
	open_door = 0x38,
	track = 0x39,
	patrol = 0x3A,
	flock = 0x3C,
	knocked_down = 0x3E
};

enum AssetType : uint32_t {
	Actor = 0,
	Level = 1,
	SoundBank = 2,
	IrPack = 3,
	Music = 4,
	Speech = 5,
	Movie = 6,
	Text_ = 7,
	Misc = 8,
	Gameplay = 9,
	Dc = 10,
	Shaders = 11,
	PipelineExecutables = 12,
};

enum MenuStack : StringId64 {
	main = 0x1f5962a2ce9803c8,
	hudMenuStack = 0xeb22d2a512e236b3,
	ingame = 0x3f4ec3ecb1cadeb4
};

namespace DcManager
{
	extern uint64_t ScriptPrintAddr;
	extern uintptr_t SkeletonOverrideAddr1;
	extern uintptr_t SkeletonOverrideAddr2;

	TYPEDEF_EXTERN_FUNCTION_PTR(const DcValue*, Lookup_, StringId64 Hash, void* unk);
	const DcValue* Lookup(StringId64 Hash);

	TYPEDEF_EXTERN_FUNCTION_PTR(DcValue*, LookupInModule_, StringId64 Symbol, StringId64 Module);
	DcValue* LookupInModule(StringId64 Symbol, StringId64 Module);

	TYPEDEF_EXTERN_FUNCTION_PTR(bool, LoadActorByName_, char const* pActorName, AssetType pAssetType, bool sync);
	bool LoadActorByName(char const* pActorName, AssetType pAssetType, bool sync);

	TYPEDEF_EXTERN_FUNCTION_PTR(void, UnloadActorByName_, char const* pActorName);
	void UnloadActorByName(char const* pActorName);

	TYPEDEF_EXTERN_FUNCTION_PTR(void, LoadLevelSet_, char const* levelSetName);
	void LoadLevelSet(char const* levelSetName);

	TYPEDEF_EXTERN_FUNCTION_PTR(uint64_t, LookupObject_, void* ctx, int printError, int consumeArg);
	uint64_t LookupObject(StringId64 nameHash);
}

namespace DC
{
	namespace detail
	{
		template<fixed_string functionName>
		uint64_t Invoke(int argCount, DcValue args)
		{
			constexpr StringId64 funcHash = SID(functionName.data);
			const DcValue* funcLookupPtr = DcManager::Lookup(funcHash);
			if (!funcLookupPtr)
				return -1;

			uintptr_t FuncAddr = funcLookupPtr->val[SCRIPT_FUNC_INDEX];
			FUNCTION_PTR(DcValue, DcCallFunc, FuncAddr, int argc, DcValue* argv_first, uint64_t argv_sec);
			DcValue ret = DcCallFunc(argCount, &args, 0);
			return ret.val[0];
		}

		template<fixed_string functionName>
		uint64_t InvokeV2(int argCount, DcValue args)
		{
			(void)argCount;
			constexpr StringId64 funcHash = SID(functionName.data);
			const DcValue* funcLookupPtr = DcManager::Lookup(funcHash);
			if (!funcLookupPtr)
				return -1;

			uintptr_t FuncAddr = funcLookupPtr->val[2];
			using FnT = DcValue* (__fastcall*)(DcValue*,
				uint64_t, uint64_t, uint64_t, uint64_t,
				uint64_t, uint64_t, uint64_t, uint64_t);
			FnT fn = reinterpret_cast<FnT>(FuncAddr);

			DcValue ret{};
			fn(&ret,
				args.val[0], args.val[1], args.val[2], args.val[3],
				args.val[4], args.val[5], args.val[6], args.val[7]);
			return ret.val[0];
		}

		constexpr size_t kMaxArgs = sizeof(DcValue) / sizeof(uint64_t);

		template <typename T>
		constexpr uint64_t ToSlot(T value)
		{
			if constexpr (std::is_same_v<T, float>)
				return std::bit_cast<uint32_t>(value);
			else if constexpr (std::is_same_v<T, double>)
				return std::bit_cast<uint32_t>(static_cast<float>(value));
			else
				return static_cast<uint64_t>(value);
		}

		template <typename... Args>
		DcValue Pack(Args... args)
		{
			static_assert(sizeof...(Args) <= kMaxArgs,
				"DC::Call: more arguments than DcValue has slots");

			return DcValue{ { ToSlot(args)... } };
		}
	}

	template <fixed_string functionName, typename OutT = uint64_t, typename... Args>
	std::conditional_t<std::is_same_v<OutT, uint64_t>, uint64_t, OutT*> Call(Args... args)
	{
        const uint64_t raw = detail::Invoke<functionName>(static_cast<int>(sizeof...(Args)), detail::Pack(args...));
        if constexpr (!std::is_same_v<OutT, uint64_t>)
            return reinterpret_cast<OutT*>(raw);
        else
            return raw;
	}

	template <fixed_string functionName, typename... Args>
	uint64_t Call_V2(Args... args)
	{
		return detail::InvokeV2<functionName>(static_cast<int>(sizeof...(Args)), detail::Pack(args...));
	}

	// A TYPED binding (ext skills.c / afk.c): rec+8 holds a descriptor (the DcCallFunc the
	// interpreter uses), rec+0x10 the C++ impl called directly -- arguments in RCX/RDX/R8/R9,
	// the result in RAX. DC::Call jumps into rec+8 and would crash on these ("its rec+8 is a
	// descriptor", ext PlayerMenuActive). Use this only for the typed-binding natives
	// (menu2-player-menu-active?, the upgrade natives, ...); a missing native answers -1.
	template <fixed_string functionName, typename RetT = uint64_t, typename... Args>
	RetT CallTyped(Args... args)
	{
		constexpr StringId64 funcHash = SID(functionName.data);
		const DcValue* rec = DcManager::Lookup(funcHash);
		if (!rec || !rec->val[2])
			return static_cast<RetT>(-1);
		using Fn = RetT(__fastcall*)(Args...);
		return reinterpret_cast<Fn>(rec->val[2])(args...);
	}

	template<fixed_string functionName>
	uint64_t GetFunctionAddress()
	{
		constexpr StringId64 funcHash = SID(functionName.data);
		const DcValue* funcLookupPtr = DcManager::Lookup(funcHash);
		if (funcLookupPtr)
			return funcLookupPtr->val[SCRIPT_FUNC_INDEX];

		TO1_LOG("ERROR: This DC function does not exist.\n");
		return -1;
	}

	template<fixed_string functionName>
	uint64_t PrintFunctionAddress()
	{
		constexpr StringId64 funcHash = SID(functionName.data);
		const DcValue* funcLookupPtr = DcManager::Lookup(funcHash);
		if (funcLookupPtr)
		{
			TO1_LOG("%s Returned: 0x%016llx\n", functionName.data,
				static_cast<unsigned long long>(funcLookupPtr->val[SCRIPT_FUNC_INDEX]));
			return 0;
		}

		TO1_LOG("ERROR: This DC function does not exist.\n");
		return -1;
	}
	bool JOYPAD_COMMAND_ACTIVE(StringId64 command);

}

float    floatFromUint64(uint64_t value);

namespace MEMORY
{
	uint64_t ALLOC_VECTOR(Vector3 xyz);
	uint64_t ALLOC_QUATERNION(Quaternion xyzw);
	uint64_t ALLOC_BOUND_FRAME(Vector3 xyz, Quaternion xyzw);
	uint64_t ALLOC_BOUND_FRAME_OBJECT(uint64_t objectA, uint64_t objectB, int unk_0);
	uint64_t ALLOC_ARRAY(uint64_t unkFlag1, uint64_t unkFlag2, uint64_t unkFlag3);
	uint64_t ALLOC_STRING(const char* text);
	uint64_t ALLOC_COLOR(float r, float g, float b, float a);
	uint64_t NEW_BOXED_VALUE(BoxedValueType type, uint64_t value);
	uint64_t ALLOC_SPAWN_PLAYER(StringId64 spawner);
	bool LOAD_ACTOR(const char* actorName, AssetType assetType, bool sync);
	void UNLOAD_ACTOR(const char* actorName);
	void LOAD_LEVEL_SET(char const* levelSetName);
	void LOAD_LEVEL(char const* levelName);
	void UNLOAD_LEVEL(char const* levelName);
}

namespace LOCATOR
{
	uintptr_t GET_LOCATOR(StringId64 entity);
	uintptr_t GET_JOINT_LOCATOR(StringId64 entity, StringId64 joint);
	uintptr_t GET_SPAWNER_LOCATOR(StringId64 entity);
	float GET_LOCATOR_DISTANCE(uintptr_t locator1, uintptr_t locator2);
	Vector3 GET_LOCATOR_POSITION(uintptr_t locator);
	uintptr_t GET_LOCATOR_POSITION_PTR(uintptr_t locator);
	Vector3 GET_POSITION_IN_FRONT_OF(StringId64 entity, float distanceInFront);
	Quaternion GET_LOCATOR_ROTATION(uintptr_t locator);
	Transform GET_TRANSFORM(StringId64 entity);
	void SET_OBJECT_LOCATOR(StringId64 entity, uintptr_t locatorOrBoundFrame); // a bound frame or a locator pointer. A locator (e.g. a raycast locator) copies more than transform - it also carries what the raycast attached to.
	uintptr_t OFFSET_LOCATOR(uintptr_t locator, uintptr_t offset); // offset = ALLOC_VECTOR with the values to offset the locator by
	uintptr_t KICK_RAYCAST(uintptr_t castFrom, uintptr_t castFromOffset, StringId64 castTowards, uint64_t unkInt); // castFrom = where the cast is shot from | castFromOffset = offset-locator result tuning how far the cast reaches | castTowards = direction | unkInt = unknown
	uintptr_t GET_CAST_LOCATOR(uintptr_t raycast);
	bool IS_WITHIN_RADIUS(const Vector3& pointToCheck, const Vector3& center, double radius);
}

namespace OBJECT
{
	const char* GET_ART_GROUP(StringId64 entity);
	const char* GET_OBJECT_SKELETON(StringId64 entity);
	const char* GET_OBJECT_ANIM(StringId64 entity);
	const char* GET_OBJECT_ANIM_STATE(StringId64 entity);
	const char* GET_OBJECT_GESTURE(StringId64 entity);
	StringId64 GET_OBJECT_ANIM_SID(StringId64 entity);
	StringId64 GET_OBJECT_ANIM_STATE_SID(StringId64 entity);
	void HIGHLIGHT(StringId64 object, float r, float g, float b, float a);
}

namespace HUD
{
	bool IS_READ_PAGE_ACTIVE();
	void SHOW_READ_PAGE(StringId64 cipher);
	void SET_READ_PAGE_VISIBILITY(bool toggle);
	float GET_BLACK_FADE(); // Returns 1.0f if screen is black
	void FADE_TO_BLACK(float timeToFade, float blackHold);

	void SHOW_NOTIFICATION(const char* message, float durationSeconds, int messageType = 1, StringId64 customCipherId = SID("CUSTOM_CIPHER_MESSAGE")); // Requires modification of text/eng.common and uke.common (uk english) to add CUSTOM_CIPHER_MESSAGE as an entry.
	void SHOW_NOTIFICATION_CIPHER(StringId64 cipherId, float durationSeconds, int messageType = 1);
	// messageType:
	//		0 = debug (shows at the bottom of the screen like a subtitle)
	//		1 = normal (normal notification that fades in and shows on the left)
	//		2 = player (fades in and shows next to the player)
	//		6 = debug with fade (shows at the bottom of the screen like a subtitle)
}

namespace ANIMATION
{
	void ANIMATE(StringId64 entity, StringId64 animation, uint64_t allocArray);
	void GESTURE(StringId64 entity, StringId64 gesture);
}

namespace TIME
{
	bool IS_GAME_PAUSED();
	float GET_DELTA_TIME();
	void SET_CLOCK_MULTIPLIER(float scale);
}

namespace AUDIO
{
	void PLAY_SOUND(StringId64 sfxName);
	void PLAY_SOUND_AT_POSITION(StringId64 sfxName, uint64_t positionPtr);
	void PLAY_VOICE_LINE(StringId64 npc, StringId64 voiceLineID);// from vox-igc
	void AUDIO_GAMEPLAY_EVENT(StringId64 eventId, uint64_t position, StringId64 causeEntity); // causeEntity = entity that caused the event
	void PLAY_MUSIC_PLAYLIST(StringId64 playlistId);
	void STOP_MUSIC_PLAYLIST();
	void SET_MASK_FLAG(StringId64 entity, StringId64 maskFlag);
	void CLEAR_MASK_FLAG(StringId64 entity);
}

namespace DEBUG
{
	void PLAY_TASK(StringId64 taskNode, StringId64 taskSubNode1, StringId64 taskSubNode2);
	bool IS_TASK_ACTIVE(StringId64 taskNode, StringId64 taskSubNode);
	void DISABLE_VIS(); // novis, but needs to be looped for map to be visible
	bool IS_DEV_MENU_OPEN();
	bool RELOAD_MODULE(StringId64 moduleName); // Also can be used for loading modules. If it cant reload it, it will load it. You will also see some text pop up in debug rendering when calling this.
}

namespace SYSTEM
{
	Vector3 QUAT_TO_EULER(Quaternion quaternion);
	Quaternion EULER_TO_QUAT(Vector3 euler);
	Vector2 WORLD_TO_SCREEN(Vector3 position);
	uint64_t GET_CURRENT_FRAME_COUNT();
	float UINT64_TO_FLOAT(uint64_t value);
	float GET_SCREEN_WIDTH();
}

namespace PLAYER
{
	bool IS_IN_STATE(StringId64 state);
	int GET_HEALTH();
	int GET_MAX_HEALTH();
	void SET_HEALTH(int health);
	float GET_HEAL_PROGRESS(); // 0..1 while a health kit is being used, 0 otherwise
}

namespace CAMERA
{
	uint64_t GET_CAMERA_LOCATOR();
	void SET_OVERLAY(StringId64 overlay);
	void OVERRIDE_FOV(float fov);
	void SET_CAMERA_POSITION(Vector3 pos, Quaternion rot);
}

namespace ENTITY
{
	Spawner* GET_SPAWNER(const char* spawnerName);
	StringId64 SPAWN_OBJECT(StringId64 object, StringId64 processType);
	uint64_t SPAWN_EXPLOSION(StringId64 explosionId, Vector3 position);
	StringId64 SPAWN_ENTITY(StringId64 spawnerName, Vector3 position, Quaternion rotation, StringId64 customLook = 0);
	void PRINT_ENTITY_INFO(StringId64 entity);
	bool KILL_ENTITY(StringId64 entity);
	float GET_HEALTH(StringId64 entity);
	void SET_HEALTH(StringId64 entity, float health);
	void ANIMATE(StringId64 entity, StringId64 animation, uint64_t allocArray);
	void SET_VISIBLE(StringId64 entity, bool visible);
	void SET_SCALE(StringId64 entity, float scale);
	void SET_COLLISION(StringId64 entity, bool collision);
	const char* GET_PROCESS_TYPE(StringId64 entity);
	void SET_UNSHOOTABLE(StringId64 entity, bool unshootable);
	bool DOES_EXIST(StringId64 entity);
	void ATTACH(StringId64 childEntity, StringId64 parentEntity, StringId64 attachPoint);
}

namespace GAMEPLAY
{
	bool IS_ANY_ENEMY_NEARBY(uintptr_t locator, float radius);
}

namespace NPC
{
	const char* GET_FACTION_NAME(StringId64 npc);
	Faction GET_FACTION_ID(StringId64 npc);
	bool CAN_SEE(StringId64 npc, StringId64 target);
	void START_SEARCH(Faction faction);
	void SET_FACTION_ID(StringId64 npc, Faction faction);
	void HIGHLIGHT(StringId64 npc, float r, float g, float b, float a);
	bool IS_ALIVE(StringId64 npc);
}

namespace MELEE
{
	const char* GET_CURRENT_MELEE_ACTION_NAME(StringId64 entity);
	StringId64 GET_CURRENT_MELEE_ACTION(StringId64 entity);
}

namespace FEDRA
{
	bool IS_PLAYER_AT_MAIN_MENU();
	bool IS_PLAYER_AT_BASE();
	std::vector<StringId64> GET_ALL_NPCS();
	std::vector<StringId64> GET_ALL_ENEMIES();
	std::vector<StringId64> GET_ALL_BUDDIES();
	void SWAP_LOOK_ID(StringId64 lookId);
	void LOAD_LOOK_ASSETS(const std::string& lookName, bool isRootCall = true);
	void CHANGE_MODEL(const char* lookId); 	// NOTE: starlight-mod-menu level must be loaded in memory for CHANGE_MODEL to work
}

namespace SL
{
	void REQUEST_KEYBOARD_INPUT(std::function<void(const std::string&)> callback);
	void CLEAN_MEMORY();
}

namespace GUI2
{
	StringId64 SPAWN_WIDGET(StringId64 canvas, const char* widgetPath);
	void SET_WIDGET_TEXT(StringId64 path, const char* text);
	void SET_WIDGET_IMAGE(StringId64 wPath, const char* imagePath);
	void SET_WIDGET_VISIBLE(StringId64 wPath, bool visible);
	bool WIDGET_EXISTS(StringId64 widget);
}

namespace MENU2
{
	void PUSH_PAGE(MenuStack stack, const char* pagePath);
	StringId64 GET_PATH(const char* widget, const char* path);
	StringId64 GET_TOP_PAGE_IN_STACK(MenuStack stack);
	StringId64 GET_PATH_IN_CURRENT_PAGE(const char* path, MenuStack stack);
	bool IS_PAGE_ON_STACK(MenuStack stack, StringId64 page);
}

namespace engine
{
	bool InitScript(HMODULE gameModule);
}
