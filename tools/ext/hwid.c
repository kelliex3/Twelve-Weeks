// HWID report (bans by hardware id). The client (loader/hwid.cpp) hashes MachineGuid, SMBIOS UUID, disk serials and MACs
// and sends them in its own matchmaker login; the shipped winmm sends none, so this sends an equivalent over the clan short connection:
//   op 0xB2 [64B token][u8 ver=1][u8 mask: bit0 machine guid, bit1 smbios uuid, bit2 first fixed disk serial][12B per present bit]
// each 12 B = first 12 bytes of SHA-256("T2R-HWID1" tag data) (the same sources as the client: MachineGuid, SMBIOS system UUID, disk serial;
// MACs left out: VPN / virtual adapters make them collide). Reply 0xB3 "ok=1": stop. A server that never answers (older) is left
// alone after 3 tries. No UI: the server decides (tools/daemon.py "HWID bans"). g_hwid_done / g_hwid_tries: t2r-ext.c (clan_worker).

typedef struct {
    uint32_t h[8];
    uint8_t b[64];
    uint64_t n;
    size_t k;
} HwSha;
static const uint32_t HW_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb,
        0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70,
        0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814,
        0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define HW_R(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void HwBlk(HwSha *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] =
            (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i +
                                                     1] << 16 | (uint32_t)
            p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 =
            HW_R(w[i - 15], 7) ^ HW_R(w[i - 15], 18) ^ (w[i - 15] >> 3),
            s1 =
            HW_R(w[i - 2], 17) ^ HW_R(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 =
            h + (HW_R(e, 6) ^ HW_R(e, 11) ^ HW_R(e, 25)) +
            ((e & f) ^ (~e & g)) + HW_K[i] + w[i];
        uint32_t t2 =
            (HW_R(a, 2) ^ HW_R(a, 13) ^ HW_R(a, 22)) +
            ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void HwInit(HwSha *s)
{
    static const uint32_t iv[8] =
        { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->n = 0;
    s->k = 0;
}

static void HwUpd(HwSha *s, const void *d, size_t len)
{
    const uint8_t *p = (const uint8_t *) d;
    s->n += len;
    while (len--) {
        s->b[s->k++] = *p++;
        if (s->k == 64) {
            HwBlk(s, s->b);
            s->k = 0;
        }
    }
}

static void HwFin(HwSha *s, uint8_t out[32])
{
    uint64_t bits = s->n * 8;
    uint8_t pad = 0x80, z = 0;
    HwUpd(s, &pad, 1);
    while (s->k != 56)
        HwUpd(s, &z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++)
        l[i] = (uint8_t) (bits >> (56 - 8 * i));
    HwUpd(s, l, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t) (s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t) (s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t) (s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t) s->h[i];
    }
}

static void HwComp(uint8_t tag, const void *d, size_t len,
                    uint8_t out[12])
{
    HwSha s;
    uint8_t h[32];
    HwInit(&s);
    HwUpd(&s, "T2R-HWID1", 9);
    HwUpd(&s, &tag, 1);
    HwUpd(&s, d, len);
    HwFin(&s, h);
    memcpy(out, h, 12);
}

static int HwMachineGuid(uint8_t out[12])
{
    typedef LONG(WINAPI * fn_rgv) (HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD,
                                   PVOID, LPDWORD);
    HMODULE a = LoadLibraryA("advapi32.dll");
    fn_rgv rgv = a ? (fn_rgv) GetProcAddress(a, "RegGetValueA") : NULL;
    char g[80] = { 0 };
    DWORD sz = sizeof g - 1;
    if (!rgv
        || rgv(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
               "MachineGuid", 0x2 /* RRF_RT_REG_SZ */ , NULL, g, &sz) != 0)
        return 0;
    size_t n = strnlen(g, sizeof g);
    if (!n)
        return 0;
    HwComp(1, g, n, out);
    return 1;
}

static int HwSmbiosUuid(uint8_t out[12])
{
    typedef UINT(WINAPI * fn_gsft) (DWORD, DWORD, PVOID, DWORD);
    fn_gsft gs =
        (fn_gsft) GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                 "GetSystemFirmwareTable");
    if (!gs)
        return 0;
    UINT sz = gs(0x52534D42 /* 'RSMB' */ , 0, NULL, 0);
    if (sz < 8 || sz > (1u << 20))
        return 0;
    uint8_t *b = (uint8_t *) malloc(sz);
    if (!b)
        return 0;
    int ok = 0;
    if (gs(0x52534D42, 0, b, sz) == sz) {
        uint32_t tl;
        memcpy(&tl, b + 4, 4);
        size_t end = 8 + (size_t) tl < sz ? 8 + (size_t) tl : sz, off = 8;
        while (off + 4 <= end) {
            uint8_t type = b[off], len = b[off + 1];
            if (len < 4)
                break;
            if (type == 1 && len >= 0x19 && off + 8 + 16 <= end) {
                const uint8_t *u = b + off + 8;
                int z0 = 1, ff = 1;
                for (int i = 0; i < 16; i++) {
                    z0 &= u[i] == 0;
                    ff &= u[i] == 0xFF;
                }
                if (!z0 && !ff) {
                    HwComp(2, u, 16, out);
                    ok = 1;
                }
                break;
            }
            size_t nx = off + len;
            while (nx + 1 < end && (b[nx] != 0 || b[nx + 1] != 0))
                nx++;
            off = nx + 2;
        }
    }
    free(b);
    return ok;
}

static int HwDiskSerial(uint8_t out[12])
{                               // smallest-sorting serial of the fixed (non USB / virtual) physical drives
    char best[128] = "";
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof path, "\\\\.\\PhysicalDrive%d", i);
        HANDLE h =
            CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        struct {
            DWORD id, type;
            BYTE ex[1];
        } q =
            { 0 /* StorageDeviceProperty */ ,
              0 /* PropertyStandardQuery */ , { 0 } };
        uint8_t buf[1024] = { 0 };
        DWORD ret = 0;
        BOOL ok =
            DeviceIoControl(h,
                            0x2D1400 /* IOCTL_STORAGE_QUERY_PROPERTY */ ,
                            &q, sizeof q, buf, sizeof buf, &ret, NULL);
        CloseHandle(h);
        if (!ok || ret < 36)
            continue;
        uint32_t so;
        memcpy(&so, buf + 24, 4);       // STORAGE_DEVICE_DESCRIPTOR: Version,Size,DeviceType(1),Modifier(1),Removable(1),CommandQueueing(1),
        uint32_t bus;
        memcpy(&bus, buf + 28, 4);      // VendorIdOffset,ProductIdOffset,ProductRevisionOffset,SerialNumberOffset(+24),BusType(+28)
        if (bus == 7 || bus == 14 || bus == 15 || so == 0
            || so >= sizeof buf - 1)
            continue;           // USB, virtual, file backed virtual
        char s[128];
        snprintf(s, sizeof s, "%.100s", (const char *) buf + so);
        char *p = s;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t n = strlen(p);
        while (n
               && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'
                   || p[n - 1] == '\n'))
            p[--n] = 0;
        if (n && (!best[0] || strcmp(p, best) < 0))
            snprintf(best, sizeof best, "%s", p);
    }
    if (!best[0])
        return 0;
    HwComp(3, best, strlen(best), out);
    return 1;
}

