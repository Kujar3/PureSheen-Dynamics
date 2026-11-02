#include "dsp/MaximizerBands.h"

#include <algorithm>
#include <cstring>

// Spectral and Multiband stages of the Maximizer (dsp-maximizer). See MaximizerBands.h.
namespace ps::maxi
{
namespace
{
    // Multiband stage (fitted on the reference's test-track renders): the gate and the sharing law
    constexpr double kGateShare = 0.15;    // the stage acts while the bands > 1 kHz make < 15 % of the recent peak
    constexpr double kGatePeakMs = 20.0;   // release of the gate's peak envelopes
    constexpr double kSharePower = 1.5;    // excess shared in proportion to contribution^1.5
    // ... and the steady-highs low path (fitted on the reference's bass-burst renders, black box)
    constexpr double kSteadyShare = 0.5;   // it acts while the bands > 1 kHz make < 50 % of the peak,
    constexpr double kSteadyCrest = 1.6;   // their peak envelope has stayed < 1.6 x their RMS (a tone: 1.41)
    constexpr double kSteadyMinMs = 100.0; // for at least 100 ms
    constexpr double kSteadyPowerMs = 25.0;
    constexpr double kLowAttackMs = 3.0;   // low path: lookahead attack, no hold, release
    constexpr double kLowReleaseMs = 20.0;
    constexpr double kLowCap = 0.8;        // a band gives up at most 80 % of its contribution (-14 dB)
    // Sparse highs over a steady bass (the bands < 1 kHz steady by the same test) use the same path with a release
    // that follows the character (Settings::steadyBassReleaseMs), fitted on the reference's sustained 60 Hz tone.
}
namespace
{
constexpr double kPi = 3.14159265358979323846;

double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double halfX = 0.5 * x;
    for (int k = 1; k < 64; ++k)
    {
        const double f = halfX / k;
        term *= f * f;
        sum += term;
        if (term < 1.0e-14 * sum)
            break;
    }
    return sum;
}

int rateFamily (double sr) noexcept { return sr <= 50000.0 ? 1 : (sr <= 100000.0 ? 2 : (sr <= 200000.0 ? 4 : 8)); }

constexpr int kMinBinsPerBand = 3;   // bands narrower than the window's main lobe would modulate a steady tone
constexpr double kSpread = 0.7;      // share of a band's reduction its neighbours get at least (squared: two away)

// Critical band edges (Hz), grouped from the Bark scale.
constexpr double kBandEdges[] { 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700,
                                3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500 };
} // namespace

//==============================================================================
SpectralStage::SpectralStage() = default;
SpectralStage::~SpectralStage() = default;

