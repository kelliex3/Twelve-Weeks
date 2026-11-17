// padinject.c - engine-level VIRTUAL PAD. Included before melee_pace.c in t2r-ext.c. Log prefix "padinj:".
//
// WHY: the old "press replay" wrapped the DC native joypad-command-active? (record +8 / +0x10). Scripts read it, the engine's C++ melee start does not
// (it reads the joypad's own pressed / held masks), so a replayed press never started a swing ("bot: melee press 1: NO swing within 900 ms (engine reads 27)").
// Static RE of tlou-ii.exe 1.6.10721 (exe RVAs; Ghidra-free, capstone + pdata, notes in re/pad-inject-1026.md):
//   * XInput is NOT the gameplay input. The exe imports no xinput; xinput1_4 is loaded only by the assert-dialog pad (0x1419dfc50). Gameplay input is the
//     engine's own Pad -> Joypad pipeline: a device layer (InputManager::Update 0x39b6d0, its per-device readers: NxApp / HID / the "Joypad Monkey" debug
//     pad) fills a `Pad` struct and ends in Joypad::Update(Joypad *jp, Pad *pad, int) at 0x1593350 (5 direct callers: 0x39c30b 0x39c496 0x39c65c 0x39c6b9
//     0x139bd4b), which copies the Pad into the Joypad object:
//         Pad+0x000..0x01c  8 floats = stick pairs (x,y) x2, each stored twice (+0x00/+0x10 ...): [0]=[4], [1]=[5], [2]=[6], [3]=[7]  (left x,y, right x,y; -1..1)
//         Pad+0x528         u64 button mask  -> Joypad+0x138 (held; the previous held goes to +0x140);   Joypad+0x128 = pressed edge, +0x130 = released edge
//     Joypad+0x168 + 8*bit is the per-bit press timestamp (buffer windows). All of it is derived INSIDE Joypad::Update from Pad+0x528, so setting a bit in the
//     Pad before the call gives the engine a real press edge (false -> true) with timestamps, pressed/held/released masks and the stick-as-button bits.
//   * Commands are read by the reader 0x158eb70 (Joypad *rcx, cmd *rdx, StringId64 command r8, u32 count r9, entry* [rsp+0x20], int [rsp+0x28]) - called
//     from 0x158f370 (the C++ and the native entry). An entry is 16 bytes: u32 kind @0, float threshold @4, u64 input mask @8. kind 8 / 0x20 = pressed edge
//     (Joypad+0x128 & mask), kind 2 / 4 = held (+0x138), kind 0x10 = released, kind 1 = not held; the first satisfied entry wins (OR). The disabled-command
//     mask is applied on top, so an injected press is blocked exactly like a real one (joypad-disable-commands, player-disable-melee ...).
// HOW: Joypad::Update and the reader are never patched themselves (no prologue relocation): the 5 + 2 direct call sites get their rel32 re-pointed at 14 byte
// thunks (jmp [rip+0]) in a page allocated right behind the image. The Update hook ORs the injected bit(s) / sets the injected sticks into the Pad and calls the
// real function; the reader hook only learns which input bit a command (melee-attack, evade, ...) uses in the current scheme (so remapped schemes work) and
// which Joypad object the game reads (the primary one). With nothing injected the Update hook is one volatile load and a tail call.
// The window must be focused for Joypad::Update to run (0x1415933ae: focus flag 0x1442620ac, else debug flag "Enable Joypad w/o Focus" 0x148e03f65): the dev
// bot sets that flag (padinj_nofocus) so an unfocused A/B window still takes injected input.
// Opt out (old native wrapper only): t2r-no-pad-inject.txt next to the exe.
#define PADINJ_UPD_RVA 0x1593350u
#define PADINJ_RD_RVA 0x158eb70u
#define PADINJ_NOFOCUS_RVA 0x8e03f65u
#define PADINJ_NW 48            // defaults (20) + endpeek probes (5) + mv3 names overflowed 24; a full table silently never learns the rest
#define PADINJ_NS 8
typedef uint64_t(*padinj_upd_t) (void *, uint8_t *, uint64_t, uint64_t);
typedef unsigned char (*padinj_rd_t)(void *, void *, uint64_t, uint32_t,
                                     const uint8_t *, uint32_t);
