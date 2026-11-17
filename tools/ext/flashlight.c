// Flashlight (the SP flashlight is turned off by the mod because T is used to mark; light and light sync are added because some areas of the map are very dark). Included by t2r-ext.c after helpcall.c. Log prefix "flashlight:". Opt out: t2r-no-flashlight.txt next to the exe (no light, no sync).
//
// WHAT THE ENGINE DOES (static RE of tlou-ii.exe 1.6.10721 + winmm):
//   * The player's SP flashlight is a pooled Flashlight object (32 entries of 0xd0 bytes; kind 4/5 player, 2 npc, 3 buddy), driven by the player controller's event
//     handler (exe 0x140b750d0): the script EVENTS turn-on-flashlight / turn-off-flashlight call the FlashlightController (player+0x3198) vfunc +0x30 / +0x28 and set
//     player+0x4980. The pad / key toggle (exe 0x140b9ebea) reads the joypad command flashlight-toggle and posts those same two events. NPCs handle the same two
//     events (exe 0x140494800 / 0x140497759: Flashlight::SetOn 0x140aaf760(flashlight, on, owner, silent) on the object of npc vfunc +0x1650 = npc+0x3e40).
//   * Why MP has no flashlight: winmm's per-frame update (0x180032ad0, first thing every frame) does send-event(turn-off-flashlight, player) (the call at
//     0x180032b73) and joypad-disable-commands(dodge, evade-start, flashlight-toggle, quick-turn). The command itself is the MARK button (T / R3): winmm's mark
//     latch reads it through its own native (0x18000a270), unaffected by the disable.
// WHAT WE DO:
//   * OWN LIGHT: toggle = the mark button (T / R3): a press that marks nothing (no target, no ping) toggles the light after 0.3 s. While it
//     is on, the 2 byte `call rsi` at 0x180032b73 is NOPped (the dodge lock's patch pattern, bytes verified) so winmm stops turning it off, and we send
//     send-event(turn-on-flashlight, player) once (the engine's own recipe, same argv as winmm's turn-off). Off = event turn-off-flashlight + the patch removed.
//     Off at spawn, off on down / death / leaving the match. The SP light comes with the SP behaviour: gun/shoulder mount, shadows, volumetrics, any weapon.
//     player-using-flashlight? is read back after the event and the event re-sent (twice) if the engine says no; the log line says what the engine answered.
//   * SYNC: MSG_FLASH 0xAB [u8 bit0 on], broadcast (enemies see it too, the SP trade-off) on every change and re-sent every 1.5 s while on; a receiver drops a
//     light that is not refreshed for 6 s. Unknown to an old client = ignored like every other message of ours.
//   * PUPPET LIGHTS / OWN OBJECT (fix: player-get-flashlight 0 = MP never creates the player's Flashlight object): the scripts' own
//     recipe is send-event(spawn-flashlight, <character>) with no argument, then send-event(turn-on-flashlight, <character>) (ss-pat-dina-flashlight,
//     ss-test-flashlight, ss-efp-joel-flashlight). The player event handler (exe 0x140b750d0, event sid 0xd0edb0e12c55b1d9) spawns the player's
//     FlashlightContainer (kind 0, attach point flashlightAttach, define = the engine default) when no player flashlight handle exists; the NPC handler does the
//     same for characters. So: own light = spawn-flashlight(player) when player-get-flashlight is 0, then turn-on; puppets (within 40 m, 4 nearest) =
//     spawn-flashlight(puppet) once, turn-on-flashlight, turn-off when culled. The engine light has shadows, so a wall hides it (no ESP). 1.5 s after the
//     spawn the puppet's flashlight field (npc+0x3e40) is logged. NOTE: attach-flashlight is NOT this (the earlier version called it wrongly: its native is
//     exe 0x140407be0 = (object, attach-name, x, y, z) -> bool, and the handler pairing in the earlier notes was shifted by one stub).
//   REWORK (delay, no strap models, puppets): (1) the toggle is decided on the press frame from the mark code's own verdict (g_mark_nomark_n / winmm counters,
//   fls_toggle_edge); (2) the own FlashlightContainer (strap model + light) is spawned at match start (fls_own_spawn_tick); (3) puppet light + strap model = the engine's FlashlightContainer spawn
//   0x140aac560 (kind 4, look flashlight-l-military, attach flashlightAttach) + Flashlight::SetOn on the container (fls_puppets_tick, opt-in t2r-flashlight-puppets.txt).
//   UNVERIFIED in game; every step logs "flashlight:".
#define MSG_FLASH 0xAB          // peer msg (ours): [u8 bit0 = the sender's flashlight is on][i16 camera yaw][i16 camera pitch] (rad x 10000, little endian; the 4 aim bytes are optional: an older sender sends 1 byte)
#define FLS_AIM_MS 80           // camera-aim refresh while on (12.5 Hz)
#define FLS_AIM_STALE_MS 1000   // an aim older than this is ignored (head-joint fallback)
#define FLS_VERDICT_MS 300      // after a mark-button press: did it mark? (async cast result)
#define FLS_HB_MS 1500          // keep-alive while on
#define FLS_STALE_MS 6000       // a remote light not refreshed this long is off
#define FLS_RANGE 120.0f        // puppet lights only within this many metres (players 44 m apart on Supermarket got no light at the old 40 m; a lit strap light is seen across the map in the dark)
#define FLS_MAX_PUP 4           // at most this many puppet lights lit at once (engine light budget)
#define FLS_WINMM_CALL 0x180032b73ULL   // winmm: `call rsi` = send-event(turn-off-flashlight, player), every frame
// OFF by default; the launcher copied t2r-no-flashlight.txt only from 0.1.33 on, and a Play press
// during its self-update kept the old launcher, so the flag never arrived. t2r-flashlight-on.txt next to the exe turns them on (tests).
static int FlsOff(void)
{
    static uint64_t at;
    static int on;
    return !FeelFlag("t2r-flashlight-on.txt", &at, &on, 120);
}

static struct {
    int on, gated, bad_gate;
    uint64_t on_ms, hb_ms, hold_ms, assert_ms;
    int hold_fired, asserts, using_logged, pressed, bomb_press;
    uint32_t mark_ok0, mark_fail0, nomark0;
    uint64_t tgt0;
} g_fls;
static struct {
    uint8_t on, have;
    uint64_t ts, aim_ts;
    float dir[3], sm[3];
} g_fls_rem[T2R_MAX_PEERS];     // remote light state per slot
static uint64_t g_flsOwnSpawnMs;     // when our own spawn-flashlight event was last sent
static struct {
    uint64_t body, on_ms, spawn_ms, cont, cont_holder;
    uint32_t cont_id, owner_id;
    uint8_t spawned, lit, checked, retries;
} g_fls_pup[T2R_MAX_PEERS];     // puppet light per slot (body = the puppet handle the spawn-flashlight went to)

static void *FlsProc(uint64_t h);
static void FlsHexdump(const char *tag, const uint8_t * p, unsigned n);
static uint8_t *FlsExe(void)
{
    return (uint8_t *) GetModuleHandleW(NULL);
}

// winmm's per-frame turn-off-flashlight: allowed (call) / suppressed (NOP) - bytes verified, like melee_dodge.c's gate
static void FlsWinmmGate(int suppress)
{
    static const uint8_t orig[2] = { 0xFF, 0xD6 }, pat[2] = { 0x66, 0x90 };
    if (g_fls.bad_gate || g_fls.gated == suppress)
        return;
    uint8_t *f = (uint8_t *) T2R_LEGACY_VA(FLS_WINMM_CALL);
    if (IsBadReadPtr(f, 2) || (memcmp(f, orig, 2) && memcmp(f, pat, 2))) {
        g_fls.bad_gate = 1;
        HOOK_MISS(f, 2,
                  "flashlight: winmm 0x180032b73 (the per-frame send-event turn-off-flashlight) bytes differ - the flashlight cannot stay on");
        return;
    }
    DWORD old;
    if (!VirtualProtect(f, 2, PAGE_EXECUTE_READWRITE, &old)) {
        g_fls.bad_gate = 1;
        HOOK_FAIL("flashlight: winmm per-frame turn-off not patchable");
        return;
    }
    memcpy(f, suppress ? pat : orig, 2);
    VirtualProtect(f, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 2);
    g_fls.gated = suppress;
    Xlog("flashlight: winmm's per-frame turn-off-flashlight %s",
         suppress ? "SUPPRESSED" : "restored");
}

static uint64_t FlsEvent(const char *ev)
{
    uint64_t a[2] = { Fnv64Rt(ev), SID_PLAYER };
    return DcCall(FNV64("send-event"), 2, a);
}

static int FlsEngineUsing(void)
{                               // -1 unknown, else the engine's answer
    uint64_t n = 0, r = DcCall(FNV64("player-using-flashlight?"), 0, &n);
    return r == ~0ULL ? -1 : (r & 0xff) ? 1 : 0;
}

static int FlsCamFwd(float *f);
// other players' beams follow THEIR camera: the sender's camera forward as yaw / pitch (rad x 10000) rides on the on/keep-alive message
static void FlsBroadcast(int on)
{
    if (!g_sendTramp)
        return;
    uint8_t b[5] = { (uint8_t) (on ? 1 : 0), 0, 0, 0, 0 };
    uint64_t n = 1;
    float f[3];
    if (on && FlsCamFwd(f)) {
        int16_t y =
            (int16_t) (__builtin_atan2f(f[0], f[2]) * 10000.0f), pt =
            (int16_t) (__builtin_asinf
                       (f[1] > 1 ? 1 : f[1] < -1 ? -1 : f[1]) * 10000.0f);
        memcpy(b + 1, &y, 2);
        memcpy(b + 3, &pt, 2);
        n = 5;
    }
    g_sendTramp(MSG_FLASH, b, n, 0xff);
}

