#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure core of tools/ext/achieve.c + tools/ext/achieve_ui.c (build 191).
// No game headers and no Windows: shared by the native owner and the standalone
// harness in mod/tests/achieve_core_test.cpp. The daemon still decides every
// unlock; this core only counts, parses the table it is given, and drives the
// page/card state machines. Wire sends, sidecars, widgets and PCM are owner seams.
namespace net::ui::achieve::core {

    constexpr int   kLabN          = 128;
    constexpr int   kMax           = 96;
    constexpr int   kToastQ        = 8;
    constexpr int   kPayloadCap    = 250;
    constexpr int   kMaxReportRows = 81;   // 5 + n*3 <= 250
    constexpr int   kMaxLiveRows   = 83;   // 1 + n*3 <= 250

    constexpr uint64_t kReportFirstMs   = 1500;
    constexpr uint64_t kReportRetryMs   = 8000;
    constexpr uint64_t kReportFailMs    = 500;
    constexpr int      kReportMaxTries  = 12;
    constexpr uint64_t kAskRetryMs      = 3000;
    constexpr uint64_t kPageAskMs       = 500;

    constexpr uint64_t kLivePeriodMs    = 500;
    constexpr uint64_t kLiveRetryMs     = 5000;
    constexpr int      kLiveMaxTries    = 3;

    constexpr uint32_t kSeenUnread      = 0xFFFFFFFFu;
    constexpr uint32_t kSeenNoFile      = 0xFFFFFFFEu;

    constexpr int kGroupN   = 6;
    constexpr int kRows     = 3;
    constexpr int kCols     = 6;
    constexpr int kItemsMax = 40;

    constexpr uint64_t kCardIn   = 300;
    constexpr uint64_t kCardHold = 4500;
    constexpr uint64_t kCardOut  = 900;
    constexpr float    kCardX    = 1440.0f;
    constexpr float    kCardY    = 115.0f;
    constexpr float    kCardS    = 0.8f;
    constexpr float    kCardCX   = 1920.0f;
    constexpr float    kCardIconL = 128.0f;
    constexpr float    kCardTextL = 324.0f;
    constexpr float    kCardBoxW  = 1112.0f;
    constexpr float    kCardTextK = 1.06f;
    constexpr int      kCardPages = 4;

    constexpr int kLSe0        = 64;
    constexpr int kLFists      = 73;
    constexpr int kLOwnMolotov = 74;
    constexpr int kBowTell     = 75;
    constexpr int kDodge       = 76;
    constexpr int kPin         = 77;
    constexpr int kPinLow      = 78;
    constexpr int kLast        = 79;
    constexpr int kStreakStop  = 80;
    constexpr int kHurt        = 81;
    constexpr int kRetal       = 82;
    constexpr int kRevenge     = 83;
    constexpr int kRapid       = 84;
    constexpr int kLockbox     = 85;
    constexpr int kCraft       = 86;
    constexpr int kComeback    = 87;
    constexpr int kXbowTell    = 88;
    constexpr int kCounter     = 89;
    constexpr int kExecuted    = 90;

    struct Row {
        char     id[20]{};
        char     group[10]{};
        char     title[48]{};
        char     desc[112]{};
        uint32_t t = 0;
        int      prog = 0;
        int      goal = 0;
        int      hidden = 0;
    };

    struct Table {
        bool ok = false;
        bool live = false;      // the daemon's last line was "LIVE 1"
        int  n = 0;
        int  unlocked = 0;
        Row  rows[kMax]{};
    };

    struct Toast {
        char id[20]{};
        char title[48]{};
        char desc[112]{};
    };

    inline float Smooth(float t)
    {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        return t * t * (3.0f - 2.0f * t);
    }