void SpectralStage::prepare (double sampleRate)
{
    sr = sampleRate;
    frameSize = 512 * rateFamily (sr);
    hop = frameSize / 4;
    controlStep = (double) hop / std::max (1.0, msToSamples (20.0, sr));
    numBins = frameSize / 2 + 1;
    int order = 0;
    while ((1 << order) < frameSize)
        ++order;
    fft = std::make_unique<Fft> (order);

    window.resize ((size_t) frameSize);
    for (int i = 0; i < frameSize; ++i)
        window[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * kPi * i / frameSize));   // sqrt periodic Hann

    const int drySize = nextPow2 (frameSize + 2);
    dryMask = drySize - 1;
    for (int c = 0; c < 2; ++c)
    {
        inBuf[(size_t) c].assign ((size_t) frameSize, 0.0f);
        outAcc[(size_t) c].assign ((size_t) frameSize, 0.0f);
        dry[(size_t) c].assign ((size_t) drySize, 0.0f);
        spec[c].assign ((size_t) numBins, {});
    }
    thrBuf.assign ((size_t) frameSize, 1.0);
    z.assign ((size_t) frameSize, {});

    // bins -> bands (by bin centre frequency); empty bands vanish
    const double binHz = sr / frameSize;
    bandStart.clear();
    bandStart.push_back (0);
    int edge = 0;
    const int numEdges = (int) (sizeof (kBandEdges) / sizeof (kBandEdges[0]));
    for (int k = 0; k < numBins; ++k)
    {
        const double f = k * binHz;
        bool advanced = false;
        while (edge < numEdges && f >= kBandEdges[edge])
        {
            ++edge;
            advanced = true;
        }
        if (advanced && k - bandStart.back() >= kMinBinsPerBand)
            bandStart.push_back (k);
    }
    if (numBins - bandStart.back() < kMinBinsPerBand && bandStart.size() > 1)
        bandStart.pop_back();
    bandStart.push_back (numBins);
    numBands = (int) bandStart.size() - 1;

    std::vector<double> centre ((size_t) numBands);
    for (int b = 0; b < numBands; ++b)
        centre[(size_t) b] = 0.5 * (bandStart[(size_t) b] + bandStart[(size_t) b + 1] - 1);
    binLo.assign ((size_t) numBins, 0);
    binFrac.assign ((size_t) numBins, 0.0);
    for (int k = 0; k < numBins; ++k)
    {
        int b = 0;
        while (b + 1 < numBands && centre[(size_t) b + 1] <= k)
            ++b;
        binLo[(size_t) k] = b;
        if (b + 1 < numBands && k > centre[(size_t) b])
            binFrac[(size_t) k] = (k - centre[(size_t) b]) / (centre[(size_t) b + 1] - centre[(size_t) b]);
        else
            binFrac[(size_t) k] = 0.0;
    }

    for (int c = 0; c < 2; ++c)
    {
        state[(size_t) c].assign ((size_t) numBands, 0.0);
        target[(size_t) c].assign ((size_t) numBands, 0.0);
        amps[c].assign ((size_t) numBands, 0.0);
    }
    relCoef.assign ((size_t) numBands, 0.0);
    sorted.assign ((size_t) numBands, 0.0);
    binRed.assign ((size_t) numBins, 0.0);
    binSmooth.assign ((size_t) numBins, 0.0);
    // hold before release: 8 ms (a steady tone's band level repeats a small frame-to-frame pattern), 12 ms below
    // 300 Hz (a frame is shorter than a bass cycle, so a bass note's band level varies with its phase)
    holdFrames.assign ((size_t) numBands, 1);
    for (int b = 0; b < numBands; ++b)
        holdFrames[(size_t) b] = (int) std::ceil (msToSamples (bandStart[(size_t) b] * sr / frameSize < 300.0 ? 12.0 : 8.0,
                                                               sr) / hop);
    for (auto& h : holdLeft)
        h.assign ((size_t) numBands, 0);
    setSettings (settings);
    reset();
}

void SpectralStage::reset() noexcept
{
    independenceNow = settings.independence;
    selectivityNow = settings.selectivity;
    amountNow = settings.amount;
    for (int c = 0; c < 2; ++c)
    {
        std::fill (inBuf[(size_t) c].begin(), inBuf[(size_t) c].end(), 0.0f);
        std::fill (outAcc[(size_t) c].begin(), outAcc[(size_t) c].end(), 0.0f);
        std::fill (dry[(size_t) c].begin(), dry[(size_t) c].end(), 0.0f);
        std::fill (state[(size_t) c].begin(), state[(size_t) c].end(), 0.0);
        std::fill (holdLeft[(size_t) c].begin(), holdLeft[(size_t) c].end(), 0);
        minGain[c] = 1.0;
    }
    std::fill (thrBuf.begin(), thrBuf.end(), 1.0);
    pos = 0;
    t = 0;
}

void SpectralStage::setSettings (const Settings& s) noexcept
{
    settings = s;
    const double binHz = sr / frameSize;
    for (int b = 0; b < numBands; ++b)
    {
        const double f = bandStart[(size_t) b] * binHz;
        const double scale = f >= 4000.0 ? s.highReleaseScale : (f < 300.0 ? s.lowReleaseScale : 1.0);
        relCoef[(size_t) b] = decayCoeff (msToSamples (s.releaseMs * scale, sr) / hop);
    }
}

