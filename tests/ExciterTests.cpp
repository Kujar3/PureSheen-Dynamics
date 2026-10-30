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

TEST_CASE ("exc. harmonic signature per model (1 kHz sine)")
{
    Harmonics hs[(int) ExciterMode::Count];
    for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
    {
        const auto out = processTone ((ExciterMode) mi, 60.0f, 1000.0, -12.0);
        const auto h = harmonicsOf (out.l, 24000, 48000, 1000.0);
        hs[mi] = h;
        const std::string tag = std::string ("exc. ") + kModeNames[mi] + " drive 60, -12 dBFS 1 kHz: ";
        note (tag + "even harmonics (dBc)", h.evenDb());
        note (tag + "odd harmonics (dBc)", h.oddDb());
        note (tag + "H2 (dBc)", pstest::dbOf (h.h[2]));
        note (tag + "H3 (dBc)", pstest::dbOf (h.h[3]));
        note (tag + "fundamental level change (dB)", pstest::dbOf (h.fundamental / pstest::dbfsToAmplitude (-12.0)));
    }

    using M = ExciterMode;
    // symmetric curves: odd only
    for (M m : { M::Analog, M::Retro, M::Tape })
    {
        CHECK (hs[(int) m].oddDb() > -40.0);
        CHECK (hs[(int) m].evenDb() < hs[(int) m].oddDb() - 60.0);
    }
    // Warm: even only (odd at least 30 dB below even)
    CHECK (hs[(int) M::Warm].evenDb() > -40.0);
    CHECK (hs[(int) M::Warm].oddDb() < hs[(int) M::Warm].evenDb() - 30.0);
    // asymmetric: both
    for (M m : { M::Tube, M::Triode, M::DualTriode })
    {
        CHECK (hs[(int) m].evenDb() > -50.0);
        CHECK (hs[(int) m].oddDb() > -60.0);
    }
    // Dual Triode is stronger and more even than one Triode stage
    CHECK (hs[(int) M::DualTriode].evenDb() > hs[(int) M::Triode].evenDb() + 3.0);
    CHECK (hs[(int) M::DualTriode].oddDb() > hs[(int) M::Triode].oddDb());
    // level compensation: the fundamental stays within a few dB at -12 dBFS
    for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
        CHECK (std::abs (pstest::dbOf (hs[mi].fundamental / pstest::dbfsToAmplitude (-12.0))) < 1.5);
}

