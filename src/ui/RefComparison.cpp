#include "ui/RefComparison.h"

#include "ui/Controls.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Comparison views of the Reference page (see RefComparison.h). Ported from RefMatch's tonal balance, loudness,
// stereo and hints panels; "your mix" there is "your master" here (PureSheen's output after every module), the advice
// and thresholds come from ps::compareProfiles (core/RefCompare) so a readout and a hint never disagree.
// Values ease towards new measurements at the 30 Hz tick (snap on a new reference / region).
namespace ps::ui
{
namespace
{
namespace Col = Colours;

constexpr float kEase = 0.28f;            // per 30 Hz tick
constexpr float kBarRangeDb = 6.0f;       // band bars: +-6 dB
constexpr float kAboveCentreDb = 12.0f;   // plot range around the curves' centre (the top grows up to
constexpr float kMaxAboveCentreDb = 24.0f;//   kMaxAboveCentreDb when a curve peaks higher, e.g. a big sub)
constexpr float kBelowCentreDb = 21.0f;
constexpr float kDisplayTiltDbPerOct = 4.5f;
constexpr float kWidthScalePct = 100.0f;  // top of the width chart ("wide")

juce::String num (float v, int decimals) { return juce::String (v, decimals); }
juce::String signedNum (float v, int decimals) { return juce::String (ps::formatSigned (v, decimals)); }
juce::String abs1 (float v) { return num (std::abs (v), 1); }

void fitLine (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification j,
              float minScale = 0.8f)
{
    g.drawFittedText (text, area.toNearestInt(), j, 1, minScale);
}

float displayTiltDb (float hz) noexcept { return kDisplayTiltDbPerOct * std::log2 (juce::jmax (1.0f, hz) / 1000.0f); }

float logPosition (float hz) noexcept
{
    return (float) (std::log (juce::jlimit (ps::kCurveMinHz, ps::kCurveMaxHz, hz) / ps::kCurveMinHz)
                    / std::log (ps::kCurveMaxHz / ps::kCurveMinHz));
}

struct Smoothed
{
    float shown = 0.0f, target = 0.0f;
    bool valid = false;

    // Returns true when the shown value moved.
    bool step (float newTarget, bool newValid, bool snap, float threshold = 0.01f)
    {
        target = newTarget;
        if (! newValid)
        {
            const bool changed = valid;
            valid = false;
            return changed;
        }
        if (! valid || snap || std::abs (newTarget - shown) < threshold)
        {
            const bool changed = ! valid || shown != newTarget;
            shown = newTarget;
            valid = true;
            return changed;
        }
        shown += (newTarget - shown) * kEase;
        return true;
    }
};

ps::Severity worse (ps::Severity a, ps::Severity b) { return (int) a >= (int) b ? a : b; }

// Plain-language explanation of each band (tooltips).
const char* bandExplanation (int band)
{
    switch (band)
    {
        case 0: return "Sub bass (20 - 60 Hz): the deep rumble you feel more than hear - sub bass and the tail of the kick. "
                       "Too much eats headroom and booms on big systems; too little feels weak in a club.";
        case 1: return "Bass (60 - 200 Hz): the body of the bass and the punch of the kick. Too much sounds boomy, too "
                       "little sounds thin.";
        case 2: return "Low mids (200 - 500 Hz): warmth, but also mud and boxiness. Too much makes the master cloudy, too "
                       "little sounds thin or cold.";
        case 3: return "Mids (500 Hz - 2 kHz): the body of vocals, synths and guitars. Too much sounds honky or nasal, too "
                       "little sounds scooped and far away.";
        case 4: return "High mids (2 - 6 kHz): presence and clarity, where the ear is most sensitive. Too much sounds "
                       "harsh and tiring, too little sounds distant.";
        case 5: return "Air (6 - 20 kHz): brightness, cymbals, breath and sizzle. Too much sounds sharp or hissy, too "
                       "little sounds dull.";
        default: return "";
    }
}

// Comparative words: the bars show differences from one reference, not faults of the master on their own.
juce::String bandWord (int band, float diff)
{
    if (std::abs (diff) < ps::kBandMinorDb)
        return "close";
    const bool more = diff > 0.0f;
    switch (band)
    {
        case 0: return more ? "heavier" : "lighter";
        case 1: return more ? "boomier" : "thinner";
        case 2: return more ? "muddier" : "colder";
        case 3: return more ? "honkier" : "more scooped";
        case 4: return more ? "harsher" : "more distant";
        case 5: return more ? "brighter" : "duller";
        default: return {};
    }
}

// The message box in the middle of a view when data is missing.
void paintGuidance (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& line1, const juce::String& line2)
{
    if (line1.isEmpty())
        return;
    const auto f1 = font (line2.isEmpty() ? 13.0f : 14.0f, 600), f2 = font (11.5f);
    const float w = juce::jmin (area.getWidth() - 16.0f, juce::jmax (textWidth (f1, line1), textWidth (f2, line2)) + 36.0f);
    const float h = line2.isEmpty() ? 30.0f : 48.0f;
    const auto box = juce::Rectangle<float> (w, juce::jmin (h, area.getHeight() - 4.0f)).withCentre (area.getCentre());
    g.setColour (Col::panel.withAlpha (0.9f));
    g.fillRoundedRectangle (box, 8.0f);
    g.setColour (Col::outline.brighter (0.1f));
    g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);
    auto t = box.reduced (12.0f, 5.0f);
    g.setFont (f1);
    g.setColour (Col::text);
    fitLine (g, line1, line2.isEmpty() ? t : t.removeFromTop (20.0f), juce::Justification::centred, 0.75f);
    if (line2.isNotEmpty())
    {
        g.setFont (f2);
        g.setColour (Col::textDim);
        fitLine (g, line2, t, juce::Justification::centred, 0.75f);
    }
}

// The guidance for the views that need both profiles (empty when both are there).
void guidanceFor (const RefViewData& d, juce::String& line1, juce::String& line2)
{
    line1 = {};
    line2 = {};
    if (! d.hasReference)
    {
        line1 = "Add a reference to compare";
        line2 = "Drop a song you like on PureSheen, or click + Add";
    }
    else if (d.referenceProblem.isNotEmpty())
    {
        line1 = d.referenceProblem;
        line2 = "Click it in the list to find the file, or pick another reference";
    }
    else if (! d.reference.valid)
    {
        line1 = d.referencePending ? "Measuring the reference..." : "The reference has no measurement yet";
        if (! d.referencePending)
            line2 = "Select a part with music (a few seconds), or the whole track";
    }
    else if (! d.mix.valid)
    {
        line1 = "Press play in your DAW";
        line2 = "PureSheen measures your master (its own output) as it plays";
    }
}

//==============================================================================
class CompareView : public juce::Component, public juce::TooltipClient
{
public:
    explicit CompareView (const juce::String& name)
    {
        setTitle (name);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    virtual void setData (const RefViewData&, bool snap) = 0;
    virtual ps::Severity attention() const = 0;   // the most important severity shown (tab dots)

    void mouseMove (const juce::MouseEvent& e) override { hoverMoved (e.position); }
    void mouseExit (const juce::MouseEvent&) override { hoverMoved ({ -100.0f, -100.0f }); }

protected:
    virtual void hoverMoved (juce::Point<float>) {}

    // Panel + title row; returns the area below the title.
    juce::Rectangle<float> layoutBase()
    {
        auto inner = getLocalBounds().toFloat().reduced (12.0f, 9.0f);
        titleRow = inner.removeFromTop (18.0f);
        inner.removeFromTop (6.0f);
        return inner;
    }

    juce::Rectangle<float> titleRow;
};

//==============================================================================
class TonalView final : public CompareView
{
public:
    TonalView() : CompareView ("Tonal balance") {}

    void resized() override
    {
        auto inner = layoutBase();
        const float barsH = juce::jlimit (34.0f, 120.0f, inner.getHeight() * 0.3f);
        bandArea = inner.removeFromBottom (14.0f + barsH + 14.0f + 13.0f);
        inner.removeFromBottom (2.0f);
        plot = inner;
        auto b = bandArea;
        namesRow = b.removeFromTop (14.0f);
        valueRow = b.removeFromBottom (13.0f + 14.0f);
        bars = b.reduced (0.0f, 2.0f);
    }

