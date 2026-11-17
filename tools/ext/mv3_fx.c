// mv3_fx.c - melee v3 presentation: every sim event (mv3.h MV3_E_*) becomes SP-faithful sound, voice, blood / impact fx and camera feel, on every screen, at the
// frame it happens, rollback-safe (spec re/melee-v3.md "Presentation"). Included after melee_v2.c, so every helper it reuses is forward-declared below.
// Audio feedback matters (audio jank was clearly noticeable next to the responsiveness of SP), and everything melee related is client-side, visually and audibly. Every screen runs the same sim, so every screen fires the same cues from its own
// events: no message, no confirmation round trip (the old punch / blade fx waited for the victim's MSG_HITREACT2, ~340 ms).
//
// Event -> cues (offsets from the event's world frame, on the local clock via mv3_net_frame_ms; SP timelines: re/sp-sandbox-runs/sea-1026-human-melee-timeline.txt,
// re/sp-melee-1027/session3/report-a.md):
//   SWING   cloth swish PCM +40 ms on the attacker (melee_tell.c mtell_tick: dm_wave EXS_PIPE_WHOOSH 0.5 fists / 0.8 weapon);
//           effort grunt on the attacker ~10 frames before contact (SP: knife light-1 give-med +16 f, contact +25 f; 2h give-lrg-short @236 ms, contact ~440 ms;
//           the attacker's own voice: dm_say give-med / give-wpn-med as mtell_tick);
//           the weapon's own SP swing whoosh 5 frames before contact (SP: club-2h-whoosh @361 ms; melee_dodge.c mdg_whiff_whoosh, fists sfx-unarmed-finisher-whoosh).
//   HIT     impact at the contact on the VICTIM body: fists = TLOU2 punch waves (melee_hitreact.c mhr_punch_at), knife = switchblade hit + SP blood (knife_hit_native),
//           1h blunt / 1h blade / 2h = the weapon's hit sfx + SP -hit clip blood (ctxdown.c melee_hit_fx -> melee_weapon_hit_sfx + knife_hit_fx);
//           pain grunt +2 frames on the victim (SP: victim vox +1 f; mhr_pain_say: take-face / take-lrg-blnt, blades -shrp); none when the blow downs (DOWN event:
//           the downed flow has its own voice);
//           attacker screen: SP melee hit kick (melee_feel.c mfk_attacker_hit's template, minus its engine-swing gate g_ms_atk); victim screen: the jolt + the
//           blunt / blade / fist overlay (mfk_victim_hit -> mhfx_note), a counter blow = the heavy concussion (dcounter.c: mhfx_counter).
//   WHIFF / DODGED (we are the attacker)  light follow-through (mfk_whiff), once per swing.
//   DODGE   dodge effort +9 frames on the dodger (SP vox-eff-gen-fight-dodge-hvy +9..18 f; melee_dodge.c mdg_dodge_say: every WLF / Seraphite voice's plain
//           -dodge, no Ellie / Abby voice); a soft swish +12 frames standing in for SP's bp-dodge-hard backpack foley (no MP event for it).
//   COUNTER effort grunt on the counterer +10 frames (SP palm bash give-lrg-short 0.17 s in; dcounter.c says give-med in the attacker's own voice). The counter's blow
//           itself is a HIT (its damage goes through the same event) and gets the counter concussion on the victim.
//   REACT   nothing (the clip is mv3_anim.c's; the pain grunt rides on the HIT).
//   DOWN    cancels a pending pain grunt of that victim (the downed flow speaks).
//   Camera  SP melee framing (melee_spcam.c) held on while we are a bubble member (spm_event every frame); no execution camera.
//
// ROLLBACK: key = (frame, type, a, b, move), matched within +-MV3FX_FUZZ frames (a resim that moves a contact by a frame or two re-keys the record instead of
// playing it twice). mv3_fx_rolled_back(f) marks records >= f tentative; a tentative record re-emitted by the resim is confirmed (deduped); one the resim no longer
// emits is CANCELLED at the next mv3_fx_tick: its pending cues are dropped, a played native sound with a handle gets esfx_stop (opt-in t2r-engine-sfx-stop.txt,
// unproven), a played PCM wave / `say` / camera kick cannot be stopped and is left (counted as "left").
// Trade-off: nothing the helpers play is stoppable today (the PCM mixer returns no handle, `say` neither), so the only real protection is to not play a cue
// while it can still vanish: every cue waits until its event is either confirmed (frame <= mv3_net_confirmed) or MV3FX_GRACE ms old (default 16 = one frame,
// t2r-mv3-fx-grace.txt "<ms>" 0..150). The swing / dodge cues start >= 40 ms after their event, so a rollback inside that time cancels them cleanly; only the
// impact / blood / pain / kick of a HIT sit near the event, so a HIT removed by a late dodge input more than the grace later is heard (the per-5-s summary counts
// "left" and the rollback depth, which is the data for tuning the grace against responsiveness). Voices never double: one record per key, fuzzy re-keying, and
// the helpers' own per-body gates (pain 350 ms, dodge 600 ms, effort g_att_vox_ms 300 ms).
// Cues found late (a resim that adds an event in the past) still play if they are less than MV3FX_STALE ms past their moment, else they are dropped (stale).
//
// Opt-out t2r-no-mv3-fx.txt (all of it). Log prefix "mv3fx:". mv3_fx_tick should run after mv3_net_tick in the frame (cues play from the tick).
#include "mv3.h"
// ---- other modules (melee v3 net; the melee helpers are included later in t2r-ext.c)
static int Mv3NetActive(void);
static const Mv3World *Mv3NetWorld(void);
static int Mv3NetLocalIdx(void);
static uint32_t Mv3NetConfirmed(void);
static int Mv3NetSlotOfIdx(int idx);
static uint32_t Mv3NetFrameMs(uint32_t frame);
static int MtellBodyOk(uint32_t who); // melee_tell.c
static void MdgWhiffWhoosh(uint64_t ent, uint64_t w, int slot);       // melee_dodge.c
static void MdgDodgeSay(uint32_t who, uint64_t ent);  // melee_dodge.c
static void MhrPainSay(uint32_t who, uint64_t ent, int str, int sharp);       // melee_hitreact.c
static uint64_t MhrPunchAt(uint64_t ent, int str, const char *who);   // melee_hitreact.c
static int MfkFlag(int which); // melee_feel.c
static const char *MfkBusy(void);
static void MfkAdd(float amp, float roll, float pitch, float yaw);
static void MfkWhiff(void);
static void MfkVictimHit(uint32_t slot, uint64_t w, float dmg,
                           float hp_before, float hp_after);