TEST_CASE ("exc. Tube emphasises attacks, Retro decays slowly")
{
    // Tube: even harmonics in the first 10 ms of a burst vs the steady state
    {
        const auto burst = pstest::concat (std::vector<float> (24000, 0.0f), pstest::sine (1000.0, -12.0, 1.0, kSr));
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.band (1, "mode", (float) ExciterMode::Tube);
        v.band (1, "drive", 50.0f);
        const auto out = run (m, v, dual (burst), 512);
        const int lat = m.getLatencySamples();
        const auto attack = harmonicsOf (out.l, 24000 + (size_t) lat + 96, 480, 1000.0);
        const auto steady = harmonicsOf (out.l, 24000 + (size_t) lat + 24000, 480, 1000.0);
        note ("exc. Tube H2 on the attack (dBc)", pstest::dbOf (attack.h[2]));
        note ("exc. Tube H2 steady (dBc)", pstest::dbOf (steady.h[2]));
        CHECK (pstest::dbOf (attack.h[2]) > pstest::dbOf (steady.h[2]) + 1.0);
    }
    // Retro: after a loud passage drops by 12 dB, the harmonics take a while to fall to the steady quiet level
    {
        const auto sig = pstest::sineSequence (1000.0, { { -6.0, 1.0 }, { -18.0, 1.5 } }, kSr);
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.band (1, "mode", (float) ExciterMode::Retro);
        v.band (1, "drive", 60.0f);
        const auto out = run (m, v, dual (sig), 512);
        const int lat = m.getLatencySamples();
        const auto decay = harmonicsOf (out.l, 48000 + (size_t) lat + 480, 960, 1000.0);
        const auto quiet = harmonicsOf (out.l, 48000 + (size_t) lat + 48000, 960, 1000.0);
        note ("exc. Retro H3 20..40 ms after the level drop (dBc)", pstest::dbOf (decay.h[3]));
        note ("exc. Retro H3 steady at the quiet level (dBc)", pstest::dbOf (quiet.h[3]));
        CHECK (pstest::dbOf (decay.h[3]) > pstest::dbOf (quiet.h[3]) + 3.0);
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
    // 150 Hz at the -12 dBFS reference level, drive 70: compare H1 (magnitude + phase) and H2..H5 of the processed signal
    // with the transferCurve applied to the same input sample by sample
    for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
    {
        const auto mode = (ExciterMode) mi;
        const auto x = pstest::sine (150.0, -12.0, 1.5, kSr);
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
        const auto hm = harmonicsOf (aligned, start, n, 150.0);
        const auto hp = harmonicsOf (pred, start, n, 150.0);
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

    // below ~100 Hz the Retro / Tube detectors follow the waveform a little (documented): report how far
    for (ExciterMode mode : { ExciterMode::Retro, ExciterMode::Tube })
    {
        const auto x = pstest::sine (50.0, -12.0, 1.5, kSr);
        Exciter m;
        m.prepare (kSr, 512);
        Values v;
        v.band (1, "mode", (float) mode);
        v.band (1, "drive", 70.0f);
        const auto out = run (m, v, dual (x), 512);
        std::vector<double> pred (x.size());
        for (size_t i = 0; i < x.size(); ++i)
            pred[i] = (double) Exciter::transferCurve (mode, x[i], 70.0f);
        std::vector<float> aligned (out.l.begin() + m.getLatencySamples(), out.l.end());
        const auto hm = harmonicsOf (aligned, 12000, 48000, 50.0);
        const auto hp = harmonicsOf (pred, 12000, 48000, 50.0);
        note (std::string ("exc. transferCurve ") + kModeNames[(int) mode] + " at 50 Hz: H1 error (dB)",
              pstest::dbOf (hm.fundamental / hp.fundamental));
        note (std::string ("exc. transferCurve ") + kModeNames[(int) mode] + " at 50 Hz: H3 error (dB)",
              pstest::dbOf (hm.h[3] / hp.h[3]));
    }

    // static points
    CHECK (Exciter::transferCurve (ExciterMode::Warm, 0.3f, 0.0f) == 0.3f);
    for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
    {
        CHECK (Exciter::transferCurve ((ExciterMode) mi, 0.0f, 80.0f) == 0.0f);
        // monotonic over the display range
        float prev = Exciter::transferCurve ((ExciterMode) mi, -1.0f, 100.0f);
        bool mono = true;
        for (int i = 1; i <= 200; ++i)
        {
            const float y = Exciter::transferCurve ((ExciterMode) mi, -1.0f + 0.01f * (float) i, 100.0f);
            mono = mono && y >= prev;   // (float: flat once fully saturated)
            prev = y;
        }
        CHECK (mono);
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
        imp[at] = 0.01f;   // quiet: practically linear (the level compensation scales it a little)
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
        CHECK (asym < 1.0e-3 * std::abs (out.l[peak]));
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
    double ref = 0.0;
    for (int mi = 0; mi < (int) ExciterMode::Count; ++mi)
        for (float drive : { 25.0f, 40.0f })
        {
            Exciter m;
            m.prepare (kSr, 64);
            Values v;
            v.band (1, "drive", drive);
            v.band (1, "mode", (float) mi);
            ref = std::max (ref, maxSecondDiff (run (m, v, dual (x1), 64).l, 4800));
        }

    for (bool os : { true, false })
    {
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
