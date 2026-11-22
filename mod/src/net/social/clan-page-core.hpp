#pragma once

// Pure core of tools/ext/clan13.h: the parse of the daemon's op 0xB7 text
// (tools/clan_ingame.py), the journal's story-thread grouping and the box
// clippers. No engine / Windows calls -- mod/tests/clan_core_test.cpp builds it
// standalone. The runtime asker lives in clan-page-data.*.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace net::clan::page {

    constexpr int C13_NP = 40;
    constexpr int C13_NJ = 24;
    constexpr int C13_NL = 72;
    constexpr int C13_NS = 5;
    constexpr int C13_NM = 8;
    constexpr int C13_NT = 32;
    constexpr int C13_NU = 4;
    constexpr int C13_NX = 12;
    constexpr int C13_ND = 86;
    constexpr int C13_NG = 48;

    struct Person {
        char name[28], role[28], st, stw[20], expr[14], pid[24], bond[28],
             bondty[20], tag[160], lately[176], icon[14];
    };
    struct Story {
        long id{};
        char cap[64], title[96], body[1720], tone[8], arc[16];
        int step{}, day{}, week{};
        char pid[24], who[28];
        int fresh{}, more{};
    };
    struct Timeline {
        long card{};
        int day{}, fresh{};
        char text[104];
    };
    struct Supply {
        char label[20];
        int stock{}, days{}, need{};
    };
    struct Raid {
        int day{};
        char map[32];
        int win{}, sup{}, need{}, fresh{};
        int got[5]{}, needc[5]{};
    };
    struct Thing {
        int day{}, fresh{};
        char qty[12], name[64], icon[20];
    };
    struct Member {
        char name[28], role[28], state[12], stw[24], pid[24], line[160];
    };
    struct DayEntry {
        int day{}, dow{}, week{};
        long id{};
        char title[80], tone[8], map[36];
        int win{}, sup{}, need{}, h{}, u{}, s{}, dn{}, fresh{};
    };
    struct Paragraph {
        int day{};
        char cap[48], text[760];
    };
    struct Player {
        char name[28];
        int team{}, me{}, kills{}, downs{}, deaths{}, revives{}, execs{},
            heads{}, parts{}, left{}, head{}, rk{}, wk{};
    };

    struct Data {
        int have[8]{};          // sections 1..6 that arrived
        int nd{}, ng{}, jweek{}, jtop{};
        DayEntry d[C13_ND];
        Paragraph g[C13_NG];
        int xday{}, xwin{}, xn{};
        char xmap[32]{};
        Player x[C13_NX];
        int ok{}, week{}, dow{}, morale{}, alive{}, worst{}, done{}, won{},
            lost{}, maxsz{};
        char fk{}, label[28]{};
        int np{}, nj{}, nl{}, ns{}, nm{}, nt{}, nu{};
        Person p[C13_NP];
        Story j[C13_NJ];
        Timeline l[C13_NL];
        Supply s[C13_NS];
        Raid m[C13_NM];
        Thing t[C13_NT];
        Member u[C13_NU];
    };

    inline void Cp(char* d, size_t n, const char* s)
    {
        if (!n || !s) return;
        size_t l = std::strlen(s);
        if (l >= n) l = n - 1;
        std::memcpy(d, s, l);
        d[l] = 0;
    }

    // split a line in place at TABs: f[i] = fields (empty ones included)
    inline int Fields(char* line, char** f, int max)
    {
        int n = 0;
        char* p = line;
        while (n < max) {
            f[n++] = p;
            char* t = std::strchr(p, '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        return n;
    }

    inline void Nums(const char* s, int* o, int n)
    {
        for (int i = 0; i < n; ++i) {
            char* e = nullptr;
            o[i] = static_cast<int>(std::strtol(s, &e, 10));
            s = *e ? e + 1 : e;
        }
    }

    // Parse one reply (section sec) into d; `text` is modified. -1 = invalid.
    inline int Parse(Data& d, int sec, char* text)
    {
        if (sec < 1 || sec > 6 || !text)
            return -1;
        char* f[24];
        int rows = 0;
        if (!text[0]) {
            d.have[sec] = 1;
            if (sec == 5) { d.xday = 0; d.xn = 0; }
            else if (sec == 6) { d.nd = d.ng = 0; d.jweek = d.jtop = 0; }
            else d.ok = 0;
            return 0;
        }
        if (sec == 1) d.np = 0;
        else if (sec == 2) d.nj = d.nl = 0;
        else if (sec == 4) d.nu = 0;
        else if (sec == 5) d.xn = d.xday = 0;
        else if (sec == 6) { d.nd = d.ng = 0; d.jweek = d.jtop = 0; }
        else d.ns = d.nm = d.nt = 0;

        int got_h = 0;
        for (char* ln = text; ln && *ln;) {
            char* nl = std::strchr(ln, '\n');
            if (nl) *nl = 0;
            const int nf = Fields(ln, f, 24);
            const char k = f[0][0];
            if (k == 'X' && nf >= 5 && sec == 5) {
                d.xday = std::atoi(f[1]);
                Cp(d.xmap, sizeof d.xmap, f[2]);
                d.xwin = f[3][0] == '1' ? 1 : f[3][0] == '0' ? 0 : -1;
                ++rows;
            } else if (k == 'Q' && nf >= 12 && sec == 5 && d.xn < C13_NX) {
                Player& x = d.x[d.xn++];
                x = Player{};
                Cp(x.name, sizeof x.name, f[1]);
                x.team = std::atoi(f[2]);
                x.me = std::atoi(f[3]) != 0;
                x.kills = std::atoi(f[4]);
                x.downs = std::atoi(f[5]);
                x.deaths = std::atoi(f[6]);
                x.revives = std::atoi(f[7]);
                x.execs = std::atoi(f[8]);
                x.heads = std::atoi(f[9]);
                x.parts = std::atoi(f[10]);
                x.left = std::atoi(f[11]) != 0;
                x.head = nf >= 13 ? std::atoi(f[12]) : 0;
                x.rk  = nf >= 15 ? std::atoi(f[13]) : -1;
                x.wk  = nf >= 15 ? std::atoi(f[14]) : 0;
                ++rows;
            } else if (k == 'K' && nf >= 3 && sec == 6) {
                d.jweek = std::atoi(f[1]);
                d.jtop = std::atoi(f[2]);
                ++rows;
            } else if (k == 'D' && nf >= 16 && sec == 6 && d.nd < C13_ND) {
                DayEntry& e = d.d[d.nd++];
                e = DayEntry{};
                e.day = std::atoi(f[1]);
                e.dow = std::atoi(f[2]);
                e.week = std::atoi(f[3]);
                e.id = std::atol(f[4]);
                Cp(e.title, sizeof e.title, f[5]);
                Cp(e.tone, sizeof e.tone, f[6]);
                Cp(e.map, sizeof e.map, f[7]);
                e.win = f[8][0] == '1' ? 1 : f[8][0] == '0' ? 0 : -1;
                e.sup = std::atoi(f[9]);
                e.need = std::atoi(f[10]);
                e.h = std::atoi(f[11]);
                e.u = std::atoi(f[12]);
                e.s = std::atoi(f[13]);
                e.dn = std::atoi(f[14]);
                e.fresh = std::atoi(f[15]) != 0;
                ++rows;
            } else if (k == 'G' && nf >= 4 && sec == 6 && d.ng < C13_NG) {
                Paragraph& g = d.g[d.ng++];
                g = Paragraph{};
                g.day = std::atoi(f[1]);
                Cp(g.cap, sizeof g.cap, f[2]);
                Cp(g.text, sizeof g.text, f[3]);
                ++rows;
            } else if (k == 'H' && nf >= 8) {
                d.done = nf >= 12 ? std::atoi(f[8]) : 0;
                d.won = nf >= 12 ? std::atoi(f[9]) : 0;
                d.lost = nf >= 12 ? std::atoi(f[10]) : 0;
                d.maxsz = nf >= 12 ? std::atoi(f[11]) : 0;
                d.ok = 1;
                d.week = std::atoi(f[1]);
                d.dow = std::atoi(f[2]);
                d.fk = f[3][0];
                d.morale = std::atoi(f[4]);
                Cp(d.label, sizeof d.label, f[5]);
                d.alive = std::atoi(f[6]);
                d.worst = std::atoi(f[7]);
                got_h = 1;
            } else if (k == 'P' && nf >= 12 && sec == 1 && d.np < C13_NP) {
                Person& p = d.p[d.np++];
                p = Person{};
                Cp(p.name, sizeof p.name, f[1]);
                Cp(p.role, sizeof p.role, f[2]);
                p.st = f[3][0];
                Cp(p.stw, sizeof p.stw, f[4]);
                Cp(p.expr, sizeof p.expr, f[5]);
                Cp(p.pid, sizeof p.pid, f[6]);
                Cp(p.bond, sizeof p.bond, f[7]);
                Cp(p.bondty, sizeof p.bondty, f[8]);
                Cp(p.tag, sizeof p.tag, f[9]);
                Cp(p.lately, sizeof p.lately, f[10]);
                Cp(p.icon, sizeof p.icon, f[11]);
                ++rows;
            } else if (k == 'J' && nf >= 13 && sec == 2 && d.nj < C13_NJ) {
                Story& j = d.j[d.nj++];
                j = Story{};
                j.id = std::atol(f[1]);
                Cp(j.cap, sizeof j.cap, f[2]);
                Cp(j.title, sizeof j.title, f[3]);
                Cp(j.body, sizeof j.body, f[4]);
                Cp(j.tone, sizeof j.tone, f[5]);
                Cp(j.arc, sizeof j.arc, f[6]);
                j.step = std::atoi(f[7]);
                j.day = std::atoi(f[8]);
                j.week = std::atoi(f[9]);
                Cp(j.pid, sizeof j.pid, f[10]);
                Cp(j.who, sizeof j.who, f[11]);
                j.fresh = std::atoi(f[12]) != 0;
                j.more = nf >= 14 ? std::atoi(f[13]) : 0;
                ++rows;
            } else if (k == 'L' && nf >= 5 && sec == 2 && d.nl < C13_NL) {
                Timeline& l = d.l[d.nl++];
                l = Timeline{};
                l.card = std::atol(f[1]);
                l.day = std::atoi(f[2]);
                Cp(l.text, sizeof l.text, f[3]);
                l.fresh = std::atoi(f[4]) != 0;
                ++rows;
            } else if (k == 'S' && nf >= 5 && sec == 3 && d.ns < C13_NS) {
                Supply& s = d.s[d.ns++];
                Cp(s.label, sizeof s.label, f[1]);
                s.stock = std::atoi(f[2]);
                s.days = std::atoi(f[3]);
                s.need = std::atoi(f[4]);
                ++rows;
            } else if (k == 'U' && nf >= 7 && sec == 4 && d.nu < C13_NU) {
                Member& u = d.u[d.nu++];
                u = Member{};
                Cp(u.name, sizeof u.name, f[1]);
                Cp(u.role, sizeof u.role, f[2]);
                Cp(u.state, sizeof u.state, f[3]);
                Cp(u.stw, sizeof u.stw, f[4]);
                Cp(u.pid, sizeof u.pid, f[5]);
                Cp(u.line, sizeof u.line, f[6]);
                ++rows;
            } else if (k == 'M' && nf >= 9 && sec == 3 && d.nm < C13_NM) {
                Raid& m = d.m[d.nm++];
                m = Raid{};
                m.day = std::atoi(f[1]);
                Cp(m.map, sizeof m.map, f[2]);
                m.win = f[3][0] == '1' ? 1 : f[3][0] == '0' ? 0 : -1;
                m.sup = std::atoi(f[4]);
                m.need = std::atoi(f[5]);
                m.fresh = std::atoi(f[6]) != 0;
                Nums(f[7], m.got, 5);
                Nums(f[8], m.needc, 5);
                ++rows;
            } else if (k == 'T' && nf >= 6 && sec == 3 && d.nt < C13_NT) {
                Thing& t = d.t[d.nt++];
                t = Thing{};
                t.day = std::atoi(f[1]);
                Cp(t.qty, sizeof t.qty, f[2]);
                Cp(t.name, sizeof t.name, f[3]);
                Cp(t.icon, sizeof t.icon, f[4]);
                t.fresh = std::atoi(f[5]) != 0;
                ++rows;
            }
            ln = nl ? nl + 1 : nullptr;
        }
        if (!got_h)
            return -1;
        d.have[sec] = 1;
        return rows;
    }

    // One card per story arc, its newest step; rows without an arc are single cards.
    inline int Cards(const Data& d, int* idx, int max)
    {
        int n = 0;
        for (int i = 0; i < d.nj && n < max; ++i) {
            bool dup = false;
            if (d.j[i].arc[0])
                for (int k = 0; k < n; ++k)
                    if (!std::strcmp(d.j[idx[k]].arc, d.j[i].arc)) { dup = true; break; }
            if (!dup) idx[n++] = i;
        }
        return n;
    }

    // The steps of card j (oldest -> newest, capped at the newest `max`).
    inline int Thread(const Data& d, int j, int* idx, int max)
    {
        int all[C13_NJ], n = 0;
        if (j < 0 || j >= d.nj || max < 1)
            return 0;
        if (!d.j[j].arc[0]) { idx[0] = j; return 1; }
        for (int i = d.nj - 1; i >= 0; --i)
            if (!std::strcmp(d.j[i].arc, d.j[j].arc)) all[n++] = i;
        for (int a = 1; a < n; ++a) {         // stable insertion by step
            const int v = all[a];
            int b = a - 1;
            while (b >= 0 && d.j[all[b]].step > d.j[v].step) {
                all[b + 1] = all[b];
                --b;
            }
            all[b + 1] = v;
        }
        const int from = n > max ? n - max : 0;
        int o = 0;
        for (int i = from; i < n; ++i) idx[o++] = all[i];
        return o;
    }

    inline int WeekRows(const Data& d, int week, int* idx, int max)
    {
        int n = 0;
        for (int i = 0; i < d.nd && n < max; ++i)
            if (d.d[i].week == week) idx[n++] = i;
        return n;
    }

    inline int DayParas(const Data& d, int day, int* idx, int max)
    {
        int n = 0;
        for (int i = 0; i < d.ng && n < max; ++i)
            if (d.g[i].day == day) idx[n++] = i;
        return n;
    }

    using Estimate = float (*)(const char*, float);

    // Greedy word wrap into a box; the cut ends in "...". Pure: Est is supplied
    // by the caller (the ext uses the engine's text measurement here).
    inline void Clip(char* dst, size_t n, const char* src, float boxw,
                     float pt, int maxlines, Estimate Est)
    {
        if (!n || !Est) return;
        dst[0] = 0;
        if (!src || !*src || maxlines < 1) return;
        char line[256], word[128];
        size_t o = 0, ll = 0;
        int lines = 1, cut = 0;
        const float sp = Est(" ", pt);
        float lw = 0.0f;
        line[0] = 0;
        const char* p = src;
        while (*p && !cut) {
            while (*p == ' ') ++p;
            size_t wl = 0;
            while (*p && *p != ' ' && wl < sizeof(word) - 1) word[wl++] = *p++;
            word[wl] = 0;
            if (!wl) break;
            const float ww = Est(word, pt);
            if (ll && lw + sp + ww > boxw) {
                if (lines >= maxlines) { cut = 1; break; }
                ++lines;
                if (o + 1 < n) dst[o++] = ' ';
                ll = 0; lw = 0.0f; line[0] = 0;
            }
            if (o + wl + 5 >= n) { cut = 1; break; }
            if (ll) {
                dst[o++] = ' ';
                lw += sp;
                line[ll++] = ' ';
            }
            std::memcpy(dst + o, word, wl);
            o += wl;
            if (ll + wl < sizeof(line) - 1) {
                std::memcpy(line + ll, word, wl);
                ll += wl;
                line[ll] = 0;
            }
            lw += ww;
        }
        dst[o] = 0;
        if (cut || *p) {
            char tmp[300];
            for (;;) {
                std::snprintf(tmp, sizeof tmp, "%s...", line);
                if (Est(tmp, pt) <= boxw || !ll) break;
                char* sp_ = std::strrchr(line, ' ');
                if (!sp_) { ll = 0; line[0] = 0; break; }
                const size_t keep = static_cast<size_t>(sp_ - line);
                dst[o - (ll - keep)] = 0;
                o -= ll - keep;
                line[keep] = 0;
                ll = keep;
            }
            while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == ','
                             || dst[o - 1] == ';')) dst[--o] = 0;
            if (o + 4 < n) std::memcpy(dst + o, "...", 4);
        }
    }

    // Wrap with explicit line breaks (the journal's reading pane). Returns the
    // number of lines written.
    inline int Wrap(char* dst, size_t n, const char* src, float boxw,
                    float pt, int maxlines, Estimate Est)
    {
        if (!n || !Est) return 0;
        dst[0] = 0;
        if (!src || !*src || maxlines < 1) return 0;
        char line[320], word[128];
        size_t o = 0, ll = 0;
        int lines = 1, cut = 0;
        const float sp = Est(" ", pt);
        float lw = 0.0f;
        line[0] = 0;
        const char* p = src;
        while (*p && !cut) {
            int brk = 0;
            while (*p == ' ' || *p == '\n') { if (*p == '\n') ++brk; ++p; }
            if (!*p) break;
            size_t wl = 0;
            while (*p && *p != ' ' && *p != '\n' && wl < sizeof(word) - 1)
                word[wl++] = *p++;
            word[wl] = 0;
            const float ww = Est(word, pt);
            const int nl = !o ? 0 : brk > 1 ? 2 : brk == 1 ? 1
                         : (ll && lw + sp + ww > boxw) ? 1 : 0;
            if (nl) {
                if (lines + nl > maxlines || o + static_cast<size_t>(nl) + 5 >= n) {
                    cut = 1;
                    break;
                }
                for (int k = 0; k < nl; ++k) dst[o++] = '\n';
                lines += nl;
                ll = 0; lw = 0.0f; line[0] = 0;
            }
            if (o + wl + 6 >= n) { cut = 1; break; }
            if (ll) {
                dst[o++] = ' ';
                lw += sp;
                if (ll < sizeof(line) - 1) line[ll++] = ' ';
            }
            std::memcpy(dst + o, word, wl);
            o += wl;
            if (ll + wl < sizeof(line) - 1) {
                std::memcpy(line + ll, word, wl);
                ll += wl;
                line[ll] = 0;
            }
            lw += ww;
        }
        dst[o] = 0;
        if (cut) {
            char tmp[340];
            for (;;) {
                std::snprintf(tmp, sizeof tmp, "%s...", line);
                if (Est(tmp, pt) <= boxw || !ll) break;
                char* sp_ = std::strrchr(line, ' ');
                if (!sp_) {
                    o -= ll;
                    dst[o] = 0;
                    ll = 0; line[0] = 0;
                    break;
                }
                const size_t keep = static_cast<size_t>(sp_ - line);
                o -= ll - keep;
                dst[o] = 0;
                line[keep] = 0;
                ll = keep;
            }
            while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == ','
                             || dst[o - 1] == ';')) dst[--o] = 0;
            if (o + 4 < n) std::memcpy(dst + o, "...", 4);
        }
        return lines;
    }

} // namespace net::clan::page
