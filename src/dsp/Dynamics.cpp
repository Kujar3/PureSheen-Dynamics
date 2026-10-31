#include "dsp/Dynamics.h"

#include "dsp/Crossover.h"
#include "dsp/Envelope.h"
#include "dsp/Filters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define PS_DYN_HAS_SSE_CSR 1
#endif

// Multiband Dynamics (dsp-dynamics). See Dynamics.h for the contract. Signal flow per block:
//
//   audio path    : in -> Crossover (Analog / Digital / Hybrid, 1..4 bands) -> lookahead -> * gain
//   detector path : in -> detection filter (None / High Pass / Tilt) -> identical Crossover
//      -> per band, per channel:
//                           -> LevelDetector (Peak / Env / RMS)
//                           [Stereo: max (|L|, |R|) of the filtered band, one detector + one gain for both channels]
//                           -> compressor curve -> compressor ballistics -> limiter curve (input = detector level +
//                           compressor gain) -> limiter ballistics
//      -> sum of bands -> output gain
//
// Choices (where the research leaves things open), documented for the lead / the UI:
//   - The detector has its own crossover after the input detection filter, as required by Dynamics.h. Filtering
//     after the split only commutes for fixed settings, and would move automation ahead of Digital's latency.
//     Structure changes (filter type, HP slope) crossfade the detector signal over 20 ms between two filter slots;
//     frequency / tilt moves ramp the coefficients.
//   - All three LevelDetector modes run continuously; mode changes blend their readings over 10 ms. Linked (max),
//     independent left and independent right gain paths stay warm; linking blends both applied gains over 10 ms.
//   - Layout changes are serialized over 20 ms. New bands inherit the parent's detector and gain history and blend
//     from its applied gain; empty aligned lookahead buffers complement the parent's unsplit history. Removing bands
//     first converges their gains and monitor weights on the surviving band, then merges the queued delay samples.
//   - High Pass: Butterworth of order 1 / 2 / 4 / 8 (6 / 12 / 24 / 48 dB per octave) at detFreq (matched biquads).
//   - Tilt: a cascade of ten first-order shelf sections, one per octave from 20 Hz to ~20 kHz, giving a constant
//     detTilt dB per octave across the audio band (flat outside it), normalised to 0 dB at detFreq (the pivot).
//   - Ballistics run in double precision on the gain in dB (one-pole, time constant = time to reach 1 - 1/e, as
//     onePoleCoeff). "Attack" is the reaction to a rising detector level: for downward and upward compression and for
//     the downward limiter that is a falling gain; for the limiter's upward expansion (negative ratio) a rising gain.
//     Gains within 1e-9 dB of their target snap to it, so even 5 s releases settle exactly (0 dB = bit-exact unity).
//   - Adaptive Release scales the set release by 0.2x (transients) .. 2x (sustained material) from the crest of the
//     detector: peak envelope (instant attack, falls 20 dB/s) minus a slow power average (300 ms, in dB). After a
//     transient the peak envelope stays far above the average while the gain recovers (fast release); after sustained
//     material both fall together (slow release).
//     crest <= 3 dB -> 2x, >= 15 dB -> 0.2x, exponential in between. It applies to compressor and limiter.
//   - Limiter: a second, fast gain stage after the compressor (high ratio, finite attack): not a brickwall; with a
//     ratio of 30:1 and a short attack peaks may overshoot the threshold slightly (as in the reference product).
//   - Band "On" off bypasses the whole band (compressor, limiter, mix, band gain). Comp On / Limiter On fade their
//     stage's gain in and out (10 ms) while the detectors keep running.
//   - Parallel: band out = (dry * (1 - mix) + wet * mix) * band gain, wet = dry * compressor * limiter * makeup.
//   - Auto Gain (per set and band, static: recomputed only when a curve, the band layout or the detection mode
//     changes, never from the signal):
//         band edges lo / hi = 20 Hz / 20 kHz or the neighbouring crossovers, phi = log2 (hi / lo) / log2 (1000)
//         (the share of a pink spectrum's power between 20 Hz and 20 kHz in that band, at least 0.01)
//         Lref = -18 dBFS + 10 log10 (phi) + (Peak / Envelope detection: +3 dB, RMS: 0 dB)
//         levels L_k = Lref + d_k, d_k = -12 .. +12 dB in 0.5 dB steps, weights w_k = exp (-d_k^2 / (2 * 4^2))
//         g(L) = comp.gainDb (L) [comp on] + lim.gainDb (L + comp.gainDb (L)) [limiter on]
//         makeup = clamp (- sum (w_k g (L_k)) / sum (w_k), -24, +24) dB (0 when the band is off or Auto Gain is off)
//     The makeup is part of the wet path (it compensates the compression, so the dry part of a parallel mix is not
//     raised) and glides over 50 ms.
//   - Smoothing: thresholds, ratios, knees 20 ms linear; mix, band gain, output gain 20 ms; on / off fades, band solo
//     and detector solo 10 ms; lookahead changes crossfade the old and new delay taps over 10 ms. The first block
//     after prepare() / reset() applies every value directly.
//   - Detector solo (ModuleMonitor::soloDetectionFilter): the output is the detector signal (filtered bands, per
//     channel, delayed like the audio so the latency is unchanged); combined with band solo it is that band's
//     detector signal.
//   - Meters (per block): the gain with the largest magnitude, the detector peak. In Stereo, set B mirrors set A.
namespace ps
{
namespace
{
constexpr double kDbToLn = 0.11512925464970228;   // ln (10) / 20
constexpr int kTiltSections = 10;                 // first-order sections (5 biquads)
constexpr int kMaxSections = 5;                   // biquads per detection filter (HP 48 dB: 4, Tilt: 5)
constexpr int kAutoGainPoints = 49;               // -12 .. +12 dB in 0.5 dB steps

double coeffFor (double ms, double sr) noexcept
{
    const double samples = ms * 0.001 * sr;
    return samples > 0.0 ? std::exp (-1.0 / samples) : 0.0;
}

#if PS_DYN_HAS_SSE_CSR
struct ScopedFlushDenormals
{
    ScopedFlushDenormals() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }   // FTZ | DAZ
    ~ScopedFlushDenormals() { _mm_setcsr (saved); }
    unsigned int saved;
};
#else
struct ScopedFlushDenormals {};
#endif

//==============================================================================
// Gain ballistics in dB (double precision), with optional adaptive release.
class Ballistics
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        fastDecay = 20.0 / sr;
        slowCoeff = coeffFor (300.0, sr);
        attMs = relMs = -1.0;
        setTimes (10.0f, 100.0f, false);
        reset();
    }

    void setTimes (float attackMs, float releaseMs, bool adaptiveRelease) noexcept
    {
        if ((double) attackMs != attMs)
        {
            attMs = attackMs;
            att = coeffFor (attMs, sr);
        }
        if ((double) releaseMs != relMs || adaptiveRelease != adaptive)
        {
            relMs = releaseMs;
            adaptive = adaptiveRelease;
            if (! adaptive)
                scale = 1.0;
            rel = coeffFor (relMs * scale, sr);
        }
    }

    void reset() noexcept
    {
        state = 0.0;
        fast = -120.0;
        slowPower = 0.0;
        scale = 1.0;
        counter = 0;
        rel = coeffFor (relMs * scale, sr);
    }

    // attackRises: true when a rising gain is the reaction to a rising level (upward expansion).
    double process (double target, float detectorDb, bool attackRises) noexcept
    {
        if (adaptive)
        {
            const double d = std::max ((double) detectorDb, -120.0);
            fast = std::max (d, fast - fastDecay);
            const double p = std::exp (d * 2.0 * kDbToLn);     // power
            slowPower = p + slowCoeff * (slowPower - p);

            if (++counter >= 16)
            {
                counter = 0;
                const double slowDb = slowPower > 1.0e-12 ? 10.0 * std::log10 (slowPower) : -120.0;
                const double crest = std::clamp (fast - slowDb, 3.0, 15.0);
                scale = 2.0 * std::pow (10.0, -(crest - 3.0) / 12.0);   // 2 .. 0.2
                rel = coeffFor (relMs * scale, sr);
            }
        }

        const bool attacking = attackRises ? target > state : target < state;
        const double c = attacking ? att : rel;
        state = target + c * (state - target);
        if (std::abs (state - target) < 1.0e-9)
            state = target;
        return state;
    }

    double getState() const noexcept { return state; }

