// ---------------------------------------------------------------- visibility diagnostics
// Characters flickered / their body disappeared during special executions and such (WLF and Seraphite looks, not only the armour coat).
// Every frame of a match, for our body and every puppet, the things that decide whether a body is drawn are read and every CHANGE is logged
// with the paired-move context, so the next playtest log shows exactly which one flips when a body vanishes:
//   - the object's handle and process (winmm replaces puppets; a new process = a pop),
//   - the draw control (process +0x1b0): visible meshes (mesh entry +0x384) of all meshes, and its visibility byte +0x48,
//   - the root position (a jump > 0.75 m in one frame, NaN, or the world origin = the body is drawn elsewhere),
//   - the pelvis height over the root (a collapsed / exploded pose reads as a vanished body),
//   - hide-object-for-one-frame calls our own code made on that handle (logged with the calling address).
// Lines start with "visdiag:" (at most 4000 per session). Cheap: a few reads per body per frame.
// t2r-no-visdiag.txt next to the exe = off.
static int g_vdOff = -1;
static struct {
    uint64_t h, frame;
    void *ra;
} g_vd_hide[32];
static unsigned g_vdHideN;
static void VdNoteHide(uint64_t h, void *ra)
{                               // dc_call: hide-object-for-one-frame
    g_vd_hide[g_vdHideN & 31].h = h;
    g_vd_hide[g_vdHideN & 31].frame = g_frame;
    g_vd_hide[g_vdHideN & 31].ra = ra;
    g_vdHideN++;
}

static int VdHiddenByUs(uint64_t h, void **ra)
{                               // hidden by our code this frame or the last one
    for (unsigned i = 0; i < 32; i++)
        if (g_vd_hide[i].h == h && g_vd_hide[i].frame + 1 >= g_frame
            && g_vd_hide[i].frame) {
            if (ra)
                *ra = g_vd_hide[i].ra;
            return 1;
        }
    return 0;
}

typedef struct {
    uint64_t h;
    uint8_t *proc;
    int nv, nm, b48;
    float pos[3], pel;
    int hid;
    uint32_t ctx;
    int valid, listen, mark;
    int cv, cfar;
    float cd;
} VdStateT;
// the coat props (the entire body disappeared except head and pants; the torso IS the coat prop, a separate process the body
// mesh counts above never saw): visible coat meshes (original + armour copy) and how far the nearer coat prop sits from the body
static int CoatPeersOn(void);
static void VdCoat(uint64_t h, uint8_t *proc, const float *body, int *cv,
                    float *cd)
{
    *cv = -1;
    *cd = -1.0f;
    if (!proc || !h || ArmorRigOn() != 1)
        return;
    int look_i = ArmorRigTwinIdx(ObjLook(h));
    if (look_i < 0 || look_i >= ARMOR_RIG_N || !ARMOR_COAT[look_i])
        return;
    int disp = CoatSetDisp(proc);
    char cp[64];
    if (disp < 0
        || !ArmorCoatCopyName(ARMOR_COAT[look_i], cp, sizeof cp))
        return;
    const char *nm[2] = { ARMOR_COAT[look_i], cp };
    *cv = 0;
    for (int k = 0; k < 2; k++) {
        CoatPropT p;
        int v = 0;
        float q[3];
        if (CoatFind(proc, disp, nm[k], &p) != 1)
            continue;
        if (!CoatMeshState(p.dc, p.sid, &v))
            CoatMeshState(p.dc, 0, &v);
        *cv += v;
        if (v && ProcPos(p.proc, q)) {
            float dx = q[0] - body[0], dy = q[1] - body[1], dz =
                q[2] - body[2], d =
                __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
            if (*cd < 0 || d < *cd)
                *cd = d;
        }
    }
}

static VdStateT g_vd[T2R_MAX_PEERS + 1];
static unsigned g_vdLines;
static int GxJointPos(uint64_t h, const char *joint, float *out);
static int PsyncBusySlot(uint32_t sl);
static uint32_t VdCtx(int who)
{                               // bits: 0 psync, 1 gx, 2 special exec, 3 exec-while-downed, 4 downed, 5 dying, 6 busy slot, 7 ctxdown hands
    uint32_t c = 0;
    if (PsyncActive())
        c |= 1;
    if (g_gx.role && g_gx.until && g_t60 < g_gx.until)
        c |= 2;
    if (g_specialExecUntil > g_t60)
        c |= 4;
    if (g_execUntil)
        c |= 8;
    if (who ==
        T2R_US ? g_downed : (who < T2R_MAX_PEERS && g_peerDowned[who]))
        c |= 16;
    if (who == T2R_US && (g_uc4DyingUntil || g_downDying))
        c |= 32;
    if (who < T2R_MAX_PEERS && PsyncBusySlot((uint32_t) who))
        c |= 64;
    if (g_cdHandsOn)
        c |= 128;
    return c;
}

static void VdRead(int who, uint64_t h, uint8_t *proc, VdStateT *s)
{
    memset(s, 0, sizeof *s);
    s->h = h;
    s->proc = proc;
    s->nv = s->nm = s->b48 = -1;
    s->pel = -99.0f;
    if (proc && !IsBadReadPtr(proc, 0x1b8)) {
        uint8_t *dc = *(uint8_t **) (proc + 0x1b0);
        if (dc && !IsBadReadPtr(dc, 0x50)) {
            s->b48 = dc[0x48];
            s->nm = CoatMeshState(dc, 0, &s->nv);
        }
        float t[8];
        if (ProcWorld(proc, t))
            memcpy(s->pos, t, 12);
    }
    float pel[3];
    if (h && GxJointPos(h, "pelvis", pel) && pel[1] == pel[1])
        s->pel = pel[1] - s->pos[1];
    s->hid = VdHiddenByUs(h, NULL);
    s->ctx = VdCtx(who);
    s->listen = who < T2R_MAX_PEERS ? g_listenOn[who] : -1;    // listen-mode X-ray fill (the ghosted executioner)
    s->mark = who < T2R_MAX_PEERS ? g_markBuddyOn[who] : 0;   // Hawk-Eyed mark outline
    s->valid = 1;
}

// ---- pose pop (characters never jitter to their standing poses): a body that was lying / the victim of a paired move whose pelvis rises above
// 0.8 m for 2 frames (rise), or a body that drops 0.5 m or more in one frame to under 0.3 m (collapse). The line names what we last forced on it.
// "pose pop: <body> pelvis X -> Y m while <state> (cause hints: ...)". At most one per body per 60 frames, 300 per session.
typedef struct {
    uint64_t h;
    int lie_n, up_n;
    uint64_t lie_frame, last_log;
    float prev_pel, lie_pel;
    int prev_ok;
} VdPopT;
static VdPopT g_vp[T2R_MAX_PEERS + 1];
static unsigned g_vpLines;
static int VdIsVictim(int who)
{                               // the victim half of a paired move / execution plays on this body
    if (who == T2R_US)
        return g_ps.role == 2 || (g_gx.role == 2 && g_gx.until
                                  && g_t60 < g_gx.until);
    return (g_ps.role && g_ps.vic == who) || (g_gx.role == 1
                                              && g_gx.slot == who
                                              && g_gx.until
                                              && g_t60 < g_gx.until);
}

static int VdReviving(int who)
{                               // a revive runs on it (the get-up is the move's end, not a pop)
    if (who == T2R_US)
        return RvVictimMove() || g_revBy < T2R_MAX_PEERS || g_getupPre;
    return who < T2R_MAX_PEERS && (RvPupSlot((uint32_t) who)
                                   || (g_revStart[who]
                                       && GetTickCount64() -
                                       g_revStartMs[who] < 15000)
                                   || (g_uc4GetupEnd[who]
                                       && g_t60 <
                                       g_uc4GetupEnd[who] + 60));
}

