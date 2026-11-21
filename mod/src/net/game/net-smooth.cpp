#include "net-smooth.hpp"

#include <cmath>
#include <cstring>

// Line-for-line behavioural port of the pure half of tools/ext/netsmooth.c. Function names
// below carry their ext originals in comments so the two can be diffed against each other.
// Everything engine-dependent in the ext (NsRebaseClearRing / the winmm time ring, the
// alpha/apply/compose hooks, the send cap, relay dedup, host bw, NsNoteVec/root-vec4, the
// anim-clock target) is deliberately absent: see the header comment in net-smooth.hpp.

namespace net::netsmooth {
namespace {

    constexpr double kInitFrameMs = 16.7;   // NsReset(): sdt = fdt = 16.7

    struct Sample {
        uint32_t seq = 0;
        uint8_t  tSrc = 0;                  // 0 none, 1 estimated, 2 paired with msg 7
        bool     tele = false;
        double   t = 0, rl = 0;
        float    p[3]{};
        float    q[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    };

    struct Slot {
        Sample   samp[kRingSize];
        uint32_t n = 0;
        uint32_t m7seq[kM7Size]{};
        double   m7t[kM7Size]{};
        uint32_t m7n = 0;
        uint32_t aSeq = 0;
        double   aT = 0;
        bool     haveAnchor = false;
        double   sdt = kInitFrameMs;
        int64_t  bkId[8]{};                 // least-delay offset buckets (8 x 500 ms)
        double   bkMax[8]{};
        uint8_t  bkOk[8]{};
        double   oref = -1e300;
        bool     synced = false;
        double   gavg = 0;
        double   jpk = 0, jmean = 0, loss = 0, jRl = 0;
        double   R = 0, fdt = kInitFrameMs, fLast = 0;
        bool     Rok = false;
        double   delay = 0, dcur = 0;
        bool     outOk = false, extrap = false;
        double   outLast = 0;
        float    outP[3]{}, outQ[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        float    E[3]{}, holdQ[4]{ 0.0f, 0.0f, 0.0f, 1.0f }, holdB = 0, vel[3]{};
        float    wLast[3]{}, wV = 0, oLast[3]{}, oV = 0;
        bool     wOk = false, oOk = false;
        double   si = 0;
        uint32_t base = 0;
        bool     haveBase = false;
        bool     ours = false;
        float    xb = 0;                    // 100 ms handover from the native placement
        Stats    st;
    };

    // ext keeps g_nsLastraw / g_ns_cand_* and g_nsDq outside NsSlot, so NsReset() does not
    // disturb them. Mirror that here: Reset() clears only Slot, ResetAll() clears all three.
    struct RebaseState {
        uint32_t lastRaw = 0, candLast = 0;
        bool     lastRawOk = false;
        uint8_t  candN = 0;
    };
    struct DqState {
        uint32_t seq[kDqN]{};
        double   t[kDqN]{};
        uint32_t n = 0, lastOut = 0;
        bool     haveOut = false, q = false;
    };

    Slot        g_slot[kSlots];
    RebaseState g_rb[kSlots];
    DqState     g_dq[kSlots];
    bool        g_init = false;

    void EnsureInit()
    {
        if (g_init) return;
        for (auto& s : g_slot) s = Slot{};
        for (auto& r : g_rb)  r = RebaseState{};
        for (auto& d : g_dq)  d = DqState{};
        g_init = true;
    }

    // ---- NsDot4 / NsNorm4 / NsSlerp / NsFinite ------------------------------------------
    float Dot4(const float* a, const float* b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    }

    void Norm4(float* q)
    {
        float l = sqrtf(Dot4(q, q));
        if (l > 1e-6f)
            for (int i = 0; i < 4; i++) q[i] /= l;
    }

    void Slerp(float* o, const float* a, const float* b0, float f)
    {
        float b[4] = { b0[0], b0[1], b0[2], b0[3] }, d = Dot4(a, b);
        if (d < 0.0f) {
            for (int i = 0; i < 4; i++) b[i] = -b[i];
            d = -d;
        }
        float wa = 1.0f - f, wb = f;
        if (d < 0.9995f) {
            float th = acosf(d), s = sinf(th);
            wa = sinf((1.0f - f) * th) / s;
            wb = sinf(f * th) / s;
        }
        for (int i = 0; i < 4; i++) o[i] = wa * a[i] + wb * b[i];
        Norm4(o);
    }

