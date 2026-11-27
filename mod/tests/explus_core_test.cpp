// Standalone test for the pure execution-foley-plus rules; no engine / game
// headers. A synthetic DataView stands in for the guarded private tables.
//   g++ -std=c++17 -Wall -Wextra -Werror -pedantic -I mod/src -I mod/src/net/audio
//       mod/tests/explus_core_test.cpp -o /tmp/opencode/explus_check
//   /tmp/opencode/explus_check

#include "explus-core.hpp"

#include <cassert>
#include <cstdio>

namespace {

    using namespace net::audio::explus::core;

    struct Fixture {
        LayerSizing sizing[kLayerCount] = {};
        WaveDesc    waves[51] = {};
        WaveDesc    footWaves[3] = {};
        Row         rows[5] = {};
        const char* gun[1] = { "pistol-execute-left" };
        const char* melee[1] = { "fist-execute-enter-front" };
        FootTier    tiers[3] = {};
        DataView    d;

        Fixture()
        {
            for (int l = 0; l < kLayerCount; ++l) {
                sizing[l].nv = 1;
                sizing[l].mats = 1;
                sizing[l].first = static_cast<uint16_t>(l);
            }
            for (int w = 0; w < 51; ++w) {
                waves[w].off = static_cast<uint32_t>(w * 10000);
                waves[w].n = 10000;
                waves[w].rate = 44100;
            }
            for (int t = 0; t < 3; ++t) {
                tiers[t].first = static_cast<uint16_t>(t);
                tiers[t].count = 1;
                footWaves[t].off = static_cast<uint32_t>(t * 10000);
                footWaves[t].n = 10000;
                footWaves[t].rate = 44100;
            }
            rows[0] = { 0, 0, 13, 0, 2, 100, 100 };    // gun CL_M
            rows[1] = { 1, 0, 3, 0, 6, 200, 100 };     // melee STEP_SWEET
            rows[2] = { 1, 0, 40, 0, 0, 255, 200 };    // melee attacker breath
            rows[3] = { 1, 0, 38, 1, 2, 100, 300 };    // melee victim BLOOD_SPLAT (prio 2)
            rows[4] = { 1, 0, 0, 0, 6, 255, 400 };     // melee STEP_L (fs tier)

            d.sizing = sizing;
            d.layerCount = kLayerCount;
            d.waves = waves;
            d.waveCount = 51;
            d.footWaves = footWaves;
            d.rows = rows;
            d.rowCount = 5;
            d.gunNames = gun;
            d.gunCount = 1;
            d.meleeNames = melee;
            d.meleeCount = 1;
            d.footTiers = tiers;
            d.footTierCount = 3;
            d.footWaveCount = 3;
            d.footRate = 44100;
            d.pcmSamples = 510000;
            d.footPcmSamples = 30000;
        }
    };

    void TestTables()
    {
        Fixture f;
        assert(TablesOk(f.d));
        f.waves[0].rate = 7999;
        assert(!TablesOk(f.d));
        f.waves[0].rate = 44100;
        f.sizing[13].nv = 0;
        assert(!TablesOk(f.d));
        f.sizing[13].nv = 1;
        f.rows[2].ms = 50;                 // out of order within (1,0)
        assert(!TablesOk(f.d));
    }

