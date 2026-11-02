#include "ui/PluginEditor.h"

#include "ui/AssistantPanel.h"
#include "ui/ChainStrip.h"
#include "ui/Controls.h"
#include "ui/DitherPanel.h"
#include "ui/EditorContext.h"
#include "ui/HeaderBar.h"
#include "ui/HelpOverlay.h"
#include "ui/IoPanel.h"
#include "ui/Vectorscope.h"

// PureSheen Dynamics editor (owner: ui-shell).
//
// Layout (logical pixels): header 46, chain strip 70, then the selected module's panel (or the Dither panel) next to
// the 176 px I/O panel. Up to the default size the layout reflows; above it everything is scaled uniformly up to 1.3x
// (text and controls grow with the window), the remaining extra space goes to the module panel.
//
// UI state (processor.getUiState(), opaque to the processor): a "PureSheenUi" tree with editorW / editorH, the selected
// module, the I/O gain links, and an "Options" child (ps::ui::UiOptions).
namespace
{
namespace Col = ps::ui::Colours;

constexpr int kScratch = 32768;
constexpr int kScreenMarginW = 40, kScreenMarginH = 90;
constexpr float kMaxZoom = 1.3f;
constexpr juce::uint32 kNoticeMs = 3500;
constexpr int kIoWidth = 176;
constexpr double kMinFrameMs = 6.5;          // display-refresh pulls: at most ~150 per second (a 240 Hz screen gets 120)
constexpr double kFrameFallbackMs = 100.0;   // no display refresh for this long (hidden, dev tool): the tick pulls

const juce::Identifier kUiTag ("PureSheenUi"), kEditorW ("editorW"), kEditorH ("editorH"), kSelected ("selected"),
    kLinkIn ("linkInput"), kLinkOut ("linkOutput");

juce::Array<PureSheenEditor*>& liveEditors()
{
    static juce::Array<PureSheenEditor*> editors;
    return editors;
}

juce::Rectangle<int> primaryUserArea()
{
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        return display->userBounds.getLargestIntegerWithin();

    return {};
}

float zoomFor (int w, int h)
{
    return juce::jlimit (1.0f, kMaxZoom, juce::jmin ((float) w / (float) PureSheenEditor::kDefaultWidth,
                                                     (float) h / (float) PureSheenEditor::kDefaultHeight));
}

bool isMultiband (PureSheenProcessor& p, int type)
{
    return type >= 0 && type < ps::kNumModuleTypes
           && p.getParameter (juce::String (ps::moduleInfo ((ps::ModuleType) type).idPrefix) + ".bands") != nullptr;
}
} // namespace

//==============================================================================
struct PureSheenEditor::Impl final : public ps::ui::EditorContext
{
    struct Content final : public juce::Component
    {
        explicit Content (Impl& o) : impl (o) { setTitle ("PureSheen content"); }
        void paint (juce::Graphics& g) override             { impl.paintContent (g); }
        void paintOverChildren (juce::Graphics& g) override { impl.paintNotice (g); }
        void resized() override                             { impl.layout(); }
        Impl& impl;
    };

    Impl (PureSheenEditor& o, PureSheenProcessor& p)
        : owner (o), processor (p), header (*this), strip (*this), io (*this), dither (*this), assistantPanel (*this), history (p)
    {
        help.onClose = [this] { setHelpVisible (false); };
        history.onClose = [this] { setHistoryVisible (false); };
    }

    //==========================================================================
    // EditorContext
    PureSheenProcessor& getProcessor() override { return processor; }
    ps::ui::EditorServices& getServices() override { return owner; }
    const ps::ui::UiOptions& getUiOptions() const override { return options; }
    void applyOptions (const ps::ui::UiOptions& o) override { owner.setOptions (o); }
    void select (int t) override { owner.selectModule (t); }
    int getSelection() const override { return selected; }
    void toggleHelp() override { setHelpVisible (! help.isVisible()); }
    bool isHelpOpen() const override { return help.isVisible(); }
    void toggleHistory() override { setHistoryVisible (! history.isVisible()); }
    bool isHistoryOpen() const override { return history.isVisible(); }

