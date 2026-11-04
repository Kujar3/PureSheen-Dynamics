#include "ui/LowEndWidgets.h"

#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"

#include <cmath>

// Building blocks of the Low Contrast and Bass Shaper panels. Owner: ui-lowend. See LowEndWidgets.h.
namespace ps::ui::lowend
{
//==============================================================================
juce::RangedAudioParameter* beginSectionPower (PureSheenProcessor& p, const juce::String& powerId)
{
    auto* param = p.getParameter (powerId);

    if (param == nullptr || param->getValue() >= 0.5f)
        return nullptr;

    // The power switch joins the edit's gesture: both end on mouse up, so the processor records one undo step.
    param->beginChangeGesture();
    param->setValueNotifyingHost (1.0f);
    return param;
}

SectionKnob::SectionKnob (PureSheenProcessor& p, const juce::String& paramId, const juce::String& caption,
                          const juce::String& power)
    : ParamKnob (p, paramId, caption), processor (p), powerId (power)
{
}

SectionKnob::~SectionKnob()
{
    if (powerGesture != nullptr)
        powerGesture->endChangeGesture();
}

void SectionKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && powerGesture == nullptr && e.mouseWasDraggedSinceMouseDown())
        powerGesture = beginSectionPower (processor, powerId);

    ParamKnob::mouseDrag (e);
}

void SectionKnob::mouseUp (const juce::MouseEvent& e)
{
    ParamKnob::mouseUp (e);

    if (powerGesture != nullptr)
    {
        powerGesture->endChangeGesture();
        powerGesture = nullptr;
    }
}

SectionFader::SectionFader (PureSheenProcessor& p, const juce::String& paramId, const juce::String& caption,
                            const juce::String& power)
    : ParamFader (p, paramId, false, caption), processor (p), powerId (power)
{
}

SectionFader::~SectionFader()
{
    if (powerGesture != nullptr)
        powerGesture->endChangeGesture();
}

void SectionFader::mouseDrag (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && powerGesture == nullptr && e.mouseWasDraggedSinceMouseDown())
        powerGesture = beginSectionPower (processor, powerId);

    ParamFader::mouseDrag (e);
}

void SectionFader::mouseUp (const juce::MouseEvent& e)
{
    ParamFader::mouseUp (e);

    if (powerGesture != nullptr)
    {
        powerGesture->endChangeGesture();
        powerGesture = nullptr;
    }
}

