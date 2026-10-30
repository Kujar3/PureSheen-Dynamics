#pragma once

#include "ui/Theme.h"

#include <functional>

// Small building blocks shared by the editor's own components (header, chain strip, I/O panel, module panel header).
// Owner: ui-shell. Not part of the Widgets.h contract, but module panels may use them.
namespace ps::ui
{
//==============================================================================
// PureSheen's logo mark: a glossy mint droplet with a lavender sheen highlight. `area` should be square.
void drawLogo (juce::Graphics&, juce::Rectangle<float> area);

// Small round status light (power LEDs).
void drawLed (juce::Graphics&, juce::Rectangle<float> area, bool on, juce::Colour onColour);

// Raised rounded surface for chips / buttons: fill gradient + 1 px outline.
void drawChip (juce::Graphics&, juce::Rectangle<float> b, float radius, juce::Colour fill, juce::Colour outline);

// Mixes two colours (alpha of `a` kept).
inline juce::Colour blend (juce::Colour a, juce::Colour b, float amountOfB)
{
    return a.interpolatedWith (b, juce::jlimit (0.0f, 1.0f, amountOfB));
}

//==============================================================================
// Icon-only button. Only left clicks act (right / middle clicks are ignored). Never takes keyboard focus.
class IconButton final : public juce::Button
{
public:
    enum class Icon { help, options, close, reset, undo, redo, history, prev, next, save, menu, plus, link, power,
                      sliders };

    IconButton (const juce::String& name, Icon iconType);

    void mouseDown (const juce::MouseEvent& e) override { if (isLeftClick (e)) juce::Button::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { if (isLeftClick (e)) juce::Button::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (isLeftClick (e)) juce::Button::mouseUp (e); }

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    static void paintIcon (juce::Graphics&, Icon, juce::Rectangle<float> box);   // 16 x 16 box

    void setIcon (Icon i) { icon = i; repaint(); }
    bool roundBackground = false;   // circular hover fill (close buttons)
    juce::Colour onColour = Colours::accent;

private:
    Icon icon;
};

//==============================================================================
// Text pill that toggles (not bound to a parameter): Solo, Delta, Learn, Gain Match... `onClick` gets left clicks;
// `onRightClick` (optional) gets right clicks / ctrl-clicks (context menus). The owner sets the toggle state.
class PillButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PillButton (const juce::String& text = {});

    void setText (const juce::String&);
    const juce::String& getText() const noexcept { return text; }
    void setOn (bool);
    bool isOn() const noexcept { return on; }
    void setOnColour (juce::Colour c) { onColour = c; repaint(); }
    void setSquare (bool s) { square = s; repaint(); }       // small square variant ("S" solo button in tiles)
    void setFilledWhenOn (bool f) { filledWhenOn = f; repaint(); }
    int getIdealWidth (float height) const;

    std::function<void()> onClick;
    std::function<void()> onRightClick;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    juce::String text;
    bool on = false, square = false, filledWhenOn = false;
    juce::Colour onColour = Colours::accent;
};

//==============================================================================
// Pill-shaped drop-down: dim prefix + value + chevron. Opens its menu on a left click (the owner builds and shows it
// in onOpenMenu and calls setMenuOpen (false) when it closes).
class MenuChip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    MenuChip();

    void setText (const juce::String& prefix, const juce::String& value);
    int getIdealWidth() const;
    void setMenuOpen (bool);
    void setValueColour (juce::Colour c) { valueColour = c; repaint(); }

    std::function<void (MenuChip&)> onOpenMenu;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    juce::String prefixText, valueText;
    bool menuOpen = false;
    juce::Colour valueColour = Colours::text;
};

//==============================================================================
// Shows a popup menu styled with `lookAndFeelSource`'s look-and-feel, targeted at `target`; `done` receives the result
// on the message thread (only while `target` still exists).
void showMenu (juce::PopupMenu&, juce::Component& target, std::function<void (int)> done, int minWidth = 0);

// Asks for a line of text (preset names) in a small modal window; `done` gets the trimmed text (not called on Cancel
// or for an empty text). Destroying the parent cancels the prompt and its callback.
void promptForText (juce::Component& parent, const juce::String& title, const juce::String& message,
                    const juce::String& initial, std::function<void (juce::String)> done);

} // namespace ps::ui
