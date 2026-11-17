// intvox.c - the voices of an Interrogation (in 2013 Factions the character just held and shook the victim around; vox like
// 'Where?! ... I don't know!' keeps it from being two people staring at each other). Included from t2r-ext.c after exec_beg.c (uses its voice-set helpers).
//
// The pair is the 2013 interrogate-enter move (MX_INTERROGATE fists / MX_INT_SHIV shiv, interrogation.c): ex_begin starts the sound timeline on every screen
// (executioner, victim and bystanders - g_ex.att_ent / vic_ent are the two bodies there). From it this file speaks, in each body's OWN NPC voice set (WLF
// mltm*/mltf* or Seraphite scrm*/scrf*, the look the body wears on THIS screen - eb_voice's rule), real TLOU2 enemy takes picked by tools/features/vox_lines.py
// (BEG_INT, kinds 8 / 9 of vox_beg.h; records t2r-vox-beg-<voice>-8/9-NN of the mod's vox module, played through `say` like the execution pleas):
//   kind 8 the interrogator's demand: "Where?!" / "Where are they?!" / WLF "C'mon, talk to me!" / Seraphite "Answer me!" ...
//   kind 9 the held victim: WLF "Please... oh god... please." / "Don't... please... don't..." / "Please don't!", Seraphite "No... no..." / "Don't... just... wait..." /
//          "Damned Wolf..." / "You'll regret this..." (the meatshield-grabbed-scared takes: a man held, begging, not telling).
// Timeline (psync clock, ms of the move; slam efforts: interrogator heavy grunt 2150 + 4450 ms, victim heavy take 2200 + 4620 ms (fists); a line waits for the previous one to end + 100 ms; the kill frame cuts the victim). Design: fists = "Where are they?!"
// (kind 8, calm-threatening) 0.5 s -> the victim's first answer (kind 9) 1.25 s -> the slam: "WHERE?!" (kind 10, aggressive) 1.95 s cutting the answer off ->
// the desperate answer (kind 11, or kind 12 "Damned Wolf..." for a Seraphite victim of a WLF interrogator) 2.35 s -> a pause -> the kill at 4.6 s. Shiv = one calm
// demand (kind 13 WLF "Talk to me."; Seraphite has none yet: kind 8) 0.35 s + one desperate answer (kind 11 / 12) 0.95 s, then the throat cut. Beats from the clips' jaw /
// head motion (see the beat table in the commit message). Variants are random per interrogation (seed), never the speaker's previous take. The takes are verbal:
// only those with a stream in the active voice language are used (eb_avail), as the execution pleas. If the engine's dialogue path refuses a line on a
// paired body (exec_beg's finding, g_eb_events) the same moments play the voice's effort events instead (a grunt of effort for the interrogator,
// "begs for life" for the victim). t2r-no-int-vox.txt next to the exe = off. Log: 'int vox:'.
#define IVX_MAX 12
typedef struct {
    double at;
    uint8_t who, kind;
    int rec;
} IvxStep;
static struct {
    double t0;
    int on, step, nstep, fac[2], g[2], v[2], ok[2], last[2];
    IvxStep st[IVX_MAX];
    double busy[2], cut_at[2];
    double kill_ms;
    uint64_t ent[2];
    int spoke[2];
    uint32_t seed;
    int fuck, nused, refused;
    uint32_t used[8];
    double shift;
    int cap_ms, shiv;
    double vline[2];
} g_ivx;                        // shift: ms of move time the slowed hold (intwarp.c) has taken off g_ex.t0 - real ms of the move = (psync_ms() - g_ex.t0) + shift
// the PCM clips of the takes `say` cannot do (tail of a take / take with a prefix / trimmed take; int_vox_gen.py -> vox_beg_int.pcm, 24 kHz mono)
__asm__
    (".section .rdata,\"dr\"\n.balign 4\n.globl ivx_pcm_blob\nivx_pcm_blob:\n.incbin \"vox_beg_int.pcm\"\n.globl ivx_pcm_blob_end\nivx_pcm_blob_end:\n.text\n");
extern const int16_t ivx_pcm_blob[];
#include "vox_beg_keys.h"
// PROBE (t2r-ivx-probe.txt next to the exe): can an interrogation line be played by the ENGINE (3D, room, occlusion) instead of our mixer? Each PCM line is
// replaced (PCM muted) by ONE engine attempt, the route rotating a / b / c / d per line so the ear tells them apart and 'int probe:' says which line used which:
//   a play-sound-at-joint(fnv64(stream key), body, head)          b play-sound-at-joint(record sid, body, head)
//   c say(body, record, 0 / 3, 0)   (other flags than the normal 2)   d natives that might play a speech stream directly (names tried, every answer logged)
// Success is judged 200 ms later: is-speaking?(body) for c, and for a / b / d the sound handle polled with candidate natives (sound-playing? ...) - the log lists every answer.
static int IvxProbeOn(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-ivx-probe.txt", &at, &off, 120);
}

// NATIVE-FIRST (t2r-ivx-native.txt): the game plays its own line (`say`: 3D, room, occlusion); 140 ms later is-speaking?(body) - a refusal (NOT a missing bank - the voice gate answers character_busy_speaking while the body's slam effort still sounds, so the line is stop-vox'd first; the old guess: the
// logs show every female voice and the male c / d voices refused as victims, male a / b accepted) falls back to the PCM clip at once ('int vox: NATIVE refused').
static struct {
    int rec, w, tries;
    double lim;
    uint64_t mid_at;
    int mid_w, mid_rec;
} g_ivn = { -1, 0, 0, 0.0, 0, 0, -1 };

// Logs (12 interrogations): the victim voice sets Seraphite male c, Seraphite female c (fist) and WLF male d were refused by `say` on EVERY line (16 of 16), voice a never;
// each refusal cost 140 ms (try 1) + 140 ms (the priority-26 retry, accepted 0 times in 24 refusals over six logs) before the PCM clip, so the reply started ~280 ms late and the first
// reply ran under the slam. Now: one try; a voice set refused once as the victim of a fist (or of a shiv) move goes straight to its PCM clip at the planned moment for the rest of the session.
static struct {
    uint64_t at;
    uint64_t h, ent;
    int route, rec;
} g_ivp;
static void IvxProbeLine(int w, int rec, double ems)
{
    static int n;
    int route = n++ & 3;
    uint64_t ent = g_ivx.ent[w], r = ~0ULL, ja = FNV64("headb");
    const char *RN = "abcd";
    if (route == 0 || route == 1) {
        uint64_t a[5] =
            { route ? VXG_LINES[rec] : VXG_KEY[rec], ent, ja, 0, 0 };
        r = DcCall(FNV64("play-sound-at-joint"), 5, a);
    } else if (route == 2) {
        uint64_t a[4] = { ent, VXG_LINES[rec], (n & 4) ? 3 : 0, 0 };
        r = DcCall(FNV64("say"), 4, a);
    } else {
        static const char *const NM[] =
            { "play-dialog-line", "play-dialogue-line", "play-speech",
"play-speech-line", "play-voice-line", "play-vo", "play-stream", "play-dialog",
"speak-line", "play-dialogue" };
        for (int i = 0; i < 10; i++) {
            uint64_t a[3] = { ent, VXG_KEY[rec], 0 }, rr =
                DcCall(FNV64(NM[i]), 3, a);
            Xlog("int probe: route d native %s(body, stream key) -> %llx%s", NM[i], (unsigned long long) rr, rr == ~0ULL ? " (not registered / refused)" : "");
            if (rr != ~0ULL && r == ~0ULL)
                r = rr;
        }
    }
    Xlog("int probe: +%.0f ms %s slot rec %d (record %llx, stream %llx, %d ms): ROUTE %c -> %llx (PCM muted)", ems, w ? "victim" : "interrogator", rec, (unsigned long long) VXG_LINES[rec], (unsigned long long) VXG_KEY[rec], (int) VXG_DUR[rec], RN[route], (unsigned long long) r);
    g_ivp.at = GetTickCount64() + 200;
    g_ivp.h = r;
    g_ivp.ent = ent;
    g_ivp.route = route;
    g_ivp.rec = rec;
}

