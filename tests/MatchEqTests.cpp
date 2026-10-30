#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"

#include "core/Loudness.h"
#include "dsp/CurveFir.h"
#include "dsp/MatchAnalysis.h"
#include "dsp/MatchEq.h"
#include "dsp/Params.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// Match EQ tests (prefix "meq."): the analyser, the matched curve, the FIR realisation (impulse responses measured
// with an independent DTFT), latency, the live capture through the module FIFO + worker, realtime safety and CPU.
namespace
{
using namespace ps;
using pstest::cd;

constexpr double kTwoPi = 2.0 * pstest::kPi;

std::vector<float> defaultValues()
{
    std::vector<float> v;
    for (const auto& s : paramSpecs())
        v.push_back (s.defaultValue);
    return v;
}

struct Rig
{
    MatchEq eq;
    std::vector<float> values = defaultValues();
    double sr;
    int block;
    ProcessContext ctx;

    explicit Rig (double rate = 48000.0, int blockSize = 512, int paramSet = 0) : sr (rate), block (blockSize)
    {
        eq.setParamSet (paramSet);
        eq.prepare (sr, block);
        ctx.paramSet = paramSet;
        apply();
    }

    void set (const std::string& id, float v)
    {
        const int i = paramIndex (id);
        REQUIRE (i >= 0);
        values[(size_t) i] = v;
    }
    void apply() { eq.setParams (values.data()); }
    void mode (ChannelMode m)
    {
        ctx.channelMode = m;
        set ("meq.chMode", (float) m);   // the choice index equals the enum for the Match EQ
    }

    void run (std::vector<float>& l, std::vector<float>& r, int blockSize = 0)
    {
        const int bs = blockSize > 0 ? blockSize : block;
        for (size_t pos = 0; pos < l.size(); pos += (size_t) bs)
        {
            const int n = (int) std::min ((size_t) bs, l.size() - pos);
            apply();
            float* p[2] { l.data() + pos, r.data() + pos };
            eq.process (p, n, ctx);
        }
    }

    // Processes silence until the worker has installed the current curve, then clears the histories.
    bool settle (int maxMs = 20000)
    {
        std::vector<float> l ((size_t) block, 0.0f), r ((size_t) block, 0.0f);
        int calm = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds (maxMs))
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            run (l, r);
            if (! eq.isDesignPending())
            {
                if (++calm >= 3)
                {
                    eq.reset();
                    return true;
                }
            }
            else
            {
                calm = 0;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        return false;
    }

    // Impulse response (impulse on `channel`, -1 = both), length n. Returns the channel's output (both: left).
    std::vector<float> impulse (int n, int inChannel = -1, int outChannel = 0)
    {
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        if (inChannel != 1) l[0] = 1.0f;
        if (inChannel != 0) r[0] = 1.0f;
        run (l, r);
        return outChannel == 1 ? r : l;
    }
};

// DTFT magnitude (dB) of y at hz.
double responseDb (const std::vector<float>& y, double hz, double sr)
{
    const double w = kTwoPi * hz / sr;
    // recursive phasor (double): accurate enough for 2^18 taps
    double re = 0.0, im = 0.0;
    const cd step = std::polar (1.0, -w);
    cd ph (1.0, 0.0);
    for (size_t n = 0; n < y.size(); ++n)
    {
        if (y[n] != 0.0f)
        {
            re += (double) y[n] * ph.real();
            im += (double) y[n] * ph.imag();
        }
        ph *= step;
        if ((n & 4095) == 4095)
            ph = std::polar (1.0, -w * (double) (n + 1));   // renormalise
    }
    return 10.0 * std::log10 (std::max (1.0e-30, re * re + im * im));
}

// Least-squares slope (dB per octave) of the spectrum's channel between lo and hi.
double slopeDbPerOct (const MatchSpectrum& s, MatchChannel ch, double lo, double hi, double* maxDev = nullptr)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (int i = 0; i < kMatchPoints; ++i)
    {
        const double f = matchGridHz (i);
        const float v = s.valueDb (ch, i);
        if (f < lo || f > hi || v <= kMatchNoValue)
            continue;
        const double x = std::log2 (f / 1000.0);
        sx += x; sy += v; sxx += x * x; sxy += x * v;
        ++n;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    if (maxDev != nullptr)
    {
        const double icpt = (sy - slope * sx) / n;
        *maxDev = 0.0;
        for (int i = 0; i < kMatchPoints; ++i)
        {
            const double f = matchGridHz (i);
            const float v = s.valueDb (ch, i);
            if (f < lo || f > hi || v <= kMatchNoValue)
                continue;
            *maxDev = std::max (*maxDev, std::abs (v - (icpt + slope * std::log2 (f / 1000.0))));
        }
    }
    return slope;
}

// Sum of PSD x cell width between lo and hi, dB.
double integratedDb (const MatchSpectrum& s, MatchChannel ch, double lo, double hi)
{
    double p = 0.0;
    for (int i = 0; i < kMatchPoints; ++i)
    {
        const double f = matchGridHz (i);
        const float v = s.valueDb (ch, i);
        if (f >= lo && f <= hi && v > kMatchNoValue)
            p += std::pow (10.0, v / 10.0) * matchGridCellHz (i);
    }
    return 10.0 * std::log10 (std::max (p, 1.0e-30));
}

MatchSpectrum analyse (const std::vector<float>& l, const std::vector<float>* r, double sr, int chunk = 4096)
{
    MatchAnalyser a;
    a.prepare (sr);
    for (size_t pos = 0; pos < l.size(); pos += (size_t) chunk)
    {
        const int n = (int) std::min ((size_t) chunk, l.size() - pos);
        a.push (l.data() + pos, r != nullptr ? r->data() + pos : nullptr, n);
    }
    MatchSpectrum s;
    a.spectrum (s);
    return s;
}

MatchEqSetParams setParams (float amount, float smooth, float boost = 24.0f, float cut = 24.0f, float lo = 20.0f,
                            float hi = 20000.0f)
{
    MatchEqSetParams p;
    p.amountPercent = amount;
    p.smoothPercent = smooth;
    p.maxBoostDb = boost;
    p.maxCutDb = cut;
    p.lowHz = lo;
    p.highHz = hi;
    return p;
}

void setAll (Rig& rig, float amount, float smooth, float boost, float cut, float lo = 20.0f, float hi = 20000.0f)
{
    for (const char* s : { "a", "b" })
    {
        const std::string p = std::string ("meq.") + s + ".";
        rig.set (p + "amount", amount);
        rig.set (p + "smooth", smooth);
        rig.set (p + "boost", boost);
        rig.set (p + "cut", cut);
        rig.set (p + "lo", lo);
        rig.set (p + "hi", hi);
    }
}

double maxAbsDiffDelayed (const std::vector<float>& in, const std::vector<float>& out, int latency, bool* exact)
{
    double m = 0.0;
    bool ex = true;
    for (size_t n = 0; n < out.size(); ++n)
    {
        const float ref = n >= (size_t) latency ? in[n - (size_t) latency] : 0.0f;
        const double d = std::abs ((double) out[n] - ref);
        m = std::max (m, d);
        if (out[n] != ref)
            ex = false;
    }
    if (exact != nullptr)
        *exact = ex;
    return m;
}

int expectedLatency (MatchPhase phase, int sizeIndex, double sr)
{
    const int mult = sr > 100000.0 ? 4 : (sr > 50000.0 ? 2 : 1);
    const int n = (4096 << sizeIndex) * mult;
    return (phase == MatchPhase::Linear ? n / 2 : 0) + n / 16;
}

int firTaps (int sizeIndex, double sr)
{
    const int mult = sr > 100000.0 ? 4 : (sr > 50000.0 ? 2 : 1);
    return (4096 << sizeIndex) * mult;
}

std::vector<float> noise (size_t n, std::uint32_t seed, float amp = 0.25f)
{
    pstest::Lcg g (seed);
    std::vector<float> v (n);
    for (auto& x : v)
        x = amp * g.next();
    return v;
}

// A spectrum with independent per-channel curves (for channel-mode tests).
MatchSpectrum spectrumFromCurves (double slopeL, double slopeR, double slopeM, double slopeS, bool sideValid = true)
{
    MatchSpectrum s;
    const double slopes[4] { slopeL, slopeR, slopeM, slopeS };
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < kMatchPoints; ++i)
            s.psdDb[c][(size_t) i] = (c == 3 && ! sideValid) ? kMatchNoValue
                                                             : (float) (-60.0 + slopes[c] * std::log2 (matchGridHz (i) / 1000.0));
    s.valid = true;
    return s;
}
} // namespace

using namespace ps;

//==============================================================================
TEST_CASE ("meq. grid, smoothing width and channel mapping")
{
    CHECK_NEAR (matchGridHz (0), 10.0, 1e-12);
    CHECK_NEAR (matchGridHz (48), 20.0, 1e-12);
    CHECK_NEAR (matchGridHz (kMatchPoints - 1), 10.0 * std::pow (2.0, 539.0 / 48.0), 1e-6);
    CHECK_NEAR (matchSmoothingOctaves (0.0f), 1.0 / 48.0, 1e-12);
    CHECK_NEAR (matchSmoothingOctaves (50.0f), (1.0 / 48.0) * std::sqrt (96.0), 1e-9);
    CHECK_NEAR (matchSmoothingOctaves (100.0f), 2.0, 1e-9);
    pstest::note ("smoothing at 50 % (octaves)", matchSmoothingOctaves (50.0f));
    CHECK (matchChannelFor (ChannelMode::Stereo, 1) == MatchChannel::StereoMean);
    CHECK (matchChannelFor (ChannelMode::TransientSustain, 0) == MatchChannel::StereoMean);
    CHECK (matchChannelFor (ChannelMode::MidSide, 0) == MatchChannel::Mid);
    CHECK (matchChannelFor (ChannelMode::MidSide, 1) == MatchChannel::Side);
    CHECK (matchChannelFor (ChannelMode::LeftRight, 0) == MatchChannel::Left);
    CHECK (matchChannelFor (ChannelMode::LeftRight, 1) == MatchChannel::Right);
    CHECK_EQ (fir::matchFirLatency (MatchPhase::Linear, 16384), 9216);
    CHECK_EQ (fir::matchFirLatency (MatchPhase::Minimum, 16384), 1024);
}

