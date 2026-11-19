#include "net-player.hpp"
#include "match.hpp"
#include "net-engine.hpp"
#include "net-patches.hpp"
#include "anim.hpp"        // NetClearRemoteAnim -- the clear-then-kill lifetime contract
#include "net-hooks.hpp"  // SetDeathBlowVictim -- lets the death blow past the damage hooks
#include <gameplay/melee.hpp>   // NotifyPuppetGone -- tear down an execution before the process dies
#include "net-matchmaking.hpp"
#include "net-gui2.hpp"
#include "net-debug.hpp"
#include "net-replication.hpp"
#include "net-common.hpp"
#include "engine/backpack-prop.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#include <array>
#include <limits>
#include <format>
#include <string>
#include <random>
#include <gameplay/loadout/purchase.hpp>
#include <gameplay/loadout/loadout.hpp>

#undef max

static NetPlayer* Players() { return net::match::Get().players; }

static void EnsureInit()
{
    net::match::Get().EnsurePlayersInit();
}

uintptr_t NetPlayer::Ptr() const
{
    if (isLocal)
        return static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));
    if (!spawned || !npcSid)
        return 0;
    return static_cast<uintptr_t>(DcManager::LookupObject(npcSid));
}

StringId64 NetPlayer::UserSid() const
{
    return isLocal ? SID("player") : npcSid;
}

const char* NetPlayer::LookOrDefault() const
{
    return look[0] ? look : NetPlayers::DefaultLook(netSlot);
}

float NetPlayer::InterpAlpha() const
{
    return (interpDuration > 0.0f)
        ? fminf(interpTimer / interpDuration, 1.0f)
        : 1.0f;
}

bool NetPlayer::IsSprinting() const { return stateNew.isSprinting != 0; }


bool NetPlayer::PushState(const PlayerNetState& s, uint32_t sequence, float now)
{
    if (hasSequence) {
        int32_t delta = static_cast<int32_t>(sequence - lastSequence);
        if (delta <= 0) {
            if (delta > -1024)
                return false;
            lastSequence = sequence;
            stateOld = s;
            stateNew = s;
            stateNew.eventBytes = 0;
            interpTimer = 0.0f;
            lastPacketTime = now;
            hasState = true;
            return true;
        }
    }
    lastSequence = sequence;
    hasSequence = true;

    stateOld = stateNew;
    stateNew = s;
    stateNew.eventBytes = 0;

    interpTimer = 0.0f;
    if (lastPacketTime > 0.0f) {
        float measured = fminf(fmaxf(now - lastPacketTime, 0.008f), 0.2f);
        interpDuration = interpDuration * 0.8f + measured * 0.2f;
    }
    lastPacketTime = now;
    hasState = true;
    return true;
}

static void ResolveLoadout(int slot, StringId64& primary, StringId64& secondary)
{
    primary = 0;
    secondary = SID("pistol-beretta");

    StringId64 mp = 0, ms = 0;
    if (net::matchmaking::SlotLoadout(slot, mp, ms)) {
        primary = mp; // 0 = that player chose no primary
        if (ms) secondary = ms;
    }
}

void NetPlayers::GiveLocalLoadout()
{
    // New life: bump the backpack hand-off's life token (ext g_spawnedFrame). GiveLocalLoadout
    // runs once per local life -- match start and every RespawnLocal -- so this is the mod's
    // equivalent of the ext's per-spawn give, and it lets the held-bag state be voided when the
    // body is replaced. See engine/backpack-prop.hpp.
    ::engine::backpack_prop::BumpLife();

    const StringId64 primary = net::gui2::PrimaryWeaponSid(); // 0 = no primary
    const int primaryStartingAmmo = net::gui2::PrimaryWeaponStartingAmmo();

    StringId64 secondary = net::gui2::SecondaryWeaponSid();
    const int secondaryStartingAmmo = net::gui2::SecondaryWeaponStartingAmmo();

    if (!secondary) secondary = SID("pistol-beretta");

    DC::Call<"player-remove-weapons">();
    ENTITY::KILL_ENTITY(DC::Call<"player-get-flashlight">());

    //weapons
    if (primary) DC::Call<"give-player-weapon">(primary, primaryStartingAmmo, 0, 1);
    DC::Call<"give-player-weapon">(secondary, secondaryStartingAmmo, 0, 1);

    //melee
    DC::Call<"give-player-weapon">(SID("abby-unarmed"), 999, 0, 1);

    //consumables
    DC::Call<"give-player-weapon">(SID("health-kit"), 0, 0, 1);
    DC::Call<"set-total-ammo">(SID("health-kit"), 0);

    //throwables
    DC::Call<"give-player-weapon">(SID("smoke-bomb"), 0, 0, 1);
    DC::Call<"set-total-ammo">(SID("smoke-bomb"), 0);

    static auto GiveUpgrade = GameFn<void(__fastcall*)(uint64_t)>(0x0CB3060);
    GiveUpgrade(SID("long-gun-second-holster"));
    GiveUpgrade(SID("short-gun-second-holster"));

    if (gameplay::purchase::PurchasableOwned()) {
        const gameplay::weapons::View pw =
            gameplay::loadout::Selected(gameplay::weapons::Slot::Purchasable);
        if (pw.sid[0]) {
            const uint64_t ammo = static_cast<uint64_t>(gameplay::purchase::PurchasableAmmo());
            DC::Call<"give-player-weapon">(SID(pw.sid), ammo, 0, 1);
            DC::Call<"set-total-ammo">(SID(pw.sid), ammo);
        }
    }
}

// PICKUPS AND ITEM SPAWNING

void NetPlayers::PollPlayerNeeds()
{
}

void NetPlayers::RequestSpawnItem()
{
    PollPlayerNeeds();
}

int NetPlayers::CalculateItemAmountToSpawn(net::replication::MatchState state)
{
    return (state == net::replication::MatchState::Losing) ? 3 : 0;
}

