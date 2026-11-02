#include "dsp/Equalizer.h"

#include "dsp/Convolver.h"
#include "dsp/EqDesign.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
 #include <xmmintrin.h>
 #define PS_EQ_HAS_MXCSR 1
#endif

// Equalizer 1 / Equalizer 2 (dsp-eq). Both EQ instances use this code.
//
// Shapes (Analog mode = what the biquads realise; "Q" is the band's Q parameter, gains are after Amount):
//   Bell            matched peak (constant Q: the Q holds for boosts and cuts)
//   Proportional Q  matched peak whose Q follows the gain: Q * 2^((|gain| - 6) / 9), i.e. x0.63 near 0 dB, x1 at
//                   6 dB, x2 at 15 dB (wide when gentle, narrow when strong, like classic console EQs)
//   Band Shelf      flat-topped band: 2nd-order high shelf (Q 0.707) up at f * 2^(-bw/2), another one back down at
//                   f * 2^(bw/2), bw = 2 asinh (1 / (2 Q)) / ln 2 octaves (the bandwidth of a bell of this Q). Very
//                   narrow bands do not reach the full gain (the two shelves overlap).
//   Shelf Analog    2nd-order matched shelf that reaches the set gain AT f and twice the gain in its plateau, slope
//                   S = 0.0835 + 0.0764 Q (Q as the reference stores it, 0.2 .. 12, which its UI shows as 1 .. 24:
//                   UI = 1.95 Q + 0.61) -> S 0.1 (very gentle) .. 1 (steepest without overshoot). Measured: low and
//                   high shelves at -6 / -1.8 / +6 dB over the whole Q range fit within 0.04 dB rms (round 17).
//   Shelf Baxandall 1st-order matched shelves measured from the reference (design::baxandallLow / High in
//                   Filters.cpp): the high shelf reaches the set gain far above f (lower corner ~0.8 f); the low shelf
//                   starts near f and keeps falling / rising far below it (depth ~2x the gain); Q not used
//   Shelf Vintage   passive-program-EQ style: 2nd-order shelf (Q 0.6) plus a resonant counter-band 1.6 octave beyond
//                   the corner (at 3 f for the low shelf, f / 3 for the high shelf) with -0.35 x the gain and
//                   Q 0.9 * sqrt (Q / 0.707) (0.3..3): a boost gets the classic dip above the corner, a cut a bump
//   Shelf Resonant  2nd-order shelf with shelf Q = 2 Q clamped to 0.707..4: a resonant bump on the shelf side of the
//                   corner and a dip on the other side (default Q 0.707 -> shelf Q 1.41)
//   HP / LP Flat    Butterworth cascade of the chosen slope (6..48 dB/oct = order 1..8): matched 1st / 2nd order
//                   sections, -3 dB at f
//   HP / LP Resonant same cascade, the highest-Q section's Q is scaled by Q / 0.707 (0.707 = Butterworth); at 6 dB/oct
//                   a 1st-order section cannot resonate, so a peak of 20 log10 (Q / 0.707) dB is added at f
//   HP / LP Brickwall 8th-order elliptic (0.1 dB ripple, 96 dB stopband from 0.74 octave beyond f), see EqDesign.cpp
//   Surgical        Analog: Bell. Digital: minimum-phase flat-topped band with very steep skirts, the LP -> BP
//                   transform of a 4th-order Butterworth-type shelf: |H|^2 = (G^2 + y^8) / (1 + y^8),
//                   y = Q (f / f0 - f0 / f), i.e. the full gain across the top and unity beyond the bandwidth f0 / Q
//   Digital mode only, Brickwall = 128th-order Butterworth magnitude: -0.1 dB at f, -3 dB 1.5 % beyond, -65 dB
//                   0.1 octave beyond (the FIR resolution, ~5.7 fs / N, is the real limit at low frequencies).
// Not in the shape list (Types.h): Notch, Band Pass and Tilt (a Bell at -30 dB with a high Q notches).
//
// Analog mode realisation: every biquad runs as a topology-preserving state-variable filter (EqDesign.h toSvf) in
// double precision. Parameter changes: freq (log2), gain (dB, after Amount) and Q (log2) of every band are smoothed
// at a control rate (one tick every ~1.33 ms: 64 samples at 44.1 / 48 kHz, 128 at 88.2 / 96, 256 above) by two
// cascaded one-poles (tau 5 ms each, no kinks in the trajectory) and the band is redesigned from the smoothed
// values; between two designs the SVF parameters (g, k, mix) are interpolated per sample. Consecutive designs are close, and (g, k) of the SVF stay valid (> 0) on
// the whole interpolation path, so even every-block automation between extreme values stays stable and click-free.
// At most 12 sections are designed per tick (round robin; the rest follow a tick later) so automating all 16 bands
// at once stays cheap. Changes of the structure (on/off, shape, slope) crossfade the band from the old to the new
// filter over 15 ms. Off bands are skipped; enabled 0 dB bands retain their filter history with an exact unity output,
// so a flat EQ is a bit-exact null and starting gain automation at exactly zero does not change its behaviour.
//
// Digital mode: see Equalizer.h. FIR length N = 4096 / 8192 / 16384 / 32768 taps at 44.1/48 kHz (x2 at 88.2/96, x4
// above), Kaiser window (beta 9) centred at D = N / 2, convolver partition P = N / 16, latency D + P. The convolver
// runs only the residual h - delta(D): the delta part is an exact delay line, so a flat EQ is a pure, bit-exact delay
// and the FIR's float rounding only touches what the EQ changes. Low-frequency resolution: the window's main lobe
// is ~5.7 fs / N wide (70 / 35 / 17 / 9 Hz at 48 kHz for the four sizes); narrower features are smeared. Measured
// (EqTests "eq. digital: low-frequency resolution"): a bell follows its target within 0.1 dB down to a centre of
// about 17 * Q * fs / N, i.e. Q 1: 198 / 99 / 50 / 25 Hz, Q 4: 794 / 397 / 198 / 99 Hz at 48 kHz for 4096 / 8192 /
// 16384 / 32768 taps (the same frequencies at 96 / 192 kHz, where the sizes double / quadruple).
// The worker (one std::thread per EQ, started in prepare, joined in the destructor and before a re-prepare) polls a
// lock-free triple buffer every 5 ms, designs the kernels of the requested FIR size (set A, and set B in M/S and L/R)
// and installs them with Convolver::setKernel into an inaudible bank. Both banks keep complete input histories.
// Once both channel kernels are installed and their partition fades have played, the audio thread crossfades the
// stereo pair together over 15 ms, then releases the old bank to the worker. A size switch gates the residual until
// the requested generation is installed, so it cannot expose a kernel left over from a previous use of that size.
//
// Mode / FIR size / soft saturation / channel-mode changes: the output fades out (4 ms), the new configuration is
// applied (filter states cleared, the new convolver reset) and the output fades in again. The delay line of the
// digital path is fed in every mode, so the digital output is available at once; the FIR residual (the EQ itself)
// joins once the new convolver has a complete input history (N + P samples, then a 10 ms fade), so a switch to
// Digital never plays the transient of a convolution that starts at the switch.
//
// Soft saturation (our choice where the research is silent): computed at 2x (linear-phase oversampler, its latency
// of 69 base-rate samples is added to the module latency while it is on). A sample is only shaped when the EQ output
// |y| exceeds both -6 dBFS and the dry level |x| (added energy), and that channel set contains a boost. Cut-only
// settings bypass the correction: phase rotation alone can make |y| > |x| even when the filter removes energy.
// Boost eligibility fades over 10 ms as bands change. The part above max (0.5, |x|) is soft-clipped with a
// tanh toward 0 dBFS. The base-rate output is y (delayed) + the band-limited correction, so below -6 dBFS (and on
// cuts) the output is exactly the EQ output, delayed.
//
// Audition: input (latency aligned) through a unity-peak band pass at auditionHz / auditionQ; Stereo: both channels,
// M/S and L/R: the chosen channel set, the other channel is muted. 10 ms fades in and out.
namespace ps
{
namespace
{
    using cd = std::complex<double>;
    using eqdesign::BandDesign;
    using eqdesign::Svf;

