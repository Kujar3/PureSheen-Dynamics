#include "dsp/Punch.h"

#include "dsp/Crossover.h"
#include "dsp/Quadrature.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if PS_QUAD_SSE2
 #include <emmintrin.h>
#endif
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define PS_PNCH_HAS_SSE_CSR 1
#endif

// Punch (Punch.h): multiband micro-dynamics from the ratio of a short and a long envelope of each band.
//
// Choices beyond the header (a mastering engineer's expectations, dsp-punch):
//   - Detectors: one quadrature pair (QuadratureEnvelope2, lowest 20 Hz) per band for both channels. The detector
//     power is max (a^2 + b^2, x^2 / 4): the quarter of the instantaneous power only takes over on fast onsets, where
//     the all-pass chains' group delay (up to ~19 ms at 40 Hz) makes the quadrature power lag by more than 6 dB, so a
//     kick's first cycle is seen at once; on steady and slowly moving tones the quadrature power always leads (no
//     ripple). In Stereo the band's mid detector needs no third splitter: the pair is linear, so the mid pair is
//     ((aL + aR) / 2, (bL + bR) / 2); its envelopes run all the time, so Stereo Link toggles crossfade (20 ms) between
//     two warm gain computers.
//   - Settle: after prepare / reset() (the chain resets the module on a channel-mode change), a band-count change or a
//     crossover-type change, the module's effect is faded out (10 ms; a band-count change waits for that before it
//     reaches the crossover, so bands never merge or split under different gains), the long envelopes are held equal
//     to the short ones for 30 ms while the detectors warm up on the new content (rel = 0), then the effect fades back
//     in over 20 ms. "Effect" = the whole band gain incl. make-up: y = x (1 + w (g makeup - 1)), w = settle x band on.
//     Band-count changes split processing at the exact fade boundary. A soloed monitor dips for 10 ms before
//     changing band membership and returns over 10 ms, so merging or removing its band cannot produce a step.
//   - Band on / off: 20 ms ramp, band solo 10 ms; band off with its ramp at 0 is exactly y = x_band.
//   - Auto gain: the make-up targets are evaluated every 32 samples on an internal counter (independent of the host
//     block size), as linear sqrt (Pin / Pout) clamped to +-12 dB; the 500 ms follower runs per sample. reset() keeps
//     the learned make-up and the 3 s power averages (a level correction, not signal history), so a transport restart
//     or channel-mode change does not drop the level for seconds.
//     Independent and shared make-up followers run together; Stereo Link crossfades them over the same 20 ms
//     ramp as the detector gains, so the final linked gain is common even after asymmetric independent processing.
//   - Meters (Stereo, both channels set A): unlinked, gainDb is the channel with the larger |gain|, gainMin / Max span
//     both channels, micro meters show the larger of the two; set B mirrors set A. The meters show the effective gain
//     20 log10 (1 + wet x (gain makeup - 1)) (0 dB for a band that is off), tracked linearly per sample. Micro meters:
//     rel sampled every 16 samples into 3 s one-pole mean / mean square; the output side runs a second quadrature
//     detector on the rendered band (input power x gain^2 would misread fast settings on bass by up to 1 dB, since the
//     gain moves within the detector's group delay). Costs ~15 % CPU; it reads exactly what a detector on the output
//     would.
//     gainMinDb is published with a plain load / store (a racing UI exchange may repeat the previous extreme).
//   - Precision / speed: envelopes and gain computer in double, both channels in the two lanes of SSE2 registers;
//     ln / exp by short series (errors < 1e-8, far below 0.0001 dB). Denormals: FTZ / DAZ inside process (plus the
//     quadrature's own flush).
namespace ps
{
namespace
{
#if PS_PNCH_HAS_SSE_CSR
struct ScopedFlushDenormals
{
    ScopedFlushDenormals() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
    ~ScopedFlushDenormals() { _mm_setcsr (saved); }
    unsigned int saved;
};
#else
struct ScopedFlushDenormals {};
#endif

constexpr double kOnset = 0.25;                    // -6 dB: instantaneous power floor for onsets
// Fitted to the reference (out/importer/tools/impopt2.py: one-band renders of a tone / kick / noise test signal at
// amounts +50 / +100 / -100, plus two music slices with a four-band preset; rms 0.6 dB on the test points, 0.4 dB
// on the music bands):
constexpr double kRelScale = 0.732;                // gain in dB per dB of envelope ratio at amount 100
constexpr double kNegativeScale = 0.578;           // negative amounts act 0.58 x as strongly as positive ones
constexpr double kGainMin = -8.0, kGainMax = 8.0;  // dB
constexpr double kFloor = 5.35e-3;                 // -22.7 dB (re a full-scale sine's detector power) added to both
                                                   // envelopes: quiet bands get little or no gain
constexpr double kLongAttack = 88.4, kLongRelease = 9.19;   // x E: the long envelope rises over seconds, falls fast
constexpr double kLongAttackMaxMs = 10000.0;       // our cap (the fit saw E ~ 100 ms only)
constexpr double kShortRelease = 1.39, kShortAttack = 0.0158;   // x E
constexpr double kDbToLn = 0.11512925464970228;    // ln (10) / 20
constexpr double kPowToDb = 4.3429448190325182;    // 10 / ln (10)
constexpr double kRatioMin = 1.0e-4, kRatioMax = 251.18864315095797;   // rel clamped to [-40, +24] dB
constexpr double kLn2 = 0.69314718055994531, kLog2e = 1.4426950408889634, kSqrt2 = 1.4142135623730951;
constexpr int kAutoGainInterval = 32;
constexpr int kMicroInterval = 16;
constexpr double kSettleOutMs = 10.0, kSettleHoldMs = 30.0, kSettleInMs = 20.0;

double coeff (double ms, double sr) noexcept
{
    const double samples = ms * 0.001 * sr;
    return samples > 0.0 ? 1.0 - std::exp (-1.0 / samples) : 1.0;
}

// ln (x) for positive normal x (the clamped envelope ratios): mantissa in [sqrt 0.5, sqrt 2), atanh series to s^9.
inline double fastLn (double x) noexcept
{
    std::uint64_t bits;
    std::memcpy (&bits, &x, sizeof (bits));
    int e = (int) ((bits >> 52) & 0x7ffu) - 1023;
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m;
    std::memcpy (&m, &bits, sizeof (m));
    if (m > kSqrt2)
    {
        m *= 0.5;
        ++e;
    }
    const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    const double ln = 2.0 * s * (1.0 + s2 * (1.0 / 3.0 + s2 * (1.0 / 5.0 + s2 * (1.0 / 7.0 + s2 * (1.0 / 9.0)))));
    return ln + (double) e * kLn2;
}

inline double relDb (double as, double al) noexcept
{
    return kPowToDb * fastLn (std::clamp ((as + kFloor) / (al + kFloor), kRatioMin, kRatioMax));
}

//==============================================================================
// Two doubles (channel 0 / 1) in one register.
#if PS_QUAD_SSE2
struct V2
{
    __m128d v;
};
inline V2 v2 (double a, double b) noexcept { return { _mm_set_pd (b, a) }; }
inline V2 splat (double a) noexcept { return { _mm_set1_pd (a) }; }
inline V2 load (const double* p) noexcept { return { _mm_loadu_pd (p) }; }
inline void store (double* p, V2 a) noexcept { _mm_storeu_pd (p, a.v); }
inline V2 operator+ (V2 a, V2 b) noexcept { return { _mm_add_pd (a.v, b.v) }; }
inline V2 operator- (V2 a, V2 b) noexcept { return { _mm_sub_pd (a.v, b.v) }; }
inline V2 operator* (V2 a, V2 b) noexcept { return { _mm_mul_pd (a.v, b.v) }; }
inline V2 operator/ (V2 a, V2 b) noexcept { return { _mm_div_pd (a.v, b.v) }; }
inline V2 vmin (V2 a, V2 b) noexcept { return { _mm_min_pd (a.v, b.v) }; }
inline V2 vmax (V2 a, V2 b) noexcept { return { _mm_max_pd (a.v, b.v) }; }
inline V2 vclamp (V2 a, V2 lo, V2 hi) noexcept { return vmin (vmax (a, lo), hi); }
inline V2 greaterSelect (V2 a, V2 b, V2 ifTrue, V2 ifFalse) noexcept   // a > b ? ifTrue : ifFalse
{
    const __m128d m = _mm_cmpgt_pd (a.v, b.v);
    return { _mm_or_pd (_mm_and_pd (m, ifTrue.v), _mm_andnot_pd (m, ifFalse.v)) };
}
inline double lane0 (V2 a) noexcept { return _mm_cvtsd_f64 (a.v); }
inline double lane1 (V2 a) noexcept { return _mm_cvtsd_f64 (_mm_unpackhi_pd (a.v, a.v)); }

inline V2 fastLn (V2 x) noexcept
{
    const __m128i bits = _mm_castpd_si128 (x.v);
    const __m128d magic = _mm_set1_pd (4503599627370496.0);   // 2^52: OR an integer < 2^52 into its mantissa
    const __m128d expField = _mm_sub_pd (_mm_castsi128_pd (_mm_or_si128 (_mm_srli_epi64 (bits, 52),
                                                                         _mm_castpd_si128 (magic))), magic);
    V2 e { _mm_sub_pd (expField, _mm_set1_pd (1023.0)) };
    const __m128i mantMask = _mm_set_epi32 (0x000fffff, -1, 0x000fffff, -1);
    V2 m { _mm_castsi128_pd (_mm_or_si128 (_mm_and_si128 (bits, mantMask), _mm_castpd_si128 (_mm_set1_pd (1.0)))) };
    const __m128d big = _mm_cmpgt_pd (m.v, _mm_set1_pd (kSqrt2));
    m.v = _mm_mul_pd (m.v, _mm_or_pd (_mm_and_pd (big, _mm_set1_pd (0.5)), _mm_andnot_pd (big, _mm_set1_pd (1.0))));
    e.v = _mm_add_pd (e.v, _mm_and_pd (big, _mm_set1_pd (1.0)));
    const V2 one = splat (1.0);
    const V2 s = (m - one) / (m + one), s2 = s * s;
    const V2 poly = one + s2 * (splat (1.0 / 3.0) + s2 * (splat (1.0 / 5.0) + s2 * (splat (1.0 / 7.0) + s2 * splat (1.0 / 9.0))));
    return splat (2.0) * s * poly + e * splat (kLn2);
}

// e^x for |x| < 700: 2^n x e^f, |f| <= ln 2 / 2, Taylor to f^7. fastExp (0) == 1 exactly.
inline V2 fastExp (V2 x) noexcept
{
    const V2 t = x * splat (kLog2e);
    const __m128i n = _mm_cvtpd_epi32 (t.v);   // round to nearest (MXCSR default)
    const V2 f = (t - V2 { _mm_cvtepi32_pd (n) }) * splat (kLn2);
    const V2 one = splat (1.0);
    const V2 p = one + f * (one + f * (splat (0.5) + f * (splat (1.0 / 6.0) + f * (splat (1.0 / 24.0)
                 + f * (splat (1.0 / 120.0) + f * (splat (1.0 / 720.0) + f * splat (1.0 / 5040.0)))))));
    const __m128i biased = _mm_unpacklo_epi32 (_mm_add_epi32 (n, _mm_set1_epi32 (1023)), _mm_setzero_si128());
    return p * V2 { _mm_castsi128_pd (_mm_slli_epi64 (biased, 52)) };
}

inline void quadPair (QuadratureEnvelope2& q, V2 x, V2& a, V2& b) noexcept { q.processPair (x.v, a.v, b.v); }
#else
struct V2
{
    double a, b;
};
inline V2 v2 (double a, double b) noexcept { return { a, b }; }
inline V2 splat (double a) noexcept { return { a, a }; }
inline V2 load (const double* p) noexcept { return { p[0], p[1] }; }
inline void store (double* p, V2 x) noexcept
{
    p[0] = x.a;
    p[1] = x.b;
}
inline V2 operator+ (V2 x, V2 y) noexcept { return { x.a + y.a, x.b + y.b }; }
inline V2 operator- (V2 x, V2 y) noexcept { return { x.a - y.a, x.b - y.b }; }
inline V2 operator* (V2 x, V2 y) noexcept { return { x.a * y.a, x.b * y.b }; }
inline V2 operator/ (V2 x, V2 y) noexcept { return { x.a / y.a, x.b / y.b }; }
inline V2 vmin (V2 x, V2 y) noexcept { return { std::min (x.a, y.a), std::min (x.b, y.b) }; }
inline V2 vmax (V2 x, V2 y) noexcept { return { std::max (x.a, y.a), std::max (x.b, y.b) }; }
inline V2 vclamp (V2 x, V2 lo, V2 hi) noexcept { return vmin (vmax (x, lo), hi); }
inline V2 greaterSelect (V2 x, V2 y, V2 t, V2 f) noexcept { return { x.a > y.a ? t.a : f.a, x.b > y.b ? t.b : f.b }; }
inline double lane0 (V2 x) noexcept { return x.a; }
inline double lane1 (V2 x) noexcept { return x.b; }
inline V2 fastLn (V2 x) noexcept { return { fastLn (x.a), fastLn (x.b) }; }
inline V2 fastExp (V2 x) noexcept { return { std::exp (x.a), std::exp (x.b) }; }
inline void quadPair (QuadratureEnvelope2& q, V2 x, V2& a, V2& b) noexcept { q.processPair (x.a, x.b, a.a, b.a, a.b, b.b); }
#endif

//==============================================================================
struct EnvPair
{
    double as = 0.0, al = 0.0;