//==============================================================================
juce::String hzText (float hz)
{
    if (hz < 999.5f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    return juce::String (hz / 1000.0f, 1) + " kHz";
}

void drawValueChip (juce::Graphics& g, juce::Rectangle<float> chip, const juce::String& text, juce::Colour c, bool active)
{
    g.setColour (Colours::background.withAlpha (0.92f));
    g.fillRoundedRectangle (chip, 5.0f);
    g.setColour (c.withAlpha (active ? 1.0f : 0.7f));
    g.drawRoundedRectangle (chip.reduced (0.5f), 5.0f, active ? 1.4f : 1.0f);
    g.setFont (font (11.0f, 700));
    g.setColour (active ? Colours::text : Colours::text.withAlpha (0.88f));
    g.drawText (text, chip, juce::Justification::centred, false);
}

void drawGrabHandle (juce::Graphics& g, juce::Rectangle<float> h, juce::Colour c, bool active, bool horizontalGrip)
{
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (h.translated (0.0f, 1.5f), 4.0f);
    g.setGradientFill (juce::ColourGradient::vertical (active ? c : blend (Colours::panelHover, c, 0.35f), h.getY(),
                                                       active ? c.darker (0.25f) : blend (Colours::panelRaised, c, 0.2f), h.getBottom()));
    g.fillRoundedRectangle (h, 4.0f);
    g.setColour (active ? c.brighter (0.3f) : c.withAlpha (0.9f));
    g.drawRoundedRectangle (h.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (active ? Colours::background.withAlpha (0.75f) : Colours::text.withAlpha (0.55f));

    for (int k = -1; k <= 1; ++k)
    {
        if (horizontalGrip)
            g.fillRect (juce::Rectangle<float> (h.getCentreX() + (float) k * 4.0f - 0.5f, h.getCentreY() - 2.5f, 1.0f, 5.0f));
        else
            g.fillRect (juce::Rectangle<float> (h.getCentreX() - 2.5f, h.getCentreY() + (float) k * 4.0f - 0.5f, 5.0f, 1.0f));
    }
}

//==============================================================================
SweetSpotMeter::SweetSpotMeter()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setInterceptsMouseClicks (false, false);
}

void SweetSpotMeter::setRange (float lo, float hi)
{
    if (! juce::approximatelyEqual (lo, minValue) || ! juce::approximatelyEqual (hi, maxValue))
    {
        minValue = lo;
        maxValue = juce::jmax (lo + 0.1f, hi);
        repaint();
    }
}

void SweetSpotMeter::setZone (float lo, float hi)
{
    if (std::abs (lo - zoneLow) > 0.01f || std::abs (hi - zoneHigh) > 0.01f)
    {
        zoneLow = lo;
        zoneHigh = hi;
        repaint();
    }
}

void SweetSpotMeter::setLabels (const juce::String& l, const juce::String& r)
{
    leftLabel = l;
    rightLabel = r;
    repaint();
}

void SweetSpotMeter::setColour (juce::Colour c)
{
    colour = c;
    repaint();
}

void SweetSpotMeter::setFormatter (std::function<juce::String (float)> f)
{
    formatter = std::move (f);
    repaint();
}

void SweetSpotMeter::setValues (float in, float out, bool isValid)
{
    in = std::isfinite (in) ? in : minValue;
    out = std::isfinite (out) ? out : minValue;
    bool changed = isValid != valid;
    valid = isValid;

    if (! primed && valid)
    {
        shownIn = in;
        shownOut = out;
        primed = true;
        changed = true;
    }
    else if (valid)
    {
        // A short glide on top of the meters' own (slow) ballistics: the ball moves smoothly at 30 Hz.
        const float nextIn = shownIn + 0.35f * (in - shownIn);
        const float nextOut = shownOut + 0.35f * (out - shownOut);
        changed = changed || std::abs (nextIn - shownIn) > 0.005f || std::abs (nextOut - shownOut) > 0.005f;
        shownIn = nextIn;
        shownOut = nextOut;
    }

    if (changed)
        repaint();
}

void SweetSpotMeter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const float trackH = 16.0f;
    const auto track = juce::Rectangle<float> (b.getX() + 2.0f, b.getY() + 2.0f, b.getWidth() - 4.0f, trackH);
    auto xOf = [&] (float v)
    {
        const float t = (juce::jlimit (minValue, maxValue, v) - minValue) / (maxValue - minValue);
        return track.getX() + 8.0f + t * (track.getWidth() - 16.0f);
    };

    // Track
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (track, trackH * 0.5f);
    g.setColour (Colours::outline.withAlpha (0.8f));
    g.drawRoundedRectangle (track.reduced (0.5f), trackH * 0.5f, 1.0f);

    // Ticks (every quarter of the span)
    g.setColour (Colours::textFaint.withAlpha (0.6f));

    for (int k = 1; k < 4; ++k)
    {
        const float x = std::round (track.getX() + 8.0f + (float) k / 4.0f * (track.getWidth() - 16.0f));
        g.fillRect (juce::Rectangle<float> (x, track.getY() + 4.0f, 1.0f, trackH - 8.0f));
    }

    // Target zone
    {
        const float z0 = xOf (zoneLow), z1 = xOf (zoneHigh);
        const auto zone = juce::Rectangle<float> (z0, track.getY() + 2.0f, juce::jmax (2.0f, z1 - z0), trackH - 4.0f);
        const auto zc = valid ? Colours::good : Colours::textFaint;
        g.setColour (zc.withAlpha (valid ? 0.22f : 0.12f));
        g.fillRoundedRectangle (zone, 4.0f);
        g.setColour (zc.withAlpha (valid ? 0.75f : 0.35f));
        g.drawRoundedRectangle (zone.reduced (0.5f), 4.0f, 1.0f);
    }

    const float cy = track.getCentreY();
    const float d = trackH - 4.0f;

    if (valid)
    {
        const bool inZone = shownIn >= zoneLow && shownIn <= zoneHigh;
        const auto ballColour = inZone ? Colours::good : colour;

        // Output: hollow ball
        if (std::abs (xOf (shownOut) - xOf (shownIn)) > 1.5f)
        {
            const auto r = juce::Rectangle<float> (d, d).withCentre ({ xOf (shownOut), cy });
            g.setColour (Colours::background.withAlpha (0.6f));
            g.fillEllipse (r);
            g.setColour (Colours::text.withAlpha (0.85f));
            g.drawEllipse (r.reduced (0.8f), 1.6f);
        }

        // Input: filled ball
        const auto r = juce::Rectangle<float> (d, d).withCentre ({ xOf (shownIn), cy });
        g.setColour (ballColour.withAlpha (0.3f));
        g.fillEllipse (r.expanded (3.0f));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (r.translated (0.0f, 1.0f));
        g.setGradientFill (juce::ColourGradient::vertical (ballColour.brighter (0.35f), r.getY(), ballColour.darker (0.1f), r.getBottom()));
        g.fillEllipse (r);
    }

    // Labels and the value
    auto labels = b.withTrimmedTop (trackH + 6.0f).withHeight (14.0f).reduced (4.0f, 0.0f);
    g.setFont (font (10.5f, 600));
    g.setColour (Colours::textSubtle);
    g.drawText (leftLabel, labels, juce::Justification::centredLeft, false);
    g.drawText (rightLabel, labels, juce::Justification::centredRight, false);

    if (formatter != nullptr)
    {
        g.setFont (font (11.5f, 700));
        g.setColour (valid ? Colours::text.withAlpha (0.92f) : Colours::textFaint);
        g.drawText (valid ? formatter (shownIn) : juce::String ("--"), labels, juce::Justification::centred, false);
    }
}

