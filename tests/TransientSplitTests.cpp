#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/TransientSplit.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Transient / sustain splitter tests (dsp/TransientSplit.cpp). Prefix "ts.".
namespace
{
using namespace ps;
using pstest::note;

struct Parts
{
    std::vector<float> tl, tr, sl, sr;
};

// Runs the splitter over (l, r) in blocks; `before` may change the parameters per block.
template <typename Fn>
Parts split (TransientSplitter& s, const std::vector<float>& l, const std::vector<float>& r, int block, Fn before)
{
    Parts p;
    p.tl.resize (l.size());
    p.tr.resize (l.size());
    p.sl.resize (l.size());
    p.sr.resize (l.size());
    int index = 0;
    for (size_t pos = 0; pos < l.size(); pos += (size_t) block, ++index)
    {
        const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
        before (index);
        const float* in[2] { l.data() + pos, r.data() + pos };
        float* t[2] { p.tl.data() + pos, p.tr.data() + pos };
        float* su[2] { p.sl.data() + pos, p.sr.data() + pos };
        s.process (in, t, su, n);
    }
    return p;
}

Parts split (TransientSplitter& s, const std::vector<float>& l, const std::vector<float>& r, int block = 512)
{
    return split (s, l, r, block, [] (int) {});
}

// Exponentially decaying noise bursts (tau 30 ms, peak about -3 dBFS) every `period` seconds over a pink bed.
std::vector<float> hits (double sr, double seconds, double bedDbfs, double period = 0.5, std::uint32_t seed = 3u)
{
    auto x = pstest::pinkNoise (pstest::dbfsToAmplitude (bedDbfs) * 3.0, seconds, sr, seed);
    pstest::Lcg lcg (seed + 99u);
    const auto periodSamples = (size_t) std::llround (period * sr);
    const double amp = pstest::dbfsToAmplitude (-3.0);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) (i % periodSamples) / sr;
        x[i] += (float) (amp * std::exp (-t / 0.03) * lcg.next());
    }
    return x;
}

double energy (const std::vector<float>& x, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i)
        e += (double) x[i] * (double) x[i];
    return e;
}

double ratioDb (double a, double b) { return 10.0 * std::log10 (std::max (a, 1.0e-30) / std::max (b, 1.0e-30)); }

// energy (T) / energy (x) in the onset windows [onset + a, onset + b) of a hit train (period 0.5 s), skipping the first
// `skipHits` hits. x is the input delayed by the latency (the splitter's x_L).
double onsetShareDb (const Parts& p, const std::vector<float>& x, double sr, int latency, double a, double b,
                     int skipHits = 2)
{
    const auto period = (size_t) std::llround (0.5 * sr);
    double eT = 0.0, eX = 0.0;
    for (size_t onset = period * (size_t) skipHits; onset + period <= x.size(); onset += period)
    {
        const auto from = onset + (size_t) std::llround (a * sr), to = onset + (size_t) std::llround (b * sr);
        eT += energy (p.tl, from + (size_t) latency, to + (size_t) latency) + energy (p.tr, from + (size_t) latency, to + (size_t) latency);
        eX += 2.0 * energy (x, from, to);
    }
    return ratioDb (eT, eX);
}
} // namespace

TEST_CASE ("ts. latency is a short lookahead (under 1 ms) and an impulse lands there")
{
    const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (int k = 0; k < 6; ++k)
    {
        TransientSplitter s;
        s.prepare (rates[k], 512);
        s.setParams (50.0f, 20.0f);
        const int expected = TransientSplitter::latencyFor (rates[k]);
        CHECK_EQ (s.getLatencySamples(), expected);
        CHECK (expected > 0 && expected < (int) (0.001 * rates[k]));
        std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
        l[100] = 0.5f;
        r[100] = -0.25f;
        const auto p = split (s, l, r, 333);
        int peak = -1;
        double best = 0.0;
        for (size_t i = 0; i < l.size(); ++i)
        {
            const double v = std::abs ((double) p.tl[i] + p.sl[i]);
            if (v > best) { best = v; peak = (int) i; }
        }
        CHECK_EQ (peak, 100 + expected);
        CHECK (std::abs (p.tl[(size_t) peak] + p.sl[(size_t) peak] - 0.5f) < 1.0e-7f);
        CHECK (std::abs (p.tr[(size_t) peak] + p.sr[(size_t) peak] + 0.25f) < 1.0e-7f);
    }
    TransientSplitter noLookahead (false);
    noLookahead.prepare (48000.0, 512);
    CHECK_EQ (noLookahead.getLatencySamples(), 0);
}

