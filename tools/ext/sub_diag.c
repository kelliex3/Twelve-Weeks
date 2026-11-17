// ---------------------------------------------------------------- subtitle diagnostics (subtitles disappeared
// instantly instead of fading out). The subtitles are the engine's own (Subtitles line queue, exe 0x148d38010 + 0x20c78, mode
// byte +0x22ef9 = 1): voice line -> the per-voice subtitle updater FUN_141e32220 -> Subtitles::Show (14153ea30); a slot leaves the
// queue by (a) its end time running out in LineQueue::Update (141862b90 - the fade), (b) Subtitles::ClearUniqueId (14153bdf0, a HARD
// removal of every entry with that voice id; called when a voice instance is stopped / destroyed before it started) or (c)
// Subtitles::Clear (14153b920, level teardown). This logs who removes what, with the return address, plus the displayed slots'
// raw fields, so a log of one vox line says which path cuts it. t2r-sub-diag.txt next to the exe turns the slot dump on.
// (the hooks return the original's rax: ClearUniqueId ends in `mov al,1; ret` - a bool its (tail-)callers may test; a void hook would hand them garbage)
typedef uint64_t(*fn_sub_clear) (uint64_t, uint64_t);
typedef uint64_t(*fn_sub_show) (uint64_t, uint64_t *);
static fn_sub_clear g_sd_clear_tramp, g_sd_clearuid_tramp;
static fn_sub_show g_sdShowTramp;
static int g_sdN;
static void *SdHookExe(uint64_t rva, const uint8_t *expect, int n,
                         void *hook, const char *what)
{
    uint8_t *f = (uint8_t *) GetModuleHandleW(NULL) + rva;
    if (memcmp(f, expect, n)) {
        HOOK_MISS(f, n, "sub diag: %s prologue differs, not hooked", what);
        return NULL;
    }
    uint8_t *t =
        VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE);
    if (!t)
        return NULL;
    memcpy(t, f, n);
    t[n] = 0xFF;
    t[n + 1] = 0x25;
    *(uint32_t *) (t + n + 2) = 0;
    *(uint64_t *) (t + n + 6) = (uint64_t) (f + n);
    DWORD old;
    VirtualProtect(f, n, PAGE_EXECUTE_READWRITE, &old);
    f[0] = 0xFF;
    f[1] = 0x25;
    *(uint32_t *) (f + 2) = 0;
    *(uint64_t *) (f + 6) = (uint64_t) hook;
    for (int i = 14; i < n; i++)
        f[i] = 0x90;
    VirtualProtect(f, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, n);
    return t;
}

static void SdRet(void *ra, char *out, size_t n)
{
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    if ((uint8_t *) ra >= exe && (uint8_t *) ra < exe + 0x9000000)
        snprintf(out, n, "exe+%llx",
                 (unsigned long long) ((uint8_t *) ra - exe));
    else
        ModOff((uint64_t) ra, out, n);
}

static uint64_t SdClearHook(uint64_t self, uint64_t flag)
{
    char r[96];
    SdRet(__builtin_return_address(0), r, sizeof r);
    if (LOGCAP(g_sdN, 300))
        Xlog("sub diag: Subtitles::Clear(%llu) from %s",
             (unsigned long long) flag, r);
    return g_sd_clear_tramp(self, flag);
}

static uint64_t SdClearuidHook(uint64_t self, uint64_t id)
{
    char r[96];
    SdRet(__builtin_return_address(0), r, sizeof r);
    if (LOGCAP(g_sdN, 300))
        Xlog("sub diag: Subtitles::ClearUniqueId(%llx) from %s",
             (unsigned long long) id, r);
    return g_sd_clearuid_tramp(self, id);
}