    void notify (const juce::String& text) override
    {
        const auto old = noticeArea();
        notice = text;
        noticeShownMs = juce::Time::getMillisecondCounter();
        content.repaint (old.getUnion (noticeArea()));
    }

    bool isGainLinked (bool isOutput) const override { return isOutput ? linkOut : linkIn; }

    void setGainLinked (bool isOutput, bool linked) override
    {
        (isOutput ? linkOut : linkIn) = linked;
        saveUiState();
    }

    //==========================================================================
    bool loadUiState()
    {
        const auto tree = processor.getUiState();

        if (tree.hasType (kUiTag))
        {
            options = ps::ui::UiOptions::fromValueTree (tree.getChildWithName ("Options"));
            savedSize = { (int) tree.getProperty (kEditorW, 0), (int) tree.getProperty (kEditorH, 0) };
            selected = (int) tree.getProperty (kSelected, (int) ps::ModuleType::Equalizer1);
            linkIn = (bool) tree.getProperty (kLinkIn, true);
            linkOut = (bool) tree.getProperty (kLinkOut, true);
        }
        else if (tree.hasType ("Options"))
        {
            options = ps::ui::UiOptions::fromValueTree (tree);
        }

        if (savedSize.x <= 0 || savedSize.y <= 0)
            savedSize = processor.getEditorSize();   // state saved by a host that kept only the processor's size

        if ((selected < -1 && selected != ps::ui::kAssistantSelection) || selected >= ps::kNumModuleTypes)
            selected = (int) ps::ModuleType::Equalizer1;

        return true;
    }

    void saveUiState()
    {
        if (! persist)
            return;

        juce::ValueTree tree (kUiTag);
        tree.setProperty (kEditorW, owner.getWidth(), nullptr);
        tree.setProperty (kEditorH, owner.getHeight(), nullptr);
        tree.setProperty (kSelected, selected, nullptr);
        tree.setProperty (kLinkIn, linkIn, nullptr);
        tree.setProperty (kLinkOut, linkOut, nullptr);
        tree.appendChild (options.toValueTree(), nullptr);
        processor.setUiState (tree);
        processor.setEditorSize (owner.getWidth(), owner.getHeight());
    }

    juce::Point<int> initialSize() const
    {
        int w = savedSize.x > 0 ? savedSize.x : kDefaultWidth;
        int h = savedSize.y > 0 ? savedSize.y : kDefaultHeight;
        const auto screen = primaryUserArea();

        if (! screen.isEmpty())
        {
            w = juce::jmin (w, screen.getWidth() - kScreenMarginW);
            h = juce::jmin (h, screen.getHeight() - kScreenMarginH);
        }

        return { juce::jlimit (kMinWidth, kMaxWidth, w), juce::jlimit (kMinHeight, kMaxHeight, h) };
    }

    //==========================================================================
    void setHelpVisible (bool show)
    {
        if (show)
        {
            setHistoryVisible (false);
            help.setBounds (content.getLocalBounds());
            help.toFront (false);
        }

        help.setVisible (show);
        header.update();
    }

    void setHistoryVisible (bool show)
    {
        if (show)
        {
            history.setBounds (content.getLocalBounds());
            const auto anchor = content.getLocalArea (&header, header.getHistoryButtonBounds());
            history.open (anchor);
        }
        else
        {
            history.setVisible (false);
        }

        header.update();
    }