    inline void run (double p, double ca, double cr, double cl, double cla) noexcept
    {
        as += (p > as ? ca : cr) * (p - as);
        al += (p > al ? cla : cl) * (p - al);
    }
    void clear() noexcept { as = al = 0.0; }
};

struct MicroStat
{
    double mean = 0.0, meanSq = 0.0;
    inline void push (double v, double c) noexcept
    {
        mean += c * (v - mean);
        meanSq += c * (v * v - meanSq);
    }
    double stdDev() const noexcept { return std::sqrt (std::max (0.0, meanSq - mean * mean)); }
    void clear() noexcept { mean = meanSq = 0.0; }
};

// One band, both channels (lane 0 = channel 0). Arrays of 2 hold the lanes between blocks.
struct BandState
{
    QuadratureEnvelope2 quad, quadOut;               // quadOut: the output side of the micro meter
    double as[2] {}, al[2] {}, asOut[2] {}, alOut[2] {};
    MicroStat microIn[2], microOut[2];
    double pin[2] {}, pout[2] {};                     // auto gain power averages (3 s)
    double makeup[2] { 1.0, 1.0 }, makeupTarget[2] { 1.0, 1.0 };   // linear
    double linkedMakeup = 1.0, linkedMakeupTarget = 1.0;
    EnvPair midIn, midOut;                            // Stereo: the band's mid
    MicroStat midMicroIn, midMicroOut;