    void setData (const RefViewData& data, bool snap) override
    {
        bool changed = false;
        const bool refOk = data.reference.valid, mixOk = data.mix.valid;
        const float offset = data.comparison.valid() ? data.comparison.normalisationOffsetDb : 0.0f;

        changed |= refOk != refValid || mixOk != mixValid;
        refValid = refOk;
        mixValid = mixOk;

        juce::String l1, l2;
        guidanceFor (data, l1, l2);
        if (! mixOk && ! refOk && data.hasReference && ! data.referencePending)
            l1 = "Press play in your DAW";
        changed |= l1 != line1 || l2 != line2;
        line1 = l1;
        line2 = l2;

        for (int i = 0; i < ps::kCurvePoints; ++i)
        {
            const float tilt = displayTiltDb (ps::curveFrequency (i));
            const float r = data.reference.curveDb[(size_t) i];
            const float m = data.mix.curveDb[(size_t) i];
            changed |= ref[(size_t) i].step (r + tilt, refOk && ps::hasValue (r), snap);
            changed |= mix[(size_t) i].step (m + offset + tilt, mixOk && ps::hasValue (m), snap);
        }

        for (int b = 0; b < ps::kNumBands; ++b)
        {
            const auto& band = data.comparison.bands[(size_t) b];
            changed |= bands[(size_t) b].step (band.levelDiffDb, data.comparison.valid() && band.valid, snap);
            changed |= severity[(size_t) b] != band.severity;
            severity[(size_t) b] = band.severity;
        }

        bool ok = false;
        float c = curveCentre (ref, ok);
        if (! ok)
            c = curveCentre (mix, ok);
        if (ok)
        {
            changed |= centre.step (c, true, snap);

            // Top of the range: room for the highest point of either curve (40 Hz - 16 kHz)
            float highest = c;
            for (int i = 0; i < ps::kCurvePoints; ++i)
            {
                const float hz = ps::curveFrequency (i);
                if (hz < 40.0f || hz > 16000.0f)
                    continue;
                if (ref[(size_t) i].valid)
                    highest = juce::jmax (highest, ref[(size_t) i].target);
                if (mix[(size_t) i].valid)
                    highest = juce::jmax (highest, mix[(size_t) i].target);
            }
            changed |= above.step (juce::jlimit (kAboveCentreDb, kMaxAboveCentreDb, highest + 3.0f - c), true, snap, 0.05f);
        }

        if (changed)
            repaint();
    }

    ps::Severity attention() const override
    {
        auto s = ps::Severity::Good;
        for (int b = 0; b < ps::kNumBands; ++b)
            if (bands[(size_t) b].valid)
                s = worse (s, severity[(size_t) b]);
        return s;
    }

    void paint (juce::Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        paintTitle (g);
        paintPlot (g);
        paintBands (g);
    }

    juce::String getTooltip() override
    {
        const auto pos = getMouseXYRelative().toFloat();
        const int band = bandAt (pos);
        if (band >= 0)
        {
            juce::String tip (bandExplanation (band));
            const auto& b = bands[(size_t) band];
            if (b.valid)
            {
                const float diff = b.target;
                const auto size = std::abs (diff) < ps::kBandMinorDb ? juce::String ("close to the reference")
                                : (std::abs (diff) < ps::kBandMajorDb ? juce::String ("a noticeable difference")
                                                                      : juce::String ("a big difference"));
                tip << "\n\n";
                if (std::abs (diff) < 0.05f)
                    tip << "Your master: about the same as the reference (both at the same loudness)";
                else
                    tip << "Your master: " << abs1 (diff) << " dB " << (diff > 0.0f ? "more" : "less")
                        << " than the reference (both at the same loudness)";
                tip << " - " << size << ".";
            }
            return tip;
        }

        if (titleRow.contains (pos) || plot.contains (pos))
            return "Tonal balance: the average spectrum of your master (mint line) and of the reference (lavender area), "
                   "both at the same loudness. The picture is tilted by +4.5 dB per octave so a typical well-balanced "
                   "song looks roughly flat. Where the line is above the area, your master has more of those "
                   "frequencies.";
        return {};
    }

private:
    void hoverMoved (juce::Point<float> p) override
    {
        const int band = bandAt (p);
        if (band != hoverBand)
        {
            hoverBand = band;
            repaint();
        }
    }

    float xForHz (float hz) const noexcept { return plot.getX() + logPosition (hz) * plot.getWidth(); }
    float aboveDb() const noexcept { return above.valid ? above.shown : kAboveCentreDb; }

    juce::Rectangle<float> bandColumn (int band, juce::Rectangle<float> rowArea) const
    {
        const auto& info = ps::bandInfo (band);
        const float x0 = xForHz (info.lowHz), x1 = xForHz (info.highHz);
        return { x0, rowArea.getY(), x1 - x0, rowArea.getHeight() };
    }

    int bandAt (juce::Point<float> p) const
    {
        if (! (plot.contains (p) || bandArea.contains (p)) || p.x < plot.getX() || p.x > plot.getRight())
            return -1;
        for (int b = 0; b < ps::kNumBands; ++b)
            if (p.x >= xForHz (ps::bandInfo (b).lowHz) && p.x <= xForHz (ps::bandInfo (b).highHz))
                return b;
        return -1;
    }

    float yForDb (float db) const noexcept
    {
        const float top = centre.shown + aboveDb(), bottom = centre.shown - kBelowCentreDb;
        return plot.getY() + (top - db) / (top - bottom) * plot.getHeight();
    }

    // Centre of the displayed range: median of the tilted curve between 60 Hz and 12 kHz (reference preferred).
    static float curveCentre (const std::array<Smoothed, ps::kCurvePoints>& curve, bool& ok)
    {
        std::vector<float> values;
        values.reserve ((size_t) ps::kCurvePoints);
        for (int i = 0; i < ps::kCurvePoints; ++i)
        {
            const float hz = ps::curveFrequency (i);
            if (hz >= 60.0f && hz <= 12000.0f && curve[(size_t) i].valid)
                values.push_back (curve[(size_t) i].target);
        }
        ok = values.size() > 8;
        if (! ok)
            return 0.0f;
        std::nth_element (values.begin(), values.begin() + (std::ptrdiff_t) (values.size() / 2), values.end());
        return values[values.size() / 2];
    }

    juce::Path curvePath (const std::array<Smoothed, ps::kCurvePoints>& curve, bool closedToBottom) const
    {
        juce::Path p;
        bool penDown = false;
        float firstX = 0.0f, lastX = 0.0f;
        const float yMin = plot.getY() - 40.0f, yMax = plot.getBottom() + 40.0f;
        auto close = [&] {
            if (penDown && closedToBottom)
            {
                p.lineTo (lastX, plot.getBottom());
                p.lineTo (firstX, plot.getBottom());
                p.closeSubPath();
            }
        };
        for (int i = 0; i < ps::kCurvePoints; ++i)
        {
            const auto& pt = curve[(size_t) i];
            if (! pt.valid)
            {
                close();
                penDown = false;
                continue;
            }
            const float x = xForHz (ps::curveFrequency (i));
            const float y = juce::jlimit (yMin, yMax, yForDb (pt.shown));
            if (! penDown)
            {
                firstX = x;
                if (closedToBottom)
                {
                    p.startNewSubPath (x, plot.getBottom());
                    p.lineTo (x, y);
                }
                else
                {
                    p.startNewSubPath (x, y);
                }
                penDown = true;
            }
            else
            {
                p.lineTo (x, y);
            }
            lastX = x;
        }
        close();
        return p;
    }

    void paintTitle (juce::Graphics& g) const
    {
        drawSectionTitle (g, "Tonal balance", titleRow.toNearestInt());

        // Legend, right aligned: [line] Master   [area] Reference   (at equal loudness)
        const auto f = font (11.0f, 600), noteFont = font (10.5f);
        const juce::String note ("at equal loudness");
        float x = titleRow.getRight();
        if (titleRow.getWidth() > 400.0f)
        {
            x -= textWidth (noteFont, note);
            g.setFont (noteFont);
            g.setColour (Col::textSubtle);
            g.drawText (note, juce::Rectangle<float> (x, titleRow.getY(), titleRow.getRight() - x, titleRow.getHeight()),
                        juce::Justification::centredLeft, false);
            x -= 14.0f;
        }
        auto legendItem = [&] (const juce::String& text, juce::Colour colour, bool area) {
            const float w = textWidth (f, text);
            x -= w;
            g.setFont (f);
            g.setColour (Col::textDim.brighter (0.2f));
            g.drawText (text, juce::Rectangle<float> (x, titleRow.getY(), w + 2.0f, titleRow.getHeight()), juce::Justification::centredLeft, false);
            x -= 19.0f;
            const auto swatch = juce::Rectangle<float> (x, titleRow.getCentreY() - (area ? 5.0f : 1.25f), 13.0f, area ? 10.0f : 2.5f);
            g.setColour (area ? colour.withAlpha (0.45f) : colour);
            g.fillRoundedRectangle (swatch, area ? 2.5f : 1.25f);
            if (area)
            {
                g.setColour (colour.withAlpha (0.85f));
                g.fillRect (swatch.withHeight (1.5f));
            }
            x -= 12.0f;
        };
        if (titleRow.getWidth() > 230.0f)
        {
            legendItem ("Reference", Col::reference, true);
            legendItem ("Master", Col::output, false);
        }
    }

