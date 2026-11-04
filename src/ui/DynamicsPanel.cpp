#include "ui/DynamicsPanel.h"

#include "ui/Controls.h"
#include "ui/DynamicsViews.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/MultibandDisplay.h"

#include "dsp/Dynamics.h"

#include <cmath>

// Dynamics module panel. Owner: ui-dynamics. Layout and behaviour: see DynamicsPanel.h.
//
// Live values: ps::DynamicsMeters (detector level, total / compressor / limiter gain per channel set and band) are
// read at 30 Hz and smoothed for the eye (levels rise at once and fall 30 dB/s, gains glide over ~2 ticks), so the
// overlay, the gain meters and the transfer-curve dot agree with each other.
namespace ps::ui
{
namespace
{
constexpr float kLevelMinDb = -66.0f, kLevelMaxDb = 6.0f;   // display scale (thresholds are -60 .. 0)
constexpr float kLineGrab = 6.0f;                          // +- px around a threshold line

bool isSplitMode (const PureSheenProcessor& p)
{
    const auto modes = ps::supportedChannelModes (ps::ModuleType::Dynamics);

    if (modes.size() < 2)
        return false;

    const int index = juce::jlimit (0, (int) modes.size() - 1, juce::roundToInt (p.getParameterValue ("dyn.chMode")));
    const auto m = modes[(size_t) index];
    return m == ps::ChannelMode::MidSide || m == ps::ChannelMode::LeftRight;
}

juce::String channelSetLabel (const PureSheenProcessor& p, int set)
{
    const auto modes = ps::supportedChannelModes (ps::ModuleType::Dynamics);
    const int index = juce::jlimit (0, juce::jmax (0, (int) modes.size() - 1), juce::roundToInt (p.getParameterValue ("dyn.chMode")));
    const bool ms = ! modes.empty() && modes[(size_t) index] == ps::ChannelMode::MidSide;
    return ms ? (set == 0 ? "Mid" : "Side") : (set == 0 ? "Left" : "Right");
}

juce::String hzText (float hz)
{
    if (hz < 999.5f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    const float k = hz / 1000.0f;
    return (k < 9.95f ? juce::String (k, 1) : juce::String (juce::roundToInt (k))) + " kHz";
}

juce::String dbShort (float db)
{
    return (db > 0.05f ? "+" : "") + juce::String (db, 1);
}

float safeDb (float v) { return std::isfinite (v) ? juce::jlimit (-200.0f, 60.0f, v) : -200.0f; }

//==============================================================================
// Rounded card surface (the panel's control groups): title row, optional band badge / subtitle, section titles.
class Card final : public juce::Component
{
public:
    Card()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    struct Text
    {
        juce::String text;
        juce::Rectangle<int> area;
        juce::Colour colour;
        bool sectionTitle = false;     // uppercase, letter-spaced, with a rule to the right
        juce::Justification justification = juce::Justification::centredLeft;
    };

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                           Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (Colours::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

        for (const auto& s : separators)
        {
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.fillRect (s);
        }

        for (const auto& t : texts)
        {
            if (t.sectionTitle)
            {
                const auto f = font (10.5f, 700).withExtraKerningFactor (0.08f);
                const auto upper = t.text.toUpperCase();
                g.setFont (f);
                g.setColour (t.colour);
                g.drawText (upper, t.area, juce::Justification::centredLeft, true);
            }
            else
            {
                g.setFont (font (11.0f, 600));
                g.setColour (t.colour);
                g.drawFittedText (t.text, t.area, t.justification, 1, 0.8f);
            }
        }

        if (badge > 0)
        {
            const auto r = badgeArea.toFloat();
            g.setColour (badgeColour.withAlpha (0.22f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (badgeColour.withAlpha (0.8f));
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
            g.setFont (font (11.0f, 700));
            g.setColour (badgeColour);
            g.drawText (juce::String (badge), r, juce::Justification::centred, false);
        }
    }

    std::vector<Text> texts;
    std::vector<juce::Rectangle<int>> separators;
    int badge = 0;
    juce::Rectangle<int> badgeArea;
    juce::Colour badgeColour;
};
} // namespace

//==============================================================================
struct DynamicsPanel::Impl
{
    struct BandControl
    {
        juce::Component* component = nullptr;
        juce::String name;     // "comp.thr" ...
    };

    Impl (DynamicsPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), services (s),
          display (p, s, ps::ModuleType::Dynamics), graph (p), overlayGesture (p)
    {
        colour = moduleColour (ps::ModuleType::Dynamics);
        dynamics = &static_cast<ps::Dynamics&> (processor.getChain().getModule (ps::ModuleType::Dynamics));

        for (auto& set : detUi)
            for (auto& v : set)
                v = -200.0f;

        display.setLevelRange (kLevelMinDb, kLevelMaxDb);
        display.paintBandOverlay = [this] (juce::Graphics& g, int band, juce::Rectangle<float> r) { paintOverlay (g, band, r); };
        display.onBandSelected = [this] (int band) { selectBand (band); };
        display.onBandMouseDown = [this] (int band, const juce::MouseEvent& e) { return lineMouseDown (band, e); };
        display.onBandMouseDrag = [this] (int band, const juce::MouseEvent& e) { lineMouseDrag (band, e); };
        display.onBandMouseUp = [this] (int, const juce::MouseEvent&) { lineMouseUp(); };
        display.onBandMouseMove = [this] (int band, const juce::MouseEvent& e) { lineHover (band, e); };
        display.getBandCursor = [this] (int band, juce::Point<float> pos)
        {
            return lineAt (band, pos.y) >= 0 ? juce::MouseCursor (juce::MouseCursor::UpDownResizeCursor)
                                             : juce::MouseCursor (juce::MouseCursor::NormalCursor);
        };
        display.onBandDoubleClick = [this] (int band, const juce::MouseEvent& e) { return lineDoubleClick (band, e); };
        display.onBandPopupMenu = [this] (int band, const juce::MouseEvent& e) { return lineMenu (band, e); };

        meters.onSelect = [this] (int band)
        {
            display.setSelectedBand (band);
            selectBand (band);
        };

        owner.addAndMakeVisible (display);
        owner.addAndMakeVisible (meters);
        owner.addAndMakeVisible (graph);
        owner.addAndMakeVisible (bandCard);
        owner.addAndMakeVisible (settingsCard);

        buildBandControls();
        buildSettings();
        rebind();
    }

    ~Impl()
    {
        overlayGesture.end();

        // Monitoring requests are not saved and are invisible once the panel is gone (another module selected, editor
        // closed): band solo and "Hear" end with the panel, so nobody is left listening to one band by accident.
        auto m = processor.getMonitor (ps::ModuleType::Dynamics);

        if (m.soloDetectionFilter || m.soloBand >= 0)
        {
            m.soloDetectionFilter = false;
            m.soloBand = -1;
            processor.setMonitor (ps::ModuleType::Dynamics, m);
        }
    }

    //==============================================================================
    // Controls
    template <typename T, typename... Args>
    T& make (std::vector<std::unique_ptr<juce::Component>>& store, juce::Component& parent, Args&&... args)
    {
        auto c = std::make_unique<T> (std::forward<Args> (args)...);
        auto& ref = *c;
        parent.addAndMakeVisible (ref);
        store.push_back (std::move (c));
        return ref;
    }

    ParamKnob& bandKnob (const juce::String& name, const juce::String& caption, const juce::String& tip)
    {
        auto& k = make<ParamKnob> (owned, bandCard, processor, dynamicsBandParamId (0, 0, name), caption);
        k.setTooltip (tip);
        bandControls.push_back ({ &k, name });
        return k;
    }

    ParamToggle& bandToggle (const juce::String& name, const juce::String& label, const juce::String& tip)
    {
        auto& t = make<ParamToggle> (owned, bandCard, processor, dynamicsBandParamId (0, 0, name), label);
        t.setTooltip (tip);
        bandControls.push_back ({ &t, name });
        return t;
    }

    void buildBandControls()
    {
        compOn = &bandToggle ("comp.on", "Compressor", "Compressor on / off for this band");
        compThr = &bandKnob ("comp.thr", "Threshold", "Compressor threshold: the level above which the band is turned down "
                                                      "(below which it is raised, with a negative ratio)");
        compRatio = &bandKnob ("comp.ratio", "Ratio", "Compressor ratio. Positive: downward compression. Negative: upward "
                                                      "compression (quiet parts are raised toward the threshold)");
        compAtt = &bandKnob ("comp.att", "Attack", "How fast the compressor reacts when the level rises");
        compRel = &bandKnob ("comp.rel", "Release", "How fast the compressor lets go when the level falls");
        compKnee = &bandKnob ("comp.knee", "Knee", "Soft knee width: how gradually the compression starts around the threshold");

        limOn = &bandToggle ("lim.on", "Limiter", "Limiter on / off for this band (after the compressor)");
        limThr = &bandKnob ("lim.thr", "Threshold", "Limiter threshold, on the compressor's output level");
        limRatio = &bandKnob ("lim.ratio", "Ratio", "Limiter ratio. Positive: limiting. Negative: upward expansion above "
                                                    "the threshold (more punch)");
        limAtt = &bandKnob ("lim.att", "Attack", "How fast the limiter reacts");
        limRel = &bandKnob ("lim.rel", "Release", "How fast the limiter lets go");
        limKnee = &bandKnob ("lim.knee", "Knee", "Limiter soft knee width");

        mix = &bandKnob ("mix", "Mix", "Parallel mix: how much of the processed band is heard (the rest is the dry band)");
        gain = &bandKnob ("gain", "Gain", "Output gain of this band");
        bandOn = &bandToggle ("on", "Band on", "Band on / off (off: the band passes unprocessed)");
    }

    void buildSettings()
    {
        auto choice = [this] (const juce::String& id, const juce::String& label, const juce::String& tip) -> ParamChoice&
        {
            auto& c = make<ParamChoice> (owned, settingsCard, processor, id, label);
            c.setTooltip (tip);
            return c;
        };
        auto knob = [this] (const juce::String& id, const juce::String& caption, const juce::String& tip) -> ParamKnob&
        {
            auto& k = make<ParamKnob> (owned, settingsCard, processor, id, caption);
            k.setColour (colour);
            k.setTooltip (tip);
            return k;
        };
        auto toggle = [this] (const juce::String& id, const juce::String& label, const juce::String& tip) -> ParamToggle&
        {
            auto& t = make<ParamToggle> (owned, settingsCard, processor, id, label);
            t.setOnColour (colour);
            t.setTooltip (tip);
            return t;
        };

        detect = &choice ("dyn.detect", "Detection", "How the level is measured: Peak (fastest), Envelope (smooth and "
                                                     "fast) or RMS (average loudness)");
        detFilter = &choice ("dyn.detFilter", "Filter", "Detection filter: shapes what the compressors listen to (not what "
                                                        "you hear). High Pass ignores the lows, Tilt leans the detector");
        detSlope = &choice ("dyn.detSlope", "Slope", "Steepness of the detection high pass");
        detFreq = &knob ("dyn.detFreq", "Freq", "Detection filter frequency (high-pass corner or tilt centre)");
        detTilt = &knob ("dyn.detTilt", "Tilt", "Detection tilt in dB per octave around the frequency");
        xoverType = &choice ("dyn.xoverType", "Crossover", "Crossover filters: Analog (no latency), Digital (linear phase, "
                                                           "adds latency) or Hybrid (steeper, no latency)");
        lookahead = &knob ("dyn.lookahead", "Lookahead", "Lookahead: the audio is delayed so the detector reacts before "
                                                         "peaks arrive (adds latency)");
        outGain = &knob ("dyn.gain", "Output", "Output gain of the whole module");
        adaptRel = &toggle ("dyn.adaptRel", "Adaptive release", "Adaptive release: shorter release on transients, longer on "
                                                                "sustained material");
        autoGain = &toggle ("dyn.autoGain", "Auto gain", "Auto gain: makes up the average level each band loses");

        hear.setText ("Hear");
        hear.setOnColour (Colours::minor);
        hear.setTooltip ("Hear the detector: plays the signal the compressors listen to (after the detection filter)");
        hear.onClick = [this]
        {
            auto m = processor.getMonitor (ps::ModuleType::Dynamics);
            m.soloDetectionFilter = ! m.soloDetectionFilter;
            processor.setMonitor (ps::ModuleType::Dynamics, m);
            hear.setOn (m.soloDetectionFilter);
        };
        settingsCard.addAndMakeVisible (hear);
    }

    //==============================================================================
    int activeSet() const { return split ? owner.getChannelSet() : 0; }
    int linkedSet() const { return split && linked ? 1 - owner.getChannelSet() : -1; }

    juce::Colour setColour() const { return split && owner.getChannelSet() == 1 ? Colours::sideChannel : colour; }

    void rebind()
    {
        split = isSplitMode (processor);
        linked = processor.getParameterValue ("dyn.link") >= 0.5f;
        const int set = activeSet();
        const int other = linkedSet();
        const int band = display.getSelectedBand();
        const auto c = setColour();

        for (auto& bc : bandControls)
        {
            const auto id = dynamicsBandParamId (set, band, bc.name);
            const auto linkedId = other >= 0 ? dynamicsBandParamId (other, band, bc.name) : juce::String();

            if (auto* k = dynamic_cast<ParamKnob*> (bc.component))
            {
                k->setParamId (id);
                k->setLinkedParamId (linkedId);
                k->setColour (bc.name.startsWith ("lim.") ? Colours::major : c);
            }
            else if (auto* t = dynamic_cast<ParamToggle*> (bc.component))
            {
                t->setParamId (id);
                t->setLinkedParamId (linkedId);
                t->setOnColour (bc.name.startsWith ("lim.") ? Colours::major : c);
            }
        }

        // Module-wide settings are not per channel set (no link needed).
        display.setAccentColour (c);
        graph.setTarget (set, band, other, c, split ? "(" + channelSetLabel (processor, set) + ")" : juce::String());
        if (owner.getChannelSet() != shownSet || split != shownSplit)
            meters.resetHolds();

        bandCard.badge = band + 1;
        bandCard.badgeColour = c;
        shownSet = owner.getChannelSet();
        shownSplit = split;
        shownLinked = linked;
        shownBand = band;
        overlayGesture.end();
        dragLine = -1;
        updateBandTexts();
        updateEnablement();
        bandCard.repaint();
        display.repaint();
    }

    void selectBand (int band)
    {
        if (band != shownBand)
            rebind();
    }

    void updateEnablement()
    {
        const int filter = juce::roundToInt (processor.getParameterValue ("dyn.detFilter"));
        auto enable = [] (juce::Component* c, bool on)
        {
            if (c->isEnabled() != on)
            {
                c->setEnabled (on);
                c->setAlpha (on ? 1.0f : 0.45f);
            }
        };

        enable (detSlope, filter == (int) ps::DetectionFilter::HighPass);
        enable (detTilt, filter == (int) ps::DetectionFilter::Tilt);
        enable (detFreq, filter != (int) ps::DetectionFilter::None);
        shownFilter = filter;
    }

    // Band card title texts and the live stage readouts (re-laid out only when the text changes).
    void updateBandTexts()
    {
        const int band = display.getSelectedBand();
        const int n = display.getNumBands();
        const float lo = band == 0 ? 20.0f : processor.getParameterValue ("dyn.xover" + juce::String (band));
        const float hi = band >= n - 1 ? 20000.0f : processor.getParameterValue ("dyn.xover" + juce::String (band + 1));
        bandRange = n > 1 ? hzText (lo) + " - " + hzText (hi) : juce::String ("Full range");

        if (split)
            bandRange << "   " << channelSetLabel (processor, activeSet());

        layoutBandTexts();
    }

    //==============================================================================
    // Live values
    void readMeters()
    {
        const auto& m = dynamics->getMeters();

        for (int s = 0; s < ps::kNumChannelSets; ++s)
        {
            for (int b = 0; b < ps::kMaxBands; ++b)
            {
                const float det = safeDb (m.detectorDb[s][b].load());
                auto& d = detUi[s][b];
                d = det >= d ? det : juce::jmax (det, d - 1.0f);

                auto glide = [] (float& v, float target) { v += (juce::jlimit (-60.0f, 30.0f, target) - v) * 0.55f; };
                glide (grUi[s][b], safeDb (m.gainReductionDb[s][b].load()));
                glide (compUi[s][b], safeDb (m.compGainDb[s][b].load()));
                glide (limUi[s][b], safeDb (m.limGainDb[s][b].load()));
            }
        }
    }

    //==============================================================================
    // Overlay in the band display: threshold lines (C / L), the detector level and the gain change.
    float lineDb (int band, int line) const
    {
        return processor.getParameterValue (dynamicsBandParamId (activeSet(), band, line == 0 ? "comp.thr" : "lim.thr"));
    }

    int lineAt (int band, float y) const
    {
        if (band < 0 || band >= display.getNumBands())
            return -1;

        const auto axis = display.getLevelAxis();
        const float dc = std::abs (axis.dbToY (lineDb (band, 0)) - y);
        const float dl = std::abs (axis.dbToY (lineDb (band, 1)) - y);

        if (dc > kLineGrab && dl > kLineGrab)
            return -1;

        return dl < dc ? 1 : 0;
    }

    void paintOverlay (juce::Graphics& g, int band, juce::Rectangle<float> r)
    {
        const int set = activeSet();
        const auto axis = display.getLevelAxis();
        const auto c = setColour();
        const auto limColour = Colours::major;
        auto v = [&] (const char* name) { return processor.getParameterValue (dynamicsBandParamId (set, band, name)); };
        const bool bandIsOn = v ("on") >= 0.5f;
        const bool compIsOn = v ("comp.on") >= 0.5f;
        const bool limIsOn = v ("lim.on") >= 0.5f;
        const float level = detUi[set][band];
        const float gainDb = grUi[set][band];
        const bool selected = band == display.getSelectedBand();
        const float inset = juce::jmin (6.0f, r.getWidth() * 0.05f);
        const auto inner = r.reduced (inset, 0.0f);

        // Detector level: soft column up to the level, with a bright edge.
        if (level > kLevelMinDb && bandIsOn)
        {
            const float y = axis.dbToY (juce::jmin (level, kLevelMaxDb));
            g.setGradientFill (juce::ColourGradient::vertical (juce::Colours::white.withAlpha (0.075f), y,
                                                               juce::Colours::white.withAlpha (0.0f), r.getBottom()));
            g.fillRect (juce::Rectangle<float> (inner.getX(), y, inner.getWidth(), r.getBottom() - y));
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (inner.getX(), y - 0.75f, inner.getWidth(), 1.5f));

            // Gain change: from the detector level to where the band is pushed (same dB scale).
            if (std::abs (gainDb) > 0.05f)
            {
                const float y2 = axis.dbToY (juce::jlimit (kLevelMinDb, kLevelMaxDb, level + gainDb));
                const auto gc = gainDb < 0.0f ? Colours::gainReduction : Colours::good;
                const auto block = juce::Rectangle<float> (inner.getX(), juce::jmin (y, y2), inner.getWidth(), std::abs (y2 - y));
                g.setColour (gc.withAlpha (0.24f));
                g.fillRect (block);
                g.setColour (gc.withAlpha (0.9f));
                g.fillRect (juce::Rectangle<float> (inner.getX(), y2 - 0.75f, inner.getWidth(), 1.5f));
            }
        }

        // Threshold lines.
        auto line = [&] (int index, float db, juce::Colour lc, bool on, const juce::String& letter, bool labelRight)
        {
            const float y = std::round (axis.dbToY (db));
            const bool active = (dragLine == index && dragBand == band) || (dragLine < 0 && hoverLine == index && hoverBand == band);
            const float alpha = bandIsOn ? (on ? 1.0f : 0.45f) : 0.3f;

            if (active)
            {
                g.setColour (lc.withAlpha (0.16f));
                g.fillRect (juce::Rectangle<float> (r.getX(), y - 5.0f, r.getWidth(), 10.0f));
            }

            if (on)
            {
                g.setColour (lc.withAlpha (alpha * (active ? 1.0f : 0.9f)));
                g.fillRect (juce::Rectangle<float> (r.getX(), y - (active ? 1.0f : 0.75f), r.getWidth(), active ? 2.0f : 1.5f));
            }
            else
            {
                const float dashes[] = { 5.0f, 4.0f };
                g.setColour (lc.withAlpha (alpha * 0.8f));
                g.drawDashedLine (juce::Line<float> (r.getX(), y, r.getRight(), y), dashes, 2, active ? 1.6f : 1.1f);
            }

            // Grab dot in the middle of the line.
            const auto dot = juce::Rectangle<float> (active ? 22.0f : 16.0f, active ? 7.0f : 5.0f).withCentre ({ r.getCentreX(), y });
            g.setColour (on ? lc.withAlpha (alpha) : Colours::background);
            g.fillRoundedRectangle (dot, dot.getHeight() * 0.5f);

            if (! on)
            {
                g.setColour (lc.withAlpha (alpha));
                g.drawRoundedRectangle (dot, dot.getHeight() * 0.5f, 1.0f);
            }

            // Label chip "C -18.0" (a stage that is off shows it only while hovered / dragged).
            if (r.getWidth() >= 64.0f && (on || active))
            {
                const auto f = font (10.5f, 700);
                const auto text = letter + "  " + dbShort (db);
                const float w = textWidth (f, text) + 12.0f;

                if (w + 8.0f < r.getWidth() * 0.5f || r.getWidth() >= 120.0f)
                {
                    const float x = labelRight ? r.getRight() - 5.0f - w : r.getX() + 5.0f;
                    auto chip = juce::Rectangle<float> (x, y - 17.0f, w, 15.0f);

                    if (chip.getY() < r.getY() + 3.0f)
                        chip.setY (y + 3.0f);

                    g.setColour (Colours::background.withAlpha (0.85f));
                    g.fillRoundedRectangle (chip, 4.0f);
                    g.setColour (lc.withAlpha (alpha * (active ? 1.0f : 0.6f)));
                    g.drawRoundedRectangle (chip.reduced (0.5f), 4.0f, 1.0f);
                    g.setFont (f);
                    g.setColour (on ? lc.brighter (0.2f).withAlpha (alpha) : Colours::textDim.withAlpha (alpha));
                    g.drawText (text, chip, juce::Justification::centred, false);
                }
            }
        };

        line (1, lineDb (band, 1), limColour, limIsOn, "L", true);
        line (0, lineDb (band, 0), c, compIsOn, "C", false);

        // Gain readout at the end of the gain block (below it for a reduction, above it for upward gain).
        if (bandIsOn && level > kLevelMinDb && std::abs (gainDb) > 0.05f && r.getWidth() >= 54.0f)
        {
            const auto f = font (11.0f, 700);
            const auto text = dbShort (gainDb) + " dB";
            const float w = textWidth (f, text) + 10.0f;
            const float y2 = axis.dbToY (juce::jlimit (kLevelMinDb, kLevelMaxDb, level + gainDb));
            auto chip = juce::Rectangle<float> (inner.getRight() - w - 2.0f, gainDb < 0.0f ? y2 + 3.0f : y2 - 18.0f, w, 15.0f);
            chip = chip.constrainedWithin (r.reduced (2.0f));
            const auto gc = gainDb < 0.0f ? Colours::gainReduction : Colours::good;
            g.setColour (Colours::background.withAlpha (0.8f));
            g.fillRoundedRectangle (chip, 4.0f);
            g.setFont (f);
            g.setColour (gc.withAlpha (selected ? 1.0f : 0.85f));
            g.drawText (text, chip, juce::Justification::centred, false);
        }

        if (! bandIsOn)
        {
            g.setColour (Colours::background.withAlpha (0.45f));
            g.fillRect (r);
            g.setFont (font (11.5f, 700).withExtraKerningFactor (0.06f));
            g.setColour (Colours::textSubtle);
            g.drawText ("BAND OFF", r.withTrimmedTop (r.getHeight() * 0.18f).withHeight (16.0f), juce::Justification::centred, false);
        }
    }

    bool lineMouseDown (int band, const juce::MouseEvent& e)
    {
        const int line = lineAt (band, e.position.y);

        if (line < 0)
            return false;

        if (band != display.getSelectedBand())
        {
            display.setSelectedBand (band);
            selectBand (band);
        }

        const auto name = line == 0 ? "comp.thr" : "lim.thr";
        const int other = linkedSet();
        overlayGesture.start (dynamicsBandParamId (activeSet(), band, name),
                              other >= 0 ? dynamicsBandParamId (other, band, name) : juce::String());
        dragLine = line;
        dragBand = band;
        dragDb = lineDb (band, line);
        lastY = e.position.y;
        display.repaint();
        return true;
    }

    void lineMouseDrag (int band, const juce::MouseEvent& e)
    {
        if (dragLine < 0 || band != dragBand)
            return;

        const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
        const float dy = e.position.y - lastY;
        lastY = e.position.y;

        if (std::abs (dy) < 1.0e-4f)
            return;

        const auto axis = display.getLevelAxis();
        const float dbPerPx = (axis.maxDb - axis.minDb) / juce::jmax (1.0f, axis.area.getHeight());
        dragDb = juce::jlimit (-60.0f, 0.0f, dragDb - dy * dbPerPx * (fine ? 0.1f : 1.0f));
        overlayGesture.set (dragDb);
        display.repaint();
        graph.repaint();
    }

    void lineMouseUp()
    {
        overlayGesture.end();
        dragLine = -1;
        display.repaint();
    }

    void lineHover (int band, const juce::MouseEvent& e)
    {
        const int line = band >= 0 ? lineAt (band, e.position.y) : -1;

        if (line != hoverLine || band != hoverBand)
        {
            hoverLine = line;
            hoverBand = band;
            display.repaint();
        }
    }

    bool lineDoubleClick (int band, const juce::MouseEvent& e)
    {
        const int line = lineAt (band, e.position.y);

        if (line < 0)
            return false;

        overlayGesture.end();
        dragLine = -1;
        const auto name = line == 0 ? "comp.thr" : "lim.thr";
        const auto id = dynamicsBandParamId (activeSet(), band, name);
        const int other = linkedSet();
        stereo::promptParamValue (display, processor, id, other >= 0 ? dynamicsBandParamId (other, band, name) : juce::String(),
                                  "Band " + juce::String (band + 1) + (line == 0 ? " compressor threshold" : " limiter threshold"));
        return true;
    }

    bool lineMenu (int band, const juce::MouseEvent& e)
    {
        const int line = lineAt (band, e.position.y);

        if (line < 0)
            return false;

        const int set = activeSet(), other = linkedSet();
        const auto thrName = line == 0 ? "comp.thr" : "lim.thr";
        const auto onName = line == 0 ? "comp.on" : "lim.on";
        const auto id = dynamicsBandParamId (set, band, thrName);
        const auto linkedId = other >= 0 ? dynamicsBandParamId (other, band, thrName) : juce::String();
        const auto onId = dynamicsBandParamId (set, band, onName);
        const auto onLinked = other >= 0 ? dynamicsBandParamId (other, band, onName) : juce::String();
        const bool on = processor.getParameterValue (onId) >= 0.5f;

        juce::PopupMenu menu;
        menu.addSectionHeader ("Band " + juce::String (band + 1) + (line == 0 ? " compressor threshold" : " limiter threshold"));
        menu.addItem (1, "Reset to default (" + dbShort (ParamGesture::defaultOf (id)) + " dB)");
        menu.addItem (3, "Enter value...");
        menu.addItem (2, line == 0 ? "Compressor on" : "Limiter on", true, on);

        juce::Component::SafePointer<DynamicsPanel> safe (&owner);
        auto& p = processor;
        const auto title = "Band " + juce::String (band + 1) + (line == 0 ? " compressor threshold" : " limiter threshold");
        showMenu (menu, display, [safe, &p, id, linkedId, onId, onLinked, on, title] (int result)
        {
            if (safe == nullptr)
                return;

            if (result == 1)
                ParamGesture::setComplete (p, id, linkedId, ParamGesture::defaultOf (id));
            else if (result == 2)
                ParamGesture::setComplete (p, onId, onLinked, on ? 0.0f : 1.0f);
            else if (result == 3)
                stereo::promptParamValue (*safe, p, id, linkedId, title);
        });
        return true;
    }

    //==============================================================================
    // Layout
    void layout (juce::Rectangle<int> content)
    {
        contentArea = content;
        auto area = content.reduced (12, 10);
        const int w = area.getWidth(), h = area.getHeight();

        knobH = h >= 640 ? 84 : (h >= 470 ? 78 : 66);
        const int bandNeed = 34 + 2 * knobH + 8 + 10;
        const int settingsNeed = 34 + 3 * 30 + knobH + 10;
        const int bottomH = juce::jmax (bandNeed, settingsNeed);
        const int topH = juce::jmax (120, h - bottomH - 10);

        auto top = area.removeFromTop (topH);
        area.removeFromTop (10);
        auto bottom = area;

        // Top row: display | gain meters | transfer curve (meters under the curve when there is height to spare).
        const int graphW = juce::jlimit (150, juce::jmax (150, (int) ((float) w * 0.3f)), topH + 12);
        const int metersW = juce::jlimit (108, 150, (int) ((float) w * 0.12f));
        auto right = top.removeFromRight (graphW);
        top.removeFromRight (10);

        if (right.getHeight() - graphW >= 150)
        {
            graph.setBounds (right.removeFromTop (graphW));
            right.removeFromTop (10);
            meters.setBounds (right);
        }
        else
        {
            graph.setBounds (right);
            meters.setBounds (top.removeFromRight (metersW));
            top.removeFromRight (10);
        }

        display.setBounds (top);

        // Bottom row: band card | settings card.
        int settingsW = juce::jlimit (330, 560, (int) ((float) w * 0.36f));
        int bandW = w - settingsW - 10;
        const int bandIdeal = 24 + 92 + 12 + 6 * 96;

        if (bandW > bandIdeal)
        {
            const int extra = bandW - bandIdeal;
            bandW = bandIdeal + extra * 2 / 5;
            settingsW = w - bandW - 10;
        }

        bandCard.setBounds (bottom.removeFromLeft (bandW));
        bottom.removeFromLeft (10);
        settingsCard.setBounds (bottom);

        layoutBandCard();
        layoutSettingsCard();
    }

    void layoutBandCard()
    {
        auto r = bandCard.getLocalBounds().reduced (12, 0);
        auto title = r.removeFromTop (34);
        bandOn->setBounds (title.removeFromRight (92).withSizeKeepingCentre (92, 22).translated (0, 1));
        bandTitleArea = title;
        r.removeFromBottom (10);

        const bool compact = knobH < 72;
        const int rowH = (r.getHeight() - 8) / 2;
        const int labelW = 92;
        const int cellW = juce::jmin (96, (r.getWidth() - labelW - 12) / 6);

        ParamKnob* rows[2][5] = { { compThr, compRatio, compAtt, compRel, compKnee }, { limThr, limRatio, limAtt, limRel, limKnee } };
        ParamToggle* toggles[2] = { compOn, limOn };
        ParamKnob* side[2] = { mix, gain };
        bandCard.separators.clear();

        for (int row = 0; row < 2; ++row)
        {
            auto rr = juce::Rectangle<int> (r.getX(), r.getY() + row * (rowH + 8), r.getWidth(), rowH);
            const int kh = juce::jmin (knobH, rowH);
            const int ky = rr.getY() + (rowH - kh) / 2;
            auto label = rr.removeFromLeft (labelW);
            toggles[row]->setBounds (label.getX(), ky + kh / 2 - 15, labelW - 8, 22);
            stageReadout[row] = juce::Rectangle<int> (label.getX() + 4, ky + kh / 2 + 10, labelW - 16, 14);

            for (int k = 0; k < 5; ++k)
            {
                rows[row][k]->setCompact (compact);
                rows[row][k]->setBounds (rr.getX() + k * cellW, ky, cellW, kh);
            }

            const int sx = rr.getX() + 5 * cellW + 6;
            side[row]->setCompact (compact);
            side[row]->setBounds (sx + 6, ky, cellW, kh);

            if (row == 0)
                bandCard.separators.push_back ({ sx, r.getY() + 4, 1, r.getHeight() - 8 });
        }

        bandCard.separators.push_back ({ r.getX(), r.getY() + rowH + 3, 5 * cellW, 1 });
        layoutBandTexts();
    }

    void layoutBandTexts()
    {
        bandCard.texts.clear();
        auto t = bandTitleArea;
        bandCard.badgeArea = t.removeFromLeft (22).withSizeKeepingCentre (22, 18).translated (0, 1);
        t.removeFromLeft (8);
        const auto f = font (10.5f, 700).withExtraKerningFactor (0.08f);
        const juce::String name = "BAND " + juce::String (display.getSelectedBand() + 1);
        const int nameW = (int) textWidth (f, name) + 6;
        bandCard.texts.push_back ({ "Band " + juce::String (display.getSelectedBand() + 1), t.removeFromLeft (nameW).translated (0, 1),
                                    Colours::text.withAlpha (0.92f), true });
        t.removeFromLeft (8);
        bandCard.texts.push_back ({ bandRange, t.translated (0, 1), Colours::textSubtle, false });

        const int set = activeSet();
        const int band = display.getSelectedBand();
        const float values[2] = { compUi[set][band], limUi[set][band] };
        const juce::Colour colours[2] = { Colours::gainReduction, Colours::major };

        for (int row = 0; row < 2; ++row)
        {
            const bool active = std::abs (values[row]) > 0.05f;
            stageText[row] = active ? dbShort (values[row]) + " dB" : juce::String ("0.0 dB");
            const auto readoutColour = ! active ? Colours::textFaint : (values[row] > 0.0f ? Colours::good : colours[row]);
            bandCard.texts.push_back ({ stageText[row], stageReadout[row], readoutColour, false,
                                        juce::Justification::centred });
        }

        bandCard.repaint();
    }

    void layoutSettingsCard()
    {
        auto r = settingsCard.getLocalBounds().reduced (12, 0);
        r.removeFromBottom (10);
        settingsCard.texts.clear();
        settingsCard.separators.clear();

        const int colGap = 16;
        const int colW = (r.getWidth() - colGap) / 2;
        auto colA = r.removeFromLeft (colW);
        r.removeFromLeft (colGap);
        auto colB = r;
        settingsCard.separators.push_back ({ colA.getRight() + colGap / 2, 12, 1, settingsCard.getHeight() - 24 });

        const bool compact = knobH < 72;
        const int rowH = 24, rowGap = 6;

        // Detection
        {
            auto title = colA.removeFromTop (34);
            hear.setBounds (title.removeFromRight (58).withSizeKeepingCentre (58, 22).translated (0, 1));
            settingsCard.texts.push_back ({ "Detection", title.translated (0, 1), Colours::text.withAlpha (0.92f), true });

            for (auto* c : { (juce::Component*) detect, (juce::Component*) detFilter, (juce::Component*) detSlope })
            {
                c->setBounds (colA.removeFromTop (rowH));
                colA.removeFromTop (rowGap);
            }

            const int kh = juce::jmin (knobH, colA.getHeight());
            const int kw = juce::jmin (96, colA.getWidth() / 2);
            detFreq->setCompact (compact);
            detTilt->setCompact (compact);
            detFreq->setBounds (colA.getX(), colA.getY(), kw, kh);
            detTilt->setBounds (colA.getX() + kw, colA.getY(), kw, kh);
        }

        // Settings
        {
            auto title = colB.removeFromTop (34);
            settingsCard.texts.push_back ({ "Settings", title.translated (0, 1), Colours::text.withAlpha (0.92f), true });
            xoverType->setBounds (colB.removeFromTop (rowH));
            colB.removeFromTop (rowGap);

            auto toggles = colB.removeFromBottom (2 * rowH + rowGap);
            autoGain->setBounds (toggles.removeFromBottom (rowH));
            toggles.removeFromBottom (rowGap);
            adaptRel->setBounds (toggles.removeFromBottom (rowH));
            colB.removeFromBottom (rowGap);

            const int kh = juce::jmin (knobH, colB.getHeight());
            const int kw = juce::jmin (96, colB.getWidth() / 2);
            lookahead->setCompact (compact || kh < 72);
            outGain->setCompact (compact || kh < 72);
            lookahead->setBounds (colB.getX(), colB.getY() + (colB.getHeight() - kh) / 2, kw, kh);
            outGain->setBounds (colB.getX() + kw, colB.getY() + (colB.getHeight() - kh) / 2, kw, kh);
        }

        settingsCard.repaint();
    }

    //==============================================================================
    void tick()
    {
        display.tick();
        readMeters();

        const bool nowSplit = isSplitMode (processor);
        const bool nowLinked = processor.getParameterValue ("dyn.link") >= 0.5f;

        if (nowSplit != shownSplit || nowLinked != shownLinked || owner.getChannelSet() != shownSet
            || display.getSelectedBand() != shownBand)
            rebind();

        if (juce::roundToInt (processor.getParameterValue ("dyn.detFilter")) != shownFilter)
            updateEnablement();

        // Band range / stage readouts
        {
            const juce::String before = bandRange, s0 = stageText[0], s1 = stageText[1];
            const int band = display.getSelectedBand();
            const int n = display.getNumBands();
            const float lo = band == 0 ? 20.0f : processor.getParameterValue ("dyn.xover" + juce::String (band));
            const float hi = band >= n - 1 ? 20000.0f : processor.getParameterValue ("dyn.xover" + juce::String (band + 1));
            juce::String range = n > 1 ? hzText (lo) + " - " + hzText (hi) : juce::String ("Full range");

            if (split)
                range << "   " << channelSetLabel (processor, activeSet());

            const int set = activeSet();
            auto text = [] (float v) { return std::abs (v) > 0.05f ? dbShort (v) + " dB" : juce::String ("0.0 dB"); };

            if (range != before || text (compUi[set][band]) != s0 || text (limUi[set][band]) != s1)
            {
                bandRange = range;
                layoutBandTexts();
            }
        }

        hear.setOn (processor.getMonitor (ps::ModuleType::Dynamics).soloDetectionFilter);

        const int set = activeSet();
        const int band = display.getSelectedBand();
        const int n = display.getNumBands();
        float gains[ps::kMaxBands] {};

        for (int b = 0; b < ps::kMaxBands; ++b)
            gains[b] = b < n && processor.getParameterValue (dynamicsBandParamId (set, b, "on")) >= 0.5f ? grUi[set][b] : 0.0f;

        meters.setValues (n, gains, band, display.getSoloBand(), setColour());
        graph.setLive (detUi[set][band], grUi[set][band], detUi[set][band] > -100.0f);
    }

    //==============================================================================
    DynamicsPanel& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    ps::Dynamics* dynamics = nullptr;
    juce::Colour colour;

    MultibandDisplay display;
    DynamicsGrMeters meters;
    DynamicsTransferGraph graph;
    Card bandCard, settingsCard;
    std::vector<std::unique_ptr<juce::Component>> owned;
    std::vector<BandControl> bandControls;

    ParamToggle* compOn = nullptr; ParamKnob* compThr = nullptr; ParamKnob* compRatio = nullptr; ParamKnob* compAtt = nullptr;
    ParamKnob* compRel = nullptr; ParamKnob* compKnee = nullptr;
    ParamToggle* limOn = nullptr; ParamKnob* limThr = nullptr; ParamKnob* limRatio = nullptr; ParamKnob* limAtt = nullptr;
    ParamKnob* limRel = nullptr; ParamKnob* limKnee = nullptr;
    ParamKnob* mix = nullptr; ParamKnob* gain = nullptr; ParamToggle* bandOn = nullptr;

    ParamChoice* detect = nullptr; ParamChoice* detFilter = nullptr; ParamChoice* detSlope = nullptr; ParamChoice* xoverType = nullptr;
    ParamKnob* detFreq = nullptr; ParamKnob* detTilt = nullptr; ParamKnob* lookahead = nullptr; ParamKnob* outGain = nullptr;
    ParamToggle* adaptRel = nullptr; ParamToggle* autoGain = nullptr;
    PillButton hear;

    // Live values (smoothed), [channel set][band]
    float detUi[ps::kNumChannelSets][ps::kMaxBands] {};
    float grUi[ps::kNumChannelSets][ps::kMaxBands] {};
    float compUi[ps::kNumChannelSets][ps::kMaxBands] {};
    float limUi[ps::kNumChannelSets][ps::kMaxBands] {};

    // Overlay interaction
    ParamGesture overlayGesture;
    int dragLine = -1, dragBand = -1, hoverLine = -1, hoverBand = -1;
    float dragDb = 0.0f, lastY = 0.0f;

    // Shown state
    bool split = false, linked = true, shownSplit = false, shownLinked = true;
    int shownSet = 0, shownBand = -1, shownFilter = -1;
    int knobH = 78;
    juce::Rectangle<int> contentArea, bandTitleArea, stageReadout[2];
    juce::String bandRange, stageText[2];
};

//==============================================================================
DynamicsPanel::DynamicsPanel (PureSheenProcessor& p, EditorServices& s) : ModulePanel (p, s, ps::ModuleType::Dynamics)
{
    impl = std::make_unique<Impl> (*this, p, s);
    neverTakeKeyboardFocus (*this);

    if (! getLocalBounds().isEmpty())
        impl->layout (getContentArea());
}

DynamicsPanel::~DynamicsPanel() = default;

int DynamicsPanel::getSelectedBand() const noexcept { return impl->display.getSelectedBand(); }

void DynamicsPanel::tick()
{
    ModulePanel::tick();
    impl->tick();
}

void DynamicsPanel::frameTick() { impl->display.repaint(); }

void DynamicsPanel::refresh()
{
    ModulePanel::refresh();
    impl->display.refresh();
    impl->rebind();
    impl->layout (getContentArea());
}

void DynamicsPanel::channelSetChanged()
{
    impl->rebind();
}

void DynamicsPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    if (impl != nullptr)
        impl->layout (contentArea);
}
} // namespace ps::ui
