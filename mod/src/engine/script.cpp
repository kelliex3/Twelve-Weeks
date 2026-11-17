#include "script.hpp"
#include "engine/patterns.hpp"
#include "loader/config.hpp"
#include "sid/sidbase.hpp"
#include "log.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Actors loaded through MEMORY::LOAD_ACTOR, unloaded by SL::CLEAN_MEMORY.
static std::vector<std::string> loadedActors;

namespace DcManager
{
    uint64_t ScriptPrintAddr = 0;
    uintptr_t SkeletonOverrideAddr1 = 0;
    uintptr_t SkeletonOverrideAddr2 = 0;

    INIT_FUNCTION_PTR(Lookup_);
    const DcValue* Lookup(StringId64 Hash)
    {
        always_assert(DcManager::Lookup_ == nullptr, "Function pointer was not set!");
        return Lookup_(Hash, nullptr);
    }

    INIT_FUNCTION_PTR(LookupInModule_);
    DcValue* LookupInModule(StringId64 Symbol, StringId64 Module)
    {
        always_assert(DcManager::LookupInModule_ == nullptr, "Function pointer was not set!");
        return LookupInModule_(Symbol, Module);
    }

    INIT_FUNCTION_PTR(LoadActorByName_);
    bool LoadActorByName(char const* pActorName, AssetType pAssetType, bool sync)
    {
        always_assert(DcManager::LoadActorByName_ == nullptr, "LoadActorByName missing function pointer");
        return LoadActorByName_(pActorName, pAssetType, sync);
    }

    INIT_FUNCTION_PTR(UnloadActorByName_);
    void UnloadActorByName(char const* pActorName)
    {
        always_assert(DcManager::UnloadActorByName_ == nullptr, "UnloadActorByName missing function pointer");
        UnloadActorByName_(pActorName);
    }

    INIT_FUNCTION_PTR(LoadLevelSet_);
    void LoadLevelSet(char const* levelSetName) {
        always_assert(DcManager::LoadLevelSet_ == nullptr, "LoadLevelSet missing function pointer");
        LoadLevelSet_(levelSetName);
    }

    struct LookupContext
    {
        StringId64* args;
        int argCount;
        int currentArg;
    };

    INIT_FUNCTION_PTR(LookupObject_);
    uint64_t LookupObject(StringId64 nameHash)
    {
        StringId64 argsArray[1] = { nameHash };

        LookupContext ctx;
        ctx.args = argsArray;
        ctx.argCount = 1;
        ctx.currentArg = 0;

        return LookupObject_(&ctx, 1, 1);
    }
}

// A script float argument is the 32 float bits zero-extended into a value slot.
// std::bit_cast is the well-defined way to reinterpret the bits (no aliasing UB,
// and it can't over-read the way a uint64_t* cast on a 4-byte float would).
float floatFromUint64(uint64_t value) {
    return std::bit_cast<float>(static_cast<uint32_t>(value));
}

namespace DC
{
    bool JOYPAD_COMMAND_ACTIVE(StringId64 command)
    {
        return DC::Call<"joypad-command-active?">(command);
    }
}

namespace MEMORY
{
    uint64_t ALLOC_VECTOR(Vector3 xyz)
    {
        return DC::Call<"alloc-vector">(xyz.x, xyz.y, xyz.z, 1.0f, 0);
    }

    uint64_t ALLOC_QUATERNION(Quaternion xyzw)
    {
        return DC::Call<"alloc-quat">(xyzw.x, xyzw.y, xyzw.z, xyzw.w, 0);
    }

    uint64_t ALLOC_BOUND_FRAME(Vector3 xyz, Quaternion xyzw)
    {
        return DC::Call<"alloc-bound-frame">(MEMORY::ALLOC_VECTOR(xyz), MEMORY::ALLOC_QUATERNION(xyzw), 0, 0, 0);
    }

    uint64_t ALLOC_BOUND_FRAME_OBJECT(uint64_t objectA, uint64_t objectB, int unk_0)
    {   
        uint64_t Loc = LOCATOR::GET_LOCATOR(objectA);
        Vector3 pos = LOCATOR::GET_LOCATOR_POSITION(Loc);
        Quaternion rot = LOCATOR::GET_LOCATOR_ROTATION(Loc);

        return DC::Call<"alloc-bound-frame">(MEMORY::ALLOC_VECTOR(pos), MEMORY::ALLOC_QUATERNION(rot), objectB, unk_0, 0);
    }

    uint64_t ALLOC_ARRAY(uint64_t unkFlag1, uint64_t unkFlag2, uint64_t unkFlag3)
    {
        return DC::Call<"#%alloc-array">(SID("single-frame"), unkFlag1, unkFlag2, unkFlag3);
    }

    uint64_t ALLOC_STRING(const char* text)
    {
        uint64_t arrayPointer = MEMORY::ALLOC_ARRAY(0x23, 2, 1);
        char* memoryAddress = reinterpret_cast<char*>(arrayPointer);
        std::strcpy(memoryAddress, text);
        return arrayPointer;
    }

    uint64_t ALLOC_COLOR(float r, float g, float b, float a)
    {
        return DC::Call<"alloc-color">(r, g, b, a, 0);
    }

