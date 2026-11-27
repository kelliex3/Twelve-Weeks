// Native owner of the victim-POV death screen. See death-ui.hpp.
//
// Everything the ext did is now native:
//   - timeline / phase machine / alpha tracks / tip rotation (core::Step)
//   - text / image / visible writes on the baked t2rDeath widgets (engine::widget)
//   - HUD-under-black fade: walk canvas.hudRoot children via engine::gui2_low,
//     scale their alpha by (1 - black), restore on exit
//   - death sting via net::ui::death_sting (raw PCM, compiled-in only;
//     no-op sink when TO1_DEATH_STING_PCM is not defined)
//   - spectate offer / switch / auto-offer presentation (core::Step) +
//     native lobby fields +0x2c44..+0x2c54 (engine::lobby)
//   - opt-out / tuning sidecars via engine::flag / engine::exe_side
//   - weapon credit resolver: core::Weapon (static table + exact SID +
//     "-supp" base fallback)
//   - edge signals: NoteEdge from net-replication (downed lethal / execution /
//     molotov burn), hp/Death fallback always read
//   - shiv / paired-victim timing: net::anim::paired + net::audio::shivsting
//   - pause-menu factor + winmm death-banner hide (core + net::gui2)
//
// The ext is still loaded first and mirrors the same baked t2rDeath widgets.
// Every death-UI decision and write is made here; native last write wins.
// No second settext hook, no ext PCM path, no t2r-*.txt toggle is flipped
// (existing sidecars are read through the mod's own flag/exe_side layer).

