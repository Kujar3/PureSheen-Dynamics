#include "ui/Widgets.h"

#include "ui/Controls.h"

#include <cmath>

// Shared widgets (owner: ui-shell). Every parameter widget binds through juce::ParameterAttachment: a drag is one
// begin / set... / end gesture (one undo step), clicks and menu choices are one complete gesture. Only left clicks
// edit; right clicks open the widget's menu. No widget takes keyboard focus (the "Enter value..." editor does, while
// it is open, because typing needs it).
namespace ps::ui
{
namespace
{
const ps::ParamSpec* findSpec (const juce::String& id)
{
    const int index = ps::paramIndex (id.toStdString());
    return index >= 0 ? &ps::paramSpecs()[(size_t) index] : nullptr;
}

juce::String plainNumber (float v, int decimals)
{
    const float half = 0.5f * std::pow (10.0f, (float) -decimals);

    if (std::abs (v) < half)
        v = 0.0f;

    return decimals > 0 ? juce::String (v, decimals) : juce::String (juce::roundToInt (v));
}

int decimalsFor (const ps::ParamSpec& s)
{
    if (s.interval >= 1.0f)
        return 0;

    if (s.interval >= 0.1f)
        return 1;

    const float range = s.maxValue - s.minValue;
    return range > 50.0f ? 1 : 2;
}

juce::String formatSpecValue (const ps::ParamSpec& s, float v)
{
    if (s.kind == ps::ParamKind::Choice)
    {
        const int index = juce::jlimit (0, juce::jmax (0, (int) s.choices.size() - 1), juce::roundToInt (v));
        return index < (int) s.choices.size() ? juce::String (s.choices[(size_t) index]) : juce::String();
    }

    if (s.kind == ps::ParamKind::Bool)
        return v >= 0.5f ? "On" : "Off";

    const juce::String unit (s.unit);

    if (unit == "dB")      return fmt::db (v, decimalsFor (s) == 0 ? 0 : 1, s.minValue < 0.0f);
    if (unit == "Hz")      return fmt::hz (v);
    if (unit == "ms")      return fmt::ms (v);
    if (unit == "%")       return fmt::percent (v);
    if (unit == ":1")      return fmt::ratio (v);
    if (unit == "LUFS")    return fmt::lufs (v);
    if (unit == "dB/oct")  return (v > 0.0f ? "+" : "") + plainNumber (v, 1) + " dB/oct";

    const auto number = plainNumber (v, decimalsFor (s));
    return unit.isEmpty() ? number : number + " " + unit;
}

// "1.2k", "350 Hz", "-3 dB", "4:1" -> number in the parameter's unit (kHz / s suffixes scaled).
bool parseValueText (const ps::ParamSpec& s, juce::String text, float& result)
{
    text = text.trim().toLowerCase();

    if (text.isEmpty())
        return false;

    if (s.kind == ps::ParamKind::Choice)
    {
        for (size_t i = 0; i < s.choices.size(); ++i)
            if (juce::String (s.choices[i]).equalsIgnoreCase (text))
            {
                result = (float) i;
                return true;
            }
    }

    if (s.kind == ps::ParamKind::Bool)
    {
        if (text == "on" || text == "1" || text == "yes")  { result = 1.0f; return true; }
        if (text == "off" || text == "0" || text == "no")  { result = 0.0f; return true; }
        return false;
    }

    const auto numberPart = text.initialSectionContainingOnly ("+-0123456789.");

    if (numberPart.isEmpty() || numberPart == "-" || numberPart == "+" || numberPart == ".")
        return false;

    double value = numberPart.getDoubleValue();
    const auto suffix = text.substring (numberPart.length()).trim();

    if (juce::String (s.unit) == "Hz" && suffix.startsWith ("k"))
        value *= 1000.0;
    else if (juce::String (s.unit) == "ms" && suffix.startsWith ("s"))
        value *= 1000.0;

    result = (float) value;
    return true;
}

//==============================================================================
// One bound parameter (and an optional linked one). Message thread only.
class Binding
{
public:
    Binding (PureSheenProcessor& p, std::function<void()> onChange) : processor (p), changed (std::move (onChange)) {}

    ~Binding()
    {
        endIfActive();
    }

    void bind (const juce::String& newId)
    {
        endIfActive();
        attachment.reset();
        id = newId;
        param = processor.getParameter (id);
        spec = findSpec (id);

        if (param != nullptr)
        {
            attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float v)
            {
                value = v;
                if (changed != nullptr)
                    changed();
            });
            attachment->sendInitialUpdate();
        }
        else if (changed != nullptr)
        {
            changed();
        }
    }

    void link (const juce::String& linkedParamId)
    {
        linked = linkedParamId.isNotEmpty() && linkedParamId != id ? processor.getParameter (linkedParamId) : nullptr;
    }

    bool isValid() const noexcept { return param != nullptr && spec != nullptr; }
    float get() const noexcept { return value; }
    float getDefault() const noexcept { return spec != nullptr ? spec->defaultValue : 0.0f; }
    float minValue() const noexcept { return spec != nullptr ? spec->minValue : 0.0f; }
    float maxValue() const noexcept { return spec != nullptr ? spec->maxValue : 1.0f; }

    float toNorm (float plain) const { return param != nullptr ? param->convertTo0to1 (plain) : 0.0f; }