static void FlsSet(int on, const char *why)
{
    if (on == g_fls.on)
        return;
    uint64_t now = GetTickCount64();
    g_fls.on = on;
    g_fls.on_ms = g_fls.hb_ms = now;
    g_fls.asserts = 0;
    g_fls.assert_ms = now + 400;
    g_fls.using_logged = 0;
    if (on)
        FlsWinmmGate(1);
    if (on) {                   // MP never gave the player a Flashlight object (night26: player-get-flashlight 0): spawn it the SP way
        uint64_t n = 0, fh0 =
            DcCall(FNV64("player-get-flashlight"), 0, &n);
        if (!fh0 || fh0 == ~0ULL) {
            uint64_t sr = FlsEvent("spawn-flashlight");
            Xlog("flashlight: no player flashlight object - send-event spawn-flashlight(player) -> %llx", (unsigned long long) sr);
        }
    }
    uint64_t r =
        FlsEvent(on ? "turn-on-flashlight" : "turn-off-flashlight");
    {
        uint64_t n = 0, fh =
            DcCall(FNV64("player-get-flashlight"), 0, &n);
        Xlog("flashlight: %s (%s) - send-event %s -> %llx; player-get-flashlight %llx; engine says using = %d; winmm turn-off %s", on ? "ON" : "OFF", why, on ? "turn-on-flashlight" : "turn-off-flashlight", (unsigned long long) r, (unsigned long long) fh, FlsEngineUsing(), g_fls.gated ? "suppressed" : "running");
    }
    if (on) {                   // one raw dump of the player's Flashlight object: its define / settings words show what the player path resolved
        static int dumped;
        uint64_t n2 = 0, fh2 =
            DcCall(FNV64("player-get-flashlight"), 0, &n2);
        uint8_t *fo = (!dumped && fh2
                       && fh2 != ~0ULL) ? (uint8_t *) FlsProc(fh2) : NULL;
        if (fo && !IsBadReadPtr(fo, 0x1a0)) {
            dumped = 1;
            FlsHexdump("player flashlight object +0x00", fo, 0x100);
            FlsHexdump("player flashlight object +0x100", fo + 0x100,
                        0xa0);
        }
    }
    if (!on)
        FlsWinmmGate(0);
    FlsBroadcast(on);
    if (!on)
        FlsBroadcast(0);       // (a lost off would leave a light on for 6 s)
}

static int FlsAlive(void)
{
    return MatchSubstate() == 6 && !g_downed && !g_downDying
        && LocalHp() > 0.0f;
}

// ---- toggle input: the mark button (engine command flashlight-toggle: T on a keyboard, R3 on a pad) marks when the press marks something and
// toggles the light otherwise. Verdict FLS_VERDICT_MS after the press: winmm's mark counter (0x18013A7E8) moved, a mark target is held (0x18013A7C8) or
// helpcall pinged a downed teammate = it marked, no toggle.
static int FlsToggleEdge(uint64_t now)
{
    // the flashlight turned on with a noticeable delay: the verdict is taken on the PRESS frame from the mark's own result instead of waiting 300 ms.
    // mark_press_tick (t2r-ext.c) counts every press whose async cast found no usable target (g_mark_nomark_n, the "mark: press found no usable target" line):
    // that moves the same frame or the next one = toggle at once. A cast target at the press (7c8 != 0) = the mark lands (winmm's counter 7e8 moves) or helpcall
    // pings = no toggle; if winmm refuses it (7e0 moves) = toggle. FLS_VERDICT_MS stays only as the safety net for a press nothing answered.
    WORD pb = 0;
    BYTE lt, rt;
    SHORT lx, ly;
    // The key is winmm's mark latch (0x18013A7D0), set by the
    // engine command flashlight-toggle through winmm's own read (not hit by joypad-disable-commands), so a rebound key works and T only when it is bound.
    // t2r-flashlight-key-t.txt = the old hard-coded T as well.
    static int keyt = -1;
    if (keyt < 0)
        keyt =
            GetFileAttributesA("t2r-flashlight-key-t.txt") !=
            INVALID_FILE_ATTRIBUTES;
    int latch = *(volatile uint8_t *) T2R_SYM(kFlashlightLatch) != 0;
    int h = GameFocused() && (latch
                               || (keyt
                                   && (GetAsyncKeyState('T') & 0x8000))
                               || PadCmdOn("flashlight-toggle")
                               || (PadState(&pb, &lt, &rt, &lx, &ly)
                                   && (pb & 0x0080)));
    static int was;
    int edge = h && !was;
    was = h;
    if (edge && !g_fls.hold_ms) {
        g_fls.hold_ms = now;
        g_fls.hold_fired = 0;
        g_fls.mark_ok0 = *(volatile uint32_t *) T2R_SYM(kMarkOkCounter);
        g_fls.mark_fail0 =
            *(volatile uint32_t *) T2R_SYM(kMarkFailCounter);
        g_fls.nomark0 = g_markNomarkN;
        g_fls.tgt0 = *(volatile uint64_t *) T2R_SYM(kMarkTarget);
        g_fls.pressed = 1;
        {
            float cp[3], cd[3];
            g_fls.bomb_press = PbMarkAimed(cp, cd) >= 0;
        }                       // The reticle is on an enemy bomb = this press marks it (placedbomb2013.c pb_mark_tick), never a toggle
    }
    if (!g_fls.hold_ms || g_fls.hold_fired)
        return 0;
    uint64_t age = now - g_fls.hold_ms;
    int pinged = 0;
    for (int i = 0; i < T2R_MAX_PEERS; i++)
        if (g_hcPingReady[i] > now
            && g_hcPingReady[i] - now > HC_PING_CD_MS - 400)
            pinged = 1;         // a ping sent within the last 0.4 s
    int marked =
        *(volatile uint32_t *) T2R_SYM(kMarkOkCounter) != g_fls.mark_ok0
        || pinged || g_fls.bomb_press;
    int nomark = g_markNomarkN != g_fls.nomark0
        || *(volatile uint32_t *) T2R_SYM(kMarkFailCounter) !=
        g_fls.mark_fail0;
    if (!marked && g_fls.tgt0 == 0 && age >= 40)
        nomark = 1;             // no cast target at the press and the mark code had two frames to say something else
    if (marked) {
        g_fls.hold_fired = 1;
        g_fls.hold_ms = 0;
        Xlog("flashlight: mark button press marked something (%llu ms) - no toggle", (unsigned long long) age);
        return 0;
    }
    if (nomark || age >= FLS_VERDICT_MS) {
        g_fls.hold_fired = 1;
        g_fls.hold_ms = 0;
        Xlog("flashlight: mark button press marked nothing (%llu ms after the press, %s) - toggle", (unsigned long long) age, nomark ? "mark code said no target" : "verdict timeout");
        return 1;
    }
    return 0;
}

// ---- remote lights
static int FlsRecv(uint32_t slot, const uint8_t *buf, uint64_t len)
{
    if (slot >= T2R_MAX_PEERS || !buf || len < 1 || slot == SkMe())
        return 0;
    uint8_t on = buf[0] & 1;
    if (g_fls_rem[slot].on != on) {
        static unsigned n;
        if (LOGCAP(n, 40))
            Xlog("flashlight: slot %u light %s", slot, on ? "on" : "off");
    }
    g_fls_rem[slot].on = on;
    g_fls_rem[slot].ts = GetTickCount64();
    if (on && len >= 5) {
        int16_t y, pt;
        memcpy(&y, buf + 1, 2);
        memcpy(&pt, buf + 3, 2);
        float yw = y * 0.0001f, ph = pt * 0.0001f;
        if (ph > -1.6f && ph < 1.6f) {
            float *d = g_fls_rem[slot].dir;
            d[0] = __builtin_sinf(yw) * __builtin_cosf(ph);
            d[1] = __builtin_sinf(ph);
            d[2] = __builtin_cosf(yw) * __builtin_cosf(ph);
            if (!g_fls_rem[slot].have) {
                memcpy(g_fls_rem[slot].sm, d, 12);
                static unsigned n;
                if (LOGCAP(n, 20))
                    Xlog("flashlight: slot %u camera aim received (yaw %.0f pitch %.0f deg) - puppet beam follows it", slot, yw * 57.2958f, ph * 57.2958f);
            }
            g_fls_rem[slot].have = 1;
            g_fls_rem[slot].aim_ts = g_fls_rem[slot].ts;
        }
    } else if (!on)
        g_fls_rem[slot].have = 0;
    return 1;
}

typedef void *(*fls_fn_proc)(uint64_t);
static void *FlsProc(uint64_t h)
{
    return h
        && h !=
        ~0ULL ? ((fls_fn_proc) T2R_SYM(kFnEntityFromHandle)) (h) : NULL;
}

static uint64_t FlsEventTo(const char *ev, uint64_t target)
{                               // send-event(event, character): the scripts' own call (ss-pat-dina-flashlight, ss-test-flashlight: spawn-flashlight / turn-on-flashlight)
    uint64_t a[2] = { Fnv64Rt(ev), target };
    return DcCall(FNV64("send-event"), 2, a);
}

// the puppet's flashlight handle field (NPC vfunc +0x1650 reads npc+0x3e40 / gen +0x3e48): set once the engine made it one
static uint64_t FlsPupField(uint64_t body)
{
    uint8_t *p = (uint8_t *) FlsProc(body);
    if (!p || IsBadReadPtr(p + 0x3e40, 12))
        return 0;
    return *(uint64_t *) (p + 0x3e40);
}

static void FlsHexdump(const char *tag, const uint8_t *p, unsigned n)
{
    char line[300];
    unsigned o = 0;
    for (unsigned i = 0; i < n && o < sizeof line - 20; i += 8)
        o += (unsigned) snprintf(line + o, sizeof line - o, "%016llx ",
                                 (unsigned long long) *(const uint64_t
                                                        *) (p + i));
    Xlog("flashlight: %s: %s", tag, line);
}

// ---- PUPPET LIGHT + STRAP MODEL, the engine's own container spawn (SP trace: EVERY character - player, buddy, NPC - goes through the
// FlashlightContainer spawn exe 0x140aac560(settings, kind, define, r9 0, byte 1, dword owner pid) which carries model look + attach point + define: it spawns the
// container process (returned in rax), that spawns the pooled Flashlight (owner = the container) and the model on the attach point. Settings struct (0xa8 bytes, as logged for
// the buddy / NPC): +0x1c f32 1.0, +0x3c f32 1.0, +0x40 owner holder ptr (proc+0x80), +0x48 owner id (proc+0x94), +0x50 look sid, +0x70 attach sid, +0x78 sid "root"
// 0xa354fd1ff0c467c5, +0xa0 byte 0xff. Puppets: kind 4 (NPC pool), look flashlight-l-military, attach flashlightAttach (r_clavicle strap), define = the first record that exists
// (looked up by the engine's own table lookup 0x141356d60 - none = nothing is called). The light is switched with Flashlight::SetOn(container, on, 0, silent) (SetOn finds the pool
// entry by its owner = the container). Opt-in t2r-flashlight-puppets.txt (light AND strap model); every step logs.
static uint8_t *FlsPoolEntry(unsigned i)
{
    return FlsExe() + 0x40d7910 + (size_t) i *0xd0;
}                               // entry i: +0 holder ptr, +8 id (u32), +0x10 flags

static uint8_t *FlsHolderProc(uint64_t holder, uint32_t id)
{                               // {holder ptr, id} -> process (holder+8), id checked at +0x94
    if (!holder || !id || IsBadReadPtr((void *) holder, 16))
        return NULL;
    uint8_t *p = *(uint8_t **) (holder + 8);
    if (!p || IsBadReadPtr(p, 0x1a0) || *(uint32_t *) (p + 0x94) != id)
        return NULL;
    return p;
}