static void MhfxCounter(uint32_t slot, uint64_t w);    // melee_hitfx.c
static void SpmEvent(void);    // melee_spcam.c

#define MV3FX_REC   96          // event records (~1.6 s of a busy 3-way bubble)
#define MV3FX_CUES  4
#define MV3FX_FUZZ  6           // frames: a re-simulated event this close with the same type / bodies / move is the same event
#define MV3FX_GRACE 16          // ms (one frame): see the trade-off above
#define MV3FX_STALE 250         // ms past its moment a late-found impact / pain may still play (swing air 100, efforts / camera 120, dodge 150)
#define MV3FX_FIST_W 0x6ac05fba9c921900ULL      // abby-unarmed
enum { MV3C_SWISH =
        1, MV3C_EFFORT, MV3C_WHOOSH, MV3C_IMPACT, MV3C_PAIN, MV3C_KICK_ATK,
        MV3C_KICK_VIC, MV3C_WHIFF, MV3C_DODGE_VOX, MV3C_DODGE_SWISH,
        MV3C_CTR_VOX };
static const char *const MV3C_NAME[] =
    { "-", "swish", "effort", "whoosh", "impact", "pain", "attacker kick",
"victim jolt", "whiff follow-through", "dodge grunt", "dodge swish", "counter grunt" };
enum { MV3C_PEND, MV3C_PLAYED, MV3C_CANCELLED, MV3C_STALE, MV3C_FILTERED };
static const char *const MV3E_NAME[] =
    { "SWING", "HIT", "WHIFF", "DODGE", "DODGED", "REACT", "COUNTER",
"DOWN" };
typedef struct {
    uint32_t at;
    uint8_t kind, st, pred;
    uint64_t h;
} Mv3fxCue;
typedef struct {
    uint8_t used, tent, dead, type, a, b, ctr;  // dead: vanished in a resim (kept so a later resim that brings it back does not replay what was heard); ctr: a HIT that is a counter blow
    int16_t move, x;
    uint32_t frame, ev_ms;
    Mv3fxCue c[MV3FX_CUES];
} Mv3fxRec;
static struct {
    Mv3fxRec r[MV3FX_REC];
    int next, grace, off;
    uint64_t off_at, cfg_at;
    int rb_pending;
    uint32_t whiff_ms[MV3_MAX_F];       // per attacker: its follow-through played (WHIFF and DODGED of one swing: once)
    uint64_t sum_ms;
    struct {
        int ev_new, ev_dedup, ev_shift, played, pred, cancelled, stopped,
            left, stale, filtered, rb, rb_max;
    } s;
} g_mv3fx = {.grace = MV3FX_GRACE };