    uint64_t NEW_BOXED_VALUE(BoxedValueType type, uint64_t value)
    {
        return DC::Call<"new-boxed-value">(type, value);
    }

    uint64_t ALLOC_SPAWN_PLAYER(StringId64 spawner)
    {
        uint64_t arrayPointer = MEMORY::ALLOC_ARRAY(0x23, 2, 1);
        unsigned char* memoryAddress = reinterpret_cast<unsigned char*>(arrayPointer);

        unsigned char bytes[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xB9, 0x7E, 0x3B, 0xB0, 0xFA, 0x80, 0x45,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x3F, 0x26, 0x4A, 0x75, 0xF0, 0xE8, 0xC1, 0x7B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x7F, 0x74, 0xDF, 0x48, 0x7D, 0xC7, 0x26, 0xB3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x39, 0xEA, 0xD4, 0x37, 0x3E, 0xC1, 0x0F, 0x1F, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x80, 0xBF, 0x0A, 0xD7,
            0x23, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x78, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        std::memcpy(memoryAddress, bytes, sizeof(bytes));

        *reinterpret_cast<uint64_t*>(memoryAddress + 0x8) = spawner; // player spawn point
        *reinterpret_cast<uint64_t*>(memoryAddress + 0x40) = spawner; // camera spawn point

        return arrayPointer;
    }

    bool LOAD_ACTOR(const char* actorName, AssetType assetType, bool sync)
    {
        bool load = DcManager::LoadActorByName(actorName, assetType, sync);
        if (load)
            loadedActors.push_back(actorName);
        return load;
    }

    void UNLOAD_ACTOR(const char* actorName)
    {
        DcManager::UnloadActorByName(actorName);
    }

    void LOAD_LEVEL_SET(char const* levelSetName)
    {
        DcManager::LoadLevelSet(levelSetName);
    }

    void LOAD_LEVEL(char const* levelName)
    {
        DC::Call<"load-level">(MEMORY::ALLOC_STRING(levelName));
    }

    void UNLOAD_LEVEL(char const* levelName)
    {
        DC::Call<"unload-level">(MEMORY::ALLOC_STRING(levelName));
    }

}

namespace LOCATOR
{
    uintptr_t GET_LOCATOR(StringId64 entity)
    {
        return DC::Call<"get-object-locator">(entity);
    }

    uintptr_t GET_JOINT_LOCATOR(StringId64 entity, StringId64 joint)
    {
        return DC::Call<"get-joint-locator">(entity, joint);
    }

    uintptr_t GET_SPAWNER_LOCATOR(StringId64 entity)
    {
        return DC::Call<"get-spawner-locator">(entity, 1);
    }

    float GET_LOCATOR_DISTANCE(uintptr_t locator1, uintptr_t locator2)
    {
        return floatFromUint64(DC::Call<"get-locator-distance">(locator1, locator2));
    }

    Vector3 GET_LOCATOR_POSITION(uintptr_t locator)
    {
        return *DC::Call<"get-locator-position", Vector3>(locator);
    }

    uintptr_t GET_LOCATOR_POSITION_PTR(uintptr_t locator)
    {
        return DC::Call<"get-locator-position">(locator);
    }

    Vector3 GET_POSITION_IN_FRONT_OF(StringId64 entity, float distanceInFront)
    {
        Transform objTransform = LOCATOR::GET_TRANSFORM(entity);

        Vector3 forward = {
            2.0f * (objTransform.Rotation.x * objTransform.Rotation.z + objTransform.Rotation.w * objTransform.Rotation.y),
            2.0f * (objTransform.Rotation.y * objTransform.Rotation.z - objTransform.Rotation.w * objTransform.Rotation.x),
            1.0f - 2.0f * (objTransform.Rotation.x * objTransform.Rotation.x + objTransform.Rotation.y * objTransform.Rotation.y)
        };

        return {
            objTransform.Position.x + forward.x * distanceInFront,
            objTransform.Position.y + forward.y * distanceInFront,
            objTransform.Position.z + forward.z * distanceInFront
        };
    }

    Quaternion GET_LOCATOR_ROTATION(uintptr_t locator)
    {
        return *DC::Call<"get-locator-rotation", Quaternion>(locator);
    }

    Transform GET_TRANSFORM(StringId64 entity)
    {
        uintptr_t locator = LOCATOR::GET_LOCATOR(entity);
        Vector3 position = LOCATOR::GET_LOCATOR_POSITION(locator);
        Quaternion rotation = LOCATOR::GET_LOCATOR_ROTATION(locator);
        Transform transform(position, rotation);
        return transform;
    }

    void SET_OBJECT_LOCATOR(StringId64 entity, uintptr_t locatorOrBoundFrame)
    {
        DC::Call<"set-object-locator">(entity, locatorOrBoundFrame);
    }

    uintptr_t OFFSET_LOCATOR(uintptr_t locator, uintptr_t offset)
    {
        return DC::Call<"offset-locator">(locator, offset);
    }

    uintptr_t KICK_RAYCAST(uintptr_t castFrom, uintptr_t castFromOffset, StringId64 castTowards, uint64_t unkInt)
    {
        return DC::Call<"kick-raycast">(castFrom, castFromOffset, castTowards, unkInt);
    }