static int FlsPoolFind(const uint8_t *owner, int *on, uint8_t **fo)
{                               // the pooled Flashlight whose owner is `owner` (-1 none)
    for (unsigned i = 0; i < 32; i++) {
        uint8_t *e = FlsPoolEntry(i);
        if (IsBadReadPtr(e, 0x20))
            return -1;
        uint8_t *f =
            FlsHolderProc(*(uint64_t *) e, *(uint32_t *) (e + 8));
        if (!f)
            continue;
        uint8_t *o =
            FlsHolderProc(*(uint64_t *) (f + 0x190),
                            *(uint32_t *) (f + 0x198));
        if (o && o == owner) {
            if (on)
                *on = (e[0x10] >> 1) & 1;
            if (fo)
                *fo = f;
            return (int) i;
        }
    }
    return -1;
}

static int FlsPoolCount(void)
{
    int n = 0;
    for (unsigned i = 0; i < 32; i++) {
        uint8_t *e = FlsPoolEntry(i);
        if (!IsBadReadPtr(e, 0x20)
            && FlsHolderProc(*(uint64_t *) e, *(uint32_t *) (e + 8)))
            n++;
    }
    return n;
}

static void FlsSeton(uint8_t *owner, int on, int silent)
{
    typedef void (*fn_seton)(void *, uint8_t, void *, uint8_t);
    ((fn_seton) (FlsExe() + 0xaaf760)) (owner, (uint8_t) on, NULL,
                                         (uint8_t) silent);
}

// a define record that exists (exe table lookup 0x141356d60: entry -> value pointer, first 0x90 bytes readable)
static int FlsDefExists(uint64_t sid)
{
    if (!sid)
        return 0;
    void *(*find)(uint64_t) =(void * (*)(uint64_t))(FlsExe() + 0x1356d60);
    void *e = find(sid);
    if (!e || IsBadReadPtr(e, 8))
        return 0;
    uint8_t *v = *(uint8_t **) e;
    return v && !IsBadReadPtr(v, 0x90);
}

static uint64_t FlsDefPick(void)
{
    static uint64_t def;
    static int tried;
    if (tried)
        return def;
    tried = 1;
    static const char *const C[] =
        { "*flashlight-wat-infected-tunnels*", "*flashlight-default*",
"*flashlight-default-buddy*", "*flashlight-npc*" };
    for (unsigned i = 0; i < sizeof C / sizeof *C; i++) {
        uint64_t sid = Fnv64Rt(C[i]);
        int ok = FlsDefExists(sid);
        Xlog("flashlight: define %s (%016llx) record %s", C[i],
             (unsigned long long) sid, ok ? "exists" : "MISSING");
        if (ok && !def)
            def = sid;
    }
    uint64_t g = *(volatile uint64_t *) (FlsExe() + 0x3f03b28);
    Xlog("flashlight: player default define global = %016llx (%s); chosen define %016llx", (unsigned long long) g, g && FlsDefExists(g) ? "record exists" : "no record", (unsigned long long) def);
    // SHADOWS: SP's PLAYER define (*flashlight-wat-infected-tunnels*) has the two flag bytes +0x78 and +0x79 set (0x0101), the buddy define only +0x78, NPC defines neither
    // (SP record dumps). They are the only per-define flags that differ besides the light numbers, so a stand-in define (*flashlight-default* ...) gets both set.
    // The record is dumped; t2r-flashlight-nodefpatch.txt skips the patch.
    if (def) {
        void *(*find)(uint64_t) =
            (void * (*)(uint64_t))(FlsExe() + 0x1356d60);
        void *e = find(def);
        uint8_t *v = e && !IsBadReadPtr(e, 8) ? *(uint8_t **) e : NULL;
        if (v && !IsBadReadPtr(v, 0x90)) {
            FlsHexdump("chosen define record +0x00", v, 0x90);
            static uint64_t at;
            static int off;
            if (def != Fnv64Rt("*flashlight-wat-infected-tunnels*")
                && !FeelFlag("t2r-flashlight-nodefpatch.txt", &at, &off,
                              120) && !IsBadWritePtr(v + 0x78, 2)
                && (v[0x78] != 1 || v[0x79] != 1)) {
                Xlog("flashlight: define flags +0x78/+0x79 were %u/%u -> 1/1 (the SP player define's)", v[0x78], v[0x79]);
                v[0x78] = 1;
                v[0x79] = 1;
            }
        }
    }
    return def;
}

// FlashlightContainer spawn for a puppet; returns the container process (NULL = nothing spawned)
static uint8_t *FlsContainerSpawn(uint32_t s, uint8_t *proc,
                                    uint64_t def)
{
    static __attribute__((aligned(16))) uint8_t ring[8][0xb0];
    static unsigned ri;
    uint8_t *st = ring[ri++ & 7];
    memset(st, 0, 0xb0);
    float one = 1.0f;
    memcpy(st + 0x1c, &one, 4);
    memcpy(st + 0x3c, &one, 4);
    *(uint64_t *) (st + 0x40) = *(uint64_t *) (proc + 0x80);
    *(uint32_t *) (st + 0x48) = *(uint32_t *) (proc + 0x94);
    *(uint64_t *) (st + 0x50) = Fnv64Rt("flashlight-l-military");
    *(uint64_t *) (st + 0x70) = 0xf2612aea948e517eULL;  // flashlightAttach
    *(uint64_t *) (st + 0x78) = 0xa354fd1ff0c467c5ULL;
    st[0xa0] = 0xff;
    typedef uint8_t *(*fn_cont)(void *, uint32_t, uint64_t, uint64_t,
                                uint64_t, uint64_t);
    Xlog("flashlight: slot %u container spawn: owner %p pid %u kind 4 look flashlight-l-military attach flashlightAttach define %016llx; pool holds %d lights", s, (void *) proc, *(uint32_t *) (proc + 0x94), (unsigned long long) def, FlsPoolCount());
    uint8_t *c =
        ((fn_cont) (FlsExe() + 0xaac560)) (st, 4, def, 0, 1,
                                            *(uint32_t *) (proc + 0x94));
    Xlog("flashlight: slot %u container spawn returned %p", s, (void *) c);
    return c && !IsBadReadPtr(c, 0x1a0) ? c : NULL;
}

// The container is kept as {holder, id} like the engine's own handles (the pool entries, the owner links): the engine's heap compaction MOVES processes ("puppet body
// watch ... moved (heap compaction)" 108x in a test log), so the raw process pointer returned by the spawn is stale after the first compaction - every later
// check of it failed silently ("pooled Flashlight" line missing after the 2nd spawn, no SetOn ever) - while holder+8 always points at the current process.
static uint8_t *FlsContGet(uint32_t s)
{
    return FlsHolderProc(g_fls_pup[s].cont_holder, g_fls_pup[s].cont_id);
}

// Kill a puppet's container + its pooled light (the engine's own process destroy funnel 0x141341470, as pst_kill does for a weapon). Without this a respawned puppet body
// (new pid; log: spawns for pid 15201 / 15435 / 36605 / 38389, pool 0 -> 2 -> 3 -> 4) left the old container's strap model + light standing where the body died.
static void FlsContKill(uint32_t s, const char *why)
{
    uint8_t *c = FlsContGet(s);
    if (!c)
        return;
    int on = 0;
    uint8_t *fo = NULL, *exe = FlsExe();
    int n0 = FlsPoolCount(), idx = FlsPoolFind(c, &on, &fo);
    if (idx >= 0)
        FlsSeton(c, 0, 1);
    if (fo)
        ((void (*)(void *)) (exe + 0x1341470)) (fo);
    ((void (*)(void *)) (exe + 0x1341470)) (c);
    Xlog("flashlight: slot %u container {%llx,%u} destroyed (%s); pooled light %s; pool %d -> %d", s, (unsigned long long) g_fls_pup[s].cont_holder, g_fls_pup[s].cont_id, why, idx >= 0 ? "destroyed too" : "was not pooled", n0, FlsPoolCount());
}

// ---- AIM (the light did not match where the characters are looking): the container's light follows the strap joint (r_clavicle), which does not turn with the look. The puppet's HEAD
// joint does (the synced look / animation drives it), so the pooled light's orientation is turned every frame onto the head's forward. The head's and the light's local forward axes are not
// known statically: they are calibrated at run time on OUR OWN body, where the truth is known (the camera's reticle locator +Z): head axis = camera forward in head-joint space (while alive),
// light axis = camera forward in our own pooled light's space (while our own light is on - the engine aims it at the camera). Until both are calibrated nothing is written.
static float g_fls_cal_h[3], g_fls_cal_l[3];
static int g_fls_cal_hn, g_fls_cal_ln, g_fls_ax_h = -1, g_fls_ax_l = 2;
// LIGHT AXIS IS STATIC (the light broke, shifted left and came off the shoulder): the pooled light's local forward is +Z for every body (measured: first calibration cos 0.96 on +Z; diag z-axis
// follows the aim). Learning it from the engine-aimed light was wrong whenever the engine light did NOT point at the camera (it follows the r_clavicle strap joint), so a calibration taken then
// picked a random axis / sign for the rest of the process and the beam stayed on the shoulder or swung left. Now +Z from the first frame, written every frame, no per-body state.
static float g_fls_axv_h[3], g_fls_axv_l[3] = { 0, 0, 1 };

static int FlsCamFwd(float *f)
{
    uint64_t none0 = 0, cam =
        DcCall(0x42413e1c037969a8ULL /* camera-through-reticle-locator */
                , 0, &none0);
    uint64_t cr = cam
        && cam != ~0ULL ? DcCall(FNV64("get-locator-rotation"), 1,
                                  &cam) : 0;
    if (!cr || cr == ~0ULL || IsBadReadPtr((void *) cr, 16))
        return 0;
    const float *q = (const float *) cr;
    f[0] = 2.0f * (q[0] * q[2] + q[3] * q[1]);
    f[1] = 2.0f * (q[1] * q[2] - q[3] * q[0]);
    f[2] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
    float n = __builtin_sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(n > 0.9f && n < 1.1f))
        return 0;
    f[0] /= n;
    f[1] /= n;
    f[2] /= n;
    return 1;
}

static void FlsQinvRot(const float *q, const float *v, float *o)
{
    float c[4] = { -q[0], -q[1], -q[2], q[3] };
    Qrotv(c, v, o);
}

