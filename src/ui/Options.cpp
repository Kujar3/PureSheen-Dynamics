#include "ui/Widgets.h"

// UiOptions <-> ValueTree "Options" (stored in the processor's opaque UI state). Unknown / missing properties keep the
// defaults, every value is clamped to its legal set, so old or hand-edited projects always load.
namespace ps::ui
{
namespace
{
template <typename Enum>
Enum enumFrom (const juce::ValueTree& t, const juce::Identifier& id, Enum fallback, int count)
{
    const int v = (int) t.getProperty (id, (int) fallback);
    return v >= 0 && v < count ? (Enum) v : fallback;
}

float nearestOf (float v, std::initializer_list<float> legal)
{
    float best = *legal.begin();

    for (float l : legal)
        if (std::abs (l - v) < std::abs (best - v))
            best = l;

    return best;
}

int nearestOf (int v, std::initializer_list<int> legal)
{
    int best = *legal.begin();

    for (int l : legal)
        if (std::abs (l - v) < std::abs (best - v))
            best = l;

    return best;
}

const juce::Identifier idMeterType ("meterType"), idMeterScale ("meterScale"), idMeterMs ("meterMidSide"),
    idPeakHold ("peakHoldMs"), idRmsWindow ("rmsWindowMs"), idTruePeaks ("detectTruePeaks"), idKType ("kSystemType"),
    idSpecType ("spectrumType"), idFreqScale ("freqScale"), idAveraging ("spectrumAveragingS"),
    idSpecPeak ("spectrumPeakHold"), idSpecFill ("spectrumFill"), idTilt ("spectrumTilt"), idFft ("fftSize"),
    idTooltips ("showTooltips"), idDim ("dimBypassed"), idHistory ("historyDepth");
} // namespace

juce::ValueTree UiOptions::toValueTree() const
{
    juce::ValueTree t ("Options");
    t.setProperty (idMeterType, (int) meterType, nullptr);
    t.setProperty (idMeterScale, (int) meterScale, nullptr);
    t.setProperty (idMeterMs, meterMidSide, nullptr);
    t.setProperty (idPeakHold, (double) peakHoldMs, nullptr);
    t.setProperty (idRmsWindow, (double) rmsWindowMs, nullptr);
    t.setProperty (idTruePeaks, detectTruePeaks, nullptr);
    t.setProperty (idKType, kSystemType, nullptr);
    t.setProperty (idSpecType, (int) spectrumType, nullptr);
    t.setProperty (idFreqScale, (int) freqScale, nullptr);
    t.setProperty (idAveraging, (double) spectrumAveragingS, nullptr);
    t.setProperty (idSpecPeak, spectrumPeakHold, nullptr);
    t.setProperty (idSpecFill, spectrumFill, nullptr);
    t.setProperty (idTilt, (double) spectrumTiltDbPerOct, nullptr);
    t.setProperty (idFft, fftSize, nullptr);
    t.setProperty (idTooltips, showTooltips, nullptr);
    t.setProperty (idDim, dimBypassed, nullptr);
    t.setProperty (idHistory, historyDepth, nullptr);
    return t;
}

UiOptions UiOptions::fromValueTree (const juce::ValueTree& t)
{
    UiOptions o;

    if (! t.isValid())
        return o;

    o.meterType = enumFrom (t, idMeterType, o.meterType, 7);
    o.meterScale = enumFrom (t, idMeterScale, o.meterScale, 5);
    o.meterMidSide = (bool) t.getProperty (idMeterMs, o.meterMidSide);
    o.peakHoldMs = juce::jlimit (5.0f, 1.0e9f, (float) (double) t.getProperty (idPeakHold, (double) o.peakHoldMs));
    o.rmsWindowMs = nearestOf ((float) (double) t.getProperty (idRmsWindow, (double) o.rmsWindowMs),
                               { 10.0f, 50.0f, 300.0f, 1475.0f, 2650.0f, 3825.0f, 5000.0f });
    o.detectTruePeaks = (bool) t.getProperty (idTruePeaks, o.detectTruePeaks);
    o.kSystemType = nearestOf ((int) t.getProperty (idKType, o.kSystemType), { 12, 14, 20 });

    o.spectrumType = enumFrom (t, idSpecType, o.spectrumType, 4);
    o.freqScale = enumFrom (t, idFreqScale, o.freqScale, 3);
    o.spectrumAveragingS = nearestOf ((float) (double) t.getProperty (idAveraging, (double) o.spectrumAveragingS),
                                      { -1.0f, 0.0f, 1.0f, 3.0f, 5.0f, 10.0f });
    o.spectrumPeakHold = (bool) t.getProperty (idSpecPeak, o.spectrumPeakHold);
    o.spectrumFill = (bool) t.getProperty (idSpecFill, o.spectrumFill);
    o.spectrumTiltDbPerOct = juce::jlimit (0.0f, 6.0f, (float) (double) t.getProperty (idTilt, (double) o.spectrumTiltDbPerOct));
    o.fftSize = nearestOf ((int) t.getProperty (idFft, o.fftSize), { 2048, 4096, 8192, 16384 });

    o.showTooltips = (bool) t.getProperty (idTooltips, o.showTooltips);
    o.dimBypassed = (bool) t.getProperty (idDim, o.dimBypassed);
    o.historyDepth = juce::jlimit (10, 1000, (int) t.getProperty (idHistory, o.historyDepth));
    return o;
}
} // namespace ps::ui