    // achievements.wire_text: "ACH <unlocked> <tot>\n" then up to kMax rows of
    // id|group|t|prog|goal|hidden|title|desc, with an optional trailing "LIVE 1".
    // Unknown fields are skipped (a newer daemon cannot break the parse).
    inline bool ParseTable(const char* text, Table& out, bool& live)
    {
        Table t;
        bool sawLive = false;
        if (!text || std::strncmp(text, "ACH ", 4) != 0)
            return false;
        if (std::sscanf(text + 4, "%d %d", &t.unlocked, &t.n) != 2)
            return false;
        t.n = 0;   // the header's total is not the number of parsed rows
        const char* s = std::strchr(text, '\n');
        while (s && *s == '\n' && t.n < kMax) {
            ++s;
            const char* e = std::strchr(s, '\n');
            if (!e) break;
            char line[400];
            const size_t len = std::min<size_t>(static_cast<size_t>(e - s), sizeof line - 1);
            std::memcpy(line, s, len);
            line[len] = 0;
            char* f[8];
            int k = 0;
            f[k++] = line;
            for (char* c = line; *c && k < 8; ++c)
                if (*c == '|') { *c = 0; f[k++] = c + 1; }
            if (k == 1 && std::strcmp(line, "LIVE 1") == 0)
                sawLive = true;
            if (k == 8) {
                Row& r = t.rows[t.n++];
                std::snprintf(r.id, sizeof r.id, "%s", f[0]);
                std::snprintf(r.group, sizeof r.group, "%s", f[1]);
                r.t = static_cast<uint32_t>(std::strtoul(f[2], nullptr, 10));
                r.prog = std::atoi(f[3]);
                r.goal = std::atoi(f[4]);
                r.hidden = std::atoi(f[5]);
                std::snprintf(r.title, sizeof r.title, "%s", f[6]);
                std::snprintf(r.desc, sizeof r.desc, "%s", f[7]);
            }
            s = e;
        }
        t.ok = true;
        t.live = sawLive;
        out = t;
        live = sawLive;
        return true;
    }

    inline const Row* FindRow(const Table& t, const char* id)
    {
        if (!id || !*id) return nullptr;
        for (int i = 0; i < t.n; ++i)
            if (std::strcmp(t.rows[i].id, id) == 0)
                return &t.rows[i];
        return nullptr;
    }

    // Rows with unlock time above the seen sidecar are toasted once. Both queues
    // cap at 8; extra unlocks still advance `newest` so they are not re-toasted.
    inline int QueueNew(const Table& t, uint32_t seen, Toast* out, int cap,
                        uint32_t& newest, int* dropped = nullptr)
    {
        int queued = 0, drop = 0;
        newest = 0;
        for (int i = 0; i < t.n; ++i)
            newest = std::max(newest, t.rows[i].t);
        if (out) {
            for (int i = 0; i < t.n; ++i) {
                const Row& r = t.rows[i];
                if (r.t <= seen) continue;
                if (queued >= cap) { ++drop; continue; }
                std::snprintf(out[queued].id, sizeof out[queued].id, "%s", r.id);
                std::snprintf(out[queued].title, sizeof out[queued].title, "%s", r.title);
                std::snprintf(out[queued].desc, sizeof out[queued].desc, "%s", r.desc);
                ++queued;
            }
        }
        if (dropped) *dropped = drop;
        return queued;
    }

    inline bool Note(int* cnt, int lab, int n)
    {
        if (!cnt || lab <= 0 || lab >= kLabN || n <= 0)
            return false;
        cnt[lab] += n;
        return true;
    }

    // ext ach_cnt_now(): labels 1..63 come from the mission tally, 64..127 from
    // the achievement-only tally. Clamped to u16 before it goes on the wire.
    inline int CountNow(const int* ach, const uint16_t* mis, int misN, int id)
    {
        if (id <= 0 || id >= kLabN) return 0;
        int v = id >= 64 ? (ach ? ach[id] : 0)
                         : (mis && id < misN ? static_cast<int>(mis[id]) : 0);
        return std::min(65535, std::max(0, v));
    }

    // 0xA9: [u32 seq][u8 n][n x (u8 label,u16 count LE)]. The sequence is owned
    // by the caller so a retry resends the exact same payload.
    inline int BuildReport(const int* ach, const uint16_t* mis, int misN,
                           uint32_t seq, uint8_t* out, int cap)
    {
        if (!out || cap < 5) return 0;
        std::memcpy(out, &seq, 4);
        int n = 0, o = 5;
        for (int id = 1; id < kLabN && o + 3 <= cap && n < kMaxReportRows; ++id) {
            const int v = CountNow(ach, mis, misN, id);
            if (v <= 0) continue;
            const uint16_t c = static_cast<uint16_t>(v);
            out[o++] = static_cast<uint8_t>(id);
            std::memcpy(out + o, &c, 2);
            o += 2;
            ++n;
        }
        out[4] = static_cast<uint8_t>(n);
        return o;
    }

