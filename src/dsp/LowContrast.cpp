#include "dsp/LowContrast.h"

#include "core/Fft.h"
#include "dsp/Quadrature.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define PS_LOWC_HAS_SSE_CSR 1
#endif

// Low Contrast (dsp-lowc). Contract: LowContrast.h. Time-domain contrast of the low end in kBands bands:
//   y = x delayed by N  +  sum_b (g_b - 1) xb_b,   xb_b = ISTFT (W_b X) (band b of the region, linear phase, delayed N)
//
// Streaming layout: one ring of N samples per channel is the input history and the dry delay (ring[pos] read before
// it is overwritten is x[n - N]); one ring per band accumulates the overlap-added band signal (slot = time mod N,
// cleared when read). Every H = N / 4 samples the last N inputs are windowed (periodic Hann), both channels go through
// one complex FFT, and per band the bins weighted by W_b go back through one inverse FFT, the synthesis window and the
// 2/3 OLA scale: Hann^2 at hop N / 4 sums to exactly 1.5, and sum_b W_b = W (the region weight). The weights are a
// fixed linear filter (no per-frame gains), so the 75 % overlap needs no more.
// The gains g_b are computed per sample from the undelayed input (per band: IIR band pass, quadrature envelope,
// envelopes) and kept in rings of N gains; the output at time n (content n - N) reads the gain computed at
// n - N + D: a lookahead of D at no extra latency.
//
// Choices where the contract leaves room (documented here, rule 10 of ARCHITECTURE.md):
// - The constants (kModes, region ramps, kBands, kXoverOct) are fitted to renders of the reference (bass bursts of
//   10 .. 300 ms at 50 .. 400 Hz, kicks; Punchy / Smooth at +-50 and +100). The fit harness simulates this structure.
// - Detector power (Stereo / T/S) = (eL + eR) / 2 of the squared quadrature envelopes (a mono signal reads like its
//   Mid in M/S). The quadrature pair keeps a steady bass tone ripple-free (no gain modulation, no distortion).
// - Envelopes run on every sample also at neutral, so engaging the contrast never meets cold envelopes. The band
//   signals are synthesised always (kBands inverse FFTs per hop), so switching the contrast or Gain on has valid bands.
// - Neutral: the contrast part is exactly 0 when the smoothed contrast is 0; a total gain below 1e-6 dB counts as 0
//   dB, so the output is the delayed input bit-exactly.
// - Mode switches glide (50 ms) between both modes' gains; both modes' detectors always run. Make-up gain, mode blend
//   and solo glide per hop and are smoothed again per sample (5 ms), so no control steps at hop boundaries.
// - The detector bands run per sample; the quadrature pairs and envelopes run at fs / R (R = fs / 24 kHz rounded: 2 at
//   44.1 / 48 kHz, 4 at 88.2 / 96, 8 at 176.4 / 192; the bands end below ~650 Hz); the gains between two detector
//   steps are interpolated linearly (held gains would step at fs / R: HF images), lagging by under R samples.
// - Stereo / T/S: the gains are computed once per band and applied to both channels.
// - Auto gain compares the region power before and after the CONTRAST gains only (2 s one-poles, frozen below -70 dB),
//   so the user's Gain stays a make-up on top; it releases with the contrast control (50 ms glide).
// - Region solo glides 30 ms between the processed output and sum_b xb_b (the unprocessed region).
// - Meters: diffDb = W (f) x (sum_b w_b (f) hop-average gain_b + make-up), 150 ms one-pole per hop; regionIn /
//   regionOut = 10 log10 of the 300 ms smoothed region power before / after the full gain, floor -100 dB.
namespace ps
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kEps = 1.0e-10;                      // -100 dB of power
constexpr double kAutoFreeze = 1.0e-7;                // -70 dB of region power
constexpr double kMaxRegionHz = 300.0;
constexpr double kNeutralDb = 1.0e-6;
constexpr double kDbPerLog2 = 3.0102999566398120;    // 10 log10 (2)

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

