// ---------------------------------------------------------------- AUTOMATED PAIRED-MOVE TEST HARNESS (DEV builds only; #ifndef T2R_RELEASE, included from the dev block of t2r-ext.c)
//
// Tests the paired moves between the two local instances A (attacker) and B (victim) with NO synthetic keyboard / mouse: the E press and the revive hold are
// injected INTO the game's own interact reads (interact_pressed / interact_down), so the real execute_enemy_tick / shiv_enemy_tick / revive tick run with all
// their gates (range, front half plane, line of sight, downed delay, stunned / rear arc, shiv in the pack, weapon in hand ...); the contextual takedown goes through
// cd_swing_start (the downing swing's start) / cd_try. Everything else is the real path: psync START / ACK / END, the victim's own client kills / downs it.
// Nothing runs without a command file. Release builds contain none of this.
//
//        dodge [weapon=<id>] [late=<ms>] [none] [oneshot]  melee dodge test: A swings at B (melee press injected into the joypad-command-active? reads), B presses evade late ms (default 120) after its window opens, or never (none)
//   ATTACKER (A):  t2r-pairtest.txt next to the exe. Read once (then deleted), one job per line, '#' comments:
//        exec gun|melee|fists|knife   special execution (E on a downed enemy); the move is picked by the weapon in hand exactly as a real press
//        shiv stunned|back            2013 shiv kill: stunned target (front / any side) or a target seen from behind
//        takedown [wall|cover|any]    contextual takedown (ctxdown.c) of a standing target that is one hit from going down, wall / low cover behind it
//        revive                       revive hold (E held) on a downed target (an enemy is revived too, through the dev any-team switch)
//      options:  weapon=<id>  (given + drawn first: exec gun default rifle-vepr, exec melee default lead-pipe, fists / knife / shiv / takedown: hands empty unless given)
//                female       (the attacker's look must be female - the job is SKIPPED and logged when it is not; 'exec knife' implies it)
//                wait=<s>     seconds after the move ends before the next job (default 7)     to=<slot>  target slot (default: first other live player; "slot <n>" line = default)
//                side=<0-3>   (exec / revive / shiv stunned) A stands in front / behind / left / right of the target (default: cycles with the job number; a side that does
//                             not start the move in 4 s is replaced by the next one)       dist=<m>  distance to the target (default 1.3, shiv 1.6 / 1.5)
//                spot=x,y,z   the target is teleported there first (needs B's file)        face=<deg>  the target is turned to that yaw first
//                hp=<n>       (takedown) the target's hp when the swing downs it (default 12)     down=hit|self  how the target is downed for exec / revive (default: A's real
//                             down hit on an enemy, B downs itself for a teammate)
//      Per job: wait for a live, healthy, standing target (B is healed / revived / un-stunned through the peer message), make the target state (down / stun / hp / back turned),
//      teleport A to the move's start (same code as the dev t2r-tp.txt), press, wait for the psync END, wait, log. Murphy: B dead / left / downed / stunned -> the job waits (120 s),
//      A dead -> the job is aborted and logged, a side that never starts is rotated, then the job is logged "NOT STARTED" with the gate snapshot.
//      Log lines (A):  "pairtest: #k <kind> begin|STARTED|NOT STARTED|SKIPPED|RESULT|done ... tms N"
//
//   TARGET (B):    t2r-pairtest-target.txt next to the exe (polled every 2 s; optional token vest=<n>): B then obeys A's dev-only peer messages (type 0x92: heal, stun,
//      down itself, get up, set hp, teleport / turn, status). Without the file B ignores every one of them - nobody is ever moved or hurt without it.
//      Log lines (B, victim side): "pairtest: v ..." and, on BOTH instances, "pairtest: ps START|KILLPT|END|SUMMARY ..." (u<uid> joins the two logs).
//
//   Report: python tools/win/pairtest_report.py <A t2r-ext.log> <B t2r-ext.log>
#ifndef T2R_RELEASE
#define PT_MAXJOB 64
#define MSG_PAIRTEST 0x92
enum { PK_EXEC = 1, PK_SHIV, PK_TAKEDOWN, PK_REVIVE, PK_DODGE };
enum { PT_HEAL = 1, PT_TP, PT_STUN, PT_DOWNSELF, PT_UP, PT_HP, PT_STATUS_REQ, PT_STATUS, PT_DODGE };    // PT_DODGE: arms B for a "dodge" job (flags bit0 = no press, f[0] = press delay ms)
enum { PST_PICK, PST_PREP, PST_SETUP, PST_STATE, PST_TRIG, PST_END,
        PST_TAIL, PST_FINAL };
typedef struct __attribute__((packed)) {
    uint8_t op, k, target, flags;
    float f[4];
} PtMsg;                       // TP flags bit0 = yaw only; STATUS flags: 1 alive 2 downed 4 stunned 8 in a move 16 dying

typedef struct {
    int kind, sub, slot, side, female, wait_s, has_spot, has_face, down,
        late, none, oneshot, extkey;
    float dist, hp, face, spot[3];
    char weapon[48];
    char text[110];
} PtJob;
static struct {
    int n, cur, st, ss, slot_default, tslot, side, side_try, wtry, stry,
        ring, started, vic_dead_view, shiv0, wready, nofb, stripped;
    uint64_t rdy_ms, poll_f, next_ms, st_ms, log_ms, ss_ms, att_ms,
        judge_ms, hold_ms, end_ms, draw_ms;
    uint32_t uid;
    PtJob job[PT_MAXJOB];
} g_pt = {.slot_default = -1 };

static struct {
    int valid, k;
    uint64_t ms;
    int flags;
    float hp, maxhp, vest;
} g_ptBst;                     // B's last status as A sees it
static int g_pt_bacc, g_pt_btvest = -1; // B: the target file exists / its vest=
static uint64_t g_ptBpoll;
static float g_ptMaxhp;
static int g_ptSeen;           // a pairtest session is going on here (the watch logs)
#define PT_MS ((unsigned long long)GetTickCount64())

static const char *PtKname(const PtJob *j)
{
    static const char *E[] =
        { "exec gun", "exec melee", "exec fists", "exec knife" }, *S[] =
        { "shiv stunned", "shiv back" }, *T[] =
        { "takedown any", "takedown wall", "takedown cover" };
    if (j->kind == PK_DODGE)
        return j->none ? "dodge none" : "dodge";
    return j->kind == PK_EXEC ? E[j->sub & 3] : j->kind ==
        PK_SHIV ? S[j->sub & 1] : j->kind ==
        PK_TAKEDOWN ? T[j->sub % 3] : "revive";
}

static const char *PtWclass(uint64_t w)
{
    if (!w || w == ~0ULL)
        return "none";
    if (w == FNV64("shiv"))
        return "shiv";
    if (w == FNV64("abby-unarmed"))
        return "fists";
    if (KnifeWeaponId(w))
        return "knife";
    if (BaseWeaponIsGun(w))
        return "gun";
    if (MeleeClassOf(w, NULL) >= 0)
        return "melee";
    return "other";
}

static void PtSend(int op, int k, int target, int flags, float a, float b,
                    float c, float d)
{
    PtMsg m =
        { (uint8_t) op, (uint8_t) k, (uint8_t) target, (uint8_t) flags,
     { a, b, c, d } };
    static int nl;
    if (nl < 300 && op != PT_STATUS_REQ && op != PT_STATUS) {
        nl++;
        Xlog("pairtest: dbg send op %d k %d target %d flags %d (%.1f %.1f %.1f %.1f) tramp %p tms %llu", op, k, target, flags, a, b, c, d, (void *) g_sendTramp, PT_MS);
    } else if (op == PT_STATUS) {
        static int ns;
        if (LOGCAP(ns, 40))
            Xlog("pairtest: dbg send STATUS k %d to slot %d flags %d hp %.0f/%.0f tramp %p tms %llu", k, target, flags, a, b, (void *) g_sendTramp, PT_MS);
    }
    if (g_sendTramp)
        g_sendTramp(MSG_PAIRTEST, (uint8_t *) & m, sizeof m, 0xff);
}

static void PtRemove(const char *name)
{
    char pa[MAX_PATH];
    GetModuleFileNameA(NULL, pa, MAX_PATH);
    char *sa = strrchr(pa, 92);
    if (sa) {
        strcpy(sa + 1, name);
        remove(pa);
    }
}

// the other player's puppet locator (pos xyzw, quat xyzw) on OUR screen
static int PtPuppet(uint32_t slot, float *out8)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint8_t *pe =
        slot < T2R_MAX_PEERS ? ((fn_peer) T2rPeer) (slot) : NULL;
    uint64_t h = pe && pe[4] && !pe[5] ? *(uint64_t *) (pe + 0x60) : 0;
    float b[16];
    const float *l = h ? PuppetLocator(h, b) : NULL;
    if (!l || !PsLocOk(l))
        return 0;
    memcpy(out8, l, 32);
    return 1;
}

