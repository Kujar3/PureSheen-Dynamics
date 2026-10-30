#include "ui/EqPanel.h"

#include "ui/Controls.h"
#include "ui/EqDisplay.h"

// Equalizer 1 / Equalizer 2 panel (owner: ui-eq). See EqPanel.h.
//
// Layout (content area):  [ display ................................................ ]
//                         [ band card: chips | on / shape / slope | Freq Gain Q Phase ] [ settings card ]
// Controls that do not apply to the selected band's shape are disabled (dimmed), never hidden, so the strip does not
// jump while shapes change; Phase and FIR size exist only in Digital mode and are shown only there.
namespace ps::ui
{
namespace
{
constexpr int kGap = 10;
constexpr int kRowH = 22;
constexpr int kChipsMinCardW = 600;   // below this width the band chips are left out (the nodes select bands too)

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

void drawCard (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto b = area.toFloat().reduced (0.5f);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                       Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);
}

//==============================================================================
// Eight numbered chips: which bands are on (filled) and which one the strip edits (ring). Left click selects.
class BandChips final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit BandChips (EqModel& m) : model (m)
    {
        noFocus (*this);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Bands. Filled = on. Click one to edit it below.");
    }

    std::function<int()> channelSet;
    std::function<void (int)> onSelect;
    int selected = 0;

    juce::Rectangle<float> chip (int i) const
    {
        const auto b = getLocalBounds().toFloat();
        const float gap = 5.0f;
        const float size = juce::jmin ((b.getWidth() - 3.0f * gap) / 4.0f, (b.getHeight() - gap) / 2.0f);
        const float w = size * 4.0f + gap * 3.0f, h = size * 2.0f + gap;
        const float x0 = b.getX() + (b.getWidth() - w) * 0.5f, y0 = b.getY() + (b.getHeight() - h) * 0.5f;
        return { x0 + (float) (i % 4) * (size + gap), y0 + (float) (i / 4) * (size + gap), size, size };
    }

    void paint (juce::Graphics& g) override
    {
        const int set = channelSet != nullptr ? channelSet() : 0;
        const auto mouse = getMouseXYRelative().toFloat();

        for (int i = 0; i < ps::kEqBands; ++i)
        {
            const auto r = chip (i).reduced (0.5f);
            const bool on = model.band (set, i).on;
            const bool hover = isMouseOver() && chip (i).contains (mouse);
            const auto c = eqBandColour (i);

            if (on)
            {
                g.setGradientFill (juce::ColourGradient::vertical (c.brighter (0.1f), r.getY(), c.darker (0.15f), r.getBottom()));
                g.fillRoundedRectangle (r, 5.0f);
            }
            else
            {
                g.setColour (hover ? Colours::panelHover : juce::Colours::black.withAlpha (0.2f));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (Colours::outline.withAlpha (0.9f));
                g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
            }

            if (i == selected)
            {
                g.setColour (Colours::text);
                g.drawRoundedRectangle (r.expanded (1.5f), 6.5f, 1.5f);
            }
            else if (hover && on)
            {
                g.setColour (Colours::text.withAlpha (0.5f));
                g.drawRoundedRectangle (r.expanded (1.0f), 6.0f, 1.0f);
            }

            g.setFont (font (11.0f, 700));
            g.setColour (on ? Colours::background.withAlpha (0.9f) : (hover ? Colours::text : Colours::textSubtle));
            g.drawText (juce::String (i + 1), r, juce::Justification::centred, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e))
            return;

        for (int i = 0; i < ps::kEqBands; ++i)
            if (chip (i).expanded (2.0f).contains (e.position) && onSelect != nullptr)
                onSelect (i);
    }

private:
    EqModel& model;
};
} // namespace

