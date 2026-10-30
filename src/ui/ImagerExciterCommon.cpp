#include "ui/ImagerExciterCommon.h"

#include "ui/Controls.h"

// Helpers shared by the Imager and Exciter panels. Owner: ui-stereo.
namespace ps::ui::stereo
{
//==============================================================================
ParamDrag::ParamDrag (PureSheenProcessor& p) : processor (p) {}

ParamDrag::~ParamDrag() { end(); }

void ParamDrag::start (const juce::String& paramId, const juce::String& linkedId)
{
    end();
    param = processor.getParameter (paramId);
    linked = linkedId.isNotEmpty() && linkedId != paramId ? processor.getParameter (linkedId) : nullptr;
    startNorm = param != nullptr ? param->getValue() : 0.0f;
}

void ParamDrag::dragTo (float deltaNormalised)
{
    if (param == nullptr)
        return;

    if (! active)
    {
        active = true;
        param->beginChangeGesture();

        if (linked != nullptr)
            linked->beginChangeGesture();
    }

    const auto& range = param->getNormalisableRange();
    const float plain = range.snapToLegalValue (range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, startNorm + deltaNormalised)));
    const float norm = param->convertTo0to1 (plain);

    if (std::abs (norm - param->getValue()) > 1.0e-7f)
        param->setValueNotifyingHost (norm);

    if (linked != nullptr && std::abs (linked->convertTo0to1 (plain) - linked->getValue()) > 1.0e-7f)
        linked->setValueNotifyingHost (linked->convertTo0to1 (plain));
}

void ParamDrag::end()
{
    if (active)
    {
        param->endChangeGesture();

        if (linked != nullptr)
            linked->endChangeGesture();
    }

    active = false;
    param = nullptr;
    linked = nullptr;
}

//==============================================================================
const ps::ParamSpec* specOf (const juce::String& paramId)
{
    const int index = ps::paramIndex (paramId.toStdString());
    return index >= 0 ? &ps::paramSpecs()[(size_t) index] : nullptr;
}

float defaultValueOf (const juce::String& paramId)
{
    const auto* spec = specOf (paramId);
    return spec != nullptr ? spec->defaultValue : 0.0f;
}

void setParam (PureSheenProcessor& p, const juce::String& paramId, const juce::String& linkedId, float plainValue)
{
    if (linkedId.isEmpty() || linkedId == paramId || p.getParameter (linkedId) == nullptr)
    {
        p.setParameterValue (paramId, plainValue);
        return;
    }

    const auto* spec = specOf (paramId);
    juce::String name = spec != nullptr ? juce::String (spec->name) : paramId;
    name = name.upToLastOccurrenceOf (" (", false, false);   // both channel sets change
    p.setParameterValues ({ { paramId, plainValue }, { linkedId, plainValue } }, name + " (linked)");
}

namespace
{
bool parseValue (const ps::ParamSpec& spec, juce::String text, float& result)
{
    text = text.trim().toLowerCase();

    if (text.isEmpty())
        return false;

    float scale = 1.0f;

    if (juce::String (spec.unit) == "Hz" && text.containsChar ('k'))
        scale = 1000.0f;

    const auto numeric = text.retainCharacters ("0123456789.-+");

    if (numeric.isEmpty() || ! numeric.containsAnyOf ("0123456789"))
        return false;

    result = juce::jlimit (spec.minValue, spec.maxValue, numeric.getFloatValue() * scale);
    return true;
}
} // namespace