    constexpr double kPi = 3.14159265358979323846;
    constexpr int kMaxSections = eqdesign::kMaxSections;
    constexpr int kNumFirSizes = 4;
    constexpr int kDesignBudget = 12;      // sections designed per control tick
    constexpr double kSmoothTau = 0.005;   // s, per stage (two cascaded one-poles)
    constexpr double kBandFadeSeconds = 0.015;
    constexpr double kConfigFadeSeconds = 0.004;
    constexpr double kAuditionFadeSeconds = 0.010;

    struct DenormalGuard
    {
#if PS_EQ_HAS_MXCSR
        unsigned int saved;
        DenormalGuard() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
        ~DenormalGuard() { _mm_setcsr (saved); }
#endif
    };

    //==========================================================================
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
                return v0;   // enabled 0 dB bands retain their history without changing a single output bit
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
        SvfSection sec[kMaxSections];
        int rampLeft = 0;
        bool identity = true;

        void updateIdentity() noexcept
        {
            identity = rampLeft == 0;
            for (int i = 0; i < n && identity; ++i)
                identity = sec[i].c.isIdentity();
        }

        void clear() noexcept
        {
            n = 0;
            rampLeft = 0;
            identity = true;
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
            for (int i = 0; i < n; ++i)
            {
                auto& s = sec[i];
                s.target = eqdesign::toSvf (d.s[i]);
                if (snap)
                {
                    s.c = s.target;
                    s.derive();
                    s.step = {};
                    s.step.g = s.step.k = s.step.m0 = 0.0;
                }
                else
                {
                    const double inv = 1.0 / (double) rampLen;
                    s.step.g = (s.target.g - s.c.g) * inv;
                    s.step.k = (s.target.k - s.c.k) * inv;
                    s.step.m0 = (s.target.m0 - s.c.m0) * inv;
                    s.step.m1 = (s.target.m1 - s.c.m1) * inv;
                    s.step.m2 = (s.target.m2 - s.c.m2) * inv;
                    moving = moving || s.step.g != 0.0 || s.step.k != 0.0 || s.step.m0 != 0.0 || s.step.m1 != 0.0
                             || s.step.m2 != 0.0;
                }
            }
            rampLeft = (! snap && moving) ? rampLen : 0;
            if (rampLeft == 0)
                for (int i = 0; i < n; ++i)
                {
                    sec[i].c = sec[i].target;
                    sec[i].derive();
                }
            updateIdentity();
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
                {
                    for (int s = 0; s < n; ++s)
                    {
                        sec[s].c = sec[s].target;
                        sec[s].derive();
                    }
                    updateIdentity();
                }
                i0 = r;
            }