private:
    double sr = 48000.0;
    double att = 0.0, rel = 0.0, state = 0.0;
    double attMs = -1.0, relMs = -1.0;
    bool adaptive = false;
    double scale = 1.0, fast = -120.0, slowPower = 0.0, fastDecay = 0.00625, slowCoeff = 0.0;
    int counter = 0;
};

//==============================================================================
// Detection filter design (shared by every band and channel).
struct FilterDesign
{
    int structure = 0;   // 0 none, 1..4 HP order index + 1, 5 tilt
    int count = 0;       // biquads
    BiquadCoeffs c[kMaxSections];
};

BiquadCoeffs firstOrderPair (double sr, double z1, double p1, double z2, double p2) noexcept
{
    // product of two bilinear first-order sections (s + wz) / (s + wp), corners prewarped
    const double K = 2.0 * sr;
    auto warp = [sr] (double hz) { return 2.0 * sr * std::tan (3.14159265358979323846 * std::min (hz, 0.45 * sr) / sr); };
    const double wz1 = warp (z1), wp1 = warp (p1), wz2 = warp (z2), wp2 = warp (p2);
    // section i: (K + wz) + (wz - K) z^-1  over  (K + wp) + (wp - K) z^-1
    const double n10 = K + wz1, n11 = wz1 - K, d10 = K + wp1, d11 = wp1 - K;
    const double n20 = K + wz2, n21 = wz2 - K, d20 = K + wp2, d21 = wp2 - K;
    const double a0 = d10 * d20;
    BiquadCoeffs c;
    c.b0 = n10 * n20 / a0;
    c.b1 = (n10 * n21 + n11 * n20) / a0;
    c.b2 = n11 * n21 / a0;
    c.a1 = (d10 * d21 + d11 * d20) / a0;
    c.a2 = d11 * d21 / a0;
    return c;
}

void designDetectionFilter (FilterDesign& f, double sr, DetectionFilter type, float hz, int slopeIndex, float tiltDb) noexcept
{
    f.count = 0;
    f.structure = 0;
    if (type == DetectionFilter::HighPass)
    {
        static constexpr int orders[4] { 1, 2, 4, 8 };
        const int order = orders[std::clamp (slopeIndex, 0, 3)];
        f.structure = 1 + std::clamp (slopeIndex, 0, 3);
        if (order == 1)
        {
            f.c[0] = design::highPass1 (sr, hz);
            f.count = 1;
        }
        else
        {
            f.count = order / 2;
            for (int k = 0; k < f.count; ++k)
                f.c[k] = design::highPass (sr, hz, design::butterworthQ (order, k));
        }
    }
    else if (type == DetectionFilter::Tilt)
    {
        f.structure = 5;
        f.count = kTiltSections / 2;
        const double r = std::pow (2.0, std::abs ((double) tiltDb) / 6.0);   // corner spacing inside a section
        double corner[kTiltSections][2];
        for (int k = 0; k < kTiltSections; ++k)
        {
            const double lo = 20.0 * std::pow (2.0, (double) k), hi = lo * r;
            corner[k][0] = tiltDb >= 0.0f ? lo : hi;   // zero
            corner[k][1] = tiltDb >= 0.0f ? hi : lo;   // pole
        }
        for (int s = 0; s < f.count; ++s)
            f.c[s] = firstOrderPair (sr, corner[2 * s][0], corner[2 * s][1], corner[2 * s + 1][0], corner[2 * s + 1][1]);

        // normalise to 0 dB at the pivot
        std::complex<double> h (1.0, 0.0);
        for (int s = 0; s < f.count; ++s)
            h *= f.c[s].response (hz, sr);
        const double m = std::abs (h);
        if (m > 1.0e-9)
        {
            f.c[0].b0 /= m;
            f.c[0].b1 /= m;
            f.c[0].b2 /= m;
        }
    }
}