bool ShouldSpawnHealthKitInBox(net::replication::MatchState matchState)
{
    return ((matchState == net::replication::MatchState::Losing) && (DC::Call_V2<"player-inventory-get-item-quantity">(SID("health-kit")) <= 0) && (PLAYER::GET_HEALTH() < 50));
}

// was winmm_patches.py: box-crowbar-ungated. The compiled base hoisted the shared
// `matchState == Losing` test out of both insert gates into one branch, so the crowbar
// insert also stopped whenever the team was not losing. The crowbar insert must not
// depend on that gate: only the health-kit insert keeps it.
bool ShouldSpawnSmokeBombInBox(net::replication::MatchState matchState)
{
    (void)matchState;
    return (DC::Call_V2<"player-inventory-get-item-quantity">(SID("health-kit")) <= 2);
}

StringId64 NetPlayers::SpawnPickupableItem(StringId64 itemSID, int quantity, Vector3 pos, Quaternion rot)
{

    StringId64 spawnedItem = DC::Call_V2<"spawn-item-pickup">(itemSID, MEMORY::ALLOC_BOUND_FRAME(pos, rot), quantity, 1, 0);

    if (!spawnedItem)
    {
        TO1_LOG("[NetPlayer][ERROR] Failed to spawn '%s'\n", NdGameSdk::Sidbase::StringIdToStringInternal(itemSID));
        return 0;
    }
    else
    {
        TO1_LOG("[NetPlayer][SUCCESS] Spawned '%s' at: %f, %f, %f\n", NdGameSdk::Sidbase::StringIdToStringInternal(itemSID), pos.x, pos.y, pos.z);

        //ENTITY::SET_COLLISION(spawnedItem,true);//simulate gravity
    }

    return spawnedItem;
}

std::vector<std::string> sharedItemSpawnTypes =
{
    "alcohol",
    "binding",
    "blade",
    "canister",
    "explosive",
    "rag",
};

void NetPlayers::SpawnItemsFromSupplyBox(SupplyBox& box)
{

    if (box.itemSpawnPoints.empty())
    {
        TO1_LOG("[NetPlayer] No valid item spawn points found\n");
        return;
    }

    TO1_LOG("[NetPlayer] Detected a total of %d possible spawn points for this box\n", box.itemSpawnPoints.size());

    int itemsToSpawnMin = 3;
    int itemsToSpawnMax = 6;
    int randRange = itemsToSpawnMax - itemsToSpawnMin + 1;
    int itemsToSpawn = rand() % (randRange)+itemsToSpawnMin;

    const int myTeamSlot = net::matchmaking::MyTeam();
    const int enemyTeamSlot = (net::matchmaking::MyTeam() == MM_TEAM_WLF) ? MM_TEAM_SERAPHITES : MM_TEAM_WLF;

    net::replication::MatchState rubberbandingState = net::replication::CalculateRubberbandingTeamState(myTeamSlot, enemyTeamSlot);

    int extraItems = NetPlayers::CalculateItemAmountToSpawn(rubberbandingState); //Add a few items if your team is losing

    itemsToSpawn += extraItems;

    // Don't try to spawn more items than there are spawn locations.
    itemsToSpawn = min(
        itemsToSpawn,
        static_cast<int>(box.itemSpawnPoints.size())
    );

    TO1_LOG("[NetPlayer] Spawning a total of %d items\n", itemsToSpawn);

    box.spawnedItems.clear();

    // Add current weapons in use to pool of available item drops

    std::vector<std::string> availableItemSpawnTypes = sharedItemSpawnTypes;

    StringId64 secondaryWeapSid = net::gui2::SecondaryWeaponSid();
    StringId64 secondaryAmmoType = DC::Call<"weapon-get-default-ammo-id">(secondaryWeapSid);
    const char* secondaryAmmoTypeName = NdGameSdk::Sidbase::StringIdToStringInternal(secondaryAmmoType);

    availableItemSpawnTypes.push_back(secondaryAmmoTypeName);

    StringId64 primaryWeapSid = net::gui2::PrimaryWeaponSid();

    if (!primaryWeapSid) { //get more chances of getting secondary weapon ammo if no primary equipped
        primaryWeapSid = net::gui2::SecondaryWeaponSid();
    }

    StringId64 primaryAmmoType = DC::Call<"weapon-get-default-ammo-id">(primaryWeapSid);
    const char* primaryAmmoTypeName = NdGameSdk::Sidbase::StringIdToStringInternal(primaryAmmoType);

    availableItemSpawnTypes.push_back(primaryAmmoTypeName);

    //shuffle the spawn points order in the vector as to not repeat the same
    //this makes it look like the items spawned 'randomly' without risking choosing the
    //same point in this item spawn wave twice (so that items don't overlap)
    unsigned seedSpawnPoints = std::chrono::system_clock::now().time_since_epoch().count();
    auto rngSpawnPoints = std::default_random_engine{ seedSpawnPoints };
    std::shuffle(std::begin(box.itemSpawnPoints), std::end(box.itemSpawnPoints), rngSpawnPoints);

    //Same as for the spawn points, shuffle once then pick an item from the list
    //so that the same doesn't repeat
    unsigned seedItems = std::chrono::system_clock::now().time_since_epoch().count();
    auto rngItems = std::default_random_engine{ seedItems };
    std::shuffle(std::begin(availableItemSpawnTypes), std::end(availableItemSpawnTypes), rngItems);

    // Insert medkit to index 0
    if (ShouldSpawnHealthKitInBox(rubberbandingState)) availableItemSpawnTypes.insert(availableItemSpawnTypes.begin(), "health-kit");

    // Inser smokebomb to index 1
    if (ShouldSpawnSmokeBombInBox(rubberbandingState)) availableItemSpawnTypes.insert(availableItemSpawnTypes.begin()+1, "crowbar");

    //#TODO: Give purchasable ammo if team is losing and sudden death mode

    int j = 0;

    for (int i = 0; i < itemsToSpawn; ++i)
    {
        int j = i;
        if (j >= availableItemSpawnTypes.size()) j -= availableItemSpawnTypes.size(); //hacky "wrap around" the list if current index is bigger than list

        StringId64 selectedSpawn = box.itemSpawnPoints.at(i);

        uint64_t Loc = LOCATOR::GET_SPAWNER_LOCATOR(selectedSpawn);
        Vector3 pos = LOCATOR::GET_LOCATOR_POSITION(Loc);
        Quaternion rot = LOCATOR::GET_LOCATOR_ROTATION(Loc);

        std::string itemToSpawn = availableItemSpawnTypes.at(j);

        int quantity = 0;

        if (itemToSpawn.find("ammo") != std::string::npos)
        {   //#TODO Remove hacky hardcoded 4 here for actually getting the weapon's current ammo
            quantity = net::replication::CalculateAmmoDrop(4, net::replication::GetAmmoProfileFromAmmoSID(SID(itemToSpawn.c_str())), rubberbandingState);
        }
        else if(itemToSpawn.find("health-kit") != std::string::npos || itemToSpawn.find("smoke-bomb") != std::string::npos)
        {
            quantity = 1;
        }
        else
        {
            quantity = 3; //#TODO #HACK Restore up to 4 again once crafting is available //full ingredient
        }

        if (quantity == 0)
        {
            TO1_LOG("[NetPlayer] Item quantity to spawn is 0, aborting spawning pickup\n");
            continue;
        }

        StringId64 spawnedItem = SpawnPickupableItem(SID(itemToSpawn.c_str()), quantity, pos, rot);

        if (spawnedItem)
        {
            box.spawnedItems.push_back(spawnedItem);
        }
    }
}