    // The module selection: a panel for a module in the chain, the Dither panel (-1) or the Assistant page.
    void showSelection()
    {
        const auto order = processor.getChainOrder();
        const bool assistantShown = selected == ps::ui::kAssistantSelection;

        if (assistantShown != assistantPanel.isVisible())
        {
            assistantPanel.setVisible (assistantShown);
            if (assistantShown)
            {
                panel.reset();
                dither.setVisible (false);
                processor.setTapModule (-1);
                assistantPanel.setBounds (moduleArea);
                assistantPanel.tick();
                help.toFront (false);
                history.toFront (false);
            }
        }

        if (assistantShown)
        {
            io.setDitherSelected (false);
            strip.refresh();
            content.repaint (moduleArea.expanded (4));
            return;
        }

        if (selected >= 0 && std::find (order.begin(), order.end(), (ps::ModuleType) selected) == order.end())
            selected = order.empty() ? selected : (int) order.front();

        const bool inChain = selected >= 0 && std::find (order.begin(), order.end(), (ps::ModuleType) selected) != order.end();
        const int wanted = selected == -1 ? -1 : (inChain ? selected : -2);
        const int current = panel != nullptr ? (int) panel->getModuleType() : (dither.isVisible() ? -1 : -2);

        if (wanted != current || (wanted >= 0 && panel == nullptr))
        {
            panel.reset();
            modulePre.clear();
            modulePost.clear();

            if (wanted >= 0)
            {
                panel = ps::ui::createModulePanel (processor, *this->ownerServices(), (ps::ModuleType) wanted);

                if (panel != nullptr)
                {
                    content.addAndMakeVisible (*panel);
                    panel->setBounds (moduleArea);
                    panel->refresh();
                    ps::ui::neverTakeKeyboardFocus (*panel);
                }
            }

            dither.setVisible (wanted == -1);
            processor.setTapModule (wanted >= 0 ? wanted : -1);
            help.toFront (false);
            history.toFront (false);
        }

        io.setDitherSelected (selected == -1);
        strip.refresh();
        content.repaint (moduleArea.expanded (4));
    }

    ps::ui::EditorServices* ownerServices() { return &owner; }

    //==========================================================================
    void layout()
    {
        auto r = content.getLocalBounds();
        header.setBounds (r.removeFromTop (46));
        r = r.withTrimmedTop (8).reduced (10, 0).withTrimmedBottom (10);
        stripArea = r.removeFromTop (70);
        strip.setBounds (stripArea);
        r.removeFromTop (8);

        const int ioW = kIoWidth + juce::jlimit (0, 60, (r.getWidth() - 1100) / 6);
        ioArea = r.removeFromRight (ioW);
        io.setBounds (ioArea);
        r.removeFromRight (10);
        moduleArea = r;

        if (panel != nullptr)
            panel->setBounds (moduleArea);

        dither.setBounds (moduleArea);
        assistantPanel.setBounds (moduleArea);
        help.setBounds (content.getLocalBounds());

        if (history.isVisible())
            setHistoryVisible (true);
        else
            history.setBounds (content.getLocalBounds());
    }

    void paintContent (juce::Graphics& g) const
    {
        for (const auto& area : { stripArea, ioArea, moduleArea })
        {
            const auto r = area.toFloat().reduced (0.5f);
            g.setColour (juce::Colours::black.withAlpha (0.16f));
            g.fillRoundedRectangle (r.translated (0.0f, 2.0f).expanded (0.5f), ps::ui::kCornerRadius + 0.5f);
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.fillRoundedRectangle (r.translated (0.0f, 1.0f), ps::ui::kCornerRadius);
        }

        if (panel == nullptr && ! dither.isVisible() && ! assistantPanel.isVisible())
        {
            ps::ui::drawPanel (g, moduleArea.toFloat());
            auto b = moduleArea.toFloat();
            ps::ui::drawLogo (g, juce::Rectangle<float> (54.0f, 54.0f).withCentre (b.getCentre().translated (0.0f, -40.0f)));
            g.setFont (ps::ui::font (16.0f, 700));
            g.setColour (Col::text);
            g.drawText ("The chain is empty", b.withTrimmedTop (b.getHeight() * 0.5f).withHeight (26.0f), juce::Justification::centred, false);
            g.setFont (ps::ui::font (12.5f));
            g.setColour (Col::textDim);
            g.drawText ("Click + in the strip above to add a module", b.withTrimmedTop (b.getHeight() * 0.5f + 28.0f).withHeight (20.0f),
                        juce::Justification::centred, false);
        }
    }

    // A message wider than the module area (e.g. an import result listing skipped modules) wraps onto two lines.
    bool noticeWraps() const
    {
        return ps::ui::textWidth (ps::ui::font (12.5f, 600), notice) + 48.0f > (float) moduleArea.getWidth() - 40.0f;
    }