static void IvxProbePoll(void)
{
    if (!g_ivp.at || GetTickCount64() < g_ivp.at)
        return;
    g_ivp.at = 0;
    if (g_ivp.route == 2 || g_ivp.route == 3) {
        uint64_t a[1] = { g_ivp.ent }, r =
            DcCall(FNV64("is-speaking?"), 1, a);
        Xlog("int probe: route %c is-speaking?(body) 200 ms later -> %llx",
             "abcd"[g_ivp.route], (unsigned long long) r);
    }
    if (g_ivp.h && g_ivp.h != ~0ULL) {
        static const char *const PN[] =
            { "sound-playing?", "is-sound-playing?", "sound-is-playing?",
"sound-handle-playing?", "sound-valid?", "get-sound-playing?" };
        for (int i = 0; i < 6; i++) {
            uint64_t h = g_ivp.h, r = DcCall(FNV64(PN[i]), 1, &h);
            if (r != ~0ULL)
                Xlog("int probe: route %c handle %llx %s -> %llx",
                     "abcd"[g_ivp.route], (unsigned long long) g_ivp.h,
                     PN[i], (unsigned long long) r);
        }
    }
}

static int IvxOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-int-vox.txt", &at, &off, 120);
}

static int IvxVoice(uint64_t ent, uint32_t slot, int *fac, int *g, int *v)
{
    int look, fem;
    if (FactionLooksOff())
        return 0;
    if (ent == SID_PLAYER) {
        *fac = VxOwnFac();
        look = g_fl.cur;
        fem = OwnFemale();
    } else {
        if (slot >= T2R_MAX_PEERS)
            return 0;
        int team;
        const char *why;
        look = FlCoreShown(&g_fl, slot, &team);
        *fac = VxFacOfSlot(slot, &why);
        fem = SlotFemale(slot);
    }
    if ((*fac != 0 && *fac != 1) || look < 0)
        return 0;
    *g = fem ? 1 : 0;
    *v = VxVoiceOfLook(*fac, look);
    return 1;
}

// a take of slot `kind` (vox_beg.h: 8 fist demand 1, 9 victim first, 10 fist demand 2, 11 victim second, 12 victim plea cut by the blow, 13 shiv demand, 14 shiv answer,
// 15 line before the kill swing, 16 line on the blow) of that body's voice set with a stream in the voice language, never one take twice in an interrogation, "Fuck you!"
// only once; -1 = none
// the answers were always the plain ones instead of the varied ones: the victim's replies of the match so far (kinds 9 / 11 / 14), by TEXT (VXG_TXT: one value for
// every voice of a line) - a line said in one of the last IVH_N interrogations is not picked again while the pool has another. Every screen runs every interrogation's timeline with the
// shared psync seed, so the history (and the picks) are the same on every screen. The old cause of "Coward!" / "Savage!" + "She'll protect me...": the first reply was capped to 900 ms and
// only those two Seraphite takes are that short, the Seraphite second-reply pool was three prayer lines (BEG_INT / int_vox_gen.py SLOTS now 13-18 lines per beat and faction).
#define IVH_N 6
static struct {
    uint32_t h[3][IVH_N];
    int n[3];
} g_ivh;
static int IvhSlot(int kind)
{
    return kind == 9 ? 0 : kind == 11 ? 1 : kind == 14 ? 2 : -1;
}

static int IvhHas(int hk, uint32_t t)
{
    int m = g_ivh.n[hk] < IVH_N ? g_ivh.n[hk] : IVH_N;
    for (int i = 0; i < m; i++)
        if (g_ivh.h[hk][i] == t)
            return 1;
    return 0;
}

static int IvxPick(int w, int kind, uint32_t salt)
{
    if (kind == 16)
        return -1;              // "You fuckin' bitch!" on the blow sounds like an insult, not an attack - the slot is cut (the head smash keeps its heavy grunt)
    const uint8_t *av = EbAvail();
    int cand[32], nc = 0, hk = IvhSlot(kind), nhist = 0, ncap = 0;
    int n =
        VXG_RANGE[g_ivx.fac[w]][g_ivx.g[w]][g_ivx.v[w]][kind][1], first =
        VXG_RANGE[g_ivx.fac[w]][g_ivx.g[w]][g_ivx.v[w]][kind][0];
    for (int pass = 0; pass < 2 && !nc; pass++) {       // pass 0 skips the lines of the match history, pass 1 (pool exhausted) allows them
        for (int i = 0; i < n && nc < 32; i++) {
            int r = first + i, dup = 0;
            if (av && !av[r] && !VXG_PCM[r].n)
                continue;       // (a PCM clip is the English stream: no voice-language stream needed)
            if (g_ivx.cap_ms && (int) VXG_DUR[r] > g_ivx.cap_ms) {
                if (!pass)
                    ncap++;
                continue;
            }
            if ((VXG_FLAG[r] & 1) && g_ivx.fuck)
                continue;
            for (int u = 0; u < g_ivx.nused; u++)
                if (g_ivx.used[u] == VXG_SRC[r])
                    dup = 1;
            if (!pass && hk >= 0 && IvhHas(hk, VXG_TXT[r])) {
                nhist++;
                continue;
            }
            if (!dup)
                cand[nc++] = r;
        }
        if (hk < 0)
            break;
    }
    if (!nc) {
        static int nl0;
        if (LOGCAP(nl0, 40))
            Xlog("int vox: pick kind %d %s: pool %d, none left (%d over the %d ms cap)", kind, w ? "victim" : "interrogator", n, ncap, g_ivx.cap_ms);
        return -1;
    }
    uint32_t hs =
        (g_ivx.
         seed ^ (uint32_t) (kind * 0x9e3779b1u) ^ (salt * 0x85ebca6bu)) *
        2654435761u;
    hs ^= hs >> 15;
    hs *= 2246822519u;
    hs ^= hs >> 13;
    int ci = (int) (hs % (uint32_t) nc);
    if (nc > 1 && cand[ci] == g_ivx.last[w])
        ci = (ci + 1) % nc;
    int r = cand[ci];
    if (VXG_FLAG[r] & 1)
        g_ivx.fuck = 1;
    if (g_ivx.nused < 8)
        g_ivx.used[g_ivx.nused++] = VXG_SRC[r];
    if (hk >= 0) {
        g_ivh.h[hk][g_ivh.n[hk] % IVH_N] = VXG_TXT[r];
        g_ivh.n[hk]++;
    }
    static int nl;
    if (LOGCAP(nl, 120))
        Xlog("int vox: pick kind %d %s (%s %s voice %c): pool %d, %d over the %d ms cap, %d in the match history, %d candidates -> #%d \"%s\" (rec %llx, %d ms)", kind, w ? "victim" : "interrogator", g_ivx.fac[w] ? "WLF" : "Seraphite", g_ivx.g[w] ? "female" : "male", 'a' + g_ivx.v[w], n, ncap, g_ivx.cap_ms, nhist, nc, ci, VXG_STR[r], (unsigned long long) VXG_LINES[r], (int) VXG_DUR[r]);
    return r;
}

