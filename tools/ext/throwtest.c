// ---------------------------------------------------------------- AUTOMATED THROWABLE TEST HARNESS (DEV builds only; #ifndef T2R_RELEASE, included from the dev block of t2r-ext.c)
//
// Verifies pipe / nail bombs, molotovs, stun / smoke bombs and placed bombs between the two local test instances A and B with NO input (no synthetic
// keys / mouse): everything goes through engine natives and the same paths a real throw takes. Nothing runs unless the command file exists.
//
//   THROWER instance (A):   t2r-throwtest.txt next to the exe. Read once (then deleted), one job per line, '#' comments:
//        <kind> <dist> [open|cover|any] [from=<m>] [wait=<s>] [to=<slot>] [downed]
//        kind   nail | pipe (= nail) | molotov | stun | smoke | placed
//        dist   metres from the target player's feet to the landing / placing point (ground distance)
//        open   (default) pick a direction with open flat ground and a clear chest-height line target -> landing point (no wall in between)
//        cover  ('behind-cover') pick a direction where a wall / prop sits between the target and the landing point
//        any    first direction with flat ground, cover or not
//        from=  how far BEHIND the landing point (away from the target) the throw starts, default 6 m, 1.6 m up (so the flight is a real arc of
//               ~1 s whose motion is sent as the type-2 throw message and tracked by MSG_BOMB_TRACK)
//        wait=  seconds to wait after the throw before the next line (default nail 9, molotov 14, stun / smoke 9, placed 8)
//        to=    target slot (default: the first other player alive in the match; "slot <n>" on a line of its own changes the default)
//        downed let the job run while the target is downed (default: wait until it is up)
//      Per job: wait until we are alive + the target is alive / up + standing still (not moved > 0.3 m for 0.6 s), probe 8 directions with world
//      rays (chest line, ground, ceiling), choose the direction, spawn the REAL throwable through the DC spawn wrapper exe+0x654cd0 (spawn_hook:
//      own-bomb registration + Bomb Expert fuse + MSG_BOMB_TRACK / MSG_BOMB_BURST), set the velocity (exe+0x11c8b00) and send the type-2 throw
//      message (send_hook) exactly as the dev throw does; placed: pb_place_commit (own bomb + MSG_MINE). We NEVER move a player.
//      Log lines:  "throwtest: #k <kind> dist <d> <mode> sent ..." / "... done" / "... SKIPPED <why>" / "throwtest: waiting ..." (every 10 s).
//
//   TARGET instance (B):    nothing to type. The existing explosives2013.c lines carry the verdict (see throwtest_log.h): "throwtest: burst" (every burst
//      that reached B, with burst + feet position), "throwtest: result" (probes / ring / damage / hp before-after / vest / downed / killed),
//      "throwtest: copy" (nearest copy of the thrower's bomb on B's screen vs the thrower's burst point), "throwtest: stunned", "throwtest: fire".
//      Optional t2r-throwtest-target.txt on B (polled every 2 s; B stays in place, YOU do not touch it): tokens  heal  (default: back to full health
//      3 s after the last blast line, so every throw meets the same hp)   vest=<n>  (also set the vest to n at that moment, e.g. vest=0).
//
//   Report: python tools/win/throwtest_report.py <A t2r-ext.log> <B t2r-ext.log>
#ifndef T2R_RELEASE
#define TT_MAXJOB 64
enum { TT_NAIL = 1, TT_MOLO, TT_STUN, TT_SMOKE, TT_PLACED };
typedef struct {
    int kind, mode, slot, downed_ok;
    float dist, from;
    int wait_s;
    char text[80];
} TtJob;
static struct {
    int n, cur, st, slot_default;
    uint64_t poll_f, next_ms, wait_until, st_ms, log_ms, kframe, stable_ms;
    float stable_pos[3];
    TtJob job[TT_MAXJOB];
    uint64_t ray[8][3];
    float base_pos[3];
    int base_ang, tslot;
} g_tt = {.slot_default = -1 };

static uint64_t g_tt_tpoll, g_tt_theal_ms;
static int g_tt_theal, g_tt_tvest = -1;