    uintptr_t GET_CAST_LOCATOR(uintptr_t raycast)
    {
        return DC::Call<"get-cast-locator">(raycast);
    }

    bool IS_WITHIN_RADIUS(const Vector3& pointToCheck, const Vector3& center, double radius)
    {
        double dx = pointToCheck.x - center.x;
        double dy = pointToCheck.y - center.y;
        double dz = pointToCheck.z - center.z;
        return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
    }
}

namespace OBJECT
{
    const char* GET_ART_GROUP(StringId64 entity)
    {
        const char* result = NdGameSdk::Sidbase::StringIdToStringInternal(DC::Call<"get-art-group">(entity));
        return result;
    }

    const char* GET_OBJECT_SKELETON(StringId64 entity)
    {
        const char* result = NdGameSdk::Sidbase::StringIdToStringInternal(DC::Call<"get-object-skeleton">(entity));
        return result;
    }

    const char* GET_OBJECT_ANIM(StringId64 entity)
    {
        StringId64 anim = DC::Call<"get-object-anim">(entity, 1);
        if (anim)
        {
            return NdGameSdk::Sidbase::StringIdToStringInternal(anim);
        }
        else
        {
            return "";
        }
    }

    const char* GET_OBJECT_ANIM_STATE(StringId64 entity)
    {
        StringId64 anim = DC::Call<"get-object-anim-state">(entity);
        if (anim)
        {
            return NdGameSdk::Sidbase::StringIdToStringInternal(anim);
        }
        else
        {
            return "";
        }
    }

    const char* GET_OBJECT_GESTURE(StringId64 entity)
    {
        StringId64 anim = DC::Call<"get-currently-playing-gesture">(entity, 0xFFFFFFFFFFFFFFFF);
        if (anim)
        {
            return NdGameSdk::Sidbase::StringIdToStringInternal(anim);
        }
        else
        {
            return "";
        }
    }

    StringId64 GET_OBJECT_ANIM_SID(StringId64 entity)
    {
        return DC::Call<"get-object-anim">(entity, 1);
    }

    StringId64 GET_OBJECT_ANIM_STATE_SID(StringId64 entity)
    {
        return DC::Call<"get-object-anim-state">(entity);
    }

    void HIGHLIGHT(StringId64 object, float r, float g, float b, float a)
    {
        DC::Call<"object-highlight">(object, 0, MEMORY::ALLOC_COLOR(r, g, b, a));
    }

}

namespace ENTITY
{
    Spawner* GET_SPAWNER(const char* spawnerName)
    {
        return DC::Call<"get-spawner", Spawner>(SID(spawnerName));
    }

    StringId64 SPAWN_OBJECT(StringId64 object, StringId64 processType)
    {
        return DC::Call<"spawn-object">(object, processType, 0, 0, 1, 0, 0, 0, 0);
    }

    uint64_t SPAWN_EXPLOSION(StringId64 explosionId, Vector3 position)
    {
        return DC::Call<"spawn-explosion">(MEMORY::ALLOC_VECTOR(position), explosionId, 0, 0, -1.0f, 0, 0, 0, 0);
    }

    StringId64 SPAWN_ENTITY(StringId64 spawnerName, Vector3 position, Quaternion rotation, StringId64 customLook)
    {
        StringId64 objectSID = DC::Call<"spawn-entity-at-with-look">(spawnerName, MEMORY::ALLOC_BOUND_FRAME(position, rotation), customLook);
        return objectSID;
    }

    void PRINT_ENTITY_INFO(StringId64 entity)
    {
        DC::Call<"entity-info->string">(entity);
    }

    bool KILL_ENTITY(StringId64 entity)
    {
        if (DC::Call<"entity-exists?">(entity))
            return DC::Call<"kill-entity-and-bound-entities">(entity);
        else
        {
            TO1_LOG("ERROR: Cannot kill process, entity does not exist.\n");
            return false;
        }
    }

    float GET_HEALTH(StringId64 entity)
    {
        uint64_t entityHealth = DC::Call<"get-object-health">(entity);
        return *reinterpret_cast<float*>(&entityHealth);
    }

    void SET_HEALTH(StringId64 entity, float health)
    {
        DC::Call<"set-object-health">(entity, health);
    }

    void ANIMATE(StringId64 entity, StringId64 animation, uint64_t allocArray)
    {
        DC::Call<"animate_">(entity, animation, allocArray);
    }

    void SET_VISIBLE(StringId64 entity, bool visible)
    {
        DC::Call<"set-visible">(entity, visible);
    }

    void SET_SCALE(StringId64 entity, float scale)
    {
        DC::Call<"set-uniform-scale">(entity, scale);
    }

    void SET_COLLISION(StringId64 entity, bool collision)
    {
        if (collision)
            DC::Call<"send-event">(SID("physicalize"), entity);
        else
            DC::Call<"send-event">(SID("unphysicalize"), entity, DC::Call<"new-boxed-value">(7, 0));
    }

    const char* GET_PROCESS_TYPE(StringId64 entity)
    {
        return NdGameSdk::Sidbase::StringIdToStringInternal(DC::Call<"process-get-type">(entity));
    }