bool sameCoeffs (const FilterDesign& a, const FilterDesign& b) noexcept
{
    if (a.structure != b.structure || a.count != b.count)
        return false;
    for (int k = 0; k < a.count; ++k)
        if (a.c[k].b0 != b.c[k].b0 || a.c[k].b1 != b.c[k].b1 || a.c[k].b2 != b.c[k].b2 || a.c[k].a1 != b.c[k].a1
            || a.c[k].a2 != b.c[k].a2)
            return false;
    return true;
}

// One band / channel detection filter: two slots so a structure change crossfades.
class DetectionFilterRunner
{
public:
    void prepare (int crossfadeSamples) noexcept
    {
        fadeLength = std::max (1, crossfadeSamples);
        for (auto& s : slots)
            for (auto& b : s.bq)
                b.setRampLength (256);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : slots)
            for (auto& b : s.bq)
                b.reset();
        fadeRemaining = 0;
    }

    // Applies the design: snaps (fresh), ramps (same structure) or starts a crossfade (structure change; deferred
    // while a crossfade runs).
    void apply (const FilterDesign& d, bool snap) noexcept
    {
        auto& cur = slots[active];
        if (snap)
        {
            load (cur, d, true);
            fadeRemaining = 0;
            return;
        }
        if (d.structure == cur.structure)
        {
            if (! sameCoeffs (d, cur.design))
                load (cur, d, false);
            return;
        }
        if (fadeRemaining > 0)
            return;   // next block
        auto& next = slots[1 - active];
        load (next, d, true);
        for (auto& b : next.bq)
            b.reset();
        active = 1 - active;
        fadeRemaining = fadeLength;
    }

    bool isIdentity() const noexcept { return fadeRemaining == 0 && slots[active].count == 0; }

    inline float process (float x) noexcept
    {
        const double y = run (slots[active], x);
        if (fadeRemaining == 0)
            return (float) y;
        const double old = run (slots[1 - active], x);
        const double a = 1.0 - (double) fadeRemaining / (double) fadeLength;
        --fadeRemaining;
        return (float) (old + a * (y - old));
    }

private:
    struct Slot
    {
        int structure = 0, count = 0;
        FilterDesign design;
        SmoothedBiquad bq[kMaxSections];
    };

    static void load (Slot& s, const FilterDesign& d, bool snap) noexcept
    {
        s.structure = d.structure;
        s.count = d.count;
        s.design = d;
        for (int k = 0; k < d.count; ++k)
        {
            s.bq[k].setTarget (d.c[k]);
            if (snap)
                s.bq[k].snapToTarget();
        }
    }

    static inline double run (Slot& s, float x) noexcept
    {
        double y = (double) x;
        for (int k = 0; k < s.count; ++k)
            y = s.bq[k].processSample (y);
        return y;
    }

    Slot slots[2];
    int active = 0, fadeRemaining = 0, fadeLength = 960;
};

//==============================================================================
// Lookahead delay with a crossfade between the old and new tap when the delay changes.
class LookaheadDelay
{
public:
    void prepare (int maxDelay, int crossfadeSamples)
    {
        int size = 1;
        while (size < maxDelay + 1)
            size <<= 1;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        fadeLength = std::max (1, crossfadeSamples);
        reset();
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        fadeRemaining = 0;
    }

    void snapDelay (int d) noexcept
    {
        delay = target = std::clamp (d, 0, mask);
        fadeRemaining = 0;
    }

    // call once per block: starts a crossfade toward the latest target when none is running
    void beginBlock (int d) noexcept
    {
        target = std::clamp (d, 0, mask);
        if (fadeRemaining == 0 && target != delay)
        {
            oldDelay = delay;
            delay = target;
            fadeRemaining = fadeLength;
        }
    }

    inline float process (float x) noexcept
    {
        buffer[(size_t) writePos] = x;
        float y = buffer[(size_t) ((writePos - delay) & mask)];
        if (fadeRemaining > 0)
        {
            const float old = buffer[(size_t) ((writePos - oldDelay) & mask)];
            const float a = 1.0f - (float) fadeRemaining / (float) fadeLength;
            y = old + a * (y - old);
            --fadeRemaining;
        }
        writePos = (writePos + 1) & mask;
        return y;
    }

    // A newly split band starts with empty history at the parent's timeline and taps.
    void startSplit (const LookaheadDelay& parent) noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = parent.writePos;
        delay = parent.delay;
        target = parent.target;
        oldDelay = parent.oldDelay;
        fadeRemaining = parent.fadeRemaining;
    }

    // Preserve samples already queued by bands that are being merged. No allocation.
    void merge (const LookaheadDelay& other) noexcept
    {
        for (int age = 1; age <= mask + 1; ++age)
            buffer[(size_t) ((writePos - age) & mask)] += other.buffer[(size_t) ((other.writePos - age) & mask)];
    }

    int firstTap() const noexcept { return fadeRemaining > 0 ? std::min (delay, oldDelay) : delay; }

