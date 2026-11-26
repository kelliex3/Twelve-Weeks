#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Pure rules of tools/ext/mfnr.c and feature_matchflow (t2r-ext.c): the map
// vote, the booster choice (stage 2) and the loading text (stage 3). The native
// owner in mfnr.cpp owns the daemon RPC, the page stack, the widgets, the input
// and the hook; nothing here touches any of them. Wire bytes are pinned in
// net-protocol.hpp, never renumbered.

namespace net::ui::mfnr::core {

    inline constexpr int kVoteN      = 24;   // MF_VOTE_N
    inline constexpr int kGrid       = 20;   // MF_GRID
    inline constexpr int kGridCols   = 5;
    inline constexpr int kPlayers    = 8;
    inline constexpr int kMapCount   = 25;   // MF_NMAPS (pack maps are unported)
    inline constexpr uint64_t kPollMs = 1000;
    inline constexpr uint64_t kResendMs = 900;
    inline constexpr int kMaxResend = 4;

    // build-191 mfnr.c geometry (widget units, 3840 x 2160).
    inline constexpr float kLeft       = 148.0f;
    inline constexpr float kRightCol   = 2940.0f;
    inline constexpr float kRightEdge  = 3700.0f;
    inline constexpr float kColRight   = 2740.0f;
    inline constexpr float kHeaderY    = 420.0f;
    inline constexpr float kTileW      = 840.0f;
    inline constexpr float kTileImgH   = 472.0f;
    inline constexpr float kTileStrip  = 130.0f;
    inline constexpr float kGridW      = 440.0f;
    inline constexpr float kGridImgH   = 247.0f;
    inline constexpr float kGridStrip  = 84.0f;
    inline constexpr float kRosterMapW = 440.0f;
    inline constexpr float kGridMapW   = 250.0f;
    inline constexpr float kRosterNameW= 581.0f;
    inline constexpr float kGridNameW  = 271.0f;
    inline constexpr float kRosterRowW = 1260.0f;
    inline constexpr float kGridRowW   = 760.0f;
    inline constexpr float kMapPt      = 30.0f;
    inline constexpr float kNamePt     = 36.0f;
    inline constexpr int kTiles        = 3;

    // ext mf_t, field for field. `pk` / `pw` are the roster ranks carried by the
    // 0xEB reply; `po` is the option each player voted for (-1 = none / old server).
    struct Vote {
        int  ok = 0, st = 0, n = 0;
        int  m[kVoteN]{};
        int  v[kVoteN]{};
        int  my = -1, w = -1, pc = 0;
        char p[kPlayers][21]{};
        char pv[kPlayers]{};
        int  pk[kPlayers]{};
        int  pw[kPlayers]{};
        int  po[kPlayers]{};
    };

    inline void Reset(Vote& m) noexcept
    {
        m = Vote{};
        for (int i = 0; i < kPlayers; ++i) m.pk[i] = m.po[i] = -1;
    }

    // mf_parse's key/value grammar: "key=value key=value ...", value is a
    // non-space run, key is at most 15 chars. Unknown keys are ignored. The
    // clamps are the ext's exact ones (note: `pc` has no low clamp, and `n`
    // stops at kGrid, not kVoteN).
    inline bool Parse(Vote& out, const char* text) noexcept
    {
        Vote m{};
        Reset(m);
        if (!text) return false;
        const char* t = text;
        for (;;) {
            while (*t && std::isspace(static_cast<unsigned char>(*t))) ++t;
            const char* ks = t;
            while (*t && *t != '=' && !std::isspace(static_cast<unsigned char>(*t))) ++t;
            const size_t kn = static_cast<size_t>(t - ks);
            if (*t != '=' || kn == 0 || kn > 15) break;
            char k[16];
            std::memcpy(k, ks, kn);
            k[kn] = 0;
            ++t;
            const char* vs = t;
            while (*t && !std::isspace(static_cast<unsigned char>(*t))) ++t;
            const size_t vn = static_cast<size_t>(t - vs);
            if (vn == 0) break;
            char v[32];
            const size_t vc = vn > 31 ? 31 : vn;
            std::memcpy(v, vs, vc);
            v[vc] = 0;

            const int x = std::atoi(v);
            if (!std::strcmp(k, "st")) m.st = x;
            else if (!std::strcmp(k, "n"))
                m.n = x < 0 ? 0 : (x < kGrid ? x : kGrid);
            else if (!std::strcmp(k, "my")) m.my = x;
            else if (!std::strcmp(k, "w")) m.w = x;
            else if (!std::strcmp(k, "pc")) m.pc = x < kPlayers ? x : kPlayers;
            else if ((k[0] == 'm' || k[0] == 'v')
                     && k[1] >= '0' && k[1] <= '9'
                     && (!k[2] || (k[2] >= '0' && k[2] <= '9' && !k[3]))) {
                const int oi = std::atoi(k + 1);
                if (oi >= 0 && oi < kVoteN) {
                    if (k[0] == 'm') m.m[oi] = x;
                    else             m.v[oi] = x;
                }
            } else if (k[0] == 'p' && k[1] == 'v'
                       && k[2] >= '0' && k[2] <= '7' && !k[3]) {
                m.pv[k[2] - '0'] = static_cast<char>(x);
            } else if (k[0] == 'p' && k[1] == 'o'
                       && k[2] >= '0' && k[2] <= '7' && !k[3]) {
                m.po[k[2] - '0'] = x;
            } else if (k[0] == 'p' && (k[1] == 'k' || k[1] == 'w')
                       && k[2] >= '0' && k[2] <= '7' && !k[3]) {
                if (k[1] == 'k') m.pk[k[2] - '0'] = x;
                else             m.pw[k[2] - '0'] = x;
            } else if (k[0] == 'p' && k[1] >= '0' && k[1] <= '7' && !k[2]) {
                const int r = k[1] - '0';
                for (int i = 0; i < 20 && i < static_cast<int>(vc); ++i)
                    m.p[r][i] = v[i] == '_' ? ' ' : v[i];
            }
        }
        // ext mf_parse always publishes a state, even when the first sscanf
        // fails: an empty reply is a valid "no vote yet" state.
        m.ok = 1;
        out = m;
        return true;
    }

