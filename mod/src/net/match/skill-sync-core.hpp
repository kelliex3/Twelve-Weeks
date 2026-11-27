#pragma once

// Pure rules for the ext's MSG_SK_SYNC peer-skill feed (tools/ext/skills.c /
// skills.h). The native owner in skill-sync.cpp supplies the clock, the
// launcher loadout JSON, local teammate positions, the wire and the slots;
// neither the engine nor any network header is read here.
//
// The pinned wire byte lives in net-protocol.hpp (NEVT_SKILLS = 0x50). A short
// message has eight bytes ([u8 id x4][u8 level x4]); the ext's ninth byte is
// the sender's EFFECTIVE Agility (Lone Wolf grants Agility II while alone),
// clamped to 3 on receive. Tests: mod/tests/skill_sync_core_test.cpp.

#include "net/gift/gift-core.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace net::match::skill_sync::core {

    inline constexpr int      kSkillCount = 33;     // ext SK_COUNT (SK_NONE + 32)
    inline constexpr int      kRows = 4;
    inline constexpr int      kMsgLen = 9;
    inline constexpr uint64_t kSendEveryMs = 1900;  // ext bw_every_ms(&sk_last, now, 1900)
    inline constexpr uint64_t kCacheMs = 2000;      // launcher loadout JSON TTL
    inline constexpr uint64_t kFreshMs = 5000;      // two missed keep-alives
    inline constexpr int      kMaxLp = 14;          // 13 LP + one slack, like ext

    // ext skills.h enum. The values are on the wire, so do not renumber them.
    enum Skill : uint8_t {
        kNone = 0,
        kZoom = 1, kExplo, kReviver, kBrawler, kCovert, kEars, kStrat,
        kHawk, kCrafter, kFat, kSharp, kMarathon, kCollector,
        kExec = 14, kDmgMark, kGunslinger, kBomb, kScav, kAware, kFort,
        kAgility, kLoneWolf, kSecond, kJack, kLucky, kLethal,
        kRiposte = 27, kGrip, kBlade, kHeavy, kSteady, kFootwork
    };

    // ext SK[]: {number of levels, LP cost per level, level-availability bits}.
    struct Def {
        uint8_t nlv = 0;
        uint8_t cost[3] = { 0, 0, 0 };
        uint8_t avail = 0;
    };

    inline constexpr Def kSkills[kSkillCount] = {
        {},                                   // SK_NONE
        { 2, { 1, 2 }, 3 },                   // SK_ZOOM
        { 2, { 1, 3 }, 3 },                   // SK_EXPLO
        { 3, { 1, 3, 4 }, 7 },                // SK_REVIVER
        { 2, { 2, 3 }, 3 },                   // SK_BRAWLER
        { 3, { 2, 4, 5 }, 7 },                // SK_COVERT
        { 3, { 1, 2, 3 }, 7 },                // SK_EARS
        { 3, { 1, 4, 5 }, 7 },                // SK_STRAT
        { 3, { 2, 3, 4 }, 7 },                // SK_HAWK
        { 3, { 2, 4, 6 }, 7 },                // SK_CRAFTER
        { 3, { 2, 4, 6 }, 7 },                // SK_FAT
        { 3, { 2, 4, 5 }, 7 },                // SK_SHARP
        { 2, { 1, 3 }, 3 },                   // SK_MARATHON
        { 2, { 3, 5 }, 3 },                   // SK_COLLECTOR
        { 3, { 1, 3, 4 }, 7 },                // SK_EXEC
        { 2, { 2, 4 }, 3 },                   // SK_DMGMARK
        { 2, { 2, 4 }, 3 },                   // SK_GUNSLINGER
        { 3, { 2, 3, 5 }, 7 },                // SK_BOMB
        { 3, { 2, 4, 5 }, 7 },                // SK_SCAV
        { 2, { 1, 3 }, 3 },                   // SK_AWARE
        { 2, { 1, 3 }, 3 },                   // SK_FORT
        { 2, { 2, 4 }, 3 },                   // SK_AGILITY
        { 2, { 2, 3 }, 3 },                   // SK_LONEWOLF
        { 2, { 1, 2 }, 3 },                   // SK_SECOND
        { 2, { 5, 10 }, 3 },                  // SK_JACK
        { 3, { 2, 3, 4 }, 7 },                // SK_LUCKY
        { 2, { 2, 3 }, 3 },                   // SK_LETHAL
        { 2, { 2, 3 }, 3 },                   // SK_RIPOSTE
        { 2, { 2, 3 }, 3 },                   // SK_GRIP
        { 2, { 1, 3 }, 3 },                   // SK_BLADE
        { 2, { 2, 6 }, 3 },                   // SK_HEAVY
        { 2, { 1, 2 }, 3 },                   // SK_STEADY
        { 3, { 2, 4, 6 }, 7 },                // SK_FOOTWORK
    };

    inline constexpr bool Valid(int id, int lv) noexcept
    {
        return id > 0 && id < kSkillCount && lv >= 1 && lv <= kSkills[id].nlv;
    }

    // skills_recv's trust check: every non-empty row must name a real level and
    // the four rows must fit 13 LP + one slack. A peer that fails keeps its
    // previous honest set (that policy belongs to the owner).
    inline bool PayloadValid(const uint8_t* b, std::size_t n) noexcept
    {
        if (!b || n < kMsgLen - 1)
            return false;
        int lp = 0;
        for (int k = 0; k < kRows; ++k) {
            const int id = b[k], lv = b[k + kRows];
            if (!id && !lv)
                continue;
            if (!Valid(id, lv))
                return false;
            lp += kSkills[id].cost[lv - 1];
        }
        return lp <= kMaxLp;
    }

    inline uint8_t AgilityByte(const uint8_t* b, std::size_t n) noexcept
    {
        if (!b || n < kMsgLen)
            return 0;
        return b[kMsgLen - 1] > 3 ? 3 : b[kMsgLen - 1];
    }

    // One peer's last accepted rows. The ext keeps them for the whole match;
    // the native owner also stamps them so a peer that stops sending goes
    // stale after two missed keep-alives.
    struct Peer {
        uint8_t  id[kRows] = {};
        uint8_t  lv[kRows] = {};
        uint8_t  agility = 0;
        uint64_t stamp = 0;
    };

    inline void Reset(Peer& p) noexcept { p = Peer{}; }

    inline bool Apply(Peer& p, const uint8_t* b, std::size_t n,
                      uint64_t now) noexcept
    {
        if (!PayloadValid(b, n))
            return false;
        const uint8_t agi = AgilityByte(b, n);
        if (agi != p.agility)
            p.agility = agi;                   // effective level, even if rows agree
        if (!p.stamp || std::memcmp(p.id, b, kRows)
            || std::memcmp(p.lv, b + kRows, kRows)) {
            std::memcpy(p.id, b, kRows);
            std::memcpy(p.lv, b + kRows, kRows);
        }
        p.stamp = now ? now : 1;
        return true;
    }

    inline bool Fresh(const Peer& p, uint64_t now,
                      uint64_t gap = kFreshMs) noexcept
    {
        return p.stamp != 0 && now >= p.stamp && now - p.stamp <= gap;
    }

    // ext sk_expand: Jack of All Trades grants level I of its five basic
    // skills (five more at II); Lone Wolf, while alone, grants Agility II,
    // Sharpshooter II and (at II) Sharp Ears III.
    inline void Expand(uint8_t eff[kSkillCount], int id, int lv,
                       bool alone) noexcept
    {
        if (id <= 0 || id >= kSkillCount || lv < 1 || lv > kSkills[id].nlv)
            return;
        if (lv > eff[id])
            eff[id] = static_cast<uint8_t>(lv);
        if (id == kJack) {
            static constexpr uint8_t j1[] = {
                kBrawler, kCovert, kEars, kStrat, kCrafter
            };
            static constexpr uint8_t j2[] = {
                kExplo, kReviver, kFat, kScav, kSecond
            };
            for (uint8_t s : j1)
                if (!eff[s]) eff[s] = 1;
            if (lv >= 2)
                for (uint8_t s : j2)
                    if (!eff[s]) eff[s] = 1;
        }
        if (id == kLoneWolf && alone) {
            if (eff[kAgility] < 2) eff[kAgility] = 2;
            if (eff[kSharp] < 2) eff[kSharp] = 2;
            if (lv >= 2 && eff[kEars] < 3) eff[kEars] = 3;
        }
    }

    inline void ExpandRows(uint8_t eff[kSkillCount], const uint8_t id[kRows],
                           const uint8_t lv[kRows], bool alone) noexcept
    {
        // Like ext sk_peer / sk_expand: `avail` is only the GEAR UP UI's
        // unlock mask; a wire row already passed nlv validation.
        for (int k = 0; k < kRows; ++k)
            if (id[k] && lv[k])
                Expand(eff, id[k], lv[k], alone);
    }

    // ext sk_peer: the declared rows plus the sender's effective Agility byte.
    inline int Effective(const Peer& p, int id) noexcept
    {
        if (!p.stamp || id <= 0 || id >= kSkillCount)
            return 0;
        uint8_t eff[kSkillCount] = {};
        for (int k = 0; k < kRows; ++k)
            Expand(eff, p.id[k], p.lv[k], false);
        if (id == kAgility && p.agility > eff[id])
            return p.agility;
        return eff[id];
    }

    // The sender's byte 8 is skl(SK_AGILITY), whose Lone Wolf grant depends on
    // being alone; the owner computes that flag from nearby live teammates.
    inline int EffectiveAgility(const uint8_t id[kRows],
                                const uint8_t lv[kRows], bool alone) noexcept
    {
        uint8_t eff[kSkillCount] = {};
        ExpandRows(eff, id, lv, alone);
        return eff[kAgility];
    }

    inline void Encode(uint8_t out[kMsgLen], const uint8_t id[kRows],
                       const uint8_t lv[kRows], int agility) noexcept
    {
        for (int k = 0; k < kRows; ++k) {
            out[k] = id[k];
            out[k + kRows] = lv[k];
        }
        out[kMsgLen - 1] = static_cast<uint8_t>(agility > 3 ? 3
                                                            : agility < 0 ? 0 : agility);
    }

    inline const char* SkillName(Skill s) noexcept;

    // Pure scan of the launcher's active custom row. Only the four authored
    // skill ids are kept; the launcher writes `{"id":"SK_X","lv":n}` objects.
    inline void RowsFromJson(std::string_view s, uint8_t id[kRows],
                             uint8_t lv[kRows]) noexcept
    {
        for (int k = 0; k < kRows; ++k) { id[k] = 0; lv[k] = 0; }
        const std::string_view row = net::gift::core::ActiveCustomLoadout(s);
        if (row.empty())
            return;
        size_t p = row.find("\"skills\"");
        if (p == std::string_view::npos)
            return;
        p = row.find('[', p);
        if (p == std::string_view::npos)
            return;
        int n = 0;
        while (n < kRows) {
            const size_t obj = row.find('{', p);
            if (obj == std::string_view::npos)
                break;
            const size_t end = row.find('}', obj);
            if (end == std::string_view::npos)
                break;
            const std::string_view one = row.substr(obj, end - obj);
            const size_t q0 = one.find("\"id\"");
            const size_t q1 = q0 == std::string_view::npos
                ? std::string_view::npos : one.find('"', one.find(':', q0));
            size_t q2 = q1 == std::string_view::npos
                ? std::string_view::npos : one.find('"', q1 + 1);
            if (q1 != std::string_view::npos && q2 != std::string_view::npos) {
                const std::string_view name = one.substr(q1 + 1, q2 - q1 - 1);
                int sid = 0;
                for (int i = 1; i < kSkillCount; ++i) {
                    // The launcher writes the ext enum's full SK_* token, so
                    // one table lookup names the wire id without a string table.
                    if (name == SkillName(static_cast<Skill>(i))) { sid = i; break; }
                }
                const int level = net::gift::core::SkillLevelFromJson(one, name);
                if (Valid(sid, level)) {
                    id[n] = static_cast<uint8_t>(sid);
                    lv[n] = static_cast<uint8_t>(level);
                    ++n;
                }
            }
            p = end + 1;
        }
    }

    // ext skills.h enum token -> launcher JSON id (with the SK_ prefix the
    // launcher writes). Pure reverse of the on-wire id, so a renamed skill
    // cannot silently move its row.
    inline const char* SkillName(Skill s) noexcept
    {
        switch (s) {
        case kZoom: return "SK_ZOOM";
        case kExplo: return "SK_EXPLO";
        case kReviver: return "SK_REVIVER";
        case kBrawler: return "SK_BRAWLER";
        case kCovert: return "SK_COVERT";
        case kEars: return "SK_EARS";
        case kStrat: return "SK_STRAT";
        case kHawk: return "SK_HAWK";
        case kCrafter: return "SK_CRAFTER";
        case kFat: return "SK_FAT";
        case kSharp: return "SK_SHARP";
        case kMarathon: return "SK_MARATHON";
        case kCollector: return "SK_COLLECTOR";
        case kExec: return "SK_EXEC";
        case kDmgMark: return "SK_DMGMARK";
        case kGunslinger: return "SK_GUNSLINGER";
        case kBomb: return "SK_BOMB";
        case kScav: return "SK_SCAV";
        case kAware: return "SK_AWARE";
        case kFort: return "SK_FORT";
        case kAgility: return "SK_AGILITY";
        case kLoneWolf: return "SK_LONEWOLF";
        case kSecond: return "SK_SECOND";
        case kJack: return "SK_JACK";
        case kLucky: return "SK_LUCKY";
        case kLethal: return "SK_LETHAL";
        case kRiposte: return "SK_RIPOSTE";
        case kGrip: return "SK_GRIP";
        case kBlade: return "SK_BLADE";
        case kHeavy: return "SK_HEAVY";
        case kSteady: return "SK_STEADY";
        case kFootwork: return "SK_FOOTWORK";
        default: return nullptr;
        }
    }

} // namespace net::match::skill_sync::core