    void paintPlot (juce::Graphics& g) const
    {
        g.setColour (Col::background.withAlpha (0.5f));
        g.fillRoundedRectangle (plot, 5.0f);

        if (hoverBand >= 0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.035f));
            g.fillRect (bandColumn (hoverBand, plot));
        }

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());
            const float top = centre.shown + aboveDb(), bottom = centre.shown - kBelowCentreDb;
            for (float db = std::floor (bottom / 6.0f) * 6.0f; db <= top; db += 6.0f)
            {
                g.setColour (Col::outline.withAlpha (0.28f));
                g.fillRect (juce::Rectangle<float> (plot.getX(), yForDb (db) - 0.5f, plot.getWidth(), 1.0f));
            }
        }

        // Band separators with frequency labels
        g.setFont (font (10.0f));
        for (int b = 1; b < ps::kNumBands; ++b)
        {
            const float hz = ps::bandInfo (b).lowHz;
            const float x = std::round (xForHz (hz));
            g.setColour (Col::outline.withAlpha (0.75f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getY() + 4.0f, 1.0f, plot.getHeight() - 8.0f));
            g.setColour (Col::textSubtle);
            const juce::String label = hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k" : juce::String ((int) hz);
            g.drawText (label, juce::Rectangle<float> (x + 4.0f, plot.getBottom() - 14.0f, 40.0f, 12.0f), juce::Justification::centredLeft, false);
        }

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());
            if (refValid)
            {
                g.setGradientFill (juce::ColourGradient::vertical (Col::reference.withAlpha (0.34f), plot.getY(),
                                                                   Col::reference.withAlpha (0.04f), plot.getBottom()));
                g.fillPath (curvePath (ref, true));
                g.setColour (Col::reference.withAlpha (0.75f));
                g.strokePath (curvePath (ref, false), juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));
            }
            if (mixValid)
            {
                const auto line = curvePath (mix, false);
                g.setColour (Col::output.withAlpha (0.16f));
                g.strokePath (line, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour (Col::output);
                g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }

        paintGuidance (g, plot, line1, line2);
    }

    void paintBands (juce::Graphics& g) const
    {
        const float zeroY = bars.getCentreY();
        const float dbPx = (bars.getHeight() * 0.5f) / kBarRangeDb;

        // "Close to the reference" zone (+-1.5 dB) and the zero line
        g.setColour (Col::good.withAlpha (0.07f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), zeroY - ps::kBandMinorDb * dbPx, plot.getWidth(), 2.0f * ps::kBandMinorDb * dbPx));
        g.setColour (Col::outline.brighter (0.1f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), zeroY - 0.5f, plot.getWidth(), 1.0f));

        const auto nameFont = font (11.0f, 600), valueFont = font (12.0f, 700), wordFont = font (10.5f, 600);
        for (int b = 0; b < ps::kNumBands; ++b)
        {
            const auto column = bandColumn (b, bandArea);
            const auto& band = bands[(size_t) b];
            const auto colour = severityColour (severity[(size_t) b]);

            if (b == hoverBand)
            {
                g.setColour (juce::Colours::white.withAlpha (0.035f));
                g.fillRoundedRectangle (column.reduced (1.0f, 0.0f), 4.0f);
            }
            if (b > 0)
            {
                g.setColour (Col::outline.withAlpha (0.5f));
                g.fillRect (juce::Rectangle<float> (std::round (column.getX()) - 0.5f, bars.getY(), 1.0f, bars.getHeight()));
            }

            g.setFont (nameFont);
            g.setColour (Col::textDim.brighter (0.1f));
            const auto nameArea = bandColumn (b, namesRow).reduced (2.0f, 0.0f);
            const juce::String name = textWidth (nameFont, ps::bandInfo (b).name) <= nameArea.getWidth()
                                          ? juce::String (ps::bandInfo (b).name) : juce::String (ps::bandInfo (b).shortName);
            fitLine (g, name, nameArea, juce::Justification::centred, 0.75f);

            if (band.valid)
            {
                const float v = juce::jlimit (-kBarRangeDb, kBarRangeDb, band.shown);
                const float barW = juce::jlimit (10.0f, 34.0f, column.getWidth() * 0.42f);
                const float y1 = zeroY - v * dbPx;
                auto bar = juce::Rectangle<float> (column.getCentreX() - barW * 0.5f, juce::jmin (zeroY, y1), barW, std::abs (y1 - zeroY));
                if (bar.getHeight() < 2.0f)
                    bar = bar.withSizeKeepingCentre (barW, 2.0f).withY (zeroY - 1.0f);
                g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (v >= 0.0f ? 0.95f : 0.55f), bar.getY(),
                                                                   colour.withAlpha (v >= 0.0f ? 0.55f : 0.95f), bar.getBottom()));
                g.fillRoundedRectangle (bar, 3.0f);
                if (std::abs (band.shown) > kBarRangeDb)
                {
                    const float tipY = v > 0.0f ? bar.getY() - 5.0f : bar.getBottom() + 5.0f;
                    const float baseY = v > 0.0f ? bar.getY() - 1.0f : bar.getBottom() + 1.0f;
                    juce::Path arrow;
                    arrow.addTriangle (column.getCentreX() - 5.0f, baseY, column.getCentreX() + 5.0f, baseY, column.getCentreX(), tipY);
                    g.setColour (colour);
                    g.fillPath (arrow);
                }
            }

            auto valueArea = bandColumn (b, valueRow).reduced (1.0f, 0.0f);
            g.setFont (valueFont);
            g.setColour (band.valid ? colour.brighter (0.1f) : Col::textFaint);
            fitLine (g, band.valid ? signedNum (band.target, 1) + " dB" : juce::String ("--"), valueArea.removeFromTop (14.0f),
                     juce::Justification::centred, 0.7f);
            if (band.valid)
            {
                g.setFont (wordFont);
                g.setColour (colour.withAlpha (0.85f));
                fitLine (g, bandWord (b, band.target), valueArea, juce::Justification::centred, 0.7f);
            }
        }
    }

    juce::Rectangle<float> plot, bandArea, namesRow, bars, valueRow;
    std::array<Smoothed, ps::kCurvePoints> ref {}, mix {};
    std::array<Smoothed, ps::kNumBands> bands {};
    std::array<ps::Severity, ps::kNumBands> severity {};
    Smoothed centre, above;
    bool refValid = false, mixValid = false;
    juce::String line1, line2;
    int hoverBand = -1;
};

//==============================================================================
// Loudness & dynamics: five rows, master vs reference, with a one-line verdict.
struct Message
{
    juce::String full, compact;   // compact: used when the full text does not fit
    ps::Severity severity = ps::Severity::Info;
};

struct RowSpec
{
    const char* name;
    const char* unit;
    float barMin, barMax;
    const char* tooltip;
};

constexpr int kNumLoudRows = 5;
const RowSpec kLoudRows[kNumLoudRows] = {
    { "Integrated", "LUFS", -30.0f, 0.0f,
      "Integrated loudness (LUFS): the average loudness of everything that played since the last reset, as your ears "
      "hear it. Streaming services play songs at about -14 LUFS; club and pop masters are often -9 to -6 LUFS. "
      "PureSheen measures its own output (after every module) and the reference's selected part." },
    { "Short-term", "LUFS", -30.0f, 0.0f,
      "Short-term loudness (LUFS): how loud the last 3 seconds were. The reference value is measured as it plays (at "
      "its own level), so play the same part of both songs to compare them." },
    { "True peak", "dBTP", -12.0f, 3.0f,
      "True peak (dBTP): the highest point of the waveform since the last reset, including peaks between samples that "
      "appear when the audio is converted. Keep a finished master below -1 dBTP so it does not distort on streaming "
      "services or as MP3." },
    { "Dynamics", "PLR", 0.0f, 20.0f,
      "Dynamics (PLR, peak-to-loudness ratio, in dB): the highest sample peak minus the integrated loudness - how far "
      "the peaks rise above the average level. Loud, heavily limited masters are around 6 - 9 dB; dynamic ones 12 dB "
      "or more." },
    { "Loudness range", "LRA", 0.0f, 20.0f,
      "Loudness range (LRA, in LU): how much the loudness changes between the quieter and louder parts. Pop and EDM "
      "masters are often 3 - 7 LU; a much higher value means big jumps between parts." },
};

juce::String loudValueText (int row, float v)
{
    if (! ps::hasValue (v))
        return "--";
    return row == 2 ? signedNum (v, 1) : num (v, 1);
}

// The one-liners follow the same thresholds as the hints of core/RefCompare.
Message integratedMessage (float m, float r, bool hasReference)
{
    if (! ps::hasValue (m))
        return { "Press play in your DAW to measure your master", "Press play to measure it", ps::Severity::Info };
    if (! ps::hasValue (r))
        return hasReference ? Message { "Waiting for the reference measurement", "Measuring the reference", ps::Severity::Info }
                            : Message { "Add a reference to compare", "Add a reference", ps::Severity::Info };
    const float d = m - r;
    if (std::abs (d) <= ps::kLoudnessDiffLu)
        return { "About as loud as the reference", "As loud as the reference", ps::Severity::Good };
    if (d < 0.0f)
        return { abs1 (d) + " LU quieter than the reference - more Maximizer gain closes the gap",
                 abs1 (d) + " LU quieter than the reference", ps::Severity::Info };
    return { abs1 (d) + " LU louder than the reference - check the limiter is not squashing it",
             abs1 (d) + " LU louder than the reference", d > ps::kLouderMajorLu ? ps::Severity::Major : ps::Severity::Minor };
}

Message shortTermMessage (float m, float r, bool hasReference)
{
    if (! ps::hasValue (m))
        return { "Loudness of the last 3 seconds while your DAW plays", "Last 3 s while playing", ps::Severity::Info };
    if (! hasReference)
        return { "Loudness of the last 3 seconds", "Last 3 seconds", ps::Severity::Info };
    if (! ps::hasValue (r))
        return { "The reference's value shows while it plays", "Shows while the reference plays", ps::Severity::Info };
    const float d = m - r;
    if (std::abs (d) <= 1.5f)
        return { "Right now about as loud as this part of the reference", "Right now as loud as the reference", ps::Severity::Good };
    return { "Right now " + abs1 (d) + " LU " + (d < 0.0f ? "quieter" : "louder") + " than this part of the reference",
             abs1 (d) + " LU " + (d < 0.0f ? "quieter" : "louder") + " right now", ps::Severity::Info };
}

