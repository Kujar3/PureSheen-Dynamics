#include "ui/TapeViews.h"

#include "dsp/Tape.h"
#include "ui/Controls.h"
#include "ui/EqModel.h"
#include "ui/ImagerExciterCommon.h"

#include <cmath>

// Tape panel views. Owner: mod-tape. See TapeViews.h.
namespace ps::ui::tape
{
namespace
{
constexpr float kRangeDb = 12.0f;        // response view: +-12 dB
constexpr float kBumpSearchLoHz = 20.0f, kBumpSearchHiHz = 300.0f;
constexpr float kHighReadoutHz = 16000.0f;

bool sameParams (const ps::TapeParams& a, const ps::TapeParams& b) noexcept
{
    return a.speed == b.speed && a.driveDb == b.driveDb && a.bias == b.bias && a.harmonics == b.harmonics
           && a.lowEmphasis == b.lowEmphasis && a.highEmphasis == b.highEmphasis;
}
} // namespace

ps::TapeParams readParams (const PureSheenProcessor& p)
{
    ps::TapeParams t;
    t.speed = (ps::TapeSpeed) juce::jlimit (0, (int) ps::TapeSpeed::Count - 1, juce::roundToInt (p.getParameterValue ("tape.speed")));
    t.driveDb = p.getParameterValue ("tape.drive");
    t.bias = p.getParameterValue ("tape.bias");
    t.harmonics = p.getParameterValue ("tape.harmonics");
    t.lowEmphasis = p.getParameterValue ("tape.lowEmph");
    t.highEmphasis = p.getParameterValue ("tape.highEmph");
    return t;
}

//==============================================================================
ResponseView::ResponseView (PureSheenProcessor& p, EditorServices& s)
    : processor (p), services (s), colour (moduleColour (ps::ModuleType::Tape))
{
    setInterceptsMouseClicks (true, false);
    dy.minDb = -kRangeDb;
    dy.maxDb = kRangeDb;
}

void ResponseView::setColour (juce::Colour c)
{
    colour = c;
    repaint();
}

void ResponseView::resized() { valid = false; }

void ResponseView::update()
{
    const auto now = readParams (processor);
    const auto area = getLocalBounds().toFloat().reduced (1.0f);
    const auto axis = makeFreqAxis (services.getOptions(), area, processor.getChain().getSampleRate());
    if (valid && sameParams (now, shown) && axis.area == fx.area && axis.minHz == fx.minHz && axis.maxHz == fx.maxHz
        && axis.mel == fx.mel)
        return;
    shown = now;
    fx = axis;
    rebuild();
    repaint();
}

void ResponseView::rebuild()
{
    const auto plot = getLocalBounds().toFloat().reduced (1.0f);
    dy.area = snapEqZeroLine (plot.withTrimmedTop (18.0f).withTrimmedBottom (22.0f));
    const int x0 = (int) std::floor (fx.area.getX()), x1 = (int) std::ceil (fx.area.getRight());
    xs.clear();
    db.clear();
    curve.clear();
    for (int px = x0; px <= x1; ++px)
    {
        const float hz = fx.xToHz ((float) px);
        const float v = (float) ps::Tape::responseDb (shown, (double) hz);
        xs.push_back ((float) px);
        db.push_back (v);
        const float y = juce::jlimit (dy.area.getY() - 20.0f, dy.area.getBottom() + 20.0f, dy.dbToY (v));
        if (px == x0)
            curve.startNewSubPath ((float) px, y);
        else
            curve.lineTo ((float) px, y);
    }
    valid = true;
}

float ResponseView::shownDbAt (float hz) const { return (float) ps::Tape::responseDb (shown, (double) hz); }

void ResponseView::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    stereo::drawWell (g, b);
    if (! valid || xs.empty())
        return;

