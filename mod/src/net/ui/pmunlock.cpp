// Ported from tools/ext/pmunlock.c. The ext kept three feature-local statics
// (the match-start unlock grid, the achievement list and the staggered rows);
// they live here, while the generated widget paths stay with the page owner
// in postmatch-screens.cpp. Every widget write tolerates a missing widget.
#include "pmunlock.hpp"

#include "boot.hpp"
#include "achieve.hpp"
#include "customize.hpp"
#include "customize-core.hpp"
#include "customize-items.hpp"
#include "nrui-core.hpp"
#include "engine/locale.hpp"
#include "engine/widget.hpp"
#include "net/match/match-liveness.hpp"
#include "log.hpp"

#include <cstdio>
#include <cstring>

namespace net::ui::pmunlock {

    namespace {

        using core::Grid;
        using core::Lists;

        constexpr int kMeleeCat = 5;    // CZ_MELEE (the generated header uses a macro)

        Grid        g_now{}, g_eligible{}, g_story{};
        core::Snapshot g_snap;
        Lists       g_lists;
        bool        g_wasLive = false;
        bool        g_disabled = false;

        bool Disabled()
        {
            return boot::ExtLoaded() || g_disabled;
        }

        int SexOf(const customize::core::Profile& p, int f)
        {
            return p.sex[f] < core::kSex
                 ? static_cast<int>(p.sex[f]) : 0;
        }

        // Fill the three grids from the shared CUSTOMIZE tables. The ext read
        // g_cz directly; here g_now is a view of the same in-memory profile.
        void ReadProfile()
        {
            customize::EnsureLoaded();
            const customize::core::Profile& p = customize::GetProfile();
            g_now = Grid{};
            g_eligible = Grid{};
            g_story = Grid{};
            for (int f = 0; f < core::kFac; ++f) {
                const int s = SexOf(p, f);
                for (int c = 0; c < core::kWear; ++c) {
                    for (int k = 0; k < core::kMaxIt; ++k) {
                        const auto* it = customize::core::Item(f, s, c, k);
                        if (!it) continue;
                        g_now[f][s][c][k] = customize::core::Unlocked(p, it);
                        g_eligible[f][s][c][k] = it->uk != 0 && it->tier <= 1;
                        g_story[f][s][c][k] = customize::core::StoryMask(it);
                    }
                }
            }
        }

        void Rebuild(int clanFac, uint64_t now)
        {
            ReadProfile();
            const int ff = core::FacFirst(clanFac);
            int n = 0;
            const auto* src = achieve::MatchNew(&n);
            core::Achievement ach[core::kMaxA]{};
            for (int i = 0; i < n && i < core::kMaxA; ++i) {
                std::snprintf(ach[i].id, sizeof ach[i].id, "%s", src[i].id);
                std::snprintf(ach[i].title, sizeof ach[i].title, "%s", src[i].title);
                std::snprintf(ach[i].desc, sizeof ach[i].desc, "%s", src[i].desc);
            }
            const customize::core::Profile& p = customize::GetProfile();
            const int sex[core::kFac] = { SexOf(p, 0), SexOf(p, 1) };
            core::Build(g_snap, g_now, g_eligible, g_story, sex,
                        ach, n, ff, now, g_lists);
        }

        // ---- widget helpers (the ext's ui_* / pmnr_* / aui_ach) ----------------
        void Text(const char* path, const char* s)
        {
            const uint64_t w = engine::widget::Page(path);
            if (!w) return;
            char b[1536];
            std::snprintf(b, sizeof b, "%s", engine::locale::Tr(s ? s : ""));
            engine::widget::SetText(w, b);
        }

        void Image(const char* path, const char* s)
        {
            engine::widget::SetImage(engine::widget::Page(path), s ? s : "");
        }

        void Vis(const char* path, bool on)
        {
            engine::widget::Visible(engine::widget::Page(path), on);
        }

        void Alpha(const char* path, float a)
        {
            engine::widget::Alpha(engine::widget::Page(path), a);
        }

