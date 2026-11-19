// ported from tools/ext/t2r-ext.c (the PCM mixer block) and loadseq.c's raw-PCM player

#include "pcm.hpp"

#include "ptr.hpp"

#include "log.hpp"          // must precede script.hpp (it uses TO1_LOG in a template)

#include "script.hpp"       // DC::Call (camera direction)
#include "object.hpp"       // engine::object::LocalPlayerWorld / EntityWorld (source bodies)
#include "raycast.hpp"      // engine::raycast::CameraPosition (the listener)

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace engine::pcm {

    namespace {

        // ---- mixer tuning (ext MIX_*) -------------------------------------------
        constexpr int kRate   = 48000;   // ext MIX_RATE
        constexpr int kFrames = 960;     // 20 ms of output per buffer (ext MIX_FRAMES)
        constexpr int kBufs   = 6;       // 6 x 20 ms of slack against a starved thread (ext MIX_BUFS)
        constexpr int kRamp   = 96;      // 2 ms fade in / out per voice (ext MIX_RAMP)
        constexpr int kVoices = 32;      // ext MIX_VOICES

        // ---- the winmm entry points ---------------------------------------------
        // Loaded from System32 explicitly: this module IS a winmm proxy (winmm_thunks), so a
        // plain import of waveOutOpen could resolve back to us. Same load the ext did.
        struct WaveOut {
            int      ok = 0;             // 0 unknown, 1 yes, -1 no
            MMRESULT (WINAPI* open)(HWAVEOUT*, UINT, const WAVEFORMATEX*, DWORD_PTR, DWORD_PTR, DWORD) = nullptr;
            MMRESULT (WINAPI* prep)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
            MMRESULT (WINAPI* unprep)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
            MMRESULT (WINAPI* write)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
            MMRESULT (WINAPI* close)(HWAVEOUT) = nullptr;
        };
        WaveOut g_wo;

        bool WaveOutReady()
        {
            if (g_wo.ok)
                return g_wo.ok > 0;
            g_wo.ok = -1;
            char dir[MAX_PATH];
            if (!GetSystemDirectoryA(dir, MAX_PATH))
                return false;
            strcat(dir, "\\winmm.dll");
            HMODULE m = LoadLibraryA(dir);
            if (!m)
                return false;
            g_wo.open  = reinterpret_cast<MMRESULT (WINAPI*)(HWAVEOUT*, UINT, const WAVEFORMATEX*, DWORD_PTR, DWORD_PTR, DWORD)>(GetProcAddress(m, "waveOutOpen"));
            g_wo.prep  = reinterpret_cast<MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT)>(GetProcAddress(m, "waveOutPrepareHeader"));
            g_wo.unprep = reinterpret_cast<MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT)>(GetProcAddress(m, "waveOutUnprepareHeader"));
            g_wo.write = reinterpret_cast<MMRESULT (WINAPI*)(HWAVEOUT, LPWAVEHDR, UINT)>(GetProcAddress(m, "waveOutWrite"));
            g_wo.close = reinterpret_cast<MMRESULT (WINAPI*)(HWAVEOUT)>(GetProcAddress(m, "waveOutClose"));
            if (g_wo.open && g_wo.prep && g_wo.unprep && g_wo.write && g_wo.close)
                g_wo.ok = 1;
            return g_wo.ok > 0;
        }

        // ---- the voice table (ext g_mixv) ---------------------------------------
        struct Slot {
            const Sample* src = nullptr;
            int           n = 0;
            double        pos = 0.0, step = 0.0;
            float         gl = 0.0f, gr = 0.0f;     // gain the mixer is at this block
            float         tgl = 0.0f, tgr = 0.0f;   // target gain it ramps to (3D voices)
            float         base = 1.0f;              // gain before the user volume (3D voices)
            int           dyn = 0;                  // 3D voice: gains recomputed per frame
            uint64_t      ent = 0;                  // source body (SID("player") / a puppet)
            float         wp[3] = {};               // source world position (ent 0 / fallback)
            float         rnear = 0.0f, rfar = 0.0f;// short-range falloff (> 0 = its own curve)
            int           played = 0;
            int           fadeN = 0, fadeLeft = 0;
            int           on = 0;
        };
        Slot             g_mixv[kVoices];
        std::mutex       g_mixLock;
        std::atomic<int> g_mixState{0};   // 0 not started, 1 running, -1 failed

        DWORD WINAPI MixThread(LPVOID)
        {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            static Sample  buf[kBufs][kFrames * 2];
            static WAVEHDR hdr[kBufs];
            static float   acc[kFrames * 2];
            WAVEFORMATEX fmt{};
            fmt.wFormatTag      = WAVE_FORMAT_PCM;
            fmt.nChannels       = 2;
            fmt.nSamplesPerSec  = kRate;
            fmt.nAvgBytesPerSec = kRate * 4;
            fmt.nBlockAlign     = 4;
            fmt.wBitsPerSample  = 16;
            if (!ev) {
                g_mixState.store(-1);
                return 0;
            }
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            int opens = 0;
            for (;;) {
                // A lost device (unplugged headset, driver reset, default device switched)
                // made every waveOutWrite fail and the mixer go silent for good: reopen the
                // stream after repeated failures.
                HWAVEOUT h = nullptr;
                const MMRESULT mr = g_wo.open(&h, WAVE_MAPPER, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
                if (mr != MMSYSERR_NOERROR) {
                    if (!opens) {
                        g_mixState.store(-1);
                        TO1_LOG("[PCM] waveOut open failed (%u)\n", static_cast<unsigned>(mr));
                        return 0;
                    }
                    Sleep(1000);        // device gone for now: retry, voices keep being consumed
                    {
                        std::lock_guard<std::mutex> lk(g_mixLock);
                        for (int v = 0; v < kVoices; v++)
                            g_mixv[v].on = 0;
                    }
                    continue;
                }
                for (int b = 0; b < kBufs; b++) {
                    memset(&hdr[b], 0, sizeof hdr[b]);
                    hdr[b].lpData = reinterpret_cast<LPSTR>(buf[b]);
                    hdr[b].dwBufferLength = sizeof buf[b];
                    g_wo.prep(h, &hdr[b], sizeof hdr[b]);
                    hdr[b].dwFlags |= WHDR_DONE;
                }
                if (!opens++)
                    TO1_LOG("[PCM] one waveOut stream, %d Hz stereo, %d x %d ms buffers, %d voices\n",
                        kRate, kBufs, kFrames * 1000 / kRate, kVoices);
                int fails = 0;
                while (fails < 8) {
                    int wrote = 0;
                    for (int b = 0; b < kBufs && fails < 8; b++) {
                        if (!(hdr[b].dwFlags & WHDR_DONE))
                            continue;
                        memset(acc, 0, sizeof acc);
                        {
                            std::lock_guard<std::mutex> lk(g_mixLock);
                            for (int v = 0; v < kVoices; v++) {
                                Slot& s = g_mixv[v];
                                if (!s.on)
                                    continue;
                                const Sample* src = s.src;
                                const int     n = s.n;
                                const double  st = s.step;
                                float         gl = s.gl, gr = s.gr;
                                // Ramp the live gain to the target across the block (3D voices
                                // set tgl/tgr each frame; a 2D voice has tgl == gl, so this is a
                                // no-op). ext: dgl = (tgl - gl) / MIX_FRAMES.
                                const float   dgl = (s.tgl - gl) * (1.0f / kFrames);
                                const float   dgr = (s.tgr - gr) * (1.0f / kFrames);
                                double pos = s.pos;
                                int played = s.played;
                                int fadeN = s.fadeN, fadeLeft = s.fadeLeft;
                                for (int f = 0; f < kFrames; f++) {
                                    const int k = static_cast<int>(pos);
                                    if (k + 1 >= n) {
                                        s.on = 0;
                                        break;
                                    }
                                    if (fadeN && fadeLeft <= 0) {
                                        s.on = 0;
                                        break;
                                    }
                                    const float t = static_cast<float>(pos - k);
                                    const float x = src[k] + (src[k + 1] - src[k]) * t;
                                    float rg = 1.0f;
                                    if (played < kRamp)
                                        rg = static_cast<float>(played) / kRamp;
                                    const float left = static_cast<float>(n - 1 - k) / static_cast<float>(st);
                                    if (left < kRamp)
                                        rg *= left / kRamp;
                                    if (fadeN) {
                                        rg *= static_cast<float>(fadeLeft) / fadeN;
                                        fadeLeft--;
                                    }
                                    played++;
                                    acc[2 * f]     += x * gl * rg;
                                    acc[2 * f + 1] += x * gr * rg;
                                    gl += dgl;
                                    gr += dgr;
                                    pos += st;
                                }
                                s.pos = pos;
                                s.played = played;
                                if (fadeN)
                                    s.fadeLeft = fadeLeft;
                                s.gl = s.tgl;
                                s.gr = s.tgr;
                            }
                        }
                        for (int f = 0; f < kFrames * 2; f++) {
                            float x = acc[f] * (1.0f / 32768.0f);
                            if (x != x)
                                x = 0.0f;
                            const float ax = x < 0 ? -x : x;
                            if (ax > 0.8f) {    // soft limiter: knee 0.8, tanh to 1.0 (summed voices used to hard-clip)
                                const float y = 0.8f + 0.2f * tanhf((ax - 0.8f) * 5.0f);
                                x = x < 0 ? -y : y;
                            }
                            buf[b][f] = static_cast<Sample>(x * 32767.0f);
                        }
                        hdr[b].dwFlags &= ~WHDR_DONE;
                        if (g_wo.write(h, &hdr[b], sizeof hdr[b]) != MMSYSERR_NOERROR) {
                            hdr[b].dwFlags |= WHDR_DONE;
                            fails++;
                            Sleep(20);
                        } else {
                            fails = 0;
                            wrote = 1;
                        }
                    }
                    if (!wrote && fails < 8)
                        WaitForSingleObject(ev, 50);
                }
                TO1_LOG("[PCM] waveOutWrite keeps failing - reopening the stream\n");
                for (int b = 0; b < kBufs; b++)
                    g_wo.unprep(h, &hdr[b], sizeof hdr[b]);
                g_wo.close(h);
                Sleep(500);
            }
        }

        // Start the mixer once; fills two free slots for a stereo wave under one lock.
        bool StartStereo(const Sample* l, const Sample* r, int n, float gain, int rate,
                         Voice& outL, Voice& outR)
        {
            outL = {};
            outR = {};
            if (!Start() || !l || !r || n < 2 || rate <= 0)
                return false;
            std::lock_guard<std::mutex> lk(g_mixLock);
            int a = -1, b = -1;
            for (int v = 0; v < kVoices && b < 0; v++) {
                if (g_mixv[v].on)
                    continue;
                if (a < 0)
                    a = v;
                else
                    b = v;
            }
            if (b < 0)
                return false;
            const Sample* src[2] = { l, r };
            const int     vv[2] = { a, b };
            for (int k = 0; k < 2; k++) {
                Slot& s = g_mixv[vv[k]];
                s.src = src[k];
                s.n = n;
                s.pos = 0.0;
                s.step = static_cast<double>(rate) / kRate;
                s.played = 0;
                s.fadeN = 0;
                s.fadeLeft = 0;
                s.gl = k ? 0.0f : gain;
                s.gr = k ? gain : 0.0f;
                s.tgl = s.gl;
                s.tgr = s.gr;
                s.base = 1.0f;
                s.dyn = 0;
                s.ent = 0;
                s.rnear = s.rfar = 0.0f;
                s.on = 1;
            }
            outL = { a, l };
            outR = { b, r };
            return true;
        }

        // ---- the game's options volume (ext SfxOptionRaw / GeneralVolume) --------
        // INFERRED ext offsets (options table 0x1270a80, getter 0x1a8bcb0, mixer group
        // volume 0x2639350), re-derived against the mod's build when one is available. The
        // reads are guarded, and every caller degrades to 1.0 when they fail.
        bool SfxOptionRaw(float* sfx, float* mus, float* dlg)
        {
            auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
            using FnTab = uint8_t* (*)(unsigned, unsigned);
            using FnOpt = float (*)(uint8_t*, int);
            uint8_t* o = reinterpret_cast<FnTab>(base + 0x1270a80)(0xb, 0);
            if (!o || !engine::ptr::Readable(reinterpret_cast<uintptr_t>(o), 0x1d900))
                return false;
            FnOpt get = reinterpret_cast<FnOpt>(base + 0x1a8bcb0);
            *sfx = get(o, 0);
            *mus = get(o, 1);
            *dlg = get(o, 2);
            return *sfx == *sfx && *sfx >= 0.0f && *sfx <= 1000.0f;
        }

        // The game's GENERAL volume = mixer group 64 (measured: 0.92 at 100 %, 0.09 at 10 %).
        // Our mixer is outside the game's master bus, so each path multiplies it in.
        float GeneralVolume()
        {
            using FnVol = float (*)(int);
            FnVol get = reinterpret_cast<FnVol>(reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr)) + 0x2639350);
            const float gm = get(64);
            return gm == gm && gm >= 0.0f ? (gm > 1.0f ? 1.0f : gm) : 1.0f;
        }

        // ---- 3D spatial (ext SfxSpatial / SfxLr / MixRangeGain / sfx_voices_update) -----
        // The 2013 Factions curve: full within ~3 m of the listener, rolling off to silent at
        // ~30 m; pan from the direction to the body relative to the camera's horizontal
        // heading. Position + pan are taken when the sound starts and then followed live.

        struct SfxLast { uint64_t ent = 0, t = 0; float p[3] = {}; };
        SfxLast g_sfxLast[16];               // last known position per body (LRU eviction)
        int     g_sfxLastN = 0;

        float g_mixRnear = 0.0f, g_mixRfar = 0.0f;   // range of the next PlayAtRange voice

        // A body's world position. ent == SID("player") is our own body.
        bool BodyPos(uint64_t ent, float out[3])
        {
            float w[8];
            const bool ok = ent == SID("player") ? engine::object::LocalPlayerWorld(w)
                                                 : engine::object::EntityWorld(ent, w);
            if (!ok)
                return false;
            out[0] = w[0];
            out[1] = w[1];
            out[2] = w[2];
            return std::isfinite(out[0] + out[1] + out[2]);
        }

        bool OwnPos(float out[3])
        {
            return BodyPos(SID("player"), out);
        }

        // Listener = the game camera (TLOU2 hears from the camera). Position comes from the
        // camera-through-reticle locator; direction from the reticle-dir native (the ext's
        // working pair). DIVERGENCE from the ext: its PRIMARY source was the render camera its
        // icam hook captured every frame, which the mod does not port; the reticle camera is
        // the primary here and the own-body-at-head-height tier matches the ext's last resort.
        bool ListenerPosDir(float cp[3], float cd[3])
        {
            Vector3 p;
            if (engine::raycast::CameraPosition(p)) {
                cp[0] = p.x; cp[1] = p.y; cp[2] = p.z;
                cd[0] = cd[1] = cd[2] = 0.0f;
                const Vector3* d = DC::Call<"camera-get-dir-through-reticle", const Vector3>();
                if (d && std::isfinite(d->x) && std::isfinite(d->z)) {
                    cd[0] = d->x; cd[1] = d->y; cd[2] = d->z;
                }
                return true;
            }
            float me[3];
            if (!OwnPos(me))
                return false;
            cp[0] = me[0]; cp[1] = me[1] + 1.5f; cp[2] = me[2];
            cd[0] = cd[1] = cd[2] = 0.0f;
            return true;
        }

        // Equal-power pan; centre = 1.0 on both channels. ext SfxLr.
        void SpatialLr(float base, float g, float pan, float* l, float* r)
        {
            const float th = (pan + 1.0f) * 0.78539816f;
            *l = base * g * 1.41421356f * std::cos(th);
            *r = base * g * 1.41421356f * std::sin(th);
        }

        // Short-range voice: by OUR body's distance to wp (full within rnear, squared roll-off
        // to silent at rfar). ext MixRangeGain.
        float RangeGain(const float* wp, float rnear, float rfar)
        {
            float pl[3];
            if (!OwnPos(pl))
                return 0.0f;                 // no body of ours (spectating / loading)
            const float dx = wp[0] - pl[0], dy = wp[1] - pl[1], dz = wp[2] - pl[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d <= rnear)
                return 1.0f;
            if (d >= rfar)
                return 0.0f;
            const float t = (rfar - d) / (rfar - rnear);
            return t * t;
        }

        // Gain + pan of the source body `ent` (or the world position `wpos`) relative to the
        // listener. bpout = the resolved source position. ext SfxSpatial.
        bool Spatial(uint64_t ent, const float* wpos, float* gain, float* pan, float* dist,
                     float* bpout, const char** why)
        {
            float bp[3];
            bool have = ent ? BodyPos(ent, bp) : false;
            if (!have && wpos) {
                bp[0] = wpos[0]; bp[1] = wpos[1]; bp[2] = wpos[2];
                have = true;
            }
            if (have && (!std::isfinite(bp[0] + bp[2]) || std::fabs(bp[0]) > 1e6f))
                have = false;
            if (!have) {
                *why = ent ? "no body locator" : "no position";
                return false;
            }
            float cp[3], cd[3];
            if (!ListenerPosDir(cp, cd)) {
                *why = "no camera, no own body";
                return false;
            }
            const float v[3] = { bp[0] - cp[0], bp[1] + 1.0f - cp[1], bp[2] - cp[2] };
            const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (!std::isfinite(d) || d > 1e5f) {
                *why = "bad distance";
                return false;
            }
            if (bpout) {
                bpout[0] = bp[0]; bpout[1] = bp[1]; bpout[2] = bp[2];
            }
            if (dist)
                *dist = d;
            if (d <= 3.0f)
                *gain = 1.0f;
            else if (d >= 30.0f)
                *gain = 0.0f;
            else {
                const float t = (30.0f - d) / 27.0f;
                *gain = t * t;
            }
            // pan: right vector of the camera heading, horizontal only. (The ext calibrated the
            // sign once by projecting a probe point to that side; uncalibrated it assumed +1,
            // which is what we use -- no nametag screen projection in the mod.)
            const float rt[3] = { cd[2], 0.0f, -cd[0] };
            const float rl = std::sqrt(rt[0] * rt[0] + rt[2] * rt[2]);
            const float hl = std::sqrt(v[0] * v[0] + v[2] * v[2]);
            *pan = 0.0f;
            if (rl < 1e-3f || hl < 0.5f)
                return true;
            const float r = (v[0] * (rt[0] / rl) + v[2] * (rt[2] / rl)) / hl;
            *pan = r > 0.85f ? 0.85f : r < -0.85f ? -0.85f : r;
            return true;
        }

        // Start a voice. dyn voices keep ent/wp/base/rnear/rfar for VoicesUpdate to follow.
        // ext PcmPlayVoice.
        Voice StartVoice(const Sample* src, int n, float gl, float gr, int rate, int dyn,
                         uint64_t ent, const float* wp, float base)
        {
            if (!Start() || !src || n < 2 || rate <= 0)
                return {};
            const float vol = SfxUserVolume();
            std::lock_guard<std::mutex> lk(g_mixLock);
            for (int v = 0; v < kVoices; v++) {
                if (g_mixv[v].on)
                    continue;
                Slot& s = g_mixv[v];
                s.src = src;
                s.n = n;
                s.pos = 0.0;
                s.step = static_cast<double>(rate) / kRate;
                s.played = 0;
                s.fadeN = 0;
                s.fadeLeft = 0;
                s.gl = s.tgl = gl * vol;
                s.gr = s.tgr = gr * vol;
                s.base = base;
                s.dyn = dyn;
                s.ent = ent;
                s.rnear = dyn ? g_mixRnear : 0.0f;
                s.rfar  = dyn ? g_mixRfar  : 0.0f;
                if (wp)
                    std::memcpy(s.wp, wp, 12);
                else
                    std::memset(s.wp, 0, 12);
                s.on = 1;
                return { v, src };
            }
            return {};
        }

    }

    float SfxUserVolume()
    {
        static float    v = 1.0f;
        static uint64_t last = 0;
        const uint64_t now = GetTickCount64();
        if (now - last < 2000)
            return v;
        last = now;
        float x = 1.0f, m = 0.0f, d = 0.0f;
        if (SfxOptionRaw(&x, &m, &d)) {
            // A value above 1.5 is a 0..100 slider position, otherwise a 0..1 gain. Squared:
            // the game's slider is a perceptual (dB-like) taper, the mixer took it as a
            // linear amplitude (30 % on the slider was ~-10 dB for us but far quieter in the
            // game's mix).
            v = x > 1.5f ? x / 100.0f : x;
            if (v > 1.0f)
                v = 1.0f;
            v = v * v;
            v *= GeneralVolume();
        } else {
            v = 1.0f;
        }
        return v;
    }

    float MusicUserVolume()
    {
        float s = 1.0f, m = 1.0f, d = 0.0f;
        if (!SfxOptionRaw(&s, &m, &d))
            return 1.0f;
        m = m > 1.5f ? m / 100.0f : m;
        m = m < 0.0f ? 0.0f : m > 1.0f ? 1.0f : m;
        return m * m * GeneralVolume();
    }

    bool Start()
    {
        if (g_mixState.load(std::memory_order_acquire) == 0) {
            int expected = 0;
            if (g_mixState.compare_exchange_strong(expected, 1)) {
                if (!WaveOutReady()) {
                    g_mixState.store(-1);
                } else {
                    HANDLE t = CreateThread(nullptr, 0, MixThread, nullptr, 0, nullptr);
                    if (t) {
                        SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
                        CloseHandle(t);
                    } else {
                        g_mixState.store(-1);
                    }
                }
            }
        }
        return g_mixState.load(std::memory_order_acquire) == 1;
    }

    Voice Play(const Sample* src, int n, float gl, float gr, int rate)
    {
        if (!Start() || !src || n < 2 || rate <= 0)
            return {};
        const float vol = SfxUserVolume();
        std::lock_guard<std::mutex> lk(g_mixLock);
        for (int v = 0; v < kVoices; v++) {
            if (g_mixv[v].on)
                continue;
            Slot& s = g_mixv[v];
            s.src = src;
            s.n = n;
            s.pos = 0.0;
            s.step = static_cast<double>(rate) / kRate;
            s.played = 0;
            s.fadeN = 0;
            s.fadeLeft = 0;
            s.gl = gl * vol;
            s.gr = gr * vol;
            s.tgl = s.gl;
            s.tgr = s.gr;
            s.base = 1.0f;
            s.dyn = 0;
            s.ent = 0;
            s.rnear = s.rfar = 0.0f;
            s.on = 1;
            return { v, src };
        }
        return {};
    }

    bool PlayStereo(const Sample* l, const Sample* r, int n, float gain, int rate,
                    Voice& outL, Voice& outR)
    {
        return StartStereo(l, r, n, gain * SfxUserVolume(), rate, outL, outR);
    }

    bool PlayStereoRaw(const Sample* l, const Sample* r, int n, float gain, int rate,
                       Voice& outL, Voice& outR)
    {
        return StartStereo(l, r, n, gain, rate, outL, outR);
    }

    Voice PlayAt(const Sample* src, int n, float gain, int rate, StringId64 ent,
                 const float* wpos)
    {
        if (!src || n < 2 || rate <= 0)
            return {};
        float g = 1.0f, pan = 0.0f, d = 0.0f, bp[3];
        const char* why = "";
        if (ent == SID("player")) {
            // ours: spatial if we can, else full-level 2D (it IS ours)
            if (Spatial(ent, nullptr, &g, &pan, &d, bp, &why)) {
                float l, r;
                SpatialLr(gain, g, pan, &l, &r);
                return StartVoice(src, n, l, r, rate, 1, ent, bp, gain);
            }
            return StartVoice(src, n, gain, gain, rate, 0, 0, nullptr, 1.0f);
        }
        if (!Spatial(ent, wpos, &g, &pan, &d, bp, &why)) {
            // last known position of that body, else the world position, else drop
            float lp[3];
            int have = 0;
            for (int i = 0; i < g_sfxLastN; i++)
                if (g_sfxLast[i].ent == ent) {
                    std::memcpy(lp, g_sfxLast[i].p, 12);
                    have = 1;
                    break;
                }
            if (!have && wpos) {
                std::memcpy(lp, wpos, 12);
                have = 1;
            }
            if (!have || !Spatial(0, lp, &g, &pan, &d, bp, &why))
                return {};
        } else if (ent) {
            int i = 0;
            while (i < g_sfxLastN && g_sfxLast[i].ent != ent)
                i++;
            if (i == g_sfxLastN) {
                if (g_sfxLastN < 16)
                    g_sfxLastN++;
                else {
                    i = 0;
                    for (int k = 1; k < 16; k++)
                        if (g_sfxLast[k].t < g_sfxLast[i].t)
                            i = k;                 // full: evict the least recently used
                }
            }
            g_sfxLast[i].ent = ent;
            g_sfxLast[i].t = GetTickCount64();
            std::memcpy(g_sfxLast[i].p, bp, 12);
        }
        if (g_mixRfar > 0.0f)
            g = RangeGain(bp, g_mixRnear, g_mixRfar);
        float l, r;
        SpatialLr(gain, g, pan, &l, &r);
        // out of earshot at the start: still a (silent) dynamic voice, so a body that walks
        // toward the sound becomes audible (VoicesUpdate follows it live)
        return StartVoice(src, n, l, r, rate, 1, ent, bp, gain);
    }

    Voice PlayAtRange(const Sample* src, int n, float gain, int rate, const float* wpos,
                      float rnear, float rfar)
    {
        g_mixRnear = rnear;
        g_mixRfar  = rfar > rnear ? rfar : rnear + 0.1f;
        const Voice v = PlayAt(src, n, gain, rate, 0, wpos);
        g_mixRnear = g_mixRfar = 0.0f;
        return v;
    }

    void VoicesUpdate()
    {
        int any = 0;
        for (int v = 0; v < kVoices; v++)
            if (g_mixv[v].on && g_mixv[v].dyn) {
                any = 1;
                break;
            }
        if (!any || g_mixState.load(std::memory_order_acquire) != 1)
            return;
        const float vol = SfxUserVolume();
        for (int v = 0; v < kVoices; v++) {
            if (!g_mixv[v].on || !g_mixv[v].dyn)
                continue;
            float g, pan, d, bp[3], l, r;
            const char* why;
            if (!Spatial(g_mixv[v].ent, g_mixv[v].wp, &g, &pan, &d, bp, &why))
                continue;                   // keep the last targets
            if (g_mixv[v].rfar > 0.0f)
                g = RangeGain(bp, g_mixv[v].rnear, g_mixv[v].rfar);
            SpatialLr(g_mixv[v].base, g, pan, &l, &r);
            std::lock_guard<std::mutex> lk(g_mixLock);
            if (g_mixv[v].on) {
                std::memcpy(g_mixv[v].wp, bp, 12);
                g_mixv[v].tgl = l * vol;
                g_mixv[v].tgr = r * vol;
            }
        }
    }

    bool Live(const Voice& v)
    {
        if (v.slot < 0 || g_mixState.load(std::memory_order_acquire) != 1)
            return false;
        std::lock_guard<std::mutex> lk(g_mixLock);
        const Slot& s = g_mixv[v.slot];
        return s.on && s.src == v.src;
    }

    void Cut(Voice& v, int ms)
    {
        if (v.slot >= 0 && g_mixState.load(std::memory_order_acquire) == 1) {
            std::lock_guard<std::mutex> lk(g_mixLock);
            Slot& s = g_mixv[v.slot];
            if (s.on && s.src == v.src) {
                if (ms <= 0)
                    s.on = 0;
                else
                    s.fadeLeft = s.fadeN = ms * kRate / 1000;
            }
        }
        v = {};
    }

    void Invalidate(Voice& v)
    {
        v = {};
    }

}
