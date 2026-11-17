// melee_drop.c (v5-dis4: pin deaths and other forms of dying also drop the melee weapon, so a not triggered disarm still allows the
// killer to take the weapon; an enemy with a melee weapon, when punched, takes it out).
//
// ROOT CAUSES (from test logs):
//  1. No killed player ever dropped a melee weapon: the only killed-player melee drop was drop_melee_pickup(g_peer_melee[slot]) inside the post-death
//     sweep, and g_peer_melee[slot] is set only when THAT slot had finished US with a melee finisher (msg 0x10) - a random weapon of that style's pool,
//     never the weapon the dead player carried, never its durability. Both logs: 0 "melee drop: killed player drops" lines over 15 deaths of melee
//     carriers (log 49118: a player died holding 7919a42b after a pin; log 39713 / 45927 ...), and the sweep itself never logged "died: clearing" once.
//  2. The disarm finisher read only the weapon IN the victim's hand (winmm's peer record +0x91). A victim carrying its melee weapon stowed (a gun in
//     the hand, or emptied hands for the hit pair) got the plain fist kill: log 45378 fist-nose-break-kill (the victim's red crowbar on its
//     backpack, the pistol in its hand; log 38587 "our gun put away for it", 39150 "wpose: no weapon"), POOL = the 5 plain fist kills only.
//
// FIX:
//  - MSG_MOWN 0xA6 [u8 ver 1][u64 carried melee weapon (MELEE_TAB base id, 0 = none)][i16 durability, -1 unknown][u8 seq]: every client tells the
//    others which droppable melee weapon it carries (held or stowed; the machete and the knives never count) and its durability - on a change, else
//    every 2 s, never while dead (a respawn's empty inventory cannot overwrite the dying player's weapon). Receivers keep the value last seen while that
//    slot was alive.
//  - Death drop: on every screen but the victim's own, every death of a slot that carried one (pin kill, finisher, shot, downed kill, bleed-out, ...)
//    spawns THAT weapon at its body with ITS durability (spawn-item-pickup count) 20 frames after the death, or once no paired move / execution holds
//    the body (max 200 frames). The old pool drop (g_peer_melee) stays only for a peer that never sent MSG_MOWN (an older build).
//  - Disarm: a victim with no melee in the hand that carries one (its MSG_MOWN) gets the disarm rows of that weapon's class (cd_fin_pick); its own
//    client draws the weapon into the hand at the disarm's START (cd_dis_tick role 2; SP's defender holds its weapon), so the take frame and the
//    hand-off work as with a held weapon. The disarm hand-off forgets the victim's carried weapon on every screen (no drop of a weapon already taken).
// Opt-outs (re-read every 2 s): t2r-no-mown-drop.txt = the old pool drop (MSG_MOWN still sent), t2r-dis-held-only.txt = disarms only of a held weapon.
// Log: "melee own:", "melee drop:".
#define MSG_MOWN 0xA6
#define MOWN_FRESH_MS 6000      // a carried-weapon report older than this (no heartbeat) is not trusted
#define MOWN_DROP_DELAY 20      // frames after the death (no pair on the body)
#define MOWN_DROP_MAX 200       // frames: a body held by a paired move / execution drops at the latest here
static struct {
    uint64_t sid, rx_ms;        // the last report
    int16_t dur;
    uint64_t a_sid, a_ms;       // the report as it was while the slot was alive
    int16_t a_dur;
    uint64_t forget_sid, forget_ms;     // a disarm took it: reports of this weapon are ignored for 5 s (a heartbeat sent before the victim's removal)
    int was_dead, pend;         // pend 1 = drop its weapon, 2 = the old pool drop (g_peer_melee)
    uint64_t died_t60, drop_h, chk_t60;
    char drop_n[32];
    uint64_t swept_t60;         // v5-mown2: the post-death sweep's last pass on this slot
    int hold;                   // v5-mown2: our drop lies at the body - no more sweeps of it this death
} g_mown[T2R_MAX_PEERS];
static int MownFile(const char *f, int *v, uint64_t *at)
{
    if (*v < 0 || GetTickCount64() - *at > 2000) {
        *at = GetTickCount64();
        int o = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES;
        if (o != *v && *v >= 0)
            Xlog("melee own: %s %s", f,
                 o ? "found - opt-out ON" : "gone - opt-out off");
        *v = o;
    }
    return *v;
}