// take back a pick (its match-history / used / "Fuck you!" marks), so the beat can be picked again under a tighter cap; the history entry must be the newest of its kind
static void IvxUnpick(int r)
{
    if (r < 0)
        return;
    for (int u = g_ivx.nused - 1; u >= 0; u--)
        if (g_ivx.used[u] == VXG_SRC[r]) {
            memmove(&g_ivx.used[u], &g_ivx.used[u + 1],
                    sizeof g_ivx.used[0] * (size_t) (g_ivx.nused - 1 - u));
            g_ivx.nused--;
            break;
        }
    if (VXG_FLAG[r] & 1)
        g_ivx.fuck = 0;
    for (int hk = 0; hk < 3; hk++)
        if (g_ivh.n[hk] > 0
            && g_ivh.h[hk][(g_ivh.n[hk] - 1) % IVH_N] == VXG_TXT[r]) {
            g_ivh.n[hk]--;
            break;
        }
}

static void IvxRepick(int r)
{                               // put a taken-back pick back
    if (r < 0)
        return;
    if (VXG_FLAG[r] & 1)
        g_ivx.fuck = 1;
    if (g_ivx.nused < 8)
        g_ivx.used[g_ivx.nused++] = VXG_SRC[r];
    int hk = IvhSlot(9);
    g_ivh.h[hk][g_ivh.n[hk] % IVH_N] = VXG_TXT[r];
    g_ivh.n[hk]++;
}

#define IVX_WET_N (24000 * 4)
static int16_t g_ivxWet[6][IVX_WET_N]; // the lines with their room tail
static int16_t g_ivxBuf[3][24000 * 5]; // a PCM line cut short (the slam / the kill): a faded copy of its start
extern const int16_t ivx_pcm_blob_end[];
static int IvxPickFit(int w, int kind, uint32_t salt, int cap_ms)
{                               // a take of at most cap_ms or -1 (the line is dropped, never squeezed)
    if (cap_ms < 300)
        return -1;
    g_ivx.cap_ms = cap_ms;
    int r = IvxPick(w, kind, salt);
    g_ivx.cap_ms = 0;
    return r;
}

static int IvxPickCapped(int w, int kind, uint32_t salt, int cap_ms)
{                               // a take of at most cap_ms if the voice set has one, else any
    g_ivx.cap_ms = cap_ms;
    int r = IvxPick(w, kind, salt);
    g_ivx.cap_ms = 0;
    return r >= 0 ? r : IvxPick(w, kind, salt);
}

// the PCM lines still playing (hold released / the pair cut): faded out in 40 ms
static void IvxPcmStop(void)
{
    if (g_mixState != 1)
        return;
    EnterCriticalSection(&g_mixCs);
    for (int v = 0; v < MIX_VOICES; v++) {
        const int16_t *s = g_mixv[v].src;
        if (g_mixv[v].on
            && ((s >= ivx_pcm_blob && s < ivx_pcm_blob_end)
                || (s >= &g_ivxBuf[0][0] && s < &g_ivxBuf[3][0])
                || (s >= &g_ivxWet[0][0] && s < &g_ivxWet[6][0]))) {
            g_mixv[v].fadeN = g_mixv[v].fadeLeft = 1920;
        }
    }
    LeaveCriticalSection(&g_mixCs);
}

__attribute__((unused))
static int IvxPcmLine(int w, int rec, double ems, double limit_ms)
{
    const int16_t *src = ivx_pcm_blob + VXG_PCM[rec].off;
    int n = (int) VXG_PCM[rec].n, rate = VXG_PCM[rec].rate;
    if (limit_ms > 0.0 && limit_ms < (double) n * 1000.0 / rate) {
        static int rr;
        int16_t *b = g_ivxBuf[rr++ % 3];
        n = (int) (limit_ms * rate / 1000.0);
        if (n < 400)
            return 0;
        if (n > 24000 * 5)
            n = 24000 * 5;
        memcpy(b, src, (size_t) n * sizeof(int16_t));
        int f = rate * 40 / 1000;
        if (f > n)
            f = n;
        for (int i = 0; i < f; i++)
            b[n - f + i] =
                (int16_t) ((float) b[n - f + i] * (float) (f - i) /
                           (float) f);
        src = b;
    }
    uint64_t ent = g_ivx.ent[w];
    (void) ems;
    return PcmPlayAt(src, n, 0.9f, rate, ent);
}

// The engine's own answer for a native line (VoxController::PlayVoxInternalNoLock, the gate every `say` passes; the `say` native itself answers 0 whether the line started or was refused): codes 0 played, 1 queued,
// 2 character_busy_speaking (the request's priority is not above the line the speaker is saying NOW), 10 character_shut_up, 14 failed_die_roll, 20 queue_reject_same, 21 cannot_interrupt, 22 speech_is_suppressed,
// 26 team-allies_vox_not_audible_by_non-allied_player. Needs the dev hook (t2r-vox-probe.txt next to the exe, deathreact.c pvox_probe_init); without it only the 140 ms is-speaking? check below decides.
static void IvxGateArm(void)
{
#ifndef T2R_RELEASE
    g_pvoxForce = 1;           // (on in every dev build for the interrogation lines - the probe's file switch stays for the other callers)
#endif
    PvoxProbeInit();
    g_pvoxRes = 0xffffu;
    g_pvoxArm = g_pvoxTramp != NULL;  // (release: only with t2r-vox-probe.txt next to the exe; the old #ifndef hid every refusal reason)
}

static const char *IvxGateTxt(void);
// VoxPlayResult names (exe 1.6.10721; tools/sp-sandbox/t2r-sp.c VOX_RES)
static const char *const IVX_RES[35] =
    { "played", "queued", "character_busy_speaking",
"waiting_for_priority_delay", "vox_system_not_initialized", "disabled",
    "invalid_character_id", "vox_not_found", "player_dead",
        "remap_produced_nil_vox-id", "character_shut_up",
        "character_shut_up_murmuration", "no_lines",
    "no_lines_for_this_character", "failed_die_roll",
        "invalid_vox_priority", "missing_metadata",
        "sound_process_failed_to_spawn", "sound_failed_to_play",
    "character_is_dead", "queue_reject_same", "cannot_interrupt",
        "speech_is_suppressed", "idle_speech_is_suppressed_in_this_task",
    "murmuration_is_suppressed_in_this_task", "murmuration_disallowed",
        "team-allies_vox_not_audible_by_non-allied_player",
        "suppressed_by_active_headset_speech",
    "speaker_is_underwater", "temp_VO_not_played",
        "MISSING_XVAG_SOUND_KEY",
        "ignored;_broadcast_mode_default_but_not_SP/MP_host",
        "missing_radio_target",
    "interrupted_by_another_vox", "stopped"
};

static const char *IvxResName(uint32_t r)
{
    return r < 35 ? IVX_RES[r] : r == 0xffffu ? "no answer" : "unknown";
}

// the engine's answer for this line: the armed result of the call, else the probe ring by the request's line key (record id or speech key) since `since`
static int IvxGateOf(int rec, uint64_t since, uint32_t *res,
                       uint32_t *prio)
{
    if (!g_pvoxTramp)
        return 0;
    return PvoxResultOf(VXG_LINES[rec], since, res, prio)
        || PvoxResultOf(VXG_KEY[rec], since, res, prio);
}

