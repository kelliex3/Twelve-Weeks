// ============================================================================
//  Score-banner widget tree:
//      canvas.hudRoot
//        ?? to1/score-banner               <- w_scoreBanner
//             ?? fader
//                  ?? teamPanel
//                       ?? allyLives        (text)  <- blue lives
//                       ?? enemyLives       (text)  <- red lives
//                       ?? matchTimer       (text)  <- match clock (MM:SS)
//
//  Death-banner widget tree (hidden until you die):
//      canvas.hudRoot
//        ?? to1/death-banner               <- w_deathBanner
//             ?? fader
//                  ?? banner
//                       ?? killerNameText    (text)
//                       ?? killerWeaponIcon  (image)
//                       ?? respawnNumber     (text)
//
// ============================================================================
#pragma once

#include "net-common.hpp"
#include "net-timer.hpp"

#include <string>

namespace net::gui2 {

    enum class ReticleMode { MovingReticle, MarkerWidget, GreyReticle, None };
    ReticleMode GetReticleMode();

    enum class RadarMode { Square, Circle };
    RadarMode GetRadarMode();

    extern StringId64 w_scoreBanner;
    extern StringId64 w_deathBanner;

    void Spawn();
    void Despawn();
    void RefreshWidgets();

    bool ScoreboardShowing();

    void BeginMatchOver(int winnerAbs = -1);

    void ResetMultiplayer();

    void LeaveMatch();

    void ResetHudLatches();

    void SetLives(int blueLives, int redLives);
    int  GetBlueLives();
    int  GetRedLives();
    void DecrementLivesForTeam(int absTeam);
    int  GetLivesForTeam(int absTeam);
    void SetLivesAbsolute(int t0, int t1);


    void RecordKill(int killerSlot, int victimSlot, bool headshot);

    void         StartMatch();
    void         ClearMatch();
    void         TickMatch(float dt);
    bool         MatchRunning();
    bool         MatchExpired();
    NetTimerSync MatchSnapshot();
    void         ApplyMatchSync(const NetTimerSync& s);

    bool         InMatch();

    bool         PauseMenuOpen();

    // ext HUD_MENU_UP() / g_hud_menu: a menu is over the HUD this frame, so every HUD
    // text draw is masked. In-match only: the pause menu anywhere on the stack, or a
    // page above the match's base page. Play menus (backpack / TAB) are deliberately
    // excluded, matching the ext. The ext knows the base-page id; the mod does not, so
    // this checks the in-match menu pages it can name.
    bool         MenuUp();

    // ext PlayerMenuActive(): the engine's menu2-player-menu-active? for the local
    // player. 1 = open, 0 = closed, -1 = the native / the player is unavailable.
    int          PlayerMenuActive();

    // ext g_tabOpen: the in-match backpack / store page is on the HUD menu stack -- a
    // menu that is part of play (TAB). Not a menu-up for the HUD, but it does freeze the
    // AFK count.
    bool         BackpackOpen();

    // ext g_gu13Up: the in-match GEAR UP page is the top HUD page. Like BackpackOpen() this
    // only names the page -- the gearup13 feature itself (list / edit views, widgets) is
    // unported. A menu that is part of play, so it freezes the AFK count.
    bool         GearUpOpen();

    void SetDeathBannerKiller(const char* killerName, const char* weaponName);
    void SetDeathBannerRespawn(int seconds);
    void SetDeathBannerVisible(bool visible);

    // The native death screen (net/ui/death-ui.*) hides this banner while it is up,
    // without clearing the wanted latch set by the respawn countdown. Kept separate
    // from SetDeathBannerVisible so the countdown can still own the latch.
    void SetDeathBannerSuppressed(bool suppressed);

    void        UpdateMenu();
    StringId64  PrimaryWeaponSid();
    StringId64  SecondaryWeaponSid();

    int  PrimaryWeaponStartingAmmo();
    int  SecondaryWeaponStartingAmmo();
    int  PurchasableWeaponStartingAmmo();

}
