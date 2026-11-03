#include "dsp/PassiveEq.h"

#include "dsp/Envelope.h"
#include "dsp/PassiveEqDesign.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__SSE__)
 #include <xmmintrin.h>
 #define PS_PEQ_HAS_SSE 1
#endif

// Passive EQ (mod-peq). Contract and signal flow: PassiveEq.h. Choices made here (the header leaves them open):
//
// - Sections: the analog prototypes of design:: (Filters.h), matched up to Nyquist. Low boost = low shelf with a
//   slightly resonant Q (a little overshoot just below the corner, as the passive LC boost has); low cut = a gentler
//   low shelf whose corner sits kLowCutRatio above it, so boost 10 + cut 5 at 60 Hz gives about +6 dB at 30 Hz and a
//   -4 dB dip at 100 Hz (the classic "boost with a dip above it"). Both shelves reach their full depth only far below
//   the corner (13.5 - 17.5: both at 10 end 4 dB down at DC, under the boost's bump).
// - Proportional Q (bottom section): Q rises with the amount (kMidQLow at a touch, kMidQHigh at 10): small moves are
//   broad, large ones narrower, as a passive midrange network behaves.
// - A section at amount 0 keeps its poles and gets numerator = denominator (exactly unity: the SVF output weights are
//   exactly 0 and 1), so its state keeps running and engaging it never starts from a cold state; a frequency step at
//   amount 0 glides its poles without touching the audio (bit-exact).
// - Designs are recomputed in setParams only for the sections whose inputs changed; each channel picks up a changed
//   set at the start of the next process() and glides there over kGlideMs (SmoothedBiquad). The first parameters
//   after prepare / reset are applied at once (no glide from the defaults).
// - Tube stage (at 2x, ps::Oversampler MinimumPhase): a triode-like operating point on a tanh curve,
//   y = (tanh (k (x + b)) - tanh (k b)) / (k sech^2 (k b)), k = kDriveMax x drive / 10, b = kTubeBias: slope 1 and
//   no offset at x = 0, the asymmetry gives mostly 2nd harmonic (drive 10, -18 dBFS: about -32 dBc H2, -53 dBc H3).
//   Only the residual y - x goes through a DC blocker (kResidualHpHz). The downsampler receives the compensated
//   residual, gain * h + (gain - 1) * x; it is added to the exact delayed clean path. Thus switching Drive cannot
//   crossfade two full-level copies with different phase and notch the treble. Make-up: a table over drive
//   (101 points, built in prepare) of the fundamental gain of a -18 dBFS sine, inverted. Drive glides linearly over
//   kDriveSmoothMs.
// - Drive 0: the output is the input delayed by kPassiveEqLatency in a plain delay line (bit-exact); the oversampled
//   path keeps running unheard (warm), and leaving / returning to 0 fades its residual (raised cosine, kCrossfadeMs).
//   The residual retains the minimum-phase round trip's phase; the clean signal always has exactly 3 samples' delay.
// - Denormals: FTZ / DAZ while processing (and the filter states flush tiny values). Non-finite or corrupt input
//   samples are rejected before entering any recursive state.
namespace ps
{
namespace
{
    //==========================================================================
    // Tunable coefficients (first-principles values; the fit to the reference's renders changes only these).
    // top section: passive LC program EQ
    constexpr double kLowBoostMaxDb = 13.5;      // low shelf boost at lowBoost 10
    constexpr double kLowBoostQ = 1.0;           // its Q (> 0.707: a small resonant bump below the corner)
    constexpr double kLowCutMaxDb = 17.5;        // low shelf cut at lowCut 10
    constexpr double kLowCutRatio = 1.6;         // the cut's corner = this x the boost's corner
    constexpr double kLowCutQ = 0.6;             // the cut's Q (gentle)
    constexpr double kHighBoostMaxDb = 16.0;     // high peak at highBoost 10
    constexpr double kHighQBroad = 0.4;          // high peak Q at highQ 0
    constexpr double kHighQSharp = 2.5;          // high peak Q at highQ 10 (log interpolation in between)
    constexpr double kBwBoostCoupling = 0.5;     // Q x (1 + this x law (highBoost / 10)): narrower as the boost rises
    constexpr double kHighCutMaxDb = 16.0;       // high shelf cut at highCut 10
    constexpr double kHighCutQ = 0.5;            // its Q (gentle, close to a first-order slope)
    // bottom section: passive midrange EQ
    constexpr double kMidMaxDb = 10.0;           // low-mid boost / mid cut / high-mid boost at 10
    constexpr double kMidQLow = 0.6;             // proportional Q: at a small amount ...
    constexpr double kMidQHigh = 1.4;            // ... and at 10
    // knob law: amount 0..10 -> share of the full gain (linear until fitted); kKnobLaw[0] must stay 0
    constexpr double kKnobLaw[11] { 0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0 };
    constexpr double kMaxHzRatio = 0.45;         // design frequencies clamped to this x the sample rate
    constexpr double kGlideMs = 10.0;            // coefficient glide
    constexpr double kNeutralProbeDb = 0.01;     // a neutral section keeps the poles of its design at this gain
    // tube make-up stage
    constexpr double kDriveMax = 1.55;           // curve gain k at drive 10
    constexpr double kTubeBias = 0.21;           // operating-point offset b (the asymmetry: even harmonics)
    constexpr double kResidualHpHz = 10.0;       // DC blocker on the saturator's residual
    constexpr double kMakeupRefDbfs = -18.0;     // level compensation reference (sine peak)
    constexpr double kDriveSmoothMs = 20.0;      // drive glide
    constexpr double kCrossfadeMs = 20.0;        // drive 0 <-> tube path