    juce::Rectangle<float> noticeBounds() const
    {
        const auto f = ps::ui::font (12.5f, 600);
        const float maxW = (float) moduleArea.getWidth() - 40.0f;
        const float textW = ps::ui::textWidth (f, notice) + 48.0f;
        const bool wraps = noticeWraps();
        const float w = wraps ? juce::jmin (maxW, juce::jmax (maxW * 0.7f, textW * 0.55f + 48.0f)) : textW;
        const float h = wraps ? 52.0f : 34.0f;
        return { (float) moduleArea.getCentreX() - w * 0.5f, (float) moduleArea.getBottom() - 16.0f - h, w, h };
    }

    juce::Rectangle<int> noticeArea() const
    {
        return notice.isEmpty() ? juce::Rectangle<int>() : noticeBounds().getSmallestIntegerContainer().expanded (24);
    }

    void paintNotice (juce::Graphics& g) const
    {
        if (notice.isEmpty())
            return;

        const auto pill = noticeBounds();
        const bool wraps = noticeWraps();
        const float radius = juce::jmin (17.0f, pill.getHeight() * 0.5f);
        juce::Path shape;
        shape.addRoundedRectangle (pill, radius);
        juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 16, { 0, 4 }).drawForPath (g, shape);
        g.setColour (Col::panel.brighter (0.08f));
        g.fillPath (shape);
        g.setColour (Col::accent.withAlpha (0.7f));
        g.drawRoundedRectangle (pill.reduced (0.5f), radius, 1.0f);
        g.setColour (Col::accent);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ pill.getX() + 20.0f, pill.getCentreY() }));
        g.setFont (ps::ui::font (12.5f, 600));
        g.setColour (Col::text);
        g.drawFittedText (notice, pill.withTrimmedLeft (32.0f).withTrimmedRight (18.0f).reduced (0.0f, wraps ? 6.0f : 0.0f).toNearestInt(),
                          juce::Justification::centredLeft, wraps ? 2 : 1, 0.85f);
    }

    //==========================================================================
    // Pulls the chain's FIFOs (the editor is their only consumer) and feeds the meters, the scope and the three
    // spectrum analysers. Returns true if a spectrum got a new frame. The meters and the scope only accumulate here;
    // their ballistics run in tick().
    bool pullAudio()
    {
        auto& chain = processor.getChain();
        const double sr = chain.getSampleRate();
        io.setSampleRate (sr);
        modulePre.setSampleRate (sr);
        modulePost.setSampleRate (sr);
        output.setSampleRate (sr);

        int n = chain.getInputFifo().pull (scratchL.data(), scratchR.data(), kScratch);
        io.processInput (scratchL.data(), scratchR.data(), n);

        n = chain.getOutputFifo().pull (scratchL.data(), scratchR.data(), kScratch);
        io.processOutput (scratchL.data(), scratchR.data(), n);
        bool changed = output.process (scratchL.data(), scratchR.data(), n);
        ps::ui::Vectorscope::publishOutput (processor, scratchL.data(), scratchR.data(), n);   // single FIFO consumer

        changed |= modulePre.update (chain.getModulePreFifo());
        changed |= modulePost.update (chain.getModulePostFifo());
        return changed;
    }

    // Every display refresh (VBlankAttachment): the spectra move at the screen's rate instead of the 30 Hz tick, each
    // frame computed from the newest audio.
    void frame()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();

        if (now - lastFrameMs < kMinFrameMs)
            return;

        lastFrameMs = now;

        if (! pullAudio())
            return;

        if (panel != nullptr)
            panel->frameTick();

        if (dither.isVisible())
            dither.frameTick();
    }

    void tick()
    {
        if (juce::Time::getMillisecondCounterHiRes() - lastFrameMs > kFrameFallbackMs)
            pullAudio();

        if (++learnCountdown >= 15)
        {
            learnCountdown = 0;

            if (panel != nullptr && isMultiband (processor, (int) panel->getModuleType()) && modulePre.hasData())
                processor.feedLearnSpectrum (panel->getModuleType(), modulePre.getBinMagnitudesDb(), modulePre.getBinHz());
        }

        io.tick();
        strip.tick();
        header.update();

        if (panel != nullptr)
            panel->tick();

        if (dither.isVisible())
            dither.tick();

        if (assistantPanel.isVisible())
            assistantPanel.tick();

        if (history.isVisible() && ++historyCountdown >= 10)
        {
            historyCountdown = 0;
            history.refresh();
        }

        if (notice.isNotEmpty() && juce::Time::getMillisecondCounter() - noticeShownMs > kNoticeMs)
        {
            const auto area = noticeArea();
            notice.clear();
            content.repaint (area);
        }

        if (--focusSweep <= 0)
        {
            focusSweep = 30;
            ps::ui::neverTakeKeyboardFocus (owner);
        }
    }

    // Declared first: destroyed last.
    ps::ui::LookAndFeel lookAndFeel;

    PureSheenEditor& owner;
    PureSheenProcessor& processor;

    ps::ui::UiOptions options;
    ps::ui::SpectrumAnalyser modulePre, modulePost, output;
    int selected = (int) ps::ModuleType::Equalizer1;
    bool linkIn = true, linkOut = true, persist = false;
    juce::Point<int> savedSize;
    bool uiStateLoaded = loadUiState();   // before the components below read the options / links

    Content content { *this };
    ps::ui::HeaderBar header;
    ps::ui::ChainStrip strip;
    ps::ui::IoPanel io;
    ps::ui::DitherPanel dither;
    ps::ui::AssistantPanel assistantPanel;
    int chainSelection = (int) ps::ModuleType::Equalizer1;   // where "Chain" returns to from the Assistant
    std::unique_ptr<ps::ui::ModulePanel> panel;
    ps::ui::HelpOverlay help;
    ps::ui::HistoryPopup history;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    juce::Rectangle<int> stripArea, ioArea, moduleArea;
    std::vector<float> scratchL = std::vector<float> ((size_t) kScratch), scratchR = std::vector<float> ((size_t) kScratch);

    juce::String notice;
    juce::uint32 noticeShownMs = 0;
    int focusSweep = 30, learnCountdown = 0, historyCountdown = 0;
    double lastFrameMs = -1.0e9;

    juce::VBlankAttachment vblank { &owner, [this] { frame(); } };   // last: its callback uses everything above
};