//==============================================================================
struct EqPanel::Impl
{
    Impl (EqPanel& o, PureSheenProcessor& p, EditorServices& s, ps::ModuleType t)
        : owner (o), model (p, t),
          display (model, s, [this] { return activeSet(); }),
          chips (model),
          bandOn (p, model.bandId (0, 0, "on"), "Band 1"),
          slope (p, model.bandId (0, 0, "slope"), "Slope"),
          freq (p, model.bandId (0, 0, "freq"), "Freq"),
          gain (p, model.bandId (0, 0, "gain"), "Gain"),
          q (p, model.bandId (0, 0, "q"), "Q"),
          phase (p, model.bandId (0, 0, "phase"), "Phase"),
          mode (p, model.moduleId ("mode"), { "Analog", "Digital" }),
          firSize (p, model.moduleId ("firSize"), "FIR size"),
          softSat (p, model.moduleId ("softSat"), "Soft saturation"),
          amount (p, model.moduleId ("amount"), "Amount")
    {
        const auto colour = moduleColour (t);

        display.onSelectionChanged = [this] (int) { updateStrip (true); };
        chips.channelSet = [this] { return activeSet(); };
        chips.onSelect = [this] (int b)
        {
            display.setSelectedBand (b);
            updateStrip (true);
        };

        shape.setTooltip ("Filter shape of the selected band");
        shape.onOpenMenu = [this] (MenuChip& chip) { showShapeMenu (chip); };

        for (auto* k : { &freq, &gain, &q, &phase })
            k->setCompact (true);

        bandOn.setTooltip ("Switches the selected band on or off");
        slope.setTooltip ("Steepness of the high pass / low pass filter");
        q.setTooltip ("Width of the band: higher Q = narrower");
        phase.setTooltip ("Digital mode: 0 % = linear phase (no phase shift), 100 % = minimum phase (like Analog mode)");
        mode.setOnColour (colour);
        mode.setTooltip ("Analog: zero latency, natural phase shift. Digital: linear phase (no phase shift), adds latency.");
        firSize.setTooltip ("Digital mode filter length: longer = more precise low end, more latency");
        softSat.setOnColour (colour);
        softSat.setTooltip ("Gentle analog-style saturation on boosts only (cuts stay clean)");
        amount.setColour (colour);
        amount.setTooltip ("Scales every band's gain: 0 % = flat, 100 % = as set, 200 % = twice as much");

        for (juce::Component* c : std::initializer_list<juce::Component*> { &display, &chips, &bandOn, &shape, &slope, &freq, &gain,
                                                                             &q, &phase, &mode, &firSize, &softSat, &amount })
        {
            noFocus (*c);
            owner.addAndMakeVisible (c);
        }

        updateStrip (true);
    }

    int activeSet() const { return model.isSplit() ? owner.getChannelSet() : 0; }

    void showShapeMenu (MenuChip& chip)
    {
        const int set = activeSet(), b = display.getSelectedBand();
        const auto current = model.band (set, b).shape;
        auto menu = eqShapeMenu (current, model.isDigital());
        chip.setMenuOpen (true);

        showMenu (menu, chip, [this, set, b, current] (int result)
        {
            shape.setMenuOpen (false);

            if (result < 1 || result > (int) ps::EqShape::Count)
                return;

            const auto newShape = (ps::EqShape) (result - 1);

            if (newShape == current)
                return;

            std::vector<std::pair<juce::String, float>> values;

            for (int s : model.editSets (set))
                values.emplace_back (model.bandId (s, b, "shape"), (float) newShape);

            const auto setName = model.isLinked() ? juce::String() : model.setName (set);
            owner.processor.setParameterValues (values, juce::String (ps::moduleInfo (model.type).shortName) + " Band "
                                                            + juce::String (b + 1) + " Shape" + (setName.isNotEmpty() ? " (" + setName + ")" : juce::String())
                                                            + ": " + eqShapeFullName (newShape));
            model.update();
            display.parametersChanged();
            updateStrip (false);
        }, 170);
    }

    // Rebinds the strip to the selected band / channel set (when they changed or `force`), then updates labels, colours
    // and which controls apply.
    void updateStrip (bool force)
    {
        const int set = activeSet(), b = display.getSelectedBand();
        const bool linked = model.isLinked();

        if (force || set != boundSet || b != boundBand || linked != boundLinked)
        {
            boundSet = set;
            boundBand = b;
            boundLinked = linked;
            const int other = 1 - set;
            auto bind = [&] (auto& widget, const char* name)
            {
                widget.setParamId (model.bandId (set, b, name));
                widget.setLinkedParamId (linked ? model.bandId (other, b, name) : juce::String());
            };

            bind (bandOn, "on");
            bind (slope, "slope");
            bind (freq, "freq");
            bind (gain, "gain");
            bind (q, "q");
            bind (phase, "phase");

            const auto c = eqBandColour (b);
            bandOn.setLabel ("Band " + juce::String (b + 1));
            bandOn.setOnColour (c);

            for (auto* k : { &freq, &gain, &q, &phase })
                k->setColour (c);

            chips.selected = b;
            chips.repaint();
            shownShape = -1;
        }

        const auto& band = model.band (set, b);
        const auto traits = eqShapeTraits (band.shape);

        if ((int) band.shape != shownShape || model.isDigital() != shownDigital || band.on != shownOn)
        {
            shownShape = (int) band.shape;
            shownOn = band.on;
            shape.setText ({}, eqShapeShortName (band.shape)
                                   + (band.shape == ps::EqShape::Surgical && ! model.isDigital() ? " (Bell)" : juce::String()));
            setApplies (gain, traits.gain);
            setApplies (q, traits.q);
            setApplies (slope, traits.slope);
            setApplies (freq, true);
            setApplies (shape, true);
            setApplies (phase, true);
        }

        if (model.isDigital() != shownDigital)
        {
            shownDigital = model.isDigital();
            phase.setVisible (shownDigital);
            firSize.setVisible (shownDigital);
            layout();
            owner.repaint();
        }

        chips.repaint();
    }

    // Controls that do not apply to the shape are disabled; the others are a little dimmed while the band is off.
    void setApplies (juce::Component& c, bool applies) const
    {
        c.setEnabled (applies);
        c.setAlpha (applies ? (shownOn ? 1.0f : 0.6f) : 0.32f);
    }