    void SET_UNSHOOTABLE(StringId64 entity, bool unshootable)
    {
        DC::Call<"set-is-unshootable">(entity, unshootable);
    }

    bool DOES_EXIST(StringId64 entity)
    {
        return DC::Call<"entity-exists?">(entity);
    }

    void ATTACH(StringId64 childEntity, StringId64 parentEntity, StringId64 attachPoint)
    {
        DC::Call<"attach">(childEntity, parentEntity, attachPoint, 0);
    }
}

namespace ANIMATION
{
    void ANIMATE(StringId64 entity, StringId64 animation, uint64_t allocArray)
    {
        DC::Call<"animate_">(entity, animation, allocArray);
    }

    void GESTURE(StringId64 entity, StringId64 gesture)
    {
        DC::Call<"gesture_">(entity, gesture, DC::Call<"alloc-gesture-play-params">());
    }
}

namespace TIME
{
    bool IS_GAME_PAUSED()
    {
        return DC::Call<"game-paused?">(0);
    }

    float GET_DELTA_TIME()
    {
        uint64_t deltaTime = DC::Call<"get-delta-time">();
        return *reinterpret_cast<float*>(&deltaTime);
    }

    void SET_CLOCK_MULTIPLIER(float scale)
    {
        DC::Call<"set-clock-multiplier/f">(scale, 1);
    }
}

namespace AUDIO
{
    void PLAY_SOUND(StringId64 sfxName)
    {
        DC::Call<"play-sound">(sfxName, 0);
    }

    void PLAY_SOUND_AT_POSITION(StringId64 sfxName, uint64_t positionPtr)
    {
        DC::Call<"play-sound-at-position">(sfxName, positionPtr, 0, 0);
    }

    void PLAY_VOICE_LINE(StringId64 npc, StringId64 voiceLineID)
    {
        DC::Call<"say">(npc, voiceLineID, 2, 0);
    }

    void AUDIO_GAMEPLAY_EVENT(StringId64 eventId, uint64_t position, StringId64 causeEntity) // causeEntity = entity that caused the event
    {
        DC::Call<"audio-gameplay-event">(eventId, position, causeEntity);
    }

    void PLAY_MUSIC_PLAYLIST(StringId64 playlistId)
    {
        DC::Call<"play-music-stinger">(playlistId, 0);
    }

    void STOP_MUSIC_PLAYLIST()
    {
        DC::Call<"play-music-stinger">(SID("*music-playlist-force-stop*"), 0);
    }

    void SET_MASK_FLAG(StringId64 entity, StringId64 maskFlag)
    {
        DC::Call<"audio-set-gas-mask-flag">(entity, maskFlag);
    }

    void CLEAR_MASK_FLAG(StringId64 entity)
    {
        DC::Call<"audio-clear-gas-mask-flag">(entity);
    }
}

namespace DEBUG
{
    void PLAY_TASK(StringId64 taskNode, StringId64 taskSubNode1, StringId64 taskSubNode2)
    {
        DC::Call<"task-play-debug!">(taskNode, taskSubNode1, taskSubNode2);
    }

    bool IS_TASK_ACTIVE(StringId64 taskNode, StringId64 taskSubNode)
    {
        uint64_t taskFind = DC::Call<"task-find">(taskNode, taskSubNode);
        return DC::Call<"task-subnode-active?">(taskFind);
    }

    void DISABLE_VIS()
    {
        DC::Call<"disable-vis">();
    }

    bool IS_DEV_MENU_OPEN()
    {
        return DC::Call<"is-dev-menu-open?">();
    }

    // Also can be used for loading modules. If it cant reload it, it will load it. You will
    // also see some text pop up in debug rendering when calling this.
    bool RELOAD_MODULE(StringId64 moduleName)
    {
        return DC::Call<"reload-module">(moduleName);
    }
}

namespace SYSTEM
{
    Vector3 QUAT_TO_EULER(Quaternion quaternion)
    {
        Vector3* quatPtr = DC::Call<"quat-to-euler", Vector3>(MEMORY::ALLOC_QUATERNION(quaternion));
        return *quatPtr;
    }

    Quaternion EULER_TO_QUAT(Vector3 euler)
    {
        // Convert degrees to radians using M_PI
        float roll = euler.x * (M_PI / 180.0f) * 0.5f;
        float pitch = euler.y * (M_PI / 180.0f) * 0.5f;
        float yaw = euler.z * (M_PI / 180.0f) * 0.5f;

        // Compute sin and cos values
        float sinRoll = std::sin(roll);
        float cosRoll = std::cos(roll);
        float sinPitch = std::sin(pitch);
        float cosPitch = std::cos(pitch);
        float sinYaw = std::sin(yaw);
        float cosYaw = std::cos(yaw);

        // Compute quaternion components (xyzw order)
        Quaternion q;
        q.x = sinRoll * cosPitch * cosYaw - cosRoll * sinPitch * sinYaw;
        q.y = cosRoll * sinPitch * cosYaw + sinRoll * cosPitch * sinYaw;
        q.z = cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw;
        q.w = cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw;

        return q;
    }

    Vector2 WORLD_TO_SCREEN(Vector3 position)
    {
        Vector2* w2sResult = DC::Call<"world-space-to-screen-space", Vector2>(0, MEMORY::ALLOC_VECTOR(position));
        return *w2sResult;
    }

