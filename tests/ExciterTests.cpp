#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "dsp/Crossover.h"
#include "dsp/Exciter.h"
#include "dsp/Filters.h"
#include "dsp/Oversampler.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

// Exciter (dsp/Exciter.cpp). Prefix "exc.".
namespace
{
using namespace ps;
using pstest::cd;
using pstest::kPi;
using pstest::note;

constexpr double kSr = 48000.0;
const char* const kModeNames[] { "Analog", "Retro", "Tape", "Tube", "Warm", "Triode", "DualTriode" };

struct Values
{
    std::vector<float> v;
    Values()
    {
        for (const auto& s : paramSpecs())
            v.push_back (s.defaultValue);
    }
    void set (const std::string& id, float x)
    {
        const int i = paramIndex (id);
        REQUIRE (i >= 0);
        v[(size_t) i] = x;
    }
    // set A (and B) band parameter
    void band (int b, const std::string& name, float x, bool bothSets = true)
    {
        set ("exc.a.b" + std::to_string (b) + "." + name, x);
        if (bothSets)
            set ("exc.b.b" + std::to_string (b) + "." + name, x);
    }
    ParamValues data() const { return v.data(); }
};

struct Stereo
{
    std::vector<float> l, r;
};

Stereo run (Exciter& m, Values& vals, const Stereo& in, int block, ChannelMode mode = ChannelMode::Stereo,
            const std::function<void (size_t, Values&)>& onBlock = {})
{
    Stereo out = in;
    ProcessContext ctx;
    ctx.channelMode = mode;
    for (size_t start = 0; start < in.l.size(); start += (size_t) block)
    {
        if (onBlock)
            onBlock (start, vals);
        m.setParams (vals.data());
        const int n = (int) std::min ((size_t) block, in.l.size() - start);
        float* ch[2] { out.l.data() + start, out.r.data() + start };
        m.process (ch, n, ctx);
    }
    return out;
}

Stereo dual (const std::vector<float>& x) { return { x, x }; }

double maxDelayedDiff (const std::vector<float>& out, const std::vector<float>& in, int latency, size_t from = 0)
{
    double m = 0.0;
    for (size_t i = std::max (from, (size_t) latency); i < out.size(); ++i)
        m = std::max (m, std::abs ((double) out[i] - (double) in[i - (size_t) latency]));
    return m;
}

struct Harmonics
{
    double h[12] {};   // amplitude relative to the fundamental, h[1] = 1
    double fundamental = 0.0;
    cd fundamentalPhasor;
    double evenDb() const
    {
        double p = 0.0;
        for (int k = 2; k < 12; k += 2)
            p += h[k] * h[k];
        return 10.0 * std::log10 (p + 1e-30);
    }
    double oddDb() const
    {
        double p = 0.0;
        for (int k = 3; k < 12; k += 2)
            p += h[k] * h[k];
        return 10.0 * std::log10 (p + 1e-30);
    }
};

template <typename T>
Harmonics harmonicsOf (const std::vector<T>& y, size_t start, size_t n, double f0)
{
    Harmonics r;
    r.fundamentalPhasor = pstest::toneOf (y, start, n, f0, kSr);
    r.fundamental = std::abs (r.fundamentalPhasor);
    r.h[1] = 1.0;
    for (int k = 2; k < 12; ++k)
        r.h[k] = k * f0 < 0.5 * kSr ? std::abs (pstest::toneOf (y, start, n, k * f0, kSr)) / r.fundamental : 0.0;
    return r;
}

Stereo processTone (ExciterMode mode, float drive, double freq, double peakDb, bool os = true, float mix = 100.0f)
{
    Exciter m;
    m.prepare (kSr, 512);
    Values v;
    v.set ("exc.os", os ? 1.0f : 0.0f);
    v.band (1, "mode", (float) mode);
    v.band (1, "drive", drive);
    v.band (1, "mix", mix);
    return run (m, v, dual (pstest::sine (freq, peakDb, 1.5, kSr)), 512);
}
} // namespace

//==============================================================================
TEST_CASE ("exc. drive 0 is transparent (default settings, any crossover)")
{
    for (CrossoverType t : { CrossoverType::Analog, CrossoverType::Hybrid, CrossoverType::Digital })
    {
        Exciter m;
        m.prepare (kSr, 333);
        Values v;
        v.set ("exc.bands", 4.0f);
        v.set ("exc.xoverType", (float) t);
        const Stereo in { pstest::whiteNoise (0.3, 0.5, kSr, 1), pstest::whiteNoise (0.3, 0.5, kSr, 2) };
        const auto out = run (m, v, in, 333);
        const int lat = m.getLatencySamples();
        const double d = std::max (maxDelayedDiff (out.l, in.l, lat), maxDelayedDiff (out.r, in.r, lat));
        note ("exc. drive 0, 4 bands, crossover " + std::to_string ((int) t) + ", latency " + std::to_string (lat)
                  + ": max diff",
              d);
        CHECK (d == 0.0);
    }
}

