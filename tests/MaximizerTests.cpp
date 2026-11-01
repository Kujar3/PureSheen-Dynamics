#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestSignals.h"

#include "core/Loudness.h"
#include "core/TruePeak.h"
#include "dsp/Maximizer.h"
#include "dsp/MaximizerLearn.h"
#include "dsp/MaximizerStages.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Maximizer (dsp/Maximizer*.cpp). Prefix "max.".
//   max. contract ...   hard ceiling (sample peak / true peak) on nasty material, every mode / style / character / rate
//   max. quality ...    THD+N, IMD, loudness, crest, transient retention per mode (printed; sanity bounds checked)
//   max. cpu ...        real-time factor per mode at 48 kHz stereo
namespace
{
using namespace ps;
using pstest::kPi;

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
    void resize (size_t n) { l.assign (n, 0.0f); r.assign (n, 0.0f); }
    void append (const Stereo& o)
    {
        l.insert (l.end(), o.l.begin(), o.l.end());
        r.insert (r.end(), o.r.begin(), o.r.end());
    }
};

struct ModeCfg
{
    const char* name;
    int mode, style3, style4;
};

const ModeCfg kModes[] {
    { "Low Latency", 0, 2, 1 },          { "Smooth", 1, 2, 1 },               { "Transparent", 2, 2, 1 },
    { "Aggressive Clipping", 3, 0, 1 },  { "Aggressive Crisp", 3, 1, 1 },     { "Aggressive Balanced", 3, 2, 1 },
    { "Aggressive Pumping", 3, 3, 1 },   { "Spectral Classic", 4, 2, 0 },     { "Spectral Modern", 4, 2, 1 },
    { "Spectral Transient", 4, 2, 2 },   { "Multiband", 5, 2, 1 },
};

struct Rig
{
    double sr;
    int block;
    ChannelMode channelMode = ChannelMode::Stereo;
    std::vector<float> values;
    Maximizer mx;

    explicit Rig (double sampleRate, int blockSize = 512) : sr (sampleRate), block (blockSize)
    {
        const auto& specs = paramSpecs();
        values.resize (specs.size());
        for (size_t i = 0; i < specs.size(); ++i)
            values[i] = specs[i].defaultValue;
        mx.prepare (sr, block);
        mx.setParams (values.data());
        mx.reset();
    }

    void set (const char* id, float v)
    {
        const int i = paramIndex (id);
        REQUIRE (i >= 0);
        values[(size_t) i] = v;
    }

    void setMode (const ModeCfg& m)
    {
        set ("max.mode", (float) m.mode);
        set ("max.style3", (float) m.style3);
        set ("max.style4", (float) m.style4);
    }

    // Applies the current values and resets (a fresh start in the configured mode).
    void restart()
    {
        mx.setParams (values.data());
        mx.reset();
    }

    void run (Stereo& s, const std::function<void (long long)>& onBlock = {}, Stereo* delta = nullptr,
              bool legacyDelta = false)
    {
        if (delta != nullptr)
            delta->resize (s.size());
        for (size_t start = 0; start < s.size(); start += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, s.size() - start);
            if (onBlock)
                onBlock ((long long) start);
            mx.setParams (values.data());
            float* ch[2] { s.l.data() + start, s.r.data() + start };
            float* d[2] { nullptr, nullptr };
            ProcessContext ctx;
            ctx.channelMode = channelMode;
            ctx.delta = legacyDelta;
            if (delta != nullptr)
            {
                d[0] = delta->l.data() + start;
                d[1] = delta->r.data() + start;
                ctx.deltaOut = d;
            }
            mx.process (ch, n, ctx);
        }
    }
};

//==============================================================================
// Signals
Stereo silence (double seconds, double sr)
{
    Stereo s;
    s.resize ((size_t) std::llround (seconds * sr));
    return s;
}

Stereo squares (double seconds, double sr)
{
    Stereo s = silence (seconds, sr);
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double t = (double) i / sr;
        s.l[i] = std::fmod (t * 100.0, 1.0) < 0.5 ? 1.0f : -1.0f;
        s.r[i] = std::fmod (t * 1000.0, 1.0) < 0.5 ? 1.0f : -1.0f;
    }
    return s;
}

Stereo bursts (double sr)
{
    Stereo s = silence (0.05, sr);
    for (double f : { 1000.0, 60.0, 5000.0 })
    {
        Stereo b = silence (0.12, sr);
        for (size_t i = 0; i < b.size(); ++i)
        {
            const double v = std::cos (2.0 * kPi * f * (double) i / sr);   // onset at the peak
            b.l[i] = (float) v;
            b.r[i] = (float) (-0.9 * v);
        }
        s.append (b);
        s.append (silence (0.05, sr));
    }
    return s;
}

Stereo whiteNoise (double seconds, double sr, std::uint32_t seed = 3)
{
    Stereo s = silence (seconds, sr);
    pstest::Lcg lcg (seed);
    for (size_t i = 0; i < s.size(); ++i)
    {
        s.l[i] = lcg.next();
        s.r[i] = lcg.next();
    }
    return s;
}

Stereo pinkNoise (double seconds, double sr, std::uint32_t seed = 5)
{
    Stereo s = silence (seconds, sr);
    pstest::Lcg lcg (seed);
    for (int c = 0; c < 2; ++c)
    {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        auto& v = c == 0 ? s.l : s.r;
        double peak = 0.0;
        std::vector<double> tmp (v.size());
        for (size_t i = 0; i < v.size(); ++i)
        {
            const double w = lcg.next();
            b0 = 0.99886 * b0 + w * 0.0555179;
            b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;
            b5 = -0.7616 * b5 - w * 0.0168980;
            tmp[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
            b6 = w * 0.115926;
            peak = std::max (peak, std::abs (tmp[i]));
        }
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = (float) (tmp[i] / peak);
    }
    return s;
}

Stereo spikes (double seconds, double sr)
{
    Stereo s = silence (seconds, sr);
    for (size_t i = 100; i < s.size(); i += 997)
    {
        s.l[i] = (i / 997) % 2 == 0 ? 1.0f : -1.0f;
        s.r[i] = -s.l[i];
        if (i + 1 < s.size())
            s.r[i + 1] = s.l[i];   // a two-sample doublet on the right: maximal intersample overshoot
    }
    return s;
}

Stereo interSample (double seconds, double sr)
{
    Stereo s = silence (seconds, sr);
    for (size_t i = 0; i < s.size(); ++i)
    {
        s.l[i] = (float) std::sin (kPi * 0.5 * (double) i + kPi * 0.25);                       // fs/4 at 45 degrees
        s.r[i] = (float) std::sin (2.0 * kPi * 0.45 * (double) i + 0.3) * (i % 4096 < 2048 ? 1.0f : 0.5f);   // near Nyquist
    }
    return s;
}

// Kick (pitch sweep 150 -> 50 Hz + click) every 0.5 s, sustained 55 Hz bass, hats on the off-beats. Peak ~0.9.
Stereo drumMix (double seconds, double sr, std::vector<size_t>* kickOnsets = nullptr)
{
    Stereo s = silence (seconds, sr);
    pstest::Lcg lcg (11);
    const size_t beat = (size_t) std::llround (0.5 * sr);
    std::vector<double> mono (s.size(), 0.0), hats (s.size(), 0.0);
    for (size_t onset = (size_t) (0.05 * sr); onset < s.size(); onset += beat)
    {
        if (kickOnsets != nullptr)
            kickOnsets->push_back (onset);
        double phase = 0.0;
        for (size_t i = 0; i < (size_t) (0.4 * sr) && onset + i < s.size(); ++i)
        {
            const double t = (double) i / sr;
            const double f = 50.0 + 100.0 * std::exp (-t / 0.03);
            phase += 2.0 * kPi * f / sr;
            double v = 0.8 * std::exp (-t / 0.15) * std::sin (phase);
            if (t < 0.002)
                v += 0.3 * lcg.next() * (1.0 - t / 0.002);
            mono[onset + i] += v;
        }
    }
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double t = (double) i / sr;
        const double note = std::fmod (t, 1.0) < 0.75 ? 1.0 : 0.0;
        mono[i] += note * 0.3 * (std::sin (2.0 * kPi * 55.0 * t) + 0.3 * std::sin (2.0 * kPi * 110.0 * t + 0.4));
    }
    double prev = 0.0;
    for (size_t onset = (size_t) (0.3 * sr); onset < s.size(); onset += beat / 2)
        for (size_t i = 0; i < (size_t) (0.06 * sr) && onset + i < s.size(); ++i)
        {
            const double w = lcg.next();
            hats[onset + i] += 0.5 * std::exp (-(double) i / (0.012 * sr)) * (w - prev);
            prev = w;
        }
    double peak = 0.0;
    for (size_t i = 0; i < s.size(); ++i)
        peak = std::max ({ peak, std::abs (mono[i] + 0.8 * hats[i]), std::abs (mono[i] + 0.5 * hats[i]) });
    for (size_t i = 0; i < s.size(); ++i)
    {
        s.l[i] = (float) (0.9 * (mono[i] + 0.8 * hats[i]) / peak);
        s.r[i] = (float) (0.9 * (mono[i] + 0.5 * hats[i]) / peak);
    }
    return s;
}

Stereo dcSteps (double sr)
{
    Stereo s;
    for (float v : { 1.0f, -1.0f, 0.0f, 1.0f, 0.3f, -1.0f })
    {
        Stereo seg = silence (0.06, sr);
        std::fill (seg.l.begin(), seg.l.end(), v);
        std::fill (seg.r.begin(), seg.r.end(), -v * 0.7f);
        s.append (seg);
    }
    return s;
}

// Everything nasty, back to back (~2.4 s).
Stereo torture (double sr)
{
    Stereo s = squares (0.25, sr);
    s.append (bursts (sr));
    s.append (whiteNoise (0.25, sr));
    s.append (pinkNoise (0.25, sr));
    s.append (spikes (0.2, sr));
    s.append (interSample (0.25, sr));
    s.append (drumMix (0.6, sr));
    s.append (dcSteps (sr));
    s.append (silence (0.05, sr));
    return s;
}