    float fromNorm (float norm) const
    {
        if (param == nullptr)
            return 0.0f;

        const auto& range = param->getNormalisableRange();
        return range.snapToLegalValue (range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, norm)));
    }

    juce::String text() const { return spec != nullptr ? formatSpecValue (*spec, value) : juce::String ("--"); }
    juce::String textFor (float plain) const { return spec != nullptr ? formatSpecValue (*spec, plain) : juce::String(); }
    juce::String hostName() const { return spec != nullptr ? juce::String (spec->name) : id; }
    const ps::ParamSpec* getSpec() const noexcept { return spec; }
    const juce::String& getId() const noexcept { return id; }

    void begin()
    {
        if (attachment == nullptr || active)
            return;

        active = true;
        attachment->beginGesture();

        if (linked != nullptr)
            linked->beginChangeGesture();
    }

    void set (float plain)
    {
        if (attachment == nullptr)
            return;

        if (! active)
        {
            setComplete (plain);
            return;
        }

        attachment->setValueAsPartOfGesture (plain);

        if (linked != nullptr)
            linked->setValueNotifyingHost (linked->convertTo0to1 (plain));
    }

    void end()
    {
        if (! active)
            return;

        active = false;

        if (attachment != nullptr)
            attachment->endGesture();

        if (linked != nullptr)
            linked->endChangeGesture();
    }

    void setComplete (float plain)
    {
        if (attachment == nullptr)
            return;

        begin();
        set (plain);
        end();
    }

    bool isGestureActive() const noexcept { return active; }

private:
    void endIfActive()
    {
        if (active)
            end();
    }

    PureSheenProcessor& processor;
    std::function<void()> changed;
    juce::String id;
    juce::RangedAudioParameter* param = nullptr;
    juce::RangedAudioParameter* linked = nullptr;
    const ps::ParamSpec* spec = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    float value = 0.0f;
    bool active = false;
};

//==============================================================================
// Relative drag / wheel / double-click / context menu logic shared by knobs and faders.
class DragLogic : private juce::Timer
{
public:
    DragLogic (juce::Component& c, Binding& b) : owner (c), binding (b) {}
    ~DragLogic() override { stopTimer(); }

    float pixelsForFullRange = 220.0f;
    bool horizontal = false;   // faders: drag along their axis only

    void mouseDown (const juce::MouseEvent& e)
    {
        if (! binding.isValid())
            return;

        if (e.mods.isPopupMenu())
        {
            showMenu();
            return;
        }

        if (! isLeftClick (e))
            return;

        dragging = true;
        dragNorm = binding.toNorm (binding.get());
        lastPos = e.position;
    }

    void mouseDrag (const juce::MouseEvent& e)
    {
        if (! dragging || ! isLeftClick (e))
            return;

        const auto d = e.position - lastPos;
        lastPos = e.position;
        const float pixels = horizontal ? d.x : -d.y;
        const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
        dragNorm = juce::jlimit (0.0f, 1.0f, dragNorm + pixels / pixelsForFullRange * (fine ? 0.1f : 1.0f));

        if (! binding.isGestureActive())
            binding.begin();

        binding.set (binding.fromNorm (dragNorm));
    }

    void mouseUp (const juce::MouseEvent&)
    {
        if (dragging)
        {
            dragging = false;
            binding.end();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e)
    {
        if (isLeftClick (e) && binding.isValid())
        {
            binding.end();
            binding.setComplete (binding.getDefault());
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        if (! binding.isValid() || dragging)
            return;

        const float delta = (std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX)
                            * (wheel.isReversed ? -1.0f : 1.0f);

        if (std::abs (delta) < 1.0e-4f)
            return;

        const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();

        if (! binding.isGestureActive())
        {
            wheelNorm = binding.toNorm (binding.get());
            binding.begin();
        }

        wheelNorm = juce::jlimit (0.0f, 1.0f, wheelNorm + delta * (fine ? 0.01f : 0.06f));
        binding.set (binding.fromNorm (wheelNorm));
        startTimer (450);   // the wheel gesture ends (one undo step) shortly after the last wheel event
    }

    bool isDragging() const noexcept { return dragging; }

    std::function<void()> onEnterValue;

private:
    void timerCallback() override
    {
        stopTimer();

        if (! dragging)
            binding.end();
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (binding.hostName());
        menu.addItem (1, "Reset to default (" + binding.textFor (binding.getDefault()) + ")");
        menu.addItem (2, "Enter value...", onEnterValue != nullptr);
        menu.addSeparator();
        menu.addItem (3, "MIDI learn (coming later)", false);

        ps::ui::showMenu (menu, owner, [this] (int result)
        {
            if (result == 1)
                binding.setComplete (binding.getDefault());
            else if (result == 2 && onEnterValue != nullptr)
                onEnterValue();
        });
    }

    juce::Component& owner;
    Binding& binding;
    bool dragging = false;
    float dragNorm = 0.0f, wheelNorm = 0.0f;
    juce::Point<float> lastPos;
};

//==============================================================================
// In-place text entry ("Enter value..."). Commits on Return, cancels on Escape or when focus leaves.
class ValueEntry final : public juce::TextEditor
{
public:
    ValueEntry (Binding& b) : binding (b)
    {
        setJustification (juce::Justification::centred);
        setFont (font (12.5f, 600));
        setSelectAllWhenFocused (true);
        setIndents (4, 2);
        setColour (juce::TextEditor::backgroundColourId, Colours::background);
        setColour (juce::TextEditor::outlineColourId, Colours::accent);
        setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
        onReturnKey = [this] { commit(); };
        onEscapeKey = [this] { close(); };
        onFocusLost = [this] { close(); };
    }

    void open (juce::Component& parent, juce::Rectangle<int> area)
    {
        parent.addAndMakeVisible (this);
        setBounds (area);
        setText (binding.text().upToFirstOccurrenceOf (" ", false, false), false);
        grabKeyboardFocus();
        selectAll();
    }

private:
    void commit()
    {
        float v = 0.0f;

        if (const auto* spec = binding.getSpec(); spec != nullptr && parseValueText (*spec, getText(), v))
            binding.setComplete (juce::jlimit (spec->minValue, spec->maxValue, v));

        close();
    }

    void close()
    {
        // Removed asynchronously: this may run inside one of the editor's own callbacks.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
        {
            if (safe != nullptr)
                if (auto* parent = safe->getParentComponent())
                    parent->removeChildComponent (safe.getComponent());
        });
        setVisible (false);
    }

    Binding& binding;
};

void setNoFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

// Angles of every knob arc (radians, JUCE convention: 0 = up, clockwise).
constexpr float kKnobStart = -2.35f, kKnobEnd = 2.35f;
} // namespace