//SUPPLY BOXES

void net::player::Tick(float dt)
{
    NetPlayers::UpdateSupplyBoxesInteractBridge();
    NetPlayers::UpdateSupplyBoxes(dt);
}

void net::player::OnMatchLive()
{
    NetPlayers::InitializeSupplyBoxes();
}

static constexpr float SUPPLY_BOX_REFILL_DELAY = 120.0f; //In seconds. 2 minutes like OG Factions
static constexpr float SUPPLY_BOX_REFILL_MIN_DIST = 15.0f; //In meters, how far away from the box the player needs to be for the timer to count for the refill

void NetPlayers::InitializeSupplyBoxes()
{
    TO1_LOG("[NetPlayer] Initializing Supply Boxes...\n");

    NetPlayers::supplyBoxes.clear();

    // was winmm_patches.py: supplybox-12-cap / supplybox-12-resize / supplybox-12-loop.
    // The base hard-coded 4 in three places (the "capacity > 3" in-place re-init, the resize
    // count and the lookup loop bound). Sizing the vector from one constant drops the shortcut
    // and looks up to1-supplybox-1..12; a box number the level lacks stays an empty entry.
    constexpr int num_boxes = 12;
    NetPlayers::supplyBoxes.resize(num_boxes);

    for (int i = 0; i < num_boxes; ++i)
    {
        SupplyBox& box = NetPlayers::supplyBoxes.at(i);
        std::string boxName = "to1-supplybox-" + std::to_string(i+1);

        StringId64 boxSID = SID(boxName.c_str());

        if (!boxSID) continue; //invalid box name, skipping

        box.boxSID = boxSID;

        for (int j = 0; j < 16; j++) {

            std::string itemSpawnPos = std::format("{}-itempos-{}", boxName, j+1);

            StringId64 spawnPointSid = SID(itemSpawnPos.c_str());;

            if (!spawnPointSid) {
                continue; // skip index since it's no longer valid
            }

            uint64_t Loc = LOCATOR::GET_SPAWNER_LOCATOR(spawnPointSid);
            Vector3 pos = LOCATOR::GET_LOCATOR_POSITION(Loc);

            if (pos.x == 0.0f && pos.y == 0.0 && pos.z == 0.0f) {
                continue; // skip index since it somehow leaked and position is in world origin
            }

            box.itemSpawnPoints.push_back(spawnPointSid);

            TO1_LOG("[NetPlayer] Initialized %s for %s, with pos %llu\n", itemSpawnPos.c_str(), boxName.c_str(), pos);
        }

        box.CreateSupplyBoxInteractable();
        box.EnableSupplyBoxInteraction();

        TO1_LOG("[NetPlayer] Finished Initializing Supply Box %s\n", boxName.c_str());
    }

    if (NetPlayers::supplyBoxes.empty())
    {
        TO1_LOG("[NetPlayer][WARNING] Couldn't find any Supply Boxes spawners to initialize\n");
    }
}

void NetPlayers::UpdateSupplyBoxes(float dt)
{
    if (NetPlayers::supplyBoxes.empty()) return;
    const float now = net::match::Get().NowSec();


    for (SupplyBox& box : NetPlayers::supplyBoxes)
    {
        if (box.opened) {

            if (box.GetDistanceToPlayer() < SUPPLY_BOX_REFILL_MIN_DIST) //If player is too close to the box, we offset the timer to refill to force them to move away from the box
            {
                box.refillTime += dt;
            }
            else //Only try to reset the box if we're far enough away, even if we're past the reset time goal
            {
                if (now >= box.refillTime)
                {
                    NetPlayers::ResetSupplyBox(box);
                }
            }
        }
    }
}

void NetPlayers::ResetSupplyBox(SupplyBox& box)
{
    box.refillTime = -1.0f;
    box.opened = false;
    box.EnableSupplyBoxInteraction();
    ENTITY::ANIMATE(box.boxSID, SID("close"), MEMORY::ALLOC_ARRAY(16, 1, 1)); //reset to closed

    //Despawn items already spawned by box previously
    for (StringId64 item : box.spawnedItems) DC::Call<"kill-entity">(item);

    box.spawnedItems = {};
    box.spawnedItems.resize(0);
}

