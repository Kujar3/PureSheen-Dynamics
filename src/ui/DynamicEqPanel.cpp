#include "ui/DynamicEqPanel.h"

#include "dsp/DynamicEq.h"
#include "ui/Controls.h"
#include "ui/DynamicEqDisplay.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/EqModel.h"

#include <cmath>

// Dynamic EQ panel (owner: ui-deq). Layout: see DynamicEqPanel.h. Built like the Equalizer panel (same cards, chips,
// knobs and display interaction) with the dynamic controls in a second knob row and a threshold meter.
//
// Every parameter of the module is reachable: the 15 band parameters of the selected band / channel set in the band
// card (on, shape, direction, freq, gain, Q, offset, ratio, knee, attack, release, threshold, dynamic, auto scale,
// sidechain) and the module settings (filter mode, FIR size, lookahead) in the settings card. The channel mode, channel
// power, link and T/S options live in the header (ModulePanel).
namespace ps::ui
{
namespace
{
constexpr int kGap = 10;
constexpr int kRowH = 22;
constexpr int kChipsMinCardW = 700;   // below this band card width the chips become a "Band n" drop-down
constexpr int kGainColumnMinW = 900;  // the GAIN column needs at least this much display area width
constexpr float kMeterMinDb = -90.0f, kMeterMaxDb = 0.0f;

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

juce::String dbShort (float db)
{
    return (db > 0.05f ? "+" : "") + juce::String (std::abs (db) < 0.05f ? 0.0f : db, 1);
}

//==============================================================================
// Six numbered chips (3 x 2): which bands are on (filled), which one the strip edits (ring), and a thin activity line
// while a band moves. Left click selects.
class BandChips final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit BandChips (DynamicEqModel& m) : model (m)
    {
        noFocus (*this);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Bands. Filled = on. Click one to edit it below.");
    }

    std::function<int()> channelSet;
    std::function<void (int)> onSelect;
    int selected = 0;
    float activity[ps::kDynEqBands] {};   // |dynamic gain| in dB

    juce::Rectangle<float> chip (int i) const
    {
        const auto b = getLocalBounds().toFloat().reduced (2.5f);   // room for the selection ring
        const float gap = 5.0f;
        const float size = juce::jmin ((b.getWidth() - 2.0f * gap) / 3.0f, (b.getHeight() - gap) / 2.0f);
        const float w = size * 3.0f + gap * 2.0f, h = size * 2.0f + gap;
        const float x0 = b.getX() + (b.getWidth() - w) * 0.5f, y0 = b.getY() + (b.getHeight() - h) * 0.5f;
        return { x0 + (float) (i % 3) * (size + gap), y0 + (float) (i / 3) * (size + gap), size, size };
    }