void SpectralStage::process (const float* const* in, float* const* out, const double* thr, int n) noexcept
{
    const int base = frameSize - hop;
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            const float x = in[c][i];
            inBuf[(size_t) c][(size_t) (base + pos)] = x;
            dry[(size_t) c][(size_t) (t & dryMask)] = x;
            out[c][i] = dry[(size_t) c][(size_t) ((t - frameSize) & dryMask)] + outAcc[(size_t) c][(size_t) pos];
        }
        thrBuf[(size_t) (base + pos)] = thr[i];
        ++t;
        if (++pos == hop)
        {
            processFrame();
            pos = 0;
        }
    }
}

void SpectralStage::waterFill (const double* a, double requirement, double* red) noexcept
{
    double total = 0.0;
    for (int b = 0; b < numBands; ++b)
    {
        total += a[b];
        red[b] = 0.0;
    }
    if (requirement >= 1.0 || total <= 1.0e-12)
        return;

    for (int b = 0; b < numBands; ++b)
        sorted[(size_t) b] = a[b];
    std::sort (sorted.begin(), sorted.end(), [] (double x, double y) { return x > y; });

    const double goal = requirement * total;
    double capped = 0.0, level = 0.0;
    for (int j = 0; j < numBands; ++j)
    {
        capped += sorted[(size_t) j];
        const double rest = total - capped;
        level = (goal - rest) / (double) (j + 1);
        if (j + 1 == numBands || level >= sorted[(size_t) j + 1])
            break;
    }
    level = std::max (level, 1.0e-9 * total);

    const double broadband = -std::log (requirement);
    const double sel = selectivityNow;
    for (int b = 0; b < numBands; ++b)
    {
        const double selective = a[b] > level ? std::log (a[b] / level) : 0.0;
        red[b] = sel * selective + (1.0 - sel) * broadband;
    }
}