//Interact
void NetPlayers::UpdateSupplyBoxesInteractBridge()
{
    if (NetPlayers::supplyBoxes.empty()) return; //early out if no interactables on the level

    if (!net::gui2::InMatch() || net::gui2::ScoreboardShowing() ||
        net::gui2::PauseMenuOpen() || net::matchmaking::MyTeam() < 0) {
        return;
    }

    NetPlayers::sortedAvailableSupplyBoxes = NetPlayers::supplyBoxes;

    std::sort(NetPlayers::sortedAvailableSupplyBoxes.begin(), NetPlayers::sortedAvailableSupplyBoxes.end(), [](NetPlayers::SupplyBox& lhs, NetPlayers::SupplyBox& rhs)
    {
        float lhs_distance = lhs.GetDistanceToPlayer();
        float rhs_distance = rhs.GetDistanceToPlayer();
        return lhs_distance < rhs_distance;
    });

    NetPlayers::supplyBoxes = NetPlayers::sortedAvailableSupplyBoxes;

    NetPlayers::SupplyBox& winnerBox = NetPlayers::supplyBoxes.front();

    float minDistance = std::numeric_limits<float>::max();
    const float kInteractDistance = 1.5f; //In meters
    float distanceChosenBox = winnerBox.GetDistanceToPlayer();
    bool playerInInteractRadius = distanceChosenBox <= kInteractDistance;

    bool kPressedInteract = DC::JOYPAD_COMMAND_ACTIVE(SID("interact"));
    const bool kIsAiming = DC::Call<"player-is-aiming?">();

    if (kPressedInteract && !kIsAiming && playerInInteractRadius) {
        TO1_LOG("[NetPlayer] Interacted with %s, which is %s\n", NdGameSdk::Sidbase::StringIdToStringInternal(winnerBox.boxSID), winnerBox.opened ? "opened" : "not opened");
        NetPlayers::OpenSupplyBox(winnerBox);
    }
}

void NetPlayers::OpenSupplyBox(SupplyBox& box)
{
    if (box.CanOpenSupplyBox(net::match::Get().NowSec()) == false ) return;

    ENTITY::ANIMATE(box.boxSID, SID("open"), MEMORY::ALLOC_ARRAY(16, 1, 1));

    DC::Call<"player-force-crouch">();
    ENTITY::ANIMATE(SID("player"), SID("t2r-abby-scav-low-crate-open-start"), MEMORY::ALLOC_ARRAY(16, 4, 1));

    AUDIO::PLAY_SOUND(SID("sfx-t2r-igc-loot-crate-open"));

    box.DisableSupplyBoxInteraction();
    NetPlayers::SpawnItemsFromSupplyBox(box);
    box.opened = true;
    box.refillTime = net::match::Get().NowSec() + SUPPLY_BOX_REFILL_DELAY;
}

static void SetupModelChangerSpawner(Vector3 position, Quaternion rotation, const char* modelName)
{
    Spawner* spawner = ENTITY::GET_SPAWNER("starlight-model-changer");
    spawner->m_pos = position;
    spawner->m_posW = 0.0f;
    spawner->m_rot = rotation;
    std::strcpy(reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(spawner) + 0x188), modelName);
}

// "spawn-player kill skip" byte: 0x75 while a remote spawn is in flight,
// restored to 0x74 afterwards.
static constexpr uint32_t kSpawnKillSkipRva = 0x0CADDD8; // deprecated

static constexpr uintptr_t kPathfindRequestHandle = 0x74F8;
static constexpr uint32_t kRvaPathfindManager = 0x93BD110;
static constexpr uint32_t kRvaPathfindManagerRemoveRequest = 0x1CE2320;

static void InvalidatePathfindHandle(uintptr_t npc)
{
    if (!npc) { TO1_LOG("[NAV] pathfind-handle init skipped: no process yet\n"); return; }
    uint32_t& handle = *reinterpret_cast<uint32_t*>(npc + kPathfindRequestHandle);
    TO1_LOG("[NAV] pathfind handle %08X -> FFFFFFFF\n", handle);
    if (static_cast<uint16_t>(handle) != 0xFFFF) {
        static auto removeRequest = GameFn<void(__fastcall*)(uintptr_t, const uint32_t*)>(kRvaPathfindManagerRemoveRequest);
        removeRequest(g_base + kRvaPathfindManager, &handle);
    }
    handle = 0xFFFFFFFF;
}