static int Mv3fxOff(void)
{
    return FeelFlag("t2r-no-mv3-fx.txt", &g_mv3fx.off_at, &g_mv3fx.off,
                     120);
}

static void Mv3fxCfg(void)
{
    uint64_t now = GetTickCount64();
    if (g_mv3fx.cfg_at && now - g_mv3fx.cfg_at < 2000)
        return;
    g_mv3fx.cfg_at = now ? now : 1;
    int g = MV3FX_GRACE;
    FILE *f = ExeSideOpen("t2r-mv3-fx-grace.txt", "r");
    if (f) {
        int v;
        if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 150)
            g = v;
        fclose(f);
    }
    if (g != g_mv3fx.grace)
        Xlog("mv3fx: grace %d ms (a cue of a still-predicted event waits this long; t2r-mv3-fx-grace.txt)", g);
    g_mv3fx.grace = g;
}

static uint32_t Mv3fxF2ms(int frames)
{
    return (uint32_t) ((frames * 1000 + MV3_HZ / 2) / MV3_HZ);
}

static const Mv3Move *Mv3fxMove(int m)
{
    return m >= 0 && m < MV3_MOVES_N ? &MV3_MOVES[m] : NULL;
}

// a fighter's body: us = SID_PLAYER (who = T2R_MAX_PEERS), anyone else = the puppet of its net slot (resolved at play time: a respawn swaps the handle)
static uint64_t Mv3fxBody(int idx, uint32_t *who)
{
    if (who)
        *who = 0xffffffffu;
    if (idx < 0 || idx >= MV3_MAX_F)
        return 0;
    if (idx == Mv3NetLocalIdx()) {
        if (who)
            *who = T2R_MAX_PEERS;
        return SID_PLAYER;
    }
    int sl = Mv3NetSlotOfIdx(idx);
    if (sl < 0 || sl >= T2R_MAX_PEERS)
        return 0;
    uint64_t h = PuppetHandle((uint32_t) sl);
    if (!h || h == ~0ULL)
        return 0;
    if (who)
        *who = (uint32_t) sl;
    return h;
}

static int Mv3fxWset(int idx, int move)
{
    const Mv3Move *m = Mv3fxMove(move);
    if (m)
        return m->wset;
    const Mv3World *w = Mv3NetWorld();
    return w && idx >= 0 && idx < w->n ? w->f[idx].wset : MV3_WS_FIST;
}

// the weapon id in the fighter's hand (the helpers pick their sfx / blood / overlay by it); the sim's weapon set wins when the engine's id disagrees (a stale record)
static uint64_t Mv3fxWeapon(int idx, int wset)
{
    uint64_t w = 0;
    if (idx == Mv3NetLocalIdx())
        w = CurrentWeapon();
    else {
        int sl = Mv3NetSlotOfIdx(idx);
        uint8_t *p = sl >= 0 && sl < T2R_MAX_PEERS ? T2rPeer((uint32_t) sl) : NULL;    // (melee_dodge.c: the peer record's weapon at +0x91)
        if (p && !IsBadReadPtr(p, 0x99))
            w = *(uint64_t *) (p + 0x91);
    }
    if (w == ~0ULL)
        w = 0;
    if (wset == MV3_WS_FIST)
        return MV3FX_FIST_W;
    if (wset == MV3_WS_KNIFE)
        return w
            && KnifeWeaponId(w) ? w : FNV64("knife-survival-mel-player");
    if (w && MeleeClassOf(w, NULL) < 0
        && MeleeClassOf(BaseWeapon(w), NULL) >= 0)
        w = BaseWeapon(w);
    return w;
}

static int Mv3fxBlade(int wset)
{
    return wset == MV3_WS_1HBLADE || wset == MV3_WS_KNIFE;
}

