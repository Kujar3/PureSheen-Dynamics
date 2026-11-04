#include "ui/DeLimiterPanel.h"

#include "dsp/DeLimiter.h"
#include "ui/DeLimiterViews.h"
#include "ui/ImagerExciterCommon.h"

#include <algorithm>
#include <cmath>
#include <vector>

// De-limiter panel. Owner: mod-dlim. Layout and interactions: see DeLimiterPanel.h. Everything reads the module's meters
// and history at 30 Hz; the module's learn state comes through the processor, which also writes the result.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr int kLearnH = 34;
} // namespace

//==============================================================================
struct DeLimiterPanel::Impl
{
    Impl (DeLimiterPanel& o, PureSheenProcessor& p)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::DeLimiter)),
          card ("De-limiter", colour), amount (p, "dlim.amount", "Amount"), input (p, "dlim.input", "Input Gain"),
          output (p, "dlim.output", "Output Gain"), learn (colour), display (p, colour), pullL (8192), pullR (8192)
    {
        for (auto* k : { &amount, &input, &output })
            k->setColour (colour);

        amount.setTooltip ("Amount: how much of the lost peaks comes back. 100 % brings them back to the level the "
                           "module estimates they had, up to 200 % goes further for an enhanced effect, 0 % changes "
                           "nothing.\nDrag to change (Ctrl: fine), double-click to type a value, right-click to reset.");
        input.setTooltip ("Input Gain: the level going into the module. It also moves the signal against the Threshold.");
        output.setTooltip ("Output Gain: the level after the module.");

        learn.onClick = [this]
        {
            if (processor.isDeLimiterLearning())
            {
                processor.cancelDeLimiterLearn();
                sawLearning = false;
                owner.showHeaderHint ("Learn cancelled: Threshold unchanged");
                return;
            }
            processor.startDeLimiterLearn();
            owner.showHeaderHint ("Learning: play the song. It listens for 6 seconds from the first sound (click again to cancel)");
        };

        owner.addAndMakeVisible (card);
        card.addAndMakeVisible (amount);
        card.addAndMakeVisible (input);
        card.addAndMakeVisible (output);
        card.addAndMakeVisible (learn);
        owner.addAndMakeVisible (display);
        update();
    }

    ps::DeLimiter* module() const { return dynamic_cast<ps::DeLimiter*> (&processor.getChain().getModule (ps::ModuleType::DeLimiter)); }

    //==========================================================================
    void update()
    {
        const float pct = processor.getParameterValue ("dlim.amount");
        card.setSubtitle (pct < 0.05f ? "Off" : (pct > 100.05f ? "Enhanced" : juce::String()));

        // Learn state
        const bool learning = processor.isDeLimiterLearning();
        if (learning)
        {
            sawLearning = true;
            learn.setState (processor.isDeLimiterWaitingForAudio() ? dlim::LearnButton::State::waiting : dlim::LearnButton::State::listening,
                            processor.getDeLimiterLearnProgress());
        }
        else
        {
            if (sawLearning)
            {
                sawLearning = false;
                owner.showHeaderHint ("Learn set Threshold to " + fmt::db (processor.getParameterValue ("dlim.thr"), 1, false));
            }
            learn.setState (dlim::LearnButton::State::idle, 0.0f);
        }

        // Meters and history
        const bool running = processor.isModuleInChain (ps::ModuleType::DeLimiter) && processor.getParameterValue ("dlim.on") >= 0.5f
                             && processor.getChain().getSampleRate() > 0.0;
        auto* m = module();
        if (running && m != nullptr)
        {
            float inputDb = -120.0f, lift = 0.0f;
            const int got = m->getHistoryFifo().pull (pullL.data(), pullR.data(), (int) pullL.size());
            for (int i = 0; i < got; ++i)
            {
                inputDb = std::max (inputDb, pullL[(size_t) i]);
                lift = std::max (lift, pullR[(size_t) i]);
            }
            // One column per tick while audio flows; nothing new (a host that stopped processing) leaves the view as is.
            if (got > 0)
                display.push (inputDb, lift);
            display.setReadout (m->getMeters().heldGainDb.load(), true);
        }
        else
        {
            display.setReadout (0.0f, false);
        }
        display.update();
    }

    //==========================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto r = content.reduced (12, 6).withTrimmedBottom (6);
        const int leftW = juce::jlimit (190, 250, juce::roundToInt ((float) r.getWidth() * 0.22f));
        auto left = r.removeFromLeft (leftW);
        r.removeFromLeft (kGap);
        display.setBounds (r);

        card.setBounds (left);
        auto body = card.getBody();
        learn.setBounds (body.removeFromBottom (kLearnH));
        body.removeFromBottom (kGap);
        const int smallH = juce::jlimit (70, 110, body.getHeight() / 3);
        auto row = body.removeFromBottom (smallH);
        input.setBounds (row.removeFromLeft (row.getWidth() / 2));
        output.setBounds (row);
        body.removeFromBottom (kGap);
        const int kw = juce::jmin (body.getWidth(), 180);
        amount.setBounds (body.withSizeKeepingCentre (kw, juce::jmin (body.getHeight(), kw + 10)));
    }

    DeLimiterPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    stereo::Card card;
    ParamKnob amount, input, output;
    dlim::LearnButton learn;
    dlim::Display display;
    std::vector<float> pullL, pullR;
    bool sawLearning = false;
};

//==============================================================================
DeLimiterPanel::DeLimiterPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::DeLimiter), impl (std::make_unique<Impl> (*this, p))
{
    neverTakeKeyboardFocus (*this);
}

DeLimiterPanel::~DeLimiterPanel() = default;

int DeLimiterPanel::getNumControls() const noexcept
{
    return 4;   // amount, input, output, the threshold line
}

void DeLimiterPanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void DeLimiterPanel::refresh()
{
    ModulePanel::refresh();
    impl->update();
}

void DeLimiterPanel::paint (juce::Graphics& g) { ModulePanel::paint (g); }

void DeLimiterPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