    struct MapDef { const char* name; const char* img; };

    // MF_MAPS, built-ins 0..24. Pack maps start at 25 and are not ported, so
    // out-of-range ids fall back to map 0 exactly like MF_MAP().
    inline constexpr MapDef kMaps[kMapCount] = {
        {"Forest", "forest"}, {"Gas Station", "gas-station"},
        {"School", "school"}, {"Shipyard", "shipyard"},
        {"Resort", "resort"}, {"Seraphite Camp", "seraphite-camp"},
        {"Houses", "houses"}, {"Hospital", "hospital"},
        {"Hardware Store", "hardware"}, {"Motel", "motel"},
        {"Hillcrest", "hillcrest-stores"}, {"TV Station", "tv-station"},
        {"Train Station", "train-station"}, {"Downtown", "downtown"},
        {"Lodge", "lodge"}, {"Jackson Suburbs", "jackson-suburbs"},
        {"Suburbs", "outpost"}, {"Bookstore (Part I)", "p1-bookstore"},
        {"Hillside", "wilds-houses"}, {"Capitol Hill Shops", "capitol-shops"},
        {"Convention Center", "into-the-city"}, {"Overpass", "overpass"},
        {"Chase", "chase"}, {"Supermarket", "jackson-market"},
        {"Plaza (Part I)", "p1-plaza"}
    };

    [[nodiscard]] inline int MapIndex(int i) noexcept
    {
        return i >= 0 && i < kMapCount ? i : 0;
    }

    [[nodiscard]] inline const char* MapName(int i) noexcept
    {
        return kMaps[MapIndex(i)].name;
    }

    [[nodiscard]] inline const char* MapImage(int i) noexcept
    {
        return kMaps[MapIndex(i)].img;
    }

    [[nodiscard]] inline bool IsPartOne(int i) noexcept
    {
        return std::strncmp(MapImage(i), "p1-", 3) == 0;
    }

    inline void MapImagePath(char* out, size_t cap, int i) noexcept
    {
        if (out && cap)
            std::snprintf(out, cap, "streaming/menus/to1/map-pvp-%s.png", MapImage(i));
    }

    // modepick's label is unported; the ext default is the honest fallback.
    [[nodiscard]] inline const char* ModeLabel() noexcept { return "SUPPLY RAID"; }

    // feature_matchflow's countdown -> stage table. `code` is the lobby's
    // +0x1b8 int while phase == 2 (phase / substate gating lives in the owner).
    [[nodiscard]] inline int StageOf(int code) noexcept
    {
        if (code < 100) return 0;
        return code >= 200 ? 3 : (code >= 160 ? 1 : (code >= 130 ? 2 : 0));
    }

    [[nodiscard]] inline bool FlowActive(int code) noexcept { return code >= 100; }

    // The ext match horn's LOADING request. The raw +0x1b8 `secs` must have
    // been seen at a flow stage (>= 100) before it falls to <= 1 while the
    // lobby is still in phase 2 / state 2. Once the latch is set it survives
    // the code falling to 0; only leaving that phase / state cancels it.
    [[nodiscard]] inline bool HornWant(bool sawStage, int code, int phase,
                                       int matchState) noexcept
    {
        return sawStage && phase == 2 && matchState == 2 && code <= 1;
    }

    [[nodiscard]] inline bool HornCanceled(int phase, int matchState) noexcept
    {
        return phase != 2 || matchState != 2;
    }

    [[nodiscard]] inline int CountdownFor(int code, int stage) noexcept
    {
        const int base = stage == 1 ? 160 : (stage == 2 ? 130 : 200);
        const int s = code - base;
        return s > 0 ? s : 0;
    }

    // The vote state can decide locally even when a pack map is not installed;
    // the ext's decided gate needs a known map. Pack maps are absent here.
    [[nodiscard]] inline bool Decided(const Vote& m) noexcept
    {
        return m.ok && m.st == 2 && m.w >= 0 && m.w < kMapCount;
    }