    constexpr double kPi = 3.14159265358979323846;
    constexpr float kMaxInput = 1.0e6f;          // reject corrupt audio before the EQ and float oversampler
    constexpr int kChunk = 64;                   // oversampler block
    constexpr int kMakeupPoints = 101;           // make-up table over drive 0..10
    constexpr int kMakeupCycle = 512;            // samples per cycle of its measurement

    using namespace peq;   // the Section enum

    inline void flushTiny (double& v) noexcept
    {
        if (std::abs (v) < 1.0e-30)
            v = 0.0;
    }

    double knobLaw (float amount) noexcept
    {
        if (! std::isfinite (amount) || amount <= 0.0f)
            return 0.0;
        const double u = std::min ((double) amount, 10.0);
        const int i = std::min ((int) u, 9);
        const double f = u - (double) i;
        return kKnobLaw[i] + (kKnobLaw[i + 1] - kKnobLaw[i]) * f;
    }

    template <size_t N>
    double pick (const std::array<double, N>& hz, int index) noexcept
    {
        return hz[(size_t) std::clamp (index, 0, (int) N - 1)];
    }

    // The inputs of one section (a redesign happens only when they change).
    struct SectionKey
    {
        int hzIndex = -1;
        float amount = -1.0f, extra = -1.0f;

        bool operator== (const SectionKey& o) const noexcept
        {
            return hzIndex == o.hzIndex && amount == o.amount && extra == o.extra;
        }
    };

    SectionKey keyOf (int section, const PassiveEqSetParams& p) noexcept
    {
        switch (section)
        {
            case kLowBoost:  return { p.lowHzIndex, p.lowBoost, 0.0f };
            case kLowCut:    return { p.lowHzIndex, p.lowCut, 0.0f };
            case kHighBoost: return { p.highHzIndex, p.highBoost, p.highQ };
            case kHighCut:   return { p.highCutHzIndex, p.highCut, 0.0f };
            case kLowMid:    return { p.lowMidHzIndex, p.lowMidBoost, 0.0f };
            case kMidCut:    return { p.midHzIndex, p.midCut, 0.0f };
            default:         return { p.highMidHzIndex, p.highMidBoost, 0.0f };
        }
    }

