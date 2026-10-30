#include "dsp/DynamicEq.h"

#include "dsp/Convolver.h"
#include "dsp/EqDesign.h"
#include "dsp/Equalizer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
 #include <xmmintrin.h>
 #define PS_DEQ_HAS_MXCSR 1
#endif

// Dynamic EQ (dsp-deq). Behaviour: see DynamicEq.h. Choices made here where the research leaves room (rule 10):
//
// Signal flow. Control ticks of 32 / 64 / 128 samples at <= 50 / <= 100 / > 100 kHz run on a fixed sample grid that
// does not depend on the host's block sizes (a block that ends inside a tick only splits the tick's audio), so the
// output is identical for any blocking:
//   x -> ring (input history) -> lookahead tap(s) -> xLa
//   detector input: x in both modes; Digital delays the resulting control stream by Dt. Delaying the detector input
//     instead would apply newly automated controls to old audio. Meters follow the delayed control stream too.
//   detector units -> level (dB, per sample) -> gain law + ballistics (per sample) -> band gain (dB, per sample)
//   control rate: at each tick start the band gain reached at the end of the previous tick is the target of the
//     audio path for the coming tick (one tick = 0.67 ms of control lag, identical in both modes; attack times below a
//     tick act like one tick):
//   Analog : that gain is designed (eqdesign::designAnalog, the Equalizer's code) and the SVF parameters glide linearly
//            to it over the tick (at most 16 section designs per tick, round robin; a band whose gain moved less than
//            0.003 dB is not redesigned, except to land exactly on 0 dB).
//   Digital: y = xLa (n - Dt) + sum_b c_b (n) (w_b * xLa)(n), c_b = 10^(gain_b / 20) - 1 at the tick ends, linear in
//            between, so the Digital gain envelope is the Analog one delayed by exactly Dt.
// Continuous parameters (freq, Q as log2; threshold, 1 - 1/ratio, knee, gain, offset in their units) are smoothed per
// tick by two cascaded one-poles (tau 5 ms each, like the Equalizer) and interpolated linearly per sample inside the
// tick, so automation never steps the gain. Discrete law changes (Dynamic on/off, direction) are declicked: the jump
// between the old and the new band gain is added as an offset that fades out linearly over 15 ms. Switching a band
// to dynamic starts its ballistics at rest.
// Detector: TPT SVF sections (double), coefficients recomputed per segment from the smoothed freq / Q (the detector
// signal is only heard in band solo, so its coefficients step per segment instead of gliding). Proportional Q uses
// the effective Q at the band's gain G (not at the moving gain, so the detector does not move with the dynamics).
// The detector level uses a fast log2 (atanh series, error < 1e-6 dB). Ballistics act on min (c, |G|) (time to 1 - 1/e;
// snaps within 1e-9 dB, so a resting band returns to exactly 0 dB and to a bit-exact unity filter).
// A detector unit that starts (band enabled, sidechain source changed) starts from silence (mean square 0).
// Digital mode: one 2-channel ps::Convolver per band and FIR size. The kernel of the size in use is prepared in
// prepare(); the other sizes are prepared by the worker the first time they are requested (the audio thread never
// allocates; until the worker has prepared a size, the output is the exact delayed input at the new latency and the
// bands fade in afterwards). A convolver that starts (band enabled, mode / size / channel-mode switch) has no input
// history: its band joins only after N + P samples of history, its current kernel generation and a 2P drain, then
// fades in over 15 ms. After prepare / reset the history is valid (silence), so the bands join at once.
// Each channel kernel is published separately; in Stereo both channels get the same kernel, and a kernel change can
// reach the second channel one partition later (both are crossfaded by the convolver; only during an edit).
// Band solo: the detector filter output of the band's sidechain source, per channel (Stereo: set A on both channels,
// M/S / L/R: set A band n on ch0, set B band n on ch1), delayed by Dt + lookahead in Digital (lookahead in Analog),
// starting only when the delay line holds valid audio; switching the soloed band fades out and in again.
namespace ps
{
std::complex<double> dynEqBandResponse (const DynamicEqBandParams& b, double bandGainDb, double freqHz,
                                        double sampleRate) noexcept
{
    EqBandParams e;
    e.on = true;
    e.shape = dynEqToEqShape (b.shape);
    e.freqHz = b.freqHz;
    e.gainDb = (float) bandGainDb;
    e.q = b.q;
    return eqBandResponse (e, EqFilterMode::Analog, freqHz, sampleRate, 100.0f);
}

namespace
{
double clampGain (double db) noexcept { return std::clamp (db, -30.0, 15.0); }

// true: the band rests at 0 dB dynamic gain and moves to G when triggered
bool movesTowardGain (const DynamicEqBandParams& b) noexcept
{
    const bool cut = b.gainDb < 0.0f;
    return b.direction == DynEqDirection::Down ? cut : ! cut;
}
} // namespace

double dynEqRestGainDb (const DynamicEqBandParams& b) noexcept
{
    if (! b.dynamic)
        return clampGain ((double) b.offsetDb + (double) b.gainDb);
    return clampGain ((double) b.offsetDb + (movesTowardGain (b) ? 0.0 : (double) b.gainDb));
}

double dynEqTriggeredGainDb (const DynamicEqBandParams& b) noexcept
{
    if (! b.dynamic)
        return clampGain ((double) b.offsetDb + (double) b.gainDb);
    return clampGain ((double) b.offsetDb + (movesTowardGain (b) ? (double) b.gainDb : 0.0));
}

namespace
{
using eqdesign::BandDesign;
using eqdesign::Svf;

constexpr double kPi = 3.14159265358979323846;
constexpr int kNumFirSizes = 4;
constexpr int kMaxTick = 128;
constexpr int kDesignBudget = 16;          // sections designed per control tick
constexpr double kSmoothTau = 0.005;       // s, per stage (two cascaded one-poles)
constexpr double kBandFadeSeconds = 0.015;
constexpr double kConfigFadeSeconds = 0.004;
constexpr double kMonitorFadeSeconds = 0.010;
constexpr double kLookaheadFadeSeconds = 0.010;
constexpr double kMaxLookaheadMs = 10.0;
constexpr double kDbToLn = 0.11512925464970229;    // ln (10) / 20
constexpr double kPowLog2ToDb = 3.0102999566398120; // 10 log10 (2)
constexpr double kBpQScale = 0.64359425290558262;   // sqrt (sqrt (2) - 1): two cascaded sections keep the -3 dB width
constexpr double kMinLevelDb = -200.0;
constexpr int kNumUnits = kNumChannelSets * kDynEqBands * 2;

struct DenormalGuard
{
#if PS_DEQ_HAS_MXCSR
    unsigned int saved;
    DenormalGuard() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
    ~DenormalGuard() { _mm_setcsr (saved); }
#endif
};

// log2 of a positive, normal double (atanh series on a mantissa in [sqrt 0.5, sqrt 2): error < 1e-7)
inline double fastLog2 (double x) noexcept
{
    std::uint64_t bits;
    std::memcpy (&bits, &x, sizeof (bits));
    int e = (int) ((bits >> 52) & 0x7ffu) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m;
    std::memcpy (&m, &bits, sizeof (m));
    if (m > 1.4142135623730951)
    {
        m *= 0.5;
        ++e;
    }
    const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    return (double) e
         + 2.8853900817779268 * s * (1.0 + s2 * (1.0 / 3.0 + s2 * (0.2 + s2 * (1.0 / 7.0 + s2 * (1.0 / 9.0)))));
}

inline double levelDbOf (double meanSquare) noexcept
{
    if (! (meanSquare > 1.0e-20))
        return kMinLevelDb;
    return std::max (kMinLevelDb, kPowLog2ToDb * fastLog2 (meanSquare));
}

// Movement in dB for x = level - threshold (soft knee of width K centred on the threshold), k = 1 - 1 / ratio.
inline double movement (double x, double k, double K) noexcept
{
    if (K > 1.0e-9)
    {
        const double h = 0.5 * K;
        if (x <= -h)
            return 0.0;
        if (x < h)
        {
            const double u = x + h;
            return k * u * u / (2.0 * K);
        }
        return k * x;
    }
    return x > 0.0 ? k * x : 0.0;
}

bool isBandPass (DynEqShape s) noexcept
{
    return s == DynEqShape::Bell || s == DynEqShape::ProportionalQ || s == DynEqShape::BandShelf;
}

bool isLowShelf (DynEqShape s) noexcept
{
    return s == DynEqShape::LowShelfBaxandall || s == DynEqShape::LowShelfAnalog;
}

//==============================================================================
// Audio-path SVF section with per-sample parameter glide (the Equalizer's realisation, EqDesign.h toSvf).
struct SvfSection
{
    Svf c, step, target;
    double a1 = 1.0 / 3.0, a2 = 1.0 / 3.0, a3 = 1.0 / 3.0;
    double ic1 = 0.0, ic2 = 0.0;

    void derive() noexcept
    {
        a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
        a2 = c.g * a1;
        a3 = c.g * a2;
    }

    inline double tick (double v0) noexcept
    {
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        if (c.isIdentity())
            return v0;   // a 0 dB band keeps its history without changing a single output bit
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }

    inline void advance() noexcept
    {
        c.g += step.g;
        c.k += step.k;
        c.m0 += step.m0;
        c.m1 += step.m1;
        c.m2 += step.m2;
        derive();
    }

    void clearState() noexcept { ic1 = ic2 = 0.0; }
};

struct Slot
{
    int n = 0;
    SvfSection sec[eqdesign::kMaxSections];
    int rampLeft = 0;

    void clear() noexcept
    {
        n = 0;
        rampLeft = 0;
        for (auto& s : sec)
            s.clearState();
    }

    void setTarget (const BandDesign& d, int rampLen, bool snap) noexcept
    {
        if (d.numSections != n)
        {
            snap = true;
            n = d.numSections;
            for (auto& s : sec)
                s.clearState();
        }
        bool moving = false;
        const double inv = 1.0 / (double) std::max (1, rampLen);
        for (int i = 0; i < n; ++i)
        {
            auto& s = sec[i];
            s.target = eqdesign::toSvf (d.s[i]);
            if (snap)
            {
                s.step = Svf {};
                s.step.g = s.step.k = s.step.m0 = 0.0;
            }
            else
            {
                s.step.g = (s.target.g - s.c.g) * inv;
                s.step.k = (s.target.k - s.c.k) * inv;
                s.step.m0 = (s.target.m0 - s.c.m0) * inv;
                s.step.m1 = (s.target.m1 - s.c.m1) * inv;
                s.step.m2 = (s.target.m2 - s.c.m2) * inv;
                moving = moving || s.step.g != 0.0 || s.step.k != 0.0 || s.step.m0 != 0.0 || s.step.m1 != 0.0
                         || s.step.m2 != 0.0;
            }
        }
        rampLeft = (! snap && moving) ? std::max (1, rampLen) : 0;
        if (rampLeft == 0)
            for (int i = 0; i < n; ++i)
            {
                sec[i].c = sec[i].target;
                sec[i].derive();
            }
    }

    void process (double* buf, int m) noexcept
    {
        if (n == 0)
            return;
        int i0 = 0;
        if (rampLeft > 0)
        {
            const int r = std::min (m, rampLeft);
            for (int i = 0; i < r; ++i)
            {
                double x = buf[i];
                for (int s = 0; s < n; ++s)
                {
                    sec[s].advance();
                    x = sec[s].tick (x);
                }
                buf[i] = x;
            }
            rampLeft -= r;
            if (rampLeft == 0)
                for (int s = 0; s < n; ++s)
                {
                    sec[s].c = sec[s].target;   // exact end point (no accumulated rounding)
                    sec[s].derive();
                }
            i0 = r;
        }
        if (i0 < m)
            for (int s = 0; s < n; ++s)
            {
                auto& sc = sec[s];
                for (int i = i0; i < m; ++i)
                    buf[i] = sc.tick (buf[i]);
            }
        for (int s = 0; s < n; ++s)
        {
            if (std::abs (sec[s].ic1) < 1.0e-30) sec[s].ic1 = 0.0;
            if (std::abs (sec[s].ic2) < 1.0e-30) sec[s].ic2 = 0.0;
        }
    }
};

struct AnBand
{
    Slot slot[2];
    int active = 0, fadePos = 0;
    bool fading = false;
    bool curOn = false;
    DynEqShape curShape = DynEqShape::Bell;
    double designedGain = 0.0;

    bool isActive() const noexcept { return fading || slot[active].n != 0; }

    void clear() noexcept
    {
        slot[0].clear();
        slot[1].clear();
        active = 0;
        fadePos = 0;
        fading = false;
        curOn = false;
        designedGain = 0.0;
    }
};

//==============================================================================
// Detector: two cascaded TPT SVF sections per channel (band pass at unity peak, or Butterworth LP / HP = LR4), then
// a one-pole mean square.
struct DetUnit
{
    int kind = 0;   // 0 band pass, 1 low pass, 2 high pass
    double k = 1.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, msCoeff = 0.01;
    double st[2][2][2] {};   // [channel][section][ic1, ic2]
    double ms[2] {};
    bool running = false;

    void clear() noexcept
    {
        for (auto& c : st)
            for (auto& s : c)
                s[0] = s[1] = 0.0;
        ms[0] = ms[1] = 0.0;
    }

    void setCoeffs (int filterKind, double g, double damping, double msC) noexcept
    {
        kind = filterKind;
        k = damping;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        msCoeff = msC;
    }

    inline double filter (int ch, double x) noexcept
    {
        for (int s = 0; s < 2; ++s)
        {
            double* z = st[ch][s];
            const double v3 = x - z[1];
            const double v1 = a1 * z[0] + a2 * v3;
            const double v2 = z[1] + a2 * z[0] + a3 * v3;
            z[0] = 2.0 * v1 - z[0];
            z[1] = 2.0 * v2 - z[1];
            x = kind == 0 ? k * v1 : (kind == 1 ? v2 : x - k * v1 - v2);
        }
        return x;
    }

    void flush() noexcept
    {
        for (auto& c : st)
            for (auto& s : c)
                for (double& z : s)
                    if (std::abs (z) < 1.0e-30)
                        z = 0.0;
        for (double& v : ms)
            if (v < 1.0e-30)
                v = 0.0;
    }
};

//==============================================================================
struct Smooth2
{
    double s1 = 0.0, s2 = 0.0;

    void snap (double v) noexcept { s1 = s2 = v; }

    // two cascaded one-poles; returns true while moving
    bool step (double target, double alpha, double eps) noexcept
    {
        if (std::abs (target - s1) <= eps && std::abs (target - s2) <= eps)
        {
            if (s1 == target && s2 == target)
                return false;
            s1 = s2 = target;
            return true;
        }
        s1 += (target - s1) * alpha;
        s2 += (s1 - s2) * alpha;
        return true;
    }
};

// Per channel set and band: smoothed controls, ballistics, gain law state and the per-sample gain of the segment.
struct GainSample
{
    float weight = 0.0f;   // c (float: 16-byte samples, 2 x 12 x 32k at 48 kHz)
    float gain = 0.0f, dynamic = 0.0f, level = (float) kMinLevelDb;
};

struct BandRt
{
    double tLf = 10.0, tLq = -0.5, tThr = -30.0, tK = 0.5, tKnee = 6.0, tG = 0.0, tOff = 0.0;
    Smooth2 lf, lq, thr, k, knee, G, off;
    // per-sample values inside the current tick (linear from the previous tick end to this tick's end) and steps
    double cThr = -30.0, cK = 0.5, cKnee = 6.0, cG = 0.0, cOff = 0.0;
    double dThr = 0.0, dK = 0.0, dKnee = 0.0, dG = 0.0, dOff = 0.0;
    bool fqMoved = false;   // freq / Q / G moved since the last analog design
    double attC = 1.0, relC = 1.0;
    double ds = 0.0;        // ballistics state: movement in dB
    bool lawOn = false, lawDynamic = true;
    DynEqDirection lawDir = DynEqDirection::Down;
    double lastGain = 0.0, lastDyn = 0.0;   // at the last processed sample
    double click = 0.0, clickStep = 0.0;
    int clickLeft = 0;
    int srcUnit = 0;
    double detMax = kMinLevelDb;
    float meterGain = 0.0f, meterDynamic = 0.0f;
    GainSample initialGain;
    // Digital: control-rate weight c = 10^(gain / 20) - 1, linear over each tick toward the previous tick's end gain
    double cCur = 0.0, cStep = 0.0, cTarget = 0.0;
    bool cValid = false;
};

//==============================================================================
struct Ring
{
    std::vector<float> buf[2];
    int mask = 0, pos = 0;

    void prepare (int maxDelay)
    {
        int size = 16;
        while (size < maxDelay + kMaxTick + 1)
            size <<= 1;
        for (auto& b : buf)
            b.assign ((size_t) size, 0.0f);
        mask = size - 1;
        pos = 0;
    }

    void clear() noexcept
    {
        for (auto& b : buf)
            std::fill (b.begin(), b.end(), 0.0f);
        pos = 0;
    }

    void write (const float* const* in, int n) noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            float* b = buf[c].data();
            for (int i = 0; i < n; ++i)
                b[(pos + i) & mask] = in[c][i];
        }
        pos = (pos + n) & mask;
    }

    // after write (): the last n samples delayed by `delay`
    void read (float* const* out, int n, int delay) noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            const float* b = buf[c].data();
            for (int i = 0; i < n; ++i)
                out[c][i] = b[(pos - n + i - delay) & mask];
        }
    }

    float at (int c, int n, int i, int delay) const noexcept
    {
        return buf[c][(size_t) ((pos - n + i - delay) & mask)];
    }
};

//==============================================================================
// What the worker designs for one band channel: the unit kernel W at the band's extreme gain g_ref.
struct KernelKey
{
    bool on = false;   // false: zero kernel
    DynEqShape shape = DynEqShape::Bell;
    float freq = 0.0f, q = 0.0f;
    double gref = 0.0;

    bool operator== (const KernelKey& o) const noexcept
    {
        if (on != o.on)
            return false;
        return ! on || (shape == o.shape && freq == o.freq && q == o.q && gref == o.gref);
    }
    bool operator!= (const KernelKey& o) const noexcept { return ! (*this == o); }
};