void NetPlayer::Spawn(Vector3 position, Quaternion rotation)
{
    if (isLocal) {
        TO1_LOG("[NetPlayer] Spawn() called on local player — use NetPlayers::RespawnLocal\n");
        return;
    }
    EnsureInit();

    StringId64 npc = DC::Call<"spawn-entity-at-with-look">(SID("starlight-mp-client-1"), // spawner with multispawn
        LOCATOR::GET_SPAWNER_LOCATOR(SID("to1-spawn-point-1")),
        SID(LookOrDefault()));
    if (!npc) {
        TO1_LOG("[NetPlayer] slot %d: NPC spawn failed\n", netSlot);
        return;
    }
    npcSid = npc;
    spawned = true;
    dead = false;
    dying = false;
    triedNpcKill = false;
    animDetached = false;
    engineHold = EngineHold::None;
    lastSyncedHealth = -1.0f;
    ledger.Reset();
    puppetWeapon = 0;
    appliedTeamRel = -2;

    DC::Call<"npc-set-script-mode">(npc, 0x2, 0x1); // disable npc ai

    DC::Call<"npc-set-ragdoll-allowed">(npc, 1ull, 0ull);
    DC::Call<"npc-set-ignore">(npc, 1ull);
    DC::Call<"set-object-invisible-to-ai">(npc, 1ull);
    DC::Call<"npc-enable-hit-reactions">(npc, 0ull);
    DC::Call<"npc-enable-stun">(npc, 0ull);
    DC::Call<"npc-enable-dodges">(npc, 0ull);
    DC::Call<"npc-set-blind">(npc, 1ull);
    DC::Call<"npc-set-deaf">(npc, 1ull);
    DC::Call<"npc-set-allow-adjust-to-other-chars">(npc, 0ull);

        const uint64_t  mode = DC::Call<"npc-get-script-mode">(npc);
        const uintptr_t np = Ptr();
        TO1_LOG("[NetPlayer] slot %d puppet inert: scriptMode=%llu ragdollBody=%p ragdollAllowed=%d\n",
            netSlot, static_cast<unsigned long long>(mode),
            np ? *reinterpret_cast<void**>(np + 0x898) : nullptr,
            (np && (*reinterpret_cast<uint8_t*>(np + 0x9FD) & 1)) ? 1 : 0);

    NPC::SET_FACTION_ID(npc, Faction::NetTeam2);

    InvalidatePathfindHandle(Ptr());

        net::hooks::PuppetTeleportScope allow;   // ours, not the engine's fall watchdog
        DC::Call<"npc-teleport-to">(npc, MEMORY::ALLOC_BOUND_FRAME(position, rotation));

    StringId64 primary, secondary;
    ResolveLoadout(netSlot, primary, secondary);
    const StringId64 spawnWeapon = primary ? primary : secondary;
    if (spawnWeapon) {
        DC::Call<"npc-give-weapon">(npc, spawnWeapon, 999, 0x1);
        net::engine::AttachWeaponToCorrectHand(Ptr(), spawnWeapon);
    }

    TO1_LOG("[NetPlayer] spawned slot %d (\"%s\") as NPC sid=0x%016llX\n",
        netSlot, name, static_cast<unsigned long long>(npc));
}

// ---- RAGDOLL HANDOFF, engine side --------------------------------------------------
static constexpr uintptr_t kOffRagdollComposite = 0x898;
static constexpr uintptr_t kOffCompMotionType = 0x68;
static constexpr uintptr_t kOffCompRagdollCtrl = 0x140;
static constexpr uintptr_t kOffCtrlRagdollOn = 0x44;
static constexpr uintptr_t kOffRagdollAllowed = 0x9FD;    // bit0
static constexpr uintptr_t kOffRagdollDelay = 0x1C2C;   // float, -1.0f by default
static constexpr uint32_t  kMotionPhysicsDriven = 3;

static constexpr uintptr_t kRvaPhysicalizeRagdoll = 0x1CEB020;
static constexpr uintptr_t kRvaRagdollSetOn = 0x1E143C0;
using PhysicalizeRagdollFn = char(__fastcall*)(uintptr_t npc, uint8_t full, uint64_t settings,
    uintptr_t unused, char noCollide);
using RagdollSetOnFn = int64_t(__fastcall*)(uintptr_t ctrl, char on);

static constexpr float kRagdollDelaySec = 0.05f;
static constexpr uint64_t kRagdollForceMs = 400;
static constexpr uint64_t kDeathSecondChanceMs = 1200;   // try npc-die after this
static constexpr uint64_t kDeathGiveUpMs = 4000;   // destroy the body after this

void NetPlayer::ReleaseToEngine()
{
    if (isLocal || EngineOwned()) return;

    NetClearRemoteAnim(netSlot, /*dropLastGood=*/false);
    animDetached = true;

    engineHold = EngineHold::Releasing;
    dead = true;    // stops ApplyRemote and keeps AutoSpawnRemote away from the slot
    dying = false;   // no death ladder: the engine owns this death end to end

    TO1_LOG("[NetPlayer] slot %d (\"%s\") released to the engine (sid=0x%016llX)\n",
        netSlot, name, static_cast<unsigned long long>(npcSid));
}

void NetPlayer::SuspendForEngineAction()
{
    if (isLocal || EngineOwned()) return;

    NetClearRemoteAnim(netSlot, /*dropLastGood=*/false);
    animDetached = true;

    if (npcSid && ENTITY::DOES_EXIST(npcSid)) {
        const Transform cur = LOCATOR::GET_TRANSFORM(npcSid);
        net::hooks::PuppetTeleportScope allow;   // ours, not the engine's fall watchdog
        DC::Call<"npc-teleport-to">(npcSid, MEMORY::ALLOC_BOUND_FRAME(cur.Position, cur.Rotation));
    }

    engineHold = EngineHold::Suspended;

    TO1_LOG("[NetPlayer] slot %d (\"%s\") suspended for an engine melee action\n", netSlot, name);
}

void NetPlayer::ResumeFromEngineAction()
{
    if (isLocal || engineHold != EngineHold::Suspended) return;

    engineHold = EngineHold::None;
    animDetached = false;

    lastSyncedHealth = -1.0f;
    appliedTeamRel = -2;

    TO1_LOG("[NetPlayer] slot %d (\"%s\") resumed from engine melee action\n", netSlot, name);
}

void NetPlayer::ReclaimFromEngine()
{
    if (isLocal || engineHold != EngineHold::Releasing) return;

    if (!npcSid || !ENTITY::DOES_EXIST(npcSid) ||
        DC::Call<"character-is-dead?">(npcSid) != 0) {
        TO1_LOG("[NetPlayer] slot %d (\"%s\") NOT reclaimed -- the engine did kill it\n",
            netSlot, name);
        return;
    }

    engineHold = EngineHold::None;
    dead = false;
    dying = false;
    triedNpcKill = false;
    animDetached = false;

    lastSyncedHealth = -1.0f;
    appliedTeamRel = -2;

    TO1_LOG("[NetPlayer] slot %d (\"%s\") reclaimed -- our finisher never killed it\n",
        netSlot, name);
}