//==============================================================================
namespace fmt
{
    juce::String db (float v, int decimals, bool sign)
    {
        if (v < -150.0f)
            return "-inf";

        const auto number = plainNumber (v, decimals);
        return (sign && number.getDoubleValue() > 0.0 ? "+" : "") + number + " dB";
    }

    juce::String hz (float v)
    {
        if (v >= 999.5f)
        {
            const float k = v / 1000.0f;
            return juce::String (k, k >= 10.0f ? 1 : 2) + " kHz";
        }

        return juce::String (juce::roundToInt (v)) + " Hz";
    }

    juce::String ms (float v)
    {
        if (v >= 1000.0f)
            return juce::String (v / 1000.0f, 2) + " s";

        if (v < 10.0f)
            return juce::String (v, 2) + " ms";

        if (v < 100.0f)
            return juce::String (v, 1) + " ms";

        return juce::String (juce::roundToInt (v)) + " ms";
    }

    juce::String percent (float v) { return juce::String (juce::roundToInt (v)) + " %"; }

    juce::String ratio (float r)
    {
        if (std::abs (r) < 1.0f)
            return "1:1";

        return juce::String (r, 1) + ":1";
    }

    juce::String lufs (float v) { return v < -150.0f ? juce::String ("--") : plainNumber (v, 1) + " LUFS"; }

    juce::String paramValue (const PureSheenProcessor& p, const juce::String& paramId)
    {
        if (const auto* spec = findSpec (paramId))
            return formatSpecValue (*spec, p.getParameterValue (paramId));

        return {};
    }
}

//==============================================================================
struct ParamKnob::Impl
{
    Impl (ParamKnob& o, PureSheenProcessor& p) : owner (o), binding (p, [this] { owner.repaint(); }), drag (o, binding)
    {
        drag.onEnterValue = [this]
        {
            entry = std::make_unique<ValueEntry> (binding);
            const auto v = valueArea().toNearestInt().expanded (4, 2);
            entry->open (owner, v.withSizeKeepingCentre (juce::jmax (v.getWidth(), 64), juce::jmax (v.getHeight(), 20)));
        };
    }

    juce::Rectangle<float> captionArea() const
    {
        return owner.getLocalBounds().toFloat().removeFromTop (compact ? 13.0f : 16.0f);
    }

    juce::Rectangle<float> valueArea() const
    {
        return owner.getLocalBounds().toFloat().removeFromBottom (compact ? 14.0f : 17.0f);
    }

    juce::Rectangle<float> knobArea() const
    {
        auto r = owner.getLocalBounds().toFloat();
        r.removeFromTop (captionArea().getHeight() + 2.0f);
        r.removeFromBottom (valueArea().getHeight() + 1.0f);
        const float d = juce::jmax (8.0f, juce::jmin (r.getWidth(), r.getHeight(), compact ? 46.0f : 64.0f));
        return r.withSizeKeepingCentre (d, d);
    }

    ParamKnob& owner;
    Binding binding;
    DragLogic drag;
    juce::String caption;
    juce::Colour colour = Colours::accent;
    bool compact = false;
    std::unique_ptr<ValueEntry> entry;
};

ParamKnob::ParamKnob (PureSheenProcessor& p, const juce::String& paramId, const juce::String& caption)
    : impl (std::make_unique<Impl> (*this, p))
{
    setNoFocus (*this);
    setRepaintsOnMouseActivity (true);
    impl->caption = caption;
    setParamId (paramId);
}

ParamKnob::~ParamKnob() = default;

void ParamKnob::setParamId (const juce::String& paramId)
{
    impl->binding.bind (paramId);
    setTooltip (impl->binding.hostName() + "\nDrag to change (Ctrl: fine), double-click to reset, right-click for more.");
    setTitle (impl->caption.isNotEmpty() ? impl->caption : impl->binding.hostName());
    repaint();
}

const juce::String& ParamKnob::getParamId() const noexcept { return impl->binding.getId(); }
void ParamKnob::setColour (juce::Colour arc) { impl->colour = arc; repaint(); }
void ParamKnob::setCompact (bool c) { impl->compact = c; repaint(); }
void ParamKnob::setLinkedParamId (const juce::String& paramId) { impl->binding.link (paramId); }
void ParamKnob::setCaption (const juce::String& c) { impl->caption = c; repaint(); }
void ParamKnob::resized() {}