float samplePeak (const Stereo& s, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < s.size(); ++i)
        p = std::max ({ p, std::abs (s.l[i]), std::abs (s.r[i]) });
    return p;
}

float truePeakDb (const Stereo& s, size_t from = 0)
{
    TruePeakMeter m;
    m.prepare (48000.0);
    m.process (s.l.data() + from, s.r.data() + from, (int) (s.size() - from));
    return m.truePeakDb();
}

double dbOfLin (double v) { return v > 1.0e-12 ? 20.0 * std::log10 (v) : -240.0; }

std::string fmt (double v, int prec = 2)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision (prec) << v;
    return os.str();
}
} // namespace

//==============================================================================
TEST_CASE ("max. channel domains: LeftRight and MidSide keep detectors independent")
{
    for (auto domain : { ChannelMode::LeftRight, ChannelMode::MidSide })
        for (int mode = 0; mode < (int) MaximizerMode::Count; ++mode)
        {
            Rig rig (48000.0);
            rig.channelMode = domain;
            rig.set ("max.mode", (float) mode);
            rig.set ("max.ceiling", 0.0f);
            rig.restart();
            Stereo s = silence (1.0, rig.sr);
            std::fill (s.l.begin(), s.l.end(), 4.0f);
            std::fill (s.r.begin(), s.r.end(), 0.1f);
            rig.run (s);
            // Default SI=0 must not link Left/Right or Mid/Side detection.
            CHECK (std::abs (pstest::rmsOf (s.r, 24000, 24000) - 0.1) < 1.0e-5);
        }

    // A loud channel must not suppress upward compression or trigger emphasis on its neighbour.
    for (auto domain : { ChannelMode::LeftRight, ChannelMode::MidSide })
    {
        Rig rig (48000.0);
        rig.channelMode = domain;
        rig.set ("max.upward", 8.0f);
        rig.set ("max.ceiling", 0.0f);
        rig.restart();
        Stereo s = silence (2.0, rig.sr);
        std::fill (s.l.begin(), s.l.end(), 0.8f);
        std::fill (s.r.begin(), s.r.end(), 0.02f);
        rig.run (s);
        CHECK (dbOfLin (pstest::rmsOf (s.r, 72000, 12000) / 0.02) > 7.8);

        Rig emphasis (48000.0);
        emphasis.channelMode = domain;
        emphasis.set ("max.trans.on", 1.0f);
        emphasis.set ("max.trans.amount", 100.0f);
        emphasis.restart();
        Stereo e = silence (1.5, emphasis.sr);
        std::fill (e.r.begin(), e.r.end(), 0.02f);
        for (size_t i = 36000; i < 40000; ++i)
            e.l[i] = 0.4f;
        emphasis.run (e);
        CHECK (std::abs (pstest::peakOf (e.r, 35000, 12000) - 0.02) < 1.0e-6);
    }
}

TEST_CASE ("max. neutral settings: every mode is an exact delay of its latency (null < -120 dB)")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int m = 0; m < (int) MaximizerMode::Count; ++m)
        {
            Rig rig (sr);
            rig.set ("max.mode", (float) m);
            rig.restart();
            const int lat = rig.mx.getLatencySamples();
            Stereo in = whiteNoise (0.6, sr, 21);
            for (size_t i = 0; i < in.size(); ++i)
            {
                in.l[i] *= 0.03f;
                in.r[i] *= 0.03f;
            }
            Stereo out = in;
            rig.run (out);
            double worst = 0.0;
            for (size_t i = (size_t) lat; i < out.size(); ++i)
                worst = std::max ({ worst, (double) std::abs (out.l[i] - in.l[i - (size_t) lat]),
                                    (double) std::abs (out.r[i] - in.r[i - (size_t) lat]) });
            for (size_t i = 0; i < (size_t) lat; ++i)
                worst = std::max ({ worst, (double) std::abs (out.l[i]), (double) std::abs (out.r[i]) });
            if (dbOfLin (worst) >= -120.0)
                std::cout << "    sr " << sr << " mode " << m << " null " << dbOfLin (worst) << " dB\n";
            CHECK (dbOfLin (worst) < -120.0);
        }
}

TEST_CASE ("max. latency: constant per mode, measured impulse delay == reported, Low Latency <= ~1 ms")
{
    std::cout << "    latency (samples / ms):\n";
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        std::cout << "      " << sr << " Hz:";
        for (int m = 0; m < (int) MaximizerMode::Count; ++m)
        {
            Rig rig (sr);
            rig.set ("max.mode", (float) m);
            rig.restart();
            const int lat = rig.mx.getLatencySamples();
            // the same with True Peak off, other characters / styles, soft clip on
            rig.set ("max.truePeak", 0.0f);
            rig.set ("max.character", 9.0f);
            rig.set ("max.clip.on", 1.0f);
            rig.set ("max.style3", 0.0f);
            rig.set ("max.style4", 2.0f);
            rig.mx.setParams (rig.values.data());
            CHECK_EQ (rig.mx.getLatencySamples(), lat);
            rig.restart();
            Stereo s = silence (0.2 + 2.0 * lat / sr, sr);
            s.l[10] = 0.01f;
            s.r[10] = -0.01f;
            rig.run (s);
            size_t at = 0;
            for (size_t i = 0; i < s.size(); ++i)
                if (std::abs (s.l[i]) > std::abs (s.l[at]))
                    at = i;
            CHECK_EQ ((int) at - 10, lat);
            CHECK (std::abs (s.l[at] - 0.01f) < 1.0e-6f);
            std::cout << "  " << lat << " (" << fmt (1000.0 * lat / sr) << ")";
            if (m == 0)
                CHECK (1000.0 * lat / sr <= 1.25);
        }
        std::cout << "\n";
    }
}

TEST_CASE ("max. contract: output never above the output level (sample peak; true peak + 0.05 dB) on nasty material")
{
    // Every mode / style, characters 0 / 3 / 10 (output level 0 / -1 (-0.3 with True Peak off) / -6.5 dB), True Peak
    // on / off, 44.1 / 48 / 96 kHz, +20 dB gain into full-scale
    // squares, onset bursts, white / pink noise, spikes, intersample test tones, a drum mix and DC steps.
    double worstTpOver = -100.0, worstSpOver = -100.0;
    std::string worstTpWhat, worstSpWhat;
    int runs = 0;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const Stereo input = torture (sr);
        for (const auto& mc : kModes)
            for (float character : { 0.0f, 3.0f, 10.0f })
                for (int tp = 0; tp < 2; ++tp)
                {
                    const float ceilingDb = character == 0.0f ? 0.0f
                                          : (character == 10.0f ? -6.5f : (tp ? -1.0f : -0.3f));
                    Rig rig (sr);
                    rig.setMode (mc);
                    rig.set ("max.gain", 20.0f);
                    rig.set ("max.ceiling", ceilingDb);
                    rig.set ("max.truePeak", (float) tp);
                    rig.set ("max.character", character);
                    rig.restart();
                    Stereo out = input;
                    rig.run (out);
                    ++runs;
                    const std::string what = std::string (mc.name) + " ch " + fmt (character, 0) + " sr " + fmt (sr, 0);
                    if (tp)
                    {
                        const double over = truePeakDb (out) - ceilingDb;
                        if (over > worstTpOver)
                        {
                            worstTpOver = over;
                            worstTpWhat = what;
                        }
                        CHECK (over <= 0.05);
                    }
                    else
                    {
                        const double lim = std::pow (10.0, ceilingDb / 20.0);
                        const double over = dbOfLin (samplePeak (out)) - ceilingDb;
                        if (over > worstSpOver)
                        {
                            worstSpOver = over;
                            worstSpWhat = what;
                        }
                        CHECK ((double) samplePeak (out) <= lim);
                    }
                }
    }
    std::cout << "    " << runs << " runs; worst true peak re output level " << fmt (worstTpOver, 4) << " dB ("
              << worstTpWhat << "), worst sample peak " << fmt (worstSpOver, 5) << " dB (" << worstSpWhat << ")\n";
}

//==============================================================================
namespace
{
// THD+N of a single tone in y[start, start + n): everything except the fundamental, re the fundamental.
double thdNDb (const std::vector<float>& y, size_t start, size_t n, double freq, double sr)
{
    const auto a = pstest::toneOf (y, start, n, freq, sr);
    double res = 0.0, fund = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double ph = 2.0 * kPi * freq * (double) (start + i) / sr;
        const double f = a.real() * std::cos (ph) - a.imag() * std::sin (ph);
        const double r = (double) y[start + i] - f;
        res += r * r;
        fund += f * f;
    }
    return 10.0 * std::log10 (std::max (res, 1.0e-30) / std::max (fund, 1.0e-30));
}

// Intermodulation of a 60 Hz + 7 kHz pair: sidebands 7 kHz +- k * 60 Hz (k = 1..4) re the 7 kHz tone.
double imdDb (const std::vector<float>& y, size_t start, size_t n, double sr)
{
    const double hi = std::abs (pstest::toneOf (y, start, n, 7000.0, sr));
    double side = 0.0;
    for (int k = 1; k <= 4; ++k)
        for (int sgn : { -1, 1 })
        {
            const double a = std::abs (pstest::toneOf (y, start, n, 7000.0 + sgn * 60.0 * k, sr));
            side += a * a;
        }
    return 10.0 * std::log10 (std::max (side, 1.0e-30) / std::max (hi * hi, 1.0e-30));
}

// Harmonics 2..5 of the 60 Hz tone re the 60 Hz tone (low-frequency distortion from gain modulation).
double bassThdDb (const std::vector<float>& y, size_t start, size_t n, double sr)
{
    const double f0 = std::abs (pstest::toneOf (y, start, n, 60.0, sr));
    double h = 0.0;
    for (int k = 2; k <= 5; ++k)
    {
        const double a = std::abs (pstest::toneOf (y, start, n, 60.0 * k, sr));
        h += a * a;
    }
    return 10.0 * std::log10 (std::max (h, 1.0e-30) / std::max (f0 * f0, 1.0e-30));
}

