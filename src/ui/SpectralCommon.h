#pragma once

#include "ui/Widgets.h"

#include <cmath>

// Helpers shared by the Tone Balance, Definition and Smoother panels. Owner: ui-spectral. Internal, not a contract.
namespace ps::ui::spectral
{
// The parameter set the panel edits and shows (0 in Stereo), whether A/B Link applies, and the module prefix.
struct SetState
{
    ps::ChannelMode mode = ps::ChannelMode::Stereo;
    int set = 0;
    bool split = false, linked = false;
};

inline SetState setState (const PureSheenProcessor& p, ps::ModuleType type, int panelSet)
{
    SetState s;
    s.mode = currentChannelMode (p, type);
    s.split = isDualChannelMode (s.mode);
    s.set = s.split ? juce::jlimit (0, 1, panelSet) : 0;
    s.linked = s.split && p.getParameterValue (juce::String (ps::moduleInfo (type).idPrefix) + ".link") >= 0.5f;
    return s;
}

// "<prefix>.<a|b>.<name>"
inline juce::String setParamId (ps::ModuleType type, int set, const char* name)
{
    return juce::String (ps::moduleInfo (type).idPrefix) + (set == 0 ? ".a." : ".b.") + name;
}

// The A/B twin of a per-set parameter while Link applies, else empty.
inline juce::String twinParamId (ps::ModuleType type, const SetState& s, const char* name)
{
    return s.linked ? setParamId (type, 1 - s.set, name) : juce::String();
}

inline juce::Colour setColour (ps::ModuleType type, const SetState& s)
{
    return s.set == 1 ? Colours::sideChannel : moduleColour (type);
}

// The module processes audio (in the chain, powered, prepared).
inline bool moduleRunning (PureSheenProcessor& p, ps::ModuleType type)
{
    return p.isModuleInChain (type) && p.getParameterValue (juce::String (ps::moduleInfo (type).idPrefix) + ".on") >= 0.5f
           && p.getChain().getSampleRate() > 0.0;
}

// A meter value that has been written by a running module (never-processed meters read exactly 0; silence < -140).
inline bool levelValid (float db) noexcept
{
    return std::isfinite (db) && db > -140.0f && db < -0.0001f;
}

// "+2.5" / "-3.0" / "0.0" (one decimal, no negative zero).
inline juce::String dbNumber (float db, bool sign = true)
{
    if (std::abs (db) < 0.05f)
        return "0.0";

    return (sign && db > 0.0f ? "+" : "") + juce::String (db, 1);
}

// Lays `count` components out evenly across `area` (knob rows), each at most maxW wide.
template <typename... Comps>
void layoutRow (juce::Rectangle<int> area, int maxW, Comps&... comps)
{
    constexpr int n = (int) sizeof...(Comps);
    const int w = juce::jmin (maxW, area.getWidth() / n);
    const int gap = n > 1 ? (area.getWidth() - w * n) / (n + 1) : 0;
    int x = area.getX() + (n > 1 ? gap : (area.getWidth() - w) / 2);
    const auto place = [&] (juce::Component& c)
    {
        c.setBounds (x, area.getY(), w, area.getHeight());
        x += w + gap;
    };
    (place (comps), ...);
}
} // namespace ps::ui::spectral