static int MownOff(void)
{
    static int v = -1;
    static uint64_t at;
    return MownFile("t2r-no-mown-drop.txt", &v, &at);
}

static int MownDisHeldOnly(void)
{
    static int v = -1;
    static uint64_t at;
    return MownFile("t2r-dis-held-only.txt", &v, &at);
}

// v5-mown2: the post-death sweep vs our death drop (t2r-mown-no-sweep-wait.txt = drop without waiting, the sweep runs its whole window as before)
static int MownSweepOff(void)
{
    static int v = -1;
    static uint64_t at;
    return MownFile("t2r-mown-no-sweep-wait.txt", &v, &at);
}

static int MownSweepHold(int s)
{
    return s >= 0 && s < T2R_MAX_PEERS && g_mown[s].hold
        && !MownSweepOff();
}

static void MownSwept(int s)
{
    if (s >= 0 && s < T2R_MAX_PEERS)
        g_mown[s].swept_t60 = g_t60;
}

// ---- v5-mown2: pickup durability ledger (weapons were forced to full durability after pickup because they spawn at
// 1 dura). The engine ignores spawn-item-pickup's count for a melee weapon: every melee pickup is taken at health 1 (log 17560 / 14860 / 36520
// "full-condition: health 1" for x10 box spawns), and feature_melee_full_condition then set EVERY pickup to the class max - also a disarmed weapon
// whose durability the disarm had just set (log 44435 "durability 10 -> 5 (the victim's)", 44440 "health 5 -> set 10"). Now each melee pickup WE spawn
// is written down with the durability it stands for (box / item spot = the class max, a death drop = the dead player's weapon's), and a disarm hand-off
// names the victim's; at the pickup edge full-condition sets THAT durability (pdur_pick: the hand-off first, else the vanished ledger pickup of the
// same weapon nearest to us, else the class max as before). t2r-no-pickup-dur.txt = always the class max (old rule). Log: "pickup dur:".
#define PDUR_N 24
static struct {
    uint64_t h, base, born;
    int dur;
    float pos[3];
} g_pdur[PDUR_N];
static struct {
    uint64_t base;
    int dur;
    uint64_t until_ms;
} g_pdurWant;
static struct {
    uint64_t base;
    int dur;
} g_pdurLast;                  // the last pickup's weapon + the durability it was set to (a stowed weapon's report)
static int PdurOff(void)
{
    static int v = -1;
    static uint64_t at;
    return MownFile("t2r-no-pickup-dur.txt", &v, &at);
}

static void PdurSetLast(uint64_t w, int dur)
{
    g_pdurLast.base = BaseWeapon(w) ? BaseWeapon(w) : w;
    g_pdurLast.dur = dur;
}

static void PdurNote(uint64_t h, uint64_t item, int dur, const float *pos,
                      const char *why)
{
    if (!h || h == ~0ULL
        || !MeleeDurW(BaseWeapon(item) ? BaseWeapon(item) : item)
        || dur <= 0 || !pos)
        return;
    int k = -1;
    for (int i = 0; i < PDUR_N && k < 0; i++)
        if (!g_pdur[i].h || g_t60 - g_pdur[i].born > 18000)
            k = i;
    if (k < 0) {
        k = 0;
        for (int i = 1; i < PDUR_N; i++)
            if (g_pdur[i].born < g_pdur[k].born)
                k = i;
    }
    g_pdur[k].h = h;
    g_pdur[k].base = BaseWeapon(item) ? BaseWeapon(item) : item;
    g_pdur[k].dur = dur;
    g_pdur[k].born = g_t60;
    memcpy(g_pdur[k].pos, pos, 12);
    static int nl;
    if (LOGCAP(nl, 60))
        Xlog("pickup dur: %llx spawned as pickup %llx with durability %d (%s) at %.1f %.1f %.1f", (unsigned long long) item, (unsigned long long) h, dur, why, pos[0], pos[1], pos[2]);
}