struct DesignRequest
{
    bool digital = false, dual = false;
    int firIdx = 1;
    KernelKey keys[kNumChannelSets][kDynEqBands];
    std::uint64_t seq = 0;

    bool sameAs (const DesignRequest& o) const noexcept
    {
        if (digital != o.digital)
            return false;
        if (! digital)
            return true;
        if (dual != o.dual || firIdx != o.firIdx)
            return false;
        for (int s = 0; s < (dual ? 2 : 1); ++s)
            for (int b = 0; b < kDynEqBands; ++b)
                if (keys[s][b] != o.keys[s][b])
                    return false;
        return true;
    }
};

// Single-producer / single-consumer triple buffer (the producer never waits, the consumer gets the latest value).
class RequestMailbox
{
public:
    void publish (const DesignRequest& r) noexcept
    {
        slots[back] = r;
        back = middle.exchange (back | kDirty, std::memory_order_acq_rel) & 3;
    }

    bool consume (DesignRequest& out) noexcept
    {
        if ((middle.load (std::memory_order_acquire) & kDirty) == 0)
            return false;
        front = middle.exchange (front, std::memory_order_acq_rel) & 3;
        out = slots[front];
        return true;
    }

    void clear() noexcept
    {
        back = 0;
        middle.store (1);
        front = 2;
    }

private:
    static constexpr int kDirty = 4;
    DesignRequest slots[3];
    int back = 0, front = 2;
    std::atomic<int> middle { 1 };
};

struct FirSize
{
    std::unique_ptr<Convolver> conv[kDynEqBands];
    std::atomic<bool> prepared { false };
    std::atomic<std::uint64_t> doneSeq[kDynEqBands][2];
};

enum class DigState { Wait, Drain, Open };

struct DigBand
{
    bool running = false;
    bool enabled[2] {};
    int enabledAge[2] {};
    long long runCount = 0;
    DigState state[2] { DigState::Wait, DigState::Wait };
    int drain[2] {};
    double weight[2] {};
    std::uint64_t required[2] {};
};

struct Config
{
    EqFilterMode mode = EqFilterMode::Analog;
    int firIdx = 1;
    bool dual = false;

    bool operator== (const Config& o) const noexcept
    {
        return mode == o.mode && (mode != EqFilterMode::Digital || firIdx == o.firIdx) && dual == o.dual;
    }
    bool operator!= (const Config& o) const noexcept { return ! (*this == o); }
};

double sanitise (float v, double lo, double hi, double fallback) noexcept
{
    return std::isfinite (v) ? std::clamp ((double) v, lo, hi) : fallback;
}

KernelKey keyOf (const DynamicEqBandParams& b) noexcept
{
    KernelKey k;
    if (! b.on)
        return k;
    const double r = dynEqRestGainDb (b), t = dynEqTriggeredGainDb (b);
    const double gref = std::abs (r) >= std::abs (t) ? r : t;
    if (! (std::abs (gref) > 1.0e-6))
        return k;
    k.on = true;
    k.shape = b.shape;
    k.freq = b.freqHz;
    k.q = b.q;
    k.gref = gref;
    return k;
}
} // namespace

//==============================================================================
struct DynamicEq::Impl
{
    double sr = 48000.0;
    bool prepared = false;

    // ---- parameters / config (audio thread)
    DynamicEqParams params;
    Config target, cur;
    bool needSnap = true, analogSnapPending = true, digSnap = true, controlSnap = true;
    std::atomic<int> latency { 0 }, targetMode { 0 }, targetFirIdx { 1 };

    // ---- control
    int tickLen = 32, tickLeft = 0, budget = kDesignBudget, roundRobin = 0;
    double tickAlpha = 0.1;
    BandRt rt[kNumChannelSets][kDynEqBands];
    DetUnit units[kNumUnits];
    bool unitNeeded[kNumUnits] {};
    float lvl[kNumUnits][kMaxTick] {};

    // ---- lookahead
    int laTarget = 0, laCur = 0, laNext = 0, laFadePos = 0, laFadeLen = 480, laMax = 480;
    bool laFading = false;
    std::vector<float> laFade;
    int laTapA[kMaxTick] {}, laTapB[kMaxTick] {};
    float laMix[kMaxTick] {};

    // ---- buffers
    Ring inRing, laRing;
    float xs[2][kMaxTick] {}, xla[2][kMaxTick] {}, direct[2][kMaxTick] {}, out[2][kMaxTick] {};
    double work[2][kMaxTick] {}, tmp[kMaxTick] {};
    float cbuf[2][kMaxTick] {};
    double cSeg[kNumChannelSets][kDynEqBands][kMaxTick] {};
    std::vector<GainSample> gainHistory[kNumChannelSets][kDynEqBands];
    int gainPos = 0, gainMask = 0;
    long long gainCount = 0;

    // ---- analog
    AnBand an[2][kDynEqBands];
    std::vector<double> bandFade;
    int bandFadeLen = 720;

    // ---- digital
    FirSize fir[kNumFirSizes];
    DigBand dig[kDynEqBands];

    // ---- monitors
    ModuleMonitor monitor;
    double audMix = 0.0, audStep = 0.0, audG = 0.0, audK = 0.25;
    double audIc1[2] {}, audIc2[2] {};
    double audSelect[2] { 1.0, 1.0 };
    int soloBand = -1, soloUnit[2] { -1, -1 };
    DynEqShape soloShape[2] { DynEqShape::Bell, DynEqShape::Bell };
    bool soloStructuralFade = false;
    DetUnit soloHeld[2];
    int soloHeldInput[2] {}, soloHeldChannel[2] {};
    long long soloFill = 0;
    double soloMix = 0.0;
    Ring soloRing;
    float soloSig[2][kMaxTick] {}, soloOut[2][kMaxTick] {};

    // ---- config fade
    double fadeGain = 1.0, fadeStep = 0.0;
    int fadeDir = 0, fadeRemaining = 0, cfgFadeLen = 192, clickLen = 720;

    // ---- meters
    DynamicEqMeters meters;

    // ---- worker
    RequestMailbox mailbox;
    DesignRequest lastReq;
    std::atomic<std::uint64_t> publishedSeq { 0 };
    std::atomic<std::uint64_t> completedSeq { 0 };  // requested nonzero kernels consumed by the audio thread
    std::atomic<bool> quit { false };
    std::thread worker;
    eqdesign::KernelDesigner designer;
    std::vector<float> taps[2];
    KernelKey installed[kNumFirSizes][kDynEqBands][2];   // worker only

    Impl()
    {
        for (auto& f : fir)
        {
            for (auto& c : f.conv)
                c = std::make_unique<Convolver>();
            for (auto& b : f.doneSeq)
                for (auto& s : b)
                    s.store (0);
        }
    }

    ~Impl() { stopWorker(); }

    //==========================================================================
    int setOfChannel (int ch) const noexcept { return cur.dual ? ch : 0; }

    int lookaheadSamples (float ms) const noexcept
    {
        return std::clamp ((int) std::lround (sanitise (ms, 0.0, kMaxLookaheadMs, 0.0) * sr / 1000.0), 0, laMax);
    }

    int latencyOf (const Config& c, int la) const noexcept
    {
        return la + (c.mode == EqFilterMode::Digital ? eqdesign::firLatency (c.firIdx, sr) : 0);
    }

    void sanitiseParams() noexcept
    {
        for (auto& set : params.bands)
            for (auto& b : set)
            {
                b.freqHz = (float) sanitise (b.freqHz, 20.0, 20000.0, 1000.0);
                b.gainDb = (float) sanitise (b.gainDb, -30.0, 15.0, 0.0);
                b.q = (float) sanitise (b.q, 0.1, 24.0, 0.707);
                b.thresholdDb = (float) sanitise (b.thresholdDb, -130.0, 0.0, -30.0);
                b.ratio = (float) sanitise (b.ratio, 1.0, 20.0, 2.0);
                b.kneeDb = (float) sanitise (b.kneeDb, 0.0, 24.0, 6.0);
                b.attackMs = (float) sanitise (b.attackMs, 0.1, 500.0, 5.0);
                b.releaseMs = (float) sanitise (b.releaseMs, 1.0, 5000.0, 80.0);
                b.offsetDb = (float) sanitise (b.offsetDb, -15.0, 15.0, 0.0);
            }
    }