//==============================================================================
struct CutoffStrip::Impl
{
    static constexpr float kMinHz = 20.0f, kMaxHz = 1000.0f;

    Impl (CutoffStrip& o, PureSheenProcessor& p, EditorServices& s, ps::ModuleType t, const juce::String& id)
        : owner (o), processor (p), services (s), type (t), paramId (id), param (p.getParameter (id))
    {
        if (const auto* spec = stereo::specOf (id))
        {
            rangeLo = spec->minValue;
            rangeHi = spec->maxValue;
        }
    }

    ~Impl() { endGesture(); }

    juce::Rectangle<float> plot() const { return owner.getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (15.0f); }

    FreqAxis axis() const
    {
        FreqAxis a;
        a.minHz = kMinHz;
        a.maxHz = kMaxHz;
        a.area = plot();
        return a;
    }

    float value() const { return processor.getParameterValue (paramId); }

    juce::Rectangle<float> handle() const
    {
        const auto p = plot();
        return juce::Rectangle<float> (10.0f, juce::jmin (24.0f, p.getHeight() - 8.0f)).withCentre ({ axis().hzToX (value()), p.getCentreY() });
    }

    void setFromX (float x)
    {
        if (param == nullptr)
            return;

        const float hz = juce::jlimit (rangeLo, rangeHi, axis().xToHz (x));

        if (! gestureOpen)
        {
            gestureOpen = true;
            param->beginChangeGesture();
        }

        param->setValueNotifyingHost (param->convertTo0to1 (hz));
        owner.repaint();
    }

    void endGesture()
    {
        if (gestureOpen && param != nullptr)
            param->endChangeGesture();

        gestureOpen = false;
        dragging = false;
    }