static void VdPop(int who, const VdStateT *n)
{
    VdPopT *p = &g_vp[who];
    if (!n->valid || n->pel < -50.0f || n->h != p->h) {
        memset(p, 0, sizeof *p);
        p->h = n->h;
        if (!n->valid || n->pel < -50.0f)
            return;
    }
    int down = (n->ctx & 16) != 0, dying = (n->ctx & 32) != 0, vic =
        VdIsVictim(who);
    int hold = down || vic || dying;
    float pel = n->pel;
    const char *kind = NULL;
    float from = 0;
    if (hold && pel < 0.5f) {
        p->lie_n++;
        p->up_n = 0;
        if (p->lie_n >= 3) {
            p->lie_frame = g_frame;
            p->lie_pel = pel;
        }
    } else if (pel > 0.8f) {
        if (hold && p->lie_frame && g_frame - p->lie_frame <= 120
            && !VdReviving(who)) {
            if (++p->up_n >= 2) {
                kind = "rise";
                from = p->lie_pel;
            }
        } else if (!hold) {
            p->lie_frame = 0;
            p->up_n = 0;
        }
        p->lie_n = 0;
    } else {
        p->lie_n = 0;
        p->up_n = 0;
    }
    if (!kind && p->prev_ok && p->prev_pel > 0.7f && pel < 0.3f
        && p->prev_pel - pel >= 0.5f && !dying
        && (n->ctx & (1 | 2 | 4 | 8 | 16 | 64 | 128))) {
        kind = "collapse";
        from = p->prev_pel;
    }
    p->prev_pel = pel;
    p->prev_ok = 1;
    if (!kind || g_vpLines >= 300
        || (p->last_log && g_frame - p->last_log < 60))
        return;
    p->last_log = g_frame;
    g_vpLines++;
    char hint[220];
    FaHint(who == T2R_US ? SID_PLAYER : n->h, hint, sizeof hint);
    const char *uc =
        who == T2R_US ? (g_uc4_me.cur ? g_uc4_me.cur : "-") : (who <
                                                               T2R_MAX_PEERS
                                                               &&
                                                               g_uc4_pup
                                                               [who].
                                                               cur ?
                                                               g_uc4_pup
                                                               [who].
                                                               cur : "-");
    Xlog("pose pop (%s): %s%d frame %llu pelvis %.2f -> %.2f m while %s%s%s%s%s (cause hints: last forced clips: %s | uc4 loop %s, net downed state %d, lying %llu f ago, psync role %d uid %08x, ctx %02x, revive/getup %d)", kind, who == T2R_US ? "own" : "slot ", who == T2R_US ? 0 : who, (unsigned long long) g_frame, from, pel, down ? "downed" : "", dying ? " dying" : "", vic ? " paired-victim" : "", (n->ctx & 4) ? " special-exec" : "", (n->ctx & 64) ? " busy-slot" : "", hint, uc, who < T2R_MAX_PEERS ? (int) g_peerDowned[who] : (int) g_downed, p->lie_frame ? (unsigned long long) (g_frame - p->lie_frame) : 0ULL, PsyncRole(), g_ps.uid, n->ctx, VdReviving(who));
}

static void VdTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef void *(*fn_proc)(uint64_t);
    typedef uint8_t *(*fn_me)(void);
    if (g_vdOff < 0) {
        FILE *f = ExeSideOpen("t2r-no-visdiag.txt", "r");
        g_vdOff = f != NULL;
        if (f)
            fclose(f);
        if (!g_vdOff)
            Xlog("visdiag: on (body visibility changes are logged; t2r-no-visdiag.txt = off)");
    }
    if (g_vdOff || MatchSubstate() != 6) {
        memset(g_vd, 0, sizeof g_vd);
        memset(g_vp, 0, sizeof g_vp);
        return;
    }
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    for (int who = 0; who <= T2R_US; who++) {
        uint64_t h = 0;
        uint8_t *proc = NULL;
        if (who == T2R_US) {
            h = SID_PLAYER;
            proc = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
        } else if (who < T2R_MAX_PEERS && (uint32_t) who != self) {
            uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) who);
            if (!peer || !peer[4]) {
                g_vd[who].valid = 0;
                continue;
            }
            h = *(uint64_t *) (peer + 0x60);
            proc =
                h ? (uint8_t *) ((fn_proc)
                                 T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
        } else
            continue;
        VdStateT n, *o = &g_vd[who];
        VdRead(who, h, proc, &n);
        n.cv = -1;
        n.cd = -1.0f;
        if (who == T2R_US || CoatPeersOn())
            VdCoat(h, proc, n.pos, &n.cv, &n.cd);      // (peer coat props: crash, see coat_jump_check)
        n.cfar = n.cd > 1.5f;
        VdPop(who, &n);
        if (!o->valid) {
            *o = n;
            continue;
        }
        char why[160];
        int k = 0;
        why[0] = 0;
        float dx = n.pos[0] - o->pos[0], dy = n.pos[1] - o->pos[1], dz =
            n.pos[2] - o->pos[2], jump =
            __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        if (n.h != o->h)
            k += snprintf(why + k, SNLEFT(why, k), " handle");
        if (n.proc != o->proc)
            k += snprintf(why + k, SNLEFT(why, k), " process");
        if (n.nv != o->nv || n.nm != o->nm)
            k += snprintf(why + k, SNLEFT(why, k), " meshes");
        if (n.b48 != o->b48)
            k += snprintf(why + k, SNLEFT(why, k), " dc+48");
        if (jump > 0.75f || !(jump == jump))
            k += snprintf(why + k, SNLEFT(why, k), " pos-jump");
        if ((n.pel < 0.15f) != (o->pel < 0.15f)
            || (n.pel > 1.6f) != (o->pel > 1.6f))
            k += snprintf(why + k, SNLEFT(why, k), " pose");
        if (n.listen != o->listen)
            k += snprintf(why + k, SNLEFT(why, k), " listen-outline");
        if (n.mark != o->mark)
            k += snprintf(why + k, SNLEFT(why, k), " mark-outline");
        if (n.hid != o->hid)
            k += snprintf(why + k, SNLEFT(why, k), " hidden-by-us");
        if (n.cv != o->cv)
            k += snprintf(why + k, SNLEFT(why, k), " coat-meshes");
        if (n.cfar != o->cfar)
            k += snprintf(why + k, SNLEFT(why, k),
                          n.
                          cfar ? " COAT-OFF-BODY" : " coat-back-on-body");
        if (k && (g_vdLines++ < 4000 || g_debugMode)) {
            void *ra = NULL;
            VdHiddenByUs(n.h, &ra);
            Xlog("visdiag: %s%d frame %llu [%s ]: meshes %d/%d -> %d/%d, dc+48 %02x -> %02x, proc %p -> %p, pos %.2f %.2f %.2f (moved %.2f m), pelvis %.2f -> %.2f m, hidden by us %d%s%p | ctx %02x (psync role %d, gx %d, special %d, exec %d, downed %d, dying %d, busy %d, cd %d) listen %d mark %d substate %d", who == T2R_US ? "own" : "slot ", who == T2R_US ? 0 : who, (unsigned long long) g_frame, why, o->nv, o->nm, n.nv, n.nm, o->b48 & 0xff, n.b48 & 0xff, (void *) o->proc, (void *) n.proc, n.pos[0], n.pos[1], n.pos[2], jump, o->pel, n.pel, n.hid, n.hid ? " from " : "", n.hid ? (void *) ((uint8_t *) ra - (uint8_t *) & __ImageBase) : NULL, n.ctx, PsyncRole(), g_gx.role, (n.ctx & 4) != 0, (n.ctx & 8) != 0, (n.ctx & 16) != 0, (n.ctx & 32) != 0, (n.ctx & 64) != 0, (n.ctx & 128) != 0, n.listen, n.mark, MatchSubstate());
            Xlog("visdiag: %s%d coat: visible meshes %d -> %d, coat prop %.2f m -> %.2f m from the body", who == T2R_US ? "own" : "slot ", who == T2R_US ? 0 : who, o->cv, n.cv, o->cd, n.cd);
        }
        *o = n;
    }
}

