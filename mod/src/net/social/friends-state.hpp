#pragma once

#include <cstddef>
#include <cstdint>

// Pure core of the in-game FRIENDS / PARTY panel, ported from tools/ext/friends13.h.
// The daemon's 0xD9 reply is parsed into a bounded state snapshot; the rows, badge,
// presence word and per-row actions are derived from it with no engine calls. Having
// this as its own module keeps the wire/state testable without a game and lets the
// panel, the party gate and the toasts share one definition of "the state".
//
// Rank badges (tools/ext/rankbadge.c + rank13.h, daemon op 0xBF) live here as a pure
// core: the shown-name key, the bounded rank table reply parser, the lookup /
// placeholder rules and the symbol / weeks helpers. The 0xBF poll and the widget
// drawing are the feature half (net/social/rank-badge.*, consumed by friends13).

namespace net::social {

    inline constexpr int kMaxFriends = 100;
    inline constexpr int kMaxRequests = 30;
    inline constexpr int kMaxInvites = 8;
    inline constexpr int kMaxMembers = 8;

    struct Friend {
        char id[40]{}, name[36]{}, status[12]{};
        int party = 0;
    };
    struct Request {
        char id[40]{}, name[36]{};
    };
    struct Member {
        char id[40]{}, name[36]{}, status[12]{};
        int leader = 0, ready = 0, me = 0;
    };

    struct State {
        int  ok = 0;
        char err[16]{};
        char meId[40]{}, meName[36]{};
        int  nf = 0, ni = 0, no = 0, npi = 0, nm = 0;
        int  hasParty = 0, meLeader = 0, pmax = 0;
        int  searching = 0, lsearch = 0;
        Friend friends[kMaxFriends]{};
        Request in[kMaxRequests]{}, out[kMaxRequests]{}, pinv[kMaxInvites]{};
        Member m[kMaxMembers]{};
    };

    enum class RowKind { Add, PartyInvite, Request, Friend, Outgoing };
    struct Row {
        RowKind kind = RowKind::Add;
        int idx = 0;
    };

    enum class Action {
        None, State, Type, Invite, Accept, Join, DeclineParty, DeclineRequest,
        Remove, Cancel, Promote, Kick, Ready, Leave, Add
    };

    // daemon sub-ops (tools/social.py): 1 state, 3 add, 4 accept, 5 remove /
    // decline / cancel, 6 invite, 7 join, 8 decline party, 9 leave, 10 kick,
    // 11 promote, 12 ready.
    int SubOp(Action action);

    // English / Polish pair (ext FRT), resolved through engine::locale's active
    // language: the generated table translates for any registered language, with
    // `pl` only as the Polish fallback for strings the table does not list.
    const char* Tr(const char* en, const char* pl);

    // text of len bytes, not necessarily NUL terminated. 1 on success; on success
    // `d->ok` is the reply's "ok" (a refused write still carries the fresh state).
    bool Parse(State* d, const char* text, size_t len);

    // right-hand list: ADD FRIEND, party invites, requests in, friends, requests sent
    int  Rows(const State* d, Row* out, int cap);
    int  Badge(const State* d);                       // requests + party invites

    int  Presence(const char* status);                // 0 offline .. 4 in a match
    bool CanInvite(const State* d, const Friend* f);
    int  MyReady(const State* d);                     // our own ready bit

    // primary / secondary action of a left (col 0) or right (col 1) row
    void RowActions(const State* d, int col, int idx, const Row* rows, int nrows,
        Action& pri, Action& sec);
    const char* ActionLabel(const State* d, Action a);
    const char* StatusWord(const char* status, char* out, size_t n);
    int  Tone(int presence);                          // presence -> accent-band index

    const char* ErrorText(const char* code);          // daemon refusal -> local text
    const char* OkText(Action a, bool becameFriends); // successful write -> local text

    // ---- rank table (ext rankbadge.c / rank13.h, daemon op 0xBF) -----------------------
    // The daemon's 0xBF reply is "n=<k> <HHHHHHHH:r:w> ...": one row per known player,
    // keyed by a hash of the SHOWN name (ext RkbHash = tools/rankbadge.py key()). r is
    // the chalk rank symbol 0..9, w the weeks survived 0..999. A name the table does not
    // list is drawn as rank 0 / 0 weeks once the daemon has answered at least once, so a
    // new player still has a badge.
    inline constexpr int kRankMax     = 400;   // ext RKB_MAX
    inline constexpr int kRankSymbols = 10;    // ext RANK13_SYMBOLS (icons rank-0..9.png)
    inline constexpr int kRankMaxWeeks = 999;  // ext RANK13_MAX_WEEKS

    struct RankEntry { uint32_t h = 0; int rk = 0; int wk = 0; };
    struct RankTable {
        RankEntry entries[kRankMax]{};
        int      n = 0;
        uint32_t ver = 0;   // ext g_rkb_ver: bumped on each successful 0xBF reply
    };

    uint32_t RankHash(const char* name);                        // ext RkbHash / py key()
    int      RankParse(RankTable* t, const char* text, size_t len); // returns rows, bumps ver
    bool     RankLookup(const RankTable* t, const char* name, int* rk, int* wk);
    bool     RankPlaceholder(const char* name);  // empty / "PLAYER n" / "SEARCHING" => no badge

    int      RankSym(int rk);                     // -1 unknown, else clamp to 0..9
    int      RankWeeks(int wk);                   // clamp to 0..999
    void     RankIconPath(char* out, size_t n, int rk); // "streaming/t2r/icons/rank-<s>.png"

}
