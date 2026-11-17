#include "menu/net-menu.hpp"
#include "anim.hpp"
#include "engine/debugdraw.hpp"
#include "engine/meshray.hpp"
#include "gameplay/barrel-check.hpp"
#include "log.hpp"
#include "net-hooks.hpp"
#include "net-debug.hpp"
#include "gameplay/perks/perk-registry.hpp"
#include <gameplay/loadout/purchase.hpp>
#include "net-player.hpp"
#include "net-session.hpp"


namespace net::menu {

void LoadMpAssets() {
  if (multiplayer)
    return;

  g_forcePlayerSkel = 2; // Force abby anims on local player

  FEDRA::LOAD_LOOK_ASSETS("scar-male-13");
  FEDRA::LOAD_LOOK_ASSETS("militia-male-11-backpack");
  FEDRA::LOAD_LOOK_ASSETS("ellie-astro-no-helmet");
  FEDRA::LOAD_LOOK_ASSETS("ellie");
  MEMORY::LOAD_ACTOR("invisible-cube", AssetType::Actor, false);
  // MEMORY::LOAD_LEVEL("starlight-mp-test"); - now loaded in level-sets to1-all entry
  MEMORY::LOAD_LEVEL("starlight-all-ellie-anims");
  MEMORY::LOAD_LEVEL("starlight-all-human-npc-anims");
  // MEMORY::LOAD_LEVEL("to1-all-weapons"); - now loaded in level-sets to1-all entry
  net::hooks::InstallMultiplayerHooks();

  multiplayer = true;
  TO1_LOG("[NET] MP assets loaded, hooks installed\n");
}

void ResetMpAssetState() {
  g_forcePlayerSkel = 0; // stop forcing abby anims (engine default)
  multiplayer = false;
}

void ApplyLocalModel() {
  net::session::EnsureLocalTeamLook();
  const char *look = net::session::LocalLook();
  if (!look[0])
    look = NetPlayers::DefaultLook(net::session::MySlot());
  FEDRA::CHANGE_MODEL(look);
}
} // namespace net::menu

bool noVis = false;
bool fastCrouchSpeed = false;
bool disableWeaponSway = false;

#ifdef TO1_DEBUG

namespace {
MenuManager g_menuManager;

bool s_comboKey1 = false; // r3
bool s_comboKey2 = false; // down
int s_comboTimeout = 0;
constexpr int kComboWindowFrames = 10;

void ToggleMenu() {
  s_debugMenuOpen = !s_debugMenuOpen;
  if (s_debugMenuOpen && !g_menuManager.PopMenu()) {
    std::unique_ptr menu = std::make_unique<Menu>("NET");

    menu->AddMenuItem(std::make_unique<ActionMenuItem>(
        "APPLY MODEL",
        []() {
          net::menu::ApplyLocalModel();
          NetPlayers::GiveLocalLoadout();
        },
        "Apply local model."));


    menu->AddMenuItem(std::make_unique<ActionMenuItem>(
        "LOAD TO1 LEVELS",
        []() {
            MEMORY::LOAD_LEVEL("starlight-mp-test");
            MEMORY::LOAD_LEVEL("tlou-mp-all");
            MEMORY::LOAD_LEVEL("to1-mp-all");
        },
        ""));

    menu->AddMenuItem(
        std::make_unique<BooleanMenuItem>("DISABLE MAP VIS", noVis, []() {}, ""));

    menu->AddMenuItem(std::make_unique<BooleanMenuItem>(
        "BARREL CHECK", gameplay::barrelcheck::g_enabled, []() {},
        "Deny local shots when the body-to-reticle ray is blocked."));

    menu->AddMenuItem(std::make_unique<BooleanMenuItem>(
        "BARREL CHECK DRAW", gameplay::barrelcheck::g_drawDebug, []() {},
        "Draw the body ray and camera hit point."));

    menu->AddMenuItem(std::make_unique<BooleanMenuItem>(
        "ENGINE DEBUG DRAW", engine::debugdraw::g_enabled, []() {},
        "Enable the engine 3D debug-prim renderer (PrimServer)."));

    menu->AddMenuItem(std::make_unique<BooleanMenuItem>(
        "ORIGIN PILLAR", engine::debugdraw::g_drawOriginPillar, []() {},
        "Draw a tall 3D box at the world origin."));

    menu->AddMenuItem(std::make_unique<BooleanMenuItem>(
        "SURFACE PROBE (MESH RAY)", engine::meshray::g_enabled, []() {},
        "Kick engine mesh raycasts to display the aimed-at surface type. UNSAFE on levels with missing materials: a ray into a material-less surface wedges the mesh raycaster and exhausts single-frame memory."));

    menu->AddMenuItem(std::make_unique<FloatScrollerMenuItem>(
        "HAND LIFT", gameplay::barrelcheck::g_rHandVOffset, 0.0f, 0.5f, 0.01f, []() {},
        "Vertical lift applied to the barrel-check cast origin."));

    menu->AddMenuItem(std::make_unique<FloatScrollerMenuItem>(
        "HAND HORIZONTAL OFFSET", gameplay::barrelcheck::g_rHandHOffset, -0.5f, 0.5f, 0.01f, []() {},
        "Sideways offset applied to the barrel-check cast origin."));

    menu->AddMenuItem(std::make_unique<ActionMenuItem>("PRINT HEALTH KIT COUNT", []() {
        uint64_t healthKitCount = DC::Call_V2<"player-inventory-get-item-quantity">(SID("health-kit"));
		TO1_LOG("[NET] Health kit count: %llu\n", healthKitCount);
        }, ""));

    menu->AddMenuItem(std::make_unique<BooleanMenuItem>("FAST CROUCH SPEED", fastCrouchSpeed, []() {},
        ""));

    menu->AddMenuItem(std::make_unique<BooleanMenuItem>("DISABLE WEAPON SWAY", disableWeaponSway, []() {},
        ""));

    menu->AddMenuItem(std::make_unique<ActionMenuItem>("INITIALIZE SUPPLY BOXES", []() {
        MEMORY::LOAD_LEVEL("starlight-mp-test");
        MEMORY::LOAD_LEVEL("tlou-mp-all");
        MEMORY::LOAD_LEVEL("to1-mp-all");
        NetPlayers::InitializeSupplyBoxes();
    }, ""));

    g_menuManager.SetCurrentMenu(std::move(menu));
  }
}
} // namespace