    // ach_live_skip(): ids 21..24 and 33 are match-report-only counters.
    inline bool LiveSkip(int id)
    {
        return id == 21 || id == 22 || id == 23 || id == 24 || id == 33;
    }

    // 0xAB: [u8 n][n x (u8 label,u16 count LE)], no sequence.
    inline int BuildLive(const int* ach, const uint16_t* mis, int misN,
                         uint8_t* out, int cap)
    {
        if (!out || cap < 1) return 0;
        int n = 0, o = 1;
        for (int id = 1; id < kLabN && o + 3 <= cap && n < kMaxLiveRows; ++id) {
            if (LiveSkip(id)) continue;
            const int v = CountNow(ach, mis, misN, id);
            if (v <= 0) continue;
            const uint16_t c = static_cast<uint16_t>(v);
            out[o++] = static_cast<uint8_t>(id);
            std::memcpy(out + o, &c, 2);
            o += 2;
            ++n;
        }
        out[0] = static_cast<uint8_t>(n);
        return o;
    }

    // tools/ext/ach_live.h: the 44 rules the daemon permits as live unlocks.
    struct LiveRule {
        const char* id;
        uint8_t     kind;   // 0 lifetime, 1 distinct Special Execution weapons, 2 this match
        uint8_t     lab;
        uint16_t    goal;
    };

    inline constexpr LiveRule kLive[] = {
        {"se_crowbar", 0, 64, 50}, {"se_hammer", 0, 65, 50}, {"se_machete", 0, 66, 50},
        {"se_hatchet", 0, 67, 50}, {"se_bat", 0, 68, 50}, {"se_2x4", 0, 69, 50},
        {"se_pipe", 0, 70, 50}, {"se_wrench", 0, 71, 50}, {"se_club", 0, 72, 50},
        {"se_fists", 0, 73, 50}, {"shiv_25", 0, 12, 75}, {"melee_all", 1, 0, 9},
        {"bow_50", 0, 20, 250}, {"bow_tell", 0, 75, 1}, {"xbow_tell", 0, 88, 1},
        {"revolver_10", 0, 49, 150}, {"shorty_25", 0, 16, 200},
        {"hunting_50", 0, 17, 250}, {"long_50", 0, 13, 250},
        {"molotov_25", 0, 8, 50}, {"nail_25", 0, 9, 50}, {"head_100", 0, 4, 250},
        {"dodge_50", 0, 76, 250}, {"counter_100", 0, 89, 100},
        {"dodge_5", 2, 76, 10}, {"pin_10", 0, 77, 25}, {"pin_low", 0, 78, 1},
        {"stopper_10", 0, 80, 10}, {"hurt_25", 0, 81, 50},
        {"rapid_25", 0, 84, 100}, {"payback_25", 0, 82, 75},
        {"avenger_25", 0, 83, 75}, {"last_alive", 0, 79, 10},
        {"down_100", 0, 1, 500}, {"down_1000", 0, 1, 2500},
        {"exec_50", 0, 7, 250}, {"revive_50", 0, 3, 50},
        {"heal_50", 0, 6, 100}, {"gift_25", 0, 10, 50},
        {"mark_200", 0, 5, 1000}, {"craft_100", 0, 86, 300},
        {"darwin", 0, 74, 1}, {"reaper", 0, 90, 100},
        {"lockbox_10", 0, 85, 25},
    };
    inline constexpr int kLiveN = static_cast<int>(sizeof kLive / sizeof kLive[0]);
    inline constexpr uint8_t kLiveSE[] = { 64, 65, 66, 67, 68, 69, 70, 71, 72 };
    inline constexpr int kLiveSEN = static_cast<int>(sizeof kLiveSE / sizeof kLiveSE[0]);

