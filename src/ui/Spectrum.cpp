#include "ui/Widgets.h"

#include "core/Fft.h"

#include <complex>
#include <cmath>

// UI-thread spectrum analyser (owner: ui-shell).
//   - Hann window, 50 % overlap, ps::Fft. Levels are calibrated so a full-scale sine reads 0 dB in its bin.
//   - Averaging (power domain): "real time" = exponential, tau 100 ms (steady but responsive); N s = exponential with
//     tau N s; infinite = running mean since clear(). A separate running mean feeds getBinMagnitudesDb() (Learn).
//   - Tilt (dB/oct around 1 kHz) is added to every displayed value (default 3 dB/oct), not to the Learn data.
//   - Display types: Linear (per bin; columns show the loudest bin they cover), 1/3-octave (ISO centres 20 Hz..20 kHz),
//     Critical (24 Bark bands, Zwicker edges), Octave (31.5 Hz..16 kHz). Band values are the mean power of the bins
//     inside the band (so the levels stay comparable with the linear view); bands narrower than a bin interpolate.
//   - Peak hold: per bin / band maximum of the displayed level since clear(); drawn when UiOptions::spectrumPeakHold.
namespace ps::ui
{
namespace
{
constexpr int kScratch = 16384;

struct Band
{
    float lo = 0, hi = 0, centre = 0;
};

std::vector<Band> makeBands (UiOptions::SpectrumType type)
{
    std::vector<Band> bands;

    switch (type)
    {
        case UiOptions::SpectrumType::ThirdOctave:
            for (int n = -17; n <= 13; ++n)
            {
                const float c = 1000.0f * std::pow (2.0f, (float) n / 3.0f);
                bands.push_back ({ c * std::pow (2.0f, -1.0f / 6.0f), c * std::pow (2.0f, 1.0f / 6.0f), c });
            }
            break;

        case UiOptions::SpectrumType::Octave:
            for (int n = -5; n <= 4; ++n)
            {
                const float c = 1000.0f * std::pow (2.0f, (float) n);
                bands.push_back ({ c * std::sqrt (0.5f), c * std::sqrt (2.0f), c });
            }
            break;

        case UiOptions::SpectrumType::Critical:
        {
            static const float edges[] = { 20, 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720,
                                           2000, 2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000,
                                           15500, 20000 };
            for (size_t i = 0; i + 1 < std::size (edges); ++i)
                bands.push_back ({ edges[i], edges[i + 1], std::sqrt (edges[i] * edges[i + 1]) });
            break;
        }

        case UiOptions::SpectrumType::Linear:
        default:
            break;
    }

    return bands;
}
} // namespace

struct SpectrumAnalyser::Impl
{
    Impl() { configure(); }

    void configure()
    {
        fftSize = juce::jlimit (512, 32768, juce::nextPowerOfTwo (options.fftSize));
        int order = 0;
        while ((1 << order) < fftSize)
            ++order;

        fft = std::make_unique<ps::Fft> (order);
        window.resize ((size_t) fftSize);
        double sum = 0.0;

        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
            sum += window[(size_t) i];
        }

        powerScale = (float) std::pow (2.0 / sum, 2.0);
        ring.assign ((size_t) fftSize, 0.0f);
        buffer.assign ((size_t) fftSize, {});
        const auto bins = (size_t) (fftSize / 2 + 1);
        avgPower.assign (bins, 0.0);
        longPower.assign (bins, 0.0);
        levelDb.assign (bins, -200.0f);
        peakDb.assign (bins, -200.0f);
        longDb.clear();
        bands = makeBands (options.spectrumType);
        bandLevel.assign (bands.size(), -200.0f);
        bandPeak.assign (bands.size(), -200.0f);
        resetState();
    }