static void Mv3fxCueAdd(Mv3fxRec *r, int kind, uint32_t ms_after)
{
    for (int i = 0; i < MV3FX_CUES; i++)
        if (!r->c[i].kind) {
            r->c[i].kind = (uint8_t) kind;
            r->c[i].at = r->ev_ms + ms_after;
            r->c[i].st = MV3C_PEND;
            r->c[i].h = 0;
            return;
        }
}

// ---- the cues
static void Mv3fxKickAtk(Mv3fxRec *r, int heavy, int wpn)
{                               // melee_feel.c mfk_attacker_hit without its engine-swing gate (the sim swing is ours)
    static uint64_t last;
    static int flip, nl;
    if (MfkFlag(0))
        return;
    uint64_t now = GetTickCount64();
    if (now - last < 140)
        return;
    const char *b = MfkBusy();
    if (b) {
        if (LOGCAP(nl, 20))
            Xlog("mv3fx: attacker kick skipped - %s", b);
        return;
    }
    last = now;
    float amp = heavy ? 1.6f : wpn ? 1.1f : 0.8f;
    flip ^= 1;
    MfkAdd(amp, flip ? 1.0f : -1.0f, 1.0f, (flip ? 0.4f : -0.4f) * amp);
    (void) r;
}

static void Mv3fxPlay(Mv3fxRec *r, Mv3fxCue *c, int pred)
{
    static int nl;
    uint32_t who_a, who_b;
    uint64_t ea = Mv3fxBody(r->a, &who_a), eb = r->type == MV3_E_HIT
        || r->type == MV3_E_COUNTER ? Mv3fxBody(r->b, &who_b) : 0;
    int ws = Mv3fxWset(r->a, r->move), li = Mv3NetLocalIdx();
    const Mv3Move *mv = Mv3fxMove(r->move);
    int str = mv ? (mv->strength > 2 ? 2 : mv->strength) : 1;
    if (r->x >= 1500 || r->ctr)
        str = 2;                // a downing / one-shot or counter blow: the heavy takes
    const char *what = "", *on = "";
    int ok = 1;
    switch (c->kind) {
    case MV3C_SWISH:           // melee_tell.c mtell_tick
        ok = ea && MtellBodyOk(who_a);
        if (ok)
            MdgWhiffWhoosh(ea, MtellWeapon(who_a),
                             who_a == T2R_MAX_PEERS ? -1 : (int) who_a);
        what = "weapon whoosh";
        on = "attacker";
        break;
    case MV3C_EFFORT:
    case MV3C_CTR_VOX:{        // melee_tell.c mtell_tick / dcounter.c palm bash grunt
            uint64_t now = GetTickCount64();
            ok = ea && MtellBodyOk(who_a) && !(g_attVoxMs
                                                 && now - g_attVoxMs <
                                                 300)
                && GetFileAttributesA("t2r-no-dodge-vox.txt") ==
                INVALID_FILE_ATTRIBUTES;
            what = c->kind == MV3C_CTR_VOX
                || ws ==
                MV3_WS_FIST ? "vox-eff-gen-fight-give-med" :
                "vox-eff-gen-fight-give-wpn-med";
            on = "attacker";
            if (ok) {
                g_attVoxMs = now;
                DmSay(ea, what, VXE_GIVE);
            }
            break;
        }
    case MV3C_WHOOSH:          // melee_dodge.c (whiff whoosh = the weapon's SP swing whoosh; 400 ms gate per body)
        ok = ea && MtellBodyOk(who_a);
        if (ok)
            MdgWhiffWhoosh(ea, Mv3fxWeapon(r->a, ws),
                             who_a == T2R_MAX_PEERS ? -1 : (int) who_a);
        what = "weapon whoosh";
        on = "attacker";
        break;
    case MV3C_IMPACT:{
            on = r->b == li ? "us (victim)" : "the victim puppet";
            if (!eb) {
                ok = 0;
                break;
            }
            uint64_t w = Mv3fxWeapon(r->a, ws);
            if (ws == MV3_WS_FIST || !w) {
                c->h = MhrPunchAt(eb, str, on);
                what = "punch";
            }                   // melee_hitreact.c
            else if (ws == MV3_WS_KNIFE) {
                KnifeHitNative(eb, 0);
                what = "knife cut + blood";
            }                   // t2r-ext.c
            else if (MeleeClassOf(w, NULL) >= 0) {
                MeleeHitFx(eb, w);
                what = "weapon hit sfx + SP blood";
            }                   // ctxdown.c
            else {
                c->h = MhrPunchAt(eb, str, on);
                what = "punch (weapon id unknown)";
            }
            if (c->h == 1)
                c->h = 0;       // (1 = a PCM wave: no handle)
            break;
        }
    case MV3C_PAIN:            // melee_hitreact.c mhr_pain_say (its own 350 ms gate per body)
        ok = eb && MtellBodyOk(who_b);
        if (ok)
            MhrPainSay(who_b, eb, str, Mv3fxBlade(ws));
        what = "pain grunt";
        on = "victim";
        break;
    case MV3C_KICK_ATK:
        ok = r->a == li;
        if (ok)
            Mv3fxKickAtk(r, str >= 2, ws != MV3_WS_FIST);
        what = "attacker kick";
        on = "our camera";
        break;
    case MV3C_KICK_VIC:{       // melee_feel.c mfk_victim_hit (+ melee_hitfx.c overlay), counter: dcounter.c's mhfx_counter
            ok = r->b == li && who_a < T2R_MAX_PEERS;
            on = "our camera";
            if (!ok)
                break;
            uint64_t w = Mv3fxWeapon(r->a, ws);
            float dmg = r->x > 0 ? (float) r->x / 10.0f : 1.0f, hb =
                LocalHp();
            if (hb < dmg + 1.0f)
                hb = dmg + 1.0f;        // (the glue may have applied the damage already: the jolt only needs "hurt, standing")
            MfkVictimHit(who_a, w, dmg, hb, hb - dmg);
            if (r->ctr)
                MhfxCounter(who_a, w);
            what =
                r->
                ctr ? "victim jolt + counter concussion" :
                "victim jolt + overlay";
            break;
        }
    case MV3C_WHIFF:{
            int att = r->type == MV3_E_DODGED ? r->b : r->a;
            uint32_t t = (uint32_t) GetTickCount64();
            ok = att == li && att < MV3_MAX_F
                && t - g_mv3fx.whiff_ms[att] > 300;
            if (ok) {
                g_mv3fx.whiff_ms[att] = t;
                MfkWhiff();
            }
            what = "whiff follow-through";
            on = "our camera";
            break;
        }
    case MV3C_DODGE_VOX:       // melee_dodge.c (600 ms gate per body)
        ok = ea && MtellBodyOk(who_a);
        if (ok)
            MdgDodgeSay(who_a, ea);
        what = "dodge grunt";
        on = "dodger";
        break;
    case MV3C_DODGE_SWISH:
        ok = ea && MtellBodyOk(who_a)
            && DmWave(EXS_PIPE_WHOOSH, ea, 0.35f);
        what = "dodge swish";
        on = "dodger";
        break;
    }
    c->st = ok ? MV3C_PLAYED : MV3C_FILTERED;
    c->pred = (uint8_t) pred;
    if (ok) {
        g_mv3fx.s.played++;
        if (pred)
            g_mv3fx.s.pred++;
    } else
        g_mv3fx.s.filtered++;
    if (LOGCAP(nl, 400))
        Xlog("mv3fx: f%u %s a%d b%d move %d: %s %s on %s, %+d ms from its moment%s", r->frame, MV3E_NAME[r->type], r->a, r->b, r->move, what ? what : MV3C_NAME[c->kind], ok ? "played" : "skipped (body busy / gone / gated)", on, (int) ((uint32_t) GetTickCount64() - c->at), pred ? " (still predicted)" : "");
}