private:
    std::vector<float> buffer;
    int mask = 0, writePos = 0, delay = 0, target = 0, oldDelay = 0, fadeRemaining = 0, fadeLength = 480;
};

//==============================================================================
// Linear parameter ramp (per-block targets, per-sample values); ends exactly on the target.
class Ramp
{
public:
    void prepare (double sampleRate, double rampMs) noexcept
    {
        length = std::max (1, (int) std::lround (sampleRate * rampMs * 0.001));
    }

    void setTarget (float v) noexcept
    {
        if (v == target)
            return;
        target = v;
        remaining = length;
        step = (target - current) / (float) length;
    }

    void snap() noexcept
    {
        current = target;
        step = 0.0f;
        remaining = 0;
    }

    inline float next() noexcept
    {
        if (remaining > 0)
        {
            if (--remaining == 0)
                current = target;
            else
                current += step;
        }
        return current;
    }

    float getCurrent() const noexcept { return current; }
    bool isSmoothing() const noexcept { return remaining > 0; }

private:
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0, length = 1;
};

//==============================================================================
// Keep every detector warm; mode changes blend their readings without resetting history.
class DetectorBank
{
public:
    void prepare (double sr) noexcept
    {
        for (int k = 0; k < 3; ++k)
        {
            detectors[k].prepare (sr);
            detectors[k].setMode ((DetectionMode) k);
            weights[k].prepare (sr, 10.0);
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& d : detectors)
            d.reset();
    }

    void setMode (DetectionMode mode, bool snap) noexcept
    {
        for (int k = 0; k < 3; ++k)
        {
            weights[k].setTarget (k == (int) mode ? 1.0f : 0.0f);
            if (snap)
                weights[k].snap();
        }
    }

    float processDb (float x) noexcept
    {
        float level = 0.0f;
        for (int k = 0; k < 3; ++k)
            level += weights[k].next() * detectors[k].processDb (x);
        return level;
    }

    void setPeakHold (double timeMs) noexcept { detectors[(int) DetectionMode::Peak].setPeakHold (timeMs); }

private:
    LevelDetector detectors[3];
    Ramp weights[3];
};

struct SetBandSmoothers
{
    Ramp compThr, compRatio, compKnee, compOn, limThr, limRatio, limKnee, limOn, bandOn, mix, gain, makeup;

    void snapAll() noexcept
    {
        for (auto* s : { &compThr, &compRatio, &compKnee, &compOn, &limThr, &limRatio, &limKnee, &limOn, &bandOn, &mix,
                         &gain, &makeup })
            s->snap();
    }
};

struct GainState
{
    DetectorBank detector;
    Ballistics comp, lim;

    // Ballistics and the Peak detector's hold: the hold (30 % of the release of the stage that leads, at most 30 ms)
    // is taken out of that stage's release, so a step still releases to 1/e in the set time.
    void setTimes (const DynamicsBandParams& bp, bool adaptive) noexcept
    {
        const float leadRelease = bp.comp.on || ! bp.lim.on ? bp.comp.releaseMs : bp.lim.releaseMs;
        const float hold = std::min (0.3f * leadRelease, (float) LevelDetector::kMaxPeakHoldMs);
        detector.setPeakHold (hold);
        auto rest = [hold] (float release) { return std::max (0.1f * release, release - hold); };
        comp.setTimes (bp.comp.attackMs, rest (bp.comp.releaseMs), adaptive);
        lim.setTimes (bp.lim.attackMs, rest (bp.lim.releaseMs), adaptive);
    }

    void prepare (double sr) noexcept
    {
        detector.prepare (sr);
        comp.prepare (sr);
        lim.prepare (sr);
    }

    void reset() noexcept
    {
        detector.reset();
        comp.reset();
        lim.reset();
    }
};

struct ChannelBandState : GainState
{
    LookaheadDelay audioDelay, detectorDelay;
};

struct BandControls
{
    GainCurve comp, lim;
    float compOn, limOn, bandOn, mix, gain, makeup;
};

BandControls nextControls (SetBandSmoothers& sb) noexcept
{
    return { { sb.compThr.next(), sb.compRatio.next(), sb.compKnee.next(), false },
             { sb.limThr.next(), sb.limRatio.next(), sb.limKnee.next(), true },
             sb.compOn.next(), sb.limOn.next(), sb.bandOn.next(), sb.mix.next(), sb.gain.next(), sb.makeup.next() };
}

struct MeterAccumulator
{
    float total = 0.0f, comp = 0.0f, lim = 0.0f, det = -200.0f;
    void clear() noexcept { total = comp = lim = 0.0f; det = -200.0f; }
};

inline void keepLargest (float& slot, float v) noexcept
{
    if (std::abs (v) > std::abs (slot))
        slot = v;
}

// Values that decide the auto-gain makeup of one set / band.
struct AutoGainKey
{
    float compThr, compRatio, compKnee, limThr, limRatio, limKnee, lo, hi;
    bool compOn, limOn, bandOn, autoGain;
    int detection;

    bool operator== (const AutoGainKey& o) const noexcept
    {
        return compThr == o.compThr && compRatio == o.compRatio && compKnee == o.compKnee && limThr == o.limThr
               && limRatio == o.limRatio && limKnee == o.limKnee && lo == o.lo && hi == o.hi && compOn == o.compOn
               && limOn == o.limOn && bandOn == o.bandOn && autoGain == o.autoGain && detection == o.detection;
    }
};
} // namespace

//==============================================================================
struct Dynamics::Impl
{
    DynamicsMeters meters;

