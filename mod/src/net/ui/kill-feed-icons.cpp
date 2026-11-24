// ported from tools/ext/killfeed.c (build-201 working tree). The ext detoured
// winmm's text-feed entry points and drove pre-baked hud-root widgets. The mod
// owns its own text feed (net/ui/kill-feed.cpp), so suppression is simply not
// emitting it while this icon feed is live -- no VA detours, no new hook.
// The ext's 0x94 wire layout is kept byte-for-byte for peer compatibility.

#include "kill-feed-icons.hpp"
#include "kill-feed-core.hpp"

#include "boot.hpp"
#include "engine/draw.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/lobby.hpp"
#include "engine/widget.hpp"
#include "net-player.hpp"
#include "net/ui/death-ui.hpp"
#include "net/ui/net-gui2.hpp"
#include "net/online/net-session.hpp"
#include "net/protocol/net-protocol.hpp"
#include "sid/sid.hpp"
#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace net::ui::killfeed {

    namespace {

        using core::Event;

        // ---- ext constants (killfeed.c:19-61) -----------------------------------
        constexpr float kIconL     = 2940.0f;
        constexpr float kKillerEnd = 2920.0f;
        constexpr float kKillerW   = 1100.0f;
        constexpr float kVictimX   = 3230.0f;
        constexpr float kVictimW   = 520.0f;
        constexpr float kShPad     = 36.0f;
        constexpr float kShY       = -21.0f;
        constexpr float kShCap     = 240.0f;
        constexpr float kShMidW0   = 1000.0f;
        constexpr float kFiW       = 66.0f;
        constexpr float kFiX       = 2864.0f;
        constexpr float kFiY       = 6.0f;
        constexpr float kIconW     = 270.0f;
        constexpr float kIconH     = 66.0f;
        constexpr float kFiSpan    = 76.0f;
        constexpr float kRowH      = 78.0f;
        constexpr float kTxtPx     = 18.0f;
        constexpr float kNameFont  = 1.0f;

        constexpr const char* kSkull = "streaming/t2r/icons/killfeed-skull.png";
        constexpr const char* kFist  = "streaming/t2r/icons/fist.png";

        enum Widget : int {
            W_GRP, W_KB, W_KR, W_IC, W_VB, W_VR, W_SHL, W_SHM, W_SHR,
            W_KBS, W_KRS, W_ICS, W_VBS, W_VRS, W_FI, W_N
        };
        constexpr const char* kSuffix[W_N] = {
            "", ".kb", ".kr", ".ic", ".vb", ".vr", ".shl", ".shm", ".shr",
            ".kbs", ".krs", ".ics", ".vbs", ".vrs", ".fi"
        };

        struct Row {
            int      st = 0;          // 0 free, 1 written while hidden, 2 shown
            uint64_t t0 = 0, born = 0, seq = 0, setFrame = 0;
            float    y = -1.0f, ly = -1.0f, la = -1.0f, a = 0.0f;
            char     killer[44]{};
            char     victim[44]{};
            bool     haveKiller = false, kf = false, vf = false, isDown = false;
            int      fiKind = 0, faMode = 0, icAnim = 0;
            uint64_t faT0 = 0;
            uint8_t  vslot = 0xFF;
            float    kend = 0.0f, size = kNameFont, kw = 0.0f, vw = 0.0f;
            const char* wicon = nullptr;
            const char* nextWicon = nullptr;
        };

        struct WeaponCache { uint64_t w = 0; uint64_t ms = 0; };
        struct RecentKill  { char name[44]{}; uint64_t ms = 0; };

        uint64_t     gW[core::kRows][W_N]{};
        Row          gRows[core::kRows];
        Event        gQueue[core::kQueue];
        int          gQueueN = 0;
        uint64_t     gSeq = 0;
        WeaponCache  gVw[kMaxNetPlayers], gDw[kMaxNetPlayers];
        uint64_t     gClose[kMaxNetPlayers]{};
        uint64_t     gSelfDown[kMaxNetPlayers]{};
        RecentKill   gRk[8];
        int          gRkN = 0;
        uint64_t     gPrevMs = 0, gLastResolve = 0;
        bool         gOk = false, gFiOk = false, gPaused = false;
        float        gVis = 1.0f;
        std::mutex   gLock;

        // ---- widget helpers ------------------------------------------------------
        bool Resolve(uint64_t now)
        {
            if (gOk) return true;
            if (gLastResolve && now - gLastResolve < 2000) return false;
            gLastResolve = now;
            for (int r = 0; r < core::kRows; ++r)
                for (int i = 0; i < W_N; ++i) {
                    char p[32];
                    std::snprintf(p, sizeof p, "t2rKf%d%s", r, kSuffix[i]);
                    gW[r][i] = engine::widget::Hud(p);
                }
            float px = 0.0f, py = 0.0f;
            if (!gW[0][W_GRP] || !engine::widget::PivotGet(gW[0][W_GRP], &px, &py)) {
                static int nl = 0;
                if (nl++ < 3)
                    TO1_LOG("killfeed: t2rKf0 not in hud-root (older gui2-widgets.pak) - stock text feed stays\n");
                return false;
            }
            gOk = true;
            gFiOk = gW[0][W_FI] != 0;
            TO1_LOG("killfeed: widgets found (%d rows, status icon %s) - icon feed on, stock text feed suppressed\n",
                core::kRows, gFiOk ? "yes" : "NO (older pak: no down entries)");
            return true;
        }

        void Hide(int r)
        {
            if (gRows[r].st)
                engine::widget::Visible(gW[r][W_GRP], false);
            if (gW[r][W_FI])
                engine::widget::Visible(gW[r][W_FI], false);
            gRows[r] = Row{};
        }

        void Image(uint64_t w, const char* path)
        {
            if (w && path) engine::widget::SetImage(w, path);
        }

        // ---- name width (the HUD text routine's own measurement) ------------------
        void Measure(const char* killer, const char* victim, float& size,
                     float& kw, float& vw)
        {
            size = kNameFont;
            kw = vw = 0.0f;
            const char* n[2] = { killer, victim };
            float* out[2] = { &kw, &vw };
            for (int i = 0; i < 2; ++i) {
                if (!n[i] || !n[i][0]) continue;
                const vec2 m = DebugDrawCommon::GetTextWidthHeight(n[i], kNameFont);
                if (m.x > 0.0f && m.x < 1280.0f) *out[i] = m.x;
            }
        }

        // ---- icon selection / row population (ext KfPickIcon / KfShowEvent) -------
        void PickIcon(Event& e)
        {
            if (!e.haveKiller) {
                e.icon = kSkull;
                return;
            }
            if (e.weapon) {
                const auto info = death_ui::core::Weapon(nullptr, e.weapon);
                if (info.found && info.icon) {
                    e.icon = info.icon;
                    e.iconKnown = true;
                    return;
                }
            }
            e.icon = kFist;
        }

        void ShowEvent(const Event& e, uint64_t now)
        {
            int r = -1;
            for (int i = 0; i < core::kRows; ++i)
                if (!gRows[i].st) { r = i; break; }
            if (r < 0) {
                r = 0;
                for (int i = 1; i < core::kRows; ++i)
                    if (gRows[i].seq < gRows[r].seq) r = i;
            }
            engine::widget::Visible(gW[r][W_GRP], false);
            uint64_t* w = gW[r];

            Image(w[W_IC], e.icon);
            engine::widget::Scale(w[W_IC], 1.0f, 1.0f);
            engine::widget::Pivot(w[W_IC], kIconL, kFiY);
            engine::widget::Alpha(w[W_IC], 1.0f);
            for (int k : { W_KBS, W_KRS, W_ICS, W_VBS, W_VRS, W_KB, W_KR, W_VB, W_VR })
                engine::widget::Visible(w[k], false);
            engine::widget::Visible(w[W_IC], true);

            const int fik = e.haveKiller && gFiOk ? (e.isDown ? 1 : 2) : 0;
            if (w[W_FI]) {
                if (fik) Image(w[W_FI], fik == 1 ? kFist : kSkull);
                engine::widget::Alpha(w[W_FI], 0.0f);
                engine::widget::Visible(w[W_FI], fik != 0);
            }

            Row& s = gRows[r];
            s.fiKind = fik;
            s.isDown = e.isDown;
            s.faMode = fik ? 1 : 0;
            s.faT0 = 0;
            s.vslot = e.vslot;
            s.kend = kKillerEnd - (fik ? kFiSpan : 0.0f);
            s.wicon = e.icon;
            s.nextWicon = nullptr;
            s.icAnim = 0;
            std::memcpy(s.killer, e.killer, sizeof s.killer);
            std::memcpy(s.victim, e.victim, sizeof s.victim);
            s.haveKiller = e.haveKiller;
            s.kf = e.kf;
            s.vf = e.vf;
            Measure(s.haveKiller ? s.killer : "", s.victim, s.size, s.kw, s.vw);

            float kw = s.kw * 3.0f;
            float vw = s.vw * 3.0f;
            if (kw > kKillerW) kw = kKillerW;
            if (vw > kVictimW) vw = kVictimW;
            float L = (e.haveKiller ? s.kend - kw : kIconL) - kShPad;
            float R = kVictimX + vw + kShPad;
            if (L < kShCap) L = kShCap;
            engine::widget::Pivot(w[W_SHL], L - kShCap, kShY);
            engine::widget::Pivot(w[W_SHM], L, kShY);
            engine::widget::Scale(w[W_SHM], (R - L) / kShMidW0, 1.0f);
            engine::widget::Pivot(w[W_SHR], R, kShY);

            engine::widget::Alpha(w[W_GRP], 0.0f);
            s.st = 1;
            s.t0 = s.born = now;
            s.seq = ++gSeq;
            s.setFrame = engine::frame::Count();
            s.y = s.ly = s.la = -1.0f;
        }

        // ---- status-icon pop / fist -> skull conversion ---------------------------
        void FiApply(int r, float sc, float dx, float al)
        {
            const uint64_t fi = gW[r][W_FI];
            engine::widget::Scale(fi, sc, sc);
            engine::widget::Pivot(fi, kFiX + dx + (1.0f - sc) * kFiW * 0.5f,
                                  kFiY + (1.0f - sc) * kFiW * 0.5f);
            engine::widget::Alpha(fi, al);
            const uint64_t ic = gRows[r].icAnim ? gW[r][W_IC] : 0;
            if (ic) {
                engine::widget::Scale(ic, sc, sc);
                engine::widget::Pivot(ic, kIconL + dx + (1.0f - sc) * kIconW * 0.5f,
                                      kFiY + (1.0f - sc) * kIconH * 0.5f);
                engine::widget::Alpha(ic, al);
            }
        }

        void FiAnim(int r, uint64_t now)
        {
            Row& s = gRows[r];
            if (!s.fiKind || !s.faMode || !gW[r][W_FI]) return;
            if (!s.faT0) s.faT0 = now;
            if (s.faMode == 2) {
                const float u = static_cast<float>(now - s.faT0) / static_cast<float>(core::kOutMs);
                if (u < 1.0f) {
                    FiApply(r, 1.0f - 0.45f * u * u, 0.0f, 1.0f - u);
                    return;
                }
                Image(gW[r][W_FI], kSkull);
                if (s.nextWicon) {
                    Image(gW[r][W_IC], s.nextWicon);
                    s.wicon = s.nextWicon;
                    s.nextWicon = nullptr;
                }
                s.faMode = 1;
                s.faT0 += core::kOutMs;
            }
            const float u = static_cast<float>(now - s.faT0) / static_cast<float>(core::kPopMs);
            if (u >= 1.0f) {
                FiApply(r, 1.0f, 0.0f, 1.0f);
                s.faMode = 0;
                s.icAnim = 0;
                return;
            }
            const float t = u - 1.0f;
            const float eb = 1.0f + 4.0f * t * t * t + 3.0f * t * t;  // easeOutBack c1=3
            const float dx = 12.0f * std::sin(u * 15.7f) * (1.0f - u);
            float al = u * 4.0f;
            if (al > 1.0f) al = 1.0f;
            FiApply(r, 0.4f + 0.6f * eb, dx, al);
        }

        bool TryConvert(const Event& e, uint64_t now)
        {
            if (!e.haveKiller || !gFiOk) return false;
            int b = -1;
            for (int r = 0; r < core::kRows; ++r) {
                const Row& s = gRows[r];
                if (s.st && s.fiKind && !std::strcmp(s.victim, e.victim)
                    && (b < 0 || s.seq > gRows[b].seq))
                    b = r;
            }
            if (b < 0 || !gRows[b].isDown || std::strcmp(gRows[b].killer, e.killer))
                return false;
            if (gRows[b].vslot < kMaxNetPlayers && gClose[gRows[b].vslot] > gRows[b].born)
                return false;
            Row& s = gRows[b];
            s.isDown = false;
            s.fiKind = 2;
            s.t0 = now;
            if (e.iconKnown && e.icon != s.wicon) s.nextWicon = e.icon;
            s.icAnim = s.nextWicon && s.st != 1;
            if (s.st == 1) {
                Image(gW[b][W_FI], kSkull);
                if (s.nextWicon) {
                    Image(gW[b][W_IC], s.nextWicon);
                    s.wicon = s.nextWicon;
                    s.nextWicon = nullptr;
                }
            }
            else {
                s.faMode = 2;
                s.faT0 = now;
            }
            TO1_LOG("killfeed: %s finished %s - entry %d fist -> skull\n",
                e.killer, e.victim, b);
            return true;
        }

        // ---- producers ------------------------------------------------------------
        void SlotInfo(uint8_t slot, char* out, bool& friendly)
        {
            out[0] = '\0';
            const int me = static_cast<int>(net::session::MySlot());
            friendly = static_cast<int>(slot) == me || NetPlayers::SlotIsFriendly(slot);
            if (static_cast<int>(slot) == me) {
                const char* n = net::session::LocalName();
                if (n) core::CopyName(out, n);
                return;
            }
            const NetPlayer& p = NetPlayers::BySlot(slot);
            core::CopyName(out, p.name);
        }

        void Queue(const Event& e)
        {
            std::lock_guard<std::mutex> lk(gLock);
            core::Push(gQueue, gQueueN, e);
        }

        void SendWeapon(uint8_t victimSlot, uint64_t weapon, bool down)
        {
            if (!weapon) return;
            uint8_t m[10]{};
            m[0] = victimSlot;
            std::memcpy(m + 1, &weapon, 8);   // little-endian, as the ext sent it
            const uint8_t n = down ? 10 : 9;
            if (down) m[9] = 1;
            net::session::SendEventRaw(static_cast<uint8_t>(NEVT_KFWEAPON), m, n);
        }

        void NoteKillAt(uint8_t victimSlot, uint8_t killerSlot, bool haveKiller,
                        uint64_t weapon, uint64_t now)
        {
            Event e;
            e.haveKiller = haveKiller;
            e.vslot = victimSlot;
            e.kslot = killerSlot;
            e.weapon = weapon;
            e.born = now;
            SlotInfo(victimSlot, e.victim, e.vf);
            if (haveKiller) SlotInfo(killerSlot, e.killer, e.kf);
            if (weapon) e.iconKnown = true;
            PickIcon(e);

            const int me = static_cast<int>(net::session::MySlot());
            if (static_cast<int>(victimSlot) == me) {
                SendWeapon(victimSlot, weapon, false);  // our death card's answer
            }
            else if (haveKiller && victimSlot < kMaxNetPlayers) {
                std::lock_guard<std::mutex> lk(gLock);
                WeaponCache& c = gVw[victimSlot];
                if (c.ms && now - c.ms < core::kWeaponCacheMs) {
                    e.weapon = c.w;
                    c.ms = 0;
                    PickIcon(e);
                }
                else e.waitUntil = now + core::kWaitMs;
            }
            Queue(e);
        }

        void NoteDown(uint8_t victim, uint8_t downer, uint64_t weapon,
                      bool local, uint64_t now)
        {
            if (victim >= kMaxNetPlayers || downer >= kMaxNetPlayers) return;
            const int me = static_cast<int>(net::session::MySlot());
            if (local) {
                if (gSelfDown[victim] && now - gSelfDown[victim] < core::kSelfDownMs)
                    return;
                gSelfDown[victim] = now;
                SendWeapon(victim, weapon, true);
            }
            else {
                if (static_cast<int>(victim) == me) return;
                if (gSelfDown[victim] && now - gSelfDown[victim] < core::kRedownMs)
                    return;
                gSelfDown[victim] = now;
            }

            Event e;
            e.haveKiller = true;
            e.isDown = true;
            e.vslot = victim;
            e.kslot = downer;
            e.weapon = weapon;
            e.born = now;
            SlotInfo(victim, e.victim, e.vf);
            SlotInfo(downer, e.killer, e.kf);
            if (!e.killer[0] || !e.victim[0]) return;
            if (weapon) e.iconKnown = true;
            PickIcon(e);

            if (!local && victim < kMaxNetPlayers) {
                std::lock_guard<std::mutex> lk(gLock);
                WeaponCache& c = gDw[victim];
                if (c.ms && now - c.ms < core::kWeaponCacheMs) {
                    e.weapon = c.w;
                    c.ms = 0;
                    PickIcon(e);
                }
                else e.waitUntil = now + core::kWaitMs;
            }
            Queue(e);
        }

        // ---- queue drain (ext FeatureKillfeed) ------------------------------------
        void Drain(uint64_t now)
        {
            Event ready[core::kQueue];
            int n = 0, nr = 0;
            {
                std::lock_guard<std::mutex> lk(gLock);
                for (int i = 0; i < gQueueN && i < core::kQueue; ++i) {
                    if (core::Waiting(gQueue[i], now)) {
                        gQueue[n++] = gQueue[i];
                        continue;
                    }
                    if (!gQueue[i].isDown && core::Blocked(gQueue, gQueueN, i, now)) {
                        gQueue[n++] = gQueue[i];
                        continue;
                    }
                    ready[nr++] = gQueue[i];
                }
                gQueueN = n;
            }
            for (int i = 0; i < nr; ++i) {
                Event& e = ready[i];
                if (e.isDown) {
                    if (!gFiOk) continue;               // older pak: no status icon, no down rows
                    bool late = false;
                    for (int k = 0; k < gRkN; ++k)
                        if (!std::strcmp(gRk[k].name, e.victim)
                            && now - gRk[k].ms < core::kKillBeforeDownMs)
                            late = true;
                    if (late) {
                        TO1_LOG("killfeed: down of %s arrived after its kill - skipped\n", e.victim);
                        continue;
                    }
                    if (!e.weapon) {
                        // The victim's answer never came: the downer's held gun, else fists.
                        e.weapon = SID("fists");
                        e.iconKnown = false;
                    }
                    PickIcon(e);
                    ShowEvent(e, now);
                    continue;
                }
                PickIcon(e);
                if (e.haveKiller) {
                    if (gRkN < 8) ++gRkN;
                    std::memmove(gRk + 1, gRk, static_cast<size_t>(gRkN - 1) * sizeof *gRk);
                    core::CopyName(gRk[0].name, e.victim);
                    gRk[0].ms = now;
                    if (TryConvert(e, now)) continue;
                }
                ShowEvent(e, now);
            }
        }

        // ---- per-frame row animation ----------------------------------------------
        void Animate(uint64_t now)
        {
            float dt = gPrevMs && now > gPrevMs
                ? static_cast<float>(now - gPrevMs) : 16.0f;
            gPrevMs = now;
            gPaused = net::gui2::MenuUp();
            float blk = death_ui::Black();
            if (blk < 0.0f) blk = 0.0f;
            else if (blk > 1.0f) blk = 1.0f;
            gVis = gPaused ? 0.0f : 1.0f - blk;

            for (int r = 0; r < core::kRows; ++r)
                if (gRows[r].st && now - gRows[r].t0 >= core::kLifeMs)
                    Hide(r);

            for (int r = 0; r < core::kRows; ++r) {
                Row& s = gRows[r];
                if (!s.st) continue;
                int rank = 0;
                for (int o = 0; o < core::kRows; ++o)
                    if (gRows[o].st && gRows[o].seq < s.seq) ++rank;
                const float ty = core::kTop + static_cast<float>(rank) * core::kPitch;
                if (s.y < 0.0f) s.y = ty;
                else s.y += (ty - s.y) * (1.0f - std::exp(-dt / 70.0f));
                if (std::fabs(s.y - ty) < 0.5f) s.y = ty;
                if (std::fabs(s.y - s.ly) > 0.4f) {
                    engine::widget::Pivot(gW[r][W_GRP], 0.0f, s.y);
                    s.ly = s.y;
                }
                if (s.st == 1) {
                    if (engine::frame::Count() <= s.setFrame) continue;
                    engine::widget::Visible(gW[r][W_GRP], true);
                    s.st = 2;
                }
                const uint64_t age = now - s.t0;
                const uint64_t bornAge = now - s.born;
                float a = 1.0f;
                if (bornAge < core::kFadeInMs)
                    a = 0.1f + 0.9f * static_cast<float>(bornAge) / static_cast<float>(core::kFadeInMs);
                if (age + core::kFadeOutMs > core::kLifeMs) {
                    const float o = static_cast<float>(core::kLifeMs - age)
                        / static_cast<float>(core::kFadeOutMs);
                    if (o < a) a = o;
                }
                if (a < 0.0f) a = 0.0f;
                a *= gVis;
                s.a = a;
                FiAnim(r, now);
                if (std::fabs(a - s.la) > 0.01f || (a == 0.0f && s.la != 0.0f)) {
                    engine::widget::Alpha(gW[r][W_GRP], a);
                    s.la = a;
                }
            }
        }

    } // namespace

    void OnPeerWeapon(uint8_t sender, const uint8_t* data, uint8_t size)
    {
        if (size < 9 || sender >= kMaxNetPlayers || !data || data[0] != sender)
            return;
        const bool dn = size >= 10 && data[9] == 1;
        uint64_t w = 0;
        std::memcpy(&w, data + 1, 8);
        // The ext's molotov / nail-bomb kill bonus (BkWeapon) has no native
        // counterpart yet; the icon row itself is fully native.

        std::lock_guard<std::mutex> lk(gLock);
        for (int i = 0; i < gQueueN; ++i)
            if (gQueue[i].waitUntil && gQueue[i].vslot == sender
                && gQueue[i].isDown == dn) {
                gQueue[i].weapon = w;
                gQueue[i].waitUntil = 0;
                gQueue[i].iconKnown = true;
                PickIcon(gQueue[i]);
                return;
            }
        WeaponCache& c = dn ? gDw[sender] : gVw[sender];
        c.w = w;
        c.ms = GetTickCount64();
    }

    void OnKill(uint8_t victimSlot, uint8_t killerSlot, bool haveKiller,
                uint64_t weaponSID, const char*, const char*,
                bool, bool)
    {
        if (boot::ExtLoaded()) return;
        NoteKillAt(victimSlot, killerSlot, haveKiller, weaponSID, GetTickCount64());
    }

    void NoteDownBegin(uint8_t victim, uint8_t downer)
    {
        if (boot::ExtLoaded()) return;
        NoteDown(victim, downer, 0, false, GetTickCount64());
    }

    void NoteDownEnd(uint8_t victim)
    {
        if (boot::ExtLoaded() || victim >= kMaxNetPlayers) return;
        gClose[victim] = GetTickCount64();
    }

    // Local-down seam. ext's downed_set calls KfDownLocal(downer, hitWeapon)
    // there, sending the 10-byte down answer before queuing the row. This
    // branch owns the same sink, but the downed producer is shadow-mode
    // (downed_bridge::kEnabled == false) and no native caller feeds it yet;
    // our own-death answer is sent by NoteKillAt() as the 9-byte kill answer.
    // Wiring this to downed-state::BeginDown / OnDownBegin would be dead code
    // until that separate module has a live producer.
    void NoteLocalDown(uint8_t downer, uint64_t weapon)
    {
        if (boot::ExtLoaded()) return;
        const uint8_t me = static_cast<uint8_t>(net::session::MySlot());
        NoteDown(me, downer, weapon, true, GetTickCount64());
    }

    void Tick(uint64_t now)
    {
        if (boot::ExtLoaded()) return;
        if (!engine::lobby::InMatch()) {
            for (int r = 0; r < core::kRows; ++r) Hide(r);
            {
                std::lock_guard<std::mutex> lk(gLock);
                gQueueN = 0;
            }
            gPrevMs = 0;
            gOk = false;
            return;
        }
        if (!Resolve(now)) {
            std::lock_guard<std::mutex> lk(gLock);
            gQueueN = 0;
            return;
        }
        if (engine::flag::Present("t2r-old-killfeed.txt")) {
            for (int r = 0; r < core::kRows; ++r) Hide(r);
            return;
        }
        Drain(now);
        Animate(now);
    }

    void Draw()
    {
        if (boot::ExtLoaded() || !gOk || !Active() || gPaused) return;
        if (!engine::lobby::InMatch()) return;
        for (int r = 0; r < core::kRows; ++r) {
            const Row& s = gRows[r];
            if (s.st != 2 || s.a <= 0.0f) continue;
            const uint32_t al = static_cast<uint32_t>(s.a * 255.0f + 0.5f) << 24;
            const float cy = (s.y + kRowH * 0.5f) / 3.0f;
            const float th = kTxtPx;
            if (s.haveKiller && s.kw > 0.0f) {
                const uint32_t rgb = s.kf ? 0x008FB0CCu : 0x00CF7870u;
                DebugDrawCommon::DrawTextPrintV(
                    s.kend / 3.0f - s.kw, cy - th * 0.5f, s.size,
                    al | rgb, "%s", s.killer);
            }
            if (s.vw > 0.0f) {
                const uint32_t rgb = s.vf ? 0x008FB0CCu : 0x00CF7870u;
                DebugDrawCommon::DrawTextPrintV(
                    kVictimX / 3.0f, cy - th * 0.5f, s.size,
                    al | rgb, "%s", s.victim);
            }
        }
    }

    bool Active()
    {
        return gOk && !engine::flag::Present("t2r-old-killfeed.txt")
            && engine::lobby::InMatch();
    }

    void Reset()
    {
        for (int r = 0; r < core::kRows; ++r) Hide(r);
        {
            std::lock_guard<std::mutex> lk(gLock);
            gQueueN = 0;
        }
        for (int i = 0; i < kMaxNetPlayers; ++i) {
            gVw[i] = {};
            gDw[i] = {};
            gClose[i] = 0;
            gSelfDown[i] = 0;
        }
        for (auto& k : gRk) { k = RecentKill{}; }
        gRkN = 0;
        gSeq = 0;
        gPrevMs = 0;
        gLastResolve = 0;
        gOk = false;
        gFiOk = false;
        gPaused = false;
        gVis = 1.0f;
    }

} // namespace net::ui::killfeed
