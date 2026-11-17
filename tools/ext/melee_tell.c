// melee_tell.c - the readable WINDUP TELL of every melee swing (SP's enemies announce their swing, but here the tell shows faster than in SP so it is
// more challenging and less forgiving; the base of the turn-based melee rhythm: read the tell, then dodge). Included BEFORE melee_dodge.c in t2r-ext.c. Log prefix "tell:".
// SP data (re/sp-melee-1027/session3/report-a.md): an NPC's swing is announced by a -tell / -enter move ~500 ms before the blow, the swing start carries the effort grunt
// (vox-eff-gen-fight-give-med, ~70-90 ms in) and a cloth swish (~40-170 ms in).
// Here:
//  1. CUE: at every melee swing start - ours (melee_dodge.c mdg_swing_note) and a puppet's (mdg_recv_swing for free swings, the exchange's T0 for a melee exchange) - the attacker body says
//     its give-med effort (+80 ms, fem_effort_sid via dm_say, the attacker's own voice) and a swish (+40 ms, the proven-audible PCM whoosh, 3D on that body). Same cue on both screens.
//  2. TELL WINDOW: contact comes TELL_MS (default 320 ms, t2r-tell-ms.txt "<ms>" 200..500) after the swing starts on the shared clock: the swing clip's windup is SPED UP
//     (x tc/tell, max x1.6, for tell ms; the old x0.65 slow-down of the windup - which lengthened it by ~150 ms - is gone while the tell is on), the exchange plans contact at min(estimate, tell)
//     (mex_tc_of) and the free-swing dodge window is tell + 120 ms (react floor = tell). Nothing is lengthened beyond what it was; SP's ~500 ms is the ceiling.
//     The authored contact is not known exactly: it starts at the exchange's tune (440 fists / 520 weapons) and is corrected from every OBSERVED contact of our own exchange blows
//     (estimate = observed contact x speed applied, EMA 0.5), so the speed-up converges on the real clip.
//  3. VISUAL: nothing new - the existing Dodge prompt / glyph opens with the window, which now opens at the tell (no extra HUD).
// Opt-out t2r-no-melee-tell.txt (old behaviour: x0.65 windup, 480 / 380 ms window). All files re-read every 2 s. No keybinds.
#define MT_TELL_MS 320
#define MT_WIN_EXTRA_MS 120
#define MT_VOX_AT_MS 80
#define MT_SWISH_AT_MS 40
#define MT_RATE_MS 450
static struct {
    int off, tell_ms, cfg_logged;
    uint64_t cfg_at;
    float auth[4], last_speed;
    int last_w;
    struct {
        uint64_t ent, vox_at, sw_at, w;
        uint32_t who;
        int cls;
    } pend[4];
    uint64_t last[T2R_MAX_PEERS + 1];
} g_mtl = {.tell_ms = MT_TELL_MS,.auth = { -1, -1, -1, -1 },.last_speed =
    1.0f };
static void MtellCfg(void)
{
    uint64_t now = GetTickCount64();
    if (g_mtl.cfg_at && now - g_mtl.cfg_at < 2000)
        return;
    g_mtl.cfg_at = now ? now : 1;
    FILE *f = ExeSideOpen("t2r-no-melee-tell.txt", "r");
    int o = f != NULL;
    if (f)
        fclose(f);              // (keep it on - fix forward, never back to the old behaviour)
    int ms = MT_TELL_MS;
    f = ExeSideOpen("t2r-tell-ms.txt", "r");
    if (f) {
        int v;
        if (fscanf(f, "%d", &v) == 1 && v >= 200 && v <= 500)
            ms = v;
        fclose(f);
    }
    if (o != g_mtl.off || ms != g_mtl.tell_ms || !g_mtl.cfg_logged) {
        g_mtl.cfg_logged = 1;
        Xlog("tell: %s (tell window %d ms from the swing start to contact; SP ~500 ms)", o ? "OFF (t2r-no-melee-tell.txt)" : "on", ms);
    }
    g_mtl.off = o;
    g_mtl.tell_ms = ms;
}

