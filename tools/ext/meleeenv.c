// Included from t2r-ext.c after ctxdown.c (uses vdf_inline / player_locator / puppet_locator / ps_loc_ok / is_teammate / g_peer_downed ...).
// Melee next to a car: the engine's move search (FindMeleeMove, exe 0x140e41b10) sometimes picks the glass-break swing (macro
// melee-player-swing-low-glass, start func set-env-obj-from-glass->cache; player-*.bin of the fists / blades / blunt sets) over the plain swing at
// the enemy player standing right there . Fix: while FindMeleeMove runs, CanMeleeAttackExecute (0x140e3f760, the one per-candidate
// call inside it at 0x140e427b1) answers "no" for a glass-break candidate when a live enemy puppet is within 2.5 m, +-70 deg of our facing. The search
// then takes its next candidate (the enemy swing) by its own rules - never a false result of the search itself (no dead press).
// One log line per override. Opt-out: t2r-melee-env-ok.txt next to the exe.
typedef uint8_t(*me_fn_find) (void *, uint8_t *, uint64_t, uint64_t);
typedef uint8_t(*me_fn_cand) (const uint8_t *, void *, uint64_t, void *,
                              void *, void *, uint32_t *, void *, uint64_t,
                              uint64_t, uint64_t);
static me_fn_find g_meFind;
static me_fn_cand g_meCand;
static int g_meInFind;
static int g_meOff;
static int g_meCacheSlot = -2;
static uint64_t g_meCacheFrame;
static float g_meCacheD;      // one enemy scan per frame

// the live enemy puppet nearest in front of us (2.5 m, +-70 deg, +-1.5 m height); slot or -1
static int MeEnemyAhead(float *dist)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (g_meCacheFrame == g_frame && g_meCacheSlot != -2) {
        *dist = g_meCacheD;
        return g_meCacheSlot;
    }
    int best = -1;
    float bd = 2.5f;
    float mb[16];
    const float *me = MatchSubstate() == 6 ? PlayerLocator(mb) : NULL;
    if (me && PsLocOk(me)) {
        float fx = 2.0f * (me[4] * me[6] + me[7] * me[5]), fz =
            1.0f - 2.0f * (me[4] * me[4] + me[5] * me[5]);
        for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
            uint8_t *peer = ((fn_peer) T2rPeer) (s);
            uint64_t h = peer ? *(uint64_t *) (peer + 0x60) : 0;
            if (!h || !peer[4] || peer[5] || peer[0x4d]
                || s == PsyncSelf() || IsTeammate(s) || g_peerDowned[s]
                || g_puppetDeadHere[s])
                continue;
            float b[16];
            const float *l = PuppetLocator(h, b);
            if (!l || !PsLocOk(l))
                continue;
            float dx = l[0] - me[0], dz = l[2] - me[2], dy =
                l[1] - me[1], d = __builtin_sqrtf(dx * dx + dz * dz);
            if (d > bd || dy > 1.5f || dy < -1.5f)
                continue;
            if (d > 0.15f && (dx * fx + dz * fz) / d < 0.342f)
                continue;       // cos 70 deg
            best = (int) s;
            bd = d;
        }
    }
    g_meCacheFrame = g_frame;
    g_meCacheSlot = best;
    g_meCacheD = bd;
    *dist = bd;
    return best;
}

// v5-dis3 ENGINE DISARM VETO (A/B, SP's hatchet disarm clips were resident): fist-light-attack-1 carries an "auto" link at 0 ms to
// the player finisher lists (melee-moves.json), and the disarm rows are the only ones there without an (is-npc? defender) condition - so against a
// lethal-hp player holding a hatchet the engine itself replaced our swing by fist-1h-blade-disarm-iron-rhino-finisher (A 41895, then its -sync
// 41920): PlayerMelee::Update ran npc-holster-weapon on the puppet and player-instant-draw-weapon-by-name(hatchet) on us (A 41950), ONE-SIDED - no
// psync START, the victim (B 36839 / 36870) saw plain swings and kept its hatchet: the weapon was duplicated. The auto transitions are candidate
// checks without a FindMeleeMove around them (re/sp-sandbox.md MCANDS), so every PLAYER disarm move (mdis_veto.h, 92 moves) answers "no" here in
// any search; the engine keeps the plain swing and the duel / ctxdown lethal finisher pick (cd_fin_pick: mf- disarm rows, psync START to the
// victim, cd_dis_* hand-off at SP's take frame on every screen) owns the disarm. t2r-engine-disarm-ok.txt next to the exe = stock (re-read every 2 s).
#include "mdis_veto.h"
static int MeDisOff(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at > 2000) {
        at = now;
        int o =
            GetFileAttributesA("t2r-engine-disarm-ok.txt") !=
            INVALID_FILE_ATTRIBUTES;
        if (o != off)
            Xlog("melee env: engine-picked player disarm moves %s",
                 o ? "ALLOWED (t2r-engine-disarm-ok.txt: stock, one-sided)"
                 : "vetoed (the duel / ctxdown disarm rows own disarms)");
        off = o;
    }
    return off;
}