// log2 for x > 0 (normal): exponent bits + atanh series of the mantissa in [1, 2), |error| < 2e-5 (6e-5 dB).
inline double fastLog2 (double x) noexcept
{
    std::uint64_t bits;
    std::memcpy (&bits, &x, sizeof bits);
    const int e = (int) ((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m;
    std::memcpy (&m, &bits, sizeof m);
    const double t = (m - 1.0) / (m + 1.0), t2 = t * t;
    return (double) e + 2.8853900817779268 * t * (1.0 + t2 * (1.0 / 3.0 + t2 * (0.2 + t2 * (1.0 / 7.0))));
}

inline double smoothTo (double v, double target, double a) noexcept
{
    v += a * (target - v);
    return std::abs (target - v) < 1.0e-9 ? target : v;
}

// Region weight: ramps linear in frequency, 0 at 0.8 x low to 1 at 2 x low, and 1 at 0.5 x high to 0 at 2 x high;
// W = their product (measured: the reference's region gain on pink noise, default 20..250 Hz: W 0.21 at 20 Hz, 0.95
// at 40, 0.92 at 150, 0.66 at 250, 0.25 at 400, 0 at 500; a 60..120 Hz region acts from ~40 to ~200 Hz).
constexpr double kLowRampFrom = 0.8, kLowRampTo = 2.0, kHighRampFrom = 0.5, kHighRampTo = 2.0;
// The region is split into kBands bands equally spaced in log f (cosine^2 crossfades kXoverOct wide, sum = 1); each
// band has its own detector and gain (a kick's pitch sweep reaches the lowest band late, so its lift lasts longer).
constexpr int kBands = 4;
constexpr double kXoverOct = 0.9;

// Per-mode constants of the gain law (times in ms). Per band: r = one-pole (rms) of the envelope power, fast F and slow
// S follow r (attack / release), gDb = k x c x clamp (10 log10 ((F + e) / (S + e)), lo, hi) (negative c: x neg, plus
// off x |c|), smoothed (tg), minus m x its one-pole (tm), applied D ms ahead.
// Fitted (round 9) to the reference's gain traces on bass bursts (10 .. 300 ms, 50 .. 400 Hz) at Punchy / Smooth +100
// / +50 / -50 and on real kicks, an 808 and a kick + bass loop at +-100 together: Punchy is a tight clamp times a large
// k, so a note's first ~20-40 ms get a flat lift that builds from the onset (short lookahead: the reference does not
// lift a kick's click), followed by a dip (m) at ~60-90 ms. Smooth's negative side (off): the Stereo bursts fit +2.5
// (negative contrast lifts ~1-2 dB); with the measured transient / sustain split the test-track renders (a sustain part
// at -49.6 in T/S) want a little more, +3.75 is the joint choice (both better than the old -1.5).
struct ModeSpec
{
    double rms, fa, fr, sa, sr, k, lo, hi, tg, tm, m, d, neg, off, epsDb;
};
constexpr ModeSpec kModes[2] {
    //  rms  fa  fr  sa  sr  k  lo  hi  tg  tm  m  D  neg  off  epsDb
    { 1.0, 0.804, 0.349, 8.39, 18.3, 3.65, -1.68, 0.509, 0.999, 45.9, 0.713, 4.62, 0.554, 1.87, -78.5 },  // Punchy
    { 5.41, 2.83, 15.6, 2.77, 19.4, -0.987, -17.3, 11.0, 2.33, 3.98, 0.453, 0.0, 0.719, 3.75, -111.0 },  // Smooth
};

struct ModeCoeffs
{
    double rms = 0.0, fa = 0.0, fr = 0.0, sa = 0.0, sr = 0.0, tg = 0.0, tm = 0.0, eps = 0.0;
    int lookahead = 0;
};

// Butterworth biquad (RBJ cookbook form = bilinear with prewarping at fc).
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    void set (double fc, double q, double sr, bool highPass) noexcept
    {
        const double w0 = 2.0 * kPi * std::clamp (fc, 1.0, 0.45 * sr) / sr, cw = std::cos (w0), al = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + al;
        if (highPass)
        {
            b0 = (1.0 + cw) / 2.0 / a0;
            b1 = -(1.0 + cw) / a0;
        }
        else
        {
            b0 = (1.0 - cw) / 2.0 / a0;
            b1 = (1.0 - cw) / a0;
        }
        b2 = b0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - al) / a0;
    }
};