    bool Finite(const float* v, int n)
    {
        for (int i = 0; i < n; i++)
            if (!(v[i] == v[i]) || v[i] > 1e6f || v[i] < -1e6f) return false;
        return true;
    }

    // ---- NsReset / NsEstT / NsVel -------------------------------------------------------
    void ResetSlot(Slot& s)
    {
        uint32_t keepBase = s.base;
        bool     kb = s.haveBase;
        s = Slot{};
        s.base = keepBase;
        s.haveBase = kb;
    }

    double EstT(const Slot& s, uint32_t seq, double rl)
    {
        if (s.haveAnchor) {
            double d = static_cast<double>(static_cast<int32_t>(seq - s.aSeq)) * s.sdt;
            if (d > -500.0 && d < 500.0) return s.aT + d;
        }
        return s.synced ? rl + s.oref : -1.0;
    }

    // velocity of the sample at absolute index `top` over >= 60 ms of history (never across a
    // teleport); returns the reference sample or NULL.
    Sample* Vel(Slot& s, uint32_t top, float v[3])
    {
        Sample* nw = &s.samp[top % kRingSize];
        Sample* ref = nullptr;
        v[0] = v[1] = v[2] = 0.0f;
        if (!nw->tSrc || nw->tele) return nullptr;

        uint32_t j = top, lo = s.n > kRingSize ? s.n - kRingSize : 0;
        for (;;) {
            if (j <= lo || s.samp[j % kRingSize].tele) break;
            j--;
            Sample* x = &s.samp[j % kRingSize];
            if (!x->tSrc || nw->t - x->t > 200.0) break;
            ref = x;
            if (nw->t - x->t >= 60.0) break;
        }
        if (!ref || nw->t - ref->t < 1.0 || nw->t - ref->t > 250.0) return nullptr;

        for (int i = 0; i < 3; i++)
            v[i] = (nw->p[i] - ref->p[i]) / static_cast<float>(nw->t - ref->t);
        float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (sp > kMaxSpeed)
            for (int i = 0; i < 3; i++) v[i] *= kMaxSpeed / sp;
        return ref;
    }

    // ---- NsOnState ----------------------------------------------------------------------
    void OnStateBody(uint8_t slot, uint32_t seq, const float p[3], const float q[4], double rl)
    {
        if (slot >= kSlots || !Finite(p, 3) || !Finite(q, 4)) return;
        Slot& s = g_slot[slot];
        s.st.states++;
        Sample* pv = s.n ? &s.samp[(s.n - 1) % kRingSize] : nullptr;
        if (pv && static_cast<int32_t>(seq - pv->seq) <= 0) {
            ResetSlot(s);
            pv = nullptr;
            s.st.states = 1;
        }
        if (pv && seq - pv->seq > 1 && seq - pv->seq < 1000)
            s.st.lost5 += seq - pv->seq - 1;

        Sample& x = s.samp[s.n % kRingSize];
        x = Sample{};
        x.seq = seq;
        x.rl = rl;
        std::memcpy(x.p, p, 12);
        std::memcpy(x.q, q, 16);
        Norm4(x.q);

        if (pv) {
            float dx = p[0] - pv->p[0], dy = p[1] - pv->p[1], dz = p[2] - pv->p[2];
            double gapMs = static_cast<double>(static_cast<int32_t>(seq - pv->seq)) * s.sdt;
            if (gapMs < 120.0)
                s.si = s.si > 0.0 ? s.si + (gapMs - s.si) * 0.05 : gapMs;
            float thr = kTeleportM;
            if (gapMs > 250.0) {
                float t2 = 2.0f + 0.008f * static_cast<float>(gapMs > 1500.0 ? 1500.0 : gapMs);
                if (t2 > thr) thr = t2;
            }
            x.tele = dx * dx + dy * dy + dz * dz > thr * thr;
            if (x.tele) s.st.tele++;
        }
        for (uint32_t k = 0; k < kM7Size && k < s.m7n; k++)
            if (s.m7seq[k] == seq) {
                x.t = s.m7t[k];
                x.tSrc = 2;
                break;
            }
        if (!x.tSrc) {
            double t = EstT(s, seq, rl);
            if (t >= 0.0) {
                x.t = t;
                x.tSrc = 1;
            }
        }
        if (x.tSrc && pv && pv->tSrc && x.t <= pv->t) x.t = pv->t + 0.5;

        if (pv && pv->tSrc && !x.tele && s.n >= 2) {
            float v[3];
            double dts = x.tSrc ? x.t - pv->t
                : static_cast<double>(static_cast<int32_t>(seq - pv->seq)) * s.sdt;
            Vel(s, s.n - 1, v);
            if (dts > 1.0 && dts < 1000.0) {
                float eCv = 0.0f, eHold = 0.0f;
                double cap = kExtrapMaxMs + 100.0;
                for (int i = 0; i < 3; i++) {
                    float pr = pv->p[i] + v[i] * static_cast<float>(dts > cap ? cap : dts);
                    eCv += (x.p[i] - pr) * (x.p[i] - pr);
                    eHold += (x.p[i] - pv->p[i]) * (x.p[i] - pv->p[i]);
                }
                eCv = sqrtf(eCv);
                eHold = sqrtf(eHold);
                s.st.predN++;
                s.st.predCvSum += eCv;
                s.st.predHoldSum += eHold;
                if (eCv > s.st.predMaxCv) s.st.predMaxCv = eCv;
                if (eHold > s.st.predMaxHold) s.st.predMaxHold = eHold;
                if (seq - pv->seq > 1) {
                    s.st.predGapN++;
                    s.st.predGapCvSum += eCv;
                    s.st.predGapHoldSum += eHold;
                }
            }
        }
        s.n++;
    }