            if (i0 >= m)
                return;
            for (int s = 0; s < n; ++s)
            {
                auto& sc = sec[s];
                for (int i = i0; i < m; ++i)
                    buf[i] = sc.tick (buf[i]);
                if (std::abs (sc.ic1) < 1.0e-30) sc.ic1 = 0.0;
                if (std::abs (sc.ic2) < 1.0e-30) sc.ic2 = 0.0;
            }
        }
    };

    struct ChannelBand
    {
        Slot slot[2];
        int active = 0;
        int fadePos = 0;
        bool fading = false;

        bool isActive() const noexcept { return fading || slot[active].n != 0; }
    };

    // Per channel set: targets, smoothed values and the realised structure of one band.
    struct BandCtl
    {
        bool on = false;
        EqShape shape = EqShape::Bell;   // analog-mapped
        EqSlope slope = EqSlope::Db12;
        double tLf = 10.0, tG = 0.0, tLq = -0.5;
        double lf = 10.0, g = 0.0, lq = -0.5;         // smoothed (second stage)
        double lf1 = 10.0, g1 = 0.0, lq1 = -0.5;      // first stage
        bool curOn = false;
        EqShape curShape = EqShape::Bell;
        EqSlope curSlope = EqSlope::Db12;
        bool needsDesign = false;

        bool structureChanged() const noexcept
        {
            if (on != curOn)
                return true;
            if (! on)
                return false;
            return shape != curShape || (eqdesign::usesSlope (shape) && slope != curSlope);
        }
    };

    bool usesGain (EqShape s) noexcept { return ! eqdesign::isPassShape (s); }

    bool usesQ (EqShape s) noexcept
    {
        switch (s)
        {
            case EqShape::LowShelfBaxandall: case EqShape::HighShelfBaxandall:
            case EqShape::HighPassFlat: case EqShape::LowPassFlat:
            case EqShape::HighPassBrickwall: case EqShape::LowPassBrickwall:
                return false;
            default:
                return true;
        }
    }

    // Two cascaded one-poles (critically damped): the trajectory has no kinks, so even random every-block automation
    // modulates the audio only with a spectrum that falls 12 dB/oct above ~30 Hz.
    bool smoothTowards (double& s1, double& s2, double target, double alpha, double eps) noexcept
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

    void snapSmoothed (BandCtl& c) noexcept
    {
        c.lf = c.lf1 = c.tLf;
        c.g = c.g1 = c.tG;
        c.lq = c.lq1 = c.tLq;
    }

    //==========================================================================
    struct DelayRing
    {
        std::vector<float> buf[2];
        int mask = 0, pos = 0;

        void prepare (int maxDelay, int maxBlock)
        {
            int size = 16;
            while (size < maxDelay + maxBlock + 1)
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

        // writes in[c][0..n) and reads the signal delayed by `delay` into out[c] (out may alias in)
        void process (const float* const* in, float* const* out, int n, int delay) noexcept
        {
            for (int c = 0; c < 2; ++c)
            {
                float* b = buf[c].data();
                for (int i = 0; i < n; ++i)
                {
                    const int w = (pos + i) & mask;
                    b[w] = in[c][i];
                    out[c][i] = b[(w - delay) & mask];
                }
            }
            pos = (pos + n) & mask;
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

        // after write(): the last n samples delayed by `delay`
        void read (float* const* out, int n, int delay) noexcept
        {
            for (int c = 0; c < 2; ++c)
            {
                const float* b = buf[c].data();
                for (int i = 0; i < n; ++i)
                    out[c][i] = b[(pos - n + i - delay) & mask];
            }
        }
    };

    //==========================================================================
    struct DesignRequest
    {
        EqBandParams bands[kNumChannelSets][kEqBands];
        float amount = 100.0f;
        int firIdx = 1;
        bool digital = false;
        bool dual = false;
        std::uint64_t seq = 0;
    };

    bool sameBand (const EqBandParams& a, const EqBandParams& b) noexcept
    {
        if (a.on != b.on)
            return false;
        if (! a.on)
            return true;
        return a.shape == b.shape && a.freqHz == b.freqHz && a.gainDb == b.gainDb && a.q == b.q && a.slope == b.slope
            && a.phasePercent == b.phasePercent;
    }

    bool sameRequest (const DesignRequest& a, const DesignRequest& b) noexcept
    {
        if (a.digital != b.digital)
            return false;
        if (! a.digital)
            return true;
        if (a.amount != b.amount || a.firIdx != b.firIdx || a.dual != b.dual)
            return false;
        for (int s = 0; s < (a.dual ? 2 : 1); ++s)
            for (int b2 = 0; b2 < kEqBands; ++b2)
                if (! sameBand (a.bands[s][b2], b.bands[s][b2]))
                    return false;
        return true;
    }

    // Single-producer / single-consumer triple buffer: the producer never waits, the consumer gets the latest value.
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
        int back = 0, front = 2;          // owned by producer / consumer
        std::atomic<int> middle { 1 };
    };

    inline double saturateBoost (double x, double y) noexcept
    {
        const double a = std::abs (y);
        const double floorLevel = std::max (0.5, std::abs (x));
        if (a <= floorLevel)
            return y;
        const double h = std::max (1.0 - floorLevel, 0.1);
        const double m = floorLevel + h * std::tanh ((a - floorLevel) / h);
        return y < 0.0 ? -m : m;
    }

    struct Config
    {
        EqFilterMode mode = EqFilterMode::Analog;
        int firIdx = 1;
        bool softSat = false;
        bool dual = false;

        bool operator== (const Config& o) const noexcept
        {
            return mode == o.mode && (mode != EqFilterMode::Digital || firIdx == o.firIdx)
                && softSat == o.softSat && dual == o.dual;
        }
        bool operator!= (const Config& o) const noexcept { return ! (*this == o); }
    };
}

