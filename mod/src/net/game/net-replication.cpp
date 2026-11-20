#include "net-replication.hpp"
#include "net-player.hpp"
#include "anim.hpp"
#include "net-engine.hpp"
#include "net-session.hpp"
#include "net-hooks.hpp"
#include "net-patches.hpp"
#include "kill-feed.hpp"
#include "net-gui2.hpp"
#include "scopeglint.hpp"
#include "notify.hpp"
#include "glass.hpp"
#include "afk.hpp"
#include "net-direct.hpp"
#include "jip-report.hpp"
#include "requeue.hpp"
#include "net/match/match-liveness.hpp"
#include "net/social/friends13.hpp"
#include "backpack.hpp"
#include "net-timer.hpp"
#include "alertsting.hpp"
#include "music-tier.hpp"

#include <cmath>
#include <chrono>
#include <random>
#include <format>
#include <algorithm>
#include <array>
#include "net-matchmaking.hpp"
#include "match.hpp"
#include "net-vox.hpp"
#include "net-debug.hpp"
#include "gameplay/perks/definitions/hawk_eyed.hpp"
#include <gameplay/perks/perk-registry.hpp>
#include <gameplay/loadout/purchase.hpp>
#include <gameplay/melee.hpp>

namespace net::replication {

    static net::match::Match& TheMatch() { return net::match::Get(); }
    static net::match::LocalPlayerState& LocalState() { return net::match::Get().local; }

    static LocalCapture g_local;
    LocalCapture& Local() { return g_local; }

    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    static void QuatSlerp(const float a[4], const float b[4], float t, float out[4])
    {
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        float bx = b[0], by = b[1], bz = b[2], bw = b[3];
        if (dot < 0.0f) { dot = -dot; bx = -bx; by = -by; bz = -bz; bw = -bw; }

        if (dot > 0.9995f) {                                // nearly parallel: lerp
            out[0] = Lerp(a[0], bx, t);
            out[1] = Lerp(a[1], by, t);
            out[2] = Lerp(a[2], bz, t);
            out[3] = Lerp(a[3], bw, t);
        }
        else {
            float theta = acosf(dot);
            float sinTheta = sinf(theta);
            float wa = sinf((1.0f - t) * theta) / sinTheta;
            float wb = sinf(t * theta) / sinTheta;
            out[0] = wa * a[0] + wb * bx;
            out[1] = wa * a[1] + wb * by;
            out[2] = wa * a[2] + wb * bz;
            out[3] = wa * a[3] + wb * bw;
        }
        float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (len > 0.0001f) {
            out[0] /= len; out[1] /= len; out[2] /= len; out[3] /= len;
        }
    }

    // runs every frame whether networked or not
    void AlwaysRun()
    {
        DC::Call<"send-event">(SID("turn-off-flashlight"), SID("player"));
        DC::Call<"set-tension-mode">(4);
        DC::Call<"player-disable-full-body-hit-reactions">(0);
        //DC::Call<"set-player-infinite-ammo">(1);
        DC::Call<"disable-pause-menu/f">();
        DC::Call<"set-difficulty">(3);   // hard difficulty

        DC::Call<"joypad-disable-commands">(SID("dodge"), SID("evade-start"), SID("flashlight-toggle"), SID("quick-turn"));

        // Debug fly while dead breaks the death/respawn flow.
        if (PLAYER::IS_IN_STATE(SID("Death")))
            DC::Call<"disable-debug-fly">();
    }

    bool CaptureLocalState(PlayerNetState& out)
    {
        if (!ENTITY::DOES_EXIST(SID("player")))
            return false;

        memset(&out, 0, offsetof(PlayerNetState, eventBuf));

        Transform t = LOCATOR::GET_TRANSFORM(SID("player"));
        out.posX = t.Position.x;  out.posY = t.Position.y;  out.posZ = t.Position.z;
        out.rotX = t.Rotation.x;  out.rotY = t.Rotation.y;
        out.rotZ = t.Rotation.z;  out.rotW = t.Rotation.w;

        out.isAiming = DC::Call<"player-is-aiming?">() ? 1 : 0;
        out.isProne = DC::Call<"player-is-prone?">() ? 1 : 0;
        out.isCrouched = DC::Call<"player-is-crouched?">() ? 1 : 0;
        out.isSprinting = DC::JOYPAD_COMMAND_ACTIVE(SID("sprint-hold")) ? 1 : 0;
        out.isJumping = PLAYER::IS_IN_STATE(SID("Jump")) ? 1 : 0;
        out.inDebugFly = PLAYER::IS_IN_STATE(SID("DebugFly")) ? 1 : 0;
        out.inBackpack = PLAYER::IS_IN_STATE(SID("Inventory")) ? 1 : 0;
        out.isFiring = DC::Call<"player-is-firing?">() ? 1 : 0;
        out.isReloading = DC::Call<"player-is-reloading?">() ? 1 : 0;
        out.inMelee = DC::Call<"character-in-melee-attack?">(SID("player")) ? 1 : 0;
        out.isMnk = DC::Call<"is-mnk-primary-input-device?">() ? 1 : 0;

        uintptr_t local = NetPlayers::LocalPtr();
        out.weaponId = net::engine::GetPlayerWeaponId(local);
        out.weaponIk = (uint8_t)(net::engine::GetWeaponIkFade(local) * 255.0f + 0.5f);
        out.aimSideLeft = net::engine::IsAimSideSwapped(local) ? 1 : 0;
        out.meleeAction = net::engine::GetMeleeAction(local);
        out.shotCount = g_local.shotCount;
        out.health = ENTITY::GET_HEALTH(SID("player"));

        // for spectating
        if (uintptr_t camLoc = CAMERA::GET_CAMERA_LOCATOR()) {
            Vector3    cp = LOCATOR::GET_LOCATOR_POSITION(camLoc);
            Quaternion cr = LOCATOR::GET_LOCATOR_ROTATION(camLoc);
            out.camPosX = cp.x; out.camPosY = cp.y; out.camPosZ = cp.z;
            out.camRotX = cr.x; out.camRotY = cr.y; out.camRotZ = cr.z; out.camRotW = cr.w;
        }

        // the local player's resolved base clip + phase + spine pose.
        NetCaptureLocalAnim(out.animSpine);

        return true;
    }

    static constexpr float kRespawnSeconds = 10.0f;
    static constexpr float kSpectateAtSeconds = 7.0f; // start spectating killer at 7 seconds
    static constexpr float kSpectateSmoothing = 10.0f;   // exponential follow rate (higher = tighter)
    static constexpr float kDamageCreditWindowSec = 6.0f; // who to credit for a kill: the last player to damage the victim within this window gets the kill credit.

    // Spectate
    static bool       s_specActive = false;   // false until the first spectate frame -> snap, then smooth
    static Vector3    s_specPos{};
    static Quaternion s_specRot{};

    static int TeamOf(const NetPlayer& p)
    {
        return net::match::Get().TeamOf(p.netSlot);
    }

    static constexpr float kMaxExtrapolateAlpha = 0.7f;
    static constexpr float kRespawnGraceSeconds = 3.0f;
    static constexpr float kAliveStaleSeconds = 3.0f;


    static constexpr uint64_t kDesyncRecoverMs = 3000;

    // Hard ceiling on holding a ragdoll back for an animated death, in case the owner stops
    // reporting the move without ever clearing it. Matches the finisher's own max hold.
    static constexpr float kAnimatedDeathHoldSec = 4.0f;

    static bool  s_deathPending[kMaxNetPlayers] = {};
    // This death is a paired melee animation that has not finished. Hold the body: the puppet
    // stays alive and replicating so the anim stream plays the move out, exactly as the victim
    // sees it, instead of collapsing the instant the kill is confirmed.
    static bool  s_deathAnimated[kMaxNetPlayers] = {};
    static float s_deathPendingAge[kMaxNetPlayers] = {};

    // Is this puppet mid-finisher rather than mid-collapse?
    //
    // The victim's own attribution sets the death weapon to the finisher move they are playing
    // (net-replication seeds firstLethalWeapon = playedMove). So a death whose weapon MATCHES
    // the move the victim is STILL reporting in their per-frame state is a paired animation in
    // progress. An ordinary death carries a gun SID, which never matches -- so someone shot
    // mid-punch still ragdolls immediately, as they should.
    //
    // No new event and no protocol change: inMelee and meleeAction are already in every state
    // packet, and inMelee is the same `character-in-melee-attack?` the victim's own half uses
    // to decide the move is over. It therefore goes false on exactly the right frame.
    static bool DeathIsAnimated(const NetPlayer& p, uint64_t weaponSID)
    {
        return p.hasState && p.State().inMelee != 0 &&
            weaponSID != 0 && p.State().meleeAction == weaponSID;
    }

    static void NoteRespawn(int netSlot)
    {
        if (netSlot < 0 || netSlot >= kMaxNetPlayers) return;
        s_deathPending[netSlot] = false;   // a stale latch must never kill a fresh puppet
        s_deathAnimated[netSlot] = false;
        s_deathPendingAge[netSlot] = 0.0f;
        TheMatch().slotState[netSlot].respawnGrace = kRespawnGraceSeconds;
    }
    static void TickAliveGrace(float dt)
    {
        for (auto& s : TheMatch().slotState)
            if (s.respawnGrace > 0.0f) s.respawnGrace = fmaxf(0.0f, s.respawnGrace - dt);
    }

    static bool IsAlive(const NetPlayer& p)
    {
        if (!p.active) return false; // empty roster slot / left the match

        bool alive;
        if (p.IsLocal()) {
            alive = !NetPlayers::LocalPtr() || !PLAYER::IS_IN_STATE(SID("Death"));
        }
        else {
            if (!p.hasState) return true;
            if (p.SecondsSinceLastPacket(net::session::Now()) > kAliveStaleSeconds)
                return true;
            const float hp = p.State().health;
            alive = !(hp <= 0.0f);
        }

        const bool slotOk = (p.netSlot >= 0 && p.netSlot < kMaxNetPlayers);
        if (alive) {
            if (slotOk) TheMatch().slotState[p.netSlot].respawnGrace = 0.0f;
            return true;
        }
        return slotOk && TheMatch().slotState[p.netSlot].respawnGrace > 0.0f;
    }

    // ===========================================================================
    //  OUT-OF-LIVES SPECTATE
    // ===========================================================================
    static constexpr float kElimDeathCamSeconds = 3.0f;   // own death cam before following anyone
    static constexpr float kSpecOrphanHold = 2.0f;   // hold the last pose when no target is left
    static constexpr float kSpecStuckWarnSecs = 30.0f;  // diagnostic only

