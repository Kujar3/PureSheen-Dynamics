#include "ui/ExciterParts.h"

#include "dsp/Exciter.h"
#include "ui/ImagerExciterCommon.h"

#include <cmath>

// Exciter panel parts. Owner: ui-stereo.
namespace ps::ui
{
namespace
{
constexpr int kNumModes = (int) ps::ExciterMode::Count;

// Builds the curve of a model over x in -1..+1 inside `box` (y scaled so the largest |output| fills the box).
juce::Path curvePath (int mode, float drive, juce::Rectangle<float> box, int points, float& peakOut)
{
    std::vector<float> ys ((size_t) points);
    peakOut = 1.0f;

    for (int i = 0; i < points; ++i)
    {
        const float x = -1.0f + 2.0f * (float) i / (float) (points - 1);
        float y = ps::Exciter::transferCurve ((ps::ExciterMode) juce::jlimit (0, kNumModes - 1, mode), x, drive);

        if (! std::isfinite (y))
            y = 0.0f;

        ys[(size_t) i] = y;
        peakOut = juce::jmax (peakOut, std::abs (y));
    }

    juce::Path p;

    for (int i = 0; i < points; ++i)
    {
        const float px = box.getX() + box.getWidth() * (float) i / (float) (points - 1);
        const float py = box.getCentreY() - ys[(size_t) i] / peakOut * box.getHeight() * 0.5f;

        if (i == 0)
            p.startNewSubPath (px, py);
        else
            p.lineTo (px, py);
    }

    return p;
}
} // namespace

juce::String exciterModeName (int mode)
{
    static const char* names[] = { "Analog", "Retro", "Tape", "Tube", "Warm", "Triode", "Dual Triode" };
    return names[juce::jlimit (0, kNumModes - 1, mode)];
}

juce::String exciterModeDescription (int mode)
{
    static const char* text[] = {
        "Odd harmonics with a gritty edge.",
        "Transistor-style odd harmonics that linger.",
        "Bright odd harmonics with soft tape clipping.",
        "Even and odd harmonics that liven up attacks.",
        "Even harmonics only. Smooth and quick.",
        "One triode stage: subtle, asymmetric warmth.",
        "Two push-pull stages: stronger and warmer.",
    };
    return text[juce::jlimit (0, kNumModes - 1, mode)];
}

//==============================================================================
ExciterModeGrid::ExciterModeGrid (PureSheenProcessor& p) : processor (p), colour (Colours::accent)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void ExciterModeGrid::setParamIds (const juce::String& id, const juce::String& linked)
{
    paramId = id;
    linkedId = linked;
    shownValue = -1;
    update();
}

void ExciterModeGrid::setColour (juce::Colour c)
{
    if (c != colour)
    {
        colour = c;
        repaint();
    }
}

int ExciterModeGrid::currentValue() const
{
    return paramId.isEmpty() ? -1 : juce::jlimit (0, kNumModes - 1, juce::roundToInt (processor.getParameterValue (paramId)));
}

void ExciterModeGrid::update()
{
    const int v = currentValue();

    if (v != shownValue)
    {
        shownValue = v;
        repaint();
    }
}

int ExciterModeGrid::getColumns() const noexcept
{
    return getWidth() >= 7 * 74 ? 7 : 4;
}

juce::Rectangle<float> ExciterModeGrid::cellBounds (int index) const
{
    const int cols = getColumns();
    const int rows = (kNumModes + cols - 1) / cols;
    const float gap = 4.0f;
    const auto b = getLocalBounds().toFloat();
    const float cellW = (b.getWidth() - gap * (float) (cols - 1)) / (float) cols;
    const float cellH = (b.getHeight() - gap * (float) (rows - 1)) / (float) rows;
    const int row = index / cols, col = index % cols;
    return { b.getX() + (float) col * (cellW + gap), b.getY() + (float) row * (cellH + gap), cellW, cellH };
}

int ExciterModeGrid::cellAt (juce::Point<float> p) const
{
    for (int i = 0; i < kNumModes; ++i)
        if (cellBounds (i).contains (p))
            return i;

    return -1;
}

void ExciterModeGrid::paint (juce::Graphics& g)
{
    const int current = currentValue();

    for (int i = 0; i < kNumModes; ++i)
    {
        const auto cell = cellBounds (i).reduced (0.5f);
        const bool selected = i == current;
        const bool hover = i == hoverCell;
        const float radius = 6.0f;

        if (selected)
        {
            g.setGradientFill (juce::ColourGradient::vertical (Colours::panelHover.interpolatedWith (colour, 0.32f), cell.getY(),
                                                               Colours::panelRaised.interpolatedWith (colour, 0.18f), cell.getBottom()));
            g.fillRoundedRectangle (cell, radius);
            g.setColour (colour.withAlpha (0.8f));
            g.drawRoundedRectangle (cell.reduced (0.5f), radius, 1.0f);
        }
        else
        {
            g.setColour (hover ? Colours::panelHover : juce::Colours::black.withAlpha (0.2f));
            g.fillRoundedRectangle (cell, radius);
            g.setColour (Colours::outline.withAlpha (hover ? 1.0f : 0.8f));
            g.drawRoundedRectangle (cell.reduced (0.5f), radius, 1.0f);
        }

        // Curve glyph on the left, name on the right.
        auto inner = cell.reduced (6.0f, 3.0f);
        const float glyphSize = juce::jmin (inner.getHeight(), 16.0f);
        const bool showGlyph = inner.getWidth() > 62.0f;

        if (showGlyph)
        {
            const auto glyph = inner.removeFromLeft (glyphSize).withSizeKeepingCentre (glyphSize, glyphSize);
            inner.removeFromLeft (5.0f);
            float peak = 1.0f;
            const auto path = curvePath (i, 70.0f, glyph, 17, peak);
            g.setColour ((selected ? colour.brighter (0.3f) : Colours::textSubtle).withAlpha (selected ? 1.0f : 0.8f));
            g.strokePath (path, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setFont (font (juce::jmin (11.5f, cell.getHeight() * 0.5f), selected ? 700 : 600));
        g.setColour (selected ? colour.brighter (0.5f) : (hover ? Colours::text : Colours::textDim));
        g.drawFittedText (exciterModeName (i), inner.toNearestInt(), showGlyph ? juce::Justification::centredLeft
                                                                                 : juce::Justification::centred, 1, 0.75f);
    }
}

void ExciterModeGrid::mouseUp (const juce::MouseEvent& e)
{
    if (! isLeftClick (e) || paramId.isEmpty())
        return;

    const int cell = cellAt (e.position);

    if (cell < 0 || cell == currentValue() || cell != cellAt (e.mouseDownPosition))
        return;

    stereo::setParam (processor, paramId, linkedId, (float) cell);
    update();
}

void ExciterModeGrid::mouseMove (const juce::MouseEvent& e)
{
    const int cell = cellAt (e.position);

    if (cell != hoverCell)
    {
        hoverCell = cell;
        setTooltip (cell >= 0 ? exciterModeName (cell) + ": " + exciterModeDescription (cell) : juce::String());
        repaint();
    }
}

void ExciterModeGrid::mouseExit (const juce::MouseEvent&)
{
    hoverCell = -1;
    repaint();
}

//==============================================================================
ExciterCurveView::ExciterCurveView() : colour (Colours::accent)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setInterceptsMouseClicks (false, false);
}

void ExciterCurveView::setCurve (int m, float d, juce::Colour c, bool bandOn)
{
    if (m == mode && std::abs (d - drive) < 1.0e-4f && c == colour && bandOn == on)
        return;

    mode = m;
    drive = d;
    colour = c;
    on = bandOn;
    repaint();
}

void ExciterCurveView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    stereo::drawWell (g, b);

    // Square plot, centred.
    const float side = juce::jmax (10.0f, juce::jmin (b.getWidth(), b.getHeight()) - 20.0f);
    const auto plot = juce::Rectangle<float> (side, side).withCentre (b.getCentre());

    g.setColour (Colours::outline.withAlpha (0.35f));

    for (int i = 1; i < 4; ++i)
    {
        const float t = (float) i / 4.0f;
        g.drawHorizontalLine (juce::roundToInt (plot.getY() + plot.getHeight() * t), plot.getX(), plot.getRight());
        g.drawVerticalLine (juce::roundToInt (plot.getX() + plot.getWidth() * t), plot.getY(), plot.getBottom());
    }

    g.setColour (Colours::outline.withAlpha (0.7f));
    g.drawRect (plot, 1.0f);

    float peak = 1.0f;
    const auto curve = curvePath (mode, drive, plot, juce::jlimit (32, 256, (int) side), peak);

    // Straight line at the same scale (what "no saturation" would draw).
    const float k = 1.0f / peak;
    const float dashes[] = { 3.0f, 3.0f };
    g.setColour (Colours::textFaint);
    g.drawDashedLine ({ plot.getX(), plot.getCentreY() + k * plot.getHeight() * 0.5f,
                        plot.getRight(), plot.getCentreY() - k * plot.getHeight() * 0.5f }, dashes, 2, 1.0f);

    // Shade between the curve and the axis.
    {
        juce::Path fill (curve);
        fill.lineTo (plot.getRight(), plot.getCentreY());
        fill.lineTo (plot.getX(), plot.getCentreY());
        fill.closeSubPath();
        g.setColour (colour.withAlpha (on ? 0.10f : 0.04f));
        g.fillPath (fill);
    }

    g.setColour (on ? colour : Colours::textFaint);
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setFont (font (10.0f, 600));
    g.setColour (Colours::textFaint);
    g.drawText ("IN", juce::Rectangle<float> (plot.getRight() - 24.0f, plot.getCentreY() + 2.0f, 22.0f, 12.0f), juce::Justification::centredRight, false);
    g.drawText ("OUT", juce::Rectangle<float> (plot.getCentreX() + 4.0f, plot.getY() + 2.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
}

} // namespace ps::ui