//==============================================================================
// Frequency response for the UI (thread-local design cache: designing costs up to ~20 us per section, the UI asks
// for hundreds of points per band and frame).
std::complex<double> eqBandResponse (const EqBandParams& b, EqFilterMode mode, double freqHz, double sampleRate,
                                     float amountPercent) noexcept
{
    if (! b.on)
        return { 1.0, 0.0 };

    const double g = eqdesign::scaledGainDb (b.gainDb, amountPercent);
    const double p = std::clamp ((double) b.phasePercent * 0.01, 0.0, 1.0);

    if (mode == EqFilterMode::Digital && eqdesign::hasDigitalPrototype (b.shape))
    {
        double m = 1.0, ph = 0.0;
        eqdesign::digitalPrototype (b.shape, b.freqHz, g, b.q, freqHz, m, ph, p > 0.0);
        return std::polar (m, p * ph);
    }

    struct Entry
    {
        bool valid = false;
        EqShape shape = EqShape::Bell;
        EqSlope slope = EqSlope::Db12;
        float freq = 0.0f, q = 0.0f;
        double gain = 0.0, sr = 0.0;
        BandDesign d;
    };
    static constexpr int kEntries = 32;
    thread_local Entry cache[kEntries];
    thread_local int nextEntry = 0;

    const EqShape shape = eqdesign::analogShape (b.shape);
    const EqSlope slope = eqdesign::usesSlope (shape) ? b.slope : EqSlope::Db12;
    const Entry* hit = nullptr;
    for (const auto& e : cache)
        if (e.valid && e.shape == shape && e.slope == slope && e.freq == b.freqHz && e.q == b.q && e.gain == g
            && e.sr == sampleRate)
        {
            hit = &e;
            break;
        }
    if (hit == nullptr)
    {
        auto& e = cache[nextEntry];
        nextEntry = (nextEntry + 1) % kEntries;
        e.valid = true;
        e.shape = shape;
        e.slope = slope;
        e.freq = b.freqHz;
        e.q = b.q;
        e.gain = g;
        e.sr = sampleRate;
        eqdesign::designAnalog (shape, slope, sampleRate, b.freqHz, g, b.q, e.d);
        hit = &e;
    }

    const cd z1 = std::polar (1.0, -2.0 * kPi * freqHz / sampleRate);
    if (mode == EqFilterMode::Analog)
        return hit->d.response (z1);

    if (p <= 0.0)
        return { std::abs (hit->d.response (z1)), 0.0 };
    double m = 1.0, ph = 0.0;
    hit->d.magPhase (z1, m, ph);
    return std::polar (m, p * ph);
}

EqShape defaultShapeForNewNode (const EqBandParams* bands, double freqHz) noexcept
{
    auto used = [bands] (EqShape s)
    {
        if (bands == nullptr)
            return false;
        for (int i = 0; i < kEqBands; ++i)
            if (bands[i].on && bands[i].shape == s)
                return true;
        return false;
    };

    if (freqHz < 100.0)
        return used (EqShape::LowShelfBaxandall) ? EqShape::ProportionalQ : EqShape::LowShelfBaxandall;
    if (freqHz > 8000.0)
        return used (EqShape::HighShelfBaxandall) ? EqShape::ProportionalQ : EqShape::HighShelfBaxandall;
    return EqShape::ProportionalQ;
}

//==============================================================================
struct Equalizer::Impl
{
    ModuleType type = ModuleType::Equalizer1;
    double sr = 48000.0;
    int chunk = 512;
    bool prepared = false;

    // ---- parameters (audio thread)
    EqParams params;
    Config target, cur;
    bool needSnap = true;
    std::atomic<int> latency { 0 };
    std::atomic<int> targetMode { 0 }, targetFirIdx { 1 };
    int osLatency = 0;

    // ---- analog
    int tickLen = 64, tickLeft = 0, roundRobin = 0;
    double alpha = 0.1;
    BandCtl ctl[kNumChannelSets][kEqBands];
    ChannelBand chan[2][kEqBands];
    std::vector<double> work[2], tmp;
    std::vector<double> bandFade;   // raised cosine 0..1
    int bandFadeLen = 720;

    // ---- digital
    // The convolver publishes channels separately. Stage a complete stereo generation in an inaudible bank,
    // then crossfade both channels together; the worker cannot reuse a bank until the audio thread releases it.
    struct FirPair
    {
        Convolver bank[2];
        std::atomic<int> writable { 1 }, ready { -1 };
        std::atomic<std::uint64_t> readySeq { 0 }, audibleSeq { 0 };
        int active = 0, incoming = -1, drain = 0, fadePos = 0;
        std::uint64_t incomingSeq = 0;
    };
    FirPair fir[kNumFirSizes];
    DelayRing dryRing;
    std::vector<float> convBuf[2], convNext[2], dry[2], dryDel[2];
    long long convRun = 0;           // samples the active convolver has run since its reset
    std::uint64_t resRequiredSeq = 0;
    double resGain = 1.0, resStep = 0.0;

    // ---- soft saturation
    Oversampler osX, osY;
    DelayRing osRing, osDryRing;
    std::vector<float> corr[2];
    double satMix[2] {};

    // ---- audition
    ModuleMonitor monitor;
    double audMix = 0.0, audStep = 0.0, audG = 0.0, audK = 0.25;
    double audIc1[2] {}, audIc2[2] {};
    double audSelect[2] { 1.0, 1.0 };

    // ---- config fade
    double fadeGain = 1.0, fadeStep = 0.0;
    int fadeDir = 0, fadeRemaining = 0, cfgFadeLen = 192;
    int fadeHold = 0;   // samples to stay silent after a switch (the soft-saturation delay line is refilling)

    // ---- worker
    RequestMailbox mailbox;
    DesignRequest lastReq;
    std::atomic<std::uint64_t> publishedSeq { 0 };
    std::atomic<bool> quit { false };
    std::thread worker;
    eqdesign::KernelDesigner designer;
    std::vector<float> tapsA, tapsB;

    ~Impl() { stopWorker(); }

    //==========================================================================
    int latencyOf (const Config& c) const noexcept
    {
        int l = c.softSat ? osLatency : 0;
        if (c.mode == EqFilterMode::Digital)
            l += eqdesign::firLatency (c.firIdx, sr);
        return l;
    }

    int setOfChannel (int ch) const noexcept { return cur.dual ? ch : 0; }

    bool hasBoost (int ch) const noexcept
    {
        for (const auto& b : params.bands[setOfChannel (ch)])
        {
            if (! b.on)
                continue;
            if (usesGain (b.shape) && eqdesign::scaledGainDb (b.gainDb, params.amountPercent) > 0.0)
                return true;
            if ((b.shape == EqShape::HighPassResonant || b.shape == EqShape::LowPassResonant) && b.q > 0.7071068f)
                return true;
        }
        return false;
    }