// the gather (registry, SMBIOS table, up to 16 CreateFile(PhysicalDriveN) + DeviceIoControl - a spun-down disk stalls for
// 100s of ms) ran on the game thread and, when clan_send failed (no token / busy), again EVERY frame. Now: ONE gather per session on a worker
// thread (started at the first tick, 6 s after boot), the result cached; the game thread only sends the cached 38 bytes, and a failed clan_send
// backs off 20 s (no per-frame retry; the tries counter only counts real sends, so a missing token is retried at that slow pace).
static volatile LONG g_hwState;        // 0 not started, 1 gathering, 2 done
static uint8_t g_hwBuf[2 + 36];
static int g_hwLen;
static volatile LONG g_hwRuns; // gather runs (diag: must stay 1)
static DWORD WINAPI HwGatherThread(LPVOID unused)
{
    (void) unused;
    uint8_t b[2 + 36] = { 1, 0 };
    int o = 2;
    if (HwMachineGuid(b + o)) {
        b[1] |= 1;
        o += 12;
    }
    if (HwSmbiosUuid(b + o)) {
        b[1] |= 2;
        o += 12;
    }
    if (HwDiskSerial(b + o)) {
        b[1] |= 4;
        o += 12;
    }
    memcpy(g_hwBuf, b, sizeof b);
    g_hwLen = b[1] ? o : 0;
    InterlockedIncrement(&g_hwRuns);
    InterlockedExchange(&g_hwState, 2);
    return 0;
}

static void HwidReportTick(void)
{
    static uint64_t next_at;
    uint64_t now = GetTickCount64();
    if (g_hwid_done || g_hwid_tries >= 3 || now < next_at)
        return;
    if (!next_at) {
        next_at = now + 6000;
        return;
    }
    if (g_hwState == 0) {
        InterlockedExchange(&g_hwState, 1);
        HANDLE t = CreateThread(NULL, 0, HwGatherThread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        else {
            g_hwid_tries = 3;
            Xlog("hwid: gather thread not started");
        }
        next_at = now + 500;
        return;
    }
    if (g_hwState != 2) {
        next_at = now + 500;
        return;
    }
    if (!g_hwLen) {
        g_hwid_tries = 3;
        Xlog("hwid: no source readable");
        return;
    }
    if (g_clan_busy) {
        next_at = now + 2000;
        return;
    }
    if (ClanSend(0xB2, g_hwBuf, g_hwLen)) {
        InterlockedIncrement(&g_hwid_tries);
        next_at = now + 15000;
        Xlog("hwid: reported %d component(s) (gather runs %ld)",
             __builtin_popcount(g_hwBuf[1]), (long) g_hwRuns);
    } else
        next_at = now + 20000;  // no token yet / send failed: slow retry, never per frame
}
