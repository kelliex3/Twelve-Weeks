// ported from tools/ext/backpack.c

#include "backpack.hpp"

#include "log.hpp"          // TO1_LOG must precede engine/script.hpp (it uses it in a template)

#include "engine/anim-prims.hpp"
#include "engine/armor-rig.hpp"
#include "engine/backpack-prop.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/local-player.hpp"
#include "engine/object.hpp"
#include "engine/ptr.hpp"
#include "engine/script.hpp"
#include "engine/transform.hpp"

#include "match.hpp"
#include "net-common.hpp"
#include "net-gui2.hpp"
#include "net-player.hpp"
#include "net-protocol.hpp"
#include "net-session.hpp"
#include "player-status.hpp"
#include "gameplay/melee.hpp"

#include <windows.h>
#include <safetyhook.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace net::match::backpack {

    namespace {

        using ::engine::frame::NowMs;    // ext BpMs() -- QPC ms (T60 is 16.7 ms-quantised)

        // ext g_bp_inmatch = MatchSubstate() >= 3 && <= 6: the level is far enough along that
        // our own PlayerBackpackAnimCtrl can exist. The own-side hooks / poll gate on this; the
        // net / puppet half uses the narrower MatchLive (ext MatchSubstate() == 6).
        bool InLevel()
        {
            const ::net::match::Phase p = ::net::match::Get().GetPhase();
            return p >= ::net::match::Phase::LoadingPlaceSpawn && p <= ::net::match::Phase::Live;
        }

        // ---- wire (ext MSG_BACKPACK / BP_VER) --------------------------------------------
        constexpr uint8_t kMsgBackpack = 0x5B;    // pinned to the ext's byte (NEVT_BACKPACK)
        constexpr uint8_t kVer         = 1;

        constexpr int kPhaseIdle = 0, kPhaseOpen = 1, kPhaseClosing = 2, kPhaseAbort = 3;
        const char* const kPhaseName[4] = { "idle", "open", "closing", "abort" };

        constexpr int kClipNone = 0, kClipIntro = 1, kClipLoop = 2, kClipOutro = 3;
        const char* const kClipName[4] = { "none", "intro", "loop", "outro" };

#pragma pack(push, 1)
        struct BpMsgT {
            uint8_t  ver, phase, bagoff, seq;
            uint32_t el_ms;
            uint16_t enter_ms, leave_ms;
        };
#pragma pack(pop)
        static_assert(sizeof(BpMsgT) == 12, "BpMsgT layout");

        // ---- the puppet's craft clip names (the character column of player-inventory2.bin).
        // The puppet resolves the overlay name itself; the first clip its anim tables know
        // (get-animation-duration > 0) wins. The body clip is NOT played here -- net/anim
        // already replays the owner's captured AnimCmdList; this is only the clip clock.
        const char* const kClips[4][4] = {
            { nullptr },
            { "abby-stand^craft-intro", "stand^craft-intro",
              "abby-lg-stand^craft-intro", "lg-stand^craft-intro" },
            { "abby-crouch-craft-idle", "crouch-craft-idle", nullptr, nullptr },
            { "abby-stand^craft-outro", "stand^craft-outro",
              "abby-lg-stand^craft-outro", "lg-stand^craft-outro" }
        };

        // The prop's own clip names: "<character clip>--backpack" in
        // sp-common's anim-abby-player-backpack.pak, authored for the backpack-abby skeleton.
        const char* const kPropClips[4][4] = {
            { nullptr },
            { "abby-stand^craft-intro--backpack", "abby-crouch-craft-intro--backpack",
              "backpack-intro", nullptr },
            { "abby-crouch-craft-idle--backpack", "abby-crouch-craft-generic^idle--backpack",
              "backpack-loop", nullptr },
            { "abby-stand^craft-outro--backpack", "abby-crouch-craft-outro--backpack",
              "backpack-outro", nullptr }
        };

        constexpr float  kPropBlend       = 0.25f;
        constexpr double kIntroMs         = 1567.0;    // own-sample intro length (the ext fixed it)
        constexpr double kLoopMs          = 7400.0;    // own-sample loop period
        constexpr double kDefaultEnterMs  = 135.0;     // the ext's enter_pred fallback
        constexpr double kPupStaleMs      = 2500.0;    // no phase message for this long = closed
        constexpr double kDefaultOnewayMs = 60.0f;     // ext PS_DEFAULT_ONEWAY
        constexpr int    kPartCap         = 64;        // was_vis[] cap (BP_NPARTS = 23)

        constexpr const char* kSatActor = "mp-t2r-satchel-bag-00001";   // ext ISP_SAT_ACTOR

        // ==================================================================================
        //  Recorded own-body prop track (ext BPT_* / BPTR_*)
        // ==================================================================================
        // While OUR prop is out, the prop's spinec joint is read in the body locator's yaw
        // frame and stored per clip (1 intro, 2 loop, 3 outro) and 25 ms step. The track is
        // saved next to the exe and streamed to the peers, so the bag on another player
        // follows that player's own recorded body-relative path. The root track (0x55) is
        // the prop's full locator relative to the body locator (position + rotation).

        constexpr double  kBptDt    = 25.0;
        constexpr int     kBptN     = 320;
        constexpr int     kBptChunk = 24;
        constexpr int     kBptrChunk = 16;
        constexpr uint8_t kBptVer   = 0x54;
        constexpr uint8_t kBptrVer  = 0x55;
        constexpr uint32_t kBptMagic  = 0x31545042u;   // "BPT1"
        constexpr uint32_t kBptrMagic = 0x31525042u;   // "BPR1"
        constexpr const char* kBptFile  = "t2r-bp-track.bin";
        constexpr const char* kBptrFile = "t2r-bp-root.bin";

        float    g_bpt[4][kBptN][3];
        uint8_t  g_bptOk[4][kBptN];
        uint8_t  g_bptDirty[4][kBptN];
        int      g_bptOpenN[4];
        int      g_bptInit, g_bptLive;

        float    g_bptr[4][kBptN][7];
        uint8_t  g_bptrOk[4][kBptN];
        uint8_t  g_bptrDirty[4][kBptN];
        int      g_bptrOpenN[4];
        int      g_bptrLive;

        float    g_bptp[kMaxNetPlayers][4][kBptN][3];
        uint8_t  g_bptpOk[kMaxNetPlayers][4][kBptN];
        int      g_bptNrx[kMaxNetPlayers];

        float    g_bptrp[kMaxNetPlayers][4][kBptN][7];
        uint8_t  g_bptrpOk[kMaxNetPlayers][4][kBptN];
        int      g_bptrNrx[kMaxNetPlayers];

        bool BptOff()   { return ::engine::flag::Present("t2r-no-bp-track.txt"); }
        bool BptRecOn() { return ::engine::flag::Present("t2r-bp-record.txt"); }

        // gap-tolerant read: between the nearest recorded samples around the clip time, held
        // at the first / last one outside them
        int BptGetT(const float (*tb)[3], const uint8_t* ok, double ms, float* out)
        {
            if (ms < 0)
                ms = 0;
            double f = ms / kBptDt;
            int i = static_cast<int>(f);
            if (i > kBptN - 1)
                i = kBptN - 1;
            int a = i, b = i + 1;
            while (a >= 0 && !ok[a])
                a--;
            while (b < kBptN && !ok[b])
                b++;
            if (a < 0 && b >= kBptN)
                return 0;
            if (a < 0) {
                std::memcpy(out, tb[b], 12);
                return 1;
            }
            if (b >= kBptN) {
                std::memcpy(out, tb[a], 12);
                return 1;
            }
            float t = static_cast<float>((f - a) / static_cast<double>(b - a));
            if (t < 0)
                t = 0;
            if (t > 1)
                t = 1;
            for (int k = 0; k < 3; k++)
                out[k] = tb[a][k] + (tb[b][k] - tb[a][k]) * t;
            return 1;
        }

        // usable = at least 16 samples (400 ms) and no hole over 200 ms between the first and last
        int BptComplete(const uint8_t* ok, int* nOut)
        {
            int n = 0, last = -1, gap = 0;
            for (int i = 0; i < kBptN; i++)
                if (ok[i]) {
                    if (last >= 0 && i - last > gap)
                        gap = i - last;
                    last = i;
                    n++;
                }
            if (nOut)
                *nOut = n;
            return n >= 16 && gap <= 8;
        }

        void BptrLoad()
        {
            int nf = 0;
            FILE* f = ::engine::exe_side::Open(kBptrFile, "rb");
            if (f) {
                uint32_t mg = 0;
                static float tb[4][kBptN][7];
                static uint8_t okb[4][kBptN];
                if (fread(&mg, 4, 1, f) == 1 && mg == kBptrMagic
                    && fread(okb, sizeof okb, 1, f) == 1
                    && fread(tb, sizeof tb, 1, f) == 1)
                    for (int c = 1; c <= 3; c++)
                        for (int i = 0; i < kBptN; i++) {
                            const float* v = tb[c][i];
                            const float n = v[3] * v[3] + v[4] * v[4] + v[5] * v[5] + v[6] * v[6];
                            if (okb[c][i] && std::isfinite(v[0] + v[1] + v[2] + n)
                                && std::fabs(v[0]) < 3.0f && std::fabs(v[1]) < 3.0f
                                && std::fabs(v[2]) < 3.0f && n > 0.8f && n < 1.2f) {
                                std::memcpy(g_bptr[c][i], v, 28);
                                g_bptrOk[c][i] = 1;
                                g_bptrDirty[c][i] = 1;
                                nf++;
                            }
                        }
                fclose(f);
            }
            TO1_LOG("bptrack: %d root samples from %s (own opens of earlier sessions, sent to the peers)\n",
                nf, kBptrFile);
        }

        void BptLoadHeader()
        {
            if (g_bptInit)
                return;
            g_bptInit = 1;
            // (the ext also seeds the track from the generated tools/ext/bp_track.h table --
            // empty in this tree, BPT_H_N 0, and not shipped with the mod: the sidecar file
            // and the live learn are the only sources here.)
            int nf = 0;
            FILE* f = ::engine::exe_side::Open(kBptFile, "rb");
            if (f) {
                uint32_t mg = 0;
                static float tb[4][kBptN][3];
                static uint8_t okb[4][kBptN];
                if (fread(&mg, 4, 1, f) == 1 && mg == kBptMagic
                    && fread(okb, sizeof okb, 1, f) == 1
                    && fread(tb, sizeof tb, 1, f) == 1)
                    for (int c = 1; c <= 3; c++)
                        for (int i = 0; i < kBptN; i++) {
                            const float* v = tb[c][i];
                            if (okb[c][i] && v[0] == v[0]
                                && std::fabs(v[0]) < 3.0f && std::fabs(v[1]) < 3.0f
                                && std::fabs(v[2]) < 3.0f) {
                                std::memcpy(g_bpt[c][i], v, 12);
                                g_bptOk[c][i] = 1;
                                g_bptDirty[c][i] = 1;
                                nf++;
                            }
                        }
                fclose(f);
            }
            TO1_LOG("bptrack: %d recorded samples from %s (own opens of earlier sessions, sent to the peers)\n",
                nf, kBptFile);
            BptrLoad();
        }

        void BptSave()
        {
            FILE* f = ::engine::exe_side::Open(kBptFile, "wb");
            if (!f) {
                TO1_LOG("bptrack: cannot write %s\n", kBptFile);
                return;
            }
            uint32_t mg = kBptMagic;
            fwrite(&mg, 4, 1, f);
            fwrite(g_bptOk, sizeof g_bptOk, 1, f);
            fwrite(g_bpt, sizeof g_bpt, 1, f);
            fclose(f);
        }

        void BptrSave()
        {
            FILE* f = ::engine::exe_side::Open(kBptrFile, "wb");
            if (!f) {
                TO1_LOG("bptrack: cannot write %s\n", kBptrFile);
                return;
            }
            uint32_t mg = kBptrMagic;
            fwrite(&mg, 4, 1, f);
            fwrite(g_bptrOk, sizeof g_bptrOk, 1, f);
            fwrite(g_bptr, sizeof g_bptr, 1, f);
            fclose(f);
        }

        int BptGet(int clip, double ms, float* out)
        {
            BptLoadHeader();
            if (clip < 1 || clip > 3)
                return 0;
            return BptGetT(g_bpt[clip], g_bptOk[clip], ms, out);
        }

        // gap tolerant as BptGetT; the rotation is blended in the same hemisphere and renormalised
        int BptrGetT(const float (*tb)[7], const uint8_t* ok, double ms, float* out)
        {
            if (ms < 0)
                ms = 0;
            double f = ms / kBptDt;
            int i = static_cast<int>(f);
            if (i > kBptN - 1)
                i = kBptN - 1;
            int a = i, b = i + 1;
            while (a >= 0 && !ok[a])
                a--;
            while (b < kBptN && !ok[b])
                b++;
            if (a < 0 && b >= kBptN)
                return 0;
            if (a < 0) {
                std::memcpy(out, tb[b], 28);
                return 1;
            }
            if (b >= kBptN) {
                std::memcpy(out, tb[a], 28);
                return 1;
            }
            float t = static_cast<float>((f - a) / static_cast<double>(b - a));
            if (t < 0)
                t = 0;
            if (t > 1)
                t = 1;
            const float sg =
                tb[a][3] * tb[b][3] + tb[a][4] * tb[b][4] + tb[a][5] * tb[b][5] + tb[a][6] * tb[b][6] < 0
                    ? -1.0f : 1.0f;
            for (int k = 0; k < 3; k++)
                out[k] = tb[a][k] + (tb[b][k] - tb[a][k]) * t;
            float n = 0;
            for (int k = 3; k < 7; k++) {
                out[k] = tb[a][k] + (sg * tb[b][k] - tb[a][k]) * t;
                n += out[k] * out[k];
            }
            n = std::sqrt(n);
            if (n < 1e-3f)
                return 0;
            for (int k = 3; k < 7; k++)
                out[k] /= n;
            return 1;
        }

        int BptrValid(const float* v)
        {
            const float n = v[3] * v[3] + v[4] * v[4] + v[5] * v[5] + v[6] * v[6];
            return std::isfinite(v[0] + v[1] + v[2] + n) && std::fabs(v[0]) < 3.0f
                && std::fabs(v[1]) < 3.0f && std::fabs(v[2]) < 3.0f && n > 0.8f && n < 1.2f;
        }

        // ---- quaternion helpers (ext Qrotv / BpQmul) -------------------------------------
        void Qrotv(const float* q, const float* v, float* o)
        {
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            const float tx = 2 * (y * v[2] - z * v[1]);
            const float ty = 2 * (z * v[0] - x * v[2]);
            const float tz = 2 * (x * v[1] - y * v[0]);
            o[0] = v[0] + w * tx + (y * tz - z * ty);
            o[1] = v[1] + w * ty + (z * tx - x * tz);
            o[2] = v[2] + w * tz + (x * ty - y * tx);
        }

        void BpQmul(const float* a, const float* b, float* o)
        {
            o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
            o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
            o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
            o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
        }

        // ---- own sampling (ext BptOwnSample / BptrOwnSample) ------------------------------
        int BptrProcLoc(uint8_t* pr, float* l8)
        {
            if (!::engine::transform::ProcessWorld(reinterpret_cast<uintptr_t>(pr), l8))
                return 0;
            const float n = l8[4] * l8[4] + l8[5] * l8[5] + l8[6] * l8[6] + l8[7] * l8[7];
            return std::isfinite(l8[0] + l8[1] + l8[2] + n) && n > 0.8f && n < 1.2f;
        }

        // owner: the prop root relative to the body locator:
        //   p = conj(qb) (p_prop - p_body), q = conj(qb) q_prop
        void BptrOwnSample(uint8_t* pr, const float* loc, int clip, int i)
        {
            float l8[8];
            if (!BptrProcLoc(pr, l8)) {
                static int nb;
                if (nb++ < 3)
                    TO1_LOG("bptrack: own prop root locator unreadable (proc %p)\n", static_cast<void*>(pr));
                return;
            }
            const float cq[4] = { -loc[4], -loc[5], -loc[6], loc[7] };
            const float d[3] = { l8[0] - loc[0], l8[1] - loc[1], l8[2] - loc[2] };
            float v[7];
            Qrotv(cq, d, v);
            BpQmul(cq, l8 + 4, v + 3);
            if (!BptrValid(v))
                return;
            std::memcpy(g_bptr[clip][i], v, 28);
            g_bptrOk[clip][i] = 1;
            g_bptrDirty[clip][i] = 1;
            g_bptrOpenN[clip]++;
            g_bptrLive++;
            static int nl;
            if (nl < 8 && (i % 10) == 0) {
                nl++;
                TO1_LOG("bptrack: own prop root clip %d idx %d rel pos %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f\n",
                    clip, i, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
            }
        }

        void BptOpenDone();

        void BptrOpenDone()
        {
            int n[4] = { 0 };
            for (int c = 1; c <= 3; c++) {
                BptComplete(g_bptrOk[c], &n[c]);
                for (int i = 0; i < kBptN; i++)
                    if (g_bptrOk[c][i])
                        g_bptrDirty[c][i] = 1;
            }
            TO1_LOG("bptrack: own open root samples intro %d / loop %d / outro %d (root track now %d / %d / %d), saved to %s\n",
                g_bptrOpenN[1], g_bptrOpenN[2], g_bptrOpenN[3], n[1], n[2], n[3], kBptrFile);
            std::memset(g_bptrOpenN, 0, sizeof g_bptrOpenN);
            if (g_bptrLive)
                BptrSave();
        }

        // own close: log what this open recorded, save it, resend the whole track
        void BptOpenDone()
        {
            BptLoadHeader();
            int n[4] = { 0 };
            for (int c = 1; c <= 3; c++) {
                BptComplete(g_bptOk[c], &n[c]);
                for (int i = 0; i < kBptN; i++)
                    if (g_bptOk[c][i])
                        g_bptDirty[c][i] = 1;
            }
            TO1_LOG("bptrack: own open recorded intro %d / loop %d / outro %d samples (track now %d / %d / %d), saved to %s, resending to the peers\n",
                g_bptOpenN[1], g_bptOpenN[2], g_bptOpenN[3], n[1], n[2], n[3], kBptFile);
            std::memset(g_bptOpenN, 0, sizeof g_bptOpenN);
            if (g_bptLive)
                BptSave();
            BptrOpenDone();
        }

        // ---- own phase (ext g_bpo / BpSend / BpOwnSet) ------------------------------------
        struct OwnState {
            int      phase = kPhaseIdle;
            double   tPhase = 0;
            uint8_t  seq = 0;
            double   nextSend = 0;
            int      copies = 0;
            double   tChange = 0;
            uint16_t enterMs = 0xffff, leaveMs = 0xffff;
            uint32_t seenEnter = 0, seenLeave = 0;
        };
        OwnState g_bpo;

        void BpSend(double now)
        {
            BpMsgT m;
            m.ver = kVer;
            m.phase = static_cast<uint8_t>(g_bpo.phase);
            m.bagoff = static_cast<uint8_t>(::engine::backpack_prop::Hid() != 0);
            m.seq = g_bpo.seq;
            const double el = now - g_bpo.tPhase;
            m.el_ms = el > 0 ? static_cast<uint32_t>(el) : 0;
            m.enter_ms = g_bpo.enterMs;
            m.leave_ms = g_bpo.leaveMs;
            net::session::SendEventRaw(kMsgBackpack, &m, static_cast<uint8_t>(sizeof m));
        }

        void BptOwnSample(double now)
        {
            if ((g_bpo.phase != kPhaseOpen && g_bpo.phase != kPhaseClosing)
                || !::engine::backpack_prop::Hid() || BptOff())
                return;
            const ::engine::backpack_prop::State bs = ::engine::backpack_prop::Query();
            if (!bs.ctrl || bs.vis != 1
                || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(bs.ctrl) + 0x198, 8))
                return;
            uint8_t* hh = *reinterpret_cast<uint8_t**>(bs.ctrl + 0x198);
            uint8_t* pr = (hh && ::engine::ptr::Readable(reinterpret_cast<uintptr_t>(hh), 16))
                ? *reinterpret_cast<uint8_t**>(hh + 8) : nullptr;
            const uint64_t ph = (pr && ::engine::ptr::Readable(reinterpret_cast<uintptr_t>(pr), 0xd0))
                ? *reinterpret_cast<uint64_t*>(pr + 0xc0) : 0;
            if (!ph)
                return;
            const double el = now - g_bpo.tPhase;
            int clip;
            double ms;
            if (g_bpo.phase == kPhaseClosing) {
                clip = kClipOutro;
                ms = el;
            } else if (el < kIntroMs) {
                clip = kClipIntro;
                ms = el;
            } else {
                clip = kClipLoop;
                ms = std::fmod(el - kIntroMs, kLoopMs);
            }
            float lb[16];
            const float* loc = ::engine::object::LocalPlayerWorld(lb) ? lb : nullptr;
            float pj[3], pq[4];
            if (!loc || !::engine::object::JointLocator(SID("player"), "spinec", pj, pq))
                return;
            const float yw = 2.0f * std::atan2(loc[5], loc[7]);
            const float cs = std::cos(yw), sn = std::sin(yw);
            const float dx = pj[0] - loc[0], dz = pj[2] - loc[2];
            const float r[3] = { dx * cs - dz * sn, pj[1] - loc[1], dx * sn + dz * cs };
            if (std::fabs(r[0]) > 3.0f || std::fabs(r[1]) > 3.0f || std::fabs(r[2]) > 3.0f)
                return;
            const int i = static_cast<int>(ms / kBptDt + 0.5);
            if (i < 0 || i >= kBptN)
                return;
            BptLoadHeader();
            std::memcpy(g_bpt[clip][i], r, 12);
            g_bptOk[clip][i] = 1;
            g_bptLive++;
            g_bptDirty[clip][i] = 1;
            g_bptOpenN[clip]++;
            BptrOwnSample(pr, loc, clip, i);
            if (BptRecOn()) {
                static int nl;
                if (nl < 400) {
                    nl++;
                    TO1_LOG("bptrack: clip %d idx %d rel %.4f %.4f %.4f\n", clip, i, r[0], r[1], r[2]);
                }
            }
        }

        int BptrSendOne()
        {
            for (int c = 1; c <= 3; c++) {
                int i0 = 0;
                while (i0 < kBptN && !g_bptrDirty[c][i0])
                    i0++;
                if (i0 >= kBptN)
                    continue;
                uint8_t b[6 + kBptrChunk * 15];
                int n = 0;
                b[0] = kBptrVer;
                b[1] = static_cast<uint8_t>(c);
                b[2] = static_cast<uint8_t>(i0 & 0xff);
                b[3] = static_cast<uint8_t>(i0 >> 8);
                b[5] = 0;
                for (int i = i0; i < kBptN && n < kBptrChunk; i++, n++) {
                    uint8_t* e = b + 6 + n * 15;
                    int16_t q[7] = { 0, 0, 0, 0, 0, 0, 0 };
                    e[0] = g_bptrOk[c][i];
                    if (e[0]) {
                        for (int k = 0; k < 3; k++)
                            q[k] = static_cast<int16_t>(std::lrintf(g_bptr[c][i][k] * 1000.0f));
                        for (int k = 3; k < 7; k++)
                            q[k] = static_cast<int16_t>(std::lrintf(g_bptr[c][i][k] * 32767.0f));
                    }
                    std::memcpy(e + 1, q, 14);
                    g_bptrDirty[c][i] = 0;
                }
                b[4] = static_cast<uint8_t>(n);
                net::session::SendEventRaw(kMsgBackpack, b, static_cast<uint8_t>(6 + n * 15));
                return 1;
            }
            return 0;
        }

        // MSG_BACKPACK track chunk: [u8 0x54][u8 clip][u16 first idx][u8 n][u8 0] then
        // n x {u8 has, i16 x, i16 y, i16 z} (mm, body yaw frame).
        void BptSendTick(double now)
        {
            static double next;
            static int alt;
            if (now < next || !net::player_status::MatchLive())
                return;
            if ((alt++ & 1) && BptrSendOne()) {
                next = now + 50.0;
                return;
            }
            for (int c = 1; c <= 3; c++) {
                int i0 = 0;
                while (i0 < kBptN && !g_bptDirty[c][i0])
                    i0++;
                if (i0 >= kBptN)
                    continue;
                uint8_t b[6 + kBptChunk * 7];
                int n = 0;
                b[0] = kBptVer;
                b[1] = static_cast<uint8_t>(c);
                b[2] = static_cast<uint8_t>(i0 & 0xff);
                b[3] = static_cast<uint8_t>(i0 >> 8);
                b[5] = 0;
                for (int i = i0; i < kBptN && n < kBptChunk; i++, n++) {
                    uint8_t* e = b + 6 + n * 7;
                    int16_t q[3] = { 0, 0, 0 };
                    e[0] = g_bptOk[c][i];
                    if (e[0])
                        for (int k = 0; k < 3; k++)
                            q[k] = static_cast<int16_t>(std::lrintf(g_bpt[c][i][k] * 1000.0f));
                    std::memcpy(e + 1, q, 6);
                    g_bptDirty[c][i] = 0;
                }
                b[4] = static_cast<uint8_t>(n);
                net::session::SendEventRaw(kMsgBackpack, b, static_cast<uint8_t>(6 + n * 7));
                next = now + 50.0;
                return;
            }
            if (BptrSendOne())
                next = now + 50.0;
        }

        void BptNetRecv(int s, const uint8_t* b, uint64_t len)
        {
            if (len < 6 || b[1] < 1 || b[1] > 3)
                return;
            const int c = b[1], i0 = b[2] | (b[3] << 8), n = b[4];
            if (len < static_cast<uint64_t>(6 + n * 7) || i0 + n > kBptN)
                return;
            for (int j = 0; j < n; j++) {
                const uint8_t* e = b + 6 + j * 7;
                int16_t q[3];
                std::memcpy(q, e + 1, 6);
                if (!e[0])
                    continue;    // a hole in the owner's track: keep what we had there
                g_bptpOk[s][c][i0 + j] = 1;
                for (int k = 0; k < 3; k++)
                    g_bptp[s][c][i0 + j][k] = q[k] / 1000.0f;
            }
            if (!g_bptNrx[s]++ || g_bptNrx[s] % 50 == 0) {
                int nn;
                BptComplete(g_bptpOk[s][c], &nn);
                TO1_LOG("bptrack: slot %d track chunk %d (clip %d idx %d+%d, that clip now %d samples)\n",
                    s, g_bptNrx[s], c, i0, n, nn);
            }
        }

        void BptrNetRecv(int s, const uint8_t* b, uint64_t len)
        {
            if (len < 6 || b[1] < 1 || b[1] > 3)
                return;
            const int c = b[1], i0 = b[2] | (b[3] << 8), n = b[4];
            if (len < static_cast<uint64_t>(6 + n * 15) || i0 + n > kBptN)
                return;
            for (int j = 0; j < n; j++) {
                const uint8_t* e = b + 6 + j * 15;
                int16_t q[7];
                std::memcpy(q, e + 1, 14);
                if (!e[0])
                    continue;
                float v[7];
                for (int k = 0; k < 3; k++)
                    v[k] = q[k] / 1000.0f;
                for (int k = 3; k < 7; k++)
                    v[k] = q[k] / 32767.0f;
                if (!BptrValid(v))
                    continue;
                std::memcpy(g_bptrp[s][c][i0 + j], v, 28);
                g_bptrpOk[s][c][i0 + j] = 1;
            }
            if (!g_bptrNrx[s]++ || g_bptrNrx[s] % 50 == 0) {
                int nn;
                BptComplete(g_bptrpOk[s][c], &nn);
                TO1_LOG("bptrack: slot %d root chunk %d (clip %d idx %d+%d, that clip now %d samples)\n",
                    s, g_bptrNrx[s], c, i0, n, nn);
            }
        }

        void BpOwnSet(int phase, double now, const char* why)
        {
            if (phase == g_bpo.phase)
                return;
            TO1_LOG("bp: own phase %s -> %s at t %.0f ms (%s)\n",
                kPhaseName[g_bpo.phase & 3], kPhaseName[phase & 3], now, why);
            if (phase == kPhaseIdle
                && (g_bpo.phase == kPhaseClosing || g_bpo.phase == kPhaseAbort))
                BptOpenDone();
            g_bpo.phase = phase;
            g_bpo.tPhase = now;
            g_bpo.seq++;
            g_bpo.tChange = now;
            g_bpo.copies = 2;
            BpSend(now);
            g_bpo.nextSend = now + 100.0;
        }

        int OwnDead()
        {
            const uint64_t d = DC::Call<"character-is-dead?">(SID("player"));
            return d != static_cast<uint64_t>(-1) && (d & 0xff);
        }

        void BpOwnTick(double now)
        {
            static int menuWas;
            const int live = net::player_status::MatchLive() ? 1 : 0;
            const int menu = live
                && (net::gui2::PlayerMenuActive() == 1 || net::gui2::BackpackOpen());
            const int gone = !live || net::player_status::LocalDowned() || OwnDead();
            if (g_bpo.seenEnter != ::engine::backpack_prop::EnterCount()) {
                g_bpo.seenEnter = ::engine::backpack_prop::EnterCount();
                if (g_bpo.phase == kPhaseOpen) {
                    const double d = ::engine::backpack_prop::EnterMs() - g_bpo.tPhase;
                    g_bpo.enterMs = d >= 0 && d < 60000 ? static_cast<uint16_t>(d) : 0xffff;
                }
            }
            if (g_bpo.seenLeave != ::engine::backpack_prop::LeaveCount()) {
                g_bpo.seenLeave = ::engine::backpack_prop::LeaveCount();
                if (g_bpo.phase == kPhaseClosing) {
                    const double d = ::engine::backpack_prop::LeaveMs() - g_bpo.tPhase;
                    g_bpo.leaveMs = d >= 0 && d < 60000 ? static_cast<uint16_t>(d) : 0xffff;
                }
            }
            if (gone) {
                if (g_bpo.phase == kPhaseOpen || g_bpo.phase == kPhaseClosing)
                    BpOwnSet(kPhaseAbort, now,
                        !live ? "match end / not in game" :
                        net::player_status::LocalDowned() ? "downed" : "dead");
                else if (g_bpo.phase == kPhaseAbort && now - g_bpo.tPhase > 1000.0)
                    BpOwnSet(kPhaseIdle, now, "abort sent");
            } else if (menu && !menuWas)
                BpOwnSet(kPhaseOpen, now, "menu opened = intro clip starts");
            else if (!menu && menuWas && g_bpo.phase == kPhaseOpen)
                BpOwnSet(kPhaseClosing, now, "menu closed = outro clip starts");
            else if (g_bpo.phase == kPhaseClosing
                && ((!::engine::backpack_prop::Hid() && now - g_bpo.tPhase > 1500.0)
                    || now - g_bpo.tPhase > 4000.0))
                BpOwnSet(kPhaseIdle, now, "outro over");
            else if (g_bpo.phase == kPhaseAbort && now - g_bpo.tPhase > 1000.0)
                BpOwnSet(kPhaseIdle, now, "abort sent");
            menuWas = menu;
            {
                static int offWas;
                if ((::engine::backpack_prop::Hid() != 0) != offWas) {   // the bag swap moment
                    offWas = ::engine::backpack_prop::Hid() != 0;
                    BpSend(now);
                    g_bpo.tChange = now;
                    g_bpo.copies = 2;
                    g_bpo.nextSend = now + 100.0;
                }
            }
            BptOwnSample(now);
            BptSendTick(now);
            if (now >= g_bpo.nextSend) {   // change copies at +100 / +300 ms, then the period
                BpSend(now);
                const double period =
                    g_bpo.phase == kPhaseOpen || g_bpo.phase == kPhaseClosing ? 500.0 : 3000.0;
                if (g_bpo.copies == 2) {
                    g_bpo.copies = 1;
                    g_bpo.nextSend = g_bpo.tChange + 300.0;
                } else {
                    g_bpo.copies = 0;
                    g_bpo.nextSend = now + period;
                }
            }
        }

        // ==================================================================================
        //  Puppets
        // ==================================================================================
        struct PupState {
            int      phase = kPhaseIdle;
            uint8_t  seq = 0;
            double   rxMs = 0;          // last message (NowMs)
            double   ctRx = 0;          // the owner's clip time (ms in phase) at rx, latency-corrected
            int      clip = kClipNone;
            double   t0 = 0;
            float    dur = 0;           // playing clip, its time 0 (NowMs), length (s)
            const char* name = nullptr;
            uint64_t h = 0;             // puppet handle the clip runs on
            int      warnedNone = 0;
            int      wantOff = 0;       // the owner's last bagoff
            int      pmode = 0;         // 1 = animated TAB prop look, 2 = rigid per-look bag, 3 = satchel
            int      off = 0;           // 1 = we took this puppet's worn bag off and hold the prop
            uint64_t prop = 0;          // held bag prop object (0 = none)
            int      path = 0;
            double   probeT = 0;
            float    lid0[3] = { 0, 0, 0 };
            uint8_t* proc = nullptr;    // its ProcessBackpack (while the prop waits for its clips)
            uint8_t  wasVis[kPartCap] = {};
            float    floorPos[3] = { 0, 0, 0 };
            int      floorOk = 0;
            int      tsrc = 0, tsrcClip = 0;
            double   tsrcT0 = 0;
            int      rsrc = 0, rsrcClip = 0;
            double   rsrcT0 = 0;
            int      ready = 0;
            double   tOff = 0, enterPred = 0;
            float    lastSet[3] = { 0, 0, 0 };
            uint64_t lastProp = 0;
            float    bpoff[3] = { 0, 0, 0 };
            int      bpoffSt = 0;
            double   tHide = 0, tProof = 0;
        };
        PupState g_bpp[kMaxNetPlayers];

        uint64_t PuppetHandle(int slot)
        {
            if (slot < 0 || slot >= kMaxNetPlayers)
                return 0;
            return NetPlayers::BySlot(slot).npcSid;
        }

        float OnewayMs(int slot)
        {
            const int ping = (slot >= 0 && slot < kMaxNetPlayers) ? NetPlayers::BySlot(slot).PingMs() : -1;
            return ping > 0 ? static_cast<float>(ping) * 0.5f : kDefaultOnewayMs;
        }

        // ---- the prop's clip names (ext BpPropClipName) -----------------------------------
        const char* PropClipName(int s, int clip, uint64_t prop)
        {
            static char cache[kMaxNetPlayers][4][96];
            clip &= 3;
            if (!clip)
                return nullptr;
            if (cache[s][clip][0])
                return cache[s][clip];
            const char* list[6];
            int n = 0;
            static char own[96];
            if (clip == (g_bpp[s].clip & 3) && g_bpp[s].name && std::strlen(g_bpp[s].name) < 80) {
                snprintf(own, sizeof own, "%s--backpack", g_bpp[s].name);
                list[n++] = own;
            }
            for (int i = 0; i < 4 && kPropClips[clip][i]; i++)
                list[n++] = kPropClips[clip][i];
            const char* pick = nullptr;
            for (int i = 0; i < n; i++) {
                const float L = ::engine::anim::Len(prop, list[i]);
                TO1_LOG("bp: slot %d prop clip candidate %s: duration %.2f s%s\n",
                    s, list[i], L, L > 0 ? " - USED" : "");
                if (L > 0) {
                    pick = list[i];
                    break;
                }
            }
            if (!pick) {
                pick = list[0];
                TO1_LOG("bp: slot %d no prop clip candidate has a duration - trying %s anyway\n", s, pick);
            }
            snprintf(cache[s][clip], sizeof cache[s][clip], "%s", pick);
            return cache[s][clip];
        }

        // prop clip in step with the puppet's craft clip `clip` at phase ph
        void PropClip(int s, int clip, float ph, float blend)
        {
            if (g_bpp[s].pmode != 1 || !g_bpp[s].prop || !(clip & 3))
                return;
            const char* nm = PropClipName(s, clip, g_bpp[s].prop);
            const uint64_t r = ::engine::anim::AnimateFrom(g_bpp[s].prop, nm, blend, ph);
            static int nl;
            if (nl < 40) {
                nl++;
                TO1_LOG("bp: slot %d bag prop %s from phase %.2f blend %.2f -> %llx\n",
                    s, nm, ph, blend, static_cast<unsigned long long>(r));
            }
        }

        void PropPose(uint64_t o, const float* pos, const float* loc8)
        {
            const float yaw = 2.0f * std::atan2(loc8[5], loc8[7]);
            const float q[4] = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
            ::engine::object::SetLocator(o, pos, q);
        }

        void PropOnBody(uint64_t o, const float* loc8)
        {
            ::engine::object::SetLocator(o, loc8, loc8 + 4);
        }

        uint64_t SpawnProp(const char* art)
        {
            const uint64_t o = ENTITY::SPAWN_OBJECT(SID(art), SID("SimpleAttachable"));
            return (o && o != ~0ULL) ? o : 0;
        }

        // ---------------------------------------------------------------------------------
        //  The engine prop spawn (ext BpSpawnReal + the bp_pick detour + BpSendMsg2)
        //
        // A SimpleAttachable spawned from a look name has no anim set, so the animated TAB
        // prop cannot be built that way. The engine's own ProcessBackpack spawn (exe 0xafd7f0,
        // PlayerBackpackAnimCtrl::SpawnProp) builds a prop with the backpack-abby skeleton and
        // anim set. We run it on a COPY of our own controller (so +0x118 is a real Player and
        // the vfuncs / parent link are valid) with the puppet's locator, and detour the art
        // pick (exe 0xafd9f6) so it answers our forced art. The prop is then re-parented /
        // unparented with the engine's set-parent message (0x14cfc60 + the event sender
        // 0x15734d0) and driven by us every frame.
        //
        // The detour: the ext hand-built an RWX trampoline over the 30-byte site. The mod uses
        // a safetyhook MidHook at the site's `cmp [rsp+198h], r15` (offset +16): the callback
        // rewrites the stack slot with BpPick's answer, then the original cmp / je run
        // unchanged (0 -> skip the spawn, non-zero -> spawn). The two restored `mov`s at
        // +0 / +8 run before the hook site and are untouched.
        // ---------------------------------------------------------------------------------

        constexpr uintptr_t kRvaRealSpawn = 0xafd7f0;   // PlayerBackpackAnimCtrl::SpawnProp
        constexpr uintptr_t kRvaPickSite  = 0xafd9f6;   // mov rdi / mov rsi / cmp / je (30 B)
        constexpr uintptr_t kRvaPickCmp   = kRvaPickSite + 16;   // the cmp [rsp+198h], r15
        constexpr uintptr_t kRvaSendMsg   = 0x15734d0;  // the process event sender
        constexpr uintptr_t kRvaSetParent = 0x14cfc60;  // set-parent argument builder

        // ext InstallBackpackPropHook expect[30]
        constexpr uint8_t kPickPrologue[30] = {
            0x48, 0x8b, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00,
            0x48, 0x8b, 0xb4, 0x24, 0x60, 0x02, 0x00, 0x00,
            0x4c, 0x39, 0xbc, 0x24, 0x98, 0x01, 0x00, 0x00,
            0x0f, 0x84, 0x07, 0x02, 0x00, 0x00 };

        constexpr uint64_t kMsgSetParent  = 0xe93d6ecbdf018772ULL;
        constexpr uint64_t kMsgShowObject = 0xc2f7fb66d732410bULL;   // what EnterBackpack sends

        volatile uint64_t g_spawnArt   = 0;        // ext g_bpSpawnArt: forced art while we spawn
        volatile uint8_t* g_lastCtrl   = nullptr;  // ext g_bpCtrl (bookkeeping for the own side)
        volatile uint8_t* g_lastPlayer = nullptr;  // ext g_bpPlayer
        volatile uint32_t g_ctrlGen    = 0;        // ext g_bpCtrlGen
        SafetyHookMid     g_pickHook;
        int               g_pickState = 0;         // 0 untried, 1 installed, -1 failed

        uintptr_t ExeBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

        // ext BpPick: the detour answers the spawn art. Per-look (the faction-look tables) is
        // gated: without it the engine's own pick is returned unchanged, but a forced art
        // (g_spawnArt, set by SpawnReal) always wins.
        uint64_t BpPick(uint8_t* player, uint64_t art, uint8_t* ctrl)
        {
            const uint64_t forced = g_spawnArt;
            if (forced)
                return forced;
            g_lastCtrl = ctrl;
            g_lastPlayer = player;
            g_ctrlGen = g_ctrlGen + 1;
            return art;   // (per-look gated: the engine's own art)
        }

        void PickHook(SafetyHookContext& ctx)
        {
            // ext BpPick(player, art, ctrl): rcx = [r14+0x118], rdx = [rsp+0x198], r8 = r14.
            if (!ctx.r14 || !::engine::ptr::Readable(ctx.r14 + 0x118, 8)
                || !::engine::ptr::Readable(ctx.rsp + 0x198, 8))
                return;
            uint8_t* player = *reinterpret_cast<uint8_t**>(ctx.r14 + 0x118);
            const uint64_t art = *reinterpret_cast<uint64_t*>(ctx.rsp + 0x198);
            *reinterpret_cast<uint64_t*>(ctx.rsp + 0x198) =
                BpPick(player, art, reinterpret_cast<uint8_t*>(ctx.r14));
        }

        void InstallPickHook()
        {
            if (g_pickState)
                return;
            g_pickState = -1;   // assume failure until proven otherwise
            HMODULE exe = GetModuleHandleW(nullptr);
            if (!exe)
                return;
            uint8_t* site = reinterpret_cast<uint8_t*>(exe) + kRvaPickSite;
            if (std::memcmp(site, kPickPrologue, sizeof kPickPrologue) != 0) {
                TO1_LOG("bp: backpack prop pick site differs (exe is not 1.6.10721?) - engine prop spawn off\n");
                return;
            }
            g_pickHook = safetyhook::create_mid(reinterpret_cast<uint8_t*>(exe) + kRvaPickCmp, PickHook);
            if (!g_pickHook) {
                TO1_LOG("bp: backpack prop pick hook failed - engine prop spawn off\n");
                return;
            }
            g_pickState = 1;
            TO1_LOG("bp: backpack prop pick hook installed (rva 0x%X)\n",
                static_cast<unsigned>(kRvaPickCmp));
        }

        bool PickReady()
        {
            InstallPickHook();
            return g_pickState == 1;
        }

        // ext BpSpawnReal: run the engine's own prop spawn on a copy of our controller with the
        // puppet's locator. Returns the prop handle (0 = none) and hands back its process.
        uint64_t SpawnReal(const float* loc8, uint64_t art, uint8_t** procOut)
        {
            if (!PickReady())
                return 0;
            uint8_t* me = ::engine::backpack_prop::Me();
            uint8_t* c = me ? ::engine::backpack_prop::CtrlOf(me) : nullptr;
            if (!c || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(c), 0x1f0))
                return 0;
            alignas(32) static uint8_t cc[0x300];
            std::memcpy(cc, c, 0x1f0);
            *reinterpret_cast<uint64_t*>(cc + 0x198) = 0;
            *reinterpret_cast<uint32_t*>(cc + 0x1a0) = 0;
            cc[0x190] = 1;
            alignas(32) float lc[8];
            std::memcpy(lc, loc8, 32);
            g_spawnArt = art ? art : SID("backpack-abby");
            using Fn = void(__fastcall*)(void*, const float*);
            reinterpret_cast<Fn>(ExeBase() + kRvaRealSpawn)(cc, lc);
            g_spawnArt = 0;
            uint8_t* slot = *reinterpret_cast<uint8_t**>(cc + 0x198);
            if (!slot || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(slot), 16))
                return 0;
            uint8_t* proc = *reinterpret_cast<uint8_t**>(slot + 8);
            if (!proc || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(proc), 0xd0)
                || *reinterpret_cast<uint32_t*>(proc + 0x94) != *reinterpret_cast<uint32_t*>(cc + 0x1a0))
                return 0;
            if (procOut)
                *procOut = proc;
            return *reinterpret_cast<uint64_t*>(proc + 0xc0);
        }

        // ext BpSendMsg2: send a process event, dropping it when the cached process is stale
        // (its handle slot no longer points back at it -- the crash the ext saw at exe+1573557).
        void SendMsg2(uint8_t* proc, uint64_t msg, void* arg)
        {
            if (!proc || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(proc), 0xd0))
                return;
            uint8_t* slot = *reinterpret_cast<uint8_t**>(proc + 0x80);
            if (!slot || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(slot), 16)
                || *reinterpret_cast<uint8_t**>(slot + 8) != proc
                || !*reinterpret_cast<uint32_t*>(proc + 0x94)) {
                static int nl;
                if (nl < 20) {
                    nl++;
                    TO1_LOG("bp: event %llx to a stale prop process %p dropped (slot %p)\n",
                        static_cast<unsigned long long>(msg),
                        static_cast<void*>(proc), static_cast<void*>(slot));
                }
                return;
            }
            alignas(16) uint8_t z[6][16] = {};
            alignas(16) uint8_t tgt[16] = {};
            alignas(16) uint8_t out[0x40] = {};
            *reinterpret_cast<uint64_t*>(tgt) = *reinterpret_cast<uint64_t*>(proc + 0x80);
            *reinterpret_cast<uint32_t*>(tgt + 8) = *reinterpret_cast<uint32_t*>(proc + 0x94);
            *reinterpret_cast<uint32_t*>(tgt + 12) = 0;
            using Fn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*, void*, void*, void*);
            reinterpret_cast<Fn>(ExeBase() + kRvaSendMsg)(
                out, z[0], msg, tgt, arg ? arg : z[1], z[2], z[3], z[4]);
        }

        // ext BpSendMsg: the no-argument form.
        void SendMsg(uint8_t* proc, uint64_t msg)
        {
            SendMsg2(proc, msg, nullptr);
        }

        // ext BpPropUnparent: the spawn parents the prop to our player; re-send set-parent with
        // a null handle so the prop's per-frame update never reads our backpack state.
        void PropUnparent(uint8_t* proc)
        {
            alignas(16) uint8_t nul[16] = {};
            alignas(16) uint8_t arg[0x20] = {};
            using Fn = void*(__fastcall*)(void*, void*);
            reinterpret_cast<Fn>(ExeBase() + kRvaSetParent)(arg, nul);
            SendMsg2(proc, kMsgSetParent, arg);
        }

        // ext BpPropParentPuppet: the same set-parent message with the puppet as the target, so
        // the prop's state graph is driven by the puppet's clip column.
        int PropParentPuppet(uint8_t* proc, uint64_t puppetH)
        {
            if (!proc || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(proc), 0xd0))
                return 0;
            uint8_t* pp = puppetH
                ? reinterpret_cast<uint8_t*>(DcManager::LookupObject(puppetH)) : nullptr;
            if (!pp || !::engine::ptr::Readable(reinterpret_cast<uintptr_t>(pp), 0xa0))
                return 0;
            alignas(16) uint8_t tgt[16] = {};
            alignas(16) uint8_t arg[0x20] = {};
            *reinterpret_cast<uint64_t*>(tgt) = *reinterpret_cast<uint64_t*>(pp + 0x80);
            *reinterpret_cast<uint32_t*>(tgt + 8) = *reinterpret_cast<uint32_t*>(pp + 0x94);
            using Fn = void*(__fastcall*)(void*, void*);
            reinterpret_cast<Fn>(ExeBase() + kRvaSetParent)(arg, tgt);
            SendMsg2(proc, kMsgSetParent, arg);
            return 1;
        }

        bool PupOffDisabled() { return ::engine::flag::Present("t2r-no-puppet-bag-off.txt"); }
        bool RigidOptin()     { return ::engine::flag::Present("t2r-puppet-bag-rigid.txt"); }

        void PupPropKill(int s, int live)
        {
            if (g_bpp[s].prop && live) {
                const uint64_t o = g_bpp[s].prop;
                ENTITY::KILL_ENTITY(o);
            }
            g_bpp[s].prop = 0;
            g_bpp[s].floorOk = 0;
            g_bpp[s].pmode = 0;
            g_bpp[s].bpoffSt = 0;
            g_bpp[s].ready = 0;
        }

        // bag back on the puppet: the parts that were shown at the take-off, the prop gone
        void PupBagOn(int s, uint64_t h, int live, const char* why)
        {
            if (!g_bpp[s].off && !g_bpp[s].prop)
                return;
            uint8_t* dc = (live && h) ? ::engine::object::DrawControl(h) : nullptr;
            if (dc)
                for (int i = 0; i < ::engine::backpack_prop::PartCount() && i < kPartCap; i++)
                    if (g_bpp[s].wasVis[i])
                        ::engine::object::ShowMesh(dc, ::engine::backpack_prop::PartSid(i));
            PupPropKill(s, live);
            if (g_bpp[s].off)
                TO1_LOG("bp: slot %d puppet bag back on (%s) at t %.0f ms\n", s, why, NowMs());
            g_bpp[s].off = 0;
            ::engine::armor_rig::SetPupBagOff(s, false);   // the armour rig re-asserts its copy / original pair from here
        }

        // ext BpPupRow / BpPropLookOf: the puppet look's pool row (FACTION_LOOKS /
        // FACTION_LOOKS_WINTER) and the per-look TAB prop look. The pool tables are ported
        // (engine/armor-rig), and PropLookOf now reads the ported per-look table
        // (engine::backpack_prop::LookProp / PropLookName). PupRow still needs g_flSpawnStr[s]
        // (the look string each puppet was last spawned with, set by the unported faction-look
        // spawn system), so it stays gated: no row -> the rigid prop falls back to the satchel
        // actor (mode 3).
        int PupRow(int s, int* pool, int* idx)
        {
            (void)s; (void)pool; (void)idx;
            return 0;
        }

        const char* PropLookOf(int pool, int idx)
        {
            const char* lk = ::engine::armor_rig::FactionLook(pool, idx);
            if (!lk)
                return nullptr;
            const int i = ::engine::backpack_prop::LookProp(SID(lk));
            return i >= 0 ? ::engine::backpack_prop::PropLookName(i) : nullptr;
        }

        // the owner took the bag off: the prop first, then the worn parts go (no prop = no change)
        // go = 0: get the prop ready but keep the worn bag on; go = 1: the swap
        void PupBagOff(int s, uint64_t h, double now, int go)
        {
            static int bad, abad;
            uint8_t* dc = ::engine::object::DrawControl(h);
            if (!dc || bad >= 3)
                return;
            float hp[3], hq[4], lb[16];
            const bool haveLoc = ::engine::object::EntityWorld(h, lb);
            if (!haveLoc)
                return;
            int pool = 0, idx = 0;
            const int row = PupRow(s, &pool, &idx);
            const char* plook = row ? PropLookOf(pool, idx) : nullptr;
            uint64_t o = 0;
            int mode = 0;
            if (abad < 3) {
                if (!g_bpp[s].clip)
                    return;    // the craft clip has not started here yet
                if (g_bpp[s].prop) {
                    // the readied animated prop: probe whether the engine has started driving
                    // it (its lid joint moved) before we trust it; otherwise keep posing it on
                    // the body until the probe timeout, then give up and fall back.
                    o = g_bpp[s].prop;
                    float jp[3] = { g_bpp[s].lid0[0], g_bpp[s].lid0[1], g_bpp[s].lid0[2] }, jq[4];
                    uint64_t r = 0;
                    int moved = g_bpp[s].ready;
                    if (!moved && g_bpp[s].probeT > 0 && now - g_bpp[s].probeT >= 66.0
                        && ::engine::object::JointLocator(o, "lid_a", jp, jq)) {
                        const float d = std::fabs(jp[0] - g_bpp[s].lid0[0])
                            + std::fabs(jp[1] - g_bpp[s].lid0[1])
                            + std::fabs(jp[2] - g_bpp[s].lid0[2]);
                        moved = d > 0.003f;
                        r = 1;
                        if (!moved)
                            g_bpp[s].probeT = 0;
                    }
                    if (!moved && g_bpp[s].probeT <= 0) {
                        r = ::engine::anim::AnimateFrom(o, PropClipName(s, kClipIntro, o), 0.0f, 0.5f);
                        if (r && r != ~0ULL && ::engine::object::JointLocator(o, "lid_a", jp, jq)) {
                            std::memcpy(g_bpp[s].lid0, jp, 12);
                            g_bpp[s].probeT = now;
                        }
                    }
                    if (!moved && g_bpp[s].path == 0 && now - g_bpp[s].tProof >= 700.0
                        && !::engine::flag::Present("t2r-no-puppet-bag-parent.txt")) {
                        g_bpp[s].path = PropParentPuppet(g_bpp[s].proc, h) ? 2 : 1;
                        g_bpp[s].probeT = 0;
                    }
                    if (moved && !go) {
                        g_bpp[s].ready = 1;
                        PropOnBody(o, lb);
                        return;
                    }
                    if (moved) {
                        g_bpp[s].ready = 0;
                        // the spawn-time process pointer is stale by now (SendMsg2's
                        // stale-process guard would drop the ShowObject), so re-resolve the
                        // live process from the prop's handle first; ShowObject (what
                        // EnterBackpack sends) is what makes the spawned-hidden prop visible.
                        uint8_t* pp = reinterpret_cast<uint8_t*>(DcManager::LookupObject(o));
                        if (pp && ::engine::ptr::Readable(reinterpret_cast<uintptr_t>(pp), 0xd0)
                            && *reinterpret_cast<uint64_t*>(pp + 0xc0) == o)
                            g_bpp[s].proc = pp;
                        SendMsg(g_bpp[s].proc, kMsgShowObject);
                        mode = 1;
                        g_bpp[s].prop = 0;
                        g_bpp[s].probeT = 0;
                    } else if (now - g_bpp[s].tProof < 2000.0) {
                        PropOnBody(o, lb);
                        return;
                    } else {
                        ENTITY::KILL_ENTITY(o);
                        o = 0;
                        g_bpp[s].prop = 0;
                        g_bpp[s].probeT = 0;
                        abad++;
                    }
                } else {
                    uint8_t* proc = nullptr;
                    o = SpawnReal(lb, plook ? SID(plook) : 0, &proc);
                    if (o) {
                        PropUnparent(proc);
                        TO1_LOG("bp: slot %d real ProcessBackpack (art %s, parent cleared) spawned hidden: proc %p handle %llx\n",
                            s, plook ? plook : "backpack-abby", static_cast<void*>(proc),
                            static_cast<unsigned long long>(o));
                        g_bpp[s].prop = o;
                        g_bpp[s].proc = proc;
                        g_bpp[s].tProof = now;
                        g_bpp[s].probeT = 0;
                        g_bpp[s].pmode = 0;
                        g_bpp[s].path = 0;
                        PropOnBody(o, lb);
                        return;
                    } else {
                        TO1_LOG("bp: slot %d real ProcessBackpack spawn gave no object (try %d) - the bag stays on the back\n", s, ++abad);
                    }
                }
            }
            if (!mode && RigidOptin()) {   // opt-in only (night21 showed the rigid bag floating at hip height)
                if (!::engine::object::JointLocator(h, "r_hand_prop_attachment", hp, hq))
                    return;
                const char* art = row ? "mp-t2r-hb-unknown" : kSatActor;   // (per-look HB actor gated)
                o = SpawnProp(art);
                if (o)
                    mode = row ? 2 : 3;
                if (!o) {
                    TO1_LOG("bp: slot %d rigid bag prop %s failed (try %d)\n", s, art, ++bad);
                    return;
                }
            }
            if (!mode)
                return;    // nothing animated: the worn bag stays on the back (old behaviour)
            g_bpp[s].prop = o;
            g_bpp[s].pmode = mode;
            g_bpp[s].floorOk = 0;
            g_bpp[s].bpoffSt = 0;
            if (mode == 1) {
                PropOnBody(o, lb);
                const double el = now - g_bpp[s].t0;
                float ph = g_bpp[s].dur > 0
                    ? static_cast<float>(el / (g_bpp[s].dur * 1000.0)) : 0.0f;
                ph -= static_cast<float>(static_cast<int>(ph));
                if (ph > 0.98f)
                    ph = 0.98f;
                if (ph < 0)
                    ph = 0;
                PropClip(s, g_bpp[s].clip, ph, 0.0f);
            } else {
                hp[1] -= 0.45f;    // the bag hangs from the hand (origin = underside)
                PropPose(o, hp, lb);
            }
            for (int i = 0; i < ::engine::backpack_prop::PartCount() && i < kPartCap; i++) {
                const StringId64 sid = ::engine::backpack_prop::PartSid(i);
                const int ix = ::engine::object::MeshIndex(dc, sid);
                g_bpp[s].wasVis[i] = (ix >= 0 && !::engine::object::MeshHidden(dc, ix)) ? 1 : 0;
                if (ix >= 0)
                    ::engine::object::HideMesh(dc, sid);
            }
            g_bpp[s].off = 1;
            ::engine::armor_rig::SetPupBagOff(s, true);
            g_bpp[s].tHide = now;
            g_bpp[s].tOff = now;
            TO1_LOG("bp: slot %d puppet bag OFF at t %.0f ms - worn parts hidden, %s %llx\n",
                s, now,
                mode == 1 ? "animated bag (TAB prop look, clips in step)"
                          : mode == 2 ? "rigid per-look bag at the hand" : "satchel at the hand",
                static_cast<unsigned long long>(o));
        }

        // ---- the open-offset / track replay (ext BpPupOpenOffset) -------------------------
        void PupOpenOffset(int s, uint64_t h, const float* loc, double now)
        {
            float c[16];
            std::memcpy(c, loc, sizeof c);
            {   // root track: prop root = body locator * owner's recorded root-relative locator
                const int cl = g_bpp[s].clip;
                double ems = now - g_bpp[s].t0;
                if (cl == kClipLoop)
                    ems = std::fmod(ems, kLoopMs);
                if (cl >= 1 && cl <= 3
                    && (g_bpp[s].rsrcClip != cl || g_bpp[s].rsrcT0 != g_bpp[s].t0)) {
                    BptLoadHeader();
                    int np = 0, nl = 0;
                    const int pk = BptComplete(g_bptrpOk[s][cl], &np);
                    const int lk = BptComplete(g_bptrOk[cl], &nl);
                    g_bpp[s].rsrc = BptOff() ? 0 : pk ? 1 : lk ? 2 : 0;
                    g_bpp[s].rsrcClip = cl;
                    g_bpp[s].rsrcT0 = g_bpp[s].t0;
                    static int nlog;
                    if (nlog < 60) {
                        nlog++;
                        TO1_LOG("bptrack: slot %d clip %s root source %s (owner's root track %d samples, own %d)\n",
                            s, kClipName[cl & 3],
                            g_bpp[s].rsrc == 1 ? "OWNER ROOT" : g_bpp[s].rsrc == 2 ? "own root" : "none - spinec track / follow",
                            np, nl);
                    }
                }
                float r7[7];
                int got = 0;
                if (cl >= 1 && cl <= 3 && g_bpp[s].rsrc == 1)
                    got = BptrGetT(g_bptrp[s][cl], g_bptrpOk[s][cl], ems, r7);
                else if (cl >= 1 && cl <= 3 && g_bpp[s].rsrc == 2)
                    got = BptrGetT(g_bptr[cl], g_bptrOk[cl], ems, r7);
                if (got) {
                    float pw[3], q[4];
                    Qrotv(loc + 4, r7, pw);
                    BpQmul(loc + 4, r7 + 3, q);
                    const float qn = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    if (qn > 0.5f) {
                        c[0] = loc[0] + pw[0];
                        c[1] = loc[1] + pw[1];
                        c[2] = loc[2] + pw[2];
                        c[3] = 1.0f;
                        for (int k = 0; k < 4; k++)
                            c[4 + k] = q[k] / qn;
                        PropOnBody(g_bpp[s].prop, c);
                        return;
                    }
                }
            }
            {   // replay of our own recorded open / close animation (bpt_*)
                float tr[3];
                double ems = now - g_bpp[s].t0;
                if (g_bpp[s].clip == kClipLoop)
                    ems = std::fmod(ems, kLoopMs);
                const int cl = g_bpp[s].clip;
                if (cl >= 1 && cl <= 3
                    && (g_bpp[s].tsrcClip != cl || g_bpp[s].tsrcT0 != g_bpp[s].t0)) {
                    BptLoadHeader();
                    int np = 0, nl = 0;
                    const int pk = BptComplete(g_bptpOk[s][cl], &np);
                    const int lk = BptComplete(g_bptOk[cl], &nl);
                    g_bpp[s].tsrc = BptOff() ? 0 : pk ? 1 : lk ? 2 : 0;
                    g_bpp[s].tsrcClip = cl;
                    g_bpp[s].tsrcT0 = g_bpp[s].t0;
                    static int nlog;
                    if (nlog < 60) {
                        nlog++;
                        TO1_LOG("bptrack: slot %d clip %s source %s (owner's track %d samples, own %d)\n",
                            s, kClipName[cl & 3],
                            g_bpp[s].tsrc == 1 ? "OWNER TRACK" : g_bpp[s].tsrc == 2 ? "own track" : "none - spinec follow",
                            np, nl);
                    }
                }
                int got = 0;
                if (cl >= 1 && cl <= 3 && g_bpp[s].tsrc == 1)
                    got = BptGetT(g_bptp[s][cl], g_bptpOk[s][cl], ems, tr);
                else if (cl >= 1 && cl <= 3 && g_bpp[s].tsrc == 2)
                    got = BptGet(cl, ems, tr);
                if (got) {
                    const float yw = 2.0f * std::atan2(loc[5], loc[7]);
                    const float cs = std::cos(yw), sn = std::sin(yw), pj[3] = { 0, 0, 0 }, pq[4] = { 0, 0, 0, 0 };
                    float w[3] = { loc[0] + tr[0] * cs + tr[2] * sn, loc[1] + tr[1],
                                   loc[2] - tr[0] * sn + tr[2] * cs };
                    if (g_bpp[s].lastProp == g_bpp[s].prop
                        && ::engine::object::JointLocator(g_bpp[s].prop, "spinec", const_cast<float*>(pj), const_cast<float*>(pq))) {
                        for (int k = 0; k < 3; k++)
                            c[k] = g_bpp[s].lastSet[k] + w[k] - pj[k];
                        const float dx = c[0] - loc[0], dy = c[1] - loc[1], dz = c[2] - loc[2];
                        const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (dl > 1.0f)
                            for (int k = 0; k < 3; k++)
                                c[k] = loc[k] + (c[k] - loc[k]) / dl;
                    }
                    std::memcpy(g_bpp[s].lastSet, c, 12);
                    g_bpp[s].lastProp = g_bpp[s].prop;
                    PropOnBody(g_bpp[s].prop, c);
                    return;
                }
            }
            if (g_bpp[s].bpoffSt == 0 && g_bpp[s].clip) {
                PropOnBody(g_bpp[s].prop, loc);   // pose it uncorrected, then read the joints next tick
                g_bpp[s].bpoffSt = -1;
                return;
            }
            if (g_bpp[s].bpoffSt == -1) {
                float bp_[3], bq[4], pp[3], pq[4];
                const char* jn = "spinec";
                int ok = ::engine::object::JointLocator(h, jn, bp_, bq)
                    && ::engine::object::JointLocator(g_bpp[s].prop, jn, pp, pq);
                if (!ok) {
                    jn = "root";
                    ok = ::engine::object::JointLocator(h, jn, bp_, bq)
                        && ::engine::object::JointLocator(g_bpp[s].prop, jn, pp, pq);
                }
                if (ok) {
                    float d[3] = { bp_[0] - pp[0], bp_[1] - pp[1], bp_[2] - pp[2] };
                    const float L = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (L > 0.15f) {
                        const float k = 0.15f / L;
                        d[0] *= k; d[1] *= k; d[2] *= k;
                    }
                    const float yw = 2.0f * std::atan2(loc[5], loc[7]);
                    const float cs = std::cos(yw), sn = std::sin(yw);
                    const float wx = loc[0] + d[0] - bp_[0], wz = loc[2] + d[2] - bp_[2];
                    g_bpp[s].bpoff[0] = wx * cs - wz * sn;
                    g_bpp[s].bpoff[2] = wx * sn + wz * cs;
                    g_bpp[s].bpoff[1] = d[1];
                    g_bpp[s].bpoffSt = ::engine::flag::Present("t2r-no-bp-offset-fix.txt") ? 2 : 1;
                } else {
                    g_bpp[s].bpoffSt = 2;
                }
            }
            if (g_bpp[s].bpoffSt == 1) {
                float bj[3], bq[4];
                if (::engine::object::JointLocator(h, "spinec", bj, bq)) {
                    const float yw = 2.0f * std::atan2(loc[5], loc[7]);
                    const float cs = std::cos(yw), sn = std::sin(yw);
                    float x = bj[0] + g_bpp[s].bpoff[0] * cs + g_bpp[s].bpoff[2] * sn;
                    float z = bj[2] - g_bpp[s].bpoff[0] * sn + g_bpp[s].bpoff[2] * cs;
                    const float dx = x - loc[0], dz = z - loc[2];
                    const float dl = std::sqrt(dx * dx + dz * dz);
                    if (dl > 0.6f) {
                        x = loc[0] + dx * 0.6f / dl;
                        z = loc[2] + dz * 0.6f / dl;
                    }
                    c[0] = x;
                    c[2] = z;
                }
                c[1] += g_bpp[s].bpoff[1];
            }
            PropOnBody(g_bpp[s].prop, c);
        }

        // every frame while off: animated = on the puppet's locator; rigid = hand (intro / outro)
        // or floor (loop); the hidden parts re-asserted every 200 ms
        void PupBagTick(int s, uint64_t h, double now)
        {
            float hp[3], hq[4], lb[16];
            const bool haveLoc = ::engine::object::EntityWorld(h, lb);
            if (!haveLoc)
                return;
            if (g_bpp[s].pmode == 1) {
                PupOpenOffset(s, h, lb, now);
            } else if (g_bpp[s].clip == kClipLoop) {
                if (!g_bpp[s].floorOk) {
                    if (!::engine::object::JointLocator(h, "r_hand_prop_attachment", hp, hq))
                        return;
                    g_bpp[s].floorPos[0] = hp[0];
                    g_bpp[s].floorPos[2] = hp[2];
                    g_bpp[s].floorPos[1] = lb[1] + 0.004f;
                    g_bpp[s].floorOk = 1;
                }
                PropPose(g_bpp[s].prop, g_bpp[s].floorPos, lb);
            } else {
                g_bpp[s].floorOk = 0;
                if (::engine::object::JointLocator(h, "r_hand_prop_attachment", hp, hq)) {
                    hp[1] -= 0.45f;
                    PropPose(g_bpp[s].prop, hp, lb);
                }
            }
            if (now - g_bpp[s].tHide > 200.0) {
                g_bpp[s].tHide = now;
                uint8_t* dc = ::engine::object::DrawControl(h);
                if (dc)
                    for (int i = 0; i < ::engine::backpack_prop::PartCount() && i < kPartCap; i++) {
                        const StringId64 sid = ::engine::backpack_prop::PartSid(i);
                        const int ix = ::engine::object::MeshIndex(dc, sid);
                        if (ix >= 0 && !::engine::object::MeshHidden(dc, ix))
                            ::engine::object::HideMesh(dc, sid);
                    }
            }
        }

        void PupStop(int s, float blend, const char* why)
        {
            // (no PsyncStop here: the mod's anim replication owns the puppet body stop -- the
            // ext stopped its own psync clip, which we do not play.)
            (void)blend;
            if (g_bpp[s].clip)
                TO1_LOG("bp: slot %d puppet %s stopped (%s) at t %.0f ms\n",
                    s, kClipName[g_bpp[s].clip & 3], why, NowMs());
            g_bpp[s].clip = kClipNone;
            g_bpp[s].name = nullptr;
        }

        // start `clip` on the puppet at clip time ct (ms); 0 = nothing playable. The body clip
        // itself is replayed by net/anim; here we only resolve the clip name / length (the clip
        // clock) and drive the prop.
        int PupPlay(int s, uint64_t h, int clip, double ct, float blend, double now)
        {
            const char* nm = nullptr;
            float dur = 0;
            for (int i = 0; i < 4 && kClips[clip][i] && dur <= 0; i++)
                if ((dur = ::engine::anim::Len(h, kClips[clip][i])) > 0)
                    nm = kClips[clip][i];
            if (!nm) {
                if (!g_bpp[s].warnedNone++)
                    TO1_LOG("bp: slot %d puppet %llx knows no %s craft clip (%s / %s) - no backpack anim on it\n",
                        s, static_cast<unsigned long long>(h), kClipName[clip],
                        kClips[clip][0], kClips[clip][1]);
                return 0;
            }
            if (ct < 0)
                ct = 0;
            float ph = static_cast<float>(ct / (dur * 1000.0));
            if (clip == kClipLoop)
                ph -= static_cast<float>(static_cast<int>(ph));
            if (ph > 0.98f)
                ph = 0.98f;
            g_bpp[s].clip = clip;
            g_bpp[s].name = nm;
            g_bpp[s].dur = dur;
            g_bpp[s].h = h;
            g_bpp[s].t0 = now - static_cast<double>(ph) * dur * 1000.0;
            if (g_bpp[s].pmode == 1)
                PropClip(s, clip, ph, blend);
            TO1_LOG("bp: slot %d puppet %s %s (%.2f s) from %.0f ms (phase %.2f), blend %.2f, at t %.0f ms; worn bag stays on the puppet\n",
                s, kClipName[clip], nm, dur, static_cast<double>(ph) * dur * 1000.0, ph, blend, now);
            return 1;
        }

        // bring the puppet to what the owner's phase / clip time says (late joiner, lost packets)
        void PupSync(int s, uint64_t h, double now)
        {
            const double ct = g_bpp[s].ctRx + (now - g_bpp[s].rxMs);
            const int ph = g_bpp[s].phase;
            if (ph == kPhaseOpen) {
                if (g_bpp[s].clip == kClipIntro || g_bpp[s].clip == kClipLoop)
                    return;
                if (g_bpp[s].clip == kClipOutro) {
                    PupPlay(s, h, kClipIntro, 0, kPropBlend, now);
                    return;
                }
                float di = ::engine::anim::Len(h, kClips[kClipIntro][0]);
                if (di <= 0)
                    di = ::engine::anim::Len(h, kClips[kClipIntro][1]);
                if (di > 0 && ct >= di * 1000.0 - 150.0)
                    PupPlay(s, h, kClipLoop, ct - di * 1000.0, kPropBlend, now);
                else
                    PupPlay(s, h, kClipIntro, ct, kPropBlend, now);
            } else if (ph == kPhaseClosing) {
                if (g_bpp[s].clip == kClipOutro)
                    return;
                float d = ::engine::anim::Len(h, kClips[kClipOutro][0]);
                if (d <= 0)
                    d = ::engine::anim::Len(h, kClips[kClipOutro][1]);
                if (g_bpp[s].clip == kClipNone && (d <= 0 || ct >= d * 1000.0 - 250.0))
                    return;
                PupPlay(s, h, kClipOutro, ct, 0.2f, now);
            } else if (ph == kPhaseAbort) {
                PupStop(s, 0.2f, "owner aborted (death / down / match end)");
            } else if (ph == kPhaseIdle
                && (g_bpp[s].clip == kClipIntro || g_bpp[s].clip == kClipLoop)) {
                PupPlay(s, h, kClipOutro, 0, 0.2f, now);
            }
        }

        void PupTick(double now)
        {
            const int live = net::player_status::MatchLive() ? 1 : 0;
            const int me = net::session::MySlot();
            for (int s = 0; s < kMaxNetPlayers; s++) {
                if (s == me)
                    continue;
                if (!g_bpp[s].clip && g_bpp[s].phase == kPhaseIdle && !g_bpp[s].off && !g_bpp[s].prop)
                    continue;
                const uint64_t h = live ? PuppetHandle(s) : 0;
                if ((g_bpp[s].clip || g_bpp[s].off) && g_bpp[s].h && g_bpp[s].h != h) {
                    TO1_LOG("bp: slot %d puppet changed %llx -> %llx mid %s - state cleared\n",
                        s, static_cast<unsigned long long>(g_bpp[s].h),
                        static_cast<unsigned long long>(h), kClipName[g_bpp[s].clip & 3]);
                    g_bpp[s].clip = kClipNone;
                    g_bpp[s].name = nullptr;
                    g_bpp[s].phase = kPhaseIdle;
                    PupPropKill(s, live);
                    g_bpp[s].off = 0;
                    ::engine::armor_rig::SetPupBagOff(s, false);
                    g_bpp[s].wantOff = 0;
                    continue;
                }
                if (!h) {
                    g_bpp[s].phase = kPhaseIdle;
                    PupPropKill(s, live);
                    g_bpp[s].off = 0;
                    ::engine::armor_rig::SetPupBagOff(s, false);
                    g_bpp[s].wantOff = 0;
                    continue;
                }
                // A puppet in a paired move (execution / pull-out) plays no craft clip: the pair
                // owns its body.
                const char* clr =
                    !live ? "match end"
                    : net::player_status::PuppetDowned(s) ? "downed"
                    : net::player_status::PuppetDeadHere(s) ? "dead"
                    : gameplay::melee::PuppetFrozenForExecution(s) ? "in a paired move"
                    : nullptr;
                if (clr) {
                    PupStop(s, 0.2f, clr);
                    PupBagOn(s, h, live, clr);
                    g_bpp[s].phase = kPhaseIdle;
                    g_bpp[s].wantOff = 0;
                    continue;
                }
                if ((g_bpp[s].phase == kPhaseOpen || g_bpp[s].phase == kPhaseClosing)
                    && now - g_bpp[s].rxMs > kPupStaleMs) {
                    TO1_LOG("bp: slot %d no backpack message for %.0f ms while %s - taken as closed\n",
                        s, now - g_bpp[s].rxMs, kPhaseName[g_bpp[s].phase]);
                    g_bpp[s].phase = kPhaseIdle;
                    g_bpp[s].wantOff = 0;
                }
                PupSync(s, h, now);
                if (!PupOffDisabled()) {
                    if (g_bpp[s].enterPred <= 0)
                        g_bpp[s].enterPred = kDefaultEnterMs;
                    const int early = g_bpp[s].phase == kPhaseOpen && g_bpp[s].clip == kClipIntro;
                    const int go = g_bpp[s].wantOff
                        || (early && now - g_bpp[s].t0 >= g_bpp[s].enterPred);
                    if ((g_bpp[s].wantOff || early) && !g_bpp[s].off)
                        PupBagOff(s, h, now, go);
                    else if (!g_bpp[s].off && g_bpp[s].prop && !g_bpp[s].wantOff) {
                        TO1_LOG("bp: slot %d readied prop not used (no swap in the intro) - removed\n", s);
                        PupPropKill(s, live);
                    } else if (!g_bpp[s].wantOff && g_bpp[s].off
                        && !(g_bpp[s].phase == kPhaseOpen && now - g_bpp[s].tOff < 800.0))
                        PupBagOn(s, h, live, "owner's bag back on");
                    if (g_bpp[s].off && g_bpp[s].prop)
                        PupBagTick(s, h, now);
                }
                if (!g_bpp[s].clip)
                    continue;
                const double el = now - g_bpp[s].t0, len = g_bpp[s].dur * 1000.0;
                if (g_bpp[s].clip == kClipIntro && el >= len - 150.0) {
                    if (g_bpp[s].phase == kPhaseOpen)
                        PupPlay(s, h, kClipLoop, 0, 0.2f, now);
                } else if (g_bpp[s].clip == kClipLoop && el >= len - 150.0) {
                    PupPlay(s, h, kClipLoop, 0, 0.15f, now);
                } else if (g_bpp[s].clip == kClipOutro && el >= len - 250.0) {
                    PupStop(s, 0.25f, "outro done");
                    if (g_bpp[s].phase == kPhaseClosing)
                        g_bpp[s].phase = kPhaseIdle;
                }
            }
        }

        // ==================================================================================
        //  Own-side per-frame poll (ext FeatureBackpackProp, the parts that are not hooks)
        // ==================================================================================
        void OwnPoll(double now, bool inMatch)
        {
            static int menuWas;
            static double tMenuOpen;
            static int fbHid;
            if (!inMatch) {
                // level gone: the bodies die with it
                ::engine::backpack_prop::ClearHeld();
                fbHid = 0;
                return;
            }
            uint8_t* me = ::engine::backpack_prop::Me();
            uint8_t* c = ::engine::backpack_prop::CtrlOwn();
            const int menu = (net::gui2::PlayerMenuActive() == 1) || net::gui2::BackpackOpen();
            if (menu != menuWas) {
                if (menu)
                    tMenuOpen = now;
                TO1_LOG("bp: own menu %s t %.0f ms; ctrl %p active %d, bag %s\n",
                    menu ? "OPEN" : "CLOSE", now, static_cast<void*>(c), c ? c[0x190] : -1,
                    ::engine::backpack_prop::Hid() ? "off (prop out)" : "on the back");
                menuWas = menu;
            }
            // TODO(bp-move-lock): the ext disables player-disable-move-stick / -jump / -roll
            // every frame while the menu is open (SP: the backpack stops the player). Needs the
            // per-frame natives; not ported. The engine's own MP behaviour stands.
            ::engine::backpack_prop::RunPendingLeave(menu != 0);
            {   // ps57 recovery (ext): the put-away refused a frame before "menu OPEN ... active 0",
                // never an ENTER, the menu could not be closed. The menu is up, the engine's bag did
                // not come up 300 ms after the open and a refusal of ours came right before it:
                // holster the weapon now with our own instant holster (our call passes every gate),
                // once per open. t2r-bp-hold-old.txt = off. The refusal-frame PRODUCER (the holster
                // fix / sreq gate) is not ported, so this stays inert until it lands.
                static double doneOpen = -1.0;
                const uint64_t refuse = ::engine::backpack_prop::RefuseFrame();
                const uint64_t frame = ::engine::frame::Count();
                if (menu && c && !c[0x190] && doneOpen != tMenuOpen
                    && now - tMenuOpen > 300.0 && refuse && frame >= refuse
                    && frame - refuse < 120
                    && ::engine::backpack_prop::CurrentWeapon()
                    && !::engine::exe_side::Exists("t2r-bp-hold-old.txt")) {
                    doneOpen = tMenuOpen;
                    const uint64_t held = ::engine::backpack_prop::CurrentWeapon();
                    const uint64_t r = DC::Call<"holster-player-weapon-instantly">(0, 0);
                    TO1_LOG("bp: ps57 menu open %.0f ms with no bag and our put-away refusal %llu frames ago - instant holster of %llx -> %llx (held now %llx)\n",
                        now - tMenuOpen, static_cast<unsigned long long>(frame - refuse),
                        static_cast<unsigned long long>(held),
                        static_cast<unsigned long long>(r),
                        static_cast<unsigned long long>(::engine::backpack_prop::CurrentWeapon()));
                }
            }
            if (::engine::backpack_prop::Hid() && !::engine::backpack_prop::HeldGenOk()) {
                // our new life (new body) voids the held state
                ::engine::backpack_prop::ClearHeld();
                fbHid = 0;
                ::engine::backpack_prop::ReapplyRig();
                TO1_LOG("bp: own new body while the bag was off - held state dropped, rig pair re-applied\n");
            }
            if (c) {
                const int active = c[0x190] != 0;
                if (active && !::engine::backpack_prop::Hid())
                    ::engine::backpack_prop::WornOff(me, 2);
                else if (!active && ::engine::backpack_prop::Hid())
                    ::engine::backpack_prop::WornOn(me, 2);
                fbHid = 0;
            } else {
                // TODO(bp-menu-fallback): the ext's src=3 fallback (hide the worn bag from the
                // menu when no controller is reachable at all) needs the per-look prop table
                // (BpLookProp / BpTables); not ported.
                (void)fbHid;
            }
            if (::engine::backpack_prop::Hid() && me)
                ::engine::backpack_prop::HideWornNow(me);
        }

    }   // namespace

    // -------------------------------------------------------------------------------------

    void Tick()
    {
        const double now = NowMs();
        const bool inLevel = InLevel();
        ::engine::backpack_prop::Install();
        InstallPickHook();   // ext InstallBackpackPropHook (first FeatureBackpackProp call)
        ::engine::backpack_prop::SetInMatch(inLevel);
        OwnPoll(now, inLevel);
        BpOwnTick(now);
        PupTick(now);
    }

    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        if (senderSlot >= kMaxNetPlayers || !data)
            return;
        if (size >= 6 && data[0] == kBptVer) {
            if (senderSlot != static_cast<uint8_t>(net::session::MySlot()))
                BptNetRecv(senderSlot, data, size);
            return;
        }
        if (size >= 6 && data[0] == kBptrVer) {
            if (senderSlot != static_cast<uint8_t>(net::session::MySlot()))
                BptrNetRecv(senderSlot, data, size);
            return;
        }
        if (size < sizeof(BpMsgT))
            return;
        BpMsgT m;
        std::memcpy(&m, data, sizeof m);
        if (m.ver != kVer || m.phase > kPhaseAbort)
            return;
        if (senderSlot == static_cast<uint8_t>(net::session::MySlot()))
            return;
        const double now = NowMs();
        const int s = senderSlot;
        const int changed = m.phase != g_bpp[s].phase || m.seq != g_bpp[s].seq;
        const float ow = OnewayMs(s);
        g_bpp[s].rxMs = now;
        g_bpp[s].ctRx = static_cast<double>(m.el_ms) + ow;
        if (changed)
            TO1_LOG("bp: slot %d rx phase %s (seq %u, %u ms into it, one-way ~%.0f ms, owner bag %s, swap at enter %d / leave %d ms) at t %.0f ms; puppet %s\n",
                s, kPhaseName[m.phase & 3], m.seq, m.el_ms, ow, m.bagoff ? "off" : "on",
                m.enter_ms == 0xffff ? -1 : m.enter_ms, m.leave_ms == 0xffff ? -1 : m.leave_ms,
                now, kClipName[g_bpp[s].clip & 3]);
        g_bpp[s].phase = m.phase;
        g_bpp[s].seq = m.seq;
        g_bpp[s].wantOff = m.bagoff != 0;
        if (m.enter_ms != 0xffff && m.enter_ms > 0 && m.enter_ms < 1500)
            g_bpp[s].enterPred = m.enter_ms;
    }

    void Reset()
    {
        for (int s = 0; s < kMaxNetPlayers; s++) {
            g_bpp[s] = PupState{};
            for (int c = 0; c < 4; c++) {
                std::memset(g_bptpOk[s][c], 0, kBptN);
                std::memset(g_bptrpOk[s][c], 0, kBptN);
            }
            g_bptNrx[s] = 0;
            g_bptrNrx[s] = 0;
        }
        g_bpo = OwnState{};
        ::engine::armor_rig::Reset();   // the rig's per-body state dies with the level (ext armor visual)
    }

}