    inline bool LiveHit(const Table& t, const int* ach, const uint16_t* mis, int misN,
                        const uint16_t* base, int i)
    {
        if (i < 0 || i >= kLiveN || !t.ok) return false;
        const LiveRule& rule = kLive[i];
        const Row* r = FindRow(t, rule.id);
        if (!r || r->t) return false;           // already unlocked
        const int now = CountNow(ach, mis, misN, rule.lab);
        if (rule.kind == 2)
            return now >= static_cast<int>(rule.goal);
        if (rule.kind == 0) {
            const int b = base && rule.lab < kLabN ? base[rule.lab] : 0;
            return r->prog + now - b >= static_cast<int>(rule.goal);
        }
        int n = 0;
        for (int j = 0; j < kLiveSEN; ++j) {
            const int w = kLiveSE[j];
            bool had = false;
            for (int k = 0; k < kLiveN; ++k)
                if (kLive[k].kind == 0 && kLive[k].lab == w) {
                    const Row* q = FindRow(t, kLive[k].id);
                    had = q && (q->prog > 0 || q->t);
                    break;
                }
            if (had || CountNow(ach, mis, misN, w) > 0) ++n;
        }
        return n >= static_cast<int>(rule.goal);
    }

    inline int NextLive(const Table& t, const int* ach, const uint16_t* mis, int misN,
                        const uint16_t* base, const uint8_t* tries, const uint64_t* at,
                        uint64_t now)
    {
        if (!t.live || !tries || !at) return -1;
        for (int i = 0; i < kLiveN; ++i)
            if (tries[i] < kLiveMaxTries
                && (at[i] == 0 || now - at[i] > kLiveRetryMs)
                && LiveHit(t, ach, mis, misN, base, i))
                return i;
        return -1;
    }

    // ---- ACHIEVEMENTS page (clanach) -----------------------------------------

    inline const char* const kGroupKey[kGroupN] = {
        "melee", "weapon", "skill", "stats", "clan", "secret"
    };
    inline const char* const kGroupLabel[kGroupN] = {
        "MELEE MASTERY", "WEAPON FEATS", "SKILL", "MILESTONES", "CLAN", "HIDDEN"
    };

    struct Group {
        char key[10]{};
        char label[24]{};
        int  n = 0;
        int  item[kItemsMax]{};
    };

    struct UiState {
        bool     on = false;
        int      groups = 0;
        int      fr = 0, fc = 0, want = 0, row0 = 0;
        int      off[kGroupN]{};
        int      hvR = -1, hvC = -1, hvTab = -1, hvPb = -1;
        uint64_t seenT = 0;
        int      ver = -1;
        Group    g[kGroupN];
    };

    inline void ClampFocus(UiState& ui);

    inline bool BuildGroups(UiState& ui, const Table& t)
    {
        Group built[kGroupN];
        int count = 0;
        for (int k = 0; k < kGroupN; ++k) {
            Group& d = built[count];
            d = Group{};
            std::snprintf(d.key, sizeof d.key, "%s", kGroupKey[k]);
            std::snprintf(d.label, sizeof d.label, "%s", kGroupLabel[k]);
            for (int i = 0; i < t.n && d.n < kItemsMax; ++i)
                if (std::strcmp(t.rows[i].group, d.key) == 0)
                    d.item[d.n++] = i;
            if (d.n) ++count;
        }
        // Compact so g[0..groups) are all focusable rows (an empty group is
        // not a row in the ext's grid).
        for (int k = 0; k < count; ++k) ui.g[k] = built[k];
        for (int k = count; k < kGroupN; ++k) ui.g[k] = Group{};
        ui.groups = count;
        ClampFocus(ui);
        return ui.groups > 0;
    }

    inline void ClampFocus(UiState& ui)
    {
        if (ui.groups <= 0) { ui.fr = ui.fc = ui.want = ui.row0 = 0; return; }
        ui.fr = std::max(0, std::min(ui.groups - 1, ui.fr));
        ui.fc = std::max(0, std::min(ui.g[ui.fr].n - 1, ui.fc));
        ui.want = std::max(0, ui.fc);
        ui.row0 = std::max(0, std::min(ui.groups - kRows, ui.row0));
        for (int k = 0; k < kGroupN; ++k)
            ui.off[k] = std::max(0, std::min(ui.g[k].n - kCols, ui.off[k]));
        if (ui.fc < ui.off[ui.fr]) ui.off[ui.fr] = ui.fc;
        if (ui.fc >= ui.off[ui.fr] + kCols) ui.off[ui.fr] = ui.fc - (kCols - 1);
    }

    // Up / down keep the absolute screen column, not the row-relative index.
    inline bool MoveRow(UiState& ui, int d)
    {
        if (ui.groups <= 0) return false;
        const int old = ui.fr;
        const int target = std::max(0, std::min(ui.groups - 1, old + d));
        if (target == old) return false;
        ui.fr = target;
        ui.fc = std::max(0, std::min(ui.g[target].n - 1,
                                     ui.want - ui.off[old] + ui.off[target]));
        ClampFocus(ui);
        return true;
    }

