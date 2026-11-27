// Native owner of tools/ext/titlebg.c. The ext drove the render camera through
// an icam filter hook; here we write the camera directly each frame and use the
// write count as the boot-cover readiness signal. Pure pose / sway / boot /
// layer parsing rules live in titlebg-core.hpp.
#include "titlebg.hpp"
#include "titlebg-core.hpp"

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/pcm.hpp"
#include "log.hpp"          // TO1_LOG before script.hpp
#include "engine/script.hpp"
#include "engine/widget.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace menu::titlebg {

    namespace {

        namespace core = menu::titlebg::core;

        struct Runtime {
            bool active = false;
            uint64_t t0 = 0, fileAt = 0, rsFileAt = 0, rearmAt = 0;
            uint32_t writes = 0;
            bool fileOk = false;
            float fp[6]{};
            std::array<core::Layer, core::kMaxLayers> layers{};
            int nLayers = 0;
            core::Boot boot;
        };

        Runtime g;

        bool Disabled()
        {
            // t2r-menu-boat.txt is the ext's opt-out: the game's own boat camera.
            return boot::ExtLoaded()
                || engine::flag::Present("t2r-menu-boat.txt");
        }

        void UiAlpha(float a)
        {
            engine::widget::Alpha(
                engine::widget::PageOnStack("anim-group", MenuStack::main), a);
        }

        void BlackCover(float fadeS)
        {
            DC::Call<"fade-to-black">(0.0f, 0.0f, fadeS, 0.0f);
        }

        void ReadPose(uint64_t now)
        {
            if (now < g.fileAt) return;
            g.fileAt = now + 500;
            FILE* f = engine::exe_side::Open("t2r-title-cam.txt", "r");
            if (!f) {
                g.fileOk = false;
                return;
            }
            char line[192] = {};
            const bool haveLine = std::fgets(line, sizeof line, f) != nullptr;
            std::fclose(f);
            float v[6] = {};
            if (!haveLine || !core::ParsePose(line, v)) {
                g.fileOk = false;
                return;
            }
            if (!g.fileOk || std::memcmp(v, g.fp, sizeof v)) {
                std::memcpy(g.fp, v, sizeof v);
                TO1_LOG("title bg: t2r-title-cam.txt -> %.2f %.2f %.2f yaw %.1f pitch %.1f fov %.1f\n",
                        v[0], v[1], v[2], v[3], v[4], v[5]);
            }
            g.fileOk = true;
        }

        void ReadLayers(uint64_t now)
        {
            if (now < g.rsFileAt) return;
            g.rsFileAt = now + 2000;
            int n = 0;
            FILE* f = engine::exe_side::Open("t2r-title-rs.txt", "r");
            if (f) {
                char line[160];
                while (n < core::kMaxLayers && std::fgets(line, sizeof line, f)) {
                    core::Layer layer;
                    if (core::ParseLayer(line, layer))
                        g.layers[n++] = layer;
                }
                std::fclose(f);
            }
            if (!n) {
                core::DefaultLayer(g.layers[0]);
                n = 1;
            }
            g.nLayers = n;
            TO1_LOG("title bg: render layer %s prio %d strength %.2f\n",
                    g.layers[0].name, g.layers[0].prio, g.layers[0].str);
        }

        bool ApplyLayers()
        {
            bool firstOk = false;
            for (int i = 0; i < g.nLayers; ++i) {
                const core::Layer& l = g.layers[i];
                const uint64_t r = DC::Call<"override-render-settings">(
                    SID(l.name), 0.0f, 0.0f, 0, l.prio, 0, 1, 0, l.str);
                if (i == 0 && r && r != ~uint64_t{0}) firstOk = true;
            }
            return firstOk;
        }

        void ApplyCamera(uint64_t now)
        {
            const float* b = g.fileOk ? g.fp : core::kPose.data();
            const core::Pose pose = core::FramePose(now, g.t0, b);
            float q[4];
            core::Quat(pose.yaw, pose.pitch, q);
            CAMERA::SET_CAMERA_POSITION(
                Vector3(pose.p[0], pose.p[1], pose.p[2]),
                Quaternion(q[0], q[1], q[2], q[3]));
            CAMERA::OVERRIDE_FOV(pose.fov);
            ++g.writes;
        }

    } // namespace

    void Tick(uint64_t now)
    {
        if (Disabled()) {
            if (g.boot.state != 0) Reset();
            return;
        }

        const uint64_t frame = engine::frame::Count();
        const int sub = engine::lobby::Substate();
        const bool titleUp = sub == 0
            && engine::widget::TopPage(MenuStack::main) != 0;
        const bool baseOk = frame > 5 && sub == 0;

        if (g.boot.state == 0 && core::BootCanStart(frame, sub)) {
            core::BootBegin(g.boot, now, g.writes);
            HUD::FADE_TO_BLACK(0.0f, 1.0f);
            TO1_LOG("title bg: boot black hold from frame %llu (writes %u)\n",
                    static_cast<unsigned long long>(frame), g.writes);
        }
        if (g.boot.state == 1 && (!baseOk || sub != 0)) {
            g.boot.state = 2;      // released without a fade-up
            g.boot.fadeAt = 0;
            BlackCover(0.3f);
            UiAlpha(1.0f);
        }

        const bool up = baseOk && (g.boot.state == 1
                                   || (frame > 60 && titleUp));
        if (!up) {
            if (g.active) {
                g.active = false;
                UiAlpha(1.0f);
                TO1_LOG("title bg: off (substate %d)\n", sub);
            }
            return;
        }
        if (!g.active) {
            g.active = true;
            g.t0 = now;
            g.fileAt = 0;
            g.rsFileAt = 0;
            TO1_LOG("title bg: on (title stack, substate 0) - Hillcrest overlook\n");
        }

        ReadPose(now);
        ReadLayers(now);
        ApplyCamera(now);
        const bool rsHandle = ApplyLayers();

        if (g.boot.state == 1) {
            if (core::BootTick(g.boot, now, g.writes, rsHandle, titleUp)) {
                BlackCover(core::kBootFadeS);
                TO1_LOG("title bg: boot FADE UP at frame %llu after %llu ms (writes %u)\n",
                        static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(now - g.boot.t0), g.writes);
            }
            if (titleUp) UiAlpha(0.0f);   // buttons above the native black
            if (g.boot.state == 1 && now - g.rearmAt >= 500) {
                g.rearmAt = now;
                HUD::FADE_TO_BLACK(0.0f, 1.0f);
            }
        } else {
            UiAlpha(core::ButtonAlpha(now, g.boot));
        }
    }

    void Reset()
    {
        if (g.boot.state == 1) BlackCover(0.3f);
        UiAlpha(1.0f);
        g = Runtime{};
        engine::pcm::SetSessionVolume(1.0f);
    }

    uint32_t CameraWrites()
    {
        return g.writes;
    }

} // namespace menu::titlebg