    void resetState()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (avgPower.begin(), avgPower.end(), 0.0);
        std::fill (longPower.begin(), longPower.end(), 0.0);
        std::fill (levelDb.begin(), levelDb.end(), -200.0f);
        std::fill (peakDb.begin(), peakDb.end(), -200.0f);
        std::fill (bandLevel.begin(), bandLevel.end(), -200.0f);
        std::fill (bandPeak.begin(), bandPeak.end(), -200.0f);
        longDb.clear();
        ringPos = 0;
        filled = 0;
        sinceFrame = 0;
        frames = 0;
        longFrames = 0;
    }

    float binHz() const noexcept { return (float) (sampleRate / (double) fftSize); }

    float tiltAt (float hz) const noexcept
    {
        return options.spectrumTiltDbPerOct * std::log2 (juce::jmax (1.0f, hz) / 1000.0f);
    }

    void push (float x)
    {
        ring[(size_t) ringPos] = x;
        ringPos = (ringPos + 1) & (fftSize - 1);
        filled = juce::jmin (fftSize, filled + 1);

        if (++sinceFrame >= fftSize / 2 && filled >= fftSize)
        {
            sinceFrame = 0;
            computeFrame();
        }
    }

    void computeFrame()
    {
        for (int i = 0; i < fftSize; ++i)
            buffer[(size_t) i] = { ring[(size_t) ((ringPos + i) & (fftSize - 1))] * window[(size_t) i], 0.0f };

        fft->forward (buffer.data());

        const double dt = (double) (fftSize / 2) / sampleRate;
        ++frames;
        ++longFrames;
        double alpha = 1.0;

        if (options.spectrumAveragingS < 0.0f)
            alpha = 1.0 / (double) frames;
        else if (frames > 1)
            alpha = 1.0 - std::exp (-dt / (options.spectrumAveragingS > 0.0f ? (double) options.spectrumAveragingS : 0.1));

        const auto bins = avgPower.size();
        const float bHz = binHz();

        for (size_t k = 0; k < bins; ++k)
        {
            const double p = (double) std::norm (buffer[k]) * powerScale;
            avgPower[k] += alpha * (p - avgPower[k]);
            longPower[k] += (p - longPower[k]) / (double) longFrames;
            levelDb[k] = (float) (10.0 * std::log10 (avgPower[k] + 1.0e-20)) + tiltAt ((float) k * bHz);
            peakDb[k] = juce::jmax (peakDb[k], levelDb[k]);
        }

        for (size_t b = 0; b < bands.size(); ++b)
        {
            const auto& band = bands[b];
            const int k0 = (int) std::ceil (band.lo / bHz), k1 = (int) std::floor (band.hi / bHz);
            double power = 0.0;
            int count = 0;

            for (int k = juce::jmax (1, k0); k <= k1 && k < (int) bins; ++k)
                if ((float) k * bHz < band.hi)
                    power += avgPower[(size_t) k], ++count;

            if (count == 0)
                power = interpolatedPower (band.centre / bHz);
            else
                power /= (double) count;

            bandLevel[b] = (float) (10.0 * std::log10 (power + 1.0e-20)) + tiltAt (band.centre);
            bandPeak[b] = juce::jmax (bandPeak[b], bandLevel[b]);
        }

        longDbDirty = true;
    }

    double interpolatedPower (float bin) const noexcept
    {
        const int maxK = (int) avgPower.size() - 1;
        const float k = juce::jlimit (0.0f, (float) maxK, bin);
        const int k0 = (int) k;
        const int k1 = juce::jmin (maxK, k0 + 1);
        const double f = (double) (k - (float) k0);
        return avgPower[(size_t) k0] * (1.0 - f) + avgPower[(size_t) k1] * f;
    }

    float interpolate (const std::vector<float>& db, float hz) const noexcept
    {
        const int maxK = (int) db.size() - 1;
        const float k = juce::jlimit (0.0f, (float) maxK, hz / binHz());
        const int k0 = (int) k;
        const int k1 = juce::jmin (maxK, k0 + 1);
        const float f = k - (float) k0;
        return db[(size_t) k0] + (db[(size_t) k1] - db[(size_t) k0]) * f;
    }

    int bandIndexFor (float hz) const noexcept
    {
        if (bands.empty())
            return -1;

        for (size_t b = 0; b < bands.size(); ++b)
            if (hz < bands[b].hi)
                return (int) b;

        return (int) bands.size() - 1;
    }

    UiOptions options;
    double sampleRate = 48000.0;
    int fftSize = 4096;
    std::unique_ptr<ps::Fft> fft;
    std::vector<float> window, ring;
    std::vector<std::complex<float>> buffer;
    float powerScale = 1.0f;
    std::vector<double> avgPower, longPower;
    std::vector<float> levelDb, peakDb;
    mutable std::vector<float> longDb;
    mutable bool longDbDirty = false;
    std::vector<Band> bands;
    std::vector<float> bandLevel, bandPeak;
    int ringPos = 0, filled = 0, sinceFrame = 0;
    long long frames = 0, longFrames = 0;
    std::vector<float> scratchL = std::vector<float> ((size_t) kScratch), scratchR = std::vector<float> ((size_t) kScratch);
};

