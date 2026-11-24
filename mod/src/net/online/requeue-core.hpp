#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure core of the build-201 requeue reply parser (tools/ext/requeue.c RqpParse
// and its persistent-lobby counters). Inline so mod/tests/requeue_core_test.cpp
// can exercise the exact wire fields without Windows or the daemon.
//
// 0xBD text: "... rq=<0|1> rqw=<n> [rqs=<s> rqb=<n> rqp=<n>]"
//   rq  ticket held for us, rqw players our search still waits for
//   rqs whole seconds until the next match forms anyway (0 = not held)
//   rqb members back (searching), rqp members still on the post-match screens
// An older daemon sends neither rqw nor the rqX fields: all of them stay 0.

namespace net::requeue::core {

    struct Answer {
        int rq = 0;                 // 0/1
        int w = 0;                  // rqw
        int left = 0;               // rqs
        int back = 0;               // rqb
        int post = 0;               // rqp
    };

    inline void ParseReply(const char* t, Answer& a)
    {
        a = Answer{};
        if (!t)
            return;
        char k[16];
        int v = 0, n = 0;
        while (std::sscanf(t, " %15[^= ]=%d%n", k, &v, &n) == 2) {
            if (!std::strcmp(k, "rq"))       a.rq = v;
            else if (!std::strcmp(k, "rqw")) a.w = v;
            else if (!std::strcmp(k, "rqs")) a.left = v;
            else if (!std::strcmp(k, "rqb")) a.back = v;
            else if (!std::strcmp(k, "rqp")) a.post = v;
            t += n;
        }
    }

    struct Lobby {
        int left = 0;               // countdown, clamped at 0
        int back = 0;
        int post = 0;
    };

    // The ext counts rqs down locally between its 1.5 s polls: rqs seconds from
    // the moment the reply arrived, rounded up (ms > 0 -> ceil), and 0 once the
    // deadline passed. Called only while our search is held (rq == 1, rqw > 0).
    inline Lobby LobbyCounts(const Answer& a, uint64_t now, uint64_t at)
    {
        Lobby out;
        out.back = a.back;
        out.post = a.post;
        if (a.left > 0) {
            const int64_t ms = static_cast<int64_t>(a.left) * 1000
                - static_cast<int64_t>(now - at);
            out.left = ms > 0 ? static_cast<int>((ms + 999) / 1000) : 0;
        }
        return out;
    }

} // namespace net::requeue::core