static void PtTp(const float *pos, float yaw_rad)
{
    float rot[4] =
        { 0.0f, __builtin_sinf(yaw_rad * 0.5f), 0.0f,
 __builtin_cosf(yaw_rad * 0.5f) };
    TeleportPlayerTo(pos, rot);
}

// ---------------------------------------------------------------- TARGET side (B)
static void PtBPoll(void)
{
    uint64_t now = GetTickCount64();
    if (now - g_ptBpoll < 2000)
        return;
    g_ptBpoll = now;
    FILE *f = ExeSideOpen("t2r-pairtest-target.txt", "r");
    int was = g_pt_bacc;
    g_pt_bacc = f != NULL;
    g_pt_btvest = -1;
    if (f) {
        char b[160];
        size_t r = fread(b, 1, sizeof b - 1, f);
        b[r] = 0;
        fclose(f);
        const char *v = strstr(b, "vest=");
        if (v)
            g_pt_btvest = atoi(v + 5);
    }
    if (g_pt_bacc != was)
        Xlog("pairtest: target mode %s (this instance obeys the attacker's pairtest messages%s) tms %llu", g_pt_bacc ? "ON" : "off", g_pt_btvest >= 0 ? ", vest set at every heal" : "", PT_MS);
    if (g_pt_bacc)
        g_ptSeen = 1;
}

static int PtFlagsNow(void)
{
    int f = 0;
    if (LocalHp() > 0.0f && MatchSubstate() == 6 && !g_downDying)
        f |= 1;
    if (g_downed)
        f |= 2;
    if (g_stun_until && g_t60 < g_stun_until)
        f |= 4;
    if (g_ps.role)
        f |= 8;
    if (g_downDying || g_uc4DyingUntil)
        f |= 16;
    return f;
}

static void PtRecv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    if (!buf || len < sizeof(PtMsg))
        return;
    PtMsg m;
    memcpy(&m, buf, sizeof m);
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    {
        static int nr;
        if (nr++ < 120 || m.op == PT_STATUS) {
            static int ns;
            if (m.op != PT_STATUS || ns++ < 40)
                Xlog("pairtest: dbg recv op %u k %u target %u flags %u from slot %u (self %u, target mode %d, len %llu) tms %llu", m.op, m.k, m.target, m.flags, from, self, g_pt_bacc, (unsigned long long) len, PT_MS);
        }
    }
    if (m.op == PT_STATUS) {    // A: B's answer
        g_ptBst.valid = 1;
        g_ptBst.k = m.k;
        g_ptBst.ms = GetTickCount64();
        g_ptBst.flags = m.flags;
        g_ptBst.hp = m.f[0];
        g_ptBst.maxhp = m.f[1];
        g_ptBst.vest = m.f[2];
        return;
    }
    if (m.target != self)
        return;
    if (!g_pt_bacc) {
        static int nw;
        if (LOGCAP(nw, 3))
            Xlog("pairtest: v message op %u from slot %u IGNORED - no t2r-pairtest-target.txt here tms %llu", m.op, from, PT_MS);
        return;
    }
    int fl = PtFlagsNow();
    switch (m.op) {
    case PT_STATUS_REQ:
        PtSend(PT_STATUS, m.k, (int) from, fl, LocalHp(), g_ptMaxhp,
                g_vest, 0);
        break;
    case PT_HEAL:{
            if (!(fl & 1) || (fl & 2)) {
                Xlog("pairtest: v #%u heal skipped (flags %d) tms %llu",
                     m.k, fl, PT_MS);
                break;
            }
            float hp = LocalHp();
            if (hp < g_ptMaxhp - 0.5f && g_ptMaxhp > 1.0f) {
                uint64_t a[2] = { SID_PLAYER, 0 };
                memcpy(&a[1], &g_ptMaxhp, sizeof g_ptMaxhp);
                DcCall(0xcac2cb5a30fccf12ULL, 2, a);
            }
            if (g_pt_btvest >= 0)
                ArmorSet(g_helm, (float) g_pt_btvest);
            Xlog("pairtest: v #%u healed %.0f -> %.0f tms %llu", m.k, hp,
                 g_ptMaxhp, PT_MS);
            break;
        }
    case PT_HP:{
            if (!(fl & 1) || (fl & 2))
                break;
            uint64_t a[2] = { SID_PLAYER, 0 };
            memcpy(&a[1], &m.f[0], sizeof(float));
            DcCall(0xcac2cb5a30fccf12ULL, 2, a);
            Xlog("pairtest: v #%u hp set to %.0f tms %llu", m.k, m.f[0],
                 PT_MS);
            break;
        }
    case PT_DODGE:
        if (fl & 1)
            MdgtBArm(m.k, m.flags & 1, (int) m.f[0]);
        break;
    case PT_STUN:
        if ((fl & 1) && !(fl & 2)) {
            StunLocal(from);
            Xlog("pairtest: v #%u stunned on request tms %llu", m.k,
                 PT_MS);
        }
        break;
    case PT_DOWNSELF:
        if ((fl & 1) && !(fl & 2) && !(fl & 8)) {
            g_down_slot = from;
            DownedSet(1);
            Xlog("pairtest: v #%u downed itself (as a teammate's test down) tms %llu", m.k, PT_MS);
        }
        break;
    case PT_UP:
        if ((fl & 2) && !(fl & 8)) {
            Xlog("pairtest: v #%u getting up (revived_recv) tms %llu", m.k,
                 PT_MS);
            RevivedRecv(from);
        }
        break;
    case PT_TP:{
            if (!(fl & 1) || (fl & 2) || (fl & 8)) {
                Xlog("pairtest: v #%u teleport refused (flags %d) tms %llu", m.k, fl, PT_MS);
                break;
            }
            float lb[16];
            const float *l = PlayerLocator(lb);
            float pos[3] = { m.f[0], m.f[1], m.f[2] };
            if (m.flags & 1) {
                if (!l)
                    break;
                memcpy(pos, l, 12);
            }
            PtTp(pos, m.f[3] * 0.0174532925f);
            Xlog("pairtest: v #%u moved to %.2f %.2f %.2f yaw %.0f tms %llu", m.k, pos[0], pos[1], pos[2], m.f[3], PT_MS);
            break;
        }
    }
}

// ---------------------------------------------------------------- the watch (BOTH instances): every paired move this instance plays a half of
static struct {
    int role, kind, att, vic, kill_done, deaths, vic_dead_n, alive_prev,
        tail, cam_ms, vic_dead_prev, downed_end;
    uint32_t uid;
    double t0_kill;
    uint64_t start_ms, end_ms;
} W;
static int PtCamsOn(void)
{
    return g_xcamOn || g_fincamOn;
}

static void PtWatchSummary(const char *why)
{
    if (!W.tail)
        return;
    Xlog("pairtest: ps SUMMARY u%08x role=%d kind=%d deaths=%d vic_dead_view=%d downed=%d hp=%.1f cam_ms=%d cams_now=%d/%d/%d why=%s tms %llu", W.uid, W.role, W.kind, W.deaths, W.vic_dead_n, g_downed, LocalHp(), W.cam_ms, g_xcamOn, g_fincamOn, g_icam_on, why, PT_MS);
    W.tail = 0;
}