    inline bool MoveCol(UiState& ui, int d)
    {
        if (ui.groups <= 0) return false;
        const int target = std::max(0, std::min(ui.g[ui.fr].n - 1, ui.fc + d));
        if (target == ui.fc) return false;
        ui.fc = target;
        ui.want = target;
        ClampFocus(ui);
        return true;
    }

    inline void FocusHover(UiState& ui, int r, int c)
    {
        const int row = ui.row0 + r;
        if (r < 0 || r >= kRows || row >= ui.groups || c < 0) return;
        const int index = ui.off[row] + c;
        if (index >= ui.g[row].n) return;
        ui.fr = row;
        ui.fc = index;
        ui.want = c;
        ClampFocus(ui);
    }

    inline int FocusedIndex(const UiState& ui)
    {
        if (ui.fr < 0 || ui.fr >= ui.groups) return -1;
        const int i = ui.g[ui.fr].item[ui.fc];
        return i >= 0 ? i : -1;
    }

    inline int HoverSignature(const UiState& ui)
    {
        return ui.hvR * 4096 + ui.hvC * 64 + (ui.hvTab + 4) * 8 + ui.hvPb + 1;
    }

    // ---- unlock card -----------------------------------------------------------

    struct CardItem {
        char id[20]{};
        char title[48]{};
        char desc[112]{};
    };

    struct CardState {
        bool      active = false;
        uint64_t  t0 = 0;
        int       n = 0;
        int       page = 0;
        bool      snd = false;
        float     tw = 0.0f;
        CardItem  q[kToastQ];
    };

    inline bool EnqueueCard(CardState& c, const Toast& t)
    {
        if (c.n >= kToastQ) return false;
        CardItem& d = c.q[c.n++];
        std::snprintf(d.id, sizeof d.id, "%s", t.id);
        std::snprintf(d.title, sizeof d.title, "%s", t.title);
        std::snprintf(d.desc, sizeof d.desc, "%s", t.desc);
        return true;
    }

    inline void StartCard(CardState& c, uint64_t now, int page)
    {
        if (c.active || c.n <= 0) return;
        c.active = true;
        c.t0 = now;
        c.page = page;
        c.snd = false;
    }

    inline float CardAlpha(uint64_t now, uint64_t t0)
    {
        const uint64_t e = now >= t0 ? now - t0 : 0;
        if (e < kCardIn)
            return static_cast<float>(e) / static_cast<float>(kCardIn);
        if (e < kCardIn + kCardHold)
            return 1.0f;
        const float f = 1.0f - static_cast<float>(e - kCardIn - kCardHold)
                              / static_cast<float>(kCardOut);
        return std::max(0.0f, std::min(1.0f, f));
    }

    inline float CardSlide(uint64_t now, uint64_t t0)
    {
        const uint64_t e = now >= t0 ? now - t0 : 0;
        if (e >= kCardIn) return 0.0f;
        const float u = 1.0f - static_cast<float>(e) / static_cast<float>(kCardIn);
        return u * u * u;
    }

    inline bool CardDone(uint64_t now, uint64_t t0)
    {
        return now >= t0 && now - t0 >= kCardIn + kCardHold + kCardOut;
    }

    inline bool AdvanceCard(CardState& c, uint64_t now)
    {
        if (!c.active || !CardDone(now, c.t0)) return false;
        for (int i = 1; i < c.n; ++i) c.q[i - 1] = c.q[i];
        --c.n;
        c.active = c.n > 0;
        c.t0 = c.active ? now : 0;
        c.snd = false;
        return true;
    }

    inline int DropCard(CardState& c, const char* id)
    {
        if (!id) return 0;
        int dropped = 0;
        for (int i = c.n - 1; i >= 0; --i)
            if (std::strcmp(c.q[i].id, id) == 0) {
                for (int j = i; j + 1 < c.n; ++j) c.q[j] = c.q[j + 1];
                --c.n;
                ++dropped;
            }
        if (c.n == 0) {
            c.active = false;
            c.t0 = 0;
            c.snd = false;
        } else if (c.active) {
            c.snd = true;   // the active card was replaced: do not chime twice
        }
        return dropped;
    }

} // namespace net::ui::achieve::core