// ---------------------------------------------------------------- torso keep (torsos still disappeared during special executions: the whole torso piece)
// What the exe does with a mesh piece's visibility (DrawControl update, exe 0x141486550 / 0x141486dd0, read from the disassembly):
//   draw byte (submesh object +0x1b0) = inst+0x384 (script mask, set-mesh-visible) & inst+0x386 & dc+0x4a & dc+0x49 & dc+0x48
//     then 0 when dc+0x50 != -1 and >= the engine's time value (hide-object-for-one-frame),
//     then 0 when (dc+0x40 bit 2 ? inst+0x38a bit 0 : inst+0x38a bit 1) = a per-piece "hide in this mode" flag,
//   and the camera fade (Character::CameraObscured, 0x1CE9650 - already answered 'not obscured' in our windows) sets bit 4 of submesh +0x1b4.
// visdiag only watched the script mask (+0x384) and dc+0x48 - the proof (a log: no mesh-count / dc+48 change in any execution) that the torso
// does NOT vanish through those. This reads the pieces the visdiag did not: for every TORSO piece (the look's torso, its pushed armour copy) of every
// body, each 250 ms while a paired move / the execution camera runs: all inputs of the formula above and the resulting draw byte, "torso: ..." lines;
// and when a torso piece with a visible script mask has a 0 draw byte for a reason that is only the mode flag / the camera bit (none of the script,
// dc+0x48-4a, hide-one-frame inputs), the draw byte is set again (and bit 4 cleared) every frame of the window: "torso: KEPT ..." (first 60 logged).
// t2r-no-torso-keep.txt next to the exe = log only.
static uint64_t g_tsSid[40];
static int g_tsN = -1;
static int TsIsTorso(uint64_t sid)
{
    if (g_tsN < 0) {
        g_tsN = 0;
        for (int l = 0; l < ARMOR_RIG_N; l++)
            for (int k = 0; k < ARMOR_RIG_BODY_N; k++) {
                const ArmorBodyT *b = &ARMOR_RIG_BODY[l][k];
                const char *nm[2] = { b->orig, b->copy };
                for (int q = 0; q < 2; q++) {
                    if (!nm[q] || !strstr(nm[q], "torso")
                        || !(b->mask & 2))
                        continue;
                    uint64_t s = FNV64(nm[q]);
                    int dup = 0;
                    for (int i = 0; i < g_tsN; i++)
                        if (g_tsSid[i] == s)
                            dup = 1;
                    if (!dup && g_tsN < 40)
                        g_tsSid[g_tsN++] = s;
                }
            }
    }
    for (int i = 0; i < g_tsN; i++)
        if (g_tsSid[i] == sid)
            return 1;
    return 0;
}

static void TsBody(int who, uint64_t h, uint8_t *proc, int log_now,
                    int keep_on)
{
    static unsigned nlines, nkept;
    if (!proc || IsBadReadPtr(proc, 0x1b8))
        return;
    uint8_t *dc = *(uint8_t **) (proc + 0x1b0);
    if (!dc || IsBadReadPtr(dc, 0x58))
        return;
    uint32_t n = *(uint32_t *) (dc + 0x14);
    uint8_t *a = *(uint8_t **) (dc + 8);
    if (!n || n > 256 || !a || IsBadReadPtr(a, (size_t) n * 0x390))
        return;
    uint32_t dflags = *(uint32_t *) (dc + 0x40);
    int pending = *(int64_t *) (dc + 0x50) != -1;
    int dcvis = dc[0x48] && dc[0x49] && dc[0x4a];
    char buf[900];
    int bl = 0, ntorso = 0, nother_hid = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *in = a + (size_t) i * 0x390;
        uint64_t sid = *(uint64_t *) (in + 0x28);
        int torso = TsIsTorso(sid), nsub = in[0x383];
        int mvis = (in[0x384] & in[0x386]) != 0;
        int modeflag =
            (dflags & 4) ? (in[0x38a] & 1) : ((in[0x38a] >> 1) & 1);
        if (nsub > 19)
            nsub = 19;
        int fin = 0, cam = 0, got = 0;
        for (int j = 0; j < nsub; j++) {
            uint8_t *so = *(uint8_t **) (in + (size_t) j * 0x30);
            if (!so || IsBadReadPtr(so, 0x1b8))
                continue;
            got++;
            if (so[0x1b0])
                fin++;
            if (so[0x1b4] & 0x10)
                cam++;
        }
        if (!torso) {
            if (got && !fin && mvis && dcvis)
                nother_hid++;
            continue;
        }
        ntorso++;
        // a torso piece that should draw (script mask, dc+0x48-4a, no hide-one-frame) but doesn't: only the mode flag / the camera bit can be the reason
        int want = mvis && dcvis && !pending, hidden_by_flag = want && got
            && (!fin || cam) && (modeflag || cam);
        if (hidden_by_flag && keep_on && !VdHiddenByUs(h, NULL)) {
            for (int j = 0; j < nsub; j++) {
                uint8_t *so = *(uint8_t **) (in + (size_t) j * 0x30);
                if (!so || IsBadReadPtr(so, 0x1b8))
                    continue;
                so[0x1b0] = 1;
                so[0x1b4] &= (uint8_t) ~ 0x10;
            }
            if (LOGCAP(nkept, 60))
                Xlog("torso: KEPT %s%d piece %016llx frame %llu - draw byte was 0 on %d/%d sub-meshes (camera bit %d, mode flag %d with dc+0x40 bit2 %d, script mask %02x/%02x, dc+48/49/4a %02x %02x %02x, hide-one-frame %d): set to drawn", who == T2R_US ? "own" : "slot ", who == T2R_US ? 0 : who, (unsigned long long) sid, (unsigned long long) g_frame, got - fin, got, cam, modeflag, (dflags >> 2) & 1, in[0x384], in[0x386], dc[0x48], dc[0x49], dc[0x4a], pending);
        }
        if (log_now && bl < (int) sizeof buf - 160)
            bl +=
                snprintf(buf + bl, SNLEFT(buf, bl),
                         " | %016llx m384 %02x m385 %02x m386 %02x m387 %02x f38a %02x drawn %d/%d camhide %d%s",
                         (unsigned long long) sid, in[0x384], in[0x385],
                         in[0x386], in[0x387], in[0x38a], fin, got, cam,
                         (!fin && got) ? " HIDDEN" : "");
    }
    if (log_now && nlines < 1500 && ntorso) {
        nlines++;
        Xlog("torso: %s%d frame %llu xcam %d psync %d | dc+40 %08x (bit2 %d) dc+48/49/4a %02x %02x %02x dc+50 %s | %d torso piece(s), %d other piece(s) not drawn despite masks%s", who == T2R_US ? "own" : "slot ", who == T2R_US ? 0 : who, (unsigned long long) g_frame, g_xcamOn, PsyncActive(), dflags, (dflags >> 2) & 1, dc[0x48], dc[0x49], dc[0x4a], pending ? "PENDING" : "-1", ntorso, nother_hid, buf);
    }
}

