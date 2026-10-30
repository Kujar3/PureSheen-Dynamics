#pragma once

#include "ui/Widgets.h"

#include <functional>
#include <utility>
#include <vector>

// Small helpers shared by the Imager and Exciter panels. Owner: ui-stereo (internal, not a contract).
namespace ps::ui::stereo
{
//==============================================================================
// One undoable edit of a parameter and, optionally, its A/B twin (Link) driven by a relative drag: the host gesture
// begins on the first movement (a click without movement changes nothing) and ends on mouse up.
class ParamDrag
{
public:
    explicit ParamDrag (PureSheenProcessor&);
    ~ParamDrag();

    // Arms a drag at the parameter's current value. linkedId may be empty.
    void start (const juce::String& paramId, const juce::String& linkedId);
    // Moves the value by deltaNormalised (0..1 of the parameter's range) from the value at start().
    void dragTo (float deltaNormalised);
    void end();
    bool isArmed() const noexcept { return param != nullptr; }
    bool isActive() const noexcept { return active; }

private:
    PureSheenProcessor& processor;
    juce::RangedAudioParameter* param = nullptr;
    juce::RangedAudioParameter* linked = nullptr;
    float startNorm = 0.0f;
    bool active = false;
};

// One undo step that sets a parameter (and its twin, if any) to a plain value.
void setParam (PureSheenProcessor&, const juce::String& paramId, const juce::String& linkedId, float plainValue);
float defaultValueOf (const juce::String& paramId);
const ps::ParamSpec* specOf (const juce::String& paramId);

// Right-click menu of a parameter shown on a custom control: "Reset to default" / "Enter value...".
void showParamMenu (juce::Component& target, PureSheenProcessor&, const juce::String& paramId,
                    const juce::String& linkedId, const juce::String& title);

//==============================================================================
// Rounded inner card (same surface as the generic panel's group cards): title row with an optional band badge,
// subtitle on the right, and a dimmed state with a short note ("Acts on bands wider than 0").
class Card : public juce::Component
{
public:
    Card (const juce::String& title, juce::Colour colour);

    void setTitle (const juce::String& t)          { if (t != title) { title = t; repaint(); } }
    void setSubtitle (const juce::String& s)       { if (s != subtitle) { subtitle = s; repaint(); } }
    void setBadge (int bandNumberOrZero)           { if (bandNumberOrZero != badge) { badge = bandNumberOrZero; repaint(); } }
    void setColour (juce::Colour c)                { if (c != colour) { colour = c; repaint(); } }
    void setInactiveNote (const juce::String& n)   { if (n != inactiveNote) { inactiveNote = n; repaint(); } }
    // Small explanatory text at the bottom of the body (one or two lines). Empty = none.
    void setBodyNote (const juce::String& n)       { if (n != bodyNote) { bodyNote = n; repaint(); } }
    juce::Rectangle<int> getBodyNoteArea() const noexcept;
    // Captions drawn in the card (e.g. left of a segmented control), card-relative areas.
    void setCaptions (std::vector<std::pair<juce::String, juce::Rectangle<int>>> c) { captions = std::move (c); repaint(); }
    // Right edge reserved in the title row (for a toggle placed there by the owner).
    void setTitleReserve (int px)                  { titleReserve = px; }

    static constexpr int kTitleH = 30, kPad = 12;
    juce::Rectangle<int> getBody() const noexcept;           // area under the title row, padded
    juce::Rectangle<int> getTitleRowRight (int width) const noexcept;   // right end of the title row

    void paint (juce::Graphics&) override;

private:
    juce::String title, subtitle, inactiveNote, bodyNote;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> captions;
    juce::Colour colour;
    int badge = 0, titleReserve = 0;
};

// Segmented control for UI state that is not a parameter (vectorscope display mode). Left clicks only.
class ChoiceSegments final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ChoiceSegments (juce::StringArray labels, juce::Colour onColour);

    void setSelected (int index);
    int getSelected() const noexcept { return selected; }
    std::function<void (int)> onChange;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

private:
    int indexAt (juce::Point<float>) const noexcept;
    juce::StringArray labels;
    juce::Colour onColour;
    int selected = 0;
};

// Draws the plot background used by the graphs (transfer curve, vectorscope): dark rounded well with an outline.
void drawWell (juce::Graphics&, juce::Rectangle<float> area, float radius = 6.0f);

// "-inf" safe dB from a linear magnitude.
inline float toDb (float linear) noexcept { return linear > 1.0e-9f ? 20.0f * std::log10 (linear) : -180.0f; }

} // namespace ps::ui::stereo