    void clearDetectors() noexcept
    {
        quad.reset();
        quadOut.reset();
        for (int c = 0; c < 2; ++c)
        {
            as[c] = al[c] = asOut[c] = alOut[c] = 0.0;
            microIn[c].clear();
            microOut[c].clear();
        }
        midIn.clear();
        midOut.clear();
        midMicroIn.clear();
        midMicroOut.clear();
    }
};

struct Ramp
{
    double value = 0.0, target = 0.0, step = 0.0;
    int remaining = 0, length = 1;

    void prepare (double sr, double ms) noexcept { length = std::max (1, (int) std::lround (ms * 0.001 * sr)); }
    void setTarget (double t) noexcept
    {
        if (t == target)
            return;
        target = t;
        remaining = length;
        step = (target - value) / (double) length;
    }
    void snap() noexcept
    {
        value = target;
        remaining = 0;
    }
    inline double next() noexcept
    {
        if (remaining > 0)
            value = --remaining == 0 ? target : value + step;
        return value;
    }
};

struct BlockMeter
{
    double gMin[2] { 1e9, 1e9 }, gMax[2] { -1e9, -1e9 }, last[2] { 1.0, 1.0 };   // linear gains
    bool any = false;
    void clear() noexcept
    {
        gMin[0] = gMin[1] = 1e9;
        gMax[0] = gMax[1] = -1e9;
        any = false;
    }
};
} // namespace

double punchEnvelopeMs (const PunchParams& p, double bpm) noexcept
{
    double ms = (double) p.envelopeMs;
    if (p.sync)
    {
        const double tempo = std::isfinite (bpm) && bpm > 0.0 ? bpm : 120.0;
        const int note = std::clamp (p.syncNote, 0, kPunchSyncNotes - 1);
        ms = kPunchSyncFractions[(size_t) note] * 4.0 * 60000.0 / tempo;
    }
    return std::clamp (std::isfinite (ms) ? ms : 100.0, 10.0, 1000.0);
}

//==============================================================================
struct Punch::Impl
{
    PunchMeters meters;

