#include "dsp/LowContrast.h"

#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define PS_LOWC_HAS_SSE_CSR 1
#endif

// Low Contrast (dsp-lowc). Contract: LowContrast.h. Time-frequency contrast of the low end in delta form:
//   y = x delayed by N  +  ISTFT ((G - 1) X)
//
// Streaming layout (same as ps::SpectralEngine, but hop N / 8 with periodic Hann analysis and synthesis): one ring of N
// samples per channel is the input history and the dry delay (ring[pos] read before it is overwritten is x[n - N]); a
// second ring accumulates the overlap-added correction (slot = time mod N, cleared when read). Every H = N / 8
// samples the last N inputs are windowed, both channels go through one complex FFT, the per-bin gains are computed
// for the low bins only, and when any correction is non-zero the residual (G - 1) X goes back through one inverse FFT,
// the synthesis window and the 1/3 OLA scale. Latency = N for any block size.
//
// Choices where the contract leaves room (documented here, rule 10 of ARCHITECTURE.md):
// - Bin power scale: P = 16 |X|^2 / (3 N^2) per one-sided bin, so sum_k P = mean square of the signal (a full-scale
//   sine sums to 0.5 = -3 dB RMS). eps = 1e-10 (-100 dB), the auto-gain freeze is 1e-7 (-70 dB region power).
// - Stereo detector power = (|X_L|^2 + |X_R|^2) / 2 (a mono signal reads like its Mid channel in M/S).
// - Detectors run on every bin up to ~480 Hz in every frame (also at neutral), so engaging the contrast or moving the
//   region never meets cold envelopes. Only the residual is skipped (exactly 0) at neutral.
// - Warm-up: after reset(), and when the detector domain changes (Stereo <-> M/S), both envelopes equal the frame
//   power P for the first 8 frames with signal (one window length), so the first hit after a reset is not boosted
//   by a temporal reference that is still at 0. Spatial contrast against the neighbourhood remains. Afterwards S
//   runs freely (silence gaps in a song are real contrast).
// - Steady tones get a static level change at |contrast| > 0: the reference is the +-1/3 octave neighbourhood, so a
//   pure tone stands above it (measured at Punchy +100: +1.8 dB at 45 Hz, +3.0 dB at 90 Hz, +5.5 dB at 200 Hz; no
//   modulation, < 0.002 dB swing). Auto gain compensates the region level.
// - Time resolution: the 85 ms window lets a gain change begin before a hit (kick after silence, Punchy +100: change
//   -39 dB re the kick peak 20 ms before the onset, -71 dB at 40 ms; tests "lowc. pre-echo ...").
// - Auto gain compares the region power before and after the CONTRAST stage only (|Gc|^2 with Gc = 10^(W gDb / 20)):
//   the auto gain cannot cancel itself, and the user's Gain stays a make-up on top.
//   Returning Contrast to zero releases the compensation with the same 50 ms glide as Contrast, including during
//   silence. Once Contrast settles, its old power mismatch is discarded so it cannot reintroduce stale make-up.
// - Mode depth (1.0 / 0.6) glides with the 50 ms contrast smoother, so a mode switch changes no gain step-wise.
// - Region solo glides 30 ms (per frame) between the processed residual and (W - 1) X: dry + ISTFT ((W - 1) X) equals
//   ISTFT (W X) (Hann^2 at hop N / 8 is exactly constant), so the soloed output is the unprocessed region.
// - Meters: regionIn / regionOut = 10 log10 of the 300 ms smoothed region power (sum W P), floor -100 dB.
namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kThirdOctave = 1.2599210498948732;   // 2^(1/3)
constexpr double kEps = 1.0e-10;                      // -100 dB of bin power
constexpr double kAutoFreeze = 1.0e-7;                // -70 dB of region power
constexpr double kActiveFloor = 1.0e-9;               // -90 dB: frame counts as "signal" for the warm-up
constexpr int kWarmFrames = 8;                        // one window length at hop N / 8
constexpr double kMaxRegionHz = 300.0;