static void PdurWant(uint64_t w, int dur, const char *why)
{
    if (!w || w == ~0ULL || dur <= 0)
        return;
    g_pdurWant.base = BaseWeapon(w) ? BaseWeapon(w) : w;
    g_pdurWant.dur = dur;
    g_pdurWant.until_ms = GetTickCount64() + 4000;
    Xlog("pickup dur: %llx comes to us with durability %d (%s) - the pickup edge keeps it", (unsigned long long) w, dur, why);
}

// at the pickup edge of weapon w: the durability it should have (0 = unknown: the class max)
static int PdurPick(uint64_t w, char *why, size_t wl)
{
    if (PdurOff() || !w || w == ~0ULL)
        return 0;
    uint64_t b = BaseWeapon(w) ? BaseWeapon(w) : w;
    if (g_pdurWant.base == b && GetTickCount64() < g_pdurWant.until_ms) {
        int d = g_pdurWant.dur;
        g_pdurWant.base = 0;
        snprintf(why, wl, "the disarm hand-off");
        return d;
    }
    typedef uint8_t *(*fn_me)(void);
    typedef const float *(*fn_loc)(void *, float *);
    uint8_t *me = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
    float mb[16];
    const float *mp =
        me
        ? ((fn_loc) ((uint8_t *) GetModuleHandleW(NULL) + 0x133db30)) (me +
                                                                       0x100,
                                                                       mb)
        : NULL;
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < PDUR_N; i++) {
        if (!g_pdur[i].h || g_pdur[i].base != b
            || DropsAlive(g_pdur[i].h))
            continue;           // (still lying there: not the one we took)
        float d2 = 0.0f;
        if (mp) {
            float dx = mp[0] - g_pdur[i].pos[0], dy =
                mp[1] - g_pdur[i].pos[1], dz = mp[2] - g_pdur[i].pos[2];
            d2 = dx * dx + dy * dy + dz * dz;
        }
        if (d2 < bd) {
            bd = d2;
            best = i;
        }
    }
    if (best < 0 || (mp && bd > 6.0f * 6.0f)) {
        snprintf(why, wl, "no ledger pickup of it gone near us");
        return 0;
    }
    int d = g_pdur[best].dur;
    snprintf(why, wl, "ledger pickup %llx, %.1f m away",
             (unsigned long long) g_pdur[best].h, mp ? sqrtf(bd) : -1.0f);
    g_pdur[best].h = 0;
    return d;
}

// the carried droppable melee weapon of slot s as its own client reported it (0 = none / unknown / stale / dead)
static uint64_t MownPeer(int s)
{
    if (s < 0 || s >= T2R_MAX_PEERS || g_graceDead[s] || !g_mown[s].sid
        || !g_mown[s].rx_ms
        || GetTickCount64() - g_mown[s].rx_ms > MOWN_FRESH_MS)
        return 0;
    return g_mown[s].sid;
}

static int MownPeerDur(int s)
{
    return MownPeer(s) ? g_mown[s].dur : -1;
}

static void MownForget(int s, uint64_t w)
{
    if (s < 0 || s >= T2R_MAX_PEERS)
        return;
    uint64_t b = w ? BaseWeapon(w) : 0;
    if (g_mown[s].sid || g_mown[s].a_sid)
        Xlog("melee own: slot %d's %llx forgotten (a disarm took it) - no death drop of it", s, (unsigned long long) (g_mown[s].a_sid ? g_mown[s].a_sid : g_mown[s].sid));
    g_mown[s].sid = g_mown[s].a_sid = 0;
    g_mown[s].forget_sid = b ? b : w;
    g_mown[s].forget_ms = GetTickCount64();
    if (g_mown[s].pend == 1)
        g_mown[s].pend = 0;
}