void NetMenuTick() {

  if (noVis)
    DEBUG::DISABLE_VIS();

  if (fastCrouchSpeed)
      DC::Call<"player-set-motion-match-crouch-speed-multiplier/f">(3.0f);

  if (disableWeaponSway)
      DC::Call<"player-disable-weapon-sway">();

  // Controller combo: both presses inside a shared 10 frame window.
  bool key1 =
      DC::Call<"debug-joypad-command-active?">(SID("debug-script-press-r3"), 0);
  bool key2 = DC::Call<"debug-joypad-command-active?">(SID("debug-script-press-down"), 0);

  if (key1) {
    s_comboKey1 = true;
    s_comboTimeout = kComboWindowFrames;
  }
  if (key2) {
    s_comboKey2 = true;
    s_comboTimeout = kComboWindowFrames;
  }

  if (s_comboKey1 && s_comboKey2) {
    ToggleMenu();
    s_comboKey1 = s_comboKey2 = false;
    s_comboTimeout = 0;
  }

  if (s_comboTimeout > 0)
    s_comboTimeout--;
  else {
    s_comboKey1 = s_comboKey2 = false;
  }

  // Keyboard toggle.
  static bool wasKeyDown = false;
  bool keyDown = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
  if (keyDown && !wasKeyDown)
    ToggleMenu();
  wasKeyDown = keyDown;

  if (!s_debugMenuOpen)
    return;

  DC::Call<"joypad-disable-all-commands-except">(SID("move"), SID("camera"),
           SID("debug-cheat"), SID("menu-back"), SID("menu-select"),
           SID("menu-left"), SID("menu-up"), SID("menu-right"),
           SID("menu-down"), SID("menu-up-hold"), SID("menu-down-hold"), 0, 0);
  DC::Call<"joypad-disable-all-commands-except">(SID("debug-script-press-up"),
           SID("debug-script-press-down"), SID("debug-script-press-left"),
           SID("debug-script-press-right"), SID("debug-script-press-l1"),
           SID("debug-script-press-r1"), SID("debug-script-press-l2"),
           SID("debug-script-press-r2"), SID("debug-script-press-l3"),
           SID("debug-script-press-r3"), SID("debug-script-press-x"));

  CAMERA::SET_OVERLAY(SID("*camera-remap-medium-shot-alt*"));

  g_menuManager.HandleInput();
  g_menuManager.Render();
}

#endif