SpectrumAnalyser::SpectrumAnalyser() : impl (std::make_unique<Impl>()) {}
SpectrumAnalyser::~SpectrumAnalyser() = default;

void SpectrumAnalyser::setOptions (const UiOptions& o)
{
    auto& d = *impl;
    const bool reconfigure = o.fftSize != d.options.fftSize || o.spectrumType != d.options.spectrumType;
    const bool reset = reconfigure || o.spectrumAveragingS != d.options.spectrumAveragingS
                       || o.spectrumTiltDbPerOct != d.options.spectrumTiltDbPerOct;
    d.options = o;

    if (reconfigure)
        d.configure();
    else if (reset)
        d.resetState();
}

void SpectrumAnalyser::setSampleRate (double sr)
{
    if (sr > 0.0 && std::abs (sr - impl->sampleRate) > 0.5)
    {
        impl->sampleRate = sr;
        impl->resetState();
    }
}

bool SpectrumAnalyser::update (StereoSampleFifo& fifo, int channelMix)
{
    auto& d = *impl;
    const int n = fifo.pull (d.scratchL.data(), d.scratchR.data(), kScratch);
    return process (d.scratchL.data(), d.scratchR.data(), n, channelMix);
}

bool SpectrumAnalyser::process (const float* left, const float* right, int numSamples, int channelMix)
{
    auto& d = *impl;

    if (left == nullptr || numSamples <= 0)
        return false;

    const auto before = d.frames;
    // Only the newest samples can reach a frame that is still shown: skip what the next frames would overwrite.
    const int start = juce::jmax (0, numSamples - 4 * d.fftSize);

    for (int i = start; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right != nullptr ? right[i] : l;
        float x = 0.5f * (l + r);

        switch (channelMix)
        {
            case 1: x = l; break;
            case 2: x = r; break;
            case 4: x = 0.5f * (l - r); break;
            default: break;
        }

        d.push (x);
    }

    return d.frames != before;
}

bool SpectrumAnalyser::hasData() const noexcept { return impl->frames > 0; }

float SpectrumAnalyser::levelDbAt (float hz) const noexcept
{
    const auto& d = *impl;

    if (d.frames == 0)
        return -200.0f;

    if (! d.bands.empty())
        return d.bandLevel[(size_t) d.bandIndexFor (hz)];

    return d.interpolate (d.levelDb, hz);
}

float SpectrumAnalyser::peakDbAt (float hz) const noexcept
{
    const auto& d = *impl;

    if (d.frames == 0)
        return -200.0f;

    if (! d.bands.empty())
        return d.bandPeak[(size_t) d.bandIndexFor (hz)];

    return d.interpolate (d.peakDb, hz);
}

const std::vector<float>& SpectrumAnalyser::getBinMagnitudesDb() const noexcept
{
    auto& d = *impl;

    if (d.longFrames == 0)
    {
        d.longDb.clear();
    }
    else if (d.longDbDirty)
    {
        d.longDb.resize (d.longPower.size());

        for (size_t k = 0; k < d.longPower.size(); ++k)
            d.longDb[k] = (float) (10.0 * std::log10 (d.longPower[k] + 1.0e-20));

        d.longDbDirty = false;
    }

    return d.longDb;
}

double SpectrumAnalyser::getBinHz() const noexcept { return impl->sampleRate / (double) impl->fftSize; }

void SpectrumAnalyser::clear() { impl->resetState(); }