#if PS_LOWC_HAS_SSE_CSR
struct ScopedFlushDenormals
{
    ScopedFlushDenormals() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
    ~ScopedFlushDenormals() { _mm_setcsr (saved); }
    ScopedFlushDenormals (const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator= (const ScopedFlushDenormals&) = delete;
    unsigned int saved;
};
#else
struct ScopedFlushDenormals {};
#endif

int orderOf (int n) noexcept
{
    int o = 0;
    while ((1 << o) < n)
        ++o;
    return o;
}

inline double glide (double& v, double target, double a, double snap) noexcept
{
    v += a * (target - v);
    if (std::abs (target - v) < snap)
        v = target;
    return v;
}

inline double flushTiny (double v) noexcept { return std::abs (v) < 1.0e-30 ? 0.0 : v; }

struct ModeCoeffs
{
    double attack = 0.0, release = 0.0, slow = 0.0, depth = 1.0;
};
} // namespace

double lowContrastDisplayHz (int index) noexcept
{
    return 16.0 * std::pow (25.0, (double) index / (double) (LowContrastMeters::kPoints - 1));
}

struct LowContrast::Impl
{
    LowContrastMeters meters;

    // configuration (prepare)
    double sampleRate = 48000.0;
    int fftSize = 4096, hop = 512, numBins = 2049;
    double binHz = 48000.0 / 4096.0;
    int kW = 0;        // bins 0 .. kW - 1 can have W > 0 (region max 300 Hz + 1/3 octave)
    int kDet = 0;      // detectors on bins 1 .. kDet - 1 (covers the +-1/3 octave reference of every bin < kW)
    std::unique_ptr<Fft> fft;
    std::vector<float> window;
    std::vector<float> ring[2], acc[2];
    std::vector<std::complex<float>> z, a, b;
    std::vector<int> avgLo, avgHi;       // per bin: the +-1/3 octave reference range (inclusive)
    std::vector<double> binLog2;         // log2 of the bin frequency (bin 0: unused)
    int pointBin[LowContrastMeters::kPoints] {};
    double pointFrac[LowContrastMeters::kPoints] {};
    double powerScale = 1.0;
    // per-hop coefficients
    double a50 = 0.0, a30 = 0.0, a150 = 0.0, a300 = 0.0, a1s = 0.0, a2s = 0.0;
    ModeCoeffs modes[2];

    // targets (setParams / setMonitor)
    LowContrastParams params;
    double targetLowLog2 = std::log2 (20.0), targetHighLog2 = std::log2 (250.0);
    double soloTarget = 0.0;

    // streaming state
    int pos = 0, hopCount = 0;
    bool linked = true, lastLinked = true;

    // smoothed values
    double lowLog2 = std::log2 (20.0), highLog2 = std::log2 (250.0), solo = 0.0;
    struct SetState
    {
        double c = 0.0, depth = 1.0, gainDb = 0.0;
        double autoDb = 0.0, autoTargetDb = 0.0, pin = 0.0, pout = 0.0;
        double meterIn = 0.0, meterOut = 0.0;
        double diff[LowContrastMeters::kPoints] {};
    } sets[kNumChannelSets];

    struct Slot
    {
        std::vector<double> raw, p, f, s, gain, curveDb;
        int warm = 0;
    } slots[2];

    std::vector<double> weight;