Message truePeakMessage (float m, bool referenceKnown, float r)
{
    if (! ps::hasValue (m))
        return { "Highest peak so far - play the loudest part", "Play the loudest part", ps::Severity::Info };
    if (m > ps::kTruePeakCeilingDbtp)
    {
        if (referenceKnown && ps::hasValue (r) && r > ps::kTruePeakCeilingDbtp && m <= r)
            return { "Above -1 dBTP - the reference peaks this high too", "Above -1 dBTP, like the reference", ps::Severity::Minor };
        return { "Above -1 dBTP - can distort on streaming and as MP3", "Above -1 dBTP - can distort", ps::Severity::Major };
    }
    if (m > -3.0f)
        return { "Below -1 dBTP - OK for streaming", "Below -1 dBTP", ps::Severity::Good };
    return { "Plenty of headroom", "Plenty of headroom", ps::Severity::Good };
}

Message dynamicsMessage (float m, float r)
{
    if (! ps::hasValue (m))
        return { "Press play to measure the dynamics", "Press play to measure it", ps::Severity::Info };
    if (! ps::hasValue (r))
    {
        if (m < 8.0f)
            return { "Heavily limited - the peaks barely rise above the average", "Heavily limited", ps::Severity::Minor };
        return m < 12.0f ? Message { "Typical for a loud master", "Typical for a loud master", ps::Severity::Info }
                         : Message { "Dynamic - the peaks rise well above the average", "Dynamic", ps::Severity::Info };
    }
    const float d = m - r;
    if (std::abs (d) <= ps::kPlrDiffDb)
        return { "Similar dynamics to the reference", "Similar to the reference", ps::Severity::Good };
    if (d > 0.0f)
        return { abs1 (d) + " dB more dynamic than the reference - may sound less dense",
                 abs1 (d) + " dB more dynamic", ps::Severity::Minor };
    return { abs1 (d) + " dB more squashed than the reference - ease off the limiter",
             abs1 (d) + " dB more squashed", d < -ps::kPlrMajorDb ? ps::Severity::Major : ps::Severity::Minor };
}

Message rangeMessage (bool mixMeasured, float m, bool refMeasured, float r)
{
    if (! mixMeasured)
        return { "Needs a minute or so of playback to measure", "Needs about a minute", ps::Severity::Info };
    if (! refMeasured)
        return { "How much the loudness moves over the song", "Movement over the song", ps::Severity::Info };
    const float d = m - r;
    if (std::abs (d) <= 2.0f)
        return { "Similar movement between quiet and loud parts", "Similar to the reference", ps::Severity::Good };
    if (d > 0.0f)
        return { "More contrast between quiet and loud parts than the reference", "More contrast", ps::Severity::Info };
    return { "Flatter than the reference - less contrast between parts", "Flatter than the reference", ps::Severity::Info };
}

class LoudnessView final : public CompareView
{
public:
    LoudnessView() : CompareView ("Loudness and dynamics") {}

    void resized() override
    {
        rowsArea = layoutBase();
        const float rowH = rowsArea.getHeight() / (float) kNumLoudRows;
        showMessages = rowH >= 33.0f;
        const float w = rowsArea.getWidth();
        nameW = juce::jlimit (92.0f, 170.0f, w * 0.3f);
        valueW = juce::jlimit (46.0f, 64.0f, w * 0.15f);
    }

    void setData (const RefViewData& data, bool snap) override
    {
        const auto& m = data.mix;
        const auto& r = data.reference;
        auto rangeMeasured = [] (const ps::TrackProfile& p) {
            return ps::hasValue (p.integratedLufs) && ps::hasValue (p.shortTermLufs) && (p.loudnessRangeLu > 0.0f || p.analysedSeconds >= 10.0);
        };
        const bool mixRange = rangeMeasured (m), refRange = r.valid && rangeMeasured (r);
        const float refInt = r.valid ? r.integratedLufs : ps::kNoValue;
        const float refShort = data.hasReference ? data.referenceShortTermLufs : ps::kNoValue;

        struct Next { float mix, ref; Message message; };
        const Next next[kNumLoudRows] {
            { m.integratedLufs, refInt, integratedMessage (m.integratedLufs, refInt, data.hasReference) },
            { m.shortTermLufs, refShort, shortTermMessage (m.shortTermLufs, refShort, data.hasReference) },
            { m.truePeakDb, r.valid ? r.truePeakDb : ps::kNoValue, truePeakMessage (m.truePeakDb, r.valid, r.truePeakDb) },
            { m.plrDb, r.valid ? r.plrDb : ps::kNoValue, dynamicsMessage (m.plrDb, r.valid ? r.plrDb : ps::kNoValue) },
            { mixRange ? m.loudnessRangeLu : ps::kNoValue, refRange ? r.loudnessRangeLu : ps::kNoValue,
              rangeMessage (mixRange, m.loudnessRangeLu, refRange, r.loudnessRangeLu) },
        };

        bool changed = false;
        for (int i = 0; i < kNumLoudRows; ++i)
        {
            auto& row = rows[(size_t) i];
            changed |= row.mix.step (next[i].mix, ps::hasValue (next[i].mix), snap, 0.02f);
            changed |= row.ref.step (next[i].ref, ps::hasValue (next[i].ref), snap, 0.02f);
            if (row.message.full != next[i].message.full || row.message.severity != next[i].message.severity)
            {
                row.message = next[i].message;
                changed = true;
            }
        }
        if (changed)
            repaint();
    }

    ps::Severity attention() const override
    {
        auto s = ps::Severity::Good;
        for (const auto& row : rows)
            s = worse (s, row.message.severity);
        return s;
    }

    void paint (juce::Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        drawSectionTitle (g, "Loudness & dynamics", titleRow.toNearestInt());
        const auto f = font (10.0f, 700).withExtraKerningFactor (0.08f);
        g.setFont (f);
        g.setColour (Col::output);
        g.drawText ("MASTER", juce::Rectangle<float> (mixX() - 10.0f, titleRow.getY(), valueW + 10.0f, titleRow.getHeight()),
                    juce::Justification::centredRight, false);
        g.setColour (Col::reference);
        g.drawText ("REF", juce::Rectangle<float> (refX(), titleRow.getY(), valueW, titleRow.getHeight()), juce::Justification::centredRight, false);

        for (int i = 0; i < kNumLoudRows; ++i)
            paintRow (g, i);
    }

    juce::String getTooltip() override
    {
        const auto pos = getMouseXYRelative().toFloat();
        const int row = rowAt (pos);
        if (row < 0)
            return titleRow.contains (pos) ? "Loudness and dynamics of your master (mint) next to the reference (lavender)."
                                           : juce::String();
        juce::String tip (kLoudRows[row].tooltip);
        const auto& r = rows[(size_t) row];
        tip << "\n\nMaster " << loudValueText (row, r.mix.valid ? r.mix.target : ps::kNoValue) << ", reference "
            << loudValueText (row, r.ref.valid ? r.ref.target : ps::kNoValue) << ": " << r.message.full << ".";
        return tip;
    }

private:
    struct Row
    {
        Smoothed mix, ref;
        Message message;
    };

    void hoverMoved (juce::Point<float> p) override
    {
        const int row = rowAt (p);
        if (row != hoverRow)
        {
            hoverRow = row;
            repaint();
        }
    }

    juce::Rectangle<float> rowBounds (int i) const
    {
        const float rowH = rowsArea.getHeight() / (float) kNumLoudRows;
        return { rowsArea.getX(), rowsArea.getY() + rowH * (float) i, rowsArea.getWidth(), rowH };
    }

    int rowAt (juce::Point<float> p) const
    {
        for (int i = 0; i < kNumLoudRows; ++i)
            if (rowBounds (i).contains (p))
                return i;
        return -1;
    }

    float mixX() const noexcept  { return rowsArea.getX() + nameW; }
    float refX() const noexcept  { return mixX() + valueW + 4.0f; }
    float barsX() const noexcept { return refX() + valueW + 14.0f; }