static struct padinj_w {        // watched commands
    uint64_t sid;
    char name[24];
    volatile uint64_t mask;
    uint32_t kind;
    void *volatile jp;
    volatile unsigned reads_true, reads_all;
    uint64_t learned_ms, seen_ms;
    int logged;
    volatile int learn_log;
    uint32_t n_ent;
    uint32_t ent[8][4];         // last entry table, for the log
} g_padw[PADINJ_NW];
static struct padinj_slot {     // pending injections
    volatile uint64_t until, hard_until;
    uint64_t mask;
    volatile int phase;
    volatile unsigned applied;
    int w;
    int blockable;              // blockable: a dev bot press, hidden by an armed block like a real one; phase 0 new, 1 release frame done, 2 pressed
} g_pads[PADINJ_NS];
static struct {
    int tried, ok, off, sites_upd, sites_rd;
    padinj_upd_t upd;
    padinj_rd_t rd;
    uint8_t *exe;
    void *volatile jp;
    volatile uint64_t jp_ms;    // the primary joypad (the one the command reads came from)
    volatile uint64_t act_until;        // any injection pending until this tick (0 = nothing: the hooks are pass-through)
    volatile float st[4];
    volatile uint64_t st_until;
    volatile int st_swap;       // lx ly rx ry
    volatile unsigned upd_primary, upd_other, st_applied, btn_applied;
    uint64_t rep_ms;
    unsigned rep_prim;
    uint64_t tick_frame;
    uint8_t nofocus_prev;
    int nofocus_set;
    volatile uint64_t blk_mask, blk_until;
    volatile int raw_blk;       // a command's input bit cleared before Joypad::Update (melee pace lock), raw_blk = it was held
    volatile unsigned blk_applied;
    volatile float raw_lx, raw_ly;
    volatile uint64_t raw_f;    // the primary pad's left stick as the engine reads it (dodge direction on a non-XInput pad)
    struct padinj_w *mel_w;     // the melee-attack watch (padinj_watch_defaults)
    volatile int raw_mel;
    volatile unsigned raw_mel_seq;
    volatile uint64_t raw_mel_f, raw_mel_ms;
    struct padinj_w *dg_w[3];   // ps53: the dodge commands (evade, dodge, evade-start)
    volatile uint64_t dg_mask;
    volatile int raw_dg;
    volatile uint64_t raw_dg_f; // ps53: every input bit of their entries (keyboard + any pad), the device state   // v5-pinpress: melee-attack's input bit as the device gave it (before block / injection), its rising edges
} g_padi;
// ---- the hooks
static void PadinjApply(void *jp, uint8_t *pad)
{
    uint64_t now = GetTickCount64();
    if (jp != g_padi.jp) {
        g_padi.upd_other++;
        return;
    }
    g_padi.upd_primary++;
    if (IsBadWritePtr(pad, 0x540))
        return;
    uint64_t *btn = (uint64_t *) (pad + 0x528);
    if (g_padi.blk_mask && now < g_padi.blk_until) {    // blocked command: remember the raw state for our own press buffer, then hide it from the engine
        g_padi.raw_blk = (*btn & g_padi.blk_mask) != 0;
        if (g_padi.raw_blk) {
            *btn &= ~g_padi.blk_mask;
            g_padi.blk_applied++;
        }
    } else
        g_padi.raw_blk = 0;
    for (int i = 0; i < PADINJ_NS; i++) {
        struct padinj_slot *s = &g_pads[i];
        if (!s->mask)
            continue;
        if (now >= s->until && s->applied >= 2) {
            s->mask = 0;
            continue;
        }                       // a press is delivered for at least two Joypad::Update calls (a low frame rate cannot swallow it)
        if (now >= s->hard_until) {
            s->mask = 0;
            continue;
        }
        if (s->phase == 0) {    // the first frame: a bit that is already held is released first, so the engine still sees an edge
            s->phase = (*btn & s->mask) ? 1 : 2;
            if (s->phase == 1) {
                *btn &= ~s->mask;
                s->applied++;
                continue;
            }
        } else if (s->phase == 1)
            s->phase = 2;
        *btn |= s->mask;
        s->applied++;
        g_padi.btn_applied++;
        if (s->blockable && (s->mask & g_padi.blk_mask)
            && now < g_padi.blk_until) {
            g_padi.raw_blk = 1;
            *btn &= ~(s->mask & g_padi.blk_mask);
            g_padi.blk_applied++;
        }                       // (the release paths' own replays stay unblocked)
    }
    if (now < g_padi.st_until) {
        float *f = (float *) pad;
        float lx = g_padi.st[0], ly = g_padi.st[1], rx = g_padi.st[2], ry =
            g_padi.st[3];
        if (g_padi.st_swap) {
            float a = lx, b = ly;
            lx = rx;
            ly = ry;
            rx = a;
            ry = b;
        }
        f[0] = f[4] = lx;
        f[1] = f[5] = ly;
        f[2] = f[6] = rx;
        f[3] = f[7] = ry;
        g_padi.st_applied++;
    }
}

