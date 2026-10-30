// STUB (lead): replaced by ui-dynamics
#include "ui/MultibandDisplay.h"

#include "plugin/PluginProcessor.h"

namespace ps::ui
{
struct MultibandDisplay::Impl
{
    Impl (PureSheenProcessor& p, EditorServices& s, ps::ModuleType t) : proc (p), services (s), type (t) {}
    PureSheenProcessor& proc;
    EditorServices& services;
    ps::ModuleType type;
    int selected = 0;
    bool showSpectrum = true;
};

MultibandDisplay::MultibandDisplay (PureSheenProcessor& p, EditorServices& s, ps::ModuleType t)
    : impl (std::make_unique<Impl> (p, s, t))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

MultibandDisplay::~MultibandDisplay() = default;

void MultibandDisplay::tick() { repaint(); }
void MultibandDisplay::refresh() { repaint(); }
int MultibandDisplay::getNumBands() const noexcept { return 1; }
int MultibandDisplay::getSelectedBand() const noexcept { return impl->selected; }
void MultibandDisplay::setSelectedBand (int band) { impl->selected = band; repaint(); }
juce::Rectangle<float> MultibandDisplay::getBandArea (int) const noexcept { return getPlotArea(); }
juce::Rectangle<float> MultibandDisplay::getPlotArea() const noexcept { return getLocalBounds().toFloat(); }
float MultibandDisplay::xForFrequency (float) const noexcept { return 0.0f; }
float MultibandDisplay::frequencyForX (float) const noexcept { return 1000.0f; }
void MultibandDisplay::setShowSpectrum (bool shouldShow) { impl->showSpectrum = shouldShow; }
void MultibandDisplay::paint (juce::Graphics&) {}
void MultibandDisplay::resized() {}
void MultibandDisplay::mouseMove (const juce::MouseEvent&) {}
void MultibandDisplay::mouseDown (const juce::MouseEvent&) {}
void MultibandDisplay::mouseDrag (const juce::MouseEvent&) {}
void MultibandDisplay::mouseUp (const juce::MouseEvent&) {}
void MultibandDisplay::mouseDoubleClick (const juce::MouseEvent&) {}
void MultibandDisplay::mouseExit (const juce::MouseEvent&) {}
} // namespace ps::ui
