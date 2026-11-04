#include "ui/DeLimiterViews.h"

#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"

#include <cmath>

// De-limiter panel building blocks (mod-dlim). See DeLimiterViews.h.
namespace ps::ui::dlim
{
namespace
{
constexpr float kScaleTop = 9.0f, kScaleBottom = -12.0f;       // the display's dB scale
constexpr float kPixelsForFullRange = 220.0f;                  // as ParamKnob
constexpr float kLineHitPx = 8.0f;
constexpr const char* kThresholdId = "dlim.thr";

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}
} // namespace

//==============================================================================
// LearnButton
//==============================================================================
LearnButton::LearnButton (juce::Colour c) : accent (c)
{
    noFocus (*this);
    setTitle ("Learn Threshold");
    setTooltip ("Learn Threshold: click, then play the song. From the first sound it listens for 6 seconds and sets "
                "Threshold to the highest peak it heard. Play a loud part for the best result. Click again to cancel.");
}

void LearnButton::setState (State s, float p)
{
    p = juce::jlimit (0.0f, 1.0f, p);
    if (s != state || std::abs (p - progress) > 0.002f)
    {
        state = s;
        progress = p;
        repaint();
    }
}

void LearnButton::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    const float radius = b.getHeight() * 0.5f;
    const bool active = state != State::idle;

    g.setColour (isMouseOver() && ! active ? Colours::panelHover : Colours::panelRaised);
    g.fillRoundedRectangle (b, radius);

    if (state == State::listening)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (b.withWidth (b.getWidth() * progress).getSmallestIntegerContainer());
        g.setColour (accent.withAlpha (0.55f));
        g.fillRoundedRectangle (b, radius);
    }

    g.setColour (active ? accent : accent.withAlpha (isMouseOver() ? 0.9f : 0.55f));
    g.drawRoundedRectangle (b, radius, 1.2f);

    const juce::String text = state == State::idle ? "Learn Threshold"
                              : state == State::waiting ? "Waiting for audio..."
                                                        : "Listening " + juce::String (juce::roundToInt (progress * 100.0f)) + " %";
    g.setFont (font (12.0f, 600));
    g.setColour (Colours::text);
    g.drawFittedText (text, getLocalBounds(), juce::Justification::centred, 1, 0.8f);
}

void LearnButton::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()) && onClick)
        onClick();
}

//==============================================================================
// Display
//==============================================================================
Display::Display (PureSheenProcessor& p, juce::Colour c) : processor (p), accent (c)
{
    noFocus (*this);
    threshold = p.getParameter (kThresholdId);
    shownThreshold = p.getParameterValue (kThresholdId);
    setTooltip ("Grey: the input after Input Gain. Colour: how much the module is lifting it, up from 0 dB. The white line "
                "is the Threshold, the level where the song is assumed to have been limited: below the song's peaks the "
                "effect is exaggerated, above them it is gentler. Drag it (Ctrl: fine), double-click to type a value, "
                "right-click to reset.");
}

Display::~Display()
{
    endDrag();
}

void Display::push (float inputDb, float liftDb)
{
    auto& col = ring[(size_t) head];
    col.input = std::isfinite (inputDb) ? inputDb : -120.0f;
    col.lift = std::isfinite (liftDb) ? juce::jmax (0.0f, liftDb) : 0.0f;
    head = (head + 1) % kCapacity;
    count = juce::jmin (count + 1, kCapacity);
    repaint();
}

void Display::setReadout (float held, bool valid)
{
    held = std::isfinite (held) ? juce::jmax (0.0f, held) : 0.0f;
    if (valid != readoutValid || std::abs (held - readout) > 0.04f)
    {
        readout = held;
        readoutValid = valid;
        repaint();
    }
}

void Display::update()
{
    const float t = processor.getParameterValue (kThresholdId);
    if (! juce::approximatelyEqual (t, shownThreshold))
    {
        shownThreshold = t;
        repaint();
    }
}

const Display::Column& Display::columnAt (int age) const noexcept
{
    return ring[(size_t) ((head - 1 - age + 2 * kCapacity) % kCapacity)];
}