// Mean over kick hits of (attack peak / body RMS) at the output re the same at the input, in dB. 0 dB = the hits keep
// their shape; negative = squashed.
double transientRetentionDb (const Stereo& in, const Stereo& out, int lat, const std::vector<size_t>& onsets, double sr)
{
    const size_t att = (size_t) (0.004 * sr), b0 = (size_t) (0.03 * sr), b1 = (size_t) (0.09 * sr);
    double sum = 0.0;
    int count = 0;
    for (size_t k = 1; k < onsets.size(); ++k)
    {
        const size_t o = onsets[k];
        if (o + b1 + (size_t) lat >= out.size())
            break;
        auto ratio = [&] (const Stereo& s, size_t at)
        {
            const double p = std::max (pstest::peakOf (s.l, at, att), pstest::peakOf (s.r, at, att));
            const double r = 0.5 * (pstest::rmsOf (s.l, at + b0, b1 - b0) + pstest::rmsOf (s.r, at + b0, b1 - b0));
            return dbOfLin (p) - dbOfLin (r);
        };
        sum += ratio (out, o + (size_t) lat) - ratio (in, o);
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

// Kick attack retention: RMS of the output over the first 5 ms of each hit re the unlimited (input x gain) signal, and
// the same for the body (30..90 ms). "punch" = attack - body: 0 dB = the hit loses no more than the sound around it.
struct Punch
{
    double attack = 0.0, body = 0.0;
};

Punch punchOf (const Stereo& in, double gainDb, const Stereo& out, int lat, const std::vector<size_t>& onsets, double sr)
{
    const size_t att = (size_t) (0.005 * sr), b0 = (size_t) (0.03 * sr), b1 = (size_t) (0.09 * sr);
    Punch p;
    int count = 0;
    for (size_t k = 1; k < onsets.size(); ++k)
    {
        const size_t o = onsets[k];
        if (o + b1 + (size_t) lat >= out.size())
            break;
        auto level = [] (const Stereo& s, size_t at, size_t n)
        {
            return dbOfLin (0.5 * (pstest::rmsOf (s.l, at, n) + pstest::rmsOf (s.r, at, n)));
        };
        p.attack += level (out, o + (size_t) lat, att) - level (in, o, att) - gainDb;
        p.body += level (out, o + (size_t) lat + b0, b1 - b0) - level (in, o + b0, b1 - b0) - gainDb;
        ++count;
    }
    if (count > 0)
    {
        p.attack /= count;
        p.body /= count;
    }
    return p;
}

Stereo tone (double f, double peak, double seconds, double sr)
{
    Stereo s = silence (seconds, sr);
    for (size_t i = 0; i < s.size(); ++i)
        s.l[i] = s.r[i] = (float) (peak * std::sin (2.0 * kPi * f * (double) i / sr));
    return s;
}

Stereo imdPair (double peak, double seconds, double sr)
{
    Stereo s = silence (seconds, sr);
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double t = (double) i / sr;
        s.l[i] = s.r[i] = (float) (peak * (0.8 * std::sin (2.0 * kPi * 60.0 * t) + 0.2 * std::sin (2.0 * kPi * 7000.0 * t)));
    }
    return s;
}

struct Quality
{
    double thdN = 0, imd = 0, bassThd = 0, lufs = 0, crest = 0, toneGr = 0;
    Punch punch;
};

Quality measureQuality (const ModeCfg& mc, float character = 3.0f)
{
    const double sr = 48000.0;
    Quality q;
    const float ceilingDb = -1.0f;
    const double ceilLin = std::pow (10.0, ceilingDb / 20.0);
    {
        Rig rig (sr);
        rig.setMode (mc);
        rig.set ("max.gain", 6.0f);
        rig.set ("max.ceiling", ceilingDb);
        rig.set ("max.character", character);
        rig.restart();
        Stereo s = tone (1000.0, ceilLin, 2.0, sr);
        rig.run (s);
        const size_t start = (size_t) rig.mx.getLatencySamples() + (size_t) sr;
        q.thdN = thdNDb (s.l, start, (size_t) sr / 2, 1000.0, sr);
        q.toneGr = dbOfLin (std::abs (pstest::toneOf (s.l, start, (size_t) sr / 2, 1000.0, sr))) - ceilingDb;
    }
    {
        Rig rig (sr);
        rig.setMode (mc);
        rig.set ("max.gain", 6.0f);
        rig.set ("max.ceiling", ceilingDb);
        rig.set ("max.character", character);
        rig.restart();
        Stereo s = imdPair (ceilLin, 2.5, sr);
        rig.run (s);
        const size_t start = (size_t) rig.mx.getLatencySamples() + (size_t) sr;
        q.imd = imdDb (s.l, start, (size_t) sr, sr);
        q.bassThd = bassThdDb (s.l, start, (size_t) sr, sr);
    }
    {
        Rig rig (sr);
        rig.setMode (mc);
        rig.set ("max.gain", 8.0f);
        rig.set ("max.ceiling", ceilingDb);
        rig.set ("max.character", character);
        rig.restart();
        std::vector<size_t> onsets;
        const Stereo in = drumMix (6.0, sr, &onsets);
        Stereo s = in;
        rig.run (s);
        const int lat = rig.mx.getLatencySamples();
        const size_t from = (size_t) lat + (size_t) sr;
        q.lufs = measureIntegratedLufs (s.l.data() + from, s.r.data() + from, (long long) (s.size() - from), sr);
        q.crest = crestFactorDb (s.l.data() + from, s.r.data() + from, (long long) (s.size() - from));
        q.punch = punchOf (in, 8.0, s, lat, onsets, sr);
    }
    return q;
}
} // namespace

TEST_CASE ("max. quality: THD+N, IMD, loudness, crest and transient retention per mode (printed)")
{
    std::cout << "    48 kHz, character 3, output level -1 dB. Tone tests: pushed 6 dB into the limiter. Mix: +8 dB.\n"
              << "    Kick: attack = level of the first 5 ms re unlimited, punch = attack - body (30-90 ms).\n"
              << "    mode                  1k THD+N  IMD 60/7k  60Hz harm  mix LUFS   crest   attack   punch  tone level\n";
    std::vector<Quality> all;
    for (const auto& mc : kModes)
    {
        const auto q = measureQuality (mc);
        all.push_back (q);
        std::cout << "    " << std::left << std::setw (21) << mc.name << std::right << std::setw (9) << fmt (q.thdN, 1)
                  << std::setw (11) << fmt (q.imd, 1) << std::setw (11) << fmt (q.bassThd, 1) << std::setw (10)
                  << fmt (q.lufs, 2) << std::setw (8) << fmt (q.crest, 2) << std::setw (9) << fmt (q.punch.attack, 2)
                  << std::setw (8) << fmt (q.punch.attack - q.punch.body, 2) << std::setw (12) << fmt (q.toneGr, 2) << "\n";
        CHECK (q.toneGr > -0.5);   // a steady tone is limited right up to the output level
        // (a clean mode's fundamental cannot exceed it; a flattened wave's fundamental can, its peak cannot)
        CHECK (q.toneGr < 0.01 || q.thdN > -60.0);
    }
    // Smooth / Transparent: clean on a sustained 1 kHz tone. They follow the reference's default and transparent
    // algorithms, which at low character shape sustained bass almost like a clipper (measured: 60 Hz sine 2 dB over
    // -> THD+N about -20 dB at character 1.4), so their bass intermodulation is not bounded here.
    CHECK (all[1].thdN < -70.0);
    CHECK (all[2].thdN < -70.0);
    // Aggressive (any style) is louder on the mix than Transparent, Spectral and Multiband (Smooth and Low Latency
    // follow their own references)
    double bestAggressive = -100.0, bestOther = -100.0;
    for (size_t i = 0; i < all.size(); ++i)
    {
        if (kModes[i].mode == (int) MaximizerMode::Smooth || kModes[i].mode == (int) MaximizerMode::LowLatency)
            continue;
        double& best = kModes[i].mode == 3 ? bestAggressive : bestOther;
        best = std::max (best, all[i].lufs);
    }
    CHECK (bestAggressive >= bestOther);
}

TEST_CASE ("max. cpu: real-time factor per mode at 48 kHz stereo (Spectral / Multiband >= 30x)")
{
    const double sr = 48000.0;
    const Stereo in = drumMix (10.0, sr);
    std::cout << "    real-time factor (10 s drum mix, +8 dB, blocks of 512):";
    for (int m = 0; m < (int) MaximizerMode::Count; ++m)
    {
        Rig rig (sr);
        rig.set ("max.mode", (float) m);
        rig.set ("max.gain", 8.0f);
        rig.restart();
        Stereo s = in;
        pstest::Stopwatch sw;
        rig.run (s);
        const double factor = 10.0 / std::max (sw.seconds(), 1.0e-6);
        std::cout << " " << m << ": " << fmt (factor, 0) << "x";
        CHECK (factor >= 30.0);
    }
    std::cout << "\n";
}

