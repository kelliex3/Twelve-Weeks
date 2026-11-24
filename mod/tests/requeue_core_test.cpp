// Standalone harness for the pure requeue core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/online
//       mod/tests/requeue_core_test.cpp -o /tmp/requeue_core_test && /tmp/requeue_core_test
//
// Covers the build-201 0xBD rqs / rqb / rqp fields and the local lobby countdown.

#include "requeue-core.hpp"

#include <cstdio>
#include <cstring>

namespace {

    using namespace net::requeue::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void ReplyTests()
    {
        Answer a;
        ParseReply("mode=2 n=4 rq=1 rqw=3 rqs=90 rqb=2 rqp=3", a);
        Check(a.rq == 1 && a.w == 3 && a.left == 90 && a.back == 2 && a.post == 3,
              "build-201 reply parses rq / rqw / rqs / rqb / rqp beside the counts");

        ParseReply("rq=1 rqw=2", a);
        Check(a.rq == 1 && a.w == 2 && a.left == 0 && a.back == 0 && a.post == 0,
              "older daemon without the rqX fields leaves plain zeros");

        ParseReply("rq=0 rqw=0 rqs=0 rqb=0 rqp=0", a);
        Check(a.rq == 0 && a.w == 0 && a.left == 0 && a.back == 0 && a.post == 0,
              "a released ticket reports no lobby");

        ParseReply(nullptr, a);
        Check(a.rq == 0 && a.w == 0 && a.left == 0 && a.back == 0 && a.post == 0,
              "a missing reply is not a ticket");

        ParseReply("nonsense", a);
        Check(a.rq == 0 && a.back == 0, "unrecognised keys are skipped");
    }

    void CountdownTests()
    {
        Answer a;
        ParseReply("rq=1 rqw=2 rqs=90 rqb=2 rqp=3", a);

        Lobby l = LobbyCounts(a, 1000, 1000);
        Check(l.left == 90 && l.back == 2 && l.post == 3,
              "the countdown starts at the whole rqs value");

        l = LobbyCounts(a, 31000, 1000);
        Check(l.left == 60 && l.back == 2 && l.post == 3,
              "the countdown ticks down between the 1.5 s polls");

        l = LobbyCounts(a, 99999, 1000);
        Check(l.left == 0 && l.back == 2 && l.post == 3,
              "the countdown clamps at 0, the members stay visible");

        ParseReply("rq=1 rqw=1 rqs=0 rqb=1 rqp=0", a);
        l = LobbyCounts(a, 1000, 1000);
        Check(l.left == 0 && l.back == 1 && l.post == 0,
              "an unknown deadline still shows the members back");

        // The ext's chat LOBBY gate: g_rq_back + g_rq_post > 0.
        Check(l.back + l.post > 0, "the lobby is open while members are back");
    }

} // namespace

int main()
{
    ReplyTests();
    CountdownTests();
    std::printf("%s\n", fails ? "REQUEUE_CORE_TEST_FAIL" : "REQUEUE_CORE_TEST_OK");
    return fails ? 1 : 0;
}