    uint64_t GET_CURRENT_FRAME_COUNT()
    {
        return DC::Call<"get-current-frame-count">();
    }

    float UINT64_TO_FLOAT(uint64_t value)
    {
        return floatFromUint64(value);
    }

    float GET_SCREEN_WIDTH()
    {
        uint64_t screenWidth = DC::Call<"get-screen-width">();
        return *reinterpret_cast<float*>(&screenWidth);
    }
}

namespace PLAYER
{
    bool IS_IN_STATE(StringId64 state)
    {
        return DC::Call<"player-in-state?">(state);
    }

    int GET_HEALTH()
    {
        return DC::Call<"get-player-health">();
    }

    int GET_MAX_HEALTH()
    {
        return 100; //DC::Call<"get-object-max-health">();
    }

    void SET_HEALTH(int health)
    {
        DC::Call<"set-player-health">(health);
    }

    float GET_HEAL_PROGRESS()
    {
        uint64_t raw = DC::Call<"player-get-heal-progress">();
        return *reinterpret_cast<float*>(&raw);
    }
}

namespace CAMERA
{
    uint64_t GET_CAMERA_LOCATOR()
    {
        return DC::Call<"camera-locator">(0);
    }

    void SET_OVERLAY(StringId64 overlay)
    {
        DC::Call<"camera-set-overlay">(overlay);
    }

    void OVERRIDE_FOV(float fov)
    {
        DC::Call<"override-fov">(fov);
    }

    void SET_CAMERA_POSITION(Vector3 pos, Quaternion rot)
    {
        DC::Call<"debug-set-camera-position/f">(MEMORY::ALLOC_BOUND_FRAME(pos, rot));
    }
}

namespace HUD
{
    bool IS_READ_PAGE_ACTIVE()
    {
        return DC::Call<"hud2-read-page-active?">(1);
    }

    void SHOW_READ_PAGE(StringId64 cipher)
    {
        DC::Call<"hud2-show-read-page">(cipher, 0, 0, 1, 0);
    }

    void SET_READ_PAGE_VISIBILITY(bool toggle)
    {
        DC::Call<"hud2-hide-read-page">(toggle);
    }

    float GET_BLACK_FADE()
    {
        uint64_t blackFade = DC::Call<"get-black-fade">();
        return *reinterpret_cast<float*>(&blackFade);
    }

    void FADE_TO_BLACK(float timeToFade, float blackHold)
    {
        DC::Call<"fade-to-black">(timeToFade, blackHold, 0, 0);
    }

    void SHOW_NOTIFICATION(const char* message, float durationSeconds, int messageType, StringId64 customCipherId)
    {
        char* cipherText = DC::Call<"cipher-get-text", char>(customCipherId);
        std::strcpy(cipherText, message);

        DC::Call<"hud-show-cipher-message">(messageType, customCipherId, durationSeconds, 2, SID("none"), 0.5f);
    }

    void SHOW_NOTIFICATION_CIPHER(StringId64 cipherId, float durationSeconds, int messageType)
    {
        DC::Call<"hud-show-cipher-message">(messageType, cipherId, durationSeconds, 2, SID("none"), 0.5f);
    }
}

namespace GAMEPLAY
{
    bool IS_ANY_ENEMY_NEARBY(uintptr_t locator, float radius)
    {
        uint64_t enemyArray = SID("all-enemy-npcs");
        uint64_t enemyCount = DC::Call<"darray-count">(enemyArray);
        for (int i = 0; i < enemyCount; i++) {
            uint64_t enemyAddr = DC::Call<"darray-at">(enemyArray, i);
            uint64_t enemySid = *(uint64_t*)enemyAddr;

            if (LOCATOR::GET_LOCATOR_DISTANCE(locator, LOCATOR::GET_LOCATOR(enemySid)) <= radius)
                return true;
        }

        return false;
    }
}

namespace NPC
{
    const char* GET_FACTION_NAME(StringId64 npc)
    {
        return NdGameSdk::Sidbase::StringIdToStringInternal(DC::Call<"fact-symbol">(npc, SID("faction"), 1, 0));
    }

    Faction GET_FACTION_ID(StringId64 npc)
    {
        return (Faction)DC::Call<"npc-get-faction-id">(npc);
    }

    bool CAN_SEE(StringId64 npc, StringId64 target)
    {
        return DC::Call<"npc-can-see?">(npc, target);
    }

    void START_SEARCH(Faction faction)
    {
        DC::Call<"npc-start-search">(faction);
    }

    void SET_FACTION_ID(StringId64 npc, Faction faction)
    {
        DC::Call<"set-object-faction">(npc, faction);
    }

    void HIGHLIGHT(StringId64 npc, float r, float g, float b, float a)
    {
        DC::Call<"character-highlight">(npc, 0, MEMORY::ALLOC_COLOR(r, g, b, a));
    }

    bool IS_ALIVE(StringId64 npc)
    {
        return DC::Call<"npc-alive?">(npc);
    }
}

namespace MELEE
{
    const char* GET_CURRENT_MELEE_ACTION_NAME(StringId64 entity)
    {
        return NdGameSdk::Sidbase::StringIdToStringInternal(DC::Call<"get-current-melee-action-name">(entity));
    }