TEST_CASE ("max. realtime: setParams / process / reset / mode switches / learn allocate nothing")
{
    Rig rig (48000.0, 256);
    Stereo s = drumMix (0.5, 48000.0);
    std::vector<float> l (256), r (256), dl (256), dr (256);
    long long allocations = 0;
    const char* ids[] { "max.mode", "max.style3", "max.style4", "max.gain", "max.ceiling", "max.truePeak", "max.character",
                        "max.clip.on", "max.clip.mode", "max.trans.on", "max.trans.amount", "max.upward", "max.si.trans",
                        "max.si.sus" };
    int idx[14];
    for (int i = 0; i < 14; ++i)
        idx[i] = paramIndex (ids[i]);
    auto& v = rig.values;
    {
        pstest::AllocationScope scope;
        rig.mx.startLearn();
        for (int b = 0; b < 400; ++b)
        {
            const float vals[14] { (float) ((b / 20) % 6), (float) ((b / 7) % 4), (float) ((b / 11) % 3), (float) (b % 21),
                                   -(float) (b % 9), (float) ((b / 13) % 2), (float) (b % 11), (float) ((b / 17) % 2),
                                   (float) (b % 3), (float) ((b / 5) % 2), 80.0f, (float) (b % 13), (float) (b % 101),
                                   (float) ((b * 7) % 101) };
            for (int i = 0; i < 14; ++i)
                v[(size_t) idx[i]] = vals[i];
            rig.mx.setParams (rig.values.data());
            for (int i = 0; i < 256; ++i)
            {
                const size_t k = ((size_t) b * 256 + (size_t) i) % s.size();
                l[(size_t) i] = s.l[k];
                r[(size_t) i] = s.r[k];
            }
            float* ch[2] { l.data(), r.data() };
            float* d[2] { dl.data(), dr.data() };
            ProcessContext ctx;
            ctx.deltaOut = (b % 3) != 0 ? d : nullptr;
            ctx.delta = (b % 5) == 0;
            rig.mx.process (ch, 256, ctx);
            if (b % 97 == 0)
                rig.mx.reset();
            if (b == 300)
                rig.mx.cancelLearn();
        }
        allocations = scope.count();
    }
    CHECK_EQ (allocations, 0LL);
}

TEST_CASE ("max. delta: deltaOut = output - (input x gain) aligned, normal output unchanged; legacy in-place delta")
{
    const double sr = 48000.0;
    const Stereo in = drumMix (1.5, sr);
    auto make = [&]
    {
        auto rig = std::make_unique<Rig> (sr, 300);
        rig->set ("max.gain", 9.0f);
        rig->set ("max.upward", 4.0f);
        rig->set ("max.clip.on", 1.0f);
        rig->restart();
        return rig;
    };
    Stereo plain = in, withDelta = in, legacy = in, bothFlags = in, delta, bothDelta;
    make()->run (plain);
    auto rig = make();
    rig->run (withDelta, {}, &delta);
    make()->run (legacy, {}, nullptr, true);
    make()->run (bothFlags, {}, &bothDelta, true);
    const int lat = rig->mx.getLatencySamples();
    const double g = std::pow (10.0, 9.0 / 20.0);

    double outDiff = 0.0, recon = 0.0, legacyDiff = 0.0, deltaPeak = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
    {
        outDiff = std::max (outDiff, (double) std::abs (plain.l[i] - withDelta.l[i]));
        legacyDiff = std::max (legacyDiff, (double) std::abs (legacy.l[i] - delta.l[i]));
        deltaPeak = std::max (deltaPeak, (double) std::abs (delta.l[i]));
        if (i >= (size_t) lat)
        {
            const double ref = (double) (float) ((double) in.l[i - (size_t) lat] * g);
            recon = std::max (recon, std::abs ((double) withDelta.l[i] - (double) delta.l[i] - ref));
        }
    }
    CHECK_EQ (outDiff, 0.0);
    CHECK (recon < 1.0e-6);
    CHECK_EQ (legacyDiff, 0.0);
    CHECK (deltaPeak > 0.1);   // the limiter did work
    CHECK (bothFlags.l == plain.l && bothFlags.r == plain.r);
    CHECK (bothDelta.l == delta.l && bothDelta.r == delta.r);

    // neutral settings: the delta is silent
    Rig quiet (sr);
    Stereo q = whiteNoise (0.5, sr);
    for (size_t i = 0; i < q.size(); ++i)
    {
        q.l[i] *= 0.05f;
        q.r[i] *= 0.05f;
    }
    Stereo qd;
    quiet.run (q, {}, &qd);
    CHECK (samplePeak (qd) == 0.0f);
}

TEST_CASE ("max. learn: listens to 5 s of playing audio, publishes the gain once, progress")
{
    const double sr = 48000.0;
    Rig rig (sr);
    rig.set ("max.learnTarget", -12.0f);
    rig.restart();
    CHECK (! rig.mx.isLearning());
    float gainDb = 0.0f;
    CHECK (! rig.mx.takeLearnedGain (gainDb));

    rig.mx.startLearn();
    CHECK (rig.mx.isLearning());
    Stereo quiet = silence (2.0, sr);
    rig.run (quiet);
    CHECK (rig.mx.getLearnProgress() == 0.0f);   // silence does not count
    CHECK (! rig.mx.takeLearnedGain (gainDb));

    Stereo noise = whiteNoise (2.5, sr, 99);
    for (size_t i = 0; i < noise.size(); ++i)
    {
        noise.l[i] *= 0.08f;
        noise.r[i] *= 0.08f;
    }
    const float inputLufs = measureIntegratedLufs (noise.l.data(), noise.r.data(), (long long) noise.size(), sr);
    Stereo a = noise;
    rig.run (a);
    const float half = rig.mx.getLearnProgress();
    CHECK (half > 0.45f && half < 0.55f);
    CHECK (rig.mx.isLearning());
    Stereo b = noise;
    rig.run (b);
    Stereo c = noise;
    rig.run (c);
    CHECK (rig.mx.getLearnProgress() == 1.0f);
    CHECK (rig.mx.isLearning());   // until the result is taken
    REQUIRE (rig.mx.takeLearnedGain (gainDb));
    CHECK (! rig.mx.isLearning());
    float again = 0.0f;
    CHECK (! rig.mx.takeLearnedGain (again));
    std::cout << "    white noise at " << fmt (inputLufs) << " LUFS, target -12: learned " << fmt (gainDb, 1) << " dB\n";
    CHECK (std::abs (gainDb - (-12.0f - inputLufs)) < 1.0f);

    // dense material that needs limiting: the learned gain must account for it and land near the target
    Rig loud (sr);
    loud.set ("max.learnTarget", -10.0f);
    loud.set ("max.mode", (float) MaximizerMode::Smooth);
    loud.restart();
    loud.mx.startLearn();
    const Stereo mix = drumMix (6.0, sr);
    Stereo m1 = mix;
    loud.run (m1);
    REQUIRE (loud.mx.takeLearnedGain (gainDb));
    Rig verify (sr);
    verify.set ("max.mode", (float) MaximizerMode::Smooth);
    verify.set ("max.gain", gainDb);
    verify.restart();
    Stereo m2 = mix;
    verify.run (m2);
    const size_t from = (size_t) verify.mx.getLatencySamples();
    const float outLufs = measureIntegratedLufs (m2.l.data() + from, m2.r.data() + from, (long long) (m2.size() - from), sr);
    const float mixLufs = measureIntegratedLufs (mix.l.data(), mix.r.data(), (long long) mix.size(), sr);
    std::cout << "    drum mix at " << fmt (mixLufs) << " LUFS, target -10: learned " << fmt (gainDb, 1)
              << " dB -> output " << fmt (outLufs) << " LUFS\n";
    CHECK (gainDb > (-10.0f - mixLufs) - 0.1f);   // at least the unlimited estimate: limiting was accounted for
    CHECK (std::abs (outLufs - (-10.0f)) < 0.75f);

    // cancel
    loud.mx.startLearn();
    CHECK (loud.mx.isLearning());
    loud.mx.cancelLearn();
    CHECK (! loud.mx.isLearning());
}

//==============================================================================
TEST_CASE ("max. automation: stereo independence ramps in both directions in every mode")
{
    constexpr double sr = 48000.0;
    for (int mode = 0; mode < (int) MaximizerMode::Count; ++mode)
    {
        Rig rig (sr, 64);
        rig.set ("max.mode", (float) mode);
        rig.set ("max.character", 0.0f); // one-sample Guard attack used to expose a full gain step
        rig.set ("max.truePeak", 0.0f);
        rig.restart();
        Stereo s = silence (0.8, sr);
        std::fill (s.l.begin(), s.l.end(), 2.0f);
        std::fill (s.r.begin(), s.r.end(), 0.2f);
        rig.run (s, [&] (long long at)
        {
            const float independence = at >= 24000 ? 0.0f : (at >= 12000 ? 100.0f : 0.0f);
            rig.set ("max.si.trans", independence);
            rig.set ("max.si.sus", independence);
        });
        double worst = 0.0;
        for (size_t i = 11000; i < s.size(); ++i)
            worst = std::max (worst, (double) std::abs (s.r[i] - s.r[i - 1]));
        if (worst >= 0.002)
            std::cout << "    mode " << mode << " independence automation step " << worst << "\n";
        CHECK (worst < 0.002); // old LowLatency output jumped 0.089125 -> 0.2 in one sample
        CHECK (s.r[22000] > s.r[11000] + 0.025f);
        CHECK (s.r[38000] < s.r[22000] - 0.025f);
        CHECK ((double) samplePeak (s) <= std::pow (10.0, -1.0 / 20.0));
    }
}

TEST_CASE ("max. automation: Guard attack changes do not jump between gain histories")
{
    for (const bool longer : { false, true })
    {
        maxi::Guard guard;
        guard.prepare (48000.0, 12);
        guard.setMaxAttack (12);
        guard.setTimes (longer ? 1 : 12, 1.0, 1.0);
        guard.reset();
        std::array<float, 256> output {};
        const double threshold = 1.0;
        const std::uint8_t tp = 0;
        for (size_t i = 0; i < output.size(); ++i)
        {
            if (i == 136)
                guard.setTimes (longer ? 12 : 1, 1.0, 1.0);
            float left = i < 80 ? 2.0f : 0.6f, right = left;
            const float* in[2] { &left, &right };
            float* out[2] { &left, &right };
            guard.process (in, out, &threshold, &tp, 1);
            output[i] = left;
        }
        double step = 0.0;
        for (size_t i = 136; i < 190; ++i)
            step = std::max (step, (double) std::abs (output[i] - output[i - 1]));
        std::cout << "    Guard attack " << (longer ? "1 -> 12" : "12 -> 1") << " step " << step << "\n";
        CHECK (step < 0.008);
        CHECK_EQ (guard.getLatency(), 26);
    }
}