static void FlsCalAdd(float *sum, int *n, const float *q,
                        const float *cam, int *ax, float *axv,
                        const char *what)
{
    float l[3];
    FlsQinvRot(q, cam, l);
    for (int i = 0; i < 3; i++)
        sum[i] += l[i];
    if (++*n < 90 || *ax >= 0)
        return;
    int best = 0;
    for (int i = 1; i < 3; i++)
        if (__builtin_fabsf(sum[i]) > __builtin_fabsf(sum[best]))
            best = i;
    float m =
        __builtin_sqrtf(sum[0] * sum[0] + sum[1] * sum[1] +
                        sum[2] * sum[2]);
    float cs = m > 0 ? __builtin_fabsf(sum[best]) / m : 0;
    Xlog("flashlight: aim calibration (%s): mean local direction of the camera forward %.2f %.2f %.2f over %d frames, best axis %d cos %.2f", what, sum[0] / m, sum[1] / m, sum[2] / m, *n, best, cs);
    if (cs >= 0.8f) {
        *ax = best;
        axv[0] = axv[1] = axv[2] = 0;
        axv[best] = sum[best] > 0 ? 1.0f : -1.0f;
    } else {
        memset(sum, 0, 12);
        *n = 0;
    }
}

static void FlsCalTick(uint8_t *own_fo)
{
    float cam[3];
    if (g_fls_ax_h >= 0 && (g_fls_ax_l >= 0 || !own_fo))
        return;
    if (!FlsCamFwd(cam))
        return;
    if (g_fls_ax_h < 0) {
        float hp[3], hq[4];
        if (XbxLoc(SID_PLAYER, "head", hp, hq))
            FlsCalAdd(g_fls_cal_h, &g_fls_cal_hn, hq, cam, &g_fls_ax_h,
                        g_fls_axv_h, "head");
    }
    if (g_fls_ax_l < 0 && own_fo && !IsBadReadPtr(own_fo + 0x130, 0x20))
        FlsCalAdd(g_fls_cal_l, &g_fls_cal_ln,
                    (const float *) (own_fo + 0x140), cam, &g_fls_ax_l,
                    g_fls_axv_l, "light");
}

// One player's beam still did not follow its camera on the other screen (log: "slot 1 aim: the engine rewrote the light orientation since our write" EVERY
// frame, and the same for our own light): the container's locator (+0x130 / +0x140) is the attach-driven strap transform, rewritten from the strap joint each frame,
// and it is NOT what aims the beam. The beam comes from the light's pool entry (exe+0x40d7910, stride 0xd0): the engine's own aim setters (0x140aaefd0: entry +0x70 =
// source position, flag 0x40 in +0x10; 0x140aaef10: entry +0x60 = aim target point, +0x44 = the float arg, flag 0x20), found by the container owner. The player's flashlight
// controller (0x140b9e6a0) calls both every frame (target = its aim point, float 0.3 / -1.0) and the NPC path (0x14090d430: character +0xc2c0 = its container)
// calls them with -1.0 - that is why our own beam follows the camera and the puppet's never did. Puppets now get the same two calls every frame:
// source = the strap joint, target = 30 m along the replicated camera direction.
static float g_flsWant[T2R_MAX_PEERS][3];      // the direction last given to a puppet's light (diag: entry +0x60 vs it)
static void FlsPoolAim(int key, uint8_t *cont, const float *pos,
                         const float *d)
{
    typedef void (*fn_srcpos)(void *, const float *);
    typedef void (*fn_target)(void *, const float *, float);
    static unsigned nlog[T2R_MAX_PEERS];
    // diag (earlier logs): OUR OWN entry, written every frame by the player controller through these same two setters, holds at +0x60 a UNIT DIRECTION
    // (A [14228] 0.99 -0.17 0.03, B [25808] 0.07 -0.14 -0.99 = the camera forward) and at +0x70 the source 11 cm in front of r_clavicle. The second setter takes a
    // direction, not a target point: we gave it a world point (A [31364] -506.64 13.57 253.19), a 500 m "direction" - the beam broke and vanished. Now exactly what
    // the controller writes: source = strap joint + 12 cm along the aim, direction = the owner's replicated camera forward (unit, w 0), float -1.0.
    // bystanders' flashlights must not cast shadows of their own player (the arms block the light and the arm shadows show): the shadow
    // origin is the light source itself (the render light builder 0x1403fea30 takes one position per pool entry, entry +0x70); no owner-exclusion field exists
    // for it - the only per-entry id (+0x98, from the container spawn's last dword, the render light's +0x140) is 0 for the player AND for SP NPCs, and the
    // player-only branches there (entry index 0: 0x1403fe0a0, the second copied light) only add geometry, no caster filter. So the source goes IN FRONT of
    // the hands: 0.7 m along the aim (aimed rifle hands are ~0.6 m ahead of the clavicle), pulled back to 0.15 m short of any wall a world ray (mask 0x207,
    // the puppet ignored) finds within that, never closer than 0.12 m; shrinks at once, grows back at 2 m/s. t2r-fls-src-near.txt = the old 12 cm.
    static uint64_t rj[T2R_MAX_PEERS];
    static float fwd[T2R_MAX_PEERS], fwant[T2R_MAX_PEERS];
    static uint64_t fms[T2R_MAX_PEERS];
    float use = 0.12f;
    {
        static uint64_t at2;
        static int near_on;
        if (key >= 0 && key < T2R_MAX_PEERS
            && !FeelFlag("t2r-fls-src-near.txt", &at2, &near_on, 120)) {
            const float FMAX = 0.7f, RLEN = 0.85f;
            if (rj[key]) {
                float t = RayT(rj[key]);
                rj[key] = 0;
                float hit = t >= 0.0f && t < 0.999f ? t * RLEN : RLEN;
                float w = hit - 0.15f;
                fwant[key] = w < 0.12f ? 0.12f : w > FMAX ? FMAX : w;
            } else if (fwant[key] <= 0.0f)
                fwant[key] = 0.12f;
            uint64_t now = GetTickCount64();
            float dt = fms[key]
                && now >
                fms[key] ? (float) (now - fms[key]) * 0.001f : 0.0f;
            if (dt > 0.2f)
                dt = 0.2f;
            fms[key] = now;
            if (fwd[key] <= 0.0f || fwant[key] < fwd[key])
                fwd[key] = fwant[key];
            else {
                fwd[key] += 2.0f * dt;
                if (fwd[key] > fwant[key])
                    fwd[key] = fwant[key];
            }
            float b[3] =
                { pos[0] + d[0] * RLEN, pos[1] + d[1] * RLEN,
           pos[2] + d[2] * RLEN };
            rj[key] = WorldRayKickM(pos, b, g_fls_pup[key].body, 0x207);
            use = fwd[key];
        }
    }
    __attribute__((aligned(16))) float p4[4] =
        { pos[0] + d[0] * use, pos[1] + d[1] * use, pos[2] + d[2] * use,
1.0f };
    __attribute__((aligned(16))) float t4[4] = { d[0], d[1], d[2], 0.0f };
    if (key >= 0 && key < T2R_MAX_PEERS)
        memcpy(g_flsWant[key], d, 12);
    uint8_t *exe = FlsExe(), *fo = NULL;
    static int ok = -1;
    if (ok < 0) {               // both setters open with mov [rsp+8],rbx / mov rbx,rdx / lea r8,[pool]
        static const uint8_t pro[8] =
            { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x8b, 0xda };
        ok = !IsBadReadPtr(exe + 0xaaefd0, 8)
            && !IsBadReadPtr(exe + 0xaaef10, 8)
            && !memcmp(exe + 0xaaefd0, pro, 8)
            && !memcmp(exe + 0xaaef10, pro, 8);
        Xlog("flashlight: pool aim setters 0x140aaefd0 / 0x140aaef10 %s",
             ok ? "found" :
             "bytes differ - puppet beams keep the strap aim");
    }
    if (!ok)
        return;
    int idx = FlsPoolFind(cont, NULL, &fo);
    if (idx < 0) {
        if (key >= 0 && key < T2R_MAX_PEERS && nlog[key] < 2) {
            nlog[key]++;
            Xlog("flashlight: slot %d pool aim: no pool entry for container %p", key, (void *) cont);
        }
        return;
    }
    uint8_t *e = FlsPoolEntry((unsigned) idx);
    if (key >= 0 && key < T2R_MAX_PEERS && nlog[key] < 4 && !IsBadReadPtr(e, 0xd0)) {   // what the previous frame's write left (does the engine clear the flags / keep the values)
        nlog[key]++;
        Xlog("flashlight: slot %d pool aim: entry %d flags %02x, +0x70 src %.2f %.2f %.2f, +0x60 dir %.2f %.2f %.2f, +0x44 %.2f -> src %.2f %.2f %.2f (%.2f m ahead of the strap) dir %.2f %.2f %.2f", key, idx, e[0x10], *(float *) (e + 0x70), *(float *) (e + 0x74), *(float *) (e + 0x78), *(float *) (e + 0x60), *(float *) (e + 0x64), *(float *) (e + 0x68), *(float *) (e + 0x44), p4[0], p4[1], p4[2], use, t4[0], t4[1], t4[2]);
    }
    // the flashlight did not show on other screens right after turning on: the beam showed for the frame before our first pool write and was gone
    // after it - the values we wrote stayed exactly (A [24708..24711]: +0x70 / +0x60 = ours, flags 93), so it is what they mean, not that they were lost. The source
    // override put the light's origin ON the strap joint (r_clavicle), inside the puppet's chest; the flashlight casts shadows, so the body occluded its own beam.
    // The player controller's source (0x140b9e6a0) is the player locator + an offset in front of the body, the strap lamp itself is outside the mesh too. So the
    // source is left to the engine (the strap lamp) and only the aim target is set (30 m along the owner's camera: the lamp's few cm offset to the clavicle is
    // nothing at that distance). t2r-fls-pool-src.txt = also override the source again (test).
    // (with the direction right the source is set again, as the controller does for our own light; t2r-fls-pool-nosrc.txt = leave it to the engine)
    static uint64_t at;
    static int nosrc;
    if (!FeelFlag("t2r-fls-pool-nosrc.txt", &at, &nosrc, 120))
        ((fn_srcpos) (exe + 0xaaefd0)) (cont, p4);
    ((fn_target) (exe + 0xaaef10)) (cont, t4, -1.0f);
}