static void PtWatchTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (!g_ptSeen || MatchSubstate() != 6)
        return;
    uint64_t now = GetTickCount64();
    float lhp = LocalHp();
    if (lhp > g_ptMaxhp)
        g_ptMaxhp = lhp;
    int role = g_ps.role == 3 ? 0 : g_ps.role;
    if (W.role && (!role || g_ps.uid != W.uid)) {       // a move of ours ended (or another one replaced it)
        Xlog("pairtest: ps END u%08x role=%d kind=%d ms=%llu hp=%.1f downed=%d cams=%d/%d tms %llu", W.uid, W.role, W.kind, (unsigned long long) (now - W.start_ms), lhp, g_downed, g_xcamOn, g_fincamOn, PT_MS);
        W.end_ms = now;
        W.tail = 1;
        W.cam_ms = -1;
        W.role = 0;
    }
    if (role && !W.role) {
        PtWatchSummary("next move");
        memset(&W, 0, sizeof W);
        W.role = role;
        W.kind = g_ps.kind;
        W.uid = g_ps.uid;
        W.att = g_ps.att;
        W.vic = g_ps.vic;
        W.start_ms = now;
        W.alive_prev = lhp > 0.0f;
        uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
        uint64_t pw = 0;
        if (role == 2) {
            uint8_t *pe = g_ps.att >= 0
                && g_ps.att <
                T2R_MAX_PEERS ? ((fn_peer) T2rPeer) ((uint32_t) g_ps.
                                                      att) : NULL;
            pw = pe ? *(uint64_t *) (pe + 0x91) : 0;
        } else
            pw = CurrentWeapon();
        const char *own = role == 1 ? "attacker" : "victim";
        Xlog("pairtest: ps START u%08x role=%d (%s) kind=%d att=%d vic=%d self=%u att_clip=%s vic_clip=%s weapon=%llx wclass=%s screen_weapon=%llx screen_class=%s kill_s=%.2f dur=%.2f ap=%.2f,%.2f,%.2f tms %llu", g_ps.uid, role, own, g_ps.kind, g_ps.att, g_ps.vic, self, PsyncName(g_ps.att_sid), PsyncName(g_ps.vic_sid), (unsigned long long) g_ps.weapon, PtWclass(g_ps.weapon), (unsigned long long) pw, PtWclass(pw), g_ps.kill_s, g_ps.dur_s, g_ps.ap[0], g_ps.ap[1], g_ps.ap[2], PT_MS);
    }
    if (role && W.role) {
        double t = PsyncMs() - g_ps.t0;
        if (!W.kill_done && g_ps.kill_s > 0.0f && t >= (double) g_ps.kill_s * 1000.0) { // where each body is at the kill / hand-off frame: A's view of the victim vs the victim itself
            W.kill_done = 1;
            float lb[16], ob[8];
            const float *me = PlayerLocator(lb);
            int other = W.role == 1 ? g_ps.vic : g_ps.att, ok = other >= 0
                && PtPuppet((uint32_t) other, ob);
            uint8_t *pe = g_ps.att >= 0
                && g_ps.att <
                T2R_MAX_PEERS ? ((fn_peer) T2rPeer) ((uint32_t) g_ps.
                                                      att) : NULL;
            Xlog("pairtest: ps KILLPT u%08x role=%d t=%.0f own=%.3f,%.3f,%.3f other=%s%.3f,%.3f,%.3f hp=%.1f screen_weapon=%llx tms %llu", W.uid, W.role, t, me ? me[0] : 0.0f, me ? me[1] : 0.0f, me ? me[2] : 0.0f, ok ? "" : "NA:", ok ? ob[0] : 0.0f, ok ? ob[1] : 0.0f, ok ? ob[2] : 0.0f, lhp, (unsigned long long) (pe && W.role == 2 ? *(uint64_t *) (pe + 0x91) : CurrentWeapon()), PT_MS);
        }
    }
    if (W.role == 2 || W.tail) {        // the victim's own deaths (killed twice = 2)
        int alive = lhp > 0.0f;
        if (W.alive_prev && !alive) {
            W.deaths++;
            Xlog("pairtest: v DIED u%08x n=%d (hp %.1f) tms %llu", W.uid,
                 W.deaths, lhp, PT_MS);
        }
        W.alive_prev = alive;
    }
    if ((W.role == 1 || W.tail) && W.vic >= 0 && W.vic < T2R_MAX_PEERS) {       // the attacker's view of the victim's death
        int d = g_puppetDeadHere[W.vic] != 0;
        if (d && !W.vic_dead_prev) {
            W.vic_dead_n++;
            g_pt.vic_dead_view++;
            Xlog("pairtest: a VICDEAD u%08x slot=%d n=%d tms %llu", W.uid,
                 W.vic, W.vic_dead_n, PT_MS);
        }
        W.vic_dead_prev = d;
    }
    if (W.tail) {
        uint64_t since = now - W.end_ms;
        if (W.cam_ms < 0 && !PtCamsOn()) {
            W.cam_ms = (int) since;
            Xlog("pairtest: ps CAMERA back u%08x %d ms after the move end tms %llu", W.uid, W.cam_ms, PT_MS);
        }
        if (W.cam_ms < 0 && since > 500 && since < 560)
            Xlog("pairtest: ps CAMERA still on 500 ms after the move end u%08x (exec cam %d, finisher cam %d) tms %llu", W.uid, g_xcamOn, g_fincamOn, PT_MS);
        if (since > 3500)
            PtWatchSummary("tail over");
    }
}

// ---------------------------------------------------------------- ATTACKER side (A)
static void PtLoad(void)
{
    FILE *f = ExeSideOpen("t2r-pairtest.txt", "r");
    if (!f)
        return;
    char buf[8192];
    size_t r = fread(buf, 1, sizeof buf - 1, f);
    buf[r] = 0;
    fclose(f);
    PtRemove("t2r-pairtest.txt");
    memset(&g_pt.job, 0, sizeof g_pt.job);
    g_pt.n = g_pt.cur = g_pt.st = 0;
    g_pt.next_ms = 0;
    g_pt.slot_default = -1;
    for (char *ln = strtok(buf, "\r\n"); ln && g_pt.n < PT_MAXJOB;
         ln = strtok(NULL, "\r\n")) {
        while (*ln == ' ' || *ln == '\t')
            ln++;
        if (!*ln || *ln == '#')
            continue;
        char tk[12][64] = { { 0 } };
        int nt =
            sscanf(ln,
                   "%63s %63s %63s %63s %63s %63s %63s %63s %63s %63s %63s %63s",
                   tk[0], tk[1], tk[2], tk[3], tk[4], tk[5], tk[6], tk[7],
                   tk[8], tk[9], tk[10], tk[11]);
        if (nt >= 2 && !strcmp(tk[0], "slot")) {
            g_pt.slot_default = atoi(tk[1]);
            continue;
        }
        PtJob j;
        memset(&j, 0, sizeof j);
        j.slot = -1;
        j.side = -1;
        j.wait_s = 7;
        j.hp = 12.0f;
        j.dist = 0.0f;
        int first_opt = 1;
        if (!strcmp(tk[0], "exec")) {
            j.kind = PK_EXEC;
            j.sub = nt >= 2 && !strcmp(tk[1], "gun") ? 0 : nt >= 2
                && !strcmp(tk[1], "melee") ? 1 : nt >= 2
                && !strcmp(tk[1], "fists") ? 2 : nt >= 2
                && !strcmp(tk[1], "knife") ? 3 : -1;
            first_opt = 2;
            if (j.sub == 3)
                j.female = 1;
        } else if (!strcmp(tk[0], "shiv")) {
            j.kind = PK_SHIV;
            j.sub = nt >= 2 && !strcmp(tk[1], "stunned") ? 0 : nt >= 2
                && !strcmp(tk[1], "back") ? 1 : -1;
            first_opt = 2;
        } else if (!strcmp(tk[0], "takedown")) {
            j.kind = PK_TAKEDOWN;
            j.sub = nt >= 2 && !strcmp(tk[1], "wall") ? 1 : nt >= 2
                && !strcmp(tk[1], "cover") ? 2 : 0;
            first_opt = nt >= 2 && (!strcmp(tk[1], "wall")
                                    || !strcmp(tk[1], "cover")
                                    || !strcmp(tk[1], "any")) ? 2 : 1;
        } else if (!strcmp(tk[0], "revive"))
            j.kind = PK_REVIVE;
        else if (!strcmp(tk[0], "dodge")) {
            j.kind = PK_DODGE;
            j.late = 120;
        }
        if (!j.kind || j.sub < 0) {
            Xlog("pairtest: line \"%s\" ignored (exec gun|melee|fists|knife / shiv stunned|back / takedown [wall|cover|any] / revive / dodge [weapon=<id>] [late=<ms>] [none] [oneshot] [extkey]  + options)", ln);
            continue;
        }
        for (int i = first_opt; i < nt; i++) {
            if (!strcmp(tk[i], "female"))
                j.female = 1;
            else if (!strncmp(tk[i], "weapon=", 7))
                snprintf(j.weapon, sizeof j.weapon, "%s", tk[i] + 7);
            else if (!strncmp(tk[i], "wait=", 5))
                j.wait_s = atoi(tk[i] + 5);
            else if (!strncmp(tk[i], "to=", 3))
                j.slot = atoi(tk[i] + 3);
            else if (!strncmp(tk[i], "side=", 5))
                j.side = atoi(tk[i] + 5) & 3;
            else if (!strncmp(tk[i], "dist=", 5))
                j.dist = (float) atof(tk[i] + 5);
            else if (!strncmp(tk[i], "hp=", 3))
                j.hp = (float) atof(tk[i] + 3);
            else if (!strncmp(tk[i], "face=", 5)) {
                j.face = (float) atof(tk[i] + 5);
                j.has_face = 1;
            } else if (!strncmp(tk[i], "spot=", 5))
                j.has_spot =
                    sscanf(tk[i] + 5, "%f,%f,%f", &j.spot[0], &j.spot[1],
                           &j.spot[2]) == 3;
            else if (!strncmp(tk[i], "late=", 5))
                j.late = atoi(tk[i] + 5);
            else if (!strcmp(tk[i], "none"))
                j.none = 1;
            else if (!strcmp(tk[i], "oneshot"))
                j.oneshot = 1;
            else if (!strcmp(tk[i], "extkey"))
                j.extkey = 1;
            else if (!strcmp(tk[i], "down=hit"))
                j.down = 1;
            else if (!strcmp(tk[i], "down=self"))
                j.down = 2;
        }
        if (j.wait_s < 2 || j.wait_s > 120)
            j.wait_s = 7;
        if (j.dist < 0.4f || j.dist > 4.0f)
            j.dist =
                j.kind == PK_SHIV ? (j.sub ? 1.5f : 1.6f) : j.kind ==
                PK_REVIVE ? 1.2f : j.kind == PK_TAKEDOWN ? 1.2f : 1.3f;
        snprintf(j.text, sizeof j.text, "%s", ln);
        g_pt.job[g_pt.n++] = j;
    }
    g_ptSeen = 1;
    Xlog("pairtest: loaded %d job(s) from t2r-pairtest.txt (A: moves and presses only through the game's own paths; B must have t2r-pairtest-target.txt) tms %llu", g_pt.n, PT_MS);
}

