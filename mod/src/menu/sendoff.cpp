// Native owner of tools/ext/sendoff.c. The ext's TEST sidecar trigger is not
// ported: the real funnel is mfnr's shared match-found horn latch, whose t=0 is
// the same clock the ext gave feature_match_horn. All engine effects live here;
// the pure camera / flock / envelope rules are in sendoff-core.hpp.
#include "sendoff.hpp"
#include "sendoff-core.hpp"
#include "sendoff-pcm.hpp"

#include "mfnr.hpp"

#include "boot.hpp"
#include "engine/anim-prims.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/pcm.hpp"
#include "log.hpp"          // TO1_LOG / AUDIO::PLAY_SOUND before script.hpp
#include "engine/script.hpp"
#include "engine/widget.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// windows.h still defines the old 16-bit `far` keyword macro; it would eat the
// flock distance field name in this translation unit.
#ifdef far
#  undef far
#endif

namespace menu::sendoff {

    namespace {

        namespace core = menu::sendoff::core;

        struct Config {
            int route = 0;
            int n = 14;
            int spawnFirst = 0;
            int fromLeft = 1;              // Seraphites always enter from the left
            float far = 0.0f, spread = -1.0f;
            char art[48] = "crow-t2";
            char type[32] = "SimpleObject";
            char takeoff[48] = "takeoff-up-0-fast";
            char fly[48] = "fly-straight-panic";
        };

        struct FleeEvent {
            uint64_t sid = 0, at = 0;
            char name[40]{};
        };

        struct Runtime {
            bool on = false, hornPlayed = false, birdsLive = false;
            int scene = 0;
            int crows = 0, fac = 0;        // 0 armed, 1 playing, 2 finished
            uint64_t lastHornAt = 0;
            uint64_t t0 = 0, blackAt = 0;
            uint64_t lastBirdAt = 0;
            float session = 1.0f;
            uint32_t rng = 0x9e3779b9u;
            Config cfg;
            core::CamBasis cam;
            core::Flock flock;
            std::array<FleeEvent, 96> flee{};
            int nFlee = 0;
        };

        Runtime g;

        bool Disabled()
        {
            return boot::ExtLoaded()
                || engine::flag::Absent("t2r-no-sendoff.txt");
        }

        void KillBirds(const char* why)
        {
            int killed = 0;
            for (int i = 0; i < core::kNb; ++i) {
                const uint64_t obj = g.flock.b[i].obj;
                if (!obj) continue;
                if (ENTITY::DOES_EXIST(static_cast<StringId64>(obj)))
                    ENTITY::KILL_ENTITY(static_cast<StringId64>(obj));
                g.flock.b[i].obj = 0;
                ++killed;
            }
            if (killed || g.nFlee)
                TO1_LOG("sendoff: birds removed (%d objects, %d flee events) - %s\n",
                        killed, g.nFlee, why);
            g.nFlee = 0;
            g.birdsLive = false;
        }

        void Stop(const char* why, int fadeMs, bool killBirds)
        {
            if (!g.on) return;
            pcm::StopHorn(fadeMs);
            pcm::StopCrows(fadeMs);
            pcm::StopFaction(fadeMs);
            if (killBirds) KillBirds(why);
            if (g.blackAt)
                DC::Call<"fade-to-black">(0.0f, 0.0f, 0.5f, 0.0f);
            engine::widget::Alpha(engine::widget::Page("anim-group"), 1.0f);
            engine::pcm::SetSessionVolume(1.0f);
            TO1_LOG("sendoff: stopped (%s)\n", why);
            const uint64_t horn = g.lastHornAt;
            const uint32_t rng = g.rng;
            g = Runtime{};
            g.lastHornAt = horn;
            g.rng = rng;
        }