static void TsTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef void *(*fn_proc)(uint64_t);
    typedef uint8_t *(*fn_me)(void);
    static int off = -1;
    static uint64_t next_log;
    if (off < 0) {
        FILE *f = ExeSideOpen("t2r-no-torso-keep.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
    }
    // R-TORSO proof line: the periodic "torso:" lines below ran into the release rate limit (XL_BURST 10 per 10 s = 2 ticks x 5 bodies of a move; the
    // a log has nothing of the downed time after the finisher). Our own body gets an EDGE-triggered line instead - a changed signature of
    // every torso piece's draw inputs + the engine's real near-camera answer + the body's height - while a paired move runs, we are downed / dying
    // and for 4 s after: "torso own: ..." (first 200).
    {
        static uint64_t tail_until, sig_prev;
        static unsigned nown;
        if (MatchSubstate() == 6 && (PsyncActive() || g_xcamOn))
            tail_until = g_frame + 240;
        int act = MatchSubstate() == 6 && (PsyncActive() || g_xcamOn
                                            || g_downed || g_downDying
                                            || g_frame < tail_until);
        if (act && nown < 200) {
            uint8_t *proc = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
            uint8_t *dc = proc
                && !IsBadReadPtr(proc,
                                 0x1b8) ? *(uint8_t **) (proc +
                                                         0x1b0) : NULL;
            uint32_t n = dc
                && !IsBadReadPtr(dc, 0x58) ? *(uint32_t *) (dc + 0x14) : 0;
            uint8_t *a = n && n <= 256 ? *(uint8_t **) (dc + 8) : NULL;
            if (a && !IsBadReadPtr(a, (size_t) n * 0x390)) {
                uint64_t sig = 1469598103934665603ULL;
                int nt = 0, ndrawn = 0, ncam = 0;
                char pb[300];
                int pl = 0;
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t *in = a + (size_t) i * 0x390;
                    if (!TsIsTorso(*(uint64_t *) (in + 0x28)))
                        continue;
                    int nsub = in[0x383] > 19 ? 19 : in[0x383], fin =
                        0, cam = 0, got = 0;
                    for (int j = 0; j < nsub; j++) {
                        uint8_t *so =
                            *(uint8_t **) (in + (size_t) j * 0x30);
                        if (!so || IsBadReadPtr(so, 0x1b8))
                            continue;
                        got++;
                        if (so[0x1b0])
                            fin++;
                        if (so[0x1b4] & 0x10)
                            cam++;
                    }
                    nt++;
                    ndrawn += fin;
                    ncam += cam;
                    sig =
                        (sig ^
                         ((uint64_t) in[0x384] | (uint64_t) in[0x386] << 8
                          | (uint64_t) fin << 16 | (uint64_t) cam << 24 |
                          (uint64_t) got << 32)) * 1099511628211ULL;
                    if (pl < (int) sizeof pb - 90)
                        pl +=
                            snprintf(pb + pl, SNLEFT(pb, pl),
                                     " | %016llx m384 %02x m386 %02x drawn %d/%d camhide %d",
                                     (unsigned long long) *(uint64_t *) (in
                                                                         +
                                                                         0x28),
                                     in[0x384], in[0x386], fin, got, cam);
                }
                int real = -1;  // what the engine's near-camera test answers for our body right now (the detour hides nothing from it: out = NULL)
                if (g_nofade_tramp && proc)
                    real = g_nofade_tramp(proc, 1, NULL) ? 1 : 0;
                uint32_t dfl = *(uint32_t *) (dc + 0x40);
                sig =
                    (sig ^
                     (uint64_t) (dc[0x48] | dc[0x49] << 8 | dc[0x4a] << 16
                                 | (uint64_t) (real +
                                               1) << 24 | (uint64_t) (dfl &
                                                                      3) <<
                                 32 | (uint64_t) g_downed << 40 |
                                 (uint64_t) PsyncActive() << 41 |
                                 (uint64_t) g_xcamOn << 42)) *
                    1099511628211ULL;
                if (nt && sig != sig_prev) {
                    sig_prev = sig;
                    nown++;
                    float t[8];
                    int pw = ProcWorld(proc, t);
                    Xlog("torso own: frame %llu psync %d xcam %d downed %d dying %d tail %d | engine near-camera answer %d, dc+40 %08x dc+48/49/4a %02x %02x %02x | %d torso piece(s), %d sub-meshes drawn, %d camera-hidden | root y %.2f%s", (unsigned long long) g_frame, PsyncActive(), g_xcamOn, g_downed, g_downDying, g_frame < tail_until, real, dfl, dc[0x48], dc[0x49], dc[0x4a], nt, ndrawn, ncam, pw ? t[1] : -99.f, pb);
                }
            }
        }
    }
    if (MatchSubstate() != 6 || !(PsyncActive() || g_xcamOn))
        return;
    uint64_t now = GetTickCount64();
    int log_now = now >= next_log;
    if (log_now)
        next_log = now + 250;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    for (int who = 0; who <= T2R_US; who++) {
        uint64_t h = 0;
        uint8_t *proc = NULL;
        if (who == T2R_US) {
            h = SID_PLAYER;
            proc = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
        } else if (who < T2R_MAX_PEERS && (uint32_t) who != self) {
            uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) who);
            if (!peer || !peer[4])
                continue;
            h = *(uint64_t *) (peer + 0x60);
            proc =
                h ? (uint8_t *) ((fn_proc)
                                 T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
        } else
            continue;
        TsBody(who, h, proc, log_now, !off);
    }
}

