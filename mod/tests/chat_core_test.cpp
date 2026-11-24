// Standalone harness for the pure chat core (no game headers / no Windows):
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src/net/chat
//       mod/tests/chat_core_test.cpp -o /tmp/chat_core_test && /tmp/chat_core_test
//
// Port of tools/ext/tests/chat_test.c plus the peer packet / dedupe / party reply
// cases that the engine-facing chat.cpp depends on.

#include "chat-core.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

    using namespace net::chat::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    void TestCensor()
    {
        struct Case { const char* in; const char* want; };
        const Case cases[] = {
            { "gg wp", "gg wp" },
            { "you nigger", "you ******" },
            { "NIGGAS lol", "****** lol" },
            { "n1gg4 pls", "***** pls" },
            { "niiigaaa", "********" },
            { "stop being a faggot.", "stop being a ******." },
            { "fag", "***" },
            { "f@ggot", "******" },
            { "retarded team", "******** team" },
            { "dykes", "*****" },
            { "trannies", "********" },
            { "chinks", "******" },
            { "kike!", "****!" },
            { "spic spice", "**** spice" },
            { "czarnuchy", "*********" },
            { "coon raccoon", "**** raccoon" },
            { "snigger", "snigger" },
            { "Niger Nigeria", "Niger Nigeria" },
        };
        for (const Case& c : cases) {
            char b[kLineBytes];
            std::snprintf(b, sizeof b, "%s", c.in);
            Censor(b);
            Check(!std::strcmp(b, c.want), c.in);
        }
    }

    void TestClean()
    {
        char out[64];
        Check(Clean("  hello\t<world>| friend \n", 25, out, sizeof out, 200) == 18
              && !std::strcmp(out, "hello world friend"), "clean strips markup / collapses spaces");
        Check(Clean("\xE2\x80\x9Cquote\xE2\x80\x9D", 11, out, sizeof out, 200) == 7
              && !std::strcmp(out, "\"quote\""), "clean folds curly quotes");
        Check(Clean("abcdef", 6, out, sizeof out, 3) == 3 && !std::strcmp(out, "abc"),
              "clean caps character count");
    }

    void TestPackets()
    {
        WirePacket p[8];
        const char* shortText = "hello team";
        size_t n = BuildPackets(p, 8, kTeam, 0x1234, shortText);
        Check(n == 1 && p[0].size == 6 + std::strlen(shortText), "short line is one ext v1 packet");
        Check(p[0].data[0] == kPacketVer && p[0].data[1] == kTeam
              && p[0].data[2] == 0x34 && p[0].data[3] == 0x12
              && p[0].data[4] == 10 && p[0].data[5] == 0, "v1 header layout");

        std::string longText;
        for (int i = 0; i < 300; i++) longText += 'a';
        longText += "\xC4\x85";   // Polish a, tests the UTF-8 trim on a chunk edge
        n = BuildPackets(p, 8, kMatch, 7, longText.c_str());
        Check(n == 2, "long line becomes two v2 fragments");
        Check(p[0].data[0] == kFragmentVer && p[0].data[6] == 0 && p[0].data[7] == 2,
              "first fragment header");
        Check(p[1].data[6] == 1 && p[1].data[7] == 2, "second fragment header");

        Receiver rx;
        Reassembled line;
        Check(!rx.Accept(1, p[1].data, p[1].size, line), "one fragment is not a line");
        Check(rx.Accept(1, p[0].data, p[0].size, line), "both fragments emit one line");
        const std::string shown(200, 'a');   // Clean caps a line at CH_MAXC characters
        Check(line.channel == kMatch && line.seq == 7 && !std::strcmp(line.text, shown.c_str()),
              "reassembled text matches");
        Check(!rx.Accept(1, p[0].data, p[0].size, line), "resent fragment is deduped");

        Receiver rx2;
        Check(!rx2.Accept(2, p[0].data, p[0].size, line) && rx2.Accept(2, p[1].data, p[1].size, line),
              "dedupe and assembly are per slot");
        uint8_t bad[8] = { kPacketVer, 0, 0, 0, 0, 0, 'x', 0 };
        ParsedPacket parsed;
        Check(!ParsePacket(bad, sizeof bad, parsed), "zero length is rejected");
        bad[4] = 1;
        Check(ParsePacket(bad, sizeof bad, parsed) && parsed.textLen == 1, "one byte is valid");
    }

    void TestResend()
    {
        WirePacket p[1];
        BuildPackets(p, 1, kMatch, 1, "resend me");
        Resender r;
        r.Add(p[0], 1000);
        int sent = 0;
        r.Tick(1149, [&](const WirePacket&) { sent++; });
        Check(sent == 0, "no resend before 150 ms");
        r.Tick(1150, [&](const WirePacket&) { sent++; });
        Check(sent == 1, "copy 2 at 150 ms");
        r.Tick(1450, [&](const WirePacket&) { sent++; });
        r.Tick(1600, [&](const WirePacket&) { sent++; });
        Check(sent == 2, "copy 3 at 450 ms only");
    }

    void TestPartyReply()
    {
        const char* json =
            "{\"ok\":1,\"err\":\"\",\"lk\":42,\"pchat\":{\"last\":9,\"lines\":["
            "{\"id\":8,\"name\":\"Ann\",\"t\":\"hello <all>\",\"me\":0,\"age\":3},"
            "{\"id\":9,\"name\":\"Me\",\"t\":\"hi there\",\"me\":1,\"age\":0}]}}";
        PartyReply d;
        Check(ParsePartyReply(d, json, std::strlen(json)), "party reply parses");
        Check(d.ok && d.has && d.last == 9 && d.lk == 42 && d.n == 2, "party reply fields");
        Check(!std::strcmp(d.lines[0].name, "Ann")
              && !std::strcmp(d.lines[0].text, "hello all")
              && d.lines[0].id == 8, "party line cleaned");
        Check(d.lines[1].me == 1 && !std::strcmp(d.lines[1].text, "hi there"), "own party line");
        Check(!ParsePartyReply(d, "{", 1), "truncated reply fails");
    }

} // namespace

int main()
{
    TestCensor();
    TestClean();
    TestPackets();
    TestResend();
    TestPartyReply();
    std::printf("chat_core_test: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
