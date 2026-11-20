// ported from tools/ext/bloodbuild.c (a purely local, cosmetic feature; no network message)
//
// The ext C is the behavioural reference. Everything here is per-client: a per-body `blood` /
// `dirt` level driven by events every screen already sees identically (synced health, the
// paired-move surface, down / death, the downed crawl, the life counter), escalating into the
// engine's own `part-effect` blood-mask events (engine::clipfx) and particle group spawns
// (engine::vfx). The three private generated tables the ext reads (CTX_FX / KNIFE_HIT_FX /
// FIN_ANIMS) are absent locally, so the port keeps the ext's faithful early-out behind
// T2R_DATA_CTX_FX (see bloodbuild.hpp).

#include "bloodbuild.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)

#include "engine/backpack-prop.hpp"
#include "engine/clipfx.hpp"
#include "engine/exe_side.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/object.hpp"
#include "engine/ptr.hpp"
#include "engine/script.hpp"
#include "engine/vfx.hpp"

#include "gameplay/ctxdown.hpp"
#include "gameplay/melee2013.hpp"

#include "match.hpp"
#include "net-common.hpp"
#include "net-player.hpp"
#include "paired.hpp"
#include "player-status.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::match::bloodbuild {

    namespace {

        // ---- constants (ext bloodbuild.c) ------------------------------------------------
        constexpr int kUs       = kMaxNetPlayers;         // ext T2R_US = T2R_MAX_PEERS
        constexpr int kBodies   = kMaxNetPlayers + 1;     // ext BB_N: slots 0..7 puppets, kUs = us
        constexpr int kMaxWounds = 6;                     // ext BB_WOUNDS: bm-knife-wound-cloth joints
        constexpr int kWoundMax  = 6;                     // ext BB_WOUND_MAX: wounds per life
        constexpr int kMaxSpurt  = 24;                    // ext BB_SPN
        constexpr int kMaxShivFx = 12;                    // ext BB_SHIVN
        constexpr int kQueue     = 24;                    // ext g_bbQ size

        // ext BB_WJ / BB_WW: the bm-knife-wound-cloth joints, zone order + zone weights
        // (the head is rarer than the torso).
        const char* const kZoneJoint[kMaxWounds] = {
            "headb", "spineb", "spinec", "spined", "l_clavicle", "l_deltoid_helper"
        };
        const uint8_t kZoneWeight[kMaxWounds] = { 2, 3, 3, 3, 2, 2 };

        // ext BB_MODS: the modules held while in a match (the mud interaction groups, the wipe).
        const char* const kBbMods[2] = {
            "part-module/part-interaction-mud-character",
            "part-module/part-sle-blood-clear-igc"
        };

        // ext BbThroatMove MX_T / FIN_T: the 2013 / finisher moves whose victim clip cuts the neck.
        const char* const kMxThroat[] = {
            "machete-decapitate", "machete-execute-right", "shiv-stealth-back",
            "fist-insta-shiv-stand-back", "shiv-ground-execution-right"
        };
        const char* const kFinThroat[] = {
            "neck-slice", "neck-chop", "neck-tear", "decap", "throat-slit"
        };

        // ---- the private generated tables (absent unless T2R_DATA_CTX_FX) -----------------
        // Row layouts copied from ctxdown.c's CTX_FX / KNIFE_HIT_FX; the generated headers
        // define CTX_FX_DATA / KNIFE_HIT_FX_DATA (rows without the sentinel) and CTX_FX_N /
        // KNIFE_HIT_FX_N. finisher_anims.h defines the FIN_ANIMS array directly (the ext
        // includes it and reads FIN_ANIMS[i].{move,att,vic}).
        struct CtxFxRow {
            const char*    pair;
            int            seg, role, ms;
            float          frame;
            uint64_t       name;
            const char*    pn;
            const char*    jn;
            int            n;
            const uint64_t (*pp)[2];
        };
        struct KnifeHitFxRow {
            uint64_t       move;
            const char*    mn;
            const char*    clip;
            int            ms;
            float          frame;
            uint64_t       name;
            const char*    pn;
            const char*    jn;
            int            n;
            const uint64_t (*pp)[2];
        };

#ifdef T2R_DATA_CTX_FX
#include "ctx_fx_events.h"
#include "finisher_anims.h"
#define BB_CTX_FX_ROWS   CTX_FX_DATA
#define BB_KNIFE_FX_ROWS KNIFE_HIT_FX_DATA
#else
#define BB_CTX_FX_ROWS
#define BB_KNIFE_FX_ROWS
#endif

        const CtxFxRow kCtxFx[] = {
            BB_CTX_FX_ROWS
            { nullptr, 0, 0, 0, 0.0f, 0, nullptr, nullptr, 0, nullptr }
        };
        const KnifeHitFxRow kKnifeHitFx[] = {
            BB_KNIFE_FX_ROWS
            { 0, nullptr, nullptr, 0, 0.0f, 0, nullptr, nullptr, 0, nullptr }
        };
        constexpr int kCtxFxCount = static_cast<int>(sizeof kCtxFx / sizeof kCtxFx[0]) - 1;
        constexpr int kKnifeHitFxCount = static_cast<int>(sizeof kKnifeHitFx / sizeof kKnifeHitFx[0]) - 1;

#undef BB_CTX_FX_ROWS
#undef BB_KNIFE_FX_ROWS

        // ---- structs (ext) ---------------------------------------------------------------
        struct BbFx {
            engine::clipfx::Entry e;
            uint64_t              clip;
            const char*           nm;
            const char*           jn;
            int                   ok;
        };

        struct BbBody {
            uint64_t h;             // puppet handle (a respawn is a new character) / SID("player")
            uint32_t life;          // ours: the life token
            int      seen;          // state initialised
            float    hp;
            int      hpOk;
            float    blood, dirt;   // 0..100
            int      wounds, body, mouth, hand, mud, bysplat;
            int      downed, dead;  // last seen
            float    crawlM, px, pz;
            int      posOk;
            float    speed;
            uint64_t born;          // frame the life was first seen
        };

        struct BbPend {
            double      due;
            int         who, kind, zone, tries;
            const char* tag;
            double      at;
        };

        enum {
            kBkWound = 1, kBkBody, kBkMouth, kBkHand, kBkMud, kBkClear, kBkThroat, kBkSpurt
        };

        // ---- state (ext) -----------------------------------------------------------------
        BbFx g_bbW[kMaxWounds]; // wound masks, zone order of kZoneJoint
        BbFx g_bbBody, g_bbMouth, g_bbHand, g_bbThroat, g_bbFace, g_bbNose;
        BbFx g_bbSp[kMaxSpurt];
        int  g_bbSpRel[kMaxSpurt], g_bbNsp;
        int  g_bbIg0, g_bbNig;
        uint8_t g_bbIg2[kMaxSpurt];

        int g_bbInit, g_bbNfx;

        BbBody g_bb[kBodies];
        BbPend g_bbQ[kQueue];
        int      g_bbUidWait;
        uint32_t g_bbUidSeen;
        uint64_t g_bbFired, g_bbFailed;

        int      g_bbPin;
        uint8_t* g_bbPinPool;
        bool     g_bbPinBad;

        struct { uint64_t ent; int who; double due; } g_gxbPool[4];

        // ext g_bbMeleeAt / g_bbMeleeBlade (t2r-ext.c): a melee blow note, so a wound within
        // 1.2 s is stamped where SP's blunt / fist clips stamp their victim (the head).
        uint64_t g_bbMeleeAt[kBodies];
        uint8_t  g_bbMeleeBlade[kBodies];

        // ext MeleeBladeKills: defined near the end; used by BbPairTick above it.
        bool MeleeBladeKills(uint64_t weapon);

        // ---- small helpers ---------------------------------------------------------------
        double PsyncMs() { return ::net::anim::paired::Ms(); }

        float FrameMsC()
        {
            const float ms = engine::frame::DeltaMs();
            return ms < 5.0f ? 5.0f : ms > 50.0f ? 50.0f : ms;   // ext FrameMsC
        }

        uint8_t* BbMe() { return engine::backpack_prop::Me(); }

        bool LogCap(int& c, int n) { return c++ < n; }

        // ext: MatchSubstate() == 6. The mod maps "in game" to the level phases.
        bool InLevel()
        {
            const ::net::match::Phase p = ::net::match::Get().GetPhase();
            return p >= ::net::match::Phase::LoadingPlaceSpawn && p <= ::net::match::Phase::Live;
        }

        const char* BbName(int who)
        {
            static char b[4][24];
            static unsigned i;
            char* o = b[i++ & 3];
            if (who == kUs)
                return "ourselves";
            std::snprintf(o, 24, "slot %d", who);
            return o;
        }

        uint32_t BbHash(uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7feb352dU;
            x ^= x >> 15;
            x *= 0x846ca68bU;
            x ^= x >> 16;
            return x;
        }

        int BbZone(uint32_t seed)
        {
            int tot = 0;
            for (int k = 0; k < kMaxWounds; k++)
                tot += kZoneWeight[k];
            int r = static_cast<int>(BbHash(seed) % static_cast<uint32_t>(tot));
            for (int k = 0; k < kMaxWounds; k++) {
                if (r < kZoneWeight[k])
                    return k;
                r -= kZoneWeight[k];
            }
            return 1;
        }

        uint64_t BbEnt(int who)
        {
            if (who == kUs)
                return SID("player");
            return who >= 0 && who < kMaxNetPlayers ? g_bb[who].h : 0;
        }

        int BbBodyOfEnt(uint64_t ent)
        {
            if (ent == SID("player"))
                return kUs;
            if (!ent)
                return -1;
            for (int s = 0; s < kMaxNetPlayers; s++)
                if (g_bb[s].h == ent)
                    return s;
            return -1;
        }

        // ext BbOff / BbNoDirt / BbNoShiv (CfxeFile: a 2 s cached sidecar test).
        bool BbOff()
        {
            static int c = -1;
            static uint64_t k;
            return engine::clipfx::File("t2r-no-blood-buildup.txt", &c, &k);
        }
        bool BbNoDirt()
        {
            static int c = -1;
            static uint64_t k;
            return engine::clipfx::File("t2r-no-blood-dirt.txt", &c, &k);
        }
        bool BbNoShiv()
        {
            static int c = -1;
            static uint64_t k;
            return engine::clipfx::File("t2r-no-shiv-blood.txt", &c, &k);
        }

        void BbSet(BbFx* f, uint64_t clip, uint64_t nm_sid, const char* nm,
                   const char* jn, int n, const uint64_t (*pp)[2], float frame)
        {
            (void)nm_sid;
            f->e.type = engine::clipfx::kType;
            f->e.frame = frame;
            f->e.n = static_cast<uint32_t>(n);
            f->e.pairs = pp;
            f->clip = clip;
            f->nm = nm;
            f->jn = jn;
            f->ok = (pp && n > 0) ? 1 : 0;
            if (f->ok)
                g_bbNfx++;
        }

        void BbQueue(int who, int kind, int zone, double delayMs)
        {
            for (int i = 0; i < kQueue; i++)
                if (!g_bbQ[i].kind) {
                    g_bbQ[i].due = PsyncMs() + delayMs;
                    g_bbQ[i].who = who;
                    g_bbQ[i].kind = kind;
                    g_bbQ[i].zone = zone;
                    g_bbQ[i].tries = 0;
                    g_bbQ[i].tag = nullptr;
                    g_bbQ[i].at = 0;
                    return;
                }
        }

        void BbInit()
        {
            if (g_bbInit)
                return;
            g_bbInit = 1;
            const uint64_t wound = SID("bm-knife-wound-cloth");
            const uint64_t body = SID("bm-blood-splatter-body");
            const uint64_t mouth = SID("bm-ellie-melee-mouth");
            const uint64_t hand = SID("bm-bloody-hand");
            for (int k = 0; k < kMaxWounds; k++)
                for (int i = 0; i < kKnifeHitFxCount; i++)
                    if (!g_bbW[k].ok && kKnifeHitFx[i].name == wound
                        && kKnifeHitFx[i].jn
                        && !std::strcmp(kKnifeHitFx[i].jn, kZoneJoint[k]))
                        BbSet(&g_bbW[k], SID(kKnifeHitFx[i].clip), wound,
                              kKnifeHitFx[i].pn, kKnifeHitFx[i].jn,
                              kKnifeHitFx[i].n, kKnifeHitFx[i].pp,
                              kKnifeHitFx[i].frame);
            const uint64_t clip0 = SID("ellie-knife-combo-slash-1-hit--npc-m");
            for (int i = 0; i < kCtxFxCount; i++) {   // victim half rows (role 1: NPC body UVs)
                if (kCtxFx[i].role != 1)
                    continue;
                if (!g_bbBody.ok && kCtxFx[i].name == body
                    && kCtxFx[i].jn && !kCtxFx[i].jn[0])
                    BbSet(&g_bbBody, clip0, body, kCtxFx[i].pn, kCtxFx[i].jn,
                          kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
                if (!g_bbMouth.ok && kCtxFx[i].name == mouth
                    && kCtxFx[i].jn && !std::strcmp(kCtxFx[i].jn, "headb"))
                    BbSet(&g_bbMouth, clip0, mouth, kCtxFx[i].pn, kCtxFx[i].jn,
                          kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
                if (!g_bbFace.ok && kCtxFx[i].name == SID("bm-face")
                    && kCtxFx[i].jn && !std::strcmp(kCtxFx[i].jn, "headb"))
                    BbSet(&g_bbFace, clip0, kCtxFx[i].name, kCtxFx[i].pn,
                          kCtxFx[i].jn, kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
                if (!g_bbNose.ok && kCtxFx[i].name == SID("bm-ellie-melee-nose")
                    && kCtxFx[i].jn && !std::strcmp(kCtxFx[i].jn, "headb"))
                    BbSet(&g_bbNose, clip0, kCtxFx[i].name, kCtxFx[i].pn,
                          kCtxFx[i].jn, kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
                if (!g_bbHand.ok && kCtxFx[i].name == hand
                    && kCtxFx[i].jn && !kCtxFx[i].jn[0])
                    BbSet(&g_bbHand, clip0, hand, kCtxFx[i].pn, kCtxFx[i].jn,
                          kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
            }
            // the THROAT SLASH mask: SP's own bm-knife-wound-cloth at the "neck" joint.
            for (int i = 0; i < kCtxFxCount && !g_bbThroat.ok; i++)
                if (kCtxFx[i].role == 1 && kCtxFx[i].name == wound
                    && kCtxFx[i].jn && !std::strcmp(kCtxFx[i].jn, "neck"))
                    BbSet(&g_bbThroat, clip0, wound, kCtxFx[i].pn, kCtxFx[i].jn,
                          kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
            // the SHIV neck-slash blood: SP's own victim rows of its neck kills, with the
            // relative time of each row from the cut.
            for (int i = 0; i < kCtxFxCount && g_bbNsp < kMaxShivFx; i++) {
                if (kCtxFx[i].role != 1 || !kCtxFx[i].pair || !kCtxFx[i].pn
                    || !kCtxFx[i].jn || !std::strncmp(kCtxFx[i].pn, "bm-", 3))
                    continue;
                int rel = -1;
                if (!std::strcmp(kCtxFx[i].pair, "wpn-machete-wall-neck-slice")
                    && kCtxFx[i].ms == 1667 && !std::strcmp(kCtxFx[i].jn, "neck"))
                    rel = 0;
                else if (!std::strcmp(kCtxFx[i].pair, "wpn-blade-wall-neck-chop")
                         && kCtxFx[i].ms == 1433
                         && !std::strcmp(kCtxFx[i].pn, "melee-blood-gore-minimal-eff"))
                    rel = 100;
                else if (!std::strcmp(kCtxFx[i].pair, "wpn-blade-wall-neck-chop")
                         && kCtxFx[i].ms >= 2633 && kCtxFx[i].ms <= 3167)
                    rel = kCtxFx[i].ms - 1333;
                if (rel < 0)
                    continue;
                BbSet(&g_bbSp[g_bbNsp], clip0, kCtxFx[i].name, kCtxFx[i].pn,
                      kCtxFx[i].jn, kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
                g_bbSpRel[g_bbNsp++] = rel;
            }
            // the Interrogation: the wall-combo-head-smash victim half + the floor pool.
            g_bbIg0 = g_bbNsp;
            for (int i = 0; i < kCtxFxCount && g_bbIg0 + g_bbNig < kMaxSpurt; i++) {
                if (kCtxFx[i].role != 1 || !kCtxFx[i].pair || !kCtxFx[i].pn
                    || !kCtxFx[i].jn)
                    continue;
                const int pool = !std::strcmp(kCtxFx[i].pn, "blood-splatter-pool-eff")
                    && !std::strcmp(kCtxFx[i].jn, "root");
                const int smash = !std::strcmp(kCtxFx[i].pair, "wall-combo-head-smash")
                    && kCtxFx[i].seg == 2 && kCtxFx[i].ms == 1067
                    && !std::strcmp(kCtxFx[i].jn, "headb")
                    && std::strncmp(kCtxFx[i].pn, "bm-", 3);
                if (!pool && !smash)
                    continue;
                int dup = 0;
                for (int k = 0; k < g_bbNig; k++)
                    if (!std::strcmp(g_bbSp[g_bbIg0 + k].nm, kCtxFx[i].pn))
                        dup = 1;
                if (dup)
                    continue;
                const int k = g_bbNig++;
                BbSet(&g_bbSp[g_bbIg0 + k], clip0, kCtxFx[i].name, kCtxFx[i].pn,
                      kCtxFx[i].jn, kCtxFx[i].n, kCtxFx[i].pp, kCtxFx[i].frame);
                g_bbIg2[k] = static_cast<uint8_t>(pool
                    || !std::strcmp(kCtxFx[i].pn, "melee-blood-gore-minimal-eff")
                    || !std::strcmp(kCtxFx[i].pn, "blood-drops-burst-eff"));
            }
            TO1_LOG("[BLOOD] interrogation blood: %d SP head-smash rows loaded "
                "(t2r-no-int-blood.txt = off)\n", g_bbNig);
            int nw = 0;
            for (int k = 0; k < kMaxWounds; k++)
                nw += g_bbW[k].ok;
            TO1_LOG("[BLOOD] shiv blood: %d SP neck-kill part-effect rows loaded "
                "(t2r-no-shiv-blood.txt = off)\n", g_bbNsp);
            TO1_LOG("[BLOOD] build-up tables: %d wound zones, body splatter %d, mouth %d, "
                "hand %d, melee face %d nose %d\n",
                nw, g_bbBody.ok, g_bbMouth.ok, g_bbHand.ok, g_bbFace.ok, g_bbNose.ok);
        }

        void BbEscalate(int who)
        {
            BbBody* b = &g_bb[who];
            if (b->blood > 100.0f)
                b->blood = 100.0f;
            if (b->blood >= 45.0f && !b->body) {
                b->body = 1;
                BbQueue(who, kBkBody, 0, 120.0);
            }
            if (b->blood >= 75.0f && !b->mouth) {
                b->mouth = 1;
                BbQueue(who, kBkMouth, 0, 300.0);
            }
            if (b->blood >= 75.0f && !b->hand) {
                b->hand = 1;
                BbQueue(who, kBkHand, 0, 500.0);
            }
        }

        // a hit taken: `drop` health lost, `hpAfter` = the health after it (same on every screen)
        void BbHit(int who, float drop, float hpAfter)
        {
            BbBody* b = &g_bb[who];
            b->blood += drop * 0.5f;
            static int nl;
            const int n = drop >= 35.0f ? 2 : drop >= 6.0f ? 1 : 0;
            for (int i = 0; i < n && b->wounds < kWoundMax; i++) {
                const int z = BbZone(static_cast<uint32_t>(hpAfter * 16.0f) * 2654435761u
                    + static_cast<uint32_t>(i) * 40503u);
                if (g_bbW[z].ok) {
                    b->wounds++;
                    BbQueue(who, kBkWound, z, 250.0 + 90.0 * i);
                }                   // 250 ms: the melee blow note lands first
            }
            if (LogCap(nl, 60))
                TO1_LOG("[BLOOD] hit on %s: -%.1f hp (now %.1f) -> blood %.0f, wounds %d\n",
                    BbName(who), static_cast<double>(drop), static_cast<double>(hpAfter),
                    static_cast<double>(b->blood), b->wounds);
            BbEscalate(who);
        }

        const float* BbPos(int who, float* buf)
        {
            if (who == kUs)
                return ::engine::object::LocalPlayerWorld(buf) ? buf : nullptr;
            return g_bb[who].h && ::engine::object::EntityWorld(g_bb[who].h, buf) ? buf : nullptr;
        }

        // a body died / was executed: bystanders within 5 m get a light splatter.
        void BbNearby(int src, const char* why)
        {
            float sb[8], ob[8];
            const float* sp = BbPos(src, sb);
            if (!sp)
                return;
            for (int w = 0; w < kBodies; w++) {
                if (w == src || !g_bb[w].seen || g_bb[w].dead
                    || (w < kMaxNetPlayers && !g_bb[w].h))
                    continue;
                const float* op = BbPos(w, ob);
                if (!op)
                    continue;
                const float dx = op[0] - sp[0], dz = op[2] - sp[2];
                if (dx * dx + dz * dz > 5.0f * 5.0f)
                    continue;
                g_bb[w].blood += 8.0f;
                if (!g_bb[w].bysplat) {
                    g_bb[w].bysplat = 1;
                    BbQueue(w, kBkBody, 0, 200.0);
                    g_bb[w].body = 1;
                }
                BbEscalate(w);
                static int nl;
                if (LogCap(nl, 30))
                    TO1_LOG("[BLOOD] %s near %s (%s): blood %.0f\n", BbName(w),
                        BbName(src), why, static_cast<double>(g_bb[w].blood));
            }
        }

        // Throat-slash moves: the 2013 pairs, the finishers whose victim clip cuts the neck,
        // and the contextual takedowns with a neck slice (the clip's gush row = the cut).
        // *at = ms from the move's start.
        int BbThroatMove(double* at, double dflt)
        {
            *at = dflt;
            const ::net::anim::paired::State& ps = ::net::anim::paired::Get();
            const uint64_t mv = ps.move;
            if (ps.kind == ::net::anim::paired::kCtxdown) {
                const int np = ::gameplay::ctxdown::Count();
                for (int pr = 0; pr < np; pr++) {
                    const char* nm = ::gameplay::ctxdown::At(pr).name;
                    if (SID(nm) != mv)
                        continue;
                    if (!std::strstr(nm, "neck-slice") && !std::strstr(nm, "neck-chop"))
                        return 0;
                    const int ns = ::gameplay::ctxdown::At(pr).nseg;
                    for (int k = 0; k < kCtxFxCount; k++) {   // the gush row = the moment of the cut
                        if (!kCtxFx[k].pair || std::strcmp(kCtxFx[k].pair, nm)
                            || kCtxFx[k].role != 1 || !kCtxFx[k].jn
                            || std::strcmp(kCtxFx[k].jn, "neck") || !kCtxFx[k].pn
                            || !std::strstr(kCtxFx[k].pn, "ribbon-pulse")
                            || kCtxFx[k].seg >= ns)
                            continue;
                        double st = 0;
                        for (int i = 0; i < kCtxFx[k].seg; i++)
                            st += ::gameplay::ctxdown::SegLen(pr, i) * 1000.0;
                        *at = st + kCtxFx[k].ms;
                        return 1;
                    }
                    return 1;
                }
                return 0;
            }
            const int m = ::gameplay::melee2013::ByMove(mv);
            if (m >= 0) {
                for (const char* t : kMxThroat)
                    if (!std::strcmp(::gameplay::melee2013::At(m).name, t))
                        return 1;
                return 0;
            }
#ifdef T2R_DATA_CTX_FX
            for (size_t i = 0; i < sizeof FIN_ANIMS / sizeof FIN_ANIMS[0]; i++)
                if (FIN_ANIMS[i].move == mv) {
                    for (const char* t : kFinThroat)
                        if (std::strstr(FIN_ANIMS[i].vic, t))
                            return 1;
                    return 0;
                }
#endif
            return 0;
        }

        // the shiv kills whose blade goes through the NECK, with the contact time (ms).
        int BbShivNeck(double* at)
        {
            static const struct { const char* n; double ms; } kT[] = {
                { "shiv-stealth-back", 330.0 },
                { "fist-insta-shiv-stand-back", 1100.0 },
                { "shiv-ground-execution-right", 2200.0 },
            };
            const ::net::anim::paired::State& ps = ::net::anim::paired::Get();
            if (ps.kind != ::net::anim::paired::kMeleeExec)
                return 0;
            const int m = ::gameplay::melee2013::ByMove(ps.move);
            if (m < 0)
                return 0;
            for (const auto& t : kT)
                if (!std::strcmp(::gameplay::melee2013::At(m).name, t.n)) {
                    *at = t.ms;
                    return 1;
                }
            return 0;
        }

        // one queued effect on a body (the engine event path for the masks, a group spawn for
        // the dirt / wipe).
        void BbDo(const BbPend* p)
        {
            static int nl;
            static uint64_t mudFail;
            const uint64_t ent = BbEnt(p->who);
            if (!ent)
                return;
            if (p->kind == kBkMud || p->kind == kBkClear) {
                const char* nm = p->kind == kBkMud ? "nav-prone-mud-mask" : "bm-blood-clear-eff";
                const uint64_t r = engine::vfx::ArmorFxSpawn(SID(nm), ent, "root");
                const int ok = (r && r != ~0ULL) ? 1 : 0;
                if (ok)
                    g_bbFired++;
                else
                    g_bbFailed++;
                if (nl < 60 || (!ok && mudFail++ < 4)) {
                    nl++;
                    TO1_LOG("[BLOOD] %s on %s -> %llx%s\n", nm, BbName(p->who),
                        static_cast<unsigned long long>(r), ok ? "" : " (not spawned)");
                }
                return;
            }
            if (p->kind == kBkSpurt) {
                const BbFx* s = &g_bbSp[p->zone];
                if (!s->ok || engine::clipfx::Off() || !engine::clipfx::Check())
                    return;
                uint8_t* so = engine::clipfx::ObjOf(ent);
                if (!so) {
                    TO1_LOG("[BLOOD] shiv blood: %s %s at %.0f ms on %s -> body not found\n",
                        p->tag, s->nm, p->at, BbName(p->who));
                    return;
                }
                if (engine::vfx::Stream(SID(s->nm))) {   // GPU-compute stream: needs a free entry
                    uint8_t* m = engine::vfx::Rtmgr();
                    if (m && engine::vfx::Used(m) >= engine::vfx::kComputeEntries
                        && p->tries < 8) {
                        if (p->tries == 0)
                            engine::vfx::MakeRoom(m);
                        for (int i = 0; i < kQueue; i++)
                            if (!g_bbQ[i].kind) {
                                g_bbQ[i] = *p;
                                g_bbQ[i].tries++;
                                g_bbQ[i].due = PsyncMs() + 20.0;
                                break;
                            }
                        return;
                    }
                }
                const uint64_t r = engine::clipfx::Fire(so, s->clip, &s->e, so == BbMe());
                if (r)
                    g_bbFired++;
                else
                    g_bbFailed++;
                TO1_LOG("[BLOOD] shiv blood: %s %s at %.0f ms on %s -> %s %llx\n", p->tag,
                    s->nm, p->at, BbName(p->who), r ? "engine event path" : "FAILED",
                    static_cast<unsigned long long>(r));
                return;
            }
            const BbFx* f =
                p->kind == kBkThroat ? &g_bbThroat :
                p->kind == kBkWound ? &g_bbW[p->zone] :
                p->kind == kBkBody ? &g_bbBody :
                p->kind == kBkMouth ? &g_bbMouth : &g_bbHand;
            // a wound from a blunt / fist MELEE blow (within 1.2 s) is stamped where SP's blunt
            // and fist clips stamp their victim - the head, never a torso knife cut.
            // t2r-blood-melee-cuts.txt = old.
            if (p->kind == kBkWound && p->who >= 0 && p->who < kBodies
                && g_bbMeleeAt[p->who]
                && GetTickCount64() - g_bbMeleeAt[p->who] < 1200
                && !g_bbMeleeBlade[p->who]
                && !engine::exe_side::Exists("t2r-blood-melee-cuts.txt")) {
                const int v = p->zone % 3;
                const BbFx* h = (v == 1 && g_bbFace.ok) ? &g_bbFace
                    : (v == 2 && g_bbNose.ok) ? &g_bbNose : &g_bbW[0];
                if (h->ok)
                    f = h;
            }
            if (!f->ok || engine::clipfx::Off() || !engine::clipfx::Check())
                return;
            uint8_t* obj = engine::clipfx::ObjOf(ent);
            if (!obj) {
                if (nl < 60) {
                    nl++;
                    TO1_LOG("[BLOOD] %s on %s: body not found\n", f->nm, BbName(p->who));
                }
                return;
            }
            const uint64_t r = engine::clipfx::Fire(obj, f->clip, &f->e, obj == BbMe());
            if (r)
                g_bbFired++;
            else
                g_bbFailed++;
            if (p->kind == kBkThroat)
                TO1_LOG("[BLOOD] throat slash mask %s at %s on %s -> engine event path %llx\n",
                    f->nm, f->jn, BbName(p->who), static_cast<unsigned long long>(r));
            else if (nl < 60) {
                nl++;
                TO1_LOG("[BLOOD] mask %s at %s on %s (blood %.0f dirt %.0f, wounds %d) -> "
                    "engine event path %llx\n", f->nm, f->jn[0] ? f->jn : "(body)",
                    BbName(p->who), static_cast<double>(g_bb[p->who].blood),
                    static_cast<double>(g_bb[p->who].dirt), g_bb[p->who].wounds,
                    static_cast<unsigned long long>(r));
            }
        }

        // the Interrogation's head slams: second 0 = the slam into the ground (a small hit),
        // 1 = the final smash (the whole head-smash effect set).
        int BbIgSlam(uint64_t ent, int second)
        {
            static int nl;
            static int offC = -1;
            static uint64_t offK;
            BbInit();
            if (!g_bbNig || !ent || ent == ~0ULL
                || engine::clipfx::File("t2r-no-int-blood.txt", &offC, &offK))
                return 0;
            const int who = BbBodyOfEnt(ent);
            int n = 0;
            if (who < 0) {
                if (LogCap(nl, 20))
                    TO1_LOG("[BLOOD] interrogation blood: slam %d - victim body not found\n",
                        second);
                return 0;
            }
            if (!second) {
                g_bb[who].blood += 3.0f;
                return 0;   // the blood belongs to the KILLING head smash, not the first slam
            }
            for (int k = 0; k < g_bbNig; k++) {
                BbPend p{};
                p.who = who;
                p.kind = kBkSpurt;
                p.zone = g_bbIg0 + k;
                p.tag = "interrogation final smash";
                p.at = 5000.0;
                BbDo(&p);
                n++;
            }
            g_bb[who].blood += 25.0f;
            return n;
        }

        // A finisher aborted before its kill frame left its queued masks to fire on a clean
        // dodge; cancel the two bodies' not-yet-due effects. t2r-bb-abort-keep.txt = old.
        void BbCancelPair(int a, int v)
        {
            if (engine::exe_side::Exists("t2r-bb-abort-keep.txt"))
                return;
            const double now = PsyncMs();
            int n = 0;
            for (int i = 0; i < kQueue; i++)
                if (g_bbQ[i].kind && g_bbQ[i].due > now + 60.0
                    && (g_bbQ[i].who == a || g_bbQ[i].who == v)) {
                    g_bbQ[i].kind = 0;
                    n++;
                }
            if (n)
                TO1_LOG("[BLOOD] the move was aborted before its kill frame - %d queued blood "
                    "effect(s) on slots %d / %d cancelled\n", n, a, v);
        }

        // paired moves = executions / takedowns on every screen (the same move on all of them).
        void BbPairTick()
        {
            const ::net::anim::paired::State& ps = ::net::anim::paired::Get();
            if (!ps.role || ps.kind == ::net::anim::paired::kRevive
                || ps.kind == ::net::anim::paired::kTackle) {
                g_bbUidWait = 0;
                return;
            }
            if (ps.uid == g_bbUidSeen)
                return;
            const int a = BbBodyOfEnt(ps.entAtt), v = BbBodyOfEnt(ps.entVic);
            if ((a < 0 || v < 0) && ++g_bbUidWait < 30)
                return;             // the halves are still being set up
            g_bbUidSeen = ps.uid;
            g_bbUidWait = 0;
            int lethal = (ps.flags & ::net::anim::paired::kFLethal) != 0;
            {
                // a blade finisher (ctxdown one-shot) kills its victim at the kill frame now:
                // lethal blood masks, as on the victim's screen. TODO: MeleeBladeKills (the
                // melee_hitfx weapon-class rule + IntLive()) is not ported -> always false.
                if (!lethal && ps.kind == ::net::anim::paired::kCtxdown && ps.weapon
                    && ps.weapon != ~0ULL && MeleeBladeKills(ps.weapon)
                    && !engine::exe_side::Exists("t2r-melee-down-revivable.txt"))
                    lethal = 1;
            }
            const double at = ps.killS > 0.2f
                ? (ps.killS > 3.5f ? 3500.0 : static_cast<double>(ps.killS) * 1000.0)
                : 1200.0;
            static int nl;
            if (LogCap(nl, 40))
                TO1_LOG("[BLOOD] execution kind %d %s: attacker %s victim %s, masks at +%.0f ms\n",
                    ps.kind, lethal ? "lethal" : "non-lethal",
                    a >= 0 ? BbName(a) : "?", v >= 0 ? BbName(v) : "?", at);
            double tat;
            double sat = 0;
            if (v >= 0 && g_bbNsp && !BbNoShiv() && BbShivNeck(&sat)) {
                const int m = ::gameplay::melee2013::ByMove(ps.move);
                for (int k = 0; k < g_bbNsp; k++) {
                    BbQueue(v, kBkSpurt, k, sat + g_bbSpRel[k]);
                    for (int i = 0; i < kQueue; i++)
                        if (g_bbQ[i].kind == kBkSpurt && g_bbQ[i].zone == k
                            && g_bbQ[i].who == v && !g_bbQ[i].tag) {
                            g_bbQ[i].tag = m >= 0 ? ::gameplay::melee2013::At(m).name : "?";
                            g_bbQ[i].at = sat + g_bbSpRel[k];
                            break;
                        }
                }
                TO1_LOG("[BLOOD] shiv blood: %s neck slash on %s: %d SP part-effects queued "
                    "from +%.0f ms\n", m >= 0 ? ::gameplay::melee2013::At(m).name : "?",
                    BbName(v), g_bbNsp, sat);
            }
            if (v >= 0 && g_bbThroat.ok && BbThroatMove(&tat, at)) {
                if (sat > 0)
                    tat = sat;
                BbQueue(v, kBkThroat, 0, tat);
                g_bb[v].blood += 12.0f;
                TO1_LOG("[BLOOD] throat slash on %s (move %016llx), mask at +%.0f ms\n",
                    BbName(v), static_cast<unsigned long long>(ps.move), tat);
            }
            if (v >= 0) {
                BbBody* b = &g_bb[v];
                b->blood += lethal ? 40.0f : 14.0f;
                if (b->wounds < kWoundMax) {
                    const int z = BbZone(ps.uid * 2654435761u + 7u);
                    if (g_bbW[z].ok) {
                        b->wounds++;
                        BbQueue(v, kBkWound, z, at);
                    }
                }
                if (lethal) {
                    if (!b->body) {
                        b->body = 1;
                        BbQueue(v, kBkBody, 0, at + 80.0);
                    }
                    if (!b->mouth) {
                        b->mouth = 1;
                        BbQueue(v, kBkMouth, 0, at + 160.0);
                    }
                }
                BbEscalate(v);
            }
            if (a >= 0 && lethal) {
                BbBody* b = &g_bb[a];
                b->blood += 22.0f;
                if (!b->body) {
                    b->body = 1;
                    BbQueue(a, kBkBody, 0, at + 100.0);
                }                   // the executioner is splattered
                if (!b->hand) {
                    b->hand = 1;
                    BbQueue(a, kBkHand, 0, at + 200.0);
                }                   // bloody hands
                BbEscalate(a);
            }
            if (lethal && v >= 0)
                BbNearby(v, "execution");
        }

        // extra modules held while in a match (the mud interaction groups, the blood wipe):
        // the same module-reference API feel.c's blood_pin uses.
        void BbPin(int want)
        {
            if (want == g_bbPin || g_bbPinBad)
                return;
            uint8_t* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            static const uint8_t pro[8] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83 };
            if (std::memcmp(exe + 0x20803a0, pro, 8) || std::memcmp(exe + 0x2080430, pro, 8)) {
                g_bbPinBad = true;
                TO1_LOG("[BLOOD] particle module pool code differs - mud / wipe modules not held\n");
                return;
            }
            uint8_t* pool = *reinterpret_cast<uint8_t**>(exe + 0x92d9720 + 0x70);
            if (!pool || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(pool), 0x400)
                || *reinterpret_cast<volatile int*>(pool + 0x300))
                return;
            using FnRef = void(*)(uint8_t*, uint64_t);
            using FnHas = char(*)(uint8_t*, uint64_t);
            FnRef f = reinterpret_cast<FnRef>(exe + (want ? 0x20803a0 : 0x2080430));
            if (!want && pool != g_bbPinPool) {
                g_bbPin = 0;
                return;
            }
            for (int i = 0; i < 2; i++) {
                const uint64_t sid = SID(kBbMods[i]);
                if (!want && !reinterpret_cast<FnHas>(exe + 0x207cff0)(pool, sid))
                    continue;
                f(pool, sid);
                TO1_LOG("[BLOOD] module %s %s (requested %d, in a pool slot %d)\n",
                    kBbMods[i], want ? "held" : "released",
                    reinterpret_cast<FnHas>(exe + 0x207cff0)(pool, sid) != 0,
                    reinterpret_cast<FnHas>(exe + 0x207cfc0)(pool, sid) != 0);
            }
            g_bbPin = want;
            g_bbPinPool = want ? pool : nullptr;
        }

        // a new life for the body: clean.
        void BbReset(int who, uint64_t h, uint32_t life)
        {
            BbBody* b = &g_bb[who];
            const float keepHp = b->hp;
            const int hpOk = b->hpOk;
            *b = BbBody{};
            b->h = h;
            b->life = life;
            b->seen = 1;
            b->born = engine::frame::Count() ? engine::frame::Count() : 1;
            b->hp = keepHp;
            b->hpOk = hpOk;
            for (int i = 0; i < kQueue; i++)
                if (g_bbQ[i].kind && g_bbQ[i].who == who)
                    g_bbQ[i].kind = 0;   // effects queued for the old life
        }

        void BbBodyTick(int who, int st, int dead, float hp, const float* pos,
                        uint64_t h, uint32_t life)
        {
            BbBody* b = &g_bb[who];
            if (!b->seen || b->h != h || b->life != life || (b->dead && !dead)) {
                const int had = b->seen;
                BbReset(who, h, life);
                if (had) {
                    TO1_LOG("[BLOOD] %s starts a new life (%s): clean\n", BbName(who),
                        who == kUs ? "our own spawn" : "new character");
                    if (who == kUs) {   // SP's own spawn: broadcast-event(clear-wetness) + wipe group
                        const uint64_t ev = SID("clear-wetness");
                        const uint64_t r = DC::Call<"broadcast-event">(ev);
                        TO1_LOG("[BLOOD] broadcast-event clear-wetness -> %llx\n",
                            static_cast<unsigned long long>(r));
                        BbQueue(who, kBkClear, 0, 700.0);
                    }
                }
            }
            // hits: a health drop on a living body (the same sequence on every screen).
            if (hp >= 0.0f && !dead) {
                if (b->hpOk && hp < b->hp - 2.0f && b->hp > 0.0f)
                    BbHit(who, b->hp - hp, hp);
                b->hp = hp;
                b->hpOk = 1;
            } else if (dead)
                b->hpOk = 0;
            // down / death: blood on the body, bystanders sprayed by a kill.
            if (st == 1 && !b->downed) {
                b->blood += 12.0f;
                if (!b->body) {
                    b->body = 1;
                    BbQueue(who, kBkBody, 0, 150.0);
                }
                BbEscalate(who);
                TO1_LOG("[BLOOD] %s goes down: blood %.0f\n", BbName(who),
                    static_cast<double>(b->blood));
            }
            if (dead && !b->dead)
                BbNearby(who, "death");
            b->downed = (st == 1 || st == 2) ? 1 : 0;
            b->dead = dead;
            // downed crawl: dirt (+ a little blood) per metre of drag.
            if (pos) {
                if (b->posOk) {
                    const float dx = pos[0] - b->px, dz = pos[2] - b->pz;
                    float v = std::sqrt(dx * dx + dz * dz) * 1000.0f / FrameMsC();
                    if (v > 8.0f)
                        v = 0;      // teleport / net snap
                    b->speed += (v - b->speed) * 0.12f;
                    if (st == 1 && b->speed > 0.35f)
                        b->crawlM += std::sqrt(dx * dx + dz * dz);
                }
                b->px = pos[0];
                b->pz = pos[2];
                b->posOk = 1;
            }
            while (st == 1 && b->crawlM >= 2.0f) {
                b->crawlM -= 2.0f;
                b->dirt += 12.5f;
                b->blood += 4.0f;
                if (b->dirt > 100.0f)
                    b->dirt = 100.0f;
                if (b->mud < 8 && !BbNoDirt()) {
                    b->mud++;
                    BbQueue(who, kBkMud, 0, 0.0);
                }
                BbEscalate(who);
            }
        }

        // ---- gun execution shots (exec_gore) --------------------------------------------
        // every shot stamps a wound mask at once; the last one leaves SP's blood pool under
        // the corpse (one pool per kill, a random group of blood-pool-01..08, at the body root).
        void GxbWound(int who, int zone)
        {
            if (who < 0 || who >= kBodies || zone < 0 || zone >= kMaxWounds || BbOff())
                return;
            BbBody* b = &g_bb[who];
            if (!g_bbW[zone].ok || b->wounds >= kWoundMax || (who != kUs && !b->h))
                return;
            b->wounds++;
            b->blood += 12.0f;
            BbQueue(who, kBkWound, zone, 30.0);
            BbEscalate(who);
        }

        void GxbPool(uint64_t ent, int who, double delayMs)
        {
            if (!ent || who < 0 || who >= kBodies || BbOff())
                return;
            for (int i = 0; i < 4; i++)
                if (!g_gxbPool[i].ent) {
                    g_gxbPool[i].ent = ent;
                    g_gxbPool[i].who = who;
                    g_gxbPool[i].due = PsyncMs() + delayMs;
                    return;
                }
        }

        void GxbTick()
        {
            static int nl;
            static unsigned pick;
            const double now = PsyncMs();
            for (int i = 0; i < 4; i++) {
                if (!g_gxbPool[i].ent || g_gxbPool[i].due > now)
                    continue;
                const uint64_t ent = g_gxbPool[i].ent;
                const int who = g_gxbPool[i].who;
                g_gxbPool[i].ent = 0;
                if (who == kUs && engine::local_player::Health() > 0.0f) {
                    TO1_LOG("[BLOOD] exec gore: pool under our body skipped - alive again\n");
                    continue;
                }
                if (!engine::clipfx::ObjOf(ent)) {
                    TO1_LOG("[BLOOD] exec gore: pool under %s skipped - body gone\n", BbName(who));
                    continue;
                }
                char nm[24];
                std::snprintf(nm, sizeof nm, "blood-pool-%02u",
                    1u + BbHash(static_cast<uint32_t>(engine::frame::Count() * 2654435761u)
                        + pick++) % 8u);
                const uint64_t r = engine::vfx::ArmorFxSpawn(SID(nm), ent, "root");
                if (LogCap(nl, 40))
                    TO1_LOG("[BLOOD] exec gore: blood pool %s under %s (1.4 s after the killing "
                        "shot) -> %llx\n", nm, BbName(who), static_cast<unsigned long long>(r));
            }
        }

        // ext MeleeBladeKills: MeleeRuleBlade(w) && !IntLive(). The melee_hitfx weapon-class
        // rule (MhfxClass) and the interrogation-live gate are not ported, so this is false.
        bool MeleeBladeKills(uint64_t weapon)
        {
            (void)weapon;
            return false;
        }

    }   // namespace

    void Tick()
    {
        static bool registered;
        if (!registered) {
            registered = true;
            // ext paired.c's psync_abort calls BbCancelPair; here it is registered instead.
            ::net::anim::paired::SetAbortHandler(BbCancelPair);
        }
        static int announced;
        if (!engine::frame::Count())
            return;
        if (BbOff()) {
            BbPin(0);
            if (g_bbInit) {
                std::memset(g_bb, 0, sizeof g_bb);
                std::memset(g_bbQ, 0, sizeof g_bbQ);
            }
            return;
        }
        const bool inMatch = InLevel();
        BbPin(inMatch ? 1 : 0);
        if (!inMatch) {
            std::memset(g_bb, 0, sizeof g_bb);
            std::memset(g_bbQ, 0, sizeof g_bbQ);
            g_bbUidSeen = 0;
            return;
        }
        BbInit();
        if (!g_bbNfx)
            return;
        if (!announced) {
            announced = 1;
            TO1_LOG("[BLOOD] build-up on (hits, executions, kills nearby, downed crawl; "
                "new life = clean; t2r-no-blood-buildup.txt = off)\n");
        }
        // ourselves: local health every 3rd frame, the life token, the downed state.
        {
            float lb[8];
            const float* ml = ::engine::object::LocalPlayerWorld(lb) ? lb : nullptr;
            static float hp = -1.0f;
            if (engine::frame::Count() % 3 == 0)
                hp = engine::local_player::Health();
            // ext: g_downed ? (g_downDying || g_uc4DyingUntil ? 2 : 1) : 0. The two dying
            // latches live in the ported downed core; without a death-clip producer they stay
            // clear, so this reads 1 while down until that feature is cut over.
            const int st = ::net::player_status::LocalDownState();
            if (ml)
                BbBodyTick(kUs, st, 0, hp, ml, SID("player"),
                    static_cast<uint32_t>(engine::backpack_prop::LifeId()));
        }
        for (int sl = 0; sl < kMaxNetPlayers; sl++) {
            NetPlayer& p = NetPlayers::BySlot(sl);
            if (!p.IsActiveRemote()) {
                if (g_bb[sl].seen)
                    g_bb[sl] = BbBody{};
                continue;
            }
            const uint64_t h = p.npcSid;
            if (!h)
                continue;
            float np[8] = {};
            np[0] = p.State().posX;
            np[1] = p.State().posY;
            np[2] = p.State().posZ;
            if (np[0] != np[0] || np[2] != np[2]
                || std::fabs(np[0]) > 1e5f || std::fabs(np[2]) > 1e5f)
                continue;
            const int dead = p.dead ? 1 : 0;
            // ext: g_peerDowned[sl] == 1 ? 1 : == 2 ? 2 : 0 -- the wire already carries the
            // distinction, so this is faithful whenever the downed bridge feeds peer events.
            const int st = ::net::player_status::PuppetDownState(sl);
            const float hp = (!dead && engine::frame::Count() % 3 == static_cast<uint64_t>(sl % 3))
                ? ENTITY::GET_HEALTH(h)
                : (g_bb[sl].hpOk ? g_bb[sl].hp : -1.0f);
            BbBodyTick(sl, st, dead, hp, np, h, 0);
        }
        BbPairTick();
        GxbTick();
        // due effects, at most 2 per frame.
        const double now = PsyncMs();
        int done = 0;
        for (int i = 0; i < kQueue && done < 2; i++) {
            if (!g_bbQ[i].kind || g_bbQ[i].due > now)
                continue;
            const BbPend p = g_bbQ[i];
            g_bbQ[i].kind = 0;
            if (p.who != kUs && !g_bb[p.who].h)
                continue;
            BbDo(&p);
            done++;
        }
    }

    void NoteMeleeBlow(int who, bool blade)
    {
        if (who >= 0 && who <= kMaxNetPlayers) {
            g_bbMeleeAt[who] = GetTickCount64();
            g_bbMeleeBlade[who] = blade ? 1 : 0;
        }
    }

    int IgSlam(uint64_t ent, int second)
    {
        return BbIgSlam(ent, second);
    }

    void GunWound(int who, int zone)
    {
        GxbWound(who, zone);
    }

    void GunPool(uint64_t ent, int who, double delayMs)
    {
        GxbPool(ent, who, delayMs);
    }

    void Reset()
    {
        BbPin(0);
        std::memset(g_bb, 0, sizeof g_bb);
        std::memset(g_bbQ, 0, sizeof g_bbQ);
        std::memset(g_bbMeleeAt, 0, sizeof g_bbMeleeAt);
        std::memset(g_bbMeleeBlade, 0, sizeof g_bbMeleeBlade);
        g_bbUidSeen = 0;
        g_bbUidWait = 0;
    }

}   // namespace net::match::bloodbuild