static volatile uint64_t g_padActMs;  // afk.c (anti-AFK): last real input on the primary joypad (a button edge or a stick out of rest), before injection
static uint64_t PadinjUpdHook(void *jp, uint8_t *pad, uint64_t r8,
                                uint64_t r9)
{
    if (jp == g_padi.jp && jp) {
        const float *f = (const float *) pad;
        g_padi.raw_lx = f[0];
        g_padi.raw_ly = f[1];
        g_padi.raw_f = g_frame; // the real left stick of ANY pad (DualSense / Steam Input / XInput), before injection
        {
            static uint64_t lbm;
            uint64_t bm = *(const uint64_t *) (pad + 0x528);
            if (bm != lbm || fabsf(f[0]) > 0.3f || fabsf(f[1]) > 0.3f
                || fabsf(f[2]) > 0.3f || fabsf(f[3]) > 0.3f)
                g_padActMs = GetTickCount64();
            lbm = bm;
        }
        // v5-pinpress (controller: could not follow through after the first hit in a pin): the duel / pin read a pad melee press only through raw_blk,
        // which exists only while a block is armed and is zeroed by every padinj_block(.., 0) - melee_pace.c cleared it every frame of a paired move (mpace_tick
        // runs before mdu_tick / mpn_tick), so a pad press during a pin was never seen (keyboard F was, through GetAsyncKeyState). The device's own bit is
        // kept here on every Update of the read joypad, block or not, with a rising-edge counter that no clear can wipe.
        struct padinj_w *mw = g_padi.mel_w;
        uint64_t mm = mw ? mw->mask : 0;
        if (mm) {
            int on = (*(const uint64_t *) (pad + 0x528) & mm) != 0;
            if (on && !g_padi.raw_mel) {
                g_padi.raw_mel_seq++;
                g_padi.raw_mel_ms = GetTickCount64();
            }
            g_padi.raw_mel = on;
            g_padi.raw_mel_f = g_frame;
        }
        // ps53 (the dodge button was not controller supported): the dodge press was read only as joypad-command-active?(evade / dodge / evade-start),
        // and padinj_learn keeps ONE input bit per command - every bit of every entry of the three commands is OR-ed here (the keyboard key and the pad
        // button are separate entries), and the device state of those bits is read before the disabled-command mask.
        {
            uint64_t dm = 0;
            for (int k = 0; k < 3; k++) {
                struct padinj_w *w = g_padi.dg_w[k];
                if (!w || !w->mask)
                    continue;
                for (uint32_t i = 0; i < w->n_ent && i < 8; i++) {
                    uint32_t kd = w->ent[i][0];
                    uint64_t m =
                        (uint64_t) w->ent[i][2] | ((uint64_t) w->
                                                   ent[i][3] << 32);
                    if ((kd == 8 || kd == 0x20 || kd == 2 || kd == 4)
                        && !(m & 0x8000000000000000ULL))
                        dm |= m;
                }
            }
            if (dm != g_padi.dg_mask) {
                g_padi.dg_mask = dm;
                static int nl;
                if (nl++ < 12)
                    Xlog("padinj: ps53 dodge input bits %016llx (evade %016llx dodge %016llx evade-start %016llx, all entries)", (unsigned long long) dm, (unsigned long long) (g_padi.dg_w[0] ? g_padi.dg_w[0]->mask : 0), (unsigned long long) (g_padi.dg_w[1] ? g_padi.dg_w[1]->mask : 0), (unsigned long long) (g_padi.dg_w[2] ? g_padi.dg_w[2]->mask : 0));
            }
            if (dm) {
                g_padi.raw_dg =
                    (*(const uint64_t *) (pad + 0x528) & dm) != 0;
                g_padi.raw_dg_f = g_frame;
            }
    }}
    if (g_padi.act_until) {
        if (GetTickCount64() < g_padi.act_until)
            PadinjApply(jp, pad);
        else
            g_padi.act_until = 0;
    }
    return g_padi.upd(jp, pad, r8, r9);
}

