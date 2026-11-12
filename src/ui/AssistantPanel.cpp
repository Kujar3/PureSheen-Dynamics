#include "ui/AssistantPanel.h"

#include "dsp/ToneBalance.h"
#include "plugin/Assistant.h"
#include "plugin/TrackReferencing.h"
#include "ui/Controls.h"

#include <array>
#include <cmath>

namespace ps::ui
{
namespace
{
using psplug::Assistant;
using ps::assist::Macro;
namespace Col = Colours;

constexpr int kRowH = 26;
constexpr float kLoMinHz = 20.0f, kHiMaxHz = 20000.0f;

float xForHz (juce::Rectangle<float> r, double hz)
{
    return r.getX() + r.getWidth() * (float) (std::log (hz / kLoMinHz) / std::log (kHiMaxHz / kLoMinHz));
}

juce::String lufsText (float v) { return v > -100.0f ? juce::String (v, 1) + " LUFS" : juce::String ("--"); }

//==============================================================================
// A big clickable card (start page).
class StartCard final : public juce::Component
{
public:
    StartCard (juce::String t, juce::String l1, juce::String l2, juce::Colour c)
        : title (std::move (t)), line1 (std::move (l1)), line2 (std::move (l2)), colour (c)
    {
        setRepaintsOnMouseActivity (true);
    }

    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        const bool hover = isMouseOver (true);
        drawChip (g, b, 10.0f, hover ? Col::panelHover : Col::panelRaised, hover ? colour.withAlpha (0.8f) : Col::outline);

        auto r = b.reduced (18.0f);
        auto top = r.removeFromTop (r.getHeight() * 0.5f);
        const auto f = font (20.0f, 700);
        const float w = textWidth (f, title) + 40.0f;
        auto row = top.withSizeKeepingCentre (w, 32.0f).withY (top.getBottom() - 40.0f);
        auto dot = row.removeFromLeft (30.0f).withSizeKeepingCentre (28.0f, 28.0f);
        g.setGradientFill (juce::ColourGradient (colour.brighter (0.3f), dot.getX(), dot.getY(), colour.darker (0.4f),
                                                 dot.getRight(), dot.getBottom(), false));
        g.fillEllipse (dot);
        g.setColour (Col::background);
        if (title == "Quick")
        {
            // sparkle
            juce::Path p;
            const auto c = dot.getCentre();
            for (int i = 0; i < 4; ++i)
            {
                const float a = juce::MathConstants<float>::halfPi * (float) i;
                p.addTriangle (c.getPointOnCircumference (8.0f, a), c.getPointOnCircumference (2.0f, a + 0.8f),
                               c.getPointOnCircumference (2.0f, a - 0.8f));
            }
            g.fillPath (p);
        }
        else
        {
            for (int i = 0; i < 3; ++i)
            {
                const float x = dot.getX() + 8.5f + 5.5f * (float) i;
                g.fillRect (x - 0.6f, dot.getY() + 7.0f, 1.2f, 14.0f);
                g.fillEllipse (juce::Rectangle<float> (4.5f, 4.5f).withCentre ({ x, dot.getY() + (i == 1 ? 17.0f : 11.0f) }));
            }
        }
        g.setFont (f);
        g.setColour (Col::text);
        g.drawText (title, row.withTrimmedLeft (8.0f), juce::Justification::centredLeft, false);

        g.setColour (Col::outline);
        g.fillRect (juce::Rectangle<float> (b.getCentreX() - 90.0f, top.getBottom(), 180.0f, 1.0f));
        g.setFont (font (13.0f));
        g.setColour (Col::textDim);
        g.drawText (line1, r.removeFromTop (22.0f).withTrimmedTop (8.0f), juce::Justification::centred, false);
        g.drawText (line2, r.removeFromTop (22.0f).withTrimmedTop (4.0f), juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isLeftClick (e) && getLocalBounds().contains (e.getPosition()) && onClick != nullptr)
            onClick();
    }

private:
    juce::String title, line1, line2;
    juce::Colour colour;
};

//==============================================================================
// The target list: Auto, one row per analysed reference of Track Referencing's list ("Ref: <name>"), "+ Reference
// track...", every tone target. Scrolls with the wheel / drag.
class TargetList final : public juce::Component
{
public:
    // A reference row. id = its Track Referencing id; 0 = the Assistant's own reference whose file is not in the list
    // (an older project): shown as "Reference: <name>". current = it is the Assistant's reference.
    struct RefRow
    {
        int id = 0;
        juce::String name;
        bool current = false;

        bool operator== (const RefRow& o) const { return id == o.id && name == o.name && current == o.current; }
        bool operator!= (const RefRow& o) const { return ! (*this == o); }
    };

    TargetList() { setRepaintsOnMouseActivity (true); }

    std::function<void (int profile)> onSelect;          // -1 = Auto
    std::function<void (int referenceId)> onUseReference; // a reference row that is not the Assistant's reference yet
    std::function<void()> onRemoveReference, onAddReference, onSelectReference;

    // referenceSelected: the Assistant's reference is the target (its row is highlighted instead of the profile's)
    void set (int selectedProfile, ps::ToneTarget detected, const std::vector<RefRow>& references, bool loading,
              bool referenceSelected)
    {
        if (selectedProfile != selected || detected != detectedTarget || references != refs || loading != refLoading
            || referenceSelected != refSelected)
        {
            selected = selectedProfile;
            detectedTarget = detected;
            refs = references;
            refLoading = loading;
            refSelected = referenceSelected;
            scroll = juce::jlimit (0.0f, maxScroll(), scroll);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        drawPanel (g, b);
        auto head = b.removeFromTop (34.0f).reduced (12.0f, 0.0f);
        g.setFont (font (13.0f, 700));
        g.setColour (Col::text);
        g.drawText ("Target", head, juce::Justification::centredLeft, false);
        plusArea = head.removeFromRight (24.0f).withSizeKeepingCentre (22.0f, 22.0f);
        g.setColour (plusHover ? Col::panelHover : Col::panelRaised);
        g.fillEllipse (plusArea);
        g.setColour (Col::text);
        g.fillRect (juce::Rectangle<float> (10.0f, 1.6f).withCentre (plusArea.getCentre()));
        g.fillRect (juce::Rectangle<float> (1.6f, 10.0f).withCentre (plusArea.getCentre()));

        listArea = b.reduced (6.0f, 0.0f).withTrimmedBottom (6.0f);
        g.reduceClipRegion (listArea.toNearestInt());
        const bool useRef = refSelected && hasCurrentReference();
        const int rows = numRows();
        for (int i = 0; i < rows; ++i)
        {
            const auto row = rowBounds (i);
            if (! row.intersects (listArea))
                continue;
            const int id = rowId (i);
            const bool isRef = id <= kRefRowBase;
            const RefRow* ref = isRef ? &refs[(size_t) (kRefRowBase - id)] : nullptr;
            const bool isSel = isRef ? (useRef && ref->current) : (id >= -1 && ! useRef && id == selected);
            const bool hover = hoverRow == i;
            if (isSel || hover)
            {
                g.setColour (isSel ? blend (Col::panelRaised, Col::accent, 0.18f) : Col::panelHover.withAlpha (0.7f));
                g.fillRoundedRectangle (row.reduced (0.0f, 1.0f), 5.0f);
            }
            auto textArea = row.reduced (10.0f, 0.0f);
            if (isRef)
            {
                // a lavender dot: a reference track (filled = the Assistant's reference)
                const auto dot = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ textArea.getX() + 3.0f, row.getCentreY() });
                g.setColour (Col::reference.withAlpha (ref->current ? 1.0f : 0.6f));
                if (ref->current)
                    g.fillEllipse (dot);
                else
                    g.drawEllipse (dot.reduced (0.5f), 1.1f);
                textArea.removeFromLeft (12.0f);
                if (ref->current)
                    textArea.removeFromRight (20.0f);
            }
            g.setFont (font (12.5f, isSel ? 600 : 400));
            g.setColour (isSel ? Col::text : Col::textDim.brighter (0.1f));
            g.drawText (rowText (id), textArea, juce::Justification::centredLeft, true);
            if (isRef && ref->current)
            {
                g.setColour (hover ? Col::text : Col::textSubtle);
                const auto x = row.withLeft (row.getRight() - 24.0f).withSizeKeepingCentre (8.0f, 8.0f);
                g.drawLine ({ x.getTopLeft(), x.getBottomRight() }, 1.4f);
                g.drawLine ({ x.getBottomLeft(), x.getTopRight() }, 1.4f);
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool p = plusArea.expanded (3.0f).contains (e.position);
        const int r = rowAt (e.position);
        if (p != plusHover || r != hoverRow)
        {
            plusHover = p;
            hoverRow = r;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        plusHover = false;
        hoverRow = -1;
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        scroll = juce::jlimit (0.0f, maxScroll(), scroll - w.deltaY * 120.0f);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e))
            return;
        if (plusArea.expanded (3.0f).contains (e.position))
        {
            if (onAddReference != nullptr)
                onAddReference();
            return;
        }
        const int r = rowAt (e.position);
        if (r < 0)
            return;
        const int id = rowId (r);
        if (id == kAddRow)
        {
            if (! refLoading && onAddReference != nullptr)
                onAddReference();
            return;
        }
        if (id <= kRefRowBase)
        {
            const auto& ref = refs[(size_t) (kRefRowBase - id)];
            if (ref.current && e.position.x > (float) getWidth() - 34.0f)
            {
                if (onRemoveReference != nullptr)
                    onRemoveReference();
            }
            else if (ref.current)
            {
                if (onSelectReference != nullptr)
                    onSelectReference();
            }
            else if (onUseReference != nullptr)
            {
                onUseReference (ref.id);
            }
            return;
        }
        if (onSelect != nullptr)
            onSelect (id);
    }

private:
    static constexpr int kAddRow = -2, kRefRowBase = -100;   // reference row k has the id kRefRowBase - k