void SpectralStage::processFrame() noexcept
{
    rampControl (independenceNow, settings.independence, controlStep);
    rampControl (selectivityNow, settings.selectivity, controlStep);
    rampControl (amountNow, settings.amount, controlStep);
    const int N = frameSize;
    const float* w = window.data();
    for (int i = 0; i < N; ++i)
        z[(size_t) i] = { inBuf[0][(size_t) i] * w[i], inBuf[1][(size_t) i] * w[i] };
    fft->forward (z.data());
    fft->splitPacked (z.data(), spec[0].data(), spec[1].data());

    // the frame's requirement: the strongest over-threshold sample in its central 3 hops
    double req[2] { 1.0, 1.0 };
    const int from = N / 2 - (3 * hop) / 2, to = N / 2 + (3 * hop) / 2;
    for (int c = 0; c < 2; ++c)
        for (int i = from; i < to; ++i)
        {
            const double a = std::abs ((double) inBuf[(size_t) c][(size_t) i]);
            if (a * req[c] > thrBuf[(size_t) i])
                req[c] = thrBuf[(size_t) i] / a;
        }

    for (int c = 0; c < 2; ++c)
    {
        for (int b = 0; b < numBands; ++b)
        {
            double e = 0.0;
            for (int k = bandStart[(size_t) b]; k < bandStart[(size_t) b + 1]; ++k)
                e += (double) std::norm (spec[c][(size_t) k]);
            amps[c][(size_t) b] = std::sqrt (e);
        }
        // only a fraction (in dB) of the needed reduction is done here; the time-domain core after it does the rest
        const double r = req[c] < 1.0 ? std::exp (amountNow * std::log (req[c])) : 1.0;
        waterFill (amps[c].data(), r, target[(size_t) c].data());
        // spread each band's reduction to its neighbours: a smooth spectral gain, no narrow notches
        auto& tg = target[(size_t) c];
        for (int b = 0; b < numBands; ++b)
            sorted[(size_t) b] = tg[(size_t) b];
        for (int b = 0; b < numBands; ++b)
        {
            double v = sorted[(size_t) b];
            for (int j = 1; j <= 2; ++j)
            {
                const double share = j == 1 ? kSpread : kSpread * kSpread;
                if (b - j >= 0)
                    v = std::max (v, share * sorted[(size_t) (b - j)]);
                if (b + j < numBands)
                    v = std::max (v, share * sorted[(size_t) (b + j)]);
            }
            tg[(size_t) b] = v;
        }
    }

    bool any[2] { false, false };
    const double si = independenceNow;
    for (int b = 0; b < numBands; ++b)
    {
        const double link = std::max (target[0][(size_t) b], target[1][(size_t) b]);
        for (int c = 0; c < 2; ++c)
        {
            const double tg = si * target[(size_t) c][(size_t) b] + (1.0 - si) * link;
            double& st = state[(size_t) c][(size_t) b];
            int& hl = holdLeft[(size_t) c][(size_t) b];
            if (tg >= st)
            {
                st = tg;
                hl = holdFrames[(size_t) b];
            }
            else if (hl > 0)
                --hl;
            else
                st = std::max (tg, st * relCoef[(size_t) b]);
            if (st < 1.0e-9)
                st = 0.0;
            any[c] = any[c] || st > 0.0;
        }
    }

    // shift the overlap-add accumulator and the input / threshold buffers by one hop
    for (int c = 0; c < 2; ++c)
    {
        auto& acc = outAcc[(size_t) c];
        std::memmove (acc.data(), acc.data() + hop, sizeof (float) * (size_t) (N - hop));
        std::fill (acc.begin() + (N - hop), acc.end(), 0.0f);
    }

    if (any[0] || any[1])
    {
        double eIn[2] { 0.0, 0.0 }, eOut[2] { 0.0, 0.0 };
        for (int c = 0; c < 2; ++c)
        {
            const auto& st = state[(size_t) c];
            for (int k = 0; k < numBins; ++k)
            {
                const int b = binLo[(size_t) k];
                const double f = binFrac[(size_t) k];
                binRed[(size_t) k] = f > 0.0 ? st[(size_t) b] + f * (st[(size_t) b + 1] - st[(size_t) b]) : st[(size_t) b];
            }
            // 7-tap triangular smoothing across bins: a short spectral-gain impulse response, so the frame's circular
            // convolution stays inside the window (no time aliasing that would modulate steady tones)
            for (int k = 0; k < numBins; ++k)
            {
                double acc = 0.0;
                for (int j = -3; j <= 3; ++j)
                {
                    const int kk = std::clamp (k + j, 0, numBins - 1);
                    acc += (4.0 - std::abs (j)) * binRed[(size_t) kk];
                }
                binSmooth[(size_t) k] = acc / 16.0;
            }
            for (int k = 0; k < numBins; ++k)
            {
                const double r = binSmooth[(size_t) k];
                const double g = r > 0.0 ? std::exp (-r) : 1.0;
                const double p = (double) std::norm (spec[c][(size_t) k]);
                eIn[c] += p;
                eOut[c] += p * g * g;
                spec[c][(size_t) k] *= (float) (g - 1.0);
            }
            if (eIn[c] > 1.0e-20)
                minGain[c] = std::min (minGain[c], std::sqrt (eOut[c] / eIn[c]));
        }
        fft->mergePacked (spec[0].data(), spec[1].data(), z.data());
        fft->inverse (z.data());
        float* a0 = outAcc[0].data();
        float* a1 = outAcc[1].data();
        for (int i = 0; i < N; ++i)
        {
            const float sw = 0.5f * w[i];
            a0[i] += z[(size_t) i].real() * sw;
            a1[i] += z[(size_t) i].imag() * sw;
        }
    }

    for (int c = 0; c < 2; ++c)
        std::memmove (inBuf[(size_t) c].data(), inBuf[(size_t) c].data() + hop, sizeof (float) * (size_t) (N - hop));
    std::memmove (thrBuf.data(), thrBuf.data() + hop, sizeof (double) * (size_t) (N - hop));
}