static void PadinjLearn(struct padinj_w *w, void *jp, uint32_t cnt,
                         const uint8_t *ent)
{
    if (cnt < 1 || cnt > 8 || !ent || IsBadReadPtr(ent, (size_t) cnt * 16))
        return;
    uint64_t best = 0;
    uint32_t bk = 0;
    for (int pass = 0; pass < 3 && !best; pass++)
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t k = *(const uint32_t *) (ent + i * 16);
            uint64_t m = *(const uint64_t *) (ent + i * 16 + 8);
            int want = pass == 0 ? k == 8 : pass == 1 ? k == 0x20 : (k == 2
                                                                     || k
                                                                     == 4);
            if (!want || !m || (m & 0x8000000000000000ULL))
                continue;
            best = m & (~m + 1);
            bk = k;
            break;              // the lowest input bit of that entry
        }
    int changed = best != w->mask || !w->jp;    // (A/B: two joypads alternate on the reader every frame - re-learning on each flip logged 3280 lines; keep the first joypad, follow only a changed input bit)
    if (changed) {
        w->n_ent = cnt;
        for (uint32_t i = 0; i < cnt; i++) {
            w->ent[i][0] = *(const uint32_t *) (ent + i * 16);
            w->ent[i][1] = *(const uint32_t *) (ent + i * 16 + 4);
            w->ent[i][2] = *(const uint32_t *) (ent + i * 16 + 8);
            w->ent[i][3] = *(const uint32_t *) (ent + i * 16 + 12);
        }
        w->mask = best;
        w->kind = bk;
        w->jp = jp;
        w->learned_ms = GetTickCount64();
        w->learn_log = 1;
        if (best) {
            g_padi.jp = jp;
            g_padi.jp_ms = w->learned_ms;
        }
    }
}

static unsigned char PadinjRdHook(void *jp, void *cmd, uint64_t sid,
                                    uint32_t cnt, const uint8_t *ent,
                                    uint32_t fl)
{
    struct padinj_w *w = NULL;
    for (int i = 0; i < PADINJ_NW; i++)
        if (g_padw[i].sid == sid) {
            w = &g_padw[i];
            break;
        }
    unsigned char r = g_padi.rd(jp, cmd, sid, cnt, ent, fl);
    if (w) {
        w->reads_all++;
        if (r)
            w->reads_true++;
        uint64_t now = GetTickCount64();
        if (now - w->seen_ms > 1000 || jp != w->jp) {
            w->seen_ms = now;
            PadinjLearn(w, jp, cnt, ent);
        }                       // refreshed once a second (scheme changes)
    }
    return r;
}

// ---- install: re-point the direct call sites at thunks
static uint8_t *PadinjNearAlloc(uint8_t *base, size_t img)
{
    uintptr_t want =
        ((uintptr_t) base + img + 0xffff) & ~(uintptr_t) 0xffff;
    for (int i = 0; i < 4096; i++) {
        uint8_t *p =
            VirtualAlloc((void *) (want + (uintptr_t) i * 0x10000), 0x1000,
                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p)
            return p;
    }
    return NULL;
}