    void updateTargets() noexcept
    {
        target.mode = params.filterMode;
        target.firIdx = std::clamp (params.firSizeIndex, 0, kNumFirSizes - 1);
        target.softSat = params.softSaturation;
        latency.store (latencyOf (target), std::memory_order_relaxed);
        targetMode.store ((int) target.mode, std::memory_order_relaxed);
        targetFirIdx.store (target.firIdx, std::memory_order_relaxed);

        const double fMax = 0.49 * sr;
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kEqBands; ++b)
            {
                const auto& bp = params.bands[s][b];
                auto& c = ctl[s][b];
                c.on = bp.on;
                c.shape = eqdesign::analogShape (bp.shape);
                c.slope = bp.slope;
                c.tLf = std::log2 (std::clamp ((double) bp.freqHz, 10.0, fMax));
                c.tG = usesGain (c.shape) ? eqdesign::scaledGainDb (bp.gainDb, params.amountPercent) : 0.0;
                c.tLq = usesQ (c.shape) ? std::log2 (std::clamp ((double) bp.q, 0.05, 50.0)) : -0.5;
            }
        publishRequest();
    }

    void publishRequest() noexcept
    {
        DesignRequest r;
        r.digital = target.mode == EqFilterMode::Digital;
        r.dual = target.dual;
        r.firIdx = target.firIdx;
        r.amount = params.amountPercent;
        if (r.digital)
            for (int s = 0; s < kNumChannelSets; ++s)
                for (int b = 0; b < kEqBands; ++b)
                    r.bands[s][b] = params.bands[s][b];
        if (sameRequest (r, lastReq))
            return;
        r.seq = lastReq.seq + 1;
        lastReq = r;
        mailbox.publish (r);
        publishedSeq.store (r.seq, std::memory_order_release);
    }

    //==========================================================================
    void design (const BandCtl& c, BandDesign& d) const noexcept
    {
        if (! c.on)
        {
            d.numSections = 0;
            return;
        }
        eqdesign::designAnalog (c.shape, c.slope, sr, std::exp2 (c.lf), c.g, std::exp2 (c.lq), d);
    }

    void snapAnalog() noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
            for (auto& cb : chan[ch])
            {
                cb.slot[0].clear();
                cb.slot[1].clear();
                cb.active = 0;
                cb.fading = false;
                cb.fadePos = 0;
            }

        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kEqBands; ++b)
            {
                auto& c = ctl[s][b];
                snapSmoothed (c);
                c.curOn = c.on;
                c.curShape = c.shape;
                c.curSlope = c.slope;
                c.needsDesign = false;
            }

        for (int ch = 0; ch < 2; ++ch)
        {
            const int s = setOfChannel (ch);
            for (int b = 0; b < kEqBands; ++b)
            {
                BandDesign d;
                design (ctl[s][b], d);
                chan[ch][b].slot[0].setTarget (d, tickLen, true);
            }
        }
        tickLeft = 0;
    }

    void controlTick() noexcept
    {
        int budget = kDesignBudget;
        const int numSets = cur.dual ? 2 : 1;
        const int total = numSets * kEqBands;
        roundRobin = (roundRobin + 1) % total;

        for (int j = 0; j < total; ++j)
        {
            const int idx = (roundRobin + j) % total;
            const int s = idx / kEqBands, b = idx % kEqBands;
            auto& c = ctl[s][b];
            const int ch0 = cur.dual ? s : 0, ch1 = cur.dual ? s : 1;

            if (c.structureChanged())
            {
                if (chan[ch0][b].fading || chan[ch1][b].fading)
                    continue;   // one crossfade at a time
                const int sections = c.on ? eqdesign::sectionCount (c.shape, c.slope) : 0;
                if (sections > budget)
                    continue;
                snapSmoothed (c);
                c.curOn = c.on;
                c.curShape = c.shape;
                c.curSlope = c.slope;
                c.needsDesign = false;
                BandDesign d;
                design (c, d);
                budget -= d.numSections;
                for (int ch = ch0; ch <= ch1; ++ch)
                {
                    auto& cb = chan[ch][b];
                    const int nw = 1 - cb.active;
                    cb.slot[nw].clear();
                    cb.slot[nw].setTarget (d, tickLen, true);
                    cb.active = nw;
                    cb.fading = true;
                    cb.fadePos = 0;
                }
                continue;
            }

            if (! c.curOn)
            {
                snapSmoothed (c);
                c.needsDesign = false;
                continue;
            }

            bool moved = smoothTowards (c.lf1, c.lf, c.tLf, alpha, 1.0e-4);
            moved = smoothTowards (c.g1, c.g, c.tG, alpha, 1.0e-3) || moved;
            moved = smoothTowards (c.lq1, c.lq, c.tLq, alpha, 1.0e-4) || moved;
            if (moved)
                c.needsDesign = true;

            if (c.needsDesign && eqdesign::sectionCount (c.curShape, c.curSlope) <= budget)
            {
                BandDesign d;
                design (c, d);
                budget -= d.numSections;
                c.needsDesign = false;
                for (int ch = ch0; ch <= ch1; ++ch)
                {
                    auto& cb = chan[ch][b];
                    cb.slot[cb.active].setTarget (d, tickLen, false);
                }
            }
        }
    }

    void processAnalogBand (ChannelBand& cb, double* buf, int m) noexcept
    {
        Slot& nw = cb.slot[cb.active];
        if (! cb.fading)
        {
            nw.process (buf, m);
            return;
        }

        Slot& old = cb.slot[1 - cb.active];
        double* t = tmp.data();
        std::copy (buf, buf + m, t);
        nw.process (buf, m);
        old.process (t, m);
        for (int i = 0; i < m; ++i)
        {
            const int pos = cb.fadePos + i;
            const double r = pos < bandFadeLen ? bandFade[(size_t) pos] : 1.0;
            buf[i] = t[i] + (buf[i] - t[i]) * r;
        }
        cb.fadePos += m;
        if (cb.fadePos >= bandFadeLen)
        {
            cb.fading = false;
            old.clear();
        }
    }

    void processAnalog (float* const* p, int n) noexcept
    {
        int done = 0;
        while (done < n)
        {
            if (tickLeft == 0)
            {
                controlTick();
                tickLeft = tickLen;
            }
            const int m = std::min (n - done, tickLeft);
            for (int ch = 0; ch < 2; ++ch)
            {
                bool any = false;
                for (const auto& cb : chan[ch])
                    any = any || cb.isActive();
                if (! any)
                    continue;

                double* w = work[ch].data();
                float* x = p[ch] + done;
                for (int i = 0; i < m; ++i)
                    w[i] = (double) x[i];
                for (auto& cb : chan[ch])
                    if (cb.isActive())
                        processAnalogBand (cb, w, m);
                for (int i = 0; i < m; ++i)
                    x[i] = (float) w[i];
            }
            tickLeft -= m;
            done += m;
        }
    }

    //==========================================================================
    void finishFirSwap (FirPair& f) noexcept
    {
        const int old = f.active;
        f.active = f.incoming;
        f.incoming = -1;
        f.audibleSeq.store (f.incomingSeq, std::memory_order_release);
        f.writable.store (old, std::memory_order_release);
    }

    void acceptFir (int index, bool immediate) noexcept
    {
        auto& f = fir[index];
        if (f.incoming < 0)
        {
            const int ready = f.ready.exchange (-1, std::memory_order_acq_rel);
            if (ready >= 0)
            {
                f.incoming = ready;
                f.incomingSeq = f.readySeq.load (std::memory_order_relaxed);
                // Pending flags clear when a partition is computed, before its output has played. Wait for both
                // publications and that entire partition before exposing the staged generation.
                f.drain = 2 * eqdesign::firPartition (eqdesign::firLength (index, sr));
                f.fadePos = 0;
            }
        }
        if (immediate && f.incoming >= 0)
        {
            f.bank[f.incoming].reset();
            finishFirSwap (f);
        }
    }

    void resetFir (int index) noexcept
    {
        acceptFir (index, true);
        for (auto& bank : fir[index].bank)
            bank.reset();
    }

    void applyConfig() noexcept
    {
        const Config prev = cur;
        cur = target;
        if (cur.mode == EqFilterMode::Analog)
            snapAnalog();
        if (cur.mode == EqFilterMode::Digital
            && (prev.mode != EqFilterMode::Digital || prev.firIdx != cur.firIdx || needSnap))
        {
            // a freshly reset convolver has no input history: its residual would be the EQ of a signal that starts
            // here. Until the history is complete the output is the (exact) delayed input, then the residual fades in.
            // (after prepare / reset there is no history to be wrong about: no gate)
            resetFir (cur.firIdx);
            convRun = 0;
            resGain = needSnap ? 1.0 : 0.0;
            resRequiredSeq = publishedSeq.load (std::memory_order_acquire);
        }
        if (cur.softSat)   // its delay lines hold the previous configuration's output: start silent
        {
            osX.reset();
            osY.reset();
            osRing.clear();
            osDryRing.clear();
            for (int c = 0; c < 2; ++c)
                satMix[c] = hasBoost (c) ? 1.0 : 0.0;
        }
        needSnap = false;
    }

    void processSegment (float* const* p, int n) noexcept
    {
        for (int i = 0; i < kNumFirSizes; ++i)
            acceptFir (i, cur.mode != EqFilterMode::Digital || i != cur.firIdx);
        float* dryP[2] { dry[0].data(), dry[1].data() };
        float* delP[2] { dryDel[0].data(), dryDel[1].data() };
        for (int c = 0; c < 2; ++c)
            std::copy (p[c], p[c] + n, dry[c].data());

        const bool digital = cur.mode == EqFilterMode::Digital;
        const float* const* aligned = dryP;

        if (digital)
        {
            dryRing.process (dryP, delP, n, eqdesign::firLatency (cur.firIdx, sr));
            float* cb[2] { convBuf[0].data(), convBuf[1].data() };
            float* next[2] { convNext[0].data(), convNext[1].data() };
            auto& f = fir[cur.firIdx];
            // A swap completing later in this segment must not open the residual gate retroactively for its
            // earlier samples, which can still contain the previous size's curve.
            const bool currentKernel = f.audibleSeq.load (std::memory_order_acquire) >= resRequiredSeq;
            for (int c = 0; c < 2; ++c)
            {
                std::copy (dry[c].data(), dry[c].data() + n, cb[c]);
                std::copy (dry[c].data(), dry[c].data() + n, next[c]);
            }
            f.bank[f.active].process (cb, n);
            f.bank[1 - f.active].process (next, n); // keep both input histories complete, even during design
            if (f.incoming >= 0)
            {
                for (int i = 0; i < n; ++i)
                {
                    if (f.drain > 0)
                        --f.drain;
                    else
                    {
                        f.fadePos = std::min (f.fadePos + 1, bandFadeLen);
                        const float mix = (float) (0.5 - 0.5 * std::cos (kPi * f.fadePos / bandFadeLen));
                        for (int c = 0; c < 2; ++c)
                            cb[c][i] += (next[c][i] - cb[c][i]) * mix;
                    }
                }
                if (f.fadePos == bandFadeLen)
                    finishFirSwap (f);
            }
            if (resGain >= 1.0)
            {
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < n; ++i)
                        p[c][i] = delP[c][i] + cb[c][i];
            }
            else
            {
                const int fl = eqdesign::firLength (cur.firIdx, sr);
                const long long full = (long long) fl + eqdesign::firPartition (fl);
                for (int i = 0; i < n; ++i)
                {
                    if (currentKernel && convRun + i >= full)
                        resGain = std::min (1.0, resGain + resStep);
                    const float rg = (float) resGain;
                    p[0][i] = delP[0][i] + rg * cb[0][i];
                    p[1][i] = delP[1][i] + rg * cb[1][i];
                }
            }
            convRun += n;
            aligned = delP;
        }
        else
        {
            dryRing.write (dryP, n);
            processAnalog (p, n);
        }

        float* alignedOut[2] { delP[0], delP[1] };
        if (cur.softSat)
        {
            float* const* X = osX.upsample (aligned, n);
            float* const* Y = osY.upsample (p, n);
            const int n2 = n * osY.getFactor();
            for (int c = 0; c < 2; ++c)
            {
                const double targetMix = hasBoost (c) ? 1.0 : 0.0;
                const double step = audStep / osY.getFactor();
                for (int i = 0; i < n2; ++i)
                {
                    satMix[c] += std::clamp (targetMix - satMix[c], -step, step);
                    const double y = Y[c][i];
                    Y[c][i] = (float) (satMix[c] * (saturateBoost (X[c][i], y) - y));
                }
            }
            float* cp[2] { corr[0].data(), corr[1].data() };
            osY.downsample (cp, n);
            osRing.process (p, p, n, osLatency);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    p[c][i] += cp[c][i];
            osDryRing.process (aligned, alignedOut, n, osLatency);   // delP may alias aligned: ring reads first
            aligned = alignedOut;
        }

        processAudition (p, aligned, n);

        if (fadeDir != 0 || fadeGain < 1.0)
        {
            for (int i = 0; i < n; ++i)
            {
                if (fadeHold > 0)
                    --fadeHold;
                else if (fadeDir != 0)
                {
                    fadeGain = std::clamp (fadeGain + fadeStep * (double) fadeDir, 0.0, 1.0);
                    if (fadeDir > 0 && fadeGain >= 1.0)
                        fadeDir = 0;
                }
                const float gf = (float) fadeGain;
                p[0][i] *= gf;
                p[1][i] *= gf;
            }
        }
    }

    void processAudition (float* const* p, const float* const* aligned, int n) noexcept
    {
        const double tgt = monitor.auditionOn ? 1.0 : 0.0;
        const int set = std::clamp (monitor.auditionChannelSet, 0, 1);
        if (audMix == 0.0 && tgt == 0.0)
            return;
        if (audMix == 0.0)
        {
            audIc1[0] = audIc1[1] = audIc2[0] = audIc2[1] = 0.0;
            audG = std::tan (kPi * std::clamp ((double) monitor.auditionHz, 10.0, 0.45 * sr) / sr);
            for (int c = 0; c < 2; ++c)
                audSelect[c] = ! cur.dual || c == set ? 1.0 : 0.0;
        }

        const double gT = std::tan (kPi * std::clamp ((double) monitor.auditionHz, 10.0, 0.45 * sr) / sr);
        const double kT = 1.0 / std::clamp ((double) monitor.auditionQ, 0.1, 24.0);
        const double sm = 1.0 - std::exp (-1.0 / (0.01 * sr));

        for (int i = 0; i < n; ++i)
        {
            audG += (gT - audG) * sm;
            audK += (kT - audK) * sm;
            if (audMix < tgt)
                audMix = std::min (tgt, audMix + audStep);
            else if (audMix > tgt)
                audMix = std::max (tgt, audMix - audStep);

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
                p[c][i] = (float) ((double) p[c][i] + (bp - (double) p[c][i]) * audMix);
            }
        }
        for (int c = 0; c < 2; ++c)
        {
            if (std::abs (audIc1[c]) < 1.0e-30) audIc1[c] = 0.0;
            if (std::abs (audIc2[c]) < 1.0e-30) audIc2[c] = 0.0;
        }
    }

    void processChunk (float* const* p, int n) noexcept
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
                fadeHold = cur.softSat ? osLatency : 0;
            }

            int len = n;
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

    //==========================================================================
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
        std::uint64_t designedSeq = ~(std::uint64_t) 0;
        bool needInstall[2] { false, false };
        bool flat[2] { true, true };
        int writing = -1;
        const float zeroTap = 0.0f;

        while (! quit.load (std::memory_order_acquire))
        {
            bool worked = false;
            // Complete both channels of an acquired bank before consuming another request. Otherwise a fast
            // parameter edit could leave one channel from each generation in the staged bank.
            if (writing < 0 && mailbox.consume (req))
                have = true;

            if (have && req.digital)
            {
                if (designedSeq != req.seq)
                {
                    flat[0] = ! designer.design (req.bands[0], req.amount, req.firIdx, tapsA.data());
                    flat[1] = req.dual ? ! designer.design (req.bands[1], req.amount, req.firIdx, tapsB.data())
                                       : flat[0];
                    designedSeq = req.seq;
                    needInstall[0] = needInstall[1] = true;
                    worked = true;
                }
                auto& f = fir[req.firIdx];
                if (writing < 0 && (needInstall[0] || needInstall[1]))
                    writing = f.writable.exchange (-1, std::memory_order_acq_rel);
                if (writing >= 0)
                {
                    const int n = eqdesign::firLength (req.firIdx, sr);
                    for (int c = 0; c < 2; ++c)
                    {
                        if (! needInstall[c])
                            continue;
                        const float* taps = (c == 1 && req.dual) ? tapsB.data() : tapsA.data();
                        const bool ok = flat[c] ? f.bank[writing].setKernel (c, &zeroTap, 1)
                                                : f.bank[writing].setKernel (c, taps, n);
                        if (ok)
                        {
                            needInstall[c] = false;
                            worked = true;
                        }
                    }
                    if (! needInstall[0] && ! needInstall[1])
                    {
                        f.readySeq.store (req.seq, std::memory_order_relaxed);
                        f.ready.store (writing, std::memory_order_release);
                        writing = -1;
                    }
                }
            }

            if (! worked)
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
    }
};