static const char *TtKindName(int k)
{
    return k == TT_NAIL ? "nail" : k == TT_MOLO ? "molotov" : k ==
        TT_STUN ? "stun" : k == TT_SMOKE ? "smoke" : "placed";
}

static uint64_t TtKindSid(int k)
{
    return k == TT_NAIL ? 0xcf576d288d014eacULL : k ==
        TT_MOLO ? 0x2a3164cb4a1c95d9ULL : k ==
        TT_STUN ? 0x8502a9c43b674354ULL : k ==
        TT_SMOKE ? 0xa2aa922866d0479dULL : 0;
}

// the file: read whole, parse, delete
static void TtLoad(void)
{
    FILE *f = ExeSideOpen("t2r-throwtest.txt", "r");
    if (!f)
        return;
    char buf[4096];
    size_t r = fread(buf, 1, sizeof buf - 1, f);
    buf[r] = 0;
    fclose(f);
    char pa[MAX_PATH];
    GetModuleFileNameA(NULL, pa, MAX_PATH);
    char *sa = strrchr(pa, 92);
    if (sa) {
        strcpy(sa + 1, "t2r-throwtest.txt");
        remove(pa);
    }
    memset(&g_tt.job, 0, sizeof g_tt.job);
    g_tt.n = g_tt.cur = g_tt.st = 0;
    g_tt.next_ms = 0;
    g_tt.slot_default = -1;
    for (char *ln = strtok(buf, "\r\n"); ln && g_tt.n < TT_MAXJOB;
         ln = strtok(NULL, "\r\n")) {
        while (*ln == ' ' || *ln == '\t')
            ln++;
        if (!*ln || *ln == '#')
            continue;
        char tk[8][24] = { { 0 } };
        int nt =
            sscanf(ln, "%23s %23s %23s %23s %23s %23s %23s %23s", tk[0],
                   tk[1], tk[2], tk[3], tk[4], tk[5], tk[6], tk[7]);
        if (nt >= 2 && !strcmp(tk[0], "slot")) {
            g_tt.slot_default = atoi(tk[1]);
            continue;
        }
        TtJob j;
        memset(&j, 0, sizeof j);
        j.kind = !strcmp(tk[0], "nail")
            || !strcmp(tk[0], "pipe") ? TT_NAIL : !strcmp(tk[0],
                                                          "molotov") ?
            TT_MOLO : !strcmp(tk[0], "stun") ? TT_STUN : !strcmp(tk[0],
                                                                 "smoke") ?
            TT_SMOKE : !strcmp(tk[0], "placed") ? TT_PLACED : 0;
        j.dist = nt >= 2 ? (float) atof(tk[1]) : 0.0f;
        if (!j.kind || !(j.dist >= 0.5f && j.dist <= 40.0f)) {
            Xlog("throwtest: line \"%s\" ignored (<nail|molotov|stun|smoke|placed> <dist 0.5-40> [open|cover|any] ...)", ln);
            continue;
        }
        j.mode = 0;
        j.slot = -1;
        j.from = 6.0f;
        j.wait_s = j.kind == TT_MOLO ? 14 : j.kind == TT_PLACED ? 8 : 9;
        for (int i = 2; i < nt; i++) {
            if (!strcmp(tk[i], "open"))
                j.mode = 0;
            else if (!strcmp(tk[i], "cover")
                     || !strcmp(tk[i], "behind-cover"))
                j.mode = 1;
            else if (!strcmp(tk[i], "any"))
                j.mode = 2;
            else if (!strcmp(tk[i], "downed"))
                j.downed_ok = 1;
            else if (!strncmp(tk[i], "from=", 5))
                j.from = (float) atof(tk[i] + 5);
            else if (!strncmp(tk[i], "wait=", 5))
                j.wait_s = atoi(tk[i] + 5);
            else if (!strncmp(tk[i], "to=", 3))
                j.slot = atoi(tk[i] + 3);
        }
        if (!(j.from >= 1.0f && j.from <= 30.0f))
            j.from = 6.0f;
        if (j.wait_s < 2 || j.wait_s > 120)
            j.wait_s = 9;
        snprintf(j.text, sizeof j.text, "%s", ln);
        g_tt.job[g_tt.n++] = j;
    }
    Xlog("throwtest: loaded %d job(s) from t2r-throwtest.txt (we never move a player; waits for a live, still target)", g_tt.n);
}