void ParamKnob::paint (juce::Graphics& g)
{
    auto& d = *impl;
    const bool valid = d.binding.isValid();
    const bool hover = isMouseOverOrDragging() && valid;
    const bool active = d.drag.isDragging() || d.binding.isGestureActive();

    // Caption
    g.setFont (font (d.compact ? 10.5f : 11.5f, 600));
    g.setColour (hover ? Colours::text : Colours::textDim);
    g.drawFittedText (d.caption, d.captionArea().toNearestInt().expanded (3, 0), juce::Justification::centred, 1, 0.7f);

    // Knob
    const auto area = d.knobArea();
    const auto centre = area.getCentre();
    const float radius = area.getWidth() * 0.5f;
    const float lineW = juce::jlimit (2.2f, 3.6f, radius * 0.12f);
    const float arcR = radius - lineW * 0.5f - 1.0f;
    const float norm = valid ? d.binding.toNorm (d.binding.get()) : 0.0f;
    const float angle = kKnobStart + norm * (kKnobEnd - kKnobStart);

    // Bipolar ranges start their value arc at zero.
    float originNorm = 0.0f;

    if (valid && d.binding.minValue() < 0.0f && d.binding.maxValue() > 0.0f)
        originNorm = d.binding.toNorm (0.0f);

    const float originAngle = kKnobStart + originNorm * (kKnobEnd - kKnobStart);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, kKnobStart, kKnobEnd, true);
    g.setColour (juce::Colours::black.withAlpha (0.28f));
    g.strokePath (track, juce::PathStrokeType (lineW + 1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (Colours::outline.brighter (hover ? 0.15f : 0.0f));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (valid && std::abs (angle - originAngle) > 0.01f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, juce::jmin (originAngle, angle),
                             juce::jmax (originAngle, angle), true);
        const auto c = d.colour.brighter (hover ? 0.15f : 0.0f);
        g.setColour (c.withAlpha (0.25f));
        g.strokePath (value, juce::PathStrokeType (lineW + 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float bodyR = arcR - lineW * 0.5f - juce::jmax (2.5f, radius * 0.11f);

    if (bodyR > 3.0f)
    {
        const auto body = juce::Rectangle<float> (bodyR * 2.0f, bodyR * 2.0f).withCentre (centre);
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillEllipse (body.translated (0.0f, 1.5f));
        const auto top = hover ? Colours::panelHover.brighter (0.1f) : Colours::panelHover;
        g.setGradientFill (juce::ColourGradient::vertical (top, body.getY(), Colours::panelRaised.darker (0.12f), body.getBottom()));
        g.fillEllipse (body);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        const float inner = bodyR - juce::jmax (2.0f, bodyR * 0.18f);
        const auto p1 = centre.getPointOnCircumference (inner, angle);
        const auto p0 = centre.getPointOnCircumference (inner * 0.45f, angle);
        g.setColour (valid ? (hover ? d.colour.brighter (0.3f) : Colours::text) : Colours::textFaint);
        g.drawLine ({ p0, p1 }, juce::jlimit (1.6f, 2.6f, bodyR * 0.14f));
    }

    // Value
    g.setFont (font (d.compact ? 11.0f : 12.5f, active ? 700 : 600));
    g.setColour (valid ? (active ? d.colour.brighter (0.3f) : Colours::text.withAlpha (hover ? 1.0f : 0.9f)) : Colours::textFaint);
    g.drawFittedText (d.binding.text(), d.valueArea().toNearestInt(), juce::Justification::centred, 1, 0.75f);
}

void ParamKnob::mouseDown (const juce::MouseEvent& e)        { impl->drag.mouseDown (e); }
void ParamKnob::mouseDrag (const juce::MouseEvent& e)        { impl->drag.mouseDrag (e); repaint(); }
void ParamKnob::mouseUp (const juce::MouseEvent& e)          { impl->drag.mouseUp (e); repaint(); }
void ParamKnob::mouseDoubleClick (const juce::MouseEvent& e) { impl->drag.mouseDoubleClick (e); }
void ParamKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) { impl->drag.mouseWheelMove (e, w); }

//==============================================================================
struct ParamFader::Impl
{
    Impl (ParamFader& o, PureSheenProcessor& p) : owner (o), binding (p, [this] { owner.repaint(); }), drag (o, binding)
    {
        drag.onEnterValue = [this]
        {
            entry = std::make_unique<ValueEntry> (binding);
            entry->open (owner, owner.getLocalBounds().withSizeKeepingCentre (juce::jmax (owner.getWidth(), 60), 22)
                                    .constrainedWithin (owner.getLocalBounds().expanded (20, 4)));
        };
    }

    ParamFader& owner;
    Binding binding;
    DragLogic drag;
    juce::String caption;
    bool vertical = true;
    juce::Colour colour = Colours::accent;
    std::unique_ptr<ValueEntry> entry;
};

ParamFader::ParamFader (PureSheenProcessor& p, const juce::String& paramId, bool vertical, const juce::String& caption)
    : impl (std::make_unique<Impl> (*this, p))
{
    setNoFocus (*this);
    setRepaintsOnMouseActivity (true);
    impl->caption = caption;
    impl->vertical = vertical;
    impl->drag.horizontal = ! vertical;
    impl->drag.pixelsForFullRange = vertical ? 200.0f : 260.0f;
    setParamId (paramId);
}

ParamFader::~ParamFader() = default;

void ParamFader::setParamId (const juce::String& paramId)
{
    impl->binding.bind (paramId);
    setTooltip (impl->binding.hostName() + "\nDrag to change (Ctrl: fine), double-click to reset, right-click for more.");
    setTitle (impl->binding.hostName());
    repaint();
}

void ParamFader::setLinkedParamId (const juce::String& paramId) { impl->binding.link (paramId); }
void ParamFader::setColour (juce::Colour c) { impl->colour = c; repaint(); }
void ParamFader::resized() {}

void ParamFader::paint (juce::Graphics& g)
{
    auto& d = *impl;
    const bool valid = d.binding.isValid();
    const bool hover = isMouseOverOrDragging() && valid;
    const bool active = d.drag.isDragging();
    const float norm = valid ? d.binding.toNorm (d.binding.get()) : 0.0f;
    float originNorm = 0.0f;

    if (valid && d.binding.minValue() < 0.0f && d.binding.maxValue() > 0.0f)
        originNorm = d.binding.toNorm (0.0f);

    auto b = getLocalBounds().toFloat();

    if (! d.vertical)
    {
        // Horizontal: a value box (caption left, value right) with a slim track along its bottom edge: the fill runs
        // from zero (bipolar ranges) and a small thumb marks the value, so the text never sits under the thumb.
        const auto box = b.reduced (0.5f);
        const float radius = juce::jmin (6.0f, box.getHeight() * 0.5f);
        drawChip (g, box, radius, hover ? Colours::panelHover : Colours::panelRaised.darker (0.15f),
                  active ? d.colour.withAlpha (0.7f) : Colours::outline.brighter (hover ? 0.15f : 0.0f));

        const auto track = juce::Rectangle<float> (box.getX() + 7.0f, box.getBottom() - 4.5f, box.getWidth() - 14.0f, 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (track, 1.0f);
        const float x0 = track.getX() + track.getWidth() * juce::jmin (originNorm, norm);
        const float x1 = track.getX() + track.getWidth() * juce::jmax (originNorm, norm);

        if (valid && x1 - x0 > 0.5f)
        {
            g.setColour (d.colour.withAlpha (0.9f));
            g.fillRoundedRectangle (juce::Rectangle<float> (x0, track.getY(), x1 - x0, track.getHeight()), 1.0f);
        }

        if (valid)
        {
            const float thumbX = track.getX() + track.getWidth() * norm;
            g.setColour (hover || active ? Colours::text : d.colour.brighter (0.2f));
            g.fillRoundedRectangle (juce::Rectangle<float> (5.0f, 4.0f).withCentre ({ thumbX, track.getCentreY() }), 1.5f);
        }

        auto text = box.reduced (8.0f, 0.0f).withTrimmedBottom (3.0f);

        if (d.caption.isNotEmpty())
        {
            g.setFont (font (10.5f, 700));
            g.setColour (Colours::textSubtle);
            g.drawText (d.caption, text, juce::Justification::centredLeft, false);
            text.removeFromLeft (textWidth (font (10.5f, 700), d.caption) + 4.0f);
        }

        g.setFont (font (11.5f, 600));
        g.setColour (valid ? (active ? d.colour.brighter (0.35f) : Colours::text) : Colours::textFaint);
        g.drawFittedText (d.binding.text(), text.toNearestInt(),
                          d.caption.isNotEmpty() ? juce::Justification::centredRight : juce::Justification::centred, 1, 0.75f);
        return;
    }

    // Vertical: caption on top, value at the bottom, slim track with a thumb.
    const auto captionArea = b.removeFromTop (d.caption.isNotEmpty() ? 15.0f : 0.0f);
    const auto valueArea = b.removeFromBottom (16.0f);
    const auto trackArea = b.reduced (0.0f, 6.0f).withSizeKeepingCentre (6.0f, juce::jmax (8.0f, b.getHeight() - 12.0f));

    if (d.caption.isNotEmpty())
    {
        g.setFont (font (11.0f, 600));
        g.setColour (hover ? Colours::text : Colours::textDim);
        g.drawFittedText (d.caption, captionArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (trackArea, 3.0f);
    const float y0 = trackArea.getBottom() - trackArea.getHeight() * juce::jmax (originNorm, norm);
    const float y1 = trackArea.getBottom() - trackArea.getHeight() * juce::jmin (originNorm, norm);

    if (valid && y1 - y0 > 0.5f)
    {
        g.setColour (d.colour.withAlpha (0.85f));
        g.fillRoundedRectangle (juce::Rectangle<float> (trackArea.getX(), y0, trackArea.getWidth(), y1 - y0), 3.0f);
    }

    const float thumbY = trackArea.getBottom() - trackArea.getHeight() * norm;
    const auto thumb = juce::Rectangle<float> (juce::jmin (b.getWidth() - 2.0f, 22.0f), 10.0f).withCentre ({ trackArea.getCentreX(), thumbY });
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (thumb.translated (0.0f, 1.0f), 4.0f);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::text, thumb.getY(), Colours::textDim, thumb.getBottom()));
    g.fillRoundedRectangle (thumb, 4.0f);
    g.setColour (d.colour);
    g.fillRect (thumb.withSizeKeepingCentre (thumb.getWidth() - 8.0f, 1.5f));

    g.setFont (font (11.5f, 600));
    g.setColour (valid ? Colours::text : Colours::textFaint);
    g.drawFittedText (d.binding.text(), valueArea.toNearestInt(), juce::Justification::centred, 1, 0.7f);
}

void ParamFader::mouseDown (const juce::MouseEvent& e)        { impl->drag.mouseDown (e); }
void ParamFader::mouseDrag (const juce::MouseEvent& e)        { impl->drag.mouseDrag (e); repaint(); }
void ParamFader::mouseUp (const juce::MouseEvent& e)          { impl->drag.mouseUp (e); repaint(); }
void ParamFader::mouseDoubleClick (const juce::MouseEvent& e) { impl->drag.mouseDoubleClick (e); }
void ParamFader::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) { impl->drag.mouseWheelMove (e, w); }

//==============================================================================
struct ParamToggle::Impl
{
    Impl (ParamToggle& o, PureSheenProcessor& p) : owner (o), binding (p, [this] { owner.repaint(); }) {}

    ParamToggle& owner;
    Binding binding;
    juce::String label;
    juce::Colour onColour = Colours::accent;
};

ParamToggle::ParamToggle (PureSheenProcessor& p, const juce::String& paramId, const juce::String& label)
    : impl (std::make_unique<Impl> (*this, p))
{
    setNoFocus (*this);
    setRepaintsOnMouseActivity (true);
    impl->label = label;
    setParamId (paramId);
}

ParamToggle::~ParamToggle() = default;

void ParamToggle::setParamId (const juce::String& paramId)
{
    impl->binding.bind (paramId);
    setTooltip (impl->binding.hostName());
    setTitle (impl->label.isNotEmpty() ? impl->label : impl->binding.hostName());
    repaint();
}

void ParamToggle::setLinkedParamId (const juce::String& paramId) { impl->binding.link (paramId); }
void ParamToggle::setLabel (const juce::String& l) { impl->label = l; repaint(); }
void ParamToggle::setOnColour (juce::Colour c) { impl->onColour = c; repaint(); }

void ParamToggle::paint (juce::Graphics& g)
{
    auto& d = *impl;
    const bool valid = d.binding.isValid();
    const bool on = valid && d.binding.get() >= 0.5f;
    const bool hover = isMouseOver (true) && valid && isEnabled();
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = b.getHeight() * 0.5f;

    auto fill = on ? blend (Colours::panelRaised, d.onColour, 0.2f) : (hover ? Colours::panelHover : Colours::panelRaised);
    auto outline = on ? d.onColour.withAlpha (0.7f) : Colours::outline.brighter (hover ? 0.2f : 0.0f);

    if (! isEnabled())
        fill = fill.withMultipliedAlpha (0.5f), outline = outline.withMultipliedAlpha (0.5f);

    drawChip (g, b, radius, fill, outline);

    const float led = juce::jmin (7.0f, b.getHeight() * 0.32f);
    const auto ledArea = juce::Rectangle<float> (led, led).withCentre ({ b.getX() + radius + 1.0f, b.getCentreY() });
    drawLed (g, ledArea, on, d.onColour);

    g.setFont (font (juce::jmin (12.0f, b.getHeight() * 0.5f), 600));
    g.setColour (! isEnabled() ? Colours::textFaint : (on ? Colours::text : (hover ? Colours::text : Colours::textDim.brighter (0.1f))));
    g.drawFittedText (d.label, b.withTrimmedLeft (ledArea.getRight() - b.getX() + 6.0f).withTrimmedRight (8.0f).toNearestInt(),
                      juce::Justification::centredLeft, 1, 0.75f);
}

void ParamToggle::mouseUp (const juce::MouseEvent& e)
{
    if (! isLeftClick (e) || ! isEnabled() || ! getLocalBounds().contains (e.getPosition()) || ! impl->binding.isValid())
        return;

    impl->binding.setComplete (impl->binding.get() >= 0.5f ? 0.0f : 1.0f);
}

//==============================================================================
struct ParamChoice::Impl
{
    Impl (ParamChoice& o, PureSheenProcessor& p) : owner (o), binding (p, [this] { owner.repaint(); }) {}

    ParamChoice& owner;
    Binding binding;
    juce::String label;
    bool menuOpen = false;
};

ParamChoice::ParamChoice (PureSheenProcessor& p, const juce::String& paramId, const juce::String& label)
    : impl (std::make_unique<Impl> (*this, p))
{
    setNoFocus (*this);
    setRepaintsOnMouseActivity (true);
    impl->label = label;
    setParamId (paramId);
}

ParamChoice::~ParamChoice() = default;

void ParamChoice::setParamId (const juce::String& paramId)
{
    impl->binding.bind (paramId);
    setTooltip (impl->binding.hostName());
    setTitle (impl->label.isNotEmpty() ? impl->label : impl->binding.hostName());
    repaint();
}

void ParamChoice::setLinkedParamId (const juce::String& paramId) { impl->binding.link (paramId); }

void ParamChoice::paint (juce::Graphics& g)
{
    auto& d = *impl;
    const bool valid = d.binding.isValid();
    const bool hover = isMouseOver (true) && valid && isEnabled();
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (7.0f, b.getHeight() * 0.5f);

    drawChip (g, b, radius, d.menuOpen ? Colours::panelHover : (hover ? Colours::panelHover.withAlpha (0.9f) : Colours::panelRaised),
              d.menuOpen ? Colours::accent.withAlpha (0.7f) : Colours::outline.brighter (hover ? 0.18f : 0.0f));

    auto text = b.reduced (10.0f, 0.0f).withTrimmedRight (14.0f);
    const float fontH = juce::jmin (12.0f, b.getHeight() * 0.52f);

    if (d.label.isNotEmpty())
    {
        const auto f = font (fontH - 0.5f, 600);
        const float w = juce::jmin (text.getWidth() * 0.45f, textWidth (f, d.label) + 2.0f);
        g.setFont (f);
        g.setColour (Colours::textDim);
        g.drawText (d.label, text.removeFromLeft (w), juce::Justification::centredLeft, true);
        text.removeFromLeft (6.0f);
    }

    g.setFont (font (fontH, 600));
    g.setColour (valid && isEnabled() ? Colours::text : Colours::textFaint);
    g.drawFittedText (d.binding.text(), text.toNearestInt(),
                      d.label.isNotEmpty() ? juce::Justification::centredRight : juce::Justification::centredLeft, 1, 0.7f);

    const float cx = b.getRight() - 11.0f, cy = b.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.0f, cy - 1.4f);
    chevron.lineTo (cx, cy + 1.6f);
    chevron.lineTo (cx + 3.0f, cy - 1.4f);
    g.setColour (hover || d.menuOpen ? Colours::text : Colours::textDim);
    g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ParamChoice::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const auto* spec = d.binding.getSpec();

    if (! isEnabled() || spec == nullptr || d.menuOpen || ! (isLeftClick (e) || e.mods.isPopupMenu()))
        return;

    juce::PopupMenu menu;
    const int current = juce::roundToInt (d.binding.get());

    if (spec->kind == ps::ParamKind::Bool)
    {
        menu.addItem (1, "Off", true, current == 0);
        menu.addItem (2, "On", true, current == 1);
    }
    else
    {
        for (size_t i = 0; i < spec->choices.size(); ++i)
            menu.addItem ((int) i + 1, juce::String (spec->choices[i]), true, (int) i == current);
    }

    d.menuOpen = true;
    repaint();
    showMenu (menu, *this, [this] (int result)
    {
        impl->menuOpen = false;
        repaint();

        if (result > 0)
            impl->binding.setComplete ((float) (result - 1));
    }, getWidth());
}

//==============================================================================
struct ParamSegments::Impl
{
    Impl (ParamSegments& o, PureSheenProcessor& p) : owner (o), binding (p, [this] { owner.repaint(); }) {}

    juce::StringArray labels() const
    {
        juce::StringArray result;
        const auto* spec = binding.getSpec();

        if (spec == nullptr)
            return result;

        const int count = spec->kind == ps::ParamKind::Bool ? 2 : (int) spec->choices.size();

        for (int i = 0; i < count; ++i)
        {
            if (override.size() > 0)
            {
                if (i >= override.size())
                    break;

                result.add (override[i]);
            }
            else
            {
                result.add (spec->kind == ps::ParamKind::Bool ? (i == 0 ? "Off" : "On") : juce::String (spec->choices[(size_t) i]));
            }
        }

        return result;
    }

    ParamSegments& owner;
    Binding binding;
    juce::StringArray override;
    juce::Colour onColour = Colours::accent;
};

ParamSegments::ParamSegments (PureSheenProcessor& p, const juce::String& paramId, juce::StringArray labelsOverride)
    : impl (std::make_unique<Impl> (*this, p))
{
    setNoFocus (*this);
    setRepaintsOnMouseActivity (true);
    impl->override = std::move (labelsOverride);
    setParamId (paramId);
}

ParamSegments::~ParamSegments() = default;

void ParamSegments::setParamId (const juce::String& paramId)
{
    impl->binding.bind (paramId);
    setTooltip (impl->binding.hostName());
    setTitle (impl->binding.hostName());
    repaint();
}

void ParamSegments::setLinkedParamId (const juce::String& paramId) { impl->binding.link (paramId); }
void ParamSegments::setOnColour (juce::Colour c) { impl->onColour = c; repaint(); }

void ParamSegments::paint (juce::Graphics& g)
{
    auto& d = *impl;
    const auto labels = d.labels();
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (7.0f, b.getHeight() * 0.5f);

    g.setColour (juce::Colours::black.withAlpha (0.22f));
    g.fillRoundedRectangle (b, radius);
    g.setColour (Colours::outline.withAlpha (0.9f));
    g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);

    if (labels.isEmpty())
        return;

    const auto inner = b.reduced (2.0f);
    const float segW = inner.getWidth() / (float) labels.size();
    const int current = juce::roundToInt (d.binding.get());
    const auto mouse = getMouseXYRelative().toFloat();
    const int hover = isMouseOver() && isEnabled() ? juce::jlimit (0, labels.size() - 1, (int) ((mouse.x - inner.getX()) / segW)) : -1;
    const float fontH = juce::jmin (12.0f, inner.getHeight() * 0.55f);

    for (int i = 0; i < labels.size(); ++i)
    {
        const auto seg = juce::Rectangle<float> (inner.getX() + segW * (float) i, inner.getY(), segW, inner.getHeight());
        const bool selected = i == current;

        if (selected)
        {
            g.setGradientFill (juce::ColourGradient::vertical (blend (Colours::panelHover, d.onColour, 0.30f), seg.getY(),
                                                               blend (Colours::panelRaised, d.onColour, 0.18f), seg.getBottom()));
            g.fillRoundedRectangle (seg.reduced (0.5f), radius - 2.0f);
            g.setColour (d.onColour.withAlpha (0.7f));
            g.drawRoundedRectangle (seg.reduced (1.0f), radius - 2.0f, 1.0f);
        }
        else if (i == hover)
        {
            g.setColour (Colours::panelHover.withAlpha (0.7f));
            g.fillRoundedRectangle (seg.reduced (0.5f), radius - 2.0f);
        }

        g.setFont (font (fontH, selected ? 700 : 600));
        g.setColour (! isEnabled() ? Colours::textFaint
                                   : (selected ? d.onColour.brighter (0.45f) : (i == hover ? Colours::text : Colours::textDim)));
        g.drawFittedText (labels[i], seg.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }
}

void ParamSegments::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const auto labels = d.labels();

    if (! isLeftClick (e) || ! isEnabled() || labels.isEmpty() || ! getLocalBounds().contains (e.getPosition()))
        return;

    const float segW = ((float) getWidth() - 4.0f) / (float) labels.size();
    const int index = juce::jlimit (0, labels.size() - 1, (int) ((e.position.x - 2.0f) / segW));

    if (index != juce::roundToInt (d.binding.get()))
        d.binding.setComplete ((float) index);
}