// ---- records
static Mv3fxRec *Mv3fxFind(const Mv3Ev *e)
{
    Mv3fxRec *best = NULL;
    int bd = MV3FX_FUZZ + 1;
    for (int i = 0; i < MV3FX_REC; i++) {
        Mv3fxRec *r = &g_mv3fx.r[i];
        if (!r->used || r->type != e->type || r->a != e->a
            || r->move != e->move || (e->type != MV3_E_SWING
                                      && e->type != MV3_E_DODGE
                                      && r->b != e->b))
            continue;
        int d = (int) (r->frame - e->frame);
        if (d < 0)
            d = -d;
        if (d < bd) {
            bd = d;
            best = r;
        }
    }
    return best;
}

static int Mv3fxHasDown(const Mv3Evbuf *ev, int victim)
{
    for (int i = 0; i < ev->n; i++)
        if (ev->e[i].type == MV3_E_DOWN && ev->e[i].a == victim)
            return 1;
    return 0;
}

static int Mv3fxCounterOpen(int a, int b, uint32_t frame)
{                               // a COUNTER a->b started shortly before: its blow is the counter blow
    for (int i = 0; i < MV3FX_REC; i++) {
        Mv3fxRec *r = &g_mv3fx.r[i];
        if (r->used && r->type == MV3_E_COUNTER && r->a == a && r->b == b
            && frame >= r->frame && frame - r->frame < 120)
            return 1;
    }
    return 0;
}