    void paint (juce::Graphics& g)
    {
        const auto b = owner.getLocalBounds().toFloat();
        stereo::drawWell (g, b.withTrimmedBottom (14.0f), 6.0f);

        const auto p = plot();
        const auto fx = axis();
        const float hz = value();
        const float x = fx.hzToX (hz);
        const bool active = dragging || hover;

        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (p.toNearestInt());

            // Range the cutoff can take
            const float x0 = fx.hzToX (rangeLo), x1 = fx.hzToX (rangeHi);
            g.setColour (juce::Colours::white.withAlpha (0.025f));
            g.fillRect (juce::Rectangle<float> (x0, p.getY(), x1 - x0, p.getHeight()));

            // Grid
            for (float f : { 30.0f, 40.0f, 60.0f, 70.0f, 80.0f, 90.0f, 300.0f, 400.0f, 600.0f, 700.0f, 800.0f, 900.0f })
            {
                g.setColour (Colours::outline.withAlpha (0.2f));
                g.fillRect (juce::Rectangle<float> (std::round (fx.hzToX (f)), p.getY(), 1.0f, p.getHeight()));
            }

            for (float f : { 50.0f, 100.0f, 200.0f, 500.0f })
            {
                g.setColour (Colours::outline.withAlpha (0.45f));
                g.fillRect (juce::Rectangle<float> (std::round (fx.hzToX (f)), p.getY(), 1.0f, p.getHeight()));
            }

            // The module's input spectrum (its own scale, quiet)
            auto& pre = services.getModulePreSpectrum();

            if (pre.hasData())
                pre.draw (g, fx, -84.0f, 0.0f, Colours::input.withAlpha (0.35f), true);

            // The band below the cutoff
            const auto low = juce::Rectangle<float> (p.getX(), p.getY(), juce::jmax (0.0f, x - p.getX()), p.getHeight());
            g.setGradientFill (juce::ColourGradient::horizontal (colour.withAlpha (0.05f), low.getX(), colour.withAlpha (0.24f), low.getRight()));
            g.fillRect (low);

            if (low.getWidth() > 70.0f)
            {
                g.setFont (font (10.5f, 700).withExtraKerningFactor (0.06f));
                g.setColour (colour.brighter (0.4f).withAlpha (0.9f));
                g.drawText ("SHAPED", low.withTrimmedLeft (8.0f).withTrimmedRight (14.0f), juce::Justification::centredRight, false);
            }

            // Cutoff line
            if (active)
            {
                g.setColour (colour.withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (x - 3.0f, p.getY(), 6.0f, p.getHeight()));
            }

            g.setColour (colour.withAlpha (active ? 0.95f : 0.75f));
            g.fillRect (juce::Rectangle<float> (x - (active ? 1.0f : 0.7f), p.getY(), active ? 2.0f : 1.4f, p.getHeight()));
            drawGrabHandle (g, handle(), colour, active);

            // Value chip right of the handle (left when there is no room)
            const auto text = hzText (hz);
            const auto f = font (11.0f, 700);
            const float w = textWidth (f, text) + 14.0f;
            auto chip = juce::Rectangle<float> (w, 20.0f).withCentre ({ 0.0f, p.getCentreY() });
            chip.setX (x + 10.0f + w <= p.getRight() - 4.0f ? x + 10.0f : x - 10.0f - w);
            drawValueChip (g, chip, text, colour, active);
        }

        // Frequency labels under the strip
        g.setFont (font (10.0f, 600));
        g.setColour (Colours::textSubtle);

        for (float f : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f })
        {
            const float lx = fx.hzToX (f);
            const auto text = f >= 1000.0f ? juce::String ("1k") : juce::String (juce::roundToInt (f));
            auto r = juce::Rectangle<float> (40.0f, 13.0f).withCentre ({ lx, b.getBottom() - 6.5f });
            r = r.withX (juce::jlimit (b.getX(), b.getRight() - 40.0f, r.getX()));
            const auto just = f <= 20.0f ? juce::Justification::centredLeft
                                         : (f >= 1000.0f ? juce::Justification::centredRight : juce::Justification::centred);
            g.drawText (text, f <= 20.0f ? r.withX (b.getX() + 2.0f) : (f >= 1000.0f ? r.withRightX (b.getRight() - 2.0f) : r),
                        just, false);
        }
    }

    CutoffStrip& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    ps::ModuleType type;
    juce::String paramId;
    juce::RangedAudioParameter* param = nullptr;
    juce::Colour colour = Colours::accent;
    float rangeLo = 65.0f, rangeHi = 300.0f;
    float shownValue = -1.0f;
    float grabOffset = 0.0f, dragX = 0.0f, lastMouseX = 0.0f;
    bool gestureOpen = false, dragging = false, hover = false;
};

CutoffStrip::CutoffStrip (PureSheenProcessor& p, EditorServices& s, ps::ModuleType t, const juce::String& paramId)
    : impl (std::make_unique<Impl> (*this, p, s, t, paramId))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    const auto* spec = stereo::specOf (paramId);
    setTooltip ((spec != nullptr ? juce::String (spec->name) : paramId)
                + ": everything below this frequency is shaped.\nDrag to change (Ctrl: fine), double-click to type a value, "
                  "right-click for more.");
}

CutoffStrip::~CutoffStrip() = default;