    double sr = 48000.0;
    int maxBlock = 512;
    bool prepared = false;
    bool fresh = true;

    PunchParams params;
    ModuleMonitor monitor;

    Crossover crossover;
    std::vector<float> bandBuffers;     // kMaxBands x 2 x maxBlock
    float* bandPtr[kMaxBands][2] {};
    float* const* bandOut[kMaxBands] {};
    std::vector<float> sumBuffer;       // 2 x maxBlock
    std::vector<double> linkRamp, settleRamp, soloTopologyRamp;
    std::vector<unsigned char> holdMask;
    std::vector<double> envBuffer, midBuffer;   // per-chunk pass buffers of processBand

    BandState bands[kMaxBands];
    Ramp amountK[2][kMaxBands];   // per channel slot: k = band amount x amount (set of that channel), 20 ms linear
    Ramp bandOn[2][kMaxBands];
    Ramp solo[kMaxBands];
    Ramp link, soloTopology;

    // settle state machine
    enum class Settle { Idle, FadingOut, Holding, FadingIn };
    Settle settle = Settle::Idle;
    double settleValue = 1.0, settleOutStep = 0.0, settleInStep = 0.0;
    int settleOutLength = 1, settleInLength = 1, fadeRemaining = 0;
    int holdRemaining = 0, holdLength = 0;
    bool structurePending = false;

    int activeBands = 1;
    CrossoverType activeType = CrossoverType::Analog;
    ChannelMode activeMode = ChannelMode::Stereo;
    bool modeKnown = false;

    double envMs = -1.0;
    double ca = 0.0, cr = 0.0, cl = 0.0, cla = 0.0;
    double cPower = 0.0, cMakeup = 0.0, cMicro = 0.0;
    int autoCounter = 0, microCounter = 0;

    std::atomic<int> latency { 0 };
    BlockMeter blockMeter[kMaxBands];

    Impl()
    {
        for (int s = 0; s < kNumChannelSets; ++s)
            for (int b = 0; b < kMaxBands; ++b)
            {
                meters.gainMinDb[s][b].store (200.0f);
                meters.gainMaxDb[s][b].store (-200.0f);
            }
    }

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
        maxBlock = std::max (1, maxBlockSize);

        crossover.prepare (sr, maxBlock);
        bandBuffers.assign ((size_t) (kMaxBands * 2 * maxBlock), 0.0f);
        for (int b = 0; b < kMaxBands; ++b)
        {
            for (int c = 0; c < 2; ++c)
                bandPtr[b][c] = bandBuffers.data() + (size_t) ((b * 2 + c) * maxBlock);
            bandOut[b] = bandPtr[b];
        }
        sumBuffer.assign ((size_t) (2 * maxBlock), 0.0f);
        linkRamp.assign ((size_t) maxBlock, 0.0);
        settleRamp.assign ((size_t) maxBlock, 1.0);
        soloTopologyRamp.assign ((size_t) maxBlock, 1.0);
        holdMask.assign ((size_t) maxBlock, 0);
        envBuffer.assign ((size_t) (8 * maxBlock), 0.0);
        midBuffer.assign ((size_t) (3 * maxBlock), 0.0);

        for (auto& bs : bands)
        {
            bs.quad.prepare (sr, 20.0);
            bs.quadOut.prepare (sr, 20.0);
            bs.linkedMakeup = bs.linkedMakeupTarget = 1.0;
            for (int c = 0; c < 2; ++c)
            {
                bs.pin[c] = bs.pout[c] = 0.0;
                bs.makeup[c] = bs.makeupTarget[c] = 1.0;
            }
        }
        for (auto& row : amountK)
            for (auto& s : row)
                s.prepare (sr, 20.0);
        for (auto& row : bandOn)
            for (auto& r : row)
                r.prepare (sr, 20.0);
        for (auto& r : solo)
            r.prepare (sr, 10.0);
        link.prepare (sr, 20.0);
        soloTopology.prepare (sr, 10.0);

        settleOutLength = std::max (1, (int) std::lround (kSettleOutMs * 0.001 * sr));
        settleInLength = std::max (1, (int) std::lround (kSettleInMs * 0.001 * sr));
        settleInStep = 1.0 / (double) settleInLength;
        holdLength = std::max (1, (int) std::lround (kSettleHoldMs * 0.001 * sr));
        cPower = coeff (3000.0, sr);
        cMakeup = coeff (500.0, sr);
        cMicro = coeff (3000.0, sr / (double) kMicroInterval);
        envMs = -1.0;

        prepared = true;
        reset();
        latency.store (crossover.getLatencySamples (params.crossoverType), std::memory_order_relaxed);
    }

    void reset() noexcept
    {
        if (! prepared)
            return;
        crossover.reset();
        for (auto& bs : bands)
            bs.clearDetectors();
        std::fill (bandBuffers.begin(), bandBuffers.end(), 0.0f);
        autoCounter = microCounter = 0;
        soloTopology.setTarget (1.0);
        soloTopology.snap();
        startSettle (true);
        fresh = true;   // the next block snaps every ramp to its target
    }