    double highBoostQ (float highQ, float boost) noexcept
    {
        const double hq = std::isfinite (highQ) ? std::clamp ((double) highQ, 0.0, 10.0) : 0.0;
        const double q0 = kHighQBroad * std::pow (kHighQSharp / kHighQBroad, hq / 10.0);
        return q0 * (1.0 + kBwBoostCoupling * knobLaw (boost));
    }

    double midQ (float amount) noexcept
    {
        return kMidQLow + (kMidQHigh - kMidQLow) * knobLaw (amount);
    }

    // What section `section` of set p is: prototype, frequency (clamped), Q and gain.
    enum class Kind { LowShelf, HighShelf, Peak };
    struct SectionSpec
    {
        Kind kind = Kind::Peak;
        double hz = 1000.0, q = 0.707, gainDb = 0.0;
    };

    SectionSpec specOf (int section, const PassiveEqSetParams& p, double sr) noexcept
    {
        SectionSpec s;
        switch (section)
        {
            case kLowBoost:
                s = { Kind::LowShelf, pick (kPassiveEqLowHz, p.lowHzIndex), kLowBoostQ, kLowBoostMaxDb * knobLaw (p.lowBoost) };
                break;
            case kLowCut:
                s = { Kind::LowShelf, pick (kPassiveEqLowHz, p.lowHzIndex) * kLowCutRatio, kLowCutQ,
                      -kLowCutMaxDb * knobLaw (p.lowCut) };
                break;
            case kHighBoost:
                s = { Kind::Peak, pick (kPassiveEqHighHz, p.highHzIndex), highBoostQ (p.highQ, p.highBoost),
                      kHighBoostMaxDb * knobLaw (p.highBoost) };
                break;
            case kHighCut:
                s = { Kind::HighShelf, pick (kPassiveEqHighCutHz, p.highCutHzIndex), kHighCutQ,
                      -kHighCutMaxDb * knobLaw (p.highCut) };
                break;
            case kLowMid:
                s = { Kind::Peak, pick (kPassiveEqLowMidHz, p.lowMidHzIndex), midQ (p.lowMidBoost),
                      kMidMaxDb * knobLaw (p.lowMidBoost) };
                break;
            case kMidCut:
                s = { Kind::Peak, pick (kPassiveEqMidHz, p.midHzIndex), midQ (p.midCut), -kMidMaxDb * knobLaw (p.midCut) };
                break;
            default:
                s = { Kind::Peak, pick (kPassiveEqHighMidHz, p.highMidHzIndex), midQ (p.highMidBoost),
                      kMidMaxDb * knobLaw (p.highMidBoost) };
                break;
        }
        s.hz = std::min (s.hz, kMaxHzRatio * sr);
        return s;
    }

    bool sameSet (const PassiveEqSetParams& a, const PassiveEqSetParams& b) noexcept
    {
        return a.lowHzIndex == b.lowHzIndex && a.lowBoost == b.lowBoost && a.lowCut == b.lowCut
               && a.highHzIndex == b.highHzIndex && a.highBoost == b.highBoost && a.highQ == b.highQ
               && a.highCutHzIndex == b.highCutHzIndex && a.highCut == b.highCut && a.lowMidHzIndex == b.lowMidHzIndex
               && a.lowMidBoost == b.lowMidBoost && a.midHzIndex == b.midHzIndex && a.midCut == b.midCut
               && a.highMidHzIndex == b.highMidHzIndex && a.highMidBoost == b.highMidBoost;
    }

    // The tube curve with slope 1 and no offset at 0 (k > 0).
    struct TubeCurve
    {
        double k = 0.0, kb = 0.0, t0 = 0.0, inv = 0.0;

        void set (double driveNorm) noexcept
        {
            k = kDriveMax * driveNorm;
            kb = k * kTubeBias;
            t0 = std::tanh (kb);
            inv = k > 1.0e-9 ? 1.0 / (k * (1.0 - t0 * t0)) : 0.0;
        }