TEST_CASE ("exc. every model follows the reference's measured signature (1 kHz sine)")
{
    // The reference's renders (tone test, one band, mix 100 %) at its amounts 3 and 15 = our drive 20 and 100:
    // fundamental change (dB), H2 and H3 (dBc; -200 = not present) at -10 and -4 dBFS. Tolerances: the static models
    // 0.6 dB / 2 dB; Tape and the triodes also move their operating point with the level in the reference (see
    // Exciter.cpp): Triode is checked at -10 dBFS only, Dual Triode at low drive, Tape not at all (reported).
    struct Row { ExciterMode mode; float drive; double peakDb, fund, h2, h3; bool strict; };
    using M = ExciterMode;
    const Row rows[] {
        { M::Analog, 20.0f, -10.0, -0.09, -26.2, -51.3, true },  { M::Analog, 20.0f, -4.0, -0.32, -20.9, -38.3, true },
        { M::Analog, 100.0f, -10.0, -0.08, -19.4, -33.6, true }, { M::Analog, 100.0f, -4.0, -2.41, -20.1, -16.9, true },
        { M::Retro, 20.0f, -10.0, -2.46, -200, -17.6, true },    { M::Retro, 20.0f, -4.0, -6.11, -200, -14.7, true },
        { M::Retro, 100.0f, -10.0, -1.86, -200, -12.7, true },   { M::Retro, 100.0f, -4.0, -6.97, -200, -11.3, true },
        { M::Tube, 20.0f, -10.0, -0.45, -49.8, -31.0, true },    { M::Tube, 20.0f, -4.0, -0.76, -29.5, -42.4, true },
        { M::Tube, 100.0f, -10.0, -7.02, -64.0, -9.6, true },    { M::Tube, 100.0f, -4.0, -13.00, -65.9, -9.6, true },
        { M::Warm, 20.0f, -10.0, 0.08, -26.5, -200, true },      { M::Warm, 20.0f, -4.0, 0.08, -22.9, -200, true },
        { M::Warm, 100.0f, -10.0, -0.96, -10.5, -200, true },    { M::Warm, 100.0f, -4.0, -0.96, -6.9, -200, true },
        { M::Triode, 20.0f, -10.0, 1.74, -34.3, -36.5, true },   { M::Triode, 20.0f, -4.0, 0.77, -24.0, -29.0, false },
        { M::DualTriode, 20.0f, -10.0, -1.00, -68.1, -200, true }, { M::DualTriode, 20.0f, -4.0, -0.66, -30.3, -37.4, true },
        { M::Tape, 20.0f, -10.0, 0.34, -200, -41.0, false },     { M::Tape, 100.0f, -4.0, -11.28, -200, -16.6, false },
        { M::Triode, 100.0f, -4.0, -5.79, -5.3, -20.7, false },  { M::DualTriode, 100.0f, -4.0, -9.66, -12.3, -12.0, false },
    };
    for (const auto& r : rows)
    {
        const auto out = processTone (r.mode, r.drive, 1000.0, r.peakDb);
        const auto h = harmonicsOf (out.l, 24000, 48000, 1000.0);
        const double fund = pstest::dbOf (h.fundamental / pstest::dbfsToAmplitude (r.peakDb));
        const double h2 = std::max (-200.0, pstest::dbOf (h.h[2])), h3 = std::max (-200.0, pstest::dbOf (h.h[3]));
        const std::string tag = std::string ("exc. ") + kModeNames[(int) r.mode] + " drive " + std::to_string ((int) r.drive)
                                + " at " + std::to_string ((int) r.peakDb) + " dBFS: ";
        note (tag + "fundamental change, ours - reference (dB)", fund - r.fund);
        note (tag + "H2 ours (dBc)", h2);
        note (tag + "H3 ours (dBc)", h3);
        if (! r.strict)
            continue;
        CHECK (std::abs (fund - r.fund) < 0.6);
        for (const auto& [got, want] : { std::pair { h2, r.h2 }, std::pair { h3, r.h3 } })
        {
            if (want > -60.0)
                CHECK (std::abs (got - want) < 2.0);
            else
                CHECK (got < -45.0);   // absent (or far down) in the reference
        }
    }
}