    StringId64 GET_CURRENT_MELEE_ACTION(StringId64 entity)
    {
        return DC::Call<"get-current-melee-action-name">(entity);
    }
}

namespace FEDRA
{
    bool IS_PLAYER_AT_MAIN_MENU()
    {
        return DEBUG::IS_TASK_ACTIVE(SID("project-fedra"), SID("project-fedra-main-menu"));
    }

    bool IS_PLAYER_AT_BASE()
    {
        return DEBUG::IS_TASK_ACTIVE(SID("project-fedra"), SID("fedra-base-fob"));
    }

    std::vector<StringId64> GET_ALL_NPCS()
    {
        std::vector<StringId64> npcList;
        uint64_t getSpawners = SID("all-npcs");
        uint64_t spawnerCount = DC::Call<"darray-count">(getSpawners);

        for (int i = 0; i < spawnerCount; i++) {
            uint64_t spawnerAddr = DC::Call<"darray-at">(getSpawners, i);
            StringId64 spawnerSid = *(StringId64*)spawnerAddr;
            npcList.push_back(spawnerSid);
        }

        return npcList;
    }

    std::vector<StringId64> GET_ALL_ENEMIES()
    {
        std::vector<StringId64> enemyList;
        uint64_t getSpawners = SID("all-enemy-npcs");
        uint64_t spawnerCount = DC::Call<"darray-count">(getSpawners);

        for (int i = 0; i < spawnerCount; i++) {
            uint64_t spawnerAddr = DC::Call<"darray-at">(getSpawners, i);
            StringId64 spawnerSid = *(StringId64*)spawnerAddr;
            enemyList.push_back(spawnerSid);
        }

        return enemyList;
    }

    std::vector<StringId64> GET_ALL_BUDDIES()
    {
        std::vector<StringId64> enemyList;
        uint64_t getSpawners = SID("all-buddy-npcs");
        uint64_t spawnerCount = DC::Call<"darray-count">(getSpawners);

        for (int i = 0; i < spawnerCount; i++) {
            uint64_t spawnerAddr = DC::Call<"darray-at">(getSpawners, i);
            StringId64 spawnerSid = *(StringId64*)spawnerAddr;
            enemyList.push_back(spawnerSid);
        }

        return enemyList;
    }

    void SWAP_LOOK_ID(StringId64 lookId)
    {
        DC::Call<"player-swap-look-id">(lookId, 1, 1);
    }

    // Cache to prevent loading the same look multiple times in one call chain
    static std::unordered_set<std::string> loadedLooks;
    static std::unordered_set<std::string> allLookNames;
    static bool lookListInitialized = false;

    void InitializeLookList() {
        if (lookListInitialized) {
            return;
        }

        // Read the look list file (one data dir for every config -- see dist/README.md)
        std::ifstream lookListFile(PROJECT_DATA_DIR "/t2r-looks.txt");
        if (!lookListFile.is_open()) {
            TO1_LOG("WARNING: Could not open t2r-looks.txt to initialize look list\n");
            return;
        }

        std::string lookName;
        while (std::getline(lookListFile, lookName)) {
            // Trim whitespace and skip empty lines
            lookName.erase(0, lookName.find_first_not_of(" \t\r\n"));
            lookName.erase(lookName.find_last_not_of(" \t\r\n") + 1);

            if (!lookName.empty()) {
                allLookNames.insert(lookName);
            }
        }

        lookListFile.close();
        lookListInitialized = true;
        TO1_LOG("Initialized look list with %zu looks\n", allLookNames.size());
    }

    // Static cache for valid actors from actor97.txt
    static std::unordered_set<std::string> validActors;
    static bool validActorsLoaded = false;

    // Helper function to load the valid actors list (called once)
    void LoadValidActorsList() {
        if (validActorsLoaded) return;

        std::ifstream file("build/pc/main/actor97.txt");
        if (!file.is_open()) {
            TO1_LOG("WARNING: Could not open build/pc/main/actor97.txt\n");
            validActorsLoaded = true; // Mark as loaded to avoid repeated attempts
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Trim trailing whitespace/carriage returns
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (!line.empty()) {
                validActors.insert(line);
            }
        }

        file.close();
        validActorsLoaded = true;
        TO1_LOG("Loaded %zu valid actors from actor97.txt\n", validActors.size());
    }