    // g_mf_lv / g_mf_lvmap optimism: our last click counts at once. A stale
    // option (same index, different map) is ignored; the old vote is only
    // decremented when the server still credits it.
    inline bool ApplyLocalVote(Vote& m, int lv, int lvmap) noexcept
    {
        if (lv < 0 || lv >= m.n || m.st != 1 || m.my == lv
            || m.m[lv] != lvmap) return false;
        if (m.my >= 0 && m.my < m.n && m.v[m.my] > 0) --m.v[m.my];
        if (m.v[lv] < kPlayers) ++m.v[lv];
        m.my = lv;
        return true;
    }

    [[nodiscard]] inline int PromptKey(bool pad, bool grid, bool can) noexcept
    {
        return (pad ? 1 : 0) | (grid ? 2 : 0) | (can ? 4 : 0);
    }

    [[nodiscard]] inline int PromptKeyOf(const Vote& m, bool pad) noexcept
    {
        return PromptKey(pad, m.ok && m.n > 3, m.ok && m.st == 1);
    }

    [[nodiscard]] inline bool PromptHit(int key, int hover) noexcept
    {
        return (key & 4) != 0 && hover == 1;
    }

    [[nodiscard]] inline bool GridMode(const Vote& m) noexcept
    {
        return m.ok && m.st >= 1 && m.n > 0 && m.n > 3;
    }

    [[nodiscard]] inline bool HaveVote(const Vote& m) noexcept
    {
        return m.ok && m.st >= 1 && m.n > 0;
    }

    // mf_team_tab passes: 1 = our team, 0 = unresolved, 2 = enemy. The roster
    // order puts our team first, unresolved next, enemy last.
    [[nodiscard]] inline int TeamPass(int tab) noexcept
    {
        return tab == 1 ? 0 : (tab == 0 ? 1 : 2);
    }

    inline int RosterOrder(const int* tabs, int n, int* ord) noexcept
    {
        int k = 0;
        for (int pass = 0; pass < 3; ++pass)
            for (int i = 0; i < kPlayers && i < n; ++i)
                if (tabs && TeamPass(tabs[i]) == pass) ord[k++] = i;
        return k;
    }

    [[nodiscard]] inline int VotedCount(const Vote& m) noexcept
    {
        int nv = 0;
        for (int i = 0; i < kPlayers && i < m.pc; ++i)
            if (m.pv[i]) ++nv;
        return nv;
    }

    inline void VotedText(char* out, size_t cap, const Vote& m) noexcept
    {
        if (out && cap)
            std::snprintf(out, cap, "%d | %d VOTED", VotedCount(m), m.pc);
    }

    // mfnr_cell's count / fraction. `pc` is raised to the highest vote count
    // and to one, so the bar never divides by zero.
    struct CellBar {
        int  votes = 0;
        int  pc = 1;
        bool grey = false;
        float frac = 0.0f;
    };

    [[nodiscard]] inline CellBar CellOf(int votes, int playerCount) noexcept
    {
        CellBar c;
        c.votes = votes < 0 ? 0 : votes;
        c.pc = playerCount < c.votes ? c.votes : playerCount;
        if (c.pc < 1) c.pc = 1;
        c.grey = c.votes == 0;
        c.frac = static_cast<float>(c.votes) / static_cast<float>(c.pc);
        if (c.frac > 1.0f) c.frac = 1.0f;
        return c;
    }

    inline void CountText(char* out, size_t cap, const CellBar& c) noexcept
    {
        if (!out || !cap) return;
        if (c.grey)
            std::snprintf(out, cap, "<font color=\"#8C8C8C\">%d | %d</font>",
                          c.votes, c.pc);
        else
            std::snprintf(out, cap, "%d | %d", c.votes, c.pc);
    }

    [[nodiscard]] inline bool CellFocused(const Vote& m, int k, int focus) noexcept
    {
        return m.st == 2 ? (k >= 0 && k < m.n && m.m[k] == m.w) : k == focus;
    }

    [[nodiscard]] inline bool CellMine(const Vote& m, int k) noexcept
    {
        return m.my == k;
    }

    // The live server draws three tiles, the test server draws the full grid.
    // Both use the same option indices; the grid can show more than `n` cells.
    [[nodiscard]] inline bool CellVisible(const Vote& m, int k) noexcept
    {
        if (!HaveVote(m) || k < 0 || k >= kGrid) return false;
        return k < m.n;
    }

    // Own name / tag match (mfnr_is_me) and the accent row colour. Pure ASCII
    // fold: the ext's _stricmp is locale-independent for the roster's wire names.
    [[nodiscard]] inline bool EqualsFold(const char* a, const char* b) noexcept
    {
        if (!a || !b) return false;
        for (; *a && *b; ++a, ++b) {
            const int x = std::tolower(static_cast<unsigned char>(*a));
            const int y = std::tolower(static_cast<unsigned char>(*b));
            if (x != y) return false;
        }
        return *a == 0 && *b == 0;
    }

    [[nodiscard]] inline bool IsMe(const char* name, const char* local) noexcept
    {
        if (!name || !name[0] || !local || !local[0]) return false;
        const size_t a = std::strlen(name), b = std::strlen(local);
        if (a == b) return EqualsFold(name, local);
        if (a <= b + 1 || (name[a - b - 1] != ' ' && name[a - b - 1] != ']'))
            return false;
        return EqualsFold(name + (a - b), local);
    }