    bool hasCurrentReference() const
    {
        for (const auto& r : refs)
            if (r.current)
                return true;
        return false;
    }

    int numRows() const { return 2 + (int) refs.size() + (int) ps::ToneTarget::Count; }
    int rowId (int row) const
    {
        const int numRefs = (int) refs.size();
        if (row == 0)
            return -1;
        if (row <= numRefs)
            return kRefRowBase - (row - 1);
        if (row == numRefs + 1)
            return kAddRow;
        return row - numRefs - 2;
    }
    juce::String rowText (int id) const
    {
        if (id == -1)
            return "Auto (detected: " + juce::String (ps::assist::targetName (detectedTarget)) + ")";
        if (id <= kRefRowBase)
        {
            const auto& r = refs[(size_t) (kRefRowBase - id)];
            return (r.id == 0 ? "Reference: " : "Ref: ") + r.name;
        }
        if (id == kAddRow)
            return refLoading ? juce::String ("Reference: analysing...") : juce::String ("+ Reference track...");
        return ps::assist::targetName ((ps::ToneTarget) id);
    }
    juce::Rectangle<float> rowBounds (int i) const
    {
        return { listArea.getX(), listArea.getY() + (float) (i * kRowH) - scroll, listArea.getWidth(), (float) kRowH };
    }
    int rowAt (juce::Point<float> p) const
    {
        if (! listArea.contains (p))
            return -1;
        const int r = (int) ((p.y - listArea.getY() + scroll) / (float) kRowH);
        return r >= 0 && r < numRows() ? r : -1;
    }
    float maxScroll() const { return juce::jmax (0.0f, (float) (numRows() * kRowH) - listArea.getHeight()); }

    int selected = -1, hoverRow = -1;
    ps::ToneTarget detectedTarget = ps::ToneTarget::Balanced;
    std::vector<RefRow> refs;
    bool refLoading = false, refSelected = false, plusHover = false;
    float scroll = 0.0f;
    juce::Rectangle<float> plusArea, listArea;
};

//==============================================================================
// One suggestion card.
class CardButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    CardButton() { setRepaintsOnMouseActivity (true); }

    std::function<void()> onClick;

