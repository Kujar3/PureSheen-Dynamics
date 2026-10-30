#include "ui/SpectralWidgets.h"

#include "ui/Controls.h"

#include <cmath>

// Views shared by the spectral panels (owner: ui-spectral). See SpectralWidgets.h.
namespace ps::ui
{
namespace
{
constexpr float kTitleH = 28.0f;
constexpr float kReadoutH = 16.0f;
constexpr float kLabelH = 20.0f;
constexpr int kHoldTicks = 45;   // 1.5 s at 30 Hz

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

void drawSurface (juce::Graphics& g, juce::Rectangle<float> b)
{
    g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), b.getY(),
                                                       Colours::background.darker (0.05f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
}

void drawOutline (juce::Graphics& g, juce::Rectangle<float> b)
{
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);
}

juce::String readout (float db, bool sign)
{
    if (std::abs (db) < 0.05f)
        return "0.0";

    return (sign && db > 0.0f ? "+" : "") + juce::String (db, 1);
}
} // namespace

//==============================================================================
// SpectralMeterColumn
//==============================================================================
SpectralMeterColumn::SpectralMeterColumn (const juce::String& t) : title (t)
{
    noFocus (*this);
}

void SpectralMeterColumn::setTitle (const juce::String& t)
{
    if (t != title)
    {
        title = t;
        repaint();
    }
}

void SpectralMeterColumn::setBars (std::vector<Bar> b)
{
    bars = std::move (b);
    states.assign (bars.size(), {});

    for (size_t i = 0; i < bars.size(); ++i)
        states[i].shown = states[i].hold = bars[i].baseDb;

    repaint();
}

void SpectralMeterColumn::setBarColour (int bar, juce::Colour c)
{
    if (bar >= 0 && bar < (int) bars.size() && bars[(size_t) bar].colour != c)
    {
        bars[(size_t) bar].colour = c;
        repaint();
    }
}

void SpectralMeterColumn::setValue (int bar, float db, bool valid)
{
    if (bar < 0 || bar >= (int) bars.size())
        return;

    const auto& b = bars[(size_t) bar];
    auto& s = states[(size_t) bar];
    const float target = valid && std::isfinite (db) ? juce::jlimit (b.minDb - 1.0f, b.maxDb + 1.0f, db) : b.baseDb;
    const float before = s.shown, beforeHold = s.hold;
    const bool beforeValid = s.valid;

    // Distance from the base decides rise vs fall (works for bars hanging from the top as well).
    const bool rising = std::abs (target - b.baseDb) > std::abs (s.shown - b.baseDb);
    s.shown += (target - s.shown) * (rising ? 0.6f : 0.18f);

    if (std::abs (s.shown - b.baseDb) >= std::abs (s.hold - b.baseDb) || s.holdTicks <= 0)
    {
        if (std::abs (s.shown - b.baseDb) >= std::abs (s.hold - b.baseDb))
        {
            s.hold = s.shown;
            s.holdTicks = kHoldTicks;
        }
        else
        {
            s.hold += (s.shown - s.hold) * 0.2f;
        }
    }
    else
    {
        --s.holdTicks;
    }

    s.valid = valid;

    if (std::abs (s.shown - before) > 0.02f || std::abs (s.hold - beforeHold) > 0.02f || valid != beforeValid)
        repaint();
}

void SpectralMeterColumn::resetHolds()
{
    for (size_t i = 0; i < states.size(); ++i)
    {
        states[i].hold = states[i].shown;
        states[i].holdTicks = 0;
    }

    repaint();
}

void SpectralMeterColumn::setLimit (int bar, float db)
{
    if (bar < 0 || bar >= (int) states.size())
        return;

    auto& l = states[(size_t) bar].limit;

    if ((std::isnan (db) && std::isnan (l)) || std::abs (db - l) < 0.01f)
        return;

    l = db;
    repaint();
}

void SpectralMeterColumn::setNote (const juce::String& n)
{
    if (n != note)
    {
        note = n;
        repaint();
    }
}