    //==============================================================================
    void configure (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        fftSize = sampleRate <= 50000.0 ? 4096 : (sampleRate <= 100000.0 ? 8192 : 16384);
        hop = fftSize / 8;
        numBins = fftSize / 2 + 1;
        binHz = sampleRate / (double) fftSize;
        kW = std::min (numBins, (int) std::ceil (kMaxRegionHz * kThirdOctave / binHz) + 1);
        kDet = std::min (numBins - 1, (int) std::ceil ((double) kW * kThirdOctave) + 2);

        fft = std::make_unique<Fft> (orderOf (fftSize));
        window.resize ((size_t) fftSize);
        for (int j = 0; j < fftSize; ++j)
            window[(size_t) j] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (double) j / (double) fftSize));
        for (int c = 0; c < 2; ++c)
        {
            ring[c].assign ((size_t) fftSize, 0.0f);
            acc[c].assign ((size_t) fftSize, 0.0f);
        }
        z.assign ((size_t) fftSize, {});
        a.assign ((size_t) numBins, {});
        b.assign ((size_t) numBins, {});

        avgLo.assign ((size_t) kDet, 1);
        avgHi.assign ((size_t) kDet, 1);
        binLog2.assign ((size_t) kDet + 1, 0.0);
        for (int k = 1; k <= kDet; ++k)
            binLog2[(size_t) k] = std::log2 ((double) k * binHz);
        for (int k = 1; k < kDet; ++k)
        {
            const int lo = std::min (k - 1, (int) std::floor ((double) k / kThirdOctave + 1.0e-9));
            const int hi = std::max (k + 1, (int) std::ceil ((double) k * kThirdOctave - 1.0e-9));
            avgLo[(size_t) k] = std::max (1, lo);
            avgHi[(size_t) k] = std::min (kDet - 1, hi);
        }
        for (auto& s : slots)
        {
            for (auto* v : { &s.raw, &s.p, &s.f, &s.s, &s.gain, &s.curveDb })
                v->assign ((size_t) kDet + 1, 0.0);
        }
        weight.assign ((size_t) kDet + 1, 0.0);

        for (int i = 0; i < LowContrastMeters::kPoints; ++i)
        {
            const double f = lowContrastDisplayHz (i);
            int k = std::max (1, (int) std::floor (f / binHz));
            k = std::min (k, kDet - 2);
            pointBin[i] = k;
            const double t = (std::log2 (f) - binLog2[(size_t) k]) / (binLog2[(size_t) k + 1] - binLog2[(size_t) k]);
            pointFrac[i] = std::clamp (t, 0.0, 1.0);
        }

        powerScale = 16.0 / (3.0 * (double) fftSize * (double) fftSize);
        const auto coeff = [this] (double ms) { return 1.0 - std::exp (-(double) hop / (ms * 0.001 * sampleRate)); };
        a50 = coeff (50.0);
        a30 = coeff (30.0);
        a150 = coeff (150.0);
        a300 = coeff (300.0);
        a1s = coeff (1000.0);
        a2s = coeff (2000.0);
        modes[(int) LowContrastMode::Punchy] = { coeff (5.0), coeff (60.0), coeff (400.0), 1.0 };
        modes[(int) LowContrastMode::Smooth] = { coeff (40.0), coeff (200.0), coeff (1500.0), 0.6 };
    }

    void snapSmoothers() noexcept
    {
        lowLog2 = targetLowLog2;
        highLog2 = targetHighLog2;
        solo = soloTarget;
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            auto& st = sets[s];
            const auto& sp = params.sets[s];
            st.c = (double) sp.contrastPercent / 100.0;
            st.depth = modes[(int) sp.mode].depth;
            st.gainDb = (double) sp.gainDb;
        }
    }

    void clearState() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (ring[c].begin(), ring[c].end(), 0.0f);
            std::fill (acc[c].begin(), acc[c].end(), 0.0f);
        }
        pos = 0;
        hopCount = 0;
        for (auto& s : slots)
        {
            for (auto* v : { &s.raw, &s.p, &s.f, &s.s, &s.gain, &s.curveDb })
                std::fill (v->begin(), v->end(), 0.0);
            s.warm = 0;
        }
        for (auto& st : sets)
        {
            st.autoDb = 0.0;
            st.autoTargetDb = 0.0;
            st.pin = st.pout = 0.0;
            st.meterIn = st.meterOut = 0.0;
            std::fill (std::begin (st.diff), std::end (st.diff), 0.0);
        }
        publishRestMeters();
    }

    void publishRestMeters() noexcept
    {
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            for (auto& d : meters.diffDb[s])
                d.store (0.0f);
            meters.regionInDb[s].store (-100.0f);
            meters.regionOutDb[s].store (-100.0f);
            meters.autoGainDb[s].store (0.0f);
        }
    }

    //==============================================================================
    void updateFrameSmoothers() noexcept
    {
        glide (lowLog2, targetLowLog2, a50, 1.0e-7);
        glide (highLog2, targetHighLog2, a50, 1.0e-7);
        glide (solo, soloTarget, a30, 1.0e-6);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            auto& st = sets[s];
            const auto& sp = params.sets[s];
            glide (st.c, (double) sp.contrastPercent / 100.0, a50, 1.0e-6);
            glide (st.depth, modes[(int) sp.mode].depth, a50, 1.0e-6);
            glide (st.gainDb, (double) sp.gainDb, a50, 1.0e-6);
        }

        // region weights from the gliding edges (raised cosine over 1/3 octave outside each edge, in log f)
        const double third = 1.0 / 3.0;
        weight[0] = 0.0;
        for (int k = 1; k <= kDet; ++k)
        {
            double w = 0.0;
            if (k < kW)
            {
                const double lf = binLog2[(size_t) k];
                double t = 0.0;
                if (lf < lowLog2)
                    t = (lowLog2 - lf) / third;
                else if (lf > highLog2)
                    t = (lf - highLog2) / third;
                w = t >= 1.0 ? 0.0 : (t <= 0.0 ? 1.0 : 0.5 * (1.0 + std::cos (kPi * t)));
            }
            weight[(size_t) k] = w;
        }
    }

    // Detector + gains of one slot. src: 0 = left / mid, 1 = right / side, -1 = linked (both channels).
    // Returns the region power before / after (contrast only, and full gain) through the out-params.
    void runSlot (int slotIndex, int src, int set) noexcept
    {
        auto& sl = slots[slotIndex];
        auto& st = sets[set];
        const auto& mc = modes[(int) params.sets[set].mode];

        double total = 0.0;
        for (int k = 0; k <= kDet; ++k)
        {
            double pw;
            if (src < 0)
                pw = 0.5 * ((double) std::norm (a[(size_t) k]) + (double) std::norm (b[(size_t) k]));
            else
                pw = (double) std::norm (src == 0 ? a[(size_t) k] : b[(size_t) k]);
            pw *= powerScale;
            sl.raw[(size_t) k] = pw;
            total += pw;
        }

        const bool active = total > kActiveFloor;
        const bool warming = sl.warm < kWarmFrames;
        if (warming && active)
            ++sl.warm;

        for (int k = 1; k < kDet; ++k)
        {
            const double p = 0.25 * sl.raw[(size_t) k - 1] + 0.5 * sl.raw[(size_t) k] + 0.25 * sl.raw[(size_t) k + 1];
            sl.p[(size_t) k] = p;
            double& f = sl.f[(size_t) k];
            double& s = sl.s[(size_t) k];
            if (warming)
            {
                f = p;
                s = p;
            }
            else
            {
                f = flushTiny (f + (p > f ? mc.attack : mc.release) * (p - f));
                s = flushTiny (s + mc.slow * (p - s));
            }
        }

        const double c = st.c * st.depth;
        const double mDb = st.gainDb + st.autoDb;
        double pin = 0.0, poutContrast = 0.0, poutFull = 0.0;
        for (int k = 0; k < kDet; ++k)
        {
            const double w = k < kW ? weight[(size_t) k] : 0.0;
            if (w <= 0.0)
            {
                sl.gain[(size_t) k] = 1.0;
                sl.curveDb[(size_t) k] = 0.0;
                continue;
            }
            double gDb = 0.0;
            if (c != 0.0)
            {
                double sAvg = 0.0;
                const int lo = avgLo[(size_t) k], hi = avgHi[(size_t) k];
                for (int j = lo; j <= hi; ++j)
                    sAvg += sl.s[(size_t) j];
                sAvg /= (double) (hi - lo + 1);
                const double rel = std::clamp (10.0 * std::log10 ((sl.f[(size_t) k] + kEps) / (sAvg + kEps)), -30.0, 20.0);
                gDb = std::clamp (c * rel, -24.0, 12.0);
            }
            const double totDb = w * (gDb + mDb);
            sl.curveDb[(size_t) k] = totDb;
            const double g = totDb == 0.0 ? 1.0 : std::pow (10.0, totDb / 20.0);
            sl.gain[(size_t) k] = g;

            const double pr = sl.raw[(size_t) k];
            const double gc = gDb == 0.0 ? 1.0 : std::pow (10.0, w * gDb / 20.0);
            pin += w * pr;
            poutContrast += w * gc * gc * pr;
            poutFull += w * g * g * pr;
        }

        // auto gain (region power before / after the contrast stage, 2 s; frozen below -70 dB)
        st.pin = flushTiny (st.pin + a2s * (pin - st.pin));
        st.pout = flushTiny (st.pout + a2s * (poutContrast - st.pout));
        const bool neutralContrast = params.sets[set].contrastPercent == 0.0f;
        if (st.c == 0.0)
            st.pout = st.pin;
        // Silence freezes a learned correction only while contrast is requested. At neutral there is no contrast
        // gain to compensate: following the old two-second power ratio would colour the signal for seconds (or
        // indefinitely below the freeze threshold). Release alongside the contrast control, without a gain step.
        if (params.autoGain && ! neutralContrast)
        {
            if (st.pin > kAutoFreeze && st.pout > 0.0)
                st.autoTargetDb = std::clamp (10.0 * std::log10 (st.pin / st.pout), -12.0, 12.0);
        }
        else
        {
            st.autoTargetDb = 0.0;
        }
        glide (st.autoDb, st.autoTargetDb, neutralContrast ? a50 : a1s, 1.0e-6);

        // meters
        st.meterIn = flushTiny (st.meterIn + a300 * (pin - st.meterIn));
        st.meterOut = flushTiny (st.meterOut + a300 * (poutFull - st.meterOut));
        for (int i = 0; i < LowContrastMeters::kPoints; ++i)
        {
            const int k = pointBin[i];
            const double t = pointFrac[i];
            const double v = (1.0 - t) * sl.curveDb[(size_t) k] + t * sl.curveDb[(size_t) k + 1];
            st.diff[i] = flushTiny (st.diff[i] + a150 * (v - st.diff[i]));
        }
    }

    void publishMeters (int set, int toSet) noexcept
    {
        const auto& st = sets[set];
        for (int i = 0; i < LowContrastMeters::kPoints; ++i)
            meters.diffDb[toSet][i].store ((float) st.diff[i]);
        meters.regionInDb[toSet].store ((float) (10.0 * std::log10 (std::max (st.meterIn, kEps))));
        meters.regionOutDb[toSet].store ((float) (10.0 * std::log10 (std::max (st.meterOut, kEps))));
        meters.autoGainDb[toSet].store ((float) st.autoDb);
    }

    void frame() noexcept
    {
        const int n = fftSize;
        for (int j = 0; j < n; ++j)
        {
            const int r = (pos + j) & (n - 1);
            const float w = window[(size_t) j];
            z[(size_t) j] = { ring[0][(size_t) r] * w, ring[1][(size_t) r] * w };
        }
        fft->forward (z.data());
        fft->splitPacked (z.data(), a.data(), b.data());

        updateFrameSmoothers();

        if (linked != lastLinked)
        {
            slots[0].warm = slots[1].warm = 0;
            lastLinked = linked;
        }

        if (linked)
        {
            runSlot (0, -1, 0);
            publishMeters (0, 0);
            publishMeters (0, 1);
        }
        else
        {
            runSlot (0, 0, 0);
            runSlot (1, 1, 1);
            publishMeters (0, 0);
            publishMeters (1, 1);
        }

        // residual factors
        const double sol = solo;
        const auto& gL = slots[0].gain;
        const auto& gR = linked ? slots[0].gain : slots[1].gain;
        // per channel: a channel whose residual is all zero gets nothing added (bit-exact even when the other
        // channel processes; the packed inverse FFT would leak rounding noise between the channels)
        bool anyL = sol != 0.0, anyR = sol != 0.0;
        for (int k = 1; k < kW && ! (anyL && anyR); ++k)
        {
            anyL = anyL || gL[(size_t) k] != 1.0;
            anyR = anyR || gR[(size_t) k] != 1.0;
        }
        if (! anyL && ! anyR)
            return;

        for (int k = 0; k < numBins; ++k)
        {
            double rl = -sol, rr = -sol;
            if (k < kW)
            {
                const double w = weight[(size_t) k];
                rl = (1.0 - sol) * (gL[(size_t) k] - 1.0) + sol * (w - 1.0);
                rr = (1.0 - sol) * (gR[(size_t) k] - 1.0) + sol * (w - 1.0);
            }
            a[(size_t) k] *= (float) rl;
            b[(size_t) k] *= (float) rr;
        }

        fft->mergePacked (a.data(), b.data(), z.data());
        fft->inverse (z.data());

        constexpr float third = 1.0f / 3.0f;   // sum of w^2 over the hops = 3
        for (int j = 0; j < n; ++j)
        {
            const int r = (pos + j) & (n - 1);
            const float w = third * window[(size_t) j];
            if (anyL)
                acc[0][(size_t) r] += z[(size_t) j].real() * w;
            if (anyR)
                acc[1][(size_t) r] += z[(size_t) j].imag() * w;
        }
    }
};