// the target's feet (its puppet's locator): 0 = not available
static int TtTargetPos(uint32_t slot, float *out)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *pe = ((fn_peer) T2rPeer) (slot);
    if (!pe || !pe[4] || pe[5] || !*(uint64_t *) (pe + 0x60))
        return 0;
    uint8_t *proc = ObProc(*(uint64_t *) (pe + 0x60));
    if (!proc || IsBadReadPtr(proc, 0x120))
        return 0;
    float lb[16];
    const float *p = ObLoc(proc, lb);
    if (!p || !__builtin_isfinite(p[0] + p[1] + p[2])
        || (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f))
        return 0;
    memcpy(out, p, 12);
    return 1;
}

static int TtPickSlot(const TtJob *j)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (j->slot >= 0)
        return j->slot;
    if (g_tt.slot_default >= 0)
        return g_tt.slot_default;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (s == self)
            continue;
        uint8_t *pe = ((fn_peer) T2rPeer) (s);
        if (pe && pe[4] && !pe[5])
            return (int) s;
    }
    return -1;
}

static void TtDir(int i, float *dx, float *dz)
{
    float a = (g_tt.base_ang + i * 45) * 0.0174532925f;
    *dx = __builtin_sinf(a);
    *dz = __builtin_cosf(a);
}

static void TtThrow(const TtJob *j, int idx, const float *T, float dx,
                     float dz, int ang)
{
    typedef void *(*fn_spawn)(float *, float *, uint64_t, uint64_t, float,
                              void *);
    typedef void *(*fn_cast)(void *);
    typedef void (*fn_vel)(void *, float *);
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    uint64_t sid = TtKindSid(j->kind);
    float start[4] =
        { T[0] + dx * j->from, T[1] + 1.6f, T[2] + dz * j->from, 0 },
        target[4] = { T[0], T[1], T[2], 0 }, vel[4];
    const float Tf = j->from / 6.0f + 0.5f;     // flight time of the ballistic arc
    vel[0] = (target[0] - start[0]) / Tf;
    vel[2] = (target[2] - start[2]) / Tf;
    vel[1] = (target[1] - start[1] + 0.5f * 9.8f * Tf * Tf) / Tf;
    vel[3] = 0;
    uint8_t *p = ((fn_spawn) (exe + 0x654cd0)) (start, target, sid, 0xed417f0ec4a41b7eULL, -1.0f, NULL);        // not under g_ext_spawn: spawn_hook registers it as our own throw
    void *q = p ? ((fn_cast) (exe + 0x11c7070)) (p) : NULL;
    if (q)
        ((fn_vel) (exe + 0x11c8b00)) (q, vel);
    uint8_t m[0x2c];
    memcpy(m, start, 12);
    memcpy(m + 12, target, 12);
    memcpy(m + 24, vel, 12);
    memcpy(m + 0x24, &sid, 8);
    if (p)
        SendHook(2, m, sizeof m, 0xff);        // the type-2 throw message (own-throw bookkeeping + the peers' copies)
    Xlog("throwtest: #%d %s dist %.2f %s sent: landing %.2f %.2f %.2f (dir %d deg, thrown from %.1f m behind it) proj %p%s tms %llu", idx + 1, TtKindName(j->kind), j->dist, j->mode == 1 ? "behind-cover" : j->mode == 2 ? "any" : "open", T[0], T[1], T[2], ang, j->from, (void *) p, p && q ? "" : " SPAWN FAILED", (unsigned long long) GetTickCount64());
}