struct BiquadState
{
    double s1 = 0.0, s2 = 0.0;   // transposed direct form II

    double run (const Biquad& c, double x) noexcept
    {
        const double y = c.b0 * x + s1;
        s1 = flushTiny (c.b1 * x - c.a1 * y + s2);
        s2 = flushTiny (c.b2 * x - c.a2 * y);
        return y;
    }
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
    int kW = 0;        // bins 0 .. kW - 1 can have W > 0 (region max 300 Hz + the taper)
    std::unique_ptr<Fft> fft;
    std::vector<float> window;
    std::vector<float> ring[2];
    std::vector<float> acc[kBands][2];           // per band and channel: the overlap-added band signal
    std::vector<float> gainRing[2][kBands][2];   // per slot, band and mode: contrast gain (dB) by detector time
    std::vector<std::complex<float>> z, a, b, ta, tb;
    std::vector<double> binLog2;                 // log2 of the bin frequency (bin 0: unused)
    std::vector<double> weight[kBands];          // W x band weight per bin
    double displayLog2[LowContrastMeters::kPoints] {};
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
    double lowLog2 = std::log2 (20.0), highLog2 = std::log2 (250.0), solo = 0.0, soloSample = 0.0;
    double aSample = 0.0;                            // 5 ms per-sample one-pole of the controls
    Biquad hp[kBands], lp1[kBands], lp2[kBands];   // detector bands (from the gliding edges)
    struct DetectorBand
    {
        BiquadState hp[2], lp1[2], lp2[2];          // per channel
        QuadratureEnvelope2 quad;                   // both channels
    } det[kBands];

    struct SetState
    {
        double c = 0.0, gainDb = 0.0;
        double modeBlend = 0.0;                     // 0 = Punchy, 1 = Smooth; glides 50 ms (no gain step on a switch)
        double mDbSample = 0.0, blendSample = 0.0;  // make-up (gain + auto) and blend, smoothed per sample
        double lastTotDb[kBands] {}, lastG[kBands] { 1.0, 1.0, 1.0, 1.0 };   // exp cache
        double lastGDb[kBands] {}, lastGc[kBands] { 1.0, 1.0, 1.0, 1.0 };
        double autoDb = 0.0, autoTargetDb = 0.0, pin = 0.0, pout = 0.0;
        double meterIn = 0.0, meterOut = 0.0;
        double hopPin = 0.0, hopPoutC = 0.0, hopPoutF = 0.0, hopGainDb[kBands] {};
        int hopCount = 0;
        double diff[LowContrastMeters::kPoints] {};
    } sets[kNumChannelSets];

    struct Env
    {
        double r = 0.0, f = 0.0, s = 0.0, g = 0.0, mk = 0.0;
        float out = 0.0f, prev = 0.0f;               // the latest two gains (dB); the ring gets them interpolated
    };
    int detDecim = 2, detPhase = 0;
    Env env[2][kBands][2];                           // per slot, band, mode