LowContrast::LowContrast() : impl (std::make_unique<Impl>())
{
    impl->configure (48000.0);
    impl->snapSmoothers();
    impl->clearState();
}

LowContrast::~LowContrast() = default;

void LowContrast::prepare (double sampleRate, int)
{
    impl->configure (sampleRate);
    impl->snapSmoothers();
    impl->clearState();
}

void LowContrast::reset() noexcept
{
    impl->snapSmoothers();
    impl->clearState();
    impl->lastLinked = impl->linked;
}

void LowContrast::setParams (ParamValues values) noexcept
{
    auto& d = *impl;
    readLowContrastParams (values, d.params);
    if (getParamSet() == 1)
        d.params.sets[0] = d.params.sets[1];
    d.targetLowLog2 = std::log2 ((double) std::max (1.0f, d.params.lowHz));
    d.targetHighLog2 = std::log2 ((double) std::max (1.0f, d.params.highHz));
}

void LowContrast::setMonitor (const ModuleMonitor& m) noexcept { impl->soloTarget = m.soloRegion ? 1.0 : 0.0; }

void LowContrast::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    auto& d = *impl;
    if (numSamples <= 0 || d.ring[0].empty())
        return;

    ScopedFlushDenormals noDenormals;
    d.linked = ! (ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight);

    const int mask = d.fftSize - 1;
    float* r0 = d.ring[0].data();
    float* r1 = d.ring[1].data();
    float* a0 = d.acc[0].data();
    float* a1 = d.acc[1].data();
    for (int i = 0; i < numSamples; ++i)
    {
        const int p = d.pos;
        const float x0 = channels[0][i], x1 = channels[1][i];
        const float c0 = a0[p], c1 = a1[p];
        channels[0][i] = c0 == 0.0f ? r0[p] : r0[p] + c0;
        channels[1][i] = c1 == 0.0f ? r1[p] : r1[p] + c1;
        a0[p] = 0.0f;
        a1[p] = 0.0f;
        r0[p] = x0;
        r1[p] = x1;
        d.pos = (p + 1) & mask;

        if (++d.hopCount >= d.hop)
        {
            d.hopCount = 0;
            d.frame();
        }
    }
}

int LowContrast::getLatencySamples() const noexcept { return impl->fftSize; }

const LowContrastMeters& LowContrast::getMeters() const noexcept { return impl->meters; }
} // namespace ps