// debug (night58): for 2 s after a light turns on (puppet SetOn / our own toggle) its pool entry every 100 ms - flags, +0x60 target, +0x70 source, +0x44, the other
// vectors of the entry (+0x20 / +0x30 / +0x50 / +0x80: where the engine keeps the computed light) and the body's head + strap joint, so own vs puppet can be compared.
static void FlsEntryDiag(int key, uint8_t *cont, uint64_t body,
                           uint64_t on_ms, uint64_t now)
{
    static uint64_t last[T2R_MAX_PEERS + 1], win[T2R_MAX_PEERS + 1];
    static int nwin[T2R_MAX_PEERS + 1];
    static int nlate;           // after the first 2 s: one line every 2 s while lit (does it stay on and on the aim), 300 lines a session
    if (key < 0 || key > T2R_MAX_PEERS || !cont)
        return;
    int late = now - on_ms > 2000;
    if (now - last[key] < (late ? 2000u : 100u))
        return;
    if (late) {
        if (nlate >= 300)
            return;
        nlate++;
    } else if (win[key] != on_ms) {
        win[key] = on_ms;
        if (++nwin[key] > 6)
            return;
    } else if (nwin[key] > 6)
        return;
    last[key] = now;
    int idx = FlsPoolFind(cont, NULL, NULL);
    if (idx < 0) {
        Xlog("flashlight: diag %s%d +%llu ms: no pool entry",
             key == T2R_MAX_PEERS ? "own" : "slot ",
             key == T2R_MAX_PEERS ? 0 : key,
             (unsigned long long) (now - on_ms));
        return;
    }
    uint8_t *e = FlsPoolEntry((unsigned) idx);
    if (IsBadReadPtr(e, 0xd0))
        return;
    float hp[3] = { 0 }, hq[4], cp[3] = { 0 }, cq[4];
    XbxLoc(body, "head", hp, hq);
    XbxLoc(body, "r_clavicle", cp, cq);
    const float *f = (const float *) e;
    char who[16];
    snprintf(who, sizeof who, key == T2R_MAX_PEERS ? "own" : "slot %d",
             key);
    float ang = -1.0f;          // beam direction in the entry vs the owner's camera direction we gave it (own light: vs our camera forward)
    {
        float w[3] = { 0, 0, 0 };
        if (key < T2R_MAX_PEERS)
            memcpy(w, g_flsWant[key], 12);
        else
            FlsCamFwd(w);
        float a =
            sqrtf(f[0x18] * f[0x18] + f[0x19] * f[0x19] +
                  f[0x1a] * f[0x1a]), b =
            sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (a > 1e-3f && b > 1e-3f) {
            float c =
                (f[0x18] * w[0] + f[0x19] * w[1] +
                 f[0x1a] * w[2]) / (a * b);
            if (c > 1)
                c = 1;
            if (c < -1)
                c = -1;
            ang = acosf(c) * 57.2958f;
        }
    }
    Xlog("flashlight: diag %s +%llu ms entry %d flags %02x (on %d) | +0x60 dir %.2f %.2f %.2f = %.1f deg off the owner's camera | +0x70 src %.2f %.2f %.2f | +0x44 %.2f | +0x20 %.2f %.2f %.2f %.2f +0x30 %.2f %.2f %.2f %.2f +0x50 %.2f %.2f %.2f %.2f +0x80 %.2f %.2f %.2f %.2f | head %.2f %.2f %.2f r_clavicle %.2f %.2f %.2f", who, (unsigned long long) (now - on_ms), idx, e[0x10], (e[0x10] >> 1) & 1, f[0x18], f[0x19], f[0x1a], ang, f[0x1c], f[0x1d], f[0x1e], f[0x11], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[20], f[21], f[22], f[23], f[32], f[33], f[34], f[35], hp[0], hp[1], hp[2], cp[0], cp[1], cp[2]);
    if (key == T2R_MAX_PEERS) { // own light: where the container locator (+0x130 / +0x140: the strap joint's transform, rewritten by the engine every frame) points, vs the entry's beam and the camera
        const uint8_t *co = (const uint8_t *) cont;
        float cam[3], lz[3], q[4];
        if (!IsBadReadPtr(co + 0x130, 0x20) && FlsCamFwd(cam)) {
            memcpy(q, co + 0x140, 16);
            if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] >
                0.81f) {
                static const float zax[3] = { 0, 0, 1 };
                Qrotv(q, zax, lz);
                float c = lz[0] * cam[0] + lz[1] * cam[1] + lz[2] * cam[2];
                if (c > 1)
                    c = 1;
                if (c < -1)
                    c = -1;
                Xlog("flashlight: diag own locator +0x130 %.2f %.2f %.2f, its +Z axis %.2f %.2f %.2f = %.1f deg off the camera (the entry's beam: %.1f deg off it)", *(const float *) (co + 0x130), *(const float *) (co + 0x134), *(const float *) (co + 0x138), lz[0], lz[1], lz[2], acosf(c) * 57.2958f, ang);
            }
        }
    }
    if (now - on_ms < 100)
        FlsHexdump(key ==
                    T2R_MAX_PEERS ? "diag own pool entry" :
                    "diag puppet pool entry", e, 0xd0);
}

// Put the pooled light at `pos` pointing along `d` (world): light forward axis (calibrated on our own engine-aimed light) turned onto d.
static void FlsLightSet(int key, uint8_t *fo, const float *pos,
                          const float *d, const char *who)
{
    static float wr[T2R_MAX_PEERS + 1][4];
    static uint8_t wrok[T2R_MAX_PEERS + 1];
    static unsigned lg;
    if (key >= 0 && key < T2R_MAX_PEERS && fo)
        FlsPoolAim(key, fo, pos, d);  // the beam (pool entry); the locator write below only moves the strap transform
    // Other players' beams still followed their flashlight model: logs of EVERY run show "slot 0 aim: look yaw" / "camera aim received" but never "slot 0 light placed"
    // (the own light's line is there) = for a puppet this function always returned before the write, silently. Every early return now logs its reason once per key, and a light object whose
    // orientation quaternion is not a unit quaternion (never filled for the pooled puppet light) gets an ABSOLUTE orientation built from the wanted direction instead of the relative turn.
    static unsigned why[T2R_MAX_PEERS + 1];
    if (g_fls_ax_l < 0 || !fo || IsBadWritePtr(fo + 0x130, 0x20)) {
        if (why[key]++ < 2)
            Xlog("flashlight: %s aim: not written (ax_l %d, light object %p%s)", who, g_fls_ax_l, (void *) fo, fo && IsBadWritePtr(fo + 0x130, 0x20) ? " not writable" : "");
        return;
    }
    float *q = (float *) (fo + 0x140);
    if (wrok[key] && LOGCAP(lg, 12)
        && (__builtin_fabsf(q[0] - wr[key][0]) +
            __builtin_fabsf(q[1] - wr[key][1]) + __builtin_fabsf(q[2] -
                                                                 wr[key]
                                                                 [2]) +
            __builtin_fabsf(q[3] - wr[key][3])) > 0.02f)
        Xlog("flashlight: %s aim: the engine rewrote the light orientation since our write (%.3f %.3f %.3f %.3f -> %.3f %.3f %.3f %.3f)", who, wr[key][0], wr[key][1], wr[key][2], wr[key][3], q[0], q[1], q[2], q[3]);
    float cur[3], qq[4] = { q[0], q[1], q[2], q[3] };
    float qn0 =
        qq[0] * qq[0] + qq[1] * qq[1] + qq[2] * qq[2] + qq[3] * qq[3];
    float dot = 0;
    if (__builtin_isfinite(qn0) && qn0 > 0.81f && qn0 < 1.21f) {
        Qrotv(qq, g_fls_axv_l, cur);
        dot = cur[0] * d[0] + cur[1] * d[1] + cur[2] * d[2];
        float r[4] =
            { cur[1] * d[2] - cur[2] * d[1], cur[2] * d[0] - cur[0] * d[2],
       cur[0] * d[1] - cur[1] * d[0], 1.0f + dot };
        if (r[3] < 1e-3f) {
            if (why[key]++ < 2)
                Xlog("flashlight: %s aim: not written (light axis opposite the wanted direction)", who);
            return;
        }
        float n =
            __builtin_sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] +
                            r[3] * r[3]);
        for (int i = 0; i < 4; i++)
            r[i] /= n;
        float o[4];
        BpQmul(r, qq, o);
        n = __builtin_sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] +
                            o[3] * o[3]);
        if (!(n > 0.9f && n < 1.1f)) {
            if (why[key]++ < 2)
                Xlog("flashlight: %s aim: not written (turned quaternion norm %.3f)", who, n);
            return;
        }
        for (int i = 0; i < 4; i++) {
            q[i] = o[i] / n;
            wr[key][i] = q[i];
        }
    } else {                    // absolute: z axis = d, x = up x d, y = d x x (the light's local forward is +Z, g_fls_axv_l)
        float up[3] = { 0, 1, 0 }, x[3] =
            { up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2],
up[0] * d[1] - up[1] * d[0] };
        float xn =
            __builtin_sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (xn < 1e-3f) {
            x[0] = 1;
            x[1] = x[2] = 0;
            xn = 1;
        }
        for (int i = 0; i < 3; i++)
            x[i] /= xn;
        float y[3] =
            { d[1] * x[2] - d[2] * x[1], d[2] * x[0] - d[0] * x[2],
       d[0] * x[1] - d[1] * x[0] };
        float m00 = x[0], m11 = y[1], m22 = d[2], tr =
            m00 + m11 + m22, o[4];
        if (tr > 0) {
            float s = __builtin_sqrtf(tr + 1.0f) * 2;
            o[3] = 0.25f * s;
            o[0] = (y[2] - d[1]) / s;
            o[1] = (d[0] - x[2]) / s;
            o[2] = (x[1] - y[0]) / s;
        } else if (m00 > m11 && m00 > m22) {
            float s = __builtin_sqrtf(1.0f + m00 - m11 - m22) * 2;
            o[3] = (y[2] - d[1]) / s;
            o[0] = 0.25f * s;
            o[1] = (y[0] + x[1]) / s;
            o[2] = (d[0] + x[2]) / s;
        } else if (m11 > m22) {
            float s = __builtin_sqrtf(1.0f + m11 - m00 - m22) * 2;
            o[3] = (d[0] - x[2]) / s;
            o[0] = (y[0] + x[1]) / s;
            o[1] = 0.25f * s;
            o[2] = (d[1] + y[2]) / s;
        } else {
            float s = __builtin_sqrtf(1.0f + m22 - m00 - m11) * 2;
            o[3] = (x[1] - y[0]) / s;
            o[0] = (d[0] + x[2]) / s;
            o[1] = (d[1] + y[2]) / s;
            o[2] = 0.25f * s;
        }
        float n =
            __builtin_sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] +
                            o[3] * o[3]);
        if (!(n > 0.9f && n < 1.1f))
            return;
        for (int i = 0; i < 4; i++) {
            q[i] = o[i] / n;
            wr[key][i] = q[i];
        }
        if (why[key]++ < 2)
            Xlog("flashlight: %s aim: light orientation quaternion was not a unit quaternion (norm^2 %.3f) - absolute orientation written", who, qn0);
    }
    float *pp = (float *) (fo + 0x130);
    pp[0] = pos[0];
    pp[1] = pos[1];
    pp[2] = pos[2];
    if (!wrok[key]) {
        wrok[key] = 1;
        Xlog("flashlight: %s light placed at %.2f %.2f %.2f, forward turned %.1f deg onto %.2f %.2f %.2f", who, pos[0], pos[1], pos[2], __builtin_acosf(dot > 1 ? 1 : dot < -1 ? -1 : dot) * 57.2958f, d[0], d[1], d[2]);
    }
}