    //==============================================================================
    void configure (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        fftSize = sampleRate <= 50000.0 ? 4096 : (sampleRate <= 100000.0 ? 8192 : 16384);
        hop = fftSize / 4;
        numBins = fftSize / 2 + 1;
        binHz = sampleRate / (double) fftSize;
        kW = std::min (numBins, (int) std::ceil (kMaxRegionHz * kHighRampTo / binHz) + 1);

        fft = std::make_unique<Fft> (orderOf (fftSize));
        window.resize ((size_t) fftSize);
        for (int j = 0; j < fftSize; ++j)
            window[(size_t) j] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (double) j / (double) fftSize));
        for (int c = 0; c < 2; ++c)
        {
            ring[c].assign ((size_t) fftSize, 0.0f);
            for (auto& band : acc)
                band[c].assign ((size_t) fftSize, 0.0f);
            for (auto& band : gainRing[c])
                for (auto& g : band)
                    g.assign ((size_t) fftSize, 0.0f);
        }
        z.assign ((size_t) fftSize, {});
        for (auto* v : { &a, &b, &ta, &tb })
            v->assign ((size_t) numBins, {});

        binLog2.assign ((size_t) kW + 1, 0.0);
        for (int k = 1; k <= kW; ++k)
            binLog2[(size_t) k] = std::log2 ((double) k * binHz);
        for (auto& w : weight)
            w.assign ((size_t) kW + 1, 0.0);
        for (int i = 0; i < LowContrastMeters::kPoints; ++i)
            displayLog2[i] = std::log2 (lowContrastDisplayHz (i));
        detDecim = std::max (1, (int) std::lround (sampleRate / 24000.0));
        for (auto& d : det)
            d.quad.prepare (sampleRate / (double) detDecim, 20.0);

        const auto hopCoeff = [this] (double ms) { return 1.0 - std::exp (-(double) hop / (ms * 0.001 * sampleRate)); };
        a50 = hopCoeff (50.0);
        a30 = hopCoeff (30.0);
        a150 = hopCoeff (150.0);
        a300 = hopCoeff (300.0);
        a1s = hopCoeff (1000.0);
        a2s = hopCoeff (2000.0);
        aSample = 1.0 - std::exp (-1.0 / (0.005 * sampleRate));
        const double detRate = sampleRate / (double) detDecim;
        const auto co = [detRate] (double ms) { return 1.0 - std::exp (-1.0 / (std::max (ms, 0.01) * 0.001 * detRate)); };
        for (int m = 0; m < 2; ++m)
        {
            const auto& s = kModes[m];
            auto& c = modes[m];
            c.rms = co (s.rms);
            c.fa = co (s.fa);
            c.fr = co (s.fr);
            c.sa = co (s.sa);
            c.sr = co (s.sr);
            c.tg = co (s.tg);
            c.tm = co (s.tm);
            c.eps = std::pow (10.0, s.epsDb / 10.0);
            c.lookahead = std::clamp ((int) std::lround (s.d * 0.001 * sampleRate), 0, fftSize - 1);
        }
    }

    void snapSmoothers() noexcept
    {
        lowLog2 = targetLowLog2;
        highLog2 = targetHighLog2;
        solo = soloSample = soloTarget;
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            auto& st = sets[s];
            const auto& sp = params.sets[s];
            st.c = (double) sp.contrastPercent / 100.0;
            st.modeBlend = st.blendSample = sp.mode == LowContrastMode::Smooth ? 1.0 : 0.0;
            st.gainDb = (double) sp.gainDb;
            st.mDbSample = st.gainDb + st.autoDb;
        }
        updateRegion();
    }

    void clearEnvelopes() noexcept
    {
        for (auto& slot : env)
            for (auto& band : slot)
                for (auto& e : band)
                    e = Env {};
    }

    void clearState() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (ring[c].begin(), ring[c].end(), 0.0f);
            for (auto& band : acc)
                std::fill (band[c].begin(), band[c].end(), 0.0f);
            for (auto& band : gainRing[c])
                for (auto& g : band)
                    std::fill (g.begin(), g.end(), 0.0f);
        }
        pos = 0;
        hopCount = 0;
        detPhase = 0;
        for (auto& d : det)
        {
            for (int c = 0; c < 2; ++c)
                d.hp[c] = d.lp1[c] = d.lp2[c] = BiquadState {};
            d.quad.reset();
        }
        clearEnvelopes();
        for (auto& st : sets)
        {
            st.autoDb = 0.0;
            st.autoTargetDb = 0.0;
            st.mDbSample = st.gainDb;
            st.pin = st.pout = 0.0;
            st.meterIn = st.meterOut = 0.0;
            st.hopPin = st.hopPoutC = st.hopPoutF = 0.0;
            std::fill (std::begin (st.hopGainDb), std::end (st.hopGainDb), 0.0);
            st.hopCount = 0;
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

    double regionWeight (double lf) const noexcept
    {
        const double f = std::exp2 (lf), lo = std::exp2 (lowLog2), hi = std::exp2 (highLog2);
        const double wl = std::clamp ((f - kLowRampFrom * lo) / ((kLowRampTo - kLowRampFrom) * lo), 0.0, 1.0);
        const double wh = std::clamp ((kHighRampTo * hi - f) / ((kHighRampTo - kHighRampFrom) * hi), 0.0, 1.0);
        return wl * wh;
    }

    double edgeLog2 (int j) const noexcept { return lowLog2 + (highLog2 - lowLog2) * (double) j / (double) kBands; }

    // band weights at lf (sum = 1): each crossfade is cosine^2 over kXoverOct around its edge
    void bandWeights (double lf, double* w) const noexcept
    {
        double rest = 1.0;
        for (int j = 1; j < kBands; ++j)
        {
            const double u = std::clamp ((lf - (edgeLog2 (j) - 0.5 * kXoverOct)) / kXoverOct, 0.0, 1.0);
            const double c = std::cos (0.5 * kPi * u);
            w[j - 1] = rest * c * c;
            rest *= 1.0 - c * c;
        }
        w[kBands - 1] = rest;
    }

    // W x band weights per bin and the detector bands from the (gliding) region edges
    void updateRegion() noexcept
    {
        double w[kBands];
        for (int k = 0; k <= kW; ++k)
        {
            const double r = (k >= 1 && k < kW) ? regionWeight (binLog2[(size_t) k]) : 0.0;
            if (r > 0.0)
                bandWeights (binLog2[(size_t) k], w);
            for (int band = 0; band < kBands; ++band)
                weight[band][(size_t) k] = r > 0.0 ? r * w[band] : 0.0;
        }
        for (int band = 0; band < kBands; ++band)
        {
            const double lo = band == 0 ? lowLog2 : edgeLog2 (band);
            const double hi = band == kBands - 1 ? highLog2 + std::log2 (0.5 * (kHighRampFrom + kHighRampTo)) : edgeLog2 (band + 1);
            hp[band].set (std::exp2 (lo), 0.70710678, sampleRate, true);
            lp1[band].set (std::exp2 (hi), 0.54119610, sampleRate, false);
            lp2[band].set (std::exp2 (hi), 1.30656296, sampleRate, false);
        }
    }

    void updateHopSmoothers() noexcept
    {
        glide (lowLog2, targetLowLog2, a50, 1.0e-7);
        glide (highLog2, targetHighLog2, a50, 1.0e-7);
        glide (solo, soloTarget, a30, 1.0e-6);
        for (int s = 0; s < kNumChannelSets; ++s)
        {
            auto& st = sets[s];
            const auto& sp = params.sets[s];
            glide (st.c, (double) sp.contrastPercent / 100.0, a50, 1.0e-6);
            glide (st.modeBlend, sp.mode == LowContrastMode::Smooth ? 1.0 : 0.0, a50, 1.0e-6);
            glide (st.gainDb, (double) sp.gainDb, a50, 1.0e-6);
        }
        updateRegion();
    }

    // Auto gain, meters: once per hop from the sums of the hop.
    void finishHop (int set, int toSet0, int toSet1) noexcept
    {
        auto& st = sets[set];
        const double n = (double) std::max (1, st.hopCount);
        const double pin = st.hopPin / n, poutC = st.hopPoutC / n, poutF = st.hopPoutF / n;
        double gAvg[kBands];
        for (int band = 0; band < kBands; ++band)
        {
            gAvg[band] = st.hopGainDb[band] / n;
            st.hopGainDb[band] = 0.0;
        }
        st.hopPin = st.hopPoutC = st.hopPoutF = 0.0;
        st.hopCount = 0;

        st.pin = flushTiny (st.pin + a2s * (pin - st.pin));
        st.pout = flushTiny (st.pout + a2s * (poutC - st.pout));
        const bool neutralContrast = params.sets[set].contrastPercent == 0.0f;
        if (st.c == 0.0)
            st.pout = st.pin;
        // Silence freezes a learned correction only while contrast is requested. At neutral there is no contrast
        // gain to compensate: release alongside the contrast control, without a gain step.
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

        st.meterIn = flushTiny (st.meterIn + a300 * (pin - st.meterIn));
        st.meterOut = flushTiny (st.meterOut + a300 * (poutF - st.meterOut));
        const double mDb = st.gainDb + st.autoDb;
        for (int i = 0; i < LowContrastMeters::kPoints; ++i)
        {
            const double r = regionWeight (displayLog2[i]);
            double v = 0.0;
            if (r > 0.0)
            {
                double w[kBands];
                bandWeights (displayLog2[i], w);
                for (int band = 0; band < kBands; ++band)
                    v += w[band] * gAvg[band];
                v = r * (v + mDb);
            }
            st.diff[i] = flushTiny (st.diff[i] + a150 * (v - st.diff[i]));
        }
        for (const int to : { toSet0, toSet1 })
        {
            if (to < 0)
                continue;
            for (int i = 0; i < LowContrastMeters::kPoints; ++i)
                meters.diffDb[to][i].store ((float) st.diff[i]);
            meters.regionInDb[to].store ((float) (10.0 * std::log10 (std::max (st.meterIn, kEps))));
            meters.regionOutDb[to].store ((float) (10.0 * std::log10 (std::max (st.meterOut, kEps))));
            meters.autoGainDb[to].store ((float) st.autoDb);
        }
    }

    // One detector step of a band (at fs / R): envelope power v -> contrast gain (dB) per mode (both modes run; the
    // output blends them, each read with its own lookahead).
    void detect (int slot, int band, int set, double v) noexcept
    {
        const double c = sets[set].c;
        for (int m = 0; m < 2; ++m)
        {
            const auto& mc = modes[m];
            const auto& ms = kModes[m];
            auto& e = env[slot][band][m];
            e.r = flushTiny (e.r + mc.rms * (v - e.r));
            e.f = flushTiny (e.f + (e.r > e.f ? mc.fa : mc.fr) * (e.r - e.f));
            e.s = flushTiny (e.s + (e.r > e.s ? mc.sa : mc.sr) * (e.r - e.s));
            double gt = 0.0;
            if (c != 0.0)
            {
                const double rel = std::clamp (kDbPerLog2 * fastLog2 ((e.f + mc.eps) / (e.s + mc.eps)), ms.lo, ms.hi);
                gt = c >= 0.0 ? ms.k * c * rel : ms.k * ms.neg * c * rel - ms.off * c;
            }
            e.g = flushTiny (e.g + mc.tg * (gt - e.g));
            e.mk = flushTiny (e.mk + mc.tm * (e.g - e.mk));
            const double gm = e.g - ms.m * e.mk;
            e.prev = e.out;
            e.out = std::abs (gm) < kNeutralDb ? 0.0f : (float) gm;
        }
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

        if (linked)
        {
            finishHop (0, 0, 1);
        }
        else
        {
            finishHop (0, 0, -1);
            finishHop (1, 1, -1);
        }
        updateHopSmoothers();
        if (linked != lastLinked)
        {
            clearEnvelopes();
            lastLinked = linked;
        }

        constexpr float olaScale = 2.0f / 3.0f;   // sum of w^2 over the hops = 1.5
        for (int band = 0; band < kBands; ++band)
        {
            const auto& wb = weight[band];
            for (int k = 0; k < numBins; ++k)
            {
                const float w = k < kW ? (float) wb[(size_t) k] : 0.0f;
                ta[(size_t) k] = a[(size_t) k] * w;
                tb[(size_t) k] = b[(size_t) k] * w;
            }
            fft->mergePacked (ta.data(), tb.data(), z.data());
            fft->inverse (z.data());
            float* o0 = acc[band][0].data();
            float* o1 = acc[band][1].data();
            for (int j = 0; j < n; ++j)
            {
                const int r = (pos + j) & (n - 1);
                const float w = olaScale * window[(size_t) j];
                o0[r] += z[(size_t) j].real() * w;
                o1[r] += z[(size_t) j].imag() * w;
            }
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

    constexpr double ln10over20 = 0.11512925464970229;
    const int mask = d.fftSize - 1;
    for (int i = 0; i < numSamples; ++i)
    {
        const int p = d.pos;
        const float x0 = channels[0][i], x1 = channels[1][i];

        // output: content n - N with each band's gain computed at n - N + D (Stereo / T/S: one set for both channels)
        d.soloSample = smoothTo (d.soloSample, d.solo, d.aSample);
        const double sol = d.soloSample;
        const int numSets = d.linked ? 1 : 2;
        double gains[2][kBands], gainsC[2][kBands];
        for (int set = 0; set < numSets; ++set)
        {
            auto& st = d.sets[set];
            st.mDbSample = smoothTo (st.mDbSample, st.gainDb + st.autoDb, d.aSample);
            st.blendSample = smoothTo (st.blendSample, st.modeBlend, d.aSample);
            const double mDb = st.mDbSample, blend = st.blendSample;
            for (int band = 0; band < kBands; ++band)
            {
                const auto& rings = d.gainRing[set][band];
                const double gP = blend >= 1.0 ? 0.0 : (double) rings[0][(size_t) ((p + d.modes[0].lookahead) & mask)];
                const double gS = blend <= 0.0 ? 0.0 : (double) rings[1][(size_t) ((p + d.modes[1].lookahead) & mask)];
                double gDb = (1.0 - blend) * gP + blend * gS;
                if (std::abs (gDb) < kNeutralDb)
                    gDb = 0.0;
                const double totDb = gDb + mDb;
                if (totDb != st.lastTotDb[band])
                {
                    st.lastTotDb[band] = totDb;
                    st.lastG[band] = std::abs (totDb) < kNeutralDb ? 1.0 : std::exp (totDb * ln10over20);
                }
                if (gDb != st.lastGDb[band])
                {
                    st.lastGDb[band] = gDb;
                    st.lastGc[band] = gDb == 0.0 ? 1.0 : std::exp (gDb * ln10over20);
                }
                gains[set][band] = st.lastG[band];
                gainsC[set][band] = d.params.autoGain ? st.lastGc[band] : 1.0;
                st.hopGainDb[band] += gDb;
            }
            ++st.hopCount;
        }
        for (int c = 0; c < 2; ++c)
        {
            const int set = d.linked ? 0 : c;
            auto& st = d.sets[set];
            double y = (double) d.ring[c][(size_t) p];
            double xs = 0.0, outC = 0.0, outF = 0.0;
            bool changed = false;
            for (int band = 0; band < kBands; ++band)
            {
                float& slot = d.acc[band][c][(size_t) p];
                const double xb = (double) slot;
                slot = 0.0f;
                const double g = gains[set][band];
                if (g != 1.0)
                {
                    y += (g - 1.0) * xb;
                    changed = true;
                }
                xs += xb;
                outC += gainsC[set][band] * xb;
                outF += g * xb;
            }
            if (sol != 0.0)
            {
                y = (1.0 - sol) * y + sol * xs;
                changed = true;
            }
            channels[c][i] = changed ? (float) y : d.ring[c][(size_t) p];

            // auto gain / meters (Stereo: both channels into set 0, averaged)
            const double sc = d.linked ? 0.5 : 1.0;
            st.hopPin += sc * xs * xs;
            st.hopPoutC += sc * outC * outC;
            st.hopPoutF += sc * outF * outF;
        }

        // detectors (undelayed input) -> contrast gains by detector time (written after the read: with D = 0 the
        // slot at p still holds the gain of n - N)
        const bool detTick = ++d.detPhase >= d.detDecim;
        if (detTick)
            d.detPhase = 0;
        for (int band = 0; band < kBands; ++band)
        {
            auto& db = d.det[band];
            const double y0 = db.lp2[0].run (d.lp2[band], db.lp1[0].run (d.lp1[band], db.hp[0].run (d.hp[band], (double) x0)));
            const double y1 = db.lp2[1].run (d.lp2[band], db.lp1[1].run (d.lp1[band], db.hp[1].run (d.hp[band], (double) x1)));
            if (detTick)
            {
                double qa0, qb0, qa1, qb1;
                db.quad.processPair (y0, y1, qa0, qb0, qa1, qb1);
                const double e0 = qa0 * qa0 + qb0 * qb0, e1 = qa1 * qa1 + qb1 * qb1;
                if (d.linked)
                {
                    d.detect (0, band, 0, 0.5 * (e0 + e1));
                }
                else
                {
                    d.detect (0, band, 0, e0);
                    d.detect (1, band, 1, e1);
                }
            }
            const float frac = (float) (d.detPhase + 1) / (float) d.detDecim;
            for (int slot = 0; slot < numSets; ++slot)
                for (int m = 0; m < 2; ++m)
                {
                    const auto& e = d.env[slot][band][m];
                    d.gainRing[slot][band][m][(size_t) p] = e.prev + frac * (e.out - e.prev);
                }
        }

        d.ring[0][(size_t) p] = x0;
        d.ring[1][(size_t) p] = x1;
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