static int PadinjPatchSites(const uint32_t *rvas, int n,
                              uint32_t target_rva, uint8_t *thunk)
{
    int done = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *s = g_padi.exe + rvas[i];
        if ((s[0] != 0xE8 && s[0] != 0xE9) || IsBadReadPtr(s, 5)) {
            Xlog("padinj: site rva 0x%x is not a call/jmp (%02x) - skipped", rvas[i], s[0]);
            continue;
        }
        int32_t rel = *(int32_t *) (s + 1);
        if (s + 5 + rel != g_padi.exe + target_rva) {
            Xlog("padinj: site rva 0x%x does not target 0x%x - skipped",
                 rvas[i], target_rva);
            continue;
        }
        int64_t nr = (int64_t) (thunk - (s + 5));
        if (nr > 0x7fffffffLL || nr < -0x7fffffffLL) {
            Xlog("padinj: site rva 0x%x out of rel32 reach of the thunk - skipped", rvas[i]);
            continue;
        }
        DWORD old;
        if (!VirtualProtect(s, 5, PAGE_EXECUTE_READWRITE, &old))
            continue;
        *(volatile int32_t *) (s + 1) = (int32_t) nr;
        VirtualProtect(s, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), s, 5);
        done++;
    }
    return done;
}

static void PadinjWatchDefaults(void);
static int PadinjInstall(void)
{
    if (g_padi.tried)
        return g_padi.ok;
    g_padi.tried = 1;
    FILE *f = ExeSideOpen("t2r-no-pad-inject.txt", "r");
    if (f) {
        fclose(f);
        g_padi.off = 1;
        Xlog("padinj: OFF (t2r-no-pad-inject.txt) - presses use the native wrapper only");
        return 0;
    }
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    g_padi.exe = exe;
    static const uint32_t usites[] =
        { 0x39c30b, 0x39c496, 0x39c65c, 0x39c6b9, 0x139bd4b }, rsites[] =
        { 0x158eb5a, 0x158f607 };
    static const uint8_t upro[11] =
        { 0x40, 0x53, 0x41, 0x55, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00,
0x00 };
    static const uint8_t rpro[12] =
        { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
0x55 };
    if (memcmp(exe + PADINJ_UPD_RVA, upro, sizeof upro)
        || memcmp(exe + PADINJ_RD_RVA, rpro, sizeof rpro)) {
        Xlog("padinj: Joypad::Update / command reader prologue differs (exe is not 1.6.10721?) - not installed, presses use the native wrapper only");
        return 0;
    }
    IMAGE_NT_HEADERS *nt =
        (IMAGE_NT_HEADERS *) (exe + ((IMAGE_DOS_HEADER *) exe)->e_lfanew);
    uint8_t *th = PadinjNearAlloc(exe, nt->OptionalHeader.SizeOfImage);
    if (!th) {
        Xlog("padinj: no page within rel32 reach of the image - not installed");
        return 0;
    }
    g_padi.upd = (padinj_upd_t) (exe + PADINJ_UPD_RVA);
    g_padi.rd = (padinj_rd_t) (exe + PADINJ_RD_RVA);
    PadinjWatchDefaults();
    uint8_t *t1 = th, *t2 = th + 32;
    t1[0] = 0xFF;
    t1[1] = 0x25;
    *(uint32_t *) (t1 + 2) = 0;
    *(uint64_t *) (t1 + 6) = (uint64_t) (uintptr_t) PadinjUpdHook;
    t2[0] = 0xFF;
    t2[1] = 0x25;
    *(uint32_t *) (t2 + 2) = 0;
    *(uint64_t *) (t2 + 6) = (uint64_t) (uintptr_t) PadinjRdHook;
    FlushInstructionCache(GetCurrentProcess(), th, 64);
    g_padi.sites_rd = PadinjPatchSites(rsites, 2, PADINJ_RD_RVA, t2); // the reader first: learning must be running before an injection is asked for
    g_padi.sites_upd = PadinjPatchSites(usites, 5, PADINJ_UPD_RVA, t1);
    g_padi.ok = g_padi.sites_upd == 5 && g_padi.sites_rd >= 1;
    Xlog("padinj: %s - Joypad::Update hook on %d/5 call sites, command reader hook on %d/2 (thunks at %p)", g_padi.ok ? "installed" : "PARTLY installed, injection disabled", g_padi.sites_upd, g_padi.sites_rd, (void *) th);
    return g_padi.ok;
}