    void TestTimeline()
    {
        Fixture f;
        State s;
        Reset(s);
        assert(!Begin(s, f.d, 1, 0, "pipe-execute", 0.0, 0, 1, false, false));
        assert(!Begin(s, f.d, 1, 0, "fist-execute-enter-front", 0.0, 0, 1, true, false));
        assert(!Begin(s, f.d, 1, 0, "fist-execute-enter-front", 0.0, 0, 1, false, true));
        assert(Begin(s, f.d, 1, 0, "fist-execute-enter-front", 0.0, 1, 0, false, false));
        assert(s.on && s.kind == 1 && s.pair == 0 && s.hi - s.lo == 4);
        assert(s.mat[0] == 1 && s.mat[1] == 0);
        assert(Matches(s, 1, 0) && !Matches(s, 0, 0));

        TickInput in;
        in.attEnt = 0xAA;
        in.vicEnt = 0xBB;
        in.haveAttPos = true;
        in.attPos[0] = 1.0f;
        in.ems = 150.0;
        Result r;
        Tick(s, f.d, in, r);
        assert(r.count == 1 && r.cues[0].kind == Cue::Kind::kPcm);
        assert(r.cues[0].layer == 3 && r.cues[0].who == 0 && r.cues[0].wave == 3);
        assert(r.cues[0].ent == 0xAA && r.cues[0].havePos && r.cues[0].pos[0] == 1.0f);
        assert(r.cues[0].gain > 0.0f && r.cues[0].gain < 0.2f);

        in.ems = 260.0;
        r = Result{};
        Tick(s, f.d, in, r);
        assert(r.count == 1 && r.cues[0].kind == Cue::Kind::kVox);
        assert(r.cues[0].who == 0 && r.cues[0].eventIndex == 0 && r.cues[0].vxe == 0);
        ReportVoice(s, 40, 0, 0, true, 260);
        assert(s.attVoxMs == 260);

        // The 600 ms speaker gate and the bad-take mark are the ext's voice rules.
        FireInput vox;
        vox.layer = 40;
        vox.who = 0;
        vox.ent = 0xAA;
        vox.nowMs = 700;
        Result vres;
        assert(Fire(s, f.d, vox, vres) == ResultCode::kVox);
        assert(!BadVoice(s, 40, 0));
        vox.nowMs = 900;
        ReportVoice(s, 40, 0, 0, false, 900);
        assert(BadVoice(s, 40, 0) && !BadVoice(s, 40, 1));
        vres = Result{};
        assert(Fire(s, f.d, vox, vres) == ResultCode::kPlayed
            && vres.cues[0].eventIndex == 1);

        in.ems = 420.0;
        in.activeVoices = 20;     // CL_M (prio 2) drops at 16; STEP_L (prio 1) still plays
        r = Result{};
        Tick(s, f.d, in, r);
        assert(r.count == 1 && r.cues[0].layer == 0 && s.skip[1] == 1);

        Cut(s);
        assert(!s.on);
    }

    void TestBudgetsAndLate()
    {
        Fixture f;
        State s;
        Reset(s);
        FireInput in;
        in.layer = 13;
        in.ent = 0xAA;
        in.nowMs = 100;
        Result r;
        in.activeVoices = 30;
        assert(Fire(s, f.d, in, r) == ResultCode::kVoice);
        in.activeVoices = 20;
        assert(Fire(s, f.d, in, r) == ResultCode::kPriority);
        in.activeVoices = 0;
        in.late = 251.0;
        assert(Fire(s, f.d, in, r) == ResultCode::kLate);
        in.late = 0.0;
        in.ent = 0;
        assert(Fire(s, f.d, in, r) == ResultCode::kNoSource);
        in.havePos = true;
        r = Result{};
        assert(Fire(s, f.d, in, r) == ResultCode::kPlayed);
        assert(!r.cues[0].ent && r.cues[0].havePos);
    }

    void TestRecipes()
    {
        Fixture f;
        State s;
        Reset(s);
        RecipeInput in;
        in.nowMs = 1000.0;
        Result out;
        Recipe rec = ContextRecipe(0, -1);             // grab: 2 immediate, 2 delayed
        assert(rec.count == 4);
        assert(QueueRecipe(s, f.d, rec, 0xAA, 0xBB, 0, 1, in, out) == 2);
        assert(out.count == 2 && s.q[0].due == 1020.0 && s.q[1].due == 1060.0);
        out = Result{};
        assert(TickQueue(s, f.d, 1010.0, in, out) == 0);
        in.nowMs = 1070.0;
        out = Result{};
        assert(TickQueue(s, f.d, 1070.0, in, out) == 2);
        assert(s.q[0].due == 0.0 && s.q[1].due == 0.0);

        assert(HeavyContextRecipe(-1).count == 3);
        assert(FinisherRecipe(5).count == 4 && FinisherRecipe(11).count == 0);
        assert(DownedRecipe(2, 1).count == 5 && DownedRecipe(2, 0).count == 4);
        Recipe a = IntFoleyRecipe(1, true);
        Recipe b = IntFoleyRecipe(4, true);
        assert(a.count == 3 && a.rows[0].layer == static_cast<uint8_t>(Layer::kStrugHead));
        assert(a.rows[0].layer == b.rows[0].layer);
        assert(IntFoleyRecipe(2, false).rows[0].layer == static_cast<uint8_t>(Layer::kClL));
    }

} // namespace

int main()
{
    TestTables();
    TestTimeline();
    TestBudgetsAndLate();
    TestRecipes();
    std::puts("EXPPLUS_CORE_TEST_OK");
    return 0;
}
