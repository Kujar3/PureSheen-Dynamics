#include "ui/PluginEditor.h"

#include "ui/AssistantPanel.h"
#include "ui/ChainStrip.h"
#include "ui/Controls.h"
#include "ui/DitherPanel.h"
#include "ui/EditorContext.h"
#include "ui/HeaderBar.h"
#include "ui/HelpOverlay.h"
#include "ui/IoPanel.h"
#include "ui/ReferencePanel.h"
#include "ui/Vectorscope.h"
#include "plugin/TrackReferencing.h"

#include <cmath>

// PureSheen Dynamics editor (owner: ui-shell).
//
// Layout (logical pixels): header 46, chain strip 70, then the selected module's panel (or the Dither panel) next to
// the 176 px I/O panel. Up to the default size the layout reflows; above it everything is scaled uniformly up to 1.3x
// (text and controls grow with the window), the remaining extra space goes to the module panel.
//
// UI state (processor.getUiState(), opaque to the processor): a "PureSheenUi" tree with editorW / editorH, the selected
// module, the I/O gain links, and an "Options" child (ps::ui::UiOptions).
//
// (phase 3) Track Referencing: the Reference page shows like the Assistant page; audio files dropped anywhere on the
// editor go to the reference list (drop overlay while dragging). The reference overlay on the module analysers
// (drawReferenceSpectrum) is computed once per tick: the reference's curve (its active region) brought to the master's
// loudness (minus Comparison::normalisationOffsetDb), with the analysers' tilt, then shifted by the smoothed mean
// difference (100 Hz - 10 kHz) between the output analyser and the master's own curve, so it lands on the analyser's
// dB scale; without a master profile the reference's mean is aligned to the output analyser's mean directly.
namespace
{
namespace Col = ps::ui::Colours;

constexpr int kScratch = 32768;
constexpr float kMaxZoom = 1.3f;
constexpr juce::uint32 kNoticeMs = 3500;
constexpr int kIoWidth = 176;
constexpr int kStripHeight = 78;             // the chain strip and the gap below it
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

float zoomFor (int w, int h, int defaultHeight)
{
    return juce::jlimit (1.0f, kMaxZoom, juce::jmin ((float) w / (float) PureSheenEditor::kDefaultWidth,
                                                     (float) h / (float) defaultHeight));
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
        void paintOverChildren (juce::Graphics& g) override { impl.paintDropOverlay (g); impl.paintNotice (g); }
        void resized() override                             { impl.layout(); }
        Impl& impl;
    };

    Impl (PureSheenEditor& o, PureSheenProcessor& p)
        : owner (o), processor (p), header (*this), io (*this), help (p), history (p)
    {
        const auto& product = p.getProduct();
        if (product.isSuite())
        {
            strip = std::make_unique<ps::ui::ChainStrip> (*this);
            assistantPanel = std::make_unique<ps::ui::AssistantPanel> (*this);
            referencePanel = std::make_unique<ps::ui::ReferencePanel> (*this);
        }
        if (product.isSuite() || product.isDither())
            dither = std::make_unique<ps::ui::DitherPanel> (*this);

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

        if ((selected < -1 && selected != ps::ui::kAssistantSelection && selected != ps::ui::kReferenceSelection)
            || selected >= ps::kNumModuleTypes)
            selected = (int) ps::ModuleType::Equalizer1;

        if (processor.getProduct().isLocked())
            selected = lockedSelection();

        return true;
    }

    // A single-module plugin always shows its one panel.
    int lockedSelection() const
    {
        const auto& product = processor.getProduct();
        return product.isDither() ? -1 : (int) product.module;
    }

    bool ditherShown() const { return dither != nullptr && dither->isVisible(); }
    bool assistantPageShown() const { return assistantPanel != nullptr && assistantPanel->isVisible(); }
    bool referencePageShown() const { return referencePanel != nullptr && referencePanel->isVisible(); }

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
        int h = savedSize.y > 0 ? savedSize.y : defaultHeightFor (processor.getProduct());
        const auto screen = primaryUserArea();

        if (! screen.isEmpty())
        {
            w = juce::jmin (w, screen.getWidth() - kScreenMarginW);
            h = juce::jmin (h, screen.getHeight() - kScreenMarginH);
        }