// OWN light = SP's: the light stays on the strap joint (r_clavicle) and points at the camera's aim point (camera + 30 m along the reticle forward; no ray cast used), not along the camera.
static void FlsOwnAim(uint8_t *own_fo)
{
    // a flashlight shone to the left (on the owner's screen only, fine on a bystander): log - our own pool entry (entry 0) is
    // never written by us, its direction is the engine controller's (5..19 deg off the camera while turning, settling), but this function still wrote the own
    // container's locator +0x130 / +0x140 every frame with an orientation built from the calibrated light axis - and the local renderer draws the own beam from
    // that locator. The own light is now left 100% to the engine's player flashlight controller (0x140b9e6a0), as in SP; t2r-fls-own-aim.txt = old write (test).
    {
        static uint64_t at;
        static int on;
        if (!FeelFlag("t2r-fls-own-aim.txt", &at, &on, 120))
            return;
    }
    if (!own_fo)
        return;
    uint64_t none0 = 0, cam =
        DcCall(0x42413e1c037969a8ULL /* camera-through-reticle-locator */
                , 0, &none0);
    uint64_t cp = cam
        && cam != ~0ULL ? DcCall(FNV64("get-locator-position"), 1,
                                  &cam) : 0;
    float f[3], sp[3], sq[4];
    if (!cp || cp == ~0ULL || IsBadReadPtr((void *) cp, 12)
        || !FlsCamFwd(f) || !XbxLoc(SID_PLAYER, "r_clavicle", sp, sq))
        return;
    float c[3];
    memcpy(c, (void *) cp, 12);
    if (!__builtin_isfinite(c[0] + c[1] + c[2])
        || __builtin_fabsf(c[0]) > 1e5f)
        return;
    float d[3] =
        { c[0] + f[0] * 30.0f - sp[0], c[1] + f[1] * 30.0f - sp[1],
   c[2] + f[2] * 30.0f - sp[2] };
    float n = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1.0f)
        return;
    d[0] /= n;
    d[1] /= n;
    d[2] /= n;
    FlsLightSet(T2R_MAX_PEERS, own_fo, sp, d, "own");
}

// PUPPET: the head joint's world rotation carries the synced look (yaw AND pitch). Its local forward axis differs per skeleton / character (our own-body calibration came out opposite
// signs), so it is measured per puppet body: the mean over 90 frames of (root's horizontal facing in head space) - the look swings about the facing, so the mean is the head's rest forward.
// Light direction = head rotation applied to that axis; position = head joint + 0.12 m along it. Angles vs the root facing are logged every 2 s.
static void FlsAimApply(uint32_t s, uint64_t body, uint8_t *fo)
{
    static float sum[T2R_MAX_PEERS][3], ax[T2R_MAX_PEERS][3];
    static int cnt[T2R_MAX_PEERS];
    static uint64_t body_of[T2R_MAX_PEERS], log_ms[T2R_MAX_PEERS];
    float hp[3], hq[4], rp[3], rq[4];
    uint64_t tnow = GetTickCount64();
    if (g_fls_rem[s].have && tnow - g_fls_rem[s].aim_ts <= FLS_AIM_STALE_MS) {  // the player's camera forward (replicated), smoothed; the source stays on the strap joint
        float sp[3], sq[4], *sm = g_fls_rem[s].sm, *tg = g_fls_rem[s].dir;
        for (int i = 0; i < 3; i++)
            sm[i] += (tg[i] - sm[i]) * 0.25f;   // per tick (every frame): lerp + renormalise = slerp for these small steps
        float m =
            __builtin_sqrtf(sm[0] * sm[0] + sm[1] * sm[1] + sm[2] * sm[2]);
        if (m > 0.2f && XbxLoc(body, "r_clavicle", sp, sq)) {
            float d2[3] = { sm[0] / m, sm[1] / m, sm[2] / m };
            char who2[16];
            snprintf(who2, sizeof who2, "slot %u", s);
            FlsLightSet((int) s, fo, sp, d2, who2);
            return;
        }
    }
    if (!XbxLoc(body, "head", hp, hq) || !XbxLoc(body, "root", rp, rq))
        return;
    float z[3] = { 0, 0, 1 }, rf[3];
    Qrotv(rq, z, rf);
    rf[1] = 0;
    float n = __builtin_sqrtf(rf[0] * rf[0] + rf[2] * rf[2]);
    if (n < 0.3f)
        return;
    rf[0] /= n;
    rf[2] /= n;
    if (body_of[s] != body) {
        body_of[s] = body;
        cnt[s] = 0;
        memset(sum[s], 0, 12);
    }
    if (cnt[s] < 90) {
        float l[3];
        FlsQinvRot(hq, rf, l);
        for (int i = 0; i < 3; i++)
            sum[s][i] += l[i];
        if (++cnt[s] == 90) {
            float m =
                __builtin_sqrtf(sum[s][0] * sum[s][0] +
                                sum[s][1] * sum[s][1] +
                                sum[s][2] * sum[s][2]);
            if (m < 30.0f) {
                cnt[s] = 0;
                memset(sum[s], 0, 12);
                return;
            }                   // the head turned about all over the place: measure again
            for (int i = 0; i < 3; i++)
                ax[s][i] = sum[s][i] / m;
            Xlog("flashlight: slot %u head forward axis (head space) %.2f %.2f %.2f (mean of %d frames, |mean| %.2f)", s, ax[s][0], ax[s][1], ax[s][2], 90, m / 90.0f);
        }
        return;
    }
    float d[3];
    Qrotv(hq, ax[s], d);
    float pos[3] =
        { hp[0] + d[0] * 0.12f, hp[1] + d[1] * 0.12f,
 hp[2] + d[2] * 0.12f };
    uint64_t now = GetTickCount64();
    if (now - log_ms[s] >= 2000) {
        log_ms[s] = now;
        float yaw =
            __builtin_atan2f(d[0], d[2]) - __builtin_atan2f(rf[0], rf[2]);
        while (yaw > 3.14159f)
            yaw -= 6.28318f;
        while (yaw < -3.14159f)
            yaw += 6.28318f;
        float pitch =
            __builtin_asinf(d[1] > 1 ? 1 : d[1] < -1 ? -1 : d[1]);
        Xlog("flashlight: slot %u aim: look yaw %.0f deg from the root facing, pitch %.0f deg%s", s, yaw * 57.2958f, pitch * 57.2958f, __builtin_fabsf(yaw) > 1.0472f ? " (OUTSIDE +-60: axis suspect)" : "");
    }
    char who[16];
    snprintf(who, sizeof who, "slot %u", s);
    FlsLightSet((int) s, fo, pos, d, who);
}

// The flashlight was still not synced to the camera from the bystander view (one player lights the ceiling, the other sees no beam move): the aim was written
// into the POOLED Flashlight process (job 249690b68d4f7013 'Flashlight', the pool entry's holder) - its +0x130 / +0x140 are no transform (A log: quaternion norm^2 0.000
// at the first write, then never touched by the engine), so nothing moved. Our own light is aimed through the FlashlightContainer process (player-get-flashlight ->
// type c39284766914189e 'FlashlightContainer', +0x130 position / +0x140 unit quaternion = its locator, B's dump [13838]) and that works: the puppet's container is the
// same class (kind 4 spawn 0x140aac560) - aim it exactly like ours, every frame. The pooled light still has to exist (lit check).
static void FlsAimTick(void)
{
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        if (!g_fls_pup[s].lit)
            continue;
        uint8_t *c = FlsContGet(s);
        if (!c || FlsPoolFind(c, NULL, NULL) < 0)
            continue;
        FlsAimApply(s, g_fls_pup[s].body, c);
    }
}

static void FlsPupOff(uint32_t s, const char *why)
{
    if (g_fls_pup[s].lit) {
        uint8_t *c = FlsContGet(s);
        if (c && FlsPoolFind(c, NULL, NULL) >= 0)
            FlsSeton(c, 0, 1);
        Xlog("flashlight: slot %u puppet light off (%s)", s, why);
    }
    g_fls_pup[s].lit = 0;
}

static int FlsPupOn(void)
{
    static uint64_t at;
    static int on;
    return FeelFlag("t2r-flashlight-puppets.txt", &at, &on, 120);
}