    double sr = 48000.0;
    int maxBlock = 512;
    bool prepared = false;
    bool fresh = true;

    DynamicsParams params;
    ModuleMonitor monitor;

    Crossover crossover, detectorCrossover;
    std::vector<float> bandBuffers;            // kMaxBands x 2 x maxBlock
    float* bandPtr[kMaxBands][2] {};
    float* const* bandOut[kMaxBands] {};
    std::vector<float> detectorBuffers, filteredInput;
    float* detectorPtr[kMaxBands][2] {};
    float* const* detectorOut[kMaxBands] {};
    DetectionFilterRunner inputFilter[2];

    ChannelBandState chan[2][kMaxBands];
    GainState linkedState[kMaxBands];
    SetBandSmoothers sets[kNumChannelSets][kMaxBands];
    Ramp soloWeight[kMaxBands];
    Ramp detectorSolo, outputGain, linkWeight;
    std::vector<float> detectorSoloRamp, outputGainRamp, linkRamp;   // per block
    Ramp topologyMix;
    std::vector<float> topologyRamp, parentWeight;
    std::vector<double> parentGain[2];
    enum class Topology { Stable, Growing, Merging };
    Topology topology = Topology::Stable;
    int topologyBand = 0;
    int topologyHold = 0;

    FilterDesign filterDesign;
    bool filterDesigned = false;
    DetectionFilter lastFilterType = DetectionFilter::None;
    float lastFilterHz = -1.0f, lastTilt = 1000.0f;
    int lastSlope = -1;

    AutoGainKey autoKeys[kNumChannelSets][kMaxBands] {};
    bool autoKeyValid[kNumChannelSets][kMaxBands] {};
    double autoWeights[kAutoGainPoints] {};
    double autoWeightSum = 0.0;

    int activeBands = 1;
    int lookaheadSamples = 0;
    std::atomic<int> latency { 0 };
    bool modeInitialised = false;

    MeterAccumulator meterAcc[kNumChannelSets][kMaxBands];

    Impl()
    {
        for (int k = 0; k < kAutoGainPoints; ++k)
        {
            const double d = -12.0 + 0.5 * k;
            autoWeights[k] = std::exp (-d * d / (2.0 * 16.0));
            autoWeightSum += autoWeights[k];
        }
    }

    //==========================================================================
    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock = std::max (1, maxBlockSize);

        crossover.prepare (sr, maxBlock);
        detectorCrossover.prepare (sr, maxBlock);
        bandBuffers.assign ((size_t) (kMaxBands * 2 * maxBlock), 0.0f);
        detectorBuffers.assign (bandBuffers.size(), 0.0f);
        filteredInput.assign ((size_t) (2 * maxBlock), 0.0f);
        for (int b = 0; b < kMaxBands; ++b)
        {
            for (int c = 0; c < 2; ++c)
            {
                bandPtr[b][c] = bandBuffers.data() + (size_t) ((b * 2 + c) * maxBlock);
                detectorPtr[b][c] = detectorBuffers.data() + (size_t) ((b * 2 + c) * maxBlock);
            }
            bandOut[b] = bandPtr[b];
            detectorOut[b] = detectorPtr[b];
        }

        const int maxLook = (int) std::ceil (0.010 * sr) + 1;
        const int fade10 = (int) std::lround (0.010 * sr);
        for (auto& ch : chan)
            for (auto& s : ch)
            {
                s.GainState::prepare (sr);
                s.audioDelay.prepare (maxLook, fade10);
                s.detectorDelay.prepare (maxLook, fade10);
            }
        for (auto& s : linkedState)
            s.prepare (sr);
        for (auto& f : inputFilter)
            f.prepare ((int) std::lround (0.020 * sr));

        for (auto& set : sets)
            for (auto& sb : set)
            {
                for (auto* s : { &sb.compThr, &sb.compRatio, &sb.compKnee, &sb.limThr, &sb.limRatio, &sb.limKnee, &sb.mix,
                                 &sb.gain })
                    s->prepare (sr, 20.0);
                for (auto* s : { &sb.compOn, &sb.limOn, &sb.bandOn })
                    s->prepare (sr, 10.0);
                sb.makeup.prepare (sr, 50.0);
            }
        for (auto& s : soloWeight)
            s.prepare (sr, 10.0);
        detectorSolo.prepare (sr, 10.0);
        outputGain.prepare (sr, 20.0);
        linkWeight.prepare (sr, 10.0);
        topologyMix.prepare (sr, 20.0);

        detectorSoloRamp.assign ((size_t) maxBlock, 0.0f);
        outputGainRamp.assign ((size_t) maxBlock, 1.0f);
        linkRamp.assign ((size_t) maxBlock, 1.0f);
        topologyRamp.assign ((size_t) maxBlock, 1.0f);
        parentWeight.assign ((size_t) maxBlock, 1.0f);
        for (auto& g : parentGain)
            g.assign ((size_t) maxBlock, 1.0);

        filterDesigned = false;
        for (auto& row : autoKeyValid)
            for (auto& v : row)
                v = false;