static void Mv3fxSchedule(Mv3fxRec *r, const Mv3Evbuf *ev)
{
    const Mv3Move *mv = Mv3fxMove(r->move);
    int hit0 = mv && mv->hit0_f ? mv->hit0_f : 19;      // no data: melee_tell.c's 320 ms tell
    int li = Mv3NetLocalIdx();
    switch (r->type) {
    case MV3_E_SWING:{
            int vf = hit0 - 10;
            if (vf < 3)
                vf = 3;
            if (vf > 20)
                vf = 20;
            int wf = hit0 - 5;
            if (wf < 4)
                wf = 4;
            Mv3fxCueAdd(r, MV3C_SWISH, 40);
            Mv3fxCueAdd(r, MV3C_EFFORT, Mv3fxF2ms(vf));
            Mv3fxCueAdd(r, MV3C_WHOOSH, Mv3fxF2ms(wf));
            break;
        }
    case MV3_E_HIT:
        r->ctr = (mv && mv->kind == MV3_K_COUNTER)
            || Mv3fxCounterOpen(r->a, r->b, r->frame);
        Mv3fxCueAdd(r, MV3C_IMPACT, 0);
        if (!Mv3fxHasDown(ev, r->b))
            Mv3fxCueAdd(r, MV3C_PAIN, Mv3fxF2ms(2));
        if (r->a == li)
            Mv3fxCueAdd(r, MV3C_KICK_ATK, 0);
        if (r->b == li)
            Mv3fxCueAdd(r, MV3C_KICK_VIC, 0);
        break;
    case MV3_E_WHIFF:
        if (r->a == li)
            Mv3fxCueAdd(r, MV3C_WHIFF, 0);
        break;
    case MV3_E_DODGED:
        if (r->b == li)
            Mv3fxCueAdd(r, MV3C_WHIFF, 0);
        break;                  // b = the attacker: its follow-through
    case MV3_E_DODGE:
        Mv3fxCueAdd(r, MV3C_DODGE_VOX, Mv3fxF2ms(9));
        Mv3fxCueAdd(r, MV3C_DODGE_SWISH, Mv3fxF2ms(12));
        break;
    case MV3_E_COUNTER:
        Mv3fxCueAdd(r, MV3C_CTR_VOX, Mv3fxF2ms(10));
        break;
    case MV3_E_DOWN:           // the downed flow speaks: drop a pending pain grunt of this victim
        for (int i = 0; i < MV3FX_REC; i++) {
            Mv3fxRec *h = &g_mv3fx.r[i];
            if (!h->used || h->type != MV3_E_HIT || h->b != r->a
                || (int) (r->frame - h->frame) > 3
                || (int) (h->frame - r->frame) > 3)
                continue;
            for (int k = 0; k < MV3FX_CUES; k++)
                if (h->c[k].kind == MV3C_PAIN && h->c[k].st == MV3C_PEND) {
                    h->c[k].st = MV3C_FILTERED;
                    g_mv3fx.s.filtered++;
                }
        }
        break;
    default:
        break;                  // REACT: the clip only
    }
}