        void Fit(const char* path, const char* s, float box, float pt)
        {
            Text(path, s);
            const uint64_t w = engine::widget::Page(path);
            if (!w) return;
            const char* tr = engine::locale::Tr(s ? s : "");
            const float est = nrui::core::Tw(tr ? tr : (s ? s : ""), pt);
            const float sc = nrui::core::FitScale(est, box);
            engine::widget::Scale(w, sc, sc);
        }

        // nr_section(base, x0, x1, y, title, right, 36)
        void Section(const char* base, float x0, float x1, float y,
                     const char* title, const char* right)
        {
            char p[160];
            std::snprintf(p, sizeof p, "%sTx", base);
            Text(p, title);
            std::snprintf(p, sizeof p, "%sRt", base);
            Text(p, right);
            const float xa = x0 + nrui::core::Tw(title, core::kTitlePt) + 40.0f;
            const float xb = x1 - (right && right[0]
                ? nrui::core::Tw(right, core::kRightPt) + 40.0f : 0.0f);
            std::snprintf(p, sizeof p, "%sRl", base);
            const uint64_t rule = engine::widget::Page(p);
            if (!rule) return;
            engine::widget::Pivot(rule, xa, y);
            engine::widget::Scale(rule, nrui::core::RuleScale(xb - xa), 1.0f);
        }

        const char* AchText(const char* id, int desc, const char* en,
                            char* out, size_t cap)
        {
            char key[96];
            std::snprintf(key, sizeof key, desc ? "ach.%s.desc" : "ach.%s.title",
                          id ? id : "");
            const char* t = engine::locale::Tr(key);
            if (t && std::strcmp(t, key) != 0)
                std::snprintf(out, cap, "%s", t);
            else
                std::snprintf(out, cap, "%s", en ? en : "");
            return out;
        }

        const char* Tr(const char* s)
        {
            const char* t = engine::locale::Tr(s ? s : "");
            return t ? t : (s ? s : "");
        }

    } // namespace

    void Tick(uint64_t now)
    {
        (void)now;
        if (Disabled()) return;
        const bool live = net::match::liveness::Live();
        if (live && !g_wasLive) {
            ReadProfile();
            core::TakeSnapshot(g_now, g_snap);
            achieve::NoteMatchStart();
            g_lists = Lists{};
            TO1_LOG("pmunlock: match start - snapshot read\n");
        }
        g_wasLive = live;
    }

    int Count(int clanFac, uint64_t now)
    {
        if (Disabled()) return 0;
        const int ff = core::FacFirst(clanFac);
        if (core::NeedRebuild(now, g_lists, ff)) Rebuild(clanFac, now);
        return g_lists.na + g_lists.ngt;
    }