// ps31 SP OPENER RULE (melee_duel.c mdu_open_veto, the full note is there): inside the move search the set's step-1 opener (fist-light-attack-1, the weapon
// sets' light-1 rows) is asked of the ENGINE first; only when the engine itself passes it and SP's rule wants another row (the left jab inside 2.0 m, the "-far"
// opener at 3-5 m) the answer is "no", like the glass / disarm vetoes - the search goes on to its next candidate, and melee_duel.c starts the wanted row itself when
// the engine has none. Nothing that started is cancelled. t2r-duel-opener-ps30.txt = off.
static int MduOpenCand(uint64_t aname);       // melee_duel.c
static int MduOpenVeto(uint64_t aname);       // melee_duel.c
static int MduP32EngineHit(uint64_t aname);  // melee_duel.c (ps32): the engine's own -hit moves of fist blows 5 / 6 (the second one is SP's stun state)
static int MduTlCand(uint64_t aname); // melee_duel_parry.c (ps41b turn lock)
static int MduTlVeto(uint64_t aname); // melee_duel_parry.c
static uint8_t MeCandHook(const uint8_t *atk, void *cache, uint64_t b3,
                            void *search, void *common, void *perm,
                            uint32_t *outp, void *aprefs, uint64_t b9,
                            uint64_t u10, uint64_t b11)
{
    if (atk && !IsBadReadPtr(atk, 8) && !MeDisOff()) {
        uint64_t aname = *(const uint64_t *) atk;
        for (int i = 0; i < MDIS_VETO_N; i++)
            if (aname == MDIS_VETO[i].sid) {
                static uint64_t last, lastsid;
                uint64_t now = GetTickCount64();
                if (aname != lastsid || now - last > 500)
                    Xlog("melee: engine disarm candidate %s (%016llx) refused%s - the duel / ctxdown disarm row owns disarms (t2r-engine-disarm-ok.txt = stock)", MDIS_VETO[i].name, (unsigned long long) aname, g_meInFind ? " in the move search" : " (auto transition / prompt check)");
                last = now;
                lastsid = aname;
                return 0;
            }
    }
    if (atk && !IsBadReadPtr(atk, 8) && MduP32EngineHit(*(const uint64_t *) atk)) {  // ps32: no engine-side stun -hit move (state, "Grab / Strike" prompt) on a player
        static uint64_t last;
        uint64_t now = GetTickCount64();
        if (now - last > 500)
            Xlog("melee: engine -hit candidate %016llx of a fist stun-combo strike refused%s - the duel's synced pair plays both halves, no stun state / prompt on a player (ps32)", (unsigned long long) *(const uint64_t *) atk, g_meInFind ? " in the move search" : " (auto transition)");
        last = now;
        return 0;
    }
    // ps41b TURN LOCK (melee_duel.c / melee_duel_parry.c): the partner's swing is registered on us and this press would be no parry / clash claim - the duel row
    // the engine itself would start is refused here, so nothing starts and nothing has to be cancelled on its first frame. The opener rule below still runs for
    // a row the turn lock lets through (one engine check for both).
    if (g_meInFind && atk && !IsBadReadPtr(atk, 8)
        && MduTlCand(*(const uint64_t *) atk)) {
        uint64_t aname = *(const uint64_t *) atk;
        uint8_t r =
            g_meCand(atk, cache, b3, search, common, perm, outp, aprefs,
                      b9, u10, b11);
        if (r && MduTlVeto(aname))
            return 0;           // (the proof line "DUEL: press during slot N's swing ... refused in the move search" is written there)
        if (r && MduOpenCand(aname) && MduOpenVeto(aname)) {
            static uint64_t last;
            uint64_t now = GetTickCount64();
            if (now - last > 250)
                Xlog("melee: opener candidate %016llx passed the engine's own check and is refused in the move search - SP's opener rule wants another row (ps31; t2r-duel-opener-ps30.txt = off)", (unsigned long long) aname);
            last = now;
            return 0;
        }
        return r;
    }
    if (g_meInFind && atk && !IsBadReadPtr(atk, 8) && MduOpenCand(*(const uint64_t *) atk)) {       // ps31: SP's opener rule
        uint64_t aname = *(const uint64_t *) atk;
        uint8_t r =
            g_meCand(atk, cache, b3, search, common, perm, outp, aprefs,
                      b9, u10, b11);
        if (r && MduOpenVeto(aname)) {
            static uint64_t last;
            uint64_t now = GetTickCount64();
            if (now - last > 250)
                Xlog("melee: opener candidate %016llx passed the engine's own check and is refused in the move search - SP's opener rule wants another row (ps31; t2r-duel-opener-ps30.txt = off)", (unsigned long long) aname);
            last = now;
            return 0;
        }
        return r;
    }
    if (g_meInFind && !g_meOff && atk && !IsBadReadPtr(atk, 0x18)) {
        // FIX: the earlier test compared the macro (atk+0x10) with melee-player-swing-low-glass, but every glass move has the macro "unsorted"
        // (data: player-*.bin of the fists / blade / blunt / 2h / rifle / switchblade sets; the string only appears as a symbol in the start lambdas),
        // so the veto never matched (no "refused" line in any A/B log). The moves are told apart by their NAME (atk+0), listed here.
        static uint64_t glass_sid[24];
        static int nglass;
        if (!nglass) {
            static const char *const nm[] =
                { "1h-blade-glass-attack-1",
"1h-blade-glass-crouch-attack-1", "1h-blade-glass-low-attack-1",
                "1h-blunt-glass-attack-1",
                    "1h-blunt-glass-crouch-attack-1",
                    "1h-blunt-glass-low-attack-1",
                "2h-glass-attack-1", "2h-glass-crouch-attack-1",
                    "2h-glass-low-attack-1",
                "fist-glass-attack-1", "fist-glass-crouched-low-attack-1",
                    "fist-glass-crouched-high-attack-1",
                "rifle-glass-attack-1", "rifle-glass-crouch-attack-1",
                    "rifle-glass-crouch-low-attack-1",
                "switchblade-glass-attack-1",
                    "switchblade-glass-crouch-attack-1",
                    "switchblade-glass-low-attack-1"
            };
            for (int i = 0; i < 18; i++)
                glass_sid[i] = FNV64(nm[i]);
            nglass = 18;
        }
        int is_glass = 0;
        uint64_t aname = *(const uint64_t *) atk;
        for (int i = 0; i < nglass; i++)
            if (aname == glass_sid[i]) {
                is_glass = 1;
                break;
            }
        if (is_glass) {
            float d = 0;
            int s = MeEnemyAhead(&d);
            if (s < 0) {        // no enemy in the cone: the smash stays; one line so the A/B log shows the engine's own glass pick
                uint8_t r =
                    g_meCand(atk, cache, b3, search, common, perm, outp,
                              aprefs, b9, u10, b11);
                static uint64_t lastok;
                uint64_t now = GetTickCount64();
                if (r && now - lastok > 500)
                    Xlog("melee: glass move %016llx allowed (no enemy within 2.5 m in front)", (unsigned long long) aname);
                if (r)
                    lastok = now;
                return r;
            }
            if (s >= 0) {
                static uint64_t last;
                uint64_t now = GetTickCount64();
                if (now - last > 250)
                    Xlog("melee: env move %016llx (glass break) refused - enemy slot %d at %.2f m", (unsigned long long) *(const uint64_t *) atk, s, d);
                last = now;
                return 0;
            }
        }
    }
    return g_meCand(atk, cache, b3, search, common, perm, outp, aprefs,
                     b9, u10, b11);
}