    [[nodiscard]] inline int VoteOf(const Vote& m, int slot, bool me) noexcept
    {
        if (slot < 0 || slot >= kPlayers) return -1;
        if (me && m.my >= 0) return m.my;
        return m.po[slot];
    }

    [[nodiscard]] inline bool RosterMapVisible(const Vote& m, int option,
                                               bool voted, bool me) noexcept
    {
        return option >= 0 && option < m.n && option < kVoteN && (voted || me);
    }

    inline void VotedMapText(char* out, size_t cap, const Vote& m,
                             int option, bool voted, bool me) noexcept
    {
        if (!out || !cap) return;
        if (!RosterMapVisible(m, option, voted, me)) { out[0] = 0; return; }
        const char* nm = MapName(m.m[option]);
        if (me) std::snprintf(out, cap, "<font color=\"#D9D86A\">%s</font>", nm);
        else    std::snprintf(out, cap, "%s", nm);
    }

    // mfnr_clan_fill's week block and the three survivor bars.
    inline void WeekOf(int day, int& week, int& dayOfWeek) noexcept
    {
        // ext mfnr_clan_fill: no low clamp (a malformed negative day keeps the
        // engine's integer division behaviour), high clamp at day 83.
        const int d = day < 84 ? day : 83;
        week = d / 7 + 1;
        dayOfWeek = d % 7 + 1;
    }

    [[nodiscard]] inline int SurvivorTotal(int h, int hu, int s) noexcept
    {
        return h + hu + s;
    }

    [[nodiscard]] inline float SurvivorFrac(int count, int total) noexcept
    {
        if (total <= 0) return 0.0f;
        float f = static_cast<float>(count) / static_cast<float>(total);
        return f > 1.0f ? 1.0f : (f < 0.0f ? 0.0f : f);
    }

    // ext mfnr_timer: the longest countdown seen this vote is the bar's full
    // length; a low frame must not shrink it.
    inline int TimerMax(int& total, int secs) noexcept
    {
        if (secs < 0) secs = 0;
        if (secs > total) total = secs;
        return secs;
    }

    [[nodiscard]] inline float TimerFrac(int total, int secs) noexcept
    {
        if (total <= 0) return 0.0f;
        float f = static_cast<float>(secs) / static_cast<float>(total);
        return f > 1.0f ? 1.0f : (f < 0.0f ? 0.0f : f);
    }

    // The ext re-sends a vote only when a later state still says a different
    // (or no) vote for the same option -> same map, at most four times.
    // `voted` is the option we sent (-1 when nothing was sent), `pending` is a
    // newer click waiting to go out, `open` is the voting stage, `busy` is an
    // in-flight daemon request, and `my` is the server's current `my`.
    [[nodiscard]] inline bool NeedResend(int voted, bool pending, bool open,
                                         bool busy, int my, int votedMap,
                                         int map, int n, uint32_t version,
                                         uint32_t sentVersion, int resends) noexcept
    {
        return voted >= 0 && !pending && open && !busy && my != voted
            && voted < n && votedMap >= 0 && votedMap == map
            && version != sentVersion && resends < kMaxResend;
    }

    // ------------------------------------------------------------------ meta
    // 0xE8 [token] read / 0xEE [token][u8 id] pick -> 0xE9 "key=value ...".
    // One field for one field of the ext's meta_t; mr[] is indexed by mission
    // id (1..63) and mg[] holds the running mission's three tier goals.
    inline constexpr int kMisN = 64;

    struct Meta {
        int ok = 0;
        int bc[8]{};
        int bp = 0, ba = 0, bn = 0;
        int mi = 0, mp = 0, ml = 0;
        int mr[kMisN]{};
        int p[3]{};
        int lmi = 0, lmp = 0, lmt = 0, lms = 0, lmb = 0, lml = 0, lmd = -1;
        int mf = 0, mz = 0, lmz = 0, lmx = 0;
        int mg[3]{};
        int mev = 0;
    };

    inline void Reset(Meta& m) noexcept { m = Meta{}; }