TEST_CASE ("max. automation: soft clip mode changes ramp the curve in every mode")
{
    constexpr double sr = 48000.0;
    for (int mode = 0; mode < (int) MaximizerMode::Count; ++mode)
    {
        Rig rig (sr, 64);
        rig.set ("max.mode", (float) mode);
        rig.set ("max.ceiling", 0.0f);
        rig.set ("max.truePeak", 0.0f);
        rig.set ("max.clip.on", 1.0f);
        rig.set ("max.clip.amount", 100.0f);
        rig.set ("max.clip.mode", 0.0f);
        rig.restart();
        Stereo s = silence (0.8, sr);
        std::fill (s.l.begin(), s.l.end(), 0.6f);
        std::fill (s.r.begin(), s.r.end(), -0.6f);
        rig.run (s, [&] (long long at)
        {
            rig.set ("max.clip.mode", at >= 24000 ? 0.0f : (at >= 12000 ? 2.0f : 0.0f));
        });
        double worst = 0.0;
        for (size_t i = 11000; i < s.size(); ++i)
            worst = std::max ({ worst, (double) std::abs (s.l[i] - s.l[i - 1]),
                               (double) std::abs (s.r[i] - s.r[i - 1]) });
        if (worst >= 0.001)
            std::cout << "    mode " << mode << " soft clip automation step " << worst << "\n";
        CHECK (worst < 0.001);
        CHECK (std::abs (s.l[11000] - 0.6f) < 1.0e-6f);
        CHECK (s.l[22000] < 0.56f); // Heavy actually changed the curve
        CHECK (std::abs (s.l[38000] - 0.6f) < 1.0e-6f);
        CHECK (std::abs (s.l[22000] + s.r[22000]) < 1.0e-6f);
    }
}

TEST_CASE ("max. automation: Character cannot restore old unity gains around a limited impulse")
{
    Rig changed (48000.0, 1), reference (48000.0, 1);
    for (auto* rig : { &changed, &reference })
    {
        rig->set ("max.mode", (float) MaximizerMode::LowLatency);
        rig->set ("max.ceiling", 0.0f);
        rig->set ("max.character", 6.0f);   // a release of ~9 ms (at 0 it is a few samples: the reference steps too)
        rig->set ("max.truePeak", 0.0f);
        rig->restart();
    }
    Stereo a = silence (0.1, 48000.0);
    std::fill (a.l.begin(), a.l.end(), 0.9f);
    std::fill (a.r.begin(), a.r.end(), 0.9f);
    a.l[2000] = a.r[2000] = 10.0f;
    Stereo b = a;
    changed.run (a, [&] (long long at) { if (at == 2014) changed.set ("max.character", 10.0f); });
    reference.run (b);
    double addedStep = 0.0;
    for (size_t i = 1901; i < 2200; ++i)
        addedStep = std::max (addedStep, std::abs ((double) (a.l[i] - a.l[i - 1])
                                                   - (double) (b.l[i] - b.l[i - 1])));
    std::cout << "    Character change incremental step " << addedStep << "\n";
    CHECK (addedStep < 0.002); // previously +0.675 at output sample 2032
    CHECK_EQ (changed.mx.getLatencySamples(), reference.mx.getLatencySamples());
    CHECK (samplePeak (a) <= 1.0f);
}

TEST_CASE ("max. automation: soft clip follows every ceiling sample independent of host block size")
{
    Stereo input = silence (0.2, 48000.0);
    std::fill (input.l.begin(), input.l.end(), 0.6f);
    std::fill (input.r.begin(), input.r.end(), -0.6f);
    auto render = [&] (int block)
    {
        Rig rig (48000.0, block);
        rig.set ("max.mode", (float) MaximizerMode::LowLatency);
        rig.set ("max.ceiling", 0.0f);
        rig.set ("max.truePeak", 0.0f);
        rig.set ("max.clip.on", 1.0f);
        rig.set ("max.clip.amount", 100.0f);
        rig.set ("max.clip.mode", 2.0f);
        rig.restart();
        Stereo s = input;
        rig.run (s, [&] (long long at) { if (at >= 4800) rig.set ("max.ceiling", -20.0f); });
        return s;
    };
    const Stereo single = render (1), blocks = render (64);
    double error = 0.0, step = 0.0;
    for (size_t i = 4800; i < 6800; ++i)
    {
        error = std::max ({ error, (double) std::abs (single.l[i] - blocks.l[i]),
                           (double) std::abs (single.r[i] - blocks.r[i]) });
        step = std::max (step, (double) std::abs (blocks.l[i] - blocks.l[i - 1]));
    }
    std::cout << "    soft clip ceiling block-size error " << error << ", largest step " << step << "\n";
    CHECK (error < 1.0e-6);
    CHECK (step < 0.002);
}

namespace
{
// Exact replica of ps::TruePeakMeter's interpolation, giving the true peak per output sample, so a time-varying bound
// can be checked. Verified against the meter below.
struct TpReplica
{
    std::array<std::array<float, 16>, 4> ph {};
    TpReplica()
    {
        std::array<double, 64> h {};
        auto i0 = [] (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 64; ++k)
            {
                const double f = 0.5 * x / k;
                term *= f * f;
                sum += term;
                if (term < 1.0e-14 * sum)
                    break;
            }
            return sum;
        };
        for (int n = 0; n < 64; ++n)
        {
            const double x = (n - 31.5) / 4.0;
            const double s = std::sin (kPi * x) / (kPi * x);
            const double r = (n - 31.5) / 31.5;
            h[(size_t) n] = s * i0 (7.0 * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0 (7.0);
        }
        for (int p = 0; p < 4; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < 16; ++k)
                sum += h[(size_t) (p + 4 * k)];
            for (int k = 0; k < 16; ++k)
                ph[(size_t) p][(size_t) k] = (float) (h[(size_t) (p + 4 * k)] / sum);
        }
    }
    // per sample n: max of |x[n]| and the 4 points the meter interpolates at n
    std::vector<float> perSample (const std::vector<float>& x) const
    {
        std::vector<float> out (x.size(), 0.0f);
        for (size_t n = 0; n < x.size(); ++n)
        {
            float m = std::abs (x[n]);
            for (const auto& p : ph)
            {
                float y = 0.0f;
                for (int k = 0; k < 16; ++k)
                    y += n >= (size_t) k ? p[(size_t) k] * x[n - (size_t) k] : 0.0f;
                m = std::max (m, std::abs (y));
            }
            out[n] = m;
        }
        return out;
    }
};
} // namespace

TEST_CASE ("max. true-peak replica used by the automation test matches ps::TruePeakMeter")
{
    const TpReplica tp;
    Stereo s = interSample (0.1, 48000.0);
    s.append (whiteNoise (0.1, 48000.0));
    const auto per = tp.perSample (s.l);
    const float rep = *std::max_element (per.begin(), per.end());
    TruePeakMeter m;
    m.prepare (48000.0);
    m.process (s.l.data(), nullptr, (int) s.size());
    CHECK (std::abs (rep - m.truePeakLinear()) < 1.0e-6f);
}

TEST_CASE ("max. coarse mode-switch smoke bound under gain / output level / mode / character automation")
{
    const TpReplica tp;
    for (double sr : { 44100.0, 96000.0 })
        for (int tpOn = 0; tpOn < 2; ++tpOn)
        {
            Rig rig (sr, 256);
            rig.set ("max.truePeak", (float) tpOn);
            rig.set ("max.gain", 12.0f);
            rig.restart();
            Stereo s = torture (sr);
            s.append (drumMix (2.0, sr));
            const size_t total = s.size();
            pstest::Lcg lcg (77);
            float ceilingNow = -1.0f;
            std::vector<float> ceilAt (total, ceilingNow);
            int switches = 0;
            rig.run (s, [&] (long long start)
            {
                if (start > 0 && (start / 256) % 9 == 0)
                {
                    rig.set ("max.gain", 20.0f * (0.5f + 0.5f * lcg.next()));
                    ceilingNow = -12.0f * (0.5f + 0.5f * lcg.next());
                    rig.set ("max.ceiling", ceilingNow);
                    rig.set ("max.character", 10.0f * (0.5f + 0.5f * lcg.next()));
                    if ((start / 256) % 27 == 0)
                    {
                        const int m = std::clamp ((int) ((0.5f + 0.5f * lcg.next()) * 11.0f), 0, 10);
                        rig.setMode (kModes[(size_t) m]);
                        ++switches;
                    }
                }
                for (size_t i = (size_t) start; i < std::min (total, (size_t) start + 256); ++i)
                    ceilAt[i] = ceilingNow;
            });
            // bound at output sample i: the largest output level set in the window the sample (and the 16 samples
            // the interpolator reads) can come from: max latency (27 ms) + 30 ms ramp + one block
            const size_t look = (size_t) (0.06 * sr) + 256 + 16;
            std::vector<float> runMax (total);
            {
                // sliding maximum of ceilAt over [i - look, i]
                for (size_t i = 0; i < total; ++i)
                {
                    float b = ceilAt[i];
                    if (i % 64 == 0 || i == 0)
                    {
                        for (size_t j = i >= look ? i - look : 0; j < i; ++j)
                            b = std::max (b, ceilAt[j]);
                    }
                    else
                        b = std::max (runMax[i - 1], ceilAt[i]);
                    runMax[i] = b;
                }
            }
            const auto peaksL = tpOn ? tp.perSample (s.l) : s.l;
            const auto peaksR = tpOn ? tp.perSample (s.r) : s.r;
            double worst = -100.0;
            size_t worstAt = 0;
            for (size_t i = 0; i < total; ++i)
            {
                const double p = std::max (std::abs ((double) peaksL[i]), std::abs ((double) peaksR[i]));
                if (dbOfLin (p) - runMax[i] > worst)
                {
                    worst = dbOfLin (p) - runMax[i];
                    worstAt = i;
                }
            }
            if (worst > (tpOn ? 0.05 : 1.0e-6))
                std::cout << "      at sample " << worstAt << " (" << fmt ((double) worstAt / sr, 4) << " s), bound "
                          << runMax[worstAt] << " dB, ceiling then " << ceilAt[worstAt] << "\n";
            std::cout << "    sr " << sr << " true peak " << tpOn << " (" << switches << " mode switches): worst "
                      << (tpOn ? "true" : "sample") << " peak re the output level in effect " << fmt (worst, 4) << " dB\n";
            CHECK (worst <= (tpOn ? 0.05 : 1.0e-6));
        }
}

