#include "ui/ImagerPanel.h"

#include "dsp/Imager.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/MultibandDisplay.h"
#include "ui/Vectorscope.h"

#include <array>

// Imager panel. Owner: ui-stereo. Layout and interactions: see ImagerPanel.h.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr int kCardsH = 148;
// Band areas start under the display's header strip. The display keeps the bottom ~48 px of the plot for its
// frequency labels, legend and band count buttons, so the overlay stays clear of them.
constexpr float kCorrStripH = 26.0f;     // per-band correlation bar along the top of a band
constexpr float kBottomKeepOut = 54.0f;

juce::String widthText (float w)
{
    const int v = juce::roundToInt (w);
    return (v > 0 ? "+" : "") + juce::String (v) + " %";
}
} // namespace

//==============================================================================
struct ImagerPanel::Impl
{
    Impl (ImagerPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::Imager)),
          display (p, s, ps::ModuleType::Imager),
          scope (p, colour),
          scopeCard ("Vectorscope", colour),
          scopeModes ({ "Polar Sample", "Polar Level", "Lissajous" }, colour),
          widthCard ("Width", colour),
          amount (p, "img.amount", "Amount"),
          antiphase (p, "img.antiphase", "Prevent antiphase"),
          stzCard ("Stereoize", colour),
          stzOn (p, "img.stz.on", "On"),
          stzMode (p, "img.stz.mode", { "Mode I", "Mode II" }),
          stzModeShort (p, "img.stz.mode", { "I", "II" }),
          stzAmount (p, "img.stz.amount", "Amount"),
          recCard ("Recover Sides", colour),
          recOn (p, "img.rec.on", "On"),
          recGain (p, "img.rec.gain", "Gain"),
          xoverCard ("Crossover", colour),
          xoverSegments (p, "img.xoverType", { "Analog", "Digital", "Hybrid" }),
          xoverChoice (p, "img.xoverType", {}),
          drag (p)
    {
        correlationShown.fill (0.0f);

        for (auto* k : { &amount, &stzAmount, &recGain })
            k->setColour (colour);

        for (auto* t : { &antiphase, &stzOn, &recOn })
            t->setOnColour (colour);

        stzMode.setOnColour (colour);
        stzModeShort.setOnColour (colour);
        xoverSegments.setOnColour (colour);

        amount.setTooltip ("Amount: scales the width of every band (0 % = no width change).");
        antiphase.setTooltip ("Prevent antiphase: keeps every band's correlation at or above 0 so the mix stays mono "
                              "compatible.");
        stzOn.setTooltip ("Stereoize: adds width to bands set wider than 0 by creating side content from the centre.");
        stzMode.setTooltip ("Mode I: short delay, wide and spacious. Mode II: decorrelation that keeps transients tight.");
        stzModeShort.setTooltip ("Mode I: short delay, wide and spacious. Mode II: decorrelation that keeps transients tight.");
        stzAmount.setTooltip ("Stereoize amount (scaled by each band's width).");
        recOn.setTooltip ("Recover Sides: when a band is narrowed, puts the removed side content back into the centre "
                          "so hard-panned sounds keep their level.");
        recGain.setTooltip ("Level of the recovered side content.");
        xoverSegments.setTooltip ("Crossover type. Analog: no latency, phase-aligned bands. Digital: linear phase "
                                  "(adds latency). Hybrid: steeper phase-aligned slopes.");
        xoverChoice.setTooltip (xoverSegments.getTooltip());
        scopeModes.setTooltip ("Vectorscope display. Polar Sample: every sample as a dot. Polar Level: level per "
                               "direction. Lissajous: classic X/Y view.");

        // Cards and their controls.
        owner.addAndMakeVisible (display);
        owner.addAndMakeVisible (scopeCard);
        scopeCard.addAndMakeVisible (scopeModes);
        scopeCard.addAndMakeVisible (scope);

        owner.addAndMakeVisible (widthCard);
        widthCard.addAndMakeVisible (amount);
        widthCard.addAndMakeVisible (antiphase);

        owner.addAndMakeVisible (stzCard);
        stzCard.addAndMakeVisible (stzOn);
        stzCard.addChildComponent (stzMode);
        stzCard.addChildComponent (stzModeShort);
        stzCard.addAndMakeVisible (stzAmount);

        owner.addAndMakeVisible (recCard);
        recCard.addAndMakeVisible (recOn);
        recCard.addAndMakeVisible (recGain);

        owner.addAndMakeVisible (xoverCard);
        xoverCard.addChildComponent (xoverSegments);
        xoverCard.addChildComponent (xoverChoice);

        scopeModes.setSelected ((int) scope.getMode());
        scopeModes.onChange = [this] (int index)
        {
            scope.setMode ((Vectorscope::Mode) index);
        };

        // Band display hooks.
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
        display.onBandDoubleClick = [this] (int band, const juce::MouseEvent&) { return resetBand (band); };
        display.onBandPopupMenu = [this] (int band, const juce::MouseEvent&) { return bandMenu (band); };
        display.onBandMouseMove = [this] (int band, const juce::MouseEvent&) { setHoverBand (band); };
        display.getBandCursor = [] (int, juce::Point<float>) { return juce::MouseCursor (juce::MouseCursor::UpDownResizeCursor); };

        update (true);
    }

    ~Impl()
    {
        drag.end();
    }

    //==========================================================================
    bool isSplit() const { return currentChannelMode (processor, ps::ModuleType::Imager) == ps::ChannelMode::TransientSustain; }
    int currentSet() const { return isSplit() ? owner.getChannelSet() : 0; }
    bool isLinked() const { return isSplit() && processor.getParameterValue ("img.link") >= 0.5f; }

    juce::String setId (int set, const juce::String& suffix) const
    {
        return juce::String (set == 1 ? "img.b." : "img.") + suffix;
    }
    juce::String bandWidthId (int band) const { return setId (currentSet(), "b" + juce::String (band + 1) + ".width"); }
    juce::String twinWidthId (int band) const
    {
        return isLinked() ? setId (1 - currentSet(), "b" + juce::String (band + 1) + ".width") : juce::String();
    }

    void rebind()
    {
        endDrag();
        const int set = currentSet();
        const auto c = set == 1 ? Colours::sideChannel : colour;
        const auto twin = [&] (const char* suffix) { return isLinked() ? setId (1 - set, suffix) : juce::String(); };
        stzOn.setParamId (setId (set, "stz.on"));
        stzOn.setLinkedParamId (twin ("stz.on"));
        stzMode.setParamId (setId (set, "stz.mode"));
        stzMode.setLinkedParamId (twin ("stz.mode"));
        stzModeShort.setParamId (setId (set, "stz.mode"));
        stzModeShort.setLinkedParamId (twin ("stz.mode"));
        stzAmount.setParamId (setId (set, "stz.amount"));
        stzAmount.setLinkedParamId (twin ("stz.amount"));
        recOn.setParamId (setId (set, "rec.on"));
        recOn.setLinkedParamId (twin ("rec.on"));
        recGain.setParamId (setId (set, "rec.gain"));
        recGain.setLinkedParamId (twin ("rec.gain"));
        for (auto* t : { &stzOn, &recOn }) t->setOnColour (c);
        for (auto* k : { &stzAmount, &recGain }) k->setColour (c);
        stzMode.setOnColour (c);
        stzModeShort.setOnColour (c);
        display.setAccentColour (c);
        display.setChannelSet (set);
        shownSet = set;
        shownLinked = isLinked();
        shownSplit = isSplit();
        display.repaint();
    }

    int numBands() const
    {
        return juce::jlimit (1, ps::kMaxBands, juce::roundToInt (processor.getParameterValue ("img.bands")));
    }

    float width (int band) const { return processor.getParameterValue (bandWidthId (band)); }

    // Area of the width bars inside a band (under the correlation strip, above the display's bottom controls).
    juce::Rectangle<float> barArea (juce::Rectangle<float> band) const
    {
        return band.withTrimmedTop (kCorrStripH + 16.0f).withTrimmedBottom (kBottomKeepOut).reduced (10.0f, 0.0f);
    }

    float yForWidth (juce::Rectangle<float> bars, float w) const
    {
        return bars.getCentreY() - juce::jlimit (-100.0f, 100.0f, w) / 100.0f * bars.getHeight() * 0.5f;
    }

    void paintBand (juce::Graphics& g, int band, juce::Rectangle<float> area)
    {
        if (band < 0 || band >= ps::kMaxBands || area.getWidth() < 8.0f)
            return;

        const auto bars = barArea (area);

        if (bars.getHeight() < 20.0f)
            return;

        const float w = width (band);
        const bool selected = display.getSelectedBand() == band;
        const bool dragging = drag.isActive() && dragBand == band;
        const bool hover = hoverBand == band;
        const float zeroY = yForWidth (bars, 0.0f);
        const float valueY = yForWidth (bars, w);
        const auto tint = w < 0.0f ? Colours::info : colour;

        // Centre line (no width change).
        g.setColour (Colours::text.withAlpha (0.18f));
        const float dashes[] = { 4.0f, 4.0f };
        g.drawDashedLine ({ bars.getX(), zeroY, bars.getRight(), zeroY }, dashes, 2, 1.0f);

        // The width as a bar from the centre line.
        if (std::abs (w) > 0.05f)
        {
            const auto fill = juce::Rectangle<float>::leftTopRightBottom (bars.getX(), juce::jmin (zeroY, valueY),
                                                                          bars.getRight(), juce::jmax (zeroY, valueY));
            const float alphaTop = selected || hover || dragging ? 0.34f : 0.22f;
            g.setGradientFill (juce::ColourGradient::vertical (tint.withAlpha (w > 0.0f ? alphaTop : 0.06f), fill.getY(),
                                                               tint.withAlpha (w > 0.0f ? 0.06f : alphaTop), fill.getBottom()));
            g.fillRoundedRectangle (fill, 3.0f);
        }

        g.setColour (tint.withAlpha (selected || hover || dragging ? 1.0f : 0.8f));
        g.fillRoundedRectangle (juce::Rectangle<float> (bars.getX(), valueY - 1.0f, bars.getWidth(), 2.0f), 1.0f);

        // Value handle.
        const auto text = widthText (w);
        const auto f = font (11.5f, 700);
        const float pillW = juce::jmin (bars.getWidth(), textWidth (f, text) + 18.0f);
        const auto pill = juce::Rectangle<float> (pillW, 20.0f).withCentre ({ bars.getCentreX(), valueY });
        g.setColour (Colours::background.withAlpha (0.92f));
        g.fillRoundedRectangle (pill, 10.0f);
        g.setColour (tint.withAlpha (dragging || hover ? 1.0f : 0.75f));
        g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, dragging ? 1.6f : 1.0f);
        g.setFont (f);
        g.setColour (dragging || hover || selected ? Colours::text : Colours::text.withAlpha (0.85f));
        g.drawText (text, pill, juce::Justification::centred, false);

        paintCorrelation (g, band, area);
    }

    void paintCorrelation (juce::Graphics& g, int band, juce::Rectangle<float> area)
    {
        auto strip = area.removeFromTop (kCorrStripH).withTrimmedTop (6.0f).reduced (10.0f, 0.0f);

        if (band == 0)   // clear of the level scale labels at the plot's left edge
            strip.removeFromLeft (22.0f);

        if (strip.getWidth() < 40.0f)
            return;

        const bool valid = metersValid;
        const float c = correlationShown[(size_t) band];
        const auto labelFont = font (10.0f, 600);
        const juce::String value = valid ? juce::String (c, 2) : juce::String ("--");
        const float labelW = strip.getWidth() > 120.0f ? 34.0f : 0.0f;

        g.setFont (labelFont);
        g.setColour (Colours::textSubtle);

        if (labelW > 0.0f)
            g.drawText ("Corr", strip.removeFromLeft (labelW), juce::Justification::centredLeft, false);

        g.setColour (valid ? Colours::text.withAlpha (0.9f) : Colours::textFaint);
        g.setFont (font (10.5f, 700));
        g.drawText (value, strip.removeFromRight (32.0f), juce::Justification::centredRight, false);

        const auto track = strip.reduced (4.0f, 0.0f).withSizeKeepingCentre (strip.getWidth() - 8.0f, 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (Colours::textFaint);
        g.fillRect (juce::Rectangle<float> (1.0f, 8.0f).withCentre (track.getCentre()));

        if (valid)
        {
            const float mid = track.getCentreX();
            const float x = mid + juce::jlimit (-1.0f, 1.0f, c) * track.getWidth() * 0.5f;
            const auto cc = c < 0.0f ? Colours::major : (c < 0.3f ? Colours::minor : Colours::good);
            g.setColour (cc.withAlpha (0.8f));
            g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (mid, x), track.getY(),
                                                                                juce::jmax (mid, x), track.getBottom()), 2.0f);
            g.setColour (Colours::text);
            g.fillRoundedRectangle (juce::Rectangle<float> (3.0f, 9.0f).withCentre ({ x, track.getCentreY() }), 1.5f);
        }
    }

    //==========================================================================
    bool bandMouseDown (int band, const juce::MouseEvent& e)
    {
        if (band < 0 || band >= numBands() || ! isLeftClick (e))
            return false;

        if (display.getSelectedBand() != band)
            display.setSelectedBand (band);

        if (e.getNumberOfClicks() >= 2)   // the double-click resets (onBandDoubleClick): no drag
            return true;

        drag.start (bandWidthId (band), twinWidthId (band));
        dragBand = band;
        dragPixels = juce::jmax (40.0f, barArea (display.getBandArea (band)).getHeight());
        return true;
    }

    void bandMouseDrag (int band, const juce::MouseEvent& e)
    {
        if (band != dragBand || ! drag.isArmed())
            return;

        const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();
        // The bar area's height = the whole -100..+100 range (normalised 0..1).
        const float delta = -(float) e.getDistanceFromDragStartY() / dragPixels * (fine ? 0.2f : 1.0f);
        drag.dragTo (delta);
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
        stereo::setParam (processor, bandWidthId (band), twinWidthId (band), stereo::defaultValueOf (bandWidthId (band)));
        display.repaint();
        return true;
    }

    bool bandMenu (int band)
    {
        if (band < 0 || band >= numBands())
            return false;

        stereo::showParamMenu (display, processor, bandWidthId (band), twinWidthId (band), "Band " + juce::String (band + 1) + " Width");
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
            owner.showHeaderHint ("Drag a band up or down to widen or narrow it");
        }
    }

    //==========================================================================
    void update (bool force)
    {
        if (force || currentSet() != shownSet || isLinked() != shownLinked || isSplit() != shownSplit)
            rebind();

        // Per-band correlation from the module (only while it runs).
        const bool running = processor.isModuleInChain (ps::ModuleType::Imager)
                             && processor.getParameterValue ("img.on") >= 0.5f;
        const auto* imager = dynamic_cast<const ps::Imager*> (&moduleForChannelSet (processor, ps::ModuleType::Imager, currentSet()));
        const bool valid = running && imager != nullptr && processor.getChain().getSampleRate() > 0.0;
        bool changed = valid != metersValid;
        metersValid = valid;

        if (valid)
        {
            for (int b = 0; b < ps::kMaxBands; ++b)
            {
                const float target = juce::jlimit (-1.0f, 1.0f, imager->getMeters().correlation[b].load());
                auto& shown = correlationShown[(size_t) b];
                const float next = shown + 0.35f * (target - shown);

                if (std::abs (next - shown) > 0.002f)
                    changed = true;

                shown = next;
            }
        }

        // Stereoize acts on widened bands only, Recover Sides on narrowed bands only.
        bool anyWide = false, anyNarrow = false;

        for (int b = 0; b < numBands(); ++b)
        {
            const float w = width (b);
            anyWide = anyWide || w > 0.0f;
            anyNarrow = anyNarrow || w < 0.0f;
        }

        if (force || anyWide != shownAnyWide)
        {
            shownAnyWide = anyWide;
            stzCard.setInactiveNote (anyWide ? juce::String() : juce::String ("No band above 0"));
            stzCard.setAlpha (anyWide ? 1.0f : 0.6f);
        }

        if (force || anyNarrow != shownAnyNarrow)
        {
            shownAnyNarrow = anyNarrow;
            recCard.setInactiveNote (anyNarrow ? juce::String() : juce::String ("No band below 0"));
            recCard.setAlpha (anyNarrow ? 1.0f : 0.6f);
        }

        const int type = juce::roundToInt (processor.getParameterValue ("img.xoverType"));

        if (force || type != shownXoverType)
        {
            shownXoverType = type;
            xoverCard.setBodyNote (type == (int) ps::CrossoverType::Digital ? "Linear phase. Adds latency (about 75 ms)."
                                   : type == (int) ps::CrossoverType::Hybrid ? "Steep, phase-aligned slopes. No latency."
                                                                              : "Gentle, phase-aligned slopes. No latency.");
        }

        if (changed)
            display.repaint();
    }

    //==========================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto c = content.reduced (12, 6).withTrimmedBottom (6);

        const int rightW = juce::jlimit (220, 480, juce::jmin (juce::roundToInt (c.getWidth() * 0.3f), c.getHeight() - 60));
        auto right = c.removeFromRight (rightW);
        c.removeFromRight (kGap + 2);

        // Vectorscope card: modes under the title, the scope (with its history and meters) below.
        scopeCard.setBounds (right);
        {
            auto body = scopeCard.getBody();
            scopeModes.setBounds (body.removeFromTop (24));
            body.removeFromTop (8);
            scope.setBounds (body);
        }

        auto cards = c.removeFromBottom (kCardsH);
        c.removeFromBottom (kGap);
        display.setBounds (c);

        // Four cards: Width, Stereoize, Recover Sides, Crossover (a little narrower).
        const float unit = (float) (cards.getWidth() - 3 * kGap) / 3.85f;
        auto next = [&] (float weight) { auto r = cards.removeFromLeft (juce::roundToInt (unit * weight)); cards.removeFromLeft (kGap); return r; };
        widthCard.setBounds (next (1.0f));
        stzCard.setBounds (next (1.0f));
        recCard.setBounds (next (1.0f));
        xoverCard.setBounds (cards);

        // Narrow cards: the On toggles move from the title row into the body.
        const auto titleFont = font (11.0f, 700).withExtraKerningFactor (0.08f);
        const bool roomy = (float) recCard.getWidth() >= textWidth (titleFont, "RECOVER SIDES") + 2.0f * stereo::Card::kPad + 66.0f;
        stzCard.setTitleReserve (roomy ? 56 : 0);
        recCard.setTitleReserve (roomy ? 56 : 0);

        const int knobH = 80;
        {
            auto body = widthCard.getBody();
            antiphase.setLabel (body.getWidth() >= 128 ? "Prevent antiphase" : "Antiphase");
            antiphase.setBounds (body.removeFromTop (24));
            body.removeFromTop (4);
            amount.setBounds (body.withSizeKeepingCentre (juce::jmin (body.getWidth(), 90), juce::jmin (body.getHeight(), knobH)));
        }
        {
            auto body = stzCard.getBody();
            auto row = body.removeFromTop (24);

            if (roomy)
            {
                stzOn.setBounds (stzCard.getTitleRowRight (52));
            }
            else
            {
                stzOn.setBounds (row.removeFromLeft (46));
                row.removeFromLeft (4);
            }

            const bool longLabels = row.getWidth() >= 110;
            stzMode.setVisible (longLabels);
            stzModeShort.setVisible (! longLabels);
            stzMode.setBounds (row);
            stzModeShort.setBounds (row);
            body.removeFromTop (4);
            stzAmount.setBounds (body.withSizeKeepingCentre (juce::jmin (body.getWidth(), 90), juce::jmin (body.getHeight(), knobH)));
        }
        {
            auto body = recCard.getBody();

            if (roomy)
            {
                recOn.setBounds (recCard.getTitleRowRight (52));
                body.removeFromTop (14);
            }
            else
            {
                recOn.setBounds (body.removeFromTop (24).withSizeKeepingCentre (juce::jmin (body.getWidth(), 64), 24));
                body.removeFromTop (4);
            }

            recGain.setBounds (body.withSizeKeepingCentre (juce::jmin (body.getWidth(), 90), juce::jmin (body.getHeight(), knobH)));
        }
        {
            auto body = xoverCard.getBody();
            const bool wide = body.getWidth() >= 170;
            xoverSegments.setVisible (wide);
            xoverChoice.setVisible (! wide);
            (wide ? static_cast<juce::Component&> (xoverSegments) : xoverChoice).setBounds (body.removeFromTop (24));
        }
    }

    ImagerPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    MultibandDisplay display;
    Vectorscope scope;
    stereo::Card scopeCard;
    stereo::ChoiceSegments scopeModes;

    stereo::Card widthCard;
    ParamKnob amount;
    ParamToggle antiphase;

    stereo::Card stzCard;
    ParamToggle stzOn;
    ParamSegments stzMode, stzModeShort;
    ParamKnob stzAmount;

    stereo::Card recCard;
    ParamToggle recOn;
    ParamKnob recGain;

    stereo::Card xoverCard;
    ParamSegments xoverSegments;
    ParamChoice xoverChoice;

    stereo::ParamDrag drag;
    int dragBand = -1, hoverBand = -1;
    bool hoverHintShown = false;
    float dragPixels = 200.0f;

    std::array<float, ps::kMaxBands> correlationShown {};
    bool metersValid = false, shownAnyWide = false, shownAnyNarrow = false;
    int shownXoverType = -1;
    int shownSet = -1;
    bool shownLinked = false, shownSplit = false;
};

//==============================================================================
ImagerPanel::ImagerPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::Imager), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

ImagerPanel::~ImagerPanel() = default;

MultibandDisplay& ImagerPanel::getBandDisplay() noexcept { return impl->display; }
Vectorscope& ImagerPanel::getVectorscope() noexcept { return impl->scope; }

void ImagerPanel::tick()
{
    ModulePanel::tick();
    impl->display.tick();
    impl->scope.tick();
    impl->update (false);
}

void ImagerPanel::frameTick() { impl->display.repaint(); }

void ImagerPanel::refresh()
{
    ModulePanel::refresh();
    impl->display.refresh();
    impl->update (true);
    impl->display.repaint();
}

void ImagerPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    impl->layout (contentArea);
}

void ImagerPanel::channelSetChanged()
{
    impl->update (true);
}

} // namespace ps::ui