TEST_CASE ("meq. analyser calibration: 1 kHz sine at -20 dBFS peak integrates to -23.01 dB")
{
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto x = pstest::sine (1000.0, -20.0, 6.0, sr);
        const auto s = analyse (x, &x, sr);
        const double l = integratedDb (s, MatchChannel::Left, 900.0, 1100.0);
        const double m = integratedDb (s, MatchChannel::Mid, 900.0, 1100.0);
        pstest::note ("integrated L (dB) at " + std::to_string ((int) sr), l);
        CHECK (s.valid);
        CHECK_NEAR (l, -23.0103, 0.1);
        CHECK_NEAR (m, -23.0103, 0.1);   // L == R: Mid = L
        // identical channels: the Side has no value or sits > 120 dB below
        CHECK (integratedDb (s, MatchChannel::Side, 900.0, 1100.0) < l - 120.0);
        CHECK_NEAR (s.sampleRate, sr, 0.0);

        // the top grid point (23.6 kHz) has a value only below the analysis Nyquist
        const auto wn = noise ((size_t) (2.0 * sr), 5);
        const auto sn = analyse (wn, nullptr, sr);
        if (sr > 50000.0)
            CHECK (sn.valueDb (MatchChannel::Left, kMatchPoints - 1) > kMatchNoValue);
        else
            CHECK (sn.valueDb (MatchChannel::Left, kMatchPoints - 1) == kMatchNoValue);
    }
}

TEST_CASE ("meq. analyser: pink noise slope, M / S sums, silence gate")
{
    const double sr = 48000.0;
    const auto l = pstest::pinkNoise (0.5, 20.0, sr, 11);
    const auto r = pstest::pinkNoise (0.5, 20.0, sr, 977);
    const auto s = analyse (l, &r, sr);
    double dev = 0.0;
    const double slope = slopeDbPerOct (s, MatchChannel::Left, 50.0, 15000.0, &dev);
    pstest::note ("pink slope L 50 Hz - 15 kHz (dB/oct)", slope);
    pstest::note ("max deviation from the fit (dB)", dev);
    pstest::note ("seconds", s.seconds);
    CHECK_NEAR (slope, -3.0, 0.15);
    CHECK (s.valid);
    CHECK_NEAR (s.seconds, 20.0, 0.25);

    // independent L / R of equal power: |M|^2 = |S|^2 = (|L|^2 + |R|^2) / 4, i.e. 3.01 dB below the L / R mean
    double dm = 0.0, ds = 0.0;
    int n = 0;
    for (int i = 0; i < kMatchPoints; ++i)
    {
        const double f = matchGridHz (i);
        if (f < 100.0 || f > 10000.0)
            continue;
        const double lr = s.valueDb (MatchChannel::StereoMean, i);
        dm += s.valueDb (MatchChannel::Mid, i) - lr;
        ds += s.valueDb (MatchChannel::Side, i) - lr;
        ++n;
    }
    pstest::note ("mean Mid - LR (dB)", dm / n);
    pstest::note ("mean Side - LR (dB)", ds / n);
    CHECK_NEAR (dm / n, -3.01, 0.1);
    CHECK_NEAR (ds / n, -3.01, 0.1);

    // 5 s of silence in the middle changes (almost) nothing: compared per 1/3 octave band (the frames that straddle
    // the silence edges hold half their usual energy; everything else is the same audio in the same frames)
    const size_t hop = 8192;
    const size_t half = l.size() / 2 / hop * hop;
    std::vector<float> lg (l.begin(), l.begin() + (long long) half), rg (r.begin(), r.begin() + (long long) half);
    lg.insert (lg.end(), 30 * hop, 0.0f);   // 5.12 s
    rg.insert (rg.end(), 30 * hop, 0.0f);
    lg.insert (lg.end(), l.begin() + (long long) half, l.end());
    rg.insert (rg.end(), r.begin() + (long long) half, r.end());
    const auto g = analyse (lg, &rg, sr);
    double maxDiff = 0.0;
    for (double f = 50.0; f < 15000.0; f *= std::pow (2.0, 1.0 / 3.0))
        maxDiff = std::max (maxDiff, std::abs (integratedDb (g, MatchChannel::Left, f, f * std::pow (2.0, 1.0 / 3.0))
                                               - integratedDb (s, MatchChannel::Left, f, f * std::pow (2.0, 1.0 / 3.0))));
    pstest::note ("silence gap: max change per 1/3 octave 50 Hz - 15 kHz (dB)", maxDiff);
    pstest::note ("silence gap: seconds", g.seconds);
    CHECK (maxDiff < 0.1);
    CHECK_NEAR (g.seconds, s.seconds, 0.35);

    // under 1 s of audio is not a valid spectrum; silence alone gives no value
    const auto shortOne = analyse (std::vector<float> (l.begin(), l.begin() + 30000), nullptr, sr);
    CHECK (! shortOne.valid);
    const auto silent = analyse (std::vector<float> ((size_t) (3.0 * sr), 0.0f), nullptr, sr);
    CHECK (! silent.valid);
    CHECK (silent.valueDb (MatchChannel::Left, 300) == kMatchNoValue);
}

TEST_CASE ("meq. curve: equal spectra, amount 0, invalid or silent sets are exactly flat")
{
    const auto white = makeMatchTiltSpectrum (0.0);
    const auto pink = makeMatchTiltSpectrum (-3.0);
    float curve[kMatchPoints];
    const auto allZero = [&] { return std::all_of (curve, curve + kMatchPoints, [] (float v) { return v == 0.0f; }); };

    computeMatchCurve (pink, pink, MatchChannel::StereoMean, setParams (100.0f, 30.0f), curve);
    CHECK (allZero());
    computeMatchCurve (white, pink, MatchChannel::StereoMean, setParams (0.0f, 30.0f), curve);
    CHECK (allZero());
    MatchSpectrum invalid = white;
    invalid.valid = false;
    computeMatchCurve (invalid, pink, MatchChannel::StereoMean, setParams (100.0f, 30.0f), curve);
    CHECK (allZero());
    computeMatchCurve (white, pink, MatchChannel::Side, setParams (100.0f, 30.0f), curve);   // no Side values
    CHECK (allZero());
    MatchSpectrum quiet = white;
    for (auto& v : quiet.psdDb[0]) v = -170.0f;
    quiet.psdDb[1] = quiet.psdDb[0];
    computeMatchCurve (quiet, pink, MatchChannel::StereoMean, setParams (100.0f, 30.0f), curve);
    CHECK (allZero());   // mean level < -150 dB
    computeMatchCurve (white, pink, MatchChannel::StereoMean, setParams (100.0f, 30.0f, 0.0f, 0.0f), curve);
    CHECK (allZero());   // both limits 0
    computeMatchCurve (white, pink, MatchChannel::StereoMean, setParams (100.0f, 30.0f), curve);
    CHECK (! allZero());
}

TEST_CASE ("meq. curve: white reference vs pink current tilts +3 dB/oct; amount 50 -> +1.5; loudness neutral")
{
    const auto white = makeMatchTiltSpectrum (0.0);
    const auto pink = makeMatchTiltSpectrum (-3.0);
    // limits far away from the curve (the parameter range ends at 24 dB; the curve function accepts more), so the
    // slope is the pure match; with the real 24 dB limits the tanh compresses the ends (checked below)
    for (float amount : { 100.0f, 50.0f })
    {
        float curve[kMatchPoints];
        computeMatchCurve (white, pink, MatchChannel::StereoMean, setParams (amount, 0.0f, 96.0f, 96.0f), curve);
        MatchSpectrum asSpec;
        for (int i = 0; i < kMatchPoints; ++i)
            asSpec.psdDb[0][(size_t) i] = curve[i];
        double dev = 0.0;
        const double slope = slopeDbPerOct (asSpec, MatchChannel::Left, 100.0, 10000.0, &dev);
        pstest::note ("curve slope at amount " + std::to_string ((int) amount), slope);
        CHECK_NEAR (slope, 3.0 * amount / 100.0, amount > 60.0f ? 0.10 : 0.05);

        // level neutral: the K-weighted power of current + curve equals the current's
        float shifted[kMatchPoints];
        for (int i = 0; i < kMatchPoints; ++i)
            shifted[i] = pink.psdDb[0][(size_t) i] + curve[i];
        const double before = matchWeightedLevelDb (pink.psdDb[0].data());
        const double after = matchWeightedLevelDb (shifted);
        pstest::note ("K-weighted level change (dB)", after - before);
        if (amount > 60.0f)
            CHECK_NEAR (after - before, 0.0, 0.05);
    }

    float limited[kMatchPoints];
    computeMatchCurve (white, pink, MatchChannel::StereoMean, setParams (100.0f, 0.0f, 24.0f, 24.0f), limited);
    const auto mm = std::minmax_element (limited, limited + kMatchPoints);
    pstest::note ("24 dB limits: curve min (dB)", *mm.first);
    pstest::note ("24 dB limits: curve max (dB)", *mm.second);
    CHECK (*mm.second < 24.0f && *mm.first > -24.0f);
}