void CutoffStrip::setColour (juce::Colour c)
{
    impl->colour = c;
    repaint();
}

void CutoffStrip::tick()
{
    const float v = impl->value();

    if (std::abs (v - impl->shownValue) > 1.0e-3f)
    {
        impl->shownValue = v;
        repaint();
    }
}

float CutoffStrip::xForFrequency (float hz) const noexcept { return impl->axis().hzToX (hz); }

void CutoffStrip::paint (juce::Graphics& g) { impl->paint (g); }

void CutoffStrip::mouseMove (const juce::MouseEvent& e)
{
    const bool h = impl->plot().contains (e.position);

    if (h != impl->hover)
    {
        impl->hover = h;
        repaint();
    }
}

void CutoffStrip::mouseExit (const juce::MouseEvent&)
{
    if (impl->hover)
    {
        impl->hover = false;
        repaint();
    }
}

void CutoffStrip::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e))
    {
        if (e.mods.isPopupMenu())
            stereo::showParamMenu (*this, d.processor, d.paramId, {}, "Cutoff: " + hzText (d.value()));

        return;
    }

    if (e.getNumberOfClicks() >= 2)
        return;

    const float x = d.axis().hzToX (d.value());
    d.dragging = true;
    d.lastMouseX = e.position.x;

    // Grabbing the handle keeps its offset; a click elsewhere moves the cutoff there (same gesture).
    if (std::abs (e.position.x - x) <= 8.0f)
    {
        d.dragX = x;
    }
    else
    {
        d.dragX = e.position.x;
        d.setFromX (d.dragX);
    }

    repaint();
}

void CutoffStrip::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! d.dragging || ! isLeftClick (e))
        return;

    const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();
    const float dx = e.position.x - d.lastMouseX;
    d.lastMouseX = e.position.x;

    if (std::abs (dx) < 1.0e-4f)
        return;

    const auto fx = d.axis();
    d.dragX += dx * (fine ? 0.12f : 1.0f);
    d.dragX = juce::jlimit (fx.hzToX (d.rangeLo), fx.hzToX (d.rangeHi), d.dragX);
    d.setFromX (d.dragX);
}

void CutoffStrip::mouseUp (const juce::MouseEvent&)
{
    impl->endGesture();
    repaint();
}

void CutoffStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! isLeftClick (e))
        return;

    impl->endGesture();
    stereo::promptParamValue (*this, impl->processor, impl->paramId, {}, "Cutoff");
    repaint();
}

//==============================================================================
LevelBar::LevelBar (const juce::String& c, juce::Colour col) : caption (c), colour (col)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setInterceptsMouseClicks (false, false);
}

void LevelBar::setColour (juce::Colour c)
{
    colour = c;
    repaint();
}

void LevelBar::setLevel (float db, bool isValid)
{
    db = std::isfinite (db) ? juce::jlimit (-60.0f, 0.0f, db) : -60.0f;
    const float next = isValid ? shown + 0.4f * (db - shown) : -60.0f;

    if (isValid != valid || std::abs (next - shown) > 0.02f)
    {
        valid = isValid;
        shown = next;
        repaint();
    }
}

void LevelBar::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    auto top = b.removeFromTop (16.0f);
    g.setFont (font (11.0f, 600));
    g.setColour (Colours::textDim);
    g.drawText (caption, top, juce::Justification::centredLeft, false);
    g.setFont (font (11.5f, 700));
    g.setColour (valid && shown > -59.5f ? Colours::text.withAlpha (0.92f) : Colours::textFaint);
    g.drawText (valid && shown > -59.5f ? fmt::db (shown, 1, false) : juce::String ("--"), top, juce::Justification::centredRight, false);

    const auto track = b.withSizeKeepingCentre (b.getWidth(), 5.0f).withY (b.getY() + 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (track, 2.5f);

    if (valid && shown > -59.5f)
    {
        const float t = (shown + 60.0f) / 60.0f;
        g.setGradientFill (juce::ColourGradient::horizontal (colour.withAlpha (0.45f), track.getX(), colour.brighter (0.3f), track.getRight()));
        g.fillRoundedRectangle (track.withWidth (juce::jmax (4.0f, track.getWidth() * t)), 2.5f);
    }
}

} // namespace ps::ui::lowend