    void paint (juce::Graphics& g) override
    {
        const int set = channelSet != nullptr ? channelSet() : 0;
        const auto mouse = getMouseXYRelative().toFloat();

        for (int i = 0; i < ps::kDynEqBands; ++i)
        {
            const auto r = chip (i).reduced (0.5f);
            const bool on = model.band (set, i).on;
            const bool hover = isMouseOver() && chip (i).contains (mouse);
            const auto c = eqBandColour (i);

            if (on)
            {
                g.setGradientFill (juce::ColourGradient::vertical (c.brighter (0.1f), r.getY(), c.darker (0.15f), r.getBottom()));
                g.fillRoundedRectangle (r, 5.0f);

                if (activity[i] > 0.1f)
                {
                    const float w = juce::jmin (1.0f, activity[i] / 9.0f) * (r.getWidth() - 8.0f);
                    g.setColour (Colours::background.withAlpha (0.75f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (r.getCentreX() - w * 0.5f, r.getBottom() - 4.5f, w, 2.0f), 1.0f);
                }
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
            g.drawText (juce::String (i + 1), r.translated (0.0f, activity[i] > 0.1f && on ? -1.0f : 0.0f),
                        juce::Justification::centred, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e))
            return;

        for (int i = 0; i < ps::kDynEqBands; ++i)
            if (chip (i).expanded (2.0f).contains (e.position) && onSelect != nullptr)
                onSelect (i);
    }

private:
    DynamicEqModel& model;
};

//==============================================================================
// Threshold meter of the selected band: the detector level (wide bar, the part over the threshold in the band colour)
// against the threshold line, and the dynamic gain (narrow bar from a centre line: down = cut, up = boost).
// Drag vertically = threshold (one gesture), double-click = type a value, right-click = Reset / Enter value.
class ThresholdMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ThresholdMeter (PureSheenProcessor& p) : processor (p)
    {
        noFocus (*this);
        setRepaintsOnMouseActivity (true);
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setTooltip ("Threshold meter: the band's detector level (wide bar) against its threshold (line), and how far "
                    "the band moves right now (narrow bar). Drag up or down to set the threshold; double-click to type a value, right-click to reset.");
    }

    ~ThresholdMeter() override { endGesture(); }

    void setTarget (const juce::String& id, const juce::String& linkedId)
    {
        if (id == paramId && linkedId == linkedParamId)
            return;

        endGesture();
        paramId = id;
        linkedParamId = linkedId;
        repaint();
    }

    void setValues (float detDb, float dynDb, float thrDb, juce::Colour c, bool isActive)
    {
        if (std::abs (detDb - det) < 0.05f && std::abs (dynDb - dyn) < 0.02f && thrDb == thr && c == colour && isActive == active)
            return;

        det = detDb;
        dyn = dynDb;
        thr = thrDb;
        colour = c;
        active = isActive;
        repaint();
    }

    juce::Rectangle<float> levelTrack() const
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f, 4.0f);
        return r.removeFromLeft (r.getWidth() * 0.62f);
    }

    juce::Rectangle<float> gainTrack() const
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f, 4.0f);
        return r.removeFromRight (r.getWidth() * 0.3f);
    }

    float yOfLevel (float db) const
    {
        const auto t = levelTrack();
        return t.getBottom() - (juce::jlimit (kMeterMinDb, kMeterMaxDb, db) - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb) * t.getHeight();
    }

    void paint (juce::Graphics& g) override
    {
        const auto lt = levelTrack(), gt = gainTrack();
        const float alpha = active ? 1.0f : 0.45f;

        for (auto t : { lt, gt })
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (t, 3.0f);
            g.setColour (Colours::outline.withAlpha (0.7f));
            g.drawRoundedRectangle (t.reduced (0.5f), 3.0f, 1.0f);
        }

        const float thrY = std::round (yOfLevel (thr));

        // Detector level
        if (det > kMeterMinDb)
        {
            const float y = yOfLevel (det);
            const auto bar = juce::Rectangle<float> (lt.getX() + 2.0f, y, lt.getWidth() - 4.0f, lt.getBottom() - 2.0f - y);
            g.setGradientFill (juce::ColourGradient::vertical (Colours::input.withAlpha (0.75f * alpha), bar.getY(),
                                                               Colours::input.withAlpha (0.35f * alpha), bar.getBottom()));
            g.fillRect (bar);

            if (y < thrY)
            {
                g.setColour (colour.withAlpha (0.9f * alpha));
                g.fillRect (juce::Rectangle<float> (bar.getX(), y, bar.getWidth(), thrY - y));
            }
        }

        // Threshold line with side ticks
        const bool hot = isMouseOverOrDragging();
        g.setColour (colour.withAlpha (alpha));
        g.fillRect (juce::Rectangle<float> (lt.getX() - 1.0f, thrY - (hot ? 1.0f : 0.75f), lt.getWidth() + 2.0f, hot ? 2.0f : 1.5f));
        juce::Path tick;
        tick.addTriangle (lt.getRight() + 1.0f, thrY, lt.getRight() + 5.0f, thrY - 3.5f, lt.getRight() + 5.0f, thrY + 3.5f);
        g.fillPath (tick);

        // Dynamic gain (+-15 dB around the centre line)
        const float zeroY = gt.getCentreY();
        g.setColour (Colours::outline);
        g.fillRect (juce::Rectangle<float> (gt.getX() + 1.0f, zeroY - 0.5f, gt.getWidth() - 2.0f, 1.0f));

        if (std::abs (dyn) > 0.05f)
        {
            const float h = juce::jmin (1.0f, std::abs (dyn) / 15.0f) * (gt.getHeight() * 0.5f - 2.0f);
            const auto c = dyn < 0.0f ? Colours::gainReduction : Colours::good;
            const auto bar = dyn < 0.0f ? juce::Rectangle<float> (gt.getX() + 2.0f, zeroY, gt.getWidth() - 4.0f, h)
                                        : juce::Rectangle<float> (gt.getX() + 2.0f, zeroY - h, gt.getWidth() - 4.0f, h);
            g.setColour (c.withAlpha (0.95f * alpha));
            g.fillRect (bar);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            stereo::showParamMenu (*this, processor, paramId, linkedParamId, "Threshold");
            return;
        }

        if (! isLeftClick (e))
            return;

        endGesture();
        param = processor.getParameter (paramId);
        linked = linkedParamId.isNotEmpty() ? processor.getParameter (linkedParamId) : nullptr;
        dragDb = thr;
        lastY = e.position.y;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e) || param == nullptr)
            return;

        const float dy = e.position.y - lastY;
        lastY = e.position.y;

        if (std::abs (dy) < 1.0e-4f)
            return;

        if (! open)
        {
            open = true;
            param->beginChangeGesture();

            if (linked != nullptr)
                linked->beginChangeGesture();
        }

        const bool fine = e.mods.isShiftDown() || e.mods.isCtrlDown();
        const float dbPerPx = (kMeterMaxDb - kMeterMinDb) / juce::jmax (1.0f, levelTrack().getHeight());
        dragDb = juce::jlimit (-130.0f, 0.0f, dragDb - dy * dbPerPx * (fine ? 0.1f : 1.0f));
        param->setValueNotifyingHost (param->convertTo0to1 (dragDb));

        if (linked != nullptr)
            linked->setValueNotifyingHost (linked->convertTo0to1 (dragDb));

        thr = dragDb;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { endGesture(); }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e))
            return;

        endGesture();
        stereo::promptParamValue (*this, processor, paramId, linkedParamId, "Threshold");
    }