TEST_CASE ("exc. the curves are static (no level memory)")
{
    // after a loud passage drops by 12 dB, the harmonics are at once those of the quiet level (the reference's models
    // that fit a static curve show no memory either)
    for (ExciterMode mode : { ExciterMode::Retro, ExciterMode::Tube })
    {
        const auto sig = pstest::sineSequence (1000.0, { { -6.0, 1.0 }, { -18.0, 1.5 } }, kSr);
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.band (1, "mode", (float) mode);
        v.band (1, "drive", 60.0f);
        const auto out = run (m, v, dual (sig), 512);
        const int lat = m.getLatencySamples();
        const auto decay = harmonicsOf (out.l, 48000 + (size_t) lat + 480, 960, 1000.0);
        const auto quiet = harmonicsOf (out.l, 48000 + (size_t) lat + 48000, 960, 1000.0);
        const double d = pstest::dbOf (decay.h[3]) - pstest::dbOf (quiet.h[3]);
        note (std::string ("exc. ") + kModeNames[(int) mode] + " H3 20..40 ms after the level drop - steady (dB)", d);
        CHECK (std::abs (d) < 0.5);
    }
}

TEST_CASE ("exc. mix 0 is an aligned dry null")
{
    // (a) neutral by mix: drive set, mix 0 everywhere, post filter on
    {
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("exc.bands", 4.0f);
        v.set ("exc.xoverType", (float) CrossoverType::Digital);
        v.set ("exc.post.on", 1.0f);
        v.set ("exc.post.gain", 12.0f);
        for (int b = 1; b <= 4; ++b)
        {
            v.band (b, "drive", 80.0f);
            v.band (b, "mix", 0.0f);
        }
        const Stereo in { pstest::whiteNoise (0.3, 0.5, kSr, 3), pstest::whiteNoise (0.3, 0.5, kSr, 4) };
        const auto out = run (m, v, in, 512);
        const int lat = m.getLatencySamples();
        const double d = std::max (maxDelayedDiff (out.l, in.l, lat), maxDelayedDiff (out.r, in.r, lat));
        note ("exc. mix 0 (4 bands Digital, post +12 dB): latency", lat);
        note ("exc. mix 0 (4 bands Digital, post +12 dB): max diff", d);
        CHECK (d == 0.0);
    }
    // (b) while processing: M/S, mid at drive 80 / mix 0, side at drive 50 / mix 100 with no side content: the
    // processed path must reproduce the latency-aligned input exactly
    for (bool os : { true, false })
    {
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("exc.os", os ? 1.0f : 0.0f);
        v.set ("exc.a.b1.drive", 80.0f);
        v.set ("exc.a.b1.mix", 0.0f);
        v.set ("exc.b.b1.drive", 50.0f);
        v.set ("exc.b.b1.mode", (float) ExciterMode::Tube);
        const Stereo in { pstest::whiteNoise (0.3, 0.5, kSr, 5), std::vector<float> ((size_t) 24000, 0.0f) };
        const auto out = run (m, v, in, 512, ChannelMode::MidSide);
        const int lat = m.getLatencySamples();
        const double d = std::max (maxDelayedDiff (out.l, in.l, lat), maxDelayedDiff (out.r, in.r, lat));
        note (std::string ("exc. M/S mid mix 0 while the side processes, os ") + (os ? "on" : "off") + ": max diff", d);
        CHECK (d == 0.0);
    }
}

TEST_CASE ("exc. oversampling reduces aliasing (7 kHz at 48 kHz, drive 100)")
{
    double aliasDb[2] {}, worstDb[2] {};
    for (int os = 0; os < 2; ++os)
    {
        const auto out = processTone (ExciterMode::Analog, 100.0f, 7000.0, -6.0, os == 1);
        const size_t start = 24000, n = 48000;   // 1 s: every multiple of 1 Hz is measured exactly
        const double f0 = std::abs (pstest::toneOf (out.l, start, n, 7000.0, kSr));
        double p = 0.0, worst = 0.0;
        bool measured[25] {};   // this tone folds onto integer-kHz bins; count each bin once
        for (int k = 2; k <= 60; ++k)
        {
            const double f = 7000.0 * k;
            if (f < 0.5 * kSr)
                continue;   // a real harmonic below Nyquist (14 and 21 kHz)
            double a = std::fmod (f, kSr);
            if (a > 0.5 * kSr)
                a = kSr - a;
            if (a < 1.0 || std::abs (a - 7000.0) < 1.0 || std::abs (a - 14000.0) < 1.0 || std::abs (a - 21000.0) < 1.0)
                continue;   // folds onto a harmonic: cannot be separated
            const int bin = (int) std::lround (a / 1000.0);
            if (measured[bin])
                continue;
            measured[bin] = true;
            const double amp = std::abs (pstest::toneOf (out.l, start, n, a, kSr)) / f0;
            p += amp * amp;
            worst = std::max (worst, amp);
        }
        aliasDb[os] = 10.0 * std::log10 (p + 1e-30);
        worstDb[os] = pstest::dbOf (worst);
    }
    note ("exc. aliasing, os off: total aliased power (dBc)", aliasDb[0]);
    note ("exc. aliasing, os off: worst alias (dBc)", worstDb[0]);
    note ("exc. aliasing, os 4x: total aliased power (dBc)", aliasDb[1]);
    note ("exc. aliasing, os 4x: worst alias (dBc)", worstDb[1]);
    CHECK (aliasDb[1] < aliasDb[0] - 40.0);
    CHECK (aliasDb[1] < -50.0);   // documented level: hard-knee Analog model, -6 dBFS 7 kHz, drive 100
}

