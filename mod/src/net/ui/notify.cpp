// ported from tools/ext/notify.c

#include <winsock2.h>
#include <windows.h>

#include "notify.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)
#include "engine/draw.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "net-common.hpp"
#include "net-gui2.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace net::notify {

    namespace {

        // ---- geometry (notify.c NTF_*) ------------------------------------------
        constexpr float kBlobW0 = 1000.0f;      // unscaled blob widget size, canvas units
        constexpr float kBlobH0 = 400.0f;
        constexpr float kTxtPx  = 18.0f;        // text line height in the 1280x720 text space
        constexpr float kPitch  = 23.0f;        // two-line pitch
        constexpr float kPadX   = 26.0f;
        constexpr float kPadY   = 14.0f;
        constexpr float kIconPx = 28.0f;
        constexpr float kTopY   = 105.0f;       // vertical centre in the text space
        constexpr float kGap    = 10.0f;
        constexpr float kMaxW   = 430.0f;       // widest line before wrapping
        constexpr float kHardW  = 760.0f;       // a line wider than this shrinks the whole text
        constexpr int   kQN     = 6;

        // the 2013 centre banner: a long thin bar with the toast's own background
        constexpr float    kBanPx   = 26.5f;
        constexpr float    kBanCy   = 198.7f;
        constexpr float    kBanPadX = 34.0f;
        constexpr float    kBanPadY = 4.5f;
        constexpr uint32_t kBanCol  = 0x00DCE6EAu;
        constexpr uint32_t kTxtCol  = 0x00DCE6EAu;
        constexpr float    kMenuCy  = 95.0f;    // menu-page toast centre (text px)

        // The smudge: per blob { width = box w * wm + wa, height = box h * hm + ha,
        // centre offset dx * box w / dy * box h, peak alpha }. Same numbers as
        // notify_preview.py NT_BLOBS.
        constexpr float kBlobs[3][7] = {
            { 1.18f, 70.0f, 1.45f, 16.0f,  0.00f,  0.00f, 0.60f },
            { 0.78f, 36.0f, 1.30f, 10.0f, -0.20f,  0.07f, 0.36f },
            { 0.64f, 24.0f, 1.20f,  8.0f,  0.24f, -0.06f, 0.32f },
        };
        // the banner's bar = three flat overlapping pieces of the same blob
        constexpr float kBanBlobs[3][7] = {
            { 0.62f, 60.0f, 0.0f, 34.0f, -0.30f, -0.08f, 0.62f },
            { 0.62f, 60.0f, 0.0f, 34.0f,  0.00f, -0.08f, 0.62f },
            { 0.62f, 60.0f, 0.0f, 34.0f,  0.30f, -0.08f, 0.62f },
        };

        // the game's own notification badges where it has one, our thin ring copies otherwise
        const char* const kIcons[NTF_N] = {
            "streaming/t2r/icons/notify-info.png",      // INFO
            "streaming/t2r/icons/notify-heal.png",      // HEAL
            "streaming/t2r/icons/notify-revive.png",    // REVIVE
            "streaming/t2r/icons/notify-warn.png",      // WARN
            "streaming/t2r/icons/notify-info.png",      // SERVER
            "streaming/t2r/icons/notify-loadout.png",   // LOADOUT
            "streaming/t2r/icons/notify-covert.png",    // COVERT
            "streaming/t2r/icons/notify-move.png",      // MOVE
            "streaming/t2r/icons/notify-craft.png",     // CRAFT
            "streaming/t2r/icons/notify-blade.png",     // BLADE
            "streaming/t2r/icons/notify-bomb.png",      // BOMB
            "streaming/t2r/icons/notify-supply.png",    // SUPPLY
        };
        const char* const kKindNames[NTF_N] = {
            "info", "heal", "revive", "warn", "server", "loadout",
            "covert", "move", "craft", "blade", "bomb", "supply",
        };

        // ---- queue -------------------------------------------------------------
        struct Item {
            int      kind = 0;
            float    secs = 0.0f;
            char     text[200] = {};
            uint64_t tEnq = 0;
            int      banner = 0;
        };
        Item         g_q[kQN];
        int          g_qn = 0;
        std::mutex   g_lock;            // the ext's spinlock (Push may come off-thread)

        // ---- widgets + live state ---------------------------------------------
        uint64_t g_w[4] = {};           // t2rNtL, t2rNtM, t2rNtR, t2rNtIc
        bool     g_ok = false;          // widgets resolved for this match

        struct State {
            int      st = 0;            // 0 idle, 1 written (hidden, waits a frame), 2 shown
            int      kind = 0;
            int      nl = 0;
            int      banner = 0;
            char     text[200] = {};
            char     line[2][160] = {};
            float    lw[2] = {};
            float    tbw = 0.0f;
            float    size = 0.0f;
            float    th = 0.0f;
            float    innerw = 0.0f;
            float    boxh = 0.0f;
            float    cy = 0.0f;
            float    a = 0.0f;
            float    la = 0.0f;
            uint64_t t0 = 0;
            uint64_t until = 0;
            uint64_t setFrame = 0;
            uint64_t key = 0;
            int      menu = 0;
            Item     it = {};
        };
        State g_ntf;

        // case-insensitive substring (ext NtfHas)
        bool Has(const char* s, const char* w)
        {
            if (!s || !w)
                return false;
            const size_t n = strlen(w);
            for (; *s; s++) {
                size_t i = 0;
                while (i < n && s[i] &&
                       tolower(static_cast<unsigned char>(s[i])) ==
                       tolower(static_cast<unsigned char>(w[i])))
                    i++;
                if (i == n)
                    return true;
            }
            return false;
        }

        // The ext's MsgIsMove / MsgIsKick: the admin team move and the kick text are shown
        // like a server notice and carry over to the menu when the match ends. The Polish
        // "DRU\xC5\xBBYNY: " prefix is copied from the ext byte-for-byte (its source encoded
        // it as the mojibake C3 85 C2 BB; kept so the two behave identically).
        bool IsKick(const char* t)
        {
            if (!t)
                return false;
            return !strncmp(t, "You got kicked from the match", 29)
                || !strncmp(t, "Wyrzucono ci", 12)
                || !strncmp(t, "TEAMS: ", 7)
                || !strncmp(t, "DRU\xC3\x85\xC2\xBBYNY: ", 10);
        }

        // The text measurement / draw the ext borrowed from winmm. The ext punned the
        // engine's u64 (low dword width, high dword height) into a float; here the mod's
        // DebugDrawCommon returns a real vec2.
        float Measure(const char* s, float size)
        {
            if (!s || !s[0])
                return 0.0f;
            const vec2 d = DebugDrawCommon::GetTextWidthHeight(s, size);
            return d.x > 0.0f && d.x < 4000.0f ? d.x : 0.0f;
        }

        float LineHeight()
        {
            static float lineh = 0.0f;
            if (!lineh) {
                const vec2 d = DebugDrawCommon::GetTextWidthHeight("Mg", 2.0f);
                lineh = d.y > 1.0f && d.y < 400.0f ? d.y / 2.0f : 40.0f;
            }
            return lineh;
        }

        bool Resolve()
        {
            static uint64_t lastTry = 0;
            const uint64_t now = GetTickCount64();
            if (lastTry && now - lastTry < 1000)
                return false;
            lastTry = now;
            static const char* const N[4] = { "t2rNtL", "t2rNtM", "t2rNtR", "t2rNtIc" };
            for (int i = 0; i < 4; i++) {
                g_w[i] = engine::widget::Hud(N[i]);
                if (!g_w[i]) {
                    TO1_LOG("[NOTIFY] %s not in hud-root (older gui2-widgets.pak) - old notification paths stay\n", N[i]);
                    return false;
                }
            }
            g_ok = true;
            TO1_LOG("[NOTIFY] widgets found - styled notifications on\n");
            return true;
        }

        // ext NtfPush2: queue one item (dup-dropped, banners jump ahead of toasts).
        bool Push2(int kind, const char* text, float secs, int banner)
        {
            if (!text || !text[0])
                return false;
            if (engine::flag::Present("t2r-old-notify.txt", 120))
                return false;
            // in the match HUD every kind; on the menu pages server notices only
            if (net::gui2::InMatch() ? !g_ok : kind != NTF_SERVER)
                return false;
            if (kind < 0 || kind >= NTF_N)
                kind = KindOf(text);

            std::lock_guard<std::mutex> lk(g_lock);
            const uint64_t now = GetTickCount64();
            bool dup = g_ntf.st && !strncmp(g_ntf.text, text, sizeof g_ntf.text - 1)
                && now + 500 < g_ntf.until;
            for (int i = 0; i < g_qn && !dup; i++)
                dup = !strncmp(g_q[i].text, text, sizeof g_q[i].text - 1);
            if (!dup) {
                if (g_qn >= kQN) {          // full: the oldest waiting one gives way
                    memmove(g_q, g_q + 1, sizeof g_q[0] * (kQN - 1));
                    g_qn = kQN - 1;
                }
                int at = g_qn;
                if (banner) {               // banners go before the toasts, in order
                    at = 0;
                    while (at < g_qn && g_q[at].banner)
                        at++;
                    memmove(g_q + at + 1, g_q + at, sizeof g_q[0] * static_cast<size_t>(g_qn - at));
                }
                g_qn++;
                Item* q = &g_q[at];
                q->kind = kind;
                q->secs = secs;
                q->tEnq = now;
                q->banner = banner;
                snprintf(q->text, sizeof q->text, "%s", text);
                TO1_LOG("[NOTIFY] %s %s \"%.120s\" %.1f s\n",
                    banner ? "BANNER" : "kind", kKindNames[kind], text, secs);
            }
            return true;
        }

        // ext NtfStart: lay the toast out on the hud-root widgets.
        void Start(const Item* it, uint64_t now)
        {
            const float lineh = LineHeight();
            const float px = it->banner ? kBanPx : kTxtPx;
            float size = px / lineh;
            const char* tr = engine::locale::TranslateDraw(it->text);
            char buf[200];
            snprintf(buf, sizeof buf, "%s", tr ? tr : it->text);

            // one line when it fits kMaxW, else the most balanced split into 2 lines
            char l0[160] = "", l1[160] = "";
            int nl = 1;
            snprintf(l0, sizeof l0, "%s", buf);
            if (!it->banner && Measure(l0, size) > kMaxW) {   // a banner is ONE long line
                char* sp[48];
                int ns = 0;
                for (char* q = buf; *q && ns < 48; q++)
                    if (*q == ' ' && q > buf && q[-1] != ' ')
                        sp[ns++] = q;
                float best = 1e9f;
                int bk = -1;
                for (int k = 0; k < ns; k++) {
                    char c0[160], c1[160];
                    size_t n0 = static_cast<size_t>(sp[k] - buf);
                    if (n0 > sizeof c0 - 1)
                        n0 = sizeof c0 - 1;
                    memcpy(c0, buf, n0);
                    c0[n0] = 0;
                    snprintf(c1, sizeof c1, "%s", sp[k] + 1);
                    const float m0 = Measure(c0, size), m1 = Measure(c1, size);
                    const float mm = m0 > m1 ? m0 : m1;
                    if (mm < best) {
                        best = mm;
                        bk = k;
                    }
                }
                if (bk >= 0) {
                    size_t n0 = static_cast<size_t>(sp[bk] - buf);
                    if (n0 > sizeof l0 - 1)
                        n0 = sizeof l0 - 1;
                    memcpy(l0, buf, n0);
                    l0[n0] = 0;
                    snprintf(l1, sizeof l1, "%s", sp[bk] + 1);
                    nl = 2;
                }
            }
            float w0 = Measure(l0, size), w1 = Measure(l1, size);
            float mx = w0 > w1 ? w0 : w1;
            if (mx > kHardW) {              // one huge word: shrink, never run off the screen
                size *= kHardW / mx;
                w0 *= kHardW / mx;
                w1 *= kHardW / mx;
                mx = kHardW;
            }
            memcpy(g_ntf.line[0], l0, sizeof l0);
            memcpy(g_ntf.line[1], l1, sizeof l1);
            g_ntf.nl = nl;
            g_ntf.lw[0] = w0;
            g_ntf.lw[1] = w1;
            g_ntf.tbw = mx;
            g_ntf.size = size;
            g_ntf.th = px * size / (px / lineh);    // = the nominal px unless shrunk
            g_ntf.banner = it->banner;
            g_ntf.kind = it->kind;
            g_ntf.menu = 0;
            g_ntf.it = *it;
            snprintf(g_ntf.text, sizeof g_ntf.text, "%s", it->text);

            const float icw = it->banner ? 0.0f : kIconPx + kGap;
            const float innerw = mx + icw;
            float innerh = static_cast<float>(nl - 1) * kPitch + g_ntf.th;
            if (innerh < kIconPx && !it->banner)
                innerh = kIconPx;
            const float boxw = innerw + 2.0f * (it->banner ? kBanPadX : kPadX);
            const float boxh = innerh + 2.0f * (it->banner ? kBanPadY : kPadY);
            g_ntf.innerw = innerw;
            g_ntf.boxh = boxh;
            const float cy = it->banner ? kBanCy : kTopY;
            g_ntf.cy = cy;

            uint64_t* w = g_w;
            for (int i = 0; i < 4; i++)
                engine::widget::Visible(w[i], false);
            for (int i = 0; i < 4; i++)
                engine::widget::Alpha(w[i], 0.0f);
            for (int i = 0; i < 3; i++) {
                const float* b = it->banner ? kBanBlobs[i] : kBlobs[i];
                const float wp = (boxw * b[0] + b[1]) * 3.0f;
                const float hp = (boxh * b[2] + b[3]) * 3.0f;
                engine::widget::Scale(w[i], wp / kBlobW0, hp / kBlobH0);
                engine::widget::Pivot(w[i], b[4] * boxw * 3.0f - wp * 0.5f,
                    (cy + b[5] * boxh) * 3.0f - hp * 0.5f);
            }
            engine::widget::SetImage(w[3], kIcons[it->kind]);
            engine::widget::Pivot(w[3], -innerw * 1.5f, (cy - kIconPx * 0.5f) * 3.0f);
            if (it->banner)
                engine::widget::SetImage(w[3], kIcons[NTF_INFO]);   // stays at alpha 0

            g_ntf.st = 1;
            g_ntf.setFrame = engine::frame::Count();
            g_ntf.la = -1.0f;
            g_ntf.a = 0.0f;
            const float secs = it->secs;
            g_ntf.until = now + static_cast<uint64_t>(
                (secs > 0 && secs < (it->kind == NTF_SERVER ? 600.0f : 30.0f) ? secs : 5.0f) * 1000.0f);
            TO1_LOG("[NOTIFY] show %s \"%.100s\" %d line(s) box %.0f x %.0f px at y %.1f, %.1f s\n",
                kKindNames[it->kind], g_ntf.text, nl, boxw, boxh, cy,
                static_cast<double>(g_ntf.until - now) / 1000.0);
        }

        // the toast's sound: the game's own "journal entry added" chime, falling back to
        // the stock menu tick where that event is not loaded
        void Sound()
        {
            static const char* const S[2] = { "sfx-tutorial-journal-entry-added", "sfx-cycle" };
            for (int i = 0; i < 2; i++) {
                const uint64_t r = DC::Call<"play-sound">(SID(S[i]), 0);
                if (r != 0 && r != ~0ULL) {
                    TO1_LOG("[NOTIFY] sound %s -> %llx\n", S[i], static_cast<unsigned long long>(r));
                    return;
                }
                TO1_LOG("[NOTIFY] sound %s not available (%llx)\n", S[i], static_cast<unsigned long long>(r));
            }
        }

        // menu pages: the same widgets under each page's anim-group; key = page + its
        // anim-group handle (a rebuilt page = new key)
        bool MenuResolve(uint64_t w[5], uint64_t* key)
        {
            static const char* const N[5] = {
                "anim-group.t2rNfL", "anim-group.t2rNfM", "anim-group.t2rNfR",
                "anim-group.t2rNfIc", "anim-group.t2rNfTx",
            };
            const uint64_t tp = engine::widget::TopPage();
            if (!tp)
                return false;
            for (int i = 0; i < 5; i++) {
                w[i] = engine::widget::Page(N[i]);
                if (!w[i])
                    return false;
            }
            *key = tp ^ engine::widget::Page("anim-group") * 0x9E3779B97F4A7C15ULL ^ 3;
            return true;
        }

        void StartMenu(const Item* it, uint64_t now, uint64_t* w, uint64_t key)
        {
            const char* tr = engine::locale::Translate(it->text);
            char buf[320];
            const int nl = engine::widget::Wrap(tr ? tr : it->text, buf, sizeof buf,
                kMaxW * 3.0f, 36.0f / 30.0f, 4);
            float mxu = 0.0f;
            for (char* p = buf; *p;) {
                char* e = strchr(p, '\n');
                char seg[200];
                size_t n = e ? static_cast<size_t>(e - p) : strlen(p);
                if (n > sizeof seg - 1)
                    n = sizeof seg - 1;
                memcpy(seg, p, n);
                seg[n] = 0;
                const float lw = engine::widget::TextWidth(seg, 36.0f / 30.0f);
                if (lw > mxu)
                    mxu = lw;
                if (!e)
                    break;
                p = e + 1;
            }
            // measured on A: the text widget's font runs wider than the feed's estimate
            const float mx = mxu / 3.0f * 1.18f;
            const float innerw = mx + kIconPx + kGap;
            float innerh = static_cast<float>(nl) * 20.0f;
            if (innerh < kIconPx)
                innerh = kIconPx;
            const float boxw = innerw + 2.0f * kPadX;
            const float boxh = innerh + 2.0f * kPadY;
            const float cy = kMenuCy;
            g_ntf.innerw = innerw;
            g_ntf.boxh = boxh;
            g_ntf.cy = cy;
            g_ntf.nl = nl;

            for (int i = 0; i < 5; i++) {
                engine::widget::Visible(w[i], false);
                engine::widget::Alpha(w[i], 0.0f);
            }
            for (int i = 0; i < 3; i++) {
                const float* b = kBlobs[i];
                const float wp = (boxw * b[0] + b[1]) * 3.0f;
                const float hp = (boxh * b[2] + b[3]) * 3.0f;
                engine::widget::Scale(w[i], wp / kBlobW0, hp / kBlobH0);
                engine::widget::Pivot(w[i], b[4] * boxw * 3.0f - wp * 0.5f,
                    (cy + b[5] * boxh) * 3.0f - hp * 0.5f);
            }
            engine::widget::SetImage(w[3], kIcons[it->kind]);
            engine::widget::Pivot(w[3], -innerw * 1.5f, (cy - kIconPx * 0.5f) * 3.0f);
            engine::widget::SetText(w[4], buf);     // 1500 x 250 units, centred in both axes
            engine::widget::Pivot(w[4],
                (-innerw * 0.5f + kIconPx + kGap + mx * 0.5f) * 3.0f - 750.0f,
                cy * 3.0f - 125.0f);

            g_ntf.menu = 1;
            g_ntf.key = key;
            g_ntf.it = *it;
            g_ntf.kind = it->kind;
            g_ntf.banner = 0;
            snprintf(g_ntf.text, sizeof g_ntf.text, "%s", it->text);
            g_ntf.st = 1;
            g_ntf.setFrame = engine::frame::Count();
            g_ntf.la = -1.0f;
            g_ntf.a = 0.0f;
            g_ntf.until = now + static_cast<uint64_t>(
                (it->secs > 0 && it->secs < 600.0f ? it->secs : 5.0f) * 1000.0f);
            TO1_LOG("[NOTIFY] show %s \"%.100s\" in the menus (page 0x%llx) %d line(s) box %.0f x %.0f px at y %.1f\n",
                kKindNames[it->kind], g_ntf.text,
                static_cast<unsigned long long>(engine::widget::TopPage()), nl, boxw, boxh, cy);
        }

        void RequeueFront(const Item* it, float secs)
        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (g_qn < kQN) {
                memmove(g_q + 1, g_q, sizeof g_q[0] * static_cast<size_t>(g_qn));
                g_q[0] = *it;
                g_q[0].secs = secs;
                g_qn++;
            }
        }

    }

    const char* IconPath(int kind)
    {
        return kind >= 0 && kind < NTF_N ? kIcons[kind] : kIcons[NTF_INFO];
    }

    int KindOf(const char* t)
    {
        // the icon follows the CONTEXT: the '!' ring only for real alerts; checked in
        // this order, first match wins
        if (!t)
            return NTF_INFO;
        if (!strncmp(t, "Gift:", 5) || Has(t, "crafted a gift"))
            return NTF_CRAFT;
        if (Has(t, "Covert"))
            return NTF_COVERT;
        if (Has(t, "been marked"))
            return NTF_WARN;
        if (Has(t, "revive") || Has(t, "reviving"))
            return NTF_REVIVE;
        if (Has(t, "heal") || Has(t, "health") || Has(t, "medkit")
            || Has(t, "first aid") || Has(t, "Second Chance"))
            return NTF_HEAL;
        if (Has(t, "bomb"))
            return NTF_BOMB;
        if (Has(t, "Loadout") || Has(t, "Too many") || Has(t, "Not enough")
            || Has(t, "per team") || Has(t, "equipped") || Has(t, "on team")
            || Has(t, "limit"))
            return NTF_LOADOUT;
        if (Has(t, "in the way") || Has(t, "vault"))
            return NTF_MOVE;
        if (Has(t, "being interrogated") || Has(t, "you were interrogated")
            || Has(t, "stole") || Has(t, "is leaking") || Has(t, "knows where")
            || Has(t, "defend"))
            return NTF_WARN;
        if (Has(t, "interrogat") || Has(t, "execute") || Has(t, "shiv"))
            return NTF_BLADE;
        if (Has(t, "Crafter") || Has(t, "Scavenger") || Has(t, "Lucky Break")
            || Has(t, "Gift box") || Has(t, "lockbox") || Has(t, "crafting")
            || Has(t, "supplies") || Has(t, "ammo"))
            return NTF_CRAFT;
        if (Has(t, "can't") || Has(t, "cannot"))
            return NTF_MOVE;
        return NTF_INFO;
    }

    bool Push(int kind, const char* text, float secs)
    {
        return Push2(kind, text, secs, 0);
    }

    bool Banner(const char* text, float secs)
    {
        return Push2(NTF_INFO, text, secs, 1);
    }

    void Cancel(const char* text)
    {
        if (!text)
            return;
        std::lock_guard<std::mutex> lk(g_lock);
        int k = 0;
        for (int i = 0; i < g_qn; i++)
            if (strncmp(g_q[i].text, text, sizeof g_q[i].text - 1))
                g_q[k++] = g_q[i];
        g_qn = k;
        if (g_ntf.st && !strncmp(g_ntf.it.text, text, sizeof g_ntf.it.text - 1)) {
            const uint64_t now = GetTickCount64();
            if (g_ntf.until > now - 200)
                g_ntf.until = now - 200;
        }
    }

    void DropNonserver()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        int k = 0;
        for (int i = 0; i < g_qn; i++)
            if (g_q[i].kind == NTF_SERVER)
                g_q[k++] = g_q[i];
        g_qn = k;
    }

    void Tick()
    {
        const uint64_t now = GetTickCount64();
        const bool hud = net::gui2::InMatch();

        if (!hud && g_ok) {
            g_ok = false;
            if (g_ntf.st && !g_ntf.menu) {
                // the match HUD is gone: a server toast (admin kick) moves on to the menu
                const float left = static_cast<float>(static_cast<int64_t>(g_ntf.until) - static_cast<int64_t>(now)) / 1000.0f;
                const bool kick = IsKick(g_ntf.it.text);
                g_ntf.st = 0;
                if (g_ntf.kind == NTF_SERVER && (kick || left > 1.0f)) {
                    RequeueFront(&g_ntf.it, kick && left < 8.0f ? 8.0f : left);
                    TO1_LOG("[NOTIFY] server toast cut by the match end - carried to the menu (%.1f s)\n",
                        kick && left < 8.0f ? 8.0f : left);
                }
            }
            DropNonserver();
        }   // (hud-root is rebuilt per level: re-resolved each match)

        if (hud && g_ntf.st && g_ntf.menu)
            g_ntf.st = 0;           // the menu page's widgets are gone
        if (hud && !g_ok && !Resolve()) {
            std::lock_guard<std::mutex> lk(g_lock);
            g_qn = 0;
            return;
        }

        uint64_t mw[5], key = 0, *w = g_w;
        int nw = 4;
        if (!hud) {
            w = mw;
            nw = 5;
            if (g_ntf.st) {         // a toast is up: the page it sits on must still be there
                if (!MenuResolve(mw, &key)) {
                    const float left = static_cast<float>(static_cast<int64_t>(g_ntf.until) - static_cast<int64_t>(now)) / 1000.0f;
                    g_ntf.st = 0;
                    if (left > 1.0f)
                        RequeueFront(&g_ntf.it, left);
                    return;
                }
                if (key != g_ntf.key) {     // another page (or the same one rebuilt): start again
                    const float left = static_cast<float>(static_cast<int64_t>(g_ntf.until) - static_cast<int64_t>(now)) / 1000.0f;
                    g_ntf.st = 0;
                    if (left > 1.0f) {
                        Item it = g_ntf.it;
                        it.secs = left;
                        StartMenu(&it, now, mw, key);
                    }
                    return;
                }
            }
        }

        if (!g_ntf.st) {
            Item it{};
            bool have = false;
            if (hud) {
                {
                    std::lock_guard<std::mutex> lk(g_lock);
                    if (g_qn) {
                        it = g_q[0];
                        memmove(g_q, g_q + 1, sizeof g_q[0] * static_cast<size_t>(--g_qn));
                        have = true;
                    }
                }
                if (!have)
                    return;
                Start(&it, now);
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_lock);
                if (g_qn) {
                    it = g_q[0];
                    have = true;
                }
            }
            if (!have)
                return;
            if (!MenuResolve(mw, &key)) {
                // booting / loading / a page without the widgets: stay queued (kept 120 s)
                if (now - it.tEnq > 120000u) {
                    std::lock_guard<std::mutex> lk(g_lock);
                    if (g_qn)
                        memmove(g_q, g_q + 1, sizeof g_q[0] * static_cast<size_t>(--g_qn));
                }
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_lock);
                if (g_qn)
                    memmove(g_q, g_q + 1, sizeof g_q[0] * static_cast<size_t>(--g_qn));
            }
            StartMenu(&it, now, mw, key);
            return;
        }

        if (g_ntf.st == 1) {
            if (engine::frame::Count() <= g_ntf.setFrame)
                return;             // the icon path / string has had no frame yet
            for (int i = 0; i < nw; i++)
                engine::widget::Visible(w[i], true);
            g_ntf.st = 2;
            g_ntf.t0 = now;
            if (!g_ntf.banner)
                Sound();
        }

        float a = 1.0f;
        if (!g_ntf.banner && now - g_ntf.t0 < 150)
            a = 0.1f + 0.9f * static_cast<float>(now - g_ntf.t0) / 150.0f;  // a banner pops in on one frame
        if (now > g_ntf.until) {
            const float o = 1.0f - static_cast<float>(now - g_ntf.until) / (g_ntf.banner ? 150.0f : 300.0f);
            if (o < a)
                a = o;
        }
        if (a <= 0.0f) {
            for (int i = 0; i < nw; i++) {
                engine::widget::Alpha(w[i], 0.0f);
                engine::widget::Visible(w[i], false);
            }
            g_ntf.st = 0;
            g_ntf.a = 0.0f;
            return;
        }
        if (net::gui2::PauseMenuOpen())
            a = 0.0f;               // a menu over the HUD: the toast keeps ageing
        g_ntf.a = a;
        if (fabsf(a - g_ntf.la) > 0.01f) {
            for (int i = 0; i < nw; i++)
                engine::widget::Alpha(w[i],
                    i < 3 ? a * (g_ntf.banner ? kBanBlobs[i][6] : kBlobs[i][6])
                          : (g_ntf.banner && i == 3) ? 0.0f : a);
            g_ntf.la = a;
        }
    }

    void Draw()
    {
        if (g_ntf.st != 2 || g_ntf.menu || g_ntf.a <= 0.0f)
            return;
        if (!net::gui2::InMatch())
            return;
        const uint32_t al = static_cast<uint32_t>(g_ntf.a * 255.0f + 0.5f) << 24;
        const float left = 640.0f - g_ntf.innerw * 0.5f + (g_ntf.banner ? 0.0f : kIconPx + kGap);
        const float y0 = g_ntf.cy - static_cast<float>(g_ntf.nl - 1) * kPitch * 0.5f - g_ntf.th * 0.5f;
        for (int i = 0; i < g_ntf.nl; i++) {
            if (g_ntf.lw[i] <= 0.0f)
                continue;
            const float x = left + (g_ntf.tbw - g_ntf.lw[i]) * 0.5f;
            DebugDrawCommon::DrawTextPrintV(x, y0 + static_cast<float>(i) * kPitch, g_ntf.size,
                (g_ntf.banner ? kBanCol : kTxtCol) | al, "%s", g_ntf.line[i]);
        }
    }

    void Reset()
    {
        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_qn = 0;
        }
        g_ntf = State{};
        g_ok = false;
        memset(g_w, 0, sizeof g_w);
    }

}