private:
    void endGesture()
    {
        if (open && param != nullptr)
        {
            param->endChangeGesture();

            if (linked != nullptr)
                linked->endChangeGesture();
        }

        open = false;
        param = linked = nullptr;
    }

    PureSheenProcessor& processor;
    juce::String paramId, linkedParamId;
    juce::RangedAudioParameter* param = nullptr;
    juce::RangedAudioParameter* linked = nullptr;
    bool open = false;
    float det = -200.0f, dyn = 0.0f, thr = -30.0f, dragDb = 0.0f, lastY = 0.0f;
    juce::Colour colour = Colours::accent;
    bool active = true;
};
} // namespace

//==============================================================================
struct DynamicEqPanel::Impl
{
    Impl (DynamicEqPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), model (p),
          display (model, s, [this] { return activeSet(); }),
          gainColumn (model),
          chips (model),
          meter (p),
          bandOn (p, model.bandId (0, 0, "on"), "Band 1"),
          direction (p, model.bandId (0, 0, "dir"), { "Down", "Up" }),
          freq (p, model.bandId (0, 0, "freq"), "Freq"),
          gain (p, model.bandId (0, 0, "gain"), "Gain"),
          q (p, model.bandId (0, 0, "q"), "Q"),
          offset (p, model.bandId (0, 0, "offset"), "Offset"),
          ratio (p, model.bandId (0, 0, "ratio"), "Ratio"),
          knee (p, model.bandId (0, 0, "knee"), "Knee"),
          attack (p, model.bandId (0, 0, "att"), "Attack"),
          release (p, model.bandId (0, 0, "rel"), "Release"),
          threshold (p, model.bandId (0, 0, "thr"), "Threshold"),
          dynamicOn (p, model.bandId (0, 0, "dyn"), "Dynamic"),
          autoScale (p, model.bandId (0, 0, "auto"), "Auto scale"),
          sidechain (p, model.bandId (0, 0, "sc"), "Listen to"),
          sidechainShort (p, model.bandId (0, 0, "sc")),
          mode (p, model.moduleId ("mode"), { "Analog", "Digital" }),
          firSize (p, model.moduleId ("firSize"), "FIR size"),
          lookahead (p, model.moduleId ("lookahead"), false, "Lookahead")
    {
        const auto colour = moduleColour (ps::ModuleType::DynamicEq);

        display.onSelectionChanged = [this] (int) { updateStrip (true); };
        gainColumn.onSelect = [this] (int b) { selectBand (b); };
        chips.channelSet = [this] { return activeSet(); };
        chips.onSelect = [this] (int b) { selectBand (b); };

        shape.setTooltip ("Filter shape of the selected band");
        shape.onOpenMenu = [this] (MenuChip& chip) { showShapeMenu (chip); };
        bandPicker.setTooltip ("The band edited below");
        bandPicker.onOpenMenu = [this] (MenuChip& chip) { showBandMenu (chip); };

        for (auto* k : { &freq, &gain, &q, &offset, &ratio, &knee, &attack, &release, &threshold })
            k->setCompact (true);

        bandOn.setTooltip ("Switches the selected band on or off");
        direction.setTooltip ("Down: the band moves toward its gain when the level goes above the threshold (a cut "
                              "that grows when loud, a boost that backs off). Up: the opposite (a boost that grows when "
                              "loud, a cut that opens up).");
        freq.setTooltip ("Centre / corner frequency of the band");
        gain.setTooltip ("The band's full dynamic gain: where it goes when fully triggered (Down with a cut, Up with a "
                         "boost) or where it rests (Down with a boost, Up with a cut)");
        q.setTooltip ("Width of the band: higher Q = narrower");
        offset.setTooltip ("Static gain added to the band, whatever the level");
        ratio.setTooltip ("How strongly the band reacts: dB of movement per dB over the threshold = 1 - 1 / ratio");
        knee.setTooltip ("Soft knee width around the threshold: how gradually the band starts to move");
        attack.setTooltip ("How fast the band moves when the level rises (with Auto scale: scales the frequency curve, 15 ms = as is)");
        release.setTooltip ("How fast the band returns when the level falls (with Auto scale: scales the frequency curve, 100 ms = as is)");
        threshold.setTooltip ("Detector level where the band starts to move");
        dynamicOn.setTooltip ("Dynamic: the band follows the level. Off: a static band at gain + offset");
        autoScale.setTooltip ("Auto scale: fast attack / release for low bands, slower for high bands");
        sidechain.setTooltip ("What the band listens to: its own frequency range, the range of another band, or the "
                              "same range on the other channel (Mid / Side, Left / Right)");
        sidechainShort.setTooltip ("Listen to: " + sidechain.getTooltip());
        mode.setOnColour (colour);
        mode.setTooltip ("Analog: natural phase, no latency (only the lookahead). Digital: linear phase (no phase "
                         "shift), adds latency.");
        firSize.setTooltip ("Digital mode filter length: longer = more precise low end, more latency");
        lookahead.setColour (colour);
        lookahead.setTooltip ("Lookahead: the audio is delayed so the bands react before the level arrives (adds latency)");

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &display, &gainColumn, &chips, &bandPicker, &meter, &bandOn, &shape, &direction, &freq, &gain, &q, &offset,
                 &ratio, &knee, &attack, &release, &threshold, &dynamicOn, &autoScale, &sidechain, &sidechainShort, &mode, &firSize,
                 &lookahead })
        {
            noFocus (*c);
            owner.addAndMakeVisible (c);
        }

        updateStrip (true);
    }

    int activeSet() const { return model.isSplit() ? owner.getChannelSet() : 0; }

    juce::Colour setColour() const
    {
        return activeSet() == 1 ? Colours::sideChannel : moduleColour (ps::ModuleType::DynamicEq);
    }

    void selectBand (int b)
    {
        display.setSelectedBand (b);
        updateStrip (true);
    }

    // One undo step for one band value (both sets when linked).
    void setBandValue (int b, const char* name, float v, const juce::String& description)
    {
        std::vector<std::pair<juce::String, float>> values;

        for (int s : model.editSets (activeSet()))
            values.emplace_back (model.bandId (s, b, name), v);

        owner.processor.setParameterValues (values, description);
        model.update();
        display.parametersChanged();
        updateStrip (false);
    }

    void showShapeMenu (MenuChip& chip)
    {
        const int set = activeSet(), b = display.getSelectedBand();
        const auto current = model.band (set, b).shape;
        auto menu = dynEqShapeMenu (current);
        chip.setMenuOpen (true);

        showMenu (menu, chip, [this, set, b, current] (int result)
        {
            shape.setMenuOpen (false);

            if (result < 1 || result > (int) ps::DynEqShape::Count)
                return;

            const auto newShape = (ps::DynEqShape) (result - 1);

            if (newShape != current)
                setBandValue (b, "shape", (float) newShape, model.bandLabel (set, b) + " Shape: " + dynEqShapeFullName (newShape));
        }, 170);
    }

    void showBandMenu (MenuChip& chip)
    {
        const int set = activeSet();
        juce::PopupMenu menu;

        for (int b = 0; b < ps::kDynEqBands; ++b)
        {
            const auto& band = model.band (set, b);
            const auto text = "Band " + juce::String (b + 1) + "   "
                              + (band.on ? dynEqShapeShortName (band.shape) + "  " + fmt::hz (band.freqHz) : juce::String ("off"));
            menu.addItem (1 + b, text, true, b == display.getSelectedBand());
        }

        chip.setMenuOpen (true);
        showMenu (menu, chip, [this] (int result)
        {
            bandPicker.setMenuOpen (false);

            if (result >= 1 && result <= ps::kDynEqBands)
                selectBand (result - 1);
        }, 200);
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
            auto id = [&] (int s, const char* name) { return model.bandId (s, b, name); };
            auto bind = [&] (auto& widget, const char* name)
            {
                widget.setParamId (id (set, name));
                widget.setLinkedParamId (linked ? id (other, name) : juce::String());
            };

            bind (bandOn, "on");
            bind (direction, "dir");
            bind (freq, "freq");
            bind (gain, "gain");
            bind (q, "q");
            bind (offset, "offset");
            bind (ratio, "ratio");
            bind (knee, "knee");
            bind (attack, "att");
            bind (release, "rel");
            bind (threshold, "thr");
            bind (dynamicOn, "dyn");
            bind (autoScale, "auto");
            bind (sidechain, "sc");
            bind (sidechainShort, "sc");
            meter.setTarget (id (set, "thr"), linked ? id (other, "thr") : juce::String());

            const auto c = eqBandColour (b);
            bandOn.setLabel ("Band " + juce::String (b + 1));
            bandOn.setOnColour (c);
            direction.setOnColour (c);
            dynamicOn.setOnColour (c);
            autoScale.setOnColour (c);

            for (auto* k : { &freq, &gain, &q, &offset, &ratio, &knee, &attack, &release, &threshold })
                k->setColour (c);

            bandPicker.setText ("Band", juce::String (b + 1));
            bandPicker.setValueColour (c);
            chips.selected = b;
            chips.repaint();
            shownShape = -1;
        }

        const auto& band = model.band (set, b);

        if ((int) band.shape != shownShape || band.on != shownOn || band.dynamic != shownDynamic)
        {
            shownShape = (int) band.shape;
            shownOn = band.on;
            shownDynamic = band.dynamic;
            shape.setText ({}, dynEqShapeShortName (band.shape));
            const bool hasQ = dynEqShapeTraits (band.shape).q;

            for (juce::Component* c : std::initializer_list<juce::Component*> { &shape, &freq, &gain, &offset, &dynamicOn })
                setApplies (*c, true, false);

            setApplies (q, hasQ, false);

            for (juce::Component* c : std::initializer_list<juce::Component*> { &direction, &ratio, &knee, &attack, &release,
                                                                                 &threshold, &autoScale, &sidechain, &sidechainShort, &meter })
                setApplies (*c, true, true);
        }

        if (model.isDigital() != shownDigital)
        {
            shownDigital = model.isDigital();
            firSize.setVisible (shownDigital);
            owner.repaint();
        }

        chips.repaint();
    }

    // Not applicable: disabled. Applicable: a little dimmed while the band is off, and (dynamic controls) while the band
    // is static.
    void setApplies (juce::Component& c, bool applies, bool dynamicControl) const
    {
        c.setEnabled (applies);
        float alpha = applies ? 1.0f : 0.32f;

        if (applies && ! shownOn)
            alpha = 0.6f;

        if (applies && dynamicControl && ! shownDynamic)
            alpha = juce::jmin (alpha, 0.45f);

        c.setAlpha (alpha);
    }

    //==========================================================================
    void layout()
    {
        auto r = contentArea.reduced (12, 10);

        if (r.isEmpty())
            return;

        const int h = r.getHeight();
        rowH = h >= 640 ? 72 : (h >= 480 ? 62 : 54);
        const int stripH = 2 * rowH + 6 + 18;
        auto strip = r.removeFromBottom (stripH);
        r.removeFromBottom (kGap);

        const bool showColumn = r.getWidth() >= kGainColumnMinW;
        gainColumn.setVisible (showColumn);

        if (showColumn)
        {
            gainColumn.setBounds (r.removeFromRight (juce::jlimit (124, 140, r.getWidth() / 8)));
            r.removeFromRight (kGap);
        }

        display.setBounds (r);

        // Settings card (right): Analog | Digital, FIR size (Digital) or the phase note, lookahead, latency
        settingsCard = strip.removeFromRight (juce::jlimit (190, 230, strip.getWidth() / 5));
        strip.removeFromRight (kGap);
        bandCard = strip;

        {
            auto inner = settingsCard.reduced (12, 9);
            const int gap = juce::jmin (12, (inner.getHeight() - 4 * kRowH) / 3);
            inner = inner.withSizeKeepingCentre (inner.getWidth(), 4 * kRowH + 3 * gap);
            mode.setBounds (inner.removeFromTop (kRowH));
            inner.removeFromTop (gap);
            firRow = inner.removeFromTop (kRowH);
            firSize.setBounds (firRow);
            inner.removeFromTop (gap);
            lookahead.setBounds (inner.removeFromTop (kRowH));
            inner.removeFromTop (gap);
            latencyRow = inner.removeFromTop (kRowH);
        }

        // Band card: [chips] [on / shape / direction] [knob grid 4 x 2] [meter + threshold / now] [dynamic / auto / sidechain]
        {
            auto inner = bandCard.reduced (12, 9);
            separators.clear();
            const bool showChips = bandCard.getWidth() >= kChipsMinCardW;
            chips.setVisible (showChips);
            bandPicker.setVisible (! showChips);

            if (showChips)
            {
                auto c = inner.removeFromLeft (84);
                c = c.withSizeKeepingCentre (c.getWidth(), juce::jmin (c.getHeight(), 20 + 62));
                chipsTitle = c.removeFromTop (18);
                chips.setBounds (c);
                inner.removeFromLeft (14);
            }

            const int meterW = 26;
            const int groupGap = 12;
            const int avail = inner.getWidth() - meterW - 6 - 3 * groupGap;
            const int colAW = juce::jlimit (114, 150, (int) ((float) avail * 0.21f));
            const int colEW = juce::jlimit (112, 150, (int) ((float) avail * 0.2f));
            const int cellW = juce::jlimit (48, 76, (avail - colAW - colEW) / 5);
            const int used = colAW + colEW + 5 * cellW + meterW + 6 + 3 * groupGap;
            const int extraGap = juce::jmax (0, (inner.getWidth() - used) / 4);
            const int gg = groupGap + extraGap;

            // Rows of 22 px, spread over the column but never more than 16 px apart, centred vertically.
            auto stack = [] (juce::Rectangle<int> col, std::initializer_list<juce::Component*> items)
            {
                const int n = (int) items.size();
                const int gap = n > 1 ? juce::jmin (16, (col.getHeight() - n * kRowH) / (n - 1)) : 0;
                col = col.withSizeKeepingCentre (col.getWidth(), n * kRowH + (n - 1) * gap);

                for (auto* c : items)
                {
                    c->setBounds (col.removeFromTop (kRowH));
                    col.removeFromTop (gap);
                }
            };

            auto colA = inner.removeFromLeft (colAW);
            inner.removeFromLeft (gg);

            if (showChips)
                stack (colA, { &bandOn, &shape, &direction });
            else
                stack (colA, { &bandPicker, &bandOn, &shape, &direction });

            const int gridH = 2 * rowH + 6;
            auto grid = inner.removeFromLeft (4 * cellW).withSizeKeepingCentre (4 * cellW, juce::jmin (inner.getHeight(), gridH));
            ParamKnob* rows[2][4] { { &freq, &gain, &q, &offset }, { &ratio, &knee, &attack, &release } };

            for (int row = 0; row < 2; ++row)
                for (int k = 0; k < 4; ++k)
                    rows[row][k]->setBounds (grid.getX() + k * cellW, grid.getY() + row * (rowH + 6), cellW, rowH);

            separators.push_back ({ grid.getX() + 4, grid.getY() + rowH + 2, 4 * cellW - 8, 1 });
            separators.push_back ({ grid.getX() - gg / 2, bandCard.getY() + 12, 1, bandCard.getHeight() - 24 });
            inner.removeFromLeft (gg);
            separators.push_back ({ inner.getX() - gg / 2, bandCard.getY() + 12, 1, bandCard.getHeight() - 24 });

            auto thr = inner.removeFromLeft (meterW + 6 + cellW);
            meter.setBounds (thr.removeFromLeft (meterW).withSizeKeepingCentre (meterW, juce::jmin (thr.getHeight(), gridH)));
            thr.removeFromLeft (6);
            const auto thrGrid = thr.withSizeKeepingCentre (thr.getWidth(), juce::jmin (thr.getHeight(), gridH));
            threshold.setBounds (thrGrid.getX(), thrGrid.getY(), cellW, rowH);
            nowArea = juce::Rectangle<int> (thrGrid.getX(), thrGrid.getY() + rowH + 6, cellW, rowH);
            inner.removeFromLeft (gg);
            separators.push_back ({ inner.getX() - gg / 2, bandCard.getY() + 12, 1, bandCard.getHeight() - 24 });

            auto colE = inner.removeFromLeft (juce::jmin (colEW, inner.getWidth()));
            const bool longChoice = colE.getWidth() >= 140;
            sidechain.setVisible (longChoice);
            sidechainShort.setVisible (! longChoice);
            stack (colE, { &dynamicOn, &autoScale, longChoice ? (juce::Component*) &sidechain : (juce::Component*) &sidechainShort });
        }

        owner.repaint();
    }

    //==========================================================================
    void tick()
    {
        if (model.update())
            display.parametersChanged();

        updateStrip (false);
        display.tick();

        const int set = activeSet(), b = display.getSelectedBand();
        const auto& live = display.getLive();
        const auto& band = model.band (set, b);

        if (gainColumn.isVisible())
            gainColumn.update (set, live, b);

        for (int i = 0; i < ps::kDynEqBands; ++i)
            chips.activity[i] = live.running && model.band (set, i).dynamic ? std::abs (live.dynamicDb[set][i]) : 0.0f;

        if (chips.isVisible())
            chips.repaint();

        meter.setValues (band.on && live.running ? live.detectorDb[set][b] : -200.0f,
                         band.on && live.running ? live.dynamicDb[set][b] : 0.0f,
                         band.thresholdDb, eqBandColour (b), band.on && band.dynamic);

        // "Now" readout and the latency line
        const float now = band.on && live.running ? live.dynamicDb[set][b] : 0.0f;
        const auto nowText = ! band.on ? juce::String ("--") : (band.dynamic ? dbShort (now) + " dB" : juce::String ("static"));
        const auto colour = ! band.on || ! band.dynamic || std::abs (now) < 0.05f ? Colours::textFaint
                                                                                    : (now < 0.0f ? Colours::gainReduction : Colours::good);

        if (nowText != shownNow || colour != shownNowColour)
        {
            shownNow = nowText;
            shownNowColour = colour;
            owner.repaint (nowArea);
        }

        const auto latency = latencyText();

        if (latency != shownLatency)
        {
            shownLatency = latency;
            owner.repaint (latencyRow);
        }
    }

    juce::String latencyText() const
    {
        auto& chain = owner.processor.getChain();
        const double sr = chain.getSampleRate() > 1000.0 ? chain.getSampleRate() : 48000.0;
        const int samples = chain.getModule (ps::ModuleType::DynamicEq).getLatencySamples();

        if (samples <= 0)
            return "Zero latency";

        const double ms = 1000.0 * (double) samples / sr;
        return "Latency " + (ms < 10.0 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms))) + " ms";
    }

    void paint (juce::Graphics& g)
    {
        drawCard (g, bandCard);
        drawCard (g, settingsCard);

        for (const auto& s : separators)
        {
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.fillRect (s);
        }

        if (chips.isVisible())
            drawSectionTitle (g, "Bands", chipsTitle, juce::Justification::centred);

        // "Now" readout under the threshold knob (like a knob's caption + value)
        if (! nowArea.isEmpty())
        {
            auto a = nowArea;
            const int top = a.getY() + (a.getHeight() - 34) / 2;
            g.setFont (font (10.5f, 600));
            g.setColour (Colours::textDim);
            g.drawText ("Moving now", juce::Rectangle<int> (a.getX() - 4, top, a.getWidth() + 8, 14), juce::Justification::centred, false);
            g.setFont (font (12.5f, 700));
            g.setColour (shownNowColour);
            g.drawFittedText (shownNow, juce::Rectangle<int> (a.getX() - 4, top + 16, a.getWidth() + 8, 16), juce::Justification::centred, 1, 0.8f);
        }

        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);

        if (! shownDigital && ! firRow.isEmpty())
            g.drawText ("Minimum phase", firRow.reduced (4, 0), juce::Justification::centredLeft, true);

        if (! latencyRow.isEmpty())
            g.drawText (shownLatency, latencyRow.reduced (4, 0), juce::Justification::centredLeft, true);
    }

    //==========================================================================
    DynamicEqPanel& owner;
    DynamicEqModel model;
    DynamicEqDisplay display;
    DynamicEqGainColumn gainColumn;
    BandChips chips;
    MenuChip bandPicker;
    ThresholdMeter meter;
    ParamToggle bandOn;
    MenuChip shape;
    ParamSegments direction;
    ParamKnob freq, gain, q, offset, ratio, knee, attack, release, threshold;
    ParamToggle dynamicOn, autoScale;
    ParamChoice sidechain, sidechainShort;
    ParamSegments mode;
    ParamChoice firSize;
    ParamFader lookahead;

    juce::Rectangle<int> contentArea, bandCard, settingsCard, chipsTitle, firRow, latencyRow, nowArea;
    std::vector<juce::Rectangle<int>> separators;
    int rowH = 62;
    int boundSet = -1, boundBand = -1, shownShape = -1;
    bool boundLinked = false, shownDigital = true, shownOn = true, shownDynamic = true;
    juce::String shownNow, shownLatency;
    juce::Colour shownNowColour = Colours::textFaint;
};