    static constexpr float kGrantTimeoutSec = 3.0f;
    static constexpr float kGrantCooldownSec = 2.5f;

    static void EnterElimSpectate()
    {
        auto& L = LocalState();
        if (L.elimSpec) return;
        L.respawnPending = false; // eliminated -> stop re-asking for a grant
        L.elimSpec = true;
        L.elimSpecTime = 0.0f;
        L.specTargetSlot = -1;
        L.specOrphan = 0.0f;
        L.specStuckLogged = false;
        s_specActive = false; // first follow frame snaps
        L.respawnTimer.Clear(); // no respawn to count down to
        L.respawnLastSec = -1;
        net::gui2::SetDeathBannerVisible(false); // no countdown -> no "respawning in N" banner
        TO1_LOG("[SPEC] out of lives -> teammate spectate\n");
    }

    static void ExitElimSpectate()
    {
        auto& L = LocalState();
        if (!L.elimSpec) return;
        L.elimSpec = false;
        L.specTargetSlot = -1;
        L.specOrphan = 0.0f;
        s_specActive = false;
        // Camera is handed back by simply not calling SET_CAMERA_POSITION any more
        TO1_LOG("[SPEC] leaving teammate spectate\n");
    }

    // match clock broadcast
    static constexpr float kMatchSyncInterval = 1.0f;   // seconds between host broadcasts
    static float           s_matchSyncAccum = 0.0f;

    static constexpr float kPingInterval = 1.0f;
    static float           s_pingAccum = 0.0f;