        // Read-only t2r-sendoff-birds.txt: the ext's dev knobs. No write path
        // exists in the native owner; a missing / malformed file keeps the
        // shipped scene defaults.
        Config ReadConfig(int scene)
        {
            Config c;
            c.route = scene >= 0 && scene < static_cast<int>(core::kRoute.size())
                ? core::kRoute[scene] : core::kNone;
            FILE* f = engine::exe_side::Open("t2r-sendoff-birds.txt", "r");
            if (!f) return c;
            char key[32], val[48];
            int routes = -1;
            while (std::fscanf(f, "%31s %47s", key, val) == 2) {
                if (!std::strcmp(key, "route")) {
                    if (routes < 0) routes = 0;
                    if (std::strstr(val, "flee")) routes |= core::kFlee;
                    if (std::strstr(val, "ext")) routes |= core::kExt;
                    if (std::strstr(val, "fx")) routes |= core::kFx;
                } else if (!std::strcmp(key, "n")) {
                    const int n = std::atoi(val);
                    c.n = std::clamp(n, 1, core::kNb);
                } else if (!std::strcmp(key, "art")) {
                    std::snprintf(c.art, sizeof c.art, "%s", val);
                } else if (!std::strcmp(key, "type")) {
                    std::snprintf(c.type, sizeof c.type, "%s", val);
                } else if (!std::strcmp(key, "takeoff")) {
                    std::snprintf(c.takeoff, sizeof c.takeoff, "%s", val);
                } else if (!std::strcmp(key, "fly")) {
                    std::snprintf(c.fly, sizeof c.fly, "%s", val);
                } else if (!std::strcmp(key, "spawn")) {
                    c.spawnFirst = std::atoi(val) != 0;
                } else if (!std::strcmp(key, "far")) {
                    c.far = static_cast<float>(std::atof(val));
                } else if (!std::strcmp(key, "spread")) {
                    c.spread = static_cast<float>(std::atof(val));
                } else if (!std::strcmp(key, "from")) {
                    c.fromLeft = std::strcmp(val, "right") != 0;
                }
            }
            std::fclose(f);
            if (routes >= 0) c.route = routes;
            return c;
        }

        // The Seraphite trail flock's FlockingBird spawners. The ext probed the
        // loaded level for each name; sending the event to a name a level does
        // not own is harmless, and no raw VMA pointer is needed here.
        void QueueFleeName(uint64_t now, const char* fmt, int i)
        {
            FleeEvent& q = g.flee[g.nFlee++];
            std::snprintf(q.name, sizeof q.name, fmt, i);
            q.sid = SID(q.name);
            q.at = now + static_cast<uint64_t>(core::Rand01(g.rng)
                                               * core::kFleeJitMs)
                 + (g.cfg.spawnFirst ? 150 : 0);
            if (g.cfg.spawnFirst)
                DC::Call<"spawn-entity">(q.sid, uint64_t{0}, uint64_t{0});
        }

        void QueueFlee(uint64_t now)
        {
            for (int i = 1; i <= 63 && g.nFlee < static_cast<int>(g.flee.size()); ++i)
                QueueFleeName(now, "flee-sle-bird-lookout-%d", i);
            for (int i = 1; i <= 21 && g.nFlee < static_cast<int>(g.flee.size()); ++i)
                QueueFleeName(now, "flee-sle-bird-forest-%d", i);
        }

        void SpawnFx()
        {
            const core::CamBasis& c = g.cam;
            for (int i = 0; i < 4; ++i) {
                const float d = 14.0f + 6.0f * static_cast<float>(i);
                const float lat = (static_cast<float>(i) - 1.5f) * d * c.th * 0.4f;
                float p[3];
                for (int k = 0; k < 3; ++k)
                    p[k] = c.c[k] + c.f[k] * d + c.r[k] * lat
                         - c.u[k] * d * c.th * 0.3f;
                const float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                const uint64_t frame = MEMORY::ALLOC_BOUND_FRAME(
                    Vector3(p[0], p[1], p[2]),
                    Quaternion(q[0], q[1], q[2], q[3]));
                if (!frame || frame == ~uint64_t{0}) continue;
                DC::Call<"spawn-particles-at-bound-frame">(
                    SID("birds-flipbook"), frame, SID("none"));
            }
        }

