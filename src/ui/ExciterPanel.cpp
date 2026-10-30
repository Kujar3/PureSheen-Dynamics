#include "ui/ExciterPanel.h"

#include "dsp/Exciter.h"
#include "ui/ExciterParts.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/MultibandDisplay.h"

#include <array>

// Exciter panel. Owner: ui-stereo. Layout and interactions: see ExciterPanel.h.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr int kCardsH = 186;
// Band areas start under the display's header strip. The display keeps the bottom ~48 px of the plot for its
// frequency labels, legend and band count buttons, so the overlay stays clear of them.
constexpr float kChipH = 20.0f;          // model chip at the top of a band
constexpr float kMeterStripH = 22.0f;    // harmonics bar under the chip
constexpr float kBottomKeepOut = 54.0f;
constexpr float kHarmFloorDb = -60.0f;

juce::String shortHz (float hz)
{
    if (hz >= 1000.0f)
        return juce::String (hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + "k";

    return juce::String (juce::roundToInt (hz));
}
} // namespace

//==============================================================================
struct ExciterPanel::Impl
{
    Impl (ExciterPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::Exciter)),
          display (p, s, ps::ModuleType::Exciter),
          bandCard ("Band 1", colour),
          bandOn (p, "exc.a.b1.on", "On"),
          modeGrid (p),
          drive (p, "exc.a.b1.drive", "Drive"),
          mix (p, "exc.a.b1.mix", "Mix"),
          curveCard ("Curve", colour),
          settingsCard ("Settings", colour),
          oversampling (p, "exc.os", "Oversampling"),
          postOn (p, "exc.post.on", "Post filter"),
          postFreq (p, "exc.post.freq", "Post Freq"),
          postGain (p, "exc.post.gain", "Post Gain"),
          xoverSegments (p, "exc.xoverType", { "Analog", "Digital", "Hybrid" }),
          xoverChoice (p, "exc.xoverType", "Crossover"),
          drag (p)
    {
        for (auto& row : harmonicsShown)
            row.fill (kHarmFloorDb);

        for (auto* k : { &postFreq, &postGain })
            k->setColour (colour);

        for (auto* t : { &oversampling, &postOn })
            t->setOnColour (colour);

        xoverSegments.setOnColour (colour);

        bandOn.setTooltip ("Band on: turns the saturation of this band on or off.");
        modeGrid.setTooltip ("Saturation model of the selected band.");
        drive.setTooltip ("Drive: how hard the band is pushed into the saturation (more harmonics).");
        mix.setTooltip ("Mix: blend of the saturated band with the clean band.");
        oversampling.setTooltip ("Oversampling (4x): removes aliasing from the saturation. Adds a little latency.");
        postOn.setTooltip ("Post filter: a high shelf on the added harmonics only, to tame or brighten them.");
        postFreq.setTooltip ("Post filter corner frequency.");
        postGain.setTooltip ("Post filter gain (high shelf on the harmonics).");
        xoverSegments.setTooltip ("Crossover type. Analog: no latency, phase-aligned bands. Digital: linear phase "
                                  "(adds latency). Hybrid: steeper phase-aligned slopes.");
        xoverChoice.setTooltip (xoverSegments.getTooltip());

        owner.addAndMakeVisible (display);

        owner.addAndMakeVisible (bandCard);
        bandCard.addAndMakeVisible (bandOn);
        bandCard.addAndMakeVisible (modeGrid);
        bandCard.addAndMakeVisible (drive);
        bandCard.addAndMakeVisible (mix);
        bandCard.setTitleReserve (56);

        owner.addAndMakeVisible (curveCard);
        curveCard.addAndMakeVisible (curve);

        owner.addAndMakeVisible (settingsCard);
        settingsCard.addAndMakeVisible (oversampling);
        settingsCard.addAndMakeVisible (postOn);
        settingsCard.addAndMakeVisible (postFreq);
        settingsCard.addAndMakeVisible (postGain);
        settingsCard.addChildComponent (xoverSegments);
        settingsCard.addChildComponent (xoverChoice);

        display.paintBandOverlay = [this] (juce::Graphics& g, int band, juce::Rectangle<float> area)
        {
            // While another band is soloed this one is dimmed by the display: dim its overlay too.
            const int solo = display.getSoloBand();

            if (solo >= 0 && solo != band)
            {
                g.beginTransparencyLayer (0.4f);
                paintBand (g, band, area);
                g.endTransparencyLayer();
            }
            else
            {
                paintBand (g, band, area);
            }
        };
        display.onBandMouseDown = [this] (int band, const juce::MouseEvent& e) { return bandMouseDown (band, e); };
        display.onBandMouseDrag = [this] (int band, const juce::MouseEvent& e) { bandMouseDrag (band, e); };
        display.onBandMouseUp = [this] (int, const juce::MouseEvent&) { endDrag(); };
        display.onBandSelected = [this] (int) { rebind(); };
        display.onBandDoubleClick = [this] (int band, const juce::MouseEvent&) { return resetBand (band); };
        display.onBandPopupMenu = [this] (int band, const juce::MouseEvent&) { return bandMenu (band); };
        display.onBandMouseMove = [this] (int band, const juce::MouseEvent&) { setHoverBand (band); };
        display.getBandCursor = [] (int, juce::Point<float>) { return juce::MouseCursor (juce::MouseCursor::UpDownResizeCursor); };

        rebind();
    }

    ~Impl()
    {
        drag.end();
    }

    //==========================================================================
    int numBands() const
    {
        return juce::jlimit (1, ps::kMaxBands, juce::roundToInt (processor.getParameterValue ("exc.bands")));
    }

    bool isSplit() const
    {
        return isDualChannelMode (currentChannelMode (processor, ps::ModuleType::Exciter));
    }

    int currentSet() const { return isSplit() ? owner.getChannelSet() : 0; }

    bool isLinked() const { return isSplit() && processor.getParameterValue ("exc.link") >= 0.5f; }

    juce::String bandId (int set, int band, const char* name) const
    {
        return juce::String ("exc.") + (set == 0 ? "a" : "b") + ".b" + juce::String (band + 1) + "." + name;
    }

    juce::String twinId (int band, const char* name) const
    {
        return isLinked() ? bandId (1 - currentSet(), band, name) : juce::String();
    }

    juce::Colour setColour() const { return currentSet() == 1 ? Colours::sideChannel : colour; }

    int selectedBand() const { return juce::jlimit (0, numBands() - 1, display.getSelectedBand()); }

    juce::String rangeText (int band) const
    {
        const int n = numBands();
        const float lo = band == 0 ? 20.0f : processor.getParameterValue ("exc.xover" + juce::String (band));
        const float hi = band == n - 1 ? 20000.0f : processor.getParameterValue ("exc.xover" + juce::String (band + 1));
        return shortHz (lo) + " - " + shortHz (hi) + " Hz";
    }

    // Rebinds the selected band's controls to the current band / channel set / Link state.
    void rebind()
    {
        const int set = currentSet(), band = selectedBand();
        const auto c = setColour();

        bandOn.setParamId (bandId (set, band, "on"));
        bandOn.setLinkedParamId (twinId (band, "on"));
        bandOn.setOnColour (c);
        drive.setParamId (bandId (set, band, "drive"));
        drive.setLinkedParamId (twinId (band, "drive"));
        drive.setColour (c);
        mix.setParamId (bandId (set, band, "mix"));
        mix.setLinkedParamId (twinId (band, "mix"));
        mix.setColour (c);
        modeGrid.setParamIds (bandId (set, band, "mode"), twinId (band, "mode"));
        modeGrid.setColour (c);

        bandCard.setTitle ("Band " + juce::String (band + 1));
        bandCard.setBadge (band + 1);
        bandCard.setColour (c);
        display.setAccentColour (c);
        display.setChannelSet (set);

        shownBand = band;
        shownSet = set;
        shownLinked = isLinked();
        shownSplit = isSplit();
        updateTexts (true);
        display.repaint();
    }

    void updateTexts (bool force)
    {
        const int set = currentSet(), band = selectedBand();
        auto sub = rangeText (band);

        if (isSplit())
            sub = channelSetName (currentChannelMode (processor, ps::ModuleType::Exciter), set) + "  " + sub;

        bandCard.setSubtitle (sub);

        const int mode = juce::roundToInt (processor.getParameterValue (bandId (set, band, "mode")));
        const float d = processor.getParameterValue (bandId (set, band, "drive"));
        const bool on = processor.getParameterValue (bandId (set, band, "on")) >= 0.5f;
        curve.setCurve (mode, d, setColour(), on);

        if (force || mode != shownMode)
        {
            shownMode = mode;
            curveCard.setSubtitle (exciterModeName (mode) + ", drive " + juce::String (juce::roundToInt (d)) + " %");
            curveCard.setBodyNote (exciterModeDescription (mode));
        }
        else
        {
            curveCard.setSubtitle (exciterModeName (mode) + ", drive " + juce::String (juce::roundToInt (d)) + " %");
        }

        modeGrid.update();
    }

    //==========================================================================
    juce::Rectangle<float> barArea (juce::Rectangle<float> band) const
    {
        return band.withTrimmedTop (6.0f + kChipH + kMeterStripH + 10.0f).withTrimmedBottom (kBottomKeepOut).reduced (10.0f, 0.0f);
    }

    void paintBand (juce::Graphics& g, int band, juce::Rectangle<float> area)
    {
        if (band < 0 || band >= ps::kMaxBands || area.getWidth() < 8.0f)
            return;

        const int set = currentSet();
        const auto c = setColour();
        const bool on = processor.getParameterValue (bandId (set, band, "on")) >= 0.5f;
        const int mode = juce::roundToInt (processor.getParameterValue (bandId (set, band, "mode")));
        const float d = processor.getParameterValue (bandId (set, band, "drive"));
        const bool selected = selectedBand() == band;
        const bool dragging = drag.isActive() && dragBand == band;
        const bool hover = hoverBand == band;
        const bool lit = selected || dragging || hover;

        // Model chip.
        {
            const auto f = font (10.5f, 700).withExtraKerningFactor (0.06f);
            const auto text = on ? exciterModeName (mode).toUpperCase() : juce::String ("OFF");
            const float w = juce::jmin (area.getWidth() - 16.0f, textWidth (f, text) + 20.0f);
            const auto chip = juce::Rectangle<float> (w, kChipH).withCentre ({ area.getCentreX(), area.getY() + 6.0f + kChipH * 0.5f });

            if (chip.getWidth() > 20.0f)
            {
                g.setColour ((on ? c : Colours::textFaint).withAlpha (on ? 0.18f : 0.12f));
                g.fillRoundedRectangle (chip, kChipH * 0.5f);
                g.setColour ((on ? c : Colours::textFaint).withAlpha (0.7f));
                g.drawRoundedRectangle (chip.reduced (0.5f), kChipH * 0.5f, 1.0f);
                g.setFont (f);
                g.setColour (on ? c.brighter (0.5f) : Colours::textSubtle);
                g.drawFittedText (text, chip.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            }
        }

        const auto bars = barArea (area);

        if (bars.getHeight() >= 20.0f)
        {
            const float top = bars.getBottom() - juce::jlimit (0.0f, 100.0f, d) / 100.0f * bars.getHeight();
            const auto tint = on ? c : Colours::textFaint;

            // Track (faint) and the drive column.
            g.setColour (juce::Colours::white.withAlpha (0.025f));
            g.fillRoundedRectangle (bars, 4.0f);

            if (d > 0.05f)
            {
                const auto fill = bars.withTop (top);
                g.setGradientFill (juce::ColourGradient::vertical (tint.withAlpha (lit ? 0.42f : 0.30f), fill.getY(),
                                                                   tint.withAlpha (0.05f), fill.getBottom()));
                g.fillRoundedRectangle (fill, 4.0f);
            }

            g.setColour (tint.withAlpha (lit ? 1.0f : 0.8f));
            g.fillRoundedRectangle (juce::Rectangle<float> (bars.getX(), top - 1.0f, bars.getWidth(), 2.0f), 1.0f);

            const auto text = juce::String (juce::roundToInt (d)) + " %";
            const auto f = font (11.5f, 700);
            const float pillW = juce::jmin (bars.getWidth(), textWidth (f, text) + 18.0f);
            const float pillY = juce::jlimit (bars.getY() + 10.0f, bars.getBottom() - 10.0f, top);
            const auto pill = juce::Rectangle<float> (pillW, 20.0f).withCentre ({ bars.getCentreX(), pillY });
            g.setColour (Colours::background.withAlpha (0.92f));
            g.fillRoundedRectangle (pill, 10.0f);
            g.setColour (tint.withAlpha (dragging || hover ? 1.0f : 0.75f));
            g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, dragging ? 1.6f : 1.0f);
            g.setFont (f);
            g.setColour (lit ? Colours::text : Colours::text.withAlpha (0.85f));
            g.drawText (text, pill, juce::Justification::centred, false);

            if (bars.getWidth() > 70.0f && bars.getHeight() > 70.0f && d < 80.0f)
            {
                g.setFont (font (10.0f, 600));
                g.setColour (Colours::textFaint);
                g.drawText ("DRIVE", bars.withTrimmedTop (4.0f).withHeight (14.0f), juce::Justification::centred, false);
            }
        }

        paintHarmonics (g, set, band, area, on);
    }

    void paintHarmonics (juce::Graphics& g, int set, int band, juce::Rectangle<float> area, bool on)
    {
        auto strip = area.withTrimmedTop (6.0f + kChipH + 2.0f).removeFromTop (kMeterStripH).reduced (10.0f, 0.0f);

        if (band == 0)   // clear of the level scale labels at the plot's left edge
            strip.removeFromLeft (22.0f);

        if (strip.getWidth() < 40.0f)
            return;

        const float db = harmonicsShown[(size_t) set][(size_t) band];
        const bool valid = metersValid && on && db > kHarmFloorDb + 0.5f;
        const float labelW = strip.getWidth() > 130.0f ? 66.0f : 0.0f;

        g.setFont (font (10.0f, 600));
        g.setColour (Colours::textSubtle);

        if (labelW > 0.0f)
            g.drawText ("Harmonics", strip.removeFromLeft (labelW), juce::Justification::centredLeft, false);

        g.setFont (font (10.5f, 700));
        g.setColour (valid ? Colours::text.withAlpha (0.9f) : Colours::textFaint);
        g.drawText (valid ? juce::String (juce::roundToInt (db)) + " dB" : juce::String ("--"), strip.removeFromRight (40.0f),
                    juce::Justification::centredRight, false);

        const auto track = strip.reduced (4.0f, 0.0f).withSizeKeepingCentre (strip.getWidth() - 8.0f, 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (track, 2.0f);

        if (valid)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (db - kHarmFloorDb) / -kHarmFloorDb);
            const auto c = setColour();
            g.setGradientFill (juce::ColourGradient::horizontal (c.withAlpha (0.5f), track.getX(), c.brighter (0.4f), track.getRight()));
            g.fillRoundedRectangle (track.withWidth (juce::jmax (3.0f, track.getWidth() * t)), 2.0f);
        }
    }

    //==========================================================================
    bool bandMouseDown (int band, const juce::MouseEvent& e)
    {
        if (band < 0 || band >= numBands() || ! isLeftClick (e))
            return false;

        if (display.getSelectedBand() != band)
        {
            display.setSelectedBand (band);
            rebind();
        }

        if (e.getNumberOfClicks() >= 2)   // the double-click resets (onBandDoubleClick): no drag
            return true;

        drag.start (bandId (currentSet(), band, "drive"), twinId (band, "drive"));
        dragBand = band;
        dragPixels = juce::jmax (40.0f, barArea (display.getBandArea (band)).getHeight());
        return true;
    }

    void bandMouseDrag (int band, const juce::MouseEvent& e)
    {
        if (band != dragBand || ! drag.isArmed())
            return;

        const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();
        // The column's full height = drive 0..100 %.
        drag.dragTo (-(float) e.getDistanceFromDragStartY() / dragPixels * (fine ? 0.2f : 1.0f));
        display.repaint();
    }

    void endDrag()
    {
        if (drag.isArmed())
        {
            drag.end();
            display.repaint();
        }

        dragBand = -1;
    }

    bool resetBand (int band)
    {
        if (band < 0 || band >= numBands())
            return false;

        endDrag();
        const auto id = bandId (currentSet(), band, "drive");
        stereo::setParam (processor, id, twinId (band, "drive"), stereo::defaultValueOf (id));
        display.repaint();
        return true;
    }

    bool bandMenu (int band)
    {
        if (band < 0 || band >= numBands())
            return false;

        stereo::showParamMenu (display, processor, bandId (currentSet(), band, "drive"), twinId (band, "drive"),
                               "Band " + juce::String (band + 1) + " Drive");
        return true;
    }

    void setHoverBand (int band)
    {
        if (band == hoverBand)
            return;

        hoverBand = band;
        display.repaint();

        if (band >= 0 && ! hoverHintShown)
        {
            hoverHintShown = true;
            owner.showHeaderHint ("Drag a band up or down to set its drive");
        }
    }

    //==========================================================================
    void update()
    {
        if (selectedBand() != shownBand || currentSet() != shownSet || isLinked() != shownLinked || isSplit() != shownSplit)
            rebind();
        else
            updateTexts (false);

        const bool running = processor.isModuleInChain (ps::ModuleType::Exciter)
                             && processor.getParameterValue ("exc.on") >= 0.5f;
        const auto* exciter = dynamic_cast<const ps::Exciter*> (&moduleForChannelSet (processor, ps::ModuleType::Exciter, currentSet()));
        const bool valid = running && exciter != nullptr && processor.getChain().getSampleRate() > 0.0;
        bool changed = valid != metersValid;
        metersValid = valid;

        if (valid)
        {
            for (int s = 0; s < ps::kNumChannelSets; ++s)
                for (int b = 0; b < ps::kMaxBands; ++b)
                {
                    float target = exciter->getMeters().harmonicsDb[s][b].load();
                    target = std::isfinite (target) ? juce::jlimit (kHarmFloorDb, 6.0f, target) : kHarmFloorDb;
                    auto& shown = harmonicsShown[(size_t) s][(size_t) b];
                    // Fast rise, slower fall.
                    const float next = target > shown ? shown + 0.6f * (target - shown) : shown + 0.15f * (target - shown);

                    if (std::abs (next - shown) > 0.05f)
                        changed = true;

                    shown = next;
                }
        }

        const int type = juce::roundToInt (processor.getParameterValue ("exc.xoverType"));
        const bool os = processor.getParameterValue ("exc.os") >= 0.5f;

        if (type != shownXoverType || os != shownOs)
        {
            shownXoverType = type;
            shownOs = os;
            settingsCard.setSubtitle (type == (int) ps::CrossoverType::Digital ? "Linear-phase bands"
                                      : (os ? "4x oversampled" : "No oversampling"));
        }

        if (changed)
            display.repaint();
    }

    //==========================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto c = content.reduced (12, 6).withTrimmedBottom (6);
        auto cards = c.removeFromBottom (kCardsH);
        c.removeFromBottom (kGap);
        display.setBounds (c);

        const int total = cards.getWidth() - 2 * kGap;
        const int bandW = juce::roundToInt ((float) total * 0.43f);
        const int curveW = juce::jlimit (150, 280, juce::roundToInt ((float) total * 0.21f));
        bandCard.setBounds (cards.removeFromLeft (bandW));
        cards.removeFromLeft (kGap);
        curveCard.setBounds (cards.removeFromLeft (curveW));
        cards.removeFromLeft (kGap);
        settingsCard.setBounds (cards);

        {
            bandOn.setBounds (bandCard.getTitleRowRight (52));
            auto body = bandCard.getBody();
            const int gridRows = body.getWidth() >= 7 * 74 ? 1 : 2;
            modeGrid.setBounds (body.removeFromTop (gridRows == 1 ? 30 : 56));
            body.removeFromTop (6);
            const int knobW = juce::jmin (90, body.getWidth() / 2);
            auto knobs = body.withSizeKeepingCentre (knobW * 2 + 24, juce::jmin (body.getHeight(), 80));
            drive.setBounds (knobs.removeFromLeft (knobW));
            mix.setBounds (knobs.removeFromRight (knobW));
        }
        {
            auto body = curveCard.getBody();
            const auto note = curveCard.getBodyNoteArea();
            body.setBottom (juce::jmax (body.getY() + 40, note.getBottom() - 30));
            curve.setBounds (body);
        }
        {
            auto body = settingsCard.getBody();
            auto row = body.removeFromTop (24);
            const int half = (row.getWidth() - 6) / 2;
            oversampling.setLabel (half >= 118 ? "Oversampling 4x" : "Oversample");
            oversampling.setBounds (row.removeFromLeft (half));
            row.removeFromLeft (6);
            postOn.setBounds (row);

            auto xover = body.removeFromBottom (24);
            body.removeFromBottom (4);
            const bool wide = xover.getWidth() >= 200;
            xoverSegments.setVisible (wide);
            xoverChoice.setVisible (! wide);

            if (wide)
            {
                settingsCard.setCaptions ({ { "Crossover", xover.removeFromLeft (74) } });
                xoverSegments.setBounds (xover);
            }
            else
            {
                settingsCard.setCaptions ({});
                xoverChoice.setBounds (xover);
            }

            const int knobW = juce::jmin (90, body.getWidth() / 2);
            auto knobs = body.withSizeKeepingCentre (knobW * 2 + 16, juce::jmin (body.getHeight(), 80));
            postFreq.setBounds (knobs.removeFromLeft (knobW));
            postGain.setBounds (knobs.removeFromRight (knobW));
        }
    }

    ExciterPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    MultibandDisplay display;

    stereo::Card bandCard;
    ParamToggle bandOn;
    ExciterModeGrid modeGrid;
    ParamKnob drive, mix;

    stereo::Card curveCard;
    ExciterCurveView curve;

    stereo::Card settingsCard;
    ParamToggle oversampling, postOn;
    ParamKnob postFreq, postGain;
    ParamSegments xoverSegments;
    ParamChoice xoverChoice;

    stereo::ParamDrag drag;
    int dragBand = -1, hoverBand = -1;
    bool hoverHintShown = false;
    float dragPixels = 200.0f;

    std::array<std::array<float, ps::kMaxBands>, ps::kNumChannelSets> harmonicsShown {};
    bool metersValid = false, shownLinked = false, shownSplit = false, shownOs = true;
    int shownBand = -1, shownSet = -1, shownMode = -1, shownXoverType = -1;
};

//==============================================================================
ExciterPanel::ExciterPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::Exciter), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

ExciterPanel::~ExciterPanel() = default;

MultibandDisplay& ExciterPanel::getBandDisplay() noexcept { return impl->display; }

void ExciterPanel::tick()
{
    ModulePanel::tick();
    impl->display.tick();
    impl->update();
}

void ExciterPanel::frameTick() { impl->display.repaint(); }

void ExciterPanel::refresh()
{
    ModulePanel::refresh();
    impl->display.refresh();
    impl->rebind();
}

void ExciterPanel::channelSetChanged()
{
    impl->rebind();
}

void ExciterPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    impl->layout (contentArea);
}

} // namespace ps::ui
