#pragma once

#include "ui/EditorContext.h"

// The module chain (70 px): one tile per module in processing order (module colour, power LED, Solo, Delta,
// difference meter, remove x on hover), drag a tile to reorder (the other tiles slide apart around a mint insertion
// marker), "+" adds a module that is not in the chain, a click selects. Right-click on a tile: its menu.
// Owner: ui-shell.
namespace ps::ui
{
class ChainStrip final : public juce::Component, public juce::TooltipClient
{
public:
    explicit ChainStrip (EditorContext&);
    ~ChainStrip() override;

    void refresh();      // chain order / selection changed
    void tick();         // 30 Hz: difference meters, LEDs, animation

    // Test hooks (snapshot tool): simulate a drag of the tile at `fromIndex` to x (component coordinates).
    juce::Rectangle<int> getTileBounds (int index) const;
    void simulateDrag (int fromIndex, float toX, bool release);

    juce::String getTooltip() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace ps::ui
