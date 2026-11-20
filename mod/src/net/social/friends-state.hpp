#pragma once

#include <cstddef>
#include <cstdint>

// Pure core of the in-game FRIENDS / PARTY panel, ported from tools/ext/friends13.h.
// The daemon's 0xD9 reply is parsed into a bounded state snapshot; the rows, badge,
// presence word and per-row actions are derived from it with no engine calls. Having
// this as its own module keeps the wire/state testable without a game and lets the
// panel, the party gate and the toasts share one definition of "the state".
//
// TODO(friends-state): rank badges (0xBF / rankbadge.c) are a separate unported feature;
// the ext's RkbLookup / RankUi naming is done by the panel, not here.

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

    // English / Polish pair (ext FRT), resolved from engine::locale.
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

}