static int PtPickSlot(const PtJob *j)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    if (j->slot >= 0)
        return j->slot;
    if (g_pt.slot_default >= 0)
        return g_pt.slot_default;
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

static uint64_t PtWeaponSid(const PtJob *j)
{
    if (j->weapon[0])
        return FNV64(j->weapon);
    if (j->kind == PK_EXEC && j->sub == 0)
        return FNV64("rifle-vepr");
    if (j->kind == PK_EXEC && j->sub == 1)
        return FNV64("lead-pipe");
    return 0;
}

// the weapon (or empty hands) for the job; 1 = done
static void PtEquip(const PtJob *j, int idx)
{
    uint64_t w = PtWeaponSid(j);
    if (w) {
        const char *id =
            j->weapon[0] ? j->weapon : j->sub ==
            0 ? "rifle-vepr" : "lead-pipe";
        // the gun's ammo id from the damage table (ammo-<weapon> is wrong for most: the bow's is ammo-arrow - the bow had 0 arrows,
        // the 2013 empty-magazine rule turned its execution into a fist / shiv one)
        char an[64];
        snprintf(an, sizeof an, "ammo-%s", id);
        for (size_t k = 0;
             k < sizeof DMG_AMMO_GUN / sizeof DMG_AMMO_GUN[0]; k++)
            if (!strcmp(DMG_AMMO_GUN[k].gun, id)) {
                snprintf(an, sizeof an, "%s", DMG_AMMO_GUN[k].ammo);
                break;
            }
        uint64_t ret[64] = { 0 }, give[4] = { w, 0, 0, 1 };
        if (g_give_orig)
            g_give_orig(ret, 4, give, 0);
        if (BaseWeaponIsGun(w))
            DcDirect(0x7258db3b9e03e625ULL, FNV64(an), 60, 0);
        uint64_t r =
            DcCall(FNV64("player-instant-draw-weapon-by-name"), 1, &w);
        if (BaseWeaponIsGun(w) && AmmoClip() < 1)
            AmmoSet(1);        // a loaded magazine: gun executions need >= 1 round
        Xlog("pairtest: #%d weapon %s given + drawn (-> %llx) tms %llu",
             idx + 1, id, (unsigned long long) r, PT_MS);
    } else {
        uint64_t z[2] = { 0, 0 };
        uint64_t r =
            DcCall(FNV64("holster-player-weapon-instantly"), 2, z);
        Xlog("pairtest: #%d hands emptied (holster-player-weapon-instantly -> %llx) tms %llu", idx + 1, (unsigned long long) r, PT_MS);
    }
}

// the hand holds what the job wants RIGHT NOW (the long gun is auto-stowed after ~4 s idle: 'exec gun' pressed E with empty hands)
static int PtHandOk(const PtJob *j)
{
    uint64_t w = PtWeaponSid(j);
    if (w)
        return XrHandOk(w);
    uint64_t c = CurrentWeapon();
    return c == 0 || c == ~0ULL || c == FNV64("abby-unarmed")
        || KnifeWeaponId(c) || c == g_femKnifeCur;
}

static void PtGateNote(uint32_t slot, const char *what)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    float t[8], lb[16];
    const float *me = PlayerLocator(lb);
    uint8_t *pe =
        slot < T2R_MAX_PEERS ? ((fn_peer) T2rPeer) (slot) : NULL;
    if (!me || !PtPuppet(slot, t)) {
        Xlog("pairtest: gate (%s): no locators tms %llu", what, PT_MS);
        return;
    }
    float dx = t[0] - me[0], dz = t[2] - me[2], dy = t[1] - me[1], d =
        __builtin_sqrtf(dx * dx + dz * dz);
    float fx = 2.0f * (me[4] * me[6] + me[7] * me[5]), fz =
        1.0f - 2.0f * (me[4] * me[4] + me[5] * me[5]);
    Xlog("pairtest: gate (%s): dist %.2f dy %.2f front_cos %.2f | peer downed %u (%llu frames ago) psync_busy %d dead_here %d peer+86 %u | we: can_act %d downed %d role %d gx_until %d held %llx rounds %d | interact edge %s tms %llu", what, d, dy, d > 0.01f ? (dx * fx + dz * fz) / d : 0.0f, g_peerDowned[slot], (unsigned long long) (g_peerDownAt[slot] ? g_frame - g_peerDownAt[slot] : 0), PsyncBusySlot(slot), g_puppetDeadHere[slot], pe ? pe[0x86] : 0, ActorCanAct(), g_downed, g_ps.role, g_gx.until && g_t60 < g_gx.until, (unsigned long long) XrHeld(), HeldClipRounds(), "injected", PT_MS);
}

// A stands at that side of the target (0 front, 1 behind, 2 left, 3 right of ITS facing), facing it
static int PtPlaceSide(uint32_t slot, int side, float dist)
{
    float t[8];
    if (!PtPuppet(slot, t))
        return 0;
    const float *q = t + 4;
    float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]), fz =
        1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]), l =
        __builtin_sqrtf(fx * fx + fz * fz);
    if (l < 1e-3f) {
        fx = 0.0f;
        fz = 1.0f;
        l = 1.0f;
    }
    fx /= l;
    fz /= l;
    float vx =
        side == 0 ? fx : side == 1 ? -fx : side == 2 ? fz : -fz, vz =
        side == 0 ? fz : side == 1 ? -fz : side == 2 ? -fx : fx;
    float pos[3] = { t[0] + vx * dist, t[1], t[2] + vz * dist };
    float yaw = __builtin_atan2f(t[0] - pos[0], t[2] - pos[2]);
    PtTp(pos, yaw);
    Xlog("pairtest: A moved to side %d of slot %u at %.2f m -> %.2f %.2f %.2f yaw %.0f (target at %.2f %.2f %.2f) tms %llu", side, slot, dist, pos[0], pos[1], pos[2], yaw * 57.2958f, t[0], t[1], t[2], PT_MS);
    return 1;
}

static int PtPlaceWorld(uint32_t slot, float ang_deg, float dist)
{
    float t[8];
    if (!PtPuppet(slot, t))
        return 0;
    float a = ang_deg * 0.0174532925f, pos[3] =
        { t[0] + __builtin_sinf(a) * dist, t[1],
t[2] + __builtin_cosf(a) * dist };
    PtTp(pos, __builtin_atan2f(t[0] - pos[0], t[2] - pos[2]));
    Xlog("pairtest: A moved to %.0f deg around slot %u at %.2f m -> %.2f %.2f %.2f tms %llu", ang_deg, slot, dist, pos[0], pos[1], pos[2], PT_MS);
    return 1;
}

static void PtDownTarget(const PtJob *j, int idx, uint32_t slot)
{
    typedef void (*fn_hit3)(uint32_t, uint64_t, float);
    int self_mode = j->down == 2 || (j->down == 0 && IsTeammate(slot));
    if (self_mode) {
        PtSend(PT_DOWNSELF, idx + 1, (int) slot, 0, 0, 0, 0, 0);
        Xlog("pairtest: #%d target asked to down itself (%s) tms %llu",
             idx + 1, IsTeammate(slot) ? "teammate" : "down=self", PT_MS);
    } else {
        ((fn_hit3) T2R_SYM(kFnApplyHit)) (slot, FNV64("abby-unarmed"),
                                          150.0f);
        Xlog("pairtest: #%d down hit sent to slot %u (the game's own hit send, abby-unarmed 150) tms %llu", idx + 1, slot, PT_MS);
    }
}

static void PtCleanup(void)
{
    g_pt_hold = 0;
    g_pt_anyteam = 0;
    g_ptPressF = 0;
}