    drawFrequencyGrid (g, fx, dy, fx.area.getWidth() > 220.0f);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (dy.area.toNearestInt());
        fillEqBand (g, xs, db, dy, colour, true);
        strokeEqCurve (g, curve, colour);
    }

    // Head-bump marker: the peak of the low end (when there is one)
    float peakHz = 0.0f, peakDb = 0.0f;
    for (size_t i = 0; i < xs.size(); ++i)
    {
        const float hz = fx.xToHz (xs[i]);
        if (hz >= kBumpSearchLoHz && hz <= kBumpSearchHiHz && db[i] > peakDb)
        {
            peakDb = db[i];
            peakHz = hz;
        }
    }

    const auto small = font (11.0f, 600);
    g.setFont (small);
    if (peakDb > 0.3f)
    {
        const float x = fx.hzToX (peakHz), y = dy.dbToY (peakDb);
        g.setColour (colour);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y }));
        const auto text = "Bump " + fmt::db (peakDb, 1) + " at " + fmt::hz (peakHz);
        const float tw = textWidth (small, text) + 4.0f;
        auto box = juce::Rectangle<float> (tw, 16.0f).withCentre ({ x + tw * 0.5f + 8.0f, y - 14.0f });
        box = box.constrainedWithin (b.reduced (6.0f, 2.0f));
        g.setColour (Colours::text.withAlpha (0.9f));
        g.drawText (text, box, juce::Justification::centredLeft, false);
    }

    // High-end readout (top right)
    const float hi = shownDbAt (kHighReadoutHz);
    const auto hiText = "Highs " + fmt::db (std::abs (hi) < 0.05f ? 0.0f : hi, 1) + " at 16 kHz";
    g.setColour (Colours::textSubtle);
    g.drawText (hiText, juce::Rectangle<float> (b.getRight() - 170.0f, b.getY() + 2.0f, 162.0f, 16.0f),
                juce::Justification::centredRight, false);
    g.drawText ("Response", juce::Rectangle<float> (b.getX() + 34.0f, b.getY() + 2.0f, 120.0f, 16.0f),
                juce::Justification::centredLeft, false);
}

//==============================================================================
DriveKnob::DriveKnob (PureSheenProcessor& p, const juce::String& paramId, const juce::String& caption)
    : ParamKnob (p, paramId, caption), processor (p), id (paramId), captionText (caption),
      colour (moduleColour (ps::ModuleType::Tape))
{
    if (const auto* spec = stereo::specOf (paramId))
    {
        minValue = spec->minValue;
        maxValue = spec->maxValue;
    }
}

void DriveKnob::setColour (juce::Colour c)
{
    colour = c;
    ParamKnob::setColour (c);
}