// ---------------------------------------------------------------- broken helmet visor (make the shield visibly gone)
// Root cause of the first version (b2787015 + the one after it): it treated the helmet instance's sub-mesh objects as shell/visor pairs and
// zeroed the draw byte (+0x1b0) of the odd ones. The live instance has 4 sub-mesh objects (in[0x383] = 4) - and those are the 4 LODs, NOT 8 submeshes:
// the instance builder (exe 0x1414877a0..) makes one object per LOD geometry resource, and each LOD geometry holds BOTH draws (shell shader0 +
// visor shader1; pak: helmet_mesh_LODShape0..3_shader0/1). Zeroing odd objects therefore hid LOD1 + LOD3 = the whole helmet at the distance the
// attacker looks from (log: "face-shield sub-meshes (odd of 4) draw byte -> 0 (draw bytes before: 1111, 2 changed)" on both clients), the hat
// garment was already hidden for the helmet -> bald; the victim's own first-person body never draws its helmet, so nothing showed there.
// The per-draw level lives one step lower: object +0x1e0 = the render mesh R (exe 0x1415e5360: entries at R+0x140, stride 0x200, count
// *(u32*)(R+0x170) & 0x1ffff; entry +0x40 = the draw's material, entry +0xd8 = its index count (copied from the geometry at build, the draw packet
// 0x1415ea3d0 reads it, +0xd4 u16)). The visor draw is the entry whose material is the fedra-helmet-VISOR one; while g_visor_broken[body] its index
// count is 0 (nothing drawn, the shell entry is never touched), saved per entry and written back when the shield is whole again.
// Visor entry = material name (a pointer scan of the material finds a string containing "visor"), else with exactly 2 entries of different
// materials the one with the clearly smaller index count (pak: visor 1326/696/324/90 vs shell 10200/5064/1980/486). "visor draw:" lines say what was
// chosen and the before / after counts (once per body). t2r-no-visor-hide.txt next to the exe = no hiding. Every client runs it for every body it sees.
static int VzStrVisor(const char *s)
{
    size_t n = 0;
    while (n < 160 && s[n] >= 0x20 && s[n] < 0x7f)
        n++;
    if (n < 5 || n >= 160)
        return 0;
    char t[161];
    for (size_t i = 0; i < n; i++)
        t[i] = (char) ((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    t[n] = 0;
    return strstr(t, "visor") != NULL;
}

static int VzMatIsVisor(uint8_t *mat, char *why, size_t wn)
{
    if (!mat || IsBadReadPtr(mat, 0x200))
        return 0;
    for (int off = 0; off < 0x200; off += 8) {
        uint64_t q = *(uint64_t *) (mat + off);
        if (q < 0x10000 || q > 0x7fffffffffffull)
            continue;
        if (!IsBadReadPtr((void *) q, 168)) {
            if (VzStrVisor((const char *) q)) {
                snprintf(why, wn, "name at material+0x%x", off);
                return 1;
            }
            uint64_t q2 = *(uint64_t *) q;      // one more hop (material -> asset record -> name)
            if (q2 >= 0x10000 && q2 <= 0x7fffffffffffull
                && !IsBadReadPtr((void *) q2, 168)
                && VzStrVisor((const char *) q2)) {
                snprintf(why, wn, "name at material+0x%x->0", off);
                return 1;
            }
        }
    }
    return 0;
}

static void VzBody(int who, uint64_t h, uint8_t *proc)
{
    static uint8_t applied[T2R_MAX_PEERS + 1];
    static unsigned nlog;
    static struct {
        uint8_t *e;
        uint8_t *mat;
        uint32_t orig;
    } Sv[T2R_MAX_PEERS + 1][24];
    static struct {
        uint8_t *e, *mat;
        int vis;
        char why[40];
    } cc[48];
    static int ncc;
    static uint64_t sidm, sidf;
    if (!sidm) {
        sidm = FNV64("slaver-male-helmet-02");
        sidf = FNV64("slaver-fhelm1-rattler-helmet-02");
    }
    int want = g_visorBroken[who] != 0;
    if (!want && !applied[who])
        return;
    if (!proc || IsBadReadPtr(proc, 0x1b8))
        return;
    uint8_t *dc = *(uint8_t **) (proc + 0x1b0);
    if (!dc || IsBadReadPtr(dc, 0x58))
        return;
    uint32_t n = *(uint32_t *) (dc + 0x14);
    uint8_t *a = *(uint8_t **) (dc + 8);
    if (!n || n > 256 || !a || IsBadReadPtr(a, (size_t) n * 0x390))
        return;
    const char *wl = who == T2R_US ? "own" : "slot ";
    int wi = who == T2R_US ? 0 : who;
    int found = 0, nhid = 0, nback = 0, nbad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *in = a + (size_t) i * 0x390;
        uint64_t sid = *(uint64_t *) (in + 0x28);
        if (sid != sidm && sid != sidf)
            continue;
        int nsub = in[0x383];
        if (nsub < 1 || nsub > 8)
            continue;
        char desc[240] = "";
        int dl = 0;
        for (int j = 0; j < nsub; j++) {
            uint8_t *so = *(uint8_t **) (in + (size_t) j * 0x30);
            if (!so || IsBadReadPtr(so, 0x1e8))
                continue;
            uint8_t *R = *(uint8_t **) (so + 0x1e0);
            if (!R || IsBadReadPtr(R, 0x180))
                continue;
            uint32_t ne = *(uint32_t *) (R + 0x170) & 0x1ffff;
            uint8_t *ent = *(uint8_t **) (R + 0x140);
            if (ne < 1 || ne > 8 || !ent
                || IsBadReadPtr(ent, (size_t) ne * 0x200))
                continue;
            found = 1;
            // classify the draws of this LOD (cached per entry + material pointer: the material scan is not per-frame work)
            int vis[8] = { 0 };
            const char *how[8] = { 0 };
            for (uint32_t k = 0; k < ne; k++) {
                uint8_t *e = ent + (size_t) k * 0x200, *mat =
                    *(uint8_t **) (e + 0x40);
                int hit = -1;
                for (int c = 0; c < ncc; c++)
                    if (cc[c].e == e && cc[c].mat == mat) {
                        hit = c;
                        break;
                    }
                if (hit < 0) {
                    int c =
                        ncc <
                        48 ? ncc++ : (int) (((uintptr_t) e >> 9) % 48);
                    cc[c].e = e;
                    cc[c].mat = mat;
                    cc[c].why[0] = 0;
                    cc[c].vis =
                        VzMatIsVisor(mat, cc[c].why, sizeof cc[c].why);
                    hit = c;
                }
                vis[k] = cc[hit].vis;
                how[k] = cc[hit].why;
            }
            int anyvis = 0;
            for (uint32_t k = 0; k < ne; k++)
                anyvis |= vis[k];
            if (!anyvis && ne == 2) {   // no name found: the clearly smaller draw of two different materials
                uint32_t c0 = *(uint32_t *) (ent + 0xd8), c1 =
                    *(uint32_t *) (ent + 0x200 + 0xd8);
                uint8_t *m0 = *(uint8_t **) (ent + 0x40), *m1 =
                    *(uint8_t **) (ent + 0x200 + 0x40);
                // a draw we zeroed earlier reads 0: use its saved count
                for (int s = 0; s < 24; s++) {
                    if (Sv[who][s].e == ent)
                        c0 = Sv[who][s].orig;
                    if (Sv[who][s].e == ent + 0x200)
                        c1 = Sv[who][s].orig;
                }
                if (m0 != m1 && c0 && c1) {
                    if (c1 * 2 <= c0) {
                        vis[1] = 1;
                        how[1] = "smaller of 2 draws";
                    } else if (c0 * 2 <= c1) {
                        vis[0] = 1;
                        how[0] = "smaller of 2 draws";
                    }
                }
            }
            for (uint32_t k = 0; k < ne; k++) {
                uint8_t *e = ent + (size_t) k * 0x200;
                uint32_t *cnt = (uint32_t *) (e + 0xd8);
                int s = -1, fr = -1;
                for (int q = 0; q < 24; q++) {
                    if (Sv[who][q].e == e) {
                        s = q;
                        break;
                    }
                    if (fr < 0 && !Sv[who][q].e)
                        fr = q;
                }
                if (dl < (int) sizeof desc - 60)
                    dl +=
                        snprintf(desc + dl, SNLEFT(desc, dl),
                                 " [lod%d R=%p draw%u %s idx %u%s%s]", j,
                                 (void *) R, k, vis[k] ? "VISOR" : "shell",
                                 *cnt, vis[k] ? " by " : "",
                                 vis[k] ? how[k] : "");
                if (!vis[k])
                    continue;
                if (want) {
                    if (s < 0) {
                        if (fr < 0 || *cnt == 0) {
                            nbad++;
                            continue;
                        }
                        s = fr;
                        Sv[who][s].e = e;
                        Sv[who][s].mat = *(uint8_t **) (e + 0x40);
                        Sv[who][s].orig = *cnt;
                    }
                    if (*cnt) {
                        *cnt = 0;
                        nhid++;
                    }
                } else if (s >= 0) {
                    if (Sv[who][s].mat == *(uint8_t **) (e + 0x40)
                        && *cnt == 0) {
                        *cnt = Sv[who][s].orig;
                        nback++;
                    }
                    Sv[who][s].e = NULL;
                }
            }
        }
        if (want && !applied[who] && nlog < 40) {
            nlog++;
            Xlog("visor draw: %s%d helmet %s (nsub %d LOD objects, render-mesh draws before):%s", wl, wi, sid == sidm ? "slaver-male-helmet-02" : "slaver-fhelm1-rattler-helmet-02", nsub, desc);
        }
    }
    if (want && found) {
        if (!applied[who] && nlog < 40) {
            nlog++;
            Xlog("visor draw: %s%d face shield draws -> index count 0 (%d zeroed now, %d unusable)", wl, wi, nhid, nbad);
        }
        applied[who] = 1;
    } else if (!want && applied[who]) {
        applied[who] = 0;
        for (int q = 0; q < 24; q++)
            Sv[who][q].e = NULL;
        if (LOGCAP(nlog, 40))
            Xlog("visor draw: %s%d face shield whole again - %d draws restored", wl, wi, nback);
    }
}

static void VzTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef void *(*fn_proc)(uint64_t);
    typedef uint8_t *(*fn_me)(void);
    static int off = -1;
    // 3 crashes (exe+16744a3 x2, exe+158633e) one frame after vz_body zeroed the visor draws' index count - the
    // engine's render jobs read that entry in the same frame (null render state at +0x278). Off by default now: t2r-visor-hide.txt = on (the glass
    // burst, sound and voice of the break stay).
    if (off < 0) {
        FILE *f = ExeSideOpen("t2r-visor-hide.txt", "r");
        off = f == NULL;
        if (f)
            fclose(f);
    }
    if (off || MatchSubstate() != 6)
        return;
    int any = 0;
    for (int i = 0; i <= T2R_US; i++)
        any |= g_visorBroken[i];
    static int was_any;
    if (!any && !was_any)
        return;
    was_any = any;
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    for (int who = 0; who <= T2R_US; who++) {
        uint64_t h = 0;
        uint8_t *proc = NULL;
        if (who == T2R_US) {
            h = SID_PLAYER;
            proc = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
        } else if (who < T2R_MAX_PEERS && (uint32_t) who != self) {
            uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) who);
            if (!peer || !peer[4])
                continue;
            h = *(uint64_t *) (peer + 0x60);
            proc =
                h ? (uint8_t *) ((fn_proc)
                                 T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
        } else
            continue;
        VzBody(who, h, proc);
    }
}