// ---- API
static struct padinj_w *PadinjWatch(const char *name)
{
    uint64_t sid = Fnv64Rt(name);
    for (int i = 0; i < PADINJ_NW; i++)
        if (g_padw[i].sid == sid)
            return &g_padw[i];
    for (int i = 0; i < PADINJ_NW; i++)
        if (!g_padw[i].sid) {
            snprintf(g_padw[i].name, sizeof g_padw[i].name, "%s", name);
            g_padw[i].sid = sid;
            return &g_padw[i];
        }
    return NULL;
}

// ('hold sprint' / 'hold evade' failed "command not learned" - a command is learned only when the engine reads it after it is
//  watched, and the first press made the watch. The bot's commands are watched from the install on.)
static void PadinjWatchDefaults(void)
{
    g_padi.mel_w = PadinjWatch("melee-attack");
    g_padi.dg_w[0] = PadinjWatch("evade");
    g_padi.dg_w[1] = PadinjWatch("dodge");
    g_padi.dg_w[2] = PadinjWatch("evade-start");
    static const char *const W[] =
        { "evade", "sprint", "aim", "fire", "crouch-press", "prone-toggle",
"reload", "interact", "jump", "melee-attack-shiv",
        "weapon-cross-up", "weapon-cross-down", "weapon-cross-left",
            "weapon-cross-right",
        "fire-press", "fire-hold", "aim-hold"
    };                          // bow test: the bow draw never read 'fire' (bot fell back to the script wrapper)
    for (size_t i = 0; i < sizeof W / sizeof *W; i++)
        PadinjWatch(W[i]);
}

// v5-pinpress: the device's melee-attack button on the read joypad, independent of any block (keyboard + every pad the engine reads). held = seen in the last 30 frames' Update;
// presses = the rising-edge counter (a caller keeps its last value: a changed counter = a new press, even one pressed and released between two of its ticks)
static int PadinjMelHeld(void)
{
    return g_padi.ok && g_padi.raw_mel && g_frame - g_padi.raw_mel_f <= 30;
}

static unsigned PadinjMelPresses(void)
{
    return g_padi.raw_mel_seq;
}

// ps53: a dodge input (keyboard or any pad) is held on the read joypad, whatever the disabled-command mask says. t2r-dodge-pad-ps53-old.txt = commands only.
static int PadinjDodgeHeld(void)
{
    static int off = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (off < 0 || now - at > 2000) {
        at = now;
        FILE *f = ExeSideOpen("t2r-dodge-pad-ps53-old.txt", "r");
        int v = f != NULL;
        if (f)
            fclose(f);
        if (v != off)
            Xlog("dodge: ps53 raw dodge input %s",
                 v ?
                 "OFF (t2r-dodge-pad-ps53-old.txt): engine commands only" :
                 "ON: every evade / dodge input bit, keyboard and pad");
        off = v;
    }
    return !off && g_padi.ok && g_padi.raw_dg
        && g_frame - g_padi.raw_dg_f <= 30;
}

static int PadinjReady(void)
{
    return g_padi.ok;
}

// forced read of a command through the engine (the reader hook then learns its input bit); a command the game is not reading right now stays unlearned
static void PadinjProbeRead(const char *name)
{
    uint64_t c = Fnv64Rt(name);
    DcCall(FNV64("joypad-command-active?"), 1, &c);
}