    void paintRow (juce::Graphics& g, int i) const
    {
        const auto& spec = kLoudRows[i];
        const auto& row = rows[(size_t) i];
        auto r = rowBounds (i);

        if (i == hoverRow)
        {
            g.setColour (juce::Colours::white.withAlpha (0.03f));
            g.fillRoundedRectangle (r.expanded (6.0f, 0.0f), 5.0f);
        }
        if (i > 0)
        {
            g.setColour (Col::outline.withAlpha (0.45f));
            g.fillRect (r.withHeight (1.0f));
        }

        const float lineH = 19.0f;
        auto content = r.withSizeKeepingCentre (r.getWidth(), showMessages ? lineH + 14.0f : lineH);
        auto top = content.removeFromTop (lineH);

        {
            auto nameArea = top.withWidth (nameW - 6.0f);
            const auto nameFont = font (12.5f, 600), unitFont = font (10.5f, 600);
            const float nw = juce::jmin (nameArea.getWidth(), textWidth (nameFont, spec.name) + 1.0f);
            g.setFont (nameFont);
            g.setColour (Col::text);
            fitLine (g, spec.name, nameArea.removeFromLeft (nw), juce::Justification::centredLeft, 0.8f);
            if (nameArea.getWidth() > textWidth (unitFont, spec.unit) + 6.0f)
            {
                g.setFont (unitFont);
                g.setColour (Col::textSubtle);
                g.drawText (spec.unit, nameArea.withTrimmedLeft (5.0f).translated (0.0f, 0.5f), juce::Justification::centredLeft, false);
            }
        }

        g.setFont (font (14.5f, 700));
        g.setColour (row.mix.valid ? Col::output.brighter (0.1f) : Col::textFaint);
        g.drawText (loudValueText (i, row.mix.valid ? row.mix.shown : ps::kNoValue),
                    juce::Rectangle<float> (mixX(), top.getY(), valueW, top.getHeight()), juce::Justification::centredRight, false);
        g.setColour (row.ref.valid ? Col::reference.brighter (0.1f) : Col::textFaint);
        g.drawText (loudValueText (i, row.ref.valid ? row.ref.shown : ps::kNoValue),
                    juce::Rectangle<float> (refX(), top.getY(), valueW, top.getHeight()), juce::Justification::centredRight, false);

        // Comparison bars (master above, reference below)
        const float barH = 4.0f, barGap = 2.0f;
        const auto barsArea = juce::Rectangle<float> (barsX(), top.getCentreY() - barH - barGap * 0.5f, top.getRight() - barsX(), 2.0f * barH + barGap);
        if (barsArea.getWidth() > 30.0f)
        {
            auto drawBar = [&] (const Smoothed& v, juce::Colour colour, float y) {
                const auto track = juce::Rectangle<float> (barsArea.getX(), y, barsArea.getWidth(), barH);
                g.setColour (Col::background.withAlpha (0.7f));
                g.fillRoundedRectangle (track, barH * 0.5f);
                if (! v.valid)
                    return;
                const float norm = juce::jlimit (0.0f, 1.0f, (v.shown - spec.barMin) / (spec.barMax - spec.barMin));
                g.setColour (colour);
                g.fillRoundedRectangle (track.withWidth (juce::jmax (barH, track.getWidth() * norm)), barH * 0.5f);
            };
            drawBar (row.mix, Col::output, barsArea.getY());
            drawBar (row.ref, Col::reference.withAlpha (0.9f), barsArea.getY() + barH + barGap);
            if (i == 2)
            {
                const float x = barsArea.getX() + barsArea.getWidth() * (ps::kTruePeakCeilingDbtp - spec.barMin) / (spec.barMax - spec.barMin);
                g.setColour (Col::major.withAlpha (0.8f));
                g.fillRect (juce::Rectangle<float> (x - 0.5f, barsArea.getY() - 3.0f, 1.0f, barsArea.getHeight() + 6.0f));
            }
        }

        if (showMessages)
        {
            auto line = content;
            const auto colour = severityColour (row.message.severity);
            g.setColour (colour);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ line.getX() + 3.0f, line.getCentreY() }));
            line.removeFromLeft (12.0f);
            const auto f = font (11.0f);
            const auto& text = textWidth (f, row.message.full) <= line.getWidth() ? row.message.full : row.message.compact;
            g.setFont (f);
            g.setColour (row.message.severity == ps::Severity::Good ? Col::textDim : blend (Col::textDim, colour, 0.35f).brighter (0.1f));
            fitLine (g, text, line, juce::Justification::centredLeft, 0.85f);
        }
    }

    juce::Rectangle<float> rowsArea;
    float nameW = 110.0f, valueW = 56.0f;
    bool showMessages = true;
    std::array<Row, kNumLoudRows> rows {};
    int hoverRow = -1;
};

//==============================================================================
class StereoView final : public CompareView
{
public:
    StereoView() : CompareView ("Stereo image") {}

    void resized() override
    {
        auto inner = layoutBase();
        const bool roomy = inner.getHeight() >= 150.0f;
        correlationArea = inner.removeFromBottom (roomy ? 42.0f : 30.0f);
        showScaleLabels = roomy;
        inner.removeFromBottom (6.0f);
        showSummary = inner.getHeight() >= 110.0f;
        summaryRow = showSummary ? inner.removeFromBottom (16.0f) : juce::Rectangle<float>();
        if (showSummary)
            inner.removeFromBottom (4.0f);
        chart = inner;
        chartNames = chart.removeFromBottom (14.0f);
        chart.removeFromBottom (3.0f);
        chartGutter = chart.removeFromLeft (36.0f);
        chartNames.removeFromLeft (36.0f);
        layoutBadge();
    }

    void setData (const RefViewData& data, bool snap) override
    {
        bool changed = false;
        const bool mixOk = data.mix.valid, refOk = data.reference.valid;
        for (int b = 0; b < ps::kNumBands; ++b)
        {
            changed |= mixWidth[(size_t) b].step (data.mix.bands[(size_t) b].widthPct, mixOk, snap, 0.2f);
            changed |= refWidth[(size_t) b].step (data.reference.bands[(size_t) b].widthPct, refOk, snap, 0.2f);
        }
        changed |= mixCorr.step (data.mix.correlation, mixOk, snap, 0.002f);
        changed |= refCorr.step (data.reference.correlation, refOk, snap, 0.002f);
        changed |= mixOverall.step (data.mix.widthPct, mixOk, snap, 0.2f);
        changed |= refOverall.step (data.reference.widthPct, refOk, snap, 0.2f);
        mixLowEnd = data.mix.lowEndCorrelation;

        // Bass mono badge: OK >= 0.8, a bit wide >= 0.5, else too wide
        juce::String text = "Bass mono: --";
        auto sev = ps::Severity::Info;
        bool known = false;
        if (mixOk)
        {
            known = true;
            if (mixLowEnd >= ps::kLowEndCorrMinor)      { text = "Bass mono: OK"; sev = ps::Severity::Good; }
            else if (mixLowEnd >= ps::kLowEndCorrMajor) { text = "Bass a bit wide"; sev = ps::Severity::Minor; }
            else                                        { text = "Bass too wide"; sev = ps::Severity::Major; }
        }
        if (text != badgeText || known != badgeKnown)
        {
            badgeText = text;
            badgeSeverity = sev;
            badgeKnown = known;
            layoutBadge();
            changed = true;
        }

        juce::String l1, l2;
        guidanceFor (data, l1, l2);
        if (mixOk && ! refOk)
        {
            l1 = {};   // the master's bars and the badge are useful alone
            l2 = {};
        }
        changed |= l1 != line1 || l2 != line2;
        line1 = l1;
        line2 = l2;

        if (changed)
            repaint();
    }

    ps::Severity attention() const override { return badgeKnown ? badgeSeverity : ps::Severity::Good; }

    void paint (juce::Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        drawSectionTitle (g, "Stereo image", titleRow.toNearestInt());
        paintBadge (g);
        paintChart (g);
        paintSummary (g);
        paintCorrelation (g);
        if (! mixWidth[0].valid && ! refWidth[0].valid)
            paintGuidance (g, chart, line1, line2);
    }

    juce::String getTooltip() override
    {
        const auto p = getMouseXYRelative().toFloat();
        if (badgeArea.contains (p))
        {
            juce::String tip ("Bass mono check: below 120 Hz almost every released track is mono, so the bass keeps its "
                              "power on mono systems, phones and in clubs. 'Too wide' means your bass would lose power "
                              "when summed to mono - narrow the lows (the Imager's lowest band, or a bass mono setting).");
            if (badgeKnown)
                tip << "\n\nYour master's low-end correlation: " << signedNum (mixLowEnd, 2) << " (+1 = perfectly mono).";
            return tip;
        }
        if (correlationArea.contains (p))
            return "Phase correlation: +1 = mono, 0 = very wide, below 0 = left and right cancel each other when played "
                   "in mono (phone speakers, clubs). Most masters sit between +0.2 and +0.8. The mint marker is your "
                   "master, the lavender marker the reference.";
        if (chart.withBottom (chartNames.getBottom()).withLeft (chartGutter.getX()).contains (p))
        {
            juce::String tip ("Stereo width per band (mint = your master, lavender = the reference): how much of each "
                              "range is spread left / right instead of in the middle. 0% = mono, 100% = as wide as two "
                              "unrelated channels. Sub and bass should be close to mono; highs are usually the widest.");
            const int band = bandAt (p);
            if (band >= 0)
            {
                const auto& m = mixWidth[(size_t) band];
                const auto& r = refWidth[(size_t) band];
                tip << "\n\n" << ps::bandInfo (band).name << ": master "
                    << (m.valid ? juce::String (juce::roundToInt (m.target)) + "%" : juce::String ("--")) << ", reference "
                    << (r.valid ? juce::String (juce::roundToInt (r.target)) + "%" : juce::String ("--")) << ".";
            }
            return tip;
        }
        return {};
    }

private:
    void hoverMoved (juce::Point<float> p) override
    {
        const int band = chart.withBottom (chartNames.getBottom()).contains (p) ? bandAt (p) : -1;
        if (band != hoverBand)
        {
            hoverBand = band;
            repaint();
        }
    }

    void layoutBadge()
    {
        const float w = textWidth (font (11.0f, 600), badgeText) + 30.0f;
        badgeArea = juce::Rectangle<float> (titleRow.getRight() - w, titleRow.getCentreY() - 10.0f, w, 20.0f);
    }

    juce::Rectangle<float> groupBounds (int band) const
    {
        const float gw = chart.getWidth() / (float) ps::kNumBands;
        return { chart.getX() + gw * (float) band, chart.getY(), gw, chart.getHeight() };
    }