//==============================================================================
namespace
{
float melOf (float hz) noexcept { return 2595.0f * std::log10 (1.0f + hz / 700.0f); }
float hzOfMel (float mel) noexcept { return 700.0f * (std::pow (10.0f, mel / 2595.0f) - 1.0f); }
} // namespace

float FreqAxis::hzToX (float hz) const noexcept
{
    float t = 0.0f;

    if (mel)
        t = (melOf (hz) - melOf (minHz)) / juce::jmax (1.0e-6f, melOf (maxHz) - melOf (minHz));
    else
        t = std::log (juce::jmax (hz, 1.0f) / minHz) / std::log (maxHz / minHz);

    return area.getX() + t * area.getWidth();
}

float FreqAxis::xToHz (float x) const noexcept
{
    const float t = area.getWidth() > 0.0f ? (x - area.getX()) / area.getWidth() : 0.0f;

    if (mel)
        return hzOfMel (melOf (minHz) + t * (melOf (maxHz) - melOf (minHz)));

    return minHz * std::pow (maxHz / minHz, t);
}

float DbAxis::dbToY (float db) const noexcept
{
    const float t = (db - minDb) / (maxDb - minDb);
    return area.getBottom() - t * area.getHeight();
}

float DbAxis::yToDb (float y) const noexcept
{
    const float t = area.getHeight() > 0.0f ? (area.getBottom() - y) / area.getHeight() : 0.0f;
    return minDb + t * (maxDb - minDb);
}