//==============================================================================
PureSheenEditor::PureSheenEditor (PureSheenProcessor& p)
    : juce::AudioProcessorEditor (p), impl (std::make_unique<Impl> (*this, p))
{
    auto& d = *impl;
    setLookAndFeel (&d.lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&d.lookAndFeel);
    liveEditors().add (this);

    setOpaque (true);
    setTitle ("PureSheen Dynamics");
    addAndMakeVisible (d.content);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &d.header, &d.strip, &d.io })
        d.content.addAndMakeVisible (c);

    d.content.addChildComponent (d.dither);
    d.content.addChildComponent (d.assistantPanel);
    d.content.addChildComponent (d.help);
    d.content.addChildComponent (d.history);

    for (auto* a : { &d.modulePre, &d.modulePost, &d.output })
        a->setOptions (d.options);

    if (d.options.showTooltips)
    {
        d.tooltipWindow = std::make_unique<juce::TooltipWindow> (&d.content, 700);
        d.tooltipWindow->setOpaque (false);
    }

    p.setHistoryDepth (d.options.historyDepth);
    p.addChangeListener (this);

    // Size first, constrainer second (setResizeLimits clamps the current bounds).
    const auto size = d.initialSize();
    setSize (size.x, size.y);
    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);

    d.showSelection();
    d.persist = true;

    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    ps::ui::neverTakeKeyboardFocus (*this);

    updateNow();
    startTimerHz (30);
}