    // ---- NsRebase (pure half: no winmm time ring, no +0x77e4 reset bit) ------------------
    uint32_t Rebase(uint8_t slot, uint32_t t)
    {
        RebaseState& r = g_rb[slot];
        Slot& s = g_slot[slot];
        int32_t rel = static_cast<int32_t>(t - s.base);
        int change = 0;
        if (s.haveBase && r.lastRawOk) {
            int32_t d = static_cast<int32_t>(t - r.lastRaw);
            if (d < -2000) {
                int32_t cd = static_cast<int32_t>(t - r.candLast);
                if (r.candN && cd > -1000 && cd < 1000) r.candN++;
                else                                    r.candN = 1;
                r.candLast = t;
                if (r.candN >= 3) change = 1;
                else              return 0;            // not (yet) a new domain
            }
            else {
                r.candN = 0;
                if (rel > (1 << 24) && d > 60000) change = 1;
            }
        }
        if (!s.haveBase || change || rel < 0 || rel > (1 << 24)) {
            const bool wasBase = s.haveBase;
            s.base = t - 1000;
            s.haveBase = true;
            r.candN = 0;
            r.lastRaw = t;
            r.lastRawOk = true;
            ResetSlot(s);
            if (wasBase) s.st.rebase++;   // ResetSlot clears Stats: count after the reset
            rel = 1000;
        }
        if (!r.lastRawOk || static_cast<int32_t>(t - r.lastRaw) > 0) {
            r.lastRaw = t;
            r.lastRawOk = true;
        }
        if (rel < 0) return 0;
        return static_cast<uint32_t>(rel);
    }