        void BirdsGo(uint64_t now)
        {
            KillBirds("a new go");
            g.cfg = ReadConfig(g.scene);
            g.cam = core::Camera(g.scene);
            g.flock = core::Flock{};
            g.flock.scene = g.scene;
            g.flock.route = g.cfg.route;
            g.flock.n = g.cfg.n;
            g.flock.goMs = now;
            g.flock.far = g.cfg.far > 0.5f ? g.cfg.far : core::Far(g.scene);
            g.flock.spread = g.cfg.spread >= 0.0f ? g.cfg.spread : core::kSpread;
            // Seraphites leave from the left (behind the big trunk); WLF uses a
            // random side unless the sidecar pins one.
            g.flock.dir = g.scene == core::kSer ? 1
                : (g.cfg.fromLeft || core::Rand01(g.rng) < 0.5f ? 1 : -1);
            if (g.cfg.route & core::kFlee) QueueFlee(now);
            if (g.cfg.route & core::kFx) SpawnFx();
            if (!(g.cfg.route & core::kExt)) return;

            int spawned = 0;
            for (int i = 0; i < g.flock.n; ++i) {
                core::Bird& b = g.flock.b[i];
                const float d = g.flock.far + g.flock.spread * core::Rand01(g.rng);
                core::PlaceBird(b, g.cam, i, g.flock.dir, d, g.cam.th, g.rng);
                uint64_t obj = ENTITY::SPAWN_OBJECT(SID(g.cfg.art),
                                                    SID(g.cfg.type));
                if ((!obj || obj == ~uint64_t{0})
                    && i == 0 && std::strcmp(g.cfg.type, "SimpleAttachable")) {
                    std::snprintf(g.cfg.type, sizeof g.cfg.type, "SimpleAttachable");
                    obj = ENTITY::SPAWN_OBJECT(SID(g.cfg.art), SID(g.cfg.type));
                }
                if (!obj || obj == ~uint64_t{0}) continue;
                b.obj = obj;
                ++spawned;
                float q[4];
                core::QuatDir(b.v, q);
                const uint64_t frame = MEMORY::ALLOC_BOUND_FRAME(
                    Vector3(b.p[0], b.p[1], b.p[2]),
                    Quaternion(q[0], q[1], q[2], q[3]));
                if (frame && frame != ~uint64_t{0})
                    LOCATOR::SET_OBJECT_LOCATOR(
                        static_cast<StringId64>(obj), frame);
                if (i == 0)
                    engine::anim::AnimateFrom(static_cast<StringId64>(obj),
                                              g.cfg.takeoff, 0.3f, 0.0f);
            }
            g.birdsLive = spawned > 0;
            TO1_LOG("sendoff: %d/%d crows from the %s, %.0f..%.0f m out\n",
                    spawned, g.flock.n, g.flock.dir > 0 ? "left" : "right",
                    g.flock.far, g.flock.far + g.flock.spread);
        }

        void FlyLifetime(uint64_t now, uint64_t t)
        {
            for (int i = 0; i < g.nFlee;) {
                if (now < g.flee[i].at) { ++i; continue; }
                DC::Call<"send-event">(SID("force-flee"), g.flee[i].sid);
                g.flee[i] = g.flee[--g.nFlee];
            }
            if (!g.birdsLive) return;

            const float dt = g.lastBirdAt && now > g.lastBirdAt
                ? std::min(0.1f, static_cast<float>(now - g.lastBirdAt)
                                     / 1000.0f)
                : 1.0f / 60.0f;
            g.lastBirdAt = now;
            const float elapsed = static_cast<float>(t) / 1000.0f;
            int alive = 0;
            for (int i = 0; i < g.flock.n; ++i) {
                core::Bird& b = g.flock.b[i];
                if (!b.obj) continue;
                if (!ENTITY::DOES_EXIST(static_cast<StringId64>(b.obj))) {
                    b.obj = 0;
                    continue;
                }
                ++alive;
                float p[3], q[4];
                if (!core::StepBird(b, g.cam, dt, elapsed, i, p, q)) {
                    b.obj = 0;
                    continue;
                }
                const uint64_t frame = MEMORY::ALLOC_BOUND_FRAME(
                    Vector3(p[0], p[1], p[2]),
                    Quaternion(q[0], q[1], q[2], q[3]));
                if (frame && frame != ~uint64_t{0})
                    LOCATOR::SET_OBJECT_LOCATOR(
                        static_cast<StringId64>(b.obj), frame);
                const bool animDue = core::FlyAnimDue(b, now, elapsed,
                                                      b.animMs);
                if (!animDue) continue;
                if (b.phase == 0) {
                    b.phase = 1;
                    b.animAt = now;
                    const float len = engine::anim::Len(
                        static_cast<StringId64>(b.obj), g.cfg.fly);
                    b.animMs = len > 0.1f ? static_cast<int>(len * 1000.0f) : 600;
                } else {
                    b.animAt = now;
                }
                engine::anim::AnimateFrom(static_cast<StringId64>(b.obj),
                                          g.cfg.fly, 0.3f, 0.0f);
            }
            if (t > 8000 || !alive)
                KillBirds(alive ? "flight over" : "all gone");
        }