int SpectralMeterColumn::notePlacement() const
{
    if (note.isEmpty())
        return 0;

    const float w = (float) getWidth() - 20.0f;

    if (textWidth (font (10.5f, 700).withExtraKerningFactor (0.08f), title.toUpperCase()) + 12.0f
            + textWidth (font (10.5f, 600), note) <= w)
        return 1;

    // Under the labels only while the bars keep a useful height.
    return (float) getHeight() - kTitleH - kReadoutH - kLabelH - 24.0f >= 80.0f ? 2 : 0;
}

juce::Rectangle<float> SpectralMeterColumn::barArea (int bar) const
{
    auto b = getLocalBounds().toFloat().reduced (10.0f, 0.0f);
    b.removeFromTop (kTitleH + kReadoutH);
    b.removeFromBottom (kLabelH + (notePlacement() == 2 ? 18.0f : 0.0f) + 6.0f);
    const int n = juce::jmax (1, (int) bars.size());
    const float barW = juce::jlimit (12.0f, 22.0f, b.getWidth() / ((float) n * 3.2f));
    const float gap = juce::jmin (46.0f, (b.getWidth() - barW * (float) n) / (float) (n + 1));
    const float total = barW * (float) n + gap * (float) (n - 1);
    const float x0 = b.getCentreX() - total * 0.5f;
    return { x0 + (float) bar * (barW + gap), b.getY(), barW, juce::jmax (10.0f, b.getHeight()) };
}