TEST_CASE ("exc. transferCurve matches the audio path for steady input")
{
    // 2.9 kHz at -12 dBFS, drive 70: compare H1 (magnitude + phase) and H2..H5 of the processed signal with the
    // transferCurve applied to the same input sample by sample (the 14.1 Hz wet high pass is negligible up there; at
    // 48 kHz the aliases of the sample-by-sample prediction do not land on the harmonics of 2.9 kHz, as they would for
    // a tone dividing 48 kHz)
    for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
    {
        const auto mode = (ExciterMode) mi;
        const auto x = pstest::sine (2900.0, -12.0, 1.5, kSr);
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.band (1, "mode", (float) mode);
        v.band (1, "drive", 70.0f);
        const auto out = run (m, v, dual (x), 512);
        const int lat = m.getLatencySamples();

        std::vector<double> pred (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            pred[i] = (double) Exciter::transferCurve (mode, x[i], 70.0f);

        const size_t start = 12000, n = 48000;
        std::vector<float> aligned (out.l.begin() + lat, out.l.end());
        const auto hm = harmonicsOf (aligned, start, n, 2900.0);
        const auto hp = harmonicsOf (pred, start, n, 2900.0);
        const double gainErr = pstest::dbOf (hm.fundamental / hp.fundamental);
        const double phaseErr = std::abs (std::arg (hm.fundamentalPhasor / hp.fundamentalPhasor)) * 180.0 / kPi;
        double worstHarm = 0.0;
        for (int k = 2; k <= 5; ++k)
            if (hp.h[k] > 1.0e-4)   // > -80 dBc
                worstHarm = std::max (worstHarm, std::abs (pstest::dbOf (hm.h[k] / hp.h[k])));
        const std::string tag = std::string ("exc. transferCurve ") + kModeNames[mi] + ": ";
        note (tag + "H1 error (dB)", gainErr);
        note (tag + "H1 phase error (deg)", phaseErr);
        note (tag + "worst H2..H5 error (dB)", worstHarm);
        CHECK (std::abs (gainErr) < 0.02);
        CHECK (phaseErr < 0.5);
        CHECK (worstHarm < 0.3);
    }

    // at 50 Hz the wet high pass (first order, 14.1 Hz) shows: the fundamental is the curve's, high-passed
    {
        const auto x = pstest::sine (50.0, -12.0, 1.5, kSr);
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.band (1, "mode", (float) ExciterMode::Retro);
        v.band (1, "drive", 70.0f);
        const auto out = run (m, v, dual (x), 512);
        std::vector<double> pred (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            pred[i] = (double) Exciter::transferCurve (ExciterMode::Retro, x[i], 70.0f);
        std::vector<float> aligned (out.l.begin() + m.getLatencySamples(), out.l.end());
        const auto hm = harmonicsOf (aligned, 12000, 48000, 50.0);
        const auto hp = harmonicsOf (pred, 12000, 48000, 50.0);
        const double expected = -10.0 * std::log10 (1.0 + (14.1 / 50.0) * (14.1 / 50.0));
        const double got = pstest::dbOf (hm.fundamental / hp.fundamental);
        note ("exc. transferCurve Retro at 50 Hz: H1 error (dB, expected from the 14.1 Hz high pass)", got);
        CHECK (std::abs (got - expected) < 0.05);
    }

    // static points
    CHECK (Exciter::transferCurve (ExciterMode::Warm, 0.3f, 0.0f) == 0.3f);
    for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
    {
        CHECK (Exciter::transferCurve ((ExciterMode) mi, 0.0f, 80.0f) == 0.0f);
        // finite and bounded over the display range (some measured curves fold back: Warm is rectifier-like)
        bool bounded = true;
        for (int i = 0; i <= 200; ++i)
            for (float d : { 10.0f, 35.0f, 75.0f, 100.0f })
            {
                const float y = Exciter::transferCurve ((ExciterMode) mi, -1.0f + 0.01f * (float) i, d);
                bounded = bounded && std::isfinite (y) && std::abs (y) < 2.0f;
            }
        CHECK (bounded);
    }
}

TEST_CASE ("exc. post filter affects only the wet path")
{
    const double fc = 4000.0, g = 12.0;
    const auto shelf = design::highShelf (kSr, fc, 0.7071067811865476, g);
    for (float mix : { 0.0f, 50.0f, 100.0f })
    {
        for (double f : { 200.0, 15000.0 })
        {
            Exciter m;
            m.prepare (kSr, 512);
            Values v;
            v.set ("exc.post.on", 1.0f);
            v.set ("exc.post.freq", (float) fc);
            v.set ("exc.post.gain", (float) g);
            v.band (1, "drive", 0.0f);
            v.band (1, "mix", mix);
            const auto x = pstest::sine (f, -12.0, 1.5, kSr);
            const auto out = run (m, v, dual (x), 512);
            const int lat = m.getLatencySamples();
            const cd yo = pstest::toneOf (out.l, 24000 + (size_t) lat, 24000, f, kSr);
            const cd yi = pstest::toneOf (x, 24000, 24000, f, kSr);
            const double measured = pstest::dbOf (std::abs (yo / yi));
            const double expected = pstest::dbOf (std::abs ((1.0 - mix / 100.0) + mix / 100.0 * shelf.response (f, kSr)));
            note ("exc. post filter +12 dB @ 4 kHz, mix " + std::to_string ((int) mix) + ", " + std::to_string ((int) f)
                      + " Hz: gain (dB)",
                  measured);
            CHECK (std::abs (measured - expected) < 0.05);
            if (mix == 0.0f)
                CHECK (maxDelayedDiff (out.l, x, lat) == 0.0);
        }
    }
}

TEST_CASE ("exc. latency is exact and identical for every band")
{
    for (bool os : { true, false })
    {
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("exc.bands", 4.0f);
        v.set ("exc.xoverType", (float) CrossoverType::Digital);
        v.set ("exc.os", os ? 1.0f : 0.0f);
        for (int b = 1; b <= 4; ++b)
        {
            v.band (b, "drive", 30.0f);
            v.band (b, "mode", (float) ExciterMode::Analog);   // odd: a symmetric pulse stays symmetric
        }
        m.setParams (v.data());
        const int lat = m.getLatencySamples();
        Crossover xo;
        xo.prepare (kSr, 512);
        Oversampler ov;
        ov.prepare (2, Oversampler::Quality::LinearPhase, 512, 2);
        CHECK_EQ (lat, xo.getLatencySamples (CrossoverType::Digital) + (os ? ov.getLatencySamples() : 0));

        const size_t len = 16384, at = 4000;
        std::vector<float> imp (len, 0.0f);
        imp[at] = 0.01f;   // quiet: practically linear
        const auto out = run (m, v, dual (imp), 512);
        size_t peak = 0;
        for (size_t i = 0; i < len; ++i)
            if (std::abs (out.l[i]) > std::abs (out.l[peak]))
                peak = i;
        double asym = 0.0;
        for (size_t j = 1; j < 64; ++j)
            asym = std::max (asym, std::abs ((double) out.l[peak + j] - (double) out.l[peak - j]));
        note (std::string ("exc. latency (4 bands Digital, os ") + (os ? "on" : "off") + ")", lat);
        note ("exc. impulse peak offset - latency", (double) ((int) (peak - at) - lat));
        note ("exc. impulse asymmetry around the peak (rel.)", asym / std::abs (out.l[peak]));
        CHECK_EQ ((int) (peak - at), lat);
        // linear phase apart from the wet path's first-order 14.1 Hz high pass (its tail: 2 pi 14.1 / fs = 1.8e-3)
        CHECK (asym < 2.5e-3 * std::abs (out.l[peak]));
    }
}

TEST_CASE ("exc. drive, mix and model changes are click-free")
{
    // A click shows up as a second difference larger than any steady setting produces (harmonics raise it too, so the
    // reference is the worst steady output over the same models and drives).
    const auto x = pstest::sine (200.0, -12.0, 2.0, kSr);
    auto maxSecondDiff = [] (const std::vector<float>& y, size_t from)
    {
        double m = 0.0;
        for (size_t i = from + 2; i < y.size(); ++i)
            m = std::max (m, std::abs ((double) y[i] - 2.0 * (double) y[i - 1] + (double) y[i - 2]));
        return m;
    };
    const std::vector<float> x1 (x.begin(), x.begin() + 24000);
    for (bool os : { true, false })
    {
        // the worst steady output with the same oversampling (without it the harder curves alias more)
        double ref = 0.0;
        for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
            for (float drive : { 25.0f, 40.0f })
            {
                Exciter sm;
                sm.prepare (kSr, 64);
                Values sv;
                sv.set ("exc.os", os ? 1.0f : 0.0f);
                sv.band (1, "drive", drive);
                sv.band (1, "mode", (float) mi);
                ref = std::max (ref, maxSecondDiff (run (sm, sv, dual (x1), 64).l, 4800));
            }

        Exciter m;
        m.prepare (kSr, 64);
        Values v;
        v.set ("exc.os", os ? 1.0f : 0.0f);
        const auto out = run (m, v, dual (x), 64, ChannelMode::Stereo, [] (size_t start, Values& vals)
        {
            const size_t step = start / 4800;   // every 100 ms
            vals.band (1, "drive", (step % 3) == 0 ? 0.0f : (step % 3 == 1 ? 40.0f : 25.0f));
            vals.band (1, "mix", (step % 4) == 0 ? 30.0f : 100.0f);
            vals.band (1, "mode", (float) (step % 7));
        });
        const double d = maxSecondDiff (out.l, 2400);
        note (std::string ("exc. parameter jumps, os ") + (os ? "on" : "off") + ": max |2nd difference| / worst steady",
              d / ref);
        CHECK (d < 1.2 * ref);
    }
}

TEST_CASE ("exc. no allocation in setParams / process")
{
    Exciter m;
    m.prepare (kSr, 512);
    Values v;
    const Stereo in { pstest::whiteNoise (0.3, 0.1, kSr, 9), pstest::whiteNoise (0.3, 0.1, kSr, 10) };
    std::vector<float> l = in.l, r = in.r;
    float* ch[2] { l.data(), r.data() };

    pstest::AllocationScope scope;
    for (int pass = 0; pass < 28; ++pass)
    {
        v.set ("exc.bands", (float) (1 + pass % 4));
        v.set ("exc.xoverType", (float) ((pass / 4) % 3));
        v.set ("exc.os", (float) ((pass / 2) % 2));
        v.set ("exc.post.on", (float) (pass % 3 != 0));
        v.set ("exc.post.gain", -4.0f);
        for (int b = 1; b <= 4; ++b)
        {
            v.band (b, "mode", (float) ((pass + b) % 7));
            v.band (b, "drive", (float) (20 * ((pass + b) % 5)));
            v.band (b, "mix", pass % 5 == 0 ? 0.0f : 80.0f);
        }
        ModuleMonitor mon;
        mon.soloBand = pass % 6 == 5 ? 0 : -1;
        m.setMonitor (mon);
        m.setParams (v.data());
        ProcessContext ctx;
        ctx.channelMode = pass % 2 ? ChannelMode::MidSide : ChannelMode::Stereo;
        m.process (ch, 512, ctx);
        if (pass == 14)
            m.reset();
    }
    CHECK_EQ (scope.count(), 0LL);
    CHECK (std::isfinite (l[100]) && std::isfinite (r[100]));
}

TEST_CASE ("exc. meters report the harmonics per band")
{
    Exciter m;
    m.prepare (kSr, 512);
    Values v;
    v.set ("exc.bands", 2.0f);
    v.set ("exc.xover1", 2000.0f);
    v.band (1, "drive", 80.0f);
    v.band (1, "mode", (float) ExciterMode::Analog);
    (void) run (m, v, dual (pstest::sine (300.0, -12.0, 1.0, kSr)), 512);
    const float low = m.getMeters().harmonicsDb[0][0].load();
    const float high = m.getMeters().harmonicsDb[0][1].load();
    note ("exc. meter band 1 (driven) dB", low);
    note ("exc. meter band 2 (drive 0) dB", high);
    CHECK (low > -40.0f);
    CHECK (high <= -119.0f);
}

TEST_CASE ("exc. cpu benchmark")
{
    for (int config = 0; config < 3; ++config)
    {
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.set ("exc.bands", config == 0 ? 1.0f : 4.0f);
        v.set ("exc.os", config == 2 ? 0.0f : 1.0f);
        v.set ("exc.post.on", 1.0f);
        v.set ("exc.post.gain", -3.0f);
        for (int b = 1; b <= 4; ++b)
        {
            v.band (b, "drive", 60.0f);
            v.band (b, "mix", 70.0f);
            v.band (b, "mode", (float) (b == 1 ? 2 : b + 2));   // Tape, Tube, Warm, Triode
        }
        const Stereo in { pstest::whiteNoise (0.3, 10.0, kSr, 1), pstest::whiteNoise (0.3, 10.0, kSr, 2) };
        pstest::Stopwatch sw;
        const auto out = run (m, v, in, 512, ChannelMode::MidSide);
        const double secs = sw.seconds();
        CHECK (std::isfinite (out.l.back()));
        const char* names[] { "1 band os 4x", "4 bands os 4x", "4 bands os off" };
        note (std::string ("exc. cpu: ") + names[config] + ", M/S, post filter, x realtime", 10.0 / secs);
        CHECK (10.0 / secs > 10.0);
    }
}


TEST_CASE ("exc. topology and latency changes crossfade complete signal paths")
{
    const auto x = pstest::sine (300.0, -12.0, 0.75, kSr);
    constexpr size_t change = 12288;
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        Exciter m;
        m.prepare (kSr, 64);
        Values v;
        if (scenario >= 2)
        {
            v.band (1, "drive", 85.0f);
            v.band (1, "mode", (float) ExciterMode::Tape);
        }
        if (scenario == 3)
        {
            v.set ("exc.bands", 4.0f);
            v.set ("exc.xover1", 200.0f);
            v.band (2, "drive", 0.0f);
        }
        const auto out = run (m, v, dual (x), 64, ChannelMode::Stereo,
                              [scenario] (size_t start, Values& values)
        {
            if (start != change)
                return;
            if (scenario == 1)
                values.set ("exc.xoverType", (float) CrossoverType::Digital);
            else if (scenario == 3)
                values.set ("exc.bands", 1.0f);
            else
                values.set ("exc.os", 0.0f);
        });
        double curvature = 0.0;
        for (size_t i = change; i < change + 2048; ++i)
            curvature = std::max (curvature, std::abs ((double) out.l[i] - 2.0 * out.l[i - 1] + out.l[i - 2]));
        // The driven Tape waveform itself has appreciable curvature. Compare the
        // transition with both settled outputs, rather than an arbitrary click bound.
        double steadyCurvature = 0.0;
        for (size_t i = 4096; i < out.l.size(); ++i)
            if (i < change || i >= change + 8192)
                steadyCurvature = std::max (steadyCurvature,
                    std::abs ((double) out.l[i] - 2.0 * out.l[i - 1] + out.l[i - 2]));
        note ("exc. topology switch scenario " + std::to_string (scenario) + ": peak second difference", curvature);
        note ("exc. topology switch scenario " + std::to_string (scenario) + ": settled second difference", steadyCurvature);
        CHECK (curvature < 1.5 * steadyCurvature + 0.001);
        if (scenario < 2)
            CHECK (maxDelayedDiff (out.l, x, m.getLatencySamples(), change + 4096) == 0.0);
    }
}

TEST_CASE ("exc. rapid topology reversals settle to the final latency")
{
    Exciter m;
    m.prepare (kSr, 64);
    Values v;
    const auto x = pstest::sine (100.0, -12.0, 0.5, kSr);
    const auto out = run (m, v, dual (x), 64, ChannelMode::Stereo, [] (size_t start, Values& values)
    {
        if (start >= 4096 && start < 8192)
            values.set ("exc.os", (float) ((start / 64) % 2));
        else if (start >= 8192)
            values.set ("exc.os", 0.0f);
    });
    double curvature = 0.0;
    for (size_t i = 4096; i < 12288; ++i)
        curvature = std::max (curvature, std::abs ((double) out.l[i] - 2.0 * out.l[i - 1] + out.l[i - 2]));
    note ("exc. rapid OS reversals: peak second difference", curvature);
    CHECK (curvature < 0.002);
    CHECK_EQ (m.getLatencySamples(), 0);
    CHECK (maxDelayedDiff (out.l, x, 0, 12288) == 0.0);
}

TEST_CASE ("exc. re-added bands cannot replay frozen audio")
{
    Exciter m;
    m.prepare (kSr, 64);
    Values v;
    v.set ("exc.bands", 4.0f);
    for (int b = 1; b <= 4; ++b)
    {
        v.band (b, "drive", 70.0f);
        v.band (b, "mode", (float) ExciterMode::Tape);
    }
    (void) run (m, v, dual (pstest::sine (7000.0, -6.0, 0.1, kSr)), 64);
    v.set ("exc.bands", 1.0f);
    const Stereo silence = dual (std::vector<float> (48000, 0.0f));
    (void) run (m, v, silence, 64);
    v.set ("exc.bands", 4.0f);
    const auto out = run (m, v, dual (std::vector<float> (4096, 0.0f)), 64);
    double peak = 0.0;
    for (float y : out.l)
        peak = std::max (peak, std::abs ((double) y));
    note ("exc. re-added bands after silence: peak", peak);
    CHECK (peak < 1.0e-7);
}

TEST_CASE ("exc. one inactive Tape channel cannot replay its old filter state")
{
    for (const char* parameter : { "mix", "drive" })
    {
        Exciter m;
        m.prepare (kSr, 64);
        Values v;
        v.set ("exc.os", 0.0f);
        v.band (1, "drive", 80.0f);
        v.band (1, "mode", (float) ExciterMode::Tape);
        (void) run (m, v, dual (pstest::sine (7000.0, -6.0, 0.1, kSr)), 64, ChannelMode::LeftRight);
        v.set (std::string ("exc.a.b1.") + parameter, 0.0f);
        (void) run (m, v, dual (pstest::sine (7000.0, -6.0, 0.1, kSr)), 64, ChannelMode::LeftRight);
        (void) run (m, v, dual (std::vector<float> (48000, 0.0f)), 64, ChannelMode::LeftRight);
        v.set (std::string ("exc.a.b1.") + parameter, parameter[0] == 'm' ? 100.0f : 80.0f);
        const auto out = run (m, v, dual (std::vector<float> (4096, 0.0f)), 64, ChannelMode::LeftRight);
        double peak = 0.0;
        for (float y : out.l)
            peak = std::max (peak, std::abs ((double) y));
        note (std::string ("exc. Tape channel resumes ") + parameter + " after silence: peak", peak);
        CHECK (peak < 1.0e-7);
    }
}

TEST_CASE ("exc. linked to independent channels preserves detector history")
{
    for (ExciterMode mode : { ExciterMode::Tube, ExciterMode::Retro })
    {
        Exciter m;
        m.prepare (kSr, 64);
        Values v;
        v.band (1, "drive", 80.0f);
        v.band (1, "mode", (float) mode);
        const auto x = pstest::sine (1000.0, -12.0, 0.5, kSr);
        (void) run (m, v, dual (x), 64);
        const auto out = run (m, v, dual (x), 64, ChannelMode::LeftRight);
        double difference = 0.0;
        for (size_t i = 0; i < out.l.size(); ++i)
            difference = std::max (difference, std::abs ((double) out.l[i] - out.r[i]));
        note (std::string ("exc. Stereo to L/R ") + kModeNames[(int) mode] + ": L/R difference", difference);
        CHECK (difference == 0.0);
    }
}


TEST_CASE ("exc. queued topology requests complete without repeated setParams")
{
    Exciter m;
    m.prepare (kSr, 64);
    Values v;
    const auto x = pstest::sine (300.0, -12.0, 0.5, kSr);
    Stereo out = dual (x);
    ProcessContext context;
    m.setParams (v.data());
    float* first[2] { out.l.data(), out.r.data() };
    m.process (first, 4096, context);
    v.set ("exc.os", 0.0f);
    m.setParams (v.data());
    float* change[2] { out.l.data() + 4096, out.r.data() + 4096 };
    m.process (change, 64, context);
    // A third topology, not just a reversal. Submit it once during the first fade.
    v.set ("exc.os", 1.0f);
    v.set ("exc.xoverType", (float) CrossoverType::Digital);
    m.setParams (v.data());
    const int latency = m.getLatencySamples();
    float* remaining[2] { out.l.data() + 4160, out.r.data() + 4160 };
    m.process (remaining, (int) x.size() - 4160, context);
    const double difference = maxDelayedDiff (out.l, x, latency, 16384);
    note ("exc. one-shot queued topology: final delayed null", difference);
    CHECK_EQ (latency, 3406);
    CHECK (difference == 0.0);
}


TEST_CASE ("exc. reset preserves the latest topology without another setParams")
{
    Exciter m;
    m.prepare (kSr, 64);
    Values v;
    m.setParams (v.data());
    Stereo warm = dual (std::vector<float> (960, 0.0f));
    float* channels[2] { warm.l.data(), warm.r.data() };
    ProcessContext context;
    m.process (channels, 64, context);
    v.set ("exc.os", 0.0f);
    m.setParams (v.data());
    m.process (channels, 960, context);   // stop exactly when the new engine becomes current
    m.reset();
    Stereo impulse = dual (std::vector<float> (512, 0.0f));
    impulse.l[0] = impulse.r[0] = 0.25f;
    const Stereo input = impulse;
    float* signal[2] { impulse.l.data(), impulse.r.data() };
    m.process (signal, 512, context);
    CHECK_EQ (m.getLatencySamples(), 0);
    CHECK (maxDelayedDiff (impulse.l, input.l, 0) == 0.0);
}