// ---------------------------------------------------------------- body catcher (the torso piece still disappeared during special executions)
// The 'torso:' lines read every draw input we knew of and all said "drawn" for the whole 1.1 s the shirt was gone (log f62272), so this catcher
// reads what we did NOT know: from the frame a paired move / the execution camera / a down / a revive / a re-instance (new handle or process =
// respawn) starts, for 3 s, EVERY frame, for our body and every puppet:
//   * every mesh instance of the draw control (all of them, not only torso names): the instance's whole 0x390 bytes and the first 0x200 bytes of
//     each of its first 4 LOD objects are diffed against the previous frame; every changed dword is logged with its offset (old -> new). A dword
//     that keeps changing (matrices, bounds that follow the body, timers) is logged 3 times and then muted for the window - what is left are the
//     fields that flip once at the start and back at the end (a cull / LOD / fade / dissolve field we do not know the offset of yet);
//   * a write watch (DR0, debug builds, the hpw helper) is armed on the FIRST dword that changes in the instance's 0x380..0x390 masks or a LOD
//     object's 0x1a0..0x1c0 draw block / +0x30 skip bits - the writer that flips it back is logged with its stack ("bodycatch watch: write at ...");
//   * the skeleton: 18 joints (main chain + the arm twist / helper joints the shirt is skinned to; the backpack shares the spine and clavicle
//     ones) relative to the pelvis, and the six arm bone lengths. Logged when a joint becomes unreadable / NON-FINITE, moves > 0.20 m against
//     the pelvis, or an arm bone length leaves +-25 % of the window's first frame ("ARM BONES COLLAPSED / STRETCHED"): skinning collapse shows here.
// Lines start with "bodycatch:" (120 per window, 3000 per session). t2r-no-bodycatch.txt next to the exe = off.
#define BC_INST 32
#define BC_LOD 4
#define BC_IN_SZ 0x390
#define BC_SO_SZ 0x200
#define BC_NJ 18
static const char *const BC_J[BC_NJ] =
    { "pelvis", "spinea", "spined", "neck", "headb", "l_clavicle",
"l_shoulder", "l_elbow", "l_wrist",
    "r_clavicle", "r_shoulder", "r_elbow", "r_wrist", "m_twist_03",
        "m_twist_04", "l_twist_02", "r_twist_02", "l_deltoid_helper"
};

typedef struct {
    uint64_t h;
    uint8_t *proc;
    uint64_t until, open_frame;
    unsigned lines, tot;
    int watched, valid, bwas;
    int ni;
    uint64_t sid[BC_INST];
    uint8_t *ia[BC_INST];
    uint8_t in[BC_INST][BC_IN_SZ], so[BC_INST][BC_LOD][BC_SO_SZ];
    uint8_t nin[BC_INST][BC_IN_SZ / 4], nso[BC_INST][BC_LOD][BC_SO_SZ / 4];     // change counts per dword (muted after 3)
    float j0[BC_NJ][3], jl[BC_NJ][3], bl0[6];
    uint8_t js0[BC_NJ], jsl[BC_NJ];
    int prev_ps, prev_xc, prev_dn, prev_rv;
} BcBodyT;
static BcBodyT *g_bc;         // T2R_US + 1 bodies, allocated on first use (~0.7 MB)
static int BcJoint(uint64_t h, const char *joint, float *out)
{                               // 0 no locator, 1 ok, 2 non-finite
    typedef char (*fn_lpos)(void *, float *);
    uint64_t a[2] = { h, Fnv64Rt(joint) };
    uint64_t r =
        h ? DcCall(0x6b8812bd30c5cd5dULL /* get-joint-locator */ , 2,
                    a) : 0;
    float t[4];
    out[0] = out[1] = out[2] = 0.0f;
    if (!r || r == ~0ULL || IsBadReadPtr((void *) r, 16)
        || !((fn_lpos) T2R_SYM(kFnLocatorPos)) ((void *) r, t))
        return 0;
    memcpy(out, t, 12);
    return __builtin_isfinite(t[0]) && __builtin_isfinite(t[1])
        && __builtin_isfinite(t[2]) ? 1 : 2;
}

static float BcD(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return __builtin_sqrtf(x * x + y * y + z * z);
}

static void BcBones(float (*p)[3], const uint8_t *s, float *bl)
{                               // l clavicle-shoulder, shoulder-elbow, elbow-wrist, then r
    static const int B[6][2] =
        { { 5, 6 }, { 6, 7 }, { 7, 8 }, { 9, 10 }, { 10, 11 }, { 11,
                                                                12 } };
    for (int i = 0; i < 6; i++)
        bl[i] = s[B[i][0]] == 1
            && s[B[i][1]] == 1 ? BcD(p[B[i][0]], p[B[i][1]]) : -1.0f;
}

// per-BODY caps (a global cap was spent by the respawn / re-instance windows of the first minutes, so the later pipe-mod / execution window logged nothing):
// 120 lines a window, 1500 a body and session (no cap in the dev build)
#define BC_LOG(b, ...) do { if ((b)->lines < 120 && ((b)->tot < 1500 || g_debugMode)) { (b)->lines++; (b)->tot++; Xlog(__VA_ARGS__); } } while (0)
static void BcSnap(BcBodyT *b, uint8_t *a, uint32_t n)
{
    b->ni = n > BC_INST ? BC_INST : (int) n;
    for (int i = 0; i < b->ni; i++) {
        uint8_t *in = a + (size_t) i * BC_IN_SZ;
        b->ia[i] = in;
        b->sid[i] = *(uint64_t *) (in + 0x28);
        memcpy(b->in[i], in, BC_IN_SZ);
        int nsub = in[0x383] > BC_LOD ? BC_LOD : in[0x383];
        for (int j = 0; j < BC_LOD; j++) {
            uint8_t *so =
                j < nsub ? *(uint8_t **) (in + (size_t) j * 0x30) : NULL;
            if (so && !IsBadReadPtr(so, BC_SO_SZ))
                memcpy(b->so[i][j], so, BC_SO_SZ);
            else
                memset(b->so[i][j], 0, BC_SO_SZ);
        }
    }
}

static uint8_t *BcInsts(uint8_t *proc, uint32_t *n)
{
    uint8_t *dc = proc
        && !IsBadReadPtr(proc,
                         0x1b8) ? *(uint8_t **) (proc + 0x1b0) : NULL;
    *n = dc && !IsBadReadPtr(dc, 0x58) ? *(uint32_t *) (dc + 0x14) : 0;
    uint8_t *a = *n && *n <= 256 ? *(uint8_t **) (dc + 8) : NULL;
    return a && !IsBadReadPtr(a, (size_t) *n * BC_IN_SZ) ? a : NULL;
}