    int bandAt (juce::Point<float> p) const
    {
        for (int b = 0; b < ps::kNumBands; ++b)
            if (p.x >= groupBounds (b).getX() && p.x < groupBounds (b).getRight())
                return b;
        return -1;
    }

    void paintBadge (juce::Graphics& g) const
    {
        const auto colour = badgeKnown ? severityColour (badgeSeverity) : Col::textFaint;
        const auto pill = badgeArea;
        g.setColour (colour.withAlpha (badgeKnown ? 0.16f : 0.08f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (colour.withAlpha (badgeKnown ? 0.65f : 0.4f));
        g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);

        const auto c = juce::Point<float> (pill.getX() + 12.0f, pill.getCentreY());
        g.setColour (colour.brighter (0.2f));
        if (badgeKnown && badgeSeverity == ps::Severity::Good)
        {
            juce::Path tick;
            tick.startNewSubPath (c.x - 3.5f, c.y + 0.2f);
            tick.lineTo (c.x - 1.0f, c.y + 2.8f);
            tick.lineTo (c.x + 3.8f, c.y - 2.8f);
            g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (badgeKnown)
        {
            g.setFont (font (11.5f, 700));
            g.drawText ("!", juce::Rectangle<float> (10.0f, 14.0f).withCentre (c), juce::Justification::centred, false);
        }
        else
        {
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (c));
        }
        g.setFont (font (11.0f, 600));
        g.setColour (badgeKnown ? colour.brighter (0.25f) : Col::textDim);
        g.drawText (badgeText, pill.withTrimmedLeft (21.0f), juce::Justification::centredLeft, false);
    }

    void paintChart (juce::Graphics& g) const
    {
        const float yZero = chart.getBottom();
        const float pxPerPct = chart.getHeight() / kWidthScalePct;
        for (const float pct : { 0.0f, 50.0f, 100.0f })
        {
            g.setColour (Col::outline.withAlpha (pct == 0.0f ? 0.9f : 0.35f));
            g.fillRect (juce::Rectangle<float> (chart.getX(), yZero - pct * pxPerPct - 0.5f, chart.getWidth(), 1.0f));
        }
        g.setFont (font (10.0f));
        g.setColour (Col::textSubtle);
        g.drawText ("wide", juce::Rectangle<float> (chartGutter.getX(), chart.getY() - 6.0f, 34.0f, 12.0f), juce::Justification::centredLeft, false);
        g.drawText ("mono", juce::Rectangle<float> (chartGutter.getX(), yZero - 12.0f, 34.0f, 12.0f), juce::Justification::centredLeft, false);

        for (int b = 0; b < ps::kNumBands; ++b)
        {
            const auto group = groupBounds (b);
            if (b == hoverBand)
            {
                g.setColour (juce::Colours::white.withAlpha (0.035f));
                g.fillRoundedRectangle (group.withBottom (chartNames.getBottom()).reduced (2.0f, -2.0f), 4.0f);
            }
            const float barW = juce::jlimit (5.0f, 13.0f, group.getWidth() * 0.22f);
            const float gap = 3.0f;
            const float x0 = group.getCentreX() - barW - gap * 0.5f;
            auto drawBar = [&] (const Smoothed& v, juce::Colour colour, float x) {
                if (! v.valid)
                {
                    g.setColour (Col::textFaint.withAlpha (0.4f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (x, yZero - 2.0f, barW, 2.0f), 1.0f);
                    return;
                }
                const float h = juce::jmax (2.0f, juce::jmin (v.shown, kWidthScalePct) * pxPerPct);
                const auto bar = juce::Rectangle<float> (x, yZero - h, barW, h);
                g.setGradientFill (juce::ColourGradient::vertical (colour, bar.getY(), colour.withAlpha (0.55f), bar.getBottom()));
                g.fillRoundedRectangle (bar, juce::jmin (3.0f, barW * 0.4f));
                if (v.shown > kWidthScalePct)
                {
                    juce::Path arrow;
                    arrow.addTriangle (x - 1.0f, bar.getY() - 1.0f, x + barW + 1.0f, bar.getY() - 1.0f, x + barW * 0.5f, bar.getY() - 5.0f);
                    g.setColour (colour);
                    g.fillPath (arrow);
                }
            };
            drawBar (mixWidth[(size_t) b], Col::output, x0);
            drawBar (refWidth[(size_t) b], Col::reference, x0 + barW + gap);
            g.setFont (font (10.5f, 600));
            g.setColour (Col::textDim);
            fitLine (g, ps::bandInfo (b).shortName, group.withY (chartNames.getY()).withHeight (chartNames.getHeight()),
                     juce::Justification::centred, 0.7f);
        }
    }

    void paintSummary (juce::Graphics& g) const
    {
        if (! showSummary || ! mixOverall.valid || ! refOverall.valid)
            return;
        const float d = mixOverall.target - refOverall.target;
        juce::String text;
        auto sev = ps::Severity::Good;
        if (std::abs (d) < 10.0f)
            text = "Overall about as wide as the reference";
        else
        {
            text = juce::String ("Overall ") + (d < 0.0f ? "narrower" : "wider") + " than the reference ("
                   + juce::String (juce::roundToInt (mixOverall.target)) + "% vs " + juce::String (juce::roundToInt (refOverall.target)) + "%)";
            sev = std::abs (d) >= 25.0f ? ps::Severity::Minor : ps::Severity::Info;
        }
        auto r = summaryRow;
        g.setColour (severityColour (sev));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ r.getX() + 3.0f, r.getCentreY() }));
        r.removeFromLeft (12.0f);
        g.setFont (font (11.0f));
        g.setColour (Col::textDim);
        fitLine (g, text, r, juce::Justification::centredLeft, 0.85f);
    }

    void paintCorrelation (juce::Graphics& g) const
    {
        auto r = correlationArea;
        auto labelRow = r.removeFromTop (15.0f);
        g.setFont (font (11.5f, 600));
        g.setColour (Col::textDim.brighter (0.1f));
        g.drawText ("Phase correlation", labelRow, juce::Justification::centredLeft, false);
        {
            const auto vf = font (11.5f, 700);
            const auto refText = "Ref " + (refCorr.valid ? signedNum (refCorr.target, 2) : juce::String ("--"));
            const auto mixText = "Master " + (mixCorr.valid ? signedNum (mixCorr.target, 2) : juce::String ("--"));
            float x = labelRow.getRight() - textWidth (vf, refText);
            g.setFont (vf);
            g.setColour (refCorr.valid ? Col::reference.brighter (0.1f) : Col::textFaint);
            g.drawText (refText, labelRow.withLeft (x), juce::Justification::centredLeft, false);
            x -= textWidth (vf, mixText) + 12.0f;
            if (x > labelRow.getX() + textWidth (font (11.5f, 600), "Phase correlation") + 8.0f)
            {
                g.setColour (mixCorr.valid ? Col::output.brighter (0.1f) : Col::textFaint);
                g.drawText (mixText, labelRow.withLeft (x), juce::Justification::centredLeft, false);
            }
        }

        r.removeFromTop (3.0f);
        const auto track = juce::Rectangle<float> (r.getX() + 6.0f, r.getY() + 5.0f, r.getWidth() - 12.0f, 6.0f);
        juce::ColourGradient grad (Col::major.withAlpha (0.55f), track.getX(), 0.0f, Col::good.withAlpha (0.65f), track.getRight(), 0.0f, false);
        grad.addColour (0.5, Col::minor.withAlpha (0.45f));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (track, 3.0f);
        auto xFor = [&track] (float c) { return track.getX() + (juce::jlimit (-1.0f, 1.0f, c) + 1.0f) * 0.5f * track.getWidth(); };
        g.setColour (Col::panel);
        g.fillRect (juce::Rectangle<float> (xFor (0.0f) - 0.5f, track.getY(), 1.0f, track.getHeight()));

        if (showScaleLabels)
        {
            g.setFont (font (10.0f));
            g.setColour (Col::textSubtle);
            const float ly = track.getBottom() + 6.0f;
            g.drawText ("-1  out of phase", juce::Rectangle<float> (track.getX() - 2.0f, ly, 120.0f, 12.0f), juce::Justification::centredLeft, false);
            g.drawText ("0", juce::Rectangle<float> (xFor (0.0f) - 10.0f, ly, 20.0f, 12.0f), juce::Justification::centred, false);
            g.drawText ("mono  +1", juce::Rectangle<float> (track.getRight() - 118.0f, ly, 120.0f, 12.0f), juce::Justification::centredRight, false);
        }

        auto marker = [&] (const Smoothed& v, juce::Colour colour, bool above) {
            if (! v.valid)
                return;
            const float x = xFor (v.shown);
            juce::Path p;
            if (above)
                p.addTriangle (x - 5.0f, track.getY() - 5.5f, x + 5.0f, track.getY() - 5.5f, x, track.getY() + 2.0f);
            else
                p.addTriangle (x - 5.0f, track.getBottom() + 5.5f, x + 5.0f, track.getBottom() + 5.5f, x, track.getBottom() - 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.strokePath (p, juce::PathStrokeType (2.0f));
            g.setColour (colour);
            g.fillPath (p);
        };
        marker (mixCorr, Col::output, true);
        marker (refCorr, Col::reference, false);
    }

    juce::Rectangle<float> chart, chartGutter, chartNames, summaryRow, correlationArea, badgeArea;
    bool showSummary = false, showScaleLabels = true;
    std::array<Smoothed, ps::kNumBands> mixWidth {}, refWidth {};
    Smoothed mixCorr, refCorr, mixOverall, refOverall;
    float mixLowEnd = 1.0f;
    juce::String badgeText { "Bass mono: --" }, line1, line2;
    ps::Severity badgeSeverity = ps::Severity::Info;
    bool badgeKnown = false;
    int hoverBand = -1;
};