void NetPlayer::Die()
{
    if (isLocal || dead) return;
    dead = true;                       // gate replication even if the NPC is already gone
    dyingSinceMs = GetTickCount64();   // stamped here so TickDeath's ladder always has a clock

    if (!spawned || !npcSid || !ENTITY::DOES_EXIST(npcSid)) {
        NetClearRemoteAnim(netSlot, /*dropLastGood=*/false);
        animDetached = true;
        npcSid = 0;
        spawned = false;
        return;
    }

    NetFreezePuppetOverlays(netSlot);

    const uintptr_t target = static_cast<uintptr_t>(DcManager::LookupObject(npcSid));
    const uintptr_t local = NetPlayers::LocalPtr();

    if (!target || !local) {
        TO1_LOG("[NetPlayer] slot %d (\"%s\") death blow unavailable -- destroying puppet\n",
            netSlot, name);
        NetClearRemoteAnim(netSlot);
        animDetached = true;
        ENTITY::KILL_ENTITY(npcSid);
        npcSid = 0;
        spawned = false;
        return;
    }

    DC::Call<"npc-set-ragdoll-allowed">(npcSid, 1ull, 0ull);

    if (const uintptr_t npNow = Ptr())
        *reinterpret_cast<float*>(npNow + kOffRagdollDelay) = kRagdollDelaySec;

    uint64_t weapon = net::engine::GetPlayerWeaponId(local);
    if (!weapon) weapon = SID("pistol-beretta");

    net::hooks::SetDeathBlowVictim(target);
    const bool applied = net::engine::FireAttack(target, local, weapon, 0, 1.0e6f);
    net::hooks::SetDeathBlowVictim(0);

    lastSyncedHealth = 0.0f;
    ledger.Reset();

    dying = true;
    triedNpcKill = false;
    animDetached = false;

    TO1_LOG("[NetPlayer] slot %d (\"%s\") death blow: fire=%d, sid=0x%016llX\n",
        netSlot, name, applied ? 1 : 0, static_cast<unsigned long long>(npcSid));
}

void NetPlayer::TickDeath()
{
    if (!dead) return;
    if (EngineReleasing()) return;   // the engine is running this death; stay out of it entirely

    if (!npcSid || !ENTITY::DOES_EXIST(npcSid)) {
        if (!animDetached) { NetClearRemoteAnim(netSlot, /*dropLastGood=*/false); animDetached = true; }
        dying = false;
        return;
    }

    const uint64_t  elapsed = GetTickCount64() - dyingSinceMs;
    const uintptr_t np = Ptr();
    const uintptr_t comp = np ? *reinterpret_cast<uintptr_t*>(np + kOffRagdollComposite) : 0;
    const uintptr_t ctrl = comp ? *reinterpret_cast<uintptr_t*>(comp + kOffCompRagdollCtrl) : 0;
    const uint32_t  motion = comp ? *reinterpret_cast<uint32_t*>(comp + kOffCompMotionType) : 0u;
    const bool ragdollLive = (motion == kMotionPhysicsDriven);

    if (dying && DC::Call<"character-is-dead?">(npcSid) != 0) {
        dying = false;
        TO1_LOG("[NetPlayer] slot %d (\"%s\") death latched in %llums -- corpse: allowed=%d "
            "comp=%p motion=%u ctrlOn=%d delay=%.2f\n",
            netSlot, name, static_cast<unsigned long long>(elapsed),
            (np && (*reinterpret_cast<uint8_t*>(np + kOffRagdollAllowed) & 1)) ? 1 : 0,
            reinterpret_cast<void*>(comp), motion,
            ctrl ? static_cast<int>(*reinterpret_cast<uint8_t*>(ctrl + kOffCtrlRagdollOn)) : -1,
            np ? *reinterpret_cast<float*>(np + kOffRagdollDelay) : 0.0f);
    }

    if (!animDetached) {
        if (!ragdollLive && np) {
            DC::Call<"npc-set-ragdoll-allowed">(npcSid, 1ull, 0ull);
            if (!dying && elapsed > kRagdollForceMs) {
                if (ctrl && *reinterpret_cast<uint8_t*>(ctrl + kOffCtrlRagdollOn))
                    reinterpret_cast<RagdollSetOnFn>(g_base + kRvaRagdollSetOn)(ctrl, 0);
                const char rc = reinterpret_cast<PhysicalizeRagdollFn>(
                    g_base + kRvaPhysicalizeRagdoll)(np, 1, 0, 0, 0);
                TO1_LOG("[NetPlayer] slot %d (\"%s\") ragdoll did not take in %llums -- forced "
                    "PhysicalizeRagdoll rc=%d (motion was %u)\n",
                    netSlot, name, static_cast<unsigned long long>(elapsed),
                    static_cast<int>(rc), motion);
            }
        }

    }

    if (!dying) return;

    if (!triedNpcKill && elapsed > kDeathSecondChanceMs) {
        triedNpcKill = true;
        net::hooks::SetDeathBlowVictim(np);
        const uint64_t rc = DC::Call<"npc-die">(npcSid, 0ull, 0ull, 0ull, 0ull);
        net::hooks::SetDeathBlowVictim(0);
        TO1_LOG("[NetPlayer] slot %d (\"%s\") blow did not latch in %llums -- npc-die rc=%lld\n",
            netSlot, name, static_cast<unsigned long long>(elapsed),
            static_cast<long long>(rc));
        return;
    }

    if (elapsed > kDeathGiveUpMs) {
        dying = false;
        TO1_LOG("[NetPlayer] slot %d (\"%s\") never died in %llums -- destroying body\n",
            netSlot, name, static_cast<unsigned long long>(elapsed));
        NetClearRemoteAnim(netSlot);
        animDetached = true;
        ENTITY::KILL_ENTITY(npcSid);
        npcSid = 0;
        spawned = false;
    }
}