TEST_CASE ("meq. curve: soft limits, region taper, smoothing")
{
    const auto white = makeMatchTiltSpectrum (0.0);
    float curve[kMatchPoints];

    // +20 dB bump (1/12 octave) at 2 kHz, boost 6; -20 dB dip, cut 3
    MatchSpectrum bump = white, dip = white;
    const int i2k = (int) std::lround (48.0 * std::log2 (2000.0 / 10.0));
    for (int i = i2k - 2; i <= i2k + 2; ++i)
        for (int c = 0; c < 3; ++c)
        {
            bump.psdDb[(size_t) c][(size_t) i] += 20.0f;
            dip.psdDb[(size_t) c][(size_t) i] -= 20.0f;
        }
    computeMatchCurve (bump, white, MatchChannel::StereoMean, setParams (100.0f, 0.0f, 6.0f, 24.0f), curve);
    const float maxBoost = *std::max_element (curve, curve + kMatchPoints);
    computeMatchCurve (dip, white, MatchChannel::StereoMean, setParams (100.0f, 0.0f, 24.0f, 3.0f), curve);
    const float maxCut = *std::min_element (curve, curve + kMatchPoints);
    pstest::note ("bump +20 dB, boost limit 6: curve max (dB)", maxBoost);
    pstest::note ("dip -20 dB, cut limit 3: curve min (dB)", maxCut);
    CHECK (maxBoost <= 6.0f && maxBoost > 5.9f);
    CHECK (maxCut >= -3.0f && maxCut < -2.9f);

    // region 200 Hz .. 5 kHz on the tilt case: exactly 0 below 141 Hz and above 7.07 kHz, full inside
    const auto pink = makeMatchTiltSpectrum (-3.0);
    float full[kMatchPoints];
    computeMatchCurve (white, pink, MatchChannel::StereoMean, setParams (100.0f, 0.0f, 24.0f, 24.0f), full);
    computeMatchCurve (white, pink, MatchChannel::StereoMean, setParams (100.0f, 0.0f, 24.0f, 24.0f, 200.0f, 5000.0f), curve);
    double outside = 0.0, insideDiff = 0.0;
    for (int i = 0; i < kMatchPoints; ++i)
    {
        const double f = matchGridHz (i);
        if (f < 200.0 / std::sqrt (2.0) || f > 5000.0 * std::sqrt (2.0))
            outside = std::max (outside, (double) std::abs (curve[i]));
        if (f >= 200.0 && f <= 5000.0)
            insideDiff = std::max (insideDiff, (double) std::abs (curve[i] - full[i]));
    }
    pstest::note ("region: max |curve| outside the taper (dB)", outside);
    CHECK (outside == 0.0);
    CHECK (insideDiff == 0.0);

    // smoothing: a +12 dB spike at one grid point
    MatchSpectrum spike = white;
    for (int c = 0; c < 3; ++c)
        spike.psdDb[(size_t) c][(size_t) i2k] += 12.0f;
    computeMatchCurve (spike, white, MatchChannel::StereoMean, setParams (100.0f, 0.0f), curve);
    const float peak0 = curve[i2k];
    computeMatchCurve (spike, white, MatchChannel::StereoMean, setParams (50.0f, 50.0f), curve);
    const float peak50 = curve[i2k];
    int widthPoints = 0;
    for (int i = 0; i < kMatchPoints; ++i)
        if (curve[i] >= 0.5f * peak50)
            ++widthPoints;
    const double widthOct = widthPoints / 48.0;
    pstest::note ("spike: peak at smooth 0 (dB)", peak0);
    pstest::note ("spike: peak at smooth 50, amount 50 (dB)", peak50);
    pstest::note ("spike: width at half the peak dB, smooth 50 (octaves)", widthOct);
    CHECK_NEAR (peak0, 24.0 * std::tanh (12.0 / 24.0), 0.1);   // the default 24 dB soft limit
    CHECK (peak50 <= 2.1f && peak50 > 1.5f);
    CHECK (widthOct > 0.75 * 0.204 && widthOct < 1.25 * 0.204);
}

TEST_CASE ("meq. spectrum builders: per-bin levels on a foreign linear grid, synthesised tilts")
{
    // a 2049-bin linear grid (k * 48000 / 4098 Hz) holding per-bin powers of pink noise: -3 dB/oct in the amps
    const int bins = 2049;
    const double step = 48000.0 / 4098.0;
    std::vector<float> hz ((size_t) bins), amps ((size_t) bins);
    for (int k = 0; k < bins; ++k)
    {
        hz[(size_t) k] = (float) (std::max (1, k) * step);
        amps[(size_t) k] = (float) (-50.0 - 10.0 * std::log10 (hz[(size_t) k] / 1000.0));   // power per bin ~ 1/f
    }
    MatchSpectrum s;
    setMatchSpectrumFromBins (s, hz.data(), amps.data(), bins, step, "import:Pink");
    double dev = 0.0;
    const double slope = slopeDbPerOct (s, MatchChannel::Left, 100.0, 10000.0, &dev);
    pstest::note ("imported pink: slope 100 Hz - 10 kHz (dB/oct)", slope);
    pstest::note ("imported pink: max deviation (dB)", dev);
    CHECK_NEAR (slope, -3.0, 0.05);
    CHECK (s.valid);
    CHECK (s.valueDb (MatchChannel::Side, 300) == kMatchNoValue);
    CHECK (std::string (s.source.data()) == "import:Pink");
    CHECK (s.valueDb (MatchChannel::Mid, 300) == s.valueDb (MatchChannel::Left, 300));
    // PSD conversion: the level at 1 kHz is amps - 10 log10 (bin width)
    const int i1k = 48 * 7 - 2;   // ~ 1 kHz: 10 * 2^(334/48)
    const double f1 = matchGridHz (i1k);
    CHECK_NEAR (s.valueDb (MatchChannel::Left, i1k), -50.0 - 10.0 * std::log10 (f1 / 1000.0) - 10.0 * std::log10 (step), 0.2);
    // nothing below the first bin / above the last
    CHECK (s.valueDb (MatchChannel::Left, 0) == kMatchNoValue);
    CHECK (s.valueDb (MatchChannel::Left, kMatchPoints - 1) == kMatchNoValue);

    const auto brown = makeMatchTiltSpectrum (-6.0, 0.0, 60.0, "synth:brown");
    CHECK_NEAR (slopeDbPerOct (brown, MatchChannel::StereoMean, 20.0, 20000.0), -6.0, 1e-4);
    const auto shelf = makeMatchTiltSpectrum (-2.0, -2.0, 60.0);
    CHECK_NEAR (shelf.valueDb (MatchChannel::Left, 0) - (-60.0 - 2.0 * std::log2 (0.01)), -2.0 / (1.0 + 1.0 / 36.0), 0.01);
}

//==============================================================================
TEST_CASE ("meq. neutral: no spectra, amount 0 and reference == current are exact delayed nulls")
{
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int phase = 0; phase < 2; ++phase)
            for (int size = 0; size < 4; ++size)
            {
                Rig rig (sr, 512);
                rig.set ("meq.phase", (float) phase);
                rig.set ("meq.firSize", (float) size);
                rig.apply();
                const int lat = rig.eq.getLatencySamples();
                CHECK_EQ (lat, expectedLatency ((MatchPhase) phase, size, sr));
                REQUIRE (rig.settle());

                const auto inL = noise ((size_t) (0.25 * sr) + (size_t) lat, 3), inR = noise ((size_t) (0.25 * sr) + (size_t) lat, 4);
                auto l = inL, r = inR;
                rig.run (l, r);
                bool exL = false, exR = false;
                maxAbsDiffDelayed (inL, l, lat, &exL);
                maxAbsDiffDelayed (inR, r, lat, &exR);
                CHECK (exL && exR);
                if (! (exL && exR))
                    pstest::note ("not exact: rate / phase / size", sr * 100 + phase * 10 + size);
            }

    // with spectra: amount 0, and reference == current (Linear 16384 and Minimum 4096 at 48 kHz)
    for (int phase = 0; phase < 2; ++phase)
    {
        Rig rig (48000.0, 256);
        rig.set ("meq.phase", (float) phase);
        rig.set ("meq.firSize", phase == 0 ? 2.0f : 0.0f);
        const int lat = expectedLatency ((MatchPhase) phase, phase == 0 ? 2 : 0, 48000.0);
        rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (0.0));
        rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
        setAll (rig, 0.0f, 20.0f, 12.0f, 12.0f);
        REQUIRE (rig.settle());
        auto inL = noise (30000, 5), inR = noise (30000, 6);
        auto l = inL, r = inR;
        rig.run (l, r);
        bool ex0 = false;
        maxAbsDiffDelayed (inL, l, lat, &ex0);
        CHECK (ex0);

        rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (0.0));
        setAll (rig, 100.0f, 20.0f, 12.0f, 12.0f);
        REQUIRE (rig.settle());
        l = inL;
        r = inR;
        rig.run (l, r);
        bool ex1 = false;
        maxAbsDiffDelayed (inL, l, lat, &ex1);
        CHECK (ex1);

        // a real curve is not a null (the test detects one)
        rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
        REQUIRE (rig.settle());
        l = inL;
        r = inR;
        rig.run (l, r);
        bool ex2 = true;
        const double d = maxAbsDiffDelayed (inL, l, lat, &ex2);
        pstest::note ("with a curve: max |out - delayed in|", d);
        CHECK (! ex2 && d > 1e-3);
        CHECK (rig.eq.getLatencySamples() == lat);

        // clear -> exact again
        rig.eq.clearSpectrum (MatchRole::Reference);
        REQUIRE (rig.settle());
        l = inL;
        r = inR;
        rig.run (l, r);
        bool ex3 = false;
        maxAbsDiffDelayed (inL, l, lat, &ex3);
        CHECK (ex3);
        CHECK (rig.eq.getLatencySamples() == lat);
    }
}