//==============================================================================
DynamicEqPanel::DynamicEqPanel (PureSheenProcessor& p, EditorServices& s) : ModulePanel (p, s, ps::ModuleType::DynamicEq)
{
    impl = std::make_unique<Impl> (*this, p, s);
    neverTakeKeyboardFocus (*this);

    if (! getLocalBounds().isEmpty())
        layoutContent (getContentArea());
}

DynamicEqPanel::~DynamicEqPanel() = default;

int DynamicEqPanel::getSelectedBand() const noexcept { return impl->display.getSelectedBand(); }

void DynamicEqPanel::setSelectedBand (int band) { impl->selectBand (juce::jlimit (0, ps::kDynEqBands - 1, band)); }

void DynamicEqPanel::tick()
{
    ModulePanel::tick();
    impl->tick();
}

void DynamicEqPanel::frameTick() { impl->display.repaint(); }

void DynamicEqPanel::refresh()
{
    ModulePanel::refresh();
    impl->model.update();
    impl->updateStrip (true);
    impl->display.parametersChanged();
}

void DynamicEqPanel::channelSetChanged()
{
    impl->model.update();
    impl->updateStrip (true);
    impl->display.parametersChanged();
}

void DynamicEqPanel::paint (juce::Graphics& g)
{
    ModulePanel::paint (g);

    if (impl != nullptr)
        impl->paint (g);
}

void DynamicEqPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    if (impl == nullptr)
        return;

    impl->contentArea = contentArea;
    impl->layout();
}
} // namespace ps::ui