// a melee weapon that drops / can be disarmed: MELEE_TAB with an SP disarm class (no machete, no knife, no fists); *nm = its MELEE_TAB name
static int MownDroppable(uint64_t w, const char **nm)
{
    const char *n = NULL;
    int ok = w && w != ~0ULL && CdDisClass(w, &n) != 0 && n;
    if (nm)
        *nm = ok ? n : NULL;
    return ok;
}

static uint64_t MownSelf(int16_t *dur)
{
    uint64_t cw = CurrentWeapon();
    if (cw == ~0ULL)
        cw = 0;
    uint64_t w = XrMeleeWeapon(cw);
    const char *n = NULL;
    if (!MownDroppable(w, &n) && g_lastMelee && w != g_lastMelee) {
        uint64_t none = 0, has =
            DcCall(FNV64("has-weapon?"), 1, &g_lastMelee);
        (void) none;
        if (has != ~0ULL && (has & 0xff))
            w = g_lastMelee;
    }                           // (a knife in the hand, the club stowed)
    // v5-mown2: a stowed weapon was only known through g_last_melee (set while it is HELD): a pickup never drawn, or a stale g_last_melee (an old
    // weapon disarmed / broken / swapped), reported nothing or the wrong weapon. Stowed = ask the inventory (has-weapon? of each MELEE_TAB row, at
    // most once a second). t2r-mown-hand-only.txt = the old rule.
    static int ho = -1;
    static uint64_t ho_at;
    if (!MownFile("t2r-mown-hand-only.txt", &ho, &ho_at)) {
        int held = cw && (cw == w || BaseWeapon(cw) == BaseWeapon(w));
        if (w && !held && MownDroppable(w, NULL)) {
            uint64_t ww = w, has = DcCall(FNV64("has-weapon?"), 1, &ww);
            if (has != ~0ULL && !(has & 0xff))
                w = 0;
        }                       // (stale: not in the inventory)
        static uint64_t scan_at, scan_w;
        if (!MownDroppable(w, NULL)) {
            if (GetTickCount64() - scan_at > 1000) {
                scan_at = GetTickCount64();
                uint64_t prev = scan_w;
                scan_w = 0;
                uint64_t none = 0, anym =
                    DcCall(0x79763b864843c645ULL
                            /* player-has-non-basic-melee-weapon? */ , 0,
                            &none);
                if (anym != ~0ULL && (anym & 0xff))
                    for (size_t i = 0;
                         i < sizeof MELEE_TAB / sizeof *MELEE_TAB
                         && !scan_w; i++) {
                        uint64_t id = FNV64(MELEE_TAB[i].n);
                        if (!MownDroppable(id, NULL))
                            continue;
                        uint64_t has =
                            DcCall(FNV64("has-weapon?"), 1, &id);
                        if (has != ~0ULL && (has & 0xff))
                            scan_w = id;
                    }
                if (scan_w != prev) {
                    static int nl;
                    if (LOGCAP(nl, 30))
                        Xlog("melee own: inventory scan - %s %llx carried stowed (never held this life / g_last_melee %llx)", scan_w ? "melee weapon" : "no droppable melee", (unsigned long long) scan_w, (unsigned long long) g_lastMelee);
                }
            }
            w = scan_w;
        } else
            scan_w = 0;
    }
    if (!MownDroppable(w, &n)) {
        *dur = 0;
        return 0;
    }
    uint64_t b = FNV64(n);
    int full = MeleeDurName(n);
    int64_t d = -1;
    if (cw && (cw == w || BaseWeapon(cw) == BaseWeapon(w))) {
        uint64_t none = 0, hp = DcCall(SID_GET_WEAPON_HEALTH, 0, &none);
        if (hp != ~0ULL && (int64_t) hp > 0 && (int64_t) hp < 1000)
            d = (int64_t) hp - (WbLent(cw) && hp > 1 ? 1 : 0);
    }                           // (wpnbreak.c's lent point is not durability - the others never see 2)
    else if (g_lastMelee && BaseWeapon(g_lastMelee) == BaseWeapon(w)
             && g_lastMeleeHp > 0)
        d = g_lastMeleeHp;
    else if (g_pdurLast.base && g_pdurLast.base == BaseWeapon(w)
             && g_pdurLast.dur > 0)
        d = g_pdurLast.dur;    // a stowed pickup never held: the durability it was picked up with
    if (full > 0 && d > full)
        d = full;
    *dur = (int16_t) d;
    return b;
}