TEST_CASE ("meq. latency == impulse delay: linear symmetric about D + P, minimum causal at P (all sizes and rates)")
{
    const auto white = makeMatchTiltSpectrum (0.0);
    const auto pink = makeMatchTiltSpectrum (-3.0);
    double worstAsym = -400.0, worstEnergy = 1.0, worstMagDiff = 0.0;
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (int size = 0; size < 4; ++size)
        {
            std::vector<float> irs[2];
            for (int phase = 0; phase < 2; ++phase)
            {
                Rig rig (sr, 1024);
                rig.set ("meq.phase", (float) phase);
                rig.set ("meq.firSize", (float) size);
                setAll (rig, 60.0f, 40.0f, 12.0f, 12.0f);
                rig.eq.setSpectrum (MatchRole::Reference, white);
                rig.eq.setSpectrum (MatchRole::Current, pink);
                rig.apply();
                const int lat = rig.eq.getLatencySamples();
                const int n = firTaps (size, sr);
                CHECK_EQ (lat, expectedLatency ((MatchPhase) phase, size, sr));
                REQUIRE (rig.settle());
                const auto y = rig.impulse (lat + n + 4096);
                irs[phase] = y;
                const int p = n / 16;

                if (phase == 0)
                {
                    // symmetric about lat (= D + P)
                    double peak = 0.0, asym = 0.0;
                    for (int m = 0; m <= n / 2; ++m)
                    {
                        peak = std::max (peak, (double) std::abs (y[(size_t) (lat + m)]));
                        asym = std::max (asym, (double) std::abs (y[(size_t) (lat + m)] - y[(size_t) (lat - m)]));
                    }
                    const double asymDb = 20.0 * std::log10 (std::max (asym, 1e-30) / peak);
                    worstAsym = std::max (worstAsym, asymDb);
                    CHECK (asymDb < -100.0);
                    // nothing before the kernel starts (lat - D = P) and after it ends
                    double outside = 0.0;
                    for (int k = 0; k < p; ++k)
                        outside = std::max (outside, (double) std::abs (y[(size_t) k]));
                    for (size_t k = (size_t) (lat + n / 2); k < y.size(); ++k)
                        outside = std::max (outside, (double) std::abs (y[k]));
                    CHECK (outside < 1.0e-6);   // float convolver rounding only
                }
                else
                {
                    // causal: exactly zero before P, then front-loaded
                    double before = 0.0, early = 0.0, total = 0.0;
                    for (int k = 0; k < lat; ++k)
                        before = std::max (before, (double) std::abs (y[(size_t) k]));
                    for (int k = lat; k < lat + n; ++k)
                    {
                        const double e = (double) y[(size_t) k] * y[(size_t) k];
                        total += e;
                        if (k < lat + n / 8)
                            early += e;
                    }
                    CHECK (before == 0.0);
                    worstEnergy = std::min (worstEnergy, early / total);
                    CHECK (early / total >= 0.9);
                    CHECK (std::abs (y[(size_t) lat]) > 0.5f);   // the main tap right at the latency
                }
            }

            // magnitudes of both phases agree (smooth curve)
            const double fLo = std::max (100.0, 8.0 * 5.7 * sr / firTaps (size, sr));
            for (double f = fLo; f < 18000.0; f *= 1.07)
                worstMagDiff = std::max (worstMagDiff, std::abs (responseDb (irs[0], f, sr) - responseDb (irs[1], f, sr)));
        }
    pstest::note ("linear: worst asymmetry re peak (dB)", worstAsym);
    pstest::note ("minimum: worst energy share in the first N/8 taps", worstEnergy);
    pstest::note ("linear vs minimum: worst magnitude difference (dB)", worstMagDiff);
    CHECK (worstMagDiff < 0.05);
}

TEST_CASE ("meq. realised FIR response follows the curve (both phases, every FIR size, 48 / 96 kHz)")
{
    const auto white = makeMatchTiltSpectrum (0.0);
    auto ref = makeMatchTiltSpectrum (-3.0);
    // a broad +4 dB presence bump in the reference
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < kMatchPoints; ++i)
        {
            const double oct = std::log2 (matchGridHz (i) / 3000.0);
            ref.psdDb[(size_t) c][(size_t) i] += (float) (4.0 * std::exp (-oct * oct / 0.5));
        }
    for (double sr : { 48000.0, 96000.0 })
        for (int phase = 0; phase < 2; ++phase)
            for (int size = 0; size < 4; ++size)
            {
                Rig rig (sr, 1024);
                rig.set ("meq.phase", (float) phase);
                rig.set ("meq.firSize", (float) size);
                setAll (rig, 80.0f, 50.0f, 12.0f, 12.0f, 40.0f, 16000.0f);
                rig.eq.setSpectrum (MatchRole::Reference, ref);
                rig.eq.setSpectrum (MatchRole::Current, white);
                REQUIRE (rig.settle());
                const int lat = rig.eq.getLatencySamples();
                const auto y = rig.impulse (lat + firTaps (size, sr) + 1024);

                float curve[kMatchPoints];
                computeMatchCurve (ref, white, MatchChannel::StereoMean, setParams (80.0f, 50.0f, 12.0f, 12.0f, 40.0f, 16000.0f), curve);
                // resolvable from ~8 window main lobes (5.7 fs / N each) up
                const double fLo = std::max (60.0, 8.0 * 5.7 * sr / firTaps (size, sr));
                double worst = 0.0;
                for (double f = fLo; f < 20000.0; f *= 1.05)
                    worst = std::max (worst, std::abs (responseDb (y, f, sr) - fir::curveDbAt (curve, f)));
                pstest::note ("max |response - curve| (dB), rate " + std::to_string ((int) sr) + " phase " + std::to_string (phase)
                                  + " size " + std::to_string (size) + " from " + std::to_string ((int) fLo) + " Hz",
                              worst);
                CHECK (worst < 0.1);
                const auto& meters = rig.eq.getMeters();
                CHECK_NEAR (meters.curveMaxDb[0].load(), *std::max_element (curve, curve + kMatchPoints), 1e-4);
                CHECK_NEAR (meters.curveMinDb[0].load(), std::min (0.0f, *std::min_element (curve, curve + kMatchPoints)), 1e-4);
            }
}

TEST_CASE ("meq. realised limits, region and smoothing (FIR 16384, 48 kHz)")
{
    const double sr = 48000.0;
    const auto white = makeMatchTiltSpectrum (0.0);
    const auto pink = makeMatchTiltSpectrum (-3.0);
    const int i2k = (int) std::lround (48.0 * std::log2 (2000.0 / 10.0));
    const auto measure = [&] (const MatchSpectrum& ref, const MatchSpectrum& cur, float amount, float smooth, float boost,
                              float cut, float lo, float hi)
    {
        Rig rig (sr, 1024);
        setAll (rig, amount, smooth, boost, cut, lo, hi);
        rig.eq.setSpectrum (MatchRole::Reference, ref);
        rig.eq.setSpectrum (MatchRole::Current, cur);
        REQUIRE (rig.settle());
        return rig.impulse (rig.eq.getLatencySamples() + 16384 + 1024);
    };

    MatchSpectrum bump = white, dip = white;
    for (int i = i2k - 2; i <= i2k + 2; ++i)
        for (int c = 0; c < 3; ++c)
        {
            bump.psdDb[(size_t) c][(size_t) i] += 20.0f;
            dip.psdDb[(size_t) c][(size_t) i] -= 20.0f;
        }
    auto y = measure (bump, white, 100.0f, 0.0f, 6.0f, 24.0f, 20.0f, 20000.0f);
    double maxGain = -100.0;
    for (double f = 1500.0; f < 2700.0; f += 2.0)
        maxGain = std::max (maxGain, responseDb (y, f, sr));
    y = measure (dip, white, 100.0f, 0.0f, 24.0f, 3.0f, 20.0f, 20000.0f);
    double minGain = 100.0;
    for (double f = 1500.0; f < 2700.0; f += 2.0)
        minGain = std::min (minGain, responseDb (y, f, sr));
    pstest::note ("realised: max gain with boost limit 6 (dB)", maxGain);
    pstest::note ("realised: min gain with cut limit 3 (dB)", minGain);
    CHECK (maxGain <= 6.05 && maxGain > 5.5);
    CHECK (minGain >= -3.05 && minGain < -2.7);

    y = measure (white, pink, 100.0f, 0.0f, 24.0f, 24.0f, 200.0f, 5000.0f);
    double lowWorst = 0.0, highWorst = 0.0;
    for (double f = 20.0; f <= 125.0; f *= 1.02)
        lowWorst = std::max (lowWorst, std::abs (responseDb (y, f, sr)));
    for (double f = 7500.0; f <= 20000.0; f *= 1.02)
        highWorst = std::max (highWorst, std::abs (responseDb (y, f, sr)));
    pstest::note ("region 200 Hz - 5 kHz: max |gain| 20 - 125 Hz (dB)", lowWorst);
    pstest::note ("region 200 Hz - 5 kHz: max |gain| 7.5 - 20 kHz (dB)", highWorst);
    pstest::note ("region: gain at 140 Hz (dB)", responseDb (y, 140.0, sr));
    pstest::note ("region: gain at 7.1 kHz (dB)", responseDb (y, 7100.0, sr));
    CHECK (lowWorst < 0.1 && highWorst < 0.1);

    MatchSpectrum spike = white;
    for (int c = 0; c < 3; ++c)
        spike.psdDb[(size_t) c][(size_t) i2k] += 12.0f;
    float curve[kMatchPoints];
    computeMatchCurve (spike, white, MatchChannel::StereoMean, setParams (100.0f, 0.0f), curve);
    y = measure (spike, white, 100.0f, 0.0f, 24.0f, 24.0f, 20.0f, 20000.0f);
    double peak = -100.0;
    for (double f = 1900.0; f < 2100.0; f += 0.5)
        peak = std::max (peak, responseDb (y, f, sr));
    pstest::note ("single-point +12 dB spike, smooth 0: realised peak (dB)", peak);
    pstest::note ("curve peak (dB)", curve[i2k]);
    CHECK (std::abs (peak - curve[i2k]) < 1.0);
}