//==============================================================================
MultibandStage::MultibandStage() = default;
MultibandStage::~MultibandStage() = default;

void MultibandStage::prepare (double sampleRate)
{
    sr = sampleRate;
    constexpr double crossovers[3] { 120.0, 1000.0, 6000.0 };
    constexpr double attenuationDb = 50.0;
    const double beta = 0.1102 * (attenuationDb - 8.7);

    // the longest kernel (lowest crossover) sets the common length; shorter ones are centred in it
    std::array<int, 3> lengths {};
    for (int j = 0; j < 3; ++j)
    {
        const double dw = 2.0 * kPi * 0.8 * crossovers[j] / sr;
        int len = (int) std::ceil ((attenuationDb - 8.0) / (2.285 * dw)) + 1;
        len |= 1;
        lengths[(size_t) j] = len;
    }
    firLength = lengths[0];
    const int centre = (firLength - 1) / 2;
    constexpr int partition = 64;

    std::vector<float> taps ((size_t) firLength);
    for (int j = 0; j < 3; ++j)
    {
        const int half = (lengths[(size_t) j] - 1) / 2;
        const double fc = crossovers[j] / sr;
        std::vector<double> h ((size_t) firLength, 0.0);
        double sum = 0.0;
        for (int n = centre - half; n <= centre + half; ++n)
        {
            const double x = (double) (n - centre);
            const double s = std::abs (x) < 1.0e-12 ? 2.0 * fc : std::sin (2.0 * kPi * fc * x) / (kPi * x);
            const double r = x / (double) (half + 1);
            h[(size_t) n] = s * besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (beta);
            sum += h[(size_t) n];
        }
        for (int n = 0; n < firLength; ++n)
            taps[(size_t) n] = (float) (h[(size_t) n] / sum);
        lowpass[j].prepare (firLength, partition, 2);
        lowpass[j].setKernel (0, taps.data(), firLength);
        lowpass[j].setKernel (1, taps.data(), firLength);
        lowpass[j].reset();   // installs the kernels at once
    }
    splitLatency = centre + lowpass[0].getLatencySamples();

    lookahead = (int) std::lround (msToSamples (4.0, sr));
    constexpr double attackMs[kBands] { 0.4, 0.2, 0.1, 0.05 };
    constexpr double holdMs[kBands] { 0.8, 0.5, 0.4, 0.4 };
    kPeakRelease = decayCoeff (msToSamples (kGatePeakMs, sr));
    const int ringSize = nextPow2 (lookahead + 4);
    ringMask = ringSize - 1;
    for (int b = 0; b < kBands; ++b)
    {
        attack[(size_t) b] = std::clamp ((int) std::lround (msToSamples (attackMs[b], sr)), 1, lookahead);
        hold[(size_t) b] = (int) std::lround (msToSamples (holdMs[b], sr));
        for (int c = 0; c < 2; ++c)
        {
            bandRing[(size_t) b][(size_t) c].assign ((size_t) ringSize, 0.0f);
            redRing[(size_t) b][(size_t) c].assign ((size_t) ringSize, 0.0);
            windowMax[(size_t) b][(size_t) c].prepare (attack[(size_t) b] + hold[(size_t) b] + 8);
            box[(size_t) b][(size_t) c].prepare (attack[(size_t) b]);
            box[(size_t) b][(size_t) c].setLength (attack[(size_t) b]);
        }
    }
    kPower = chargeCoeff (msToSamples (kSteadyPowerMs, sr));
    steadyMin = (long long) std::lround (msToSamples (kSteadyMinMs, sr));
    lowAttack = std::clamp ((int) std::lround (msToSamples (kLowAttackMs, sr)), 1, lookahead);
    kLowRelease[0] = decayCoeff (msToSamples (kLowReleaseMs, sr));
    for (int b = 0; b < kLowStreams; ++b)
        for (int c = 0; c < 2; ++c)
        {
            lowRedRing[(size_t) b][(size_t) c].assign ((size_t) ringSize, 0.0);
            lowWindowMax[(size_t) b][(size_t) c].prepare (lowAttack + 8);
            lowBox[(size_t) b][(size_t) c].prepare (lowAttack);
            lowBox[(size_t) b][(size_t) c].setLength (lowAttack);
        }
    for (int c = 0; c < 2; ++c)
    {
        inputDelay[(size_t) c].prepare (splitLatency + 1);
        inputDelay[(size_t) c].setDelay (splitLatency);
        xdRing[(size_t) c].assign ((size_t) ringSize, 0.0f);
        xdBuf[(size_t) c].assign ((size_t) kChunk, 0.0f);
        for (int j = 0; j < 3; ++j)
            lpBuf[(size_t) j][(size_t) c].assign ((size_t) kChunk, 0.0f);
    }
    setSettings (settings);
    reset();
}

