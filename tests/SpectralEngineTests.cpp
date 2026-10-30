#include "AllocationCounter.h"
#include "DspTestUtil.h"
#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/SpectralEngine.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Residual STFT engine (dsp/SpectralEngine.cpp). Prefix "spec.".
namespace
{
using namespace ps;
using pstest::note;

struct ConstantGains final : SpectralEngine::Client
{
    float g[2] { 1.0f, 1.0f };
    float minHz = 0.0f;           // gains apply from this frequency up (below: unity)
    int calls = 0;
    bool sawSilent = false;
    double sinePower = 0.0;       // largest per-bin power seen (calibration)

    bool computeGains (SpectralEngine::Frame& f) noexcept override
    {
        ++calls;
        sawSilent = sawSilent || f.silent;
        double lobe = 0.0;
        for (int k = 0; k < f.numBins; ++k)
            lobe = std::max (lobe, (double) f.power[0][k] + (k > 0 ? f.power[0][k - 1] : 0.0f) + (k + 1 < f.numBins ? f.power[0][k + 1] : 0.0f));
        sinePower = std::max (sinePower, lobe);
        if (g[0] == 1.0f && g[1] == 1.0f)
            return true;
        for (int k = 0; k < f.numBins; ++k)
        {
            const bool apply = (double) k * f.binHz >= (double) minHz;
            f.gain[0][k] = apply ? g[0] : 1.0f;
            if (! f.linked)
                f.gain[1][k] = apply ? g[1] : 1.0f;
        }
        return false;
    }
};

void run (SpectralEngine& e, std::vector<float>& l, std::vector<float>& r, int block, bool linked)
{
    for (size_t pos = 0; pos < l.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, l.size() - pos);
        float* ch[2] { l.data() + pos, r.data() + pos };
        e.process (ch, n, linked);
    }
}
} // namespace

TEST_CASE ("spec. sizes and latency per sample rate")
{
    const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
    const int n1024[] { 1024, 1024, 2048, 4096 };
    for (int i = 0; i < 4; ++i)
    {
        SpectralEngine e;
        e.prepare (rates[i], 1024, 512);
        CHECK_EQ (e.getFftSize(), n1024[i]);
        CHECK_EQ (e.getLatencySamples(), n1024[i]);
        CHECK_EQ (e.getHopSize(), n1024[i] / 4);
        CHECK (std::abs (e.getBinHz() - rates[i] / n1024[i]) < 1.0e-9);
    }
    SpectralEngine t;
    t.prepare (48000.0, 2048, 512);
    CHECK_EQ (t.getLatencySamples(), 2048);
}

TEST_CASE ("spec. unity gains: bit-exact delay by N, any block size")
{
    const auto x = pstest::pinkNoise (0.5, 2.0, 48000.0, 5u);
    for (int block : { 1, 77, 512 })
    {
        SpectralEngine e;
        ConstantGains c;
        e.prepare (48000.0, 1024, 512);
        e.setClient (&c);
        auto l = x, r = x;
        run (e, l, r, block, block == 77);
        bool exact = true;
        for (size_t i = 1024; i < x.size(); ++i)
            exact = exact && l[i] == x[i - 1024] && r[i] == x[i - 1024];
        CHECK (exact);
        CHECK (c.calls > 0);
    }
}