        bool active() const noexcept { return k > 1.0e-9; }
        double operator() (double x) const noexcept { return (std::tanh (k * x + kb) - t0) * inv; }
    };
} // namespace

//==============================================================================
namespace peq
{
BiquadCoeffs designSection (int section, const PassiveEqSetParams& p, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto s = specOf (section, p, sr);
    const bool neutral = s.gainDb == 0.0;
    const double g = neutral ? kNeutralProbeDb : s.gainDb;
    BiquadCoeffs c;
    switch (s.kind)
    {
        case Kind::LowShelf:  c = design::lowShelf (sr, s.hz, s.q, g); break;
        case Kind::HighShelf: c = design::highShelf (sr, s.hz, s.q, g); break;
        case Kind::Peak:      c = design::peak (sr, s.hz, s.q, g); break;
    }

    if (neutral)
        c = { 1.0, c.a1, c.a2, c.a1, c.a2 };   // exactly unity, poles kept
    return c;
}

bool isSectionActive (int section, const PassiveEqSetParams& p) noexcept
{
    return specOf (section, p, 48000.0).gainDb != 0.0;
}

double sectionHz (int section, const PassiveEqSetParams& p, double sampleRate) noexcept
{
    return specOf (section, p, sampleRate > 0.0 ? sampleRate : 48000.0).hz;
}
} // namespace peq

//==============================================================================
struct PassiveEq::Impl
{
    struct SetDesign
    {
        BiquadCoeffs c[kNumSections];
        SectionKey key[kNumSections];
        unsigned sectionVersion[kNumSections] {};
        unsigned version = 1;
    };

    struct ChannelEq
    {
        SmoothedBiquad s[kNumSections];
        int set = -1;
        unsigned version = 0;
        unsigned sectionVersion[kNumSections] {};
        bool snap = true;
    };

    // Seqlock for getMagnitudeDb: the audio thread writes (never waits), readers retry while a write is in progress.
    struct Published
    {
        std::atomic<std::uint32_t> seq { 0 };
        std::atomic<double> sampleRate { 0.0 };
        std::atomic<bool> valid { false };
        std::atomic<double> c[kNumChannelSets][kNumSections][5];
    };

    Impl()
    {
        for (auto& set : pub.c)
            for (auto& sec : set)
                for (auto& v : sec)
                    v.store (0.0, std::memory_order_relaxed);
    }

    //==========================================================================
    void prepare (double rate, int set)
    {
        sr = rate > 0.0 ? rate : 48000.0;
        paramSet = set;

        const int glide = std::max (1, (int) std::lround (kGlideMs * 0.001 * sr));
        for (auto& ch : eq)
            for (auto& s : ch.s)
                s.setRampLength (glide);

        os.prepare (1, Oversampler::Quality::MinimumPhase, kChunk, 2);
        wetDelaySamples = std::max (0, kPassiveEqLatency - os.getLatencySamples());
        for (int c = 0; c < 2; ++c)
        {
            dryDelay[c].prepare (kPassiveEqLatency);
            dryDelay[c].setDelay (kPassiveEqLatency);
            wetDelay[c].prepare (std::max (1, wetDelaySamples));
            wetDelay[c].setDelay (wetDelaySamples);
        }

        driveSmoother.prepare (2.0 * sr, kDriveSmoothMs);
        xfadeStep = 1.0 / std::max (1.0, kCrossfadeMs * 0.001 * sr);
        residualPole = std::exp (-2.0 * kPi * kResidualHpHz / (2.0 * sr));
        buildMakeup();

        // designs of the current parameters at the new rate
        for (auto& d : designs)
            for (auto& k : d.key)
                k = SectionKey {};
        redesign (0, current.sets[0], true);
        redesign (1, current.sets[1], true);
        if (haveParams)
            publish();
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : eq)
        {
            for (auto& s : ch.s)
                s.reset();
            ch.snap = true;
            ch.set = -1;
        }
        os.reset();
        for (int c = 0; c < 2; ++c)
        {
            dryDelay[c].reset();
            wetDelay[c].reset();
            residualIn[c] = residualOut[c] = 0.0;
        }
        driveSmoother.snap (current.drive);
        snapDrive = true;
        curveDrive = -1.0;   // the curve is set again on the next sample
        xfade = current.drive > 0.0f ? 1.0 : 0.0;
    }

