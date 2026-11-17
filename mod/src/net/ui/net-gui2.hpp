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

    void SetDeathBannerKiller(const char* killerName, const char* weaponName);
    void SetDeathBannerRespawn(int seconds);
    void SetDeathBannerVisible(bool visible);

    void        UpdateMenu();
    StringId64  PrimaryWeaponSid();
    StringId64  SecondaryWeaponSid();

    int  PrimaryWeaponStartingAmmo();
    int  SecondaryWeaponStartingAmmo();
    int  PurchasableWeaponStartingAmmo();

}