// ---- the Fx interface (mv3.h / re/melee-v3-brief.md)
static void Mv3FxEvents(const Mv3Evbuf *ev, int resim)
{
    static int nl;
    if (!ev || ev->n <= 0)
        return;
    if (Mv3fxOff())
        return;
    uint32_t now = (uint32_t) GetTickCount64();
    for (int i = 0; i < ev->n && i < MV3_EV_MAX; i++) {
        const Mv3Ev *e = &ev->e[i];
        if (e->type > MV3_E_DOWN)
            continue;
        Mv3fxRec *r = Mv3fxFind(e);
        if (r) {
            if (r->frame != e->frame) { // the resim moved it: re-key, move the cues that have not played yet
                uint32_t ms = Mv3NetFrameMs(e->frame);
                if (!ms)
                    ms = now;
                for (int k = 0; k < MV3FX_CUES; k++)
                    if (r->c[k].kind
                        && (r->c[k].st == MV3C_PEND
                            || r->c[k].st == MV3C_CANCELLED))
                        r->c[k].at += ms - r->ev_ms;
                if (LOGCAP(nl, 80))
                    Xlog("mv3fx: f%u %s a%d b%d re-simulated to f%u - same event, cues moved %+d ms", r->frame, MV3E_NAME[e->type], e->a, e->b, e->frame, (int) (ms - r->ev_ms));
                r->frame = e->frame;
                r->ev_ms = ms;
                g_mv3fx.s.ev_shift++;
            } else
                g_mv3fx.s.ev_dedup++;
            if (r->dead) {      // back after a resim dropped it: what was heard stays heard, the rest plays again
                r->dead = 0;
                for (int k = 0; k < MV3FX_CUES; k++)
                    if (r->c[k].st == MV3C_CANCELLED) {
                        r->c[k].st = MV3C_PEND;
                        g_mv3fx.s.cancelled--;
                    }
                if (LOGCAP(nl, 80))
                    Xlog("mv3fx: f%u %s a%d b%d is back in a resim - played cues not repeated", r->frame, MV3E_NAME[e->type], e->a, e->b);
            }
            r->tent = 0;
            r->x = e->x;
            continue;
        }
        r = &g_mv3fx.r[g_mv3fx.next++ % MV3FX_REC];     // (ring: the oldest record is long finished)
        memset(r, 0, sizeof *r);
        r->used = 1;
        r->type = e->type;
        r->a = e->a;
        r->b = e->b;
        r->move = e->move;
        r->x = e->x;
        r->frame = e->frame;
        r->ev_ms = Mv3NetFrameMs(e->frame);
        if (!r->ev_ms)
            r->ev_ms = now;
        Mv3fxSchedule(r, ev);
        g_mv3fx.s.ev_new++;
        if (resim && LOGCAP(nl, 80))
            Xlog("mv3fx: f%u %s a%d b%d move %d is NEW in a resim (%d ms after its moment)", e->frame, MV3E_NAME[e->type], e->a, e->b, e->move, (int) (now - r->ev_ms));
    }
}

static void Mv3FxRolledBack(uint32_t to_frame)
{
    const Mv3World *w = Mv3NetWorld();
    int depth = w
        && w->frame >= to_frame ? (int) (w->frame - to_frame) : 0;
    g_mv3fx.s.rb++;
    if (depth > g_mv3fx.s.rb_max)
        g_mv3fx.s.rb_max = depth;
    for (int i = 0; i < MV3FX_REC; i++)
        if (g_mv3fx.r[i].used && g_mv3fx.r[i].frame >= to_frame)
            g_mv3fx.r[i].tent = 1;
    g_mv3fx.rb_pending = 1;
}

static void Mv3fxCancel(Mv3fxRec *r)
{
    static int nl;
    int dropped = 0, stopped = 0, left = 0;
    for (int k = 0; k < MV3FX_CUES; k++) {
        Mv3fxCue *c = &r->c[k];
        if (!c->kind)
            continue;
        if (c->st == MV3C_PEND) {
            c->st = MV3C_CANCELLED;
            dropped++;
        } else if (c->st == MV3C_PLAYED) {
            if (c->h > 1) {
                EsfxStop(c->h);
                stopped++;
            } else
                left++;
        }
    }
    g_mv3fx.s.cancelled += dropped;
    g_mv3fx.s.stopped += stopped;
    g_mv3fx.s.left += left;
    if (LOGCAP(nl, 120))
        Xlog("mv3fx: f%u %s a%d b%d move %d vanished in the resim - %d cue(s) cancelled before playing, %d stopped, %d already heard (left)", r->frame, MV3E_NAME[r->type], r->a, r->b, r->move, dropped, stopped, left);
    r->dead = 1;
    r->tent = 0;
}