void NetPlayer::Respawn(Vector3 position, Quaternion rotation)
{
    if (isLocal) {
        NetPlayers::RespawnLocal(position, rotation);
        return;
    }

    gameplay::melee::NotifyPuppetGone(netSlot);

    if (npcSid) {
        NetClearRemoteAnim(netSlot);
        if (ENTITY::DOES_EXIST(npcSid)) {
            TO1_LOG("[NetPlayer] slot %d (\"%s\") destroying old puppet sid=0x%016llX\n",
                netSlot, name, static_cast<unsigned long long>(npcSid));
            ENTITY::KILL_ENTITY(npcSid);
        }
    }

    npcSid = 0;
    spawned = false;
    dead = false;
    Spawn(position, rotation);
}

void NetPlayer::Reanchor(Vector3 position, Quaternion rotation)
{
    if (isLocal) return;

    if (!npcSid || !spawned || !ENTITY::DOES_EXIST(npcSid)) {
        npcSid = 0; spawned = false;
        Spawn(position, rotation);
        return;
    }
    InvalidatePathfindHandle(Ptr());

        net::hooks::PuppetTeleportScope allow;
        DC::Call<"npc-teleport-to">(npcSid, MEMORY::ALLOC_BOUND_FRAME(position, rotation));

    const Transform after = LOCATOR::GET_TRANSFORM(npcSid);
    if (after.Position.distance(position) > 1.0f)
        TO1_LOG("[NetPlayer][WARN] slot %d reanchor teleport ignored: asked (%.2f %.2f %.2f), puppet is at "
            "(%.2f %.2f %.2f) -- something still owns this NPC\n",
            netSlot, position.x, position.y, position.z,
            after.Position.x, after.Position.y, after.Position.z);

    lastSyncedHealth = -1.0f;   // force ApplyRemote to re-apply the owner's health
    ledger.Reset();             // drop our stale estimate + re-arm the kill reticle
    appliedTeamRel = -2;        // re-apply team faction next ApplyRemote
    TO1_LOG("[NetPlayer] re-anchored slot %d (\"%s\") NPC sid=0x%016llX (no destroy)\n",
        netSlot, name, static_cast<unsigned long long>(npcSid));
}

void NetPlayer::Despawn()
{
    if (!isLocal)
        gameplay::melee::NotifyPuppetGone(netSlot);

    if (!isLocal && spawned && npcSid) {
        TO1_LOG("[NetPlayer] despawning slot %d (\"%s\") NPC sid=0x%016llX\n",
            netSlot, name, static_cast<unsigned long long>(npcSid));
        NetClearRemoteAnim(netSlot);
        NetSetSlotStreamAlive(netSlot, false);
        ENTITY::KILL_ENTITY(npcSid);
    }
    spawned = false;
    npcSid = 0;
    ResetRuntime();
}

void NetPlayer::ResetRuntime()
{
    stateNew = {};
    stateOld = {};
    hasState = false;
    hasSequence = false;
    lastSequence = 0;
    interpTimer = 0.0f;
    interpDuration = 0.033f;
    lastPacketTime = 0.0f;
    announcedSpawn = false;
    dead = false;
    dying = false;
    triedNpcKill = false;
    animDetached = false;
    engineHold = EngineHold::None;
    lastSyncedHealth = -1.0f;
    ledger.Reset();
    puppetWeapon = 0;
    appliedTeamRel = -2;
    aimMarkActive = false;
    aimMarkAge = 0.0f;
    radarFade = 0.0f;
    pendingLaunchValid = false;
    memset(pendingLaunchVel, 0, sizeof(pendingLaunchVel));
    pendingLaunchAt = -1000.0f;
    for (int i = 0; i < kPingSamples; ++i) pingRing[i] = kPingUnknown;
    pingRingPos = 0;
    reportedPingMs = kPingUnknown;
}

void NetPlayer::RecordPing(uint32_t rttMs)
{
    pingRing[pingRingPos] = static_cast<uint16_t>((std::min)(rttMs, static_cast<uint32_t>(kPingUnknown - 1)));
    pingRingPos = (pingRingPos + 1) % kPingSamples;
}

int NetPlayer::PingMs() const
{
    uint16_t best = kPingUnknown;
    for (int i = 0; i < kPingSamples; ++i)
        best = (std::min)(best, pingRing[i]);
    return best == kPingUnknown ? -1 : static_cast<int>(best);
}

void NetPlayer::StopMelee() { net::engine::StopMeleeAction(UserSid()); }
void NetPlayer::ForceMelee(StringId64 actionSid) { net::engine::ForceMeleeAction(Ptr(), actionSid); }

void NetPlayer::ApplyTeamFaction()
{
    if (!npcSid) return;
    const int myTeam = net::matchmaking::MyTeam();
    const int t = net::match::Get().TeamOf(netSlot);
    if (myTeam < 0 || t < 0) return;
    const int rel = (t == myTeam) ? 1 : 0;
    if (rel == appliedTeamRel) return;
    if (rel == 1) {
        NPC::SET_FACTION_ID(npcSid, Faction::NetTeam1);      // own team
    }
    else {
        NPC::SET_FACTION_ID(npcSid, Faction::NetTeam2);      // opponent
    }
    appliedTeamRel = rel;
}

namespace NetPlayers {

    NetPlayer& BySlot(int netSlot)
    {
        EnsureInit();
        if (netSlot < 0) netSlot = 0;
        if (netSlot >= kMaxNetPlayers) netSlot = kMaxNetPlayers - 1;
        return Players()[netSlot];
    }

    bool SlotIsFriendly(int netSlot)
    {
        if (netSlot < 0 || netSlot >= kMaxNetPlayers) return false;
        const int myTeam = net::match::Get().MyTeam();
        if (myTeam < 0) return false;
        const int t = net::match::Get().TeamOf(netSlot);
        return t >= 0 && t == myTeam;
    }

    NetPlayer* Local()
    {
        EnsureInit();
        for (int i = 0; i < kMaxNetPlayers; i++)
            if (Players()[i].active && Players()[i].isLocal)
                return &Players()[i];
        return nullptr;
    }

