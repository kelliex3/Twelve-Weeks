// Standalone harness for the shared No Return frame rules:
//   g++ -std=c++20 -Wall -Wextra -Werror -pedantic -I mod/src/net/ui
//       mod/tests/nrui_core_test.cpp -o /tmp/opencode/nrui_core_test
//   /tmp/opencode/nrui_core_test

#include "nrui-core.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

    using namespace net::ui::nrui::core;

    int fails = 0;
    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++fails;
    }

    const char* Upper(const char* s)
    {
        static char buf[64];
        std::snprintf(buf, sizeof buf, "%s", s ? s : "");
        for (char* p = buf; *p; ++p)
            if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 32);
        return buf;
    }

} // namespace

int main()
{
    std::printf("nrui core:\n");

    Check(kRight == 3700.0f && kLeft == 148.0f && kHeadPt == 54.0f,
          "right edge / left edge / header size match the ext");
    Check(kPromptY == 2000.0f && kPromptH == 88.0f && kPromptBoxW0 == 400.0f,
          "prompt band geometry is unchanged");
    Check(kNPrompt == 6 && kMaxTabs == 8 && kWeekX0 == 2940.0f,
          "six prompts, eight tabs, week block at 2940");

    Check(CharWidth(' ') == 0.40f && CharWidth('a') == 0.62f
          && CharWidth('A') == 0.80f && CharWidth(0x80) == 0.62f,
          "ext char widths: space / lower / upper / high byte");
    // "AB" = .8 + .8 = 1.6 * 36 = 57.6, x1.06 = 61.056.
    Check(std::fabs(Est("AB", 36.0f) - 57.6f) < 0.01f,
          "coarse estimate sums character widths * pt");
    Check(std::fabs(Tw("AB", 36.0f) - 61.056f) < 0.01f,
          "tracked width is est * 1.06");
    Check(Est("<font color=\"#fff\">AB</font>", 36.0f) == Est("AB", 36.0f),
          "colour markup is not measured");
    Check(Est("\xc4\x85" "B", 10.0f) == Est("\xc4" "B", 10.0f),
          "UTF-8 continuation bytes are skipped (the lead byte counts as one char)");
    Check(Est("ab", 10.0f, Upper) == Est("AB", 10.0f),
          "the translated string is measured");

    Check(FineCharWidth(' ') == 0.308f && FineCharWidth('A') == 0.803f
          && FineCharWidth(0x80) == 0.66f && FineCharWidth('\n') == 0.0f,
          "fine widths: space / upper / high byte / control");
    // "VOTE" = .946 + 1.045 + .867 + .803 = 3.049 * 36 * 1.06 = 116.34984.
    Check(std::fabs(TwFine("VOTE", 36.0f) - 116.34984f) < 0.01f,
          "fine width uses the per-character advance table with tracking");
    Check(TwFine("<font color=\"#fff\">VOTE</font>", 36.0f) == TwFine("VOTE", 36.0f),
          "colour markup is not measured by the fine width either");
    Check(TwFine("vote", 10.0f, Upper) == TwFine("VOTE", 10.0f),
          "the translated string is measured with the fine widths");

    Check(RuleScale(500.0f) == 0.5f && RuleScale(0.0f) == 0.002f,
          "rules use the 1000-unit template with a 2-unit floor");
    Check(!BarVisible(0.004f) && BarVisible(0.005f)
          && !BarVisible(-1.0f) && BarVisible(2.0f),
          "bars hide at <= 0.004 and clamp an over-full fraction to 1");
    Check(std::fabs(BarScale(100.0f, 1100.0f, 0.5f) - 0.5f) < 0.0001f,
          "a half-full bar over 1000 units scales to 0.5");
    Check(FitScale(200.0f, 100.0f) == 0.6f,
          "fit shrink is clamped to 0.6 minimum");
    Check(FitScale(10.0f, 100.0f) == 1.0f && FitScale(200.0f, 100.0f) == 0.6f,
          "fit never grows and never goes below 0.6");

    HeaderPos h = HeaderPositions(Tw("FOUND MATCH", kHeadPt), Tw("MAP VOTE", kHeadPt));
    Check(h.divX > kLeft && h.pageX > h.divX && h.ruleX > h.pageX && h.ruleW > 0.0f,
          "breadcrumb divider / page / rule advance left to right");
    char db[64], sv[64];
    WeekText(3, 2, 17, db, sizeof db, sv, sizeof sv);
    Check(std::strcmp(db, "WEEK 3: DAY 2") == 0
          && std::strcmp(sv, "DAYS SURVIVED 17 | 84") == 0,
          "week block text");
    Check(WeekFrac(84) == 1.0f && WeekFrac(-3) == 0.0f
          && std::fabs(WeekFrac(42) - 0.5f) < 0.0001f,
          "survival bar fraction clamps to 0..1");

    PromptKey k = PromptKeyOf("menu:prev+menu:next", nullptr, false);
    Check(k.glyph && std::strcmp(k.k1, "menu:prev") == 0
          && std::strcmp(k.k2, "menu:next") == 0 && k.kw == 136.0f,
          "a two-glyph key splits on '+' and gets the 136-unit box");
    PromptKey one = PromptKeyOf("menu:select", nullptr, false);
    Check(one.glyph && one.k2[0] == 0 && one.kw == 64.0f,
          "one glyph gets the 64-unit box");
    PromptKey plain = PromptKeyOf("W A S D", "menu:prev+menu:next", false);
    Check(!plain.glyph && plain.kw > 60.0f,
          "plain key text is measured and floored at 60");
    PromptKey padKey = PromptKeyOf("A / D", "menu:prev+menu:next", true);
    Check(padKey.glyph && padKey.kw == 136.0f,
          "pad mode swaps in the pad glyph");

    PromptBox b = PromptBoxAt(one, "VOTE", kRight, true);
    // "VOTE" = 116.34984 fine; tot = 18+64+22+116.34984+26.
    Check(std::fabs(b.tot - 246.34984f) < 0.01f,
          "prompt tail is +26 with the fine label width");
    Check(std::fabs(b.x1 - kRight) < 0.0001f && std::fabs(b.x0 - (kRight - b.tot)) < 0.0001f,
          "a prompt box is laid out with its right edge at NR_R1");
    Check(std::fabs(b.alpha - 70.0f / 255.0f) < 0.0001f
          && std::fabs(PromptBoxAt(one, "VOTE", kRight, false).alpha - 30.0f / 255.0f) < 0.0001f,
          "enabled / disabled prompt alphas");

    PromptBox boxes[kNPrompt]{};
    boxes[0] = PromptBoxAt(PromptKeyOf("menu:select", nullptr, false), "VOTE", kRight, true);
    boxes[1] = PromptBoxAt(PromptKeyOf("A / D", nullptr, false), "SWITCH", boxes[0].x0 - 28.0f, true);
    Check(PromptHover(boxes[0].x0 + 1.0f, kPromptY, boxes, 2) == 1,
          "hover maps the rightmost box to declaration index 1");
    Check(PromptHover(boxes[1].x0 + 1.0f, kPromptY, boxes, 2) == 0,
          "hover maps the next box to declaration index 0");
    Check(PromptHover(kRight + 1.0f, kPromptY, boxes, 2) == -1,
          "a cursor past the right edge hits nothing");
    Check(PromptHover(boxes[0].x0 + 1.0f, kPromptY - 60.0f, boxes, 2) == -1,
          "a cursor outside the prompt band hits nothing");

    SectionLine s = SectionRule(148.0f, 2740.0f, Tw("PLAYERS", 36.0f), Tw("2 | 8 VOTED", 33.0f));
    Check(s.xa > 148.0f && s.xb < 2740.0f && s.width > 0.0f,
          "section rule fits between title and right text");

    float px = 0.0f, py = 0.0f, sx = 0.0f, sy = 0.0f;
    FramePiece(0, 100.0f, 200.0f, 400.0f, 80.0f, px, py, sx, sy);
    Check(px == 100.0f && py == 202.0f && sx == 4.0f && sy == 1.0f,
          "focus frame top hairline sits at y+2 and scales by w/100");
    FramePiece(3, 100.0f, 200.0f, 400.0f, 80.0f, px, py, sx, sy);
    Check(px == 496.0f && py == 200.0f && sx == 1.0f && sy == 0.8f,
          "focus frame right hairline sits at x+w-4 and scales by h/100");

    float v[4]{};
    Check(!ThumbRect(0.0f, 0.0f, 1000.0f, 100.0f, 0, 8, 8, v),
          "scroll thumb hides when all rows fit");
    Check(ThumbRect(0.0f, 0.0f, 1000.0f, 100.0f, 0, 4, 8, v)
          && v[0] == -2.0f && v[1] == 0.0f && v[2] == 500.0f && v[3] == 100.0f,
          "scroll thumb covers the first half of the track");
    Check(ThumbRect(0.0f, 0.0f, 100.0f, 100.0f, 7, 1, 100, v) && v[2] == 8.0f,
          "scroll thumb has an 8-unit floor");

    Check(std::fabs(GlideStep(16.0f) - (1.0f - std::exp(-16.0f / 38.0f))) < 0.0001f,
          "glide uses exp(-dt / 38ms)");
    Check(GlideStep(1000.0f) == GlideStep(100.0f), "glide delta is capped at 100 ms");
    Check(GlideDone(9.6f, 10.0f) && !GlideDone(9.0f, 10.0f),
          "glide finishes within 0.5 units");

    const char* labels[] = { "OVERVIEW", "SURVIVORS", "RAIDS", "JOURNAL", "MISSIONS" };
    TabLayout tabs;
    BuildTabs(tabs, labels, 5);
    Check(tabs.n == 5 && tabs.x[0] == kLeft + 108.0f && tabs.x[1] > tabs.x[0],
          "clan tabs start at 256 and advance by measured width + 80");
    Check(TabHover(tabs.x[1] + 1.0f, kTabY, tabs) == 1,
          "tab hover finds the second tab");
    Check(TabHover(kLeft, kTabY, tabs) == -2, "the Q zone is -2");
    float ex = tabs.x[4] + tabs.w[4] + 28.0f;
    Check(TabHover(ex + 10.0f, kTabY, tabs) == -3,
          "past the last tab's hit box the E zone is -3");
    Check(TabHover(1500.0f, kTabY + 61.0f, tabs) == -1, "hover outside the band is none");
    Check(WrapTab(-1, 5) == 4 && WrapTab(5, 5) == 0 && WrapTab(2, 5) == 2,
          "tab navigation wraps both ways");

    if (fails) std::printf("nrui core: %d FAILED\n", fails);
    else       std::printf("nrui core: all passed\n");
    return fails ? 1 : 0;
}