    // Starts (or extends) the settle sequence. immediate: the effect is already off (fresh state).
    void startSettle (bool immediate) noexcept
    {
        if (immediate)
        {
            settleValue = 0.0;
            settle = Settle::Holding;
            holdRemaining = holdLength;
        }
        else if (settle == Settle::Idle || settle == Settle::FadingIn)
        {
            settle = Settle::FadingOut;
            fadeRemaining = std::max (1, (int) std::lround (settleValue * settleOutLength));
            settleOutStep = settleValue / (double) fadeRemaining;
        }
        else if (settle == Settle::Holding)
        {
            holdRemaining = holdLength;
        }
    }

    //==========================================================================
    void updateCoefficients (double bpm) noexcept
    {
        const double e = punchEnvelopeMs (params, bpm);
        if (e == envMs)
            return;
        envMs = e;
        cl = coeff (e * kLongRelease, sr);
        cla = coeff (std::min (e * kLongAttack, kLongAttackMaxMs), sr);
        cr = coeff (std::max (1.0, e * kShortRelease), sr);
        ca = coeff (std::clamp (e * kShortAttack, 0.2, 2.0), sr);
    }

    // set used by channel slot c in the given mode
    static int setFor (int c, ChannelMode mode) noexcept
    {
        return (mode == ChannelMode::MidSide || mode == ChannelMode::LeftRight) ? c : 0;
    }

    void updateTargets (ChannelMode mode, bool snap) noexcept
    {
        const auto& p = params;
        const double amount = std::isfinite (p.amountPercent) ? std::clamp ((double) p.amountPercent, 0.0, 100.0) * 0.01 : 0.0;
        for (int c = 0; c < 2; ++c)
        {
            const int set = setFor (c, mode);
            for (int b = 0; b < kMaxBands; ++b)
            {
                const auto& bp = p.bands[set][b];
                const double a = std::isfinite (bp.amountPercent) ? std::clamp ((double) bp.amountPercent, -100.0, 100.0) : 0.0;
                amountK[c][b].setTarget ((a < 0.0 ? kNegativeScale : 1.0) * a * 0.01 * amount);
                bandOn[c][b].setTarget (bp.on ? 1.0 : 0.0);
                if (snap)
                {
                    amountK[c][b].snap();
                    bandOn[c][b].snap();
                }
            }
        }

        const bool stereo = mode == ChannelMode::Stereo || mode == ChannelMode::TransientSustain;
        link.setTarget (stereo && p.stereoLink ? 1.0 : 0.0);

        updateSoloTargets();

        if (snap)
        {
            link.snap();
            for (auto& r : solo)
                r.snap();
        }
    }

    void updateSoloTargets() noexcept
    {
        const int s = monitor.soloBand;
        const bool soloOn = s >= 0 && s < activeBands;
        for (int b = 0; b < kMaxBands; ++b)
            solo[b].setTarget (! soloOn || b == s ? 1.0 : 0.0);

    }

    // Structural changes (band count, crossover type). Called at the start of every chunk.
    void updateStructure (bool snap) noexcept
    {
        const auto& p = params;
        const int wantBands = std::clamp (p.numBands, 1, kMaxBands);
        if (snap)
        {
            activeBands = wantBands;
            activeType = p.crossoverType;
        }
        else
        {
            if (p.crossoverType != activeType)
            {
                activeType = p.crossoverType;   // applies at once (the latency is reported from the switch)
                startSettle (false);
            }
            if (wantBands != activeBands)
            {
                startSettle (false);
                bool soloAffected = monitor.soloBand >= 0 && monitor.soloBand < std::max (activeBands, wantBands);
                for (int b = 0; b < activeBands; ++b)
                    soloAffected = soloAffected || solo[b].value != 1.0 || solo[b].target != 1.0;
                if (soloAffected)
                    soloTopology.setTarget (0.0);
                // A soloed band's membership can jump when bands merge. Dip that monitor
                // for 10 ms, including requests during the effect's hold or fade-in.
                if (settle == Settle::Holding && (soloTopology.target == 1.0 || soloTopology.remaining == 0))
                {
                    for (int b = activeBands; b < wantBands; ++b)
                        bands[b].clearDetectors();
                    activeBands = wantBands;
                    holdRemaining = holdLength;
                    updateSoloTargets();
                    // Inactive bands have not advanced their solo ramps. Set all new
                    // memberships while muted (or while every band is already audible).
                    for (auto& r : solo)
                        r.snap();
                    soloTopology.setTarget (1.0);
                }
            }
            else
                soloTopology.setTarget (1.0);   // also cancels a superseded request
        }
        structurePending = wantBands != activeBands;
        crossover.setup (activeBands, p.crossoverHz, activeType);
    }