void MultibandStage::reset() noexcept
{
    controlStep = 1.0 / std::max (1.0, msToSamples (20.0, sr));
    independenceNow = independence;
    for (auto& l : lowpass)
        l.reset();
    for (int c = 0; c < 2; ++c)
    {
        inputDelay[(size_t) c].reset();
        std::fill (xdRing[(size_t) c].begin(), xdRing[(size_t) c].end(), 0.0f);
        minGain[c] = 1.0;
    }
    for (int b = 0; b < kBands; ++b)
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (bandRing[(size_t) b][(size_t) c].begin(), bandRing[(size_t) b][(size_t) c].end(), 0.0f);
            std::fill (redRing[(size_t) b][(size_t) c].begin(), redRing[(size_t) b][(size_t) c].end(), 0.0);
            windowMax[(size_t) b][(size_t) c].reset();
            box[(size_t) b][(size_t) c].reset();
        }
        release[(size_t) b].fill (0.0);
    }
    for (int b = 0; b < kLowStreams; ++b)
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (lowRedRing[(size_t) b][(size_t) c].begin(), lowRedRing[(size_t) b][(size_t) c].end(), 0.0);
            lowWindowMax[(size_t) b][(size_t) c].reset();
            lowBox[(size_t) b][(size_t) c].reset();
        }
        lowRelease[(size_t) b].fill (0.0);
    }
    hiPeak[0] = hiPeak[1] = allPeak[0] = allPeak[1] = hiPower[0] = hiPower[1] = 0.0;
    steadyRun[0] = steadyRun[1] = bassRun[0] = bassRun[1] = 0;
    loPeak[0] = loPeak[1] = loPower[0] = loPower[1] = 0.0;
    t = 0;
}

void MultibandStage::setSettings (const Settings& s) noexcept
{
    settings = s;
    constexpr double scale[kBands] { 2.0, 1.3, 1.0, 1.0 };
    for (int b = 0; b < kBands; ++b)
    {
        kRelease[(size_t) b] = decayCoeff (msToSamples (s.releaseMs * scale[b], sr));
        independence[(size_t) b] = std::clamp (b < 2 ? s.siSus : s.siTrans, 0.0, 1.0);
    }
    kLowRelease[1] = decayCoeff (msToSamples (s.steadyBassReleaseMs, sr));
}