void showParamMenu (juce::Component& target, PureSheenProcessor& p, const juce::String& paramId,
                    const juce::String& linkedId, const juce::String& title)
{
    const auto* spec = specOf (paramId);

    if (spec == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader (title);
    menu.addItem (1, "Reset to default");
    menu.addItem (2, "Enter value...");

    juce::Component::SafePointer<juce::Component> safe (&target);
    showMenu (menu, target, [&p, safe, paramId, linkedId, title, spec] (int result)
    {
        if (result == 1)
        {
            setParam (p, paramId, linkedId, spec->defaultValue);
        }
        else if (result == 2 && safe != nullptr)
        {
            const auto current = fmt::paramValue (p, paramId);
            promptForText (*safe, title, "Enter a value (" + juce::String (spec->minValue) + " to "
                                             + juce::String (spec->maxValue) + " " + juce::String (spec->unit) + ")",
                           current, [&p, paramId, linkedId, spec] (juce::String text)
                           {
                               float v = 0.0f;
                               if (parseValue (*spec, text, v))
                                   setParam (p, paramId, linkedId, v);
                           });
        }
    });
}

//==============================================================================
Card::Card (const juce::String& t, juce::Colour c) : title (t), colour (c)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

juce::Rectangle<int> Card::getBody() const noexcept
{
    return getLocalBounds().withTrimmedTop (kTitleH).reduced (kPad, 0).withTrimmedBottom (kPad - 2);
}

juce::Rectangle<int> Card::getTitleRowRight (int width) const noexcept
{
    return { getWidth() - kPad - width, (kTitleH - 22) / 2 + 1, width, 22 };
}

void Card::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                       Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

    auto titleArea = juce::Rectangle<int> (kPad, 0, getWidth() - 2 * kPad - titleReserve, kTitleH);

    if (badge > 0)
    {
        const auto box = juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ (float) kPad + 9.0f, (float) kTitleH * 0.5f + 1.0f });
        g.setColour (colour.withAlpha (0.2f));
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (colour);
        g.setFont (font (11.0f, 700));
        g.drawText (juce::String (badge), box, juce::Justification::centred, false);
        titleArea.removeFromLeft (26);
    }

    const auto titleFont = font (11.0f, 700).withExtraKerningFactor (0.08f);
    g.setFont (titleFont);
    g.setColour (Colours::text.withAlpha (0.9f));
    const auto upper = title.toUpperCase();
    g.drawText (upper, titleArea.translated (0, 1), juce::Justification::centredLeft, true);

    const int titleW = (int) textWidth (titleFont, upper) + 10;
    const auto rest = titleArea.withTrimmedLeft (titleW);

    if (inactiveNote.isNotEmpty())
    {
        // The note, or "Inactive" when it does not fit (nothing when even that does not fit).
        const auto noteFont = font (10.5f, 600);
        const auto text = textWidth (noteFont, inactiveNote) <= (float) rest.getWidth() ? inactiveNote
                          : (textWidth (noteFont, "Inactive") <= (float) rest.getWidth() ? juce::String ("Inactive") : juce::String());
        g.setFont (noteFont);
        g.setColour (Colours::minor.withAlpha (0.85f));
        g.drawText (text, rest.translated (0, 1), juce::Justification::centredRight, false);
    }
    else if (subtitle.isNotEmpty())
    {
        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawFittedText (subtitle, rest.translated (0, 1), juce::Justification::centredRight, 1, 0.8f);
    }

    for (const auto& c : captions)
    {
        g.setFont (font (11.5f, 600));
        g.setColour (Colours::textDim);
        g.drawFittedText (c.first, c.second, juce::Justification::centredLeft, 1, 0.8f);
    }

    if (bodyNote.isNotEmpty())
    {
        g.setFont (font (11.0f, 400));
        g.setColour (Colours::textSubtle);
        g.drawFittedText (bodyNote, getBodyNoteArea(), juce::Justification::bottomLeft, 3, 0.9f);
    }
}

juce::Rectangle<int> Card::getBodyNoteArea() const noexcept
{
    auto body = getBody();
    return body.removeFromBottom (juce::jmin (body.getHeight(), 44));
}

//==============================================================================
ChoiceSegments::ChoiceSegments (juce::StringArray l, juce::Colour c) : labels (std::move (l)), onColour (c)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void ChoiceSegments::setSelected (int index)
{
    index = juce::jlimit (0, juce::jmax (0, labels.size() - 1), index);

    if (index != selected)
    {
        selected = index;
        repaint();
    }
}

int ChoiceSegments::indexAt (juce::Point<float> p) const noexcept
{
    const auto inner = getLocalBounds().toFloat().reduced (2.5f);
    const float segW = inner.getWidth() / (float) juce::jmax (1, labels.size());
    return juce::jlimit (0, juce::jmax (0, labels.size() - 1), (int) ((p.x - inner.getX()) / segW));
}

void ChoiceSegments::paint (juce::Graphics& g)
{
    // Same look as ParamSegments.
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
    const int hover = isMouseOver() ? indexAt (getMouseXYRelative().toFloat()) : -1;
    const float fontH = juce::jmin (12.0f, inner.getHeight() * 0.55f);

    for (int i = 0; i < labels.size(); ++i)
    {
        const auto seg = juce::Rectangle<float> (inner.getX() + segW * (float) i, inner.getY(), segW, inner.getHeight());
        const bool on = i == selected;

        if (on)
        {
            g.setGradientFill (juce::ColourGradient::vertical (Colours::panelHover.interpolatedWith (onColour, 0.30f), seg.getY(),
                                                               Colours::panelRaised.interpolatedWith (onColour, 0.18f), seg.getBottom()));
            g.fillRoundedRectangle (seg.reduced (0.5f), radius - 2.0f);
            g.setColour (onColour.withAlpha (0.7f));
            g.drawRoundedRectangle (seg.reduced (1.0f), radius - 2.0f, 1.0f);
        }
        else if (i == hover)
        {
            g.setColour (Colours::panelHover.withAlpha (0.7f));
            g.fillRoundedRectangle (seg.reduced (0.5f), radius - 2.0f);
        }

        g.setFont (font (fontH, on ? 700 : 600));
        g.setColour (on ? onColour.brighter (0.45f) : (i == hover ? Colours::text : Colours::textDim));
        g.drawFittedText (labels[i], seg.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }
}

void ChoiceSegments::mouseUp (const juce::MouseEvent& e)
{
    if (! isLeftClick (e) || ! getLocalBounds().contains (e.getPosition()))
        return;

    const int index = indexAt (e.position);

    if (index != selected)
    {
        selected = index;
        repaint();

        if (onChange != nullptr)
            onChange (index);
    }
}

//==============================================================================
void drawWell (juce::Graphics& g, juce::Rectangle<float> area, float radius)
{
    g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), area.getY(),
                                                       Colours::background.darker (0.05f), area.getBottom()));
    g.fillRoundedRectangle (area, radius);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
}

} // namespace ps::ui::stereo