// press a command for ms (the engine sees a real press edge). 1 = queued on the pad path (bit known, hooks in); 0 = caller falls back to the native wrapper
static int g_padinjBotPress;  // set by bot.c around its padinj_press: that press obeys padinj_block
static int PadinjPress(const char *name, int ms)
{
    if (!g_padi.ok)
        return 0;
    struct padinj_w *w = PadinjWatch(name);
    if (!w)
        return 0;
    if (!w->mask || !g_padi.jp) {
        PadinjProbeRead(name);
        if (!w->mask)
            return 0;
    }
    uint64_t now = GetTickCount64();
    int fi = -1;
    for (int i = 0; i < PADINJ_NS; i++)
        if (!g_pads[i].mask) {
            fi = i;
            break;
        }
    if (fi < 0)
        return 0;
    struct padinj_slot *s = &g_pads[fi];
    s->mask = 0;
    s->phase = 0;
    s->applied = 0;
    s->w = (int) (w - g_padw);
    s->blockable = g_padinjBotPress;
    s->until = now + (uint64_t) (ms < 20 ? 20 : ms);
    s->hard_until = s->until + 600;
    w->reads_true = 0;
    w->reads_all = 0;
    s->mask = w->mask;          // armed last
    if (s->hard_until > g_padi.act_until)
        g_padi.act_until = s->hard_until;
    return 1;
}

// sticks (-1..1) for ms, refreshed by the caller every frame while it steers; swap = the engine's pad order is right-then-left
static void PadinjStick(float lx, float ly, float rx, float ry, int ms,
                         int swap)
{
    if (!g_padi.ok)
        return;
    g_padi.st[0] = lx;
    g_padi.st[1] = ly;
    g_padi.st[2] = rx;
    g_padi.st[3] = ry;
    g_padi.st_swap = swap;
    uint64_t u = GetTickCount64() + (uint64_t) (ms < 20 ? 20 : ms);
    g_padi.st_until = u;
    if (u > g_padi.act_until)
        g_padi.act_until = u;
}

static void PadinjStickOff(void)
{
    g_padi.st_until = 0;
}

// the engine's own left stick (-1..1, y > 0 = forward) of the primary pad, read in the last 30 frames; 0 = none (no pad / not learned yet)
static int PadinjLeftStick(float *x, float *y)
{
    if (!g_padi.ok || !g_padi.raw_f || g_frame - g_padi.raw_f > 30)
        return 0;
    *x = g_padi.raw_lx;
    *y = g_padi.raw_ly;
    return 1;
}

// let Joypad::Update run in an unfocused window (dev A/B): the debug flag "Enable Joypad w/o Focus"
static void PadinjNofocus(int on)
{
    if (!g_padi.exe)
        return;
    uint8_t *p = g_padi.exe + PADINJ_NOFOCUS_RVA;
    if (IsBadWritePtr(p, 1))
        return;
    if (on && !g_padi.nofocus_set) {
        g_padi.nofocus_prev = *p;
        g_padi.nofocus_set = 1;
        *p = 1;
        Xlog("padinj: unfocused-window input ON (debug flag 0x%x, was %d)",
             PADINJ_NOFOCUS_RVA, g_padi.nofocus_prev);
    } else if (!on && g_padi.nofocus_set) {
        *p = g_padi.nofocus_prev;
        g_padi.nofocus_set = 0;
        Xlog("padinj: unfocused-window input restored");
    }
}

static unsigned PadinjApplied(void)
{
    return g_padi.btn_applied;
}

static int PadinjNofocusOn(void)
{
    return g_padi.nofocus_set;
}                               // ps47: the unfocused-input test mode (bots) is on

// hide a command's input bit from the engine until `until` (0 = stop). The engine's own C++ (combo follow-ups) reads the pad masks, not the
// scripted joypad command, so player-disable-melee alone let a buffered combo press chain the next punch inside the pace lock (seen live:
// swings 600 ms apart under an 850 ms lock, "he goes through everything"). 1 = armed (bit known); padinj_blocked_raw() = the real button.
static int PadinjBlock(const char *name, uint64_t until)
{
    if (!g_padi.ok)
        return 0;
    if (!until) {
        g_padi.blk_until = 0;
        g_padi.raw_blk = 0;
        return 1;
    }
    struct padinj_w *w = PadinjWatch(name);
    if (!w || !w->mask || !g_padi.jp)
        return 0;
    g_padi.blk_mask = w->mask;
    g_padi.blk_until = until;
    if (until > g_padi.act_until)
        g_padi.act_until = until;
    return 1;
}