        return { juce::jlimit (kMinWidth, kMaxWidth, w), juce::jlimit (minHeightFor (processor.getProduct()), kMaxHeight, h) };
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

    // The module selection: a panel for a module in the chain, the Dither panel (-1), the Assistant or the Reference page.
    void showSelection()
    {
        const auto order = processor.getChainOrder();
        const bool assistantShown = selected == ps::ui::kAssistantSelection;
        const bool referenceShown = selected == ps::ui::kReferenceSelection;

        if (assistantShown != assistantPageShown() || referenceShown != referencePageShown())
        {
            assistantPanel->setVisible (assistantShown);
            referencePanel->setVisible (referenceShown);
            if (assistantShown || referenceShown)
            {
                panel.reset();
                dither->setVisible (false);
                processor.setTapModule (-1);
                if (assistantShown)
                {
                    assistantPanel->setBounds (moduleArea);
                    assistantPanel->tick();
                }
                else
                {
                    referencePanel->setBounds (moduleArea);
                    referencePanel->tick();
                }
                help.toFront (false);
                history.toFront (false);
            }
        }

        if (assistantShown || referenceShown)
        {
            io.setDitherSelected (false);
            strip->refresh();
            content.repaint (moduleArea.expanded (4));
            return;
        }

        if (selected >= 0 && std::find (order.begin(), order.end(), (ps::ModuleType) selected) == order.end())
            selected = order.empty() ? selected : (int) order.front();

        const bool inChain = selected >= 0 && std::find (order.begin(), order.end(), (ps::ModuleType) selected) != order.end();
        const int wanted = selected == -1 ? -1 : (inChain ? selected : -2);
        const int current = panel != nullptr ? (int) panel->getModuleType() : (ditherShown() ? -1 : -2);

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

            if (dither != nullptr)
                dither->setVisible (wanted == -1);
            processor.setTapModule (wanted >= 0 ? wanted : -1);
            help.toFront (false);
            history.toFront (false);
        }