    static uint32_t NowMs()
    {
        using namespace std::chrono;
        return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    static void TickPing(float dt)
    {
        s_pingAccum += dt;
        if (s_pingAccum < kPingInterval) return;
        s_pingAccum -= kPingInterval;
        uint16_t hostPing = NetPlayer::kPingUnknown;
        if (!net::session::IsHost()) {
            const int p = NetPlayers::BySlot(0).PingMs();
            if (p >= 0) hostPing = static_cast<uint16_t>(p);
        }
        const PingEvent ping{ NowMs(), hostPing, 0 };
        net::session::SendEvent(NEVT_PING, ping);
    }

    static void StartRespawnCountdown()
    {
        auto& L = LocalState();
        L.respawnTimer.Start(kRespawnSeconds);
        L.respawnLastSec = L.respawnTimer.Ceil();
        net::gui2::SetDeathBannerVisible(true);
        TO1_LOG("[NET] death detected — respawning in %.0fs\n", kRespawnSeconds);
    }

    static void ApplyRespawnGrant(int slot, uint64_t spawnerSid, int absTeam);

    static int LivesForSlot(int slot)
    {
        if (slot < 0 || slot >= kMaxNetPlayers) return -1;
        NetPlayer& pl = NetPlayers::BySlot(slot);
        const int t = TeamOf(pl);
        if (t != 0 && t != 1) return -1;
        return TheMatch().LivesForTeam(t);
    }

    // ---------------------------------------------------------------------------
    //  RESPAWN SPAWN-POINT PICKER  (host only)
    //
    // Score every spawn point by its distance to the NEAREST living enemy of the player who is
    // respawning, then pick at random from the three that score highest. Furthest-from-danger,
    // with enough randomness that it is not predictable.
    //
    // Why nearest-enemy and not, say, average: a spawn 100 m from three enemies but 4 m from a
    // fourth is a death sentence, and an average would hide that. The minimum is the only measure
    // that reflects how bad the worst case is.
    //
    // Why top THREE and not the single best: always taking the maximum makes spawns deterministic,
    // so a player who dies twice in the same place comes back in the same place twice, and the
    // enemy learns it immediately. Three keeps it safe but unguessable.
    //
    // Teammates are deliberately ignored - spawning near a teammate is good, and including them
    // would push you away from your own side.
    //
    // Runs on the host, which is the only machine holding every player: remotes as NPC puppets
    // (npcSid) and itself as SID("player"). Any locator we cannot resolve is skipped rather than
    // guessed, and if NOTHING resolves this degrades to a uniform random pick.
    // ---------------------------------------------------------------------------
    static constexpr int kSpawnCandidates = 3;   // pick at random from the best N

    // How many "to1-spawn-point-N" spawners this map has. Matchmaker provides this on connect to MP.
    static int NumSpawnPoints()
    {
        return net::match::Get().SpawnCount();
    }

    static int PickRespawnPoint(int forSlot)
    {
        static std::mt19937 rng{ std::random_device{}() };

        const int myTeam = (forSlot >= 0 && forSlot < kMaxNetPlayers)
            ? TeamOf(NetPlayers::BySlot(forSlot)) : -1;

        // Gather the enemy locators once, rather than re-resolving them per spawn point.
        uintptr_t enemyLoc[kMaxNetPlayers];
        int       enemyCount = 0;
        if (myTeam == 0 || myTeam == 1) {
            auto consider = [&](NetPlayer& p) {
                if (enemyCount >= kMaxNetPlayers) return;
                if (!p.active || p.netSlot == forSlot) return;
                const int t = TeamOf(p);
                if (t != 0 && t != 1) return;      // unknown side: cannot call them an enemy
                if (t == myTeam) return;           // teammates do not push us away
                if (!IsAlive(p)) return;           // a corpse is not a threat

                const uint64_t sid = p.IsLocal() ? SID("player") : p.npcSid;
                if (!sid) return;
                const uintptr_t loc = LOCATOR::GET_LOCATOR(sid);
                if (!loc) return;
                enemyLoc[enemyCount++] = loc;
                };
            if (NetPlayer* me = NetPlayers::Local()) consider(*me);
            for (NetPlayer& r : NetPlayers::Remotes()) consider(r);
        }

        const int numPoints = NumSpawnPoints();

        // No enemies to avoid
        if (enemyCount == 0) {
            std::uniform_int_distribution<int> any(1, numPoints);
            return any(rng);
        }

        // Score = distance to the nearest enemy. Higher is safer.
        float best[kSpawnCandidates] = { -1.0f, -1.0f, -1.0f };
        int   bestIdx[kSpawnCandidates] = { 0, 0, 0 };
        int   scored = 0;

        for (int i = 1; i <= numPoints; ++i) {
            char sname[48];
            sprintf_s(sname, "to1-spawn-point-%d", i);
            const uintptr_t sloc = LOCATOR::GET_SPAWNER_LOCATOR(SID(sname));
            if (!sloc) continue;                   // spawner missing from this map - skip it

            float nearest = 3.4e38f;
            for (int e = 0; e < enemyCount; ++e) {
                const float d = LOCATOR::GET_LOCATOR_DISTANCE(sloc, enemyLoc[e]);
                if (!(d >= 0.0f)) continue;        // NaN-safe: a bad reading must not win
                if (d < nearest) nearest = d;
            }
            if (nearest > 3.3e38f) continue;       // no usable distance for this point

            ++scored;
            // Insertion into a 3-slot descending top-list.
            for (int k = 0; k < kSpawnCandidates; ++k) {
                if (nearest <= best[k]) continue;
                for (int j = kSpawnCandidates - 1; j > k; --j) {
                    best[j] = best[j - 1]; bestIdx[j] = bestIdx[j - 1];
                }
                best[k] = nearest; bestIdx[k] = i;
                break;
            }
        }

        if (scored == 0) {                          // nothing resolved -> old behaviour
            std::uniform_int_distribution<int> any(1, numPoints);
            return any(rng);
        }

        const int pool = (scored < kSpawnCandidates) ? scored : kSpawnCandidates;
        std::uniform_int_distribution<int> pick(0, pool - 1);
        const int chosen = pick(rng);
        TO1_LOG("[SPAWN] slot%d respawn: %d enemies, top%d = {%d:%.1fm, %d:%.1fm, %d:%.1fm} -> %d\n",
            forSlot, enemyCount, pool,
            bestIdx[0], best[0], bestIdx[1], best[1], bestIdx[2], best[2], bestIdx[chosen]);
        return bestIdx[chosen];
    }

    // HOST ONLY. The single decision point for "may this player come back?". Checks that side's
    // pool, picks the spawn point so every machine agrees where they reappear, and broadcasts the
    // grant. Applies it locally too, since we never receive our own broadcast.
    static void HostGrantRespawn(int slot)
    {
        if (!net::session::IsHost()) return;
        if (slot < 0 || slot >= kMaxNetPlayers) return;

        if (TheMatch().slotState[slot].grantCooldown > 0.0f) {
            TO1_LOG("[NET] respawn re-request from slot%d ignored (grant already in flight)\n", slot);
            return;
        }

        // Not dead in our view -> we already granted this death (the flag clears on grant), or we
        // never saw the death at all. Either way a life must NOT be spent. Re-broadcast the grant
        // WITHOUT charge so a requester whose copy genuinely expired still gets unstuck - the
        // receivers' own dead-gate makes the duplicate free for everyone who already applied it.
        if (!TheMatch().slotState[slot].dead) {
            char rname[48];
            sprintf_s(rname, "to1-spawn-point-%d", PickRespawnPoint(slot));
            const int  rt = TheMatch().TeamOf(slot);
            RespawnGrantEvent rg{ SID(rname), static_cast<uint8_t>(slot),
                                  static_cast<uint8_t>((rt == 1) ? 1 : 0) };
            TheMatch().slotState[slot].grantCooldown = kGrantCooldownSec;
            net::session::SendEvent(NEVT_RESPAWN_GRANT, rg);
            TO1_LOG("[NET] respawn RE-ISSUED (no charge) for slot%d - not marked dead here\n", slot);
            ApplyRespawnGrant(slot, rg.spawnerSid, rt);
            return;
        }

        const int lives = LivesForSlot(slot);
        if (lives < 0) {
            TO1_LOG("[NET] respawn for slot%d deferred: team unresolved\n", slot);
            return;
        }
        if (lives <= 0) {
            TO1_LOG("[NET] respawn DENIED for slot%d (lives=%d)\n", slot, lives);
            return;
        }

        char spawnerName[48];
        sprintf_s(spawnerName, "to1-spawn-point-%d", PickRespawnPoint(slot));

        const int  gt = TheMatch().TeamOf(slot);
        RespawnGrantEvent g{ SID(spawnerName), static_cast<uint8_t>(slot),
                             static_cast<uint8_t>((gt == 1) ? 1 : 0) };
        TheMatch().slotState[slot].grantCooldown = kGrantCooldownSec;
        net::session::SendEvent(NEVT_RESPAWN_GRANT, g);
        TO1_LOG("[NET] respawn GRANTED slot%d at %s (lives were %d)\n", slot, spawnerName, lives);
        ApplyRespawnGrant(slot, g.spawnerSid, g.team); // the host never receives its own broadcast
    }

    // Request host respawn. The host will check the pool and, if a life is available, broadcast a grant to all
    void RequestRespawn()
    {
        const int mySlot = net::session::MySlot();
        if (mySlot < 0) return;
        LocalState().respawnPending = true;
        LocalState().respawnPendingSec = 0.0f;
        if (net::session::IsHost()) { HostGrantRespawn(mySlot); return; }
        net::session::SendEvent(NEVT_RESPAWN_REQUEST, 0);   // the host is always slot 0
        TO1_LOG("[NET] respawn requested from host\n");
    }

    static void DoRespawn(uint64_t spawnerSid)
    {
        LocalState().firstLethalSlot = -1;
        LocalState().lastDamagerSlot = -1;
        LocalState().lastDamageAt = -1000.0f;

        uint64_t loc = LOCATOR::GET_SPAWNER_LOCATOR(spawnerSid);
        Vector3 pos = LOCATOR::GET_LOCATOR_POSITION(loc);
        Quaternion rot = LOCATOR::GET_LOCATOR_ROTATION(loc);

        NetClearRemoteAnimAll(/*dropLastGood=*/false);
        for (NetPlayer& rp : NetPlayers::Remotes()) {
            if (!rp.spawned || !rp.hasState) continue;
            // A corpse stays a corpse: our own respawn is not their respawn.
            if (rp.dead) continue;
            const PlayerNetState& rs = rp.State();
            if (!AllFinite(&rs.posX, 3) || !IsUnitQuat(&rs.rotX)) continue;

            rp.Reanchor(Vector3{ rs.posX, rs.posY, rs.posZ },
                Quaternion{ rs.rotX, rs.rotY, rs.rotZ, rs.rotW });
        }

        NetPlayers::RespawnLocal(pos, rot);

        // Stop spectating: we're alive again. We deliberately do NOT restore the camera
        // ourselves -- simply ceasing to call SET_CAMERA_POSITION hands it back to the engine,
        // which returns it to the normal player cam on its own.
        LocalState().killerSlot = -1;
        s_specActive = false;
        ExitElimSpectate();
        NoteRespawn(net::session::MySlot());
        net::gui2::SetDeathBannerVisible(false);

        gameplay::perks::NotifySpawn(true);
    }

    static void ApplyRespawnGrant(int slot, uint64_t spawnerSid, int absTeam)
    {
        if (slot < 0 || slot >= kMaxNetPlayers) return;

        if (TheMatch().slotState[slot].dead) {
            TheMatch().slotState[slot].dead = false;
            TheMatch().DecrementLivesForTeam(absTeam);
        }
        else {
            TO1_LOG("[NET] grant for slot%d without a recorded death - respawning WITHOUT charge\n", slot);
        }

        if (slot == net::session::MySlot()) {
            auto& L = LocalState();
            if (L.respawnPending || L.respawnTimer.Running()) {
                L.respawnTimer.Clear();
                L.respawnLastSec = -1;
                L.respawnPending = false; // granted: stop the re-ask timer
                DoRespawn(spawnerSid);
            }
            return;
        }

        NetPlayer& sender = NetPlayers::BySlot(slot);
        uint64_t   loc = LOCATOR::GET_SPAWNER_LOCATOR(spawnerSid);
        Vector3    pos = LOCATOR::GET_LOCATOR_POSITION(loc);
        Quaternion rot = LOCATOR::GET_LOCATOR_ROTATION(loc);
        TO1_LOG("[NET] %s respawning\n", sender.name);
        NoteRespawn(slot);
        NetClearRemoteAnim(slot);
        sender.Respawn(pos, rot);
    }

    void PlaceLocalAtSpawn(int spawnIndex)
    {
        const net::matchmaking::MatchInfo* mi = net::matchmaking::GetMatch();
        const int myTeam = mi ? static_cast<int>(mi->yourTeam) : -1;
        const int mySlot = mi ? static_cast<int>(mi->yourSlot) : -1;

        int teamIndex = -1;
        if (mi && (myTeam == 0 || myTeam == 1) && mySlot >= 0) {
            int n = 0;
            for (uint8_t i = 0; i < mi->playerCount && i < MM_MAX_SLOTS; ++i) {
                if (static_cast<int>(mi->roster[i].team) != myTeam) continue;
                if (static_cast<int>(mi->roster[i].slot) < mySlot) ++n;
            }
            teamIndex = n + 1; // 1 based
        }

        char name[64];
        if (teamIndex > 0) {
            sprintf_s(name, "to1-t%d-intro-spawn-point-%d", myTeam, teamIndex);
        }
        else if (spawnIndex > 0) {
            sprintf_s(name, "to1-spawn-point-%d", spawnIndex);
            TO1_LOG("[SPAWN] team spawn unresolved (team=%d slot=%d) — falling back to %s\n",
                myTeam, mySlot, name);
        }
        else {
            TO1_LOG("[SPAWN] no spawn available (team=%d slot=%d) — leaving player in place\n",
                myTeam, mySlot);
            return;
        }

        uint64_t   loc = LOCATOR::GET_SPAWNER_LOCATOR(SID(name));
        Vector3    pos = LOCATOR::GET_LOCATOR_POSITION(loc);
        Quaternion rot = LOCATOR::GET_LOCATOR_ROTATION(loc);

        NetPlayers::RespawnLocal(pos, rot);

        TO1_LOG("[SPAWN] placed local player at %s (team %d, #%d on that team)\n",
            name, myTeam, teamIndex);

        gameplay::perks::NotifySpawn(false);
    }

    static void UpdateRespawnPending(float dt)
    {
        for (auto& s : TheMatch().slotState)
            if (s.grantCooldown > 0.0f) s.grantCooldown -= dt;

        auto& L = LocalState();
        if (!L.respawnPending) return;
        L.respawnPendingSec += dt;
        if (L.respawnPendingSec < kGrantTimeoutSec) return;

        L.respawnPendingSec = 0.0f;
        if (TheMatch().BlueLives() > 0) {
            TO1_LOG("[NET] no respawn grant in %.0fs — re-asking\n", kGrantTimeoutSec);
            RequestRespawn();
        }
        else {
            TO1_LOG("[NET] respawn denied (side out of lives) — entering elimination spectate\n");
            L.respawnPending = false;
            EnterElimSpectate();
        }
    }

    static void UpdateRespawnCountdown(float dt)
    {
        auto& L = LocalState();
        if (!L.respawnTimer.Running()) return;

        bool expired = L.respawnTimer.Tick(dt);

        int sec = L.respawnTimer.Ceil();
        net::gui2::SetDeathBannerRespawn(sec);
        if (sec != L.respawnLastSec && sec > 0) {
            //AUDIO::PLAY_SOUND(SID("sfx-ui-acc-sonar-npc-infected-menu"));
            //AUDIO::PLAY_SOUND(SID("sfx-rl-round-timer-tick-negative"));
            AUDIO::PLAY_SOUND(SID("sfx-rl-round-timer-tick-positive"));
            //AUDIO::PLAY_SOUND(SID("sfx-rl-round-timer-tick"));
            //AUDIO::PLAY_SOUND(SID("sfx-rl-round-timer-end"));
            L.respawnLastSec = sec;
        }
        if (expired && !net::gui2::ScoreboardShowing()) {   // no respawn once the end-of-match scoreboard is up
            if (TheMatch().BlueLives() > 0) {
                AUDIO::PLAY_SOUND(SID("sfx-rl-round-timer-tick-small"));
                RequestRespawn();
                L.respawnTimer.Stop();
            }
            else {
                // Our team spent its last life while we were waiting: this countdown is VOID.
                EnterElimSpectate();
            }
        }
    }

    static Quaternion NLerpQuat(const Quaternion& a, const Quaternion& b, float t)
    {
        // Take the shortest arc: q and -q are the same rotation, so flip b if it's on the
        // far side, otherwise the camera can spin the long way round on a big view change.
        const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        const float s = (d < 0.0f) ? -1.0f : 1.0f;
        const float x = a.x + (b.x * s - a.x) * t;
        const float y = a.y + (b.y * s - a.y) * t;
        const float z = a.z + (b.z * s - a.z) * t;
        const float w = a.w + (b.w * s - a.w) * t;
        const float len = sqrtf(x * x + y * y + z * z + w * w);
        if (len < 1e-6f) return b;                       // degenerate -> just take the target
        const float inv = 1.0f / len;
        return Quaternion{ x * inv, y * inv, z * inv, w * inv };
    }

    static bool SpectateTargetValid(int slot)
    {
        if (slot < 0 || slot >= kMaxNetPlayers) return false;
        NetPlayer& p = NetPlayers::BySlot(slot);
        if (!p.active || p.IsLocal()) return false;
        const int myTeam = net::matchmaking::MyTeam();
        if (myTeam < 0 || TeamOf(p) != myTeam) return false;
        if (!IsAlive(p)) return false;
        const PlayerNetState& s = p.State();
        return AllFinite(&s.camPosX, 3) && IsUnitQuat(&s.camRotX);
    }

    // Random alive teammate, excluding the one we just lost so we don't re-pick a corpse on the
    // same frame. -1 = nobody left, which (at 0 lives) is exactly when the match should be ending.
    static int PickSpectateTarget(int exclude)
    {
        int cand[kMaxNetPlayers];
        int n = 0;
        for (NetPlayer& p : NetPlayers::Remotes()) {
            if (p.netSlot == exclude) continue;
            if (SpectateTargetValid(p.netSlot)) cand[n++] = p.netSlot;
        }
        if (n == 0) return -1;
        static std::mt19937 rng{ std::random_device{}() };
        return cand[std::uniform_int_distribution<int>(0, n - 1)(rng)];
    }

    // Drive our camera from one slot's replicated camera pose for a single frame. The first frame
    // SNAPS -- smoothing onto a target across the map would visibly fly the camera through the
    // level. After that it follows exponentially, which also smooths their ~30-60 Hz packet
    // arrivals into per-frame motion. Returns false when that slot has nothing usable this frame;
    // the caller decides whether to hold the last pose or release the camera.
    static bool DriveCameraFromSlot(int slot, float dt)
    {
        if (slot < 0 || slot >= kMaxNetPlayers) return false;
        NetPlayer& p = NetPlayers::BySlot(slot);
        if (!p.hasState) return false;
        const PlayerNetState& s = p.State();
        if (!AllFinite(&s.camPosX, 3) || !IsUnitQuat(&s.camRotX)) return false;

        const Vector3    tgtPos{ s.camPosX, s.camPosY, s.camPosZ };
        const Quaternion tgtRot{ s.camRotX, s.camRotY, s.camRotZ, s.camRotW };

        if (!s_specActive) {
            s_specPos = tgtPos; s_specRot = tgtRot; s_specActive = true;
        }
        else {
            const float a = 1.0f - expf(-dt * kSpectateSmoothing);
            s_specPos.x += (tgtPos.x - s_specPos.x) * a;
            s_specPos.y += (tgtPos.y - s_specPos.y) * a;
            s_specPos.z += (tgtPos.z - s_specPos.z) * a;
            s_specRot = NLerpQuat(s_specRot, tgtRot, a);
        }
        CAMERA::SET_CAMERA_POSITION(s_specPos, s_specRot);
        return true;
    }

    static void UpdateSpectateCamera(float dt)
    {
        auto& L = LocalState();

        if (net::gui2::ScoreboardShowing()) {
            ExitElimSpectate();
            s_specActive = false;
            return;
        }

        // out of lives, follow teammates until the match ends
        if (L.elimSpec) {
            L.elimSpecTime += dt;

            if (!L.specStuckLogged && L.elimSpecTime > kSpecStuckWarnSecs) {
                L.specStuckLogged = true;
                TO1_LOG("[SPEC] %.0fs of out-of-lives spectate with no MATCH_OVER — "
                    "the host's elimination test never fired\n", L.elimSpecTime);
            }
            if (L.elimSpecTime < kElimDeathCamSeconds)
                return;

            if (!SpectateTargetValid(L.specTargetSlot)) {   // none yet, or they died / dropped
                const int next = PickSpectateTarget(L.specTargetSlot);
                if (next >= 0) {
                    L.specTargetSlot = next;
                    s_specActive = false;   // new target -> snap
                    L.specOrphan = 0.0f;
                    TO1_LOG("[SPEC] following slot %d (%s)\n", next, NetPlayers::BySlot(next).name);
                }
                else {
                    L.specTargetSlot = -1;
                    L.specOrphan += dt;
                    if (s_specActive && L.specOrphan < kSpecOrphanHold)
                        CAMERA::SET_CAMERA_POSITION(s_specPos, s_specRot);
                    else
                        s_specActive = false;
                    return;
                }
            }

            if (!DriveCameraFromSlot(L.specTargetSlot, dt) && s_specActive)
                CAMERA::SET_CAMERA_POSITION(s_specPos, s_specRot);
            return;
        }

        // lives remain -> own death cam for 3s, then spectate killer
        if (!L.respawnTimer.Running()) return;                            // alive -> engine owns the camera
        if (L.respawnTimer.Remaining() > kSpectateAtSeconds) return;      // still on our own death cam
        if (L.killerSlot < 0 || L.killerSlot >= kMaxNetPlayers) return;   // environmental / unknown killer
        if (!DriveCameraFromSlot(L.killerSlot, dt) && s_specActive)
            CAMERA::SET_CAMERA_POSITION(s_specPos, s_specRot);            // packet gap -> hold
    }

    static void AutoSpawnRemote(NetPlayer& p)
    {
        const PlayerNetState& st = p.State();

        Vector3 pos{ st.posX, st.posY, st.posZ };
        Quaternion rot{ 0, 0, 0, 1 };

        if (fabsf(pos.x) < 0.001f && fabsf(pos.y) < 0.001f && fabsf(pos.z) < 0.001f)
            pos = LOCATOR::GET_POSITION_IN_FRONT_OF(SID("player"), 5.0f);

        TO1_LOG("[NET] auto-spawning \"%s\" (slot %d) at %.1f %.1f %.1f\n",
            p.name, p.netSlot, pos.x, pos.y, pos.z);
        p.Spawn(pos, rot);

        if (!p.announcedSpawn) {
            const std::string msg = std::format("{} connected!", p.name);
            HUD::SHOW_NOTIFICATION(msg.c_str(), 5.0f);
            p.announcedSpawn = true;
        }
    }

    static void ApplyRemote(NetPlayer& p, float dt)
    {
        uintptr_t ptr = p.Ptr();
        if (!ptr || !p.hasState) return;

        if (!ENTITY::DOES_EXIST(p.npcSid)) { NetClearRemoteAnim(p.netSlot); return; }

        if (p.dead) return;
        if (p.EngineOwned()) return;

        const PlayerNetState& st = p.State();
        const PlayerNetState& prev = p.Prev();

        // While an execution involves this puppet, the LOCAL synced melee action owns its
        // body (locator + animation) -- replication writes would fight it every frame.
        if (gameplay::melee::PuppetFrozenForExecution(p.netSlot)) {
            p.ApplyTeamFaction();
            if (st.health > 0.0f && st.health < 1.0e6f)
                p.ledger.OnStateReport(st.health, NowSec());
            return;
        }

        // position / rotation interpolation
        p.interpTimer += dt;
        float a = p.InterpAlpha();

        auto posSane = [](const float* v) {
            return AllFinite(v, 3) &&
                v[0] > -1.0e6f && v[0] < 1.0e6f &&
                v[1] > -1.0e6f && v[1] < 1.0e6f &&
                v[2] > -1.0e6f && v[2] < 1.0e6f;
            };
        if (posSane(&st.posX) && IsUnitQuat(&st.rotX) &&
            posSane(&prev.posX) && IsUnitQuat(&prev.rotX)) {
            const float ax = fminf(a, kMaxExtrapolateAlpha);
            const float pos[3] = {
                Lerp(st.posX, st.posX + (st.posX - prev.posX), ax),
                Lerp(st.posY, st.posY + (st.posY - prev.posY), ax),
                Lerp(st.posZ, st.posZ + (st.posZ - prev.posZ), ax),
            };
            const float q[4] = { st.rotX, st.rotY, st.rotZ, st.rotW };

            LOCATOR::SET_OBJECT_LOCATOR(p.npcSid,
                MEMORY::ALLOC_BOUND_FRAME(Vector3{ pos[0], pos[1], pos[2] },
                    Quaternion{ q[0], q[1], q[2], q[3] }));
        }

        NetApplyRemoteAnim(p.netSlot, p.npcSid, st.animSpine);
        NetSetPuppetWeaponIk(p.netSlot, st.weaponIk * (1.0f / 255.0f));
        p.ApplyTeamFaction();

        if (st.health > 0.0f && st.health < 1.0e6f && st.health != p.lastSyncedHealth) {
            ENTITY::SET_HEALTH(p.npcSid, st.health);
            p.lastSyncedHealth = st.health;
        }

        if (st.health > 0.0f && st.health < 1.0e6f)
            p.ledger.OnStateReport(st.health, NowSec());

        if (st.weaponId != 0 && st.weaponId != p.puppetWeapon) {
            DC::Call<"npc-destroy-weapon">(p.npcSid);
            DC::Call<"npc-give-weapon">(p.npcSid, st.weaponId, 999, 0x1);
            p.puppetWeapon = st.weaponId;
        }

        if (st.weaponId != 0)
            net::engine::AttachWeaponToCorrectHand(p.Ptr(), st.weaponId, st.aimSideLeft != 0);
    }

    static void UpdateTeamHighlight()
    {
        const int myTeam = net::matchmaking::MyTeam();
        if (myTeam < 0) return;
        for (NetPlayer& p : NetPlayers::Remotes()) {
            if (!p.Exists() || !p.npcSid || p.EngineOwned()) continue;
            const int t = TeamOf(p);
            if (t < 0 || t != myTeam) continue;

            NPC::HIGHLIGHT(p.npcSid, kTeamBlueR / 255.0f, kTeamBlueG / 255.0f, kTeamBlueB / 255.0f, 1.0f); // applied every frame
        }
    }

    // Limited LISTEN MODE Durability
    static constexpr float kListenModeDuration = 5.0f;
    static constexpr float kListenModeSpendingSpeed = 1.0f / kListenModeDuration;

    static constexpr float kListenModeRecoveryDuration = 25.0f;
    static constexpr float kListenModeRecoverySpeed = 1.0f / kListenModeRecoveryDuration;

    static constexpr float kListenModeReactivationThreshold = 0.5f;

    // SHARP EARS scales the numbers above: 30% longer sessions, 35% faster recovery.
    static constexpr float kSharpDurationScale = 1.30f;
    static constexpr float kSharpRecoveryScale = 1.35f;
    static constexpr float kSharpReactThreshold = 0.25f;
    static constexpr float kSharpMoveScale = 3.00f; // 3x faster

    static float kListenModeDurability = 1.0f;
    static bool s_wasPressingListen = false; //Button state from prev frame
    static bool s_listenSessionActive = false;//Current hold successfully activated Listen Mode

    static void UpdateListenMode()
    {
        if (!net::gui2::InMatch() || net::gui2::ScoreboardShowing() ||
            net::gui2::PauseMenuOpen() || net::matchmaking::MyTeam() < 0) {
            return;
        }

        float dt = TIME::GET_DELTA_TIME();
        if (dt <= 0.0f) return; // paused
        if (dt > 0.1f)  dt = 0.1f;

        const bool sharp = gameplay::perks::IsActive(gameplay::PerkId::SHARPEARS);
        const float spendSpeed = kListenModeSpendingSpeed / (sharp ? kSharpDurationScale : 1.0f);
        const float recoverSpeed = kListenModeRecoverySpeed * (sharp ? kSharpRecoveryScale : 1.0f);
        const float reactAt = sharp ? kSharpReactThreshold : kListenModeReactivationThreshold;

        const bool holdingListen = DC::JOYPAD_COMMAND_ACTIVE(SID("listen-hold"));
        const bool aiming = DC::Call<"player-is-aiming?">();

        const bool newPress = holdingListen && !s_wasPressingListen;

        if (newPress && !aiming && kListenModeDurability > reactAt)
        {
            s_listenSessionActive = true;
        }

        if (!holdingListen)
        {
            s_listenSessionActive = false;
        }

        if (s_listenSessionActive && holdingListen && !aiming)
        {
            // Spend while held
            kListenModeDurability -= spendSpeed * dt;

            if (kListenModeDurability <= 0.0f)
            {
                kListenModeDurability = 0.0f;

                // Treat reaching zero as an artificial key release.
                // The player must physically release and press again,
                // and durability must be > the reactivation threshold
                // before another session can start
                s_listenSessionActive = false;
            }
            else
            {
                //DC::Call<"joypad-force-command">(SID("listen-hold"));
                DC::Call<"joypad-force-commands">(SID("listen"), SID("listen-toggle"), SID("listen-hold"));

                if (sharp)
                    DC::Call<"player-set-motion-match-crouch-speed-multiplier/f">(kSharpMoveScale);
            }
        }
        else
        {
            // Released, aiming, or spent: disable Listen Mode and recover durability

            DC::Call<"joypad-disable-commands">(SID("listen"), SID("listen-toggle"), SID("listen-hold"));

            kListenModeDurability += recoverSpeed * dt;

            if (kListenModeDurability > 1.0f) kListenModeDurability = 1.0f;
        }

        DC::Call<"swim-set-breath-meter">(kListenModeDurability);

        s_wasPressingListen = holdingListen;
    }

    static void ApplyConfirmedDeath(int victimSlot, int killerSlot, uint64_t weaponSID, bool headshot)
    {
        if (victimSlot < 0 || victimSlot >= kMaxNetPlayers) return;
        const int mySlot = net::session::MySlot();

        const bool haveKiller = (killerSlot >= 0 && killerSlot < kMaxNetPlayers &&
            killerSlot != victimSlot);

        const char* victimName = (victimSlot == mySlot) ? net::session::LocalName()
            : NetPlayers::BySlot(victimSlot).name;
        const char* killerName = !haveKiller ? nullptr
            : (killerSlot == mySlot ? net::session::LocalName()
                : NetPlayers::BySlot(killerSlot).name);

        const char* weaponName = NdGameSdk::Sidbase::StringIdToStringInternal(weaponSID);

        //SPAWN AMMO ON KILL - only for the clien who just killed an enemy, and if it wasn't a suicide
        if (haveKiller && killerSlot == mySlot && victimSlot != mySlot) {
            SpawnItemFromKill(killerSlot, victimSlot, weaponSID);
        }

        if (haveKiller && killerSlot == mySlot && victimSlot != mySlot && !NetPlayers::SlotIsFriendly(victimSlot))
            net::vox::PlayForLocal(net::vox::Scenario::EnemyKill);

        if (haveKiller) {
            AddKillFeedEntry(killerName, victimName,
                (killerSlot == mySlot) || NetPlayers::SlotIsFriendly(killerSlot),
                (victimSlot == mySlot) || NetPlayers::SlotIsFriendly(victimSlot));
        }
        else {
            AddDeathFeedEntry(victimName,
                (victimSlot == mySlot) || NetPlayers::SlotIsFriendly(victimSlot));
        }

        if (victimSlot >= 0 && victimSlot < kMaxNetPlayers) TheMatch().slotState[victimSlot].dead = true;

        if (victimSlot != mySlot) {
            NetPlayer& victim = NetPlayers::BySlot(victimSlot);
            // Two reasons to hold the body. The first is ours -- we are a participant and our
            // own move owns it. The second is anybody's: the victim is playing out a finisher,
            // which a THIRD party has no other way to know. Without it a bystander sees the
            // victim ragdoll a fraction of a second into a two-second animation, because the
            // kill confirms long before the move ends and a dead puppet gets no anim replay.
            const bool animated = DeathIsAnimated(victim, weaponSID);
            if (gameplay::melee::PuppetFrozenForExecution(victimSlot) || animated) {
                s_deathPending[victimSlot] = true;
                s_deathAnimated[victimSlot] = animated;
                s_deathPendingAge[victimSlot] = 0.0f;
                TO1_LOG("[MELEE] slot%d death confirmed mid-%s -- holding the body\n",
                    victimSlot, animated ? "animation" : "execution");
            }
            else {
                victim.ReclaimFromEngine();
                if (!victim.dead) victim.Die();
            }
        }

        net::gui2::RecordKill(haveKiller ? killerSlot : -1, victimSlot, headshot);

        if (victimSlot == mySlot) {
            net::gui2::SetDeathBannerKiller(haveKiller ? killerName : victimName, weaponName);
            if (haveKiller && killerSlot != mySlot) {
                LocalState().killerSlot = killerSlot;
                s_specActive = false;
            }
        }

        TO1_LOG("[NET] %s killed %s (%s%s)\n",
            haveKiller ? killerName : "(world)", victimName,
            weaponName, headshot ? ", headshot" : "");
    }

    float NowSec() { return TheMatch().NowSec(); }

    // SPAWNABLES

    int CalculateAmmoDrop(
        int currentAmmo,
        AmmoDropProfile ammoDropProfile,
        MatchState state)
    {
        int stateOffset = 0;

        switch (state)
        {
        case MatchState::Winning:
            stateOffset = -ammoDropProfile.stateStep;
            break;

        case MatchState::Losing:
            stateOffset = +ammoDropProfile.stateStep;
            break;

        case MatchState::Contested:
            stateOffset = 0;
            break;
        }

        int target = ammoDropProfile.baseTarget + stateOffset + (currentAmmo % ammoDropProfile.cycleLength);

        return (std::max)(0, target - currentAmmo);
    }


    MatchState CalculateRubberbandingTeamState(int localSlot, int enemySlot) {

        const int localTeamScore = TheMatch().LivesForTeam(net::matchmaking::SlotTeam(localSlot));
        const int enemyTeamScore = TheMatch().LivesForTeam(net::matchmaking::SlotTeam(enemySlot));

        // Rubberbanding ammo drops to promote comebacks
        //  If killer team is 4 or more lives ahead of the victim's team it means they're winning, and they need less resources
        //  If killer team is 3 or more lives behind of the victim's team it means they're losing, and they need more ammo 
        //  If none of the above happens, just go for the regular drops per ammo type as it's contested and balanced 

        if (localTeamScore >= enemyTeamScore + 4)
        {
            return MatchState::Winning;
            TO1_LOG("[NetPlayer] Match State is: Winning (local team is at least 4 lives ahead of the enemy team). Nerfing drops.\n");
        }
        else if (localTeamScore + 3 <= enemyTeamScore)
        {
            return MatchState::Losing;
            TO1_LOG("[NetPlayer] Match State is: Losing (local team is at least 3 lives behind the enemy team). Buffing drops.\n");
        }
        else
        {
            return MatchState::Contested;
            TO1_LOG("[NetPlayer] Match State is: Contested (no team is >= 4 lives over the other).\n");
        }

    }


    AmmoDropProfile GetAmmoProfileFromAmmoSID(StringId64 ammoSID) {
        switch (ammoSID) {
        case SID("ammo-arrow"):
            return {
                .baseTarget = 5,
                .cycleLength = 3,
                .stateStep = 3,
            };
        case SID("ammo-crossbow"):
            return {
                .baseTarget = 4,
                .cycleLength = 2,
                .stateStep = 2,
            };
        case SID("ammo-flamethrower"):
            return {
                .baseTarget = 100,
                .cycleLength = 2,
                .stateStep = 20,
            };
        case SID("ammo-pistol"):
            return {
                .baseTarget = 8,
                .cycleLength = 3,
                .stateStep = 3,
            };
        case SID("ammo-revolver"):
            return {
                .baseTarget = 8,
                .cycleLength = 3,
                .stateStep = 3,
            };
        case SID("ammo-rifle"):
            return {
                .baseTarget = 6,
                .cycleLength = 2,
                .stateStep = 2,
            };
        case SID("ammo-rifle-mpx5"):
            return {
                .baseTarget = 6,
                .cycleLength = 2,
                .stateStep = 2,
            };
        case SID("ammo-rifle-tommy"):
            return {
                .baseTarget = 6,
                .cycleLength = 2,
                .stateStep = 2,
            };
        case SID("ammo-rifle-vepr"):
            return {
                .baseTarget = 7,
                .cycleLength = 2,
                .stateStep = 2,
            };
        case SID("ammo-shotgun"):
            return {
                .baseTarget = 4,
                .cycleLength = 2,
                .stateStep = 2,
            };
        case SID("ammo-shotgun-baserri"):
            return {
                .baseTarget = 4,
                .cycleLength = 2,
                .stateStep = 2,
            };
        case SID("ammo-xcaliber"):
            return {
                .baseTarget = 4,
                .cycleLength = 2,
                .stateStep = 2,
            };
        default:
            return {
                .baseTarget = 4,
                .cycleLength = 2,
                .stateStep = 2,
            };
        }
    }


    void AwardParts(int partsAmount, int slot, std::string reason)
    {
        if (slot < 0 || slot >= kMaxNetPlayers || partsAmount == 0) return;

        int& balance = TheMatch().stats[slot].parts;
        balance += partsAmount;

        TO1_LOG("[parts] slot%d %+d for %s -> %d\n",
            slot, partsAmount, reason.c_str(), balance);

        if (slot == net::session::MySlot())
            gameplay::perks::NotifyPartsAwarded(partsAmount);
    }

    void SpawnItemFromKill(int killerSlot, int victimSlot, StringId64 weaponSID)
    {
        StringId64 victimEntiySID = NetPlayers::BySlot(victimSlot).npcSid;

        /*
        uint64_t victimLoc = LOCATOR::GET_JOINT_LOCATOR(victimEntiySID, SID("spinea"));

        if (!victimLoc) {
            victimLoc = LOCATOR::GET_LOCATOR(victimEntiySID); // Couldn't find a valid joint for the victim, defaulting to entity root
        }*/

        uint64_t victimLoc = LOCATOR::GET_LOCATOR(victimEntiySID); // Couldn't find a valid joint for the victim, defaulting to entity root

        Vector3 victimPos = LOCATOR::GET_LOCATOR_POSITION(victimLoc);
        Quaternion victimRot = LOCATOR::GET_LOCATOR_ROTATION(victimLoc);

        //get ammo type from weapon used to execute the kill. to be replaced with a needs system

        StringId64 ammoType = DC::Call<"weapon-get-default-ammo-id">(weaponSID);

        const char* ammoTypeName = NdGameSdk::Sidbase::StringIdToStringInternal(ammoType);

        TO1_LOG("[NetPlayer] Ammo type for %s is %s\n", NdGameSdk::Sidbase::StringIdToStringInternal(weaponSID), ammoTypeName);

        if (!ammoType)
        {
            TO1_LOG("[NetPlayer][ERROR] Invalid ammo type %s\n", ammoTypeName);
            return;
        }

        int weaponTotalAmmo = DC::Call<"get-total-ammo">(weaponSID);

        MatchState rubberbandingState = CalculateRubberbandingTeamState(killerSlot, victimSlot);

        int ammoQuantityToDrop = CalculateAmmoDrop(weaponTotalAmmo, GetAmmoProfileFromAmmoSID(ammoType), rubberbandingState);

        if (ammoQuantityToDrop == 0)
        {
            TO1_LOG("[NetPlayer] Ammo quantity to spawn is 0, aborting spawning pickup\n");
            return;
        }

        TO1_LOG("[NetPlayer][SUCCESS] Spawning %d rounds of %s...\n", ammoQuantityToDrop, ammoTypeName);

        StringId64 spawnedItem = DC::Call_V2<"spawn-item-pickup">(ammoType, victimLoc, ammoQuantityToDrop, 1, 0);

        if (!spawnedItem)
        {
            TO1_LOG("[NetPlayer][ERROR] Failed to spawn a pickup for '%s'\n", ammoTypeName);
        }
        else
        {
            TO1_LOG("[NetPlayer][SUCCESS] Spawned %d of '%s' at: %f, %f, %f\n", ammoQuantityToDrop, ammoTypeName, victimPos.x, victimPos.y, victimPos.z);
            //ENTITY::SET_COLLISION(spawnedItem,true); //simulate gravity
        }

    }

    // We died. Tell everyone, and apply it here too (we never receive our own broadcast).
    static void ReportOwnDeath()
    {
        const int mySlot = net::session::MySlot();
        if (mySlot < 0) return;

        // Who killed us, in priority order:
        //  1. The FIRST person to send a lethal claim. They committed to the kill, forced it through
        //     with kForceKillDamage, and have already drawn their red reticle -- so this is not a
        //     judgement call, it is the shot that actually ended us. With several people firing at
        //     once this is what makes the answer deterministic: earliest claim to arrive, full stop.
        //  2. Otherwise the last person to damage us, if recent enough to plausibly be the cause --
        //     covers deaths nobody claimed (bleed-out, a fall right after a fight).
        //  3. Otherwise nobody: an environmental death, which still shows in the feed.
        int      killerSlot = -1;
        uint64_t weaponSID = 0;
        bool     headshot = false;

        auto& L = LocalState();
        if (L.firstLethalSlot >= 0) {
            killerSlot = L.firstLethalSlot;
            weaponSID = L.firstLethalWeapon;
            headshot = L.firstLethalHead;
        }
        else if (L.lastDamagerSlot >= 0 &&
            (NowSec() - L.lastDamageAt) <= kDamageCreditWindowSec) {
            killerSlot = L.lastDamagerSlot;
            weaponSID = L.lastDamageWeapon;
            headshot = L.lastDamageHead;
        }

        PlayerDeathEvent death{};
        death.weaponSID = weaponSID;
        death.headshotKill = headshot ? 1 : 0;   // the SHOOTER saw the hit zone and sent it to us
        death.victimSlot = static_cast<uint8_t>(mySlot);
        death.killerSlot = (killerSlot >= 0) ? static_cast<uint8_t>(killerSlot) : kInvalidSlot;
        net::session::SendEvent(NEVT_PLAYER_DEATH, death);

        ApplyConfirmedDeath(mySlot, killerSlot, weaponSID, headshot);

        gameplay::DeathContext dctx;
        dctx.killerSlot = killerSlot;
        dctx.weaponSid = weaponSID;
        dctx.headshot = headshot;
        gameplay::perks::NotifyDeath(dctx);
        gameplay::purchase::OnDeath();

        L.firstLethalSlot = -1;
        L.lastDamagerSlot = -1;
        L.lastDamageAt = -1000.0f;
    }

    // A side is eliminated when it is out of lives AND every player on it is dead.
    static bool SideEliminated(bool mine)
    {
        if (!TheMatch().MatchRunning()) return false;

        const int myTeam = net::matchmaking::MyTeam();
        if (myTeam < 0) return false;

        const int lives = mine ? TheMatch().BlueLives() : TheMatch().RedLives();
        if (lives > 0) return false;

        int  onSide = 0, alive = 0;
        bool anyUnresolved = false;

        auto check = [&](NetPlayer& pl) {
            if (!pl.active) return;
            const int plTeam = TeamOf(pl);
            if (plTeam < 0) { anyUnresolved = true; return; }
            if ((plTeam == myTeam) != mine) return;
            ++onSide;
            if (IsAlive(pl)) ++alive;
            };
        if (NetPlayer* me = NetPlayers::Local()) check(*me);
        for (NetPlayer& r : NetPlayers::Remotes()) check(r);

        if (anyUnresolved) return false;

        return onSide > 0 && alive == 0;
    }

    static constexpr float kElimConfirmSeconds = 0.5f;
    static float           s_blueElimFor = 0.0f;
    static float           s_redElimFor = 0.0f;
    void TickMatchAuthority(float dt)
    {
        TheMatch().nowSec += dt;
        TickAliveGrace(dt);

        TheMatch().TickMatch(dt);

        if (!net::session::IsHost() || net::gui2::ScoreboardShowing()) {
            s_blueElimFor = s_redElimFor = 0.0f;
            return;
        }

        if (!TheMatch().MatchRunning() && !TheMatch().MatchExpired()) {
            net::gui2::StartMatch();
            s_matchSyncAccum = kMatchSyncInterval;
        }
        s_matchSyncAccum += dt;
        if (s_matchSyncAccum >= kMatchSyncInterval) {
            s_matchSyncAccum -= kMatchSyncInterval;
            const NetTimerSync t = TheMatch().MatchSnapshot();
            MatchStateSync ms{ t.running, t.remaining, t.duration,
                               { static_cast<uint8_t>(TheMatch().LivesForTeam(0)),
                                 static_cast<uint8_t>(TheMatch().LivesForTeam(1)) } };
            net::session::SendEvent(NEVT_MATCH_TIMER_SYNC, ms);
        }

        // End the match when the clock hits 0:00, or a team is out of lives AND all of that
        // team's players are dead. Elimination must hold CONTINUOUSLY for kElimConfirmSeconds:
        // a single-frame health glitch, a packet gap, or the instant a respawn spends the last
        // life must never end a match. MatchExpired() is a latch, so the timer route needs no
        // hysteresis. BeginMatchOver() latches ScoreboardShowing(), so this fires exactly once.
        const bool timeUp = TheMatch().MatchExpired();
        s_blueElimFor = SideEliminated(true) ? (s_blueElimFor + dt) : 0.0f;
        s_redElimFor = SideEliminated(false) ? (s_redElimFor + dt) : 0.0f;
        const bool blueElim = s_blueElimFor >= kElimConfirmSeconds;   // my team wiped
        const bool redElim = s_redElimFor >= kElimConfirmSeconds;   // enemy wiped

        if (!timeUp && !blueElim && !redElim) return;

        // Decide the winning team (0 = Seraphites, 1 = WLF, 2 = draw, -1 = unknown)
        const int myTeam = net::matchmaking::MyTeam();
        int winner = -1;
        if (myTeam >= 0) {
            if (blueElim != redElim) {
                winner = redElim ? myTeam : (1 - myTeam); // the side NOT eliminated wins
            }
            else {
                const int blue = TheMatch().BlueLives();
                const int red = TheMatch().RedLives();
                winner = (blue > red) ? myTeam
                    : (blue < red) ? (1 - myTeam)
                    : MM_WINNER_DRAW;
            }
        }
        TO1_LOG("[MATCH] OVER — timeUp=%d blueElim=%d redElim=%d winner=%d\n",
            timeUp ? 1 : 0, blueElim ? 1 : 0, redElim ? 1 : 0, winner);

        MatchOverEvent mo{ static_cast<int8_t>(winner) };
        net::session::SendEvent(NEVT_MATCH_OVER, mo); // tell every client (carries the winner)
        net::gui2::BeginMatchOver(winner);
    }

    static constexpr float kHealDoneFrac = 0.9f;

    static void UpdateHealVox(bool dead)
    {
        auto& L = LocalState();

        if (dead) { L.healProgress = 0.0f; L.healPeak = 0.0f; return; }

        const float p = PLAYER::GET_HEAL_PROGRESS();

        if (p > 0.0f) {                       // still healing
            if (p > L.healPeak) L.healPeak = p;
            L.healProgress = p;
            return;
        }

        if (L.healProgress > 0.0f) {          // falling edge -- heal just ended
            TO1_LOG("[vox] heal ended, peak=%.2f (fires at >= %.2f)\n",
                L.healPeak, kHealDoneFrac);
            if (L.healPeak >= kHealDoneFrac)
                net::vox::PlayForLocal(net::vox::Scenario::HealFinish);
            L.healPeak = 0.0f;
        }
        L.healProgress = 0.0f;
    }

    void Tick()
    {
        net::replication::AlwaysRun();
        float dt = TIME::GET_DELTA_TIME();

        net::vox::Tick(dt);
        net::player::Tick(dt);

        UpdateKillFeed();
        UpdateListenMode();

        const bool dead = PLAYER::IS_IN_STATE(SID("Death"));
        const bool haveLives = TheMatch().BlueLives() > 0;
        const bool scoreboard = net::gui2::ScoreboardShowing();
        auto& L = LocalState();

        if (dead && !L.wasDead && !L.respawnTimer.Running() && !scoreboard) {
            ReportOwnDeath();
            if (haveLives) StartRespawnCountdown();
            else           EnterElimSpectate();
        }

        if (dead && !haveLives && L.respawnTimer.Running() && !scoreboard)
            EnterElimSpectate();

        if (!dead && L.elimSpec)
            ExitElimSpectate();

        L.wasDead = dead;
        UpdateHealVox(dead);
        UpdateRespawnCountdown(dt);
        UpdateRespawnPending(dt);
        UpdateSpectateCamera(dt);
        TickPing(dt);

        DC::Call<"send-event">(SID("disable-auto-holster"), SID("player"), MEMORY::NEW_BOXED_VALUE(BoxedValueType::kF32, 1.00f));
        DC::Call<"player-disable-explosion-state">();

        for (NetPlayer& p : NetPlayers::Remotes()) {
            NetSetSlotStreamAlive(p.netSlot,
                p.spawned && p.hasState && !p.animDetached &&
                (NowSec() - p.lastPacketTime) < kAliveStaleSeconds);

            // Release the held body once BOTH reasons have cleared: our own move no longer owns
            // it, and the victim has stopped reporting the melee action they were dying into.
            // The second is what a bystander waits on -- inMelee goes false the frame the
            // victim's own half ends, so the ragdoll lands exactly where they saw it.
            if (s_deathPending[p.netSlot]) {
                if (std::isfinite(dt) && dt > 0.0f) s_deathPendingAge[p.netSlot] += dt;

                const bool stillAnimating =
                    s_deathAnimated[p.netSlot] && p.hasState && p.State().inMelee != 0 &&
                    (NowSec() - p.lastPacketTime) < kAliveStaleSeconds &&
                    s_deathPendingAge[p.netSlot] < kAnimatedDeathHoldSec;

                if (!gameplay::melee::PuppetFrozenForExecution(p.netSlot) && !stillAnimating) {
                    s_deathPending[p.netSlot] = false;
                    s_deathAnimated[p.netSlot] = false;
                    TO1_LOG("[MELEE] slot%d death animation over -- running the deferred puppet death\n",
                        p.netSlot);
                    // Reclaim FIRST. A puppet we released for our own finisher already has
                    // dead=true from that predicted kill, and Die() opens with
                    // `if (isLocal || dead) return;` -- so without this the deferred death is a
                    // silent no-op and the body never drops. ReclaimFromEngine self-guards: if
                    // the engine really did kill it, dead stays true and Die() correctly does
                    // nothing.
                    p.ReclaimFromEngine();
                    if (!p.dead) p.Die();
                }
            }

            if (!p.spawned && p.hasState && !p.dead) {
                s_deathPending[p.netSlot] = false;
                s_deathAnimated[p.netSlot] = false;
                s_deathPendingAge[p.netSlot] = 0.0f;
                AutoSpawnRemote(p);
            }

            if (p.dead && p.hasState && p.State().health > 0.0f &&
                !TheMatch().slotState[p.netSlot].dead &&
                !gameplay::melee::PuppetFrozenForExecution(p.netSlot) &&
                (NowSec() - p.lastPacketTime) < kAliveStaleSeconds &&
                p.dyingSinceMs && (GetTickCount64() - p.dyingSinceMs) > kDesyncRecoverMs) {
                const PlayerNetState& rs = p.State();
                if (AllFinite(&rs.posX, 3) && IsUnitQuat(&rs.rotX)) {
                    TO1_LOG("[NET] slot%d desync: a corpse here, but they report %.0f hp -- resyncing\n",
                        p.netSlot, rs.health);
                    p.Respawn(Vector3{ rs.posX, rs.posY, rs.posZ },
                        Quaternion{ rs.rotX, rs.rotY, rs.rotZ, rs.rotW });
                    continue;
                }
            }

            // A corpse gets no replication at all
            if (p.dead) { p.TickDeath(); continue; }
            if (p.spawned && p.Exists())
                ApplyRemote(p, dt);
        }

        net::engine::ApplyWeaponSkeleton(NetPlayers::LocalPtr());
        //  for (NetPlayer& p : NetPlayers::Remotes())
        //      if (p.spawned && p.Exists())
        //          net::engine::ApplyWeaponSkeleton(p.Ptr());

        UpdateTeamHighlight();
        gameplay::perks::Tick(dt);
        gameplay::melee::Tick(dt);

        // HUD
        if (!s_debugMenuOpen && !TIME::IS_GAME_PAUSED() && !net::gui2::PauseMenuOpen() && !net::gui2::ScoreboardShowing()) {
            net::gui2::Spawn();
            net::gui2::RefreshWidgets();
            DrawKillFeed();
            DrawRadar();
            net::notify::Draw();   // styled notification text (the widgets are driven by notify::Tick)
            net::afk::Draw();      // anti-AFK warning (ported from tools/ext/afk.c)
        }

        const bool mnk = DC::Call<"is-mnk-primary-input-device?">();
        for (NetPlayer& p : NetPlayers::Remotes()) {
            if (!p.spawned || !p.npcSid || p.EngineOwned()) continue;
            if (mnk) DC::Call<"player-disable-aim-assist-on-object">(p.npcSid);
            else     DC::Call<"player-enable-aim-assist-on-object">(p.npcSid);
        }

        // disable gameplay controls while in menus
        if (net::gui2::PauseMenuOpen() || MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, SID("to1/backpack-menu")))
            DC::Call<"joypad-disable-all-commands-except">(SID("menu-back"), SID("menu-select"), SID("menu-left"), SID("menu-up"), SID("menu-right"), SID("menu-down"), SID("menu-up-hold"), SID("menu-down-hold"), SID("menu-select-vc"));
    }

