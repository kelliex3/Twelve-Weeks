// mv3_stubs.c - empty stand-ins for the melee v3 modules that are not in this tree yet (so the tree compiles without them).
// t2r-ext.c defines MV3_STUB_<module> for every module whose file is missing (or all of them with -DMV3_STUBS). A stub never starts a bubble, so v3 stays
// inert: every mv3_stand() gate answers 0 and melee runs as v2.
#ifdef MV3_STUB_MOVES
const Mv3Move MV3_MOVES[1] = { { 0 } };

const int MV3_MOVES_N = 0;
#endif
#ifdef MV3_STUB_SIM
void Mv3SimInit(Mv3World *w, uint32_t seed)
{
    if (w) {
        memset(w, 0, sizeof *w);
        w->rng = seed ? seed : 1;
        for (int i = 0; i < MV3_MAX_F; i++)
            w->f[i].slot = 0xff;
    }
}

int Mv3SimAdd(Mv3World *w, uint8_t slot, uint8_t team, const Mv3In *in)
{
    (void) in;
    if (!w || w->n >= MV3_MAX_F)
        return -1;
    w->f[w->n].slot = slot;
    w->f[w->n].team = team;
    w->f[w->n].move = -1;
    return w->n++;
}

void Mv3SimRemove(Mv3World *w, int idx)
{
    if (w && idx >= 0 && idx < MV3_MAX_F) {
        memset(&w->f[idx], 0, sizeof w->f[idx]);
        w->f[idx].slot = 0xff;
    }
}

void Mv3SimStep(Mv3World *w, const Mv3In in[MV3_MAX_F], Mv3Evbuf *ev)
{
    (void) in;
    if (ev)
        ev->n = 0;
    if (w)
        w->frame++;
}

uint32_t Mv3SimHash(const Mv3World *w)
{
    return w ? w->frame : 0;
}
#endif
#ifdef MV3_STUB_NET
static Mv3World g_mv3StubWorld;
static void Mv3NetTick(uint64_t now)
{
    (void) now;
}

static void Mv3Recv(uint32_t from, const uint8_t *buf, uint64_t len)
{
    (void) from;
    (void) buf;
    (void) len;
}

static int Mv3NetPropose(uint32_t slot)
{
    (void) slot;
    return 0;
}

static void Mv3NetEnd(const char *why)
{
    (void) why;
}

static int Mv3NetActive(void)
{
    return 0;
}

static const Mv3World *Mv3NetWorld(void)
{
    return &g_mv3StubWorld;
}

static int Mv3NetLocalIdx(void)
{
    return -1;
}

static uint32_t Mv3NetConfirmed(void)
{
    return 0;
}

static int Mv3NetIdxOfSlot(uint32_t slot)
{
    (void) slot;
    return -1;
}

static int Mv3NetSlotOfIdx(int idx)
{
    (void) idx;
    return -1;
}

static uint32_t Mv3NetFrameMs(uint32_t frame)
{
    (void) frame;
    return (uint32_t) GetTickCount64();
}

static void Mv3NetSetHp(int idx, int16_t hp10)
{
    (void) idx;
    (void) hp10;
}
#endif
#ifdef MV3_STUB_ANIM
#define MV3A_US 0xffffffffu
static int Mv3AnimBodyBusy(uint32_t slot_or_us)
{
    (void) slot_or_us;
    return 0;
}

static void Mv3AnimReleaseAll(const char *why)
{
    (void) why;
}

static void Mv3AnimTestTick(void)
{
}

static void Mv3AnimApply(const Mv3World *w, int local_idx,
                           uint32_t world_frame_frac_ms)
{
    (void) w;
    (void) local_idx;
    (void) world_frame_frac_ms;
}

static void Mv3AnimRelease(uint32_t slot)
{
    (void) slot;
}

static void Mv3AnimSuppressEngine(int on)
{
    (void) on;
}
#endif
#ifdef MV3_STUB_FX
static void Mv3FxEvents(const Mv3Evbuf *ev, int resim)
{
    (void) ev;
    (void) resim;
}

static void Mv3FxRolledBack(uint32_t to_frame)
{
    (void) to_frame;
}

static void Mv3FxTick(uint64_t now)
{
    (void) now;
}
#endif