juce::Rectangle<float> Display::plotArea() const
{
    auto r = getLocalBounds().toFloat().reduced (10.0f);
    r.removeFromLeft (24.0f);   // the scale
    return r;
}

float Display::dbToY (float db) const noexcept
{
    const auto plot = plotArea();
    const float top = plot.getY() + 6.0f, bottom = plot.getBottom() - 6.0f;
    return top + (kScaleTop - juce::jlimit (kScaleBottom, kScaleTop, db)) / (kScaleTop - kScaleBottom) * (bottom - top);
}

float Display::yToDb (float y) const noexcept
{
    const auto plot = plotArea();
    const float top = plot.getY() + 6.0f, bottom = plot.getBottom() - 6.0f;
    return kScaleTop - (y - top) / juce::jmax (1.0f, bottom - top) * (kScaleTop - kScaleBottom);
}

bool Display::hitsLine (juce::Point<float> p) const noexcept
{
    return plotArea().contains (p) && std::abs (p.y - dbToY (shownThreshold)) <= kLineHitPx;
}

void Display::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    drawPanel (g, bounds.reduced (0.5f));
    const auto plot = plotArea();
    if (plot.getWidth() < 60.0f || plot.getHeight() < 40.0f)
        return;

    stereo::drawWell (g, plot, 7.0f);

    // The scale, every 3 dB
    g.setFont (font (10.5f, 400));
    for (float db = kScaleBottom; db <= kScaleTop + 0.01f; db += 3.0f)
    {
        const float y = std::round (dbToY (db)) + 0.5f;
        g.setColour (Colours::outline.withAlpha (db == 0.0f ? 0.8f : 0.25f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f));
        g.setColour (Colours::textSubtle);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (bounds.getX() + 6.0f, y - 7.0f, plot.getX() - bounds.getX() - 10.0f, 14.0f),
                    juce::Justification::centredRight, false);
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (plot.reduced (1.0f), 6.0f);
        g.reduceClipRegion (clip);

        const float right = plot.getRight() - 1.0f, colW = juce::jmax (1.5f, plot.getWidth() / (float) kCapacity * 2.0f);
        const int n = juce::jmin (count, (int) std::ceil (plot.getWidth() / colW) + 2);

        if (n >= 2)
        {
            // Input, up from the bottom of the scale
            const float base = plot.getBottom();
            juce::Path input;
            input.startNewSubPath (right, base);
            for (int a = 0; a < n; ++a)
                input.lineTo (right - (float) a * colW, dbToY (columnAt (a).input));
            input.lineTo (right - (float) (n - 1) * colW, base);
            input.closeSubPath();
            g.setColour (Colours::textFaint.withAlpha (0.28f));
            g.fillPath (input);

            // The lift, up from 0 dB
            const float y0 = dbToY (0.0f);
            juce::Path fill, line;
            fill.startNewSubPath (right, y0);
            for (int a = 0; a < n; ++a)
            {
                const float x = right - (float) a * colW, y = dbToY (columnAt (a).lift);
                fill.lineTo (x, y);
                if (a == 0)
                    line.startNewSubPath (x, y);
                else
                    line.lineTo (x, y);
            }
            fill.lineTo (right - (float) (n - 1) * colW, y0);
            fill.closeSubPath();
            g.setColour (accent.withAlpha (0.28f));
            g.fillPath (fill);
            g.setColour (accent);
            g.strokePath (line, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));
        }
    }

    // The Threshold line, its handle and its value
    {
        const float y = dbToY (shownThreshold);
        const bool lit = hover || dragging;
        g.setColour (juce::Colours::white.withAlpha (lit ? 1.0f : 0.8f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), y - 0.75f, plot.getWidth(), lit ? 2.0f : 1.5f));

        const auto handleFont = font (11.0f, 600);
        const float w = textWidth (handleFont, "Threshold") + 22.0f;
        const auto handle = juce::Rectangle<float> (w, 18.0f).withCentre ({ plot.getCentreX(), y });
        g.setColour (Colours::background);
        g.fillRoundedRectangle (handle, 9.0f);
        g.setColour (juce::Colours::white.withAlpha (lit ? 1.0f : 0.8f));
        g.drawRoundedRectangle (handle.reduced (0.5f), 9.0f, 1.0f);
        g.setFont (handleFont);
        g.drawText ("Threshold", handle, juce::Justification::centred, false);

        g.setFont (font (11.0f, 600));
        const float textY = y < plot.getY() + 22.0f ? y + 3.0f : y - 17.0f;
        g.drawText (fmt::db (shownThreshold, 1, false), juce::Rectangle<float> (plot.getX() + 6.0f, textY, 70.0f, 14.0f),
                    juce::Justification::centredLeft, false);
    }

    // The readout: the largest lift, held 1 s
    {
        const auto box = juce::Rectangle<float> (64.0f, 40.0f).withPosition (plot.getRight() - 72.0f, plot.getY() + 8.0f);
        g.setColour (Colours::background.withAlpha (0.8f));
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (Colours::outline);
        g.drawRoundedRectangle (box.reduced (0.5f), 5.0f, 1.0f);
        g.setColour (readoutValid ? accent : Colours::textFaint);
        g.setFont (font (15.0f, 600));
        g.drawFittedText (readoutValid ? "+" + juce::String (readout, 1) : juce::String ("--"),
                          box.withHeight (25.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
        g.setFont (font (10.5f, 400));
        g.drawText ("dB", box.withTrimmedTop (23.0f), juce::Justification::centred, false);
    }
}

void Display::mouseMove (const juce::MouseEvent& e)
{
    const bool now = hitsLine (e.position);
    if (now != hover)
    {
        hover = now;
        setMouseCursor (hover ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void Display::mouseExit (const juce::MouseEvent&)
{
    if (hover)
    {
        hover = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void Display::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (hitsLine (e.position))
            showMenu();
        return;
    }
    if (! isLeftClick (e) || threshold == nullptr || ! hitsLine (e.position))
        return;
    dragging = true;
    dragValue = shownThreshold;
    dragStartY = e.position.y;
    threshold->beginChangeGesture();
}

void Display::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || threshold == nullptr)
        return;
    const float dy = e.position.y - dragStartY;
    dragStartY = e.position.y;
    const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
    const float dbPerPixel = (kScaleTop - kScaleBottom) / juce::jmax (40.0f, plotArea().getHeight() - 12.0f);
    dragValue = juce::jlimit (kScaleBottom, 0.0f, dragValue - dy * (fine ? dbPerPixel * 0.1f : dbPerPixel));
    threshold->setValueNotifyingHost (threshold->convertTo0to1 (dragValue));
    update();
}

void Display::mouseUp (const juce::MouseEvent&)
{
    endDrag();
}

void Display::endDrag()
{
    if (dragging)
    {
        dragging = false;
        if (threshold != nullptr)
            threshold->endChangeGesture();
    }
}

void Display::mouseDoubleClick (const juce::MouseEvent& e)
{
    endDrag();
    if (isLeftClick (e) && hitsLine (e.position))
        promptValue();
}

void Display::showMenu()
{
    const auto* spec = stereo::specOf (kThresholdId);
    if (spec == nullptr)
        return;
    const float def = spec->defaultValue;
    juce::PopupMenu menu;
    menu.addSectionHeader ("Threshold");
    menu.addItem (1, "Reset to default (" + fmt::db (def, 1, false) + ")");
    menu.addItem (2, "Enter value...");
    ps::ui::showMenu (menu, *this, [this, def] (int result)
    {
        if (result == 1)
            processor.setParameterValue (kThresholdId, def);
        else if (result == 2)
            promptValue();
    });
}

void Display::promptValue()
{
    const auto* spec = stereo::specOf (kThresholdId);
    if (spec == nullptr)
        return;
    juce::Component::SafePointer<juce::Component> safe (this);
    promptForText (*this, "Threshold", "Enter a value (-12 to 0 dB)", fmt::paramEditValue (processor, kThresholdId),
                   [this, safe, spec] (juce::String text)
                   {
                       float v = 0.0f;
                       if (safe != nullptr && parseValueText (*spec, text, v))
                           processor.setParameterValue (kThresholdId, juce::jlimit (spec->minValue, spec->maxValue, v));
                   });
}
} // namespace ps::ui::dlim