//==============================================================================
Equalizer::Equalizer (ModuleType eq1or2) : impl (std::make_unique<Impl>())
{
    impl->type = eq1or2;
}

Equalizer::~Equalizer() = default;

ModuleType Equalizer::getType() const noexcept { return impl->type; }

void Equalizer::prepare (double sampleRate, int maxBlockSize)
{
    auto& m = *impl;
    m.stopWorker();

    m.sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    m.chunk = std::clamp (maxBlockSize, 64, 8192);
    m.tickLen = m.sr > 100000.0 ? 256 : (m.sr > 50000.0 ? 128 : 64);
    m.alpha = 1.0 - std::exp (-(double) m.tickLen / (kSmoothTau * m.sr));

    for (auto& w : m.work)
        w.assign ((size_t) m.chunk, 0.0);
    m.tmp.assign ((size_t) m.chunk, 0.0);
    m.bandFadeLen = std::max (16, (int) std::lround (kBandFadeSeconds * m.sr));
    m.bandFade.assign ((size_t) m.bandFadeLen, 0.0);
    for (int i = 0; i < m.bandFadeLen; ++i)
        m.bandFade[(size_t) i] = 0.5 - 0.5 * std::cos (kPi * ((double) i + 0.5) / (double) m.bandFadeLen);
    m.cfgFadeLen = std::max (16, (int) std::lround (kConfigFadeSeconds * m.sr));
    m.fadeStep = 1.0 / (double) m.cfgFadeLen;
    m.audStep = 1.0 / std::max (1.0, kAuditionFadeSeconds * m.sr);
    m.resStep = 1.0 / std::max (1.0, kAuditionFadeSeconds * m.sr);

    const float zeroTap = 0.0f;
    int maxN = 0;
    for (int i = 0; i < kNumFirSizes; ++i)
    {
        const int n = eqdesign::firLength (i, m.sr);
        maxN = std::max (maxN, n);
        auto& f = m.fir[i];
        for (auto& bank : f.bank)
        {
            bank.prepare (n, eqdesign::firPartition (n), 2);
            bank.setKernel (0, &zeroTap, 1);
            bank.setKernel (1, &zeroTap, 1);
            bank.reset();
        }
        f.active = 0;
        f.incoming = -1;
        f.writable.store (1);
        f.ready.store (-1);
        f.readySeq.store (0);
        f.audibleSeq.store (0);
    }
    m.dryRing.prepare (eqdesign::firLatency (kNumFirSizes - 1, m.sr), m.chunk);
    for (int c = 0; c < 2; ++c)
    {
        m.convBuf[c].assign ((size_t) m.chunk, 0.0f);
        m.convNext[c].assign ((size_t) m.chunk, 0.0f);
        m.dry[c].assign ((size_t) m.chunk, 0.0f);
        m.dryDel[c].assign ((size_t) m.chunk, 0.0f);
        m.corr[c].assign ((size_t) m.chunk, 0.0f);
    }

    m.osX.prepare (1, Oversampler::Quality::LinearPhase, m.chunk, 2);
    m.osY.prepare (1, Oversampler::Quality::LinearPhase, m.chunk, 2);
    m.osLatency = m.osY.getLatencySamples();
    m.osRing.prepare (m.osLatency, m.chunk);
    m.osDryRing.prepare (m.osLatency, m.chunk);

    m.designer.prepare (m.sr, maxN);
    m.tapsA.assign ((size_t) maxN, 0.0f);
    m.tapsB.assign ((size_t) maxN, 0.0f);

    m.mailbox.clear();
    m.lastReq = {};
    m.publishedSeq.store (0);
    m.prepared = true;

    reset();
    m.updateTargets();   // latency and request of the current parameters
    m.worker = std::thread ([&m] { m.workerLoop(); });
}