        void Start(int scene, uint64_t t0)
        {
            g = Runtime{};
            g.on = true;
            g.scene = scene;
            g.t0 = t0;
            g.lastHornAt = t0;
            g.cam = core::Camera(scene);
            TO1_LOG("sendoff: armed (%s): crows %llu ms, %s %llu ms\n",
                    scene == core::kWlf ? "WLF" : "Seraphites",
                    static_cast<unsigned long long>(core::kCrowsMs),
                    scene == core::kWlf ? "vehicles" : "horses",
                    static_cast<unsigned long long>(core::kFacMs));
        }

    } // namespace

    void Tick(uint64_t now)
    {
        if (Disabled()) {
            if (g.on) Stop("disabled", 0, true);
            return;
        }

        const auto horn = net::ui::mfnr::Horn();
        if (horn.active && horn.startedAt != g.lastHornAt) {
            if (horn.scene == core::kWlf || horn.scene == core::kSer)
                Start(horn.scene, horn.startedAt);
            else
                g.lastHornAt = horn.startedAt;   // no scene: do not retry
        }
        if (!g.on) return;
        if (!horn.active && now - g.t0 < core::kEndMs) {
            Stop("horn aborted", 300, true);
            return;
        }

        const uint64_t raw = now >= g.t0 ? now - g.t0 : 0;
        const uint64_t t = std::min(raw, core::kMaxMs);
        g.session = t < core::kEndMs
            ? core::Duck(t) * core::BlackFade(t) : 1.0f;
        engine::pcm::SetSessionVolume(g.session);

        // The top menu page fades out over MH_UI_MS. Stop writing after 3.5 s:
        // the ext left the faded instance alone once the LOADING page took over.
        if (t < core::kUiMs + 3000) {
            const float a = t >= core::kUiMs ? 0.0f
                : 1.0f - static_cast<float>(t) / static_cast<float>(core::kUiMs);
            engine::widget::Alpha(engine::widget::Page("anim-group"), a);
        }

        if (!g.hornPlayed && t >= core::kHornMs) {
            g.hornPlayed = true;
            // The bank clip is tried first; the private clip is a silent no-op
            // unless TO1_SENDOFF_PCM and its data header were built in.
            AUDIO::PLAY_SOUND(SID("t2r-fx-match-horn"));
            pcm::PlayHorn(0.9f * core::Compensate(1.0f, g.session));
        } else if (g.hornPlayed) {
            pcm::HornGain(0.9f * core::Compensate(1.0f, g.session));
        }

        if (!g.crows && t >= core::kCrowsMs) {
            g.crows = 1;
            pcm::PlayCrows(core::Compensate(1.0f, g.session));
            BirdsGo(now);
        } else if (g.crows == 1) {
            pcm::CrowsGain(core::Compensate(1.0f, g.session));
        }

        const float env = core::Environment(t);
        const float voice = core::Compensate(env, g.session);
        if (!g.fac && t >= core::kFacMs) {
            g.fac = 1;
            pcm::PlayFaction(g.scene, voice);
        } else if (g.fac == 1) {
            pcm::FactionGain(voice);
            if (t >= core::kFadeMs) {
                g.fac = 2;
                pcm::StopFaction(60);
            }
        }

        FlyLifetime(now, t);

        if (!g.blackAt && t >= core::kBlackMs) {
            g.blackAt = now;
            HUD::FADE_TO_BLACK(core::kBlackFadeS, 2.0f);
        } else if (g.blackAt && t < core::kEndMs + 1500
                   && now > g.blackAt + 1500) {
            // Lift once so the LOADING screen does not show the camera through
            // a fully black picture, then rearm under the page.
            HUD::FADE_TO_BLACK(0.0f, 1.0f);
            g.blackAt = now;
        }

        if (t >= core::kEndMs || t >= core::kMaxMs)
            Stop("level start", 150, true);
    }

    void Reset()
    {
        Stop("session reset", 0, true);
    }

} // namespace menu::sendoff