static void PtNext(uint64_t now, uint64_t gap_ms)
{
    PtCleanup();
    g_pt.cur++;
    g_pt.st = PST_PICK;
    g_pt.ss = 0;
    g_pt.st_ms = 0;
    g_pt.next_ms = now + gap_ms;
    g_pt.started = 0;
    g_pt.uid = 0;
    g_pt.judge_ms = 0;
}

static void PtSkip(const PtJob *j, uint64_t now, const char *why)
{
    Xlog("pairtest: #%d %s SKIPPED - %s tms %llu", g_pt.cur + 1,
         PtKname(j), why, PT_MS);
    PtNext(now, 800);
}

// B's own state as OUR screen shows it: used after 5 s without a status reply (a lost status must not block a run)
static int PtBReadyFallback(uint32_t slot)
{
    return GetTickCount64() - g_pt.st_ms > 5000 && !(g_ptBst.valid
                                                     && g_ptBst.k ==
                                                     g_pt.cur + 1)
        && !g_peerDowned[slot] && !g_puppetDeadHere[slot]
        && PtPuppet(slot, (float[8]) { 0 });
}

static int PtBReady(void)
{
    return g_ptBst.valid && g_ptBst.k == g_pt.cur + 1
        && GetTickCount64() - g_ptBst.ms < 1500 && (g_ptBst.flags & 1)
        && !(g_ptBst.flags & (2 | 4 | 8 | 16))
        && g_ptBst.hp >= g_ptBst.maxhp - 1.0f;
}

// "dodge" job (dev): A swings at B through the injected melee read; B (armed by PT_DODGE) presses evade `late` ms after ITS window opens (or never: "none")
static void PtDodgeStep(PtJob *j, int idx, uint32_t slot, uint64_t now)
{
    switch (g_pt.ss) {
    case 0:
        if (IsTeammate(slot)) {
            PtSkip(j, now,
                    "the dodge test needs the two instances on ENEMY teams (a teammate never opens a window)");
            return;
        }
        PtSend(PT_DODGE, idx + 1, (int) slot, j->none ? 1 : 0,
                (float) j->late, 0, 0, 0);
        if (j->oneshot) {
            g_upgradeArmed = 1;
            Xlog("pairtest: #%d one-shot armed by the test (was_up_g = %d: the pair only runs with a really upgraded melee in hand) tms %llu", idx + 1, was_up_g, PT_MS);
        }
        if (!PtPlaceSide(slot, 0, 1.5f)) {
            PtSkip(j, now, "target puppet gone while placing");
            return;
        }
        g_pt.ss = 1;
        g_pt.ss_ms = now;
        g_pt.st_ms = now;
        g_pt.stry = 0;
        return;
    case 1:
        if (now - g_pt.ss_ms < 1500)
            return;             // B armed, both bodies settled
        if (!PtHandOk(j) && now - g_pt.draw_ms > 900) {
            g_pt.draw_ms = now;
            PtEquip(j, idx);
            return;
        }
        g_pt.att_ms = now;
        if (j->extkey) {
            g_mdgt.miss_n = 0;
            g_mdgt.miss_anim[0] = 0;
            g_pt.ss = 4;
            Xlog("pairtest: #%d dodge: EXTKEY - armed and placed; waiting up to 6 s for a REAL melee swing on A (press F) tms %llu", idx + 1, PT_MS);
            return;
        }
        MdgtAPressMelee(110);
        g_pt.ss = 2;
        Xlog("pairtest: #%d dodge: melee press injected (held %llx, late %d ms%s) tms %llu", idx + 1, (unsigned long long) XrHeld(), j->late, j->none ? ", B will not press" : "", PT_MS);
        return;
    case 4:                    // a real swing (the engine's melee start reads the pad below the script natives: the injected read is polled but starts nothing)
        if (g_mdg.my_swing_ms >= g_pt.att_ms) {
            g_pt.started = 1;
            g_pt.ss = 3;
            g_pt.att_ms = now;
            Xlog("pairtest: #%d dodge STARTED by a real press - swing announced %llu ms after the wait began tms %llu", idx + 1, (unsigned long long) (g_mdg.my_swing_ms - g_pt.att_ms), PT_MS);
            return;
        }
        if (now - g_pt.att_ms > 6000) {
            Xlog("pairtest: #%d dodge NOT STARTED - no real swing in 6 s tms %llu", idx + 1, PT_MS);
            g_pt.st = PST_TAIL;
            g_pt.end_ms = now;
            g_pt.st_ms = now;
        }
        return;
    case 2:
        if (g_mdg.my_swing_ms >= g_pt.att_ms) {
            g_pt.started = 1;
            g_pt.ss = 3;
            Xlog("pairtest: #%d dodge STARTED - the swing was announced %llu ms after the press (engine melee-attack reads that saw it: %u) tms %llu", idx + 1, (unsigned long long) (g_mdg.my_swing_ms - g_pt.att_ms), g_mdgt.melee_hits, PT_MS);
            return;
        }
        if (now - g_pt.att_ms > 2500) {
            if (g_pt.stry++ < 2) {
                Xlog("pairtest: #%d dodge: no swing in 2.5 s (melee read hits %u) - placing again tms %llu", idx + 1, g_mdgt.melee_hits, PT_MS);
                PtPlaceSide(slot, 0, 1.5f);
                g_pt.ss = 1;
                g_pt.ss_ms = now - 1000;
                return;
            }
            Xlog("pairtest: #%d dodge: the injected melee press never produced a swing (melee read hits %u) - waiting 6 s for a REAL press (F) on A tms %llu", idx + 1, g_mdgt.melee_hits, PT_MS);
            g_pt.att_ms = now;
            g_pt.ss = 4;
        }
        return;
    default:
        if (now - g_pt.att_ms < 4500)
            return;
        Xlog("pairtest: #%d dodge RESULT-A swing=%d attacker_miss=%d miss_anim=%s attacker_lock_ms=%d (miss tail x%.2f %d ms) one_shot_left=%d tms %llu", idx + 1, g_pt.started, g_mdgt.miss_n, g_mdgt.miss_anim[0] ? g_mdgt.miss_anim : "-", MD_ATK_RECOVER_MS, MD_MISS_SPEED, MD_MISS_MS, g_upgradeArmed, PT_MS);
        g_upgradeArmed = j->oneshot ? 0 : g_upgradeArmed;
        g_pt.st = PST_TAIL;
        g_pt.end_ms = now;
        g_pt.st_ms = now;
        return;
    }
}