    void layout()
    {
        auto r = contentArea.reduced (12, 10);

        if (r.isEmpty())
            return;

        const int stripH = r.getHeight() >= 520 ? 100 : 90;
        auto strip = r.removeFromBottom (stripH);
        r.removeFromBottom (kGap);
        display.setBounds (r);

        // Settings card (right)
        settingsCard = strip.removeFromRight (juce::jmin (strip.getWidth() / 3, 250));
        strip.removeFromRight (kGap);
        bandCard = strip;

        {
            auto inner = settingsCard.reduced (10, 8);
            const int knobW = juce::jlimit (60, 84, inner.getWidth() - 150);
            amount.setBounds (inner.removeFromRight (knobW));
            inner.removeFromRight (8);
            const int gap = (inner.getHeight() - 3 * kRowH) / 2;
            mode.setBounds (inner.removeFromTop (kRowH));
            inner.removeFromTop (gap);
            firRow = inner.removeFromTop (kRowH);
            firSize.setBounds (firRow);
            inner.removeFromTop (gap);
            softSat.setBounds (inner.removeFromTop (kRowH));
        }

        // Band card
        {
            auto inner = bandCard.reduced (10, 8);
            const bool showChips = bandCard.getWidth() >= kChipsMinCardW;
            chips.setVisible (showChips);

            if (showChips)
            {
                auto c = inner.removeFromLeft (104);
                chipsTitle = c.removeFromTop (16);
                chips.setBounds (c.reduced (0, 1));
                inner.removeFromLeft (14);
            }

            const int numKnobs = shownDigital ? 4 : 3;
            const int columnW = juce::jlimit (150, 196, inner.getWidth() - numKnobs * 66 - 14);
            auto column = inner.removeFromLeft (columnW);
            inner.removeFromLeft (14);
            const int gap = (column.getHeight() - 3 * kRowH) / 2;
            bandOn.setBounds (column.removeFromTop (kRowH));
            column.removeFromTop (gap);
            shape.setBounds (column.removeFromTop (kRowH));
            column.removeFromTop (gap);
            slope.setBounds (column.removeFromTop (kRowH));

            // Knob cells share the rest of the card, centred when there is more room than they need.
            const int knobW = juce::jlimit (58, 96, inner.getWidth() / numKnobs);
            inner = inner.withSizeKeepingCentre (juce::jmin (inner.getWidth(), knobW * numKnobs), inner.getHeight());
            ParamKnob* knobs[] { &freq, &gain, &q, &phase };

            for (int i = 0; i < numKnobs; ++i)
                knobs[i]->setBounds (inner.removeFromLeft (knobW));
        }
    }

    EqPanel& owner;
    EqModel model;
    EqDisplay display;
    BandChips chips;
    ParamToggle bandOn;
    MenuChip shape;
    ParamChoice slope;
    ParamKnob freq, gain, q, phase;
    ParamSegments mode;
    ParamChoice firSize;
    ParamToggle softSat;
    ParamKnob amount;

    juce::Rectangle<int> contentArea, bandCard, settingsCard, chipsTitle, firRow;
    int boundSet = -1, boundBand = -1, shownShape = -1;
    bool boundLinked = false, shownDigital = true, shownOn = true;
};



//==============================================================================
EqPanel::EqPanel (PureSheenProcessor& p, EditorServices& s, ps::ModuleType eq1or2)
    : ModulePanel (p, s, eq1or2)
{
    impl = std::make_unique<Impl> (*this, p, s, eq1or2);
}

EqPanel::~EqPanel() = default;

int EqPanel::getSelectedBand() const noexcept { return impl->display.getSelectedBand(); }

void EqPanel::tick()
{
    ModulePanel::tick();
    impl->model.update();
    impl->updateStrip (false);
    impl->display.tick();
}

void EqPanel::frameTick() { impl->display.repaint(); }

void EqPanel::refresh()
{
    ModulePanel::refresh();
    impl->model.update();
    impl->updateStrip (true);
    impl->display.parametersChanged();
}

void EqPanel::channelSetChanged()
{
    impl->model.update();
    impl->updateStrip (true);
    impl->display.parametersChanged();
}

void EqPanel::paint (juce::Graphics& g)
{
    ModulePanel::paint (g);

    if (impl == nullptr)
        return;

    auto& d = *impl;
    drawCard (g, d.bandCard);
    drawCard (g, d.settingsCard);

    if (d.chips.isVisible())
        drawSectionTitle (g, "Bands", d.chipsTitle, juce::Justification::centred);

    if (! d.shownDigital && ! d.firRow.isEmpty())
    {
        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText ("Zero latency, analog phase", d.firRow.reduced (4, 0), juce::Justification::centredLeft, true);
    }
}

void EqPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    if (impl == nullptr)
        return;

    impl->contentArea = contentArea;
    impl->layout();
}
} // namespace ps::ui