        io.setDitherSelected (selected == -1);
        if (strip != nullptr)
            strip->refresh();
        content.repaint (moduleArea.expanded (4));
    }

    ps::ui::EditorServices* ownerServices() { return &owner; }

    //==========================================================================
    void layout()
    {
        auto r = content.getLocalBounds();
        header.setBounds (r.removeFromTop (46));
        r = r.withTrimmedTop (8).reduced (10, 0).withTrimmedBottom (10);
        if (strip != nullptr)
        {
            stripArea = r.removeFromTop (70);
            strip->setBounds (stripArea);
            r.removeFromTop (8);
        }

        const int ioW = kIoWidth + juce::jlimit (0, 60, (r.getWidth() - 1100) / 6);
        ioArea = r.removeFromRight (ioW);
        io.setBounds (ioArea);
        r.removeFromRight (10);
        moduleArea = r;

        if (panel != nullptr)
            panel->setBounds (moduleArea);

        if (dither != nullptr)
            dither->setBounds (moduleArea);
        if (assistantPanel != nullptr)
            assistantPanel->setBounds (moduleArea);
        if (referencePanel != nullptr)
            referencePanel->setBounds (moduleArea);
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
            if (area.isEmpty())
                continue;
            const auto r = area.toFloat().reduced (0.5f);
            g.setColour (juce::Colours::black.withAlpha (0.16f));
            g.fillRoundedRectangle (r.translated (0.0f, 2.0f).expanded (0.5f), ps::ui::kCornerRadius + 0.5f);
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.fillRoundedRectangle (r.translated (0.0f, 1.0f), ps::ui::kCornerRadius);
        }

        if (panel == nullptr && ! ditherShown() && ! assistantPageShown() && ! referencePageShown())
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
    // (phase 3) Drop overlay: a dark veil with the drop message while files are dragged over the editor, lavender for
    // audio files, coral when none of them is a supported audio file.
    void paintDropOverlay (juce::Graphics& g) const
    {
        if (! dragActive)
            return;

        const auto tint = dragSupported ? ps::ui::Colours::reference : ps::ui::Colours::major;
        const auto b = content.getLocalBounds().toFloat().reduced (8.0f);
        g.setColour (ps::ui::Colours::background.withAlpha (0.8f));
        g.fillRect (content.getLocalBounds());
        g.setColour (tint.withAlpha (0.08f));
        g.fillRoundedRectangle (b, 12.0f);
        juce::Path outline, dashed;
        outline.addRoundedRectangle (b.reduced (1.0f), 12.0f);
        const float dashes[] = { 8.0f, 6.0f };
        juce::PathStrokeType (2.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (tint.withAlpha (0.8f));
        g.fillPath (dashed);

        // The message on a card of its own, so it reads over any page
        const juce::String line1 = dragSupported ? "Drop to add as reference tracks" : "PureSheen cannot read these files";
        const juce::String line2 = dragSupported ? "They go to the Reference page (up to 10 songs) to compare your master with"
                                                 : "Use WAV, MP3, FLAC, AIFF or OGG files";
        const auto f1 = ps::ui::font (20.0f, 700), f2 = ps::ui::font (13.0f);
        const float w = juce::jmin (b.getWidth() - 40.0f, juce::jmax (ps::ui::textWidth (f1, line1), ps::ui::textWidth (f2, line2)) + 60.0f);
        const auto card = juce::Rectangle<float> (w, 132.0f).withCentre (b.getCentre());
        juce::Path shape;
        shape.addRoundedRectangle (card, 12.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 24, { 0, 6 }).drawForPath (g, shape);
        g.setColour (ps::ui::Colours::panel.brighter (0.06f));
        g.fillPath (shape);
        g.setColour (tint.withAlpha (0.7f));
        g.strokePath (shape, juce::PathStrokeType (1.2f));

        auto icon = juce::Rectangle<float> (38.0f, 38.0f).withCentre ({ card.getCentreX(), card.getY() + 36.0f });
        g.setColour (tint.withAlpha (0.16f));
        g.fillEllipse (icon);
        g.setColour (tint.brighter (0.2f));
        const auto ic = icon.getCentre();
        juce::Path arrow;
        if (dragSupported)
        {
            arrow.startNewSubPath (ic.x, ic.y - 9.0f);
            arrow.lineTo (ic.x, ic.y + 5.0f);
            arrow.startNewSubPath (ic.x - 6.0f, ic.y - 1.0f);
            arrow.lineTo (ic.x, ic.y + 5.0f);
            arrow.lineTo (ic.x + 6.0f, ic.y - 1.0f);
        }
        else
        {
            arrow.startNewSubPath (ic.x - 6.0f, ic.y - 6.0f);
            arrow.lineTo (ic.x + 6.0f, ic.y + 6.0f);
            arrow.startNewSubPath (ic.x + 6.0f, ic.y - 6.0f);
            arrow.lineTo (ic.x - 6.0f, ic.y + 6.0f);
        }
        g.strokePath (arrow, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setFont (f1);
        g.setColour (ps::ui::Colours::text);
        g.drawFittedText (line1, juce::Rectangle<float> (card.getX() + 16.0f, card.getY() + 62.0f, card.getWidth() - 32.0f, 28.0f).toNearestInt(),
                          juce::Justification::centred, 1, 0.8f);
        g.setFont (f2);
        g.setColour (dragSupported ? ps::ui::Colours::textDim : tint.brighter (0.2f));
        g.drawFittedText (line2, juce::Rectangle<float> (card.getX() + 16.0f, card.getY() + 92.0f, card.getWidth() - 32.0f, 22.0f).toNearestInt(),
                          juce::Justification::centred, 1, 0.8f);
    }

    //==========================================================================
    // (phase 3) The reference overlay of the module analysers (see the top of this file). Message thread, per tick.
    void updateReferenceOverlay()
    {
        auto& refs = processor.getTrackReferencing();
        if (! options.showReferenceSpectrum)
        {
            overlayValid = false;
            return;
        }

        const int version = refs.getAnalysisVersion();
        if (version != overlayVersion)
        {
            overlayVersion = version;
            overlayRef = refs.getReferenceProfile();
            overlayMix = refs.getMixProfile();
            const auto comparison = refs.getComparison();
            overlayOffset = comparison.valid() ? comparison.normalisationOffsetDb : 0.0f;
        }

        if (! overlayRef.valid || ! output.hasData())
        {
            overlayValid = false;
            return;
        }

        const float tiltPerOct = options.spectrumTiltDbPerOct;
        const bool mixValid = overlayMix.valid;
        double sumDiff = 0.0, sumLevel = 0.0;
        int n = 0;
        for (int i = 0; i < ps::kCurvePoints; ++i)
        {
            const float hz = ps::curveFrequency (i);
            const float tilt = tiltPerOct * std::log2 (hz / 1000.0f);
            const float r = overlayRef.curveDb[(size_t) i];
            overlayDb[(size_t) i] = ps::hasValue (r) ? r - (mixValid ? overlayOffset : 0.0f) + tilt : ps::kNoValue;

            if (hz < 100.0f || hz > 10000.0f)
                continue;
            const float level = output.levelDbAt (hz);
            const float base = mixValid ? overlayMix.curveDb[(size_t) i] : r;
            if (level <= -150.0f || ! ps::hasValue (base) || ! ps::hasValue (r))
                continue;
            sumDiff += (double) (level - (mixValid ? base + tilt : overlayDb[(size_t) i]));
            sumLevel += (double) level;
            ++n;
        }

        if (n < 8)
        {
            overlayValid = false;
            return;
        }

        // A silent output (host stopped) keeps the last alignment instead of sinking with the analyser.
        const float shift = (float) (sumDiff / n);
        if (! overlayHasShift)
            overlayShift = shift;
        else if (sumLevel / n > -110.0)
            overlayShift += (shift - overlayShift) * 0.12f;
        overlayHasShift = true;
        overlayValid = true;
    }

    void drawReferenceSpectrum (juce::Graphics& g, const ps::ui::FreqAxis& axis, float minDb, float maxDb) const
    {
        if (! overlayValid || ! options.showReferenceSpectrum || axis.area.isEmpty() || maxDb <= minDb)
            return;

        const auto area = axis.area;
        auto yFor = [&] (float db) {
            const float t = (juce::jlimit (minDb - 6.0f, maxDb + 2.0f, db) - minDb) / (maxDb - minDb);
            return juce::jlimit (area.getY(), area.getBottom() + 2.0f, area.getBottom() - t * area.getHeight());
        };

        juce::Path p;
        bool penDown = false;
        for (int i = 0; i < ps::kCurvePoints; ++i)
        {
            const float hz = ps::curveFrequency (i);
            const float db = overlayDb[(size_t) i];
            if (hz < axis.minHz || hz > axis.maxHz || ! ps::hasValue (db))
            {
                penDown = false;
                continue;
            }
            const juce::Point<float> pt (axis.hzToX (hz), yFor (db + overlayShift));
            if (penDown)
                p.lineTo (pt);
            else
                p.startNewSubPath (pt);
            penDown = true;
        }

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.strokePath (p, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (ps::ui::Colours::reference.withAlpha (0.9f));
        g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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

        if (ditherShown())
            dither->frameTick();
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
        if (strip != nullptr)
            strip->tick();
        header.update();

        if (panel != nullptr)
            panel->tick();

        if (ditherShown())
            dither->tick();

        if (assistantPageShown())
            assistantPanel->tick();

        if (referencePageShown())
            referencePanel->tick();

        if (processor.getProduct().isSuite())
            updateReferenceOverlay();

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
    ps::ui::IoPanel io;
    // The suite's pages. A single-module plugin has none of them (the Dither plugin has only the Dither panel).
    std::unique_ptr<ps::ui::ChainStrip> strip;
    std::unique_ptr<ps::ui::DitherPanel> dither;
    std::unique_ptr<ps::ui::AssistantPanel> assistantPanel;
    std::unique_ptr<ps::ui::ReferencePanel> referencePanel;
    int chainSelection = (int) ps::ModuleType::Equalizer1;   // where "Chain" returns to from the Assistant / Reference page
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

    bool dragActive = false, dragSupported = false;   // files dragged over the editor

    // The reference overlay of the module analysers
    ps::TrackProfile overlayRef, overlayMix;
    std::array<float, ps::kCurvePoints> overlayDb {};
    float overlayOffset = 0.0f, overlayShift = 0.0f;
    int overlayVersion = -1;
    bool overlayValid = false, overlayHasShift = false;

    juce::VBlankAttachment vblank { &owner, [this] { frame(); } };   // last: its callback uses everything above
};

//==============================================================================
int PureSheenEditor::defaultHeightFor (const psplug::Product& product) noexcept
{
    return kDefaultHeight - (product.isLocked() ? kStripHeight : 0);
}

int PureSheenEditor::minHeightFor (const psplug::Product& product) noexcept
{
    return kMinHeight - (product.isLocked() ? kStripHeight : 0);
}

PureSheenEditor::PureSheenEditor (PureSheenProcessor& p)
    : juce::AudioProcessorEditor (p), impl (std::make_unique<Impl> (*this, p))
{
    auto& d = *impl;
    setLookAndFeel (&d.lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&d.lookAndFeel);
    liveEditors().add (this);

    setOpaque (true);
    setTitle (p.getProduct().name);
    addAndMakeVisible (d.content);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &d.header, d.strip.get(), &d.io })
        if (c != nullptr)
            d.content.addAndMakeVisible (c);

    for (juce::Component* c : std::initializer_list<juce::Component*> { d.dither.get(), d.assistantPanel.get(), d.referencePanel.get() })
        if (c != nullptr)
            d.content.addChildComponent (c);

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
    setResizeLimits (kMinWidth, minHeightFor (p.getProduct()), kMaxWidth, kMaxHeight);

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
    const float zoom = zoomFor (getWidth(), getHeight(), defaultHeightFor (d.processor.getProduct()));
    d.content.setTransform (zoom > 1.0f ? juce::AffineTransform::scale (zoom) : juce::AffineTransform());
    d.content.setBounds (0, 0, (int) std::ceil ((float) getWidth() / zoom), (int) std::ceil ((float) getHeight() / zoom));
    d.saveUiState();
}

void PureSheenEditor::selectModule (int moduleTypeOrMinus1)
{
    auto& d = *impl;
    if (d.processor.getProduct().isLocked())
        moduleTypeOrMinus1 = d.lockedSelection();
    if (moduleTypeOrMinus1 == ps::ui::kChainSelection)
        moduleTypeOrMinus1 = d.chainSelection;
    if (moduleTypeOrMinus1 == ps::ui::kAssistantSelection || moduleTypeOrMinus1 == ps::ui::kReferenceSelection)
    {
        if (d.selected != ps::ui::kAssistantSelection && d.selected != ps::ui::kReferenceSelection)
            d.chainSelection = d.selected;
        d.selected = moduleTypeOrMinus1;
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

void PureSheenEditor::drawReferenceSpectrum (juce::Graphics& g, const ps::ui::FreqAxis& axis, float minDb, float maxDb)
{
    impl->drawReferenceSpectrum (g, axis, minDb, maxDb);
}

bool PureSheenEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return ! files.isEmpty() && impl->processor.getProduct().isSuite();   // the drop goes to the Reference page
}

void PureSheenEditor::fileDragEnter (const juce::StringArray& files, int, int)
{
    auto& d = *impl;
    d.dragActive = true;
    d.dragSupported = false;
    for (const auto& f : files)
        d.dragSupported = d.dragSupported || psplug::TrackReferencing::isSupportedFile (juce::File (f));
    d.content.repaint();
}

void PureSheenEditor::fileDragExit (const juce::StringArray&)
{
    impl->dragActive = false;
    impl->content.repaint();
}

void PureSheenEditor::filesDropped (const juce::StringArray& files, int, int)
{
    auto& d = *impl;
    d.dragActive = false;
    d.content.repaint();
    if (ps::ui::ReferencePanel::addFiles (d, files) > 0)
        selectModule (ps::ui::kReferenceSelection);
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
    if (d.strip != nullptr)
        d.strip->refresh();
    d.header.update();
    d.showSelection();

    if (d.panel != nullptr)
        d.panel->refresh();

    if (d.history.isVisible())
        d.history.refresh();
}

void PureSheenEditor::timerCallback() { impl->tick(); }