static void FlsPuppetsTick(uint64_t now)
{
    if (!FlsPupOn()) {
        static int once;
        if (!once) {
            once = 1;
            Xlog("flashlight: puppet lights / strap models off (dev opt-in t2r-flashlight-puppets.txt: engine container spawn, unverified in game)");
        }
        return;
    }
    float mb[16];
    const float *me = PlayerLocator(mb);
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *pe = SkPeerPtr(s);
        uint64_t body = pe ? SkPeerBody(s) : 0;
        uint8_t *proc = body ? (uint8_t *) FlsProc(body) : NULL;
        // a new body (another handle, or another process id behind the same handle): the old container does NOT go with it (log: pool 0 -> 2 -> 3 -> 4, strap models left standing
        // in the air where each body spawned) - destroy it, then spawn on the new body. Checked every tick, so a body swapped under the same handle is caught too.
        if (g_fls_pup[s].spawned
            && (g_fls_pup[s].body != body
                || (proc && !IsBadReadPtr(proc, 0x98)
                    && g_fls_pup[s].owner_id
                    && *(uint32_t *) (proc + 0x94) !=
                    g_fls_pup[s].owner_id))) {
            FlsContKill(s, "its body was replaced");
            memset(&g_fls_pup[s], 0, sizeof g_fls_pup[s]);
        }
        int valid = pe && pe[4] && !pe[5] && s != SkMe() && proc
            && !IsBadReadPtr(proc, 0x1a0)
            && *(uint32_t *) (proc + 0x94) != 0 && !g_puppetDeadHere[s];
        if (!valid) {
            if (g_fls_pup[s].lit)
                FlsPupOff(s, "puppet gone / dead");
            continue;
        }
        float pb[16];
        const float *pp = SkPeerPos(s, pb);
        if (!pp || !me)
            continue;
        float dx = pp[0] - me[0], dy = pp[1] - me[1], dz =
            pp[2] - me[2], d =
            __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        if (!g_fls_pup[s].spawned) {
            if (d > 2.0f * FLS_RANGE || now < g_flsOwnSpawnMs + 3000)
                continue;       // everyone gets the strap model; not while our own spawn is in flight
            uint64_t def = FlsDefPick();
            g_fls_pup[s].spawned = 1;
            g_fls_pup[s].body = body;
            g_fls_pup[s].spawn_ms = now;
            g_fls_pup[s].checked = 0;
            if (!def) {
                Xlog("flashlight: slot %u no flashlight define record exists - nothing spawned", s);
                continue;
            }
            uint8_t *c = FlsContainerSpawn(s, proc, def);
            g_fls_pup[s].owner_id = *(uint32_t *) (proc + 0x94);
            if (c) {
                g_fls_pup[s].cont = (uint64_t) c;
                g_fls_pup[s].cont_holder = *(uint64_t *) (c + 0x80);
                g_fls_pup[s].cont_id = *(uint32_t *) (c + 0x94);
            }
            continue;
        }
        uint8_t *c = FlsContGet(s);
        if (!c) {               // the container is gone (killed with a level piece / never made): spawn it again, at most 3 times per body
            if (g_fls_pup[s].cont && now - g_fls_pup[s].spawn_ms > 3000
                && g_fls_pup[s].retries < 3) {
                uint8_t r = (uint8_t) (g_fls_pup[s].retries + 1);
                Xlog("flashlight: slot %u container %llx id %u no longer resolves - spawning it again (%u/3)", s, (unsigned long long) g_fls_pup[s].cont_holder, g_fls_pup[s].cont_id, r);
                memset(&g_fls_pup[s], 0, sizeof g_fls_pup[s]);
                g_fls_pup[s].retries = r;
            }
            continue;
        }
        int on = 0;
        uint8_t *fo = NULL;
        int idx = FlsPoolFind(c, &on, &fo);
        if (idx < 0) {
            if (!g_fls_pup[s].checked
                && now - g_fls_pup[s].spawn_ms > 2000) {
                g_fls_pup[s].checked = 1;
                Xlog("flashlight: slot %u no pooled Flashlight owned by the container %p 2 s after the spawn (pool holds %d)", s, (void *) c, FlsPoolCount());
                if (g_fls_pup[s].retries < 3) {
                    uint8_t r = (uint8_t) (g_fls_pup[s].retries + 1);
                    FlsContKill(s, "no light came with it");
                    memset(&g_fls_pup[s], 0, sizeof g_fls_pup[s]);
                    g_fls_pup[s].retries = r;
                }
            }
            continue;
        }
        if (!g_fls_pup[s].checked) {
            g_fls_pup[s].checked = 1;
            Xlog("flashlight: slot %u pooled Flashlight %p in pool slot %d owned by container %p (%d ms after the spawn), on bit %d", s, (void *) fo, idx, (void *) c, (int) (now - g_fls_pup[s].spawn_ms), on);
            FlsHexdump("puppet flashlight object +0x00", fo, 0x60);
            if (!IsBadReadPtr(c, 0x160))
                FlsHexdump
                    ("puppet flashlight CONTAINER +0x100 (+0x130 pos, +0x140 quat = the aimed locator)",
                     c + 0x100, 0x60);
        }
        int want = g_fls_rem[s].on && now - g_fls_rem[s].ts <= FLS_STALE_MS
            && !g_peerDowned[s] && d <= FLS_RANGE;
        if (want && (!on || now - g_fls_pup[s].on_ms > 4000)) {
            FlsSeton(c, 1, 0);
            int on2 = 0;
            FlsPoolFind(c, &on2, NULL);
            if (!g_fls_pup[s].lit || !on)
                Xlog("flashlight: slot %u puppet SetOn(1): on bit %d -> %d", s, on, on2);
            g_fls_pup[s].lit = 1;
            g_fls_pup[s].on_ms = now;
        } else if (!want && (g_fls_pup[s].lit || on)) {
            FlsSeton(c, 0, 1);
            Xlog("flashlight: slot %u puppet SetOn(0) (remote off / stale / downed / far)", s);
            g_fls_pup[s].lit = 0;
        }
    }
}

static uint64_t FlsSpawnWith(uint64_t target, uint64_t sid)
{                               // send-event(spawn-flashlight, target, new-boxed-value(7 = symbol, sid))
    uint64_t bx[2] = { 7, sid };
    uint64_t box = DcCall(FNV64("new-boxed-value"), 2, bx);
    if (!box || box == ~0ULL)
        return ~0ULL;
    uint64_t a[3] = { FNV64("spawn-flashlight"), target, box };
    return DcCall(FNV64("send-event"), 3, a);
}

// ---- FOG GLOW (the player's flashlight lit up the fog between players in the winter Supermarket scoped view, which made enemies hard to see).
// The volumetric fog in-scatters every runtime light: render setting new-volumetrics:run-time-lights:player-flashlight-mult scales that for the PLAYER's flashlight only
// (other players' lights are untouched, so their cone still shows in fog), new-volumetrics:fog-run-time-light-contribution (live 1.0 in the report log) is the global
// scale for all runtime lights. Surface lighting is not touched. Written with change-render-setting-value-float into the always-live pat regions, re-asserted every 2 s
// (a region blend / level load restores the file value); the read-back is logged ("flashlight: fogfix"). Value: t2r-fls-fogmult.txt (float 0..1, default 0.05);
// t2r-no-fls-fogfix.txt = off. Fallback when the mult key is unreadable in this build: the global key at 0.35.
static void FlsFogfixTick(uint64_t now)
{
    static uint64_t at, off_at, nxt;
    static int off, n;
    if (FeelFlag("t2r-no-fls-fogfix.txt", &off_at, &off, 120))
        return;
    if (now < nxt)
        return;
    nxt = now + 2000;
    static const char *const SYM[] =
        { "*pat-rs-pat*", "*pat-rs-pat-cloudy*", "*pat-rs-pat-snow-accum*",
"*pat-rs-pat-more-sunfog*", "*pat-rs-pat-supermarket-flashlight*",
        "*pat-rs-pat-supermarket-main-bld*", "*pat-rs-pat-forest-blizzard*"
    };
    float v = 0.05f;
    (void) at;
    FILE *f = ExeSideOpen("t2r-fls-fogmult.txt", "r");
    if (f) {
        float t;
        if (fscanf(f, "%f", &t) == 1 && t >= 0.0f && t <= 1.0f)
            v = t;
        fclose(f);
    }
    uint64_t kk =
        FNV64("new-volumetrics:run-time-lights:player-flashlight-mult"),
        kg = FNV64("new-volumetrics:fog-run-time-light-contribution");
    uint64_t fn = FNV64("change-render-setting-value-float"), gf =
        FNV64("get-render-setting-value-float");
    uint64_t rd = DcCall(gf, 1, &kk);
    uint32_t ru = (uint32_t) rd;
    float cur;
    memcpy(&cur, &ru, 4);
    int have = rd != ~0ULL && cur == cur;
    float gv = have ? 1.0f : 0.35f;
    uint32_t vb, gb;
    memcpy(&vb, &v, 4);
    memcpy(&gb, &gv, 4);
    uint64_t r0 = 0, r1 = 0;
    for (unsigned i = 0; i < sizeof SYM / sizeof *SYM; i++) {
        uint64_t g = FNV64(SYM[i]);
        uint64_t a[3] = { g, kk, vb }, b[3] = { g, kg, gb };
        uint64_t x = DcCall(fn, 3, a);
        if (i == 0)
            r0 = x;
        if (!have) {
            uint64_t y = DcCall(fn, 3, b);
            if (i == 0)
                r1 = y;
        }
    }
    uint64_t rd2 = DcCall(gf, 1, &kk);
    uint32_t ru2 = (uint32_t) rd2;
    float after;
    memcpy(&after, &ru2, 4);
    uint64_t rg = DcCall(gf, 1, &kg);
    uint32_t rgu = (uint32_t) rg;
    float gnow;
    memcpy(&gnow, &rgu, 4);
    if (n < 8 || (n < 40 && fabsf(after - v) > 0.01f && (n % 10) == 0)) {
        n++;
        Xlog("flashlight: fogfix player-flashlight-mult %s %g -> want %g, read-back %s%g (change -> %llx); fog-run-time-light-contribution now %g%s", have ? "was" : "UNREADABLE", have ? (double) cur : 0.0, (double) v, rd2 == ~0ULL ? "unreadable " : "", rd2 == ~0ULL ? 0.0 : (double) after, (unsigned long long) r0, rg == ~0ULL ? 0.0 : (double) gnow, have ? "" : " (fallback: global key written)");
        (void) r1;
    }
}

// OWN STRAP MODEL + FlashlightContainer (the flashlight models were missing on the backpack straps): in SP the player's Flashlight container (the
// model look flashlight-l-skinny / -military on attach flashlightAttach = r_clavicle, chosen by the player event handler 0x140b750d0 from player vfunc +0xb18, plus the light)
// is spawned by the event spawn-flashlight as soon as the player owns the flashlight, so the model sits on the strap whether or not the light is on. MP only spawned it at the
// first toggle. Now: once the match runs and we are alive, player-get-flashlight 0 -> send-event(spawn-flashlight, player), re-checked every 2 s (a respawn drops the object),
// at most 8 tries per object-less stretch. The light stays off (winmm's per-frame turn-off-flashlight is not suppressed while the toggle is off).
static void FlsOwnSpawnTick(uint64_t now, int alive)
{
    static uint64_t next_ms, since_ms;
    static int tries, had;
    if (!alive) {
        since_ms = 0;
        return;
    }
    if (!since_ms)
        since_ms = now;
    if (now < since_ms + 2500 || now < next_ms)
        return;                 // let the spawn / intro settle
    next_ms = now + 2000;
    uint64_t n = 0, fh = DcCall(FNV64("player-get-flashlight"), 0, &n);
    if (fh && fh != ~0ULL) {
        if (!had) {
            had = 1;
            Xlog("flashlight: player flashlight object present (%llx) - strap model comes with it", (unsigned long long) fh);
        }
        tries = 0;
        return;
    }
    had = 0;
    if (tries >= 8)
        return;
    tries++;
    // the player handler takes the define from the event argument, else the engine global (exe+0x3f03b28, set by SP level scripts: 0 in MP): without a record the Flashlight
    // would get no light settings, so MP passes an existing define as the scripts' spawn-flashlight-with-settings does (boxed symbol argument).
    uint64_t g = *(volatile uint64_t *) (FlsExe() + 0x3f03b28), def =
        FlsDefPick(), sr;
    int gok = g && FlsDefExists(g);
    if (gok)
        sr = FlsEvent("spawn-flashlight");
    else if (def)
        sr = FlsSpawnWith(SID_PLAYER, def);
    else
        sr = FlsEvent("spawn-flashlight");
    g_flsOwnSpawnMs = now;
    Xlog("flashlight: no player flashlight object - spawn-flashlight(player%s) for the strap model + light (try %d; expect FlashlightContainer kind 0, look flashlight-l-skinny/-military, attach flashlightAttach) -> %llx", gok ? ", engine default define" : def ? ", explicit define" : "", tries, (unsigned long long) sr);
}