    void LOAD_LOOK_ASSETS(const std::string& lookName, bool isRootCall) {
        // Initialize the look list on first call
        if (!lookListInitialized) {
            InitializeLookList();
        }

        // Load the valid actors list on first call
        LoadValidActorsList();

        // Clear cache on root call
        if (isRootCall) {
            loadedLooks.clear();
        }

        // Skip if already loaded in this chain
        if (loadedLooks.contains(lookName)) {
            return;
        }
        loadedLooks.insert(lookName);

        // Get the look pointer from game memory
        DcValue* lookPtrReturn = DcManager::LookupInModule(SID(lookName.c_str()), SID("look2/characters"));
        if (!lookPtrReturn || !lookPtrReturn->val[0]) {
            lookPtrReturn = DcManager::LookupInModule(SID(lookName.c_str()), SID("look2/animals"));
        }


        if (!lookPtrReturn || !lookPtrReturn->val[0]) {
            TO1_LOG("WARNING: Look '%s' not found in game memory\n", lookName.c_str());
            return;
        }

        uint64_t lookPtr = lookPtrReturn->val[0];

        // Helper lambda to load a package
        auto loadPackage = [](const std::string& pakName) {
            if (!MEMORY::LOAD_ACTOR(pakName.c_str(), AssetType::Actor, false)) {
                TO1_LOG("WARNING: Failed to load package '%s'\n", pakName.c_str());
                return false;
            }
            return true;
            };

        // Helper lambda to safely read a null-terminated string from memory
        auto readString = [](uint64_t ptr) -> std::string {
            if (!ptr) return "";
            try {
                return std::string((const char*)ptr);
            }
            catch (...) {
                return "";
            }
            };

        uint64_t basePtr = lookPtr;

        // Load the skeleton first
        // Skeleton name pointer is at offset 0x10
        uint64_t skelNamePtr = *(uint64_t*)(basePtr + 0x10);
        std::string skelName = readString(skelNamePtr);

        if (!skelName.empty()) {
            //TO1_LOG("Loading skeleton: %s\n", skelName.c_str());
            loadPackage(skelName);
        }
        else {
            //TO1_LOG("WARNING: No skeleton found for look '%s'\n", lookName.c_str());
        }

        // Load Kits after skeleton
        // KitArray is at offset 0x198 from the base
        uint64_t kitArrayOffset = basePtr + 0x190;
        uint64_t kitCount = *(uint64_t*)(kitArrayOffset);
        uint64_t kitArrayPtr = *(uint64_t*)(kitArrayOffset + 8);

        if (kitArrayPtr && kitCount > 0 && kitCount < 100) { // Sanity check on count
            for (uint32_t i = 0; i < kitCount; i++) {
                uint64_t kitElementOffset = kitArrayPtr + (i * 16);
                uint64_t kitNamePtr = *(uint64_t*)kitElementOffset;

                std::string kitName = readString(kitNamePtr);

                if (!kitName.empty()) {
                    //TO1_LOG("Loading kit: %s\n", kitName.c_str());
                    loadPackage(kitName);
                }
            }
        }

        int structOffset = 0x90; // T2 default, use 0xA0 for T1X

        // Process each asset type (Meshes, Props, Dependents, AdditionalDependents)
        for (int assetType = 0; assetType < 4; assetType++) {
            uint64_t listOffset = basePtr + structOffset + (assetType * 0x10);

            // Read array pointer and count
            uint64_t arrayPtr = *(uint64_t*)listOffset;
            uint32_t count = *(uint32_t*)(listOffset + 8);

            if (!arrayPtr || count == 0) {
                continue;
            }

            // Iterate through assets in this list
            for (uint32_t i = 0; i < count; i++) {
                uint64_t assetPtr = *(uint64_t*)(arrayPtr + (i * 8));

                if (!assetPtr) {
                    continue;
                }

                // Get asset name pointer (offset 0x10 in asset structure)
                uint64_t namePtr = *(uint64_t*)(assetPtr + 0x10);
                std::string assetName = readString(namePtr);

                if (assetName.empty()) {
                    continue;
                }

                // Check if this asset name is in our look list
                if (allLookNames.contains(assetName)) {
                    // If this asset is the current look we're processing, just load it as a package
                    if (assetName == lookName) {
                        //TO1_LOG("Loading asset (self-reference): %s\n", assetName.c_str());
                        if (validActors.contains(assetName)) {
                            loadPackage(assetName);
                        }
                        else {
                            //TO1_LOG("Skipping asset '%s' - not in actor97.txt\n", assetName.c_str());
                        }
                    }
                    // Otherwise, recursively load the referenced look
                    else {
                        //TO1_LOG("Loading referenced look: %s\n", assetName.c_str());
                        LOAD_LOOK_ASSETS(assetName, false);
                    }
                }
                else {
                    // Load as regular asset package only if it exists in actor97.txt
                    if (validActors.contains(assetName)) {
                        loadPackage(assetName);
                    }
                    else {
                        //TO1_LOG("Skipping asset '%s' - not in actor97.txt\n", assetName.c_str());
                    }
                }
            }
        }
    }

    // NOTE: starlight-mod-menu level must be loaded in memory for this to work
    void CHANGE_MODEL(const char* lookId)
    {
        Spawner* spawner = ENTITY::GET_SPAWNER("starlight-model-changer");
        Transform playerTransform = LOCATOR::GET_TRANSFORM(SID("player"));

        spawner->m_pos = playerTransform.Position;
        spawner->m_posW = 0.0f;
        spawner->m_rot = playerTransform.Rotation;

        std::strcpy(reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(spawner) + 0x188), lookId);

        DC::Call<"spawn-player">(MEMORY::ALLOC_SPAWN_PLAYER(SID("starlight-model-changer")), 0);
    }
}

namespace SL
{
    void REQUEST_KEYBOARD_INPUT(std::function<void(const std::string&)> callback)
    {
        (void)callback;
        TO1_LOG("SL::REQUEST_KEYBOARD_INPUT: keyboard input not available in standalone build\n");
    }