// Is the line's vox-info record in the LOADED data, and is it the take this ext means? (a data build older than the ext's vox_beg.h has fewer
// records - one data stage has 913 of the 1349 - or the same id on another take.) lookup-pointer / lookup-symbol of the record id; the record
// carries its speech key twice at +0x70 / +0x78 (dc_data_patches.patch_vox_lines). Cached per record. 'in data: yes (speech key ok)' / 'NO' / 'unknown'.
static const char *IvxRecInData(int rec)
{
    static int16_t st[2048];    // 0 not asked, 1 yes + key ok, 2 yes + other key, 3 no, 4 no lookup native
    static int lk = -1;         // which lookup native answers: 0 lookup-pointer, 1 lookup-symbol, 2 none
    if (rec < 0 || rec >= 2048)
        return "unknown";
    // OFF in every build. lookup-pointer is not a native here, so this fell to lookup-symbol, and a record id missing from the loaded data
    // makes the engine's LookupSymbolValue assert (int3 at exe+d50fda, rbp -> "LookupSymbolValue: Symbol ...") - both clients died on the first
    // interrogation line. Never look a vox record up by symbol at runtime.
    (void) lk;
    (void) st;
    return "unknown";
    if (!st[rec]) {
        uint64_t sid = VXG_LINES[rec], p = ~0ULL;
        static const char *const LN[2] =
            { "lookup-pointer", "lookup-symbol" };
        for (int k = lk < 0 ? 0 : lk; k < 2 && (lk < 0 || k == lk); k++) {
            uint64_t a[1] = { sid };
            p = DcCall(Fnv64Rt(LN[k]), 1, a);
            if (p != ~0ULL) {
                if (lk < 0) {
                    lk = k;
                    Xlog("int vox: record check by %s", LN[k]);
                }
                break;
            }
        }
        if (p == ~0ULL) {
            if (lk < 0) {
                lk = 2;
                Xlog("int vox: record check - neither lookup-pointer nor lookup-symbol is a native, records not checked");
            }
            st[rec] = 4;
        } else if (!p || p < 0x10000
                   || IsBadReadPtr((void *) (uintptr_t) p, 0x80))
            st[rec] = 3;
        else
            st[rec] = (*(uint64_t *) (uintptr_t) (p + 0x70) == VXG_KEY[rec]
                       || *(uint64_t *) (uintptr_t) (p + 0x78) ==
                       VXG_KEY[rec]) ? 1 : 2;
    }
    return st[rec] == 1 ? "yes (speech key ok)" : st[rec] ==
        2 ? "yes, OTHER speech key (data build out of step with the ext)" :
        st[rec] == 3 ? "NO (not in the loaded data)" : "unknown";
}

// one engine line through the character (`say` on the body: the engine's 3D voice, lip sync, subtitle). The engine's own answer is logged; a busy / cannot-
// interrupt / priority-delay answer (the speaker still has a line or effort going) is retried at once after stop-vox at priority 26 (SP's own for efforts).
static uint64_t g_ivxSayT;
static uint64_t IvxNativeSay(int w, int rec, int prio, const char *tag)
{
    uint64_t ent = g_ivx.ent[w], sa[1] = { ent }, pre =
        DcCall(FNV64("is-speaking?"), 1, sa);
    int busy = pre != ~0ULL && (pre & 0xff);
    // Both Seraphite-male-a victim lines were refused on both screens (say -> 0, is-speaking? 0 at 140 ms, again after the retry) with the body 'silent before'. The record's
    // priority is the cloned team-call template's low one (deathreact.c bd_scream: the gate answers character_busy_speaking against whatever the speaker is saying NOW), and is-speaking? does not
    // see the engine's own effort events (grab / fight-take grunts of the paired clip), so 'silent' proved nothing and the stop below never ran. The victim - and every retry - is cut unconditionally first,
    // as the burn scream and the headshot take do; the stop is logged.
    if (busy || w == 1 || prio >= 26)
        VoxStop(ent);
    uint64_t a[4] = { ent, VXG_LINES[rec], (uint64_t) prio, 0 };
    g_ivxSayT = GetTickCount64();
    IvxGateArm();
    uint64_t r = DcCall(FNV64("say"), 4, a);
    const char *gt = IvxGateTxt();
    uint32_t res = 0xffffu, pr = 0;
    int known = IvxGateOf(rec, g_ivxSayT, &res, &pr);
    Xlog("int vox: %s %s (%s %s voice %c) rec %llx \"%s\": body %s before, say(prio %d) -> %llx; engine: %s%s; record in data: %s", tag, w ? "victim" : "interrogator", g_ivx.fac[w] ? "WLF" : "Seraphite", g_ivx.g[w] ? "female" : "male", 'a' + g_ivx.v[w], (unsigned long long) VXG_LINES[rec], VXG_STR[rec], busy ? "SPEAKING (stopped)" : (w == 1 || prio >= 26) ? "silent to is-speaking? (stopped anyway)" : "silent", prio, (unsigned long long) r, known ? IvxResName(res) : "answer not yet in (async)", gt, IvxRecInData(rec));
    if (ent == SID_PLAYER) {
        static int nh;
        if (LOGCAP(nh, 60))
            Xlog("int vox: (the speaker is our body: hp %.1f - a body at 0 hp answers character_is_dead / player_dead)", (double) LocalHp());
    }
    if (known && (res == 2 || res == 3 || res == 21 || res == 33)
        && prio < 26)
        return IvxNativeSay(w, rec, 26, "NATIVE retry (busy)");
    return r;
}

static const char *IvxGateTxt(void)
{
    static char b[96];
    b[0] = 0;
    if (g_pvoxArm) {
        g_pvoxArm = 0;
        snprintf(b, sizeof b, " [engine gate: %s %u at priority %u]",
                 g_pvoxRes == 0xffffu ? "no answer" : "result",
                 (unsigned) g_pvoxRes, (unsigned) g_pvox_prio);
    }
    return b;
}

// speak step s: rec (-1 = pick now); a line a body cannot speak (no take in this voice set / language, dialogue refused earlier) becomes the effort event of the moment
static void IvxSay(int w, int kind, int step, double ems, int rec)
{
    if (rec < 0 && g_ivx.ok[w])
        rec = IvxPick(w, kind, (uint32_t) step * 7u + 3u);
    if (!g_ivx.ok[w])
        rec = -1;
    uint64_t ent = g_ivx.ent[w];
    if (!ent || ent == ~0ULL)
        return;
    uint64_t r = ~0ULL;
    static int nl;
    if (rec < 0 && (kind == 12 || kind == 15 || kind == 16))
        return;                 // optional slots: nothing to say in this voice set
    if (rec >= 0 && !VXG_PCM[rec].n && g_ivx.refused)
        rec = -1;               // dialogue refused on a paired body (learned THIS interrogation only): the effort events (a PCM line does not use the dialogue system)
    if (rec < 0) {              // no take for this voice set / language: the effort event of the same moment
        static const char *const EV[2][2] =
            { { "vox-eff-gen-fight-give-lrg-short",
               "vox-eff-gen-fight-give-hvy-short" },
            { "vox-eff-gen-begs-for-life-stun",
             "vox-eff-gen-fight-take-hvy" } };
        for (int i = 0; i < 2 && r == ~0ULL; i++) {
            uint64_t a[4] = { ent, Fnv64Rt(EV[w][i]), 2, 0 };
            uint64_t fs = FemEffortSid(ent, w ? VXE_PAIN : VXE_GIVE);
            if (fs)
                a[1] = fs;
            r = DcCall(FNV64("say"), 4, a);
        }
        if (LOGCAP(nl, 60))
            Xlog("int vox: +%.0f ms %s slot %d: no take for this voice set / language / dialogue refused -> effort event %llx", ems, w ? "victim" : "interrogator", kind, (unsigned long long) r);
    } else {
        double dur =
            (double) VXG_DUR[rec] > 0 ? (double) VXG_DUR[rec] : 900.0;
        double lim = w
            && g_ivx.cut_at[1] > ems ? g_ivx.cut_at[1] - ems : 0.0;
        if (VXG_PCM[rec].n && IvxProbeOn()) {
            IvxProbeLine(w, rec, ems);
            r = 0;
        } else {                // every interrogation line plays through the character with its subtitle, like every other vox - `say` only, no PCM clip
            (void) lim;
            r = IvxNativeSay(w, rec, 2, "NATIVE");
            XfaceTalk(ent, dur);       // the emotion pose drops to a low weight while this body speaks (xface_set): the speech owns the jaw
            Xlog("int lip: +%.0f ms %s speaker %s (ent %llx, slot-voice %s %s %c) line \"%s\" stream %llx %.0f ms: lip sync = the engine's speech facial layer driven by this record's stream on that body; emotion weight %.2f while it speaks; xface %s", ems, w ? "victim" : "interrogator", ent == SID_PLAYER ? "our body" : "puppet", (unsigned long long) ent, g_ivx.fac[w] ? "WLF" : "Seraphite", g_ivx.g[w] ? "female" : "male", 'a' + g_ivx.v[w], VXG_STR[rec], (unsigned long long) VXG_KEY[rec], dur, (double) XF_TALK_W, g_xface.on ? "on" : "OFF (no emotion pose to lower)");
            g_ivx.spoke[w] = 1;
            g_ivn.rec = rec;
            g_ivn.w = w;
            g_ivn.lim = lim;
            g_ivn.tries = 1;
            g_ivn.mid_at = g_ivxSayT + (uint64_t) (dur * 0.5);
            g_ivn.mid_w = w;
            g_ivn.mid_rec = rec;
            if (w) {
                g_ivx.vline[0] = ems;
                g_ivx.vline[1] = ems + dur;
            }                   // (the foley under a victim line is ducked: intfoley_vic_line)
        }
        g_ivx.last[w] = rec;
        if (g_ivx.busy[w] < ems + dur)
            g_ivx.busy[w] = ems + dur;
        if (LOGCAP(nl, 60))
            Xlog("int vox: +%.0f ms %s slot %d (%s voice %c%s): %s %llx (%.0f ms) -> %llx", ems, w ? "victim" : "interrogator", kind, g_ivx.fac[w] ? "WLF" : "Seraphite", 'a' + g_ivx.v[w], g_ivx.g[w] ? " f" : "", "record", (unsigned long long) VXG_LINES[rec], dur, (unsigned long long) r);
    }
    g_ivx.busy[w] += 100.0;
}