    void Fill(int clanFac, uint64_t now)
    {
        if (Disabled()) return;
        Rebuild(clanFac, now);
        char p[128], b[256], t[256];
        for (int i = 0; i < g_lists.na; ++i)
            achieve::DropNew(g_lists.a[i].id);

        std::snprintf(b, sizeof b, "%d", g_lists.na);
        Section("anim-group.unlocks.pmSecUa", core::kLx0, core::kLx1,
                core::kSecY, "ACHIEVEMENTS", g_lists.na ? b : "");
        std::snprintf(b, sizeof b, "%d", g_lists.ngt);
        Section("anim-group.unlocks.pmSecUc", core::kRx0, core::kRx1,
                core::kSecY, "NEW UNLOCKS", g_lists.ngt ? b : "");

        const float twa = core::kLx1 - core::kLx0 - core::kTx - 50.0f;
        const float twc = core::kRx1 - core::kRx0 - core::kTx - 50.0f;
        for (int k = 0; k < core::kRows; ++k) {
            const bool on = k < g_lists.na;
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUa%d", k);
            Vis(p, on);
            if (on) {
                const auto& a = g_lists.a[k];
                std::snprintf(p, sizeof p, "anim-group.unlocks.pmUa%d.pmUa%dTi", k, k);
                Fit(p, AchText(a.id, 0, a.title, t, sizeof t), twa, 39.0f);
                std::snprintf(p, sizeof p, "anim-group.unlocks.pmUa%d.pmUa%dDs", k, k);
                Text(p, AchText(a.id, 1, a.desc, t, sizeof t));
                std::snprintf(p, sizeof p, "anim-group.unlocks.pmUa%d.pmUa%dIc", k, k);
                std::snprintf(b, sizeof b, "streaming/t2r/ach/%s.png", a.id);
                Image(p, b);
            }
            const bool gearOn = k < g_lists.ng;
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUc%d", k);
            Vis(p, gearOn);
            if (!gearOn) continue;
            const auto& row = g_lists.g[k];
            const auto* it = customize::core::Item(row.f, row.s, row.c, row.k);
            if (!it) continue;
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUc%d.pmUc%dTi", k, k);
            Fit(p, Tr(it->label), twc, 39.0f);
            char cat[96];
            std::snprintf(cat, sizeof cat, "%s", Tr(customize::core::CZ_CAT_LABEL[row.c]));
            std::snprintf(b, sizeof b, "%s  |  %s",
                          Tr(customize::core::kFacName[row.f]), cat);
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUc%d.pmUc%dSub", k, k);
            Text(p, b);
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUc%d.pmUc%dCh", k, k);
            Fit(p, Tr(customize::core::CZ_CHAL[it->uk].title), twc, 30.0f);
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUc%d.pmUc%dPic", k, k);
            Vis(p, it->pic != nullptr);
            if (it->pic) {
                std::snprintf(b, sizeof b, "streaming/t2r/icons/%s.png", it->pic);
                Image(p, b);
            }
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUc%d.pmUc%dIc", k, k);
            Vis(p, it->pic == nullptr);
            if (!it->pic) {
                const char* ic = row.c == kMeleeCat
                    ? (std::strstr(it->label, "KNIFE") ? "cz-knife" : "cz-fists")
                    : customize::core::kCatIcon[row.c];
                std::snprintf(b, sizeof b, "streaming/t2r/icons/%s.png", ic);
                Image(p, b);
            }
        }
        Vis("anim-group.unlocks.pmUaNone", g_lists.na == 0);
        Vis("anim-group.unlocks.pmUcNone", g_lists.ngt == 0);

        char more[128];
        core::MoreText(g_lists.na, more, sizeof more);
        Vis("anim-group.unlocks.pmUaMore", g_lists.na > core::kRows);
        if (g_lists.na > core::kRows)
            Text("anim-group.unlocks.pmUaMore", more);
        Vis("anim-group.unlocks.pmUcFoot", g_lists.ngt > 0);
        core::GearFootText(g_lists.ngt, more, sizeof more);
        Text("anim-group.unlocks.pmUcFoot", more);
        const uint64_t foot = engine::widget::Page("anim-group.unlocks.pmUcFoot");
        if (foot) {
            engine::widget::Pivot(foot, core::kRx0,
                core::GearFootY(g_lists.ngt < core::kRows ? g_lists.ngt : core::kRows));
        }
        TO1_LOG("pmunlock: unlocks page - %d achievement(s), %d item(s) (%d listed)\n",
                g_lists.na, g_lists.ngt, g_lists.ng);
    }

    void Frame(int clanFac, uint64_t now, uint64_t elapsed)
    {
        if (Disabled()) return;
        const int na = g_lists.na, ngt = g_lists.ngt;
        if (core::NeedRebuild(now, g_lists, core::FacFirst(clanFac))) {
            Rebuild(clanFac, now);
            if (g_lists.na != na || g_lists.ngt != ngt) Fill(clanFac, now);
        }
        char p[128];
        for (int k = 0; k < core::kRows; ++k) {
            const float a = core::RowAlpha(elapsed, k);
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUa%d", k);
            Alpha(p, a);
            std::snprintf(p, sizeof p, "anim-group.unlocks.pmUc%d", k);
            Alpha(p, a);
        }
    }

    uint64_t AnimMs()
    {
        return core::AnimMs();
    }

    void Reset()
    {
        g_snap = core::Snapshot{};
        g_lists = Lists{};
        g_wasLive = false;
        g_disabled = boot::ExtLoaded();
    }

} // namespace net::ui::pmunlock
