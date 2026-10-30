#pragma once

#include "ui/ModulePanel.h"

#include <memory>

// Dynamics module panel. Owner: ui-dynamics. Implementation: DynamicsPanel.cpp (+ DynamicsViews.h/.cpp).
//
//   +--------------------------------------------------------------+---------+-------------+
//   | MultibandDisplay: spectra, crossovers, band headers (S solo), | GAIN    | TRANSFER    |
//   | per band: compressor (C) / limiter (L) threshold lines        | meters, | curve of    |
//   | (drag up / down), live detector level and gain change         | one per | the selected|
//   |                                                              | band    | band        |
//   +--------------------------------------------------------------+---------+-------------+
//   | BAND n  range                              [Band on]          | DETECTION      [Hear]  |
//   | [Compressor] Threshold Ratio Attack Release Knee  | Mix        | Mode / Filter / Slope  |
//   | [Limiter]    Threshold Ratio Attack Release Knee  | Gain       | Freq, Tilt  | SETTINGS |
//   +--------------------------------------------------------------+------------------------+
// (At tall sizes the gain meters move under the transfer curve.)
//
// Band controls follow the selected band and the header's A/B tab (Mid / Side); with Link on (M/S) every edit also
// goes to the other channel set. Threshold drags in the display and in the transfer curve are one gesture each.
// Never takes keyboard focus; left-click-only edits.
namespace ps::ui
{
class DynamicsPanel final : public ModulePanel
{
public:
    DynamicsPanel (PureSheenProcessor&, EditorServices&);
    ~DynamicsPanel() override;

    void tick() override;
    void frameTick() override;
    void refresh() override;
    bool hasLearn() const override { return true; }
    void channelSetChanged() override;

    // (tests / dev tool) the band whose controls are shown
    int getSelectedBand() const noexcept;

protected:
    void layoutContent (juce::Rectangle<int> contentArea) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
