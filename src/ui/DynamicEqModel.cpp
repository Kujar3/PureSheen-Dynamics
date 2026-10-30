#include "ui/DynamicEqModel.h"

// Dynamic EQ panel helpers (owner: ui-deq). See DynamicEqModel.h.
namespace ps::ui
{
DynEqShapeTraits dynEqShapeTraits (ps::DynEqShape s) noexcept
{
    DynEqShapeTraits t;

    switch (s)
    {
        case ps::DynEqShape::LowShelfBaxandall:  t.q = false; t.lowShelf = true; break;
        case ps::DynEqShape::HighShelfBaxandall: t.q = false; t.highShelf = true; break;
        case ps::DynEqShape::LowShelfAnalog:     t.lowShelf = true; break;
        case ps::DynEqShape::HighShelfAnalog:    t.highShelf = true; break;
        case ps::DynEqShape::Bell:
        case ps::DynEqShape::ProportionalQ:
        case ps::DynEqShape::BandShelf:
        case ps::DynEqShape::Count:
        default: break;
    }

    return t;
}

juce::String dynEqShapeShortName (ps::DynEqShape s)
{
    switch (s)
    {
        case ps::DynEqShape::Bell:               return "Bell";
        case ps::DynEqShape::ProportionalQ:      return "Prop Q";
        case ps::DynEqShape::BandShelf:          return "Band Shelf";
        case ps::DynEqShape::LowShelfBaxandall:  return "Baxandall Low";
        case ps::DynEqShape::HighShelfBaxandall: return "Baxandall High";
        case ps::DynEqShape::LowShelfAnalog:     return "Low Shelf";
        case ps::DynEqShape::HighShelfAnalog:    return "High Shelf";
        case ps::DynEqShape::Count:
        default: break;
    }

    return {};
}

juce::String dynEqShapeFullName (ps::DynEqShape s)
{
    const int index = ps::paramIndex ("deq.a.b1.shape");
    const auto i = (size_t) juce::jlimit (0, (int) ps::DynEqShape::Count - 1, (int) s);

    if (index >= 0 && i < ps::paramSpecs()[(size_t) index].choices.size())
        return ps::paramSpecs()[(size_t) index].choices[i];

    return dynEqShapeShortName (s);
}

juce::String dynEqSidechainName (ps::DynEqSidechain s)
{
    const int index = ps::paramIndex ("deq.a.b1.sc");
    const auto i = (size_t) juce::jlimit (0, (int) ps::DynEqSidechain::Count - 1, (int) s);

    if (index >= 0 && i < ps::paramSpecs()[(size_t) index].choices.size())
        return ps::paramSpecs()[(size_t) index].choices[i];

    return {};
}

juce::PopupMenu dynEqShapeMenu (ps::DynEqShape current)
{
    struct Group { const char* name; std::vector<std::pair<ps::DynEqShape, const char*>> shapes; };
    const Group groups[] = {
        { "Bell", { { ps::DynEqShape::Bell, "Bell" }, { ps::DynEqShape::ProportionalQ, "Proportional Q" },
                    { ps::DynEqShape::BandShelf, "Band Shelf" } } },
        { "Low shelf", { { ps::DynEqShape::LowShelfAnalog, "Analog" }, { ps::DynEqShape::LowShelfBaxandall, "Baxandall" } } },
        { "High shelf", { { ps::DynEqShape::HighShelfAnalog, "Analog" }, { ps::DynEqShape::HighShelfBaxandall, "Baxandall" } } },
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

ps::DynEqShape dynEqDefaultShapeForNewNode (const ps::DynamicEqBandParams* bands, double hz) noexcept
{
    auto used = [bands] (ps::DynEqShape s)
    {
        for (int b = 0; b < ps::kDynEqBands; ++b)
            if (bands[b].on && bands[b].shape == s)
                return true;

        return false;
    };

    if (hz < 100.0 && ! used (ps::DynEqShape::LowShelfBaxandall))
        return ps::DynEqShape::LowShelfBaxandall;

    if (hz > 8000.0 && ! used (ps::DynEqShape::HighShelfBaxandall))
        return ps::DynEqShape::HighShelfBaxandall;

    return ps::DynEqShape::ProportionalQ;
}

float dynEqDefaultOf (const juce::String& id)
{
    const int i = ps::paramIndex (id.toStdString());
    return i >= 0 ? ps::paramSpecs()[(size_t) i].defaultValue : 0.0f;
}

//==============================================================================
DynamicEqModel::DynamicEqModel (PureSheenProcessor& p) : processor (p)
{
    const auto& specs = ps::paramSpecs();
    values.assign (specs.size(), 0.0f);
    auto& state = processor.getValueTreeState();

    for (size_t i = 0; i < specs.size(); ++i)
    {
        if (specs[i].module != (int) ps::ModuleType::DynamicEq)
            continue;

        indices.push_back ((int) i);
        raw.push_back (state.getRawParameterValue (specs[i].id));
        values[i] = specs[i].defaultValue;
    }

    chModeRaw = state.getRawParameterValue ("deq.chMode");
    linkRaw = state.getRawParameterValue ("deq.link");
    update();
}

bool DynamicEqModel::update()
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

    const auto modes = ps::channelModeChoices (ps::ModuleType::DynamicEq);
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
        ps::readDynamicEqParams (values.data(), params);

    return changed;
}

juce::String DynamicEqModel::bandId (int set, int b, const char* name) const
{
    return juce::String (set == 1 ? "deq.b.b" : "deq.a.b") + juce::String (b + 1) + "." + name;
}

juce::String DynamicEqModel::moduleId (const char* name) const { return juce::String ("deq.") + name; }

juce::RangedAudioParameter* DynamicEqModel::bandParam (int set, int b, const char* name) const
{
    return processor.getParameter (bandId (set, b, name));
}

std::vector<int> DynamicEqModel::editSets (int set) const
{
    if (isLinked())
        return { set, 1 - set };

    return { isSplit() ? set : 0 };
}

juce::String DynamicEqModel::bandLabel (int set, int b) const
{
    const auto name = isLinked() || ! isSplit() ? juce::String() : setName (set);
    return juce::String (ps::moduleInfo (ps::ModuleType::DynamicEq).shortName) + " Band " + juce::String (b + 1)
           + (name.isNotEmpty() ? " (" + name + ")" : juce::String());
}
} // namespace ps::ui