// the slam efforts: the interrogator's heavy exertion on each head slam (his own NPC voice set's fight-give events, as the SP melee finishers) and the
// victim's heavy take on the impact (kind 20 = interrogator, 21 = victim); no dialogue line: the engine's effort events through `say`
static void IvxEff(int w, int step, double ems)
{
    uint64_t ent = g_ivx.ent[w];
    if (!ent || ent == ~0ULL)
        return;
    static const char *const EV[2][3] =
        { { "vox-eff-gen-fight-give-hvy",
           "vox-eff-gen-fight-give-hvy-short",
           "vox-eff-gen-fight-give-lrg-short" },
    { "vox-eff-gen-fight-take-hvy", "vox-eff-gen-fight-take-lrg",
     "vox-eff-gen-fight-take-med" }
    };
    uint64_t r = ~0ULL;
    for (int i = 0; i < 3 && r == ~0ULL; i++) {
        uint64_t a[4] = { ent, Fnv64Rt(EV[w][i]), 2, 0 };
        uint64_t fs = i == 0 ? FemEffortSid(ent, w ? VXE_PAIN : VXE_GIVE) : 0;        // a female body: her own take of the effort
        if (fs)
            a[1] = fs;
        r = DcCall(FNV64("say"), 4, a);
    }
    static int nl;
    if (LOGCAP(nl, 40))
        Xlog("int vox: +%.0f ms slam effort of the %s (step %d) -> %llx",
             ems, w ? "victim" : "interrogator", step,
             (unsigned long long) r);
}

// the head slams of the fist interrogation (kind 22 steps: 2267 ms into the ground, 5000 ms the final smash; the same move clock on every screen): TLOU2's own SP slam events at the
// victim's head (sfx-unarmed-finisher-headslam-skin = the unarmed finisher's head slam, sfx-wall-impact-sweetener; banks wpn-melee-unarmed / gbl-fight are in the MP level), for the first
// slam on top of the 2013 impact wave that ex_play_label plays on the exec timeline (the impact sound was missing - that wave alone was inaudible under the hold's
// layers), for the final smash the 2013 wave too; and SP's head-smash blood (bloodbuild.c bb_ig_slam: gibs / gore / drops / spray at headb + the floor pool decal)
static void IvxSlam(int second, double ems)
{
    uint64_t ent = g_ivx.ent[1];
    if (!ent || ent == ~0ULL)
        return;
    // The slam SOUND is the PS4 composite played by the timeline row of the clip's contact frame (t2r-ext.c ex_play_label: 2267 / 4967 ms); the SP events and the 2013 impact wave that
    // used to be started here are gone (replaced, not doubled). Only the blood remains. t2r-no-int-slam-ps4.txt (old sounds) re-enables them.
    uint64_t r[2] = { 0, 0 };
    const char *const EV[2] =
        { "sfx-unarmed-finisher-headslam-skin",
"sfx-wall-impact-sweetener" };
    int pcm = 0;
    if (GetFileAttributesA("t2r-no-int-slam-ps4.txt") !=
        INVALID_FILE_ATTRIBUTES) {
        for (int i = 0; i < 2; i++) {
            uint64_t a[5] = { Fnv64Rt(EV[i]), ent, FNV64("headb"), 0, 0 };
            r[i] = DcCall(FNV64("play-sound-at-joint"), 5, a);
        }
        if (second && g_ex.pair >= 0 && (size_t) g_ex.pair < MX_N) {
            g_ivxSmashLayer = 1;
            pcm =
                ExPlayLabel(EXS_IMPACT_HEAD, 1.0f,
                              MELEE2013[g_ex.pair].name, 1);
            g_ivxSmashLayer = 0;
        }
    }
    int nb = BbIgSlam(ent, second);
    Xlog("int slam: +%.0f ms %s: SP %s -> %llx, %s -> %llx%s; blood %d effects on the victim", ems, second ? "final smash" : "head slam", EV[0], (unsigned long long) r[0], EV[1], (unsigned long long) r[1], second ? (pcm ? ", 2013 impact wave" : ", 2013 impact wave refused") : "", nb);
}

// LOOK-AT (the two characters look at each other; the victim's head AND eyes on the interrogator first). TLOU2's own script natives (DC bytecode, tools/local/dcx/out h.bin / ph.bin):
// npc-look-at-object(npc, target, joint sid, seconds) - e.g. (tommy, player, headb, 10.0) - the engine's head + eye look-at with its own blend, npc-clear-look-at(npc) ends it;
// npc-set-look-aim-mode(npc, 1) is the scripts' companion to the look-aim-at pair. Every body of the pair looks at the other's headb, the victim first, on every screen (the ext runs the timeline
// on all of them; both bodies are either our own body or a puppet). The paired anim keeps the head / eye joints free (the engine look-at is its own additive layer). It is re-asserted every
// 600 ms (the engine drops a look-at when the body changes state) and cleared with the pair. Our OWN body (SID_PLAYER) may not be an npc: the answer of the native is logged per body
// ('int look:') and a short probe of player-side names runs once, so a log says which one answered. t2r-no-int-look.txt next to the exe = off.
static int IvxLookOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-int-look.txt", &at, &off, 120);
}