    void updateTargets() noexcept
    {
        target.mode = params.filterMode;
        target.firIdx = std::clamp (params.firSizeIndex, 0, kNumFirSizes - 1);
        laTarget = lookaheadSamples (params.lookaheadMs);
        latency.store (latencyOf (target, laTarget), std::memory_order_relaxed);
        targetMode.store ((int) target.mode, std::memory_order_relaxed);
        targetFirIdx.store (target.firIdx, std::memory_order_relaxed);

        const double fMax = 0.49 * sr;
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                const auto& bp = params.bands[s][b];
                auto& r = rt[s][b];
                r.tLf = std::log2 (std::clamp ((double) bp.freqHz, 10.0, fMax));
                r.tLq = std::log2 (std::clamp ((double) bp.q, 0.05, 50.0));
                r.tThr = bp.thresholdDb;
                r.tK = 1.0 - 1.0 / std::max (1.0, (double) bp.ratio);
                r.tKnee = bp.kneeDb;
                r.tG = bp.gainDb;
                r.tOff = bp.offsetDb;
            }
        publishRequest();
    }

    void publishRequest() noexcept
    {
        DesignRequest r;
        r.digital = target.mode == EqFilterMode::Digital;
        r.dual = target.dual;
        r.firIdx = target.firIdx;
        if (r.digital)
            for (int s = 0; s < kNumChannelSets; ++s)
                for (int b = 0; b < kDynEqBands; ++b)
                    r.keys[s][b] = keyOf (params.bands[s][b]);
        if (r.sameAs (lastReq))
            return;
        r.seq = lastReq.seq + 1;
        lastReq = r;
        mailbox.publish (r);
        publishedSeq.store (r.seq, std::memory_order_release);
    }

    //==========================================================================
    void snapSmoothing() noexcept
    {
        for (auto& set : rt)
            for (auto& r : set)
            {
                r.lf.snap (r.tLf);
                r.lq.snap (r.tLq);
                r.thr.snap (r.tThr);
                r.k.snap (r.tK);
                r.knee.snap (r.tKnee);
                r.G.snap (r.tG);
                r.off.snap (r.tOff);
                r.cThr = r.tThr;
                r.cK = r.tK;
                r.cKnee = r.tKnee;
                r.cG = r.tG;
                r.cOff = r.tOff;
                r.dThr = r.dK = r.dKnee = r.dG = r.dOff = 0.0;
                r.fqMoved = true;
            }
    }

    void resetDetection() noexcept
    {
        for (auto& u : units)
        {
            u.clear();
            u.running = false;
        }
        for (auto& set : rt)
            for (auto& r : set)
            {
                r.ds = 0.0;
                r.lawOn = false;
                r.click = r.clickStep = 0.0;
                r.clickLeft = 0;
                r.lastGain = r.lastDyn = 0.0;
                r.meterGain = r.meterDynamic = 0.0f;
                r.detMax = kMinLevelDb;
                r.cCur = r.cStep = r.cTarget = 0.0;
                r.cValid = false;
            }
    }

    void applyConfig() noexcept
    {
        const Config prev = cur;
        cur = target;
        if (needSnap)
        {
            snapSmoothing();
            laCur = laNext = laTarget;
            laFading = false;
        }
        else if (prev.dual != cur.dual)
        {
            resetDetection();   // the channels carry other signals now (L/R <-> M/S); set B was not smoothed
            snapSmoothing();
            gainPos = 0;
            gainCount = 0;      // previous channel-domain control history is no longer meaningful
        }
        analogSnapPending = true;
        controlSnap = true;
        for (auto& d : dig)
            d.running = false;  // a (re)started convolver rebuilds its history before its band joins
        tickLeft = 0;           // the new configuration starts with a control tick
        needSnap = false;
    }

    //==========================================================================
    void lookahead (int m) noexcept
    {
        if (! laFading && laTarget != laCur)
        {
            laNext = laTarget;
            laFading = true;
            laFadePos = 0;
        }
        for (int i = 0; i < m; ++i)
        {
            laTapA[i] = laCur;
            laTapB[i] = laFading ? laNext : laCur;
            laMix[i] = laFading ? laFade[(size_t) laFadePos] : 0.0f;
            for (int c = 0; c < 2; ++c)
            {
                const float a = inRing.at (c, m, i, laCur);
                if (laFading)
                {
                    const float b = inRing.at (c, m, i, laNext);
                    const float r = laFade[(size_t) laFadePos];
                    xla[c][i] = a + (b - a) * r;
                }
                else
                    xla[c][i] = a;
            }
            if (laFading && ++laFadePos >= laFadeLen)
            {
                laCur = laNext;
                laFading = false;
            }
        }
    }

    //==========================================================================
    int sourceUnit (int s, int b) const noexcept
    {
        const auto sc = params.bands[s][b].sidechain;
        int j = b, cs = 0;
        if (sc >= DynEqSidechain::Band1 && sc <= DynEqSidechain::Band6)
            j = (int) sc - (int) DynEqSidechain::Band1;
        else if (sc == DynEqSidechain::OtherChannel && cur.dual)
            cs = 1;
        return (s * kDynEqBands + j) * 2 + cs;
    }

    // Gain of the law at rest (no trigger), from the smoothed controls.
    double restGain (int s, int b) const noexcept
    {
        const auto& r = rt[s][b];
        const auto& bp = params.bands[s][b];
        const double G = r.G.s2, off = r.off.s2;
        if (! bp.dynamic)
            return clampGain (off + G);
        const bool toward = (bp.direction == DynEqDirection::Down) == (G < 0.0);
        return clampGain (off + (toward ? 0.0 : G));
    }

    // The gain the audio path moves to during the coming tick: the band gain at the end of the previous tick.
    double controlGain (int s, int b) const noexcept
    {
        return rt[s][b].lawOn ? rt[s][b].lastGain : restGain (s, b);
    }

    // Once per tick (tickLen samples, independent of the host's block sizes): smoothing, ballistics coefficients and
    // the audio-path control (Analog designs / Digital weights) toward the gain at the end of the previous tick.
    void controlTick() noexcept
    {
        budget = kDesignBudget;
        roundRobin = (roundRobin + 1) % (2 * kDynEqBands);
        const double inv = 1.0 / (double) tickLen;
        const int numSets = cur.dual ? 2 : 1;
        for (int s = 0; s < numSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                auto& r = rt[s][b];
                const auto& bp = params.bands[s][b];
                r.cThr = r.thr.s2;   // exact start values (no accumulated rounding)
                r.cK = r.k.s2;
                r.cKnee = r.knee.s2;
                r.cG = r.G.s2;
                r.cOff = r.off.s2;
                bool moved = r.lf.step (r.tLf, tickAlpha, 1.0e-4);
                moved = r.lq.step (r.tLq, tickAlpha, 1.0e-4) || moved;
                r.thr.step (r.tThr, tickAlpha, 1.0e-4);
                r.k.step (r.tK, tickAlpha, 1.0e-6);
                r.knee.step (r.tKnee, tickAlpha, 1.0e-4);
                const bool gMoved = r.G.step (r.tG, tickAlpha, 1.0e-4);
                r.off.step (r.tOff, tickAlpha, 1.0e-4);
                if (moved || (gMoved && bp.shape == DynEqShape::ProportionalQ))
                    r.fqMoved = true;
                r.dThr = (r.thr.s2 - r.cThr) * inv;
                r.dK = (r.k.s2 - r.cK) * inv;
                r.dKnee = (r.knee.s2 - r.cKnee) * inv;
                r.dG = (r.G.s2 - r.cG) * inv;
                r.dOff = (r.off.s2 - r.cOff) * inv;

                const double f = std::exp2 (r.lf.s2);
                const double scale = bp.autoScale ? std::clamp (std::sqrt (1000.0 / f), 0.25, 4.0) : 1.0;
                r.attC = 1.0 - std::exp (-1000.0 / ((double) bp.attackMs * scale * sr));
                r.relC = 1.0 - std::exp (-1000.0 / ((double) bp.releaseMs * scale * sr));
                r.srcUnit = sourceUnit (s, b);
            }
        // Keep the control history running in Analog too, so a mode switch can use the same past gains.
        digitalControl();
        if (cur.mode == EqFilterMode::Analog)
            analogControl();
        if (gainCount == 0)
            for (int s = 0; s < numSets; ++s)
                for (int b = 0; b < kDynEqBands; ++b)
                {
                    auto& r = rt[s][b];
                    const auto& bp = params.bands[s][b];
                    r.initialGain.weight = (float) r.cCur;
                    r.initialGain.gain = bp.on ? (float) controlGain (s, b) : 0.0f;
                    const bool toward = (bp.direction == DynEqDirection::Down) == (r.G.s2 < 0.0);
                    r.initialGain.dynamic = bp.on && bp.dynamic && ! toward ? (float) r.G.s2 : 0.0f;
                    r.initialGain.level = (float) kMinLevelDb;
                }
        controlSnap = false;
    }

    void digitalControl() noexcept
    {
        const double inv = 1.0 / (double) tickLen;
        const int numSets = cur.dual ? 2 : 1;
        for (int s = 0; s < numSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                auto& r = rt[s][b];
                r.cCur = r.cTarget;
                if (! params.bands[s][b].on)
                {
                    r.cStep = 0.0;   // an off band fading out keeps its last weight
                    continue;
                }
                const double tgt = std::expm1 (controlGain (s, b) * kDbToLn);
                if (controlSnap || ! r.cValid)
                    r.cCur = tgt;
                r.cValid = true;
                r.cTarget = tgt;
                r.cStep = (tgt - r.cCur) * inv;
            }
    }

    void setUnitCoeffs (int u) noexcept
    {
        const int s = u / (2 * kDynEqBands), j = (u / 2) % kDynEqBands;
        const auto& r = rt[s][j];
        const auto& bp = params.bands[s][j];
        const double f = std::clamp (std::exp2 (r.lf.s2), 10.0, 0.49 * sr);
        const double g = std::tan (kPi * f / sr);
        const double tau = std::clamp (2.0 / f, 0.001, 0.050);
        const double msC = 1.0 - std::exp (-1.0 / (tau * sr));
        if (isBandPass (bp.shape))
        {
            double qd = std::exp2 (r.lq.s2);
            if (bp.shape == DynEqShape::ProportionalQ)
                qd = eqdesign::proportionalQ (qd, r.G.s2);
            qd = std::clamp (qd, 0.3, 8.0);
            units[u].setCoeffs (0, g, 1.0 / (kBpQScale * qd), msC);
        }
        else
            units[u].setCoeffs (isLowShelf (bp.shape) ? 1 : 2, g, 1.4142135623730951, msC);
    }

    void runUnits (const float* const* in, int m) noexcept
    {
        std::fill (std::begin (unitNeeded), std::end (unitNeeded), false);
        const int numSets = cur.dual ? 2 : 1;
        for (int s = 0; s < numSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                const auto& bp = params.bands[s][b];
                if (bp.on && bp.dynamic)
                    unitNeeded[rt[s][b].srcUnit] = true;
            }
        if (! soloStructuralFade)
        {
            soloUnit[0] = soloUnit[1] = -1;
            if (soloBand >= 0)
                for (int c = 0; c < 2; ++c)
                {
                    const int u = sourceUnit (setOfChannel (c), soloBand);
                    soloUnit[c] = u;
                    soloShape[c] = params.bands[u / (2 * kDynEqBands)][(u / 2) % kDynEqBands].shape;
                    unitNeeded[u] = true;
                }
        }

        for (int u = 0; u < kNumUnits; ++u)
        {
            auto& unit = units[u];
            if (! unitNeeded[u])
            {
                unit.running = false;
                continue;
            }
            if (! unit.running)
            {
                unit.clear();
                unit.running = true;
            }
            setUnitCoeffs (u);
            float* L = lvl[u];
            const double a = unit.msCoeff;
            if (! cur.dual)
            {
                const bool solo = soloUnit[0] == u;
                for (int i = 0; i < m; ++i)
                {
                    const double y0 = unit.filter (0, (double) in[0][i]);
                    const double y1 = unit.filter (1, (double) in[1][i]);
                    unit.ms[0] += (y0 * y0 - unit.ms[0]) * a;
                    unit.ms[1] += (y1 * y1 - unit.ms[1]) * a;
                    L[i] = (float) levelDbOf (std::max (unit.ms[0], unit.ms[1]));
                    if (solo)
                    {
                        soloSig[0][i] = (float) y0;
                        soloSig[1][i] = (float) y1;
                    }
                }
            }
            else
            {
                const int s = u / (2 * kDynEqBands), cs = u % 2;
                const int inCh = cs == 0 ? s : 1 - s;
                const int soloCh = soloUnit[0] == u ? 0 : (soloUnit[1] == u ? 1 : -1);
                for (int i = 0; i < m; ++i)
                {
                    const double y = unit.filter (0, (double) in[inCh][i]);
                    unit.ms[0] += (y * y - unit.ms[0]) * a;
                    L[i] = (float) levelDbOf (unit.ms[0]);
                    if (soloCh >= 0)
                        soloSig[soloCh][i] = (float) y;
                }
            }
            unit.flush();
        }
        if (soloStructuralFade)
            for (int c = 0; c < 2; ++c)
            {
                for (int i = 0; i < m; ++i)
                    soloSig[c][i] = (float) soloHeld[c].filter (soloHeldChannel[c], in[soloHeldInput[c]][i]);
                soloHeld[c].flush();
            }
    }

    //==========================================================================
    void computeGains (int s, int b, int m) noexcept
    {
        auto& r = rt[s][b];
        const auto& bp = params.bands[s][b];
        if (! bp.on)
        {
            r.lawOn = false;
            r.lastGain = r.lastDyn = 0.0;
            for (int i = 0; i < m; ++i)
                gainHistory[s][b][(size_t) ((gainPos + i) & gainMask)] = GainSample {};
            return;
        }
        const bool fresh = ! r.lawOn;
        const bool lawChange = ! fresh && (bp.dynamic != r.lawDynamic || (bp.dynamic && bp.direction != r.lawDir));
        if (bp.dynamic && (fresh || ! r.lawDynamic))
            r.ds = 0.0;
        r.lawOn = true;
        r.lawDynamic = bp.dynamic;
        r.lawDir = bp.direction;
        const double prevGain = r.lastGain;

        double thr = r.cThr, k = r.cK, knee = r.cKnee, G = r.cG, off = r.cOff;
        const bool down = bp.direction == DynEqDirection::Down;
        const float* L = lvl[r.srcUnit];
        double ds = r.ds, g = 0.0, total = r.lastGain;

        for (int i = 0; i < m; ++i)
        {
            thr += r.dThr;
            k += r.dK;
            knee += r.dKnee;
            G += r.dG;
            off += r.dOff;
            if (bp.dynamic)
            {
                const double level = (double) L[i];
                const double aG = std::abs (G);
                const double tgt = std::min (movement (level - thr, k, knee), aG);
                ds += (tgt - ds) * (tgt > ds ? r.attC : r.relC);
                if (std::abs (tgt - ds) < 1.0e-9)
                    ds = tgt;
                const double t = aG < 0.01 ? 0.0 : std::min (ds, aG) / aG;
                g = (down == (G < 0.0)) ? G * t : G * (1.0 - t);
                total = clampGain (off + g);
            }
            else
            {
                g = 0.0;
                total = clampGain (off + G);
            }
            if (i == 0 && lawChange)
            {
                r.click = prevGain - total;
                r.clickLeft = clickLen;
                r.clickStep = r.click / (double) clickLen;
            }
            if (r.clickLeft > 0)
            {
                total = clampGain (total + r.click);
                r.click -= r.clickStep;
                if (--r.clickLeft == 0)
                    r.click = 0.0;
            }
            auto& sample = gainHistory[s][b][(size_t) ((gainPos + i) & gainMask)];
            sample.gain = (float) total;
            sample.dynamic = (float) g;
            sample.level = bp.dynamic ? L[i] : (float) kMinLevelDb;
        }
        r.cThr = thr;
        r.cK = k;
        r.cKnee = knee;
        r.cG = G;
        r.cOff = off;
        r.ds = ds;
        r.lastGain = total;
        r.lastDyn = g;
    }

    //==========================================================================
    void designBand (int s, int b, double gainDb, BandDesign& d) const noexcept
    {
        const auto& r = rt[s][b];
        const auto& bp = params.bands[s][b];
        eqdesign::designAnalog (dynEqToEqShape (bp.shape), EqSlope::Db12, sr, std::exp2 (r.lf.s2), gainDb,
                                std::exp2 (r.lq.s2), d);
    }

    void analogControl() noexcept
    {
        const int m = tickLen;
        const int groups = cur.dual ? 2 * kDynEqBands : kDynEqBands;
        if (analogSnapPending)
        {
            for (int idx = 0; idx < groups; ++idx)
            {
                const int s = cur.dual ? idx / kDynEqBands : 0, b = idx % kDynEqBands;
                const auto& bp = params.bands[s][b];
                auto& r = rt[s][b];
                const double gNow = controlGain (s, b);
                BandDesign d;
                if (bp.on)
                    designBand (s, b, gNow, d);
                for (int ch = cur.dual ? s : 0; ch <= (cur.dual ? s : 1); ++ch)
                {
                    auto& ab = an[ch][b];
                    ab.clear();
                    ab.curOn = bp.on;
                    ab.curShape = bp.shape;
                    ab.designedGain = gNow;
                    if (bp.on)
                        ab.slot[0].setTarget (d, m, true);
                }
                r.fqMoved = false;
            }
            analogSnapPending = false;
            return;
        }

        for (int j = 0; j < groups; ++j)
        {
            const int idx = (roundRobin + j) % groups;
            const int s = cur.dual ? idx / kDynEqBands : 0, b = idx % kDynEqBands;
            const int ch0 = cur.dual ? s : 0, ch1 = cur.dual ? s : 1;
            const auto& bp = params.bands[s][b];
            auto& r = rt[s][b];
            auto& ab0 = an[ch0][b];
            const double gEnd = bp.on ? controlGain (s, b) : 0.0;

            const bool structural = bp.on != ab0.curOn || (bp.on && bp.shape != ab0.curShape);
            if (structural)
            {
                if (an[ch0][b].fading || an[ch1][b].fading)
                    continue;   // one crossfade at a time
                const int sections = bp.on ? eqdesign::sectionCount (dynEqToEqShape (bp.shape), EqSlope::Db12) : 0;
                if (sections > budget)
                    continue;
                BandDesign d;
                if (bp.on)
                    designBand (s, b, gEnd, d);
                budget -= sections;
                for (int ch = ch0; ch <= ch1; ++ch)
                {
                    auto& ab = an[ch][b];
                    const int nw = 1 - ab.active;
                    ab.slot[nw].clear();
                    if (bp.on)
                        ab.slot[nw].setTarget (d, m, true);
                    ab.active = nw;
                    ab.fading = true;
                    ab.fadePos = 0;
                    ab.curOn = bp.on;
                    ab.curShape = bp.shape;
                    ab.designedGain = gEnd;
                }
                r.fqMoved = false;
                continue;
            }
            if (! ab0.curOn)
                continue;

            const double dg = ab0.designedGain;
            const bool need = r.fqMoved || std::abs (gEnd - dg) > 0.003
                              || ((std::abs (gEnd) < 1.0e-6) != (std::abs (dg) < 1.0e-6));
            if (! need)
                continue;
            const int sections = eqdesign::sectionCount (dynEqToEqShape (bp.shape), EqSlope::Db12);
            if (sections > budget)
                continue;
            BandDesign d;
            designBand (s, b, gEnd, d);
            budget -= sections;
            r.fqMoved = false;
            for (int ch = ch0; ch <= ch1; ++ch)
            {
                auto& ab = an[ch][b];
                ab.slot[ab.active].setTarget (d, m, false);
                ab.designedGain = gEnd;
            }
        }
    }

    void processAnBand (AnBand& ab, double* buf, int m) noexcept
    {
        Slot& nw = ab.slot[ab.active];
        if (! ab.fading)
        {
            nw.process (buf, m);
            return;
        }
        Slot& old = ab.slot[1 - ab.active];
        std::copy (buf, buf + m, tmp);
        nw.process (buf, m);
        old.process (tmp, m);
        for (int i = 0; i < m; ++i)
        {
            const int pos = ab.fadePos + i;
            const double r = pos < bandFadeLen ? bandFade[(size_t) pos] : 1.0;
            buf[i] = tmp[i] + (buf[i] - tmp[i]) * r;
        }
        ab.fadePos += m;
        if (ab.fadePos >= bandFadeLen)
        {
            ab.fading = false;
            old.clear();
        }
    }

    void processAnalog (int m) noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            bool any = false;
            for (const auto& ab : an[c])
                any = any || ab.isActive();
            if (! any)
            {
                std::copy (xla[c], xla[c] + m, out[c]);
                continue;
            }
            double* w = work[c];
            for (int i = 0; i < m; ++i)
                w[i] = (double) xla[c][i];
            for (auto& ab : an[c])
                if (ab.isActive())
                    processAnBand (ab, w, m);
            for (int i = 0; i < m; ++i)
                out[c][i] = (float) w[i];
        }
    }

    //==========================================================================
    void delayControl (int m) noexcept
    {
        const int delay = cur.mode == EqFilterMode::Digital ? eqdesign::firLatency (cur.firIdx, sr) : 0;
        const int numSets = cur.dual ? 2 : 1;
        for (int s = 0; s < numSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                auto& r = rt[s][b];
                auto& history = gainHistory[s][b];
                double c = r.cCur;
                for (int i = 0; i < m; ++i)
                {
                    history[(size_t) ((gainPos + i) & gainMask)].weight = (float) (c += r.cStep);
                    const auto& sample = gainCount + i < delay ? r.initialGain
                        : history[(size_t) ((gainPos + i - delay) & gainMask)];
                    cSeg[s][b][i] = (double) sample.weight;
                    r.meterGain = sample.gain;
                    r.meterDynamic = sample.dynamic;
                    r.detMax = std::max (r.detMax, (double) sample.level);
                }
                r.cCur = c;
            }
        gainPos = (gainPos + m) & gainMask;
        gainCount += m;
    }

    void processDigital (int m) noexcept
    {
        auto& fs = fir[cur.firIdx];
        const bool ready = fs.prepared.load (std::memory_order_acquire);
        const int n = eqdesign::firLength (cur.firIdx, sr);
        const int P = eqdesign::firPartition (n);
        const int controlDelay = eqdesign::firLatency (cur.firIdx, sr);
        const long long hist = (long long) n + P;
        const double wStep = 1.0 / (double) bandFadeLen;
        bool any = false;

        for (int b = 0; b < kDynEqBands; ++b)
        {
            auto& d = dig[b];
            bool on[2];
            for (int c = 0; c < 2; ++c)
                on[c] = params.bands[setOfChannel (c)][b].on;
            const bool wantRun = on[0] || on[1] || d.weight[0] > 0.0 || d.weight[1] > 0.0;
            if (! ready || ! wantRun)
            {
                d = DigBand {};
                continue;
            }
            Convolver& conv = *fs.conv[b];
            if (! d.running)
            {
                conv.reset();
                d.running = true;
                d.runCount = digSnap ? hist : 0;
                for (int c = 0; c < 2; ++c)
                {
                    d.enabled[c] = false;
                    d.state[c] = DigState::Wait;
                    d.weight[c] = 0.0;
                    d.required[c] = lastReq.seq;
                }
            }
            for (int c = 0; c < 2; ++c)
            {
                if (on[c] && ! d.enabled[c])
                {
                    // The other channel may have kept this convolver running while this one was disabled.
                    // Retain its valid input history, but wait for this channel's newly requested kernel. Any
                    // unfinished fade continues toward zero until that generation has installed and drained.
                    d.state[c] = DigState::Wait;
                    d.drain[c] = 0;
                    d.required[c] = lastReq.seq;
                    d.enabledAge[c] = digSnap ? controlDelay : 0;
                }
                d.enabled[c] = on[c];
            }
            for (int c = 0; c < 2; ++c)
                std::copy (xla[c], xla[c] + m, cbuf[c]);
            float* cb[2] { cbuf[0], cbuf[1] };
            conv.process (cb, m);
            d.runCount += m;

            for (int c = 0; c < 2; ++c)
            {
                if (on[c])
                    d.enabledAge[c] = std::min (controlDelay, d.enabledAge[c] + m);
                // A kept-running convolver can have valid audio history while its delayed coefficient still
                // belongs to the band's previous enabled interval (possibly another gain sign/reference).
                if (d.state[c] == DigState::Wait && on[c] && d.runCount >= hist && d.enabledAge[c] >= controlDelay
                    && fs.doneSeq[b][c].load (std::memory_order_acquire) >= d.required[c] && ! conv.isKernelPending (c))
                {
                    if (digSnap)
                    {
                        d.state[c] = DigState::Open;
                        d.weight[c] = 1.0;
                    }
                    else
                    {
                        d.state[c] = DigState::Drain;
                        d.drain[c] = 2 * P;
                    }
                }
                else if (d.state[c] == DigState::Drain)
                {
                    d.drain[c] -= m;
                    if (d.drain[c] <= 0)
                        d.state[c] = DigState::Open;
                }
                const double tgt = (d.state[c] == DigState::Open && on[c]) ? 1.0 : 0.0;
                if (tgt == 0.0 && d.weight[c] == 0.0)
                    continue;
                const double* cw = cSeg[setOfChannel (c)][b];
                if (! any)
                {
                    std::fill (work[0], work[0] + m, 0.0);
                    std::fill (work[1], work[1] + m, 0.0);
                    any = true;
                }
                double* acc = work[c];
                double w = d.weight[c];
                for (int i = 0; i < m; ++i)
                {
                    w = tgt > w ? std::min (tgt, w + wStep) : std::max (tgt, w - wStep);
                    acc[i] += w * cw[i] * (double) cbuf[c][i];
                }
                d.weight[c] = w;
            }
        }

        for (int c = 0; c < 2; ++c)
        {
            if (! any)
                std::copy (direct[c], direct[c] + m, out[c]);
            else
                for (int i = 0; i < m; ++i)
                    out[c][i] = (float) ((double) direct[c][i] + work[c][i]);
        }
    }

    //==========================================================================
    void processAudition (const float* const* aligned, int m) noexcept
    {
        const double tgt = monitor.auditionOn ? 1.0 : 0.0;
        const int set = std::clamp (monitor.auditionChannelSet, 0, 1);
        if (audMix == 0.0 && tgt == 0.0)
            return;
        const double gT = std::tan (kPi * std::clamp ((double) monitor.auditionHz, 10.0, 0.45 * sr) / sr);
        const double kT = 1.0 / std::clamp ((double) monitor.auditionQ, 0.1, 24.0);
        if (audMix == 0.0)
        {
            audIc1[0] = audIc1[1] = audIc2[0] = audIc2[1] = 0.0;
            audG = gT;
            audK = kT;
            for (int c = 0; c < 2; ++c)
                audSelect[c] = ! cur.dual || c == set ? 1.0 : 0.0;
        }
        const double sm = 1.0 - std::exp (-1.0 / (0.01 * sr));
        for (int i = 0; i < m; ++i)
        {
            audG += (gT - audG) * sm;
            audK += (kT - audK) * sm;
            audMix = tgt > audMix ? std::min (tgt, audMix + audStep) : std::max (tgt, audMix - audStep);
            const double a1 = 1.0 / (1.0 + audG * (audG + audK));
            const double a2 = audG * a1;
            const double a3 = audG * a2;
            for (int c = 0; c < 2; ++c)
            {
                const double v0 = aligned[c][i];
                const double v3 = v0 - audIc2[c];
                const double v1 = a1 * audIc1[c] + a2 * v3;
                const double v2 = audIc2[c] + a2 * audIc1[c] + a3 * v3;
                audIc1[c] = 2.0 * v1 - audIc1[c];
                audIc2[c] = 2.0 * v2 - audIc2[c];
                const double heard = ! cur.dual || c == set ? 1.0 : 0.0;
                audSelect[c] += std::clamp (heard - audSelect[c], -audStep, audStep);
                const double bp = audSelect[c] * audK * v1;
                out[c][i] = (float) ((double) out[c][i] + (bp - (double) out[c][i]) * audMix);
            }
        }
        for (int c = 0; c < 2; ++c)
        {
            if (std::abs (audIc1[c]) < 1.0e-30) audIc1[c] = 0.0;
            if (std::abs (audIc2[c]) < 1.0e-30) audIc2[c] = 0.0;
        }
    }

    // Before the detectors: band changes and discrete detector edits fade out first. A monitor-only snapshot
    // preserves the old detector signal during a structural fade; the dynamics detectors update immediately.
    void soloUpdate() noexcept
    {
        const int req = monitor.soloBand >= 0 && monitor.soloBand < kDynEqBands ? monitor.soloBand : -1;
        if (soloBand < 0 && soloMix <= 0.0)
            soloStructuralFade = false;   // also clears a transition interrupted by reset()
        bool changed = false;
        if (soloBand >= 0 && ! soloStructuralFade)
            for (int c = 0; c < 2; ++c)
            {
                const int u = sourceUnit (setOfChannel (c), soloBand);
                const auto shape = params.bands[u / (2 * kDynEqBands)][(u / 2) % kDynEqBands].shape;
                changed = changed || u != soloUnit[c] || shape != soloShape[c];
            }
        if (changed && soloMix > 0.0)
        {
            for (int c = 0; c < 2; ++c)
            {
                const int u = soloUnit[c];
                soloHeld[c] = units[u];
                const int set = u / (2 * kDynEqBands);
                soloHeldInput[c] = cur.dual ? (u % 2 == 0 ? set : 1 - set) : c;
                soloHeldChannel[c] = cur.dual ? 0 : c;
            }
            soloStructuralFade = true;
        }
        if ((req != soloBand || changed || soloStructuralFade) && soloMix <= 0.0)
        {
            soloBand = req;
            soloStructuralFade = false;
            soloFill = 0;
            if (req >= 0)
                soloRing.clear();
        }
    }

    void processSolo (int m) noexcept
    {
        const int req = monitor.soloBand >= 0 && monitor.soloBand < kDynEqBands ? monitor.soloBand : -1;
        if (soloBand < 0 && soloMix <= 0.0)
            return;
        const int dt = cur.mode == EqFilterMode::Digital ? eqdesign::firLatency (cur.firIdx, sr) : 0;
        double tgt = 0.0;
        if (soloBand >= 0)
        {
            float* sig[2] { soloSig[0], soloSig[1] };
            soloRing.write (sig, m);
            soloFill += m;
            for (int i = 0; i < m; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    const float a = soloRing.at (c, m, i, dt + laTapA[i]);
                    const float b = soloRing.at (c, m, i, dt + laTapB[i]);
                    soloOut[c][i] = a + (b - a) * laMix[i];
                }
            const int delay = dt + std::max (laCur, laNext);
            tgt = (! soloStructuralFade && req == soloBand && soloFill >= (long long) delay + m) ? 1.0 : 0.0;
        }
        for (int i = 0; i < m; ++i)
        {
            soloMix = tgt > soloMix ? std::min (tgt, soloMix + audStep) : std::max (tgt, soloMix - audStep);
            if (soloMix > 0.0)
                for (int c = 0; c < 2; ++c)
                    out[c][i] = (float) ((double) out[c][i] + ((double) soloOut[c][i] - (double) out[c][i]) * soloMix);
        }
    }

    //==========================================================================
    void processSegment (float* const* p, int m) noexcept
    {
        if (tickLeft <= 0)
        {
            controlTick();
            tickLeft = tickLen;
        }

        for (int c = 0; c < 2; ++c)
            std::copy (p[c], p[c] + m, xs[c]);
        const float* xsP[2] { xs[0], xs[1] };
        inRing.write (xsP, m);
        lookahead (m);
        const float* xlaP[2] { xla[0], xla[1] };
        laRing.write (xlaP, m);

        const bool digital = cur.mode == EqFilterMode::Digital;
        const float* detIn[2] { xs[0], xs[1] };
        if (digital)
        {
            const int Dt = eqdesign::firLatency (cur.firIdx, sr);
            float* dirP[2] { direct[0], direct[1] };
            laRing.read (dirP, m, Dt);
        }

        soloUpdate();
        runUnits (detIn, m);
        const int numSets = cur.dual ? 2 : 1;
        for (int s = 0; s < numSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
                computeGains (s, b, m);
        delayControl (m);

        if (digital)
            processDigital (m);
        else
            processAnalog (m);
        digSnap = false;

        const float* aligned[2] { digital ? direct[0] : xla[0], digital ? direct[1] : xla[1] };
        processAudition (aligned, m);
        processSolo (m);

        if (fadeDir != 0 || fadeGain < 1.0)
            for (int i = 0; i < m; ++i)
            {
                if (fadeDir != 0)
                {
                    fadeGain = std::clamp (fadeGain + fadeStep * (double) fadeDir, 0.0, 1.0);
                    if (fadeDir > 0 && fadeGain >= 1.0)
                        fadeDir = 0;
                }
                const float gf = (float) fadeGain;
                out[0][i] *= gf;
                out[1][i] *= gf;
            }

        for (int c = 0; c < 2; ++c)
            std::copy (out[c], out[c] + m, p[c]);
        tickLeft -= m;
    }

    void processBlock (float* const* p, int n) noexcept
    {
        float* ptr[2] { p[0], p[1] };
        while (n > 0)
        {
            if (target != cur && fadeDir >= 0)
            {
                fadeDir = -1;
                fadeRemaining = (int) std::ceil (fadeGain * cfgFadeLen);
            }
            if (fadeDir < 0 && fadeRemaining <= 0)
            {
                applyConfig();
                fadeGain = 0.0;
                fadeDir = 1;
            }
            int len = std::min (n, tickLeft > 0 ? tickLeft : tickLen);
            if (fadeDir < 0)
                len = std::min (len, fadeRemaining);
            processSegment (ptr, len);
            if (fadeDir < 0)
            {
                fadeRemaining -= len;
                if (fadeRemaining <= 0)
                    fadeGain = 0.0;
            }
            ptr[0] += len;
            ptr[1] += len;
            n -= len;
        }
    }

    void publishMeters() noexcept
    {
        const int numSets = cur.dual ? 2 : 1;
        for (int s = 0; s < numSets; ++s)
            for (int b = 0; b < kDynEqBands; ++b)
            {
                auto& r = rt[s][b];
                const auto& bp = params.bands[s][b];
                const bool dyn = bp.on && bp.dynamic;
                for (int t = s; t < (cur.dual ? s + 1 : kNumChannelSets); ++t)
                {
                    meters.bandGainDb[t][b].store (bp.on ? r.meterGain : 0.0f);
                    meters.dynamicDb[t][b].store (bp.on ? r.meterDynamic : 0.0f);
                    meters.detectorDb[t][b].store (dyn ? (float) r.detMax : (float) kMinLevelDb);
                }
                r.detMax = kMinLevelDb;
            }
    }

    //==========================================================================
    void prepareSize (int idx)   // allocates (prepare or worker thread, never while the audio thread uses it)
    {
        auto& f = fir[idx];
        const int n = eqdesign::firLength (idx, sr);
        const float zeroTap = 0.0f;
        for (int b = 0; b < kDynEqBands; ++b)
        {
            f.conv[b]->prepare (n, eqdesign::firPartition (n), 2);
            f.conv[b]->setKernel (0, &zeroTap, 1);
            f.conv[b]->reset();   // installs the zero kernel of channel 0
            f.conv[b]->setKernel (1, &zeroTap, 1);
            f.conv[b]->reset();
            for (int c = 0; c < 2; ++c)
            {
                installed[idx][b][c] = KernelKey {};
                f.doneSeq[b][c].store (0, std::memory_order_relaxed);
            }
        }
        f.prepared.store (true, std::memory_order_release);
    }

    void stopWorker()
    {
        quit.store (true);
        if (worker.joinable())
            worker.join();
        quit.store (false);
    }

    void workerLoop()
    {
        DesignRequest req;
        bool have = false;
        while (! quit.load (std::memory_order_acquire))
        {
            bool worked = false, waiting = false;
            if (mailbox.consume (req))
                have = true;

            if (have && req.digital)
            {
                const int idx = std::clamp (req.firIdx, 0, kNumFirSizes - 1);
                auto& f = fir[idx];
                if (! f.prepared.load (std::memory_order_acquire))
                {
                    prepareSize (idx);
                    worked = true;
                }
                const int n = eqdesign::firLength (idx, sr);
                KernelKey designedKey[2];
                bool designedValid[2] { false, false };
                for (int b = 0; b < kDynEqBands && ! quit.load (std::memory_order_relaxed); ++b)
                    for (int c = 0; c < 2; ++c)
                    {
                        if (f.doneSeq[b][c].load (std::memory_order_relaxed) == req.seq)
                            continue;
                        const KernelKey key = req.keys[req.dual ? c : 0][b];
                        // Off / zero-gain bands do not need a new kernel. Retain the old one for their
                        // coefficient / enable fade, and do not strand a zero publication in a stopped convolver.
                        if (! key.on || key == installed[idx][b][c])
                        {
                            f.doneSeq[b][c].store (req.seq, std::memory_order_release);
                            continue;
                        }
                        // Only the audio thread frees this slot. In particular, stopped audio must not make
                        // the worker repeatedly redesign the same FIR while setKernel() cannot accept it.
                        if (f.conv[b]->isKernelPending (c))
                        {
                            waiting = true;
                            continue;
                        }
                        // design (or reuse channel 0's design of the same key)
                        int src = -1;
                        if (c == 1 && designedValid[0] && designedKey[0] == key)
                            src = 0;
                        else
                        {
                            designKernel (key, idx, taps[c].data());
                            designedKey[c] = key;
                            designedValid[c] = true;
                            src = c;
                        }
                        const bool ok = f.conv[b]->setKernel (c, taps[src].data(), n);
                        if (ok)
                        {
                            installed[idx][b][c] = key;
                            f.doneSeq[b][c].store (req.seq, std::memory_order_release);
                            worked = true;
                        }
                        else
                            waiting = true;   // the previous kernel of this channel is still being installed
                    }

                // doneSeq records publication, not installation. Publish completion separately so UI reads
                // touch only atomics and remain pending until every currently relevant kernel was consumed.
                bool complete = true;
                for (int b = 0; b < kDynEqBands; ++b)
                    for (int c = 0; c < 2; ++c)
                        if (f.doneSeq[b][c].load (std::memory_order_acquire) != req.seq
                            || (req.keys[req.dual ? c : 0][b].on && f.conv[b]->isKernelPending (c)))
                            complete = false;
                if (complete)
                    completedSeq.store (req.seq, std::memory_order_release);
                else
                    waiting = true;
            }

            if (! worked)
                std::this_thread::sleep_for (std::chrono::milliseconds (waiting ? 1 : 5));
        }
    }

    // The band's unit kernel W = (|H_ref| - 1) / (10^(g_ref / 20) - 1), linear phase, centred at N / 2.
    void designKernel (const KernelKey& key, int idx, float* dst) noexcept
    {
        const int n = eqdesign::firLength (idx, sr);
        if (! key.on)
        {
            std::fill (dst, dst + n, 0.0f);
            return;
        }
        EqBandParams bands[kEqBands];
        bands[0].on = true;
        bands[0].shape = dynEqToEqShape (key.shape);
        bands[0].freqHz = key.freq;
        bands[0].gainDb = (float) key.gref;
        bands[0].q = key.q;
        bands[0].slope = EqSlope::Db12;
        bands[0].phasePercent = 0.0f;
        if (! designer.design (bands, 100.0f, idx, dst))
            return;   // flat: zeroed
        const float scale = (float) (1.0 / std::expm1 (key.gref * kDbToLn));
        for (int t = 0; t < n; ++t)
            dst[t] *= scale;
    }
};