    // Fills settleRamp / holdMask for n samples and advances the state machine.
    void runSettle (int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            unsigned char hold = 0;
            switch (settle)
            {
                case Settle::Idle: break;
                case Settle::FadingOut:
                    settleValue -= settleOutStep;
                    if (--fadeRemaining == 0)
                    {
                        settleValue = 0.0;
                        settle = Settle::Holding;
                        holdRemaining = holdLength;
                    }
                    break;
                case Settle::Holding:
                    hold = 1;
                    // a band-count change waiting for the crossover keeps the hold until it is applied
                    if (! structurePending && --holdRemaining <= 0)
                    {
                        settle = Settle::FadingIn;
                        fadeRemaining = settleInLength;
                    }
                    break;
                case Settle::FadingIn:
                    settleValue += settleInStep;
                    if (--fadeRemaining == 0)
                    {
                        settleValue = 1.0;
                        settle = Settle::Idle;
                    }
                    break;
            }
            settleRamp[(size_t) i] = settleValue;
            holdMask[(size_t) i] = hold;
        }
    }

    void updateMakeupTargets (BandState& bs, bool autoOn) noexcept
    {
        static constexpr double gLo = 0.25118864315095801, gHi = 3.9810717055349722;   // -12 / +12 dB
        if (! autoOn)
        {
            bs.makeupTarget[0] = bs.makeupTarget[1] = 1.0;
            bs.linkedMakeupTarget = 1.0;
        }
        else
        {
            const double pin = 0.5 * (bs.pin[0] + bs.pin[1]), pout = 0.5 * (bs.pout[0] + bs.pout[1]);
            if (pin >= 1.0e-7 && pout > 0.0)
                bs.linkedMakeupTarget = std::clamp (std::sqrt (pin / pout), gLo, gHi);
            for (int c = 0; c < 2; ++c)
                if (bs.pin[c] >= 1.0e-7 && bs.pout[c] > 0.0)
                    bs.makeupTarget[c] = std::clamp (std::sqrt (bs.pin[c] / bs.pout[c]), gLo, gHi);
        }
        for (int c = 0; c < 2; ++c)
            if (std::abs (bs.makeup[c] - bs.makeupTarget[c]) < 1.0e-9)
                bs.makeup[c] = bs.makeupTarget[c];
        if (std::abs (bs.linkedMakeup - bs.linkedMakeupTarget) < 1.0e-9)
            bs.linkedMakeup = bs.linkedMakeupTarget;
    }

    static double makeupDb (double m) noexcept { return m == 1.0 ? 0.0 : 20.0 * std::log10 (m); }
    static double toDb (double g) noexcept { return g == 1.0 ? 0.0 : 20.0 * std::log10 (std::max (g, 1.0e-10)); }

    //==========================================================================
    // One band over a chunk, in three passes so the long gain computer (division, ln, exp) has no loop-carried
    // dependency and pipelines: 1) detector + envelopes (recursive), 2) rel -> gain (independent per sample),
    // 3) make-up, output, output detector, auto gain and meters (recursive). The arithmetic is the same as one pass.
    void processBand (int b, int n, bool stereo, float* sum0, float* sum1) noexcept
    {
        auto& bs = bands[b];
        auto& k0s = amountK[0][b];
        auto& k1s = amountK[1][b];
        auto& on0 = bandOn[0][b];
        auto& on1 = bandOn[1][b];
        auto& so = solo[b];
        auto& bm = blockMeter[b];
        const float* x0p = bandPtr[b][0];
        const float* x1p = bandPtr[b][1];
        const bool autoOn = params.autoGain;
        double* envAs = envBuffer.data();                 // 2 per sample
        double* envAl = envBuffer.data() + 2 * maxBlock;
        double* pw = envBuffer.data() + 4 * maxBlock;     // detector power, 2 per sample
        double* linBuf = envBuffer.data() + 6 * maxBlock; // 2 per sample
        double* midAs = midBuffer.data();
        double* midAl = midBuffer.data() + maxBlock;
        double* midP = midBuffer.data() + 2 * maxBlock;

        const V2 vca = splat (ca), vcr = splat (cr), vcl = splat (cl), vcla = splat (cla);
        const V2 rmin = splat (kRatioMin), rmax = splat (kRatioMax), gmin = splat (kGainMin), gmax = splat (kGainMax), vks = splat (kRelScale), vfl = splat (kFloor);
        const V2 vOnset = splat (kOnset), vPow = splat (cPower), vMk = splat (cMakeup), one = splat (1.0);
        const V2 toDb = splat (kPowToDb), toLn = splat (kDbToLn), zero = splat (0.0);

        // 1) detector and input envelopes
        {
            V2 as = load (bs.as), al = load (bs.al);
            for (int i = 0; i < n; ++i)
            {
                const V2 x = v2 ((double) x0p[i], (double) x1p[i]);
                V2 qa, qb;
                quadPair (bs.quad, x, qa, qb);
                const V2 p = vmax (qa * qa + qb * qb, vOnset * x * x);
                as = as + greaterSelect (p, as, vca, vcr) * (p - as);
                al = al + greaterSelect (p, al, vcla, vcl) * (p - al);
                if (stereo)
                {
                    const double am = 0.5 * (lane0 (qa) + lane1 (qa)), bmid = 0.5 * (lane0 (qb) + lane1 (qb));
                    const double xm = 0.5 * (lane0 (x) + lane1 (x));
                    const double pm = std::max (am * am + bmid * bmid, kOnset * xm * xm);
                    bs.midIn.run (pm, ca, cr, cl, cla);
                    midP[i] = pm;
                }
                if (holdMask[(size_t) i] != 0)
                {
                    al = as;
                    bs.midIn.al = bs.midIn.as;
                }
                store (envAs + 2 * i, as);
                store (envAl + 2 * i, al);
                store (pw + 2 * i, p);
                midAs[i] = bs.midIn.as;
                midAl[i] = bs.midIn.al;
            }
            store (bs.as, as);
            store (bs.al, al);
        }

        // 2) gain computer
        {
            int mc = microCounter;
            for (int i = 0; i < n; ++i)
            {
                const V2 k = v2 (k0s.next(), k1s.next());
                const double w = linkRamp[(size_t) i];
                const bool microTick = mc + 1 >= kMicroInterval;
                mc = microTick ? 0 : mc + 1;
                V2 g = zero, rel = zero;
                if (w < 1.0 || microTick)
                {
                    rel = toDb * fastLn (vclamp ((load (envAs + 2 * i) + vfl) / (load (envAl + 2 * i) + vfl), rmin, rmax));
                    g = vclamp (k * rel * vks, gmin, gmax);
                }
                double relM = 0.0;
                if (w > 0.0 || (stereo && microTick))
                {
                    relM = relDb (midAs[i], midAl[i]);
                    if (w > 0.0)
                    {
                        const double gm = std::clamp (lane0 (k) * relM * kRelScale, kGainMin, kGainMax);
                        g = g + splat (w) * (splat (gm) - g);
                    }
                }
                store (linBuf + 2 * i, fastExp (g * toLn));
                if (microTick)
                {
                    bs.microIn[0].push (lane0 (rel), cMicro);
                    bs.microIn[1].push (lane1 (rel), cMicro);
                    if (stereo)
                        bs.midMicroIn.push (relM, cMicro);
                }
            }
        }

        // 3) make-up, output, auto gain, meters
        double* yBuf = pw;   // the detector power is not needed any more: holds the band output for pass 4
        {
            int ac = autoCounter;
            V2 pin = load (bs.pin), pout = load (bs.pout), mk = load (bs.makeup), mkT = load (bs.makeupTarget);
            V2 mMin = load (bm.gMin), mMax = load (bm.gMax), mLast = load (bm.last);
            for (int i = 0; i < n; ++i)
            {
                const V2 x = v2 ((double) x0p[i], (double) x1p[i]);
                const V2 lin = load (linBuf + 2 * i);
                const double w = linkRamp[(size_t) i];

                mk = mk + vMk * (mkT - mk);
                bs.linkedMakeup += cMakeup * (bs.linkedMakeupTarget - bs.linkedMakeup);
                // Keep the independent and common correction warm. Stereo Link must
                // crossfade the complete gain, including previously learned make-up.
                const V2 appliedMakeup = mk + splat (w) * (splat (bs.linkedMakeup) - mk);
                const V2 wet = v2 (on0.next(), on1.next()) * splat (settleRamp[(size_t) i]);
                const double sw = so.next() * soloTopologyRamp[(size_t) i];
                const V2 appliedGain = one + wet * (lin * appliedMakeup - one);
                const V2 y = x * appliedGain;
                const V2 yn = x * (one + wet * (lin - one));
                store (yBuf + 2 * i, y);

                pin = pin + vPow * (x * x - pin);
                pout = pout + vPow * (yn * yn - pout);

                sum0[i] += (float) (sw * lane0 (y));
                sum1[i] += (float) (sw * lane1 (y));

                // gain meters in the linear domain (dB at publish time: the log is monotonic, extremes are exact)
                mMin = vmin (mMin, appliedGain);
                mMax = vmax (mMax, appliedGain);
                mLast = appliedGain;

                if (++ac >= kAutoGainInterval)
                {
                    ac = 0;
                    store (bs.pin, pin);
                    store (bs.pout, pout);
                    store (bs.makeup, mk);
                    updateMakeupTargets (bs, autoOn);
                    mk = load (bs.makeup);
                    mkT = load (bs.makeupTarget);
                }
            }
            store (bs.pin, pin);
            store (bs.pout, pout);
            store (bs.makeup, mk);
            store (bs.makeupTarget, mkT);
            store (bm.gMin, mMin);
            store (bm.gMax, mMax);
            store (bm.last, mLast);
            bm.any = bm.any || n > 0;
            if (b == activeBands - 1)
                autoCounter = ac;
        }

        // 4) output side of the micro meter: its own detector on the rendered band (the gain moves within the
        //    detector's group delay at fast settings, so "input power x gain^2" would misread it by up to 1 dB)
        {
            int mc = microCounter;
            V2 aso = load (bs.asOut), alo = load (bs.alOut);
            for (int i = 0; i < n; ++i)
            {
                const V2 y = load (yBuf + 2 * i);
                V2 qoa, qob;
                quadPair (bs.quadOut, y, qoa, qob);
                const V2 po = vmax (qoa * qoa + qob * qob, vOnset * y * y);
                aso = aso + greaterSelect (po, aso, vca, vcr) * (po - aso);
                alo = alo + greaterSelect (po, alo, vcla, vcl) * (po - alo);
                if (stereo)
                {
                    const double am = 0.5 * (lane0 (qoa) + lane1 (qoa)), bmid = 0.5 * (lane0 (qob) + lane1 (qob));
                    const double ym = 0.5 * (lane0 (y) + lane1 (y));
                    bs.midOut.run (std::max (am * am + bmid * bmid, kOnset * ym * ym), ca, cr, cl, cla);
                }
                if (holdMask[(size_t) i] != 0)
                {
                    alo = aso;
                    bs.midOut.al = bs.midOut.as;
                }

                if (mc + 1 >= kMicroInterval)
                {
                    mc = 0;
                    const V2 relO = toDb * fastLn (vclamp ((aso + vfl) / (alo + vfl), rmin, rmax));
                    bs.microOut[0].push (lane0 (relO), cMicro);
                    bs.microOut[1].push (lane1 (relO), cMicro);
                    if (stereo)
                        bs.midMicroOut.push (relDb (bs.midOut.as, bs.midOut.al), cMicro);
                }
                else
                {
                    ++mc;
                }
            }
            store (bs.asOut, aso);
            store (bs.alOut, alo);
            if (b == activeBands - 1)
                microCounter = mc;
        }
    }

    void processChunk (float* const* io, int n, bool stereo) noexcept
    {
        const float* in[2] { io[0], io[1] };
        crossover.process (in, bandOut, n);

        runSettle (n);
        for (int i = 0; i < n; ++i)
        {
            linkRamp[(size_t) i] = link.next();
            soloTopologyRamp[(size_t) i] = soloTopology.next();
        }

        float* sum0 = sumBuffer.data();
        float* sum1 = sumBuffer.data() + maxBlock;
        std::fill (sum0, sum0 + n, 0.0f);
        std::fill (sum1, sum1 + n, 0.0f);

        for (int b = 0; b < activeBands; ++b)
            processBand (b, n, stereo, sum0, sum1);

        std::memcpy (io[0], sum0, sizeof (float) * (size_t) n);
        std::memcpy (io[1], sum1, sizeof (float) * (size_t) n);
    }

    //==========================================================================
    void publishMeters (bool stereo) noexcept
    {
        const bool linked = stereo && link.target >= 0.5;
        for (int b = 0; b < kMaxBands; ++b)
        {
            auto& bm = blockMeter[b];
            const auto& bs = bands[b];
            auto publish = [this, b] (int set, double last, double lo, double hi, double mkDb, double microIn,
                                      double microOut)
            {
                auto& mt = meters;
                mt.gainDb[set][b].store ((float) last);
                if (lo <= hi)
                {
                    // Publish unconditionally: a UI reset after the load must not
                    // discard this block when its minimum exceeds the old one.
                    mt.gainMinDb[set][b].store (std::min ((float) lo, mt.gainMinDb[set][b].load()));
                    mt.gainMaxDb[set][b].storeMax ((float) hi);
                }
                mt.makeupDb[set][b].store ((float) mkDb);
                mt.microInDb[set][b].store ((float) microIn);
                mt.microOutDb[set][b].store ((float) microOut);
            };

            if (b >= activeBands || ! bm.any)
            {
                for (int set = 0; set < kNumChannelSets; ++set)
                    publish (set, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0);
            }
            else if (! stereo)
            {
                for (int c = 0; c < 2; ++c)
                    publish (c, toDb (bm.last[c]), toDb (bm.gMin[c]), toDb (bm.gMax[c]), makeupDb (bs.makeup[c]),
                             bs.microIn[c].stdDev(), bs.microOut[c].stdDev());
            }
            else
            {
                const double last0 = toDb (bm.last[0]), last1 = toDb (bm.last[1]);
                const double last = std::abs (last1) > std::abs (last0) ? last1 : last0;
                const double lo = toDb (std::min (bm.gMin[0], bm.gMin[1]));
                const double hi = toDb (std::max (bm.gMax[0], bm.gMax[1]));
                const double mk0 = makeupDb (bs.makeup[0] + link.value * (bs.linkedMakeup - bs.makeup[0]));
                const double mk1 = makeupDb (bs.makeup[1] + link.value * (bs.linkedMakeup - bs.makeup[1]));
                const double mk = std::abs (mk1) > std::abs (mk0) ? mk1 : mk0;
                double mi, mo;
                if (linked)
                {
                    mi = bs.midMicroIn.stdDev();
                    mo = bs.midMicroOut.stdDev();
                }
                else
                {
                    mi = std::max (bs.microIn[0].stdDev(), bs.microIn[1].stdDev());
                    mo = std::max (bs.microOut[0].stdDev(), bs.microOut[1].stdDev());
                }
                for (int set = 0; set < kNumChannelSets; ++set)
                    publish (set, last, lo, hi, mk, mi, mo);
            }
            bm.clear();
        }
    }

    //==========================================================================
    void process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
    {
        if (! prepared || numSamples <= 0 || channels == nullptr || channels[0] == nullptr || channels[1] == nullptr)
            return;

        ScopedFlushDenormals noDenormals;

        const ChannelMode mode = ctx.channelMode;
        const bool stereo = mode == ChannelMode::Stereo || mode == ChannelMode::TransientSustain;
        if (modeKnown && mode != activeMode)
            startSettle (false);   // the chain resets us on a mode change; this covers a direct switch too
        activeMode = mode;
        modeKnown = true;

        updateCoefficients (ctx.bpm);
        if (fresh)
        {
            updateStructure (true);
            updateTargets (mode, true);
            fresh = false;
        }
        else
        {
            updateTargets (mode, false);
        }

        for (int start = 0; start < numSamples;)
        {
            // Apply topology at a sample boundary, independent of host block size.
            updateStructure (false);
            int n = std::min (maxBlock, numSamples - start);
            if (structurePending)
            {
                if (settle == Settle::FadingOut)
                    n = std::min (n, fadeRemaining);
                if (soloTopology.target == 0.0 && soloTopology.remaining > 0)
                    n = std::min (n, soloTopology.remaining);
            }
            float* io[2] { channels[0] + start, channels[1] + start };
            processChunk (io, n, stereo);
            start += n;
        }

        publishMeters (stereo);
    }
};

