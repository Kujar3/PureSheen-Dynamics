#include "ui/EqModel.h"

// Equalizer panel helpers (owner: ui-eq). See EqModel.h.
namespace ps::ui
{
EqShapeTraits eqShapeTraits (ps::EqShape s) noexcept
{
    EqShapeTraits t;

    switch (s)
    {
        case ps::EqShape::LowShelfBaxandall:
        case ps::EqShape::HighShelfBaxandall:
            t.q = false;
            break;

        case ps::EqShape::HighPassFlat:
        case ps::EqShape::LowPassFlat:
            t.gain = false; t.q = false; t.slope = true; t.pass = true;
            break;

        case ps::EqShape::HighPassResonant:
        case ps::EqShape::LowPassResonant:
            t.gain = false; t.q = true; t.slope = true; t.pass = true;
            break;

        case ps::EqShape::HighPassBrickwall:
        case ps::EqShape::LowPassBrickwall:
            t.gain = false; t.q = false; t.slope = false; t.pass = true;
            break;

        case ps::EqShape::Bell:
        case ps::EqShape::ProportionalQ:
        case ps::EqShape::BandShelf:
        case ps::EqShape::LowShelfAnalog:
        case ps::EqShape::LowShelfVintage:
        case ps::EqShape::LowShelfResonant:
        case ps::EqShape::HighShelfAnalog:
        case ps::EqShape::HighShelfVintage:
        case ps::EqShape::HighShelfResonant:
        case ps::EqShape::Surgical:
        case ps::EqShape::Count:
        default:
            break;
    }

    return t;
}

juce::Colour eqBandColour (int band)
{
    static const juce::uint32 colours[ps::kEqBands] {
        0xffef8077,   // coral
        0xfff2a95f,   // orange
        0xffe6cf66,   // butter
        0xff93d27c,   // green
        0xff5ed0b4,   // teal
        0xff63b4f0,   // sky
        0xffa192f4,   // violet
        0xffe68ccf,   // pink
    };

    return juce::Colour (colours[juce::jlimit (0, ps::kEqBands - 1, band)]);
}

juce::String eqShapeShortName (ps::EqShape s)
{
    switch (s)
    {
        case ps::EqShape::Bell:               return "Bell";
        case ps::EqShape::ProportionalQ:      return "Prop Q";
        case ps::EqShape::BandShelf:          return "Band Shelf";
        case ps::EqShape::LowShelfAnalog:     return "Low Shelf";
        case ps::EqShape::LowShelfBaxandall:  return "Baxandall Low";
        case ps::EqShape::LowShelfVintage:    return "Vintage Low";
        case ps::EqShape::LowShelfResonant:   return "Resonant Low";
        case ps::EqShape::HighShelfAnalog:    return "High Shelf";
        case ps::EqShape::HighShelfBaxandall: return "Baxandall High";
        case ps::EqShape::HighShelfVintage:   return "Vintage High";
        case ps::EqShape::HighShelfResonant:  return "Resonant High";
        case ps::EqShape::HighPassFlat:       return "High Pass";
        case ps::EqShape::HighPassResonant:   return "Resonant HP";
        case ps::EqShape::HighPassBrickwall:  return "Brickwall HP";
        case ps::EqShape::LowPassFlat:        return "Low Pass";
        case ps::EqShape::LowPassResonant:    return "Resonant LP";
        case ps::EqShape::LowPassBrickwall:   return "Brickwall LP";
        case ps::EqShape::Surgical:           return "Surgical";
        case ps::EqShape::Count:
        default: break;
    }

    return {};
}

juce::String eqShapeFullName (ps::EqShape s)
{
    const auto index = (size_t) juce::jlimit (0, (int) ps::EqShape::Count - 1, (int) s);

    for (const auto& spec : ps::paramSpecs())
        if (spec.kind == ps::ParamKind::Choice && juce::String (spec.id).endsWith (".shape") && index < spec.choices.size())
            return spec.choices[index];

    return eqShapeShortName (s);
}

juce::PopupMenu eqShapeMenu (ps::EqShape current, bool digital)
{
    struct Group { const char* name; std::vector<std::pair<ps::EqShape, const char*>> shapes; };
    const Group groups[] = {
        { "Bell", { { ps::EqShape::Bell, "Bell" }, { ps::EqShape::ProportionalQ, "Proportional Q" },
                    { ps::EqShape::BandShelf, "Band Shelf" },
                    { ps::EqShape::Surgical, digital ? "Surgical" : "Surgical (Digital mode only)" } } },
        { "Low shelf", { { ps::EqShape::LowShelfAnalog, "Analog" }, { ps::EqShape::LowShelfBaxandall, "Baxandall" },
                         { ps::EqShape::LowShelfVintage, "Vintage" }, { ps::EqShape::LowShelfResonant, "Resonant" } } },
        { "High shelf", { { ps::EqShape::HighShelfAnalog, "Analog" }, { ps::EqShape::HighShelfBaxandall, "Baxandall" },
                          { ps::EqShape::HighShelfVintage, "Vintage" }, { ps::EqShape::HighShelfResonant, "Resonant" } } },
        { "High pass", { { ps::EqShape::HighPassFlat, "Flat" }, { ps::EqShape::HighPassResonant, "Resonant" },
                         { ps::EqShape::HighPassBrickwall, "Brickwall" } } },
        { "Low pass", { { ps::EqShape::LowPassFlat, "Flat" }, { ps::EqShape::LowPassResonant, "Resonant" },
                        { ps::EqShape::LowPassBrickwall, "Brickwall" } } },
    };

    juce::PopupMenu menu;

    for (const auto& group : groups)
    {
        juce::PopupMenu sub;
        bool containsCurrent = false;

        for (const auto& [shape, label] : group.shapes)
        {
            sub.addItem (1 + (int) shape, label, true, shape == current);
            containsCurrent = containsCurrent || shape == current;
        }

        menu.addSubMenu (group.name, sub, true, nullptr, containsCurrent);
    }

    return menu;
}

//==============================================================================
EqModel::EqModel (PureSheenProcessor& p, ps::ModuleType t)
    : processor (p), type (t), prefix (ps::moduleInfo (t).idPrefix)
{
    const auto& specs = ps::paramSpecs();
    values.assign (specs.size(), 0.0f);
    auto& state = processor.getValueTreeState();

    for (size_t i = 0; i < specs.size(); ++i)
    {
        if (specs[i].module != (int) type)
            continue;

        indices.push_back ((int) i);
        raw.push_back (state.getRawParameterValue (specs[i].id));
        values[i] = specs[i].defaultValue;
    }

    chModeRaw = state.getRawParameterValue (prefix + ".chMode");
    linkRaw = state.getRawParameterValue (prefix + ".link");
    update();
}

bool EqModel::update()
{
    bool changed = first;
    first = false;

    for (size_t k = 0; k < indices.size(); ++k)
    {
        if (raw[k] == nullptr)
            continue;

        const float v = raw[k]->load (std::memory_order_relaxed);
        float& slot = values[(size_t) indices[k]];

        if (v != slot)
        {
            slot = v;
            changed = true;
        }
    }

    const auto modes = ps::supportedChannelModes (type);
    auto newMode = ps::ChannelMode::Stereo;

    if (chModeRaw != nullptr && modes.size() > 1)
        newMode = modes[(size_t) juce::jlimit (0, (int) modes.size() - 1, juce::roundToInt (chModeRaw->load()))];

    const bool newLinked = linkRaw == nullptr || linkRaw->load() >= 0.5f;

    if (newMode != mode || newLinked != linked)
    {
        mode = newMode;
        linked = newLinked;
        changed = true;
    }

    if (changed)
        ps::readEqParams (values.data(), type, params);

    return changed;
}

juce::String EqModel::setName (int set) const
{
    if (mode == ps::ChannelMode::MidSide)
        return set == 0 ? "Mid" : "Side";

    if (mode == ps::ChannelMode::LeftRight)
        return set == 0 ? "Left" : "Right";

    return {};
}

juce::String EqModel::bandId (int set, int b, const char* name) const
{
    return prefix + (set == 1 ? ".b.b" : ".a.b") + juce::String (b + 1) + "." + name;
}

juce::String EqModel::moduleId (const char* name) const
{
    return prefix + "." + name;
}

juce::RangedAudioParameter* EqModel::bandParam (int set, int b, const char* name) const
{
    return processor.getParameter (bandId (set, b, name));
}

std::vector<int> EqModel::editSets (int set) const
{
    if (isLinked())
        return { set, 1 - set };

    return { isSplit() ? set : 0 };
}

float EqModel::scaledGain (const ps::EqBandParams& b) const noexcept
{
    return juce::jlimit (-30.0f, 15.0f, b.gainDb * amountScale());
}
} // namespace ps::ui