//==============================================================================
DynamicEq::DynamicEq() : impl (std::make_unique<Impl>()) {}

DynamicEq::~DynamicEq() = default;

void DynamicEq::prepare (double sampleRate, int)
{
    auto& m = *impl;
    m.stopWorker();

    m.sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    m.tickLen = m.sr > 100000.0 ? 128 : (m.sr > 50000.0 ? 64 : 32);
    m.tickAlpha = 1.0 - std::exp (-(double) m.tickLen / (kSmoothTau * m.sr));
    m.bandFadeLen = std::max (16, (int) std::lround (kBandFadeSeconds * m.sr));
    m.bandFade.assign ((size_t) m.bandFadeLen, 0.0);
    for (int i = 0; i < m.bandFadeLen; ++i)
        m.bandFade[(size_t) i] = 0.5 - 0.5 * std::cos (kPi * ((double) i + 0.5) / (double) m.bandFadeLen);
    m.clickLen = m.bandFadeLen;
    m.cfgFadeLen = std::max (16, (int) std::lround (kConfigFadeSeconds * m.sr));
    m.fadeStep = 1.0 / (double) m.cfgFadeLen;
    m.audStep = 1.0 / std::max (1.0, kMonitorFadeSeconds * m.sr);
    m.laMax = (int) std::lround (kMaxLookaheadMs * m.sr / 1000.0);
    m.laFadeLen = std::max (16, (int) std::lround (kLookaheadFadeSeconds * m.sr));
    m.laFade.assign ((size_t) m.laFadeLen, 0.0f);
    for (int i = 0; i < m.laFadeLen; ++i)
        m.laFade[(size_t) i] = (float) (0.5 - 0.5 * std::cos (kPi * ((double) i + 1.0) / (double) m.laFadeLen));

    const int maxDt = eqdesign::firLatency (kNumFirSizes - 1, m.sr);
    m.inRing.prepare (maxDt + m.laMax);
    m.laRing.prepare (maxDt);
    m.soloRing.prepare (maxDt + m.laMax);
    int gainSize = 16;
    while (gainSize < maxDt + kMaxTick + 1)
        gainSize <<= 1;
    m.gainMask = gainSize - 1;
    for (auto& set : m.gainHistory)
        for (auto& band : set)
            band.assign ((size_t) gainSize, GainSample {});

    const int maxN = eqdesign::firLength (kNumFirSizes - 1, m.sr);
    m.designer.prepare (m.sr, maxN);
    for (auto& t : m.taps)
        t.assign ((size_t) maxN, 0.0f);

    for (auto& f : m.fir)
        f.prepared.store (false);
    m.prepareSize (std::clamp (m.params.firSizeIndex, 0, kNumFirSizes - 1));

    m.mailbox.clear();
    m.lastReq = {};
    m.publishedSeq.store (0);
    m.completedSeq.store (0);
    m.prepared = true;

    reset();
    m.updateTargets();
    m.worker = std::thread ([&m] { m.workerLoop(); });
}