    // meta_parse's grammar is the same key/value loop as mf_parse, but its set
    // of keys is the clan meta table. An empty reply is still a published
    // (ok=1) state, exactly as in the ext.
    inline bool ParseMeta(Meta& out, const char* text) noexcept
    {
        Meta m{};
        m.lmd = -1;
        if (!text) return false;
        const char* t = text;
        for (;;) {
            while (*t && std::isspace(static_cast<unsigned char>(*t))) ++t;
            const char* ks = t;
            while (*t && *t != '=' && !std::isspace(static_cast<unsigned char>(*t))) ++t;
            const size_t kn = static_cast<size_t>(t - ks);
            if (*t != '=' || kn == 0 || kn > 15) break;
            char k[16];
            std::memcpy(k, ks, kn);
            k[kn] = 0;
            ++t;
            const char* vs = t;
            while (*t && !std::isspace(static_cast<unsigned char>(*t))) ++t;
            const size_t vn = static_cast<size_t>(t - vs);
            if (vn == 0) break;
            char v[32];
            const size_t vc = vn > 31 ? 31 : vn;
            std::memcpy(v, vs, vc);
            v[vc] = 0;
            const int x = std::atoi(v);

            if (k[0] == 'b' && k[1] == 'c' && k[2] >= '1' && k[2] <= '7' && !k[3])
                m.bc[k[2] - '0'] = x;
            else if (!std::strcmp(k, "bp")) m.bp = x;
            else if (!std::strcmp(k, "ba")) m.ba = x;
            else if (!std::strcmp(k, "bn")) m.bn = x;
            else if (!std::strcmp(k, "mi")) m.mi = x;
            else if (!std::strcmp(k, "mp")) m.mp = x;
            else if (!std::strcmp(k, "ml")) m.ml = x;
            else if (!std::strcmp(k, "mz")) m.mz = x;
            else if (!std::strcmp(k, "lmz")) m.lmz = x;
            else if (!std::strcmp(k, "lmx")) m.lmx = x;
            else if (k[0] == 'm' && k[1] == 'g' && k[2] >= '1' && k[2] <= '3' && !k[3])
                m.mg[k[2] - '1'] = x;
            else if (!std::strcmp(k, "mev")) m.mev = x;
            else if (k[0] == 'm' && k[1] == 'r') {
                const int id = std::atoi(k + 2);
                if (id > 0 && id < kMisN) m.mr[id] = x;
            } else if (k[0] == 'p' && k[1] >= '1' && k[1] <= '3' && !k[2]) {
                m.p[k[1] - '1'] = (x >= 0 && x < kMisN) ? x : 0;
            } else if (!std::strcmp(k, "lmi")) m.lmi = x;
            else if (!std::strcmp(k, "lmp")) m.lmp = x;
            else if (!std::strcmp(k, "lmt")) m.lmt = x;
            else if (!std::strcmp(k, "lms")) m.lms = x;
            else if (!std::strcmp(k, "lmb")) m.lmb = x;
            else if (!std::strcmp(k, "lml")) m.lml = x;
            else if (!std::strcmp(k, "lmd")) m.lmd = x;
            else if (!std::strcmp(k, "mf")) m.mf = (x >= 0 && x < 100) ? x : 0;
        }
        m.ok = 1;
        out = m;
        return true;
    }

    // -------------------------------------------------------------- boosters
    struct BoosterDef { const char* name; const char* desc; const char* icon; };

    // MF_BOOSTER / MF_BOOSTER_ICON. Id 0 is "Nothing"; ids 1..7 are owned
    // one-use boosters. Pack data is absent, so these are the 2013 rows.
    inline constexpr BoosterDef kBoosters[8] = {
        {"Nothing", "Earn One-Use Boosters by growing your clan population and completing Missions.", "nothing"},
        {"Starting Ammo Multiplier", "Spawn with 25% more ammo for your firearms. Used up when the match starts.", "ammo-start"},
        {"Cheaper Ammo", "Ammo costs 25% fewer parts for the whole match. Used up when the match starts.", "ammo-cost"},
        {"Cheaper Armor", "Armor costs 25% fewer parts for the whole match. Used up when the match starts.", "armor-cost"},
        {"Cheaper Purchased Weapons", "Purchased weapons cost 25% fewer parts for the whole match. Used up when the match starts.", "power-cost"},
        {"Starting Small Firearm Upgrade", "Your handgun starts the match upgraded. Not available yet.", "short-upgrade"},
        {"Starting Large Firearm Upgrade", "Your primary starts the match upgraded. Not available yet.", "long-upgrade"},
        {"Increased Part Earnings", "Earn 10% more parts from everything you do. Used up when the match starts.", "parts-multiplier"},
    };

    // mf_bo_list: slot 0 is Nothing, then owned boosters (count > 0, or the
    // one already picked) in id order. Returns the number of live slots (1..8).
    inline int BoosterList(const Meta& m, int* ids) noexcept
    {
        if (!ids) return 0;
        int n = 1;
        ids[0] = 0;
        for (int k = 1; k < 8; ++k)
            if (m.ok && (m.bc[k] > 0 || m.bp == k)) ids[n++] = k;
        return n;
    }

    [[nodiscard]] inline int BoosterSlotId(const int* ids, int n, int slot) noexcept
    {
        if (!ids || slot < 0 || slot >= n) return 0;
        return ids[slot];
    }

    [[nodiscard]] inline int BoosterSlotOf(const int* ids, int n, int id) noexcept
    {
        if (!ids) return 0;
        for (int s = 0; s < n; ++s)
            if (ids[s] == id) return s;
        return 0;
    }

    inline void BoosterCountText(char* out, size_t cap, int count) noexcept
    {
        if (out && cap) std::snprintf(out, cap, "x%d", count);
    }

    inline void BoosterIconPath(char* out, size_t cap, int id) noexcept
    {
        if (!out || !cap) return;
        if (id < 0 || id > 7) id = 0;
        std::snprintf(out, cap, "streaming/t2r/icons/booster-%s.png", kBoosters[id].icon);
    }