TEST_CASE ("spec. gains are applied: constant, per channel, linked, and the calibration of the powers")
{
    const double sr = 48000.0;
    const auto xl = pstest::pinkNoise (0.5, 2.0, sr, 6u), xr = pstest::pinkNoise (0.5, 2.0, sr, 7u);
    const size_t from = 4096;

    // per channel: L x 0.5, R x 2 (unlinked)
    {
        SpectralEngine e;
        ConstantGains c;
        c.g[0] = 0.5f;
        c.g[1] = 2.0f;
        e.prepare (sr, 1024, 512);
        e.setClient (&c);
        auto l = xl, r = xr;
        run (e, l, r, 333, false);
        double el = 0.0, er = 0.0;
        for (size_t i = from; i < xl.size(); ++i)
        {
            el = std::max (el, std::abs ((double) l[i] - 0.5 * xl[i - 1024]));
            er = std::max (er, std::abs ((double) r[i] - 2.0 * xr[i - 1024]));
        }
        note ("L x 0.5 error (dBFS)", pstest::dbOf (el));
        note ("R x 2 error (dBFS)", pstest::dbOf (er));
        CHECK (pstest::dbOf (el) < -100.0);
        CHECK (pstest::dbOf (er) < -100.0);
    }

    // linked: one curve (gain[0]) for both channels
    {
        SpectralEngine e;
        ConstantGains c;
        c.g[0] = 0.25f;
        c.g[1] = 4.0f;   // ignored when linked
        e.prepare (sr, 1024, 512);
        e.setClient (&c);
        auto l = xl, r = xr;
        run (e, l, r, 512, true);
        double er = 0.0;
        for (size_t i = from; i < xl.size(); ++i)
            er = std::max (er, std::abs ((double) r[i] - 0.25 * xr[i - 1024]));
        CHECK (pstest::dbOf (er) < -100.0);
    }

    // frequency selective: -20 dB from 4 kHz up: a 1 kHz tone passes, a 10 kHz tone drops by 20 dB
    {
        SpectralEngine e;
        ConstantGains c;
        c.g[0] = c.g[1] = 0.1f;
        c.minHz = 4000.0f;
        e.prepare (sr, 1024, 512);
        e.setClient (&c);
        auto l = pstest::added (pstest::sine (1000.0, -12.0, 1.0, sr), pstest::sine (10000.0, -12.0, 1.0, sr));
        auto r = l;
        const auto in = l;
        run (e, l, r, 512, true);
        const size_t start = 12000, len = 24000;
        const double g1 = pstest::dbOf (std::abs (pstest::toneOf (l, start, len, 1000.0, sr)))
                          - pstest::dbOf (std::abs (pstest::toneOf (in, start - 1024, len, 1000.0, sr)));
        const double g10 = pstest::dbOf (std::abs (pstest::toneOf (l, start, len, 10000.0, sr)))
                           - pstest::dbOf (std::abs (pstest::toneOf (in, start - 1024, len, 10000.0, sr)));
        note ("1 kHz gain (dB)", g1);
        note ("10 kHz gain (dB)", g10);
        CHECK (std::abs (g1) < 0.05);
        CHECK (std::abs (g10 + 20.0) < 0.1);
    }

    // calibration: a full-scale sine reads 1.0 summed over its main lobe
    {
        SpectralEngine e;
        ConstantGains c;
        e.prepare (sr, 1024, 512);
        e.setClient (&c);
        auto l = pstest::sine (1000.0, 0.0, 0.5, sr), r = l;
        run (e, l, r, 512, true);
        note ("full-scale sine: power summed over 3 bins", c.sinePower);
        CHECK (c.sinePower > 0.85 && c.sinePower < 1.05);
    }
}

TEST_CASE ("spec. silence is flagged and processing is allocation free")
{
    SpectralEngine e;
    ConstantGains c;
    c.g[0] = c.g[1] = 0.5f;
    e.prepare (48000.0, 2048, 512);
    e.setClient (&c);
    std::vector<float> l (48000, 0.0f), r (48000, 0.0f);
    long long count = 0;
    {
        pstest::AllocationScope scope;
        run (e, l, r, 512, false);
        e.reset();
        run (e, l, r, 17, true);
        count = scope.count();
    }
    CHECK_EQ (count, 0LL);
    CHECK (c.sawSilent);
    bool zero = true;
    for (size_t i = 0; i < l.size(); ++i)
        zero = zero && l[i] == 0.0f && r[i] == 0.0f;
    CHECK (zero);
}