static void BcOpen(BcBodyT *b, int who, uint64_t h, uint8_t *proc,
                    const char *why)
{
    const char *wl = who == T2R_US ? "own" : "slot ";
    int wi = who == T2R_US ? 0 : who;
    b->until = g_frame + 180;
    b->open_frame = g_frame;
    b->lines = !strncmp(why, "new handle", 10) ? 106 : 0;       // (a fresh puppet's first-frame init diff is noise: window open + a few lines only)
    b->watched = 0;
    b->ni = 0;
    b->bwas = 0;
    memset(b->nin, 0, sizeof b->nin);
    memset(b->nso, 0, sizeof b->nso);
    uint32_t n = 0;
    uint8_t *a = BcInsts(proc, &n);
    char lst[900];
    int k = 0;
    lst[0] = 0;
    if (a) {
        BcSnap(b, a, n);
        for (int i = 0; i < b->ni && k < (int) sizeof lst - 48; i++) {
            uint8_t *in = b->in[i];
            int nsub = in[0x383], dr = 0;
            for (int j = 0; j < BC_LOD && j < nsub; j++)
                dr += b->so[i][j][0x1b0] != 0;
            k += snprintf(lst + k, SNLEFT(lst, k),
                          " %d:%016llx m%02x%02x lod%d d%d", i,
                          (unsigned long long) b->sid[i], in[0x384],
                          in[0x386], nsub, dr);
        }
    }
    for (int j = 0; j < BC_NJ; j++) {
        b->js0[j] = b->jsl[j] = (uint8_t) BcJoint(h, BC_J[j], b->j0[j]);
        memcpy(b->jl[j], b->j0[j], 12);
    }
    BcBones(b->j0, b->js0, b->bl0);
    int nj = 0;
    for (int j = 0; j < BC_NJ; j++)
        nj += b->js0[j] == 1;
    BC_LOG(b,
           "bodycatch: %s%d window open frame %llu (%s): proc %p, %u mesh instances,%s | joints readable %d/%d, arm bones l %.2f %.2f %.2f r %.2f %.2f %.2f",
           wl, wi, (unsigned long long) g_frame, why, (void *) proc, n,
           lst, nj, BC_NJ, b->bl0[0], b->bl0[1], b->bl0[2], b->bl0[3],
           b->bl0[4], b->bl0[5]);
}

static void BcFrame(BcBodyT *b, int who, uint64_t h, uint8_t *proc)
{
    const char *wl = who == T2R_US ? "own" : "slot ";
    int wi = who == T2R_US ? 0 : who;
    unsigned long long fr = (unsigned long long) (g_frame - b->open_frame);
    uint32_t n = 0;
    uint8_t *a = BcInsts(proc, &n);
    if (a) {
        int ni = n > BC_INST ? BC_INST : (int) n;
        if (ni != b->ni || (ni && a != b->ia[0])) {
            BC_LOG(b,
                   "bodycatch: %s%d +%llu instance table changed (%d -> %d instances, array %p -> %p) - new baseline",
                   wl, wi, fr, b->ni, ni,
                   (void *) (b->ni ? b->ia[0] : NULL), (void *) a);
            BcSnap(b, a, n);
        } else {
            for (int i = 0; i < ni; i++) {
                uint8_t *in = a + (size_t) i * BC_IN_SZ;
                char ln[640];
                int k = 0, nc = 0;
                uint64_t waddr = 0;
                for (int o = 0; o < BC_IN_SZ; o += 4) {
                    uint32_t ov = *(uint32_t *) (b->in[i] + o), nv =
                        *(uint32_t *) (in + o);
                    if (ov == nv)
                        continue;
                    *(uint32_t *) (b->in[i] + o) = nv;
                    if (b->nin[i][o / 4] < 255)
                        b->nin[i][o / 4]++;
                    if (b->nin[i][o / 4] > 3)
                        continue;
                    nc++;
                    if (k < (int) sizeof ln - 48)
                        k += snprintf(ln + k, SNLEFT(ln, k),
                                      " in+%03x %08x->%08x%s", o, ov, nv,
                                      b->nin[i][o / 4] ==
                                      3 ? "(mute)" : "");
                    if (!waddr && o >= 0x380 && b->nin[i][o / 4] == 1)
                        waddr = (uint64_t) (in + o);
                }
                int nsub = in[0x383] > BC_LOD ? BC_LOD : in[0x383];
                for (int j = 0; j < nsub; j++) {
                    uint8_t *so = *(uint8_t **) (in + (size_t) j * 0x30);
                    if (!so || IsBadReadPtr(so, BC_SO_SZ))
                        continue;
                    for (int o = 0; o < BC_SO_SZ; o += 4) {
                        uint32_t ov = *(uint32_t *) (b->so[i][j] + o), nv =
                            *(uint32_t *) (so + o);
                        if (ov == nv)
                            continue;
                        *(uint32_t *) (b->so[i][j] + o) = nv;
                        if (b->nso[i][j][o / 4] < 255)
                            b->nso[i][j][o / 4]++;
                        if (b->nso[i][j][o / 4] > 3)
                            continue;
                        nc++;
                        if (k < (int) sizeof ln - 48)
                            k += snprintf(ln + k, SNLEFT(ln, k),
                                          " lod%d+%03x %08x->%08x%s", j, o,
                                          ov, nv,
                                          b->nso[i][j][o / 4] ==
                                          3 ? "(mute)" : "");
                        if (!waddr
                            && ((o >= 0x1a0 && o < 0x1c0) || o == 0x30)
                            && b->nso[i][j][o / 4] == 1)
                            waddr = (uint64_t) (so + o);
                    }
                }
                if (nc)
                    BC_LOG(b,
                           "bodycatch: %s%d +%llu inst %d %016llx (draw %02x%02x%02x%02x lod%d):%s",
                           wl, wi, fr, i,
                           (unsigned long long) *(uint64_t *) (in + 0x28),
                           in[0x384], in[0x385], in[0x386], in[0x387],
                           in[0x383], ln);
                if (waddr && !b->watched) {
                    b->watched = 1;
                    int armed = dbg() && !g_hpwAddr;
                    HpwArmAt(waddr, 240, "bodycatch watch");  // (debug builds; one DR0 watch at a time - a busy watch is skipped)
                    BC_LOG(b,
                           "bodycatch: %s%d +%llu write watch on %p (inst %d %016llx) for 240 frames%s",
                           wl, wi, fr, (void *) waddr, i,
                           (unsigned long long) *(uint64_t *) (in + 0x28),
                           armed ? "" :
                           " - NOT armed (no t2r-debug.txt or DR0 busy)");
                }
            }
        }
    }
    float p[BC_NJ][3];
    uint8_t s[BC_NJ];
    char ln[760];
    int k = 0, hit = 0;
    for (int j = 0; j < BC_NJ; j++)
        s[j] = (uint8_t) BcJoint(h, BC_J[j], p[j]);
    for (int j = 0; j < BC_NJ; j++) {
        int chg = s[j] != b->jsl[j];
        if (!chg && j && s[j] == 1 && s[0] == 1 && b->jsl[0] == 1) {
            float r[3] =
                { p[j][0] - p[0][0], p[j][1] - p[0][1],
           p[j][2] - p[0][2] };
            float q[3] =
                { b->jl[j][0] - b->jl[0][0], b->jl[j][1] - b->jl[0][1],
           b->jl[j][2] - b->jl[0][2] };
            chg = BcD(r, q) > 0.20f;
        }
        if (!chg)
            continue;
        hit++;
        if (k < (int) sizeof ln - 64)
            k += snprintf(ln + k, SNLEFT(ln, k), " %s %s %.2f", BC_J[j],
                          s[j] == 1 ? "ok" : s[j] ==
                          2 ? "NON-FINITE" : "unreadable", s[j] == 1
                          && s[0] == 1 ? BcD(p[j], p[0]) : -1.0f);
    }
    float bl[6];
    BcBones(p, s, bl);
    int bchg = 0;
    for (int i = 0; i < 6; i++)
        if (b->bl0[i] > 0.05f
            && (bl[i] < 0
                || __builtin_fabsf(bl[i] - b->bl0[i]) > 0.25f * b->bl0[i]))
            bchg = 1;
    if (hit || bchg != b->bwas) {
        BC_LOG(b,
               "bodycatch: %s%d +%llu skeleton (m from pelvis):%s | arm bones l %.2f %.2f %.2f r %.2f %.2f %.2f (window start l %.2f %.2f %.2f r %.2f %.2f %.2f)%s",
               wl, wi, fr, k ? ln : " (no joint moved > 0.2 m)", bl[0],
               bl[1], bl[2], bl[3], bl[4], bl[5], b->bl0[0], b->bl0[1],
               b->bl0[2], b->bl0[3], b->bl0[4], b->bl0[5],
               bchg ? " ARM BONES COLLAPSED / STRETCHED" :
               " arm bones normal");
        for (int j = 0; j < BC_NJ; j++) {
            b->jsl[j] = s[j];
            memcpy(b->jl[j], p[j], 12);
        }
    }
    b->bwas = bchg;
    if (g_frame + 1 >= b->until)
        BC_LOG(b, "bodycatch: %s%d window closed (+%llu)", wl, wi, fr);
}