static void Mv3FxTick(uint64_t now64)
{
    uint32_t now = (uint32_t) now64;
    Mv3fxCfg();
    if (Mv3fxOff()) {
        if (g_mv3fx.next) {
            memset(g_mv3fx.r, 0, sizeof g_mv3fx.r);
            g_mv3fx.next = 0;
        }
        return;
    }
    const Mv3World *w = Mv3NetWorld();
    uint32_t cur = w ? w->frame : 0, conf = Mv3NetConfirmed();
    // after a resim: the tentative records it did not emit again are gone
    if (g_mv3fx.rb_pending) {
        g_mv3fx.rb_pending = 0;
        for (int i = 0; i < MV3FX_REC; i++)
            if (g_mv3fx.r[i].used && g_mv3fx.r[i].tent
                && !g_mv3fx.r[i].dead && g_mv3fx.r[i].frame <= cur)
                Mv3fxCancel(&g_mv3fx.r[i]);
    }
    for (int i = 0; i < MV3FX_REC; i++) {
        Mv3fxRec *r = &g_mv3fx.r[i];
        if (!r->used)
            continue;
        if (r->dead) {
            if ((int) (now - r->ev_ms) > 2000)
                r->used = 0;
            continue;
        }
        int pend = 0, pred = r->frame > conf;
        int ripe = !pred || (int) (now - r->ev_ms) >= g_mv3fx.grace;
        for (int k = 0; k < MV3FX_CUES; k++) {
            Mv3fxCue *c = &r->c[k];
            if (!c->kind || c->st != MV3C_PEND)
                continue;
            if ((int) (now - c->at) < 0 || !ripe) {
                pend = 1;
                continue;
            }
            int stale = MV3FX_STALE;    // impact / pain: a late blow is still a blow
            if (c->kind == MV3C_SWISH || c->kind == MV3C_WHOOSH)
                stale = 100;    // a swing's air sounds after its contact are noise
            else if (c->kind == MV3C_EFFORT || c->kind == MV3C_CTR_VOX)
                stale = 120;    // the effort ~150 ms before contact: never on top of the blow
            else if (c->kind == MV3C_KICK_ATK || c->kind == MV3C_KICK_VIC
                     || c->kind == MV3C_WHIFF)
                stale = 120;
            else if (c->kind == MV3C_DODGE_VOX
                     || c->kind == MV3C_DODGE_SWISH)
                stale = 150;
            if ((int) (now - c->at) > stale) {
                c->st = MV3C_STALE;
                g_mv3fx.s.stale++;
                continue;
            }
            Mv3fxPlay(r, c, pred);
        }
        if (!pend && (int) (now - r->ev_ms) > 2000)
            r->used = 0;        // finished and out of the rollback history (MV3_HIST 1.6 s)
    }
    // SP melee framing (melee_spcam.c) while we fight in a bubble: its own rules pick the enemy, the pull-in and the blend
    int li = Mv3NetLocalIdx();
    if (Mv3NetActive() && w && li >= 0 && li < w->n
        && w->f[li].state != MV3_S_OUT)
        SpmEvent();
    if (!g_mv3fx.sum_ms)
        g_mv3fx.sum_ms = now64;
    if (now64 - g_mv3fx.sum_ms >= 5000) {
        g_mv3fx.sum_ms = now64;
        if (g_mv3fx.s.ev_new || g_mv3fx.s.rb) {
            static int nl;
            if (LOGCAP(nl, 400))
                Xlog("mv3fx: 5 s - events %d new, %d deduped, %d re-keyed; cues %d played (%d while predicted), %d cancelled, %d stopped, %d left playing, %d stale, %d skipped; rollbacks %d (deepest %d frames), grace %d ms", g_mv3fx.s.ev_new, g_mv3fx.s.ev_dedup, g_mv3fx.s.ev_shift, g_mv3fx.s.played, g_mv3fx.s.pred, g_mv3fx.s.cancelled, g_mv3fx.s.stopped, g_mv3fx.s.left, g_mv3fx.s.stale, g_mv3fx.s.filtered, g_mv3fx.s.rb, g_mv3fx.s.rb_max, g_mv3fx.grace);
        }
        memset(&g_mv3fx.s, 0, sizeof g_mv3fx.s);
    }
}

// glue helper (interface request): 1 = mv3_fx played a HIT on this body in the last ms (who: slot, or T2R_MAX_PEERS = us) - the old hit-path fx
// (recv_hit_hook blood, hrx_victim_hit jolt / overlay, mhr punch / pain, bfx) must stand down for it or the blow is heard / felt twice
static int Mv3FxHitRecent(uint32_t who, uint32_t ms)
{
    uint32_t now = (uint32_t) GetTickCount64();
    for (int i = 0; i < MV3FX_REC; i++) {
        Mv3fxRec *r = &g_mv3fx.r[i];
        uint32_t wb;
        if (!r->used || r->dead || r->type != MV3_E_HIT
            || now - r->ev_ms > ms)
            continue;
        if (Mv3fxBody(r->b, &wb) && wb == who)
            return 1;
    }
    return 0;
}