void DynamicEq::reset() noexcept
{
    auto& m = *impl;
    if (! m.prepared)
        return;
    m.inRing.clear();
    m.laRing.clear();
    m.soloRing.clear();
    m.resetDetection();
    m.gainPos = 0;
    m.gainCount = 0;
    for (auto& ch : m.an)
        for (auto& ab : ch)
            ab.clear();
    for (auto& d : m.dig)
        d = DigBand {};
    for (auto& f : m.fir)
        if (f.prepared.load (std::memory_order_acquire))
            for (auto& c : f.conv)
                c->reset();
    m.digSnap = true;
    m.analogSnapPending = true;
    m.audMix = 0.0;
    m.audIc1[0] = m.audIc1[1] = m.audIc2[0] = m.audIc2[1] = 0.0;
    m.soloBand = -1;
    m.soloMix = 0.0;
    m.soloFill = 0;
    m.fadeGain = 1.0;
    m.fadeDir = 0;
    m.fadeRemaining = 0;
    m.tickLeft = 0;
    m.needSnap = true;
}

void DynamicEq::setParams (ParamValues v) noexcept
{
    auto& m = *impl;
    readDynamicEqParams (v, m.params);
    if (getParamSet() == 1)   // T/S sustain twin: set B for both channels (Module.h)
        for (int b = 0; b < kDynEqBands; ++b)
            m.params.bands[0][b] = m.params.bands[1][b];
    m.sanitiseParams();
    if (m.prepared)
        m.updateTargets();
}