    uintptr_t LocalPtr()
    {
        if (NetPlayer* l = Local())
            return l->Ptr();
        return static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));  // pre-session
    }

    NetPlayer* FromEnginePtr(uintptr_t playerPtr)
    {
        if (!playerPtr) return nullptr;
        EnsureInit();
        for (int i = 0; i < kMaxNetPlayers; i++) {
            NetPlayer& p = Players()[i];
            if (!p.active) continue;
            if (p.Ptr() == playerPtr)
                return &p;
        }
        return nullptr;
    }

    int ActiveCount()
    {
        EnsureInit();
        int n = 0;
        for (int i = 0; i < kMaxNetPlayers; i++)
            if (Players()[i].active) n++;
        return n;
    }

    void CountActivePerTeam(int& team0, int& team1)
    {
        EnsureInit();
        team0 = 0;
        team1 = 0;
        for (int i = 0; i < kMaxNetPlayers; i++) {
            if (!Players()[i].active) continue;
            int t = net::match::Get().TeamOf(Players()[i].netSlot);
            if (t == MM_TEAM_SERAPHITES) team0++;
            else if (t == MM_TEAM_WLF)   team1++;
        }
    }

    void RemoteRange::It::Advance()
    {
        while (slot < kMaxNetPlayers && !BySlot(slot).IsActiveRemote())
            slot++;
    }

    void Reset()
    {
        EnsureInit();
        for (int i = 0; i < kMaxNetPlayers; i++) {
            NetPlayer& p = Players()[i];
            if (p.IsActiveRemote() && p.spawned)
                p.Despawn();
            p.active = false;
            p.isLocal = false;
            p.spawned = false;
            p.ResetRuntime();
            p.team = -1;
            p.tamperFlags = 0;
            p.tamperPid = 0;
            memset(p.tamperProcess, 0, sizeof(p.tamperProcess));
        }
    }

    void SetupLocal(int netSlot, const char* localName, const char* localLook)
    {
        EnsureInit();
        NetPlayer& p = BySlot(netSlot);
        p.active = true;
        p.isLocal = true;
        p.spawned = true;
        strncpy_s(p.name, localName && localName[0] ? localName : "Player", _TRUNCATE);
        strncpy_s(p.look, localLook ? localLook : "", _TRUNCATE);
    }

    void ApplyRoster(const RosterPayload& roster, int mySlot)
    {
        EnsureInit();
        for (uint8_t i = 0; i < roster.count && i < kMaxNetPlayers; i++) {
            const RosterEntry& e = roster.players[i];
            if (e.slot >= kMaxNetPlayers) continue;
            NetPlayer& p = BySlot(e.slot);

            bool wasActive = p.active;
            bool nowActive = e.active != 0;
            bool isMe = (e.slot == mySlot);

            bool occupantChanged = wasActive && nowActive && !isMe &&
                strncmp(p.name, e.name, kNetNameLen) != 0;

            if (nowActive) {
                strncpy_s(p.name, e.name, _TRUNCATE);
                strncpy_s(p.look, e.look, _TRUNCATE);
                p.team = (e.team == kTeamUnknown) ? -1 : static_cast<int>(e.team);
            }

            p.isLocal = isMe;
            if (isMe) {
                p.active = true;
                p.spawned = true;
                continue;
            }

            if (wasActive && !nowActive) {
                TO1_LOG("[NET] roster: \"%s\" (slot %d) left\n", p.name, e.slot);
                const std::string msg = std::format("{} disconnected", p.name);
                HUD::SHOW_NOTIFICATION(msg.c_str(), 5.0f);
                p.Despawn();
                p.active = false;
                p.team = -1;
            }
            else if (!wasActive && nowActive) {
                TO1_LOG("[NET] roster: \"%s\" joined as slot %d\n", p.name, e.slot);
                p.active = true;
                p.ResetRuntime();
            }
            else {
                p.active = nowActive;
                if (occupantChanged) {
                    TO1_LOG("[NET] roster: slot %d occupant changed -> \"%s\"\n", e.slot, e.name);
                    p.Despawn();
                }
            }
        }
    }

    void OnPeerDropped(int netSlot)
    {
        NetPlayer& p = BySlot(netSlot);
        if (!p.active || p.isLocal) return;
        TO1_LOG("[NET] \"%s\" (slot %d) dropped\n", p.name, netSlot);
        const std::string msg = std::format("{} disconnected", p.name);
        HUD::SHOW_NOTIFICATION(msg.c_str(), 5.0f);
        p.Despawn();
        p.active = false;
    }

    void RespawnLocal(Vector3 position, Quaternion rotation)
    {
        const char* look = Local() ? Local()->LookOrDefault() : DefaultLook(0);
        SetupModelChangerSpawner(position, rotation, look);

        uintptr_t deadPlayerPtr = LocalPtr();

        DC::Call<"spawn-player">(MEMORY::ALLOC_SPAWN_PLAYER(SID("starlight-model-changer")), 0);

        GiveLocalLoadout();

        if (deadPlayerPtr)
            net::engine::KillProcess(deadPlayerPtr);
    }

    static const char* kSeraphiteLooks[] = { "scar-male-13" };
    static const char* kWlfLooks[] = { "militia-male-11-backpack" };

    const char* RandomLookForTeam(int team)
    {
        const char** pool = kSeraphiteLooks;
        int count = static_cast<int>(sizeof(kSeraphiteLooks) / sizeof(kSeraphiteLooks[0]));
        if (team == MM_TEAM_WLF) {
            pool = kWlfLooks;
            count = static_cast<int>(sizeof(kWlfLooks) / sizeof(kWlfLooks[0]));
        }
        if (count <= 1) return pool[0];
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> dist(0, count - 1);
        return pool[dist(rng)];
    }

    const char* DefaultLook(int netSlot)
    {
        int team = net::matchmaking::SlotTeam(netSlot);
        return RandomLookForTeam(team < 0 ? MM_TEAM_SERAPHITES : team);
    }

}