TEST_CASE ("meq. live capture: pink noise matched to white / brown / tilted references (realised within 0.5 dB)")
{
    const double sr = 48000.0;
    const auto pinkIdeal = makeMatchTiltSpectrum (-3.0);
    for (double refSlope : { 0.0, -6.0, -4.5 })
    {
        Rig rig (sr, 512);
        setAll (rig, 100.0f, 50.0f, 24.0f, 24.0f);
        const auto ref = makeMatchTiltSpectrum (refSlope);
        rig.eq.setSpectrum (MatchRole::Reference, ref);

        // capture the current spectrum live: 20 s of pink noise through the module (paced so the FIFO never overflows)
        rig.eq.startCapture (MatchRole::Current);
        MatchRole role = MatchRole::Reference;
        CHECK (rig.eq.isCapturing (&role) && role == MatchRole::Current);
        auto l = pstest::pinkNoise (0.5, 20.0, sr, 21);
        auto r = pstest::pinkNoise (0.5, 20.0, sr, 22);
        const unsigned genBefore = rig.eq.getSpectrumGeneration();
        for (size_t pos = 0; pos < l.size(); pos += 8192)
        {
            const size_t n = std::min ((size_t) 8192, l.size() - pos);
            std::vector<float> bl (l.begin() + (long long) pos, l.begin() + (long long) (pos + n));
            std::vector<float> br (r.begin() + (long long) pos, r.begin() + (long long) (pos + n));
            rig.run (bl, br);
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (300));
        MatchSpectrum runningSpec;
        CHECK (rig.eq.getRunningSpectrum (runningSpec));
        CHECK (rig.eq.getCaptureSeconds() > 10.0);
        CHECK_EQ (rig.eq.getSpectrumGeneration(), genBefore);   // a running capture is not a spectrum change
        rig.eq.stopCapture();
        CHECK (! rig.eq.isCapturing());
        CHECK_EQ (rig.eq.getSpectrumGeneration(), genBefore + 1u);
        CHECK_EQ (rig.eq.getCaptureGaps(), 0);
        CHECK (! rig.eq.getRunningSpectrum (runningSpec));
        const auto cur = rig.eq.getSpectrum (MatchRole::Current);
        CHECK (cur.valid);
        CHECK (std::string (cur.source.data()) == "capture");
        pstest::note ("captured seconds", cur.seconds);
        pstest::note ("captured pink slope (dB/oct)", slopeDbPerOct (cur, MatchChannel::StereoMean, 50.0, 15000.0));

        REQUIRE (rig.settle());
        const auto y = rig.impulse (rig.eq.getLatencySamples() + 16384 + 1024);

        // expected: (reference - ideal pink) normalised to equal K-weighted level, soft-limited at 24 dB
        const double offset = matchWeightedLevelDb (ref.psdDb[0].data()) - matchWeightedLevelDb (pinkIdeal.psdDb[0].data());
        double worst = 0.0;
        for (double f = 100.0; f <= 10000.0; f *= 1.03)
        {
            const double d = (refSlope + 3.0) * std::log2 (f / 1000.0) - offset;
            const double expected = 24.0 * std::tanh (d / 24.0);
            worst = std::max (worst, std::abs (responseDb (y, f, sr) - expected));
        }
        pstest::note ("reference slope " + std::to_string (refSlope) + ": max |realised - expected| 100 Hz - 10 kHz (dB)", worst);
        CHECK (worst < 0.5);
    }
}

TEST_CASE ("meq. loudness neutrality: K-weighted power of pink noise through the match within 0.2 dB")
{
    const double sr = 48000.0;
    Rig rig (sr, 512);
    setAll (rig, 100.0f, 0.0f, 24.0f, 24.0f);
    rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (0.0));
    rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
    REQUIRE (rig.settle());
    const auto inL = pstest::pinkNoise (0.3, 12.0, sr, 31), inR = pstest::pinkNoise (0.3, 12.0, sr, 32);
    auto l = inL, r = inR;
    rig.run (l, r);
    const int lat = rig.eq.getLatencySamples();

    // K-weighted power over 20 Hz .. 20 kHz (the band the curve normalisation uses; pink noise carries ~0.26 octave
    // above it, where the match boosts), from averaged periodograms (independent double FFT, core K-weighting)
    const auto kPower = [&] (const std::vector<float>& x, size_t start)
    {
        const size_t n = 32768;
        double p = 0.0;
        for (size_t seg = start + 48000; seg + n <= x.size(); seg += n / 2)
        {
            std::vector<cd> a (n);
            for (size_t i = 0; i < n; ++i)
                a[i] = (0.5 - 0.5 * std::cos (kTwoPi * (double) i / (double) n)) * (double) x[seg + i];
            pstest::fftInPlace (a);
            for (size_t k = 1; k < n / 2; ++k)
            {
                const double f = (double) k * sr / (double) n;
                if (f >= 20.0 && f <= 20000.0)
                    p += std::norm (a[k]) * std::pow (10.0, kWeightingResponseDb (f, sr) / 10.0);
            }
        }
        return 10.0 * std::log10 (p);
    };
    const auto trimmed = [&] (const std::vector<float>& x) { return std::vector<float> (x.begin(), x.end() - lat); };
    const double diff = (kPower (l, (size_t) lat) - kPower (trimmed (inL), 0) + kPower (r, (size_t) lat) - kPower (trimmed (inR), 0)) / 2.0;
    pstest::note ("K-weighted level change 20 Hz - 20 kHz through the match (dB)", diff);
    CHECK (std::abs (diff) < 0.2);
}

//==============================================================================
TEST_CASE ("meq. channel modes: M/S and L/R use their own curves, mono reference keeps set B flat, T/S twin reads set B")
{
    const double sr = 48000.0;
    const auto flatWorst = [&] (const std::vector<float>& y, int lat)
    {
        (void) lat;
        double w = 0.0;
        for (double f = 30.0; f < 20000.0; f *= 1.1)
            w = std::max (w, std::abs (responseDb (y, f, sr)));
        return w;
    };
    const auto tilt = [&] (const std::vector<float>& y) { return responseDb (y, 8000.0, sr) - responseDb (y, 125.0, sr); };

    // M/S: Side of the reference 6 dB brighter (+1 dB/oct over 6 octaves) than the current's
    {
        Rig rig (sr, 512);
        rig.mode (ChannelMode::MidSide);
        setAll (rig, 100.0f, 30.0f, 24.0f, 24.0f);
        rig.eq.setSpectrum (MatchRole::Reference, spectrumFromCurves (-3.0, -3.0, -3.0, -2.0));
        rig.eq.setSpectrum (MatchRole::Current, spectrumFromCurves (-3.0, -3.0, -3.0, -3.0));
        REQUIRE (rig.settle());
        const int lat = rig.eq.getLatencySamples();
        const auto mid = rig.impulse (lat + 17408, 0, 0);
        const auto side = rig.impulse (lat + 17408, 1, 1);
        pstest::note ("M/S: set A (Mid) max |gain| (dB)", flatWorst (mid, lat));
        pstest::note ("M/S: set B (Side) tilt 125 Hz -> 8 kHz (dB)", tilt (side));
        CHECK (flatWorst (mid, lat) < 0.1);
        CHECK_NEAR (tilt (side), 6.0, 0.2);

        // a mono reference (no Side values): set B flat
        auto mono = spectrumFromCurves (-3.0, -3.0, -3.0, -2.0, false);
        rig.eq.setSpectrum (MatchRole::Reference, mono);
        REQUIRE (rig.settle());
        const auto side2 = rig.impulse (lat + 17408, 1, 1);
        pstest::note ("mono reference: set B max |gain| (dB)", flatWorst (side2, lat));
        CHECK (flatWorst (side2, lat) < 1e-6);
    }

    // L/R: set A Left, set B Right; set B has amount 0
    {
        Rig rig (sr, 512);
        rig.mode (ChannelMode::LeftRight);
        setAll (rig, 100.0f, 30.0f, 24.0f, 24.0f);
        rig.set ("meq.b.amount", 0.0f);
        rig.eq.setSpectrum (MatchRole::Reference, spectrumFromCurves (-2.0, -2.0, -2.0, -2.0));
        rig.eq.setSpectrum (MatchRole::Current, spectrumFromCurves (-3.0, -3.0, -3.0, -3.0));
        REQUIRE (rig.settle());
        const int lat = rig.eq.getLatencySamples();
        const auto left = rig.impulse (lat + 17408, 0, 0);
        const auto right = rig.impulse (lat + 17408, 1, 1);
        pstest::note ("L/R: left tilt (dB)", tilt (left));
        pstest::note ("L/R: right max |gain| (dB)", flatWorst (right, lat));
        CHECK_NEAR (tilt (left), 6.0, 0.2);
        CHECK (flatWorst (right, lat) < 1e-6);
        const auto& m = rig.eq.getMeters();
        CHECK (m.curveMaxDb[0].load() > 1.0f);
        CHECK (m.curveMaxDb[1].load() == 0.0f && m.curveMinDb[1].load() == 0.0f);

        // Stereo: both channels use set A on the L/R power mean
        rig.mode (ChannelMode::Stereo);
        REQUIRE (rig.settle());
        const auto st = rig.impulse (lat + 17408, 1, 1);
        CHECK_NEAR (tilt (st), 6.0, 0.2);
    }

    // T/S twin: paramSet 1 reads set B for both channels
    {
        Rig normal (sr, 512, 0), twin (sr, 512, 1);
        for (Rig* rg : { &normal, &twin })
        {
            setAll (*rg, 100.0f, 30.0f, 24.0f, 24.0f);
            rg->set ("meq.a.amount", 0.0f);
            rg->ctx.channelMode = ChannelMode::Stereo;
            rg->set ("meq.chMode", 3.0f);   // T/S: the instances get Stereo
            rg->eq.setSpectrum (MatchRole::Reference, spectrumFromCurves (-2.0, -2.0, -2.0, -2.0));
            rg->eq.setSpectrum (MatchRole::Current, spectrumFromCurves (-3.0, -3.0, -3.0, -3.0));
            REQUIRE (rg->settle());
        }
        const int lat = normal.eq.getLatencySamples();
        CHECK_EQ (twin.eq.getLatencySamples(), lat);
        const auto a = normal.impulse (lat + 17408, -1, 1);
        const auto b = twin.impulse (lat + 17408, -1, 1);
        pstest::note ("T/S: normal (set A amount 0) max |gain| (dB)", flatWorst (a, lat));
        pstest::note ("T/S: twin (set B amount 100) tilt (dB)", tilt (b));
        CHECK (flatWorst (a, lat) < 1e-6);
        CHECK_NEAR (tilt (b), 6.0, 0.2);
    }
}