void SpectrumAnalyser::draw (juce::Graphics& g, const FreqAxis& axis, float minDb, float maxDb, juce::Colour colour,
                             bool fill) const
{
    const auto& d = *impl;

    if (d.frames == 0 || axis.area.isEmpty() || maxDb <= minDb)
        return;

    const auto area = axis.area;
    auto yFor = [&] (float db)
    {
        const float t = (juce::jlimit (minDb - 6.0f, maxDb + 2.0f, db) - minDb) / (maxDb - minDb);
        return juce::jlimit (area.getY(), area.getBottom() + 2.0f, area.getBottom() - t * area.getHeight());
    };

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (area.toNearestInt());

    if (! d.bands.empty())
    {
        // Bars, one per band, with a brighter cap; peak-hold ticks above.
        for (size_t b = 0; b < d.bands.size(); ++b)
        {
            const auto& band = d.bands[b];
            const float x0 = axis.hzToX (juce::jmax (band.lo, axis.minHz));
            const float x1 = axis.hzToX (juce::jmin (band.hi, axis.maxHz));

            if (x1 <= area.getX() || x0 >= area.getRight() || x1 - x0 < 1.0f)
                continue;

            const float y = yFor (d.bandLevel[b]);
            const auto bar = juce::Rectangle<float> (x0 + 1.0f, y, juce::jmax (1.0f, x1 - x0 - 2.0f), area.getBottom() - y);

            if (fill)
            {
                g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (0.42f), bar.getY(),
                                                                   colour.withAlpha (0.08f), area.getBottom()));
                g.fillRect (bar);
            }

            g.setColour (colour);
            g.fillRect (bar.withHeight (2.0f));

            if (d.options.spectrumPeakHold)
            {
                g.setColour (colour.brighter (0.4f).withAlpha (0.8f));
                g.fillRect (bar.withY (yFor (d.bandPeak[b]) - 1.0f).withHeight (1.5f));
            }
        }

        return;
    }

    // Linear: one point every 2 px; a column covering several bins shows the loudest.
    const float bHz = d.binHz();
    auto valueFor = [&] (const std::vector<float>& db, float xa, float xb)
    {
        const float h0 = axis.xToHz (xa), h1 = axis.xToHz (xb);
        const int k0 = (int) std::ceil (h0 / bHz), k1 = (int) std::floor (h1 / bHz);

        if (k1 - k0 >= 1)
        {
            float m = -300.0f;
            for (int k = juce::jmax (1, k0); k <= k1 && k < (int) db.size(); ++k)
                m = juce::jmax (m, db[(size_t) k]);
            return m;
        }

        return d.interpolate (db, 0.5f * (h0 + h1));
    };

    juce::Path line, peakLine;
    const float step = 2.0f;
    bool started = false;

    for (float x = area.getX(); x <= area.getRight() + step; x += step)
    {
        const float y = yFor (valueFor (d.levelDb, x - step * 0.5f, x + step * 0.5f));

        if (! started)
            line.startNewSubPath (x, y), started = true;
        else
            line.lineTo (x, y);
    }

    if (fill)
    {
        juce::Path area2 (line);
        area2.lineTo (area.getRight() + step, area.getBottom() + 2.0f);
        area2.lineTo (area.getX(), area.getBottom() + 2.0f);
        area2.closeSubPath();
        g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (0.34f), area.getY() + area.getHeight() * 0.2f,
                                                           colour.withAlpha (0.03f), area.getBottom()));
        g.fillPath (area2);
    }

    g.setColour (colour.withAlpha (0.95f));
    g.strokePath (line, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (d.options.spectrumPeakHold)
    {
        started = false;

        for (float x = area.getX(); x <= area.getRight() + step; x += step)
        {
            const float y = yFor (valueFor (d.peakDb, x - step * 0.5f, x + step * 0.5f));

            if (! started)
                peakLine.startNewSubPath (x, y), started = true;
            else
                peakLine.lineTo (x, y);
        }

        g.setColour (colour.brighter (0.4f).withAlpha (0.55f));
        g.strokePath (peakLine, juce::PathStrokeType (1.0f));
    }
}
} // namespace ps::ui