static uint64_t SdShowHook(uint64_t self, uint64_t *p)
{
    char r[96];
    SdRet(__builtin_return_address(0), r, sizeof r);
    if (g_sdN++ < 300) {
        const char *tx = p[0xc]
            && !IsBadReadPtr((void *) p[0xc],
                             8) ? (const char *) p[0xc] : "";
        Xlog("sub diag: Subtitles::Show sid %llx uniqueId %llx flag35 %d flagD %d text '%.50s' from %s", (unsigned long long) p[0], (unsigned long long) p[0xb], ((uint8_t *) p)[0x35], ((uint8_t *) p)[0x68], tx, r);
    }
    return g_sdShowTramp(self, p);
}

static void InstallSubDiag(void)
{
    {                           // diagnostics only: hook the engine's subtitle functions only when t2r-sub-diag.txt is next to the exe (nothing patched in normal play)
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, sizeof p);
        char *sl = strrchr(p, 92);
        if (sl)
            strcpy(sl + 1, "t2r-sub-diag.txt");
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES)
            return;
    }
    static const uint8_t cl[14] =
        { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56,
0x57, 0x41, 0x56 };
    static const uint8_t cu[15] =
        { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48,
0x89, 0x74, 0x24, 0x20 };
    static const uint8_t sh[16] =
        { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
0x55, 0x41, 0x56, 0x41, 0x57 };
    g_sd_clear_tramp =
        (fn_sub_clear) SdHookExe(0x153B920, cl, 14,
                                   (void *) SdClearHook, "Clear");
    g_sd_clearuid_tramp =
        (fn_sub_clear) SdHookExe(0x153BDF0, cu, 15,
                                   (void *) SdClearuidHook,
                                   "ClearUniqueId");
    g_sdShowTramp =
        (fn_sub_show) SdHookExe(0x153EA30, sh, 16, (void *) SdShowHook,
                                  "Show");
    Xlog("sub diag: Subtitles Clear/ClearUniqueId/Show hooks %s/%s/%s",
         g_sd_clear_tramp ? "on" : "-", g_sd_clearuid_tramp ? "on" : "-",
         g_sdShowTramp ? "on" : "-");
}

// the displayed slots (2 x 0xd8 at queue + 0xe00) and the queued lines: logged when a slot appears / goes / every 150 ms raw
static void FeatureSubDiag(void)
{
    static int on = -1;
    static uint64_t last_slot[2], t_dump[2];
    static int last_q = -1, last_d = -1, logged;
    uint8_t *exe = (uint8_t *) GetModuleHandleW(NULL);
    if (on < 0) {
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, sizeof p);
        char *s = strrchr(p, 92);
        if (s)
            strcpy(s + 1, "t2r-sub-diag.txt");
        on = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        Xlog("sub diag: slot dump %s (file t2r-sub-diag.txt next to the exe turns it on)", on ? "on" : "off");
    }
    if (!on || logged > 1500)
        return;
    uint8_t *g = exe + 0x8D38010 + 0x20C78;
    if (IsBadReadPtr(g, 0xfe0))
        return;
    int q = *(int *) (g + 0xfb8), d = *(int *) (g + 0xfbc);
    if (q != last_q || d != last_d) {
        Xlog("sub diag: queued %d displayed %d (mode byte %d)", q, d,
             exe[0x8D5AF09]);
        last_q = q;
        last_d = d;
        logged++;
    }
    for (int i = 0; i < 2; i++) {
        uint8_t *e = g + 0xe00 + i * 0xd8;
        uint64_t id = *(uint64_t *) e, now = GetTickCount64();
        if (id != last_slot[i]) {
            Xlog("sub diag: slot %d %s id %llx", i, id ? "SHOWN" : "GONE",
                 (unsigned long long) (id ? id : last_slot[i]));
            logged++;
            if (id)
                t_dump[i] = 0;
            last_slot[i] = id;
        }
        if (id && now - t_dump[i] >= 150) {
            t_dump[i] = now;
            char b[0xd8 * 2 + 1];
            for (int k = 0; k < 0xd8; k++)
                sprintf(b + k * 2, "%02x", e[k]);
            Xlog("sub diag: slot %d raw %s", i, b);
            logged++;
        }
    }
}