// This is a fixed-mode, sample-aligned bound: reproduce the public ceiling's
// exact 30 ms linear ramp, then delay each stamp by the reported latency. The
// four interpolation points may use any of their 16 contributing stamps, so
// their bound is the largest ceiling over those samples. Points touching a
// True Peak-off sample are intentionally excluded from the true-peak check.
TEST_CASE ("max. ceiling automation obeys exact delayed stamps at arbitrary block sizes")
{
    const TpReplica tp;
    int runs = 0;
    double maximumSampleExcess = -200.0, maximumTpExcess = -200.0;
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int mode = 0; mode < (int) MaximizerMode::Count; ++mode)
            for (int detection = 0; detection < 3; ++detection)
                for (int block : { 1, 17, 257 })
                {
                    Rig rig (sr, block);
                    rig.set ("max.mode", (float) mode);
                    rig.set ("max.ceiling", 0.0f);
                    rig.set ("max.character", 0.0f);
                    rig.set ("max.truePeak", detection == 0 ? 0.0f : 1.0f);
                    rig.restart();
                    const int latency = rig.mx.getLatencySamples();
                    const size_t total = (size_t) std::lround (0.16 * sr) + (size_t) latency + 32;
                    Stereo s;
                    s.resize (total);
                    pstest::Lcg noise (8237);
                    for (size_t i = 0; i < total; ++i)
                    {
                        const double nearNyquist = std::sin (2.0 * kPi * 0.47 * (double) i + 0.3);
                        const double square = i % 128 < 64 ? 3.0 : -3.0;
                        const double pulse = i % 127 == 0 ? 25.0 : 0.0;
                        s.l[i] = (float) (square + 2.0 * noise.next() + pulse);
                        s.r[i] = (float) (1.4 * nearNyquist + 0.4 * noise.next() - pulse);
                    }
                    std::vector<double> stampedCeiling (total, 1.0);
                    std::vector<bool> stampedTp (total, detection != 0);
                    const int rampLength = std::max (1, (int) std::lround (0.03 * sr));
                    const long long updateLength = std::max (1LL, std::llround (0.003 * sr));
                    double current = 1.0, target = 1.0, step = 0.0;
                    int remaining = 0;
                    long long previousUpdate = -1;
                    bool tpOn = detection != 0;
                    rig.run (s, [&] (long long start)
                    {
                        const long long update = start / updateLength;
                        if (update != previousUpdate)
                        {
                            previousUpdate = update;
                            static constexpr float ceilings[] { 0.0f, -18.0f, -3.0f, -12.0f, -1.0f };
                            const float ceilingDb = ceilings[(size_t) (update % 5)];
                            rig.set ("max.ceiling", ceilingDb);
                            const double nextTarget = std::pow (10.0, (double) ceilingDb / 20.0);
                            if (nextTarget != target)
                            {
                                target = nextTarget;
                                remaining = rampLength;
                                step = (target - current) / (double) rampLength;
                            }
                            rig.set ("max.gain", update % 2 != 0 ? 20.0f : 0.0f);
                            rig.set ("max.character", update % 2 != 0 ? 10.0f : 0.0f);
                            rig.set ("max.si.trans", update % 3 != 0 ? 100.0f : 0.0f);
                            rig.set ("max.si.sus", update % 2 != 0 ? 100.0f : 0.0f);
                            rig.set ("max.style3", (float) (update % 4));
                            rig.set ("max.style4", (float) (update % 3));
                            rig.set ("max.clip.on", update % 3 != 0 ? 1.0f : 0.0f);
                            rig.set ("max.clip.mode", (float) (update % 3));
                            rig.set ("max.clip.amount", 100.0f);
                            tpOn = detection == 1 || (detection == 2 && update % 4 != 0);
                            rig.set ("max.truePeak", tpOn ? 1.0f : 0.0f);
                        }
                        const size_t end = std::min (total, (size_t) start + (size_t) block);
                        for (size_t i = (size_t) start; i < end; ++i)
                        {
                            if (remaining > 0)
                                current = --remaining == 0 ? target : current + step;
                            stampedCeiling[i] = current;
                            stampedTp[i] = tpOn;
                        }
                    });
                    const auto peaksL = tp.perSample (s.l);
                    const auto peaksR = tp.perSample (s.r);
                    double worstSampleRatio = 0.0, worstTpRatio = 0.0;
                    for (size_t i = (size_t) latency + 16; i < total; ++i)
                    {
                        const size_t inputIndex = i - (size_t) latency;
                        const double sample = std::max (std::abs ((double) s.l[i]), std::abs ((double) s.r[i]));
                        CHECK (std::isfinite (sample));
                        worstSampleRatio = std::max (worstSampleRatio, sample / stampedCeiling[inputIndex]);
                        bool tpRequired = true;
                        double bound = 0.0;
                        for (size_t k = 0; k < 16; ++k)
                        {
                            tpRequired = tpRequired && stampedTp[inputIndex - k];
                            bound = std::max (bound, stampedCeiling[inputIndex - k]);
                        }
                        if (tpRequired)
                            worstTpRatio = std::max (worstTpRatio,
                                (double) std::max (peaksL[i], peaksR[i]) / bound);
                    }
                    if (worstSampleRatio > 1.0 || worstTpRatio > std::pow (10.0, 0.05 / 20.0))
                        std::cout << "    strict ceiling sr " << sr << " mode " << mode << " detection " << detection
                                  << " block " << block << ": sample " << fmt (dbOfLin (worstSampleRatio), 6)
                                  << " dB, true peak " << fmt (dbOfLin (worstTpRatio), 6) << " dB\n";
                    CHECK (worstSampleRatio <= 1.0);
                    CHECK (worstTpRatio <= std::pow (10.0, 0.05 / 20.0));
                    maximumSampleExcess = std::max (maximumSampleExcess, dbOfLin (worstSampleRatio));
                    maximumTpExcess = std::max (maximumTpExcess, dbOfLin (worstTpRatio));
                    ++runs;
                }
    std::cout << "    strict ceiling: " << runs << " runs; maximum sample / true-peak excess "
              << fmt (maximumSampleExcess, 6) << " / " << fmt (maximumTpExcess, 6) << " dB\n";
}

TEST_CASE ("max. low latency independence separates brief transients from sustained reduction")
{
    constexpr double sr = 48000.0;
    const size_t transientAt = 4800, sustainedAt = 12000;
    auto quietChannel = [&] (float transientIndependence, float sustainIndependence)
    {
        Rig rig (sr, 17);
        rig.set ("max.mode", (float) MaximizerMode::LowLatency);
        rig.set ("max.character", 0.0f);
        rig.set ("max.ceiling", 0.0f);
        rig.set ("max.truePeak", 0.0f);
        rig.set ("max.si.trans", transientIndependence);
        rig.set ("max.si.sus", sustainIndependence);
        rig.restart();
        Stereo s = silence (0.7, sr);
        std::fill (s.r.begin(), s.r.end(), 0.1f);
        for (size_t i = transientAt; i < transientAt + 8; ++i)
            s.l[i] = 4.0f;
        std::fill (s.l.begin() + (ptrdiff_t) sustainedAt, s.l.end(), 4.0f);
        rig.run (s);
        const size_t latency = (size_t) rig.mx.getLatencySamples();
        // The quiet channel exposes linking directly: its level cannot trigger
        // limiting. Measure a short isolated hit and the end of a held plateau.
        return std::array<double, 2> {
            pstest::rmsOf (s.r, transientAt + latency - 8, 32),
            pstest::rmsOf (s.r, 28800 + latency, 2400)
        };
    };
    const auto transientIndependent = quietChannel (100.0f, 0.0f);
    const auto sustainIndependent = quietChannel (0.0f, 100.0f);
    const double transientBenefit = dbOfLin (transientIndependent[0] / sustainIndependent[0]);
    const double sustainBenefit = dbOfLin (sustainIndependent[1] / transientIndependent[1]);
    std::cout << "    low latency independence: transient / sustained quiet-channel preservation "
              << fmt (transientBenefit) << " / " << fmt (sustainBenefit) << " dB\n";
    CHECK (transientBenefit > 3.0);
    CHECK (sustainBenefit > 6.0);
    CHECK (std::abs (dbOfLin (sustainIndependent[1] / 0.1)) < 0.1);
}
TEST_CASE ("max. mode transition reports the active pipeline latency until its silent reconfiguration")
{
    constexpr double sr = 48000.0;
    for (int from = 0; from < (int) MaximizerMode::Count; ++from)
    {
        const int to = (from + 4) % (int) MaximizerMode::Count;
        Rig rig (sr, 17);
        rig.set ("max.mode", (float) from);
        rig.restart();
        const int oldLatency = rig.mx.getLatencySamples();
        Stereo warm = silence (0.1, sr);
        std::fill (warm.l.begin(), warm.l.end(), 0.1f);
        std::fill (warm.r.begin(), warm.r.end(), -0.1f);
        rig.run (warm);

        Rig reference (sr, 17);
        reference.set ("max.mode", (float) to);
        reference.restart();
        const int newLatency = reference.mx.getLatencySamples();
        rig.set ("max.mode", (float) to);
        rig.mx.setParams (rig.values.data());
        CHECK_EQ (rig.mx.getLatencySamples(), oldLatency);

        bool changed = oldLatency == newLatency;
        int firstChange = -1;
        for (int start = 0; start < 4800; start += 17)
        {
            float l[17], r[17];
            std::fill (std::begin (l), std::end (l), 0.1f);
            std::fill (std::begin (r), std::end (r), -0.1f);
            float* channels[2] { l, r };
            rig.mx.process (channels, 17, {});
            if (! changed && rig.mx.getLatencySamples() != oldLatency)
            {
                changed = true;
                firstChange = start;
                CHECK_EQ (rig.mx.getLatencySamples(), newLatency);
                CHECK (std::abs (l[16]) < 1.0e-7f);
                CHECK (std::abs (r[16]) < 1.0e-7f);
            }
            if (start < 400)
                CHECK_EQ (rig.mx.getLatencySamples(), oldLatency);
        }
        CHECK (changed);
        CHECK_EQ (rig.mx.getLatencySamples(), newLatency);
        if (oldLatency != newLatency)
            CHECK (firstChange >= 480 && firstChange < 1920);
    }
}
TEST_CASE ("max. smooth changes: a gain jump and an output level jump ramp, no steps in the output")
{
    const double sr = 48000.0;
    // gain 0 -> 12 dB on a quiet tone (no limiting): the output grows over ~30 ms
    {
        Rig rig (sr);
        rig.restart();
        Stereo s = tone (200.0, 0.02, 1.0, sr);
        rig.run (s, [&] (long long start) { if (start >= 24000) rig.set ("max.gain", 12.0f); });
        double worstStep = 0.0;
        for (size_t i = 1; i < s.size(); ++i)
            worstStep = std::max (worstStep, (double) std::abs (s.l[i] - s.l[i - 1]));
        const double maxSlope = 2.0 * kPi * 200.0 / sr * 0.02 * std::pow (10.0, 12.0 / 20.0);
        CHECK (worstStep < 1.05 * maxSlope);
        CHECK (std::abs (pstest::peakOf (s.l, 40000, 4000) - 0.02 * std::pow (10.0, 0.6)) < 0.002);
    }
    // output level -1 -> -10 dB on a limited tone: the gain change is spread out (no step), then settles at -10 dB
    for (int m = 0; m < (int) MaximizerMode::Count; ++m)
    {
        Rig rig (sr);
        rig.set ("max.mode", (float) m);
        rig.set ("max.gain", 6.0f);
        rig.restart();
        Stereo s = tone (200.0, 0.9, 1.5, sr);
        rig.run (s, [&] (long long start) { if (start >= 36000) rig.set ("max.ceiling", -10.0f); });
        // the largest sample-to-sample step after the change is no larger than before it (limiting a 200 Hz tone
        // with 6 dB of reduction already shapes the wave; the level change must not add a step of its own)
        double before = 0.0, after = 0.0;
        for (size_t i = 24000; i < 36000; ++i)
            before = std::max (before, (double) std::abs (s.l[i] - s.l[i - 1]));
        for (size_t i = 36000; i < s.size(); ++i)
            after = std::max (after, (double) std::abs (s.l[i] - s.l[i - 1]));
        if (after >= 1.02 * before)
            std::cout << "    mode " << m << " output level change: step " << after << " vs " << before << " before\n";
        CHECK (after < 1.02 * before);
        CHECK (std::abs (dbOfLin (pstest::peakOf (s.l, 60000, 9600)) + 10.0) < 0.3);
    }
}