static void TtPlace(const TtJob *j, int idx, const float *T, int ang)
{
    if (!InvQty(PB_PIPE_SID))
        DcDirect(0x7258db3b9e03e625ULL, PB_PIPE_SID, 1, 0);
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    int mine = 0;
    for (int i = 0; i < PB_MAX; i++)
        if (g_pb[i].used && g_pb[i].owner == (int) self)
            mine++;
    if (mine >= PB_CAP_OWNER) {
        Xlog("throwtest: #%d placed dist %.2f SKIPPED - %d of our own bombs are still down (cap %d)", idx + 1, j->dist, mine, PB_CAP_OWNER);
        return;
    }
    float pos[3] = { T[0], T[1], T[2] };
    PbPlaceCommit(pos);
    Xlog("throwtest: #%d placed dist %.2f %s sent: bomb at %.2f %.2f %.2f (dir %d deg; the target triggers it itself inside its sphere) tms %llu", idx + 1, j->dist, j->mode == 1 ? "behind-cover" : j->mode == 2 ? "any" : "open", T[0], T[1], T[2], ang, (unsigned long long) GetTickCount64());
}

static void TtThrowerTick(void)
{
    uint64_t now = GetTickCount64();
    if (!g_tt.n) {
        if (g_frame - g_tt.poll_f < 30)
            return;
        g_tt.poll_f = g_frame;
        TtLoad();
        return;
    }
    if (MatchSubstate() != 6) {
        if (now - g_tt.log_ms > 10000) {
            g_tt.log_ms = now;
            Xlog("throwtest: waiting - not in a running match (job %d of %d)", g_tt.cur + 1, g_tt.n);
        }
        return;
    }
    if (g_tt.cur >= g_tt.n) {
        Xlog("throwtest: all %d job(s) done", g_tt.n);
        g_tt.n = 0;
        return;
    }
    TtJob *j = &g_tt.job[g_tt.cur];
    if (g_tt.st == 2) {
        if (now < g_tt.wait_until)
            return;
        Xlog("throwtest: #%d %s done (waited %d s)", g_tt.cur + 1,
             TtKindName(j->kind), j->wait_s);
        g_tt.cur++;
        g_tt.st = 0;
        g_tt.next_ms = now + 1500;
        return;
    }
    if (g_tt.st == 0) {
        if (now < g_tt.next_ms)
            return;
        if (!g_tt.st_ms)
            g_tt.st_ms = now;
        const char *why = NULL;
        int slot = TtPickSlot(j);
        float T0[3] = { 0 };
        typedef uint8_t *(*fn_peer)(uint32_t);
        if (slot < 0 || slot >= T2R_MAX_PEERS)
            why = "no target player in the match";
        else if (LocalHp() <= 0.0f || g_downed)
            why = "we are dead / downed";
        else if (!TtTargetPos((uint32_t) slot, T0))
            why = "target dead / respawning / no puppet";
        else if (g_peerDowned[slot] && !j->downed_ok)
            why = "target is downed";
        if (!why) {             // standing still
            float dd =
                __builtin_sqrtf((T0[0] - g_tt.stable_pos[0]) * (T0[0] -
                                                                g_tt.
                                                                stable_pos
                                                                [0]) +
                                (T0[1] - g_tt.stable_pos[1]) * (T0[1] -
                                                                g_tt.
                                                                stable_pos
                                                                [1]) +
                                (T0[2] - g_tt.stable_pos[2]) * (T0[2] -
                                                                g_tt.
                                                                stable_pos
                                                                [2]));
            if (dd > 0.3f || !g_tt.stable_ms) {
                memcpy(g_tt.stable_pos, T0, 12);
                g_tt.stable_ms = now;
            }
            if (now - g_tt.stable_ms < 600)
                why = "target is moving";
        } else
            g_tt.stable_ms = 0;
        if (why) {
            if (now - g_tt.log_ms > 10000) {
                g_tt.log_ms = now;
                Xlog("throwtest: #%d waiting - %s", g_tt.cur + 1, why);
            }
            if (now - g_tt.st_ms > 120000) {
                Xlog("throwtest: #%d %s dist %.2f SKIPPED - waited 120 s (%s)", g_tt.cur + 1, TtKindName(j->kind), j->dist, why);
                g_tt.cur++;
                g_tt.st_ms = 0;
                g_tt.next_ms = now + 500;
            }
            return;
        }
        g_tt.tslot = slot;
        memcpy(g_tt.base_pos, T0, 12);
        g_tt.base_ang = (g_tt.cur * 67) % 360;  // different first direction per job: not always the same spot
        for (int i = 0; i < 8; i++) {
            float dx, dz;
            TtDir(i, &dx, &dz);
            float T[3] =
                { T0[0] + dx * j->dist, T0[1], T0[2] + dz * j->dist };
            float chest[3] = { T0[0], T0[1] + 1.25f, T0[2] }, tp[3] =
                { T[0], T[1] + 0.5f, T[2] };
            g_tt.ray[i][0] = WorldRayKick(chest, tp); // chest -> landing point (wall between = cover)
            float ga[3] = { T[0], T[1] + 3.0f, T[2] }, gb[3] =
                { T[0], T[1] - 1.0f, T[2] };
            g_tt.ray[i][1] = WorldRayKick(ga, gb);    // flat ground under the landing point, no ceiling within 3 m
            float ca[3] = { T[0], T[1] + 0.6f, T[2] }, cb[3] =
                { T[0], T[1] + 4.0f, T[2] };
            g_tt.ray[i][2] = WorldRayKick(ca, cb);    // open sky / high ceiling over it
        }
        g_tt.kframe = g_frame;
        g_tt.st = 1;
        return;
    }
    if (g_tt.st == 1) {
        if (g_frame == g_tt.kframe)
            return;
        int pick = -1, pick_any = -1;
        for (int k = 0; k < 8; k++) {   // collect every ray (no job left dangling)
            int i = k;
            float tc = RayT(g_tt.ray[i][0]), tg =
                RayT(g_tt.ray[i][1]), tr = RayT(g_tt.ray[i][2]);
            float len = __builtin_sqrtf(j->dist * j->dist + 0.75f * 0.75f);
            int blocked = tc >= 0.0f
                && tc < 0.999f, hitd = (int) (tc * len * 100.0f);
            float gy = 3.0f - tg * 4.0f;
            int ground = g_tt.ray[i][1] && tg >= 0.0f && tg < 0.999f && gy > -0.6f && gy < 0.6f;        // (+-0.35 skipped every cover case on Capitol)
            int sky = !(tr >= 0.0f && tr < 0.999f);
            if (!ground)
                continue;
            if (pick_any < 0)
                pick_any = i;
            if (pick >= 0)
                continue;
            if (j->mode == 1) {
                if (blocked && hitd >= 30
                    && hitd <= (int) (len * 100.0f) - 30)
                    pick = i;
            } else if (j->mode == 0) {
                if (!blocked && sky)
                    pick = i;
            } else
                pick = i;
        }
        if (pick < 0 && j->mode == 2)
            pick = pick_any;
        if (pick < 0) {
            Xlog("throwtest: #%d %s dist %.2f %s SKIPPED - none of the 8 directions around the target has %s", g_tt.cur + 1, TtKindName(j->kind), j->dist, j->mode == 1 ? "behind-cover" : "open", j->mode == 1 ? "a wall between it and the landing point (and flat ground)" : "open flat ground with a clear line");
            g_tt.cur++;
            g_tt.st = 0;
            g_tt.st_ms = 0;
            g_tt.next_ms = now + 500;
            return;
        }
        float dx, dz;
        TtDir(pick, &dx, &dz);
        float T[3] =
            { g_tt.base_pos[0] + dx * j->dist, g_tt.base_pos[1],
       g_tt.base_pos[2] + dz * j->dist };
        float cur[3];           // the target must not have moved while the rays were in flight
        if (!TtTargetPos((uint32_t) g_tt.tslot, cur)
            || __builtin_fabsf(cur[0] - g_tt.base_pos[0]) +
            __builtin_fabsf(cur[2] - g_tt.base_pos[2]) > 0.8f) {
            Xlog("throwtest: #%d target moved during the probe - retrying",
                 g_tt.cur + 1);
            g_tt.st = 0;
            g_tt.next_ms = now + 400;
            return;
        }
        int ang = (g_tt.base_ang + pick * 45) % 360;
        if (j->kind == TT_PLACED)
            TtPlace(j, g_tt.cur, T, ang);
        else
            TtThrow(j, g_tt.cur, T, dx, dz, ang);
        g_tt.st = 2;
        g_tt.st_ms = 0;
        g_tt.wait_until = now + (uint64_t) j->wait_s * 1000;
        return;
    }
}