    // ---- NsDequant ----------------------------------------------------------------------
    uint32_t Dequant(uint8_t slot, uint32_t seq, uint32_t t)
    {
        DqState& d = g_dq[slot];
        if (d.n) {
            uint32_t li = (d.n - 1) % kDqN;
            int32_t  ds = static_cast<int32_t>(seq - d.seq[li]);
            double   dt = t - d.t[li];
            if (ds <= 0 && ds > -1000) return t;       // old / repeated list
            if (ds >= 1000 || dt < 0.0 || dt > 5000.0) {
                d.n = 0;
                d.haveOut = false;
                d.q = false;
            }
        }
        d.seq[d.n % kDqN] = seq;
        d.t[d.n % kDqN] = t;
        d.n++;
        g_slot[slot].st.dequant++;                     // st_total

        uint32_t m = d.n < kDqN ? d.n : kDqN;
        if (m < 12) return t;
        uint32_t i0 = d.n - m;
        double   t0 = d.t[i0 % kDqN];
        uint32_t s0 = d.seq[i0 % kDqN];
        uint32_t on = 0;
        for (uint32_t k = 0; k < m; k++) {
            double x = (d.t[(i0 + k) % kDqN] - t0) / kDqQ, fr = x - floor(x + 0.5);
            if (fr < 0.0) fr = -fr;
            if (fr < 0.09) on++;
        }
        if (d.q ? on * 10 < m * 6 : on * 10 < m * 9) {
            d.q = false;
            return t;
        }
        d.q = true;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (uint32_t k = 0; k < m; k++) {
            double x = static_cast<double>(static_cast<int32_t>(d.seq[(i0 + k) % kDqN] - s0));
            double y = d.t[(i0 + k) % kDqN] - t0;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double den = m * sxx - sx * sx;
        if (den < 1e-6) return t;
        double b = (m * sxy - sx * sy) / den, a = (sy - b * sx) / m;
        if (b < 3.0 || b > 150.0) return t;
        double xn = static_cast<double>(static_cast<int32_t>(seq - s0));
        double out = t0 + a + b * xn + kDqQ * 0.5;
        uint32_t o = static_cast<uint32_t>(out + 0.5);
        if (d.haveOut && static_cast<int32_t>(o - d.lastOut) <= 0) o = d.lastOut + 1;
        d.lastOut = o;
        d.haveOut = true;
        g_slot[slot].st.dequant++;                     // st_q
        return o;
    }

    // ---- NsOnAnim -----------------------------------------------------------------------
    void OnAnimBody(uint8_t slot, uint32_t seq, double t, double rl)
    {
        if (slot >= kSlots) return;
        Slot& s = g_slot[slot];
        s.st.anims++;
        if (s.haveAnchor && static_cast<int32_t>(seq - s.aSeq) <= 0
            && static_cast<int32_t>(seq - s.aSeq) > -1000) return;

        if (s.haveAnchor) {
            int32_t g = static_cast<int32_t>(seq - s.aSeq) - 1;
            if (g >= 0 && g < 1000) {
                s.st.lost7 += static_cast<uint32_t>(g);
                s.loss += (static_cast<double>(g) / (static_cast<double>(g) + 1) - s.loss) * 0.02;
            }
        }
        if (s.haveAnchor && seq != s.aSeq) {
            double d = (t - s.aT) / static_cast<double>(static_cast<int32_t>(seq - s.aSeq));
            if (d > 3.0 && d < 150.0) s.sdt += (d - s.sdt) * 0.05;
        }
        if (s.haveAnchor && (t - s.aT > 60000.0 || t < s.aT)) ResetSlot(s);

        s.aSeq = seq;
        s.aT = t;
        s.haveAnchor = true;
        s.m7seq[s.m7n % kM7Size] = seq;
        s.m7t[s.m7n % kM7Size] = t;
        s.m7n++;

        // least-delay offset over 4 s (8 buckets of 500 ms)
        double   o = t - rl;
        int64_t  b = static_cast<int64_t>(rl / 500.0);
        int      i = static_cast<int>(b & 7);
        if (!s.bkOk[i] || s.bkId[i] != b) {
            s.bkId[i] = b;
            s.bkMax[i] = o;
            s.bkOk[i] = 1;
        }
        else if (o > s.bkMax[i]) s.bkMax[i] = o;
        double oref = -1e300;
        for (int k = 0; k < 8; k++)
            if (s.bkOk[k] && b - s.bkId[k] < 8 && s.bkMax[k] > oref) oref = s.bkMax[k];
        s.oref = oref;
        double j = oref - o;
        if (!s.synced) {
            s.synced = true;
            s.jpk = 8.0;
            s.jmean = 4.0;
        }
        {
            double jd = s.jRl > 0.0 ? rl - s.jRl : 10.0;
            if (jd < 0.0) jd = 0.0;
            if (jd > 500.0) jd = 500.0;
            s.jRl = rl;
            if (j > s.jpk) s.jpk += (j - s.jpk) * 0.3;
            else           s.jpk -= (s.jpk - j) * (1.0 - exp(-jd / kJpkTauMs));
        }
        s.jmean += (j - s.jmean) * 0.02;
        if (static_cast<float>(j) > s.st.jitterMax) s.st.jitterMax = static_cast<float>(j);

        // pair with the msg 5 sample of the same seq; re-estimate the samples after it
        int found = -1;
        for (uint32_t k = 0; k < 24 && k < s.n; k++) {
            Sample& x = s.samp[(s.n - 1 - k) % kRingSize];
            if (x.seq == seq) {
                x.t = t;
                x.tSrc = 2;
                found = static_cast<int>(k);
                break;
            }
            if (static_cast<int32_t>(x.seq - seq) < 0) break;
        }
        for (int k = found - 1; k >= 0; k--) {
            Sample& x = s.samp[(s.n - 1 - static_cast<uint32_t>(k)) % kRingSize];
            if (x.tSrc != 2) {
                x.t = t + static_cast<double>(static_cast<int32_t>(x.seq - seq)) * s.sdt;
                x.tSrc = 1;
            }
        }
    }

    // ---- NsStutter ----------------------------------------------------------------------
    void Stutter(float last[3], float* lv, bool* ok, const float p[3], double dt,
                 uint32_t* cnt, uint32_t* moving)
    {
        if (*ok && dt > 1.0 && dt < 100.0) {
            float dx = p[0] - last[0], dz = p[2] - last[2];
            float v = sqrtf(dx * dx + dz * dz) / static_cast<float>(dt * 0.001);
            if (moving && (v > 1.0f || *lv > 1.0f)) (*moving)++;
            if ((v > 1.0f || *lv > 1.0f) && fabsf(v - *lv) > kStutterMps
                && v < 40.0f && *lv < 40.0f) (*cnt)++;
            *lv = v;
        }
        std::memcpy(last, p, 12);
        *ok = true;
    }

    // ---- NsLocator's playout core -------------------------------------------------------
    bool SampleBody(uint8_t slot, double now, double oneway, Transform& out,
                    const Transform* handover)
    {
        Slot& s = g_slot[slot];

        // winmm's own placement (handover) is the stutter baseline, as in NsLocator.
        double dt = s.fLast > 0.0 ? now - s.fLast : 1000.0;
        if (dt < 0.0) dt = 0.0;
        s.fLast = now;
        if (handover)
            Stutter(s.wLast, &s.wV, &s.wOk, handover->pos, dt, &s.st.stutterNative, &s.st.moving);
        s.st.frames++;

        s.st.frameMs = s.sdt;
        s.st.sampleMs = s.si;
        s.st.jitterMean = s.jmean;
        s.st.lossEwma = s.loss;

        if (!s.synced || s.n < 2) {
            s.Rok = false;
            s.outOk = false;
            s.ours = false;
            return false;
        }

        double sfr = s.si > s.sdt ? s.si : s.sdt;
        double D = kJitterK * s.jpk + sfr + 2.0;
        if (s.loss > 0.03) {
            double l = s.loss > 0.7 ? 0.7 : s.loss, run = log(0.05) / log(l);
            if (run > 6.0) run = 6.0;
            double lt = sfr * (run - 1.0);
            D += lt < 0.0 ? 0.0 : (lt > 80.0 ? 80.0 : lt);
        }
        if (D < kDelayMinMs) D = kDelayMinMs;
        if (D > kDelayMaxMs) D = kDelayMaxMs;

        if (s.dcur <= 0.0 || dt > 300.0) s.dcur = D;
        else if (D > s.dcur + kDcurUpHyst) s.dcur = D;
        else if (D < s.dcur - kDcurDnHyst) {
            s.dcur -= dt * kDcurDown;
            if (s.dcur < D) s.dcur = D;
        }
        D = s.dcur;

        if (oneway < 0.0) oneway = 0.0;
        {
            double floorMs = sfr + s.jpk + 2.0;
            if (floorMs < kDelayFloor) floorMs = kDelayFloor;
            double lead = kLeadK * oneway;
            if (lead > kLeadMaxMs) lead = kLeadMaxMs;
            if (lead > D - floorMs) lead = D - floorMs;
            if (lead > 0.0) D -= lead;
        }
        s.delay = D;
        s.st.delaySum += D;

        double Rt = now + s.oref - D;
        if (!s.Rok || dt > 300.0 || fabs(Rt - (s.R + dt)) > 300.0) {
            if (s.Rok) s.st.resync++;
            s.R = Rt;
            s.Rok = true;
            s.outOk = false;
        }
        else {
            double err = Rt - (s.R + dt), rate = err / 250.0;
            if (rate >  0.08) rate =  0.08;
            if (rate < -0.08) rate = -0.08;
            s.R += dt * (1.0 + rate);
            s.fdt += (dt - s.fdt) * 0.1;
        }

        // samples around R
        uint32_t cnt = s.n < kRingSize ? s.n : kRingSize;
        Sample* nw = nullptr;
        Sample* A = nullptr;
        Sample* B = nullptr;
        Sample* old = nullptr;
        uint32_t nwi = 0;
        for (uint32_t k = 0; k < cnt; k++) {
            Sample* x = &s.samp[(s.n - 1 - k) % kRingSize];
            if (!x->tSrc) continue;
            if (!nw) {
                nw = x;
                nwi = s.n - 1 - k;
            }
            old = x;
            if (x->t <= s.R) {
                A = x;
                break;
            }
            B = x;
        }
        if (!nw) return false;

        float P[3], Q[4];
        int ex = 0;
        if (A && B) {
            if (B->tele) {
                std::memcpy(P, A->p, 12);
                std::memcpy(Q, A->q, 16);
            }
            else {
                float f = static_cast<float>((s.R - A->t) / (B->t - A->t));
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                for (int i = 0; i < 3; i++) P[i] = A->p[i] + (B->p[i] - A->p[i]) * f;
                Slerp(Q, A->q, B->q, f);
            }
        }
        else if (A) {
            double ahead = s.R - nw->t;
            ex = 1;
            s.st.extrap++;
            if (ahead > kExtrapMaxMs) {
                ahead = kExtrapMaxMs;
                s.st.hold++;
            }
            float v[3];
            Sample* ref = Vel(s, nwi, v);
            float ea = static_cast<float>(ahead - ahead * ahead / (2.0 * kExtrapMaxMs));
            for (int i = 0; i < 3; i++) P[i] = nw->p[i] + v[i] * ea;
            std::memcpy(Q, nw->q, 16);
            if (ref) {
                float d = Dot4(ref->q, nw->q);
                float ang = 2.0f * acosf(d < 0.0f ? -d : (d > 1.0f ? 1.0f : d));
                double rdt = nw->t - ref->t;
                float fx = 1.0f + static_cast<float>(ea / rdt);
                if (ang > 1e-4f && (fx - 1.0f) * ang > 0.7f) fx = 1.0f + 0.7f / ang;
                if (ang > 1e-4f && fx > 1.0f) Slerp(Q, ref->q, nw->q, fx);
            }
        }
        else {
            std::memcpy(P, old->p, 12);
            std::memcpy(Q, old->q, 16);
        }

        // smooth error correction instead of snaps
        double odt = s.outOk ? now - s.outLast : 0.0;
        if (s.outOk && odt > 0.0 && odt < 300.0) {
            float dec = static_cast<float>(exp(-odt / kTauMs));
            for (int i = 0; i < 3; i++) s.E[i] *= dec;
            s.holdB *= dec;
            if (s.extrap && !ex) {
                float e[3], el = 0.0f;
                for (int i = 0; i < 3; i++) {
                    e[i] = s.outP[i] + s.vel[i] * static_cast<float>(odt) - P[i];
                    el += e[i] * e[i];
                }
                el = sqrtf(el);
                if (el > kSnapM) {
                    std::memset(s.E, 0, 12);
                    s.holdB = 0.0f;
                    s.st.snap++;
                }
                else {
                    std::memcpy(s.E, e, 12);
                    std::memcpy(s.holdQ, s.outQ, 16);
                    s.holdB = 1.0f;
                    s.st.corr++;
                    if (el > s.st.corrMax) s.st.corrMax = el;
                }
            }
            float o[3], jump = 0.0f;
            for (int i = 0; i < 3; i++) {
                o[i] = P[i] + s.E[i];
                jump += (o[i] - s.outP[i]) * (o[i] - s.outP[i]);
            }
            if (jump > kTeleportM * kTeleportM) {
                std::memset(s.E, 0, 12);
                s.holdB = 0.0f;
                s.st.snap++;
            }
        }
        else {
            std::memset(s.E, 0, 12);
            s.holdB = 0.0f;
            odt = 0.0;
        }

        float O[3], OQ[4];
        for (int i = 0; i < 3; i++) O[i] = P[i] + s.E[i];
        if (s.holdB > 0.01f) Slerp(OQ, Q, s.holdQ, s.holdB);
        else                 std::memcpy(OQ, Q, 16);
        if (odt > 1.0)
            for (int i = 0; i < 3; i++) s.vel[i] = (O[i] - s.outP[i]) / static_cast<float>(odt);
        else
            std::memset(s.vel, 0, 12);
        if (ex) {
            float sp = sqrtf(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);
            if (sp > kMaxSpeed)
                for (int i = 0; i < 3; i++) s.vel[i] *= kMaxSpeed / sp;
        }
        std::memcpy(s.outP, O, 12);
        std::memcpy(s.outQ, OQ, 16);
        s.outOk = true;
        s.outLast = now;
        s.extrap = ex != 0;
        Stutter(s.oLast, &s.oV, &s.oOk, O, dt, &s.st.stutterSmooth, nullptr);

        if (handover) {
            float lag = sqrtf((O[0] - handover->pos[0]) * (O[0] - handover->pos[0])
                            + (O[2] - handover->pos[2]) * (O[2] - handover->pos[2]));
            s.st.lagSum += lag;
            s.st.lagN++;
        }

        if (!Finite(O, 3) || !Finite(OQ, 4)) return false;

        std::memcpy(out.pos, O, 12);
        std::memcpy(out.quat, OQ, 16);
        if (handover) {
            if (!s.ours) s.xb = 1.0f;
            if (s.xb > 0.0f && Finite(handover->pos, 3)) {
                float xb = s.xb > 1.0f ? 1.0f : s.xb;
                for (int i = 0; i < 3; i++)
                    out.pos[i] = O[i] + (handover->pos[i] - O[i]) * xb;
                if (Finite(handover->quat, 4)) {
                    float wq[4];
                    std::memcpy(wq, handover->quat, 16);
                    Norm4(wq);
                    Slerp(out.quat, OQ, wq, xb);
                }
                s.xb -= static_cast<float>(dt / 100.0);   // NS_BLEND_MS
                if (s.xb < 0.0f) s.xb = 0.0f;
            }
        }
        s.ours = true;
        return true;
    }

} // namespace

