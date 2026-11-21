#include "locale.hpp"
#include "loc_table_pl.hpp"
#include "exe_side.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace engine::locale {

    namespace {

        // The language registry. English (index 0) is the source language: no
        // table, source strings pass through. To add a language, generate a
        // loc_table_<code>.hpp and add a row here -- call sites need no changes.
        const Language kLanguages[] = {
            { "en", nullptr, 0, nullptr, 0 },
            { "pl", kPolishEntries, kPolishEntryCount, kPolishProbes, kPolishProbeCount },
        };
        constexpr int kLanguageCount = static_cast<int>(sizeof kLanguages / sizeof kLanguages[0]);

        constexpr uint64_t kRecheckMs = 2000;   // the ext re-probed every 2 s
        constexpr int      kRing = 16;          // ring of translation buffers (ext LOC_RING)
        constexpr size_t   kRingBuf = 1536;     // ext LOC_BUF

        uint64_t g_at = 0;
        int      g_index = 0;                   // active registry index
        bool     g_envDone = false, g_envSet = false;
        char     g_envCode[16] = {};
        char     g_fileCode[16] = {};
        char     g_override[16] = {};
        bool     g_overrideSet = false;

        // ---- lookup tables (built for the active language) -------------------
        struct Part { uint8_t kind = 0; std::string lit; };  // 0 literal, 1 number capture, 2 text capture
        struct Tpl {
            std::vector<Part> parts;
            int         ncap = 0;
            const char* tr = nullptr;
            size_t      litlen = 0;
        };
        struct Cap { const char* s = nullptr; size_t n = 0; };

        std::vector<int>    g_hash;             // exact keys: open addressing, entry index + 1
        size_t              g_hashMask = 0;
        const LocEntry*     g_entries = nullptr;
        std::vector<Tpl>    g_tpl;
        int                 g_built = -2;       // -2 never, -1 English/none, >= 0 built for index

        struct Hit { uint64_t h = 0; uint16_t n = 0; char t[254] = {}; };
        Hit      g_hit[512];                    // positive cache (the same texts are set every frame)
        uint64_t g_miss[1024] = {};             // negative cache

        char         g_ring[kRing][kRingBuf];
        volatile LONG g_ringI = 0;

        char* RingNext()
        {
            const uint32_t i = static_cast<uint32_t>(InterlockedIncrement(&g_ringI));
            return g_ring[i % kRing];
        }

        uint64_t Fnv(const char* s, size_t n)
        {
            uint64_t h = 1469598103934665603ULL;
            for (size_t i = 0; i < n; i++)
                h = (h ^ static_cast<uint8_t>(s[i])) * 1099511628211ULL;
            return h;
        }

        // The spec a '%' at s starts: length (0 = a literal percent); kind = 1 number, 2 text.
        int Spec(const char* s, int* kind)
        {
            const char* p = s + 1;
            if (*p == '.') {
                p++;
                while (*p >= '0' && *p <= '9') p++;
            } else {
                while (*p >= '0' && *p <= '9') p++;
            }
            while (*p == 'l' || *p == 'z') p++;
            if (*p == 'd' || *p == 'u' || *p == 'f' || *p == 'x') { *kind = 1; return static_cast<int>(p - s) + 1; }
            if (*p == 's' || *p == 'c') { *kind = 2; return static_cast<int>(p - s) + 1; }
            return 0;
        }

        bool HasCapture(const char* k)
        {
            for (const char* p = k; *p; p++) {
                if (*p != '%') continue;
                if (p[1] == '%') { p++; continue; }
                int kind = 0;
                if (Spec(p, &kind)) return true;
            }
            return false;
        }

        void Build(int lang)
        {
            // Tables for a previous language are left in place (another thread may
            // still be inside a lookup); Detect sets the index before a rebuild.
            g_hash.clear();
            g_tpl.clear();
            g_entries = nullptr;
            g_built = lang;
            std::memset(g_miss, 0, sizeof g_miss);
            std::memset(g_hit, 0, sizeof g_hit);
            if (lang <= 0)
                return;                        // English / no table

            const Language& L = kLanguages[lang];
            g_entries = L.entries;
            size_t cap = 64;
            while (cap < static_cast<size_t>(L.count) * 2) cap <<= 1;
            g_hash.assign(cap, 0);
            g_hashMask = cap - 1;
            g_tpl.resize(static_cast<size_t>(L.count));

            int ntpl = 0;
            for (int i = 0; i < L.count; i++) {
                const char* k = L.entries[i].en;
                if (!HasCapture(k)) {
                    unsigned h = static_cast<unsigned>(Fnv(k, std::strlen(k))) & static_cast<unsigned>(g_hashMask);
                    while (g_hash[h]) h = (h + 1) & static_cast<unsigned>(g_hashMask);
                    g_hash[h] = i + 1;
                    continue;
                }
                Tpl& t = g_tpl[static_cast<size_t>(ntpl)];
                t.tr = L.entries[i].tr;
                std::string lit;
                for (const char* p = k; *p;) {
                    if (*p == '%' && p[1] == '%') { lit.push_back('%'); p += 2; continue; }
                    int kind = 0;
                    const int sl = (*p == '%') ? Spec(p, &kind) : 0;
                    if (sl) {
                        if (!lit.empty()) {
                            Part part; part.kind = 0; part.lit = lit;
                            t.parts.push_back(part);
                            t.litlen += lit.size();
                            lit.clear();
                        }
                        Part part; part.kind = static_cast<uint8_t>(kind);
                        t.parts.push_back(part);
                        t.ncap++;
                        p += sl;
                        continue;
                    }
                    lit.push_back(*p++);
                }
                if (!lit.empty()) {
                    Part part; part.kind = 0; part.lit = lit;
                    t.parts.push_back(part);
                    t.litlen += lit.size();
                }
                ntpl++;
            }
            g_tpl.resize(static_cast<size_t>(ntpl));
            // Most specific templates first (longest literal text).
            std::stable_sort(g_tpl.begin(), g_tpl.end(),
                [](const Tpl& a, const Tpl& b) { return a.litlen > b.litlen; });
        }

        const char* Exact(const char* s, size_t n)
        {
            if (g_hash.empty()) return nullptr;
            unsigned h = static_cast<unsigned>(Fnv(s, n)) & static_cast<unsigned>(g_hashMask);
            while (g_hash[h]) {
                const LocEntry* e = &g_entries[g_hash[h] - 1];
                if (std::strlen(e->en) == n && !std::memcmp(e->en, s, n))
                    return e->tr;
                h = (h + 1) & static_cast<unsigned>(g_hashMask);
            }
            return nullptr;
        }

        bool Core(const char* s, size_t n, std::string& out, int depth);

        bool Match(const Tpl& t, size_t pi, const char* s, Cap* cap, int nc)
        {
            if (pi == t.parts.size())
                return *s == 0;
            const Part& p = t.parts[pi];
            if (p.kind == 0) {
                if (std::strncmp(s, p.lit.c_str(), p.lit.size())) return false;
                return Match(t, pi + 1, s + p.lit.size(), cap, nc);
            }
            if (p.kind == 1) {                 // number capture
                const char* e = s;
                if (*e == '+' || *e == '-') e++;
                const char* d = e;
                while (*e >= '0' && *e <= '9') e++;
                if (e == d) return false;
                if (*e == '.' && e[1] >= '0' && e[1] <= '9') {
                    e++;
                    while (*e >= '0' && *e <= '9') e++;
                }
                cap[nc].s = s; cap[nc].n = static_cast<size_t>(e - s);
                return Match(t, pi + 1, e, cap, nc + 1);
            }
            const size_t len = std::strlen(s); // text capture: shortest first
            for (size_t k = 0; k <= len; k++) {
                cap[nc].s = s; cap[nc].n = k;
                if (Match(t, pi + 1, s + k, cap, nc + 1)) return true;
            }
            return false;
        }

        void Expand(const Tpl& t, const Cap* c, std::string& out, int depth)
        {
            for (const char* p = t.tr; *p;) {
                const bool raw = p[0] == '{' && p[1] == '=' && p[2] >= '1' && p[2] <= '9';
                if (*p == '{' && (raw || (p[1] >= '1' && p[1] <= '9'))) {
                    const int idx = p[1 + raw] - '1';
                    const char* q = p + 2 + raw;
                    char forms[3][64] = {};
                    int nf = 0;
                    while (*q == '|' && nf < 3) {
                        q++;
                        int k = 0;
                        while (*q && *q != '|' && *q != '}') { if (k < 63) forms[nf][k++] = *q; q++; }
                        forms[nf][k] = 0;
                        nf++;
                    }
                    if (*q == '}' && idx < t.ncap) {
                        if (nf) {
                            int v = std::atoi(c[idx].s);
                            if (v < 0) v = -v;
                            int f = v == 1 ? 0 : ((v % 10 >= 2 && v % 10 <= 4) && !(v % 100 >= 12 && v % 100 <= 14)) ? 1 : 2;
                            if (c[idx].n && std::memchr(c[idx].s, '.', c[idx].n)) f = 2;
                            if (f >= nf) f = nf - 1;
                            out.append(forms[f]);
                        } else {
                            const size_t cn = std::min<size_t>(c[idx].n, 511);
                            const std::string src(c[idx].s, cn);
                            std::string sub;
                            if (!raw && Core(src.data(), src.size(), sub, depth + 1) && !sub.empty())
                                out.append(sub);
                            else
                                out.append(src);
                        }
                        p = q + 1;
                        continue;
                    }
                }
                out.push_back(*p++);
            }
        }

        bool Piece(const char* s, size_t n, std::string& out, int depth)
        {
            if (!n) return false;
            bool letters = false;
            for (size_t i = 0; i < n; i++)
                if ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) { letters = true; break; }
            if (!letters) return false;         // timers / numbers: nothing to translate

            if (const char* tr = Exact(s, n)) { out = tr; return true; }
            if (depth > 1) return false;

            std::string tmp(s, n);
            Cap c[12];
            for (const Tpl& t : g_tpl) {
                if (t.ncap > 12) continue;
                if (!t.parts.empty() && t.parts[0].kind == 0 && tmp[0] != t.parts[0].lit[0]) continue;
                if (Match(t, 0, tmp.c_str(), c, 0)) {
                    out.clear();
                    Expand(t, c, out, depth);
                    return true;
                }
            }
            return false;
        }

        bool Trimmed(const char* s, size_t n, std::string& out, int depth)
        {
            size_t a = 0, b = n;
            while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
            if (a == b) return false;
            std::string mid;
            if (!Piece(s + a, b - a, mid, depth)) return false;
            out.assign(s, a);
            out += mid;
            out.append(s + b, n - b);
            return true;
        }

        bool Core(const char* s, size_t n, std::string& out, int depth)
        {
            if (Trimmed(s, n, out, depth)) return true;
            out.clear();
            bool any = false;
            for (size_t i = 0; i < n;) {
                const char ch = s[i];
                if (ch == '<' || ch == '|') {
                    const char close = ch == '<' ? '>' : '|';
                    size_t j = i + 1;
                    while (j < n && s[j] != close && s[j] != '\n') j++;
                    if (j < n && s[j] == close) {
                        out.append(s + i, j - i + 1);
                        i = j + 1;
                        continue;
                    }
                }
                if (ch == '\n') { out.push_back(ch); i++; continue; }
                size_t j = i;
                while (j < n && s[j] != '\n' && s[j] != '<' && !(s[j] == '|' && j + 1 < n)) j++;
                if (j == i) j = i + 1;
                std::string piece;
                if (Trimmed(s + i, j - i, piece, depth)) {
                    any = true;
                    out += piece;
                } else {
                    out.append(s + i, j - i);
                }
                i = j;
            }
            return any;
        }

        const char* Fold(const char* s)
        {
            bool hi = false;
            for (const char* p = s; *p; p++)
                if (static_cast<uint8_t>(*p) >= 0x80) { hi = true; break; }
            if (!hi) return s;
            char* out = RingNext();
            size_t o = 0;
            const auto* p = reinterpret_cast<const uint8_t*>(s);
            while (*p && o + 1 < kRingBuf) {
                if (p[0] == 0xC4 || p[0] == 0xC5) {
                    static const struct { uint8_t a, b; char c; } F[] = {
                        { 0xC4, 0x85, 'a' }, { 0xC4, 0x84, 'A' }, { 0xC4, 0x87, 'c' }, { 0xC4, 0x86, 'C' },
                        { 0xC4, 0x99, 'e' }, { 0xC4, 0x98, 'E' }, { 0xC5, 0x82, 'l' }, { 0xC5, 0x81, 'L' },
                        { 0xC5, 0x84, 'n' }, { 0xC5, 0x83, 'N' }, { 0xC5, 0x9B, 's' }, { 0xC5, 0x9A, 'S' },
                        { 0xC5, 0xBA, 'z' }, { 0xC5, 0xB9, 'Z' }, { 0xC5, 0xBC, 'z' }, { 0xC5, 0xBB, 'Z' },
                    };
                    bool hit = false;
                    for (const auto& f : F)
                        if (p[0] == f.a && p[1] == f.b) { out[o++] = f.c; p += 2; hit = true; break; }
                    if (hit) continue;
                }
                if (p[0] == 0xC3 && (p[1] == 0xB3 || p[1] == 0x93)) {
                    out[o++] = p[1] == 0xB3 ? 'o' : 'O';
                    p += 2;
                    continue;
                }
                out[o++] = static_cast<char>(*p++);
            }
            out[o] = 0;
            return out;
        }

        // ---- language selection ----------------------------------------------
        int Lower(char c) { return static_cast<int>(std::tolower(static_cast<unsigned char>(c))); }

        bool CodeEquals(const char* a, const char* b)
        {
            while (*a && *b) { if (Lower(*a) != Lower(*b)) return false; a++; b++; }
            return *a == 0 && *b == 0;
        }

        int Resolve(const char* s)
        {
            if (!s) return -1;                 // no explicit choice: keep detecting
            while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
            char buf[16] = {};
            size_t n = 0;
            for (; *s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && n < sizeof buf - 1; n++)
                buf[n] = static_cast<char>(Lower(*s++));
            if (!n || !std::strcmp(buf, "auto")) return -1;   // auto-detect
            for (int i = 0; i < kLanguageCount; i++)
                if (CodeEquals(buf, kLanguages[i].code)) return i;
            return 0;                                          // unknown code -> English
        }

        void ProbeEnv()
        {
            char buf[16] = {};
            const DWORD n = GetEnvironmentVariableA("T2R_LANG", buf, sizeof buf);
            g_envSet = n > 0 && n < sizeof buf;
            if (g_envSet) std::snprintf(g_envCode, sizeof g_envCode, "%s", buf);
            g_envDone = true;
        }

        int FromFile()
        {
            FILE* f = engine::exe_side::Open("t2r-lang.txt", "r");
            if (!f) return 0;
            char buf[32] = {};
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[n] = 0;
            std::snprintf(g_fileCode, sizeof g_fileCode, "%s", buf);
            return Resolve(buf);
        }

        int AutoDetect()
        {
            // The ext identifies the engine's TEXT language by looking up a few
            // probe ids (ported in each table's k*Probes) through the engine text
            // lookup native and comparing the wording. That native call is not
            // wired in mod/ yet, so there is no signal here and auto resolves to
            // English, exactly like the previous minimal locale port. Explicit
            // codes (env/file/SetOverride) work for every registered language.
            return 0;
        }

        int Detect()
        {
            const char* code = nullptr;
            if (g_overrideSet) {
                code = g_override;
            } else {
                if (!g_envDone) ProbeEnv();
                if (g_envSet) code = g_envCode;
            }
            int want = Resolve(code);
            if (want >= 0) return want;
            if (!code) want = FromFile();       // no override/env: try the sidecar
            if (want >= 0) return want;
            return AutoDetect();
        }

    }  // namespace

    int CurrentIndex()
    {
        const uint64_t now = GetTickCount64();
        if (now - g_at >= kRecheckMs) {
            g_at = now;
            const int want = Detect();
            if (want != g_index || g_built != want) {
                g_index = want;
                Build(want);
            }
        }
        return g_index;
    }

    const char* Code()
    {
        const int i = CurrentIndex();
        return kLanguages[i].code;
    }

    bool IsEnglish() { return CurrentIndex() == 0; }

    bool IsPolish()
    {
        const int i = CurrentIndex();
        return i > 0 && i < kLanguageCount && CodeEquals(kLanguages[i].code, "pl");
    }

    int LanguageCount() { return kLanguageCount; }

    const char* LanguageCode(int index)
    {
        return (index >= 0 && index < kLanguageCount) ? kLanguages[index].code : "en";
    }

    void SetOverride(const char* code)
    {
        std::snprintf(g_override, sizeof g_override, "%s", code ? code : "");
        g_overrideSet = true;
        g_at = 0;
    }

    void ClearOverride()
    {
        g_overrideSet = false;
        g_at = 0;
    }

    void Reset()
    {
        g_at = 0;
        g_envDone = false;
        g_built = -2;
    }

    const char* Translate(const char* s)
    {
        const int lang = CurrentIndex();
        if (lang <= 0 || !s || !s[0]) return s;
        if (g_built != lang) return s;
        const size_t n = std::strlen(s);
        if (n >= kRingBuf / 2) return s;
        const uint64_t h = Fnv(s, n) | 1;
        if (g_miss[h & 1023] == h) return s;
        char* out = RingNext();
        const unsigned hi = static_cast<unsigned>(h >> 7) & 511;
        if (g_hit[hi].h == h) {
            std::memcpy(out, g_hit[hi].t, g_hit[hi].n + 1u);
            return out;
        }
        std::string res;
        if (Core(s, n, res, 0) && !res.empty()) {
            if (res.size() < sizeof g_hit[0].t) {
                g_hit[hi].h = 0;
                std::memcpy(g_hit[hi].t, res.c_str(), res.size() + 1);
                g_hit[hi].n = static_cast<uint16_t>(res.size());
                g_hit[hi].h = h;
            }
            if (res.size() >= kRingBuf) res.resize(kRingBuf - 1);
            std::memcpy(out, res.c_str(), res.size() + 1);
            return out;
        }
        g_miss[h & 1023] = h;
        return s;
    }

    const char* TranslateDraw(const char* s)
    {
        const char* t = Translate(s);
        return t == s ? s : Fold(t);
    }

    const char* Tr(const char* en)
    {
        const char* t = Translate(en);
        return t ? t : en;
    }

    const char* Tr(const char* en, const char* pl)
    {
        const char* t = Translate(en);
        if (t && t != en) return t;             // table translation (any language)
        return IsPolish() && pl ? pl : en;       // inline fallback, Polish only
    }

}