void SpectralMeterColumn::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawSurface (g, b);

    g.setFont (font (10.5f, 700).withExtraKerningFactor (0.08f));
    g.setColour (Colours::text.withAlpha (0.9f));
    g.drawText (title.toUpperCase(), b.reduced (10.0f, 0.0f).removeFromTop (kTitleH).translated (0.0f, 1.0f),
                juce::Justification::centredLeft, false);

    for (int i = 0; i < (int) bars.size(); ++i)
    {
        const auto& bar = bars[(size_t) i];
        const auto& s = states[(size_t) i];
        const auto m = barArea (i);
        auto yOf = [&] (float db)
        {
            return m.getBottom() - (juce::jlimit (bar.minDb, bar.maxDb, db) - bar.minDb) / (bar.maxDb - bar.minDb) * m.getHeight();
        };

        // Scale ticks.
        if (bar.scaleSide != 0)
        {
            g.setFont (font (9.5f, 600));
            // Fewer ticks on a short meter (at least 15 px apart).
            float step = bar.tickStep;

            while (m.getHeight() * step / (bar.maxDb - bar.minDb) < 15.0f && step < bar.maxDb - bar.minDb)
                step *= 2.0f;

            const float first = std::ceil (bar.minDb / step) * step;

            for (float db = first; db <= bar.maxDb + 0.01f; db += step)
            {
                const float y = std::round (yOf (db));
                const bool zero = std::abs (db - bar.baseDb) < 0.01f;
                const auto text = std::abs (db) < 0.01f ? juce::String ("0")
                                                        : (bar.signedReadout && db > 0.0f ? "+" : "") + juce::String (juce::roundToInt (db));
                g.setColour (zero ? Colours::textDim : Colours::textSubtle);
                const auto r = bar.scaleSide < 0 ? juce::Rectangle<float> (m.getX() - 30.0f, y - 6.0f, 25.0f, 12.0f)
                                                 : juce::Rectangle<float> (m.getRight() + 5.0f, y - 6.0f, 25.0f, 12.0f);
                g.drawText (text, r, bar.scaleSide < 0 ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
                g.setColour (Colours::outline.withAlpha (zero ? 0.8f : 0.3f));
                g.fillRect (bar.scaleSide < 0 ? juce::Rectangle<float> (m.getX() - 4.0f, y, 3.0f, 1.0f)
                                              : juce::Rectangle<float> (m.getRight() + 1.0f, y, 3.0f, 1.0f));
            }
        }

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (m, 3.0f);

        const float baseY = yOf (bar.baseDb);

        if (s.valid && std::abs (s.shown - bar.baseDb) > 0.01f)
        {
            const float y = yOf (s.shown);
            const auto fill = juce::Rectangle<float> (m.getX(), juce::jmin (y, baseY), m.getWidth(), std::abs (y - baseY));
            const bool up = y < baseY;
            g.setGradientFill (juce::ColourGradient::vertical (bar.colour.withAlpha (up ? 0.95f : 0.6f), fill.getY(),
                                                               bar.colour.withAlpha (up ? 0.6f : 0.95f), fill.getBottom()));
            g.fillRect (fill.reduced (2.0f, 0.0f));
        }

        if (s.valid && std::abs (s.hold - bar.baseDb) > 0.05f)
        {
            const float y = std::round (yOf (s.hold));
            g.setColour (bar.colour.brighter (0.3f));
            g.fillRect (juce::Rectangle<float> (m.getX() + 2.0f, y - 0.5f, m.getWidth() - 4.0f, 1.5f));
        }

        if (baseY > m.getY() + 1.0f && baseY < m.getBottom() - 1.0f)
        {
            g.setColour (Colours::outline.brighter (0.2f));
            g.fillRect (juce::Rectangle<float> (m.getX(), std::round (baseY), m.getWidth(), 1.0f));
        }

        // Limit: two small inward ticks at the bar's sides.
        if (std::isfinite (s.limit) && std::abs (s.limit - bar.baseDb) > 0.05f)
        {
            const float y = std::round (yOf (s.limit));
            g.setColour (Colours::textDim.withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (m.getX() - 4.0f, y - 0.5f, 6.0f, 1.5f));
            g.fillRect (juce::Rectangle<float> (m.getRight() - 2.0f, y - 0.5f, 6.0f, 1.5f));
        }

        g.setColour (Colours::outline.withAlpha (0.7f));
        g.drawRoundedRectangle (m.reduced (0.5f), 3.0f, 1.0f);

        // Readout above, label below.
        g.setFont (font (10.5f, 700));
        g.setColour (s.valid ? Colours::text.withAlpha (0.9f) : Colours::textFaint);
        g.drawFittedText (s.valid ? readout (s.hold, bar.signedReadout) : juce::String ("--"),
                          juce::Rectangle<float> (m.getCentreX() - 26.0f, m.getY() - kReadoutH - 1.0f, 52.0f, kReadoutH).toNearestInt(),
                          juce::Justification::centred, 1, 0.7f);
        g.setFont (font (10.5f, 600));
        g.setColour (Colours::textDim);
        g.drawFittedText (bar.label, juce::Rectangle<float> (m.getCentreX() - 30.0f, m.getBottom() + 4.0f, 60.0f, 14.0f).toNearestInt(),
                          juce::Justification::centred, 1, 0.7f);
    }

    if (const int placement = notePlacement(); placement != 0)
    {
        g.setFont (font (10.5f, 600));
        g.setColour (Colours::textSubtle);

        if (placement == 1)
            g.drawText (note, b.reduced (10.0f, 0.0f).removeFromTop (kTitleH).translated (0.0f, 1.0f),
                        juce::Justification::centredRight, false);
        else
            g.drawFittedText (note, b.reduced (8.0f, 0.0f).removeFromBottom (22.0f).toNearestInt(),
                              juce::Justification::centredTop, 1, 0.75f);
    }

    drawOutline (g, b);
}

//==============================================================================
// SpectralTraceView
//==============================================================================
SpectralTraceView::SpectralTraceView() : accent (Colours::accent)
{
    noFocus (*this);
}

void SpectralTraceView::setAccentColour (juce::Colour c)
{
    if (c != accent)
    {
        accent = c;
        repaint();
    }
}

void SpectralTraceView::setTitle (const juce::String& t)
{
    if (t != title)
    {
        title = t;
        repaint();
    }
}

void SpectralTraceView::setLevelRange (float minDb, float maxDb)
{
    levelMin = minDb;
    levelMax = juce::jmax (minDb + 1.0f, maxDb);
    repaint();
}

void SpectralTraceView::setGrRange (float minDb)
{
    grMin = juce::jmin (-1.0f, minDb);
    repaint();
}