        prepared = true;
        reset();
        updateTargets (true);
    }

    void reset() noexcept
    {
        crossover.reset();
        detectorCrossover.reset();
        for (auto& f : inputFilter)
            f.reset();
        for (auto& ch : chan)
            for (auto& s : ch)
                resetChannelBand (s);
        for (auto& s : linkedState)
            s.reset();
        for (auto& set : meterAcc)
            for (auto& m : set)
                m.clear();
        publishMeters();
        modeInitialised = false;
        topology = Topology::Stable;
        fresh = true;
    }

    static void resetChannelBand (ChannelBandState& s) noexcept
    {
        s.GainState::reset();
        s.audioDelay.reset();
        s.detectorDelay.reset();
    }

    //==========================================================================
    double makeupFor (const DynamicsBandParams& bp, float lo, float hi) const noexcept
    {
        if (! params.autoGain || ! bp.on)
            return 0.0;

        const double phi = std::max (0.01, std::log2 ((double) hi / (double) lo) / std::log2 (1000.0));
        const double offset = params.detection == DetectionMode::Rms ? 0.0 : 3.0;
        const double lref = -18.0 + 10.0 * std::log10 (phi) + offset;

        const GainCurve cc { bp.comp.thresholdDb, bp.comp.ratio, bp.comp.kneeDb, false };
        const GainCurve lc { bp.lim.thresholdDb, bp.lim.ratio, bp.lim.kneeDb, true };

        double acc = 0.0;
        for (int k = 0; k < kAutoGainPoints; ++k)
        {
            const float level = (float) (lref - 12.0 + 0.5 * k);
            const float c = bp.comp.on ? cc.gainDb (level) : 0.0f;
            const float l = bp.lim.on ? lc.gainDb (level + c) : 0.0f;
            acc += autoWeights[k] * (double) (c + l);
        }
        return std::clamp (-acc / autoWeightSum, -24.0, 24.0);
    }

    void bandEdges (int band, float& lo, float& hi) const noexcept
    {
        lo = band == 0 ? 20.0f : params.crossoverHz[band - 1];
        hi = band == params.numBands - 1 ? 20000.0f : params.crossoverHz[band];
        lo = std::clamp (lo, 20.0f, 20000.0f);
        hi = std::clamp (hi, lo, 20000.0f);
    }

    // Serialize layout edits. Before merging, converge the retiring gains/monitor weights on the
    // surviving band's values; only then can raw queued samples be merged without a level step.
    void updateTopology (bool snap) noexcept
    {
        const auto& p = params;
        if (snap)
        {
            activeBands = p.numBands;
            topology = Topology::Stable;
        }
        if (topology != Topology::Stable && ! topologyMix.isSmoothing())
        {
            if (topology == Topology::Merging)
            {
                for (auto& ch : chan)
                    for (int b = topologyBand + 1; b < activeBands; ++b)
                    {
                        ch[topologyBand].audioDelay.merge (ch[b].audioDelay);
                        ch[topologyBand].detectorDelay.merge (ch[b].detectorDelay);
                    }
                activeBands = topologyBand + 1;
            }
            topology = Topology::Stable;
        }
        if (topology == Topology::Stable && p.numBands != activeBands)
        {
            topologyBand = std::min (activeBands, p.numBands) - 1;
            if (p.numBands > activeBands)
            {
                // The parent retains unsplit history; the children's empty, aligned delays complement it.
                for (int b = activeBands; b < p.numBands; ++b)
                {
                    linkedState[b] = linkedState[topologyBand];
                    const auto& linkedParams = p.bands[0][b];
                    linkedState[b].setTimes (linkedParams, p.adaptiveRelease);
                    for (int c = 0; c < 2; ++c)
                    {
                        auto& ch = chan[c];
                        resetChannelBand (ch[b]);
                        ch[b].detector = ch[topologyBand].detector;
                        ch[b].comp = ch[topologyBand].comp;
                        ch[b].lim = ch[topologyBand].lim;
                        const auto& bp = p.bands[c][b];
                        ch[b].setTimes (bp, p.adaptiveRelease);
                        ch[b].audioDelay.startSplit (ch[topologyBand].audioDelay);
                        ch[b].detectorDelay.startSplit (ch[topologyBand].detectorDelay);
                        sets[c][b].snapAll();
                    }
                }
                activeBands = p.numBands;
                topology = Topology::Growing;
                topologyHold = chan[0][topologyBand].audioDelay.firstTap();
            }
            else
            {
                topology = Topology::Merging;
                topologyHold = 0;
            }
            topologyMix.setTarget (0.0f);
            topologyMix.snap();
            topologyMix.setTarget (1.0f);
        }
        crossover.setup (activeBands, p.crossoverHz, p.crossoverType);
        detectorCrossover.setup (activeBands, p.crossoverHz, p.crossoverType);
    }

    // Pushes params / monitor into the smoothers and stages. snap = apply directly.
    void updateTargets (bool snap) noexcept
    {
        const auto& p = params;
        updateTopology (snap);
        lookaheadSamples = (int) std::lround (std::clamp (p.lookaheadMs, 0.0f, 10.0f) * 0.001 * sr);
        latency.store (lookaheadSamples + crossover.getLatencySamples (p.crossoverType), std::memory_order_relaxed);

        // detection filter
        if (! filterDesigned || p.detectionFilter != lastFilterType || p.detectionFilterHz != lastFilterHz
            || p.detectionFilterSlopeIndex != lastSlope || p.detectionTiltDb != lastTilt)
        {
            FilterDesign d;
            designDetectionFilter (d, sr, p.detectionFilter, p.detectionFilterHz, p.detectionFilterSlopeIndex,
                                   p.detectionTiltDb);
            if (! filterDesigned || ! sameCoeffs (d, filterDesign))
            {
                filterDesign = d;
                for (auto& f : inputFilter)
                    f.apply (filterDesign, snap);
            }
            filterDesigned = true;
            lastFilterType = p.detectionFilter;
            lastFilterHz = p.detectionFilterHz;
            lastSlope = p.detectionFilterSlopeIndex;
            lastTilt = p.detectionTiltDb;
        }
        else
        {
            // a deferred structure change (crossfade was running) is retried every block
            for (auto& f : inputFilter)
                f.apply (filterDesign, snap);
        }

        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < kMaxBands; ++b)
            {
                auto& s = chan[c][b];
                const auto& bp = p.bands[c][b];
                s.detector.setMode (p.detection, snap);
                s.setTimes (bp, p.adaptiveRelease);
                if (snap)
                {
                    s.audioDelay.snapDelay (lookaheadSamples);
                    s.detectorDelay.snapDelay (lookaheadSamples);
                }
                else
                {
                    s.audioDelay.beginBlock (lookaheadSamples);
                    s.detectorDelay.beginBlock (lookaheadSamples);
                }
            }

        for (int b = 0; b < kMaxBands; ++b)
        {
            auto& s = linkedState[b];
            const auto& bp = p.bands[0][b];
            s.detector.setMode (p.detection, snap);
            s.setTimes (bp, p.adaptiveRelease);
        }

        for (int set = 0; set < kNumChannelSets; ++set)
            for (int b = 0; b < kMaxBands; ++b)
            {
                const auto& bp = p.bands[set][b];
                auto& sb = sets[set][b];

                float lo = 20.0f, hi = 20000.0f;
                if (b < p.numBands)
                    bandEdges (b, lo, hi);
                const AutoGainKey key { bp.comp.thresholdDb, bp.comp.ratio, bp.comp.kneeDb, bp.lim.thresholdDb,
                                        bp.lim.ratio, bp.lim.kneeDb, lo, hi, bp.comp.on, bp.lim.on, bp.on, p.autoGain,
                                        (int) p.detection };
                if (! autoKeyValid[set][b] || ! (key == autoKeys[set][b]))
                {
                    autoKeys[set][b] = key;
                    autoKeyValid[set][b] = true;
                    sb.makeup.setTarget ((float) makeupFor (bp, lo, hi));
                }

                sb.compThr.setTarget (bp.comp.thresholdDb);
                sb.compRatio.setTarget (bp.comp.ratio);
                sb.compKnee.setTarget (bp.comp.kneeDb);
                sb.compOn.setTarget (bp.comp.on ? 1.0f : 0.0f);
                sb.limThr.setTarget (bp.lim.thresholdDb);
                sb.limRatio.setTarget (bp.lim.ratio);
                sb.limKnee.setTarget (bp.lim.kneeDb);
                sb.limOn.setTarget (bp.lim.on ? 1.0f : 0.0f);
                sb.bandOn.setTarget (bp.on ? 1.0f : 0.0f);
                sb.mix.setTarget (std::clamp (bp.parallelPercent, 0.0f, 100.0f) * 0.01f);
                sb.gain.setTarget (bp.gainDb == 0.0f ? 1.0f : dbToGainF (bp.gainDb));
                if (snap)
                    sb.snapAll();
            }

        outputGain.setTarget (p.outputGainDb == 0.0f ? 1.0f : dbToGainF (p.outputGainDb));
        updateMonitorTargets();

        if (snap)
        {
            outputGain.snap();
            detectorSolo.snap();
            for (auto& s : soloWeight)
                s.snap();
        }
    }

    void updateMonitorTargets() noexcept
    {
        const int solo = monitor.soloBand;
        const bool soloOn = solo >= 0 && solo < params.numBands;
        for (int b = 0; b < kMaxBands; ++b)
            soloWeight[b].setTarget (! soloOn || b == solo ? 1.0f : 0.0f);
        detectorSolo.setTarget (monitor.soloDetectionFilter ? 1.0f : 0.0f);
    }

    //==========================================================================
    // The A control frame is shared by linked and independent paths, so ramps advance exactly once.
    inline double bandGain (const BandControls& v, GainState& st, float level, MeterAccumulator& m) noexcept
    {
        const double cg = st.comp.process ((double) v.comp.gainDb (level), level, false) * (double) v.compOn;
        const double lg = st.lim.process ((double) v.lim.gainDb (level + (float) cg), level, v.lim.ratio <= -1.0f)
                          * (double) v.limOn;
        const double wetDb = cg + lg + (double) v.makeup;
        const double wet = wetDb == 0.0 ? 1.0 : std::exp (wetDb * kDbToLn);

        double g = v.mix >= 1.0f ? wet : (v.mix <= 0.0f ? 1.0 : (1.0 - (double) v.mix) + (double) v.mix * wet);
        g *= (double) v.gain;
        if (v.bandOn < 1.0f)
            g = v.bandOn <= 0.0f ? 1.0 : 1.0 + (double) v.bandOn * (g - 1.0);

        keepLargest (m.total, (float) (cg + lg) * v.bandOn);
        keepLargest (m.comp, (float) cg * v.bandOn);
        keepLargest (m.lim, (float) lg * v.bandOn);
        m.det = std::max (m.det, level);
        return g;
    }

    void processChunk (float* const* io, int n, bool linked) noexcept
    {
        const int numBands = activeBands;

        const float* in[2] { io[0], io[1] };
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                filteredInput[(size_t) (c * maxBlock + i)] = inputFilter[c].process (io[c][i]);
        const float* detectorInput[2] { filteredInput.data(), filteredInput.data() + maxBlock };
        detectorCrossover.process (detectorInput, detectorOut, n);
        crossover.process (in, bandOut, n);

        // per-block ramps of the global smoothers
        for (int i = 0; i < n; ++i)
        {
            detectorSoloRamp[(size_t) i] = detectorSolo.next();
            outputGainRamp[(size_t) i] = outputGain.next();
            linkRamp[(size_t) i] = linkWeight.next();
            if (topologyHold > 0)
            {
                --topologyHold;
                topologyRamp[(size_t) i] = 0.0f;
            }
            else
                topologyRamp[(size_t) i] = topologyMix.next();
        }

        std::memset (io[0], 0, sizeof (float) * (size_t) n);
        std::memset (io[1], 0, sizeof (float) * (size_t) n);

        for (int b = 0; b < numBands; ++b)
        {
            float* x0 = bandPtr[b][0];
            float* x1 = bandPtr[b][1];
            auto& s0 = chan[0][b];
            auto& s1 = chan[1][b];
            auto& m0 = meterAcc[0][b];
            auto& m1 = meterAcc[1][b];
            auto& w = soloWeight[b];

            for (int i = 0; i < n; ++i)
            {
                const float a0 = x0[i], a1 = x1[i];
                const float d0 = detectorPtr[b][0][i], d1 = detectorPtr[b][1][i];

                float levelInput0 = d0, levelInput1 = d1;
                if (topology == Topology::Merging && b == topologyBand)
                {
                    for (int other = b + 1; other < numBands; ++other)
                    {
                        levelInput0 += detectorPtr[other][0][i];
                        levelInput1 += detectorPtr[other][1][i];
                    }
                }
                const double link = (double) linkRamp[(size_t) i];
                const auto controlsA = nextControls (sets[0][b]);
                const auto controlsB = nextControls (sets[1][b]);
                MeterAccumulator unused;
                auto& ls = linkedState[b];
                const float peak = std::max (std::abs (levelInput0), std::abs (levelInput1));
                const double linkedGain = bandGain (controlsA, ls, ls.detector.processDb (peak), linked ? m0 : unused);
                double g0 = bandGain (controlsA, s0, s0.detector.processDb (levelInput0), linked ? unused : m0);
                double g1 = bandGain (controlsB, s1, s1.detector.processDb (levelInput1), m1);
                g0 = link >= 1.0 ? linkedGain : (link <= 0.0 ? g0 : g0 + link * (linkedGain - g0));
                g1 = link >= 1.0 ? linkedGain : (link <= 0.0 ? g1 : g1 + link * (linkedGain - g1));
                float wb = w.next();
                if (topology != Topology::Stable)
                {
                    if (b == topologyBand)
                    {
                        parentGain[0][(size_t) i] = g0;
                        parentGain[1][(size_t) i] = g1;
                        parentWeight[(size_t) i] = wb;
                    }
                    else if (b > topologyBand)
                    {
                        const float t = topologyRamp[(size_t) i];
                        const double own = topology == Topology::Growing ? (double) t : 1.0 - (double) t;
                        g0 = parentGain[0][(size_t) i] + own * (g0 - parentGain[0][(size_t) i]);
                        g1 = parentGain[1][(size_t) i] + own * (g1 - parentGain[1][(size_t) i]);
                        wb = parentWeight[(size_t) i] + (float) own * (wb - parentWeight[(size_t) i]);
                    }
                }

                const float y0 = (float) ((double) s0.audioDelay.process (a0) * g0);
                const float y1 = (float) ((double) s1.audioDelay.process (a1) * g1);
                const float dd0 = s0.detectorDelay.process (d0);
                const float dd1 = s1.detectorDelay.process (d1);

                const float ds = detectorSoloRamp[(size_t) i];
                io[0][i] += wb * (y0 + ds * (dd0 - y0));
                io[1][i] += wb * (y1 + ds * (dd1 - y1));
            }

            if (linked)
                m1 = m0;
        }

        if (outputGain.getCurrent() != 1.0f || outputGainRamp[0] != 1.0f)
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    io[c][i] *= outputGainRamp[(size_t) i];
    }

    void publishMeters() noexcept
    {
        for (int set = 0; set < kNumChannelSets; ++set)
            for (int b = 0; b < kMaxBands; ++b)
            {
                auto& m = meterAcc[set][b];
                if (b >= params.numBands)
                    m.clear();
                meters.gainReductionDb[set][b].store (m.total);
                meters.compGainDb[set][b].store (m.comp);
                meters.limGainDb[set][b].store (m.lim);
                meters.detectorDb[set][b].store (m.det);
                m.clear();
            }
    }

    void process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
    {
        if (! prepared || numSamples <= 0)
            return;

        ScopedFlushDenormals noDenormals;

        if (fresh)
        {
            updateTargets (true);
            fresh = false;
        }

        const bool linked = ctx.channelMode == ChannelMode::Stereo || ctx.channelMode == ChannelMode::TransientSustain;
        linkWeight.setTarget (linked ? 1.0f : 0.0f);
        if (! modeInitialised)
        {
            linkWeight.snap();
            modeInitialised = true;
        }

        for (int start = 0; start < numSamples; start += maxBlock)
        {
            updateTopology (false);
            const int n = std::min (maxBlock, numSamples - start);
            float* io[2] { channels[0] + start, channels[1] + start };
            processChunk (io, n, linked);
        }

        publishMeters();
    }
};

//==============================================================================
Dynamics::Dynamics() : impl (std::make_unique<Impl>()) {}
Dynamics::~Dynamics() = default;

void Dynamics::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }
void Dynamics::reset() noexcept { impl->reset(); }

void Dynamics::setParams (ParamValues values) noexcept
{
    readDynamicsParams (values, impl->params);
    if (! impl->prepared)
        return;
    const bool snap = impl->fresh;
    impl->updateTargets (snap);
    impl->fresh = false;
}

void Dynamics::setMonitor (const ModuleMonitor& m) noexcept
{
    impl->monitor = m;
    impl->updateMonitorTargets();
}

void Dynamics::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
    impl->process (channels, numSamples, ctx);
}

int Dynamics::getLatencySamples() const noexcept { return impl->latency.load (std::memory_order_relaxed); }
const DynamicsMeters& Dynamics::getMeters() const noexcept { return impl->meters; }
} // namespace ps