    // Redesigns the changed sections of set `set` (all with force); publishes when anything changed.
    void redesign (int set, const PassiveEqSetParams& p, bool force) noexcept
    {
        auto& d = designs[set];
        bool changed = false;
        for (int k = 0; k < kNumSections; ++k)
        {
            const auto key = keyOf (k, p);
            if (! force && key == d.key[k])
                continue;
            d.key[k] = key;
            d.c[k] = peq::designSection (k, p, sr);
            ++d.sectionVersion[k];
            changed = true;
        }
        if (changed)
        {
            ++d.version;
            publishPending = true;
        }
    }

    void setParams (ParamValues values) noexcept
    {
        PassiveEqParams p;
        readPassiveEqParams (values, p);
        if (paramSet == 1)
            p.sets[0] = p.sets[1];   // T/S twin: set B for both channels (Module.h rule 1)

        p.drive = std::isfinite (p.drive) ? std::clamp (p.drive, 0.0f, 10.0f) : 0.0f;
        current.drive = p.drive;
        driveSmoother.setTarget (p.drive);

        for (int set = 0; set < kNumChannelSets; ++set)
            if (! haveParams || ! sameSet (p.sets[set], current.sets[set]))
            {
                current.sets[set] = p.sets[set];
                redesign (set, p.sets[set], false);
            }

        if (! haveParams)
        {
            haveParams = true;
            publishPending = true;
        }
        if (snapDrive)
        {
            driveSmoother.snap (p.drive);
            xfade = p.drive > 0.0f ? 1.0 : 0.0;
            snapDrive = false;
        }

        if (publishPending)
            publish();
    }