    void Reset(uint8_t slot)
    {
        EnsureInit();
        if (slot < kSlots) ResetSlot(g_slot[slot]);
    }

    void ResetAll()
    {
        EnsureInit();
        for (auto& s : g_slot) s = Slot{};
        for (auto& r : g_rb)  r = RebaseState{};
        for (auto& d : g_dq)  d = DqState{};
    }

    void OnState(uint8_t slot, uint32_t seq, const float pos[3], const float quat[4], double nowMs)
    {
        EnsureInit();
        OnStateBody(slot, seq, pos, quat, nowMs);
    }

    void OnAnim(uint8_t slot, uint32_t seq, uint32_t sendMs, double nowMs)
    {
        EnsureInit();
        if (slot >= kSlots) return;
        uint32_t t = Rebase(slot, sendMs);
        if (!t) return;
        t = Dequant(slot, seq, t);
        OnAnimBody(slot, seq, static_cast<double>(t), nowMs);
    }

    bool Sample(uint8_t slot, double nowMs, double onewayMs, Transform& out,
                const Transform* handover)
    {
        EnsureInit();
        if (slot >= kSlots) return false;
        return SampleBody(slot, nowMs, onewayMs, out, handover);
    }

    const Stats& StatsFor(uint8_t slot)
    {
        EnsureInit();
        return g_slot[slot < kSlots ? slot : 0].st;
    }

    void ResetStats(uint8_t slot)
    {
        EnsureInit();
        if (slot < kSlots) g_slot[slot].st = Stats{};
    }

    bool Primed(uint8_t slot) noexcept
    {
        return slot < kSlots && g_init && g_slot[slot].synced && g_slot[slot].n >= 2;
    }

    bool Enabled(uint8_t slot) noexcept { return Primed(slot); }

} // namespace net::netsmooth