//==============================================================================
// What to try: the first (most important) hint as a large card, the others as cards (one or two columns) when they
// all fit, else as one-line rows with the advice in their tooltip. Scrolls with the mouse wheel.
struct HintText
{
    ps::Severity severity = ps::Severity::Info;
    ps::Topic topic = ps::Topic::Setup;
    int band = -1;
    float amount = 0.0f;
    juce::String title, detail;

    bool operator== (const HintText& o) const
    {
        return severity == o.severity && topic == o.topic && band == o.band && title == o.title && detail == o.detail;
    }
};

// Checks of the master alone (true peak, low-end mono, the broadband phase check), not differences.
bool isMasterOnlyCheck (const HintText& h)
{
    return h.topic == ps::Topic::TruePeak || h.topic == ps::Topic::LowEnd
           || (h.topic == ps::Topic::Stereo && h.band < 0 && h.amount > -1.01f);
}

juce::String severityTag (const HintText& h)
{
    switch (h.severity)
    {
        case ps::Severity::Good:  return "GOOD";
        case ps::Severity::Info:  return "TIP";
        case ps::Severity::Minor: return "WORTH A LOOK";
        case ps::Severity::Major: return isMasterOnlyCheck (h) ? "CHECK THIS" : "BIG DIFFERENCE";
    }
    return {};
}

// The core's "play your track" setup hint title (the other setup hints are about the reference), taken from the
// core itself so the two never drift apart.
const std::string& masterSetupHintTitle()
{
    static const std::string title = [] {
        ps::TrackProfile validReference;
        validReference.valid = true;
        for (const auto& h : ps::compareProfiles (ps::TrackProfile(), validReference).hints)
            if (h.topic == ps::Topic::Setup)
                return h.title;
        return std::string();
    }();
    return title;
}

std::vector<HintText> hintsFor (const RefViewData& d)
{
    const auto fallback = d.comparison.hints.empty() ? ps::compareProfiles (d.mix, d.reference) : ps::Comparison();
    const auto& source = d.comparison.hints.empty() ? fallback.hints : d.comparison.hints;
    const bool pending = d.referencePending && ! d.reference.valid;
    const bool problem = d.referenceProblem.isNotEmpty() && ! d.reference.valid;

    std::vector<HintText> out;
    bool replaced = false;
    for (const auto& h : source)
    {
        if ((pending || problem) && h.topic == ps::Topic::Setup && h.title != masterSetupHintTitle())
        {
            if (! replaced)
                out.push_back (problem ? HintText { ps::Severity::Minor, ps::Topic::Setup, -1, 0.0f, d.referenceProblem,
                                                    "Click it in the reference list to find the file (its loops are kept), "
                                                    "or select another reference." }
                                       : HintText { ps::Severity::Info, ps::Topic::Setup, -1, 0.0f, "Measuring the reference",
                                                    "Loading and measuring the reference takes a few seconds." });
            replaced = true;
            continue;
        }
        out.push_back ({ h.severity, h.topic, h.band, h.amount, juce::String (h.title), juce::String (h.detail) });
    }
    return out;
}

class HintsView final : public CompareView
{
public:
    HintsView() : CompareView ("What to try") {}

    void resized() override
    {
        listArea = layoutBase();
        rebuild();
    }

    void setData (const RefViewData& data, bool) override
    {
        auto next = hintsFor (data);
        if (next == hints)
            return;
        hints = std::move (next);
        rebuild();
        repaint();
    }

    ps::Severity attention() const override { return hints.empty() ? ps::Severity::Good : hints.front().severity; }

    void paint (juce::Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        drawSectionTitle (g, "What to try", titleRow.toNearestInt());
        if (hints.size() >= 2)
        {
            const int below = numBelow();
            juce::String text (juce::String ((int) hints.size()) + " tips");
            if (below > 0)
                text << " - " << below << " more below";
            g.setFont (font (10.5f, below > 0 ? 600 : 400));
            g.setColour (below > 0 ? Col::textDim : Col::textSubtle);
            g.drawText (text, titleRow, juce::Justification::centredRight, false);
        }

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (listArea.toNearestInt());
        for (size_t i = 0; i < cards.size(); ++i)
            paintCard (g, cards[i], (int) i == hoverCard);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const float before = scroll;
        scroll = juce::jlimit (0.0f, maxScroll(), scroll - w.deltaY * 90.0f);
        if (scroll != before)
        {
            rebuild();
            repaint();
        }
    }

    juce::String getTooltip() override
    {
        const int i = cardAt (getMouseXYRelative().toFloat());
        if (i < 0)
            return titleRow.contains (getMouseXYRelative().toFloat())
                       ? "What to try: the biggest differences from the reference and checks of your master, most "
                         "important first. Each tip says what it means and what to try."
                       : juce::String();
        return cards[(size_t) i].row ? cards[(size_t) i].detailText : juce::String();
    }

private:
    struct Card
    {
        bool lead = false, row = false;
        ps::Severity severity = ps::Severity::Info;
        juce::String tag, title, detailText;
        juce::TextLayout titleLayout, detail;
        juce::Rectangle<float> bounds, titleArea, detailArea;   // titleArea / detailArea relative to bounds
    };

    static juce::Font tagFont() { return font (9.5f, 700).withExtraKerningFactor (0.06f); }

    void hoverMoved (juce::Point<float> p) override
    {
        const int i = cardAt (p);
        if (i != hoverCard)
        {
            hoverCard = i;
            repaint();
        }
    }

    static Card makeCard (const HintText& h, bool lead, bool row, float x, float y, float w)
    {
        Card c;
        c.lead = lead;
        c.row = row;
        c.severity = h.severity;
        c.tag = severityTag (h);
        c.title = h.title;
        c.detailText = h.detail;
        const float tagW = textWidth (tagFont(), c.tag) + 8.0f;
        const float textX = 14.0f, textW = w - textX - 10.0f;
        if (row)
        {
            c.titleArea = { textX, 0.0f, juce::jmax (20.0f, textW - tagW), 28.0f };
            c.bounds = { x, y, w, 28.0f };
            return c;
        }
        juce::AttributedString title;
        title.setWordWrap (juce::AttributedString::byWord);
        title.append (h.title, font (lead ? 13.5f : 12.5f, 600), Col::text);
        c.titleLayout.createLayout (title, juce::jmax (40.0f, textW - tagW));
        juce::AttributedString detail;
        detail.setWordWrap (juce::AttributedString::byWord);
        detail.setLineSpacing (1.5f);
        detail.append (h.detail, font (lead ? 11.5f : 11.0f), Col::textDim.brighter (lead ? 0.2f : 0.12f));
        c.detail.createLayout (detail, juce::jmax (40.0f, textW));
        const float top = lead ? 9.0f : 7.0f;
        c.titleArea = { textX, top, textW - tagW, c.titleLayout.getHeight() };
        c.detailArea = { textX, c.titleArea.getBottom() + 3.0f, textW, c.detail.getHeight() };
        c.bounds = { x, y, w, c.detailArea.getBottom() + (lead ? 10.0f : 8.0f) };
        return c;
    }

    // Cards at scroll 0 in list coordinates (y from 0), then shifted by the scroll offset into the component.
    void rebuild()
    {
        cards.clear();
        if (hints.empty() || listArea.isEmpty())
            return;
        const float w = listArea.getWidth(), gap = 7.0f;
        std::vector<Card> out;
        out.push_back (makeCard (hints.front(), true, false, 0.0f, 0.0f, w));
        float y = out.back().bounds.getBottom() + gap;

        // Try cards (two columns when wide enough); fall back to rows when they do not fit.
        const int columns = w >= 520.0f ? 2 : 1;
        const float cardW = (w - gap * (float) (columns - 1)) / (float) columns;
        std::vector<Card> asCards;
        float yc = y;
        for (size_t i = 1; i < hints.size(); i += (size_t) columns)
        {
            float rowH = 0.0f;
            const size_t start = asCards.size();
            const bool alone = i + 1 >= hints.size();
            for (int c = 0; c < columns && i + (size_t) c < hints.size(); ++c)
            {
                asCards.push_back (makeCard (hints[i + (size_t) c], false, false, (cardW + gap) * (float) c, yc, alone ? w : cardW));
                rowH = juce::jmax (rowH, asCards.back().bounds.getHeight());
            }
            for (size_t k = start; k < asCards.size(); ++k)
                asCards[k].bounds.setHeight (rowH);
            yc += rowH + gap;
        }
        if (yc - gap <= listArea.getHeight() + 0.5f)
        {
            for (auto& c : asCards)
                out.push_back (std::move (c));
        }
        else
        {
            for (size_t i = 1; i < hints.size(); ++i)
            {
                out.push_back (makeCard (hints[i], false, true, 0.0f, y, w));
                y = out.back().bounds.getBottom() + 5.0f;
            }
        }

        contentHeight = out.back().bounds.getBottom();
        scroll = juce::jlimit (0.0f, maxScroll(), scroll);
        for (auto& c : out)
            c.bounds = c.bounds.translated (listArea.getX(), listArea.getY() - scroll);
        cards = std::move (out);
    }