PureSheenEditor::~PureSheenEditor()
{
    stopTimer();
    impl->processor.removeChangeListener (this);
    juce::PopupMenu::dismissAllActiveMenus();
    impl->tooltipWindow.reset();
    impl->panel.reset();

    liveEditors().removeAllInstancesOf (this);

    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &impl->lookAndFeel)
        juce::LookAndFeel::setDefaultLookAndFeel (liveEditors().isEmpty() ? nullptr : &liveEditors().getLast()->getLookAndFeel());

    setLookAndFeel (nullptr);
}

void PureSheenEditor::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (Col::background.brighter (0.03f), b.getY(), Col::background.darker (0.08f), b.getBottom()));
    g.fillRect (b);
}

void PureSheenEditor::resized()
{
    auto& d = *impl;
    const float zoom = zoomFor (getWidth(), getHeight());
    d.content.setTransform (zoom > 1.0f ? juce::AffineTransform::scale (zoom) : juce::AffineTransform());
    d.content.setBounds (0, 0, (int) std::ceil ((float) getWidth() / zoom), (int) std::ceil ((float) getHeight() / zoom));
    d.saveUiState();
}

void PureSheenEditor::selectModule (int moduleTypeOrMinus1)
{
    auto& d = *impl;
    if (moduleTypeOrMinus1 == ps::ui::kChainSelection)
        moduleTypeOrMinus1 = d.chainSelection;
    if (moduleTypeOrMinus1 == ps::ui::kAssistantSelection)
    {
        if (d.selected != ps::ui::kAssistantSelection)
            d.chainSelection = d.selected;
        d.selected = ps::ui::kAssistantSelection;
    }
    else
    {
        d.selected = juce::jlimit (-1, ps::kNumModuleTypes - 1, moduleTypeOrMinus1);
    }
    d.showSelection();
    d.saveUiState();
}

int PureSheenEditor::getSelectedModule() const noexcept { return impl->selected; }

const ps::ui::UiOptions& PureSheenEditor::getOptions() const { return impl->options; }
ps::ui::SpectrumAnalyser& PureSheenEditor::getModulePreSpectrum() { return impl->modulePre; }
ps::ui::SpectrumAnalyser& PureSheenEditor::getModulePostSpectrum() { return impl->modulePost; }
ps::ui::SpectrumAnalyser& PureSheenEditor::getOutputSpectrum() { return impl->output; }

void PureSheenEditor::showNotice (const juce::String& text)
{
    impl->notify (text);
}

void PureSheenEditor::showHint (const juce::String& text)
{
    if (impl->panel != nullptr)
        impl->panel->showHeaderHint (text);
    else
        impl->notify (text);
}

void PureSheenEditor::setOptions (const ps::ui::UiOptions& o)
{
    auto& d = *impl;
    d.options = o;

    for (auto* a : { &d.modulePre, &d.modulePost, &d.output })
        a->setOptions (o);

    if (o.showTooltips && d.tooltipWindow == nullptr)
    {
        d.tooltipWindow = std::make_unique<juce::TooltipWindow> (&d.content, 700);
        d.tooltipWindow->setOpaque (false);
    }
    else if (! o.showTooltips)
    {
        d.tooltipWindow.reset();
    }

    d.processor.setHistoryDepth (o.historyDepth);
    d.io.optionsChanged();
    d.saveUiState();

    if (d.panel != nullptr)
        d.panel->refresh();

    repaint();
}

void PureSheenEditor::setHelpVisible (bool show) { impl->setHelpVisible (show); }

void PureSheenEditor::setHistoryVisible (bool show) { impl->setHistoryVisible (show); }

ps::ui::ModulePanel* PureSheenEditor::getCurrentPanel() noexcept { return impl->panel.get(); }

void PureSheenEditor::updateNow()
{
    // Timers do not run in the dev tool: do the processor's housekeeping (gesture commits, latency, learn result) too.
    impl->processor.flushPendingChanges();
    impl->tick();
}

void PureSheenEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    auto& d = *impl;
    d.strip.refresh();
    d.header.update();
    d.showSelection();

    if (d.panel != nullptr)
        d.panel->refresh();

    if (d.history.isVisible())
        d.history.refresh();
}

void PureSheenEditor::timerCallback() { impl->tick(); }