static int MtellOn(void)
{
    MtellCfg();
    return !g_mtl.off;
}

static float MtellAuth(int wcls, int raw)
{
    if (wcls < 0 || wcls > 3)
        return (float) raw;
    if (g_mtl.auth[wcls] < 0)
        g_mtl.auth[wcls] = (float) raw;
    return g_mtl.auth[wcls];
}

// planned contact (ms after the swing start on the shared clock) of an exchange blow: the tell, or the clip's own contact when that is already faster
static int MtellTc(int wcls, int raw)
{
    if (!MtellOn())
        return raw;
    float a = MtellAuth(wcls, raw);
    return a > (float) g_mtl.tell_ms ? g_mtl.tell_ms : (int) (a + 0.5f);
}

// windup speed-up that brings the clip's contact to the tell (0 = tell off; 1 = nothing to speed up)
static float MtellSpeed(int wcls, int raw)
{
    if (!MtellOn())
        return 0.0f;
    if (Mv2On())
        return 1.0f;            // melee v2: SP windup speed (no speed-up, no slow-down)
    float s = MtellAuth(wcls, raw) / (float) g_mtl.tell_ms;
    if (s < 1.0f)
        s = 1.0f;
    if (s > 1.25f)
        s = 1.25f;              // (was 1.6 - a windup played 60 % fast twitches; fluid over fast. The rest of the tell comes from the shorter window)
    g_mtl.last_speed = s;
    g_mtl.last_w = wcls;
    return s;
}

static void MtellNoteContact(int wcls, int obs_ms)
{
    if (!MtellOn() || wcls < 0 || wcls > 3 || obs_ms < 100
        || obs_ms > 1500)
        return;
    float Est =
        (float) obs_ms * (g_mtl.last_w == wcls ? g_mtl.last_speed : 1.0f);
    if (Est < 150.0f)
        Est = 150.0f;
    if (Est > 800.0f)
        Est = 800.0f;
    float old = g_mtl.auth[wcls] < 0 ? Est : g_mtl.auth[wcls];
    g_mtl.auth[wcls] = 0.5f * old + 0.5f * Est;
    Xlog("tell: observed contact %d ms after T0 (class %d, windup x%.2f) -> clip contact estimate %.0f ms (target %d ms)", obs_ms, wcls, g_mtl.last_w == wcls ? (double) g_mtl.last_speed : 1.0, (double) g_mtl.auth[wcls], g_mtl.tell_ms);
}

// free-swing dodge window (melee_dodge.c mdg_window): 0 = tell off
static uint64_t MtellWinMs(void)
{
    return MtellOn()
        && !Mv2On()? (uint64_t) (g_mtl.tell_ms + MT_WIN_EXTRA_MS) : 0;
}

static uint64_t MtellReactMs(void)
{
    return MtellOn() && !Mv2On()? (uint64_t) g_mtl.tell_ms : 0;
}

// the cue. who = slot of the swinging puppet or T2R_MAX_PEERS = us. Never on a downed / dying / dead body, never on a body in a paired move (its own voice),
// never twice within 450 ms on one body, and the execution lines' own gate (g_att_vox_ms) is respected and renewed so dm_snd's blow grunt does not double it.
static void MdgWhiffWhoosh(uint64_t ent, uint64_t w, int slot);       // melee_dodge.c: the engine's own per-weapon whoosh event (sfx-weapon-*-whoosh, unarmed = sfx-unarmed-finisher-whoosh)
static uint64_t DcWeapon(void);        // dcounter.c: our held melee weapon, abby-unarmed for fists
// the weapon a swinging body holds (puppet: winmm's held id at peer+0x91)
static uint64_t MtellWeapon(uint32_t who)
{
    if (who == T2R_MAX_PEERS)
        return DcWeapon();
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *pe = who < T2R_MAX_PEERS ? ((fn_peer) T2rPeer) (who) : NULL;
    uint64_t w = 0;
    if (pe && !IsBadReadPtr(pe + 0x91, 8))
        memcpy(&w, pe + 0x91, 8);
    return w;
}