TEST_CASE ("ts. parts sum to the delayed input (any rate, block size and automation)")
{
    double worst = 0.0;
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const auto l = hits (sr, 4.0, -10.0, 0.5, 5u);
        const auto r = hits (sr, 4.0, -10.0, 0.37, 6u);
        for (int block : { 1, 37, 512 })
        {
            TransientSplitter s;
            s.prepare (sr, 512);
            pstest::Lcg lcg (77u);
            const auto p = split (s, l, r, block, [&] (int)
            {
                s.setParams (50.0f + 50.0f * lcg.next(), 52.5f + 47.5f * lcg.next());
            });
            const int L = s.getLatencySamples();
            for (size_t i = (size_t) L; i < l.size(); ++i)
            {
                worst = std::max (worst, std::abs ((double) p.tl[i] + (double) p.sl[i] - (double) l[i - (size_t) L]));
                worst = std::max (worst, std::abs ((double) p.tr[i] + (double) p.sr[i] - (double) r[i - (size_t) L]));
            }
        }
    }
    note ("max |T + S - x(n - L)| (dBFS)", pstest::dbOf (worst));
    CHECK (pstest::dbOf (worst) < -130.0);
}

TEST_CASE ("ts. a steady tone stays in the sustain part, steady noise keeps a small transient share")
{
    const double sr = 48000.0;
    {
        const auto x = pstest::sine (1000.0, -20.0, 5.0, sr);
        TransientSplitter s;
        s.prepare (sr, 512);
        s.setParams (50.0f, 20.0f);
        const auto p = split (s, x, x);
        const int L = s.getLatencySamples();
        const double share = ratioDb (energy (p.tl, 48000, x.size()), energy (x, 48000 - (size_t) L, x.size() - (size_t) L));
        note ("1 kHz sine: transient share after 1 s (dB)", share);
        CHECK (share < -40.0);
        bool exact = true;
        for (size_t i = 96000; i < x.size(); ++i)
            exact = exact && p.tl[i] == 0.0f && p.tr[i] == 0.0f && p.sl[i] == x[i - (size_t) L] && p.sr[i] == x[i - (size_t) L];
        CHECK (exact);   // after 2 s the masks have snapped to 0: T == 0 and S == x_L bitwise
    }
    {
        const auto l = pstest::pinkNoise (pstest::dbfsToAmplitude (-20.0) * 3.0, 5.0, sr, 41u);
        const auto r = pstest::pinkNoise (pstest::dbfsToAmplitude (-20.0) * 3.0, 5.0, sr, 42u);
        TransientSplitter s;
        s.prepare (sr, 512);
        s.setParams (50.0f, 20.0f);
        const auto p = split (s, l, r);
        const double share = ratioDb (energy (p.tl, 48000, l.size()) + energy (p.tr, 48000, l.size()),
                                      energy (l, 48000, l.size()) + energy (r, 48000, l.size()));
        note ("pink noise: transient share after 1 s (dB)", share);
        CHECK (share > -24.0);   // measured reference: a mask of ~0.15 on steady pink noise (-16 dB of energy)
        CHECK (share < -10.0);
    }
}

TEST_CASE ("ts. hits go to the transient part, their tails to the sustain part")
{
    const double sr = 48000.0;
    const auto x = hits (sr, 6.0, -30.0);
    for (bool lookahead : { true, false })
    {
        TransientSplitter s (lookahead);
        s.prepare (sr, 512);
        s.setParams (50.0f, 20.0f);
        const auto p = split (s, x, x);
        const int L = s.getLatencySamples();
        const double onset = onsetShareDb (p, x, sr, L, 0.0, 0.010);
        const double tail = onsetShareDb (p, x, sr, L, 0.150, 0.450);
        note (lookahead ? "lookahead: onset (0..10 ms) transient share (dB)" : "no lookahead: onset share (dB)", onset);
        note (lookahead ? "lookahead: tail (150..450 ms) transient share (dB)" : "no lookahead: tail share (dB)", tail);
        if (lookahead)
        {
            CHECK (onset >= -1.5);
            CHECK (tail <= -10.0);   // measured reference: a hit's mask is ~0.2 at 150 ms and ~0 by 300 ms
        }
    }
}

TEST_CASE ("ts. sensitivity and length are monotonic")
{
    const double sr = 48000.0;
    const auto x = hits (sr, 5.0, -30.0);

    double prev = -1000.0;
    for (float sens : { 0.0f, 50.0f, 100.0f })
    {
        TransientSplitter s;
        s.prepare (sr, 512);
        s.setParams (sens, 20.0f);
        const auto p = split (s, x, x);
        const double share = ratioDb (energy (p.tl, 48000, x.size()), energy (x, 48000, x.size()));
        note ("sensitivity " + std::to_string ((int) sens) + ": transient share (dB)", share);
        CHECK (share > prev);
        prev = share;
    }

    prev = -1000.0;
    for (float len : { 5.0f, 20.0f, 100.0f })
    {
        TransientSplitter s;
        s.prepare (sr, 512);
        s.setParams (50.0f, len);
        const auto p = split (s, x, x);
        const double window = onsetShareDb (p, x, sr, s.getLatencySamples(), 0.100, 0.200);
        note ("length " + std::to_string ((int) len) + " ms: transient share 100..200 ms after onsets (dB)", window);
        CHECK (window >= prev + 3.0);
        prev = window;
    }
}