static struct {
    uint64_t next;
    int n[2];
    int probed;
} g_ivl;
static void IvxLookSet(int w, double ems)
{
    uint64_t ent = g_ivx.ent[w], tgt = g_ivx.ent[1 - w];
    if (!ent || ent == ~0ULL || !tgt || tgt == ~0ULL)
        return;
    float dur = (float) ((g_ivx.kill_ms - ems) / 1000.0) + 1.0f;
    if (dur < 1.0f)
        dur = 1.0f;
    uint64_t a[4] = { ent, tgt, FNV64("headb"), 0 };
    memcpy(&a[3], &dur, 4);
    uint64_t r = DcCall(FNV64("npc-look-at-object"), 4, a), r2 = ~0ULL;
    if (r != ~0ULL && !g_ivl.n[w]) {
        uint64_t m[2] = { ent, 1 };
        r2 = DcCall(FNV64("npc-set-look-aim-mode"), 2, m);
    }
    if (g_ivl.n[w]++ < 2)
        Xlog("int look: %s (%s, ent %llx) -> %s (%s, ent %llx) head 'headb' for %.1f s: npc-look-at-object -> %llx, npc-set-look-aim-mode(1) -> %llx%s", w ? "victim" : "interrogator", ent == SID_PLAYER ? "our body" : "puppet", (unsigned long long) ent, w ? "interrogator" : "victim", tgt == SID_PLAYER ? "our body" : "puppet", (unsigned long long) tgt, (double) dur, (unsigned long long) r, (unsigned long long) r2, r == ~0ULL ? " (native not registered / refused)" : "");
    if (r == ~0ULL && ent == SID_PLAYER && !g_ivl.probed) {     // our own body: which look-at native does the player side have? (answers only; the first that answers is taken next time)
        static const char *const PN[] =
            { "player-look-at-object", "player-look-at",
"player-set-look-at", "player-look-at-point", "player-set-look-at-object" };
        g_ivl.probed = 1;
        for (int i = 0; i < 5; i++) {
            uint64_t pa[3] = { tgt, FNV64("headb"), 0 };
            memcpy(&pa[2], &dur, 4);
            uint64_t pr = DcCall(FNV64(PN[i]), 3, pa);
            Xlog("int look: probe %s(target, headb, secs) -> %llx%s",
                 PN[i], (unsigned long long) pr,
                 pr == ~0ULL ? " (not a native)" : "");
        }
    }
}

static void IvxLookClear(void)
{
    for (int w = 0; w < 2; w++) {
        uint64_t ent = g_ivx.ent[w];
        if (ent && ent != ~0ULL && g_ivl.n[w]) {
            uint64_t r = DcCall(FNV64("npc-clear-look-at"), 1, &ent);
            Xlog("int look: cleared on the %s -> %llx",
                 w ? "victim" : "interrogator", (unsigned long long) r);
        }
    }
    memset(&g_ivl, 0, sizeof g_ivl);
}

