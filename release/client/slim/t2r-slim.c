// t2r-slim: rebuilds the full winmm.dll of a "slim" Twelve Weeks client from the player's own game files.
//
//   t2r-slim.exe <slim winmm.dll> <t2r-slim.dat> <game folder> <out winmm.dll>
//
// The slim DLL is the normal client DLL whose embedded mod archive (T2ME blob: XOR-encrypted DSAR of lz4 chunks around a
// PSARC) has the data chunks of some files zeroed out: those files are edited copies of files the game ships. The .dat
// holds, per such file, a zstd patch against the game's original (--patch-from) and the list of its DSAR chunks
// (payload offset, raw size, compressed size). For every file this tool
//   1. reads the original out of <game>\build\pc\main\<archive>.psarc (DSAR lz4/stored chunks around a PSARC with zlib
//      or stored blocks),
//   2. applies the patch (zstd also checks the checksum of the result: wrong game version -> clear error),
//   3. cuts the result into 256 KiB chunks, lz4-HC (level 12, the same call the build used) compresses each one, checks
//      the size against the .dat, XORs the keystream (SplitMix64 per qword of the payload) and writes it into the copy,
// and finally recomputes the FNV-1a-64 of the whole payload and compares it to the hash in the T2ME header. The output
// is written to <out>.tmp and renamed only when everything checked out; it is byte-identical to the unslimmed DLL.
// T2SLIM02: the files the game does not have (TLOU Part I ports, menu images ...) are no longer left in the DLL as lz4:
// they travel as ONE solid xz stream inside the .dat (about 40 % smaller than lz4 + XOR, which nothing can compress), are decoded
// in order here and go through the same lz4-HC / XOR / write path. The lz4 work of a file runs on all cores (T2R_SLIM_THREADS).
// Read-only on the game folder. Exit codes: 0 ok, 1 usage / io, 2 game files differ from the pinned version, 3 internal.
//
// Build (MSYS2 UCRT64): gcc -O2 -s -static -municode -o t2r-slim.exe t2r-slim.c -lzstd -llz4 -llzma -lz
#include "t2r-slim.h"

static void die(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "t2r-slim: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (g_fo)
        fclose(g_fo);
    if (g_tmp[0]) {
        _wremove(g_tmp);
    }                           // never leave a half-written copy behind
    exit(code);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die(1, "out of memory (%llu bytes)", (unsigned long long) n);
    return p;
}