void SpectralTraceView::setSeconds (float s)
{
    seconds = juce::jlimit (1.0f, (float) kCapacity / (float) kColumnsPerSecond, s);
    repaint();
}

void SpectralTraceView::push (float levelDb, float grDb, bool valid)
{
    head = (head + 1) % kCapacity;
    ring[(size_t) head] = { std::isfinite (levelDb) ? levelDb : -200.0f, std::isfinite (grDb) ? juce::jmin (0.0f, grDb) : 0.0f, valid };
    count = juce::jmin (kCapacity, count + 1);
}

void SpectralTraceView::clear()
{
    count = 0;
    repaint();
}

void SpectralTraceView::setStatusText (const juce::String& s)
{
    if (s != status)
    {
        status = s;
        repaint();
    }
}

const SpectralTraceView::Column& SpectralTraceView::columnAt (int age) const noexcept
{
    return ring[(size_t) ((head - age + kCapacity * 2) % kCapacity)];
}

juce::Rectangle<float> SpectralTraceView::plotArea() const
{
    return getLocalBounds().toFloat().reduced (1.0f).withTrimmedTop ((float) kHeaderH);
}

void SpectralTraceView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawSurface (g, b);

    const auto header = b.reduced (1.0f).removeFromTop ((float) kHeaderH);
    g.setColour (juce::Colours::white.withAlpha (0.018f));
    g.fillRect (header.withTrimmedTop (6.0f));
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (juce::Rectangle<float> (header.getX(), header.getBottom() - 1.0f, header.getWidth(), 1.0f));

    // Header: title left, legend right.
    {
        auto row = header.reduced (10.0f, 0.0f);
        const auto f = font (10.5f, 600);

        auto item = [&] (const juce::String& text, juce::Colour c)
        {
            const float w = textWidth (f, text) + 16.0f;

            if (row.getWidth() < w + 120.0f)
                return;

            auto r = row.removeFromRight (w);
            row.removeFromRight (10.0f);
            g.setColour (c);
            g.fillRoundedRectangle (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 3.0f), 1.5f);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
        };

        item ("Reduction", accent);
        item ("Region level", Colours::input);

        g.setFont (font (10.5f, 700).withExtraKerningFactor (0.08f));
        g.setColour (Colours::text.withAlpha (0.9f));
        g.drawText (title.toUpperCase(), row, juce::Justification::centredLeft, true);
    }

    paintPlot (g);
    drawOutline (g, b);
}