void MultibandStage::process (const float* const* in, float* const* out, const double* thr, int n) noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        for (int j = 0; j < 3; ++j)
            std::memcpy (lpBuf[(size_t) j][(size_t) c].data(), in[c], sizeof (float) * (size_t) n);
        std::memcpy (xdBuf[(size_t) c].data(), in[c], sizeof (float) * (size_t) n);
        inputDelay[(size_t) c].process (xdBuf[(size_t) c].data(), n);
    }
    for (int j = 0; j < 3; ++j)
    {
        float* chans[2] { lpBuf[(size_t) j][0].data(), lpBuf[(size_t) j][1].data() };
        lowpass[j].process (chans, n);
    }

    for (int i = 0; i < n; ++i)
    {
        const long long idx = t;
        const double th = thr[i];
        for (int c = 0; c < 2; ++c)
        {
            const double l0 = lpBuf[0][(size_t) c][(size_t) i];
            const double l1 = lpBuf[1][(size_t) c][(size_t) i];
            const double l2 = lpBuf[2][(size_t) c][(size_t) i];
            const double xd = xdBuf[(size_t) c][(size_t) i];
            const double bands[kBands] { l0, l1 - l0, l2 - l1, xd - l2 };
            xdRing[(size_t) c][(size_t) (idx & ringMask)] = (float) xd;
            for (int b = 0; b < kBands; ++b)
                bandRing[(size_t) b][(size_t) c][(size_t) (idx & ringMask)] = (float) bands[b];

            double red[kBands] { 0.0, 0.0, 0.0, 0.0 };
            double lowRed[kLowStreams] { 0.0, 0.0, 0.0, 0.0 };
            const double a = std::abs (xd);
            const double hi = bands[2] + bands[3];
            hiPeak[c] = std::max (std::abs (hi), hiPeak[c] * kPeakRelease);
            allPeak[c] = std::max (a, allPeak[c] * kPeakRelease);
            hiPower[c] += (hi * hi - hiPower[c]) * kPower;
            const bool sparse = hiPeak[c] < kGateShare * allPeak[c];
            if (hiPeak[c] * hiPeak[c] < kSteadyCrest * kSteadyCrest * hiPower[c])
                ++steadyRun[c];
            else
                steadyRun[c] = 0;
            const double lo = bands[0] + bands[1];
            loPeak[c] = std::max (std::abs (lo), loPeak[c] * kPeakRelease);
            loPower[c] += (lo * lo - loPower[c]) * kPower;
            if (loPeak[c] * loPeak[c] < kSteadyCrest * kSteadyCrest * loPower[c])
                ++bassRun[c];
            else
                bassRun[c] = 0;
            const bool steadyBass = sparse && bassRun[c] >= steadyMin;
            const bool steady = (! sparse && hiPeak[c] < kSteadyShare * allPeak[c] && steadyRun[c] >= steadyMin) || steadyBass;
            const int q0 = steadyBass ? kLowBands : 0;
            if (a > th && steady)
            {
                // steady highs (a tone, a pad) under a bass-made peak, or a steady bass under sparse highs: the
                // excess comes out of the bands < 1 kHz alone, shared by contribution^1.5, through the smooth low
                // path (no gain ripple on the highs, a clean sustained bass; the core takes what the 80 % cap leaves)
                const double sgn = xd < 0.0 ? -1.0 : 1.0;
                double contrib[kLowBands], w[kLowBands], wsum = 0.0;
                for (int b = 0; b < kLowBands; ++b)
                {
                    contrib[b] = bands[b] * sgn;
                    w[b] = contrib[b] > 0.0 ? std::pow (contrib[b], kSharePower) : 0.0;
                    wsum += w[b];
                }
                const double excess = a - th;
                for (int b = 0; b < kLowBands; ++b)
                    if (w[b] > 0.0)
                    {
                        const double share = std::min (kLowCap * contrib[b], excess * w[b] / wsum);
                        lowRed[q0 + b] = settings.amount * std::log (contrib[b] / (contrib[b] - share));
                    }
            }
            for (int b = 0; b < kLowStreams; ++b)
                lowRedRing[(size_t) b][(size_t) c][(size_t) (idx & ringMask)] = lowRed[b];
            if (a > th && sparse && ! steadyBass)
            {
                // share the excess among the bands that make the peak, in proportion to contribution^1.5 (no band
                // gives up more than 90 % of its contribution; the core takes what is left)
                const double sgn = xd < 0.0 ? -1.0 : 1.0;
                double contrib[kBands], w[kBands], wsum = 0.0;
                for (int b = 0; b < kBands; ++b)
                {
                    contrib[b] = bands[b] * sgn;
                    w[b] = contrib[b] > 0.0 ? std::pow (contrib[b], kSharePower) : 0.0;
                    wsum += w[b];
                }
                const double excess = a - th;
                for (int b = 0; b < kBands; ++b)
                    if (w[b] > 0.0)
                    {
                        const double share = std::min (0.9 * contrib[b], excess * w[b] / wsum);
                        red[b] = settings.amount * std::log (contrib[b] / (contrib[b] - share));
                    }
            }
            for (int b = 0; b < kBands; ++b)
                redRing[(size_t) b][(size_t) c][(size_t) (idx & ringMask)] = red[b];
        }

        const long long sIdx = idx - lookahead;
        double gains[kBands][2];
        for (int b = 0; b < kBands; ++b)
        {
            const int A = attack[(size_t) b];
            const long long k = idx - (lookahead - A + 1);
            const long long oldest = k - (A + hold[(size_t) b]) + 1;
            double dm[2];
            for (int c = 0; c < 2; ++c)
            {
                auto& wm = windowMax[(size_t) b][(size_t) c];
                wm.push (k, redRing[(size_t) b][(size_t) c][(size_t) (k & ringMask)]);
                wm.dropOlderThan (oldest);
                dm[c] = wm.max();
            }
            auto& rel = release[(size_t) b];
            const double kr = kRelease[(size_t) b];
            rel[0] = std::max (dm[0], rel[0] * kr);
            rel[1] = std::max (dm[1], rel[1] * kr);
            rel[2] = std::max (std::max (dm[0], dm[1]), rel[2] * kr);
            const double si = rampControl (independenceNow[(size_t) b], independence[(size_t) b], controlStep);
            for (int c = 0; c < 2; ++c)
            {
                const double g = si * rel[(size_t) c] + (1.0 - si) * rel[2];
                gains[b][c] = box[(size_t) b][(size_t) c].process (g > 0.0 ? std::exp (-g) : 1.0);
            }
        }
        // the smooth low path: a window of the attack length, release, box; the band keeps the lower of both gains
        for (int b = 0; b < kLowStreams; ++b)
        {
            const int A = lowAttack;
            const long long k = idx - (lookahead - A + 1);
            const long long oldest = k - A + 1;
            double dm[2];
            for (int c = 0; c < 2; ++c)
            {
                auto& wm = lowWindowMax[(size_t) b][(size_t) c];
                wm.push (k, lowRedRing[(size_t) b][(size_t) c][(size_t) (k & ringMask)]);
                wm.dropOlderThan (oldest);
                dm[c] = wm.max();
            }
            auto& rel = lowRelease[(size_t) b];
            const double kr = kLowRelease[(size_t) (b / kLowBands)];
            rel[0] = std::max (dm[0], rel[0] * kr);
            rel[1] = std::max (dm[1], rel[1] * kr);
            rel[2] = std::max (std::max (dm[0], dm[1]), rel[2] * kr);
            const double si = independenceNow[(size_t) (b % kLowBands)];
            for (int c = 0; c < 2; ++c)
            {
                const double g = si * rel[(size_t) c] + (1.0 - si) * rel[2];
                gains[b % kLowBands][c] = std::min (gains[b % kLowBands][c], lowBox[(size_t) b][(size_t) c].process (g > 0.0 ? std::exp (-g) : 1.0));
            }
        }

        for (int c = 0; c < 2; ++c)
        {
            double y = xdRing[(size_t) c][(size_t) (sIdx & ringMask)];
            double num = 0.0, den = 0.0;
            for (int b = 0; b < kBands; ++b)
            {
                const double g = gains[b][c];
                const double v = bandRing[(size_t) b][(size_t) c][(size_t) (sIdx & ringMask)];
                if (g != 1.0)
                    y += (g - 1.0) * v;
                num += g * g * v * v;
                den += v * v;
            }
            if (den > 1.0e-12)
                minGain[c] = std::min (minGain[c], std::sqrt (num / den));
            out[c][i] = (float) y;
        }
        ++t;
    }
}

} // namespace ps::maxi