    float maxScroll() const { return juce::jmax (0.0f, contentHeight - listArea.getHeight()); }

    int numBelow() const
    {
        int n = 0;
        for (const auto& c : cards)
            if (c.bounds.getBottom() > listArea.getBottom() + 1.0f)
                ++n;
        return n;
    }

    int cardAt (juce::Point<float> p) const
    {
        if (! listArea.contains (p))
            return -1;
        for (size_t i = 0; i < cards.size(); ++i)
            if (cards[i].bounds.contains (p))
                return (int) i;
        return -1;
    }

    void paintCard (juce::Graphics& g, const Card& c, bool hovered) const
    {
        const auto b = c.bounds;
        const auto colour = severityColour (c.severity);
        g.setColour ((hovered && c.row ? Col::panelHover : Col::panelRaised).withAlpha (c.lead ? 0.8f : 0.55f));
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (c.lead ? colour.withAlpha (0.35f) : Col::outline.withAlpha (hovered && c.row ? 0.9f : 0.5f));
        g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
        const float inset = c.row ? 7.0f : 6.0f;
        g.setColour (colour);
        g.fillRoundedRectangle (juce::Rectangle<float> (b.getX() + 1.0f, b.getY() + inset, c.lead ? 4.0f : 3.0f, b.getHeight() - 2.0f * inset), 1.5f);

        const float tagY = c.row ? b.getY() : b.getY() + 7.0f;
        const float tagH = c.row ? b.getHeight() : 16.0f;
        g.setFont (tagFont());
        g.setColour (colour.withAlpha (0.85f));
        g.drawText (c.tag, juce::Rectangle<float> (b.getRight() - 130.0f, tagY, 120.0f, tagH), juce::Justification::centredRight, false);

        if (c.row)
        {
            g.setFont (font (12.0f, 600));
            g.setColour (Col::text.withAlpha (hovered ? 1.0f : 0.92f));
            fitLine (g, c.title, c.titleArea.translated (b.getX(), b.getY()), juce::Justification::centredLeft, 0.85f);
            return;
        }
        c.titleLayout.draw (g, c.titleArea.translated (b.getX(), b.getY()));
        c.detail.draw (g, c.detailArea.translated (b.getX(), b.getY()));
    }

    juce::Rectangle<float> listArea;
    std::vector<HintText> hints;
    std::vector<Card> cards;
    float scroll = 0.0f, contentHeight = 0.0f;
    int hoverCard = -1;
};

//==============================================================================
// Tabs over the views that do not fit side by side; a dot shows a tab with something worth a look.
class ViewTabs final : public juce::Component, public juce::TooltipClient
{
public:
    ViewTabs()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
    }

    std::function<void (int index)> onSelect;

    void set (std::vector<juce::String> newLabels, std::vector<int> newIds, int selectedId)
    {
        labels = std::move (newLabels);
        ids = std::move (newIds);
        selected = selectedId;
        dots.assign (ids.size(), ps::Severity::Good);
        repaint();
    }

    void setAttention (size_t index, ps::Severity s)
    {
        if (index < dots.size() && dots[index] != s)
        {
            dots[index] = s;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        drawChip (g, b, b.getHeight() * 0.5f, Col::background.darker (0.1f), Col::outline);
        const float segW = b.getWidth() / (float) juce::jmax ((size_t) 1, labels.size());
        const auto mouse = getMouseXYRelative().toFloat();
        for (size_t i = 0; i < labels.size(); ++i)
        {
            auto seg = juce::Rectangle<float> (b.getX() + segW * (float) i, b.getY(), segW, b.getHeight()).reduced (2.0f);
            const bool on = ids[i] == selected;
            if (on)
                drawChip (g, seg, seg.getHeight() * 0.5f, blend (Col::panelRaised, Col::reference, 0.2f), Col::reference.withAlpha (0.6f));
            else if (isMouseOver (true) && seg.contains (mouse))
            {
                g.setColour (Col::panelHover.withAlpha (0.8f));
                g.fillRoundedRectangle (seg, seg.getHeight() * 0.5f);
            }
            const auto f = font (11.5f, 600);
            const bool dot = dots[i] == ps::Severity::Minor || dots[i] == ps::Severity::Major;
            const float tw = juce::jmin (seg.getWidth() - 16.0f, textWidth (f, labels[i]));
            const float x0 = seg.getCentreX() - (tw + (dot ? 10.0f : 0.0f)) * 0.5f;
            g.setFont (f);
            g.setColour (on ? Col::text : Col::textDim);
            fitLine (g, labels[i], { x0, seg.getY(), tw + 1.0f, seg.getHeight() }, juce::Justification::centredLeft, 0.75f);
            if (dot)
            {
                g.setColour (severityColour (dots[i]));
                g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ x0 + tw + 7.0f, seg.getCentreY() }));
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e) || labels.empty() || ! getLocalBounds().contains (e.getPosition()) || onSelect == nullptr)
            return;
        const int i = juce::jlimit (0, (int) labels.size() - 1, (int) (e.position.x * (float) labels.size() / (float) getWidth()));
        onSelect (ids[(size_t) i]);
    }

    juce::String getTooltip() override
    {
        return "Switch between the comparison views (a dot marks a view with something worth a look)";
    }

private:
    std::vector<juce::String> labels;
    std::vector<int> ids;
    std::vector<ps::Severity> dots;
    int selected = 0;
};
} // namespace

juce::Colour severityColour (ps::Severity s)
{
    switch (s)
    {
        case ps::Severity::Good:  return Col::good;
        case ps::Severity::Info:  return Col::info;
        case ps::Severity::Minor: return Col::minor;
        case ps::Severity::Major: return Col::major;
    }
    return Col::info;
}

//==============================================================================
struct RefComparison::Impl
{
    explicit Impl (RefComparison& o) : owner (o)
    {
        for (auto* v : views())
            owner.addAndMakeVisible (v);
        owner.addChildComponent (tabs);
        tabs.onSelect = [this] (int id) { current = (View) id; owner.resized(); };
    }

    std::array<CompareView*, 4> views() { return { &tonal, &loudness, &stereo, &hints }; }

    enum class Mode { grid, split, tabbed };

    RefComparison& owner;
    TonalView tonal;
    LoudnessView loudness;
    StereoView stereo;
    HintsView hints;
    ViewTabs tabs;
    View current = View::hints;
    Mode mode = Mode::grid;
    std::vector<int> tabIds;
};

RefComparison::RefComparison() : impl (std::make_unique<Impl> (*this))
{
    setTitle ("Comparison");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

RefComparison::~RefComparison() = default;

void RefComparison::setData (const RefViewData& data, bool snap)
{
    auto& d = *impl;
    for (auto* v : d.views())
        v->setData (data, snap);
    for (size_t i = 0; i < d.tabIds.size(); ++i)
        d.tabs.setAttention (i, d.views()[(size_t) d.tabIds[i]]->attention());
}

void RefComparison::showView (View v)
{
    impl->current = v;
    resized();
}

void RefComparison::resized()
{
    auto& d = *impl;
    auto r = getLocalBounds();
    const int gap = 8;
    const auto mode = (r.getWidth() >= 800 && r.getHeight() >= 400) ? Impl::Mode::grid
                    : (r.getWidth() >= 620 ? Impl::Mode::split : Impl::Mode::tabbed);
    d.mode = mode;

    for (auto* v : d.views())
        v->setVisible (false);

    auto showIn = [] (juce::Component& c, juce::Rectangle<int> area) {
        c.setBounds (area);
        c.setVisible (true);
    };

    if (mode == Impl::Mode::grid)
    {
        d.tabs.setVisible (false);
        d.tabIds.clear();
        auto left = r.removeFromLeft (r.getWidth() * 55 / 100);
        r.removeFromLeft (gap);
        const int topH = r.getHeight() * 56 / 100;
        showIn (d.tonal, left.removeFromTop (topH));
        left.removeFromTop (gap);
        showIn (d.loudness, left);
        showIn (d.hints, r.removeFromTop (topH));
        r.removeFromTop (gap);
        showIn (d.stereo, r);
        return;
    }

    std::vector<juce::String> labels;
    std::vector<int> ids;
    if (mode == Impl::Mode::split)
    {
        auto left = r.removeFromLeft (r.getWidth() * 56 / 100);
        r.removeFromLeft (gap);
        showIn (d.tonal, left);
        labels = { "What to try", "Loudness", "Stereo" };
        ids = { (int) View::hints, (int) View::loudness, (int) View::stereo };
        if (d.current == View::tonal)
            d.current = View::hints;
    }
    else
    {
        labels = { "Tonal balance", "Loudness", "Stereo", "What to try" };
        ids = { (int) View::tonal, (int) View::loudness, (int) View::stereo, (int) View::hints };
    }

    d.tabIds = ids;
    d.tabs.set (labels, ids, (int) d.current);
    showIn (d.tabs, r.removeFromTop (26));
    r.removeFromTop (6);
    showIn (*d.views()[(size_t) d.current], r);
    for (size_t i = 0; i < ids.size(); ++i)
        d.tabs.setAttention (i, d.views()[(size_t) ids[i]]->attention());
}

void RefComparison::paint (juce::Graphics&) {}
} // namespace ps::ui