static void PtATick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    uint64_t now = GetTickCount64();
    if (!g_pt.n) {
        if (g_frame - g_pt.poll_f < 30)
            return;
        g_pt.poll_f = g_frame;
        PtLoad();
        return;
    }
    if (MatchSubstate() != 6) {
        if (now - g_pt.log_ms > 10000) {
            g_pt.log_ms = now;
            Xlog("pairtest: waiting - not in a running match (job %d of %d) tms %llu", g_pt.cur + 1, g_pt.n, PT_MS);
        }
        return;
    }
    if (g_pt.cur >= g_pt.n) {
        PtCleanup();
        Xlog("pairtest: all %d job(s) done tms %llu", g_pt.n, PT_MS);
        g_pt.n = 0;
        return;
    }
    PtJob *j = &g_pt.job[g_pt.cur];
    int idx = g_pt.cur;
    uint32_t slot = (uint32_t) g_pt.tslot;
    // A died / went down mid-job: abort it (the move, if any, is psync's to clean up)
    if (g_pt.st >= PST_SETUP && g_pt.st <= PST_END
        && (LocalHp() <= 0.0f || (g_downed && j->kind != PK_REVIVE)
            || g_downDying)) {
        Xlog("pairtest: #%d %s ABORTED - the attacker died / went down (hp %.1f downed %d) tms %llu", idx + 1, PtKname(j), LocalHp(), g_downed, PT_MS);
        PtNext(now, 3000);
        return;
    }
    if (j->kind == PK_DODGE && g_pt.st >= PST_STATE && g_pt.st <= PST_END) {
        PtDodgeStep(j, idx, slot, now);
        return;
    }
    switch (g_pt.st) {
    case PST_PICK:{
            if (now < g_pt.next_ms)
                return;
            if (!g_pt.st_ms)
                g_pt.st_ms = now;
            int s = PtPickSlot(j);
            float t[8];
            const char *why = NULL;
            if (s < 0 || s >= T2R_MAX_PEERS)
                why = "no target player in the match";
            else if (LocalHp() <= 0.0f || g_downed || g_downDying
                     || g_ps.role)
                why = "we are dead / downed / in a move";
            else if (!PtPuppet((uint32_t) s, t))
                why = "target dead / respawning / no puppet";
            if (why) {
                if (now - g_pt.log_ms > 10000) {
                    g_pt.log_ms = now;
                    Xlog("pairtest: #%d waiting - %s tms %llu", idx + 1,
                         why, PT_MS);
                }
                if (now - g_pt.st_ms > 120000)
                    PtSkip(j, now, why);
                return;
            }
            if (j->female && !g_femEllieOn) {
                PtSkip(j, now,
                        "needs a female attacker look (g_fem_ellie_on = 0) - pick the female look first");
                return;
            }
            g_pt.tslot = s;
            g_pt.st = PST_PREP;
            g_pt.st_ms = now;
            g_pt.ss_ms = 0;
            g_ptBst.valid = 0;
            g_pt.wready = 0;
            g_pt.nofb = 0;
            g_pt.stripped = 0;
            g_pt.draw_ms = 0;
            g_pt.side = j->side >= 0 ? j->side : idx & 3;
            g_pt.side_try = g_pt.wtry = g_pt.stry = 0;
            g_pt.vic_dead_view = 0;
            Xlog("pairtest: #%d %s begin (target slot %d%s, %s; line \"%s\") tms %llu", idx + 1, PtKname(j), s, IsTeammate((uint32_t) s) ? " = teammate" : "", OwnFemale()? "attacker look FEMALE" : "attacker look male", j->text, PT_MS);
            return;
        }
    case PST_PREP:{            // B healed / revived / un-stunned (it keeps nothing of the last job)
            if (now - g_pt.ss_ms >= 1000) {
                g_pt.ss_ms = now;
                if (g_ptBst.valid && g_ptBst.k == idx + 1
                    && (g_ptBst.flags & 2) && !(g_ptBst.flags & 8))
                    PtSend(PT_UP, idx + 1, (int) slot, 0, 0, 0, 0, 0);
                else
                    PtSend(PT_HEAL, idx + 1, (int) slot, 0, 0, 0, 0, 0);
                PtSend(PT_STATUS_REQ, idx + 1, (int) slot, 0, 0, 0, 0, 0);
            }
            int fb = PtBReadyFallback(slot);
            if (fb && !g_pt.nofb) {
                g_pt.nofb = 1;
                Xlog("pairtest: #%d no status reply from slot %u in 5 s - going on with our own view of it (puppet alive, not downed, not dead) tms %llu", idx + 1, slot, PT_MS);
            }
            if (!(PtBReady() || fb) || g_peerDowned[slot]
                || g_puppetDeadHere[slot] || g_ps.role
                || !PtPuppet(slot, (float[8]) { 0 })) {
                if (now - g_pt.st_ms > 120000) {
                                                PtSkip(j, now,
                                                        "the target never became ready (alive, standing, healthy, not stunned)");
                                                return;
                                                }
                                                if (now - g_pt.log_ms >
                                                    10000) {
                                                g_pt.log_ms = now;
                                                Xlog
                                                ("pairtest: #%d waiting - target not ready (status flags %d hp %.0f/%.0f, peer downed %u dead_here %u) tms %llu",
                                                 idx + 1, g_ptBst.flags,
                                                 g_ptBst.hp,
                                                 g_ptBst.maxhp,
                                                 g_peerDowned[slot],
                                                 g_puppetDeadHere[slot],
                                                 PT_MS);}
                                                g_pt.wready = 0; return;}
                                                if (!g_pt.wready) {
                                                g_pt.wready = 1;
                                                g_pt.rdy_ms = now;}
                                                if (now - g_pt.rdy_ms < 8000) return;   // ready for 8 s running: a puppet that respawned < 6 s ago has the pose-stall check clear its downed state (revive aborted 'not down any more')            // let B stand for a moment (its puppet settles)
                                                g_pt.wready = 0;
                                                g_pt.nofb = 0;
                                                g_pt.st = PST_SETUP;
                                                g_pt.st_ms = now;
                                                g_pt.ss = 0;
                                                Xlog
                                                ("pairtest: #%d target ready (hp %.0f/%.0f) tms %llu",
                                                 idx + 1, g_ptBst.hp,
                                                 g_ptBst.maxhp, PT_MS);
                                                return;}
    case PST_SETUP:                            {
                                                if (g_pt.ss == 6) {
                                                if (now - g_pt.ss_ms <
                                                    1800) return;
                                                g_pt.ss = 0;}
                                                if (g_pt.ss == 0) {
                                                if (!PtWeaponSid(j) && !g_pt.stripped) {      // empty hands: a melee weapon in the pack is used by the empty-handed melee / execution ('exec fists' played the pipe) - break it
                                                uint64_t n0 = 0, has =
                                                DcCall
                                                (0x79763b864843c645ULL
                                                 /* player-has-non-basic-melee-weapon? */
                                                 , 0, &n0);
                                                uint64_t mw = has != ~0ULL && (has & 0xff) ? XrMeleeWeapon(0) : 0; g_pt.stripped = 1; if (mw) {       // first: out of the pack (add-item-quantity -n); the break below stays as the fallback (run 3: the machete's health set did nothing)
                                                uint64_t q0 = InvQty(mw);
                                                if (q0)
                                                DcDirect
                                                (0x7258db3b9e03e625ULL, mw,
                                                 (uint64_t) (-(int64_t)
                                                             q0), 0);
                                                uint64_t q1 = InvQty(mw);
                                                Xlog
                                                ("pairtest: #%d melee weapon %llx removed from the pack: quantity %llu -> %llu tms %llu",
                                                 idx + 1,
                                                 (unsigned long long) mw,
                                                 (unsigned long long) q0,
                                                 (unsigned long long) q1,
                                                 PT_MS);}
                                                if (mw && InvQty(mw)) {
                                                uint64_t r =
                                                XrDraw(mw), zero = 0, r2 =
                                                DcCall
                                                (SID_SET_WEAPON_HEALTH2, 1,
                                                 &zero);
                                                Xlog
                                                ("pairtest: #%d melee weapon %llx in the pack - drawn (%llx) and broken (weapon health 0 -> %llx) so the hands are really empty tms %llu",
                                                 idx + 1,
                                                 (unsigned long long) mw,
                                                 (unsigned long long) r,
                                                 (unsigned long long) r2,
                                                 PT_MS); g_pt.ss = 6;
                                                g_pt.ss_ms = now; return;}
                                                }
                                                if (j->kind == PK_TAKEDOWN
                                                    && !g_meleeDown) {
                                                PtSkip(j, now,
                                                        "contextual takedowns need the Melee Down rule (g_melee_down = 0)");
                                                return;}
                                                if (j->kind == PK_SHIV) {
                                                if (!InvQty
                                                    (FNV64("shiv")))
                                                DcDirect
                                                (0x7258db3b9e03e625ULL,
                                                 FNV64("shiv"), 1, 0);
                                                g_pt.shiv0 =
                                                (int)
                                                InvQty(FNV64("shiv"));
                                                Xlog
                                                ("pairtest: #%d shiv in the pack: %d tms %llu",
                                                 idx + 1, g_pt.shiv0,
                                                 PT_MS);}
                                                if (j->has_spot
                                                    || j->has_face) {
                                                float t[8];
                                                if (j->
                                                    has_spot)
                                                PtSend(PT_TP, idx + 1,
                                                        (int) slot, 0,
                                                        j->spot[0],
                                                        j->spot[1],
                                                        j->spot[2],
                                                        j->has_face ? j->
                                                        face : 0.0f);
                                                else
                                                if (PtPuppet(slot, t))
                                                PtSend(PT_TP, idx + 1,
                                                        (int) slot, 1, 0,
                                                        0, 0, j->face);}
                                                PtEquip(j, idx);
                                                g_pt.ss = 1;
                                                g_pt.ss_ms = now; return;}
                                                if (now - g_pt.ss_ms < (j->has_spot ? 1800u : 1300u)) return;   // the weapon draw / the target's move settle
                                                uint64_t w =
                                                PtWeaponSid(j), held =
                                                XrHeld();
                                                int ok = PtHandOk(j);
                                                Xlog
                                                ("pairtest: #%d in hand: %llx (%s) wanted %llx (%s)%s tms %llu",
                                                 idx + 1,
                                                 (unsigned long long) held,
                                                 PtWclass(held),
                                                 (unsigned long long) w,
                                                 w ? PtWclass(w) :
                                                 "empty hands",
                                                 ok ? "" : " - MISMATCH",
                                                 PT_MS);
                                                if (!ok && g_pt.wtry < 2) {
                                                g_pt.wtry++; g_pt.ss = 0; return;}      // draw again (twice)
                                                g_pt.st = PST_STATE;
                                                g_pt.ss = 0;
                                                g_pt.ss_ms = now;
                                                g_pt.st_ms = now; return;}
    case PST_STATE:                            {
                                                // make the target state, per move
                                                if (j->kind == PK_EXEC
                                                    || j->kind ==
                                                    PK_REVIVE) {
                                                if (g_pt.ss == 0) {
                                                PtDownTarget(j, idx,
                                                               slot);
                                                g_pt.ss = 1;
                                                g_pt.ss_ms = now;
                                                g_pt.st_ms = now; return;}
                                                if (g_pt.ss == 1) {
                                                if (g_peerDowned[slot] ==
                                                    1) {
                                                g_pt.ss = 2;
                                                g_pt.ss_ms = now;
                                                Xlog
                                                ("pairtest: #%d target is downed on our screen after %llu ms tms %llu",
                                                 idx + 1,
                                                 (unsigned long long) (now
                                                                       -
                                                                       g_pt.
                                                                       st_ms),
                                                 PT_MS); return;}
                                                if (now - g_pt.st_ms >
                                                    8000) {
                                                if (g_pt.stry++ < 1) {
                                                g_pt.ss = 0;
                                                Xlog
                                                ("pairtest: #%d target did not go down in 8 s - sending the down again tms %llu",
                                                 idx + 1, PT_MS); return;}
                                                PtSkip(j, now,
                                                        "the target never went down");
                                                return;}
                                                return;}
                                                if (now - g_pt.ss_ms < 2200) return;    // fall + EXEC_DOWN_DELAY + the revive / execute gates
                                                }
                                                else
                                                if (j->kind == PK_SHIV
                                                    && j->sub == 0) {
                                                // A is placed FIRST (the stun lasts 3.5 s): side, then 0.7 s later the stun, then the press the moment the stun shows on our screen
                                                if (g_pt.ss == 0) {
                                                if (!PtPlaceSide
                                                    (slot,
                                                     (g_pt.side +
                                                      g_pt.side_try) & 3,
                                                     j->dist)) {
                                                PtSkip(j, now,
                                                        "target puppet gone while placing");
                                                return;}
                                                g_pt.ss = 1;
                                                g_pt.ss_ms = now; return;}
                                                if (g_pt.ss == 1) {
                                                if (g_pt.ss_ms
                                                    && now - g_pt.ss_ms <
                                                    700) return;
                                                PtSend(PT_STUN, idx + 1,
                                                        (int) slot, 0, 0,
                                                        0, 0, 0);
                                                g_pt.ss = 2;
                                                g_pt.ss_ms = now;
                                                g_pt.st_ms = now; return;}
                                                uint8_t * pe =
                                                ((fn_peer)
                                                 T2rPeer) (slot);
                                                uint64_t h =
                                                pe ? *(uint64_t *) (pe +
                                                                    0x60) :
                                                0;
                                                if (!h
                                                    || !ShivStunned(slot,
                                                                     h)) {
                                                if (now - g_pt.st_ms >
                                                    5000) {
                                                if (g_pt.stry++ < 1) {
                                                g_pt.ss = 1;
                                                g_pt.ss_ms = 0;
                                                Xlog
                                                ("pairtest: #%d the stun did not show on our screen in 5 s - asking again tms %llu",
                                                 idx + 1, PT_MS); return;}
                                                PtSkip(j, now,
                                                        "the target never showed as stunned");
                                                return;}
                                                return;}
                                                Xlog("pairtest: #%d target stunned on our screen after %llu ms tms %llu", idx + 1, (unsigned long long) (now - g_pt.st_ms), PT_MS); g_pt.st = PST_TRIG; g_pt.ss = 0; g_pt.ss_ms = now; g_pt.att_ms = now - 800; g_pt.judge_ms = 0; g_pt.st_ms = now;    // already placed: press at once
                                                return;}
                                                else
                                                if (j->kind == PK_SHIV) {
                                                // back turned: pick the spot around the target first, B turns away from it
                                                if (g_pt.ss == 0) {
                                                float t[8];
                                                if (!PtPuppet(slot, t)) {
                                                PtSkip(j, now,
                                                        "target puppet gone");
                                                return;}
                                                float ang = (float)((idx * 67 + g_pt.side * 90) % 360); float a = ang * 0.0174532925f, px = t[0] + __builtin_sinf(a) * j->dist, pz = t[2] + __builtin_cosf(a) * j->dist; float yawb = __builtin_atan2f(t[0] - px, t[2] - pz) * 57.2958f;        // B looks along A -> B: its back is to A
                                                PtSend(PT_TP, idx + 1,
                                                        (int) slot, 1, 0,
                                                        0, 0, yawb);
                                                g_pt.ring = (int) ang;
                                                g_pt.ss = 1;
                                                g_pt.ss_ms = now;
                                                g_pt.st_ms = now; return;}
                                                if (now - g_pt.ss_ms <
                                                    1000) return;}
                                                else
                                                if (j->kind == PK_TAKEDOWN) {
                                                if (g_pt.ss == 0) {
                                                PtSend(PT_HP, idx + 1,
                                                        (int) slot, 0,
                                                        j->hp, 0, 0, 0);
                                                g_pt.ss = 1;
                                                g_pt.ss_ms = now;
                                                g_pt.st_ms = now;
                                                g_pt.ring = 0; return;}
                                                if (now - g_pt.ss_ms <
                                                    900) return;}
                                                g_pt.st = PST_TRIG;
                                                g_pt.ss = 0;
                                                g_pt.ss_ms = 0;
                                                g_pt.att_ms = 0;
                                                g_pt.judge_ms = 0;
                                                g_pt.side_try = 0;
                                                g_pt.stry = 0;
                                                g_pt.st_ms = now; return;}
    case PST_TRIG:                             {
                                                // (re)place A for this attempt
                                                if (!g_pt.ss_ms) {
                                                int placed = 0;
                                                if (j->kind ==
                                                    PK_TAKEDOWN) placed =
                                                PtPlaceWorld(slot,
                                                               (float)
                                                               (g_pt.ring *
                                                                45),
                                                               j->dist);
                                                else
                                                if (j->kind == PK_SHIV
                                                    && j->sub ==
                                                    1) placed =
                                                PtPlaceWorld(slot,
                                                               (float)
                                                               g_pt.ring,
                                                               j->dist);
                                                else
                                                placed =
                                                PtPlaceSide(slot,
                                                              (g_pt.side +
                                                               g_pt.
                                                               side_try) &
                                                              3, j->dist);
                                                if (!placed) {
                                                PtSkip(j, now,
                                                        "target puppet gone while placing");
                                                return;}
                                                g_pt.ss_ms = now;
                                                g_pt.att_ms = now; return;}
                                                // started?
                                                if (g_ps.role == 1
                                                    && g_ps.uid) {
                                                g_pt.started = 1;
                                                g_pt.uid = g_ps.uid;
                                                const char *pn =
                                                j->kind == PK_TAKEDOWN
                                                && g_cd.plan_pair >=
                                                0 ? CD_PAIRS[g_cd.
                                                             plan_pair].
                                                name : "";
                                                Xlog
                                                ("pairtest: #%d %s STARTED uid=%08x psync_kind=%d att=%s vic=%s %s%s side=%d dist=%.2f after %llu ms tms %llu",
                                                 idx + 1, PtKname(j),
                                                 g_ps.uid, g_ps.kind,
                                                 PsyncName(g_ps.att_sid),
                                                 PsyncName(g_ps.vic_sid),
                                                 pn[0] ? "pair=" : "", pn,
                                                 (g_pt.side +
                                                  g_pt.side_try) & 3,
                                                 j->dist,
                                                 (unsigned long long) (now
                                                                       -
                                                                       g_pt.
                                                                       st_ms),
                                                 PT_MS); g_ptPressF = 0;
                                                g_pt.st = PST_END;
                                                g_pt.st_ms = now;
                                                g_pt.hold_ms = now;
                                                return;}
                                                if (j->kind != PK_TAKEDOWN
                                                    && g_gx.until
                                                    && g_t60 <
                                                    g_gx.until) {
                                                g_pt.started = 1;
                                                Xlog
                                                ("pairtest: #%d %s STARTED (legacy gun path, no psync uid) tms %llu",
                                                 idx + 1, PtKname(j),
                                                 PT_MS); g_ptPressF = 0;
                                                g_pt.st = PST_END;
                                                g_pt.st_ms = now;
                                                g_pt.hold_ms = now;
                                                return;}
                                                // drive
                                                uint64_t since =
                                                now - g_pt.att_ms;
                                                if (j->kind == PK_EXEC
                                                    || j->kind ==
                                                    PK_SHIV) {
                                                if (since > 700) {
                                                if (PtHandOk(j) || (j->kind == PK_SHIV && !PtWeaponSid(j))) g_ptPressF = g_frame; // press only with the right thing in the hand
                                                else
                                                if (now - g_pt.draw_ms >
                                                    900) {
                                                g_pt.draw_ms = now;
                                                Xlog
                                                ("pairtest: #%d hand is not what the job wants (held %llx, desired %llx) - drawing again before the press tms %llu",
                                                 idx + 1,
                                                 (unsigned long long)
                                                 XrHeld(),
                                                 (unsigned long long)
                                                 CurrentWeapon(), PT_MS);
                                                PtEquip(j, idx);}
                                                }
                                                }       // the press edge, after the placement settled (consumed by execute_enemy_tick / shiv_enemy_tick this frame)
                                                else
                                                if (j->kind == PK_REVIVE) {
                                                g_pt_anyteam = !IsTeammate(slot); if (since > 700) g_pt_hold = 1;}     // Any-team first, the hold later: a hold that starts on the frame the target first qualifies meets the revive tick's wait_release of the previous revive (E must be seen released at a target) and never starts
                                                else
                                                if (j->kind == PK_TAKEDOWN
                                                    && since > 600
                                                    && g_cd.plan_ok
                                                    && g_cd.plan_slot ==
                                                    (int32_t) slot
                                                    && !(g_frame % 6)) {
                                                int kind =
                                                CD_PAIRS[g_cd.plan_pair].
                                                kind;
                                                if ((j->sub == 1
                                                     && kind != 1)
                                                    || (j->sub == 2
                                                        && kind != 2)) {
                                                if (since > 1600) {
                                                g_pt.ss_ms = 0;
                                                g_pt.ring++;
                                                Xlog
                                                ("pairtest: #%d plan is %s, wanted %s - next ring position tms %llu",
                                                 idx + 1,
                                                 CD_PAIRS[g_cd.plan_pair].
                                                 name,
                                                 j->sub ==
                                                 1 ? "wall" : "cover",
                                                 PT_MS);}
                                                }
                                                else {
                                                int r = CdSwingStart();
                                                if (!r) {
                                                r = CdTry((int32_t) slot);
                                                Xlog
                                                ("pairtest: #%d takedown plan %s: swing-start path declined, contact path (cd_try) -> %d tms %llu",
                                                 idx + 1,
                                                 CD_PAIRS[g_cd.plan_pair].
                                                 name, r, PT_MS);}
                                                else
                                                Xlog("pairtest: #%d takedown plan %s: started through cd_swing_start tms %llu", idx + 1, CD_PAIRS[g_cd.plan_pair].name, PT_MS); g_pt.judge_ms = now + 3000;     // wait for the psync (STARTED line above) a few seconds before judging
                                                }
                                                }
                                                uint64_t limit =
                                                j->kind ==
                                                PK_TAKEDOWN ? 2200 : j->
                                                kind ==
                                                PK_REVIVE ? 6000 : 4000;
                                                if (since > limit
                                                    && now >=
                                                    g_pt.judge_ms) {
                                                int tries =
                                                j->kind ==
                                                PK_TAKEDOWN ? g_pt.ring +
                                                1 : j->kind == PK_SHIV
                                                && j->sub ==
                                                1 ? ++g_pt.stry : g_pt.
                                                side_try + 1;
                                                int maxtry =
                                                j->kind ==
                                                PK_TAKEDOWN ? 8 : 4;
                                                if (tries >= maxtry
                                                    || (j->kind == PK_SHIV
                                                        && tries >= 2)) {
                                                PtGateNote(slot,
                                                             j->kind ==
                                                             PK_TAKEDOWN ?
                                                             g_cd.
                                                             note :
                                                             "final");
                                                Xlog
                                                ("pairtest: #%d %s NOT STARTED after %d attempt(s) in %llu ms%s%s tms %llu",
                                                 idx + 1, PtKname(j),
                                                 tries,
                                                 (unsigned long long) (now
                                                                       -
                                                                       g_pt.
                                                                       st_ms),
                                                 j->kind ==
                                                 PK_TAKEDOWN ?
                                                 " - scan note: " : "",
                                                 j->kind ==
                                                 PK_TAKEDOWN ? g_cd.
                                                 note : "", PT_MS);
                                                g_ptPressF = 0;
                                                g_pt_hold = 0;
                                                g_pt.st = PST_TAIL;
                                                g_pt.end_ms = now;
                                                g_pt.st_ms = now; return;}
                                                PtGateNote(slot,
                                                             "attempt failed");
                                                if (j->kind == PK_SHIV
                                                    && j->sub == 0) {
                                                g_pt.side_try++; g_pt.st = PST_STATE; g_pt.ss = 0; g_pt.stry = 0; g_pt.st_ms = now; return;}    // a new stun from the next side
                                                if (j->kind ==
                                                    PK_TAKEDOWN) g_pt.
                                                ring++;
                                                else
                                                if (!(j->kind == PK_SHIV && j->sub == 1)) g_pt.side_try++;      // (a back-turned target keeps its spot: the same one again)
                                                g_pt.ss_ms = 0;
                                                g_pt.judge_ms = 0;
                                                Xlog
                                                ("pairtest: #%d no start in %llu ms - next %s tms %llu",
                                                 idx + 1,
                                                 (unsigned long long)
                                                 since,
                                                 j->kind ==
                                                 PK_TAKEDOWN ?
                                                 "ring position" : "side",
                                                 PT_MS);}
                                                return;}
    case PST_END:                              {
                                                uint64_t since = now - g_pt.st_ms; if (j->kind == PK_REVIVE) {  // the hold goes on until the target is up, then released
                                                if (g_pt_hold
                                                    && g_peerDowned[slot]
                                                    == 0 && since > 1500) {
                                                g_pt_hold = 0;
                                                Xlog
                                                ("pairtest: #%d revive finished on our screen after %llu ms - hold released tms %llu",
                                                 idx + 1,
                                                 (unsigned long long)
                                                 since, PT_MS);}
                                                if (since > 40000) {
                                                g_pt_hold = 0;
                                                Xlog
                                                ("pairtest: #%d revive did not finish in 40 s - hold released tms %llu",
                                                 idx + 1, PT_MS);}
                                                }
                                                if (!g_ps.role
                                                    && since > (j->kind ==
                                                                PK_REVIVE ?
                                                                2500u :
                                                                1000u)
                                                    && !(j->kind ==
                                                         PK_REVIVE
                                                         && g_pt_hold)) {
                                                g_pt.st = PST_TAIL;
                                                g_pt.end_ms = now;
                                                g_pt.st_ms = now;
                                                Xlog
                                                ("pairtest: #%d %s move over (%llu ms) tms %llu",
                                                 idx + 1, PtKname(j),
                                                 (unsigned long long)
                                                 since, PT_MS); return;}
                                                if (since > 60000) {
                                                Xlog
                                                ("pairtest: #%d %s - the move did not end in 60 s (role %d) tms %llu",
                                                 idx + 1, PtKname(j),
                                                 g_ps.role, PT_MS);
                                                g_pt_hold = 0;
                                                g_pt.st = PST_TAIL;
                                                g_pt.end_ms = now;
                                                g_pt.st_ms = now;}
                                                return;}
    case PST_TAIL:
    PtCleanup(); if (now - g_pt.end_ms < (uint64_t) j->wait_s * 1000) return; PtSend(PT_STATUS_REQ, idx + 1, (int) slot, 0, 0, 0, 0, 0); g_ptBst.valid = 0; g_pt.st = PST_FINAL; g_pt.st_ms = now; return; case PST_FINAL:
                                                if (now - g_pt.st_ms <
                                                    700) return;
                                                Xlog
                                                ("pairtest: #%d %s RESULT started=%d uid=%08x vic_dead_view=%d shiv %d -> %d target_status=%s flags=%d hp=%.0f weapon_now=%llx tms %llu",
                                                 idx + 1, PtKname(j),
                                                 g_pt.started, g_pt.uid,
                                                 g_pt.vic_dead_view,
                                                 g_pt.shiv0,
                                                 (int)
                                                 InvQty(FNV64("shiv")),
                                                 g_ptBst.
                                                 valid ? "ok" : "none",
                                                 g_ptBst.valid ? g_ptBst.
                                                 flags : -1,
                                                 g_ptBst.valid ? g_ptBst.
                                                 hp : 0.0f,
                                                 (unsigned long long)
                                                 CurrentWeapon(), PT_MS);
                                                Xlog
                                                ("pairtest: #%d %s done tms %llu",
                                                 idx + 1, PtKname(j),
                                                 PT_MS);
                                                PtNext(now, 1500);
                                                return;}
                                                }

                                                static void
                                                pairtest_tick(void) {
                                                PtATick();}

                                                static void BotTick(void);     // bot.c (t2r-bot.txt command program)
                                                static void pt_always_tick(void) {      // from feature_finpos (every frame, also while we are downed / dying)
                                                PtBPoll();
                                                PtWatchTick();
                                                BotTick();}
#endif