TEST_CASE ("meq. audition: band pass of the aligned input, chosen set only in M/S")
{
    const double sr = 48000.0;
    Rig rig (sr, 512);
    rig.mode (ChannelMode::MidSide);
    rig.apply();
    ModuleMonitor mon;
    mon.auditionOn = true;
    mon.auditionHz = 1000.0f;
    mon.auditionQ = 4.0f;
    mon.auditionChannelSet = 1;
    rig.eq.setMonitor (mon);
    auto tone1k = pstest::sine (1000.0, -6.0, 1.0, sr);
    auto tone100 = pstest::sine (100.0, -6.0, 1.0, sr);
    auto l = tone1k, r = tone1k;
    rig.run (l, r);
    const size_t start = 24000;
    const double passB = std::abs (pstest::toneOf (r, start, 19200, 1000.0, sr)) / pstest::dbfsToAmplitude (-6.0);
    const double mutedA = pstest::rmsOf (l, start, 19200);
    l = tone100;
    r = tone100;
    rig.run (l, r);
    const double rejB = std::abs (pstest::toneOf (r, start, 19200, 100.0, sr)) / pstest::dbfsToAmplitude (-6.0);
    pstest::note ("audition 1 kHz Q 4: gain at 1 kHz (dB)", 20.0 * std::log10 (passB));
    pstest::note ("audition 1 kHz Q 4: gain at 100 Hz (dB)", 20.0 * std::log10 (rejB));
    CHECK_NEAR (20.0 * std::log10 (passB), 0.0, 0.1);
    CHECK (20.0 * std::log10 (rejB) < -20.0);
    CHECK (mutedA < 1e-6);

    // audition off: fades back (10 ms) to the exact delayed signal
    rig.eq.setMonitor (ModuleMonitor {});
    const auto in = noise (96000, 9);
    l = in;
    r = in;
    rig.run (l, r);
    const size_t lat = (size_t) rig.eq.getLatencySamples();
    bool exact = true;
    for (size_t n = lat + 1000; n < l.size(); ++n)
        exact = exact && l[n] == in[n - lat] && r[n] == in[n - lat];
    CHECK (exact);
}

//==============================================================================
TEST_CASE ("meq. kernel swaps: Amount automated 0 <-> 100 every 100 ms on a 1 kHz sine, no steps or splatter")
{
    const double sr = 48000.0;
    Rig rig (sr, 256);
    setAll (rig, 0.0f, 40.0f, 12.0f, 12.0f);
    auto ref = makeMatchTiltSpectrum (-3.0);
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < kMatchPoints; ++i)
        {
            const double oct = std::log2 (matchGridHz (i) / 1000.0);
            ref.psdDb[(size_t) c][(size_t) i] += (float) (8.0 * std::exp (-oct * oct / 0.3));   // +8 dB around 1 kHz
        }
    rig.eq.setSpectrum (MatchRole::Reference, ref);
    rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
    REQUIRE (rig.settle());

    const double seconds = 3.0;
    const auto x = pstest::sine (1000.0, -6.0, seconds, sr);
    auto l = x, r = x;
    const int blocksPer100ms = (int) (0.1 * sr / 256);
    int block = 0, toggles = 0;
    for (size_t pos = 0; pos < l.size(); pos += 256, ++block)
    {
        if (block % blocksPer100ms == 0)
        {
            setAll (rig, (toggles++ % 2) == 0 ? 100.0f : 0.0f, 40.0f, 12.0f, 12.0f);
            std::this_thread::sleep_for (std::chrono::milliseconds (20));   // give the worker realtime-like room
        }
        const int n = (int) std::min ((size_t) 256, l.size() - pos);
        rig.apply();
        float* p[2] { l.data() + pos, r.data() + pos };
        rig.eq.process (p, n, rig.ctx);
    }

    // sample-step detector: second difference against a sine's own (A w^2), with the gain range of the run
    const double w = kTwoPi * 1000.0 / sr;
    double maxD2 = 0.0, maxAmp = 0.0, minAmp = 10.0;
    const size_t skip = (size_t) rig.eq.getLatencySamples() + 4800;
    for (size_t n = skip; n < l.size(); ++n)
        maxD2 = std::max (maxD2, std::abs ((double) l[n] - 2.0 * l[n - 1] + l[n - 2]));
    for (size_t n = skip; n + 480 < l.size(); n += 480)
    {
        const double a = std::abs (pstest::toneOf (l, n, 480, 1000.0, sr));
        maxAmp = std::max (maxAmp, a);
        minAmp = std::min (minAmp, a);
    }
    pstest::note ("toggles", toggles);
    pstest::note ("1 kHz level range (dB)", 20.0 * std::log10 (maxAmp / minAmp));
    pstest::note ("max |2nd difference| / (A_max w^2)", maxD2 / (maxAmp * w * w));
    CHECK (20.0 * std::log10 (maxAmp / minAmp) > 3.0);   // the swaps really happened
    CHECK (maxD2 / (maxAmp * w * w) < 1.05);

    // splatter: spectrum of the output, energy away from 1 kHz
    const size_t fftN = 65536;
    std::vector<cd> a (fftN);
    for (size_t i = 0; i < fftN; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos (kTwoPi * (double) i / (double) fftN);
        a[i] = win * (double) l[skip + i];
    }
    pstest::fftInPlace (a);
    double peak = 0.0, far = 0.0;
    for (size_t k = 1; k < fftN / 2; ++k)
    {
        const double f = (double) k * sr / (double) fftN;
        const double m = std::abs (a[k]);
        peak = std::max (peak, m);
        if (f > 3000.0)
            far = std::max (far, m);
    }
    pstest::note ("splatter above 3 kHz re the tone (dB)", 20.0 * std::log10 (far / peak));
    CHECK (20.0 * std::log10 (far / peak) < -100.0);
}

