#include "ui/Controls.h"

namespace ps::ui
{
namespace
{
const juce::Colour kLavender { 0xffb9a6f5 };

juce::Path dropletPath (juce::Rectangle<float> a)
{
    auto pt = [a] (float x, float y) { return juce::Point<float> (a.getX() + x * a.getWidth(), a.getY() + y * a.getHeight()); };
    juce::Path p;
    p.startNewSubPath (pt (0.5f, 0.03f));
    p.cubicTo (pt (0.40f, 0.19f), pt (0.15f, 0.40f), pt (0.15f, 0.62f));
    p.cubicTo (pt (0.15f, 0.81f), pt (0.31f, 0.97f), pt (0.5f, 0.97f));
    p.cubicTo (pt (0.69f, 0.97f), pt (0.85f, 0.81f), pt (0.85f, 0.62f));
    p.cubicTo (pt (0.85f, 0.40f), pt (0.60f, 0.19f), pt (0.5f, 0.03f));
    p.closeSubPath();
    return p;
}
} // namespace

//==============================================================================
void drawLogo (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto a = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), area.getHeight()),
                                               juce::jmin (area.getWidth(), area.getHeight()));
    const float s = a.getWidth();
    const auto drop = dropletPath (a);

    // Soft contact shadow + glow.
    juce::DropShadow (Colours::accent.withAlpha (0.28f), juce::roundToInt (s * 0.28f), { 0, 0 }).drawForPath (g, drop);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillPath (drop, juce::AffineTransform::translation (0.0f, s * 0.03f));

    // Body: bright mint top-left to a deeper teal bottom-right.
    g.setGradientFill (juce::ColourGradient (Colours::accent.brighter (0.35f), a.getX() + s * 0.30f, a.getY() + s * 0.25f,
                                             juce::Colour (0xff2f8f7b), a.getX() + s * 0.80f, a.getBottom(), false));
    g.fillPath (drop);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (drop);

        // Lavender sheen sweeping across the lower right.
        juce::ColourGradient sheen (kLavender.withAlpha (0.0f), a.getX() + s * 0.35f, a.getY() + s * 0.55f,
                                    kLavender.withAlpha (0.0f), a.getX() + s * 0.95f, a.getY() + s * 0.95f, false);
        sheen.addColour (0.55, kLavender.withAlpha (0.75f));
        g.setGradientFill (sheen);
        g.fillEllipse (a.getX() + s * 0.30f, a.getY() + s * 0.48f, s * 0.80f, s * 0.60f);

        // Inner shade at the bottom for depth.
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), a.getCentreX(), a.getY() + s * 0.6f,
                                                 juce::Colours::black.withAlpha (0.22f), a.getCentreX(), a.getBottom(), false));
        g.fillRect (a);
    }

    // Glossy highlight: a curved streak along the upper-left edge and a small specular dot.
    juce::Path streak;
    streak.startNewSubPath (a.getX() + s * 0.30f, a.getY() + s * 0.70f);
    streak.quadraticTo (a.getX() + s * 0.27f, a.getY() + s * 0.48f, a.getX() + s * 0.43f, a.getY() + s * 0.27f);
    g.setColour (juce::Colours::white.withAlpha (0.78f));
    g.strokePath (streak, juce::PathStrokeType (juce::jmax (1.2f, s * 0.075f), juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.fillEllipse (juce::Rectangle<float> (s * 0.075f, s * 0.075f).withCentre ({ a.getX() + s * 0.31f, a.getY() + s * 0.79f }));

    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.strokePath (drop, juce::PathStrokeType (juce::jmax (0.8f, s * 0.025f)));
}

void drawLed (juce::Graphics& g, juce::Rectangle<float> area, bool on, juce::Colour onColour)
{
    const auto d = juce::jmin (area.getWidth(), area.getHeight());
    const auto r = area.withSizeKeepingCentre (d, d);

    if (on)
    {
        g.setColour (onColour.withAlpha (0.28f));
        g.fillEllipse (r.expanded (d * 0.45f));
        g.setGradientFill (juce::ColourGradient (onColour.brighter (0.5f), r.getX(), r.getY(), onColour.darker (0.1f),
                                                 r.getRight(), r.getBottom(), false));
        g.fillEllipse (r);
    }
    else
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (r);
        g.setColour (Colours::outline.brighter (0.25f));
        g.drawEllipse (r.reduced (0.5f), 1.0f);
    }
}