static u64 rd64(const u8 *p)
{
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

static u32 rd32(const u8 *p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static u32 be32(const u8 *p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static u64 be40(const u8 *p)
{
    return (u64) p[0] << 32 | be32(p + 1);
}

static int seek64(FILE *f, u64 off)
{
    return _fseeki64(f, (long long) off, SEEK_SET);
}

// ------------------------------------------------------------------ hashes / keystream (the T2ME layer)
static u64 fnv_payload(const u8 *p, size_t n, u64 h)
{                               // streamed: qword aligned pieces, tail bytes last
    size_t nq = n / 8, i;
    for (i = 0; i < nq; i++) {
        u64 v;
        memcpy(&v, p + i * 8, 8);
        h = (h ^ v) * 0x100000001B3ULL;
    }
    for (i = nq * 8; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ULL;
    return h;
}

static u64 ks_word(u64 i)
{
    u64 x = i * 0x9E3779B97F4A7C15ULL + 0xED9AE8F47523C0FBULL;
    x ^= 0xECBDD9B9C0033EFFULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static u64 g_ks_end;            // only whole qwords of the payload are encrypted: the tail bytes are plain

static void ks_xor(u8 *buf, size_t n, u64 payload_off)
{                               // payload byte offset of buf[0]
    for (size_t k = 0; k < n; k++) {
        u64 o = payload_off + k;
        if (o >= g_ks_end)
            break;
        buf[k] ^= (u8) (ks_word(o >> 3) >> ((o & 7) * 8));
    }
}

// ------------------------------------------------------------------ the game's archives: DSAR (lz4 chunks) around a PSARC

static Dsar *dsar_open(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return NULL;
    u8 h[0x28];
    if (fread(h, 1, 0x28, f) != 0x28 || memcmp(h, "DSAR", 4)) {
        fclose(f);
        return NULL;
    }
    Dsar *d = xmalloc(sizeof *d);
    memset(d, 0, sizeof *d);
    d->f = f;
    d->n = rd32(h + 8);
    d->total = rd64(h + 16);
    u8 *dir = xmalloc((size_t) d->n * 0x20);
    if (fread(dir, 1, (size_t) d->n * 0x20, f) != (size_t) d->n * 0x20) {
        fclose(f);
        return NULL;
    }
    d->off = xmalloc(d->n * 8);
    d->ustart = xmalloc(d->n * 8);
    d->usz = xmalloc(d->n * 4);
    d->csz = xmalloc(d->n * 4);
    u64 u = 0;
    for (u32 i = 0; i < d->n; i++) {
        d->off[i] = rd64(dir + i * 0x20);
        d->usz[i] = rd32(dir + i * 0x20 + 8);
        d->csz[i] = rd32(dir + i * 0x20 + 12);
        d->ustart[i] = u;
        u += d->usz[i];
    }
    free(dir);
    d->cidx = -1;
    return d;
}

static int dsar_chunk(Dsar *d, u32 i)
{
    if (d->cidx == (long) i)
        return 1;
    u32 usz = d->usz[i], csz = d->csz[i];
    if (usz > d->ccap) {
        d->cdata = realloc(d->cdata, usz);
        d->ccap = usz;
        if (!d->cdata)
            die(1, "out of memory");
    }
    d->cidx = -1;
    if (csz == 0)
        memset(d->cdata, 0, usz);
    else if (csz == usz) {
        if (seek64(d->f, d->off[i])
            || fread(d->cdata, 1, usz, d->f) != usz)
            return 0;
    } else {
        if (csz > d->bcap) {
            d->cbuf = realloc(d->cbuf, csz);
            d->bcap = csz;
            if (!d->cbuf)
                die(1, "out of memory");
        }
        if (seek64(d->f, d->off[i]) || fread(d->cbuf, 1, csz, d->f) != csz)
            return 0;
        if (LZ4_decompress_safe
            ((const char *) d->cbuf, (char *) d->cdata, (int) csz,
             (int) usz) != (int) usz)
            return 0;
    }
    d->cidx = (long) i;
    return 1;
}

static int dsar_read(Dsar *d, u64 pos, size_t n, u8 *out)
{
    size_t got = 0;
    while (got < n) {
        u64 p = pos + got;
        long i = d->cidx;
        if (i < 0 || p < d->ustart[i] || p >= d->ustart[i] + d->usz[i]) {       // binary search
            u32 lo = 0, hi = d->n;
            while (hi - lo > 1) {
                u32 m = (lo + hi) / 2;
                if (d->ustart[m] <= p)
                    lo = m;
                else
                    hi = m;
            }
            i = lo;
            if (p >= d->ustart[i] + d->usz[i])
                return 0;
        }
        if (!dsar_chunk(d, (u32) i))
            return 0;
        size_t a = (size_t) (p - d->ustart[i]), take = d->usz[i] - a;
        if (take > n - got)
            take = n - got;
        memcpy(out + got, d->cdata + a, take);
        got += take;
    }
    return 1;
}

static void dsar_close(Dsar *d)
{
    fclose(d->f);
    free(d->off);
    free(d->ustart);
    free(d->usz);
    free(d->csz);
    free(d->cdata);
    free(d->cbuf);
    free(d);
}

// one file out of a game PSARC (looked up by the MD5 of its path); returns a malloc'd buffer of *usz bytes, NULL + msg on failure
static u8 *psarc_file(Dsar *d, const u8 md5[16], u64 *usz_out,
                      const char **err)
{
    u8 hd[32];
    if (!dsar_read(d, 0, 32, hd) || memcmp(hd, "PSAR", 4)) {
        *err = "not a PSARC";
        return NULL;
    }
    if (memcmp(hd + 8, "zlib", 4)) {
        *err = "PSARC compression is not zlib";
        return NULL;
    }
    u32 toc_len = be32(hd + 12), esz = be32(hd + 16), n =
        be32(hd + 20), bsize = be32(hd + 24);
    if (esz != 30 || toc_len > (64u << 20)) {
        *err = "unexpected PSARC layout";
        return NULL;
    }
    u8 *toc = xmalloc(toc_len);
    if (!dsar_read(d, 0, toc_len, toc)) {
        free(toc);
        *err = "cannot read the PSARC table of contents";
        return NULL;
    }
    u32 bt = 32 + n * 30, width =
        bsize <= 0x10000 ? 2 : (bsize <= 0x1000000 ? 3 : 4);
    u32 ent = 0;
    for (u32 i = 1; i < n; i++)
        if (!memcmp(toc + 32 + i * 30, md5, 16)) {
            ent = i;
            break;
        }
    if (!ent) {
        free(toc);
        *err = "file not found in the game archive";
        return NULL;
    }
    const u8 *e = toc + 32 + ent * 30;
    u32 bi = be32(e + 16);
    u64 usz = be40(e + 20), off = be40(e + 25);
    u8 *out = xmalloc(usz);
    u8 *blk = xmalloc(bsize + 16);
    u64 got = 0;
    while (got < usz) {
        u32 cs = 0;
        const u8 *b = toc + bt + (size_t) bi * width;
        for (u32 k = 0; k < width; k++)
            cs = cs << 8 | b[k];
        if (!cs)
            cs = bsize;
        u64 want = usz - got < bsize ? usz - got : bsize;
        if (cs > bsize + 16 || !dsar_read(d, off, cs, blk)) {
            *err = "cannot read a block of the game archive";
            free(out);
            out = NULL;
            break;
        }
        if (cs == want)
            memcpy(out + got, blk, cs);
        else {
            uLongf dl = (uLongf) want;
            if (uncompress(out + got, &dl, blk, cs) != Z_OK || dl != want) {
                *err = "zlib block of the game archive is damaged";
                free(out);
                out = NULL;
                break;
            }
        }
        got += want;
        off += cs;
        bi++;
    }
    free(blk);
    free(toc);
    *usz_out = usz;
    return out;
}

static void pack_init(FILE *fd, u64 off, u64 len)
{
    g_pk = fd;
    if (seek64(fd, off))
        die(1, "cannot seek in the data file");
    g_pk_left = len;
    if (lzma_stream_decoder(&g_xs, UINT64_MAX, 0) != LZMA_OK)
        die(3, "cannot start the xz decoder");
}

static void pack_read(u8 *out, size_t n)
{
    g_xs.next_out = out;
    g_xs.avail_out = n;
    while (g_xs.avail_out) {
        if (!g_xs.avail_in && g_pk_left) {
            size_t k =
                g_pk_left <
                sizeof g_pk_in ? (size_t) g_pk_left : sizeof g_pk_in;
            if (fread(g_pk_in, 1, k, g_pk) != k)
                die(1, "the data file is truncated (xz stream)");
            g_pk_left -= k;
            g_xs.next_in = g_pk_in;
            g_xs.avail_in = k;
        }
        lzma_ret r = lzma_code(&g_xs, g_pk_left ? LZMA_RUN : LZMA_FINISH);
        if (r == LZMA_STREAM_END) {
            g_pk_end = 1;
            break;
        }
        if (r != LZMA_OK)
            die(1, "the data file is damaged (xz stream, code %d)",
                (int) r);
    }
    if (g_xs.avail_out)
        die(1, "the data file ends early (xz stream)");
    g_pk_pos += n;
}

// after the last file: the rest of the stream (index, footer, CRC64 check) must decode cleanly and hold no further data
static void pack_finish(void)
{
    u8 one;
    while (!g_pk_end) {
        if (!g_xs.avail_in && g_pk_left) {
            size_t k =
                g_pk_left <
                sizeof g_pk_in ? (size_t) g_pk_left : sizeof g_pk_in;
            if (fread(g_pk_in, 1, k, g_pk) != k)
                die(1, "the data file is truncated (xz stream)");
            g_pk_left -= k;
            g_xs.next_in = g_pk_in;
            g_xs.avail_in = k;
        }
        g_xs.next_out = &one;
        g_xs.avail_out = 1;
        lzma_ret r = lzma_code(&g_xs, g_pk_left ? LZMA_RUN : LZMA_FINISH);
        if (g_xs.avail_out == 0)
            die(3, "the pack stream holds more data than expected");
        if (r == LZMA_STREAM_END)
            g_pk_end = 1;
        else if (r != LZMA_OK)
            die(1, "the data file is damaged (xz stream end, code %d)",
                (int) r);
    }
    if (g_xs.avail_in || g_pk_left)
        die(3, "trailing bytes after the pack stream");
}

static void lz4_one(const u8 *src, u32 usz, u8 *dst, size_t cap, u32 *outn)
{
    int n =
        LZ4_compress_HC((const char *) src, (char *) dst, (int) usz,
                        (int) cap, LZ4_LEVEL);
    if (n <= 0) {
        *outn = 0;
        return;
    }
    if ((u32) n == usz) {       // exactly usz bytes would read back as stored: literal-only block instead
        u32 q = 0;
        if (usz < 15)
            dst[q++] = (u8) (usz << 4);
        else {
            u32 rr = usz - 15;
            dst[q++] = 0xF0;
            while (rr >= 255) {
                dst[q++] = 255;
                rr -= 255;
            }
            dst[q++] = (u8) rr;
        }
        memcpy(dst + q, src, usz);
        n = (int) (q + usz);
    }
    *outn = (u32) n;
}

static DWORD WINAPI lz4_worker(LPVOID arg)
{
    Job *j = (Job *) arg;
    for (;;) {
        LONG i = InterlockedIncrement(&j->next) - 1;
        if (i >= (LONG) j->n)
            break;
        lz4_one(j->src + (size_t) i * DSAR_CHUNK, j->ck[i].usz,
                j->out + (size_t) i * j->stride, j->stride, &j->outn[i]);
        if (!j->outn[i])
            InterlockedExchange(&j->fail, 1);
    }
    return 0;
}

// kind 2: a file for the game folder (path relative to it, e.g. tlou-ii-mp/sidbase.bin); skipped when it is already there and equal
static void write_extra(const wchar_t *game, const Rec *rc, const u8 *res)
{
    for (const char *c = rc->path; *c; c++)     // plain relative path only
        if (!
            ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
             || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'
             || *c == '.' || *c == '/'))
            die(3, "bad extra file path %s", rc->path);
    if (!rc->path[0] || rc->path[0] == '/' || strstr(rc->path, "..")
        || strstr(rc->path, "//"))
        die(3, "bad extra file path %s", rc->path);
    wchar_t wp[1024];
    int n = _snwprintf(wp, 1024, L"%ls\\", game);
    for (const char *c = rc->path; *c && n < 1000; c++)
        wp[n++] = (*c == '/') ? L'\\' : (wchar_t) *c;
    wp[n] = 0;
    for (int i = (int)wcslen(game) + 1; i < n; i++)     // create the folders below the game folder
        if (wp[i] == L'\\') {
            wp[i] = 0;
            CreateDirectoryW(wp, NULL);
            wp[i] = L'\\';
        }
    FILE *f = _wfopen(wp, L"rb");
    if (f) {                    // same size and checksum: nothing to write
        _fseeki64(f, 0, SEEK_END);
        u64 have = (u64) _ftelli64(f);
        int same = 0;
        if (have == rc->usz) {
            u8 *buf = xmalloc((size_t) have ? (size_t) have : 1);
            _fseeki64(f, 0, SEEK_SET);
            same = fread(buf, 1, (size_t) have, f) == (size_t) have
                && fnv_payload(buf, (size_t) have,
                               0xCBF29CE484222325ULL) == rc->raw_fnv;
            free(buf);
        }
        fclose(f);
        if (same) {
            printf("t2r-slim: %s is up to date\n", rc->path);
            return;
        }
    }
    wchar_t tp[1100];
    _snwprintf(tp, 1100, L"%ls.t2rtmp", wp);
    f = _wfopen(tp, L"wb");
    if (!f)
        die(1, "cannot write %s in the game folder", rc->path);
    if (fwrite(res, 1, (size_t) rc->usz, f) != (size_t) rc->usz
        || fclose(f)) {
        _wremove(tp);
        die(1, "writing %s failed (disk full?)", rc->path);
    }
    if (!MoveFileExW(tp, wp, MOVEFILE_REPLACE_EXISTING)) {
        _wremove(tp);
        die(1, "cannot replace %s (error %lu)", rc->path, GetLastError());
    }
    printf("t2r-slim: wrote %s\n", rc->path);
}

// compress `rc`'s file (res, usz bytes) chunk by chunk exactly as the build did and write the chunks into the DLL copy
static void write_file_chunks(FILE *fo, const Head *h, const Rec *rc,
                              const Chunk *chunks, const u8 *res)
{
    size_t stride = (size_t) LZ4_compressBound(DSAR_CHUNK) + 64;
    static u8 *gout;
    static u32 *gn;
    if (!gout) {
        gout = xmalloc(stride * GROUP);
        gn = xmalloc(sizeof(u32) * GROUP);
    }
    for (u32 c0 = 0; c0 < rc->nchunks; c0 += GROUP) {
        u32 n = rc->nchunks - c0 < GROUP ? rc->nchunks - c0 : GROUP;
        for (u32 c = 0; c < n; c++) {
            const Chunk *ck = &chunks[rc->chunk0 + c0 + c];
            if ((u64) (c0 + c) * DSAR_CHUNK + ck->usz > rc->usz)
                die(3, "%s: chunk table does not match the file",
                    rc->path);
        }
        Job j;
        j.src = res + (size_t) c0 *DSAR_CHUNK;
        j.ck = &chunks[rc->chunk0 + c0];
        j.n = n;
        j.out = gout;
        j.stride = stride;
        j.outn = gn;
        j.next = 0;
        j.fail = 0;
        int nt = g_threads < (int) n ? g_threads : (int) n;
        HANDLE th[32];
        int started = 0;
        for (int t = 1; t < nt; t++) {
            th[started] = CreateThread(NULL, 0, lz4_worker, &j, 0, NULL);
            if (th[started])
                started++;
        }
        lz4_worker(&j);
        if (started) {
            WaitForMultipleObjects(started, th, TRUE, INFINITE);
            for (int t = 0; t < started; t++)
                CloseHandle(th[t]);
        }
        if (j.fail)
            die(3, "lz4 failed");
        for (u32 c = 0; c < n; c++) {
            const Chunk *ck = &chunks[rc->chunk0 + c0 + c];
            u8 *buf = gout + (size_t) c * stride;
            if (gn[c] != ck->csz)
                die(3,
                    "%s: compressed size of chunk %u is %u, expected %u (lz4 version differs from the build's)",
                    rc->path, c0 + c, gn[c], ck->csz);
            ks_xor(buf, gn[c], ck->payload_off);
            if (seek64(fo, h->blob_off + ck->payload_off)
                || fwrite(buf, 1, gn[c], fo) != gn[c])
                die(1, "writing the DLL failed (disk full?)");
        }
    }
}

static void progress(u64 add)
{
    g_pdone += add;
    if (!g_ptotal)
        return;
    int pct =
        (int) ((g_pdone > g_ptotal ? g_ptotal : g_pdone) * 100 / g_ptotal);
    if (pct == g_plast)
        return;
    g_plast = pct;
    printf("t2r-slim: progress %llu %llu\n",
           (unsigned long long) (g_pdone > g_ptotal ? g_ptotal : g_pdone),
           (unsigned long long) g_ptotal);
    fflush(stdout);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "usage: t2r-slim <slim winmm.dll> <t2r-slim.dat> <game folder> <out winmm.dll>\n");
        return 1;
    }
    const wchar_t *dll = argv[1], *dat = argv[2], *game = argv[3], *outp =
        argv[4];
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_threads = (int) si.dwNumberOfProcessors;
        wchar_t env[16];
        if (GetEnvironmentVariableW(L"T2R_SLIM_THREADS", env, 16))
            g_threads = _wtoi(env);
        g_skip_extras =
            GetEnvironmentVariableW(L"T2R_SLIM_SKIP_EXTRAS", env, 16) != 0;
        if (g_threads < 1)
            g_threads = 1;
        if (g_threads > 16)
            g_threads = 16;
    }
    FILE *fd = _wfopen(dat, L"rb");
    if (!fd)
        die(1, "cannot open %ls", dat);
    Head h;
    if (fread(&h, sizeof h, 1, fd) != 1 || memcmp(h.magic, "T2SLIM02", 8))
        die(1, "%ls is not a slim data file (or from another version)",
            dat);
    g_ks_end = h.blob_size & ~7ULL;
    Rec *recs = xmalloc(sizeof(Rec) * (h.nrec ? h.nrec : 1));
    Chunk *chunks = xmalloc(sizeof(Chunk) * (h.nchunks ? h.nchunks : 1));
    if (fread(recs, sizeof(Rec), h.nrec, fd) != h.nrec
        || fread(chunks, sizeof(Chunk), h.nchunks, fd) != h.nchunks)
        die(1, "%ls is truncated", dat);

    // 1. copy the slim DLL to <out>.tmp
    wchar_t tmp[1024];
    _snwprintf(tmp, 1024, L"%ls.tmp", outp);
    FILE *fi = _wfopen(dll, L"rb");
    if (!fi)
        die(1, "cannot open %ls", dll);
    _fseeki64(fi, 0, SEEK_END);
    u64 sz = (u64) _ftelli64(fi);
    if (sz != h.dll_size)
        die(1,
            "%ls has the wrong size (%llu, expected %llu): not the DLL this data file belongs to",
            dll, (unsigned long long) sz, (unsigned long long) h.dll_size);
    _fseeki64(fi, 0, SEEK_SET);
    g_ptotal = sz + h.blob_size;        // copy + hash; every record adds its size below
    for (u32 r = 0; r < h.nrec; r++)
        if (recs[r].kind <= 2)
            g_ptotal += recs[r].usz;
    progress(0);
    FILE *fo = _wfopen(tmp, L"w+b");
    wcscpy(g_tmp, tmp);
    g_fo = fo;
    if (!fo)
        die(1, "cannot create %ls (is the game folder writable?)", tmp);
    {
        u8 *buf = xmalloc(1 << 20);
        u64 left = sz;
        while (left) {
            size_t n = left < (1 << 20) ? (size_t) left : (1 << 20);
            if (fread(buf, 1, n, fi) != n || fwrite(buf, 1, n, fo) != n)
                die(1, "copying the DLL failed (disk full?)");
            left -= n;
            progress(n);
        }
        free(buf);
    }
    fclose(fi);

    // 2a. kind 0: edited copies of game files (zstd patch against the game's original)
    for (u32 r = 0; r < h.nrec; r++) {
        Rec *rc = &recs[r];
        if (rc->kind != 0)
            continue;
        wchar_t ap[1024], an[64];
        MultiByteToWideChar(CP_ACP, 0, rc->arch, -1, an, 64);
        _snwprintf(ap, 1024, L"%ls\\build\\pc\\main\\%ls", game, an);
        Dsar *d = dsar_open(ap);
        if (!d)
            die(2,
                "cannot read the game archive %ls (needed for %s). Is this The Last of Us Part II Remastered v1.6.10721 (Steam)?",
                ap, rc->path);
        const char *err = "";
        u64 bsz = 0;
        u8 *base = psarc_file(d, rc->md5, &bsz, &err);
        dsar_close(d);
        if (!base)
            die(2,
                "%s: %s in %ls. Your game version differs from v1.6.10721.",
                rc->path, err, ap);
        if (bsz != rc->base_size)
            die(2,
                "%s: the game's file has %llu bytes, expected %llu. Your game version differs from v1.6.10721.",
                rc->path, (unsigned long long) bsz,
                (unsigned long long) rc->base_size);
        u8 *res;
        if (rc->delta_len) {
            u8 *dl = xmalloc(rc->delta_len);
            if (seek64(fd, rc->delta_off)
                || fread(dl, 1, rc->delta_len, fd) != rc->delta_len)
                die(1, "%ls is truncated", dat);
            res = xmalloc(rc->usz);
            ZSTD_DCtx *dc = ZSTD_createDCtx();
            ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 31);
            size_t zr =
                ZSTD_decompress_usingDict(dc, res, rc->usz, dl,
                                          rc->delta_len, base,
                                          (size_t) bsz);
            ZSTD_freeDCtx(dc);
            free(dl);
            free(base);
            if (ZSTD_isError(zr) || zr != rc->usz)
                die(2,
                    "%s: the patch does not fit the game's file (%s). Your game version differs from v1.6.10721.",
                    rc->path,
                    ZSTD_isError(zr) ? ZSTD_getErrorName(zr) : "size");
        } else {
            res = base;
            if (bsz != rc->usz)
                die(3, "%s: identical file with different size", rc->path);
        }
        if (fnv_payload(res, rc->usz, 0xCBF29CE484222325ULL) !=
            rc->raw_fnv)
            die(2,
                "%s: result hash mismatch. Your game version differs from v1.6.10721.",
                rc->path);
        write_file_chunks(fo, &h, rc, chunks, res);
        free(res);
        progress(rc->usz);
        printf("t2r-slim: rebuilt %s\n", rc->path);
        fflush(stdout);
    }

    // 2b. kind 1: the xz pack stream (files in stream order)
    if (h.pack_len) {
        pack_init(fd, h.pack_off, h.pack_len);
        u32 nfiles = 0;
        for (u32 r = 0; r < h.nrec; r++) {
            Rec *rc = &recs[r];
            if (rc->kind != 1 && rc->kind != 2)
                continue;
            if (rc->pack_pos != g_pk_pos)
                die(3, "%s: pack stream order mismatch", rc->path);
            u8 *res = xmalloc(rc->usz);
            pack_read(res, (size_t) rc->usz);
            if (fnv_payload(res, rc->usz, 0xCBF29CE484222325ULL) !=
                rc->raw_fnv)
                die(3,
                    "%s: unpacked data does not match its checksum (damaged download?)",
                    rc->path);
            if (rc->kind == 2) {
                if (!g_skip_extras)
                    write_extra(game, rc, res);
            } else
                write_file_chunks(fo, &h, rc, chunks, res);
            free(res);
            progress(rc->usz);
            nfiles++;
        }
        if (g_pk_pos != h.pack_usz)
            die(3, "pack stream size mismatch");
        pack_finish();
        lzma_end(&g_xs);
        printf("t2r-slim: unpacked %u files\n", nfiles);
        fflush(stdout);
    }
    // 3. the T2ME hash of the finished payload
    {
        if (fflush(fo))
            die(1, "flush failed");
        u8 *buf = xmalloc(1 << 22);     // multiple of 8: qword words stay aligned between pieces
        u64 hh = 0xCBF29CE484222325ULL, left = h.blob_size, pos =
            h.blob_off;
        while (left) {
            size_t n = left < (1 << 22) ? (size_t) left : (1 << 22);
            if (seek64(fo, pos) || fread(buf, 1, n, fo) != n)
                die(1, "read back failed");
            hh = fnv_payload(buf, n, hh);
            pos += n;
            left -= n;
            progress(n);
        }
        free(buf);
        if (hh != h.blob_fnv)
            die(3,
                "the rebuilt mod archive does not match its checksum (%016llx != %016llx)",
                (unsigned long long) hh, (unsigned long long) h.blob_fnv);
    }
    g_fo = NULL;
    g_tmp[0] = 0;
    fclose(fo);
    fclose(fd);
    if (!MoveFileExW(tmp, outp, MOVEFILE_REPLACE_EXISTING))
        die(1, "cannot replace %ls (error %lu). Is the game running?",
            outp, GetLastError());
    printf("t2r-slim: %ls written (%llu bytes), checksum ok\n", outp,
           (unsigned long long) sz);
    return 0;
}