TEST_CASE ("max. character: slower recovery from 0 to 10 in every mode")
{
    const double sr = 48000.0;
    for (int m = 0; m < (int) MaximizerMode::Count; ++m)
    {
        double previous = -1.0, first = 0.0;
        std::cout << "    mode " << m << " recovery to -1 dB (ms) at character 0 / 5 / 10:";
        for (float character : { 0.0f, 5.0f, 10.0f })
        {
            Rig rig (sr);
            rig.set ("max.mode", (float) m);
            rig.set ("max.character", character);
            rig.set ("max.ceiling", 0.0f);
            rig.restart();
            // 0.3 s burst 12 dB over the ceiling, then a quiet tone 20 dB under it
            Stereo s = tone (1000.0, 4.0, 0.3, sr);
            const size_t burstEnd = s.size();
            s.append (tone (1000.0, 0.1, 3.0, sr));
            rig.run (s);
            const size_t lat = (size_t) rig.mx.getLatencySamples();
            const size_t win = 48;   // 1 ms
            size_t at = s.size();
            // band-split modes ring for a few ms after the burst: start looking 12 ms after it
            for (size_t i = burstEnd + lat + (size_t) (0.012 * sr); i + win < s.size(); i += win)
                if (pstest::peakOf (s.l, i, win) > 0.1 * std::pow (10.0, -1.0 / 20.0))
                {
                    at = i;
                    break;
                }
            const double ms = 1000.0 * (double) (at - burstEnd - lat) / sr;
            std::cout << " " << fmt (ms, 1);
            CHECK (ms >= previous);
            if (character == 0.0f)
                first = ms;
            previous = ms;
        }
        CHECK (previous > first);
        std::cout << "\n";
    }
}

TEST_CASE ("max. stereo independence: linked ducks the quiet channel, independent leaves it")
{
    const double sr = 48000.0;
    auto quietChannelGrDb = [&] (float si)
    {
        Rig rig (sr);
        rig.set ("max.mode", (float) MaximizerMode::Smooth);
        rig.set ("max.character", 10.0f);   // a steady gain on the tone (fast settings follow its waveform)
        rig.set ("max.ceiling", 0.0f);
        rig.set ("max.si.trans", si);
        rig.set ("max.si.sus", si);
        rig.restart();
        Stereo s = silence (1.5, sr);
        for (size_t i = 0; i < s.size(); ++i)
        {
            s.l[i] = (float) (2.0 * std::sin (2.0 * kPi * 300.0 * (double) i / sr));   // 6 dB over
            s.r[i] = (float) (0.1 * std::sin (2.0 * kPi * 500.0 * (double) i / sr));
        }
        rig.run (s);
        const size_t from = (size_t) sr;
        return dbOfLin (std::abs (pstest::toneOf (s.r, from, 24000, 500.0, sr))) - dbOfLin (0.1);
    };
    const double linked = quietChannelGrDb (0.0f), independent = quietChannelGrDb (100.0f),
                 half = quietChannelGrDb (50.0f);
    std::cout << "    quiet channel change: linked " << fmt (linked) << " dB, 50 % " << fmt (half) << " dB, 100 % "
              << fmt (independent) << " dB\n";
    CHECK (linked < -5.5);
    CHECK (std::abs (independent) < 0.05);
    CHECK (half < -2.0 && half > -4.0);
}

TEST_CASE ("max. multiband: bass-made peaks leave sparse highs alone; dense highs limit like the broadband core")
{
    // 60 Hz 6 dB over the ceiling plus a 5 kHz tone: quiet (sparse highs, the band stage acts and takes the peaks out of
    // the low band) or loud (dense highs, the stage stays out and the core limits broadband). The 5 kHz tone's change
    // in Multiband vs the broadband Aggressive core.
    const double sr = 48000.0;
    auto highChangeDb = [&] (MaximizerMode mode, double highAmp)
    {
        Rig rig (sr);
        rig.set ("max.mode", (float) mode);
        rig.set ("max.ceiling", 0.0f);
        rig.restart();
        Stereo s = silence (1.5, sr);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double t = (double) i / sr;
            s.l[i] = s.r[i] = (float) (2.0 * std::sin (2.0 * kPi * 60.0 * t) + highAmp * std::sin (2.0 * kPi * 5000.0 * t));
        }
        rig.run (s);
        return dbOfLin (std::abs (pstest::toneOf (s.l, (size_t) sr, 24000, 5000.0, sr))) - dbOfLin (highAmp);
    };
    const double sparseMb = highChangeDb (MaximizerMode::Multiband, 0.05), sparseAg = highChangeDb (MaximizerMode::Aggressive, 0.05);
    const double denseMb = highChangeDb (MaximizerMode::Multiband, 0.6), denseAg = highChangeDb (MaximizerMode::Aggressive, 0.6);
    std::cout << "    5 kHz change, sparse highs: Multiband " << fmt (sparseMb) << " dB, Aggressive " << fmt (sparseAg)
              << " dB; dense highs: Multiband " << fmt (denseMb) << " dB, Aggressive " << fmt (denseAg) << " dB\n";
    CHECK (sparseMb > sparseAg + 1.0);
    CHECK (std::abs (denseMb - denseAg) < 1.0);
}

TEST_CASE ("max. upward compress lifts quiet passages and leaves loud ones; meter")
{
    const double sr = 48000.0;
    Rig rig (sr);
    rig.set ("max.upward", 8.0f);
    rig.set ("max.ceiling", 0.0f);
    rig.restart();
    Stereo s = tone (500.0, 0.03, 1.5, sr);   // -30 dBFS
    s.append (tone (500.0, 0.8, 1.5, sr));
    rig.run (s);
    const size_t lat = (size_t) rig.mx.getLatencySamples();
    const double quietGain = dbOfLin (pstest::peakOf (s.l, lat + 48000, 9600)) - dbOfLin (0.03);
    const double loudGain = dbOfLin (pstest::peakOf (s.l, lat + 72000 + 48000, 9600)) - dbOfLin (0.8);
    std::cout << "    upward 8 dB: quiet tone " << fmt (quietGain) << " dB, loud tone " << fmt (loudGain) << " dB\n";
    CHECK (std::abs (quietGain - 8.0) < 0.5);
    CHECK (std::abs (loudGain) < 0.1);

    Rig r2 (sr);
    r2.set ("max.upward", 8.0f);
    r2.restart();
    Stereo q = tone (500.0, 0.03, 1.0, sr);
    r2.run (q);
    CHECK (std::abs (r2.mx.getMeters().upwardGainDb.load() - 8.0f) < 0.5f);
}