    // mf_booster_fill's boSelText. `focusId` is the booster under the focus,
    // `n` the live slot count; `pending` is g_bo_want and is shown as chosen
    // before the daemon confirms it (the owner passes an effective meta).
    inline void BoosterConfirmText(char* out, size_t cap, const Meta& effective,
                                   int focusId, int n) noexcept
    {
        if (!out || !cap) return;
        if (!effective.ok)
            std::snprintf(out, cap, "CONFIRM");
        else if (effective.bp == focusId)
            std::snprintf(out, cap, "CHOSEN: %s",
                          effective.bp ? kBoosters[focusId].name : "NOTHING");
        else if (n == 1)
            std::snprintf(out, cap,
                          "NO BOOSTERS - %d MORE SURVIVORS FOR THE NEXT ONE",
                          effective.bn);
        else
            std::snprintf(out, cap, "CONFIRM");
    }

    // mf_timer_text: one double-buffered title, "Choose One-Use Booster - " +
    // a coloured 00:ss. `s` is the raw flow code minus 130.
    inline void BoosterTitleText(char* out, size_t cap, int s) noexcept
    {
        if (!out || !cap) return;
        if (s < 0) s = 0;
        if (s > 99) s = 99;
        std::snprintf(out, cap,
                      "Choose One-Use Booster - <font color=\"#e04848\">00:%02d</font>", s);
    }

    // ----------------------------------------------------------- event days
    // clan.py EVENT_DAYS: events start on days 5,10,..80 and run three matches;
    // 15/30/35/50/65/75/80 are attacks, the others growth events.
    [[nodiscard]] inline int EventKindAt(int day) noexcept
    {
        static constexpr int kAttacks[7] = { 15, 30, 35, 50, 65, 75, 80 };
        if (day < 5 || day > 80 || day % 5) return 0;
        for (int k : kAttacks) if (k == day) return 2;
        return 1;
    }

    [[nodiscard]] inline int EventWinStart(int day) noexcept
    {
        for (int s = day; s >= 0 && s > day - 3; --s)
            if (EventKindAt(s)) return s;
        return -1;
    }

    [[nodiscard]] inline int EventNextStart(int day) noexcept
    {
        for (int s = day < 0 ? 0 : day; s <= 80; ++s)
            if (EventKindAt(s)) return s;
        return -1;
    }

    [[nodiscard]] inline int EventLossPct(int day, int tier) noexcept
    {
        static constexpr int kEarly[4] = { 60, 40, 20, 10 };
        static constexpr int kNormal[4] = { 100, 60, 40, 20 };
        const int* p = (day == 15 || day == 30) ? kEarly : kNormal;
        if (tier < 0) tier = 0;
        if (tier > 3) tier = 3;
        return p[tier];
    }

    [[nodiscard]] inline const char* EventGrowthPct(int tier) noexcept
    {
        if (tier == 0) return "+5%";
        if (tier == 1) return "+7.5%";
        return "+10%";
    }