void Equalizer::reset() noexcept
{
    auto& m = *impl;
    if (! m.prepared)
        return;
    for (auto& ch : m.chan)
        for (auto& cb : ch)
        {
            cb.slot[0].clear();
            cb.slot[1].clear();
            cb.active = 0;
            cb.fading = false;
        }
    m.resetFir (m.cur.firIdx);
    if (m.target.firIdx != m.cur.firIdx)
        m.resetFir (m.target.firIdx);
    m.convRun = 0;
    m.resGain = 1.0;   // after a reset there is no signal yet: nothing to gate
    m.dryRing.clear();
    m.osX.reset();
    m.osY.reset();
    m.osRing.clear();
    m.osDryRing.clear();
    m.audMix = 0.0;
    m.audIc1[0] = m.audIc1[1] = m.audIc2[0] = m.audIc2[1] = 0.0;
    m.fadeGain = 1.0;
    m.fadeDir = 0;
    m.fadeRemaining = 0;
    m.fadeHold = 0;
    m.needSnap = true;
}

void Equalizer::setParams (ParamValues v) noexcept
{
    auto& m = *impl;
    readEqParams (v, m.type, m.params);
    if (getParamSet() == 1)   // T/S sustain twin: set B for both channels (Module.h)
        for (int b = 0; b < kEqBands; ++b)
            m.params.bands[0][b] = m.params.bands[1][b];
    m.updateTargets();   // the first parameters after prepare / reset are applied without a fade by process()
}

void Equalizer::setMonitor (const ModuleMonitor& mon) noexcept { impl->monitor = mon; }

void Equalizer::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
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

    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (m.chunk, numSamples - done);
        float* p[2] { channels[0] + done, channels[1] + done };
        m.processChunk (p, n);
        done += n;
    }
}

int Equalizer::getLatencySamples() const noexcept { return impl->latency.load (std::memory_order_relaxed); }

bool Equalizer::isDesignPending() const noexcept
{
    auto& m = *impl;
    if (m.targetMode.load (std::memory_order_relaxed) != (int) EqFilterMode::Digital)
        return false;
    const auto& f = m.fir[std::clamp (m.targetFirIdx.load (std::memory_order_relaxed), 0, kNumFirSizes - 1)];
    return f.audibleSeq.load (std::memory_order_acquire) != m.publishedSeq.load (std::memory_order_acquire);
}
} // namespace ps