    auto listGetAt = GameFn<uintptr_t(__fastcall*)(uintptr_t, int)>(0x1E236A0);
    auto asWeapon = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(0x1160970);
    auto isFirearm = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(0x11619F0);

    template <typename T>
    static bool ReadPod(const uint8_t* data, uint8_t size, T& out)
    {
        if (size < sizeof(T)) return false;
        memcpy(&out, data, sizeof(T));
        return true;
    }

    void HandleNetEvent(uint8_t senderSlot, uint8_t type, const uint8_t* data, uint8_t size)
    {
        if (senderSlot >= kMaxNetPlayers) return;
        NetPlayer& sender = NetPlayers::BySlot(senderSlot);

        switch (static_cast<NetEventType>(type)) {

        case NEVT_CHAT: {
            char msg[201] = {};
            memcpy(msg, data, (std::min<size_t>)(size, 200));
            TO1_LOG("[CHAT] %s: %s\n", sender.name, msg);
            break;
        }

        case NEVT_OPEN_DOOR: {
            DoorOpenEvent evt;
            if (!ReadPod(data, size, evt)) break;
            DC::Call<"send-event">(SID("open-door"), evt.doorSid, MEMORY::NEW_BOXED_VALUE(BoxedValueType::kSymbol, evt.stateSid));
            TO1_LOG("[NET] %s opened door %llx state=%llx\n", sender.name,
                static_cast<unsigned long long>(evt.doorSid),
                static_cast<unsigned long long>(evt.stateSid));
            break;
        }

        case NEVT_THROWPROJECTILE: {
            ProjectileThrowEvent evt;
            if (!ReadPod(data, size, evt)) break;

            uint64_t fromVec = MEMORY::ALLOC_VECTOR({ evt.fromPos[0], evt.fromPos[1], evt.fromPos[2] });
            uint64_t toVec = MEMORY::ALLOC_VECTOR({ evt.toPos[0],  evt.toPos[1],  evt.toPos[2] });

            uint64_t projectileSID = DC::Call<"throw-projectile">(fromVec, toVec, evt.throwableSID, -1.0f);

            uintptr_t projectile = DcManager::LookupObject(projectileSID);
            if (projectile) {
                auto getPhysics = GameFn<uintptr_t(__fastcall*)(uint64_t)>(0x11C7070);
                auto applyVel = GameFn<void(__fastcall*)(uintptr_t, float*)>(0x11C8B00);
                if (uintptr_t phys = getPhysics(projectile)) {
                    alignas(16) float vel[4] = { evt.vel[0], evt.vel[1], evt.vel[2], 0.0f };
                    applyVel(phys, vel);
                }
            }
            break;
        }

        case NEVT_FIRE_SHOT: {
            FireShotEvent evt;
            if (!ReadPod(data, size, evt)) break;
            const int exists = sender.npcSid ? (int)ENTITY::DOES_EXIST(sender.npcSid) : 0;
            const bool velFinite = std::isfinite(evt.launchVel[0]) &&
                std::isfinite(evt.launchVel[1]) &&
                std::isfinite(evt.launchVel[2]);
            const float launchSpeed = velFinite
                ? std::sqrt(evt.launchVel[0] * evt.launchVel[0] +
                    evt.launchVel[1] * evt.launchVel[1] +
                    evt.launchVel[2] * evt.launchVel[2])
                : 0.0f;
            TO1_LOG("[FIRE] recv shot slot=%d npcSid=%llx exists=%d launch |v|=%.2f\n",
                senderSlot, static_cast<unsigned long long>(sender.npcSid), exists, launchSpeed);
            if (sender.npcSid && exists) {
                if (launchSpeed > 0.0f) {
                    memcpy(sender.pendingLaunchVel, evt.launchVel, sizeof(sender.pendingLaunchVel));
                    sender.pendingLaunchAt = NowSec();
                    sender.pendingLaunchValid = true;
                }
                DC::Call<"npc-fire-weapon">(sender.npcSid);
            }
            break;
        }

        case NEVT_TAKE_DAMAGE: {
            PlayerDamageEvent evt;
            if (!ReadPod(data, size, evt)) break;
            uintptr_t local = NetPlayers::LocalPtr();
            uintptr_t attacker = sender.Ptr();

            if (gameplay::melee::CommittedToExecutor()) {
                TO1_LOG("[NET] %.1f damage from %s ignored -- a finisher owns this kill\n",
                    evt.damage, sender.name);
                break;
            }

            TO1_LOG("[NET] taking %.1f damage from %s\n", evt.damage, sender.name);

            // ext recv_hit_hook: the alert sting reads the calm clock BEFORE the music heat
            // input marks this frame hot (AlertstingCombat then MusicEvent("we took a hit")).
            if (evt.damage > 0.0f && evt.damage < 100000.0f) {
                net::audio::alertsting::Combat("shot / hit out of the quiet");
                net::audio::music_tier::NoteEvent(3.0f, "we took a hit");
            }

            auto& L = LocalState();
            L.lastDamagerSlot = static_cast<int>(senderSlot);
            L.lastDamageWeapon = evt.weaponSID;
            L.lastDamageHead = evt.headshot != 0;
            L.lastDamageAt = NowSec();

            if (evt.lethalClaim && L.firstLethalSlot < 0) {
                L.firstLethalSlot = static_cast<int>(senderSlot);
                L.firstLethalWeapon = evt.weaponSID;
                L.firstLethalHead = evt.headshot != 0;
                TO1_LOG("[NET] lethal claim from %s (slot%u) — they get the kill\n",
                    sender.name, senderSlot);
            }

            net::engine::FireAttack(local, attacker, evt.weaponSID, evt.weaponAmmoSID, evt.damage);
            break;
        }

        case NEVT_MATCH_OVER: {
            // Host says the match ended: scoreboard -> 10s -> to1 main menu.
            MatchOverEvent mo{ -1 };
            ReadPod(data, size, mo);
            net::gui2::BeginMatchOver(mo.winner);
            break;
        }

        case NEVT_VOX_LINE: {
            VoxLineEvent evt;
            if (!ReadPod(data, size, evt)) break;
            net::vox::HandleLine(evt.speakerSlot, evt.lineSID);
            break;
        }

        case NEVT_MARK_PLAYER: {
            AimMarkEvent evt;
            if (!ReadPod(data, size, evt)) break;
            gameplay::ApplyNetMark(senderSlot, evt.markedSlot);
            break;
        }

        case NEVT_RESPAWN_REQUEST: {
            if (!net::session::IsHost()) break;
            HostGrantRespawn(static_cast<int>(senderSlot));
            break;
        }

        case NEVT_DEBUGGING: {
            if (!net::session::IsHost()) break;
            DebuggingEvent evt;
            if (!ReadPod(data, size, evt)) break;
            sender.tamperFlags |= evt.flags;
            TO1_LOG("[NET] tamper slot%u \"%s\" debugger flags=0x%04x\n", senderSlot, sender.name, evt.flags);
            break;
        }

        case NEVT_SUSPICIOUS_PROCESS: {
            if (!net::session::IsHost()) break;
            SuspiciousProcessEvent evt;
            if (!ReadPod(data, size, evt)) break;
            evt.processName[sizeof(evt.processName) - 1] = 0;
            for (char& c : evt.processName) {
                if (c == 0) break;
                if (c < 0x20 || c >= 0x7F) c = '?';
            }
            sender.tamperPid = evt.pid;
            memcpy(sender.tamperProcess, evt.processName, sizeof(sender.tamperProcess));
            TO1_LOG("[NET] tamper slot%u \"%s\" process pid=%u name=%s\n", senderSlot, sender.name, evt.pid, evt.processName);
            break;
        }

        case NEVT_RESPAWN_GRANT: {
            RespawnGrantEvent g;
            if (!ReadPod(data, size, g)) break;
            if (senderSlot != 0) { // the host is always slot 0
                TO1_LOG("[NET] ignoring respawn grant from non-host slot%u\n", senderSlot);
                break;
            }
            ApplyRespawnGrant(static_cast<int>(g.slot), g.spawnerSid, static_cast<int>(g.team));
            break;
        }

        case NEVT_PLAYER_DEATH: {
            PlayerDeathEvent evt;
            if (!ReadPod(data, size, evt)) break;

            if (evt.victimSlot != senderSlot) {
                TO1_LOG("[NET] ignoring death for slot%u sent by slot%u\n",
                    evt.victimSlot, senderSlot);
                break;
            }

            const int killerSlot = (evt.killerSlot == kInvalidSlot) ? -1 : static_cast<int>(evt.killerSlot);
            ApplyConfirmedDeath(static_cast<int>(evt.victimSlot), killerSlot, evt.weaponSID, evt.headshotKill != 0);
            break;
        }

        case NEVT_EXECUTION_REQUEST: {
            if (!gameplay::melee::tuning::kExecutionEnabled) break;
            ExecutionRequestEvent req;
            if (!ReadPod(data, size, req)) break;
            NetPlayer* local = NetPlayers::Local();
            if (!local || local->netSlot != static_cast<int>(req.victimSlot)) break;

            const int code = (sender.spawned && sender.npcSid)
                ? gameplay::melee::AcceptIncomingExecution(sender.netSlot)
                : gameplay::melee::kExecReplyRefused;
            const bool ok = (code == gameplay::melee::kExecReplyAccepted);

            ExecutionReplyEvent reply{};
            reply.attackerSlot = static_cast<uint8_t>(sender.netSlot);
            reply.accepted = static_cast<uint8_t>(code);
            net::session::SendEvent(NEVT_EXECUTION_REPLY, reply,
                static_cast<uint8_t>(sender.netSlot));

            TO1_LOG("[MELEE] execution request from %s -> %s\n", sender.name,
                ok ? "accepted" : "refused");
            break;
        }

        case NEVT_EXECUTION_REPLY: {
            if (!gameplay::melee::tuning::kExecutionEnabled) break;
            ExecutionReplyEvent reply;
            if (!ReadPod(data, size, reply)) break;
            NetPlayer* local = NetPlayers::Local();
            if (!local || local->netSlot != static_cast<int>(reply.attackerSlot)) break;
            gameplay::melee::OnExecutionReply(sender.netSlot, static_cast<int>(reply.accepted));
            break;
        }

        case NEVT_PLAYER_EXECUTION: {
            if (!gameplay::melee::tuning::kExecutionEnabled) break;
            ExecutionEvent evt;
            if (!ReadPod(data, size, evt)) break;
            NetPlayer* local = NetPlayers::Local();
            if (!local || local->netSlot != static_cast<int>(evt.victimSlot)) break;
            if (!sender.spawned || !sender.npcSid) break;

            if (gameplay::melee::AcceptIncomingExecution(sender.netSlot) !=
                gameplay::melee::kExecReplyAccepted) break;

            net::engine::StopMeleeAction(SID("player"));
            sender.StopMelee();
            gameplay::melee::ExemptMeleeTarget(sender.npcSid,
                gameplay::melee::tuning::kExecutionMaxHoldSec + gameplay::melee::tuning::kExecutionMinHoldSec);

            uint64_t playedMove = evt.moveSID;
            uint64_t started = DC::Call<"start-melee-action">(sender.npcSid, SID("player"), playedMove, 0ull, 0ull, 1ull, 0ull, 0ull);
            if (!started && playedMove != gameplay::melee::tuning::kExecutionMove) {
                playedMove = gameplay::melee::tuning::kExecutionMove;
                started = DC::Call<"start-melee-action">(sender.npcSid, SID("player"), playedMove, 0ull, 0ull, 1ull, 0ull, 0ull);
                TO1_LOG("[MELEE] executed by %s: %llx refused, retried canonical %llx -> %s\n",
                    sender.name, static_cast<unsigned long long>(evt.moveSID),
                    static_cast<unsigned long long>(playedMove),
                    started ? "playing" : "refused by engine");
            }
            else {
                TO1_LOG("[MELEE] executed by %s: sync half %llx -> %s\n", sender.name,
                    static_cast<unsigned long long>(playedMove),
                    started ? "playing" : "refused by engine");
            }
            if (started) {
                auto& L = LocalState();
                L.firstLethalSlot = sender.netSlot;
                L.firstLethalWeapon = playedMove;
                L.firstLethalHead = false;
                L.lastDamagerSlot = sender.netSlot;
                L.lastDamageWeapon = playedMove;
                L.lastDamageHead = false;
                L.lastDamageAt = NowSec();

                gameplay::melee::OnExecutedBy(sender.netSlot, sender.npcSid, playedMove);
            }
            else
                gameplay::melee::ExemptMeleeTarget(0, 0.0f);
            break;
        }

        case NEVT_PLAYER_MELEE_ATTACK: {
            uint64_t actionSid;
            if (!ReadPod(data, size, actionSid)) break;
            sender.StopMelee();
            sender.ForceMelee(actionSid);
            break;
        }

        case NEVT_MATCH_TIMER_SYNC: {
            MatchStateSync ms;
            if (!ReadPod(data, size, ms)) break;
            NetTimerSync sync{ ms.timerRunning, ms.timerRemaining, ms.timerDuration };
            TheMatch().ApplyMatchSync(sync);
            TheMatch().SetLivesAbsolute(ms.teamLives[0], ms.teamLives[1]);
            break;
        }

        case NEVT_PING: {
            PingEvent evt;
            if (!ReadPod(data, size, evt)) break;
            if (!evt.reply) {
                sender.reportedPingMs = evt.hostPingMs;
                evt.reply = 1;
                net::session::SendEvent(NEVT_PING, evt, senderSlot);
                break;
            }
            const int32_t rttMs = static_cast<int32_t>(NowMs() - evt.sendMs);
            if (rttMs < 0) break;
            sender.RecordPing(static_cast<uint32_t>(rttMs));
            break;
        }

        case NEVT_SCOPE_GLINT: {
            net::scopeglint::OnEvent(senderSlot, data, size);
            break;
        }

        case NEVT_GLASS: {
            net::glass::OnEvent(senderSlot, data, size);
            break;
        }

        case NEVT_BACKPACK: {
            net::match::backpack::OnEvent(senderSlot, data, size);
            break;
        }

        default:
            break;
        }
    }