TEST_CASE ("max. soft clip: adds harmonics, no aliasing (4x)")
{
    const double sr = 44100.0;
    Rig rig (sr);
    rig.set ("max.ceiling", 0.0f);
    rig.set ("max.clip.on", 1.0f);
    rig.set ("max.clip.mode", 2.0f);   // Heavy
    rig.set ("max.clip.amount", 100.0f);
    rig.restart();
    Stereo s = tone (7000.0, 0.5, 2.0, sr);
    rig.run (s);
    const size_t from = (size_t) rig.mx.getLatencySamples() + 22050;
    REQUIRE (from + 44100 <= s.size());
    const double fund = std::abs (pstest::toneOf (s.l, from, 44100, 7000.0, sr));
    const double h3 = std::abs (pstest::toneOf (s.l, from, 44100, 21000.0, sr));
    const double alias5 = std::abs (pstest::toneOf (s.l, from, 44100, 44100.0 - 35000.0, sr));
    const double alias7 = std::abs (pstest::toneOf (s.l, from, 44100, 49000.0 - 44100.0, sr));
    std::cout << "    heavy clip of a 7 kHz tone at 44.1 kHz: 3rd harmonic " << fmt (dbOfLin (h3 / fund), 1)
              << " dBc, aliases of the 5th / 7th " << fmt (dbOfLin (alias5 / fund), 1) << " / "
              << fmt (dbOfLin (alias7 / fund), 1) << " dBc\n";
    CHECK (dbOfLin (h3 / fund) > -40.0);
    CHECK (dbOfLin (alias5 / fund) < -80.0);
    CHECK (dbOfLin (alias7 / fund) < -80.0);
}

TEST_CASE ("max. transient emphasis makes the hits stand out")
{
    const double sr = 48000.0;
    std::vector<size_t> onsets;
    Stereo in = drumMix (4.0, sr, &onsets);
    for (size_t i = 0; i < in.size(); ++i)
    {
        in.l[i] *= 0.25f;   // no limiting
        in.r[i] *= 0.25f;
    }
    auto retention = [&] (float amount)
    {
        Rig rig (sr);
        rig.set ("max.ceiling", 0.0f);
        rig.set ("max.trans.on", amount > 0.0f ? 1.0f : 0.0f);
        rig.set ("max.trans.amount", amount);
        rig.restart();
        Stereo s = in;
        rig.run (s);
        return transientRetentionDb (in, s, rig.mx.getLatencySamples(), onsets, sr);
    };
    const double off = retention (0.0f), on = retention (100.0f);
    std::cout << "    hit-to-body ratio change: emphasis off " << fmt (off) << " dB, 100 % " << fmt (on) << " dB\n";
    CHECK (std::abs (off) < 0.01);
    CHECK (on > 2.0);
}

TEST_CASE ("max. mode switch: fades out and in, no click, new latency")
{
    const double sr = 48000.0;
    for (int from = 0; from < (int) MaximizerMode::Count; ++from)
    {
        const int to = (from + 4) % (int) MaximizerMode::Count;
        Rig rig (sr);
        rig.set ("max.mode", (float) from);
        rig.restart();
        Stereo s = tone (200.0, 0.25, 1.0, sr);   // below the output level: no limiting
        rig.run (s, [&] (long long start) { if (start >= 24000) rig.set ("max.mode", (float) to); });
        double worstStep = 0.0;
        for (size_t i = 1; i < s.size(); ++i)
            worstStep = std::max (worstStep, (double) std::abs (s.l[i] - s.l[i - 1]));
        const double maxSlope = 2.0 * kPi * 200.0 / sr * 0.25;
        CHECK (worstStep < 1.05 * maxSlope);
        Rig fresh (sr);
        fresh.set ("max.mode", (float) to);
        fresh.restart();
        CHECK_EQ (rig.mx.getLatencySamples(), fresh.mx.getLatencySamples());
        // back at full level after the switch
        CHECK (std::abs (pstest::peakOf (s.l, 40000, 4800) - 0.25) < 0.001);
    }
}

TEST_CASE ("max. meters: gain reduction per channel, held peak, upward gain")
{
    const double sr = 48000.0;
    Rig rig (sr);
    rig.set ("max.mode", (float) MaximizerMode::Smooth);
    rig.set ("max.gain", 6.0f);
    rig.set ("max.ceiling", 0.0f);
    rig.restart();
    Stereo s = tone (1000.0, 1.0, 1.0, sr);
    for (size_t i = 0; i < s.size(); ++i)
        s.r[i] *= 0.25f;   // right 12 dB lower: linked, it gets the same reduction
    rig.run (s);
    const auto& m = rig.mx.getMeters();
    std::cout << "    6 dB push: GR L " << fmt (m.gainReductionDb[0].load()) << " dB, R "
              << fmt (m.gainReductionDb[1].load()) << " dB, held " << fmt (m.peakGainReductionDb.load()) << " dB\n";
    CHECK (std::abs (m.gainReductionDb[0].load() + 6.0f) < 0.2f);
    CHECK (std::abs (m.gainReductionDb[1].load() + 6.0f) < 0.2f);
    const float held = rig.mx.getMeters().peakGainReductionDb.exchange (0.0f);
    CHECK (held <= m.gainReductionDb[0].load() + 0.01f);
    CHECK (std::abs (m.upwardGainDb.load()) < 1.0e-6f);
    // quiet material: no reduction
    Stereo q = tone (1000.0, 0.1, 3.0, sr);   // the slow stage has released after a sustained 6 dB
    rig.run (q);
    CHECK (m.gainReductionDb[0].load() > -0.01f);
}

TEST_CASE ("max. learn restart: stale capture completion and progress cannot replace a new request")
{
    using Mailbox = ps::maxi::LearnCaptureMailbox;
    Mailbox mailbox;
    mailbox.start();
    const auto oldCallback = mailbox.load();
    // Interleave the UI's restart with an audio callback that has already copied its final input samples.
    mailbox.start();
    CHECK (! mailbox.publish (oldCallback, 240000u, Mailbox::Captured));
    const auto restarted = mailbox.load();
    CHECK (Mailbox::state (restarted) == Mailbox::Listening);
    CHECK (Mailbox::count (restarted) == 0u);
    CHECK (Mailbox::generation (restarted) != Mailbox::generation (oldCallback));
    CHECK (mailbox.publish (restarted, 120000u, Mailbox::Listening));
    const auto halfCaptured = mailbox.load();
    CHECK (Mailbox::count (halfCaptured) == 120000u);
    mailbox.cancel();
    CHECK (! mailbox.publish (halfCaptured, 240000u, Mailbox::Captured));
    CHECK (Mailbox::state (mailbox.load()) == Mailbox::Idle);
    CHECK (Mailbox::count (mailbox.load()) == 0u);
    mailbox.start();
    CHECK (mailbox.publish (mailbox.load(), 240000u, Mailbox::Captured));
    const auto complete = mailbox.load();
    CHECK (Mailbox::state (complete) == Mailbox::Captured);
    CHECK (mailbox.publish (complete, 240000u, Mailbox::Idle));
    CHECK (! mailbox.publish (complete, 240000u, Mailbox::Idle));
}

TEST_CASE ("max. learn selected pipeline: learned gain reaches the target with upward compression")
{
    constexpr double sr = 48000.0;
    Rig rig (sr);
    rig.set ("max.upward", 12.0f);
    rig.set ("max.learnTarget", -20.0f);
    rig.restart();
    rig.mx.startLearn();
    const Stereo source = tone (1000.0, 0.01, 5.0, sr);
    Stereo capture = source;
    rig.run (capture);
    float learned = 0.0f;
    REQUIRE (rig.mx.takeLearnedGain (learned));
    rig.set ("max.gain", learned);
    rig.restart();
    Stereo output = source;
    output.append (silence ((double) rig.mx.getLatencySamples() / sr, sr));
    rig.run (output);
    const auto latency = (size_t) rig.mx.getLatencySamples();
    const float achieved = measureIntegratedLufs (output.l.data() + latency, output.r.data() + latency,
                                                  (long long) source.size(), sr);
    std::cout << "    Learn with +12 dB upward: gain " << learned << " dB, target -20, output " << achieved << " LUFS\n";
    CHECK (std::abs (achieved + 20.0f) < 0.2f);
}

TEST_CASE ("max. learn selected modes: simulation includes clipping, emphasis and independent gain")
{
    constexpr double sr = 48000.0;
    Stereo source = drumMix (5.0, sr);
    // Keep gaps above the capture's silence gate so the known reference and the learned capture are identical.
    for (size_t i = 0; i < source.size(); ++i)
    {
        source.l[i] += (float) (0.002 * std::sin (2.0 * kPi * 333.0 * (double) i / sr));
        source.r[i] += (float) (0.002 * std::sin (2.0 * kPi * 417.0 * (double) i / sr));
    }
    for (int mode = 0; mode < (int) MaximizerMode::Count; ++mode)
    {
        Rig rig (sr);
        rig.set ("max.mode", (float) mode);
        rig.set ("max.character", 7.0f);
        rig.set ("max.clip.on", 1.0f);
        rig.set ("max.clip.mode", 2.0f);
        rig.set ("max.clip.amount", 60.0f);
        rig.set ("max.upward", 4.0f);
        rig.set ("max.trans.on", 1.0f);
        rig.set ("max.trans.amount", 60.0f);
        rig.set ("max.si.trans", 35.0f);
        rig.set ("max.si.sus", 70.0f);
        rig.set ("max.gain", 6.0f);
        rig.restart();
        const auto latency = (size_t) rig.mx.getLatencySamples();
        Stereo reference = source;
        reference.append (silence ((double) latency / sr, sr));
        rig.run (reference);
        const float target = measureIntegratedLufs (reference.l.data() + latency, reference.r.data() + latency,
                                                    (long long) source.size(), sr);
        REQUIRE (target >= -24.0f && target <= -5.0f);
        rig.set ("max.gain", 0.0f);
        rig.set ("max.learnTarget", target);
        rig.restart();
        rig.mx.startLearn();
        Stereo input = source;
        rig.run (input);
        float learned = 0.0f;
        REQUIRE (rig.mx.takeLearnedGain (learned));
        std::cout << "    Learn mode " << mode << ": target " << target << ", known gain 6, learned " << learned << " dB\n";
        CHECK (std::abs (learned - 6.0f) <= 0.15f);
    }
}