void drawChip (juce::Graphics& g, juce::Rectangle<float> b, float radius, juce::Colour fill, juce::Colour outline)
{
    g.setGradientFill (juce::ColourGradient::vertical (fill.brighter (0.04f), b.getY(), fill.darker (0.05f), b.getBottom()));
    g.fillRoundedRectangle (b, radius);
    g.setColour (outline);
    g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);
}

//==============================================================================
IconButton::IconButton (const juce::String& name, Icon iconType) : juce::Button (name), icon (iconType)
{
    setTitle (name);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool enabled = isEnabled();
    const bool on = getToggleState();
    highlighted = highlighted && enabled;
    down = down && enabled;

    if (highlighted || down || on)
    {
        g.setColour (Colours::panelHover.withAlpha (down ? 1.0f : 0.85f));

        if (roundBackground)
        {
            g.fillEllipse (b);
        }
        else
        {
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (on ? onColour.withAlpha (0.45f) : Colours::outline.brighter (0.1f));
            g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
        }
    }

    auto colour = on ? onColour : (highlighted ? Colours::text : Colours::textDim);

    if (! enabled)
        colour = Colours::textFaint.withAlpha (0.6f);

    g.setColour (colour);
    paintIcon (g, icon, b.withSizeKeepingCentre (16.0f, 16.0f));
}

void IconButton::paintIcon (juce::Graphics& g, Icon type, juce::Rectangle<float> box)
{
    const float x0 = box.getX(), y0 = box.getY();
    const auto c = box.getCentre();
    const juce::PathStrokeType stroke (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path p;

    auto chevron = [&] (bool left)
    {
        const float d = left ? 1.0f : -1.0f;
        p.startNewSubPath (c.x + 2.2f * d, c.y - 4.5f);
        p.lineTo (c.x - 2.2f * d, c.y);
        p.lineTo (c.x + 2.2f * d, c.y + 4.5f);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };

    auto curvedArrow = [&] (bool clockwise)
    {
        // Half-circle arrow ("undo" points left, "redo" points right).
        const float d = clockwise ? -1.0f : 1.0f;
        p.startNewSubPath (c.x + 5.5f * d, c.y + 4.5f);
        p.cubicTo (c.x + 6.5f * d, c.y - 1.0f, c.x + 1.0f * d, c.y - 5.0f, c.x - 4.5f * d, c.y - 2.0f);
        g.strokePath (p, stroke);
        juce::Path head;
        head.startNewSubPath (c.x - 4.0f * d, c.y - 6.2f);
        head.lineTo (c.x - 5.0f * d, c.y - 1.6f);
        head.lineTo (c.x - 0.6f * d, c.y - 0.4f);
        g.strokePath (head, stroke);
    };

    switch (type)
    {
        case Icon::help:
        {
            const auto circle = box.reduced (0.5f);
            g.drawEllipse (circle, 1.4f);
            g.setFont (font (11.0f, 700));
            g.drawText ("?", circle.translated (0.0f, 0.2f), juce::Justification::centred, false);
            break;
        }

        case Icon::options:
            for (int i = 0; i < 3; ++i)
                g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ x0 + 3.5f + 4.5f * (float) i, y0 + 8.0f }));
            break;

        case Icon::sliders:
            for (int i = 0; i < 3; ++i)
            {
                const float y = y0 + 3.5f + 4.5f * (float) i;
                p.startNewSubPath (x0 + 1.5f, y);
                p.lineTo (x0 + 14.5f, y);
                const float knobX = x0 + (i == 0 ? 10.5f : (i == 1 ? 5.0f : 9.0f));
                g.fillEllipse (juce::Rectangle<float> (4.4f, 4.4f).withCentre ({ knobX, y }));
            }
            g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;

        case Icon::close:
        {
            const float s = 4.0f;
            p.startNewSubPath (c.x - s, c.y - s);
            p.lineTo (c.x + s, c.y + s);
            p.startNewSubPath (c.x + s, c.y - s);
            p.lineTo (c.x - s, c.y + s);
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        case Icon::reset:
        {
            p.addCentredArc (c.x, c.y, 5.6f, 5.6f, 0.0f, -2.2f, 2.6f, true);
            g.strokePath (p, stroke);
            const auto tip = c.getPointOnCircumference (5.6f, -2.2f);
            juce::Path head;
            head.startNewSubPath (tip.x - 3.6f, tip.y - 0.6f);
            head.lineTo (tip.x, tip.y);
            head.lineTo (tip.x + 0.2f, tip.y - 3.6f);
            g.strokePath (head, stroke);
            break;
        }

        case Icon::undo: curvedArrow (false); break;
        case Icon::redo: curvedArrow (true); break;

        case Icon::history:
        {
            g.drawEllipse (box.reduced (1.5f), 1.4f);
            p.startNewSubPath (c.x, c.y - 4.0f);
            p.lineTo (c.x, c.y);
            p.lineTo (c.x + 3.0f, c.y + 2.0f);
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        case Icon::prev: chevron (true); break;
        case Icon::next: chevron (false); break;

        case Icon::save:
        {
            // Tray with a down arrow.
            p.startNewSubPath (c.x, c.y - 6.0f);
            p.lineTo (c.x, c.y + 2.5f);
            p.startNewSubPath (c.x - 3.5f, c.y - 1.0f);
            p.lineTo (c.x, c.y + 2.5f);
            p.lineTo (c.x + 3.5f, c.y - 1.0f);
            p.startNewSubPath (c.x - 6.0f, c.y + 2.0f);
            p.lineTo (c.x - 6.0f, c.y + 6.0f);
            p.lineTo (c.x + 6.0f, c.y + 6.0f);
            p.lineTo (c.x + 6.0f, c.y + 2.0f);
            g.strokePath (p, stroke);
            break;
        }

        case Icon::menu:
            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle (juce::Rectangle<float> (12.0f, 1.6f).withCentre ({ c.x, y0 + 4.0f + 4.0f * (float) i }), 0.8f);
            break;

        case Icon::plus:
            p.startNewSubPath (c.x - 5.0f, c.y);
            p.lineTo (c.x + 5.0f, c.y);
            p.startNewSubPath (c.x, c.y - 5.0f);
            p.lineTo (c.x, c.y + 5.0f);
            g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;

        case Icon::link:
        {
            const auto r1 = juce::Rectangle<float> (7.5f, 5.0f).withCentre ({ c.x - 2.6f, c.y + 1.4f });
            const auto r2 = juce::Rectangle<float> (7.5f, 5.0f).withCentre ({ c.x + 2.6f, c.y - 1.4f });
            juce::Path a, b;
            a.addRoundedRectangle (r1, 2.5f);
            b.addRoundedRectangle (r2, 2.5f);
            const auto rot = juce::AffineTransform::rotation (-0.6f, c.x, c.y);
            g.strokePath (a, juce::PathStrokeType (1.4f), rot);
            g.strokePath (b, juce::PathStrokeType (1.4f), rot);
            break;
        }

        case Icon::power:
            p.addCentredArc (c.x, c.y + 0.6f, 5.4f, 5.4f, 0.0f, 0.65f, juce::MathConstants<float>::twoPi - 0.65f, true);
            p.startNewSubPath (c.x, c.y - 6.0f);
            p.lineTo (c.x, c.y - 0.5f);
            g.strokePath (p, stroke);
            break;
    }
}