TEST_CASE ("meq. phase / FIR size switch: fades, exact latency afterwards, curve rejoins without a step")
{
    const double sr = 48000.0;
    for (bool withCurve : { false, true })
    {
        Rig rig (sr, 256);
        setAll (rig, withCurve ? 60.0f : 0.0f, 40.0f, 12.0f, 12.0f);
        rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (-2.0));
        rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
        REQUIRE (rig.settle());
        const int lat0 = rig.eq.getLatencySamples();

        const auto x = pstest::sine (1000.0, -6.0, 3.0, sr);
        auto l = x, r = x;
        const size_t switchAt = 24064;   // a block boundary (94 x 256)
        int lat1 = 0;
        for (size_t pos = 0; pos < l.size(); pos += 256)
        {
            if (pos == switchAt)
            {
                rig.set ("meq.phase", 1.0f);
                rig.set ("meq.firSize", 0.0f);
                rig.apply();
                lat1 = rig.eq.getLatencySamples();
            }
            const int n = (int) std::min ((size_t) 256, l.size() - pos);
            rig.apply();
            float* p[2] { l.data() + pos, r.data() + pos };
            rig.eq.process (p, n, rig.ctx);
            if (pos % 4096 == 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        CHECK_EQ (lat0, 9216);
        CHECK_EQ (lat1, 256);

        const double w = kTwoPi * 1000.0 / sr;
        const double amp = pstest::dbfsToAmplitude (-6.0) * 4.0;   // generous: up to +12 dB gain
        double maxD2 = 0.0;
        for (size_t n = (size_t) lat0 + 2; n < l.size(); ++n)
            maxD2 = std::max (maxD2, std::abs ((double) l[n] - 2.0 * l[n - 1] + l[n - 2]));
        pstest::note (std::string (withCurve ? "with curve" : "flat") + ": max |2nd diff| / (4 A w^2)", maxD2 / (amp * w * w));
        CHECK (maxD2 / (amp * w * w) < 1.0);

        // the output dips to silence around the switch (4 ms fades)
        double minAbs = 1.0;
        for (size_t n = switchAt; n < switchAt + 2048; ++n)
            minAbs = std::min (minAbs, (double) std::abs (pstest::toneOf (l, n, 48, 1000.0, sr)));
        CHECK (minAbs < 0.05);

        if (! withCurve)
        {
            // flat: exact delayed null at the new latency after the fade-in
            bool exact = true;
            for (size_t n = switchAt + 2048; n < l.size(); ++n)
                exact = exact && l[n] == x[n - (size_t) lat1];
            CHECK (exact);
        }
        else
        {
            // the curve rejoins: the 1 kHz gain at the end equals the curve
            float curve[kMatchPoints];
            computeMatchCurve (makeMatchTiltSpectrum (-2.0), makeMatchTiltSpectrum (-3.0), MatchChannel::StereoMean,
                               setParams (60.0f, 40.0f, 12.0f, 12.0f), curve);
            const double g = 20.0 * std::log10 (std::abs (pstest::toneOf (l, l.size() - 9600, 9600, 1000.0, sr))
                                                / pstest::dbfsToAmplitude (-6.0));
            pstest::note ("final 1 kHz gain (dB)", g);
            pstest::note ("curve at 1 kHz (dB)", fir::curveDbAt (curve, 1000.0));
            CHECK_NEAR (g, fir::curveDbAt (curve, 1000.0), 0.05);
        }
    }
}

//==============================================================================
TEST_CASE ("meq. capture: FIFO + worker, calibration, M/S decode, gaps, generation counter")
{
    const double sr = 48000.0;
    const auto x = pstest::sine (1000.0, -20.0, 6.0, sr);
    MatchSpectrum stereoCap, msCap;
    for (int pass = 0; pass < 2; ++pass)
    {
        Rig rig (sr, 512);
        std::vector<float> l = x, r = x;
        if (pass == 1)
        {
            rig.mode (ChannelMode::MidSide);
            for (size_t i = 0; i < l.size(); ++i)   // the module sees M / S
            {
                const float m = 0.5f * (x[i] + x[i]), s = 0.0f;
                l[i] = m;
                r[i] = s;
            }
        }
        rig.apply();
        const unsigned g0 = rig.eq.getSpectrumGeneration();
        rig.eq.startCapture (MatchRole::Reference);
        CHECK_EQ (rig.eq.getSpectrumGeneration(), g0);
        long long allocations = 0;
        for (size_t pos = 0; pos < l.size(); pos += 512)
        {
            const int n = (int) std::min ((size_t) 512, l.size() - pos);
            rig.apply();
            float* p[2] { l.data() + pos, r.data() + pos };
            {
                pstest::AllocationScope scope;
                rig.eq.process (p, n, rig.ctx);
                allocations += scope.count();
            }
            if (pos % 8192 == 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        CHECK_EQ (allocations, 0LL);
        rig.eq.stopCapture();
        CHECK_EQ (rig.eq.getSpectrumGeneration(), g0 + 1u);
        CHECK_EQ (rig.eq.getCaptureGaps(), 0);
        (pass == 0 ? stereoCap : msCap) = rig.eq.getSpectrum (MatchRole::Reference);
    }
    const double lvl = integratedDb (stereoCap, MatchChannel::Left, 900.0, 1100.0);
    pstest::note ("live capture: 1 kHz -20 dBFS sine integrates to (dB)", lvl);
    CHECK_NEAR (lvl, -23.0103, 0.1);
    double worst = 0.0;
    for (int i = 0; i < kMatchPoints; ++i)
        if (stereoCap.valueDb (MatchChannel::Left, i) > -120.0f)
            worst = std::max (worst, (double) std::abs (stereoCap.valueDb (MatchChannel::Left, i) - msCap.valueDb (MatchChannel::Left, i)));
    pstest::note ("M/S capture (decoded) vs Stereo capture: max difference (dB)", worst);
    CHECK (worst < 0.01);

    // a block that cannot fit into the FIFO (2^17) is dropped and counted as one gap (deterministic: one block of
    // 2^18 samples); consecutive dropped blocks are one gap; paced audio before and after is analysed normally
    {
        const int huge = 1 << 18;
        Rig rig (sr, huge);
        rig.eq.startCapture (MatchRole::Current);
        const auto paced = [&] (std::uint32_t seed)
        {
            auto l = noise ((size_t) (1.5 * sr), seed), r = noise ((size_t) (1.5 * sr), seed + 1);
            for (size_t pos = 0; pos < l.size(); pos += 4096)
            {
                const int n = (int) std::min ((size_t) 4096, l.size() - pos);
                float* p[2] { l.data() + pos, r.data() + pos };
                rig.eq.process (p, n, rig.ctx);
                if (pos % 16384 == 0)
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        };
        paced (77);
        auto bl = noise ((size_t) huge, 3), br = noise ((size_t) huge, 4);
        long long allocations = 0;
        for (int k = 0; k < 2; ++k)
        {
            float* p[2] { bl.data(), br.data() };
            pstest::AllocationScope scope;
            rig.eq.process (p, huge, rig.ctx);
            allocations += scope.count();
        }
        paced (79);
        rig.eq.stopCapture();
        pstest::note ("over-full blocks: gaps", rig.eq.getCaptureGaps());
        pstest::note ("over-full blocks: seconds analysed", rig.eq.getSpectrum (MatchRole::Current).seconds);
        CHECK_EQ (rig.eq.getCaptureGaps(), 1);
        CHECK_EQ (allocations, 0LL);
        CHECK (rig.eq.getSpectrum (MatchRole::Current).valid);
        CHECK (rig.eq.getSpectrum (MatchRole::Current).seconds < 3.0);   // the dropped 11 s are not in the average
    }

    // generation: set, clear, stop count; isDesignPending follows a spectrum change
    {
        Rig rig (sr, 512);
        REQUIRE (rig.settle());
        CHECK (! rig.eq.isDesignPending());
        const unsigned g = rig.eq.getSpectrumGeneration();
        rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (0.0));
        CHECK_EQ (rig.eq.getSpectrumGeneration(), g + 1u);
        CHECK (rig.eq.isDesignPending());
        rig.eq.clearSpectrum (MatchRole::Reference);
        CHECK_EQ (rig.eq.getSpectrumGeneration(), g + 2u);
        CHECK (! rig.eq.getSpectrum (MatchRole::Reference).valid);
        rig.eq.stopCapture();   // not capturing: nothing happens
        CHECK_EQ (rig.eq.getSpectrumGeneration(), g + 2u);
        REQUIRE (rig.settle());
        CHECK (! rig.eq.isDesignPending());
        // a too-short capture publishes an invalid (empty) spectrum
        rig.eq.startCapture (MatchRole::Current);
        auto l = noise (4800, 1), r = noise (4800, 2);
        rig.run (l, r);
        rig.eq.stopCapture();
        CHECK_EQ (rig.eq.getSpectrumGeneration(), g + 3u);
        CHECK (! rig.eq.getSpectrum (MatchRole::Current).valid);
    }
}

TEST_CASE ("meq. realtime: no allocation in setParams / process / reset / setMonitor (swaps, switches, capture)")
{
    const double sr = 48000.0;
    Rig rig (sr, 256);
    setAll (rig, 50.0f, 40.0f, 12.0f, 12.0f);
    rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (-2.0));
    rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
    rig.eq.startCapture (MatchRole::Current);
    auto l = noise ((size_t) (4.0 * sr), 5), r = noise ((size_t) (4.0 * sr), 6);
    long long allocations = 0;
    int block = 0;
    for (size_t pos = 0; pos < l.size(); pos += 256, ++block)
    {
        const int k = block / 16;
        rig.set ("meq.a.amount", (float) ((k * 37) % 101));
        rig.set ("meq.b.smooth", (float) ((k * 13) % 101));
        if (block % 200 == 0)
        {
            rig.set ("meq.phase", (float) ((block / 200) % 2));
            rig.set ("meq.firSize", (float) ((block / 400) % 4));
            rig.mode ((ChannelMode) ((block / 300) % 3));
        }
        ModuleMonitor mon;
        mon.auditionOn = (k % 5) == 0;
        mon.auditionHz = 200.0f + (float) (k % 50) * 100.0f;
        const int n = (int) std::min ((size_t) 256, l.size() - pos);
        float* p[2] { l.data() + pos, r.data() + pos };
        {
            pstest::AllocationScope scope;
            rig.eq.setParams (rig.values.data());
            rig.eq.setMonitor (mon);
            rig.eq.process (p, n, rig.ctx);
            if (block % 500 == 499)
                rig.eq.reset();
            allocations += scope.count();
        }
        if (block % 8 == 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    rig.eq.stopCapture();
    pstest::note ("allocations on the audio thread", (double) allocations);
    CHECK_EQ (allocations, 0LL);
    bool finite = true;
    for (size_t i = 0; i < l.size(); ++i)
        finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
    CHECK (finite);
}

TEST_CASE ("meq. stability: extreme automation and denormal input stay finite and bounded")
{
    const double sr = 44100.0;
    Rig rig (sr, 128);
    rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (-6.0, 6.0, 80.0));
    rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (0.0));
    pstest::Lcg g (99);
    auto l = noise ((size_t) (5.0 * sr), 7, 0.5f), r = noise ((size_t) (5.0 * sr), 8, 0.5f);
    for (size_t i = (size_t) (2.5 * sr); i < l.size(); ++i)
    {
        l[i] = 1.0e-39f;   // denormal input
        r[i] = -1.0e-39f;
    }
    int block = 0;
    for (size_t pos = 0; pos < l.size(); pos += 128, ++block)
    {
        const auto u = [&] { return 0.5f + 0.5f * g.next(); };
        rig.set ("meq.a.amount", 100.0f * u());
        rig.set ("meq.a.smooth", 100.0f * u());
        rig.set ("meq.a.boost", 24.0f * u());
        rig.set ("meq.a.cut", 24.0f * u());
        rig.set ("meq.a.lo", 20.0f * std::pow (1000.0f, u()));
        rig.set ("meq.a.hi", 20.0f * std::pow (1000.0f, u()));
        if (block % 50 == 0)
            rig.set ("meq.phase", (float) (block / 50 % 2));
        const int n = (int) std::min ((size_t) 128, l.size() - pos);
        rig.apply();
        float* p[2] { l.data() + pos, r.data() + pos };
        rig.eq.process (p, n, rig.ctx);
        if (block % 16 == 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    double peak = 0.0;
    bool finite = true;
    for (size_t i = 0; i < l.size(); ++i)
    {
        finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
        peak = std::max (peak, (double) std::max (std::abs (l[i]), std::abs (r[i])));
    }
    pstest::note ("peak output (input peak 0.5)", peak);
    CHECK (finite);
    CHECK (peak < 0.5 * 16.0 * 2.0);   // +24 dB limit, plus FIR overshoot margin

    // after 2.5 s of denormal-level input the output is (essentially) silent
    double tail = 0.0;
    for (size_t i = l.size() - 4410; i < l.size(); ++i)
        tail = std::max (tail, (double) std::abs (l[i]));
    pstest::note ("peak of the output tail after denormal input", tail);
    CHECK (tail < 1.0e-30);
}

TEST_CASE ("meq. CPU: x realtime per FIR size and phase (48 kHz stereo)")
{
    const double sr = 48000.0;
    const double seconds = 6.0;
    double worst = 1e9;
    for (int phase = 0; phase < 2; ++phase)
        for (int size = 0; size < 4; ++size)
        {
            Rig rig (sr, 512);
            rig.set ("meq.phase", (float) phase);
            rig.set ("meq.firSize", (float) size);
            setAll (rig, 60.0f, 40.0f, 12.0f, 12.0f);
            rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (-2.0));
            rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
            REQUIRE (rig.settle());
            auto l = noise ((size_t) (seconds * sr), 1), r = noise ((size_t) (seconds * sr), 2);
            pstest::Stopwatch sw;
            rig.run (l, r);
            const double x = seconds / sw.seconds();
            pstest::note (std::string (phase == 0 ? "Linear " : "Minimum ") + std::to_string (4096 << size) + ": x realtime", x);
            worst = std::min (worst, x);

            if (phase == 0 && size == 3)
            {
                rig.eq.startCapture (MatchRole::Current);
                auto l2 = noise ((size_t) (seconds * sr), 3), r2 = noise ((size_t) (seconds * sr), 4);
                pstest::Stopwatch sw2;
                for (size_t pos = 0; pos < l2.size(); pos += 65536)
                {
                    const size_t n = std::min ((size_t) 65536, l2.size() - pos);
                    std::vector<float> bl (l2.begin() + (long long) pos, l2.begin() + (long long) (pos + n));
                    std::vector<float> br (r2.begin() + (long long) pos, r2.begin() + (long long) (pos + n));
                    rig.run (bl, br);
                }
                const double xc = seconds / sw2.seconds();
                rig.eq.stopCapture();
                pstest::note ("Linear 32768 while capturing: x realtime", xc);
            }
        }
    CHECK (worst >= 60.0);
}

TEST_CASE ("meq. stereo mean includes a signal whose other channel is silent")
{
    const auto pink = makeMatchTiltSpectrum (-3.0);
    const auto white = makeMatchTiltSpectrum (0.0);
    float fullCurve[kMatchPoints], oneSidedCurve[kMatchPoints];
    const auto params = setParams (100.0f, 30.0f);
    computeMatchCurve (white, pink, MatchChannel::StereoMean, params, fullCurve);
    double worstCurve = 0.0;
    for (int silentChannel : { 0, 1 })
    {
        auto oneSided = pink;
        oneSided.psdDb[silentChannel].fill (kMatchNoValue);
        for (int i = 0; i < kMatchPoints; ++i)
            CHECK_NEAR (oneSided.valueDb (MatchChannel::StereoMean, i),
                        pink.psdDb[0][(size_t) i] - 3.01029995664, 1.0e-5);
        computeMatchCurve (white, oneSided, MatchChannel::StereoMean, params, oneSidedCurve);
        for (int i = 0; i < kMatchPoints; ++i)
            worstCurve = std::max (worstCurve, (double) std::abs (fullCurve[i] - oneSidedCurve[i]));
    }
    CHECK (worstCurve < 1.0e-4);
    CHECK (*std::max_element (oneSidedCurve, oneSidedCurve + kMatchPoints) > 1.0f);

    const auto signal = noise (144000, 714, 0.004f);
    const std::vector<float> silence (signal.size(), 0.0f);
    const auto captured = analyse (signal, &silence, 48000.0);
    REQUIRE (captured.valid);
    double worstMean = 0.0;
    int measured = 0;
    for (int i = 0; i < kMatchPoints; ++i)
        if (captured.valueDb (MatchChannel::Left, i) > -120.0f)
        {
            worstMean = std::max (worstMean, std::abs ((double) captured.valueDb (MatchChannel::StereoMean, i)
                                        - captured.valueDb (MatchChannel::Left, i) + 3.01029995664));
            ++measured;
        }
    CHECK (measured > 400);
    CHECK (worstMean < 1.0e-4);
    pstest::note ("one-sided capture: stereo mean vs half left power, max error (dB)", worstMean);
    pstest::note ("one-sided vs dual-mono normalised curve, max error (dB)", worstCurve);

    auto empty = pink;
    empty.psdDb[0].fill (kMatchNoValue);
    empty.psdDb[1].fill (kMatchNoValue);
    CHECK_EQ (empty.valueDb (MatchChannel::StereoMean, 300), kMatchNoValue);
}

TEST_CASE ("meq. analyser rejects corrupt frames without poisoning subsequent audio")
{
    const auto clean = noise (96000, 816);
    const auto expected = analyse (clean, nullptr, 48000.0);
    REQUIRE (expected.valid);
    double worst = 0.0;
    for (float corrupt : { std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::max() })
    {
        MatchAnalyser analyser;
        analyser.prepare (48000.0);
        const std::vector<float> badFrame (16384, corrupt);
        analyser.push (badFrame.data(), nullptr, (int) badFrame.size());
        CHECK_EQ (analyser.getSeconds(), 0.0);
        analyser.push (clean.data(), nullptr, (int) clean.size());
        MatchSpectrum result;
        analyser.spectrum (result);
        CHECK (result.valid);
        CHECK_EQ (result.seconds, expected.seconds);
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < kMatchPoints; ++i)
                worst = std::max (worst, (double) std::abs (result.psdDb[c][(size_t) i]
                                                           - expected.psdDb[c][(size_t) i]));
    }
    pstest::note ("clean spectrum after corrupt frames, max difference (dB)", worst);
    CHECK_EQ (worst, 0.0);
}

TEST_CASE ("meq. residual gate opens before the worker may start the kernel crossfade")
{
    Rig rig (48000.0, 8192);
    rig.set ("meq.phase", 1.0f);
    rig.set ("meq.firSize", 0.0f);
    setAll (rig, 100.0f, 40.0f, 12.0f, 12.0f);
    rig.eq.setSpectrum (MatchRole::Reference, makeMatchTiltSpectrum (-6.0));
    rig.eq.setSpectrum (MatchRole::Current, makeMatchTiltSpectrum (-3.0));
    const auto input = pstest::sine (100.0, -20.0, 1.0, rig.sr);
    auto left = input, right = input;
    int pos = 0;
    const auto run = [&] (int n)
    {
        rig.apply();
        float* p[2] { left.data() + pos, right.data() + pos };
        rig.eq.process (p, n, rig.ctx);
        pos += n;
    };
    // The first fresh configuration closes the residual gate. Let the worker submit zero, then consume it.
    run (1);
    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    run (256);
    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    // This whole segment is gated. Previously the worker could already have submitted a nonzero kernel,
    // completing its fade invisibly here and revealing the full correction at the next segment boundary.
    run (8192);
    const int boundary = pos;
    run (8192);
    double worst = 0.0;
    for (int i = boundary - 32; i < boundary + 32; ++i)
        worst = std::max (worst, std::abs ((double) left[(size_t) i] - 2.0 * left[(size_t) (i - 1)]
                                         + left[(size_t) (i - 2)]));
    pstest::note ("gate opening max second difference", worst);
    CHECK (worst < 0.001);
    // Also prove that the handshake eventually permits the requested correction.
    while (pos + 4096 <= (int) left.size())
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
        run (4096);
    }
    CHECK (! rig.eq.isDesignPending());
    float curve[kMatchPoints];
    computeMatchCurve (makeMatchTiltSpectrum (-6.0), makeMatchTiltSpectrum (-3.0), MatchChannel::StereoMean,
                       setParams (100.0f, 40.0f, 12.0f, 12.0f), curve);
    const double gain = 20.0 * std::log10 (std::abs (pstest::toneOf (left, (size_t) pos - 9600, 9600, 100.0, rig.sr)) / 0.1);
    pstest::note ("gate regression final gain (dB)", gain);
    CHECK_NEAR (gain, fir::curveDbAt (curve, 100.0), 0.5);
}

TEST_CASE ("meq. capture restarts count their first gap and stopping continuous input completes")
{
    {
        const int huge = 1 << 18;
        Rig rig (48000.0, huge);
        std::vector<float> left ((size_t) huge, 0.1f), right = left;
        for (int session = 0; session < 3; ++session)
        {
            rig.eq.startCapture (session == 1 ? MatchRole::Current : MatchRole::Reference);
            float* p[2] { left.data(), right.data() };
            rig.eq.process (p, huge, rig.ctx);
            CHECK_EQ (rig.eq.getCaptureGaps(), 1);
            rig.eq.stopCapture();
            CHECK_EQ (rig.eq.getCaptureSeconds(), 0.0);
        }
    }
    Rig rig (48000.0, 4096);
    std::atomic<bool> finish { false }, stopped { false };
    std::atomic<int> blocks { 0 };
    std::thread producer ([&]
    {
        std::vector<float> left (4096), right (4096);
        while (! finish.load())
        {
            std::fill (left.begin(), left.end(), 0.1f);
            std::fill (right.begin(), right.end(), 0.1f);
            float* p[2] { left.data(), right.data() };
            rig.eq.process (p, 4096, rig.ctx);
            blocks.fetch_add (1);
        }
    });
    double worstStopMs = 0.0;
    for (int session = 0; session < 12; ++session)
    {
        rig.eq.startCapture (MatchRole::Current);
        const int startBlock = blocks.load();
        while (blocks.load() < startBlock + 48)
            std::this_thread::yield();
        stopped.store (false);
        const auto start = std::chrono::steady_clock::now();
        std::thread stopper ([&] { rig.eq.stopCapture(); stopped.store (true); });
        while (! stopped.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds (2))
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        const bool completed = stopped.load();
        if (! completed)
            finish.store (true);   // let a broken drain terminate, so the regression cannot hang the suite
        stopper.join();
        worstStopMs = std::max (worstStopMs, std::chrono::duration<double, std::milli> (
                                               std::chrono::steady_clock::now() - start).count());
        CHECK (completed);
        CHECK (! rig.eq.isCapturing());
        CHECK (rig.eq.getSpectrum (MatchRole::Current).valid);
        if (! completed)
            break;
    }
    finish.store (true);
    producer.join();
    pstest::note ("continuous-input capture worst stop time (ms)", worstStopMs);
}