FreqAxis makeFreqAxis (const UiOptions& o, juce::Rectangle<float> area, double sampleRate)
{
    FreqAxis axis;
    axis.area = area;
    axis.mel = o.freqScale == UiOptions::FreqScale::Mel;

    if (o.freqScale == UiOptions::FreqScale::ExtendedLog)
    {
        axis.minHz = 10.0f;
        axis.maxHz = 30000.0f;

        if (sampleRate > 0.0)
            axis.maxHz = juce::jmin (axis.maxHz, (float) (sampleRate * 0.5));
    }

    return axis;
}

void drawFrequencyGrid (juce::Graphics& g, const FreqAxis& fx, const DbAxis& dy, bool labels)
{
    const auto area = fx.area;
    static const float majors[] = { 10.0f, 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };
    static const float minors[] = { 30.0f, 40.0f, 60.0f, 70.0f, 80.0f, 90.0f, 300.0f, 400.0f, 600.0f, 700.0f, 800.0f, 900.0f,
                                    3000.0f, 4000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f };

    g.setColour (Colours::outline.withAlpha (0.22f));

    for (float hz : minors)
        if (hz > fx.minHz && hz < fx.maxHz)
            g.fillRect (juce::Rectangle<float> (std::round (fx.hzToX (hz)), area.getY(), 1.0f, area.getHeight()));

    for (float hz : majors)
    {
        if (hz <= fx.minHz * 1.01f || hz >= fx.maxHz * 0.99f)
            continue;

        const float x = std::round (fx.hzToX (hz));
        g.setColour (Colours::outline.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float> (x, area.getY(), 1.0f, area.getHeight()));

        if (labels)
        {
            const juce::String text = hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k"
                                                    : juce::String (juce::roundToInt (hz));
            g.setFont (font (10.0f, 600));
            g.setColour (Colours::textSubtle);
            g.drawText (text, juce::Rectangle<float> (x + 3.0f, area.getBottom() - 14.0f, 30.0f, 12.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    // dB lines: a step that gives 4..8 lines.
    const float range = dy.maxDb - dy.minDb;
    float step = 3.0f;

    for (float s : { 3.0f, 6.0f, 10.0f, 12.0f, 20.0f, 24.0f, 30.0f })
    {
        step = s;
        if (range / s <= 8.0f)
            break;
    }

    const float first = std::ceil (dy.minDb / step) * step;

    for (float db = first; db <= dy.maxDb + 0.01f; db += step)
    {
        const float y = std::round (dy.dbToY (db));

        if (y < area.getY() + 1.0f || y > area.getBottom() - 1.0f)
            continue;

        const bool zero = std::abs (db) < 0.01f;
        g.setColour (Colours::outline.withAlpha (zero ? 0.75f : 0.35f));
        g.fillRect (juce::Rectangle<float> (area.getX(), y, area.getWidth(), 1.0f));

        if (labels)
        {
            g.setFont (font (10.0f, 600));
            g.setColour (zero ? Colours::textDim : Colours::textSubtle);
            g.drawText ((db > 0.0f ? "+" : "") + juce::String (juce::roundToInt (db)),
                        juce::Rectangle<float> (area.getX() + 4.0f, y - 12.0f, 40.0f, 11.0f),
                        juce::Justification::centredLeft, false);
        }
    }
}

//==============================================================================
juce::Colour moduleColour (ps::ModuleType type)
{
    switch (type)
    {
        case ps::ModuleType::Equalizer1: return juce::Colour (0xff62a8f2);   // sky
        case ps::ModuleType::Equalizer2: return juce::Colour (0xff5fc4e8);   // cyan
        case ps::ModuleType::Dynamics:   return juce::Colour (0xfff1b562);   // amber
        case ps::ModuleType::Imager:     return juce::Colour (0xffaa8cf2);   // lavender
        case ps::ModuleType::Exciter:    return juce::Colour (0xfff08a6f);   // coral
        case ps::ModuleType::Maximizer:  return juce::Colour (0xff7ad3b8);   // brand mint
        case ps::ModuleType::DynamicEq:   return juce::Colour (0xff7f9cf5);   // periwinkle
        case ps::ModuleType::MatchEq:     return juce::Colour (0xffe8c46a);   // gold
        case ps::ModuleType::ToneBalance: return juce::Colour (0xff6fcf97);   // leaf
        case ps::ModuleType::Definition:  return juce::Colour (0xff58d6d0);   // aqua
        case ps::ModuleType::Smoother:    return juce::Colour (0xffd59cf0);   // orchid
        case ps::ModuleType::Punch:       return juce::Colour (0xfff07a9a);   // rose
        case ps::ModuleType::LowContrast: return juce::Colour (0xffc9a27e);   // tan
        case ps::ModuleType::BassShaper:  return juce::Colour (0xffe39a52);   // copper
        case ps::ModuleType::Count:
        default: break;
    }

    return Colours::accent;
}

//==============================================================================
ps::ChannelMode currentChannelMode (const PureSheenProcessor& p, ps::ModuleType type)
{
    const auto choices = ps::channelModeChoices (type);
    if (choices.empty())
        return ps::ChannelMode::Stereo;

    const juce::String prefix (ps::moduleInfo (type).idPrefix);
    if (type == ps::ModuleType::Exciter && p.getParameterValue ("exc.ts.on") >= 0.5f)
        return ps::ChannelMode::TransientSustain;

    const int index = juce::jlimit (0, (int) choices.size() - 1, juce::roundToInt (p.getParameterValue (prefix + ".chMode")));
    return choices[(size_t) index];
}

bool isDualChannelMode (ps::ChannelMode m) noexcept
{
    return m == ps::ChannelMode::MidSide || m == ps::ChannelMode::LeftRight || m == ps::ChannelMode::TransientSustain;
}

juce::String channelSetName (ps::ChannelMode m, int set)
{
    switch (m)
    {
        case ps::ChannelMode::MidSide:          return set == 0 ? "Mid" : "Side";
        case ps::ChannelMode::LeftRight:        return set == 0 ? "Left" : "Right";
        case ps::ChannelMode::TransientSustain: return set == 0 ? "Transient" : "Sustain";
        case ps::ChannelMode::Stereo:
        default: break;
    }
    return {};
}

juce::String channelModeShortLabel (ps::ChannelMode m)
{
    switch (m)
    {
        case ps::ChannelMode::Stereo:           return "Stereo";
        case ps::ChannelMode::MidSide:          return "M/S";
        case ps::ChannelMode::LeftRight:        return "L/R";
        case ps::ChannelMode::TransientSustain: return "T/S";
    }
    return {};
}

ps::Module& moduleForChannelSet (PureSheenProcessor& p, ps::ModuleType type, int set)
{
    const bool twin = set == 1 && currentChannelMode (p, type) == ps::ChannelMode::TransientSustain;
    return p.getChain().getModule (type, twin ? 1 : 0);
}

} // namespace ps::ui
