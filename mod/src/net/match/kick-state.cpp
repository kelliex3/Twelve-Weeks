// ported from tools/ext/t2r-ext.c (MsgIsKick / MsgIsMove / g_kickMs in MsgHook)

#include "kick-state.hpp"

#include "log.hpp"             // TO1_LOG
#include "net/ui/notice.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace net::match::kick {

    namespace {

        struct State {
            uint64_t at = 0;   // ext g_kickMs
        } gKick;

        bool Prefix(const char* t, const char* p, size_t n)
        {
            return t && !strncmp(t, p, n);
        }

    }

    bool IsMove(const char* t)
    {
        return Prefix(t, "TEAMS: ", 7)
            || Prefix(t, "DRU\xC3\x85\xC2\xBBYNY: ", 10);
    }

    bool IsKick(const char* t)
    {
        return Prefix(t, "You got kicked from the match", 29)
            || Prefix(t, "Wyrzucono ci", 12)
            || IsMove(t);
    }

    void Note(const char* t)
    {
        if (!IsKick(t))
            return;
        gKick.at = GetTickCount64();
        TO1_LOG("kick: %s text \"%.120s\" (stamp %llu)\n",
            IsMove(t) ? "team move" : "kick", t ? t : "",
            static_cast<unsigned long long>(gKick.at));
    }

    uint64_t ShownAt() { return gKick.at; }

    bool Since(uint64_t when) { return gKick.at && gKick.at > when; }

    void InstallNoticeHooks()
    {
        net::notice::Hooks h{};
        h.isKick = &IsKick;
        h.kickShownAt = &ShownAt;
        net::notice::SetHooks(h);
        // kickSet is deliberately left null: kicknote.c (the MP-menu box) is not
        // ported yet, so the kick keeps its server toast.
    }

    void Reset()
    {
        gKick = State{};
    }

}