//==============================================================================
Punch::Punch() : impl (std::make_unique<Impl>()) {}
Punch::~Punch() = default;

void Punch::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }
void Punch::reset() noexcept { impl->reset(); }

void Punch::setParams (ParamValues values) noexcept
{
    auto& p = impl->params;
    readPunchParams (values, p);
    if (getParamSet() == 1)   // T/S rule (Module.h): the twin reads set B for both channels
        for (int b = 0; b < kMaxBands; ++b)
            p.bands[0][b] = p.bands[1][b];
    impl->latency.store (impl->prepared ? impl->crossover.getLatencySamples (p.crossoverType) : 0,
                         std::memory_order_relaxed);
}

void Punch::setMonitor (const ModuleMonitor& m) noexcept { impl->monitor = m; }

void Punch::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    impl->process (channels, numSamples, ctx);
}

int Punch::getLatencySamples() const noexcept { return impl->latency.load (std::memory_order_relaxed); }
int Punch::getLatencySamplesForParams (ParamValues values) const noexcept
{
    PunchParams params;
    readPunchParams (values, params);
    return impl->prepared ? impl->crossover.getLatencySamples (params.crossoverType) : getLatencySamples();
}
const PunchMeters& Punch::getMeters() const noexcept { return impl->meters; }
PunchMeters& Punch::getMeters() noexcept { return impl->meters; }
} // namespace ps
