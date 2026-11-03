#pragma once

#include "core/RefCompare.h"
#include "ui/Theme.h"

#include <array>
#include <memory>

// The comparison views of the Reference page (owner: referencing UI; ported from RefMatch's panels, restyled):
//   Tonal balance   the master's and the reference's spectrum at equal loudness (display tilt +4.5 dB / octave around
//                   1 kHz), and six band bars (+-6 dB, coloured by severity) with comparative words
//   Loudness        integrated, short-term (the reference as heard), true peak, PLR, LRA: master vs reference
//   Stereo          width per band (master vs reference), phase correlation, bass mono badge
//   What to try     the ranked hints (the first one large, the others as cards, or rows with the advice as a tooltip)
// RefComparison lays them out for the space it gets: a 2 x 2 grid when roomy, the tonal balance next to a tabbed view
// when medium, one tabbed view when small.
namespace ps::ui
{
// What the views show (the panel fills it from Track Referencing at the 30 Hz tick).
struct RefViewData
{
    ps::TrackProfile mix, reference;          // the master (PureSheen's output), the selected reference's region
    ps::Comparison comparison;
    float referenceShortTermLufs = ps::kNoValue;   // the reference as heard (unity gain)
    bool hasReference = false;                // a reference is selected
    bool referencePending = false;            // it is loading / being measured and has no profile yet
    juce::String referenceProblem;            // its file is missing / unreadable ("The reference file is missing")
};

juce::Colour severityColour (ps::Severity);

class RefComparison final : public juce::Component
{
public:
    RefComparison();
    ~RefComparison() override;

    // snap: jump to the values (a new reference / region) instead of easing towards them. Repaints what changed.
    void setData (const RefViewData&, bool snap);

    enum class View { tonal = 0, loudness, stereo, hints };
    void showView (View);   // the tab shown when the views do not all fit (also a snapshot hook)

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