TEST_CASE ("ts. the channels are split independently; equal channels split equally")
{
    const double sr = 48000.0;
    const auto l = hits (sr, 3.0, -24.0);
    const auto r = pstest::sine (1000.0, -12.0, 3.0, sr);
    TransientSplitter s;
    s.prepare (sr, 512);
    s.setParams (50.0f, 20.0f);
    const auto p = split (s, l, r);
    const double shareR = ratioDb (energy (p.tr, 48000, r.size()), energy (r, 48000, r.size()));
    note ("hits on the left, steady sine on the right: right transient share (dB)", shareR);
    CHECK (shareR < -40.0);

    TransientSplitter s2;
    s2.prepare (sr, 512);
    s2.setParams (50.0f, 20.0f);
    const auto q = split (s2, l, l);
    bool same = true;
    for (size_t i = 0; i < l.size(); ++i)
        same = same && q.tl[i] == q.tr[i] && q.sl[i] == q.sr[i];
    CHECK (same);
}

TEST_CASE ("ts. one broadband mask: a loud low hit takes a steady treble tone with it")
{
    const double sr = 48000.0;
    const size_t n = (size_t) (4.0 * sr);
    std::vector<float> x (n);
    const auto period = (size_t) (0.5 * sr);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) (i % period) / sr;
        const double kick = t < 0.2 ? 0.6 * std::exp (-t / 0.04) * std::sin (2.0 * pstest::kPi * 60.0 * t) : 0.0;
        x[i] = (float) (0.1 * std::sin (2.0 * pstest::kPi * 4000.0 * (double) i / sr) + kick);
    }
    TransientSplitter s;
    s.prepare (sr, 512);
    s.setParams (50.0f, 20.0f);
    const auto p = split (s, x, x);
    // 4 kHz content of T and S over the first 40 ms of the hits and between them (after the first second)
    double t4 = 0.0, s4 = 0.0, t4late = 0.0, s4late = 0.0;
    for (size_t onset = 2 * period; onset + period <= n; onset += period)
    {
        const size_t from = onset + (size_t) s.getLatencySamples();
        t4 += std::norm (pstest::toneOf (p.tl, from + 240, (size_t) (0.04 * sr), 4000.0, sr));
        s4 += std::norm (pstest::toneOf (p.sl, from + 240, (size_t) (0.04 * sr), 4000.0, sr));
        t4late += std::norm (pstest::toneOf (p.tl, from + (size_t) (0.35 * sr), (size_t) (0.1 * sr), 4000.0, sr));
        s4late += std::norm (pstest::toneOf (p.sl, from + (size_t) (0.35 * sr), (size_t) (0.1 * sr), 4000.0, sr));
    }
    note ("4 kHz in T vs S during the hits (dB)", ratioDb (t4, s4));
    note ("4 kHz in T vs S 350..450 ms after the hits (dB)", ratioDb (t4late, s4late));
    CHECK (ratioDb (t4, s4) > 0.0);   // measured reference: the tone follows the hit almost fully (ours about half way)
    CHECK (ratioDb (t4late, s4late) < -20.0);
}

TEST_CASE ("ts. silence after a hit: exact zeros, no denormal slowdown")
{
    const double sr = 48000.0;
    auto x = hits (sr, 0.5, -30.0);
    x.resize ((size_t) (10.5 * sr), 0.0f);
    TransientSplitter s;
    s.prepare (sr, 512);
    s.setParams (50.0f, 20.0f);
    pstest::Stopwatch watch;
    const auto p = split (s, x, x);
    note ("10.5 s through the splitter (s)", watch.seconds());
    bool zero = true;
    for (size_t i = (size_t) (5.5 * sr); i < x.size(); ++i)
        zero = zero && p.tl[i] == 0.0f && p.tr[i] == 0.0f && p.sl[i] == 0.0f && p.sr[i] == 0.0f;
    CHECK (zero);
    CHECK_EQ (s.getMaskActivity(), 0.0f);
}

TEST_CASE ("ts. no allocation in process / setParams / reset, and CPU")
{
    const double sr = 48000.0;
    const auto x = hits (sr, 60.0, -20.0);
    TransientSplitter s;
    s.prepare (sr, 512);
    std::vector<float> t[2], su[2];
    for (int c = 0; c < 2; ++c)
    {
        t[c].resize (512);
        su[c].resize (512);
    }
    long long allocations = 0;
    pstest::Stopwatch watch;
    {
        pstest::AllocationScope scope;
        int k = 0;
        for (size_t pos = 0; pos + 512 <= x.size(); pos += 512, ++k)
        {
            s.setParams ((float) (k % 101), 5.0f + (float) (k % 96));
            if (k % 997 == 0)
                s.reset();
            const float* in[2] { x.data() + pos, x.data() + pos };
            float* tt[2] { t[0].data(), t[1].data() };
            float* ss[2] { su[0].data(), su[1].data() };
            s.process (in, tt, ss, 512);
        }
        allocations = scope.count();
    }
    const double secs = watch.seconds();
    note ("60 s stereo 48 kHz through the splitter (s)", secs);
    note ("realtime factor", 60.0 / std::max (secs, 1.0e-6));
    CHECK_EQ (allocations, 0LL);
    CHECK (secs < 0.6);   // >= 100x realtime even on a slow machine (spec target 500x, printed above)
}