static void MownSendTick(void)
{
    static uint64_t last_sid, last_ms, last_chk;
    static int16_t last_dur = -2;
    static uint8_t seq;
    uint64_t now = GetTickCount64();
    if (!g_sendTramp || now - last_chk < 250)
        return;
    last_chk = now;
    if ((LocalHp() <= 0.0f && !g_downed) || g_inSpawn)
        return;                 // dead / respawning: the receivers keep what we carried alive (downed: still sent)
    int16_t d = -1;
    uint64_t w = MownSelf(&d);
    if (w == last_sid && d == last_dur && now - last_ms < 2000)
        return;
    if (w != last_sid || d != last_dur) {
        static int nl;
        if (LOGCAP(nl, 60))
            Xlog("melee own: we carry %s (%llx), durability %d - sent (MSG_MOWN)", w ? (MeleeClassOf(w, NULL) >= 0 ? "a melee weapon" : "?") : "no droppable melee weapon", (unsigned long long) w, (int) d);
    }
    uint8_t m[12];
    m[0] = 1;
    memcpy(m + 1, &w, 8);
    memcpy(m + 9, &d, 2);
    m[11] = seq++;
    g_sendTramp(MSG_MOWN, m, sizeof m, 0xff);
    last_sid = w;
    last_dur = d;
    last_ms = now;
}

static void MownRecv(uint32_t slot, const uint8_t *b, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || !b || len < 11 || b[0] != 1)
        return;
    uint64_t w;
    int16_t d;
    memcpy(&w, b + 1, 8);
    memcpy(&d, b + 9, 2);
    if (w && !MownDroppable(w, NULL))
        w = 0;                  // (an id we do not know as a droppable melee: nothing)
    if (w && g_mown[slot].forget_sid == w
        && GetTickCount64() - g_mown[slot].forget_ms < 5000)
        return;
    if (w != g_mown[slot].sid || d != g_mown[slot].dur) {
        static int nl;
        if (LOGCAP(nl, 80))
            Xlog("melee own: slot %u carries %llx, durability %d", slot,
                 (unsigned long long) w, (int) d);
    }
    g_mown[slot].sid = w;
    g_mown[slot].dur = d;
    g_mown[slot].rx_ms = GetTickCount64();
}

static uint64_t DropMeleeNamed(const float *pos, const char *name,
                                 int count, const char *why);