void DriveKnob::paint (juce::Graphics& g)
{
    constexpr float kStart = -2.35f, kEnd = 2.35f;   // the shared knob's sweep
    const bool hover = isMouseOverOrDragging();
    auto r = getLocalBounds().toFloat();

    g.setFont (font (12.5f, 600));
    g.setColour (hover ? Colours::text : Colours::textDim);
    g.drawText (captionText, r.removeFromTop (18.0f), juce::Justification::centred, false);
    auto valueRow = r.removeFromBottom (20.0f);

    const auto scaleFont = font (10.5f, 600);
    const float labelPad = 22.0f;   // room for the scale labels around the arc
    const float d = juce::jmax (24.0f, juce::jmin (r.getWidth() - 2.0f * labelPad, r.getHeight() - labelPad));
    const auto area = juce::Rectangle<float> (d, d).withCentre ({ r.getCentreX(), r.getY() + labelPad * 0.5f + d * 0.5f });
    const auto centre = area.getCentre();
    const float radius = d * 0.5f;
    const float lineW = juce::jlimit (3.0f, 6.0f, radius * 0.09f);
    const float arcR = radius - lineW * 0.5f - 1.0f;

    const float v = processor.getParameterValue (id);
    const float norm = juce::jlimit (0.0f, 1.0f, (v - minValue) / juce::jmax (1.0e-6f, maxValue - minValue));
    const float angle = kStart + norm * (kEnd - kStart);
    const float originNorm = minValue < 0.0f && maxValue > 0.0f ? -minValue / (maxValue - minValue) : 0.0f;
    const float originAngle = kStart + originNorm * (kEnd - kStart);

    // scale: ticks and labels every 10 dB
    g.setFont (scaleFont);
    for (float t = minValue; t <= maxValue + 0.01f; t += 10.0f)
    {
        const float a = kStart + (t - minValue) / (maxValue - minValue) * (kEnd - kStart);
        const auto p0 = centre.getPointOnCircumference (radius + 2.0f, a);
        const auto p1 = centre.getPointOnCircumference (radius + 6.0f, a);
        g.setColour (Colours::textFaint);
        g.drawLine ({ p0, p1 }, 1.2f);
        const auto lp = centre.getPointOnCircumference (radius + 14.0f, a);
        const auto text = t > 0.0f ? "+" + juce::String ((int) t) : juce::String ((int) t);
        g.setColour (Colours::textSubtle);
        g.drawText (text, juce::Rectangle<float> (30.0f, 12.0f).withCentre (lp), juce::Justification::centred, false);
    }

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, kStart, kEnd, true);
    g.setColour (juce::Colours::black.withAlpha (0.28f));
    g.strokePath (track, juce::PathStrokeType (lineW + 1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (Colours::outline.brighter (hover ? 0.15f : 0.0f));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (std::abs (angle - originAngle) > 0.01f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin (originAngle, angle), juce::jmax (originAngle, angle),
                             true);
        const auto c = colour.brighter (hover ? 0.15f : 0.0f);
        g.setColour (c.withAlpha (0.25f));
        g.strokePath (value, juce::PathStrokeType (lineW + 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float bodyR = arcR - lineW * 0.5f - juce::jmax (3.0f, radius * 0.1f);
    if (bodyR > 4.0f)
    {
        const auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillEllipse (body.translated (0.0f, 2.0f));
        const auto top = hover ? Colours::panelHover.brighter (0.1f) : Colours::panelHover;
        g.setGradientFill (juce::ColourGradient::vertical (top, body.getY(), Colours::panelRaised.darker (0.12f), body.getBottom()));
        g.fillEllipse (body);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        const float inner = bodyR - juce::jmax (3.0f, bodyR * 0.16f);
        const auto p1 = centre.getPointOnCircumference (inner, angle);
        const auto p0 = centre.getPointOnCircumference (inner * 0.5f, angle);
        g.setColour (hover ? colour.brighter (0.3f) : Colours::text);
        g.drawLine ({ p0, p1 }, juce::jlimit (2.0f, 3.4f, bodyR * 0.07f));
    }

    g.setFont (font (15.0f, 700));
    g.setColour (Colours::text.withAlpha (hover ? 1.0f : 0.92f));
    g.drawText (fmt::paramValue (processor, id), valueRow, juce::Justification::centred, false);
}

//==============================================================================
SaturationMeter::SaturationMeter() : colour (moduleColour (ps::ModuleType::Tape))
{
    setInterceptsMouseClicks (true, false);
}

void SaturationMeter::setColour (juce::Colour c)
{
    colour = c;
    repaint();
}

void SaturationMeter::setValues (float driveDb, float saturationDb, bool valid)
{
    const float d = std::isfinite (driveDb) ? driveDb : -100.0f;
    const float s = std::isfinite (saturationDb) ? juce::jmax (0.0f, saturationDb) : 0.0f;
    if (std::abs (d - drive) < 0.05f && std::abs (s - saturation) < 0.02f && valid == active)
        return;
    drive = d;
    saturation = s;
    active = valid;
    repaint();
}

void SaturationMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const auto caption = font (11.0f, 600);
    const auto value = font (12.0f, 600);
    const float rowH = r.getHeight() * 0.5f;
    const float valueW = juce::jmin (64.0f, r.getWidth() * 0.32f);

    auto row = [&] (juce::Rectangle<float> area, const juce::String& name, float norm, juce::Colour fill,
                    const juce::String& text, float markNorm) {
        auto top = area.removeFromTop (15.0f);
        g.setFont (caption);
        g.setColour (Colours::textSubtle);
        g.drawText (name, top, juce::Justification::centredLeft, false);
        auto barRow = area.removeFromTop (juce::jmin (14.0f, area.getHeight()));
        auto valueBox = barRow.removeFromRight (valueW);
        barRow.removeFromRight (6.0f);
        stereo::drawWell (g, barRow, 4.0f);
        const auto inner = barRow.reduced (2.0f);
        if (active && norm > 0.0f)
        {
            g.setColour (fill);
            g.fillRoundedRectangle (inner.withWidth (inner.getWidth() * juce::jlimit (0.0f, 1.0f, norm)), 3.0f);
        }
        if (markNorm > 0.0f && markNorm < 1.0f)
        {
            g.setColour (Colours::textFaint);
            g.fillRect (juce::Rectangle<float> (inner.getX() + inner.getWidth() * markNorm - 0.5f, barRow.getY(), 1.0f,
                                                barRow.getHeight()));
        }
        g.setFont (value);
        g.setColour (active ? Colours::text : Colours::textFaint);
        g.drawText (active ? text : juce::String ("--"), valueBox, juce::Justification::centredRight, false);
    };

    // level into the tape: -36 .. +18 dBFS, mark at 0 dBFS; amber above 0
    const float dn = (drive + 36.0f) / 54.0f;
    const auto driveFill = drive > 0.0f ? blend (colour, Colours::minor, juce::jmin (1.0f, drive / 12.0f)) : colour;
    row (r.removeFromTop (rowH), "Level into tape", dn, driveFill.withAlpha (0.85f),
         drive <= -99.0f ? juce::String ("-inf") : fmt::db (drive, 1, false), 36.0f / 54.0f);
    // saturation: 0 .. 12 dB
    row (r, "Saturation", saturation / 12.0f, Colours::gainReduction.withAlpha (0.85f),
         juce::String (saturation, 1) + " dB", 0.0f);
}
} // namespace ps::ui::tape