static int MtellBodyOk(uint32_t who)
{
    if (who == T2R_MAX_PEERS)
        return !(g_downed || g_downDying || PsyncOwn() || g_execUntil
                 || g_xcamOn || LocalHp() <= 0.0f);
    return who < T2R_MAX_PEERS && !g_peerDowned[who]
        && !g_puppetDeadHere[who] && !PsyncBusySlot(who);
}

static void MtellCue(uint32_t who, uint64_t ent, int armed,
                      const char *src)
{
    if (!MtellOn() || who > T2R_MAX_PEERS || !ent || ent == ~0ULL
        || !MtellBodyOk(who))
        return;
    if (MduOwnsCue(who))
        return;                 // melee_duel.c plays the duel swing's cues at the clip's own frames (both screens)
    if (Mv3Stand
        ("tell: swing swish + effort grunt",
         who == T2R_MAX_PEERS ? *(uint32_t *) T2R_SYM(kMySlot) : who))
        return;                 // melee v3: mv3_fx plays it at MV3_E_SWING
    uint64_t now = GetTickCount64();
    if (now - g_mtl.last[who] < MT_RATE_MS
        || (g_attVoxMs && now - g_attVoxMs < 300))
        return;
    for (int i = 0; i < 4; i++)
        if (!g_mtl.pend[i].vox_at) {
            g_mtl.last[who] = now;
            g_mtl.pend[i].ent = ent;
            g_mtl.pend[i].who = who;
            g_mtl.pend[i].cls = armed;
            g_mtl.pend[i].w = MtellWeapon(who);
            g_mtl.pend[i].vox_at = now + MT_VOX_AT_MS;
            g_mtl.pend[i].sw_at = now + MT_SWISH_AT_MS;
            static int nl;
            if (LOGCAP(nl, 80))
                Xlog("tell: cue on %s (%s, %s): swish +%d ms, grunt +%d ms; contact in ~%d ms", who == T2R_MAX_PEERS ? "us" : "a puppet", src, armed ? "weapon" : "fists", MT_SWISH_AT_MS, MT_VOX_AT_MS, g_mtl.tell_ms);
            return;
        }
}

static void MtellTick(void)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < 4; i++) {
        if (!g_mtl.pend[i].vox_at)
            continue;
        if (g_mtl.pend[i].sw_at && now >= g_mtl.pend[i].sw_at) {
            g_mtl.pend[i].sw_at = 0;
            if (MtellBodyOk(g_mtl.pend[i].who))
                MdgWhiffWhoosh(g_mtl.pend[i].ent, g_mtl.pend[i].w,
                                 g_mtl.pend[i].who ==
                                 T2R_MAX_PEERS ? -1 : (int) g_mtl.pend[i].
                                 who);
        }                       // the ripped pipe whoosh sounded like a pipe dragged on the floor; the weapon's own SP whoosh
        if (now < g_mtl.pend[i].vox_at)
            continue;
        g_mtl.pend[i].vox_at = 0;
        if (g_mtl.pend[i].sw_at) {
            g_mtl.pend[i].sw_at = 0;
        }
        if (!MtellBodyOk(g_mtl.pend[i].who))
            continue;           // pain / death / a paired move took the body: its own voice wins
        if (GetFileAttributesA("t2r-no-dodge-vox.txt") !=
            INVALID_FILE_ATTRIBUTES)
            continue;
        g_attVoxMs = now;
        DmSay(g_mtl.pend[i].ent,
               g_mtl.pend[i].
               cls ? "vox-eff-gen-fight-give-wpn-med" :
               "vox-eff-gen-fight-give-med", VXE_GIVE);
    }
}
