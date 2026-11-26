#pragma once

// Native home for the 2013 CLAN server state and the two post-match page latches
// (tools/ext/t2r-ext.c: ClanT / ClanParse / g_pm.pending / g_ev.pending,
// PmReport / PmReady / EvWanted / EvOffer). Ported with the ext's exact wire
// opcodes (net-protocol.hpp: 0xE0 / 0xE1 / 0xE2 / 0xE4) and the same daemon RPC
// helper every other wire module uses. The page push / widget render / input
// lifecycle for to1/clan-postmatch and to1/clan-event lives in
// net/ui/postmatch-screens.*; this module owns the decision and the latch that
// feed it.
//
// Pure core (State parsing, EventWanted, PmReady, the award tally) is inline in
// this header so mod/tests/clan_core_test.cpp can compile it without Windows.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace net::clan {

    // ext ClanT, field for field (the 0xE1 "k=v k=v ..." state).
    struct State {
        int ok = 0, fac = 0, st = 0, day = 0, h = 0, hu = 0, s = 0, need = 0;
        int wk = 0, lday = 0, lparts = 0, lsup = 0, lneed = 0;
        int lgain = 0, ldied = 0, lhungry = 0, lsick = 0, lheal = 0, lleft = 0;
        int ev = 0, evn = 0, levk = 0, levn = 0;
        int levl = 0, levg = 0, levt = 0, mx = 0, rw = 0, rl = 0;
        int ag[3] = { 0, 0, 0 };
        int lmh = 0, rk = 0, dt = 0;
        int ro = 0;                          // ext build-201 ClanT.ro: our own role (tools/roles.py)
        int rc = 0;                          // ext caps bit0: living clan must choose its side again
    };

    // ext ClanParse's key table. Unknown keys are skipped so a newer daemon
    // cannot break the parse (the ext does the same).
    inline bool ParseState(const char* text, State& out)
    {
        if (!text)
            return false;
        static const char* const K[] = {
            "fac", "st", "day", "h", "hu", "s", "need", "wk", "lday",
            "lparts", "lsup", "lneed", "lgain", "ldied", "lhungry", "lsick",
            "lheal", "lleft", "ev", "evn", "levk", "levn", "levl", "levg",
            "levt", "mx", "rw", "rl", "ag1", "ag2", "ag3", "lmh", "rk", "dt",
            "ro", "rc"    // build 201: own role (chat colour), one-time re-choice (side page)
        };
        State c{};
        // Rebind the field table to the local snapshot: a malformed reply must
        // never leave the last good state half-updated.
        int* L[] = {
            &c.fac, &c.st, &c.day, &c.h, &c.hu, &c.s, &c.need,
            &c.wk, &c.lday, &c.lparts, &c.lsup, &c.lneed,
            &c.lgain, &c.ldied, &c.lhungry, &c.lsick, &c.lheal,
            &c.lleft, &c.ev, &c.evn, &c.levk, &c.levn, &c.levl,
            &c.levg, &c.levt, &c.mx, &c.rw, &c.rl, &c.ag[0],
            &c.ag[1], &c.ag[2], &c.lmh, &c.rk, &c.dt,
            &c.ro, &c.rc
        };
        const char* t = text;
        char k[16];
        int v = 0, n = 0;
        while (std::sscanf(t, " %15[^=]=%d%n", k, &v, &n) == 2) {
            for (size_t i = 0; i < sizeof(K) / sizeof(*K); ++i)
                if (!std::strcmp(k, K[i]))
                    *L[i] = v;
            t += n;
        }
        c.ok = 1;
        out = c;
        return true;
    }

    // ext g_ev's bookkeeping that survives a page close / a restart.
    struct EventBook {
        int pending = 0;     // g_ev.pending (the page is due or up)
        int shown = 0;       // g_ev.shown
        int mode = 0;        // 1 announcement, 2 result
        int annDay = -1;     // the day already announced this run
        int outDay = -1;     // the day whose result is due
        int outDone = 0;     // its result was seen / given up on
    };

    // ext EvWanted(): 1 = announcement due, 2 = result due, -1 = the match was
    // played but the day is not in yet, 0 = nothing due.
    inline int EventWanted(const State& c, const EventBook& ev)
    {
        if (!c.ok)
            return 0;                       // ext also checks g_noClan (no reader in the mod)
        const bool played = ev.outDay >= 0 && ev.outDone == 0;
        if (played && c.lday != ev.outDay)
            return -1;
        if (played && c.levk)
            return 2;
        if (c.st == 1 && c.ev && c.day != ev.annDay)
            return 1;
        return 0;
    }

    // ext PmReady(): the server has booked our day, with a new match tag.
    inline bool PmReady(const State& c, int day, int lmh0)
    {
        return c.ok && c.lday == day && (!c.lmh || c.lmh != lmh0);
    }

    constexpr int kPmRows = 5;    // ext PM_ROWS (the last row folds the rest into OTHER)
    struct PmRow {
        char label[24]{};
        int count = 0;
        int parts = 0;
    };

    // ext AwardHook's tally: uppercase the award label, fold overflow into OTHER.
    inline void NoteAwardRow(PmRow* row, int& n, const char* label, int amount)
    {
        if (!label || !row)
            return;
        char up[24];
        int i = 0;
        for (; label[i] && i < static_cast<int>(sizeof(up)) - 1; ++i)
            up[i] = (label[i] >= 'a' && label[i] <= 'z')
                  ? static_cast<char>(label[i] - 32) : label[i];
        up[i] = 0;
        if (!up[0])
            std::strcpy(up, "OTHER");
        for (i = 0; i < n && std::strcmp(row[i].label, up); ++i) {}
        if (i == n) {
            if (n == kPmRows)
                i = kPmRows - 1;
            else {
                std::strcpy(row[n].label, up);
                ++n;
            }
        }
        ++row[i].count;
        row[i].parts += amount;
    }

    // ---- runtime (game thread; synchronous RPC through net::daemon::Rpc) ------

    // Once per frame, BEFORE postmatch::Tick / requeue::Tick.
    void Tick();
    void Reset();

    // A 0xE1 reply arrived at least once this session.
    bool HaveState();
    const State& Get();

    // ext clan_get().ro (build 201): our own role from the 0xE1 state, 0 = plain.
    // 0 until a state arrives, so an answerless daemon never tints a chat name.
    // Others' roles come from the shared 0xBF table (net::rank_badge::RoleOf).
    int OwnRole();

    // ext PmReport: the 0xE2 match report was sent; arm the supply-counter page.
    // `labels` / `counts` are the ext's award-tail pairs (may be null / 0).
    void Report(uint32_t parts, uint8_t flags,
                const uint8_t* labels, const uint16_t* counts, int n);

    // AwardHook's contribution to the page's itemised breakdown.
    void NoteAward(const char* label, int amount);

    // Non-wire arm used while the ext owns the 0xE2 send: set the same local
    // page latch / tally sort / event bookkeeping as Report(), without sending
    // anything. Called once at match end so postmatch::Tick can drive the page
    // even when the ext is not the active bridge. Report() remains the wire
    // path and is intentionally left uncalled while both modules are loaded.
    void ArmPost(uint32_t parts);

    // ext EvOffer: decide whether the event page is due on the MP menu. Returns
    // true when a page push is due (the caller checks the stack first so the
    // ext's earlier push and ours cannot stack two copies of the same page).
    bool Offer(bool menuTop, bool navBusy, bool pageUp, uint64_t now);

    // Page handlers' observable edges. postmatch::Tick owns these calls now,
    // both for pages it pushes and for pages the ext pushed first (its handlers
    // still run while the ext module is the active bridge).
    void NotePostShown();
    void NotePostClosed(bool continued);
    void NoteEventShown();
    void NoteEventClosed();

    bool PostPending();
    bool PostShown();
    int  PostDay();
    int  PostParts();
    int  PostLmh0();                      // ext g_pm.lmh0 (match tag before the report)
    uint64_t PostSentAt();                // ext g_pm.sent_at (push gate + wait watchdog)
    const PmRow* PmRows(int* n);          // ext g_pm.row (sorted tally)
    bool EventPending();
    int  EventMode();
    const EventBook& Events();            // ext g_ev's bookkeeping
    bool Blocking();                      // either page is due (requeue's settle gate)

} // namespace net::clan
