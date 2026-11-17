#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <safetyhook.hpp>

#include "engine/patterns.hpp"

#if NDEBUG
#define BREAK()\
{ \
    if (!IsDebuggerPresent()) \
    { \
        while (true) \
        { \
            Sleep(1000); \
        } \
    } \
    else \
    { \
        __debugbreak(); \
    } \
}
#else
#define BREAK() __debugbreak();
#endif

namespace Assertion
{
    void always_assert_(const char* reason, size_t line_num, const char* msg = nullptr);
    #define always_assert(reason, message) if (reason) { Assertion::always_assert_(#reason, __LINE__, message); BREAK() }
}

#ifdef TO1_DEBUG
#define TO1_NAME(s) L##s
#else
#define TO1_NAME(s) L""
#endif

#define FUNCTION_PTR(return_type, func_name, func_addr, ...) \
    always_assert(func_addr == 0, "Unable to resolve " #return_type " " #func_name "(" #__VA_ARGS__ ")" "!"); \
    typedef return_type (*func_name##_t)(__VA_ARGS__); \
    func_name##_t func_name = (func_name##_t)(void*)(func_addr)

#define TYPEDEF_EXTERN_FUNCTION_PTR(ret_type, func_name, ...) \
    typedef ret_type (*func_name##_ptr)(__VA_ARGS__); \
    extern func_name##_ptr func_name

#define INIT_FUNCTION_PTR(func_name) \
    func_name##_ptr func_name = nullptr

enum class MemoryMapId : uint32_t {
    ALLOCATION_PRX_DIRECT_MEM = 1,
    ALLOCATION_APP_CPU_LARGE_PAGE_MEMORY = 2,
    ALLOCATION_CPU_MEMORY = 3,
    ALLOCATION_APP_GPU_LARGE_PAGE_MEMORY = 4,
    ALLOCATION_GPU_MEMORY = 5,
    ALLOCATION_REQUIRED_PRX_FLEX_MEM = 6,
    ALLOCATION_CODE_AND_DATA = 7,
    ALLOCATION_THREAD_CONTEXTS = 8,
    ALLOCATION_PRX_FLEX_MEM = 9,
    ALLOCATION_APP_FLEX_MEM = 10,
    ALLOCATION_DEV_CODE_AND_DATA = 11,
    ALLOCATION_DEV_PRX_MEM = 12,
    ALLOCATION_DEV_CPU_MEM = 13,
    ALLOCATION_DEV_CPU_LARGE_PAGE_MEMORY = 14,
    ALLOCATION_DEV_GPU_MEM = 15,
    ALLOCATION_SCOPED_TEMP_ALLOCS = 33,
    ALLOCATION_SCREAM_SND_MEM_USE_STREAMING = 124,
    ALLOCATION_SCREAM_SOUND_BANK_HEAP = 125,
    ALLOCATION_AUDIO_EFFECTS = 67,
    ALLOCATION_LOADING_HEAP = 85,
    ALLOCATION_LOADING_HEAP_HIDDEN = 86,
    ALLOCATION_GAMEPLAY_LOADING_HEAP = 87,
    ALLOCATION_GAMEPLAY_LOADING_HEAP_HIDDEN = 88,
    ALLOCATION_PSO_PRE_LOADING_HEAP = 89,
    ALLOCATION_PROCESS_HEAP = 112,
    ALLOCATION_ANIMATION_HEAP = 114,
    ALLOCATION_NDGI_HEAP = 177,
    ALLOCATION_RENDER_POOL_HEAP = 179,
    ALLOCATION_PHYSICS_MEMORY = 108,
    ALLOCATION_ICON = 210,
    ALLOCATION_GUI2 = 164,
    ALLOCATION_GUI2_STREAMING = 165,
    ALLOCATION_GUI2_PRIMITIVE_HEAP = 166,
    ALLOCATION_TEXT2_FONT = 163,
    ALLOCATION_WAVETABLE_SYNTH = 168,
    ALLOCATION_DELTA_BITSTREAM_HEAP = 170,
    ALLOCATION_TAGGED_HEAP = 35,
    ALLOCATION_SINGLE_GAME_FRAME = 37,
    ALLOCATION_DOUBLE_GAME_FRAME = 38,
    ALLOCATION_DOUBLE_GAMEPLAY_UPDATE = 39,
    ALLOCATION_SINGLE_RENDER_FRAME = 180,
    ALLOCATION_GAME_TO_GPU_RING = 181,
    ALLOCATION_DOUBLE_GAME_TO_GPU_RING = 182,
    ALLOCATION_RENDER_TO_GPU_RING = 183,
    ALLOCATION_DOUBLE_RENDER_TO_GPU_RING = 184,
    ALLOCATION_RESOURCES_TO_GPU_RING = 185,
    ALLOCATION_NET_SNAPSHOT_NODE = 186,
    ALLOCATION_CPU_ACCESS_ONLY_RING = 187,
    ALLOCATION_SHADER = 202,
    ALLOCATION_TEXTURE_READING = 222,
    ALLOCATION_APP_CPU_LARGE_PAGE_UNACCOUNTED = 16,
    ALLOCATION_APP_CPU_LARGE_PAGE_FREE = 17,
    ALLOCATION_WRAPPED_MALLOC_POOL = 34,
    ALLOCATION_IK_DATA = 83,
    ALLOCATION_MENU_HEAP = 94,
    ALLOCATION_LEADERBOARD_DATA = 95,
    ALLOCATION_MUSIC_MGR = 96,
    ALLOCATION_PARTICLE_DESC = 105,
    ALLOCATION_PARTICLE_MGR = 106,
    ALLOCATION_PARTICLE_RUNTIME = 107,
    ALLOCATION_PHYSICS_PERSIST = 109,
    ALLOCATION_PRINTF_STATIC_BUFFER_MEM = 110,
    ALLOCATION_TEXTDB_MEM = 146,
    ALLOCATION_DYNAMIC_LIGHT_MEM = 174,
    ALLOCATION_FIOS_MEM_POOL = 80,
    ALLOCATION_PACKAGE_ARRAY = 102,
    ALLOCATION_PACKAGE_PAGE_HEADERS = 104,
    ALLOCATION_CBUFFER_MGR = 70,
    ALLOCATION_FONTS = 81,
    ALLOCATION_LEVEL_DEFS = 90,
    ALLOCATION_LEVEL_STREAMING_MGR = 91,
    ALLOCATION_LVLMGR = 92,
    ALLOCATION_SCRIPT_HEAP = 130,
    ALLOCATION_SCRIPT_RELOC_HEAP = 133,
    ALLOCATION_SCRIPT_ROGUE = 134,
    ALLOCATION_SCRIPT_STACK_FRAMES = 135,
    ALLOCATION_SCRIPT_SCOPED_ALLOCATIONS = 136,
    ALLOCATION_SCRIPT_SYMBOL_TBL = 137,
    ALLOCATION_PROCESS_MGR = 111,
    ALLOCATION_TASK_GRAPH = 145,
    ALLOCATION_MESH_RAY_CASTER = 176,
    ALLOCATION_ARM_PROCESS = 62,
    ALLOCATION_ARM_PUSHBUFFER = 63,
    ALLOCATION_ARM_WORKDATA = 64,
    ALLOCATION_SCREAM_SND_MEM_USE_SYNTH = 120,
    ALLOCATION_SCREAM_SND_MEM_USE_SCRIPTING = 121,
    ALLOCATION_SCREAM_SND_MEM_USE_CCS = 122,
    ALLOCATION_SCREAM_SOUND_BANK_DS_HEAP = 126,
    ALLOCATION_SCREAM_PLUGIN_EVENT_QUEUE_HEAP = 129,
    ALLOCATION_SOUND_VIRTUAL_BANK = 144,
    ALLOCATION_NCA_BATCH = 97,
    ALLOCATION_VIBRATION_MANAGER = 167,
    ALLOCATION_HTTP_POOL_SIZE = 52,
    ALLOCATION_FG_ANIM_DATA = 73,
    ALLOCATION_FG_ANIM_IDENTITY_MATS = 74,
    ALLOCATION_FG_DRAW_DATA = 76,
    ALLOCATION_FG_ANIMCMD_DATA = 75,
    ALLOCATION_FG_KILLER = 79,
    ALLOCATION_HUD = 82,
    ALLOCATION_SAVE_MANAGER = 118,
    ALLOCATION_SAVE_CHECKPOINT = 117,
    ALLOCATION_BODY_MANAGER = 68,
    ALLOCATION_LIP_SYNC_BUFFER = 84,
    ALLOCATION_WATER = 209,
    ALLOCATION_RENDER_GLOBALS = 115,
    ALLOCATION_PSO_CACHE = 220,
    ALLOCATION_REDIS = 56,
    ALLOCATION_REDIS_LARGE = 57,
    ALLOCATION_DMENU_LOWMEM = 71,
    ALLOCATION_AUDIO_VOX = 65,
    ALLOCATION_AUDIO_VOX_TABLES = 66,
    ALLOCATION_SIMPLE_NPC = 142,
    ALLOCATION_VEGETATION = 151,
    ALLOCATION_GAME_CONFIG = 152,
    ALLOCATION_ANIM_STREAMING = 153,
    ALLOCATION_ANIM_CHAIN = 154,
    ALLOCATION_ANIM_DUMMY_INSTANCE = 155,
    ALLOCATION_REGION_TAG_TABLE = 157,
    ALLOCATION_RAIN_MGR = 160,
    ALLOCATION_CINEMATICS = 161,
    ALLOCATION_EVENT_QUEUE_SCRIPT_HEAP = 40,
    ALLOCATION_EVENT_QUEUE_BUCKET_HEAP = 41,
    ALLOCATION_EVENT_QUEUE_MENU_HEAP = 42,
    ALLOCATION_VOX_REQUEST_HEAP = 43,
    ALLOCATION_FRAME_PARAMS = 47,
    ALLOCATION_PREFETCH_BUFFER = 48,
    ALLOCATION_TEXTURE_DICT = 218,
    ALLOCATION_TEXTURE_REPORTER = 211,
    ALLOCATION_GESTURE_CACHE = 156,
    ALLOCATION_RESOURCE_TABLE = 59,
    ALLOCATION_NDRG_HEAP = 178,
    ALLOCATION_ANIM_ALIGN_CACHE = 61,
    ALLOCATION_COMPONENT_MGR = 140,
    ALLOCATION_SUBSYSTEM_HEAP = 141,
    ALLOCATION_STATE_SCRIPT = 138,
    ALLOCATION_SMOKE_TEST = 143,
    ALLOCATION_FIOS_STATIC_PSARC = 216,
    ALLOCATION_FIOS_DYNAMIC_PSARC = 217,
    ALLOCATION_SETTINGS_POOL = 139,
    ALLOCATION_LVLMGR_NAME_CACHE = 93,
    ALLOCATION_PACKAGE_MGR_RING_BUFFER = 103,
    ALLOCATION_NP_POOL_1 = 53,
    ALLOCATION_NP_POOL_2 = 54,
    ALLOCATION_HTTP_CACHE = 51,
    ALLOCATION_PLAYGO_HEAP = 158,
    ALLOCATION_NAV_MESH_MANAGER = 98,
    ALLOCATION_NPC_GLOBALS = 99,
    ALLOCATION_PATHFIND_MANAGER = 100,
    ALLOCATION_FIOS_DEARCHIVER_MEM = 49,
    ALLOCATION_PLAYER_GLOBALS = 147,
    ALLOCATION_RIGIDBODY_DATA = 116,
    ALLOCATION_EVENT_LOG = 159,
    ALLOCATION_CATMULL_ROM = 69,
    ALLOCATION_FACTS = 72,
    ALLOCATION_PHOTO_MODE = 150,
    ALLOCATION_PAD = 60,
    ALLOCATION_WIDGET_MANAGER = 169,
    ALLOCATION_GUI2_GPU = 219,
    ALLOCATION_CLOTH_COMPUTE = 215,
    ALLOCATION_LOD_MANAGER = 172,
    ALLOCATION_DELTA_BITSTREAM = 171,
    ALLOCATION_VOL_PROBE_LIGHT_CACHE = 173,
    ALLOCATION_NETWORK_HEAP = 148,
    ALLOCATION_APP_CPU_UNACCOUNTED = 18,
    ALLOCATION_APP_CPU_FREE = 19,
    ALLOCATION_STATIC_TEXTURE = 197,
    ALLOCATION_TEXTURE = 198,
    ALLOCATION_GUI2_TEXTURE = 201,
    ALLOCATION_TEXTURE_STAGING = 199,
    ALLOCATION_LEVEL_TEXTURE = 200,
    ALLOCATION_GPU_UPLOAD_RING = 188,
    ALLOCATION_CLOTH_RO_BUFFER = 205,
    ALLOCATION_CLOTH_RW_BUFFER = 206,
    ALLOCATION_GPU_READBACK_RING = 189,
    ALLOCATION_EXPOSURE_MAP_BUFFER = 207,
    ALLOCATION_EXPOSURE_MAP_HEIGHTMAPS = 208,
    ALLOCATION_APP_GPU_UNACCOUNTED = 20,
    ALLOCATION_APP_GPU_FREE = 21,
    ALLOCATION_STATIC_RENDER_TARGET = 190,
    ALLOCATION_STATIC_NON_RENDER_TARGET = 191,
    ALLOCATION_STATIC_BUFFER = 192,
    ALLOCATION_DYNAMIC_RENDER_TARGET = 193,
    ALLOCATION_DYNAMIC_NON_RENDER_TARGET = 194,
    ALLOCATION_DYNAMIC_BUFFER = 195,
    ALLOCATION_LUT = 196,
    ALLOCATION_DEFORMER_BUFFER = 204,
    ALLOCATION_RENDER_BUFFER = 203,
    ALLOCATION_LUT_TABLE = 175,
    ALLOCATION_APP_GPU_UNACCOUNTED_2 = 22,
    ALLOCATION_APP_GPU_FREE_2 = 23,
    ALLOCATION_JOB_SYSTEM = 32,
    ALLOCATION_FIOS_RAM_CACHE = 212,
    ALLOCATION_FIOS_PREFETCHER = 214,
    ALLOCATION_APP_FLEX_UNACCOUNTED = 24,
    ALLOCATION_APP_FLEX_FREE = 25,
    ALLOCATION_OVERFLOW_LOADING_HEAP = 233,
    ALLOCATION_DEBUG_LOADING_HEAP = 235,
    ALLOCATION_DEBUG_PSO_PRE_LOADING_HEAP = 237,
    ALLOCATION_OVERFLOW_GAMEPLAY_LOADING_HEAP = 234,
    ALLOCATION_DEBUG_GAMEPLAY_LOADING_HEAP = 236,
    ALLOCATION_DEBUG_SHADER = 238,
    ALLOCATION_DEV_CPU_UNACCOUNTED = 26,
    ALLOCATION_DEV_CPU_FREE = 27,
    ALLOCATION_TAGGED_DEV_HEAP = 36,
    ALLOCATION_DEV_RESOURCES_TO_GPU_RING = 247,
    ALLOCATION_DEBUG_SCREAM_DEBUG = 223,
    ALLOCATION_DEBUG_SCREAM_SERVER_TTY = 224,
    ALLOCATION_DEBUG_SCREAM_SERVER_AUDITIONING = 225,
    ALLOCATION_DEBUG_AUDIO_BIN = 227,
    ALLOCATION_DEBUG_VOX_BIN = 228,
    ALLOCATION_PHYSICS_OVERFLOW_MEMORY = 229,
    ALLOCATION_PROCESS_HEAP_OVERFLOW = 113,
    ALLOCATION_SID_DB = 58,
    ALLOCATION_PROFILE_SAMPLE_BUFFERS = 44,
    ALLOCATION_PROFILE_GPU_SAMPLE_BUFFERS = 45,
    ALLOCATION_PROFILE_CMP_SAMPLE_BUFFERS = 46,
    ALLOCATION_SCRIPT_MGR_DECI_INPUT = 132,
    ALLOCATION_SCRIPT_DBG_HEAP = 131,
    ALLOCATION_NPC_DEBUG_MEM = 232,
    ALLOCATION_DEBUG_DMENU = 242,
    ALLOCATION_DEBUG_ORBIS_EXCEPTION = 243,
    ALLOCATION_DEBUG_DRAWING = 244,
    ALLOCATION_MEMORY_TRACKER = 50,
    ALLOCATION_DEV_GAME_TO_GPU_RING = 245,
    ALLOCATION_DEV_RENDER_TO_GPU_RING = 246,
    ALLOCATION_PROFILE_TRACE = 162,
    ALLOCATION_TEXTDB_DEBUG_MEM = 248,
    ALLOCATION_DEV_SCREEN_CAPTURE = 249,
    ALLOCATION_FIOS_RAM_CACHE_PERSISTENT = 213,
    ALLOCATION_DEBUG_BLOCKMESH_MGR = 250,
    ALLOCATION_DEBUG_TOOLS_MGR = 251,
    ALLOCATION_DEV_CPU_UNACCOUNTED_2 = 28,
    ALLOCATION_DEV_CPU_FREE_2 = 29,
    ALLOCATION_DEV_GPU_TARGET_CAPTURE = 239,
    ALLOCATION_PS_CAMERA = 240,
    ALLOCATION_OVERFLOW_DYNAMIC_RENDER_TARGET = 252,
    ALLOCATION_OVERFLOW_DYNAMIC_NON_RENDER_TARGET = 253,
    ALLOCATION_OVERFLOW_DYNAMIC_BUFFER = 254,
    ALLOCATION_DEV_GPU_UNACCOUNTED = 30,
    ALLOCATION_DEV_GPU_FREE = 31,
};

namespace Utility::memory
{
    struct Address {
    public:
        Address(void* ptr) : m_ptr(ptr) {};
        Address(uintptr_t addr) : m_ptr((void*)addr) {};

        void* get() const {
            return m_ptr;
        }

        operator uintptr_t() const {
            return (uintptr_t)m_ptr;
        }

        operator void* () const {
            return m_ptr;
        }

    private:
        void* m_ptr { nullptr };
    };

    class Pattern {
    public:
        Pattern() = delete;
        explicit Pattern(const Patterns::Pattern& pattern);
        ~Pattern() = default;

        uint8_t* find(std::uint8_t* scanBytes, size_t sizeOfImage);
        uint8_t* find(HMODULE module);
        std::wstring patternName() const noexcept { return m_name; }

    private:
        std::wstring m_name;
        std::vector<int32_t> m_pattern;
    };

    class Patch {
    public:
        using Ptr = std::shared_ptr<Patch>;
        Patch(uintptr_t Address, const std::vector<int16_t>& patchb, std::wstring name, bool enable);
        virtual ~Patch();
        bool toggle(bool state);
        Address GetPatchAddress() const noexcept { return m_address; }
        size_t GetPatchSize() const noexcept { return m_patchb.size(); }
        std::wstring GetName() const noexcept { return m_name; }

        static Ptr create(uintptr_t Address, const std::vector<int16_t>& patchb, std::wstring name, bool enable = true);
        static bool PatchBytes(uintptr_t Address, const unsigned char* pattern, unsigned int numBytes);
        static bool PatchBytes(uintptr_t Address, const std::vector<int16_t>& patchb);
        static std::optional<DWORD> protect(uintptr_t Address, size_t size, DWORD protection);
    private:
        uintptr_t m_address;
        std::wstring m_name;
        std::vector<int16_t> m_patchb;
        std::vector<int16_t> m_original_patchb;
        bool m_enabled;
    };

    std::optional<size_t> get_module_size(HMODULE module);
    std::string get_module_name(HMODULE module);

    uintptr_t char_Scan(HMODULE module, const char* text);

    bool InitMemoryMap(HMODULE game);
    bool StaticMemoryMapPatch(MemoryMapId id, uint64_t newSizeBytes);
}

namespace Utility
{
    memory::Patch::Ptr WritePatchPattern(HMODULE module, const Patterns::Pattern& pattern, const std::vector<int16_t>& Patch_Bytes, bool enable = true);
    memory::Patch::Ptr WritePatchPattern(HMODULE module, const Patterns::Pattern& pattern, const unsigned char* Patch_Bytes, size_t Patch_Size, bool enable = true);
    uintptr_t FindAndPrintPattern(HMODULE module, const Patterns::Pattern& pattern, size_t extraOffset = 0);
    uintptr_t ReadLEA32(HMODULE module, const Patterns::Pattern& pattern, size_t lea_size, size_t lea_opcode_size);
    safetyhook::MidHook MakeMidHook(void* source_target, safetyhook::MidHookFn target_jmp, const wchar_t* source_name, const wchar_t* hook_name);
}