    void ResetRuntime()
    {
        memset(s_deathPending, 0, sizeof(s_deathPending));
        memset(s_deathAnimated, 0, sizeof(s_deathAnimated));
        memset(s_deathPendingAge, 0, sizeof(s_deathPendingAge));   // no latch survives a session
        auto& L = LocalState();
        L.respawnTimer.Clear();
        L.respawnLastSec = -1;
        L.wasDead = false;
        L.healProgress = 0.0f;
        L.healPeak = 0.0f;
        net::gui2::ClearMatch();
        s_matchSyncAccum = 0.0f;
        s_pingAccum = 0.0f;
        g_local = LocalCapture{};

        gameplay::perks::Reset();
        gameplay::melee::Reset();
        net::vox::Reset();
        net::afk::Reset();
        net::jip::Reset();
        net::friends13::Reset();
        net::match::liveness::Reset();
        net::requeue::Reset();
        net::direct::Reset();
        net::audio::music_tier::Reset();

        kListenModeDurability = 1.0f;

        net::patch::FlushMaterialPatchQueue();

        L.killerSlot = -1;
        s_specActive = false;
        L.elimSpec = false;
        L.elimSpecTime = 0.0f;
        L.specTargetSlot = -1;
        L.specOrphan = 0.0f;
        L.specStuckLogged = false;
        s_blueElimFor = 0.0f;
        s_redElimFor = 0.0f;
        for (auto& s : TheMatch().slotState) s.respawnGrace = 0.0f;

        L.respawnPending = false;
        L.respawnPendingSec = 0.0f;
        for (auto& s : TheMatch().slotState) s.grantCooldown = 0.0f;
        for (auto& s : TheMatch().slotState) s.dead = false;

        L.firstLethalSlot = -1;
        L.firstLethalWeapon = 0;
        L.firstLethalHead = false;
        L.lastDamagerSlot = -1;
        L.lastDamageWeapon = 0;
        L.lastDamageHead = false;
        L.lastDamageAt = -1000.0f;
        TheMatch().nowSec = 0.0f;
    }

}