static void MdropTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    static int in_match;
    if (MatchSubstate() != 6) {
        if (in_match) {
            memset(g_mown, 0, sizeof g_mown);
            in_match = 0;
        }
        return;
    }
    in_match = 1;
    MownSendTick();
    uint32_t me = *(uint32_t *) T2R_SYM(kMySlot);
    uint64_t now = GetTickCount64();
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (s == me)
            continue;
        uint8_t *peer = ((fn_peer) T2rPeer) (s);
        if (!peer || !peer[4])
            continue;           // (no puppet this frame: the state waits - g_grace_dead is not updated either)
        int dead = g_graceDead[s] != 0;
        if (!dead && g_mown[s].rx_ms
            && now - g_mown[s].rx_ms < MOWN_FRESH_MS) {
            g_mown[s].a_sid = g_mown[s].sid;
            g_mown[s].a_dur = g_mown[s].dur;
            g_mown[s].a_ms = now;
        }
        if (dead && !g_mown[s].was_dead) {
            g_mown[s].died_t60 = g_t60;
            g_mown[s].pend = 0;
            g_mown[s].hold = 0;
            const char *n = NULL;
            if (!MownOff() && g_mown[s].a_sid && now - g_mown[s].a_ms < 60000 && MownDroppable(g_mown[s].a_sid, &n)) {        // (the last report while it lived: a bleed-out can be 15 s after it)
                g_mown[s].pend = 1;
                snprintf(g_mown[s].drop_n, sizeof g_mown[s].drop_n, "%s",
                         n);
                Xlog("melee drop: slot %u died carrying %s (durability %d) - its weapon drops at the body", s, n, (int) g_mown[s].a_dur);
            } else if (g_peerMelee[s] >= 0
                       && (MownOff() || !g_mown[s].a_ms)) {
                g_mown[s].pend = 2;
                Xlog("melee drop: slot %u died - %s: the old pool drop (style %d)", s, MownOff()? "t2r-no-mown-drop.txt" : "no MSG_MOWN from it (older build)", (int) g_peerMelee[s]);
            } else
                Xlog("melee drop: slot %u died - %s", s,
                     g_mown[s].
                     a_ms ? "it carried no droppable melee weapon" :
                     "no MSG_MOWN from it, no finisher style seen: nothing drops");
        }
        g_mown[s].was_dead = dead;
        if (!dead) {
            g_mown[s].pend = 0;
            g_mown[s].hold = 0;
            continue;
        }
        if (g_mown[s].pend) {
            uint64_t age = g_t60 - g_mown[s].died_t60;
            int busy = PsyncBusySlot(s) || GxBusySlot(s);
            if (age < MOWN_DROP_DELAY || (busy && age < MOWN_DROP_MAX))
                continue;
            // v5-mown2: the post-death sweep (feature_no_puppet_drops: kill-dropped-weapons-in-sphere, 5 m, from 20 / 200 ticks to 420 after the death)
            // never ran before this build (g_grace_dead was never set); our drop waits for its first pass, the later passes skip this slot (mown_sweep_hold)
            if (g_mown[s].pend == 1 && g_noDrops && !MownSweepOff()
                && g_mown[s].swept_t60 < g_mown[s].died_t60 && age < 480)
                continue;
            const float *pos = KillBodyLocator(s);
            if (!pos) {
                if (age < MOWN_DROP_MAX + 60)
                    continue;
                Xlog("melee drop: slot %u - no body position %llu frames after the death: nothing dropped", s, (unsigned long long) age);
                g_mown[s].pend = 0;
                continue;
            }
            float p[3] = { pos[0], pos[1], pos[2] };
            if (g_mown[s].pend == 1) {
                int full = MeleeDurName(g_mown[s].drop_n), d =
                    g_mown[s].a_dur > 0 ? g_mown[s].a_dur : full;
                if (full > 0 && d > full)
                    d = full;
                char why[64];
                snprintf(why, sizeof why,
                         "slot %u's own weapon, %llu frames after its death%s",
                         s, (unsigned long long) age,
                         busy ? " (pair cap)" : "");
                g_mown[s].drop_h =
                    DropMeleeNamed(p, g_mown[s].drop_n, d, why);
                g_mown[s].chk_t60 = g_t60 + 120;
                g_mown[s].hold = 1;     // the sweep leaves this body alone from here (mown_sweep_hold)
            } else if (g_peerMelee[s] >= 0)
                DropMeleePickup(p, g_peerMelee[s]);
            g_peerMelee[s] = -1;
            g_mown[s].pend = 0;
            g_mown[s].a_sid = 0;
        }
        if (g_mown[s].chk_t60 && g_t60 >= g_mown[s].chk_t60) {
            g_mown[s].chk_t60 = 0;
            Xlog("melee drop: slot %u's %s %s 2 s after the spawn (handle %llx)", s, g_mown[s].drop_n, DropsAlive(g_mown[s].drop_h) ? "still lies there" : "is GONE (taken or removed)", (unsigned long long) g_mown[s].drop_h);
        }
    }
}