// TARGET side: heal between blasts so each throw meets the same hp (only with t2r-throwtest-target.txt)
static float g_ttSpot[3];
static int g_ttSpotOn;
static void TtTargetTick(void)
{
    uint64_t now = GetTickCount64();
    if (now - g_tt_tpoll > 2000) {
        g_tt_tpoll = now;
        FILE *f = ExeSideOpen("t2r-throwtest-target.txt", "r");
        int was = g_tt_theal;
        g_tt_theal = f != NULL;
        g_tt_tvest = -1;
        if (f) {
            char b[160];
            size_t r = fread(b, 1, sizeof b - 1, f);
            b[r] = 0;
            fclose(f);
            const char *v = strstr(b, "vest=");
            if (v)
                g_tt_tvest = atoi(v + 5);
            const char *sp = strstr(b, "spot=");        // spot=x,y,z: the target stands here (open ground for 'open' tests - its own spawn can be enclosed)
            g_ttSpotOn = sp
                && sscanf(sp + 5, "%f,%f,%f", &g_ttSpot[0], &g_ttSpot[1],
                          &g_ttSpot[2]) == 3;
        }
        if (g_tt_theal != was)
            Xlog("throwtest: target mode %s (heal to full %d s after the last blast%s)", g_tt_theal ? "ON" : "off", 3, g_tt_tvest >= 0 ? ", vest set" : "");
    }
    if (!g_tt_theal || MatchSubstate() != 6)
        return;
    static float maxhp;
    float hp = LocalHp();
    if (hp > maxhp)
        maxhp = hp;
    if (g_downed && hp > 0.0f && now - g_ttLastMs > 3000) {   // (a downed target bled out and respawned mid-run - every throw after a
        static uint64_t upd;    // down hit a dead / respawning target): get up again like a revive
        if (now - upd > 3000) {
            upd = now;
            uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
            RevivedRecv(me ^ 1u);
            Xlog("throwtest: target got up after the down (revived_recv)");
        }
        return;
    }
    if (!(hp > 0.0f) || g_downed || maxhp < 1.0f)
        return;
    if (g_ttSpotOn && now - g_ttLastMs > 3000) {    // keep the target on its test spot (moved back after every respawn / push)
        float lb[16];
        const float *l = PlayerLocator(lb);
        if (l) {
            float dx = l[0] - g_ttSpot[0], dz = l[2] - g_ttSpot[2];
            if (dx * dx + dz * dz > 1.0f) {
                float l8[8] =
                    { g_ttSpot[0], g_ttSpot[1], g_ttSpot[2], 1.0f, l[4],
              l[5], l[6], l[7] };
                TeleportPlayerTo(l8, l8 + 4); // (set-object-locator over many m launched the player)
                Xlog("throwtest: target moved to its spot %.2f %.2f %.2f",
                     g_ttSpot[0], g_ttSpot[1], g_ttSpot[2]);
            }
        }
    }
    uint64_t quiet =
        g_ttLastMs > g_tt_theal_ms ? g_ttLastMs : g_tt_theal_ms;
    if (now - quiet < 3000)
        return;
    int needvest = g_tt_tvest >= 0 && (int) (g_vest + 0.5f) != g_tt_tvest;
    if (hp >= maxhp - 0.5f && !needvest)
        return;
    g_tt_theal_ms = now;
    if (hp < maxhp - 0.5f) {
        uint64_t a[2] = { SID_PLAYER, 0 };
        memcpy(&a[1], &maxhp, sizeof maxhp);
        DcCall(0xcac2cb5a30fccf12ULL /* set-object-health */ , 2, a);
    }
    if (needvest)
        ArmorSet(g_helm, (float) g_tt_tvest);
    Xlog("throwtest: target healed to %.0f%s", maxhp,
         needvest ? " + vest set" : "");
}

static void ThrowtestTick(void)
{
    TtTargetTick();
    TtThrowerTick();
}
#endif