    void publish() noexcept
    {
        publishPending = false;
        const auto s = pub.seq.load (std::memory_order_relaxed);
        pub.seq.store (s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        pub.sampleRate.store (sr, std::memory_order_relaxed);
        for (int set = 0; set < kNumChannelSets; ++set)
            for (int k = 0; k < kNumSections; ++k)
            {
                const auto& c = designs[set].c[k];
                const double v[5] { c.b0, c.b1, c.b2, c.a1, c.a2 };
                for (int i = 0; i < 5; ++i)
                    pub.c[set][k][i].store (v[i], std::memory_order_relaxed);
            }
        pub.valid.store (haveParams, std::memory_order_relaxed);
        pub.seq.store (s + 2u, std::memory_order_release);
    }

    double magnitudeDb (int set, double hz) const noexcept
    {
        set = set == 1 ? 1 : 0;
        double c[kNumSections][5] {};
        double rate = 0.0;
        bool valid = false;
        for (int attempt = 0; attempt < 10000; ++attempt)
        {
            const auto s1 = pub.seq.load (std::memory_order_acquire);
            if ((s1 & 1u) != 0u)
            {
                std::this_thread::yield();
                continue;
            }
            rate = pub.sampleRate.load (std::memory_order_relaxed);
            valid = pub.valid.load (std::memory_order_relaxed);
            for (int k = 0; k < kNumSections; ++k)
                for (int i = 0; i < 5; ++i)
                    c[k][i] = pub.c[set][k][i].load (std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_acquire);
            if (pub.seq.load (std::memory_order_relaxed) == s1)
                break;
            valid = false;
        }

        if (! valid || ! (rate > 0.0) || ! std::isfinite (hz))
            return 0.0;

        const double f = std::clamp (hz, 0.0, 0.4999 * rate);
        double total = 0.0;
        for (int k = 0; k < kNumSections; ++k)
        {
            const BiquadCoeffs b { c[k][0], c[k][1], c[k][2], c[k][3], c[k][4] };
            total += b.magnitudeDb (f, rate);
        }
        return std::isfinite (total) ? total : 0.0;
    }

    //==========================================================================
    void buildMakeup()
    {
        const double a = std::pow (10.0, kMakeupRefDbfs / 20.0);
        for (int i = 0; i < kMakeupPoints; ++i)
        {
            TubeCurve tube;
            tube.set ((double) i / (double) (kMakeupPoints - 1));
            if (! tube.active())
            {
                makeup[(size_t) i] = 1.0;
                continue;
            }
            double acc = 0.0;
            for (int n = 0; n < kMakeupCycle; ++n)
            {
                const double s = std::sin (2.0 * kPi * (double) n / (double) kMakeupCycle);
                acc += tube (a * s) * s;
            }
            const double fundamental = 2.0 * acc / (double) kMakeupCycle;
            makeup[(size_t) i] = fundamental > 0.0 ? a / fundamental : 1.0;
        }
    }

    double makeupFor (double driveNorm) const noexcept
    {
        const double x = std::clamp (driveNorm, 0.0, 1.0) * (double) (kMakeupPoints - 1);
        const int i = std::min ((int) x, kMakeupPoints - 2);
        const double f = x - (double) i;
        return makeup[(size_t) i] + (makeup[(size_t) i + 1] - makeup[(size_t) i]) * f;
    }

    // Brings channel c's filters to the design of the set it plays now.
    void applyTargets (int c, int set) noexcept
    {
        auto& ch = eq[c];
        const auto& d = designs[set];
        if (ch.set == set && ch.version == d.version && ! ch.snap)
            return;
        for (int k = 0; k < kNumSections; ++k)
        {
            // SmoothedBiquad restarts a running ramp even when its target is unchanged.
            // An edit to another section must not extend this section's settling time.
            if (! ch.snap && ch.set == set && ch.sectionVersion[k] == d.sectionVersion[k])
                continue;
            ch.s[k].setTarget (d.c[k]);
            if (ch.snap)
                ch.s[k].snapToTarget();
            ch.sectionVersion[k] = d.sectionVersion[k];
        }
        ch.set = set;
        ch.version = d.version;
        ch.snap = false;
    }

    void process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
    {
        const bool dual = ctx.channelMode == ChannelMode::MidSide || ctx.channelMode == ChannelMode::LeftRight;
        applyTargets (0, 0);
        applyTargets (1, dual ? 1 : 0);

        // 1 - 7: the passive sections, per channel
        bool finite = true;
        for (int c = 0; c < 2; ++c)
        {
            auto& ch = eq[c];
            float* x = channels[c];
            for (int i = 0; i < numSamples; ++i)
            {
                double v = std::isfinite (x[i]) && std::abs (x[i]) <= kMaxInput ? (double) x[i] : 0.0;
                for (auto& s : ch.s)
                    v = s.processSample (v);
                x[i] = (float) v;
            }
            if (numSamples > 0 && ! std::isfinite (x[numSamples - 1]))
                finite = false;
        }

        // 8: the tube make-up stage
        for (int start = 0; start < numSamples; start += kChunk)
            tubeChunk (channels, start, std::min (kChunk, numSamples - start));

        if (! finite)
        {
            for (auto& ch : eq)
            {
                for (auto& s : ch.s)
                    s.reset();
            }
            os.reset();
            residualIn[0] = residualIn[1] = residualOut[0] = residualOut[1] = 0.0;
        }
    }

    void tubeChunk (float* const* channels, int start, int len) noexcept
    {
        float* ch[2] { channels[0] + start, channels[1] + start };

        for (int c = 0; c < 2; ++c)
        {
            std::copy (ch[c], ch[c] + len, dry[c].data());
            dryDelay[c].process (dry[c].data(), len);
        }

        // the oversampled path runs always (warm), heard only while xfade > 0
        const float* in[2] { ch[0], ch[1] };
        float* const* up = os.upsample (in, len);
        const int n2 = 2 * len;
        for (int i = 0; i < n2; ++i)
        {
            const double driveNorm = (double) driveSmoother.next() * 0.1;
            if (driveNorm != curveDrive)
            {
                curveDrive = driveNorm;
                curve.set (driveNorm);
                curveGain = makeupFor (driveNorm);
            }
            const double gain = curveGain;
            for (int c = 0; c < 2; ++c)
            {
                const double x = (double) up[c][i];
                const double r = curve.active() ? curve (x) - x : 0.0;
                double h = residualPole * (residualOut[c] + r - residualIn[c]);
                residualIn[c] = r;
                flushTiny (h);
                residualOut[c] = h;
                up[c][i] = (float) (h * gain + x * (gain - 1.0));
            }
        }
        float* wetOut[2] { wet[0].data(), wet[1].data() };
        os.downsample (wetOut, len);
        if (wetDelaySamples > 0)
            for (int c = 0; c < 2; ++c)
                wetDelay[c].process (wet[c].data(), len);

        // The same exact delayed clean signal is present throughout; only the tube correction fades.
        const double target = current.drive > 0.0f ? 1.0 : 0.0;
        if (xfade == target)
        {
            for (int c = 0; c < 2; ++c)
                if (target == 0.0)
                    std::copy (dry[c].data(), dry[c].data() + len, ch[c]);
                else
                    for (int i = 0; i < len; ++i)
                        ch[c][i] = dry[c][(size_t) i] + wet[c][(size_t) i];
            return;
        }

        for (int i = 0; i < len; ++i)
        {
            xfade = target > xfade ? std::min (target, xfade + xfadeStep) : std::max (target, xfade - xfadeStep);
            const double w = 0.5 - 0.5 * std::cos (kPi * xfade);
            for (int c = 0; c < 2; ++c)
                ch[c][i] = (float) ((double) dry[c][(size_t) i] + w * (double) wet[c][(size_t) i]);
        }
    }

    //==========================================================================
    double sr = 48000.0;
    int paramSet = 0;
    bool haveParams = false, publishPending = false, snapDrive = true;
    PassiveEqParams current;
    SetDesign designs[kNumChannelSets];
    ChannelEq eq[2];
    Published pub;

    Oversampler os;
    DelayLine dryDelay[2], wetDelay[2];
    int wetDelaySamples = 0;
    std::array<std::array<float, kChunk>, 2> dry {}, wet {};
    LinearSmoother driveSmoother;
    TubeCurve curve;
    double curveDrive = 0.0, curveGain = 1.0;
    double residualPole = 0.0, residualIn[2] {}, residualOut[2] {};
    double xfade = 0.0, xfadeStep = 0.001;
    std::array<double, kMakeupPoints> makeup {};
};

//==============================================================================
PassiveEq::PassiveEq() : impl (std::make_unique<Impl>()) {}
PassiveEq::~PassiveEq() = default;

void PassiveEq::prepare (double sampleRate, int)
{
    impl->prepare (sampleRate, getParamSet());
}

void PassiveEq::reset() noexcept { impl->reset(); }

void PassiveEq::setParams (ParamValues values) noexcept { impl->setParams (values); }

void PassiveEq::process (float* const* channels, int numSamples, const ProcessContext& ctx) noexcept
{
#if PS_PEQ_HAS_SSE
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr (savedCsr | 0x8040u);   // FTZ | DAZ
#endif
    impl->process (channels, numSamples, ctx);
#if PS_PEQ_HAS_SSE
    _mm_setcsr (savedCsr);
#endif
}

int PassiveEq::getLatencySamples() const noexcept { return kPassiveEqLatency; }

double PassiveEq::getMagnitudeDb (int set, double hz) const noexcept { return impl->magnitudeDb (set, hz); }
} // namespace ps
