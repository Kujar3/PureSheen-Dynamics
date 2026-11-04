#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Punch panel. Owner: ui-punch. Implementation: PunchPanel.cpp (+ PunchViews.h/.cpp).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::Punch; it is a
// ModulePanel (the base draws the header: name, presets, channel modes Stereo | M/S, the Mid | Side tabs with Link,
// Learn (crossovers), Delta, Reset).
//
//   +---------------------------------------------------------------------------------------------+
//   | MultibandDisplay: spectra, crossovers, band count, band headers (S solo), and per band:      |
//   |   - the amount as a bar from the centre line (up = punchier, rose; down = softer, sky) with a |
//   |     value pill; drag vertically anywhere in the band (one gesture; Ctrl / Shift = fine;      |
//   |     double-click = type a value; right-click = menu); with Link Bands every band moves by the same     |
//   |     amount                                                                                    |
//   |   - the live gain trace (last 3 s, min / max per 30 Hz tick, +-12 dB over 70 % of the band's |
//   |     height; mint above 0 dB, amber below) and the current gain                                |
//   +-----------------------+---------------------------------------------------------------------+
//   | SETTINGS              | BAND 1        | BAND 2        | ...  one card per band: On, range,   |
//   | Amount  Envelope  Sync|               |               |      Amount knob, live gain meter,   |
//   | Crossover    St. Link |               |               |      micro-dynamics in / out meters  |
//   |              Auto Gain|               |               |      (+ make-up with Auto Gain)      |
//   |              Link Bnds|               |               |                                      |
//   +-----------------------+---------------------------------------------------------------------+
// With Sync on, the Envelope knob sets the note value ("pnch.syncNote", 1/64T .. 1/2D) instead of milliseconds.
// Stereo Link only acts in Stereo (dimmed in M/S). Band parameters follow the header's Mid | Side tab; with Link on
// every band edit also goes to the other set. Meters come from ps::PunchMeters (gainMinDb / gainMaxDb are exchanged
// once per tick; this panel is their only reader). Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class MultibandDisplay;

class PunchPanel final : public ModulePanel
{
public:
    PunchPanel (PureSheenProcessor&, EditorServices&);
    ~PunchPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;
    bool hasLearn() const override { return true; }

    MultibandDisplay& getBandDisplay() noexcept;   // test / tooling access

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