static void BcTick(void)
{
    typedef uint8_t *(*fn_peer)(uint32_t);
    typedef void *(*fn_proc)(uint64_t);
    typedef uint8_t *(*fn_me)(void);
    static int off = -1;
    if (off < 0) {
        FILE *f = ExeSideOpen("t2r-no-bodycatch.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        if (!off)
            Xlog("bodycatch: on (3 s of per-frame mesh-instance / LOD-object / joint diffs from every paired move, down, revive, re-instance; t2r-no-bodycatch.txt = off)");
    }
    if (off)
        return;
    if (MatchSubstate() != 6) {
        if (g_bc)
            for (int i = 0; i <= T2R_US; i++)
                g_bc[i].valid = 0;
        return;
    }
    if (!g_bc) {
        g_bc = (BcBodyT *) calloc(T2R_US + 1, sizeof *g_bc);
        if (!g_bc) {
            off = 1;
            Xlog("bodycatch: no memory - off");
            return;
        }
    }
    uint32_t self = *(uint32_t *) T2R_SYM(kMySlot);
    int ps = PsyncActive() != 0, xc = g_xcamOn != 0;
    for (int who = 0; who <= T2R_US; who++) {
        uint64_t h = 0;
        uint8_t *proc = NULL;
        if (who == T2R_US) {
            h = SID_PLAYER;
            proc = ((fn_me) T2R_SYM(kFnLocalPlayer)) ();
        } else if (who < T2R_MAX_PEERS && (uint32_t) who != self) {
            uint8_t *peer = ((fn_peer) T2rPeer) ((uint32_t) who);
            if (!peer || !peer[4]) {
                g_bc[who].valid = 0;
                continue;
            }
            h = *(uint64_t *) (peer + 0x60);
            proc =
                h ? (uint8_t *) ((fn_proc)
                                 T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
        } else
            continue;
        BcBodyT *b = &g_bc[who];
        int dn =
            who == T2R_US ? g_downed != 0 : g_peerDowned[who] != 0, rv =
            VdReviving(who) != 0;
        const char *why = NULL;
        if (!b->valid) {
            b->valid = 1;
            b->h = h;
            b->proc = proc;
            b->prev_ps = ps;
            b->prev_xc = xc;
            b->prev_dn = dn;
            b->prev_rv = rv;
            b->until = 0;
            continue;
        }
        if (h != b->h)
            why = "new handle (respawn / new puppet)";
        else if (ps && !b->prev_ps)
            why =
                proc !=
                b->
                proc ? "paired move started (process re-instanced)" :
                "paired move started";
        else if (xc && !b->prev_xc)
            why = "execution camera on";
        else if (dn && !b->prev_dn)
            why = "downed";
        else if (rv && !b->prev_rv)
            why = "revive started";
        else if (!dn && b->prev_dn)
            why = "up again / dead";
        else if (proc != b->proc)
            why = "process re-instanced";
        b->h = h;
        b->proc = proc;
        b->prev_ps = ps;
        b->prev_xc = xc;
        b->prev_dn = dn;
        b->prev_rv = rv;
        if (why && h && proc)
            BcOpen(b, who, h, proc, why);
        else if (b->until && g_frame < b->until && h && proc)
            BcFrame(b, who, h, proc);
    }
}

// Input-lock probe (after a special execution with the enforcer a player could not move or input commands other than the pause menu; the
// log after that execution is clean - no paired move, no execution state, no downed state - so the lock is engine-side). While a movement key /
// the left stick has been held 1.5 s with the body not moving (alive, up, no paired move, no menu), one line per 10 s (12 max) dumps every state of
// ours that can hold the controls plus the engine's answer for a few commands, so the next report shows which lock is up. Log only; t2r-no-inputprobe.txt = off.
static void InputprobeTick(void)
{
    static int off = -1, nl;
    static uint64_t chk, since, last;
    if (off < 0 || g_frame - chk > 300) {
        FILE *f = ExeSideOpen("t2r-no-inputprobe.txt", "r");
        off = f != NULL;
        if (f)
            fclose(f);
        chk = g_frame;
    }
    if (off || nl >= 12 || MatchSubstate() != 6 || g_downed
        || LocalHp() <= 0.0f || PsyncActive()
        || PlayerMenuActive() == 1 || g_tabOpen || !GameFocused()) {
        since = 0;
        return;
    }
    int moving =
        ((GetAsyncKeyState('W') | GetAsyncKeyState('A') |
          GetAsyncKeyState('S') | GetAsyncKeyState('D')) & 0x8000)
        || PadStickMoving();
    if (!moving || BpOwnSpeed() > 0.15f) {
        since = 0;
        return;
    }
    if (!since) {
        since = g_t60 ? g_t60 : 1;
        return;
    }
    if (g_t60 - since < S60(1.5f) || (last && g_t60 - last < S60(10.0f)))
        return;
    last = g_t60;
    nl++;
    uint64_t me = SID_PLAYER, none = 0, c_mel = FNV64("melee"), c_int =
        FNV64("interact"), c_aim = FNV64("aim");
    uint64_t rm = DcCall(FNV64("joypad-command-active?"), 1, &c_mel), ri =
        DcCall(FNV64("joypad-command-active?"), 1, &c_int), ra =
        DcCall(FNV64("joypad-command-active?"), 1, &c_aim);
    uint64_t pr = DcCall(FNV64("player-is-prone?"), 0, &none), im =
        DcCall(FNV64("character-in-melee-attack?"), 1, &me);
    Xlog("input probe: move held %.1f s, body speed %.2f m/s - exec until %lld, gx until %lld, stun until %lld, stand until %lld, no-auto-reload until %lld, path-gate hold %lld (frame %llu), downed-melee lock %d, psync role %d, " "engine: melee %llx interact %llx aim %llx prone %llx in-melee-attack %llx", (double) (g_t60 - since) / 60.0, (double) BpOwnSpeed(), (long long) g_execUntil - (long long) g_t60, (long long) g_gx.until - (long long) g_t60, (long long) g_stun_until - (long long) g_t60, (long long) g_standUntil - (long long) g_frame, (long long) g_narUntil - (long long) g_t60, (long long) g_pgHoldF - (long long) g_frame, (unsigned long long) g_frame, DmLocked(), PsyncRole(), (unsigned long long) rm, (unsigned long long) ri, (unsigned long long) ra, (unsigned long long) pr, (unsigned long long) im);
}