void SpectralTraceView::paintPlot (juce::Graphics& g) const
{
    const auto plot = plotArea();
    juce::Graphics::ScopedSaveState state (g);
    juce::Path clip;
    clip.addRoundedRectangle (plot.getX(), plot.getY(), plot.getWidth(), plot.getHeight(), 6.0f, 6.0f, false, false, true, true);
    g.reduceClipRegion (clip);

    const float labelW = 30.0f;
    const auto inner = plot.withTrimmedLeft (labelW).withTrimmedRight (labelW).withTrimmedBottom (16.0f).withTrimmedTop (6.0f);
    auto levelY = [&] (float db) { return inner.getBottom() - (juce::jlimit (levelMin, levelMax, db) - levelMin) / (levelMax - levelMin) * inner.getHeight(); };
    auto grY = [&] (float db) { return inner.getY() + juce::jlimit (0.0f, 1.0f, db / grMin) * inner.getHeight(); };

    // Grid: level lines (left labels) and the GR scale (right labels) on the same lines.
    g.setFont (font (10.0f, 600));
    const int lines = 6;

    for (int i = 0; i <= lines; ++i)
    {
        const float t = (float) i / (float) lines;
        const float y = std::round (inner.getY() + t * inner.getHeight());
        g.setColour (Colours::outline.withAlpha (i == 0 ? 0.6f : 0.3f));
        g.fillRect (juce::Rectangle<float> (inner.getX(), y, inner.getWidth(), 1.0f));
        const float lv = levelMax - t * (levelMax - levelMin);
        const float gv = t * grMin;
        g.setColour (Colours::textSubtle);
        g.drawText (juce::String (juce::roundToInt (lv)), juce::Rectangle<float> (plot.getX() + 2.0f, y - 6.0f, labelW - 6.0f, 12.0f),
                    juce::Justification::centredRight, false);
        g.setColour (accent.withAlpha (0.75f));
        g.drawText (i == 0 ? juce::String ("0") : juce::String (juce::roundToInt (gv)),
                    juce::Rectangle<float> (inner.getRight() + 4.0f, y - 6.0f, labelW - 6.0f, 12.0f),
                    juce::Justification::centredLeft, false);
    }

    // Time labels.
    const int columns = juce::roundToInt (seconds * (float) kColumnsPerSecond);
    const float dx = inner.getWidth() / (float) juce::jmax (1, columns - 1);
    g.setColour (Colours::textSubtle);

    for (int s = 0; s <= (int) seconds; s += (seconds > 8.0f ? 2 : 1))
    {
        const float x = inner.getRight() - (float) s * (float) kColumnsPerSecond * dx;

        if (x < inner.getX() - 1.0f)
            break;

        g.setColour (Colours::outline.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float> (std::round (x), inner.getY(), 1.0f, inner.getHeight()));
        g.setColour (Colours::textSubtle);
        g.drawText (s == 0 ? juce::String ("now") : "-" + juce::String (s) + " s",
                    juce::Rectangle<float> (x - 40.0f, inner.getBottom() + 2.0f, 40.0f - 3.0f, 12.0f),
                    juce::Justification::centredRight, false);
    }

    const int n = juce::jmin (count, columns);

    if (n >= 2)
    {
        juce::Path level, levelLine, gr, grLine;
        bool started = false;

        for (int age = n - 1; age >= 0; --age)
        {
            const auto& c = columnAt (age);
            const float x = inner.getRight() - (float) age * dx;
            const float ly = c.valid ? levelY (c.levelDb) : inner.getBottom();
            const float gy = c.valid ? grY (c.grDb) : inner.getY();

            if (! started)
            {
                level.startNewSubPath (x, inner.getBottom());
                gr.startNewSubPath (x, inner.getY());
                levelLine.startNewSubPath (x, ly);
                grLine.startNewSubPath (x, gy);
                started = true;
            }
            else
            {
                levelLine.lineTo (x, ly);
                grLine.lineTo (x, gy);
            }

            level.lineTo (x, ly);
            gr.lineTo (x, gy);
        }

        level.lineTo (inner.getRight(), inner.getBottom());
        level.closeSubPath();
        gr.lineTo (inner.getRight(), inner.getY());
        gr.closeSubPath();

        g.setGradientFill (juce::ColourGradient::vertical (Colours::input.withAlpha (0.38f), inner.getY(),
                                                           Colours::input.withAlpha (0.06f), inner.getBottom()));
        g.fillPath (level);
        g.setColour (Colours::input.withAlpha (0.75f));
        g.strokePath (levelLine, juce::PathStrokeType (1.2f));

        g.setGradientFill (juce::ColourGradient::vertical (accent.withAlpha (0.42f), inner.getY(),
                                                           accent.withAlpha (0.12f), inner.getBottom()));
        g.fillPath (gr);
        g.setColour (accent);
        g.strokePath (grLine, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (status.isNotEmpty() || n < 2)
    {
        const auto text = status.isNotEmpty() ? status : juce::String ("Play audio to see the trace");
        const auto f = font (12.0f, 600);
        const auto box = juce::Rectangle<float> (juce::jmin (textWidth (f, text) + 28.0f, inner.getWidth() - 20.0f), 26.0f)
                             .withCentre (inner.getCentre());
        g.setColour (Colours::background.withAlpha (0.82f));
        g.fillRoundedRectangle (box, 13.0f);
        g.setColour (Colours::outline.withAlpha (0.8f));
        g.drawRoundedRectangle (box.reduced (0.5f), 13.0f, 1.0f);
        g.setFont (f);
        g.setColour (Colours::textDim);
        g.drawFittedText (text, box.toNearestInt().reduced (8, 0), juce::Justification::centred, 1, 0.8f);
    }
}
} // namespace ps::ui