//==============================================================================
PillButton::PillButton (const juce::String& t) : text (t)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void PillButton::setText (const juce::String& t)
{
    if (t != text)
    {
        text = t;
        repaint();
    }
}

void PillButton::setOn (bool shouldBeOn)
{
    if (shouldBeOn != on)
    {
        on = shouldBeOn;
        repaint();
    }
}

int PillButton::getIdealWidth (float height) const
{
    return (int) std::ceil (textWidth (font (juce::jmin (12.0f, height * 0.5f), 600), text) + height * 0.9f + 4.0f);
}

void PillButton::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = square ? 4.0f : b.getHeight() * 0.5f;
    const bool hover = isMouseOver (true) && isEnabled();
    const bool enabled = isEnabled();

    juce::Colour fill = Colours::panelRaised, outline = Colours::outline;

    if (on)
    {
        fill = filledWhenOn ? onColour.withAlpha (0.9f) : blend (Colours::panelRaised, onColour, 0.22f);
        outline = onColour.withAlpha (0.75f);
    }
    else if (hover)
    {
        fill = Colours::panelHover;
        outline = Colours::outline.brighter (0.2f);
    }

    if (! enabled)
        fill = fill.withMultipliedAlpha (0.5f), outline = outline.withMultipliedAlpha (0.5f);

    drawChip (g, b, radius, fill, outline);

    auto textColour = on ? (filledWhenOn ? Colours::background : onColour.brighter (0.35f))
                         : (hover ? Colours::text : Colours::textDim.brighter (0.1f));

    if (! enabled)
        textColour = Colours::textFaint;

    g.setColour (textColour);
    g.setFont (font (juce::jmin (12.0f, b.getHeight() * 0.52f), 600));
    g.drawFittedText (text, b.reduced (juce::jmin (6.0f, b.getHeight() * 0.3f), 0.0f).toNearestInt(),
                      juce::Justification::centred, 1, 0.75f);
}