static uint8_t MeFindHook(void *search, uint8_t *start, uint64_t x3,
                            uint64_t x4)
{
    int prev = g_meInFind;
    g_meInFind = 1;
    uint8_t r = g_meFind(search, start, x3, x4);
    g_meInFind = prev;
    return r;
}

static void InstallMeleeEnv(void)
{
    FILE *f = ExeSideOpen("t2r-melee-env-ok.txt", "r");
    g_meOff = f != NULL;
    if (f)
        fclose(f);
    if (g_meOff)
        Xlog("melee env: glass-break veto off (t2r-melee-env-ok.txt)"); // (v5-dis3: the hooks stay for the engine disarm veto, me_dis_off)
    // FindMeleeMove: mov [rsp+18h],rbx; push rbp,rsi,rdi,r12,r13,r14,r15 (16 bytes, no rip-relative operand)
    static const uint8_t e_find[16] =
        { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
0x55, 0x41, 0x56, 0x41, 0x57 };
    // CanMeleeAttackExecute: mov rax,rsp; mov [rax+18h],rbx; push rbp,rsi,rdi,r12,r13 (14 bytes)
    static const uint8_t e_cand[14] =
        { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41,
0x54, 0x41, 0x55 };
    g_meCand =
        (me_fn_cand) VdfInline(0x140e3f760, e_cand, 14,
                                (void *) MeCandHook,
                                "CanMeleeAttackExecute",
                                (void **) &g_meCand);
    if (g_meCand)
        g_meFind =
            (me_fn_find) VdfInline(0x140e41b10, e_find, 16,
                                    (void *) MeFindHook, "FindMeleeMove",
                                    (void **) &g_meFind);
    Xlog("melee env: glass-break candidates vetoed next to an enemy: %s; engine player disarm moves (%d) vetoed: %s", g_meCand && g_meFind ? (g_meOff ? "off" : "on") : "NOT installed", MDIS_VETO_N, g_meCand ? (MeDisOff()? "off (t2r-engine-disarm-ok.txt)" : "on") : "NOT installed");
}