    void CLEAN_MEMORY()
    {
        StringId64 currentTaskNode = DC::Call<"task-get-current-node-id">();
        StringId64 currentSubNode = DC::Call<"task-get-current-subnode-id">();

        DEBUG::PLAY_TASK(currentTaskNode, currentSubNode, 0);

        for (const auto& actorName : loadedActors) {
            MEMORY::UNLOAD_ACTOR(actorName.c_str());
        }
        loadedActors.clear();

        DEBUG::PLAY_TASK(SID("project-fedra"), SID("dialogue-reviewer"), 0);
    }
}

namespace GUI2
{
    StringId64 SPAWN_WIDGET(StringId64 canvas, const char* widgetPath)
    {
        return DC::Call_V2<"gui2-spawn-widget">(canvas, MEMORY::ALLOC_STRING(widgetPath), 0, 1);
    }

    void SET_WIDGET_TEXT(StringId64 path, const char* text)
    {
        DC::Call<"gui2-set-widget-text-devonly">(path, MEMORY::ALLOC_STRING(text));
    }

    void SET_WIDGET_IMAGE(StringId64 wPath, const char* imagePath)
    {
        DC::Call<"gui2-set-widget-image">(wPath, MEMORY::ALLOC_STRING(imagePath));
    }

    void SET_WIDGET_VISIBLE(StringId64 wPath, bool visible)
    {
        DC::Call<"gui2-set-widget-visible">(wPath, visible);
    }

    bool WIDGET_EXISTS(StringId64 widget)
    {
        return DC::Call<"gui2-widget-exists?">(widget);
    }
}

namespace MENU2
{
    void PUSH_PAGE(MenuStack stack, const char* pagePath)
    {
        DC::Call<"menu2-push-page">(stack, MEMORY::ALLOC_STRING(pagePath), 0);
    }

    StringId64 GET_PATH(const char* widget, const char* path)
    {
        const std::string widgetPath = std::string("canvas.hudRoot.hudMenuStack.") + widget;
        return DC::Call<"gui2-find-widget-path">(MEMORY::ALLOC_STRING(path), SID(widgetPath.c_str()), 0, 0, 0, 0, 0, 0, 0);
    }

    StringId64 GET_TOP_PAGE_IN_STACK(MenuStack stack)
    {
        return DC::Call<"menu2-get-top-page-in-stack">(stack, 0);
    }

    StringId64 GET_PATH_IN_CURRENT_PAGE(const char* path, MenuStack stack)
    {
        return DC::Call<"gui2-find-widget-path">(MEMORY::ALLOC_STRING(path), GET_TOP_PAGE_IN_STACK(stack), 0, 0, 0, 0, 0, 0, 0);
    }

    bool IS_PAGE_ON_STACK(MenuStack stack, StringId64 page)
    {
        return DC::Call<"menu2-is-page-on-stack?">(stack, page);
    }
}

namespace engine
{
    bool InitScript(HMODULE gameModule)
    {

                DcManager::Lookup_ = (DcManager::Lookup__ptr)Utility::FindAndPrintPattern(gameModule, Patterns::ScriptManager_Lookup);

                DcManager::LookupInModule_ = (DcManager::LookupInModule__ptr)Utility::FindAndPrintPattern(gameModule, Patterns::ScriptManager_LookupInModule);

                DcManager::LoadActorByName_ = (DcManager::LoadActorByName__ptr)Utility::FindAndPrintPattern(gameModule, Patterns::LoadActorByName);

                DcManager::UnloadActorByName_ = (DcManager::UnloadActorByName__ptr)Utility::FindAndPrintPattern(gameModule, Patterns::UnloadActorByName);

                DcManager::LoadLevelSet_ = (DcManager::LoadLevelSet__ptr)Utility::FindAndPrintPattern(gameModule, Patterns::LoadLevelSet);

                DcManager::LookupObject_ = (DcManager::LookupObject__ptr)Utility::FindAndPrintPattern(gameModule, Patterns::LookupObject);

                DcManager::SkeletonOverrideAddr1 = Utility::FindAndPrintPattern(gameModule, Patterns::SkelOverrideHook_1);

                DcManager::SkeletonOverrideAddr2 = Utility::FindAndPrintPattern(gameModule, Patterns::SkelOverrideHook_2);

        if (!DcManager::Lookup_ || !DcManager::LookupInModule_ ||
            !DcManager::SkeletonOverrideAddr1 || !DcManager::SkeletonOverrideAddr2) {
            TO1_LOG("[script] FATAL: core DC patterns not found.\n");
            return false;
        }

        if (!DcManager::LoadActorByName_)
            TO1_LOG("[script] WARNING: LoadActorByName pattern not found.\n");
        if (!DcManager::UnloadActorByName_)
            TO1_LOG("[script] WARNING: UnloadActorByName pattern not found.\n");
        if (!DcManager::LoadLevelSet_)
            TO1_LOG("[script] WARNING: LoadLevelSet pattern not found.\n");
        if (!DcManager::LookupObject_)
            TO1_LOG("[script] WARNING: LookupObject pattern not found.\n");

        return true;
    }
}