    // -------------------------------------------------------------- missions
    // MIS[] (name / unit / goals). The 2013 table is fixed; goals are the eight
    // completion tiers and are indexed by (repetitions + tier).
    struct MissionDef { const char* name; const char* unit; int g[8]; };
    // A constexpr lookup keeps the sparse 2013 list readable without C-style
    // array-designated initializers (unsupported by C++20 + -pedantic).
    [[nodiscard]] inline constexpr MissionDef MissionDefOf(int id) noexcept
    {
        switch (id) {
        case 1:  return {"Down Enemies", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 2:  return {"Executions", "Executions", {2, 4, 7, 12, 15, 20, 35, 40}};
        case 3:  return {"Revives", "Revives", {2, 4, 6, 8, 10, 15, 20, 25}};
        case 4:  return {"Headshots", "Headshots", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 5:  return {"Mark Enemies", "Marks", {7, 14, 21, 28, 35, 42, 48, 56}};
        case 6:  return {"Heal Teammates", "Heals", {5, 10, 15, 20, 25, 30, 35, 40}};
        case 7:  return {"Special Executions", "Executions", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 8:  return {"Down with Molotovs", "Downs", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 9:  return {"Down with Nail Bombs", "Downs", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 10: return {"Give Crafting Items", "Gifts", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 11: return {"Down with Melee", "Downs", {3, 6, 9, 12, 15, 18, 25, 30}};
        case 12: return {"Shiv Executions", "Executions", {1, 2, 3, 5, 7, 9, 12, 15}};
        case 13: return {"Downs from Long Range", "Downs", {2, 4, 6, 8, 10, 16, 20, 24}};
        case 14: return {"Downs with 9mm Pistol", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 15: return {"Downs with Revolver", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 16: return {"Downs with Shorty", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 17: return {"Downs with Hunting Rifle", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 18: return {"Downs with Semi Auto Rifle", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 19: return {"Downs with Burst Rifle", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 20: return {"Downs with Bow", "Downs", {3, 6, 9, 15, 20, 25, 35, 45}};
        case 40: return {"Variable Rifle Executions", "Executions", {2, 3, 5, 7, 9, 12, 15, 20}};
        case 41: return {"Hunting Rifle Executions", "Executions", {1, 2, 3, 5, 7, 9, 12, 15}};
        case 42: return {"Bow Executions", "Executions", {1, 2, 3, 5, 7, 9, 12, 15}};
        case 43: return {"Full-Auto Rifle Executions", "Executions", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 44: return {"Semi-Auto Rifle Executions", "Executions", {2, 3, 5, 7, 9, 12, 15, 20}};
        case 45: return {"Tactical Shotgun Executions", "Executions", {2, 3, 5, 7, 9, 12, 15, 20}};
        case 46: return {"Frontier Rifle Executions", "Executions", {1, 2, 3, 5, 7, 9, 12, 15}};
        case 47: return {"Burst Rifle Executions", "Executions", {2, 4, 6, 8, 10, 12, 15, 20}};
        case 48: return {"9mm Pistol Executions", "Executions", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 49: return {"Revolver Executions", "Executions", {1, 2, 3, 5, 7, 9, 12, 15}};
        case 50: return {"Burst Pistol Executions", "Executions", {2, 4, 6, 8, 10, 12, 16, 20}};
        case 51: return {"Enforcer Executions", "Executions", {2, 3, 5, 7, 9, 12, 15, 20}};
        case 52: return {"Shorty Executions", "Executions", {1, 2, 3, 5, 7, 9, 12, 15}};
        default: return {};
        }
    }

    [[nodiscard]] inline bool MissionValid(int id) noexcept
    {
        return id >= 1 && id < kMisN && MissionDefOf(id).name != nullptr;
    }

    [[nodiscard]] inline int MissionGoal(int id, int tier, int rep) noexcept
    {
        if (!MissionValid(id)) return 0;
        int i = (rep < 0 ? 0 : rep) + tier;
        if (i > 7) i = 7;
        return MissionDefOf(id).g[i];
    }

    [[nodiscard]] inline int MissionGoalM(const Meta& m, int id, int tier) noexcept
    {
        if (m.mi == id && tier >= 0 && tier < 3 && m.mg[tier] > 0)
            return m.mg[tier];
        return MissionGoal(id, tier, m.mr[id]);
    }

    [[nodiscard]] inline int MissionNextGoal(const Meta& m, int id) noexcept
    {
        int nx = MissionGoalM(m, id, 2);
        for (int k = 2; k >= 0; --k)
            if (m.mp < MissionGoalM(m, id, k)) nx = MissionGoalM(m, id, k);
        return nx;
    }

    inline void MissionLine(char* out, size_t cap, const Meta& m, int id) noexcept
    {
        if (!out || !cap) return;
        if (!MissionValid(id)) { out[0] = 0; return; }
        const int nx = MissionNextGoal(m, id);
        if (m.mp >= nx)
            std::snprintf(out, cap, "%d/%d %s - done", nx, nx, MissionDefOf(id).name);
        else
            std::snprintf(out, cap, "%d/%d %s", m.mp, nx, MissionDefOf(id).name);
    }

    inline void MissionDayLine(char* out, size_t cap, const Meta& m) noexcept
    {
        if (out && cap) std::snprintf(out, cap, "Day %d/3", 3 - m.ml + 1);
    }

    inline void MissionTierLine(char* out, size_t cap, const Meta& m,
                                int id, int tier) noexcept
    {
        if (!out || !cap) return;
        if (!MissionValid(id)) { out[0] = 0; return; }
        std::snprintf(out, cap, "%d %s", MissionGoalM(m, id, tier),
                      MissionDefOf(id).unit);
    }

    // The no-mission branch of mf_loading_fill: a short relative next-event
    // line (the old "Week 1: Day 6" ran through the divider).
    inline void NextEventLine(char* out, size_t cap, int clanDay, int next) noexcept
    {
        if (!out || !cap) return;
        const int dd = next >= 0 ? next - clanDay : -1;
        if (dd == 1) std::snprintf(out, cap, "Next event tomorrow");
        else if (dd > 1) std::snprintf(out, cap, "Next event in %d days", dd);
        else if (next >= 0) std::snprintf(out, cap, "Next event today");
        else std::snprintf(out, cap, "No more events");
    }

    // ------------------------------------------------------------------ tips
    // tools/ext/tips.h T2R_TIPS: cat / text / image / npic. The loading screen
    // shows "CAT:" + text and rotates one tip every 7 s; the CLAN tip has 3
    // framed shots.
    struct Tip { const char* cat; const char* text; const char* img; int npic; };
    inline constexpr Tip kTips[] = {
        {"WEAPONS", "The Molotov flushes enemies out of cover. Arc-throw it from low cover to stay hidden while you throw.", "use-molotov", 1},
        {"WEAPONS", "Shotguns are deadly up close but fall off fast. Keep moving and close the distance.", "weapon-shotgun", 1},
        {"WEAPONS", "A suppressor keeps your shots quiet. Pick your fights.", "crouch-beside-enemy", 1},
        {"WEAPONS", "The bow is silent, and a fully drawn arrow hits harder. Pick your arrows back up after the fight.", "silent-bow", 1},
        {"WEAPONS", "Bullets drop with gravity. Aim higher to compensate.", "weapon-bullet-drop", 1},
        {"WEAPONS", "A pipe bomb's fuse burns for a moment before it goes off. Lob one over cover to catch an enemy hiding behind it.", "use-pipe-bombs", 1},
        {"WEAPONS", "Smoke bomb clouds completely block enemy vision, hiding your movements and preventing them from firing on you.", "player-upgrade-stun-bomb-smoke", 1},
        {"WEAPONS", "One-handed melee weapons are fast and durable. Two-handed melee weapons are slower but deal massive damage.", "melee-weapon-types", 1},
        {"WEAPONS", "Upgraded melee weapons are restored to full durability and have greatly increased damage.", "upgraded-melee", 1},
        {"SURVIVAL", "Crafting takes time. Make medkits and bombs before the fight, not during it.", "ellie-in-backpack", 1},
        {"SURVIVAL", "Healing takes time. Run from the fight and use your health kit somewhere safe.", "use-health-kit", 1},
        {"SURVIVAL", "Supply boxes refill over time. Remember where they are.", "scavenge-items", 1},
        {"SURVIVAL", "Armor soaks up damage from every direction. Buy it early.", "shooting-armor-legs", 1},
        {"TEAMWORK", "A downed teammate can be revived. Mark them and run to them - the run doesn't drain your stamina.", "buddy-rescue", 1},
        {"TEAMWORK", "Mark an enemy to show your whole team where they are.", "general-flank-them", 1},
        {"TEAMWORK", "Stick together. A lone survivor is easy prey for the other faction.", "overwhelmed", 1},
        {"STEALTH", "Stay low in the grass. A prone survivor is hard to spot.", "grass-crouch-prone", 1},
        {"STEALTH", "Listen mode shows you footsteps. Moving slowly keeps you off it.", "escape-sneak", 1},
        {"STEALTH", "Use the environment to lose your enemies. They will search your last known position, so don't be there.", "hide", 1},
        {"STEALTH", "Breaking glass windows can open up new routes, but the sound will draw enemy attention.", "glass-break", 1},
        {"COMBAT", "Enemies are deadly accurate when you charge straight at them. Use the environment to flank them instead.", "easy-target", 1},
        {"COMBAT", "Enemies are less accurate while you're sprinting away or to the side.", "sprint-to-avoid", 1},
        {"COMBAT", "Stay crouched behind cover to protect yourself from gunfire.", "cover-from-gunfire", 1},
        {"COMBAT", "Look around to track enemy movements so you aren't caught off guard.", "general-dont-get-flanked", 1},
        {"COMBAT", "Stand still to steady your aim. Crouching makes you more accurate, and going prone even more.", "stand-aim-rifle", 1},
        {"COMBAT", "Headshots down enemies faster. Take your time and aim.", "crouch-prone-aim", 1},
        {"COMBAT", "Finishing a downed enemy earns more parts. Watch for their friends.", "vulnerable-enemies-5-6", 1},
        {"COMBAT", "If your usual plan isn't working, vary your tactics: fight quietly, by force, or avoid the fight. Try a new weapon or crafting recipe.", "general-vary-your-tactics", 1},
        {"CLAN", "Parts you earn in a match become supplies for your clan. Every survivor eats, so keep them fed.", "limited-supplies", 3},
    };
    inline constexpr int kTipCount = static_cast<int>(sizeof(kTips) / sizeof(kTips[0]));
    inline constexpr uint64_t kTipMs = 7000;
    inline constexpr uint64_t kSpinMs = 400;
    inline constexpr uint64_t kFadeMs = 400;
    inline constexpr uint64_t kBlackMs = 300;

    [[nodiscard]] inline int TipIndex(uint64_t now) noexcept
    {
        return static_cast<int>((now / 1000) % static_cast<uint64_t>(kTipCount));
    }

    [[nodiscard]] inline int TipNext(int tip) noexcept
    {
        return (tip + 1) % kTipCount;
    }

    // The framed picture rotates only for multi-picture tips; the caller keeps
    // the rotation counter and the previous path for the old-picture twin.
    inline void TipPicPath(char* out, size_t cap, int tip, int rot) noexcept
    {
        if (!out || !cap) return;
        if (tip < 0 || tip >= kTipCount) tip = 0;
        const int np = kTips[tip].npic > 0 ? kTips[tip].npic : 1;
        const int pic = np > 1 ? (rot % np) : 0;
        std::snprintf(out, cap, "streaming/t2r/clan/tip-%02d-%d.png",
                      tip + 1, 1 + pic);
    }

    inline void TipCatText(char* out, size_t cap, int tip) noexcept
    {
        if (!out || !cap) return;
        if (tip < 0 || tip >= kTipCount) tip = 0;
        std::snprintf(out, cap, "%s:", kTips[tip].cat);
    }

    inline void SpinText(char* out, size_t cap, uint64_t now) noexcept
    {
        if (!out || !cap) return;
        const int dots = static_cast<int>((now / kSpinMs) % 4);
        std::snprintf(out, cap, "LOADING%.*s", dots, "...");
    }

    // Release fade: black under the group for kBlackMs, then the group fades
    // out over kFadeMs. Returns the group alpha (1 until the black ends).
    [[nodiscard]] inline float ReleaseAlpha(uint64_t since) noexcept
    {
        if (since < kBlackMs) return 1.0f;
        if (since >= kBlackMs + kFadeMs) return 0.0f;
        return 1.0f - static_cast<float>(since - kBlackMs) /
                          static_cast<float>(kFadeMs);
    }

} // namespace net::ui::mfnr::core
