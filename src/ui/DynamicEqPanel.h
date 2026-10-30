#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Dynamic EQ panel. Owner: ui-deq. Implementation: DynamicEqPanel.cpp (+ DynamicEqDisplay, DynamicEqModel).
// Contract: the editor creates it through createModulePanel (ModulePanels.cpp) for ps::ModuleType::DynamicEq; it is a
// ModulePanel (the base draws the header: name, presets, channel modes, channel tabs, Delta, Reset).
//
//   +--------------------------------------------------------------------------------------+---------+
//   | DynamicEqDisplay: spectra, rest / live curves, band travel ranges, nodes with Up /    | GAIN    |
//   | Down arrows, threshold bar and detector level of the selected band (ui/DynamicEqDisplay.h) | column  |
//   +--------------------------------------------------------------------------------------+---------+
//   | BANDS | Band n on   | Freq  Gain  Q     Offset  | [meter] Threshold | Dynamic    | Analog | Digital  Lookahead |
//   | chips | Shape v     |                           |          Now x dB | Auto scale | FIR size / latency          |
//   | 3 x 2 | Down | Up   | Ratio Knee  Attack Release|                   | Listen to v|                             |
//   +--------------------------------------------------------------------------------------+---------+
// The GAIN column is shown when the display area is at least 900 px wide; below 700 px of band card width the chips
// become a "Band n" drop-down. Band controls follow the selected band and the header's channel tab; with Link on
// (M/S, L/R, T/S) every edit also goes to the other channel set. Controls that do not apply (Q of a Baxandall shelf)
// are disabled; the dynamic controls are dimmed while the band is static. Never takes keyboard focus; left-click-only
// edits; one gesture per drag.
namespace ps::ui
{
class DynamicEqPanel final : public ModulePanel
{
public:
    DynamicEqPanel (PureSheenProcessor&, EditorServices&);
    ~DynamicEqPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    void channelSetChanged() override;
    void paint (juce::Graphics&) override;

    // (tests / snapshot checks) the band the strip edits, 0..5, and select it.
    int getSelectedBand() const noexcept;
    void setSelectedBand (int band);

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
