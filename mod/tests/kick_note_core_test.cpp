// Standalone harness for the pure kick-notice core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/kick_note_core_test.cpp -o /tmp/kick_note_core_test && /tmp/kick_note_core_test
//
// Covers the ext kick text parser (EN / PL, AFK / ping / packet-loss) and the
// page push / dedupe timing gates.

#include "kick-note-core.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::kick_note::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    bool Has(const char* hay, const char* needle)
    {
        return hay && needle && std::strstr(hay, needle) != nullptr;
    }

    void ParseTests()
    {
        Parsed p = Parse("You were removed for no input for 95 seconds (AFK).");
        Check(p.title == Title::Kicked && Has(p.msg, "NO INPUT FOR 95 SECONDS (AFK)."),
              "AFK text becomes the canonical no-input message");

        p = Parse("Wyrzucono cię z meczu: brak aktywności przez 120 s (AFK).");
        Check(p.title == Title::Kicked && Has(p.msg, "NO INPUT FOR 120 SECONDS (AFK)."),
              "Polish AFK text is parsed too");

        p = Parse("An admin removed you from the match. Reason: \"cheating <b>\"");
        Check(p.title == Title::Kicked && Has(p.msg, "REASON:") && Has(p.msg, "cheating")
              && !Has(p.msg, "<b>"), "admin kick reason is extracted and tags stripped");

        p = Parse("You were removed from the match.");
        Check(p.title == Title::Kicked && Has(p.msg, "AN ADMIN REMOVED YOU FROM THE MATCH.")
              && !Has(p.msg, "REASON"), "a plain kick needs no reason clause");

        p = Parse("Your ping to the host was 450 MS for over 12 seconds. The limit is 300 MS.");
        Check(p.title == Title::Ping && Has(p.msg, "450 MS") && Has(p.msg, "12 SECONDS")
              && Has(p.msg, "300 MS"), "ping kick extracts all three numbers");

        p = Parse("Your ping to the host was too high.");
        Check(p.title == Title::Ping && Has(p.msg, "TOO HIGH"),
              "a short ping fallback still reports a ping removal");

        p = Parse("Your connection lost 37 percent of its packets for over 9 seconds (limit 25 percent). You can search again now.");
        Check(p.title == Title::PacketLoss && Has(p.msg, "37 PERCENT") && Has(p.msg, "SEARCH AGAIN NOW")
              && !Has(p.msg, "SECOND TIME"), "re-searchable packet loss keeps its own message");

        p = Parse("Twoje połączenie gubiło 42 procent pakietów przez ponad 8 s (limit 25 procent), drugi raz w krótkim czasie.");
        Check(p.title == Title::PacketLoss && Has(p.msg, "42 PERCENT") && Has(p.msg, "SECOND TIME"),
              "the Polish packet-loss repeat branch is parsed");

        p = Parse("You got kicked from the match: your connection lost some of its packets for over a while (limit a few).");
        Check(p.title == Title::PacketLoss && Has(p.msg, "TOO MANY PACKETS"),
              "a short packet-loss fallback stays packet-loss titled");

        p = Parse("TEAMS: red vs blue");
        Check(IsMove(p.msg) || IsMove("TEAMS: red vs blue"),
              "a team move is not a kick box");
        Check(IsMove("DRUŻYNY: czerwoni vs niebiescy"), "the Polish team move is recognised");
    }

    void HelperTests()
    {
        int v[3]{};
        Check(Digits("abc 12 and 345 then 6", 3, v) == 3 && v[0] == 12 && v[1] == 345 && v[2] == 6,
              "Digits picks up to count decimal groups");

        char reason[200];
        Reason("Reason: \"team damage\" trailing", reason, sizeof reason);
        Check(std::strcmp(reason, "team damage") == 0, "Reason extracts a quoted reason");
        Reason("no reason here", reason, sizeof reason);
        Check(reason[0] == '\0', "Reason is empty when no quotes exist");
    }

    void GateTests()
    {
        Check(ShouldPush(State::None, 1000, 0, 0) == false,
              "no state never pushes");
        Check(ShouldPush(State::Due, 1000, 0, 0) == true,
              "a due notice pushes at once");
        Check(ShouldPush(State::Pushed, 1000 + kRetryMs, 1000, 1) == false,
              "a push is not retried inside the retry window");
        Check(ShouldPush(State::Pushed, 1000 + kRetryMs + 1, 1000, 1) == true,
              "a push is retried after the window");
        Check(ShouldPush(State::Pushed, 1000 + kRetryMs + 1, 1000, kMaxPushes) == false,
              "pushes are capped");
        Check(DropPushes(State::Pushed, 1000 + kRetryMs + 1, 1000, kMaxPushes) == true,
              "an exhausted push state drops out");
        Check(DropPushes(State::Up, 99999, 1000, kMaxPushes) == false,
              "an already up page is not dropped");
        Check(Ramp(1000, 1000, kFadeMs) == 0.0f, "ramp starts at zero");
        Check(Ramp(1000 + kFadeMs / 2, 1000, kFadeMs) > 0.49f
              && Ramp(1000 + kFadeMs / 2, 1000, kFadeMs) < 0.51f,
              "ramp is linear through the middle");
        Check(Ramp(1000 + kFadeMs, 1000, kFadeMs) == 1.0f, "ramp saturates at one");
        Check(Duplicate("same text", "same text", 1000, 1000 + kDedupeMs - 1) == true,
              "identical text is deduped inside the window");
        Check(Duplicate("same text", "same text", 1000, 1000 + kDedupeMs) == false,
              "the dedupe window expires");
        Check(Duplicate("old text", "new text", 1000, 1100) == false,
              "different texts are never deduped");
    }

} // namespace

int main()
{
    ParseTests();
    HelperTests();
    GateTests();
    std::printf("%s\n", fails ? "kick-note core: FAILED" : "kick-note core: all tests passed");
    return fails ? 1 : 0;
}