// v5-pinpress: for melee-attack the device bit itself (raw_mel, kept on every Update): raw_blk is zeroed by every clear and was wiped each frame by melee_pace.c's
// paired-move clear before the duel / pin read it (pad presses lost; a re-arm after a long unblocked stretch could also read a stale 1)
static int PadinjBlockedRaw(void)
{
    return g_padi.blk_until && GetTickCount64() < g_padi.blk_until
        && (g_padi.mel_w
            && g_padi.blk_mask ==
            g_padi.mel_w->mask ? PadinjMelHeld() : g_padi.raw_blk);
}

// v5-pinpress: one melee press-edge reader for the duel / pin (keyboard F AND every pad: the engine's own melee-attack bit, read by the Update hook whether a
// block is armed or not). The caller calls it EVERY frame (the baseline follows the buttons, so a press made before its window never counts later) and acts on
// the result only when it wants a press. 1 = a new press (*src = its source), 0 = none, 2 = the same press seen through a second source (keyboard F also sets the
// engine bit: the two edges can land a frame apart) within 90 ms of the last one - not a new press.
typedef struct {
    unsigned seq;
    int init, kb, bot;
    uint64_t last_ms;
} PadinjEdgeT;
static int PadinjMelEdge(PadinjEdgeT *t, uint64_t now, int kb_down,
                           int bot_down, const char **src)
{
    unsigned sq = PadinjMelPresses();
    int pad = t->init && sq != t->seq, kbe = kb_down
        && !t->kb, bote = bot_down && !t->bot;
    t->seq = sq;
    t->kb = kb_down;
    t->bot = bot_down;
    t->init = 1;
    if (!pad && !kbe && !bote)
        return 0;
    if (src)
        *src =
            pad ? (kbe ? "pad bit + keyboard F" : "pad / engine bit") : kbe
            ? "keyboard F / engine melee command" : "bot";
    if (t->last_ms && now - t->last_ms < 90)
        return 2;
    t->last_ms = now;
    return 1;
}

static unsigned PadinjReadsTrue(const char *name)
{
    struct padinj_w *w = PadinjWatch(name);
    return w ? w->reads_true : 0;
}

// per frame: log what the reader hook learned, and a heartbeat while an injection is pending (is Joypad::Update reaching the read joypad at all?)
static void PadinjTick(void)
{
    if (!g_padi.ok || g_padi.tick_frame == g_frame)
        return;
    g_padi.tick_frame = g_frame;
    for (int i = 0; i < PADINJ_NW; i++) {
        struct padinj_w *w = &g_padw[i];
        if (!w->learn_log)
            continue;
        w->learn_log = 0;
        char b[400];
        int q =
            snprintf(b, sizeof b,
                     "padinj: learned '%s' on joypad %p: %u entr%s",
                     w->name, w->jp, w->n_ent,
                     w->n_ent == 1 ? "y" : "ies");
        for (uint32_t k = 0; k < w->n_ent && q < (int) sizeof b - 60; k++)
            q += snprintf(b + q, SNLEFT(b, q),
                          " [kind %u thr %.2f mask %08x%08x]",
                          w->ent[k][0], *(float *) &w->ent[k][1],
                          w->ent[k][3], w->ent[k][2]);
        snprintf(b + q, SNLEFT(b, q), " -> injects input bit %016llx",
                 (unsigned long long) w->mask);
        static int nlog;
        if (LOGCAP(nlog, 20))
            Xlog("%s", b);
    }
    uint64_t now = GetTickCount64();
    if (g_padi.act_until && now - g_padi.rep_ms >= 1500) {
        g_padi.rep_ms = now;
        unsigned p = g_padi.upd_primary;
        static int n;
        if (LOGCAP(n, 40))
            Xlog("padinj: heartbeat - Joypad::Update calls for the read joypad %u (+%u in 1.5 s), other joypads %u, pad buttons applied %u, sticks applied %u", p, p - g_padi.rep_prim, g_padi.upd_other, g_padi.btn_applied, g_padi.st_applied);
        g_padi.rep_prim = p;
    }
}