    void set (const ps::assist::Suggestion& s, bool isApplied)
    {
        name = s.name;
        description = s.description;
        detail = juce::String (ps::assist::targetName (s.target)) + "  " + juce::String (s.loudnessLufs, 1) + " LUFS";
        isDefault = s.isDefault;
        applied = isApplied;
        setTooltip ("Build this chain: " + description + ". One undo step; every module stays editable.");
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        const bool hover = isMouseOver (true);
        drawChip (g, b, 8.0f, applied ? blend (Col::panelRaised, Col::accent, 0.2f) : (hover ? Col::panelHover : Col::panelRaised),
                  applied ? Col::accent.withAlpha (0.9f) : Col::outline.brighter (hover ? 0.2f : 0.0f));
        auto r = b.reduced (10.0f, 6.0f);
        g.setFont (font (13.0f, 700));
        g.setColour (Col::text);
        g.drawFittedText (name, r.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        g.setFont (font (11.0f));
        g.setColour (Col::textDim);
        g.drawFittedText (description, r.removeFromTop (16.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        auto bottom = r.removeFromTop (16.0f);
        if (isDefault)
        {
            // the reference-like default: a small "DEFAULT" badge on the detail line (the name stays readable)
            const auto f = font (8.5f, 700).withExtraKerningFactor (0.08f);
            const float w = textWidth (f, "DEFAULT") + 10.0f;
            const auto badge = bottom.removeFromRight (w).withSizeKeepingCentre (w, 13.0f);
            g.setColour (Col::accent.withAlpha (0.2f));
            g.fillRoundedRectangle (badge, 6.5f);
            g.setColour (Col::accent);
            g.setFont (f);
            g.drawText ("DEFAULT", badge, juce::Justification::centred, false);
        }
        g.setFont (font (11.0f, 600));
        g.setColour (applied ? Col::accent.brighter (0.3f) : Col::textSubtle);
        g.drawFittedText (detail, bottom.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isLeftClick (e) && getLocalBounds().contains (e.getPosition()) && onClick != nullptr)
            onClick();
    }

private:
    juce::String name, description, detail;
    bool isDefault = false, applied = false;
};

//==============================================================================
// A macro control: a slider (rotary, horizontal or vertical) with its name and value. Drags are one undo step.
class MacroControl final : public juce::Component
{
public:
    MacroControl (Assistant& a, Macro m, juce::Slider::SliderStyle style) : assistant (a), macro (m)
    {
        const auto& info = ps::assist::macroInfo (m);
        slider.setSliderStyle (style);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (info.minValue, info.maxValue, 0.0);
        slider.setScrollWheelEnabled (true);
        slider.setWantsKeyboardFocus (false);
        slider.setMouseClickGrabsKeyboardFocus (false);
        if (style == juce::Slider::RotaryHorizontalVerticalDrag)
            slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.onDragStart = [this] { dragging = true; };
        slider.onValueChange = [this] {
            if (! updating)
                assistant.setMacro (macro, (float) slider.getValue(), ! dragging);
            repaint();
        };
        slider.onDragEnd = [this] {
            dragging = false;
            assistant.setMacro (macro, (float) slider.getValue(), true);
        };
        addAndMakeVisible (slider);
    }

    void update (float value, float resetValue, bool enabled)
    {
        slider.setDoubleClickReturnValue (true, resetValue);
        setEnabled (enabled);
        if (! dragging && std::abs ((float) slider.getValue() - value) > 1.0e-4f)
        {
            updating = true;
            slider.setValue (value, juce::dontSendNotification);
            updating = false;
            repaint();
        }
    }

    juce::String valueText() const
    {
        const auto& info = ps::assist::macroInfo (macro);
        const float v = (float) slider.getValue();
        const juce::String unit (info.unit);
        if (macro == Macro::Tilt)
            return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB";
        if (macro >= Macro::WidthLow && macro <= Macro::WidthHigh)
            return (v > 0.5f ? "+" : "") + juce::String (juce::roundToInt (v));
        if (macro == Macro::Loudness)
            return juce::String (v, 1);
        return juce::String (juce::roundToInt (v)) + (unit.isNotEmpty() ? " " + unit : juce::String());
    }

    juce::String caption;
    bool vertical = false;

    void paint (juce::Graphics& g) override
    {
        const auto& info = ps::assist::macroInfo (macro);
        g.setColour (isEnabled() ? Col::textDim : Col::textFaint);
        g.setFont (font (11.5f, 600));
        const auto name = caption.isNotEmpty() ? caption : juce::String (info.name);
        if (slider.getSliderStyle() == juce::Slider::LinearHorizontal)
        {
            auto r = getLocalBounds().removeFromTop (16);
            g.drawText (name, r, juce::Justification::centredLeft, false);
            g.setColour (isEnabled() ? Col::text : Col::textFaint);
            g.drawText (valueText(), r, juce::Justification::centredRight, false);
        }
        else
        {
            auto r = getLocalBounds();
            g.drawText (name, r.removeFromBottom (16), juce::Justification::centred, false);
            g.setColour (isEnabled() ? Col::text : Col::textFaint);
            g.setFont (font (vertical ? 11.0f : 12.5f, 600));
            g.drawText (valueText(), r.removeFromBottom (16), juce::Justification::centred, false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (slider.getSliderStyle() == juce::Slider::LinearHorizontal)
            slider.setBounds (r.withTrimmedTop (14).reduced (-2, 0));
        else
            slider.setBounds (r.withTrimmedBottom (34));
    }

private:
    Assistant& assistant;
    Macro macro;
    LeftClickOnly<juce::Slider> slider;
    bool dragging = false, updating = false;
};

//==============================================================================
// Plain slider for a setting (Custom page): name, value text, onChange.
class SettingSlider final : public juce::Component
{
public:
    SettingSlider (juce::String n, double lo, double hi, double step, std::function<juce::String (double)> fmt)
        : name (std::move (n)), format (std::move (fmt))
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (lo, hi, step);
        slider.setWantsKeyboardFocus (false);
        slider.setMouseClickGrabsKeyboardFocus (false);
        slider.onValueChange = [this] { repaint(); if (onChange != nullptr) onChange (slider.getValue()); };
        addAndMakeVisible (slider);
    }

    std::function<void (double)> onChange;

    void setValue (double v)
    {
        if (std::abs (slider.getValue() - v) > 1.0e-6)
            slider.setValue (v, juce::dontSendNotification);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().removeFromTop (16);
        g.setFont (font (11.5f, 600));
        g.setColour (isEnabled() ? Col::textDim : Col::textFaint);
        g.drawText (name, r, juce::Justification::centredLeft, false);
        g.setColour (isEnabled() ? Col::text : Col::textFaint);
        g.drawText (format (slider.getValue()), r, juce::Justification::centredRight, false);
    }

    void resized() override { slider.setBounds (getLocalBounds().withTrimmedTop (14).reduced (-2, 0)); }

private:
    juce::String name;
    std::function<juce::String (double)> format;
    LeftClickOnly<juce::Slider> slider;
};

// The target's tonal-balance band (with the track's own balance when known), Low / Low-Mid / High-Mid / High.
void drawTargetBand (juce::Graphics& g, juce::Rectangle<float> r, ps::ToneTarget target,
                     const ps::assist::ReferenceTarget* ref, const ps::Curve& track)
{
    g.setColour (Col::background.darker (0.15f));
    g.fillRoundedRectangle (r, 6.0f);
    auto plot = r.reduced (8.0f, 10.0f).withTrimmedBottom (14.0f);
    const float edges[] { 250.0f, 2000.0f, 8000.0f };
    const char* names[] { "Low", "Low-Mid", "High-Mid", "High" };
    float last = plot.getX();
    for (int i = 0; i < 4; ++i)
    {
        const float x = i < 3 ? xForHz (plot, edges[i]) : plot.getRight();
        if (i < 3)
        {
            g.setColour (Col::outline.withAlpha (0.6f));
            g.fillRect (juce::Rectangle<float> (x, plot.getY(), 1.0f, plot.getHeight() + 14.0f));
        }
        g.setColour (Col::textSubtle);
        g.setFont (font (10.5f, 600));
        g.drawText (names[i], juce::Rectangle<float> (last, plot.getBottom() + 1.0f, x - last, 14.0f), juce::Justification::centred, false);
        last = x;
    }

    // y: +-12 dB around the curve's mean over 100 Hz .. 5.6 kHz
    auto yFor = [&] (double db) { return plot.getCentreY() - (float) (db / 12.0) * plot.getHeight() * 0.5f; };
    auto curveDb = [&] (double hz, double* tol) {
        if (ref != nullptr && ref->valid)
        {
            if (tol != nullptr)
                *tol = 2.0;
            return (double) ps::assist::curveAt (ref->octaveDb, hz);
        }
        return ps::toneTargetDb (target, hz, tol);
    };

    juce::Path upper, lower, line;
    const int n = 96;
    for (int i = 0; i <= n; ++i)
    {
        const double hz = kLoMinHz * std::pow (kHiMaxHz / kLoMinHz, (double) i / n);
        double tol = 0.0;
        const double db = curveDb (hz, &tol);
        const float x = xForHz (plot, hz);
        if (i == 0)
        {
            upper.startNewSubPath (x, yFor (db + tol));
            line.startNewSubPath (x, yFor (db));
        }
        else
        {
            upper.lineTo (x, yFor (db + tol));
            line.lineTo (x, yFor (db));
        }
    }
    for (int i = n; i >= 0; --i)
    {
        const double hz = kLoMinHz * std::pow (kHiMaxHz / kLoMinHz, (double) i / n);
        double tol = 0.0;
        const double db = curveDb (hz, &tol);
        upper.lineTo (xForHz (plot, hz), yFor (db - tol));
    }
    upper.closeSubPath();
    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());
    g.setColour (Col::info.withAlpha (0.22f));
    g.fillPath (upper);
    g.setColour (Col::info.withAlpha (0.9f));
    g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // the track (relative to pink, 0 mean over 100 Hz .. 5.6 kHz: the same reference level as the target curves)
    juce::Path t;
    bool started = false;
    for (int i = 0; i < ps::kCurvePoints; ++i)
    {
        const float v = track[(size_t) i];
        if (v <= ps::kNoValue || (v == 0.0f && i == 0))
            continue;
        const auto p = juce::Point<float> (xForHz (plot, ps::curveFrequency (i)), yFor (v));
        if (! started)
            t.startNewSubPath (p);
        else
            t.lineTo (p);
        started = true;
    }
    if (started)
    {
        g.setColour (Col::input.withAlpha (0.8f));
        g.strokePath (t, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.restoreState();
}
} // namespace

//==============================================================================
struct AssistantPanel::Impl
{
    Impl (AssistantPanel& o, EditorContext& c) : owner (o), ctx (c), proc (c.getProcessor()), assistant (proc.getAssistant())
    {
        quick.onClick = [this] { startQuick(); };
        custom.onClick = [this] { page = Page::Custom; userPage = true; layout(); };

        // Custom page
        targets.onSelect = [this] (int profile) { setProfile (profile); };
        targets.onAddReference = [this] { chooseReference(); };
        targets.onRemoveReference = [this] { assistant.clearReference(); };   // (stays in Track Referencing's list)
        targets.onSelectReference = [this] { selectReference(); };
        targets.onUseReference = [this] (int id) { useReference (id); };
        resultsTargets.onUseReference = targets.onUseReference;
        resultsTargets.onSelect = targets.onSelect;
        resultsTargets.onSelectReference = targets.onSelectReference;
        resultsTargets.onAddReference = targets.onAddReference;
        resultsTargets.onRemoveReference = targets.onRemoveReference;

        const std::pair<ps::ModuleType, const char*> modules[] {
            { ps::ModuleType::Equalizer1, "EQ 1" }, { ps::ModuleType::Punch, "Punch" }, { ps::ModuleType::Imager, "Imager" },
            { ps::ModuleType::Definition, "Definition" }, { ps::ModuleType::ToneBalance, "Tone Balance" },
            { ps::ModuleType::DynamicEq, "Dynamic EQ" }, { ps::ModuleType::Exciter, "Exciter" },
            { ps::ModuleType::LowContrast, "Low Contrast" } };
        for (const auto& [type, name] : modules)
        {
            auto pill = std::make_unique<PillButton> (name);
            pill->setTooltip (juce::String ("Let the Assistant use ") + name);
            const auto t = type;
            pill->onClick = [this, t] {
                auto s = assistant.getSettings();
                s.engine.allowedModules ^= (1u << (unsigned) t);
                assistant.setSettings (s);
            };
            moduleTypes.push_back (type);
            modulePills.push_back (std::move (pill));
        }
        for (int i = 0; i < 3; ++i)
        {
            const char* names[] { "Gentle", "Balanced", "Strong" };
            intensity[(size_t) i].setText (names[i]);
            intensity[(size_t) i].onClick = [this, i] {
                auto s = assistant.getSettings();
                s.engine.intensity = (ps::assist::Intensity) i;
                assistant.setSettings (s);
            };
        }
        useMax.setText ("Maximizer");
        useMax.onClick = [this] { auto s = assistant.getSettings(); s.engine.useMaximizer = ! s.engine.useMaximizer; assistant.setSettings (s); };
        autoLoud.setText ("Auto");
        autoLoud.setTooltip ("Auto: the target's own loudness. Off: the value of the slider.");
        autoLoud.onClick = [this] { auto s = assistant.getSettings(); s.engine.autoLoudness = ! s.engine.autoLoudness; assistant.setSettings (s); };
        truePeak.setText ("True Peak");
        truePeak.onClick = [this] { auto s = assistant.getSettings(); s.engine.truePeak = ! s.engine.truePeak; assistant.setSettings (s); };
        loudness.onChange = [this] (double v) { auto s = assistant.getSettings(); s.engine.loudnessLufs = (float) v; s.engine.autoLoudness = false; assistant.setSettings (s); };
        ceiling.onChange = [this] (double v) { auto s = assistant.getSettings(); s.engine.ceilingDb = (float) v; assistant.setSettings (s); };
        seconds.onChange = [this] (double v) { auto s = assistant.getSettings(); s.analysisSeconds = (float) v; assistant.setSettings (s); };
        // (1.3) Whole song: no need to know where the loudest part is. The time above is then the length of that part.
        wholeSong.setText ("Whole song");
        wholeSong.setTooltip ("Listen for as long as the song plays and use its loudest part. Play the song, then stop "
                              "playback (or press Done). Off: listen to the part you play for the time set here");
        wholeSong.onClick = [this] { auto s = assistant.getSettings(); s.wholeSong = ! s.wholeSong; assistant.setSettings (s); };
        done.setText ("Done");
        done.setTooltip ("The song has played: use the loudest part heard so far");
        done.setOn (true);
        done.setFilledWhenOn (true);
        done.onClick = [this] { assistant.finishListening(); };
        back.setText ("Back");
        back.onClick = [this] { page = assistant.getSuggestions().empty() ? Page::Start : Page::Results; userPage = page == Page::Start; layout(); };
        start.setText ("Start listening");
        start.setOn (true);
        start.setFilledWhenOn (true);
        start.onClick = [this] {
            if (assistant.hasAnalysis())
            {
                assistant.rebuild();   // the same audio, the new targets
                userPage = false;
                page = Page::Results;
                layout();
            }
            else
            {
                listen();
            }
        };

        // Listen page
        cancel.setText ("Cancel");
        cancel.onClick = [this] { assistant.cancel(); userPage = false; page = assistant.getSuggestions().empty() ? Page::Start : Page::Results; layout(); };
        fromFile.setText ("Analyse a file instead...");
        fromFile.onClick = [this] { chooseFile(); };

        // Results page
        relearn.setText ("Relearn");
        relearn.setTooltip ("Listen again (same targets) and rebuild the suggestions");
        relearn.onClick = [this] { listen(); };
        setup.setText ("Targets...");
        setup.setTooltip ("Change the targets, loudness and modules the Assistant uses");
        setup.onClick = [this] { page = Page::Custom; userPage = true; layout(); };
        bypass.setText ("Bypass");
        bypass.setTooltip ("Compare with the unprocessed track, level matched (hold the button)");
        // (1.3) The cards differ in loudness on purpose (Gentle is quieter, Loud is louder), and the louder one tends
        // to win a comparison. This plays them all at the loudness of the unprocessed track: Gain Match in Modern
        // mode, the I/O panel's own switch, so it shows there too and an export ignores it.
        equalLoud.setText ("Equal loudness");
        equalLoud.setTooltip ("Play every suggestion at the loudness of your unprocessed track, so you compare the sound "
                              "and not the level. This is Gain Match (Modern): turn it off to hear the real level");
        equalLoud.onClick = [this]
        {
            if (equalLoudnessOn())
            {
                proc.setParameterValue ("io.gainMatch", 0.0f);
                proc.setParameterValue ("io.gainMatchModern", modernBeforeEqual);
            }
            else
            {
                modernBeforeEqual = proc.getParameterValue ("io.gainMatchModern");
                proc.setParameterValue ("io.gainMatchModern", 1.0f);
                proc.setParameterValue ("io.gainMatch", 1.0f);
            }
        };
        openChain.setText ("Open chain");
        openChain.setTooltip ("Edit the modules of this chain");
        openChain.onClick = [this] { ctx.select (kChainSelection); };
        matched.setText ("Loudness matched");
        matched.setTooltip ("Moving a macro keeps the master's loudness, so you hear the change and not a level jump");
        matched.onClick = [this] { assistant.setLoudnessMatched (! assistant.isLoudnessMatched()); };

        for (auto* comp : std::initializer_list<juce::Component*> { &quick, &custom, &targets, &resultsTargets })
            owner.addChildComponent (comp);
        for (auto& p : modulePills)
            owner.addChildComponent (*p);
        for (auto& p : intensity)
            owner.addChildComponent (p);
        for (auto* comp : std::initializer_list<juce::Component*> { &useMax, &autoLoud, &truePeak, &loudness, &ceiling, &seconds,
                                                                &back, &start, &cancel, &fromFile, &relearn, &setup,
                                                                &bypass, &openChain, &matched, &equalLoud, &wholeSong, &done })
            owner.addChildComponent (comp);

        toneMatch = std::make_unique<MacroControl> (assistant, Macro::ToneMatch, juce::Slider::RotaryHorizontalVerticalDrag);
        loudMacro = std::make_unique<MacroControl> (assistant, Macro::Loudness, juce::Slider::RotaryHorizontalVerticalDrag);
        tilt = std::make_unique<MacroControl> (assistant, Macro::Tilt, juce::Slider::LinearHorizontal);
        for (auto m : { Macro::Dynamics, Macro::Width, Macro::Compress, Macro::Definition, Macro::ToneBalance })
            extras.push_back (std::make_unique<MacroControl> (assistant, m, juce::Slider::LinearHorizontal));
        for (int b = 0; b < 4; ++b)
        {
            auto w = std::make_unique<MacroControl> (assistant, (Macro) ((int) Macro::WidthLow + b), juce::Slider::LinearVertical);
            w->vertical = true;
            widths.push_back (std::move (w));
        }
        toneMatch->caption = "Tone Match";
        loudMacro->caption = "Loudness (LUFS)";
        for (auto* comp : allMacros())
            owner.addChildComponent (comp);
    }

    std::vector<MacroControl*> allMacros()
    {
        std::vector<MacroControl*> v { toneMatch.get(), loudMacro.get(), tilt.get() };
        for (auto& e : extras)
            v.push_back (e.get());
        for (auto& w : widths)
            v.push_back (w.get());
        return v;
    }

    //==========================================================================
    void startQuick()
    {
        auto s = assistant.getSettings();
        s.engine.profile = -1;
        s.engine.autoLoudness = true;
        assistant.setSettings (s);
        listen();
    }

    void listen()
    {
        assistant.startListening();
        userPage = false;
        page = Page::Listen;
        layout();
    }

    void setProfile (int profile)
    {
        auto s = assistant.getSettings();
        s.engine.profile = profile;
        s.engine.useReference = false;   // a loaded reference stays in the list, the target is the profile now
        assistant.setSettings (s, page == Page::Results);   // the results page rebuilds at once; Custom waits for Start
    }

    void selectReference()
    {
        auto s = assistant.getSettings();
        s.engine.useReference = true;
        assistant.setSettings (s, page == Page::Results);
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Analyse an audio file", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, safe = juce::Component::SafePointer<AssistantPanel> (&owner)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr)
                                      return;
                                  const auto f = fc.getResult();
                                  if (f.existsAsFile() && ! assistant.analyseFile (f))
                                      ctx.notify ("That file could not be read");
                                  userPage = false;
                                  page = Page::Listen;
                                  layout();
                              });
    }

    void chooseReference()
    {
        chooser = std::make_unique<juce::FileChooser> ("Reference track", juce::File(),
                                                       psplug::TrackReferencing::getSupportedWildcard());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, safe = juce::Component::SafePointer<AssistantPanel> (&owner)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr)
                                      return;
                                  const auto f = fc.getResult();
                                  if (f.existsAsFile())
                                      useReferenceFile (f);
                              });
    }

    // A reference for the Assistant goes into Track Referencing's list (one list, one analysis per file); it becomes
    // the target when its analysis is done (at once when the file is already analysed).
    void useReferenceFile (const juce::File& f)
    {
        auto& refs = proc.getTrackReferencing();
        const int id = refs.addReference (f, refs.getSelectedId() == 0);
        if (id == 0)
        {
            ctx.notify ("The reference list is full (10 tracks): remove one on the Reference page first");
            return;
        }
        useReference (id);
    }

    void useReference (int id)
    {
        auto& refs = proc.getTrackReferencing();
        const auto target = refs.getAssistantTarget (id);
        if (target.valid)
        {
            assistant.setReference (target);
            return;
        }
        for (const auto& e : refs.getInfo().entries)
        {
            if (e.id != id)
                continue;
            if (e.state == psplug::TrackReferencing::State::Error || e.state == psplug::TrackReferencing::State::Missing)
                ctx.notify ("That reference file could not be read (" + e.error + ")");
            else
                assistant.awaitReference (e.file.getFullPathName(), e.name);
        }
    }

    //==========================================================================
    Page wantedPage() const
    {
        if (userPage)
            return page;
        const auto ph = assistant.getPhase();
        const bool hasCards = ! assistant.getSuggestions().empty();
        if (ph == Assistant::Phase::Listening || (ph == Assistant::Phase::Analysing && ! hasCards))
            return Page::Listen;
        if (hasCards)
            return Page::Results;
        if (ph == Assistant::Phase::Failed)
            return Page::Listen;
        return Page::Start;
    }

    void tick()
    {
        // A listen or analysis that starts (from this page, a rebuild or elsewhere) takes the page back from a page the
        // user picked.
        const auto ph = assistant.getPhase();
        if (ph != lastPhase)
        {
            if (ph == Assistant::Phase::Listening || ph == Assistant::Phase::Analysing)
                userPage = userPage && page == Page::Custom && ph == Assistant::Phase::Analysing
                           && ! assistant.getSuggestions().empty();   // a rebuild from the Custom page stays there
            lastPhase = ph;
        }

        const auto p = wantedPage();
        if (p != page)
        {
            page = p;
            layout();
        }

        const auto s = assistant.getSettings();
        const auto ref = assistant.getReference();
        updateReferenceRows (ref);
        targets.set (s.engine.profile, assistant.getDetectedTarget(), referenceRows, assistant.isLoadingReference(),
                     s.engine.useReference);
        resultsTargets.set (s.engine.profile, assistant.getDetectedTarget(), referenceRows, assistant.isLoadingReference(),
                            s.engine.useReference);

        for (size_t i = 0; i < modulePills.size(); ++i)
            modulePills[i]->setOn ((s.engine.allowedModules & (1u << (unsigned) moduleTypes[i])) != 0);
        for (int i = 0; i < 3; ++i)
            intensity[(size_t) i].setOn ((int) s.engine.intensity == i);
        useMax.setOn (s.engine.useMaximizer);
        autoLoud.setOn (s.engine.autoLoudness);
        truePeak.setOn (s.engine.truePeak);
        loudness.setValue (s.engine.loudnessLufs);
        loudness.setAlpha (s.engine.autoLoudness ? 0.5f : 1.0f);
        ceiling.setValue (s.engine.ceilingDb);
        seconds.setValue (s.analysisSeconds);
        wholeSong.setOn (s.wholeSong);
        if (page == Page::Listen && done.isVisible() != assistant.isWholeSongListen())
            layout();
        matched.setOn (assistant.isLoudnessMatched());
        start.setText (assistant.hasAnalysis() ? "Rebuild" : "Start listening");
        start.setTooltip (assistant.hasAnalysis() ? "Build the suggestions again from the audio already heard, with these targets"
                                                  : "Listen to the track and build the suggestions");

        // Bypass: held = global bypass with classic gain match (monitoring parameters, never in the history); the
        // previous bypass / gain match / match mode come back on release. With Equal loudness on the match stays
        // Modern: the suggestions already play at the unprocessed track's loudness, which is what bypass plays.
        const bool held = bypass.isMouseButtonDown();
        if (held != bypassHeld)
        {
            bypassHeld = held;
            if (held)
            {
                monitorBefore = { proc.getParameterValue ("io.bypass"), proc.getParameterValue ("io.gainMatch"),
                                  proc.getParameterValue ("io.gainMatchModern") };
                if (! equalLoudnessOn())
                {
                    proc.setParameterValue ("io.gainMatchModern", 0.0f);
                    proc.setParameterValue ("io.gainMatch", 1.0f);
                }
                proc.setParameterValue ("io.bypass", 1.0f);
            }
            else
            {
                restoreMonitoring();
            }
        }
        bypass.setOn (held);
        if (! held)
            equalLoud.setOn (equalLoudnessOn());

        // Cards
        const auto cards = assistant.getSuggestions();
        const int applied = assistant.getAppliedIndex();
        if (cards.size() != cardButtons.size())
        {
            cardButtons.clear();
            for (size_t i = 0; i < cards.size(); ++i)
            {
                auto b = std::make_unique<CardButton>();
                b->onClick = [this, i] { assistant.apply ((int) i); };
                owner.addChildComponent (*b);
                cardButtons.push_back (std::move (b));
            }
            layout();
        }
        for (size_t i = 0; i < cards.size(); ++i)
            cardButtons[i]->set (cards[i], (int) i == applied);

        // Macros
        const bool haveBase = applied >= 0 && applied < (int) cards.size();
        const auto m = assistant.getMacros();
        const auto order = proc.getChainOrder();
        auto has = [&] (ps::ModuleType t) { return std::find (order.begin(), order.end(), t) != order.end(); };
        auto enabledFor = [&] (Macro k) {
            if (! haveBase)
                return false;
            switch (k)
            {
                case Macro::ToneMatch: case Macro::Tilt: return has (ps::ModuleType::Equalizer1);
                case Macro::Dynamics: return has (ps::ModuleType::Punch);
                case Macro::Width: case Macro::WidthLow: case Macro::WidthLowMid: case Macro::WidthHighMid:
                case Macro::WidthHigh: return has (ps::ModuleType::Imager);
                case Macro::Definition: return has (ps::ModuleType::Definition);
                case Macro::ToneBalance: return has (ps::ModuleType::ToneBalance);
                case Macro::Loudness: return has (ps::ModuleType::Maximizer);
                case Macro::Compress: case Macro::Count: default: return true;
            }
        };
        for (auto* c : allMacros())
        {
            const auto k = macroOf (c);
            const float reset = haveBase ? cards[(size_t) applied].macros[(size_t) k] : 0.0f;
            c->update (m[(size_t) k], reset, enabledFor (k));
        }

        // Tone display data
        bandTarget = haveBase ? cards[(size_t) applied].target : assistant.getDetectedTarget();
        bandRef = ref;
        bandUsesRef = haveBase && ref.valid && cards[(size_t) applied].name == "Match reference";
        trackCurve = assistant.getTrackCurve();
        status = assistant.getStatusText();
        progress = assistant.getProgress();
        working = assistant.isWorking() || assistant.getPhase() == Assistant::Phase::Listening;
        if (page == Page::Listen)
            updateOrbLevels();
        inputLufs = assistant.getInputLufs();
        outputLufs = proc.getChain().getIoMeters().outputShortTermLufs.load();
        owner.repaint();
    }

    // The target list's reference rows: every analysed reference of Track Referencing's list, the Assistant's own
    // reference highlighted (matched by path); an Assistant reference whose file is not in the list (an older
    // project) still shows as one row. Rebuilt only when the list or the Assistant's reference changes.
    void updateReferenceRows (const ps::assist::ReferenceTarget& ref)
    {
        auto& refs = proc.getTrackReferencing();
        const int version = refs.getInfoVersion();
        const juce::String key = ref.valid ? juce::String (ref.path) + "|" + juce::String (ref.name) : juce::String();
        if (version == rowsInfoVersion && key == rowsReferenceKey)
            return;
        rowsInfoVersion = version;
        rowsReferenceKey = key;

        referenceRows.clear();
        bool found = false;
        for (const auto& e : refs.getInfo().entries)
        {
            if (e.state != psplug::TrackReferencing::State::Ready)
                continue;
            const bool current = ref.valid && juce::String (ref.path) == e.file.getFullPathName();
            found = found || current;
            referenceRows.push_back ({ e.id, e.name, current });
        }
        if (ref.valid && ! found)
            referenceRows.insert (referenceRows.begin(), { 0, juce::String (ref.name), true });
    }

    Macro macroOf (MacroControl* c) const
    {
        if (c == toneMatch.get()) return Macro::ToneMatch;
        if (c == loudMacro.get()) return Macro::Loudness;
        if (c == tilt.get()) return Macro::Tilt;
        const Macro extraOrder[] { Macro::Dynamics, Macro::Width, Macro::Compress, Macro::Definition, Macro::ToneBalance };
        for (size_t i = 0; i < extras.size(); ++i)
            if (extras[i].get() == c)
                return extraOrder[i];
        for (size_t i = 0; i < widths.size(); ++i)
            if (widths[i].get() == c)
                return (Macro) ((int) Macro::WidthLow + (int) i);
        return Macro::ToneMatch;
    }

    //==========================================================================
    void layout()
    {
        for (auto* c : owner.getChildren())
            c->setVisible (false);

        auto r = owner.getLocalBounds().reduced (14);
        titleArea = r.removeFromTop (30);
        r.removeFromTop (8);

        if (page == Page::Start)
        {
            auto row = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 860), juce::jmin (r.getHeight(), 190));
            const int w = (row.getWidth() - 40) / 2;
            quick.setBounds (row.removeFromLeft (w));
            custom.setBounds (row.removeFromRight (w));
            quick.setVisible (true);
            custom.setVisible (true);
        }
        else if (page == Page::Custom)
        {
            auto left = r.removeFromLeft (juce::jlimit (200, 280, r.getWidth() / 3));
            targets.setBounds (left);
            targets.setVisible (true);
            r.removeFromLeft (14);
            auto bottom = r.removeFromBottom (32);
            start.setBounds (bottom.removeFromRight (150));
            bottom.removeFromRight (10);
            back.setBounds (bottom.removeFromRight (90));
            start.setVisible (true);
            back.setVisible (true);
            r.removeFromBottom (10);
            customArea = r;

            auto col = r.reduced (14, 10);
            col.removeFromTop (24);   // "Processing"
            auto pills = col.removeFromTop (60);
            const int pw = (pills.getWidth() - 3 * 8) / 4;
            for (size_t i = 0; i < modulePills.size(); ++i)
            {
                const int rowI = (int) i / 4, colI = (int) i % 4;
                modulePills[i]->setBounds (pills.getX() + colI * (pw + 8), pills.getY() + rowI * 30, pw, 26);
                modulePills[i]->setVisible (true);
            }
            col.removeFromTop (14);
            col.removeFromTop (20);   // "Intensity"
            auto irow = col.removeFromTop (26);
            const int iw = (irow.getWidth() - 16) / 3;
            for (auto& p : intensity)
            {
                p.setBounds (irow.removeFromLeft (iw));
                irow.removeFromLeft (8);
                p.setVisible (true);
            }
            col.removeFromTop (16);
            col.removeFromTop (20);   // "Loudness"
            auto lrow = col.removeFromTop (26);
            useMax.setBounds (lrow.removeFromLeft (110));
            lrow.removeFromLeft (8);
            autoLoud.setBounds (lrow.removeFromLeft (70));
            lrow.removeFromLeft (8);
            truePeak.setBounds (lrow.removeFromLeft (100));
            for (auto* c : std::initializer_list<juce::Component*> { &useMax, &autoLoud, &truePeak })
                c->setVisible (true);
            col.removeFromTop (8);
            auto srow = col.removeFromTop (36);
            const int sw = (srow.getWidth() - 20) / 2;
            loudness.setBounds (srow.removeFromLeft (sw));
            ceiling.setBounds (srow.removeFromRight (sw));
            col.removeFromTop (12);
            auto timeRow = col.removeFromTop (36);
            seconds.setBounds (timeRow.removeFromLeft (sw));
            timeRow.removeFromLeft (20);
            wholeSong.setBounds (timeRow.removeFromLeft (104).withSizeKeepingCentre (104, 26).translated (0, 5));
            for (auto* c : std::initializer_list<juce::Component*> { &loudness, &ceiling, &seconds, &wholeSong })
                c->setVisible (true);
        }
        else if (page == Page::Listen)
        {
            listenArea = r;
            const bool whole = assistant.isWholeSongListen();   // a whole-song listen ends with Done (or when playback stops)
            auto bottom = r.removeFromBottom (32).withSizeKeepingCentre (whole ? 460 : 360, 30);
            if (whole)
            {
                done.setBounds (bottom.removeFromLeft (90));
                bottom.removeFromLeft (10);
                done.setVisible (true);
            }
            cancel.setBounds (bottom.removeFromLeft (110));
            fromFile.setBounds (bottom.removeFromRight (230));
            cancel.setVisible (true);
            fromFile.setVisible (true);
        }
        else
        {
            // Results
            auto top = titleArea;
            openChain.setBounds (top.removeFromRight (104).withSizeKeepingCentre (104, 26));
            top.removeFromRight (8);
            bypass.setBounds (top.removeFromRight (80).withSizeKeepingCentre (80, 26));
            top.removeFromRight (8);
            equalLoud.setBounds (top.removeFromRight (118).withSizeKeepingCentre (118, 26));
            top.removeFromRight (8);
            relearn.setBounds (top.removeFromRight (84).withSizeKeepingCentre (84, 26));
            top.removeFromRight (8);
            setup.setBounds (top.removeFromRight (90).withSizeKeepingCentre (90, 26));
            for (auto* c : std::initializer_list<juce::Component*> { &openChain, &bypass, &equalLoud, &relearn, &setup })
                c->setVisible (true);

            auto cardsRow = r.removeFromTop (68);
            if (! cardButtons.empty())
            {
                const int n = (int) cardButtons.size();
                const int w = (cardsRow.getWidth() - (n - 1) * 8) / n;
                for (auto& b : cardButtons)
                {
                    b->setBounds (cardsRow.removeFromLeft (w));
                    cardsRow.removeFromLeft (8);
                    b->setVisible (true);
                }
            }
            r.removeFromTop (10);

            auto left = r.removeFromLeft (juce::jlimit (170, 230, r.getWidth() / 5));
            resultsTargets.setBounds (left);
            resultsTargets.setVisible (true);
            r.removeFromLeft (10);

            auto right = r.removeFromRight (juce::jlimit (200, 260, r.getWidth() / 3));
            extrasArea = right;
            auto ex = right.reduced (12, 10);
            ex.removeFromTop (22);   // "Macros"
            matched.setBounds (ex.removeFromBottom (26));
            matched.setVisible (true);
            ex.removeFromBottom (8);
            auto widthRow = ex.removeFromBottom (juce::jmin (120, ex.getHeight() / 3));
            widthArea = widthRow;
            widthRow.removeFromTop (18);   // "Band widths"
            const int ww = widthRow.getWidth() / 4;
            for (auto& w : widths)
            {
                w->setBounds (widthRow.removeFromLeft (ww));
                w->setVisible (true);
            }
            const int eh = juce::jmin (44, ex.getHeight() / (int) extras.size());
            for (auto& e : extras)
            {
                e->setBounds (ex.removeFromTop (eh).withTrimmedBottom (eh - 34 > 0 ? eh - 34 : 0));
                e->setVisible (true);
            }

            r.removeFromRight (10);
            const int toneH = r.getHeight() * 55 / 100;
            toneArea = r.removeFromTop (toneH);
            r.removeFromTop (10);
            loudArea = r;

            auto tone = toneArea.reduced (12, 10);
            tone.removeFromTop (22);
            auto knob = tone.removeFromLeft (juce::jmin (130, tone.getWidth() / 3));
            toneMatch->setBounds (knob.removeFromTop (juce::jmin (knob.getHeight() - 40, 124)));
            tilt->setBounds (knob.removeFromBottom (36));
            tone.removeFromLeft (10);
            bandArea = tone;

            auto loud = loudArea.reduced (12, 10);
            loud.removeFromTop (22);
            loudMacro->setBounds (loud.removeFromLeft (juce::jmin (130, loud.getWidth() / 3)));
            loudReadout = loud.reduced (10, 0);
            for (auto* c : { toneMatch.get(), tilt.get(), loudMacro.get() })
                c->setVisible (true);
        }
        owner.repaint();
    }

    void paint (juce::Graphics& g)
    {
        drawPanel (g, owner.getLocalBounds().toFloat());
        g.setFont (font (15.0f, 700));
        g.setColour (Col::text);
        const juce::String title = page == Page::Start ? "Assistant" : page == Page::Custom ? "Assistant: targets"
                                 : page == Page::Listen ? "Assistant: listening" : "Assistant";
        g.drawText (title, titleArea, juce::Justification::centredLeft, false);

        if (page == Page::Start)
        {
            g.setFont (font (12.5f));
            g.setColour (Col::textDim);
            g.drawText ("Builds a complete, editable master chain from what it hears. Pick how you want to start.",
                        quick.getBounds().getUnion (custom.getBounds()).translated (0, -40).withHeight (24),
                        juce::Justification::centred, false);
        }
        else if (page == Page::Custom)
        {
            drawPanel (g, customArea.toFloat());
            auto col = customArea.reduced (14, 10);
            drawSectionTitle (g, "PROCESSING", col.removeFromTop (24));
            col.removeFromTop (74);
            drawSectionTitle (g, "INTENSITY", col.removeFromTop (20));
            col.removeFromTop (42);
            drawSectionTitle (g, "LOUDNESS", col.removeFromTop (20));
        }
        else if (page == Page::Listen)
        {
            paintListen (g);
        }
        else
        {
            paintResults (g);
        }
    }

    void paintListen (juce::Graphics& g)
    {
        auto r = listenArea.toFloat();
        const auto ph = assistant.getPhase();
        const juce::String headline = ph == Assistant::Phase::Analysing ? "Analysing..."
                                    : ph == Assistant::Phase::Failed ? "Something went wrong"
                                    : "Play the loudest part of your track";
        g.setFont (font (20.0f, 600));
        g.setColour (Col::text);
        g.drawText (headline, r.removeFromTop (r.getHeight() * 0.3f), juce::Justification::centredBottom, false);

        const auto orbArea = r.removeFromTop (r.getHeight() * 0.6f);
        const float radius = juce::jlimit (34.0f, 64.0f, juce::jmin (orbArea.getWidth(), orbArea.getHeight()) / 3.3f);
        paintOrb (g, orbArea.getCentre(), radius, ph);
        g.setFont (font (13.0f));
        g.setColour (Col::textDim);
        g.drawText (status.isNotEmpty() ? status : juce::String ("Ready to listen..."), r.removeFromTop (30.0f),
                    juce::Justification::centred, false);
    }

    // The listening orb: a dark sphere in a slowly turning halo (mint, sky, violet) with the progress on its rim.
    // Inside it, while listening, the live output spectrum as a mirrored wave (lows left, highs right); while the
    // suggestions are built, translucent planes drifting around each other.
    void updateOrbLevels()
    {
        auto& spectrum = ctx.getServices().getOutputSpectrum();

        for (int i = 0; i < kOrbPoints; ++i)
        {
            const float hz = 40.0f * std::pow (400.0f, (float) i / (float) (kOrbPoints - 1));   // 40 Hz .. 16 kHz
            const float db = spectrum.levelDbAt (hz) + 4.5f * std::log2 (hz / 1000.0f);         // tilt: music falls
            const float target = juce::jlimit (0.0f, 1.0f, (db + 72.0f) / 54.0f);              // ~4.5 dB / octave
            auto& v = orbLevel[(size_t) i];
            v += (target - v) * (target > v ? 0.45f : 0.12f);
        }
    }

    void paintOrb (juce::Graphics& g, juce::Point<float> c, float radius, Assistant::Phase ph) const
    {
        const float twoPi = juce::MathConstants<float>::twoPi;
        const float t = (float) std::fmod (juce::Time::getMillisecondCounterHiRes() * 0.001, 3600.0);
        const auto orb = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);
        const juce::Colour hues[] { Col::accent, Col::info, Col::sideChannel };
        const bool failed = ph == Assistant::Phase::Failed;

        // Halo: three soft glows circling behind the rim at their own speeds
        for (int i = 0; i < 3; ++i)
        {
            const float a = t * (0.32f + 0.11f * (float) i) * (i == 1 ? -1.0f : 1.0f) + (float) i * twoPi / 3.0f;
            const auto p = c + juce::Point<float> (std::cos (a), std::sin (a)) * radius * 0.5f;
            const float gr = radius * 1.08f;
            const auto hue = failed ? Col::textFaint : hues[i];
            g.setGradientFill (juce::ColourGradient (hue.withAlpha (0.42f), p.x, p.y, hue.withAlpha (0.0f), p.x + gr, p.y, true));
            g.fillEllipse (juce::Rectangle<float> (gr * 2.0f, gr * 2.0f).withCentre (p));
        }

        // Body: lit from the top left
        g.setGradientFill (juce::ColourGradient (Col::panelRaised.darker (0.15f), c.x - radius * 0.35f, c.y - radius * 0.45f,
                                                 Col::background.darker (0.6f), c.x + radius * 0.55f, c.y + radius * 0.85f, true));
        g.fillEllipse (orb);

        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addEllipse (orb);
            g.reduceClipRegion (clip);

            if (ph == Assistant::Phase::Analysing)
            {
                // Planes drifting around each other
                for (int i = 0; i < 3; ++i)
                {
                    const float a = t * (0.55f + 0.2f * (float) i) * (i == 1 ? -1.0f : 1.0f) + (float) i * 2.1f;
                    const auto off = juce::Point<float> (std::cos (a * 0.7f), std::sin (a * 0.9f)) * radius * 0.28f;
                    juce::Path plane;
                    plane.addEllipse (-radius * 1.05f, -radius * 0.42f, radius * 2.1f, radius * 0.84f);
                    const auto xf = juce::AffineTransform::rotation (a).translated (c + off);
                    const auto p0 = juce::Point<float> (-radius, 0.0f).transformedBy (xf);
                    const auto p1 = juce::Point<float> (radius, 0.0f).transformedBy (xf);
                    g.setGradientFill (juce::ColourGradient (hues[i].withAlpha (0.55f), p0.x, p0.y, hues[i].withAlpha (0.04f), p1.x, p1.y, false));
                    g.fillPath (plane, xf);
                }

                const float a = t * 1.6f;
                const auto dot = c + juce::Point<float> (std::cos (a), std::sin (a * 1.3f)) * radius * 0.5f;
                g.setColour (Col::text.withAlpha (0.9f));
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (dot));
            }
            else if (! failed)
            {
                // Faint rings breathing outwards
                for (int k = 1; k <= 4; ++k)
                {
                    const float rr = radius * (0.2f * (float) k + 0.04f * std::sin (t * 1.4f - (float) k));
                    g.setColour (Col::text.withAlpha (0.05f));
                    g.drawEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c), 1.0f);
                }

                // The spectrum as a mirrored wave: a wide tinted layer behind, a bright one in front
                const float x0 = c.x - radius * 0.9f, x1 = c.x + radius * 0.9f;
                auto wave = [&] (float scale, float phase)
                {
                    juce::Path p;
                    std::array<juce::Point<float>, kOrbPoints> top {};

                    for (int i = 0; i < kOrbPoints; ++i)
                    {
                        const float u = (float) i / (float) (kOrbPoints - 1);
                        const float taper = std::sin (u * juce::MathConstants<float>::pi);
                        const float wobble = 0.05f + 0.035f * std::sin (t * 2.2f + phase + (float) i * 0.8f);
                        const float amp = radius * 0.46f * taper * (wobble + 0.9f * orbLevel[(size_t) i]) * scale;
                        top[(size_t) i] = { x0 + (x1 - x0) * u, c.y - amp };
                    }

                    // Smooth through the midpoints, along the top and back along the mirrored bottom
                    p.startNewSubPath (top[0]);
                    for (int i = 1; i < kOrbPoints; ++i)
                        p.quadraticTo (top[(size_t) i - 1], (top[(size_t) i - 1] + top[(size_t) i]) * 0.5f);
                    p.lineTo (top.back());
                    for (int i = kOrbPoints - 1; i > 0; --i)
                    {
                        const auto a = top[(size_t) i], b = top[(size_t) i - 1];
                        p.quadraticTo ({ a.x, 2.0f * c.y - a.y }, { (a.x + b.x) * 0.5f, c.y - ((a.y + b.y) * 0.5f - c.y) });
                    }
                    p.closeSubPath();
                    return p;
                };

                g.setGradientFill (juce::ColourGradient (Col::info.withAlpha (0.35f), x0, c.y, Col::sideChannel.withAlpha (0.35f), x1, c.y, false));
                g.fillPath (wave (1.3f, 1.7f));
                g.setGradientFill (juce::ColourGradient (Col::text.withAlpha (0.95f), c.x, c.y - radius * 0.3f,
                                                         Col::accent.withAlpha (0.85f), c.x, c.y + radius * 0.3f, false));
                g.fillPath (wave (1.0f, 0.0f));
            }
        }

        g.setColour (Col::text.withAlpha (0.08f));
        g.drawEllipse (orb.reduced (0.5f), 1.0f);

        // Progress on the rim, with a bright head
        const float rr = radius + 7.0f;
        juce::Path track;
        track.addCentredArc (c.x, c.y, rr, rr, 0.0f, 0.0f, twoPi, true);
        g.setColour (Col::outline.withAlpha (0.55f));
        g.strokePath (track, juce::PathStrokeType (2.5f));

        if (progress > 0.001f)
        {
            const float end = twoPi * juce::jlimit (0.0f, 1.0f, progress);
            juce::Path value;
            value.addCentredArc (c.x, c.y, rr, rr, 0.0f, 0.0f, end, true);
            g.setColour (Col::accent);
            g.strokePath (value, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            const auto head = c + juce::Point<float> (std::sin (end), -std::cos (end)) * rr;
            g.setColour (Col::accent.withAlpha (0.3f));
            g.fillEllipse (juce::Rectangle<float> (13.0f, 13.0f).withCentre (head));
            g.setColour (Col::text);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (head));
        }
    }

    void paintResults (juce::Graphics& g)
    {
        for (const auto& a : { toneArea, loudArea, extrasArea })
            drawPanel (g, a.toFloat());
        drawSectionTitle (g, "TONE", toneArea.reduced (12, 8).removeFromTop (18));
        drawSectionTitle (g, "LOUDNESS", loudArea.reduced (12, 8).removeFromTop (18));
        drawSectionTitle (g, "MACROS", extrasArea.reduced (12, 8).removeFromTop (18));
        drawSectionTitle (g, "BAND WIDTHS", widthArea.withHeight (16));

        drawTargetBand (g, bandArea.toFloat(), bandTarget, bandUsesRef ? &bandRef : nullptr, trackCurve);
        g.setFont (font (10.5f, 600));
        g.setColour (Col::info);
        g.drawText (bandUsesRef ? "Reference" : juce::String (ps::assist::targetName (bandTarget)) + " target",
                    bandArea.reduced (10, 6).removeFromTop (14), juce::Justification::topRight, false);
        g.setColour (Col::input);
        g.drawText ("Your track", bandArea.reduced (10, 6).removeFromTop (14).translated (0, 14), juce::Justification::topRight, false);

        auto r = loudReadout.toFloat();
        auto line = [&] (const juce::String& label, const juce::String& value, juce::Colour c) {
            auto row = r.removeFromTop (24.0f);
            g.setFont (font (12.0f));
            g.setColour (Col::textDim);
            g.drawText (label, row, juce::Justification::centredLeft, false);
            g.setFont (font (13.0f, 600));
            g.setColour (c);
            g.drawText (value, row, juce::Justification::centredRight, false);
        };
        const auto m = assistant.getMacros();
        line ("Target", lufsText (m[(int) Macro::Loudness]), Col::text);
        line ("Your track (heard)", lufsText (inputLufs), Col::input);
        line ("Output now (short-term)", lufsText (outputLufs), Col::output);
        r.removeFromTop (6.0f);
        g.setFont (font (11.0f));
        g.setColour (Col::textSubtle);
        g.drawFittedText (status, r.removeFromTop (34.0f).toNearestInt(), juce::Justification::topLeft, 2, 0.9f);

        if (working)
        {
            auto bar = loudReadout.toFloat().removeFromBottom (4.0f);
            g.setColour (Col::outline);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (Col::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.02f, 1.0f, progress)), 2.0f);
        }
    }

    AssistantPanel& owner;
    EditorContext& ctx;
    PureSheenProcessor& proc;
    Assistant& assistant;
    Page page = Page::Start;
    bool userPage = false;
    Assistant::Phase lastPhase = Assistant::Phase::Idle;

    StartCard quick { "Quick", "Listen and build a master.", "One click, detected target.", Col::info };
    StartCard custom { "Custom", "Choose the target, loudness", "and modules first.", Col::sideChannel };
    TargetList targets, resultsTargets;
    std::vector<std::unique_ptr<PillButton>> modulePills;
    std::vector<ps::ModuleType> moduleTypes;
    std::array<PillButton, 3> intensity;
    PillButton useMax, autoLoud, truePeak, back, start, cancel, fromFile, relearn, setup, bypass, openChain, matched;
    PillButton equalLoud, wholeSong, done;
    // io.gainMatchModern before Equal loudness turned Modern on. An editor opened with it already on does not know,
    // and leaves the mode at Modern when the pill goes off.
    float modernBeforeEqual = 1.0f;

    bool equalLoudnessOn() const
    {
        return proc.getParameterValue ("io.gainMatch") > 0.5f && proc.getParameterValue ("io.gainMatchModern") > 0.5f;
    }

    SettingSlider loudness { "Loudness target", ps::assist::kMinTargetLufs, ps::assist::kMaxTargetLufs, 0.5, [] (double v) { return juce::String (v, 1) + " LUFS"; } };
    SettingSlider ceiling { "Output level", -3.0, 0.0, 0.1, [] (double v) { return juce::String (v, 1) + " dB"; } };
    SettingSlider seconds { "Analysis time", 5.0, 30.0, 1.0, [] (double v) { return juce::String (juce::roundToInt (v)) + " s"; } };
    std::vector<std::unique_ptr<CardButton>> cardButtons;
    std::unique_ptr<MacroControl> toneMatch, loudMacro, tilt;
    std::vector<std::unique_ptr<MacroControl>> extras, widths;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<TargetList::RefRow> referenceRows;
    int rowsInfoVersion = -1;
    juce::String rowsReferenceKey;

    juce::Rectangle<int> titleArea, customArea, listenArea, toneArea, loudArea, extrasArea, widthArea, bandArea, loudReadout;
    ps::ToneTarget bandTarget = ps::ToneTarget::Balanced;
    ps::assist::ReferenceTarget bandRef;
    bool bandUsesRef = false, working = false, bypassHeld = false;
    std::array<float, 3> monitorBefore {};   // io.bypass, io.gainMatch, io.gainMatchModern before Compare

    void restoreMonitoring()
    {
        proc.setParameterValue ("io.bypass", monitorBefore[0]);
        proc.setParameterValue ("io.gainMatchModern", monitorBefore[2]);
        proc.setParameterValue ("io.gainMatch", monitorBefore[1]);
    }
    ps::Curve trackCurve {};
    juce::String status;
    float progress = 0.0f, inputLufs = ps::kNoValue, outputLufs = ps::kNoValue;
    static constexpr int kOrbPoints = 28;
    std::array<float, kOrbPoints> orbLevel {};   // smoothed spectrum levels 0..1 for the listening orb
};

//==============================================================================
AssistantPanel::AssistantPanel (EditorContext& c) : impl (std::make_unique<Impl> (*this, c))
{
    setTitle ("Assistant");
}

AssistantPanel::~AssistantPanel()
{
    // Never leave the host bypassed when the page closes while the compare button is held.
    if (impl->bypassHeld)
        impl->restoreMonitoring();
}

void AssistantPanel::tick() { impl->tick(); }

void AssistantPanel::showPage (Page p)
{
    impl->page = p;
    impl->userPage = true;
    impl->layout();
}

AssistantPanel::Page AssistantPanel::getPage() const noexcept { return impl->page; }
void AssistantPanel::paint (juce::Graphics& g) { impl->paint (g); }
void AssistantPanel::resized() { impl->layout(); }
} // namespace ps::ui
