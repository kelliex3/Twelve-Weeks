// ported from tools/ext/denynote.c

#include <winsock2.h>
#include <windows.h>

#include "denynote.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "net-common.hpp"
#include "net-gui2.hpp"
#include "notify.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::denynote {

    namespace {

        enum {
            kGrp, kPanel, kBar, kRing, kSlash, kText, kN
        };
        constexpr const char* const kSuffix[kN] = { "", ".panel", ".bar", ".ring", ".slash", ".text" };

        // ---- geometry (denynote.c DN_*) -----------------------------------------
        constexpr float kNoteW  = 1100.0f;
        constexpr float kBlobW0 = 1000.0f;
        constexpr float kBlobH0 = 400.0f;
        constexpr float kPkCy   = 1010.0f;
        constexpr float kX      = 1370.0f;
        constexpr float kTopPk  = 270.0f;   // (unused: the picker notice is centred now)
        constexpr float kTopHud = 300.0f;
        constexpr float kMaxH   = 270.0f;   // (unused: kept for parity with the ext)
        constexpr float kIcon   = 96.0f;
        constexpr float kIconX  = 56.0f;
        constexpr float kTextX  = 190.0f;
        constexpr float kTextH  = 250.0f;
        constexpr float kLineH  = 60.0f;
        constexpr float kPadV   = 30.0f;
        constexpr float kMinH   = 130.0f;
        constexpr float kWrapW  = 790.0f;
        constexpr float kPt     = 36.0f;
        constexpr int   kMaxL   = 4;

        // wm, wa, hm, ha (units), dx, dy, peak -- dense + wide: the list rows must not read through
        constexpr float kBlobs[3][7] = {
            { 1.55f, 380.0f, 2.60f, 200.0f,  0.00f,  0.00f, 1.00f },
            { 1.20f, 240.0f, 2.30f, 140.0f, -0.10f,  0.04f, 1.00f },
            { 1.00f, 200.0f, 2.00f, 110.0f,  0.12f, -0.03f, 1.00f },
        };

        struct State {
            int      st = 0;
            int      hud = 0;
            int      lines = 0;
            uint64_t t0 = 0;
            uint64_t until = 0;
            uint64_t setFrame = 0;
            uint64_t key = 0;
            char     text[220] = {};
            float    la = 0.0f;
        };
        State g_dn;

        bool PageOk(uint64_t tp)
        {
            static uint64_t p[5];
            if (!p[0]) {
                p[0] = engine::widget::PageId("picker-skill");
                p[1] = engine::widget::PageId("picker-primary");
                p[2] = engine::widget::PageId("picker-secondary");
                p[3] = engine::widget::PageId("picker-purchase");
                p[4] = engine::widget::PageId("gearup13");
            }
            for (int i = 0; i < 5; i++)
                if (p[i] && tp == p[i])
                    return true;
            return false;
        }

        // the notice widgets reachable right now: 1 = picker page, 2 = match HUD, 0 = none
        int Resolve(uint64_t w[kN], uint64_t* key)
        {
            char p[64];
            const uint64_t tp = engine::widget::TopPage();
            if (tp && PageOk(tp)) {
                const uint64_t rw = engine::widget::Page("anim-group");
                if (!rw)
                    return 0;
                for (int i = 0; i < kN; i++) {
                    snprintf(p, sizeof p, "anim-group.denyNote%s", kSuffix[i]);
                    w[i] = engine::widget::Page(p);
                    if (!w[i])
                        return 0;
                }
                *key = tp ^ rw * 0x9E3779B97F4A7C15ULL ^ 1;
                return 1;
            }
            if (net::gui2::InMatch()) {
                for (int i = 0; i < kN; i++) {
                    snprintf(p, sizeof p, "t2rDenyNote%s", kSuffix[i]);
                    w[i] = engine::widget::Hud(p);
                    if (!w[i])
                        return 0;
                }
                *key = w[kGrp] ^ 2;
                return 2;
            }
            return 0;
        }

        // alpha goes to every child (a group alpha alone is not known to reach its children)
        void Alpha(const uint64_t* w, float a)
        {
            engine::widget::Alpha(w[kPanel], a * kBlobs[0][6]);
            engine::widget::Alpha(w[kBar],   a * kBlobs[1][6]);
            engine::widget::Alpha(w[kSlash], a * kBlobs[2][6]);
            engine::widget::Alpha(w[kRing],  a);
            engine::widget::Alpha(w[kText],  a);
        }

    }

    bool Show(const char* msg, float secs)
    {
        if (!msg || !msg[0])
            return false;
        int dnk = notify::KindOf(msg);
        if (dnk == notify::NTF_INFO)
            dnk = notify::NTF_LOADOUT;   // a refusal that matches no keyword is a loadout / limit one
        {
            const uint64_t tp = engine::widget::TopPage();
            if (!(tp && PageOk(tp)) && notify::Push(dnk, msg, secs))
                return true;
        }   // match HUD: the styled notification; pickers keep this panel

        uint64_t w[kN], key = 0;
        const int where = Resolve(w, &key);
        if (!where) {
            TO1_LOG("[DENYNOTE] no notice widgets here (page 0x%llx) - plain message\n",
                static_cast<unsigned long long>(engine::widget::TopPage()));
            return false;
        }
        const uint64_t now = GetTickCount64();
        const char* tr = engine::locale::Translate(msg);
        if (!tr)
            tr = msg;
        if (g_dn.st && g_dn.key == key && !strcmp(g_dn.text, tr) && now + 500 < g_dn.until)
            return true;            // already up
        if (g_dn.st && g_dn.key != key)
            g_dn.st = 0;

        char buf[320];
        const int lines = engine::widget::Wrap(tr, buf, sizeof buf, kWrapW, kPt / 30.0f, kMaxL);
        float H = static_cast<float>(lines) * kLineH + 2.0f * kPadV;
        if (H < kMinH)
            H = kMinH;

        engine::widget::Visible(w[kGrp], false);
        Alpha(w, 0.0f);
        engine::widget::SetText(w[kText], buf);
        engine::widget::Pivot(w[kGrp], kX, where == 1 ? kPkCy - H * 0.5f : kTopHud);
        {
            const int bw[3] = { kPanel, kBar, kSlash };   // the smudge (group units, centred on the box)
            for (int i = 0; i < 3; i++) {
                const float* b = kBlobs[i];
                const float wu = kNoteW * b[0] + b[1];
                const float hu = H * b[2] + b[3];
                engine::widget::Scale(w[bw[i]], wu / kBlobW0, hu / kBlobH0);
                engine::widget::Pivot(w[bw[i]],
                    kNoteW * 0.5f + b[4] * kNoteW - wu * 0.5f,
                    H * 0.5f + b[5] * H - hu * 0.5f);
            }
        }
        engine::widget::SetImage(w[kRing], notify::IconPath(dnk));   // written while hidden
        engine::widget::Pivot(w[kRing], kIconX, (H - kIcon) * 0.5f);
        engine::widget::Pivot(w[kText], kTextX, (H - kTextH) * 0.5f);

        g_dn.st = 1;
        g_dn.hud = where == 2;
        g_dn.key = key;
        g_dn.lines = lines;
        g_dn.setFrame = engine::frame::Count();
        g_dn.la = -1.0f;
        g_dn.until = now + static_cast<uint64_t>((secs > 0 && secs < 30 ? secs : 5) * 1000.0f);
        snprintf(g_dn.text, sizeof g_dn.text, "%s", tr);
        TO1_LOG("[DENYNOTE] \"%.100s\" %.1f s, %d line(s), panel %.0f high (%s)\n",
            tr, secs, lines, H, where == 1 ? "picker page" : "match HUD");
        return true;
    }

    void Tick()
    {
        if (!g_dn.st)
            return;
        uint64_t w[kN], key = 0;
        const uint64_t now = GetTickCount64();
        if (!Resolve(w, &key) || key != g_dn.key) {
            g_dn.st = 0;
            return;
        }   // page popped / rebuilt / match over: widgets are gone
        if (g_dn.st == 1) {
            if (engine::frame::Count() <= g_dn.setFrame)
                return;             // the string has had no frame yet
            engine::widget::Visible(w[kGrp], true);
            g_dn.st = 2;
            g_dn.t0 = now;
        }
        float a = 1.0f;
        if (now - g_dn.t0 < 150)
            a = 0.1f + 0.9f * static_cast<float>(now - g_dn.t0) / 150.0f;
        if (now > g_dn.until) {
            const float o = 1.0f - static_cast<float>(now - g_dn.until) / 300.0f;
            if (o < a)
                a = o;
        }
        if (a <= 0.0f) {
            Alpha(w, 0.0f);
            engine::widget::Visible(w[kGrp], false);
            g_dn.st = 0;
            return;
        }
        if (fabsf(a - g_dn.la) > 0.01f) {
            Alpha(w, a);
            g_dn.la = a;
        }
    }

    void Reset()
    {
        g_dn = State{};
    }

}
