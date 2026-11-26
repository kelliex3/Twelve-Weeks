// Native owner for tools/ext/customize_stage.c (build 191). The pure layout,
// tuning, timing, camera, light and grave rules live in customize-stage-core.hpp;
// this file owns every engine effect: the fade + camera cut, composed NPC spawn
// and pose, photo-mode lights, the stage freecam and the compaction-time kill /
// reap. There is deliberately no ext cutover: while t2r-ext.dll is loaded we
// yield completely.

#include "customize-stage.hpp"

#include "log.hpp"

#include "boot.hpp"
#include "engine/anim-prims.hpp"
#include "engine/armor-rig.hpp"
#include "engine/exe_side.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/menu-input.hpp"
#include "engine/ptr.hpp"
#include "engine/raycast.hpp"
#include "engine/script.hpp"
#include "engine/transform.hpp"
#include "engine/window.hpp"
#include "net/ui/notify.hpp"
#include "net/ui/customize-compose.hpp"
#include "net/ui/customize.hpp"
#include "sid/sid.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::customize::stage {

    namespace {

        using namespace core;

        constexpr uintptr_t kPhotoMgrRva = 0x4250bc0;
        constexpr uintptr_t kPhotoGateRva = 0x3ffc82;
        constexpr uintptr_t kCompactionRva = 0x14476D0;
        constexpr uintptr_t kCleanupDeadRva = 0x133f260;

        constexpr uint16_t kPadUp = 0x0001;
        constexpr uint16_t kPadDown = 0x0002;
        constexpr uint16_t kPadLeft = 0x0004;
        constexpr uint16_t kPadRight = 0x0008;
        constexpr uint16_t kPadL3 = 0x0040;
        constexpr uint16_t kPadR3 = 0x0080;
        constexpr uint16_t kPadX = 0x4000;
        constexpr uint16_t kPadY = 0x8000;

        struct Model {
            uint64_t proc = 0, id = 0;
            int fac = 0, sex = 0;
            uint8_t it[CZ_NCAT]{};
            char look[0x28] = "";
            uint64_t tSpawn = 0;
            bool passive = false;
            float pos[3] = {0, 0, 0}, yaw = 0.0f;
            char clip[48] = "";
            int clipTried = 0;
            uint64_t tPose = 0;
            float clipLen = 0.0f, blendNext = 0.0f;
            float apo[4] = {0, 0, 0, 0};
            int slot = 0, unarmed = 0;
            uint64_t tFace = 0, tClean = 0, tEyes = 0;
            bool eyesOn = false;
        };

        struct Grave {
            uint64_t id = 0, proc = 0, tSpawn = 0, tPark = 0;
            Model m;
            float hp[3] = {0, 0, 0};
        };

        struct Freecam {
            bool allowed = false, on = false, look = false, l3r3 = false;
            uint64_t chk = 0, lastMs = 0;
            uint16_t pbLast = 0;
            float p[3] = {0, 0, 0}, yaw = 0.0f, pitch = 0.0f, fov = 40.0f;
            float gx = 0.0f, gz = 0.0f, ry0[3] = {0, 0, 0}, ry1 = 0.0f;
            uint64_t ray[3] = {0, 0, 0};
            int rayWait = 0;
            POINT anchor{};
        };

        struct Browser {
            int sel = 0;
            int idx[2] = {-1, -1};
            int nextra = 0;
            char extra[48][48] = {};
            char pick[2][48] = {};
        };

        struct State {
            bool active = false, off = false, camReady = false, lit = false;
            bool builtIn = false, yieldOff = false;
            uint64_t fileAt = 0, openAt = 0;
            uint64_t spawnMs = 0, killMs = 0, killFrame = 0, skipUntil = 0;
            uint64_t ignore = 0, plChk = 0;
            float pl0[3] = {0, 0, 0};
            bool plMoved = false;
            int front = 0, scene = 0;
            float camYawDeg = 0.0f;
            float lspot[4] = {0, 0, 0, 999};
            int lspotScene = 0;
            uint64_t swapT0 = 0;
            float userYaw = 0.0f;
            bool drag = false;
            float dragX = 0.0f;
            float zFov = 0.0f, zDp = 0.0f, zDy = 0.0f, zMy = 0.0f;
            uint64_t zMs = 0;
            int pvFoc[2] = {-1, -1};
            uint64_t pvAt[2] = {0, 0};
            int poseState = 0, poseLogged = 0;
            uint64_t poseAt = 0, poseT0 = 0, posePaksAt = 0;
            Tuning tuning{};
            LightSettings lights{};
            Layout ab[3]{}, abFile[3]{};
            core::Derived derived{};
            core::SwapFrame swap{};
            Model m[2], nx[2];
            Grave grave[kGraveN];
            uint64_t killQ[kKillQ] = {}, killAsked[kKillQ] = {};
            Freecam fc;
            Browser browser;
            int fcShield = 0;
        } g;

        uintptr_t gModule = 0;
        bool gHookAttempted = false;
        uint64_t gCompactionFrame = 0;
        SafetyHookInline gCompactionHook;
        int gWatchCompact = -1, gWatchSkip = -1, gWatchReap = -1;

        // Mirrors ext's static "read this sidecar once" gates. These files are
        // always read-only from the native owner.
        bool Sidecar(const char* name, int& cache)
        {
            if (cache < 0)
                cache = engine::exe_side::Exists(name) ? 1 : 0;
            return cache != 0;
        }

        const char* FacName(int fac)
        {
            return (fac & 1) ? "Seraphites" : "WLF";
        }

        bool Valid(uint64_t id)
        {
            return id != 0 && id != ~0ULL;
        }

        bool Alive(const Model& m)
        {
            return m.proc != 0 && m.id != 0 && engine::ptr::Readable(m.proc + 0x94, 4);
        }

        uint64_t Frame() { return engine::frame::Count(); }

        bool Edge(int vk)
        {
            static std::array<bool, 256> held{};
            const bool down = engine::window::KeyDown(vk);
            const bool rose = down && !held[static_cast<size_t>(vk & 0xff)];
            held[static_cast<size_t>(vk & 0xff)] = down;
            return rose;
        }

        void Notify(const char* text, float secs)
        {
            net::notify::Push(net::notify::NTF_INFO, text, secs);
        }

        void Fade(float in, float hold, float out)
        {
            DC::Call<"fade-to-black">(in, hold, out, 0.0f);
        }

        void WriteU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof v); }
        void WriteF(uint8_t* p, float v) { std::memcpy(p, &v, sizeof v); }

        // ---------------------------------------------------------- sidecars

        template <class F>
        void EachLine(const char* text, F fn)
        {
            if (!text) return;
            const char* p = text;
            while (*p) {
                const char* e = std::strchr(p, '\n');
                const size_t n = e ? static_cast<size_t>(e - p) : std::strlen(p);
                char line[256];
                const size_t cap = std::min(n, sizeof line - 1);
                std::memcpy(line, p, cap);
                line[cap] = 0;
                if (cap) fn(line);
                if (!e) break;
                p = e + 1;
            }
        }

        void MergeLayout(Layout& dst, const Layout& src)
        {
            if (src.scene < 1 || src.scene > kScenes) return;
            if (src.model[0]) {
                dst.scene = src.scene;
                dst.model[0] = true;
                std::memcpy(dst.mv[0], src.mv[0], sizeof dst.mv[0]);
            }
            if (src.model[1]) {
                dst.scene = src.scene;
                dst.model[1] = true;
                std::memcpy(dst.mv[1], src.mv[1], sizeof dst.mv[1]);
            }
            if (src.camera) {
                dst.scene = src.scene;
                dst.camera = true;
                std::memcpy(dst.cv, src.cv, sizeof dst.cv);
            }
            for (int i = 0; i < 3; ++i)
                if (src.light[i]) {
                    dst.scene = src.scene;
                    dst.light[i] = true;
                    std::memcpy(dst.lv[i], src.lv[i], sizeof dst.lv[i]);
                }
        }

        void ReadFile(uint64_t now)
        {
            if (now < g.fileAt) return;
            g.fileAt = now + kFilePollMs;

            FILE* f = engine::exe_side::Open("t2r-no-customize-stage.txt", "r");
            g.off = f != nullptr;
            if (f) std::fclose(f);

            if (!g.builtIn) {
                g.builtIn = true;
                EachLine(kShippedLayout, [](const char* line) {
                    ParseStageLine(line, g.tuning);
                    Layout tmp;
                    if (ParseLayoutLine(line, tmp)) {
                        const int sc = tmp.scene;
                        if (sc >= 1 && sc <= kScenes) MergeLayout(g.ab[sc], tmp);
                    }
                });
            }

            f = engine::exe_side::Open("t2r-cz-stage.txt", "r");
            if (!f) return;
            for (Layout& l : g.abFile) l = Layout{};
            char line[256];
            while (std::fgets(line, sizeof line, f)) {
                Layout tmp;
                if (ParseLayoutLine(line, tmp)) {
                    const int sc = tmp.scene;
                    if (sc >= 1 && sc <= kScenes) MergeLayout(g.abFile[sc], tmp);
                    continue;
                }
                ParseStageLine(line, g.tuning);
            }
            std::fclose(f);
            // A scene the file lays out completely wins over the compiled-in one.
            for (int sc = 1; sc <= kScenes; ++sc)
                if (g.abFile[sc].Complete(sc)) g.ab[sc] = g.abFile[sc];
        }

        void LightLoad()
        {
            FILE* f = engine::exe_side::Open("t2r-cz-light.txt", "r");
            if (!f) return;
            char line[128];
            while (std::fgets(line, sizeof line, f))
                ParseLightLine(line, g.lights);
            std::fclose(f);
        }

        void LightSave()
        {
            char text[192];
            FormatLightFile(g.lights, text, sizeof text);
            FILE* f = engine::exe_side::Open("t2r-cz-light.txt", "w");
            if (!f) return;
            std::fputs(text, f);
            std::fclose(f);
        }

        void LightStep(int dir)
        {
            const int stage = g.front & 1;
            const int setup = g.lights.setup[stage];
            if (setup > 0 && setup <= kCineCount)
                g.lights.setupK[stage][setup] = StepStrength(g.lights.setupK[stage][setup], dir);
            else
                g.lights.strength[stage] = StepStrength(g.lights.strength[stage], dir);
            LightSave();
        }

        float LightPercent() { return ActiveStrength(g.lights, g.front & 1); }

        const char* CineStep()
        {
            const int stage = g.front & 1;
            g.lights.setup[stage] = NextSetup(g.lights.setup[stage]);
            LightSave();
            return SetupName(g.lights.setup[stage]);
        }

        // ------------------------------------------------------ photo lights

        uintptr_t ModuleBase()
        {
            return gModule ? gModule : reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
        }

        uint8_t* LightSlots()
        {
            const uintptr_t base = ModuleBase();
            if (!engine::ptr::Readable(base + kPhotoMgrRva, sizeof(uintptr_t))) return nullptr;
            const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(base + kPhotoMgrRva);
            if (!engine::ptr::Writable(mgr + 0xd40, 3 * 0x70)) return nullptr;
            return reinterpret_cast<uint8_t*>(mgr + 0xd40);
        }

        bool LightGate(bool open)
        {
            static bool bad = false;
            uint8_t* p = reinterpret_cast<uint8_t*>(ModuleBase() + kPhotoGateRva);
            if (!engine::ptr::Readable(reinterpret_cast<uintptr_t>(p) - 2, 4)) return false;
            const uint8_t want[2] = {
                static_cast<uint8_t>(open ? 0x90 : 0x74),
                static_cast<uint8_t>(open ? 0x90 : 0x0e)};
            const uint8_t from[2] = {
                static_cast<uint8_t>(open ? 0x74 : 0x90),
                static_cast<uint8_t>(open ? 0x0e : 0x90)};
            if (p[0] == want[0] && p[1] == want[1]) return true;
            if (p[-2] != 0x84 || p[-1] != 0xc0 || p[0] != from[0] || p[1] != from[1]) {
                if (!bad) {
                    bad = true;
                    TO1_LOG("[customize-stage] photo-mode light gate: unexpected bytes %02x %02x %02x %02x - no stage lights\n",
                            p[-2], p[-1], p[0], p[1]);
                }
                return false;
            }
            DWORD old = 0;
            if (!::VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) return false;
            p[0] = want[0];
            p[1] = want[1];
            ::VirtualProtect(p, 2, old, &old);
            ::FlushInstructionCache(::GetCurrentProcess(), p, 2);
            return true;
        }

        void WriteSlot(uint8_t* q, const LightSlot& s)
        {
            if (!q) return;
            std::memset(q, 0, 0x70);
            WriteF(q + 0x10, s.pos[0]);
            WriteF(q + 0x14, s.pos[1]);
            WriteF(q + 0x18, s.pos[2]);
            WriteF(q + 0x1c, s.pos[3]);
            WriteF(q + 0x20, s.target[0]);
            WriteF(q + 0x24, s.target[1]);
            WriteF(q + 0x28, s.target[2]);
            WriteF(q + 0x2c, s.target[3]);
            WriteF(q + 0x30, s.color[0]);
            WriteF(q + 0x34, s.color[1]);
            WriteF(q + 0x38, s.color[2]);
            WriteF(q + 0x40, s.intensity);
            WriteF(q + 0x44, 1.0f);
            WriteF(q + 0x48, s.cone);
            WriteF(q + 0x4c, s.cone * 0.7f);
            WriteF(q + 0x50, s.soft);
            WriteU32(q + 0x54, s.shadows ? 1u : 0u);
            WriteF(q + 0x64, 0.1f);
            WriteU32(q + 0x00, s.enabled ? 1u : 0u);
        }

        void LightsOff()
        {
            if (!g.lit) return;
            if (uint8_t* s = LightSlots())
                for (int i = 0; i < 3; ++i) WriteU32(s + i * 0x70, 0);
            LightGate(false);
            g.lit = false;
        }

        void Lights(int sc, const float cam[3], const float front[3], const float back[3])
        {
            if (!g.tuning.lights) {
                LightsOff();
                return;
            }
            uint8_t* s = LightSlots();
            if (!s) return;
            if (!g.lit) {
                if (!LightGate(true)) {
                    g.tuning.lights = false;
                    return;
                }
                g.lit = true;
            }
            const int stage = g.front & 1;
            const int setup = g.lights.setup[stage];
            const float strength = ActiveStrength(g.lights, stage);
            LightSlot slot[3];
            if (setup > 0 && setup <= kCineCount)
                CineSlots(setup, front, cam, strength, slot);
            else
                LayoutSlots(g.ab[sc], sc, cam, front, back, g.tuning.light, strength, slot);
            for (int i = 0; i < 3; ++i) WriteSlot(s + i * 0x70, slot[i]);
        }

        // --------------------------------------------------- kill / reap / hook

        bool ReapDeadList()
        {
            if (Sidecar("t2r-no-cz-reap.txt", gWatchReap)) return false;
            const uintptr_t fn = ModuleBase() + kCleanupDeadRva;
            static int ok = -1;
            if (ok < 0) {
                static const uint8_t prologue[] = {
                    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                    0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
                ok = engine::ptr::Readable(fn, 0x40)
                    && std::memcmp(reinterpret_cast<const void*>(fn), prologue, sizeof prologue) == 0
                    && *reinterpret_cast<const uint8_t*>(fn + 0x2e) == 0x44
                    && *reinterpret_cast<const uint8_t*>(fn + 0x2f) == 0x38
                    && *reinterpret_cast<const uint8_t*>(fn + 0x30) == 0x3d
                    ? 1 : 0;
                if (!ok)
                    TO1_LOG("[customize-stage] Process::CleanupDeadList not recognised - removals are not reaped at once\n");
            }
            if (!ok) return false;
            const int32_t disp = *reinterpret_cast<const int32_t*>(fn + 0x31);
            const uintptr_t busy = fn + 0x35 + static_cast<uintptr_t>(disp);
            if (!engine::ptr::Readable(busy, 1) || *reinterpret_cast<const uint8_t*>(busy)) return false;
            reinterpret_cast<void (*)()>(fn)();
            return true;
        }

        void KillEntity(uint64_t id)
        {
            if (!id) return;
            DC::Call<"kill-entity">(id);
            g.killFrame = Frame();
            // dc_call() in the ext holds this compaction pass and the next two.
            g.skipUntil = g.killFrame + 2;
        }

        void KillDrainImpl()
        {
            for (int i = 0; i < kKillQ; ++i) {
                if (!g.killQ[i]) continue;
                const uint64_t id = g.killQ[i];
                g.killQ[i] = 0;
                KillEntity(id);
                ReapDeadList();
                return; // one queued removal per compaction pass
            }
        }

        void KillRequest(uint64_t id)
        {
            if (!id) return;
            if (gCompactionHook && gCompactionFrame + 5 >= Frame()) {
                for (int i = 0; i < kKillQ; ++i)
                    if (!g.killQ[i]) {
                        g.killAsked[i] = Frame();
                        g.killQ[i] = id;
                        return;
                    }
            }
            KillEntity(id); // no hook / queue full: remove at once, as the ext does
        }

        void __fastcall CompactionHook(void* pm)
        {
            gCompactionFrame = Frame();
            KillDrainImpl();
            if (g.skipUntil && gCompactionFrame <= g.skipUntil
                && !Sidecar("t2r-no-cm-skip.txt", gWatchSkip))
                return;
            gCompactionHook.fastcall<void>(pm);
        }

        void EnsureCompactionHook()
        {
            if (gHookAttempted || ::GetTickCount64() == 0) return;
            if (Frame() < 3 || boot::ExtLoaded()) return;
            gHookAttempted = true;
            if (Sidecar("t2r-no-cm-watch.txt", gWatchCompact)) return;
            static const uint8_t prologue[19] = {
                0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68,
                0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};
            const uintptr_t target = ModuleBase() + kCompactionRva;
            if (!engine::ptr::Readable(target, sizeof prologue)
                || std::memcmp(reinterpret_cast<const void*>(target), prologue, sizeof prologue) != 0) {
                TO1_LOG("[customize-stage] compaction prologue not recognised - removals run at once\n");
                return;
            }
            gCompactionHook = safetyhook::create_inline(
                reinterpret_cast<void*>(target), reinterpret_cast<void*>(&CompactionHook));
            if (!gCompactionHook)
                TO1_LOG("[customize-stage] compaction hook failed - removals run at once\n");
        }

        // ---------------------------------------------------------- geometry

        bool PlayerLoc(float out[3])
        {
            float packed[8] = {};
            if (!engine::transform::ProcessWorld(engine::local_player::Ptr(), packed)) return false;
            out[0] = packed[0];
            out[1] = packed[1];
            out[2] = packed[2];
            return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
        }

        bool SpotFor(int sc, float out[4])
        {
            if (sc < 1 || sc > kScenes) return false;
            if (g.ab[sc].Complete(sc)) {
                out[0] = g.ab[sc].mv[0][0];
                out[1] = g.ab[sc].mv[0][1];
                out[2] = g.ab[sc].mv[0][2];
                out[3] = g.ab[sc].cv[3];
                return true;
            }
            if (g.tuning.spot[sc][3] < 900.0f) {
                for (int i = 0; i < 4; ++i) out[i] = g.tuning.spot[sc][i];
                return true;
            }
            // The hidden menu player's spot, read ONCE per scene per visit. The
            // ext's MS_POSE view directions are used to recover the scene yaw.
            if (g.lspotScene != sc) {
                float here[3] = {};
                if (!PlayerLoc(here)) return false;
                static const float pose[2][6] = {
                    {-26.72f, -5.21f, 1036.27f, -44.42f, -1.07f, 1027.95f},
                    {272.87f, 18.17f, 52.01f, 253.20f, 19.26f, 48.56f}};
                const float* d = pose[sc - 1];
                const float fx = d[3] - d[0], fz = d[5] - d[2];
                if (fx * fx + fz * fz < 1e-6f) return false;
                g.lspot[0] = here[0];
                g.lspot[1] = here[1];
                g.lspot[2] = here[2];
                g.lspot[3] = std::atan2(fx, fz) / kRad;
                g.lspotScene = sc;
            }
            for (int i = 0; i < 4; ++i) out[i] = g.lspot[i];
            return true;
        }

        void Derive(int sc)
        {
            float spot[4] = {0, 0, 0, 999};
            SpotFor(sc, spot);
            const bool abs = g.ab[sc].Complete(sc);
            float cam[3] = {}, front[3] = {}, back[3] = {};
            if (abs) {
                for (int i = 0; i < 3; ++i) {
                    cam[i] = g.ab[sc].cv[i];
                    front[i] = g.ab[sc].mv[0][i];
                }
                g.camYawDeg = g.ab[sc].cv[3];
                g.derived.pitch = g.ab[sc].cv[4];
                g.derived.fov = g.ab[sc].cv[5];
                g.derived.frontRest = g.ab[sc].mv[0][3] * kRad;
                if (g.ab[sc].model[1]) {
                    for (int i = 0; i < 3; ++i) back[i] = g.ab[sc].mv[1][i];
                    g.derived.backRest = g.ab[sc].mv[1][3] * kRad;
                } else {
                    // Only the front model was laid out: keep the absolute
                    // camera / front place, derive the back one from the spot.
                    float spotCam[3] = {}, spotFront[3] = {};
                    LayoutFromSpot(spot, spotCam, spotFront, back);
                    (void)spotCam;
                    (void)spotFront;
                    g.derived.backRest = FaceYaw(back, cam);
                }
            } else {
                LayoutFromSpot(spot, cam, front, back);
                g.camYawDeg = spot[3];
                g.derived.pitch = g.tuning.pitch;
                g.derived.fov = g.tuning.fov;
                g.derived.frontRest = FaceYaw(front, cam);
                g.derived.backRest = FaceYaw(back, cam);
            }
            for (int i = 0; i < 3; ++i) {
                g.derived.cam[i] = cam[i];
                g.derived.front[i] = front[i];
                g.derived.back[i] = back[i];
            }
            g.derived.yaw = spot[3] * kRad;
            g.derived.valid = true;
        }

        // ----------------------------------------------------------- outfit

        void WantItems(int fac, int sex, uint8_t* out, uint64_t now)
        {
            std::memcpy(out, GetProfile().it[fac][sex], CZ_NCAT);
            const int cat = EditingCategory();
            if (!PageUp() || EditingFaction() != fac || cat == CZ_SEX
                || cat < 0 || cat >= CZ_NCAT) return;
            const int foc = FocusedKey(fac, sex, cat);
            if (foc != g.pvFoc[fac]) {
                g.pvFoc[fac] = foc;
                g.pvAt[fac] = now;
            }
            if (foc < 0) return;
            const ::net::ui::customize::core::cz_item_t* item =
                ::net::ui::customize::core::Item(fac, sex, cat, foc);
            if (!item || item->tier > 1 || now - g.pvAt[fac] < kPreviewHoldMs) return;
            if (::net::ui::customize::core::ConflictCat(
                    fac, sex, GetProfile().it[fac][sex], cat, foc) >= 0) return;
            out[cat] = static_cast<uint8_t>(foc);
        }

        void SpawnAs(Model& m, int slot, int fac, const char* look,
                     const float* pos, float yaw, uint64_t now)
        {
            static char name[32][32], looks[32][0x28];
            static unsigned seq = 0;
            const uint64_t bf = MEMORY::ALLOC_BOUND_FRAME(
                Vector3(pos[0], pos[1], pos[2]),
                Quaternion(0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)));
            if (!bf || bf == ~0ULL) return;
            const unsigned k = seq++ % 32;
            std::snprintf(name[k], sizeof name[k], "t2r-cz-stage-%d-%u", slot, seq);
            std::snprintf(looks[k], sizeof looks[k], "%s", look);
            const uint64_t proc = DC::Call<"c-spawn-npc-debug">(
                bf, reinterpret_cast<uint64_t>(name[k]), reinterpret_cast<uint64_t>(looks[k]),
                ToStringId64(g.tuning.cls[fac]), ToStringId64(g.tuning.arch[fac]),
                0ULL, 0ULL, 0ULL);
            uint64_t id = 0;
            if (Valid(proc) && engine::ptr::Readable(proc + 0x94, 4))
                id = DC::Call<"get-process-user-id">(proc);
            std::snprintf(m.look, sizeof m.look, "%s", look);
            m.slot = slot;
            m.tSpawn = now;
            m.passive = false;
            std::memcpy(m.pos, pos, 12);
            m.yaw = yaw;
            m.clip[0] = 0;
            m.clipTried = 0;
            m.tPose = 0;
            m.clipLen = 0.0f;
            m.blendNext = 0.0f;
            m.unarmed = 0;
            m.tFace = m.tClean = m.tEyes = 0;
            m.eyesOn = false;
            g.spawnMs = m.tSpawn;
            if (!Valid(id)) {
                m.proc = m.id = 0;
                return;
            }
            m.proc = proc;
            m.id = id;
        }

        void SpawnIt(Model& m, int slot, int fac, int sex, const uint8_t* it,
                     const float* pos, float yaw, uint64_t now)
        {
            char look[0x28] = "";
            m.fac = fac;
            m.sex = sex;
            std::memcpy(m.it, it, CZ_NCAT);
            m.look[0] = 0;
            m.tSpawn = now;
            g.spawnMs = now;
            if (g.tuning.look[fac][0])
                std::snprintf(look, sizeof look, "%s", g.tuning.look[fac]);
            else if (!compose::SpawnLookItems(engine::armor_rig::kUs, fac, sex, it, look))
                return;
            SpawnAs(m, slot, fac, look, pos, yaw, now);
        }

        void Spawn(Model& m, int fac, const float* pos, float yaw, uint64_t now)
        {
            const int sex = Sex(fac) < CZ_NSEX ? Sex(fac) : 0;
            uint8_t it[CZ_NCAT] = {};
            WantItems(fac, sex, it, now);
            SpawnIt(m, fac, fac, sex, it, pos, yaw, now);
        }

        // -------------------------------------------------------------- pose

        int SlotOf(const Model& m)
        {
            bool front = (m.fac & 1) == g.front;
            if (g.swapT0) front = !front;
            return front ? 0 : 1;
        }

        const char* SlotClip(const Model& m)
        {
            if (g.scene < 1 || g.scene > kScenes) return "";
            return g.tuning.spose[g.scene][SlotOf(m)];
        }

        float ActorLoaded(const char* actor)
        {
            const StringId64 sid = ToStringId64(actor);
            uint64_t r = DC::Call<"is-actor-loaded?">(sid);
            if (r == ~0ULL) r = DC::Call<"is-actor-loaded">(sid);
            return static_cast<float>(r & 0xff);
        }

        void PoseData(int sc, uint64_t now)
        {
            if (g.poseState == 0) {
                static const char* const actors[] = {
                    "rogue-mode-poses", "anim-gen-crowds", "crow-t2",
                    "base-male-skel", "base-female-skel"};
                char text[400] = "";
                size_t o = 0;
                for (int i = 0; i < 5; ++i)
                    o += static_cast<size_t>(std::snprintf(
                        text + o, sizeof text - o, "%s%s %.2f", i ? ", " : "",
                        actors[i], ActorLoaded(actors[i])));
                const StringId64 level = ToStringId64(sc == 1 ? "to1-menu-wlf" : "to1-menu-ser");
                (void)DC::Call<"is-level-loaded?">(level);
                if (ActorLoaded("rogue-mode-poses") > 0.0f) {
                    g.poseState = 3;
                    return;
                }
                const uint64_t r = DC::Call<"load-actor">(ToStringId64("rogue-mode-poses"));
                g.poseState = r == ~0ULL ? 4 : 1;
                g.poseT0 = now;
                g.poseAt = now + 500;
                return;
            }
            if (g.poseState != 1 || now < g.poseAt) return;
            g.poseAt = now + 500;
            bool ok = ActorLoaded("rogue-mode-poses") > 0.0f;
            for (int f = 0; f < 2 && !ok; ++f)
                if (Alive(g.m[f]))
                    ok = engine::anim::Len(g.m[f].id,
                             g.tuning.pose[g.m[f].sex == 1 ? 1 : 0][0]) > 0.0f;
            if (ok) {
                g.poseState = 2;
                for (int f = 0; f < 2; ++f) {
                    g.m[f].clipTried = 0;
                    g.m[f].tPose = 0;
                }
            } else if (now - g.poseT0 > 10000) {
                g.poseState = 4;
            }
        }

        void PosePaks(uint64_t now)
        {
            if (now < g.posePaksAt || now - g.openAt > 15000 || g.swapT0) return;
            g.posePaksAt = now + 500;
            for (int f = 0; f < 2; ++f) {
                Model& m = g.m[f];
                const char* sp = Alive(m) && m.clipTried ? SlotClip(m) : "";
                if (sp[0] && std::strcmp(m.clip, sp) != 0
                    && !g.browser.pick[m.sex == 1 ? 1 : 0][0]
                    && engine::anim::Len(m.id, sp) > 0.0f) {
                    m.clipTried = 0;
                    m.tPose = 0;
                }
            }
        }

        void PosePick(Model& m)
        {
            if (m.clipTried || !Alive(m)) return;
            m.clipTried = 1;
            std::memset(m.apo, 0, sizeof m.apo);
            const int sx = m.sex == 1 ? 1 : 0;
            if (g.browser.pick[sx][0]) {
                const float len = engine::anim::Len(m.id, g.browser.pick[sx]);
                if (len > 0.0f) {
                    std::snprintf(m.clip, sizeof m.clip, "%s", g.browser.pick[sx]);
                    m.clipLen = len;
                    return;
                }
            }
            const char* sp = SlotClip(m);
            if (sp[0]) {
                const float len = engine::anim::Len(m.id, sp);
                if (len > 0.0f) {
                    std::snprintf(m.clip, sizeof m.clip, "%s", sp);
                    m.clipLen = len;
                    const int slot = SlotOf(m);
                    if (g.scene >= 1 && g.scene <= kScenes)
                        std::memcpy(m.apo, g.tuning.sapo[g.scene][slot], sizeof m.apo);
                    return;
                }
            }
            const char* fp = g.tuning.fpose[m.fac & 1][sx];
            if (fp[0]) {
                const float len = engine::anim::Len(m.id, fp);
                if (len > 0.0f) {
                    std::snprintf(m.clip, sizeof m.clip, "%s", fp);
                    m.clipLen = len;
                    return;
                }
            }
            for (int k = 0; k < 4 && g.tuning.pose[sx][k][0]; ++k) {
                const float len = engine::anim::Len(m.id, g.tuning.pose[sx][k]);
                if (len > 0.0f) {
                    std::snprintf(m.clip, sizeof m.clip, "%s", g.tuning.pose[sx][k]);
                    m.clipLen = len;
                    return;
                }
            }
            m.clip[0] = 0;
            m.clipLen = 0.0f;
        }

        void PosePlay(Model& m, const float* pos, float yaw, float blend)
        {
            float ap7[7] = {};
            ApReference7(pos, yaw, m.apo, g.tuning.poseYaw * kRad, ap7);
            engine::anim::AnimateAt(m.id, m.clip, blend, ap7);
        }

        void Place(Model& m, const float* pos, float yaw, uint64_t now)
        {
            if (!Alive(m)) return;
            const bool same =
                std::fabs(pos[0] - m.pos[0]) + std::fabs(pos[1] - m.pos[1])
                    + std::fabs(pos[2] - m.pos[2]) < 0.002f
                && std::fabs(yaw - m.yaw) < 0.002f;
            if (m.clip[0]) {
                const bool lng = m.clipLen > 2.0f;
                const uint64_t hold = lng
                    ? static_cast<uint64_t>(m.clipLen * 1000.0f) - 50 : 2000;
                if (same && m.tPose && now - m.tPose < hold) return;
                const float blend = m.tPose && !(lng && same)
                    ? 0.0f : (m.blendNext > 0.0f ? m.blendNext : 0.2f);
                PosePlay(m, pos, yaw, blend);
                m.blendNext = 0.0f;
                m.tPose = now;
                std::memcpy(m.pos, pos, 12);
                m.yaw = yaw;
                return;
            }
            if (same) return;
            const uint64_t bf = MEMORY::ALLOC_BOUND_FRAME(
                Vector3(pos[0], pos[1], pos[2]),
                Quaternion(0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)));
            if (!bf || bf == ~0ULL) return;
            DC::Call<"npc-teleport">(m.id, bf);
            std::memcpy(m.pos, pos, 12);
            m.yaw = yaw;
        }

        // ------------------------------------------------------- model care

        void Unarm(Model& m, uint64_t now)
        {
            if (m.unarmed || !Alive(m) || now - m.tSpawn < kUnarmMs) return;
            DC::Call<"npc-destroy-weapon">(m.id, 0ULL);
            m.unarmed = 1;
        }

        void FaceSet(Model& m, uint64_t now)
        {
            if (!g.tuning.face[0] || g.tuning.face[0] == '-' || !Alive(m)
                || now - m.tSpawn < 150
                || (m.tFace && now - m.tFace < kFaceEveryMs)) return;
            const float secs = kFaceSecs;
            const float fade = m.tFace ? 0.0f : 0.1f;
            const float weight = 1.0f;
            DC::Call<"npc-set-emotion">(m.id, ToStringId64(g.tuning.face),
                                       secs, fade, weight);
            m.tFace = now;
        }

        void Clean(Model& m, uint64_t now)
        {
            if (!Alive(m) || (m.tClean && now - m.tClean < kCleanEveryMs)) return;
            DC::Call<"set-character-mud-mask-value">(m.id, 0.0f);
            DC::Call<"set-character-wet-mask-value">(m.id, 0.0f);
            m.tClean = now;
        }

        void Eyes(Model& m, const float cam[3], uint64_t now)
        {
            if (!g.tuning.eyes || !Alive(m) || now - m.tSpawn < kEyesDelayMs
                || (m.tEyes && now - m.tEyes < kEyesEveryMs)) return;
            m.tEyes = now;
            float d = WrapRadians(std::atan2(cam[0] - m.pos[0], cam[2] - m.pos[2]) - m.yaw);
            const bool want = std::fabs(d) < kEyesDeg * kRad;
            if (want) {
                const uint64_t pt = DC::Call<"alloc-point">(cam[0], cam[1], cam[2], 0.0f);
                if (Valid(pt)) DC::Call<"npc-look-at-point">(m.id, pt, kEyesSecs);
            } else if (m.eyesOn) {
                DC::Call<"npc-clear-look-at">(m.id);
            }
            m.eyesOn = want;
        }

        void Passive(Model& m, uint64_t now, const float cam[3])
        {
            Unarm(m, now);
            FaceSet(m, now);
            Clean(m, now);
            Eyes(m, cam, now);
            if (!Alive(m) || (now - m.tSpawn >= 1500 && Frame() % 60 != 0)) return;
            DC::Call<"npc-set-script-mode">(m.id, 2, 1);
            DC::Call<"npc-disable-perception">(m.id);
            DC::Call<"npc-request-dont-attack">(m.id);
            if (!m.clip[0]) DC::Call<"npc-stop">(m.id);
            if (!m.passive) {
                DC::Call<"npc-disable-character-collider">(m.id);
                m.passive = true;
            }
        }

        void PlayerGuard(uint64_t now)
        {
            float here[3] = {};
            if (!PlayerLoc(here)) return;
            if (!g.plChk) {
                std::memcpy(g.pl0, here, 12);
                g.plChk = now + 1000;
                g.plMoved = false;
            }
            uint64_t best = 0;
            float bd = 1e9f;
            for (int f = 0; f < 2; ++f) {
                if (!Alive(g.m[f])) continue;
                const float dx = g.m[f].pos[0] - here[0], dz = g.m[f].pos[2] - here[2];
                const float d = dx * dx + dz * dz;
                if (d < bd) {
                    bd = d;
                    best = g.m[f].id;
                }
            }
            if (best) {
                DC::Call<"player-ignore-object-collision">(best);
                g.ignore = best;
            }
            if (now >= g.plChk) {
                g.plChk = now + 1000;
                const float dx = here[0] - g.pl0[0], dy = here[1] - g.pl0[1];
                const float dz = here[2] - g.pl0[2];
                if (!g.plMoved && dx * dx + dy * dy + dz * dz > 0.09f) g.plMoved = true;
            }
        }

        void PlayerGuardOff()
        {
            if (!g.ignore) return;
            DC::Call<"player-ignore-object-collision">(0ULL);
            g.ignore = 0;
        }

        // ------------------------------------------------ graveyard / pool

        bool GraveRoom()
        {
            int free = 0;
            for (int i = 0; i < kGraveN; ++i) free += g.grave[i].id == 0;
            return free >= kGraveRoom;
        }

        bool KillPending()
        {
            for (int i = 0; i < kKillQ; ++i)
                if (g.killQ[i]) return true;
            return false;
        }

        bool PoolTake(int fac, int sex, const uint8_t* it, Model& out)
        {
            for (int i = 0; i < kGraveN; ++i) {
                Grave& gv = g.grave[i];
                if (!gv.id) continue;
                const Model& w = gv.m;
                if (w.fac != fac || w.sex != sex || std::memcmp(w.it, it, CZ_NCAT) != 0
                    || !Alive(w)) continue;
                out = w;
                std::memcpy(out.pos, gv.hp, 12);
                gv.id = 0;
                return true;
            }
            return false;
        }

        void Retire(Model& m, uint64_t now)
        {
            if (!m.id) {
                m.proc = m.id = 0;
                m.passive = false;
                return;
            }
            int k = 0;
            for (int i = 0; i < kGraveN; ++i) {
                if (!g.grave[i].id) {
                    k = i;
                    break;
                }
                if (g.grave[i].tSpawn < g.grave[k].tSpawn) k = i;
            }
            if (g.grave[k].id && engine::ptr::Readable(g.grave[k].proc + 0x94, 4)) {
                KillRequest(g.grave[k].id);
                g.killMs = now;
            }
            float hidden[3] = {m.pos[0], m.pos[1] + kHideY + 5.0f * (k + 1), m.pos[2]};
            Place(m, hidden, m.yaw, now);
            Grave& gv = g.grave[k];
            gv.id = m.id;
            gv.proc = m.proc;
            gv.tSpawn = m.tSpawn;
            gv.tPark = now;
            gv.m = m;
            std::memcpy(gv.hp, hidden, 12);
            m.proc = m.id = 0;
            m.passive = false;
        }

        void GraveTick(uint64_t now)
        {
            int parked = 0, old = -1;
            for (int i = 0; i < kGraveN; ++i) {
                if (!g.grave[i].id) continue;
                ++parked;
                if (now - g.grave[i].tSpawn >= kKillAgeMs
                    && (old < 0 || g.grave[i].tPark < g.grave[old].tPark)) old = i;
            }
            const bool go = old >= 0
                && CanRetire(now, g.grave[old].tSpawn, g.spawnMs, g.killMs,
                             KillPending(), parked, g.active);
            for (int i = 0; i < kGraveN; ++i) {
                if (!g.grave[i].id || (go && i == old)) continue;
                Grave& gv = g.grave[i];
                if (!Alive(gv.m)) continue;
                if (!gv.m.clip[0] && !gv.m.clipTried && now - gv.m.tSpawn > kPoseDelayMs)
                    PosePick(gv.m);
                if (gv.m.clip[0])
                    Place(gv.m, gv.hp, gv.m.yaw, now);
                else if (Frame() % kHoldFrames == 0) {
                    const uint64_t bf = MEMORY::ALLOC_BOUND_FRAME(
                        Vector3(gv.hp[0], gv.hp[1], gv.hp[2]),
                        Quaternion(0.0f, std::sin(gv.m.yaw * 0.5f), 0.0f,
                                   std::cos(gv.m.yaw * 0.5f)));
                    if (bf && bf != ~0ULL) DC::Call<"npc-teleport">(gv.m.id, bf);
                }
            }
            if (!go) return;
            Grave& gv = g.grave[old];
            if (engine::ptr::Readable(gv.proc + 0x94, 4)) KillRequest(gv.id);
            gv.id = 0;
            g.killMs = now;
        }

        // --------------------------------------------------------- freecam

        static const char* const kPoseClips[] = {
            "rogue-mode-rattler-factions", "rogue-mode-scar-archer", "rogue-mode-bosses--rattler-king",
            "npc-m-nw-mm-ambi-idle", "npc-m-nw-mm-uneasy-idle", "npc-m-nw-mm-aggr-idle",
            "npc-m-p-mm-aggr-idle", "npc-m-lg-mm-aggr-idle", "npc-m-lg-tilt-mm-aggr-idle",
            "npc-m-1hblade-mm-aggr-idle", "npc-m-1hblade-melee-idle", "npc-mm-ambi-buddy-idle",
            "npc-m-p-mm-injured-idle", "npc-m-nw-mm-panic-idle", "npc-m-p-crouch-idle",
            "npc-m-p-ambi-investigate-idle", "cap-npc-m-wait-arms-folded-loop",
            "cap-npc-m-wait-shoulder-lean-loop", "cap-npc-m-ambi-idle-shoulder-on-wall-loop",
            "cap-npc-m-stand-talking-fw-a-loop", "cap-npc-m-ambi-idle-lean-wall-loop",
            "cap-npc-m-wall-lean-back-loop", "cap-npc-m-share-booze-wait-a",
            "cap-npc-m-share-booze-wait-b", "cap-npc-m-lean-fw-wall-1m-loop",
            "cap-npc-m-explore-wait-lean-1m-loop", "cap-lead-npc-lean-rail-loop",
            "cap-lead-npc-lean-wall-loop", "cap-npc-m-ambi-lean-on-table-0-9m-loop",
            "cap-npc-m-sit-on-1m-idle", "npc-pap-nw-uneasy-idle-look-around-a",
            "npc-pap-nw-uneasy-idle-look-around-b", "npc-pap-nw-uneasy-idle-look-around-c",
            "npc-pap-nw-uneasy-idle-look-around-d", "npc-pap-nw-uneasy-idle-look-around-e",
            "npc-pap-nw-uneasy-idle-look-around-f", "npc-pap-nw-uneasy-idle-look-around-g",
            "npc-pap-nw-uneasy-idle-guard-door-lt-a", "npc-pap-nw-uneasy-idle-guard-door-rt-a",
            "npc-pap-nw-ambi-idle-look-around-small-a", "npc-pap-nw-ambi-idle-look-around-small-b",
            "npc-pap-nw-ambi-idle-look-around-small-c", "npc-pap-nw-ambi-idle-look-around-small-d",
            "npc-pap-nw-ambi-idle-look-around-small-e", "npc-pap-nw-ambi-idle-look-around-small-f",
            "efj-lounging-idle--male-a", "efj-lounging-idle--male-b",
            "efj-lounging-idle--female-a", "npc-standing-ambi-idle",
            "gen-market-seller-01-m", "gen-market-search-lean-down-m", "gen-market-search-02-m",
            "gen-market-search-03-m", "gen-market-search-08-m", "gen-market-search-09-m",
            "gen-market-search-05-f", "gen-market-vendor-01-f", "gen-market-group-01-buyer-m",
            "gen-market-group-03-buyer-m", "gen-market-group-04-buyer-m",
            "gen-market-group-05-buyer-m", "gen-market-group-10-buyer-m",
            "gen-market-group-11-buyer-m", "gen-market-group-01-vendor",
            "gen-market-group-05-vendor", "gen-market-group-07-vendor",
            "gen-market-group-10-vendor", "gen-market-group-01-buyer-f",
            "gen-market-group-03-buyer-f", "gen-market-group-04-buyer-f",
            "gen-market-group-11-buyer-f"};

        const char* PoseName(int i)
        {
            const int n = static_cast<int>(std::size(kPoseClips));
            return i < n ? kPoseClips[i] : g.browser.extra[i - n];
        }

        void PoseExtra()
        {
            g.browser.nextra = 0;
            FILE* f = engine::exe_side::Open("t2r-cz-poses.txt", "r");
            if (!f) return;
            char line[128];
            while (g.browser.nextra < 48 && std::fgets(line, sizeof line, f)) {
                char word[48];
                if (std::sscanf(line, " %47s", word) == 1 && word[0] != '#')
                    std::snprintf(g.browser.extra[g.browser.nextra++], 48, "%s", word);
            }
            std::fclose(f);
        }

        void AppendSaved(const char* text)
        {
            FILE* f = engine::exe_side::Open("t2r-cz-stage-saved.txt", "a");
            if (!f) return;
            std::fputs(text, f);
            std::fclose(f);
        }

        void PoseBrowse(uint16_t pressed, uint64_t now)
        {
            const int dir = Edge(VK_NEXT) || (pressed & kPadRight) ? 1
                : Edge(VK_PRIOR) || (pressed & kPadLeft) ? -1 : 0;
            const bool other = Edge(VK_HOME) || (pressed & kPadX);
            const bool save = Edge(VK_END) || (pressed & kPadY);
            if (other) g.browser.sel = !g.browser.sel;
            Model& m = g.m[g.browser.sel & 1];
            if (!Alive(m)) {
                if (dir || save) Notify("POSE BROWSER: that model is not spawned", 3.0f);
                return;
            }
            const int sx = m.sex == 1 ? 1 : 0;
            const int n = static_cast<int>(std::size(kPoseClips)) + g.browser.nextra;
            if (dir) {
                int i = g.browser.idx[g.browser.sel & 1];
                if (i < 0)
                    for (int k = 0; k < n; ++k)
                        if (std::strcmp(PoseName(k), m.clip) == 0) {
                            i = k;
                            break;
                        }
                int found = -1;
                float len = 0.0f;
                for (int step = 1; step <= n; ++step) {
                    const int base = i < 0 ? (dir > 0 ? -1 : 0) : i;
                    const int k = (base + dir * step + n * 4) % n;
                    const float l = engine::anim::Len(m.id, PoseName(k));
                    if (l > 0.0f) {
                        found = k;
                        len = l;
                        break;
                    }
                }
                if (found < 0) {
                    Notify("POSE BROWSER: no candidate clip is loaded for this body", 4.0f);
                    return;
                }
                int playable = 0, position = 0;
                for (int k = 0; k < n; ++k)
                    if (engine::anim::Len(m.id, PoseName(k)) > 0.0f) {
                        ++playable;
                        if (k <= found) ++position;
                    }
                g.browser.idx[g.browser.sel & 1] = found;
                std::snprintf(m.clip, sizeof m.clip, "%s", PoseName(found));
                m.clipLen = len;
                m.clipTried = 1;
                m.tPose = 0;
                std::snprintf(g.browser.pick[sx], sizeof g.browser.pick[0], "%s", m.clip);
                Place(m, m.pos, m.yaw, now);
                char text[200];
                std::snprintf(text, sizeof text, "%s POSE %d / %d: %s (%.1f s)",
                              FacName(m.fac), position, playable, m.clip, len);
                Notify(text, 6.0f);
            }
            if (save) {
                char text[192];
                std::snprintf(text, sizeof text, "# pose browser: %s model\npose %d %s\n",
                              FacName(m.fac), sx, m.clip[0] ? m.clip : "-");
                AppendSaved(text);
                char msg[160];
                std::snprintf(msg, sizeof msg, "SAVED: pose %d %s (t2r-cz-stage-saved.txt)",
                              sx, m.clip[0] ? m.clip : "-");
                Notify(msg, 4.0f);
            }
        }

        void SetFreecam(bool on, const float cam[3], float viewYawDeg, const char* why)
        {
            if (on == g.fc.on) return;
            g.fc.on = on;
            g.fc.look = false;
            g.fc.rayWait = 0;
            g.fc.lastMs = 0;
            if (on) {
                std::memcpy(g.fc.p, cam, 12);
                g.fc.yaw = WrapDegrees(viewYawDeg);
                g.fc.pitch = g.derived.pitch;
                g.fc.fov = g.derived.fov;
                PoseExtra();
                g.browser.sel = g.front;
                char text[320];
                std::snprintf(text, sizeof text,
                    "FREECAM (%s): WASD / QE fly, hold mouse to look, F9 print, F10 place, PGUP / PGDN poses, HOME other model, END save, F8 exit",
                    why);
                Notify(text, 6.0f);
            }
        }

        void SaveFreecamShot(int sc)
        {
            float ground[4] = {};
            float feetY = g.fc.p[1] - kFrontY;
            if (SpotFor(sc, ground)) feetY = ground[1];
            float spot[4] = {};
            SpotFromCamera(g.fc.p, g.fc.yaw, feetY, spot);
            char text[512], lines[256], msg[200];
            std::snprintf(lines, sizeof lines, "spot %d %.3f %.3f %.3f %.2f\ncam %.1f %.1f\n",
                          sc, spot[0], spot[1], spot[2], spot[3], g.fc.fov, g.fc.pitch);
            std::snprintf(text, sizeof text,
                "# camera %.3f %.3f %.3f yaw %.2f pitch %.2f fov %.2f\n%s",
                g.fc.p[0], g.fc.p[1], g.fc.p[2], g.fc.yaw, g.fc.pitch, g.fc.fov, lines);
            AppendSaved(text);
            std::snprintf(msg, sizeof msg,
                "CAMERA %.2f %.2f %.2f  YAW %.1f  PITCH %.1f  FOV %.0f (saved to t2r-cz-stage-saved.txt)",
                g.fc.p[0], g.fc.p[1], g.fc.p[2], g.fc.yaw, g.fc.pitch, g.fc.fov);
            Notify(msg, 6.0f);
        }

        bool FreecamTick(int sc, const float cam[3], float viewYawDeg, float pose[6], uint64_t now)
        {
            if (!g.fc.chk || now >= g.fc.chk) {
                g.fc.chk = now + 2000;
                const bool allowed = engine::exe_side::Exists("t2r-freecam.txt");
                if (allowed != g.fc.allowed) g.fc.allowed = allowed;
            }
            const uint16_t pad = engine::menu_input::PadButtons();
            const uint16_t pressed = static_cast<uint16_t>(pad & ~g.fc.pbLast);
            g.fc.pbLast = pad;
            bool toggle = Edge(VK_F8) || Edge(VK_SCROLL);
            if ((pad & (kPadL3 | kPadR3)) == (kPadL3 | kPadR3)) {
                if (!g.fc.l3r3) {
                    g.fc.l3r3 = true;
                    toggle = true;
                }
            } else if ((pad & (kPadL3 | kPadR3)) == 0) {
                g.fc.l3r3 = false;
            }
            if (!g.fc.allowed) {
                if (g.fc.on) SetFreecam(false, cam, viewYawDeg, "t2r-freecam.txt removed");
                return false;
            }
            if (g.fc.on && Edge(VK_ESCAPE)) {
                SetFreecam(false, cam, viewYawDeg, "Esc");
                g.fcShield = 1; // let the page latch Esc before it sees the camera return
                return false;
            }
            if (toggle) {
                SetFreecam(!g.fc.on, cam, viewYawDeg, "F8");
                if (!g.fc.on) return false;
            }
            if (!g.fc.on) return false;

            float dt = g.fc.lastMs ? static_cast<float>(now - g.fc.lastMs) / 1000.0f : 0.016f;
            g.fc.lastMs = now;
            dt = std::clamp(dt, 0.001f, 0.1f);
            const float speedKey = Edge(VK_SHIFT) || engine::window::KeyDown(VK_SHIFT) ? 4.0f
                : engine::window::KeyDown(VK_CONTROL) ? 0.15f : 1.0f;

            const bool mouse = engine::window::Focused()
                && ((engine::window::KeyDown(VK_LBUTTON) || engine::window::KeyDown(VK_RBUTTON)));
            POINT cursor{};
            if (mouse && ::GetCursorPos(&cursor)) {
                if (!g.fc.look) {
                    g.fc.look = true;
                    g.fc.anchor = cursor;
                } else {
                    g.fc.yaw -= static_cast<float>(cursor.x - g.fc.anchor.x) * kFreecamLookPx;
                    g.fc.pitch += static_cast<float>(cursor.y - g.fc.anchor.y) * kFreecamLookPx;
                    ::SetCursorPos(g.fc.anchor.x, g.fc.anchor.y);
                }
            } else {
                g.fc.look = false;
            }
            float rsx = 0.0f, rsy = 0.0f, lsx = 0.0f, lsy = 0.0f, lt = 0.0f, rt = 0.0f;
            engine::menu_input::RightStick(rsx, rsy);
            engine::menu_input::LeftStick(lsx, lsy);
            engine::menu_input::Triggers(lt, rt);
            if (rsx * rsx + rsy * rsy < 0.24f * 0.24f) rsx = rsy = 0.0f;
            if (lsx * lsx + lsy * lsy < 0.24f * 0.24f) lsx = lsy = 0.0f;
            g.fc.yaw += (((engine::window::KeyDown(VK_LEFT) ? 1.0f : 0.0f)
                        - (engine::window::KeyDown(VK_RIGHT) ? 1.0f : 0.0f)) * 40.0f * speedKey
                        - rsx * 90.0f) * dt;
            g.fc.pitch += (((engine::window::KeyDown(VK_DOWN) ? 1.0f : 0.0f)
                          - (engine::window::KeyDown(VK_UP) ? 1.0f : 0.0f)) * 40.0f * speedKey
                          - rsy * 60.0f) * dt;
            g.fc.pitch = ClampPitch(g.fc.pitch);
            g.fc.yaw = WrapDegrees(g.fc.yaw);

            if (engine::window::KeyDown('Z')) g.fc.fov = FovZoom(g.fc.fov, 1, dt);
            if (engine::window::KeyDown('X')) g.fc.fov = FovZoom(g.fc.fov, -1, dt);
            const float rtByte = rt * 255.0f, ltByte = lt * 255.0f;
            if (rtByte > 30.0f)
                g.fc.fov = FovZoom(g.fc.fov, 1, dt * (rtByte - 30.0f) / 225.0f);
            if (ltByte > 30.0f)
                g.fc.fov = FovZoom(g.fc.fov, -1, dt * (ltByte - 30.0f) / 225.0f);

            const float forward = (engine::window::KeyDown('W') ? 1.0f : 0.0f)
                - (engine::window::KeyDown('S') ? 1.0f : 0.0f) + lsy;
            const float right = (engine::window::KeyDown('D') ? 1.0f : 0.0f)
                - (engine::window::KeyDown('A') ? 1.0f : 0.0f) + lsx;
            const float up = (engine::window::KeyDown('E') ? 1.0f : 0.0f)
                - (engine::window::KeyDown('Q') ? 1.0f : 0.0f)
                + ((pad & kPadUp) ? 1.0f : 0.0f) - ((pad & kPadDown) ? 1.0f : 0.0f);
            FreecamMove(g.fc.p, g.fc.yaw, g.fc.pitch, forward, right, up,
                        3.0f * speedKey * dt);

            if (Edge(VK_F9) || (pressed & kPadR3)) SaveFreecamShot(sc);

            if (!g.fc.rayWait && (Edge(VK_F10) || (pressed & kPadY))) {
                float spot[4] = {};
                SpotFromCamera(g.fc.p, g.fc.yaw, g.fc.p[1] - kFrontY, spot);
                g.fc.gx = spot[0];
                g.fc.gz = spot[2];
                g.fc.ry1 = g.fc.p[1] - 30.0f;
                static const float starts[3] = {1.0f, 0.1f, -1.2f};
                for (int k = 0; k < 3; ++k) {
                    g.fc.ry0[k] = g.fc.p[1] + starts[k];
                    g.fc.ray[k] = engine::raycast::KickWorldRay(
                        Vector3(g.fc.gx, g.fc.ry0[k], g.fc.gz),
                        Vector3(g.fc.gx, g.fc.ry1, g.fc.gz));
                }
                g.fc.rayWait = 2;
            }
            if (g.fc.rayWait && --g.fc.rayWait == 0) {
                float t = -3.0f;
                float ty[3] = {-1e9f, -1e9f, -1e9f};
                for (int k = 0; k < 3; ++k) {
                    const float tk = engine::raycast::RayT(g.fc.ray[k]);
                    g.fc.ray[k] = 0;
                    ty[k] = tk > 0.002f && tk <= 1.0f
                        ? g.fc.ry0[k] + (g.fc.ry1 - g.fc.ry0[k]) * tk : -1e9f;
                    if (ty[k] > -1e8f && t < 0.0f) t = ty[k];
                }
                // g.fc.ry1 now carries the chosen ground height for the save.
                const float feet = t >= 0.0f ? t : g.fc.p[1] - kFrontY;
                float spot[4] = {};
                SpotFromCamera(g.fc.p, g.fc.yaw, feet, spot);
                float cam[3], front[3], back[3];
                LayoutFromSpot(spot, cam, front, back);
                std::memcpy(g.fc.p, cam, 12);
                g.fc.pitch = g.derived.pitch;
                g.fc.fov = g.derived.fov;
                for (int i = 0; i < 4; ++i) g.tuning.spot[sc][i] = spot[i];
                char spotLine[160], text[256], msg[160];
                std::snprintf(spotLine, sizeof spotLine, "spot %d %.3f %.3f %.3f %.2f\n",
                              sc, spot[0], spot[1], spot[2], spot[3]);
                std::snprintf(text, sizeof text, "# placed (F10)%s\n%s",
                              t >= 0.0f ? "" : " - no ground found under it", spotLine);
                AppendSaved(text);
                std::snprintf(msg, sizeof msg,
                    "CHARACTERS PLACED: SPOT %.2f %.2f %.2f  YAW %.1f%s",
                    spot[0], spot[1], spot[2], spot[3], t >= 0.0f ? "" : "  (NO GROUND FOUND)");
                Notify(msg, 5.0f);
            }
            PoseBrowse(pressed, now);

            pose[0] = g.fc.p[0];
            pose[1] = g.fc.p[1];
            pose[2] = g.fc.p[2];
            pose[3] = g.fc.yaw;
            pose[4] = g.fc.pitch;
            pose[5] = g.fc.fov;
            return true;
        }

        // ------------------------------------------------------------ stage

        void Off(const char* why, bool fade, uint64_t now)
        {
            if (!g.active) return;
            PlayerGuardOff();
            for (int f = 0; f < 2; ++f) {
                Retire(g.m[f], now);
                if (g.nx[f].tSpawn) Retire(g.nx[f], now);
                g.nx[f] = Model{};
            }
            LightsOff();
            g.lspotScene = 0;
            g.plChk = 0;
            g.active = false;
            g.camReady = false;
            g.drag = false;
            if (g.fc.on) SetFreecam(false, g.derived.cam, g.camYawDeg, "stage off");
            if (fade) Fade(0.0f, 0.12f, 0.35f);
            (void)why;
        }

        void ApplyZoom(float pose[6], const float front[3], const float cam[3],
                       uint64_t now)
        {
            static const struct { float y, w, turn; } framing[CZ_NCAT] = {
                {1.70f, 1.25f, 0}, {1.24f, 2.30f, 0}, {0.52f, 2.30f, 0},
                {1.26f, 2.00f, 180.0f}, {1.63f, 1.00f, 0}, {0.90f, 1.80f, 0},
                {1.63f, 1.00f, 0}, {0, 0, 0}};
            const float baseYaw = pose[3], basePitch = pose[4], baseFov = pose[5];
            float tf = baseFov, tp = 0.0f, ty = 0.0f, tm = 0.0f;
            const int cat = EditingCategory();
            if (Zoomed() && cat >= 0 && cat < CZ_NCAT && framing[cat].w > 0
                && baseFov > 1.0f) {
                const float hy = framing[cat].y * (g.m[g.front].sex ? 0.955f : 1.0f);
                const float dx = front[0] - cam[0], dz = front[2] - cam[2];
                const float dy = front[1] + hy - cam[1];
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d > 0.5f) {
                    float feet = WrapDegrees(std::atan2(dx, dz) / kRad - baseYaw);
                    const float part = std::tan(feet * kRad)
                        / std::tan(baseFov * 0.5f * kRad);
                    tf = 2.0f * std::atan(framing[cat].w * 0.5f
                        / std::sqrt(d * d + dy * dy)) / kRad;
                    if (tf > baseFov) tf = baseFov;
                    tf = baseFov * std::pow(tf / baseFov, ZoomUse());
                    if (tf < 3.0f) tf = 3.0f;
                    ty = feet - std::atan(part * std::tan(tf * 0.5f * kRad)) / kRad;
                    tp = -std::atan2(dy, d) / kRad - basePitch;
                    tm = framing[cat].turn;
                }
            }
            float dt = g.zMs ? static_cast<float>(now - g.zMs) : 0.0f;
            if (dt > 100.0f) dt = 100.0f;
            g.zMs = now;
            const float k = 1.0f - std::exp(-dt / 110.0f);
            if (g.zFov < 1.0f) g.zFov = baseFov;
            g.zFov += (tf - g.zFov) * k;
            g.zDp += (tp - g.zDp) * k;
            g.zDy += (ty - g.zDy) * k;
            g.zMy += (tm - g.zMy) * k;
            pose[4] += g.zDp;
            pose[3] += g.zDy;
            pose[5] = g.zFov;
        }

        void WriteCamera(const float pose[6])
        {
            float quat[4] = {};
            CameraQuat(pose[3] * kRad, pose[4] * kRad, quat);
            CAMERA::SET_CAMERA_POSITION(
                Vector3(pose[0], pose[1], pose[2]),
                Quaternion(quat[0], quat[1], quat[2], quat[3]));
            CAMERA::OVERRIDE_FOV(pose[5]);
        }

    } // namespace

    void Install(void* gameModule)
    {
        gModule = reinterpret_cast<uintptr_t>(gameModule);
    }

    void KillDrain() { KillDrainImpl(); }

    void Tick(uint64_t now, bool pageUp, int editFac, int currentSide)
    {
        if (g.fcShield > 0) --g.fcShield;
        if (boot::ExtLoaded()) {
            if (g.active) {
                Off("t2r-ext.dll", false, now);
                g.yieldOff = true;
            }
            return;
        }
        g.yieldOff = false;
        EnsureCompactionHook();

        GraveTick(now);
        ReadFile(now);

        const int sc = SceneFor(currentSide, editFac);
        g.scene = sc;
        if (!pageUp || g.off || !SceneForFactionIsValid(sc)) {
            Off(!pageUp ? "page left" : g.off ? "t2r-no-customize-stage.txt" : "no faction scene",
                !(pageUp && !g.off), now);
            return;
        }
        float spot[4] = {};
        if (!SpotFor(sc, spot)) {
            Off("no stage spot", true, now);
            return;
        }
        Derive(sc);

        if (!g.active) {
            g.active = true;
            g.openAt = now;
            g.camReady = false;
            g.poseState = 0;
            g.posePaksAt = 0;
            g.pvFoc[0] = g.pvFoc[1] = -1;
            g.pvAt[0] = g.pvAt[1] = 0;
            g.front = editFac & 1;
            g.swapT0 = 0;
            g.userYaw = 0.0f;
            g.drag = false;
            g.zFov = g.zDp = g.zDy = g.zMy = 0.0f;
            g.zMs = 0;
            LightLoad();
            const uint64_t showAt = customize::ShowAt();
            if (!(showAt > now)) Fade(0.18f, 0.25f, 0.45f);
        }
        const uint64_t showAt = customize::ShowAt();
        const uint64_t cutDelay = showAt > now ? 0 : kCamReadyMs;
        if (!g.camReady && now - g.openAt >= cutDelay) {
            g.camReady = true;
            Spawn(g.m[g.front], g.front, g.derived.front,
                  g.derived.frontRest, now);
            Spawn(g.m[!g.front], !g.front, g.derived.back,
                  g.derived.backRest, now);
        }
        if (!g.camReady) return;

        PlayerGuard(now);
        float pose[6] = {g.derived.cam[0], g.derived.cam[1], g.derived.cam[2],
                         g.camYawDeg, g.derived.pitch, g.derived.fov};
        if (!FreecamTick(sc, g.derived.cam, g.derived.yaw / kRad, pose, now))
            ApplyZoom(pose, g.derived.front, g.derived.cam, now);
        WriteCamera(pose);

        // An outfit edit respawns that faction's model in its new look. The
        // replacement waits 40 m above its place until it is posed, then takes
        // the place and the standing model goes to the pool.
        for (int f = 0; f < 2; ++f) {
            Model& m = g.m[f];
            Model& nx = g.nx[f];
            const int sex = Sex(f) < CZ_NSEX ? Sex(f) : 0;
            uint8_t want[CZ_NCAT] = {};
            WantItems(f, sex, want, now);
            const Model& cur = nx.tSpawn ? nx : m;
            const bool changed = cur.sex != sex || std::memcmp(cur.it, want, CZ_NCAT) != 0;
            if (m.tSpawn && !g.swapT0 && !nx.tSpawn && Alive(m) && changed
                && PoolTake(f, sex, want, nx)) {
                // a waiting model wears this exact outfit: bring it back down
            } else if (m.tSpawn && !g.swapT0 && !nx.tSpawn
                && CanSpawn(now, g.spawnMs, KillPending(), Frame() - g.killFrame)
                && GraveRoom()
                && (!Alive(m) || now - m.tSpawn >= kMinLifeMs) && changed) {
                float p[3], y = m.yaw;
                std::memcpy(p, m.pos, 12);
                if (!Alive(m)) {
                    Retire(m, now);
                    SpawnIt(m, f, f, sex, want, p, y, now);
                } else {
                    nx = Model{};
                    float hidden[3] = {m.pos[0], m.pos[1] + kHideY, m.pos[2]};
                    SpawnIt(nx, m.slot == f ? 2 + f : f, f, sex, want, hidden, m.yaw, now);
                    if (!nx.id) {
                        nx = Model{};
                        std::memcpy(m.it, want, CZ_NCAT);
                        m.sex = static_cast<int>(sex);
                    }
                }
            }
            if (nx.tSpawn) {
                if (!Alive(nx)) {
                    if (now - nx.tSpawn > 3000) nx = Model{};
                } else {
                    DC::Call<"npc-set-script-mode">(nx.id, 2, 1);
                    DC::Call<"npc-disable-perception">(nx.id);
                    DC::Call<"npc-request-dont-attack">(nx.id);
                    DC::Call<"npc-disable-character-collider">(nx.id);
                    Unarm(nx, now);
                    FaceSet(nx, now);
                    Clean(nx, now);
                    if (!nx.clipTried && now - nx.tSpawn > 60) PosePick(nx);
                    if (nx.clipTried) Place(nx, nx.pos, nx.yaw, now);
                    if (nx.clipTried
                        && (now - nx.tSpawn > 1200
                            || (now - nx.tSpawn > 150
                                && (!nx.clip[0]
                                    || (nx.tPose && now - nx.tPose > 60))))) {
                        float p[3], y = m.yaw;
                        std::memcpy(p, m.pos, 12);
                        Retire(m, now);
                        m = nx;
                        nx = Model{};
                        m.passive = false;
                        Place(m, p, y, now);
                    }
                }
            }
            Passive(m, now, g.derived.cam);
            if (Alive(m) && !m.clipTried && now - m.tSpawn > kPoseDelayMs) PosePick(m);
        }
        if (g.m[0].tSpawn && g.m[1].tSpawn) {
            PoseData(sc, now);
            PosePaks(now);
        }

        // SWITCH FACTION: both walk one circle around their midpoint, clockwise
        // seen from above, while they keep facing the camera.
        if (editFac != g.front && !g.swapT0) {
            g.swapT0 = now;
            Model& incoming = g.m[!g.front];
            if (g.tuning.swapMode && Alive(incoming)) {
                char was[48];
                std::snprintf(was, sizeof was, "%s", incoming.clip);
                incoming.clipTried = 0;
                PosePick(incoming);
                if (incoming.clip[0] && std::strcmp(was, incoming.clip) != 0) {
                    PosePlay(incoming, incoming.pos, incoming.yaw,
                             static_cast<float>(kRiseMs) / 1000.0f);
                    incoming.tPose = now;
                }
            }
        }
        if (g.swapT0) {
            const uint64_t elapsed = now - g.swapT0;
            g.swap = SwapPositions(g.derived.front, g.derived.back, elapsed,
                                  g.tuning.swapMs, g.tuning.swapDir,
                                  g.tuning.swapMode);
            if (elapsed >= static_cast<uint64_t>(g.tuning.swapMs)) {
                g.front = !g.front;
                g.swapT0 = 0;
                g.userYaw = 0.0f;
                if (g.tuning.swapMode) {
                    for (int f = 0; f < 2; ++f) {
                        Model& m = g.m[f];
                        char was[48];
                        std::snprintf(was, sizeof was, "%s", m.clip);
                        m.clipTried = 0;
                        PosePick(m);
                        if (std::strcmp(was, m.clip) != 0) {
                            m.tPose = 0;
                            m.blendNext = kSwapBlendSecs;
                        }
                    }
                }
            } else {
                const uint64_t rise = g.tuning.swapMode ? kRiseMs : 0;
                Place(g.m[g.front], g.swap.front,
                      FaceYaw(g.swap.front, g.derived.cam), now);
                if (elapsed > rise)
                    Place(g.m[!g.front], g.swap.back,
                          FaceYaw(g.swap.back, g.derived.cam), now);
                Lights(sc, g.derived.cam, g.derived.front, g.derived.back);
                return;
            }
        }

        // Rotate the front model: drag with the left button over its hit area,
        // or the pad triggers. The freecam owns its own look keys.
        float cx = 0.0f, cy = 0.0f;
        const bool lb = !g.fc.on && engine::window::CursorCanvas(&cx, &cy)
            && engine::window::KeyDown(VK_LBUTTON);
        if (lb) {
            if (!g.drag && FrontHovered()) {
                g.drag = true;
                g.dragX = cx;
            } else if (g.drag) {
                g.userYaw += (cx - g.dragX) * 0.25f;
                g.dragX = cx;
            }
        } else {
            g.drag = false;
        }
        if (!g.fc.on) {
            float lt = 0.0f, rt = 0.0f;
            if (engine::menu_input::Triggers(lt, rt))
                g.userYaw += (rt - lt) * 3.0f;
        }
        g.userYaw = WrapDegrees(g.userYaw);
        const float frontYaw = g.derived.frontRest
            + (g.userYaw + g.zMy) * kRad;
        Place(g.m[g.front], g.derived.front, frontYaw, now);
        Place(g.m[!g.front], g.derived.back, g.derived.backRest, now);
        Lights(sc, g.derived.cam, g.derived.front, g.derived.back);
    }

    void Reset()
    {
        const uint64_t now = ::GetTickCount64();
        Off("reset", false, now);
        // Reset is a level-reload safety path: do the removals now, never
        // re-queue them into a queue we are about to clear.
        for (int i = 0; i < kGraveN; ++i) {
            if (g.grave[i].id) KillEntity(g.grave[i].id);
            g.grave[i] = Grave{};
        }
        for (int i = 0; i < kKillQ; ++i) {
            if (g.killQ[i]) KillEntity(g.killQ[i]);
            g.killQ[i] = 0;
            g.killAsked[i] = 0;
        }
        for (Model& m : g.m) m = Model{};
        for (Model& nx : g.nx) nx = Model{};
        g.swapT0 = 0;
        g.skipUntil = 0;
        g.fcShield = 0;
        g.fc.on = false;
        g.scene = 0;
        g.derived = core::Derived{};
        g.swap = SwapFrame{};
    }

    bool Active() { return g.active; }
    bool FreecamActive() { return g.fc.on || g.fcShield > 0; }
    bool Off() { return g.off; }

    const Tuning& TuningState() { return g.tuning; }
    const LightSettings& Lights() { return g.lights; }
    const Layout& SceneLayout(int scene)
    {
        return scene >= 1 && scene <= kScenes ? g.ab[scene] : g.ab[0];
    }
    const core::Derived& Derived() { return g.derived; }
    const SwapFrame& Swap() { return g.swap; }
    int FrontFaction() { return g.front; }

} // namespace net::ui::customize::stage