// LOCAL-LIGHT SHADOWS OF PUPPET BODIES (when a flashlight shone at a player, the cast shadows only showed their face and hands; SP: an NPC lit by Ellie's
// light casts a full body shadow). Static RE of tlou-ii.exe 1.6.10721 (no game run): every sub-draw (sub-mesh) of a draw control has a runtime flags dword
// (hash lookup exe 0x1403b9d60, keyed by the sub-mesh pointer). Bit 0x80 = "does not cast sun shadow", bit 0x800000 = "does not cast LOCAL (spot / omni / flashlight) shadow".
// They are written when a mesh is ADDED to the draw control (0x141480340: `dc+0x40 bit 6 set ? clear 0x80 : set 0x80`, `dc+0x40 bit 7 set ? clear 0x800000 : set 0x800000`), i.e. a
// look part added while the draw control's bit 7 is still 0 is born with 0x800000 and never enters a spot-light shadow map (the head / hand parts are added in a different
// phase and cast). The script natives set-instance-flag-cast-local-shadow / -cast-shadow (0x140423900 / 0x140423a90) are the engine's own switch: true = clear the bit on EVERY
// sub-draw of the object's draw control (0x141481380: and [flags], ~mask), false = set it (0x141486d20). So: once a second, a puppet body that has sub-draws with 0x800000 set gets
// set-instance-flag-cast-local-shadow(body, true). Every sub-draw count is logged (first 40 lines, "lshadow:") so the next A/B log proves it. Opt-out: t2r-no-lshadow.txt.
static int LshdOff(void)
{
    static uint64_t at;
    static int off;
    return FeelFlag("t2r-no-lshadow.txt", &at, &off, 120);
}

static void LshdCount(const uint8_t *dc, int *no_local, int *no_sun,
                       int *tot)
{
    *no_local = *no_sun = *tot = 0;
    if (!dc || IsBadReadPtr(dc, 0x18))
        return;
    uint32_t n = *(const uint32_t *) (dc + 0x14);
    const uint8_t *a = *(uint8_t * const *) (dc + 8);
    if (!n || n > 256 || !a || IsBadReadPtr(a, (size_t) n * 0x390))
        return;
    typedef uint32_t *(*fn_flags)(void *);
    fn_flags gf = (fn_flags) (FlsExe() + 0x3b9d60);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *in = a + (size_t) i * 0x390;
        uint32_t k = in[0x383];
        if (k > 19)
            k = 19;
        for (uint32_t j = 0; j < k; j++) {
            void *m = *(void *const *) (in + (size_t) j * 48);
            if (!m || IsBadReadPtr(m, 8))
                continue;
            uint32_t *f = gf(m);
            if (!f || IsBadReadPtr(f, 4))
                continue;
            (*tot)++;
            if (*f & 0x800000u)
                (*no_local)++;
            if (*f & 0x80u)
                (*no_sun)++;
        }
    }
}

static void LshdTick(uint64_t now)
{
    static uint64_t at;
    static unsigned nlog;
    if (LshdOff() || now - at < 1000)
        return;
    at = now;
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++) {
        uint8_t *pe = SkPeerPtr(s);
        uint64_t body = pe ? SkPeerBody(s) : 0;
        uint8_t *proc = body ? (uint8_t *) FlsProc(body) : NULL;
        if (!
            (pe && pe[4] && !pe[5] && s != SkMe() && proc
             && !IsBadReadPtr(proc, 0x1b8)
             && *(uint32_t *) (proc + 0x94) != 0
             && !g_puppetDeadHere[s]))
            continue;
        uint64_t vt = *(const uint64_t *) proc;
        if (vt < 0x100000000ULL
            || IsBadReadPtr((const void *) (uintptr_t) vt, 8))
            continue;
        const uint8_t *dc = *(uint8_t * const *) (proc + 0x1b0);
        int nl, ns, tot;
        LshdCount(dc, &nl, &ns, &tot);
        if (!tot || !nl)
            continue;
        uint64_t a[2] = { body, 1 };
        uint64_t r =
            DcCall(FNV64("set-instance-flag-cast-local-shadow"), 2, a);
        int nl2, ns2, tot2;
        LshdCount(dc, &nl2, &ns2, &tot2);
        if (nlog++ < 40)
            Xlog("lshadow: slot %u body %llx pid %u: %d of %d sub-draws had 'no local shadow' (0x800000; %d 'no sun shadow' 0x80) -> set-instance-flag-cast-local-shadow(true) = %llx -> %d of %d left", s, (unsigned long long) body, *(uint32_t *) (proc + 0x94), nl, tot, ns, (unsigned long long) r, nl2, tot2);
    }
}

static void FlsTick(void)
{
    static int was_in;
    uint64_t now = GetTickCount64();
    int in = MatchSubstate() == 6;
    if (FlsOff()) {
        if (g_fls.on)
            FlsSet(0, "opt-out file");
        else
            FlsWinmmGate(0);
        if (in)
            for (uint32_t s = 0; s < T2R_MAX_PEERS; s++)
                FlsPupOff(s, "opt-out file");
        return;
    }
    if (!in) {
        if (was_in) {           // the match is over: the objects went with the level
            int gated = g_fls.gated, bad = g_fls.bad_gate;
            memset(&g_fls, 0, sizeof g_fls);
            g_fls.gated = gated;
            g_fls.bad_gate = bad;
            memset(g_fls_pup, 0, sizeof g_fls_pup);
            memset(g_fls_rem, 0, sizeof g_fls_rem);
            FlsWinmmGate(0);
            Xlog("flashlight: left the match - state cleared");
        }
        was_in = 0;
        return;
    }
    was_in = 1;
    LshdTick(now);
    FlsFogfixTick(now);
    int alive = FlsAlive();
    if (g_fls.on && !alive)
        FlsSet(0, "down / dead");
    if (alive) {
        if (FlsToggleEdge(now))
            FlsSet(!g_fls.on, "toggle");
    } else {
        g_fls.hold_ms = 0;
        g_fls.hold_fired = 0;
    }
    if (g_fls.on) {
        if (now - g_fls.hb_ms >= FLS_AIM_MS) {
            g_fls.hb_ms = now;
            FlsBroadcast(1);
        }
        if (g_fls.asserts < 2 && now >= g_fls.assert_ms) {      // read the engine back: did the event light it?
            g_fls.assert_ms = now + 1000;
            uint64_t nn = 0, fh1 =
                DcCall(FNV64("player-get-flashlight"), 0, &nn);
            int u = (!fh1 || fh1 == ~0ULL) ? 0 : 1;
            if (u == 0) {
                uint64_t sr = FlsEvent("spawn-flashlight");
                Xlog("flashlight: still no player flashlight object - spawn-flashlight again -> %llx", (unsigned long long) sr);
            }
            if (u == 0) {
                g_fls.asserts++;
                uint64_t r = FlsEvent("turn-on-flashlight");
                Xlog("flashlight: engine reports no light after the event - turn-on-flashlight sent again (#%d) -> %llx", g_fls.asserts, (unsigned long long) r);
            } else {
                if (!g_fls.using_logged) {
                    g_fls.using_logged = 1;
                    Xlog("flashlight: engine reports using = %d (own light)", u);
                }
                g_fls.asserts = 2;
            }
        }
    }
    if (g_fls.on) {             // diagnostic for the "own light points left" report (seen on one client only): where the engine's light points, against the strap joint, once a second
        static uint64_t diag_ms;
        static unsigned dn;
        if (now - diag_ms >= 1000 && dn < 80) {
            diag_ms = now;
            uint64_t n0 = 0, fh =
                DcCall(FNV64("player-get-flashlight"), 0, &n0);
            uint8_t *fo = (fh
                           && fh !=
                           ~0ULL) ? (uint8_t *) FlsProc(fh) : NULL;
            float cp[3], cq[4], pp[3], pq[4];
            if (fo && !IsBadReadPtr(fo, 0x160)
                && XbxLoc(SID_PLAYER, "r_clavicle", cp, cq)
                && XbxLoc(SID_PLAYER, "pelvis", pp, pq)) {
                const float *q = (const float *) (fo + 0x140);
                float ax[3][3], u[3] = { 1, 0, 0 }, v[3] =
                    { 0, 1, 0 }, w[3] = { 0, 0, 1 }, pz[3];
                Qrotv(q, u, ax[0]);
                Qrotv(q, v, ax[1]);
                Qrotv(q, w, ax[2]);
                Qrotv(pq, w, pz);
                dn++;
                Xlog("flashlight: diag light at %.2f %.2f %.2f quat %.3f %.3f %.3f %.3f axes x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f) | r_clavicle %.2f %.2f %.2f quat %.3f %.3f %.3f %.3f | pelvis z-axis (%.2f %.2f %.2f)", *(float *) (fo + 0x130), *(float *) (fo + 0x134), *(float *) (fo + 0x138), q[0], q[1], q[2], q[3], ax[0][0], ax[0][1], ax[0][2], ax[1][0], ax[1][1], ax[1][2], ax[2][0], ax[2][1], ax[2][2], cp[0], cp[1], cp[2], cq[0], cq[1], cq[2], cq[3], pz[0], pz[1], pz[2]);
            }
        }
    }
    if (alive) {                // aim calibration from our own body / light; the puppets' aim override every frame
        uint64_t n1 = 0, fh1 =
            g_fls.on ? DcCall(FNV64("player-get-flashlight"), 0, &n1) : 0;
        FlsCalTick(fh1
                     && fh1 != ~0ULL ? (uint8_t *) FlsProc(fh1) : NULL);
    }
    if (g_fls.on && alive) {
        uint64_t n2 = 0, fh2 =
            DcCall(FNV64("player-get-flashlight"), 0, &n2);
        if (fh2 && fh2 != ~0ULL)
            FlsOwnAim((uint8_t *) FlsProc(fh2));
    }
    FlsAimTick();
    for (uint32_t s = 0; s < T2R_MAX_PEERS; s++)
        if (g_fls_pup[s].lit)
            FlsEntryDiag((int) s, FlsContGet(s), g_fls_pup[s].body,
                           g_fls_pup[s].on_ms, now);
    if (g_fls.on && alive) {
        uint64_t n3 = 0, fh3 =
            DcCall(FNV64("player-get-flashlight"), 0, &n3);
        if (fh3 && fh3 != ~0ULL && now - g_fls.on_ms <= 2000)
            FlsEntryDiag(T2R_MAX_PEERS, (uint8_t *) FlsProc(fh3),
                           SID_PLAYER, g_fls.on_ms, now);
    }
    FlsOwnSpawnTick(now, alive);
    static uint64_t pup_at;
    if (now - pup_at >= 100) {
        pup_at = now;
        FlsPuppetsTick(now);
    }
}