void DynamicEq::setMonitor (const ModuleMonitor& mon) noexcept { impl->monitor = mon; }

void DynamicEq::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    auto& m = *impl;
    if (! m.prepared || numSamples <= 0)
        return;
    DenormalGuard guard;

    const bool dual = ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight;
    if (dual != m.target.dual)
    {
        m.target.dual = dual;
        m.publishRequest();
    }
    if (m.needSnap)
        m.applyConfig();

    m.processBlock (channels, numSamples);
    m.publishMeters();
}

int DynamicEq::getLatencySamples() const noexcept { return impl->latency.load (std::memory_order_relaxed); }

int DynamicEq::getLatencySamplesForParams (ParamValues values) const noexcept
{
    DynamicEqParams params;
    readDynamicEqParams (values, params);
    auto config = impl->target;
    config.mode = params.filterMode;
    config.firIdx = std::clamp (params.firSizeIndex, 0, kNumFirSizes - 1);
    return impl->prepared ? impl->latencyOf (config, impl->lookaheadSamples (params.lookaheadMs)) : getLatencySamples();
}

const DynamicEqMeters& DynamicEq::getMeters() const noexcept { return impl->meters; }

bool DynamicEq::isDesignPending() const noexcept
{
    auto& m = *impl;
    if (m.targetMode.load (std::memory_order_relaxed) != (int) EqFilterMode::Digital)
        return false;
    const auto& f = m.fir[std::clamp (m.targetFirIdx.load (std::memory_order_relaxed), 0, kNumFirSizes - 1)];
    if (! f.prepared.load (std::memory_order_acquire))
        return true;
    const auto want = m.publishedSeq.load (std::memory_order_acquire);
    return m.completedSeq.load (std::memory_order_acquire) < want;
}
} // namespace ps