#include "death-ui.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp
#include "engine/locale.hpp"
#include "engine/local-player.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "engine/gui2-low.hpp"
#include "engine/lobby.hpp"
#include "engine/flag.hpp"
#include "engine/exe_side.hpp"
#include "net-gui2.hpp"
#include "net/match/match-liveness.hpp"
#include "net-session.hpp"
#include "net/match/net-player.hpp"
#include "net/anim/paired.hpp"
#include "net/audio/shivsting.hpp"
#include "net/audio/death-theme.hpp"
#include "gameplay/melee2013.hpp"
#include "net/ui/death-sting.hpp"
#include "sid/sidbase.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::death_ui {

    namespace {
        float g_lastBlack = 0.0f;

        using namespace core;

        // Path SID for canvas.hudRoot (the ext walks its children for the
        // HUD-under-black fade). Computed once with the same FNV-1a 64 as the
        // engine's SID().
        inline uint64_t CanvasHudRoot() noexcept
        {
            return Fnv64("canvas.hudRoot");
        }

        // ---- sidecar config -----------------------------------------------
        // Read through engine::flag / engine::exe_side. These mirror the ext's
        // t2r-*.txt behaviour natively; no toggle's default is changed.
        bool   g_instantMode = false;      // t2r-death-screen-instant.txt
        int    g_instantDelay = int(kDelayMs);   // t2r-death-delay.txt (0..3000 ms)
        bool   g_softMode = false;         // t2r-death-screen-delay.txt
        int    g_leadMs = 0;               // t2r-death-screen-delay.txt value (ms)
        bool   g_pairHoldOld = false;      // t2r-death-pair-hold-old.txt
        bool   g_execEndNew = true;        // !t2r-death-exec-end-ps45-old.txt
        bool   g_noShivDelay = false;      // t2r-no-shiv-death-delay.txt

        void ReadSidecars()
        {
            g_instantMode = engine::flag::Present("t2r-death-screen-instant.txt");

            g_instantDelay = int(kDelayMs);
            if (FILE* df = engine::exe_side::Open("t2r-death-delay.txt", "r")) {
                int v = 0;
                if (std::fscanf(df, "%d", &v) == 1 && v >= 0 && v <= 3000)
                    g_instantDelay = v;
                std::fclose(df);
            }

            g_softMode = false;
            g_leadMs = 0;
            if (FILE* sf = engine::exe_side::Open("t2r-death-screen-delay.txt", "r")) {
                double sv = 0.0;
                if (std::fscanf(sf, "%lf", &sv) == 1 && sv >= 0.0 && sv <= 6.0)
                    g_leadMs = int(sv * 1000.0 + 0.5);
                g_softMode = g_leadMs > 0;
                std::fclose(sf);
            }

            g_pairHoldOld = engine::flag::Present("t2r-death-pair-hold-old.txt");
            g_execEndNew = !engine::flag::Present("t2r-death-exec-end-ps45-old.txt");
            g_noShivDelay = engine::flag::Present("t2r-no-shiv-death-delay.txt");
        }

        // ---- producer notes -----------------------------------------------
        struct Notes {
            int      killerSlot = -1;
            uint64_t weaponSid = 0;
            char     killer[48] = {};
            bool     respawnRunning = false;
            float    respawnRemaining = 0.0f;
            bool     outOfLives = false;
            int      spectateTarget = -1;
            char     spectateName[48] = {};
            bool     edgeSignalled = false;
            uint64_t edgeAtMs = 0;
            EdgeKind edgeKind = EdgeKind::HpState;
            int      edgeKillerSlot = -1;
            uint64_t edgeWeapon = 0;
            int      spectateSlots[kMaxNetPlayers];
            int      spectateSlotCount = 0;
        } g_notes;

        State  g_state;
        uint64_t g_lastNow = 0;
        uint64_t g_edgeMs = 0;          // time of last NoteEdge (pairedLeftMs reference)
        bool   g_available = false;
        bool   g_rootShown = false;

        uint64_t g_w[TrackCount] = {};
        float    g_wrote[TrackCount];

        int      g_lastTip = -1;
        int      g_lastCount = -2;
        bool     g_lastNoLives = false;
        uint64_t g_lastCard = ~0ULL;    // death id the card was filled for
        int      g_lastSpec = -2;       // spectate target the bar names
        bool     g_lastSwitch = false;
        uint64_t g_deathId = 0;

        // HUD-under-black: saved widget alpha values for restore
        struct HudEntry { uintptr_t w; float base; float wrote; };
        HudEntry g_hud[32];
        int      g_hudN = 0;
        bool     g_hudBuilt = false;

        // ---- widget helpers ------------------------------------------------
        uint64_t W(const char* rel) { return engine::widget::Hud(rel); }

        void UiText(const char* path, const char* text)
        {
            const uint64_t w = W(path);
            if (!w) return;
            engine::widget::SetText(w, engine::locale::Tr(text ? text : ""));
        }

        void UiImage(const char* path, const char* image)
        {
            const uint64_t w = W(path);
            if (w && image) engine::widget::SetImage(w, image);
        }

        void UiVisible(const char* path, bool on)
        {
            const uint64_t w = W(path);
            if (w) engine::widget::Visible(w, on);
        }

        void DbVisible(const char* a, const char* b, bool on)
        {
            UiVisible(a, on);
            UiVisible(b, on);
        }

        void DbText(const char* a, const char* b, const char* text)
        {
            UiText(a, text);
            UiText(b, text);
        }

        void Copy(char* dst, size_t cap, const char* src)
        {
            if (!dst || !cap) return;
            if (!src) { dst[0] = '\0'; return; }
            size_t n = 0;
            while (src[n] && n + 1 < cap) { dst[n] = src[n]; ++n; }
            dst[n] = '\0';
        }

        // Called when the mod reports a fresh death (or on Tip output from the core).
        void WriteTip(int index)
        {
            if (index == g_lastTip) return;
            g_lastTip = index;
            char path[128];
            TipImage(index, path, sizeof path);
            UiImage("t2rDeath.dTip.dTipImg", path);
            UiText("t2rDeath.dTip.dTipText", TipText(index));
            UiVisible("t2rDeath.dSpec.dSpecLbl", true);
            UiVisible("t2rDeath.dSpec.dSpecLblB", true);
        }

        void FillCard()
        {
            if (g_lastCard == g_deathId) return;
            g_lastCard = g_deathId;

            const int k = g_notes.killerSlot;
            const int me = net::session::MySlot();
            char kn[48] = {};
            if (k >= 0 && k != me) {
                Copy(kn, sizeof kn, g_notes.killer[0] ? g_notes.killer : nullptr);
                if (!kn[0]) Copy(kn, sizeof kn, "UNKNOWN");
            } else if (k == me) {
                Copy(kn, sizeof kn, "YOURSELF");
            }

            const char* sid = g_notes.weaponSid
                ? NdGameSdk::Sidbase::StringIdToStringInternal(g_notes.weaponSid)
                : "";
            const WeaponInfo wi = Weapon(sid, g_notes.weaponSid);

            UiText("t2rDeath.dCard.dKilledBy", kn[0] ? "KILLED BY" : "YOU DIED");
            UiText("t2rDeath.dCard.dKiller", kn);
            UiVisible("t2rDeath.dCard.dKiller", kn[0] != 0);
            if (wi.found) {
                UiText("t2rDeath.dCard.dWName", wi.name);
                UiImage("t2rDeath.dCard.dWIcon", wi.icon);
            }
            UiVisible("t2rDeath.dCard.dWName", wi.found);
            UiVisible("t2rDeath.dCard.dWIcon", wi.found && wi.icon != nullptr);
            TO1_LOG("[death-ui] card: slot %d '%s', weapon %llx -> %s\n",
                k, kn, static_cast<unsigned long long>(g_notes.weaponSid),
                wi.found ? wi.name : "none");
        }

        void WriteCount(int seconds, bool noLives)
        {
            if (seconds == g_lastCount && noLives == g_lastNoLives) return;
            g_lastCount = seconds;
            g_lastNoLives = noLives;

            const char* label = seconds >= 0 ? "RESPAWNING IN"
                : noLives ? "NO REINFORCEMENTS" : "RESPAWNING";
            UiText("t2rDeath.dCount.dCountLbl", label);
            const bool haveNumber = seconds >= 0;
            DbVisible("t2rDeath.dCount.dCountNum",
                "t2rDeath.dCount.dCountNumB", haveNumber);
            if (haveNumber) {
                char b[16];
                std::snprintf(b, sizeof b, "%d", seconds);
                DbText("t2rDeath.dCount.dCountNum",
                    "t2rDeath.dCount.dCountNumB", b);
            }
        }

        void WriteSpecBar(const Outputs& out)
        {
            if (out.showSpecBar && g_notes.spectateTarget != g_lastSpec) {
                g_lastSpec = g_notes.spectateTarget;
                char b[48];
                Copy(b, sizeof b, g_notes.spectateName[0]
                    ? g_notes.spectateName : "TEAMMATE");
                DbText("t2rDeath.dSpecBar.dSpecNm",
                    "t2rDeath.dSpecBar.dSpecNmB", b);
            }
            if (!out.showSpecBar) g_lastSpec = -2;
            if (out.showSwitch != g_lastSwitch) {
                g_lastSwitch = out.showSwitch;
                UiVisible("t2rDeath.dSpecBar.dSwitch", out.showSwitch);
            }
        }

        // ---- HUD-under-black (ext DuHudBuild / Apply / Restore) ------------
        // Walk canvas.hudRoot's children via engine::gui2_low, save each
        // non-t2rDeath widget's base alpha. While black is up, scale every
        // child's alpha by (1 - black); on exit, restore the saved values.
        void BuildHud()
        {
            g_hudN = 0;
            uint64_t child = 0;
            engine::gui2_low::FirstChild(CanvasHudRoot(), &child);
            for (int guard = 0; child && g_hudN < 32 && guard < 64; ++guard) {
                const uintptr_t w = engine::gui2_low::Widget(child);
                if (w) {
                    const char* nm = engine::gui2_low::Name(w);
                    if (std::strncmp(nm, "t2r", 3)) {   // skip our own widgets
                        g_hud[g_hudN].w = w;
                        g_hud[g_hudN].base = engine::gui2_low::Alpha(w);
                        g_hud[g_hudN].wrote = -1.0f;
                        ++g_hudN;
                    }
                }
                uint64_t nx = 0;
                engine::gui2_low::NextSibling(child, &nx);
                child = nx;
            }
        }

        void ApplyHud(float black)
        {
            if (g_hudN == 0) return;
            for (int i = 0; i < g_hudN; ++i) {
                const uintptr_t w = g_hud[i].w;
                if (!w) continue;
                const float a = g_hud[i].base * (1.0f - black);
                if (std::fabs(a - g_hud[i].wrote) >= 0.002f) {
                    engine::gui2_low::SetAlpha(w, a);
                    g_hud[i].wrote = a;
                }
            }
        }

        void RestoreHud()
        {
            for (int i = 0; i < g_hudN; ++i) {
                const uintptr_t w = g_hud[i].w;
                if (w && g_hud[i].wrote >= 0.0f) {
                    engine::gui2_low::SetAlpha(w, g_hud[i].base);
                    g_hud[i].wrote = -1.0f;
                }
            }
        }

        // ---- spectate switch -----------------------------------------------
        // Cycle the lobby spectate target to the next / previous living
        // watchable teammate (ext DuMates + the camera owner's switch).
        void DoSpectateSwitch(int dir)
        {
            if (g_notes.spectateSlotCount <= 0) return;

            bool on = false;
            float timer = 0.0f;
            int target = -1;
            engine::lobby::SpectateState(on, timer, target);

            int idx = -1;
            for (int i = 0; i < g_notes.spectateSlotCount; ++i) {
                if (g_notes.spectateSlots[i] == target) { idx = i; break; }
            }
            if (idx < 0) idx = 0;

            const int step = dir > 0 ? 1 : -1;
            int next = (idx + step + g_notes.spectateSlotCount) % g_notes.spectateSlotCount;
            const int nextSlot = g_notes.spectateSlots[next];

            if (nextSlot != target && nextSlot >= 0) {
                engine::lobby::SpectateSetTarget(nextSlot);
                // Update the presentation on the next frame via the spectate
                // owner; also nudge it now so the spec bar names the new target.
                const NetPlayer& p = NetPlayers::BySlot(nextSlot);
                NoteSpectate(nextSlot, p.name);
            }
        }

        // ---- apply ---------------------------------------------------------
        void Apply(const Outputs& out)
        {
            const uint64_t root = W("t2rDeath");
            if (!root) return;

            if (out.active) {
                if (!g_rootShown) {
                    engine::widget::Visible(root, true);
                    g_rootShown = true;
                    for (int i = 0; i < TrackCount; ++i) g_wrote[i] = -1.0f;
                }

                if (out.setTip) {
                    g_deathId = g_lastNow;
                    g_lastCard = ~0ULL;
                    g_lastCount = -2;
                    g_lastNoLives = false;
                    WriteTip(out.tipIndex);
                }
                if (out.fillCard) FillCard();
                if (out.setCount) WriteCount(out.seconds, out.noLives);
                WriteSpecBar(out);

                for (int i = 0; i < TrackCount; ++i) {
                    const float v = out.tracks[i];
                    if (std::fabs(v - g_wrote[i]) < 0.002f) continue;
                    if (!g_w[i]) g_w[i] = W(kTrackPath[i]);
                    if (g_w[i]) engine::widget::Alpha(g_w[i], v);
                    g_wrote[i] = v;
                }
            }

            net::gui2::SetDeathBannerSuppressed(out.active);

            if (out.exit) {
                engine::widget::Visible(root, false);
                g_rootShown = false;
                g_lastTip = -1;
                g_lastCount = -2;
                g_lastSpec = -2;
                g_lastSwitch = false;
                net::gui2::SetDeathBannerSuppressed(false);
                if (out.resetRecords) {
                    g_notes.killerSlot = -1;
                    g_notes.weaponSid = 0;
                    g_notes.killer[0] = '\0';
                    g_notes.spectateTarget = -1;
                    g_notes.spectateName[0] = '\0';
                }
            }
        }

    } // namespace

    void NoteDeath(int killerSlot, uint64_t weaponSid, const char* killerName)
    {
        g_notes.killerSlot = killerSlot;
        g_notes.weaponSid = weaponSid;
        Copy(g_notes.killer, sizeof g_notes.killer, killerName);
    }

    void NoteRespawn(bool running, float remaining, bool outOfLives)
    {
        g_notes.respawnRunning = running;
        g_notes.respawnRemaining = remaining;
        g_notes.outOfLives = outOfLives;
    }

    void NoteSpectate(int targetSlot, const char* targetName)
    {
        g_notes.spectateTarget = targetSlot;
        Copy(g_notes.spectateName, sizeof g_notes.spectateName, targetName);
    }

    void NoteEdge(core::EdgeKind kind, uint64_t atMs, int killerSlot, uint64_t weapon)
    {
        g_notes.edgeSignalled = true;
        g_notes.edgeAtMs = atMs ? atMs : ::GetTickCount64();
        g_notes.edgeKind = kind;
        g_notes.edgeKillerSlot = killerSlot;
        g_notes.edgeWeapon = weapon;
        g_edgeMs = g_notes.edgeAtMs;
    }

    void NoteSpectateOptions(const int* slots, int count)
    {
        g_notes.spectateSlotCount = count;
        for (int i = 0; i < count && i < kMaxNetPlayers; ++i)
            g_notes.spectateSlots[i] = slots[i];
    }

    bool Active() noexcept { return g_state.phase != 0; }

    void Reset()
    {
        core::Reset(g_state, g_lastNow ? g_lastNow : ::GetTickCount64());
        g_available = false;
        g_rootShown = false;
        g_hudBuilt = false;
        g_hudN = 0;
        g_lastTip = -1;
        g_lastCard = ~0ULL;
        g_lastCount = -2;
        g_lastNoLives = false;
        g_lastSpec = -2;
        g_lastSwitch = false;
        g_deathId = 0;
        g_edgeMs = 0;
        for (int i = 0; i < TrackCount; ++i) { g_w[i] = 0; g_wrote[i] = -1.0f; }
        g_notes = Notes{};
        net::gui2::SetDeathBannerSuppressed(false);
    }

    void Tick(uint64_t nowMs)
    {
        g_lastNow = nowMs;
        ReadSidecars();

        if (engine::flag::Present("t2r-no-death-ui.txt")) {
            if (g_state.phase || g_rootShown) {
                if (g_rootShown) {
                    if (const uint64_t root = W("t2rDeath"))
                        engine::widget::Visible(root, false);
                    g_rootShown = false;
                }
                if (g_hudBuilt) { RestoreHud(); g_hudBuilt = false; }
                net::gui2::SetDeathBannerSuppressed(false);
                core::Reset(g_state, nowMs);
            }
            return;
        }

        // The ext probes the baked group once per match. With a GUI pak that
        // has no t2rDeath there is no native death screen either: stay off,
        // keep the banner available.
        if (!g_available) {
            if (!W("t2rDeath")) {
                net::gui2::SetDeathBannerSuppressed(false);
                return;
            }
            g_available = true;
            core::Reset(g_state, nowMs);
        }

        // ---- local player state -------------------------------------------
        const float hp = engine::local_player::Health();
        const bool  deathState = PLAYER::IS_IN_STATE(SID("Death"));
        const bool  dead = (hp > -0.5f && hp <= 0.0f) || deathState;
        const bool  alive = hp > 0.0f && !deathState;

        // ---- shiv / paired-victim / execution timing ----------------------
        bool   shivVictim = false;
        int    shivStingMs = 0;
        int    pairedLeftMs = -1;
        bool   execution = false;
        {
            const auto& ps = net::anim::paired::Get();
            if (ps.role == 2) {   // we are the paired-move victim
                execution = (ps.flags & net::anim::paired::kFLethal) != 0;
                shivVictim = gameplay::melee2013::IsShivMove(ps.move);
                shivStingMs = net::audio::shivsting::RemainingMs();
                if (ps.killS > 0.0f && g_edgeMs != 0) {
                    const int killMs = int(ps.killS * 1000.0f + 0.5f);
                    const int elapsed = int(nowMs - g_edgeMs);
                    pairedLeftMs = killMs - elapsed;
                    if (pairedLeftMs < 0) pairedLeftMs = 0;
                }
            }
        }
        // The edge note's own classification refines execution (e.g. a downed
        // lethal that is not a paired move is not an execution).
        if (g_notes.edgeSignalled && g_notes.edgeKind == EdgeKind::Execution)
            execution = true;

        // ---- inputs --------------------------------------------------------
        Inputs in;
        in.matchLive = net::match::liveness::Live();
        in.matchOver = net::match::liveness::MatchOver();
        in.alive = alive;
        in.dead = dead;
        in.deadEdge = dead;          // rising edge detected by core via wasDead
        in.execution = execution;
        in.pauseMenu = net::gui2::PauseMenuOpen();
        in.softMode = g_softMode && !g_instantMode;   // instant overrides soft
        in.leadMs = g_leadMs;
        in.respawnRunning = g_notes.respawnRunning;
        in.respawnRemaining = g_notes.respawnRemaining;
        in.outOfLives = g_notes.outOfLives;
        in.spectateTarget = g_notes.spectateTarget;
        in.edgeSignalled = g_notes.edgeSignalled;
        in.edgeAtMs = g_notes.edgeAtMs;
        in.forceInstant = g_instantMode;
        in.shivVictim = shivVictim;
        in.shivStingMs = shivStingMs;
        in.pairedLeftMs = pairedLeftMs;
        in.pairHoldOld = g_pairHoldOld;
        in.execEndNew = g_execEndNew;
        in.spectateCandidates = g_notes.spectateSlotCount;
        in.spectateButton = false;   // no native interact-edge producer yet
        in.spectateSwitch = 0;       // no native switch-edge producer yet
        in.delayMsOverride = g_instantMode ? g_instantDelay : -1;

        Outputs out;
        core::Step(g_state, in, out, nowMs);
        g_lastBlack = out.black;

        // ---- death sting + drone (native PCM sinks; no-op without data) ----
        if (out.stingDue || out.softStingAtEdge) {
            death_sting::Play();
            // The ext arms its death drone on the same sting edge (including the
            // shiv / execution victim cue). The owner reads the actual sidechain
            // itself; stingMs / exec are only used for its one-time log line.
            net::audio::death_theme::Arm(shivStingMs, execution);
        }

        // ---- spectate lobby fields ----------------------------------------
        if (out.spectateOn) {
            engine::lobby::SpectateBegin();
        }
        if (out.spectateOff) {
            engine::lobby::SpectateEnd();
        }
        if (out.spectateSwitchDir != 0 && g_notes.spectateSlotCount > 0) {
            DoSpectateSwitch(out.spectateSwitchDir);
        }

        // ---- HUD-under-black fade -----------------------------------------
        if (out.hideHud) {
            if (!g_hudBuilt) { BuildHud(); g_hudBuilt = true; }
            ApplyHud(out.black);
        } else if (g_hudBuilt) {
            RestoreHud();
            g_hudBuilt = false;
        }
        if (out.exit && g_hudBuilt) {
            RestoreHud();
            g_hudBuilt = false;
        }

        Apply(out);

        // Consume the edge note once (newest note wins).
        g_notes.edgeSignalled = false;
    }

    int Phase() noexcept { return g_state.phase; }

    bool Final() noexcept { return g_state.final; }

    float Black() noexcept { return g_lastBlack; }

} // namespace net::ui::death_ui