static void IntvoxTick(void)
{
    static int armed_chk;
    static uint64_t chk_at;
    static int chk_w, chk_kind;
    int running = g_ex.kind == 2 && g_ex.pair >= 0
        && (size_t) g_ex.pair < MX_N;
    const char *nm = running ? MELEE2013[g_ex.pair].name : "";
    int fist = !strncmp(nm, "fist-interrogate", 16), shiv =
        !strncmp(nm, "shiv-interrogate", 16);
    if (!fist && !shiv) {       // no interrogation (any more): a cut one (the interrogator went down / the pair aborted) silences its voices
        if (g_ivx.on == 1) {
            for (int w = 0; w < 2; w++)
                if (g_ivx.spoke[w] && g_ivx.ent[w]
                    && g_ivx.ent[w] != ~0ULL)
                    VoxStop(g_ivx.ent[w]);
            IvxPcmStop();
        }
        if (g_ivx.on && g_ivl.next)
            IvxLookClear();   // (the pair ended: the look-at lets go; the engine blends the heads back)      // (hold released / pair cut: the PCM lines fade out too)
        g_ivx.on = 0;
        return;
    }
    if (IvxOff())
        return;
    g_intQuietUntil = GetTickCount64() + 4000;        // (the interrogation's lines are the only voices of the moment: no team kill taunt / callout of ours until 4 s after it)
    IvxProbePoll();
    double ems = PsyncMs() - g_ex.t0 + g_ivx.shift;    // real ms of the move (the fist interrogation's slowed hold moves g_ex.t0 on: intwarp.c)
    if (g_ex.t0 != g_ivx.t0 || !g_ivx.on) {
        if (g_ex.t0 == g_ivx.t0)
            return;             // this run was cut / finished
        memset(&g_ivx, 0, sizeof g_ivx);
        ems = PsyncMs() - g_ex.t0;
        g_ivx.t0 = g_ex.t0;
        g_ivx.on = 1;
        g_ivx.last[0] = g_ivx.last[1] = -1;
        g_ivx.shiv = shiv;
        g_ivx.ent[0] = g_ex.att_ent;
        g_ivx.ent[1] = g_ex.vic_ent;
        g_ivx.kill_ms = (double) MELEE2013[g_ex.pair].kill_s * 1000.0 + (fist ? IW_EXTRA : 0.0);        // (real ms: the fist interrogation's hold is slowed, 4.6 s of clip = 7.45 s)
        uint32_t slot_att, slot_vic;
        if (g_exThird) {
            slot_att = g_thirdAttSlot;
            slot_vic = g_thirdVicSlot;
        } else {
            slot_att = slot_vic = (uint32_t) g_gx.slot;
        }
        g_ivx.ok[0] =
            IvxVoice(g_ivx.ent[0], slot_att, &g_ivx.fac[0], &g_ivx.g[0],
                      &g_ivx.v[0]);
        g_ivx.ok[1] =
            IvxVoice(g_ivx.ent[1], slot_vic, &g_ivx.fac[1], &g_ivx.g[1],
                      &g_ivx.v[1]);
        {                       // The interrogation vox was different on each screen: the seed came from a LOCAL per-client counter (cnt[]) and from the slot each screen used for the victim, so the two clients drew different takes. Now the
            // seed is a function of what psync shares: the move's uid (one value on every screen), the pair and the two slots; no local counter, no rand().
            uint32_t uid = g_ps.uid ? g_ps.uid : (uint32_t) g_ex.t0, sa =
                g_ps.uid ? (uint32_t) g_ps.att : 0, Sv =
                g_ps.uid ? (uint32_t) g_ps.vic : 0;
            g_ivx.seed =
                (uid * 0x2545f491u) ^ ((uint32_t) (g_ex.pair + 1) *
                                       0x9e3779b1u) ^ ((sa +
                                                        1) *
                                                       0x85ebca6bu) ^ ((Sv
                                                                        +
                                                                        7)
                                                                       *
                                                                       0xc2b2ae35u);
            Xlog("int vox: seed %08x (psync uid %08x, pair %d, attacker slot %u, victim slot %u) - identical on every screen", g_ivx.seed, uid, g_ex.pair, sa, Sv);
        }
        // The beats (measured on the 2013 clips, 10 fps decode, jaw / head joints): fist-interrogate-enter-left/right - the interrogator's jaw opens 0.5-1.0 s (first talking beat;
        // the victim's jaw 0.9-1.3 s = his answer), he pulls the victim up (head 0.1 -> 0.5 m, 1.7-2.0 s), jaw opens again 1.9-2.1 s and SLAMS the head into the ground at
        // 2.1-2.2 s (head speed 3.1 m/s; the victim's jaw opens wide 2.2-2.4 s), a held pause 2.6-4.2 s, the head is lifted again 4.3-4.6 s (kill frame 4.6 s) and slammed at
        // 4.9-5.0 s (f150 sfx-head-slam). Shiv moves: one demand, one answer, the throat cut (2.25 / 2.55 s). Slots (int_vox_gen.py SLOTS):
        //   fist: I_FIST1 450 | V_FIRST 1150 (cut at the slam) | I_FIST2 1700 | slam efforts 2150 / 2200 | V_SECOND 2450 | I_PRE ends 150 ms before the swing | V_CUT ends at the kill |
        //         I_BLOW's last word on the head smash at 5000 ms (WLF) / kill grunt 4450 (Seraphite) | victim take on the final slam 4620.   shiv: I_SHIV 350 | V_SHIV 700
        // timeline (REAL ms of the move; the dialogue is spaced out a little): every line is a PCM clip of the speaker's own voice set (the dialogue
        // system refused / never streamed most of these categories on a paired body - the victim's lines were silent, so were half of the interrogator's), each one
        // starts 420 ms after the previous line ENDED (either speaker: no overlaps). The slam efforts (clip 2150 / 2200 ms, before the slowed section) and the kill
        // swing stay at the clips' own speed. fist: I_FIST1 450 | slam efforts 2150 / 2200 | V_FIRST >= 2600 (after the impact) | I_FIST2 | V_SECOND if it fits |
        // I_PRE ends 150 ms before the kill swing (clip 4300 = real 7150) | V_CUT (plea) only if there is room before it | I_BLOW's last word on the head smash (clip 5000 = real 7850) |
        // victim take on the final slam at the kill. shiv: I_SHIV 350 | V_SHIV after it (+ gap).
        int n = 0;
#define IVX_STEP(a, w_, k, r_) do { g_ivx.st[n].at = (a); g_ivx.st[n].who = (w_); g_ivx.st[n].kind = (k); g_ivx.st[n].rec = (r_); n++; } while (0)
#define IVX_D(r_) ((r_) >= 0 ? ((double)VXG_DUR[r_] > 0 ? (double)VXG_DUR[r_] : 900.0) : 600.0)
        if (fist) {
            // Q1 (random: calm opener + question in one breath, or the question alone) | victim answer (after a realistic pause) | SLAM | "Where?!" | victim desperate answer | final smash = the interrogator's HEAVY GRUNT; no threat.
            // The move runs through intwarp.c's two eased slow-downs (kick-mount-question before the slam, the shake after it), so every time below is REAL ms: the clip beats
            // (slam impact 2267, swing 4300, kill 4600, final smash 5000 clip ms) go through iw_real_ms. Lines start after the previous one ended + a gap (no overlaps).
            const double K = g_ivx.kill_ms, impact =
                IwRealMs(2267.0), swing = IwRealMs(4300.0), smash =
                IwRealMs(5000.0), PAUSE = 600.0, GAP = 300.0;
            // (log: "answer 2351-1751 (rec 0)" - the victim's first reply was DROPPED: Q1 ran to 1751 and no take of that victim voice fit the 735 ms left): the reply is picked FIRST (a short take,
            // <= 900 ms if the voice set has one) and Q1 gets what is left, so the first reply always plays; if Q1 still overruns (no short take) the pause shrinks to 250 ms and the reply may run into the slam
            int r9 = g_ivx.ok[1] ? IvxPickCapped(1, 9, 9, 1350) : -1; // (was 900 ms - only "Coward!" / "Savage!" of the Seraphite takes are that short: every Seraphite victim said one of the two)
            double d9 = r9 >= 0 ? IVX_D(r9) : 0.0;
            int q1cap = (int) (impact - 120.0 - PAUSE - d9 - 450.0);
            if (q1cap < 700)
                q1cap = 700;
            int r8 = g_ivx.ok[0] ? IvxPickCapped(0, 8, 8, q1cap) : -1;
            double t8 = 450.0, e8 = t8 + IVX_D(r8);
            double t9 = e8 + PAUSE;     // the realistic pause for an answer
            if (r9 >= 0 && e8 + 250.0 + d9 > impact + 150.0) {
                // (log [21628]: Q1 450-2130, reply 2230-3431 over the slam 3042, the slam efforts 2933 / 2984 on top - some voicelines were missing): Q1 had no take
                // under its cap and ran long, so the reply no longer fits before the slam. The reply is picked again from the takes that END by the slam (+150 ms: the slam may clip its last
                // syllable); none of them: the first pick stays. Same seed and order on every screen, so every screen re-picks the same line.
                int cap = (int) (impact + 150.0 - (e8 + 250.0));
                IvxUnpick(r9);
                int f9 = IvxPickFit(1, 9, 19, cap);
                Xlog("int vox: reply 1 (%.0f ms) does not fit between Q1 (ends %.0f) and the slam (%.0f): re-picked under %d ms -> %s", d9, e8, impact, cap, f9 >= 0 ? VXG_STR[f9] : "none fits, first pick kept");
                if (f9 >= 0) {
                    r9 = f9;
                    d9 = IVX_D(r9);
                } else
                    IvxRepick(r9);
            }
            if (t9 + d9 > impact - 120.0) {
                t9 = e8 + 250.0;
                if (t9 + d9 > impact + 150.0)
                    t9 = impact + 150.0 - d9;
                if (t9 < e8 + 100.0)
                    t9 = e8 + 100.0;
            }
            double e9 = r9 >= 0 ? t9 + d9 : e8;
            int r10 = g_ivx.ok[0] ? IvxPick(0, 10, 10) : -1;
            double t10 = impact + 500.0 > e9 + 300.0 ? impact + 500.0 : e9 + 300.0, e10 = t10 + IVX_D(r10);     // "Where?!" right after the slam; the scared victim answers 0.3 s later
            double t11 = e10 + GAP;
            int r11 = g_ivx.ok[1] ? IvxPickFit(1, 11, 11, (int) (swing + 400.0 - t11)) : -1;  // may run into the lift: the blow cuts it ("Oh god, wait-!")
            IVX_STEP(t8, 0, 8, r8);
            IVX_STEP(IwRealMs(2150.0), 0, 20, -1);
            IVX_STEP(IwRealMs(2200.0), 1, 21, -1);
            IVX_STEP(impact, 1, 22, -1);
            IVX_STEP(smash - 60.0, 0, 20, -1);
            IVX_STEP(smash, 1, 22, -2); // the final smash: the interrogator's heavy grunt (ivx_eff: fight-give-hvy of his own set, always)
            if (r9 >= 0)
                IVX_STEP(t9, 1, 9, r9);
            if (r10 >= 0)
                IVX_STEP(t10, 0, 10, r10);
            if (r11 >= 0)
                IVX_STEP(t11, 1, 11, r11);
            IVX_STEP(K + 20.0, 1, 21, -1);
            Xlog("int vox: fist timeline (REAL ms; clip->real: slam 2267->%.0f, swing 4300->%.0f, kill 4600->%.0f, smash 5000->%.0f): Q1 %.0f-%.0f (rec %llx) | answer %.0f-%.0f (rec %llx) | slam %.0f | Q2 %.0f-%.0f (rec %llx) | answer2 %.0f (rec %llx) | heavy grunt %.0f", impact, swing, K, smash, t8, e8, r8 >= 0 ? (unsigned long long) VXG_LINES[r8] : 0ULL, t9, e9, r9 >= 0 ? (unsigned long long) VXG_LINES[r9] : 0ULL, impact, t10, e10, r10 >= 0 ? (unsigned long long) VXG_LINES[r10] : 0ULL, t11, r11 >= 0 ? (unsigned long long) VXG_LINES[r11] : 0ULL, smash - 60.0);
        } else {
            int r13 =
                g_ivx.ok[0] ? IvxPickCapped(0, 13, 13, 1500) : -1, r14 =
                g_ivx.ok[1] ? IvxPickCapped(1, 14, 14, 1500) : -1;
            double e13 = 350.0 + IVX_D(r13), t14 =
                e13 + 450.0 > 700.0 ? e13 + 450.0 : 700.0;
            IVX_STEP(350.0, 0, 13, r13);
            IVX_STEP(t14, 1, 14, r14);
        }
#undef IVX_D
#undef IVX_STEP
        for (int a = 1; a < n; a++)
            for (int b = a; b > 0 && g_ivx.st[b - 1].at > g_ivx.st[b].at;
                 b--) {
                IvxStep x = g_ivx.st[b];
                g_ivx.st[b] = g_ivx.st[b - 1];
                g_ivx.st[b - 1] = x;
            }
        g_ivx.nstep = n;
        Xlog("int vox: %s on %s screen - interrogator voice %s, victim voice %s, kill at %.0f ms, %d steps", nm, g_exThird ? "a bystander" : g_ex.att_ent == SID_PLAYER ? "the interrogator's" : "the victim's", g_ivx.ok[0] ? "set" : "unknown (effort events)", g_ivx.ok[1] ? "set" : "unknown (effort events)", g_ivx.kill_ms, n);
    }
    if (!IvxLookOff() && ems < g_ivx.kill_ms + 300.0 && GetTickCount64() >= g_ivl.next) {     // look-at: the victim's head + eyes on the interrogator first, then the interrogator on the victim
        g_ivl.next = GetTickCount64() + 600;
        IvxLookSet(1, ems);
        IvxLookSet(0, ems);
    }
    // the next step at its planned moment; a spoken line waits for the same speaker's previous line (two voices may overlap: a plea under a shout)
    while (g_ivx.step < g_ivx.nstep) {
        IvxStep *S = &g_ivx.st[g_ivx.step];
        int w = S->who, kind = S->kind;
        if (ems < S->at)
            break;
        if (kind < 20 && kind != 16 && ems < g_ivx.busy[w] - 1.0)
            break;              // (the line on the blow never waits: it is timed to the smash)
        int s = g_ivx.step++;
        if (kind == 22) {
            if (ems - S->at <= 400.0)
                IvxSlam(S->rec == -2, ems);
            continue;
        }                       // (the head slams: SP impact + blood, rec -2 = the final smash)
        if (kind >= 20) {       // a slam effort: no line, not cut off by the kill
            if (ems - S->at <= 300.0)
                IvxEff(w, s, ems);
            continue;
        }
        int early = kind == 8 || kind == 9 || kind == 10 || kind == 11
            || kind == 13 || kind == 14;
        if (early
            && (S->at + 400.0 > g_ivx.kill_ms
                || ems > g_ivx.kill_ms - 200.0))
            continue;           // too close to the kill
        if (ems - S->at > (kind == 16 ? 300.0 : 600.0))
            continue;           // joined late: the moment is gone
        if (kind == 11 || kind == 14)
            g_ivx.cut_at[1] = g_ivx.kill_ms - 80.0;
        else if (kind == 12)
            g_ivx.cut_at[1] = 0.0;
        IvxSay(w, kind, s, ems, S->rec);
        chk_at = GetTickCount64() + EB_CHECK_MS;
        chk_w = w;
        chk_kind = kind;
        armed_chk = 1;
    }
    if (g_ivx.cut_at[1] > 0.0 && ems >= g_ivx.cut_at[1]) {      // a say line of the victim overrunning its cut: stopped (PCM lines were cut when started)
        if (g_ivx.spoke[1] && g_ivx.ent[1] && g_ivx.ent[1] != ~0ULL)
            VoxStop(g_ivx.ent[1]);
        g_ivx.cut_at[1] = 0.0;
    }
    if (armed_chk && GetTickCount64() >= chk_at && !g_ivx.refused && g_ivx.spoke[chk_w]) {      // 140 ms after a record line: is the body speaking? (exec_beg eb_check)
        armed_chk = 0;
        uint64_t a[1] = { g_ivx.ent[chk_w] };
        uint64_t r = DcCall(FNV64("is-speaking?"), 1, a);
        static int nl;
        if (LOGCAP(nl, 40))
            Xlog("int vox: is-speaking? on the %s -> %llx",
                 chk_w ? "victim" : "interrogator",
                 (unsigned long long) r);
        if (g_ivn.rec >= 0 && g_ivn.w == chk_w) {       // a native line: the engine's answer and the speaking state
            int rc = g_ivn.rec;
            g_ivn.rec = -1;
            uint32_t res = 0xffffu, pr = 0;
            int known = IvxGateOf(rc, g_ivxSayT, &res, &pr);
            int speaking = r != ~0ULL && (r & 0xff);
            Xlog("int vox: %s line rec %llx \"%s\" 140 ms in: is-speaking? %d, engine: %s (priority %u)%s", chk_w ? "victim" : "interrogator", (unsigned long long) VXG_LINES[rc], VXG_STR[rc], speaking, known ? IvxResName(res) : "no answer seen", pr, speaking ? " - PLAYING through the character (subtitle: the engine's)" : "");
            if (!speaking) {
                char dm[1100];
                PvoxDump(g_ivxSayT > 150 ? g_ivxSayT - 150 : 0, dm,
                          sizeof dm);
                Xlog("int vox: %s line rec %llx NOT speaking 140 ms in - vox system since the say (-150 ms): %s", chk_w ? "victim" : "interrogator", (unsigned long long) VXG_LINES[rc], dm);
            }
            if (!speaking && g_ivn.tries == 1) {        // refused: one retry through the character (stop-vox, priority 26), checked again 140 ms later
                g_ivn.tries = 2;
                IvxNativeSay(chk_w, rc, 26, "NATIVE try 2");
                g_ivn.rec = rc;
                chk_at = GetTickCount64() + EB_CHECK_MS;
                armed_chk = 1;
            } else if (!speaking)
                Xlog("int vox: line rec %llx NOT PLAYED after 2 tries - engine: %s; record in data: %s", (unsigned long long) VXG_LINES[rc], known ? IvxResName(res) : "no answer seen", IvxRecInData(rc));
            r = ~0ULL;          // (a native line never switches the interrogation to effort events)
        }
        if (r != ~0ULL && !(r & 0xff)) {
            if (chk_kind == 8 || chk_kind == 13)
                g_ivx.refused = 1;      // (this check once latched the GLOBAL g_eb_events on a false negative of a late line and silenced every later interrogation, the victim screen included - now only the first line of one interrogation can switch ITS voices to effort events)
            Xlog("int vox: the record line is NOT speaking on an interrogation body (dialogue system refused it) - %s, re-speaking now", g_ivx.refused ? "effort events for this interrogation" : "kept (a late line: not learned)");
            if (g_ivx.refused)
                IvxSay(chk_w, chk_kind, 90, ems, -1);
        }
    }
    if (g_ivn.mid_rec >= 0 && GetTickCount64() >= g_ivn.mid_at) {       // half way through the take: still speaking (audible to the end, not cut)?
        uint64_t a[1] = { g_ivx.ent[g_ivn.mid_w] }, r =
            DcCall(FNV64("is-speaking?"), 1, a);
        static int nl;
        if (LOGCAP(nl, 80))
            Xlog("int vox: %s line rec %llx half way: is-speaking? %d%s",
                 g_ivn.mid_w ? "victim" : "interrogator",
                 (unsigned long long) VXG_LINES[g_ivn.mid_rec], r != ~0ULL
                 && (r & 0xff) ? 1 : 0, r != ~0ULL
                 && (r & 0xff) ? "" : " - CUT or never started");
        g_ivn.mid_rec = -1;
    }
    // the kill frame cuts the victim's voice (the death vocals are the engine's)
    if (ems >= g_ivx.kill_ms - 80.0 && g_ivx.on == 1) {
        g_ivx.on = 2;
        if (g_ivx.spoke[1] && g_ivx.ent[1] && g_ivx.ent[1] != ~0ULL)
            VoxStop(g_ivx.ent[1]);     // (the victim only: the interrogator's last line / effort runs on)
    }
}