void PillButton::mouseUp (const juce::MouseEvent& e)
{
    if (! isEnabled() || ! getLocalBounds().contains (e.getPosition()))
        return;

    if (e.mods.isPopupMenu())
    {
        if (onRightClick != nullptr)
            onRightClick();
        return;
    }

    if (isLeftClick (e) && onClick != nullptr)
        onClick();
}

//==============================================================================
MenuChip::MenuChip()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void MenuChip::setText (const juce::String& prefix, const juce::String& value)
{
    if (prefix == prefixText && value == valueText)
        return;

    prefixText = prefix;
    valueText = value;
    repaint();
}

int MenuChip::getIdealWidth() const
{
    const float prefixW = prefixText.isEmpty() ? 0.0f : textWidth (font (11.5f, 600), prefixText) + 6.0f;
    return (int) std::ceil (12.0f + prefixW + textWidth (font (12.5f, 600), valueText) + 26.0f);
}

void MenuChip::setMenuOpen (bool open)
{
    menuOpen = open;
    repaint();
}

void MenuChip::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (8.0f, b.getHeight() * 0.5f);
    const bool hover = isMouseOver (true) && isEnabled();

    const auto fill = menuOpen ? Colours::panelHover : (hover ? Colours::panelHover.withAlpha (0.85f) : Colours::panelRaised);
    drawChip (g, b, radius, fill, menuOpen ? Colours::accent.withAlpha (0.7f) : Colours::outline.brighter (hover ? 0.18f : 0.0f));

    auto text = b.withTrimmedLeft (12.0f).withTrimmedRight (24.0f);

    if (prefixText.isNotEmpty())
    {
        const auto f = font (11.5f, 600);
        const float w = juce::jmin (text.getWidth() * 0.5f, textWidth (f, prefixText) + 1.0f);
        g.setFont (f);
        g.setColour (Colours::textDim);
        g.drawText (prefixText, text.removeFromLeft (w), juce::Justification::centredLeft, false);
        text.removeFromLeft (6.0f);
    }

    g.setFont (font (12.5f, 600));
    g.setColour (valueColour.withMultipliedAlpha (hover || menuOpen ? 1.0f : 0.92f));
    g.drawFittedText (valueText, text.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

    const float cx = b.getRight() - 13.0f, cy = b.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.2f, cy - 1.5f);
    chevron.lineTo (cx, cy + 1.7f);
    chevron.lineTo (cx + 3.2f, cy - 1.5f);
    g.setColour (hover || menuOpen ? Colours::text : Colours::textDim);
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void MenuChip::mouseDown (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && isEnabled() && ! menuOpen && onOpenMenu != nullptr)
        onOpenMenu (*this);
}

//==============================================================================
void showMenu (juce::PopupMenu& menu, juce::Component& target, std::function<void (int)> done, int minWidth)
{
    menu.setLookAndFeel (&target.getLookAndFeel());
    auto options = juce::PopupMenu::Options().withTargetComponent (&target);

    if (minWidth > 0)
        options = options.withMinimumWidth (minWidth);

    menu.showMenuAsync (options, [safe = juce::Component::SafePointer<juce::Component> (&target),
                                  done = std::move (done)] (int result)
                        {
                            if (safe != nullptr && done != nullptr)
                                done (result);
                        });
}

void promptForText (juce::Component& parent, const juce::String& title, const juce::String& message,
                    const juce::String& initial, std::function<void (juce::String)> done)
{
    auto* window = new juce::AlertWindow (title, message, juce::MessageBoxIconType::NoIcon, &parent);
    window->setLookAndFeel (&parent.getLookAndFeel());
    window->addTextEditor ("text", initial);
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, done] (int result)
    {
        const auto text = window->getTextEditorContents ("text").trim();

        if (result == 1 && text.isNotEmpty() && done != nullptr)
            done (text);
    }), true);
}

} // namespace ps::ui
