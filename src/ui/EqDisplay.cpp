#include "ui/EqDisplay.h"

#include "dsp/Equalizer.h"
#include "ui/Controls.h"

#include <cmath>

// Equalizer display (owner: ui-eq). See EqDisplay.h for the mouse map.
//
// Curves: every band of the shown channel set(s) is evaluated with ps::eqBandResponse (the same function the DSP and
// the Digital-mode designer use, with the current filter mode and Amount) on a grid of points spaced evenly in x
// (so the Log / Mel / Extended frequency scales all work). Each band's curve is cached and recomputed only when that
// band, the filter mode, the amount, the sample rate or the grid changes, so dragging one node re-evaluates one band.
namespace ps::ui
{
namespace
{
constexpr float kNodeRadius = 8.5f;
constexpr float kHitRadius = 12.0f;
constexpr float kSpectrumMinDb = -90.0f, kSpectrumMaxDb = 0.0f;
constexpr int kRanges[] { 6, 12, 18, 30 };
constexpr int kWheelGestureMs = 450;

// Display range per EQ, remembered for the session (UI state, not a parameter).
int& rememberedRange (ps::ModuleType t)
{
    static int ranges[2] { 12, 12 };
    return ranges[t == ps::ModuleType::Equalizer2 ? 1 : 0];
}

float freqClamp (float hz) { return juce::jlimit (20.0f, 20000.0f, hz); }

bool sameBand (const ps::EqBandParams& a, const ps::EqBandParams& b) noexcept
{
    return a.on == b.on && a.shape == b.shape && a.freqHz == b.freqHz && a.gainDb == b.gainDb && a.q == b.q
           && a.slope == b.slope && a.phasePercent == b.phasePercent;
}

// Width in octaves of a band-pass of quality q (between its -3 dB points).
float octavesForQ (float q)
{
    const double x = 1.0 / (2.0 * (double) juce::jmax (0.05f, q));
    return (float) (2.0 / std::log (2.0) * std::log (x + std::sqrt (x * x + 1.0)));
}

juce::String qText (float q) { return "Q " + juce::String (q, q < 10.0f ? 2 : 1); }

//==============================================================================
// "6 | 12 | 18 | 30 dB" display range switch (top-right corner of the display).
class RangeSelector final : public juce::Component, public juce::SettableTooltipClient
{
public:
    RangeSelector()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Display range of the curve (+/- dB). Only changes the view, not the sound.");
    }

    int idealWidth() const { return 26 + 4 * 24; }

    juce::Rectangle<float> segment (int i) const
    {
        auto b = getLocalBounds().toFloat().withTrimmedLeft (26.0f).reduced (0.5f);
        const float w = b.getWidth() / 4.0f;
        return { b.getX() + w * (float) i, b.getY(), w, b.getHeight() };
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setFont (font (10.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText ("dB", b.withWidth (24.0f), juce::Justification::centredRight, false);

        const auto track = b.withTrimmedLeft (26.0f).reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillRoundedRectangle (track, 5.0f);
        g.setColour (Colours::outline.withAlpha (0.7f));
        g.drawRoundedRectangle (track.reduced (0.5f), 5.0f, 1.0f);

        const auto mouse = getMouseXYRelative().toFloat();

        for (int i = 0; i < 4; ++i)
        {
            const auto s = segment (i).reduced (2.0f);
            const bool sel = kRanges[i] == range;
            const bool hover = isMouseOver() && segment (i).contains (mouse);

            if (sel)
            {
                g.setColour (colour.withAlpha (0.22f));
                g.fillRoundedRectangle (s, 3.5f);
                g.setColour (colour.withAlpha (0.65f));
                g.drawRoundedRectangle (s.reduced (0.5f), 3.5f, 1.0f);
            }
            else if (hover)
            {
                g.setColour (Colours::panelHover.withAlpha (0.8f));
                g.fillRoundedRectangle (s, 3.5f);
            }

            g.setFont (font (10.5f, sel ? 700 : 600));
            g.setColour (sel ? Colours::text : (hover ? Colours::text.withAlpha (0.85f) : Colours::textDim));
            g.drawText (juce::String (kRanges[i]), s, juce::Justification::centred, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e))
            return;

        for (int i = 0; i < 4; ++i)
            if (segment (i).contains (e.position) && onChange != nullptr)
                onChange (kRanges[i]);
    }

    int range = 12;
    juce::Colour colour = Colours::accent;
    std::function<void (int)> onChange;
};


// Shows a popup menu at a screen position (the display is large: menus open where the click was).
void showMenuAt (juce::PopupMenu& menu, juce::Component& owner, juce::Point<int> screenPos, std::function<void (int)> done,
                 int minWidth)
{
    menu.setLookAndFeel (&owner.getLookAndFeel());
    const auto options = juce::PopupMenu::Options()
                             .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
                             .withMinimumWidth (minWidth);

    menu.showMenuAsync (options, [safe = juce::Component::SafePointer<juce::Component> (&owner), done = std::move (done)] (int result)
    {
        if (safe != nullptr && done != nullptr)
            done (result);
    });
}

struct BandCurve
{
    ps::EqBandParams key;
    bool valid = false;
    std::vector<float> db;
};
} // namespace

//==============================================================================
struct EqDisplay::Impl
{
    Impl (EqDisplay& o, EqModel& m, EditorServices& s, std::function<int()> cs)
        : owner (o), model (m), services (s), channelSetFn (std::move (cs))
    {
        range.range = rememberedRange (model.type);
        range.colour = moduleColour (model.type);
        range.onChange = [this] (int r) { owner.setDisplayRangeDb (r); };
        owner.addAndMakeVisible (range);
        selected = firstEnabledBand (0);
    }

    //==========================================================================
    // Geometry
    int activeSet() const { return model.isSplit() ? (channelSetFn != nullptr ? (channelSetFn() == 1 ? 1 : 0) : 0) : 0; }

    juce::Colour setColour (int set) const { return set == 1 && model.isSplit() ? Colours::sideChannel : moduleColour (model.type); }

    double sampleRate() const
    {
        const double sr = model.processor.getChain().getSampleRate();
        return sr > 1000.0 ? sr : 48000.0;
    }

    void layout()
    {
        plot = owner.getLocalBounds().toFloat().reduced (1.0f);
        dy.minDb = -(float) rangeDb();
        dy.maxDb = (float) rangeDb();
        dy.area = plot.withTrimmedTop (18.0f).withTrimmedBottom (22.0f);

        const int w = range.idealWidth();
        range.setBounds (juce::Rectangle<int> ((int) plot.getRight() - w - 8, (int) plot.getY() + 7, w, 20));
    }

    int rangeDb() const { return range.range; }

    // Rebuilds the x grid when the plot, the frequency scale or the sample rate changed (invalidates every curve).
    void ensureGrid()
    {
        const auto& options = services.getOptions();
        const double sr = sampleRate();
        const int n = juce::jlimit (128, 720, (int) (plot.getWidth() * 0.5f));
        const auto axis = makeFreqAxis (options, plot, model.processor.getChain().getSampleRate());

        if (n == (int) gridX.size() && axis.minHz == fx.minHz && axis.maxHz == fx.maxHz && axis.mel == fx.mel
            && axis.area == fx.area && sr == gridSampleRate)
            return;

        fx = axis;
        gridSampleRate = sr;
        gridX.resize ((size_t) n);
        gridHz.resize ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
            gridX[(size_t) i] = x;
            gridHz[(size_t) i] = fx.xToHz (x);
        }

        invalidateCurves();
    }

    void invalidateCurves()
    {
        for (auto& set : curves)
            for (auto& c : set)
                c.valid = false;
    }

    void recompute()
    {
        ensureGrid();
        const auto& p = model.get();

        if (p.filterMode != keyMode || p.amountPercent != keyAmount)
        {
            keyMode = p.filterMode;
            keyAmount = p.amountPercent;
            invalidateCurves();
        }

        const double sr = gridSampleRate;
        const double maxHz = sr * 0.4999;
        const int sets = model.isSplit() ? 2 : 1;

        for (int set = 0; set < 2; ++set)
        {
            auto& comp = composite[set];
            comp.assign (gridX.size(), 0.0f);

            if (set >= sets)
                continue;

            for (int b = 0; b < ps::kEqBands; ++b)
            {
                const auto& band = p.bands[set][b];
                auto& c = curves[set][b];

                if (! band.on)
                {
                    c.key = band;
                    continue;
                }

                if (! c.valid || ! sameBand (c.key, band))
                {
                    c.key = band;
                    c.valid = true;
                    c.db.resize (gridX.size());

                    for (size_t i = 0; i < gridX.size(); ++i)
                    {
                        const double hz = juce::jmin ((double) gridHz[i], maxHz);
                        const double mag = std::abs (ps::eqBandResponse (band, p.filterMode, hz, sr, p.amountPercent));
                        c.db[i] = (float) (20.0 * std::log10 (juce::jmax (mag, 1.0e-7)));
                    }
                }

                for (size_t i = 0; i < gridX.size(); ++i)
                    comp[i] += c.db[i];
            }
        }
    }

    // Curve value (dB) of band b of `set` at x, from the cache (exact evaluation if not cached).
    float bandDbAtX (int set, int b, float x) const
    {
        const auto& c = curves[set][b];

        if (c.valid && c.db.size() == gridX.size() && gridX.size() > 1)
        {
            const float t = (x - gridX.front()) / (gridX.back() - gridX.front()) * (float) (gridX.size() - 1);
            const int i = juce::jlimit (0, (int) gridX.size() - 2, (int) std::floor (t));
            const float f = juce::jlimit (0.0f, 1.0f, t - (float) i);
            return c.db[(size_t) i] + f * (c.db[(size_t) i + 1] - c.db[(size_t) i]);
        }

        const auto& p = model.get();
        const double hz = juce::jmin ((double) fx.xToHz (x), sampleRate() * 0.4999);
        const double mag = std::abs (ps::eqBandResponse (p.bands[set][b], p.filterMode, hz, sampleRate(), p.amountPercent));
        return (float) (20.0 * std::log10 (juce::jmax (mag, 1.0e-7)));
    }

    float clampY (float y) const { return juce::jlimit (dy.area.getY(), dy.area.getBottom(), y); }

    juce::Point<float> nodePos (int set, int b) const
    {
        const auto& band = model.band (set, b);
        const auto traits = eqShapeTraits (band.shape);
        const float x = fx.hzToX (band.freqHz);
        const float db = traits.pass ? bandDbAtX (set, b, x) : model.scaledGain (band);
        return { x, clampY (dy.dbToY (db)) };
    }

    int hitNode (juce::Point<float> pos) const
    {
        const int set = activeSet();
        int best = -1;
        float bestD = kHitRadius;

        for (int b = 0; b < ps::kEqBands; ++b)
        {
            if (! model.band (set, b).on)
                continue;

            float d = nodePos (set, b).getDistanceFrom (pos);

            if (b == selected)
                d -= 1.5f;   // the selected node wins ties

            if (d < bestD)
            {
                bestD = d;
                best = b;
            }
        }

        return best;
    }

    int firstEnabledBand (int set) const
    {
        for (int b = 0; b < ps::kEqBands; ++b)
            if (model.band (set, b).on)
                return b;

        return 0;
    }

    void select (int b)
    {
        b = juce::jlimit (0, ps::kEqBands - 1, b);

        if (b != selected)
        {
            selected = b;

            if (owner.onSelectionChanged != nullptr)
                owner.onSelectionChanged (b);
        }

        owner.repaint();
    }

    //==========================================================================
    // Edits (all sets an edit applies to: the shown one, plus the other one when linked)
    void beginGesture (int b, std::initializer_list<const char*> names)
    {
        for (int set : model.editSets (activeSet()))
            for (const char* name : names)
                if (auto* prm = model.bandParam (set, b, name))
                {
                    prm->beginChangeGesture();
                    gestureParams.push_back (prm);
                }
    }

    void endGesture()
    {
        for (auto* prm : gestureParams)
            prm->endChangeGesture();

        gestureParams.clear();
    }

    void setBand (int b, const char* name, float plain)
    {
        for (int set : model.editSets (activeSet()))
            if (auto* prm = model.bandParam (set, b, name))
            {
                const float norm = prm->convertTo0to1 (prm->getNormalisableRange().snapToLegalValue (plain));

                if (std::abs (prm->getValue() - norm) > 1.0e-7f)
                    prm->setValueNotifyingHost (norm);
            }
    }

    // One undo step for several values of one band (menus, double-clicks).
    void applyBandValues (int b, const std::vector<std::pair<const char*, float>>& values, const juce::String& description)
    {
        std::vector<std::pair<juce::String, float>> list;

        for (int set : model.editSets (activeSet()))
            for (const auto& [name, v] : values)
                list.emplace_back (model.bandId (set, b, name), v);

        model.processor.setParameterValues (list, description);
    }

    juce::String bandLabel (int b) const
    {
        const auto setName = model.isLinked() ? juce::String() : model.setName (activeSet());
        return juce::String (ps::moduleInfo (model.type).shortName) + " Band " + juce::String (b + 1)
               + (setName.isNotEmpty() ? " (" + setName + ")" : juce::String());
    }

    float currentPlain (int b, const char* name) const
    {
        if (auto* prm = model.bandParam (activeSet(), b, name))
            return prm->convertFrom0to1 (prm->getValue());

        return 0.0f;
    }

    static float defaultOf (const juce::String& id)
    {
        const int i = ps::paramIndex (id.toStdString());
        return i >= 0 ? ps::paramSpecs()[(size_t) i].defaultValue : 0.0f;
    }

    //==========================================================================
    // Node creation
    void addBandAt (juce::Point<float> pos)
    {
        const int set = activeSet();
        int b = -1;

        for (int i = 0; i < ps::kEqBands; ++i)
            if (! model.band (set, i).on)
            {
                b = i;
                break;
            }

        if (b < 0)
        {
            services.showHint ("All 8 bands are in use");
            return;
        }

        const float hz = freqClamp (fx.xToHz (pos.x));
        const auto shape = ps::defaultShapeForNewNode (model.get().bands[set], (double) hz);
        float gain = 0.0f;

        if (eqShapeTraits (shape).gain && model.amountScale() > 0.01f)
            gain = juce::jlimit (-30.0f, 15.0f, std::round (dy.yToDb (pos.y) / model.amountScale() * 10.0f) / 10.0f);

        const auto id = [&] (const char* name) { return model.bandId (set, b, name); };
        applyBandValues (b, { { "on", 1.0f }, { "shape", (float) shape }, { "freq", hz }, { "gain", gain },
                              { "q", defaultOf (id ("q")) }, { "slope", defaultOf (id ("slope")) },
                              { "phase", defaultOf (id ("phase")) } },
                         "Added " + bandLabel (b) + ": " + eqShapeFullName (shape) + " at " + fmt::hz (hz));
        model.update();
        owner.parametersChanged();
        select (b);
    }

    //==========================================================================
    // Menus
    void showNodeMenu (int b)
    {
        const auto& band = model.band (activeSet(), b);
        const auto traits = eqShapeTraits (band.shape);
        const bool digital = model.isDigital();

        juce::PopupMenu menu;
        menu.addSectionHeader ("Band " + juce::String (b + 1) + "  " + eqShapeFullName (band.shape));
        menu.addSubMenu ("Shape", eqShapeMenu (band.shape, digital));

        if (traits.slope)
        {
            juce::PopupMenu slopes;
            for (int i = 0; i < (int) ps::EqSlope::Count; ++i)
                slopes.addItem (100 + i, juce::String (ps::kEqSlopeDb[(size_t) i]) + " dB/oct", true, (int) band.slope == i);
            menu.addSubMenu ("Slope", slopes);
        }

        if (digital)
        {
            juce::PopupMenu phase;
            const int steps[] { 0, 25, 50, 75, 100 };

            for (int s : steps)
                phase.addItem (200 + s, s == 0 ? juce::String ("Linear phase (0 %)")
                                               : (s == 100 ? juce::String ("Minimum phase (100 %)") : juce::String (s) + " %"),
                               true, std::abs (band.phasePercent - (float) s) < 0.5f);
            menu.addSubMenu ("Phase", phase);
        }

        menu.addSeparator();
        menu.addItem (2, "Reset band");
        menu.addItem (1, "Turn band off");

        showMenuAt (menu, owner, juce::Desktop::getMousePosition(), [this, b] (int result)
        {
            if (result <= 0)
                return;

            const auto& current = model.band (activeSet(), b);

            if (result == 1)
            {
                applyBandValues (b, { { "on", 0.0f } }, "Turned off " + bandLabel (b));
            }
            else if (result == 2)
            {
                const auto id = [&] (const char* name) { return model.bandId (activeSet(), b, name); };
                applyBandValues (b, { { "shape", defaultOf (id ("shape")) }, { "gain", 0.0f }, { "q", defaultOf (id ("q")) },
                                      { "slope", defaultOf (id ("slope")) }, { "phase", defaultOf (id ("phase")) } },
                                 "Reset " + bandLabel (b));
            }
            else if (result >= 1 + (int) ps::EqShape::Bell && result <= (int) ps::EqShape::Count)
            {
                const auto shape = (ps::EqShape) (result - 1);

                if (shape != current.shape)
                    applyBandValues (b, { { "shape", (float) shape } }, bandLabel (b) + " Shape: " + eqShapeFullName (shape));
            }
            else if (result >= 100 && result < 100 + (int) ps::EqSlope::Count)
            {
                const int slope = result - 100;
                applyBandValues (b, { { "slope", (float) slope } },
                                 bandLabel (b) + " Slope: " + juce::String (ps::kEqSlopeDb[(size_t) slope]) + " dB/oct");
            }
            else if (result >= 200 && result <= 300)
            {
                applyBandValues (b, { { "phase", (float) (result - 200) } },
                                 bandLabel (b) + " Phase: " + juce::String (result - 200) + " %");
            }

            model.update();
            owner.parametersChanged();
        }, 200);
    }

    void showBackgroundMenu (juce::Point<float> pos)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Add a band at " + fmt::hz (freqClamp (fx.xToHz (pos.x))));
        menu.addSeparator();
        menu.addSectionHeader ("Display range");

        for (int r : kRanges)
            menu.addItem (10 + r, "+/- " + juce::String (r) + " dB", true, r == rangeDb());

        showMenuAt (menu, owner, juce::Desktop::getMousePosition(), [this, pos] (int result)
        {
            if (result == 1)
                addBandAt (pos);
            else if (result > 10)
                owner.setDisplayRangeDb (result - 10);
        }, 180);
    }

    //==========================================================================
    // Audition (Alt-drag)
    void startAudition (juce::Point<float> pos, int band)
    {
        audition.on = true;
        audition.band = band;
        audition.virt = pos;
        audition.last = pos;

        if (band >= 0)
        {
            const auto& bp = model.band (activeSet(), band);
            const auto traits = eqShapeTraits (bp.shape);
            audition.virt.x = fx.hzToX (bp.freqHz);
            audition.q = traits.q && ! traits.pass ? juce::jlimit (0.2f, 12.0f, bp.q) : 2.0f;
        }
        else
        {
            audition.q = freeAuditionQ;
        }

        sendAudition();
    }

    void sendAudition()
    {
        auto m = model.processor.getMonitor (model.type);
        m.auditionOn = audition.on;
        m.auditionHz = freqClamp (fx.xToHz (audition.virt.x));
        m.auditionQ = juce::jlimit (0.2f, 12.0f, audition.q);
        m.auditionChannelSet = activeSet();
        model.processor.setMonitor (model.type, m);
        owner.repaint();
    }

    void stopAudition()
    {
        if (! audition.on)
            return;

        audition.on = false;
        sendAudition();
    }

    //==========================================================================
    // Painting
    // skipFlat: leaves out the stretches where the curve sits on 0 dB (band outlines would otherwise all paint the
    // 0 dB line in their colours).
    juce::Path curvePath (const std::vector<float>& db, bool skipFlat = false) const
    {
        juce::Path p;
        const float lo = dy.area.getY() - 40.0f, hi = dy.area.getBottom() + 40.0f;
        bool drawing = false;

        for (size_t i = 0; i < gridX.size() && i < db.size(); ++i)
        {
            const float y = juce::jlimit (lo, hi, dy.dbToY (db[i]));
            const bool flat = skipFlat && std::abs (db[i]) < 0.08f
                              && (i + 1 >= db.size() || std::abs (db[i + 1]) < 0.08f)
                              && (i == 0 || std::abs (db[i - 1]) < 0.08f);

            if (flat)
            {
                drawing = false;
                continue;
            }

            if (! drawing)
                p.startNewSubPath (gridX[i], y);
            else
                p.lineTo (gridX[i], y);

            drawing = true;
        }

        return p;
    }

    juce::Path fillPath (const std::vector<float>& db) const
    {
        auto p = curvePath (db);

        if (gridX.empty())
            return p;

        const float zero = dy.dbToY (0.0f);
        p.lineTo (gridX.back(), zero);
        p.lineTo (gridX.front(), zero);
        p.closeSubPath();
        return p;
    }

    void paintNode (juce::Graphics& g, int b, juce::Point<float> c, bool isSel, bool isHover) const
    {
        const auto colour = eqBandColour (b);
        const float r = kNodeRadius + (isHover || isSel ? 1.0f : 0.0f);

        if (isHover || isSel)
        {
            g.setColour (colour.withAlpha (isSel ? 0.22f : 0.16f));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f + 10.0f, r * 2.0f + 10.0f).withCentre (c));
        }

        const auto body = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (body.translated (0.0f, 1.2f).expanded (0.8f));
        g.setGradientFill (juce::ColourGradient::vertical (colour.brighter (0.18f), body.getY(), colour.darker (0.12f), body.getBottom()));
        g.fillEllipse (body);

        if (isSel)
        {
            g.setColour (Colours::text);
            g.drawEllipse (body.expanded (2.2f), 1.6f);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawEllipse (body, 1.0f);
        }

        g.setFont (font (11.0f, 700));
        g.setColour (Colours::background.withAlpha (0.92f));
        g.drawText (juce::String (b + 1), body.translated (0.0f, 0.5f), juce::Justification::centred, false);
    }

    void paintReadout (juce::Graphics& g, int b, juce::Point<float> node) const
    {
        const auto& band = model.band (activeSet(), b);
        const auto traits = eqShapeTraits (band.shape);
        const auto colour = eqBandColour (b);

        juce::StringArray parts;
        parts.add (fmt::hz (band.freqHz));

        if (traits.gain)
            parts.add (fmt::db (band.gainDb));

        if (traits.slope)
            parts.add (juce::String (ps::kEqSlopeDb[(size_t) juce::jlimit (0, 5, (int) band.slope)]) + " dB/oct");

        if (traits.q)
        {
            const auto shown = eqQDisplay (band.shape);
            parts.add (qText (shown.scale * band.q + shown.offset));
        }

        const auto title = juce::String (b + 1) + "  " + eqShapeShortName (band.shape)
                           + (band.shape == ps::EqShape::Surgical && ! model.isDigital() ? " (as Bell)" : juce::String());
        const auto line = parts.joinIntoString ("   ");
        const auto titleFont = font (11.5f, 700), lineFont = font (11.5f, 600);
        const float w = juce::jmax (textWidth (titleFont, title), textWidth (lineFont, line)) + 20.0f;
        const float h = 38.0f;

        auto box = juce::Rectangle<float> (w, h).withCentre ({ node.x, node.y - kNodeRadius - 12.0f - h * 0.5f });

        if (box.getY() < plot.getY() + 4.0f)
            box.setY (node.y + kNodeRadius + 12.0f);

        box = box.constrainedWithin (plot.reduced (4.0f));

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (box.translated (0.0f, 2.0f), 6.0f);
        g.setColour (Colours::panelRaised.withAlpha (0.96f));
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (colour.withAlpha (0.55f));
        g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);

        auto inner = box.reduced (10.0f, 4.0f);
        g.setFont (titleFont);
        g.setColour (colour.brighter (0.15f));
        g.drawText (title, inner.removeFromTop (15.0f), juce::Justification::centredLeft, false);
        g.setFont (lineFont);
        g.setColour (Colours::text);
        g.drawText (line, inner, juce::Justification::centredLeft, false);
    }

    void paint (juce::Graphics& g)
    {
        if (gridX.empty())
            recompute();

        const auto b = owner.getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.3f), b.getY(),
                                                           Colours::background.darker (0.08f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (plot, 6.0f);
        g.reduceClipRegion (clip);

        drawFrequencyGrid (g, fx, dy, true);

        // Spectra (their own scale, behind everything)
        const auto& options = services.getOptions();
        auto& pre = services.getModulePreSpectrum();
        auto& post = services.getModulePostSpectrum();
        FreqAxis specAxis = fx;
        specAxis.area = plot.withTrimmedBottom (18.0f);
        // (the analyser sets its own alphas: transparency layers keep the spectra in the background)
        g.beginTransparencyLayer (0.6f);
        pre.draw (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb, Colours::input, options.spectrumFill);
        g.endTransparencyLayer();
        g.beginTransparencyLayer (0.42f);
        post.draw (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb, blend (setColour (activeSet()), Colours::input, 0.3f), false);
        g.endTransparencyLayer();

        const int set = activeSet();
        const auto colour = setColour (set);
        const auto& p = model.get();

        // The other channel set's curve, dimmed (M/S, L/R)
        if (model.isSplit())
        {
            const int other = 1 - set;
            bool any = false;

            for (int i = 0; i < ps::kEqBands; ++i)
                any = any || p.bands[other][i].on;

            if (any)
            {
                g.setColour (setColour (other).withAlpha (0.42f));
                const float dashes[] { 5.0f, 4.0f };
                juce::Path dashed;
                juce::PathStrokeType (1.5f).createDashedStroke (dashed, curvePath (composite[other]), dashes, 2);
                g.fillPath (dashed);
            }
        }

        // Per-band soft fills and outlines
        const int focus = drag.band >= 0 ? drag.band : (hover >= 0 ? hover : selected);

        for (int i = 0; i < ps::kEqBands; ++i)
        {
            if (! p.bands[set][i].on || ! curves[set][i].valid)
                continue;

            const bool isFocus = i == focus;
            const auto bc = eqBandColour (i);
            g.setColour (bc.withAlpha (isFocus ? 0.20f : 0.085f));
            g.fillPath (fillPath (curves[set][i].db));
            g.setColour (bc.withAlpha (isFocus ? 0.85f : 0.32f));
            g.strokePath (curvePath (curves[set][i].db, true), juce::PathStrokeType (isFocus ? 1.4f : 1.0f));
        }

        // Audition range
        if (audition.on)
        {
            const float hz = freqClamp (fx.xToHz (audition.virt.x));
            const float half = octavesForQ (audition.q) * 0.5f;
            const float x0 = fx.hzToX (hz * std::pow (2.0f, -half)), x1 = fx.hzToX (hz * std::pow (2.0f, half));
            const float xc = fx.hzToX (hz);
            const auto zone = juce::Rectangle<float> (x0, plot.getY(), juce::jmax (2.0f, x1 - x0), plot.getHeight());
            juce::ColourGradient grad (Colours::minor.withAlpha (0.02f), x0, 0.0f, Colours::minor.withAlpha (0.02f), x1, 0.0f, false);
            grad.addColour (0.5, Colours::minor.withAlpha (0.16f));
            g.setGradientFill (grad);
            g.fillRect (zone);
            g.setColour (Colours::minor.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (xc - 0.5f, plot.getY(), 1.0f, plot.getHeight()));

            const auto text = "Listening around " + fmt::hz (hz);
            const auto f = font (11.0f, 700);
            const float tw = textWidth (f, text) + 16.0f;
            auto pill = juce::Rectangle<float> (tw, 20.0f).withCentre ({ xc, plot.getY() + 40.0f }).constrainedWithin (plot.reduced (6.0f));
            g.setColour (Colours::minor.withAlpha (0.9f));
            g.fillRoundedRectangle (pill, 10.0f);
            g.setColour (Colours::background);
            g.setFont (f);
            g.drawText (text, pill, juce::Justification::centred, false);
        }

        // Composite curve of the shown set
        bool anyOn = false;

        for (int i = 0; i < ps::kEqBands; ++i)
            anyOn = anyOn || p.bands[set][i].on;

        if (anyOn)
        {
            const auto path = curvePath (composite[set]);
            const auto curveColour = blend (colour, Colours::text, 0.35f);
            g.setColour (colour.withAlpha (0.18f));
            g.strokePath (path, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (curveColour);
            g.strokePath (path, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Nodes (the focused one on top)
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < ps::kEqBands; ++i)
            {
                if (! p.bands[set][i].on || (i == focus) != (pass == 1))
                    continue;

                paintNode (g, i, nodePos (set, i), i == selected, i == hover || i == drag.band);
            }

        // Readout of the hovered / dragged node
        const int readout = drag.band >= 0 ? drag.band : hover;

        if (readout >= 0 && p.bands[set][readout].on && ! audition.on)
            paintReadout (g, readout, nodePos (set, readout));
        else if (mouseInside && hover < 0 && drag.band < 0 && ! audition.on && plot.contains (mousePos))
        {
            // Cursor frequency / gain
            const auto text = fmt::hz (fx.xToHz (mousePos.x)) + "   " + fmt::db (dy.yToDb (mousePos.y));
            const auto f = font (10.5f, 600);
            auto r = juce::Rectangle<float> (textWidth (f, text) + 4.0f, 14.0f).withPosition (mousePos.x + 12.0f, mousePos.y - 20.0f);

            if (r.getRight() > plot.getRight() - 4.0f)
                r.setX (mousePos.x - 12.0f - r.getWidth());

            r = r.constrainedWithin (plot.reduced (4.0f));
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r, juce::Justification::centredLeft, false);
        }

        // Legend (left of the range switch)
        {
            auto legend = juce::Rectangle<float> ((float) range.getX() - 170.0f, (float) range.getY(), 160.0f, (float) range.getHeight());
            auto item = [&] (const juce::String& text, juce::Colour c)
            {
                const auto f = font (10.5f, 600);
                const float w = textWidth (f, text) + 16.0f;
                auto r = legend.removeFromRight (w);
                legend.removeFromRight (10.0f);
                g.setColour (c);
                g.fillRoundedRectangle (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 3.0f), 1.5f);
                g.setFont (f);
                g.setColour (Colours::textDim);
                g.drawText (text, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
            };
            item ("Output", setColour (set));
            item ("Input", Colours::input);
        }

        // Digital mode: the new filter is still being designed
        if (designPending)
        {
            const auto text = juce::String ("Designing filter...");
            const auto f = font (10.5f, 600);
            const float w = textWidth (f, text) + 26.0f;
            auto pill = juce::Rectangle<float> (w, 20.0f).withCentre ({ plot.getCentreX(), (float) range.getBounds().getCentreY() });
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillRoundedRectangle (pill, 10.0f);
            g.setColour (Colours::outline.withAlpha (0.8f));
            g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, 1.0f);
            const float pulse = 0.45f + 0.55f * (0.5f + 0.5f * std::sin ((float) (juce::Time::getMillisecondCounter() % 100000) * 0.008f));
            g.setColour (colour.withAlpha (pulse));
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ pill.getX() + 11.0f, pill.getCentreY() }));
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, pill.withTrimmedLeft (18.0f).withTrimmedRight (6.0f), juce::Justification::centred, false);
        }

        // Empty state
        if (! anyOn && ! audition.on)
        {
            const auto setName = model.isSplit() ? model.setName (set) : juce::String();
            const auto title = setName.isNotEmpty() ? "No bands on the " + setName + " channel yet" : juce::String ("No bands yet");
            const auto sub = juce::String ("Double-click anywhere on the display to add a band");
            const auto titleFont = font (13.0f, 700), subFont = font (11.5f, 600);
            const float w = juce::jmax (textWidth (titleFont, title), textWidth (subFont, sub)) + 36.0f;
            const auto box = juce::Rectangle<float> (w, 50.0f).withCentre (dy.area.getCentre());
            g.setColour (Colours::background.withAlpha (0.82f));
            g.fillRoundedRectangle (box, 9.0f);
            g.setColour (Colours::outline.withAlpha (0.7f));
            g.drawRoundedRectangle (box.reduced (0.5f), 9.0f, 1.0f);
            g.setFont (titleFont);
            g.setColour (Colours::text.withAlpha (0.9f));
            g.drawText (title, box.withTrimmedBottom (22.0f).withTrimmedTop (4.0f), juce::Justification::centred, false);
            g.setFont (subFont);
            g.setColour (Colours::textDim);
            g.drawText (sub, box.withTrimmedTop (24.0f).withTrimmedBottom (6.0f), juce::Justification::centred, false);
        }
    }

    //==========================================================================
    EqDisplay& owner;
    EqModel& model;
    EditorServices& services;
    std::function<int()> channelSetFn;
    RangeSelector range;

    juce::Rectangle<float> plot;
    FreqAxis fx;
    DbAxis dy;
    std::vector<float> gridX, gridHz;
    double gridSampleRate = 0.0;
    BandCurve curves[2][ps::kEqBands];
    std::vector<float> composite[2];
    ps::EqFilterMode keyMode = ps::EqFilterMode::Analog;
    float keyAmount = -1.0f;

    int selected = 0, hover = -1;
    juce::Point<float> mousePos;
    bool mouseInside = false;
    bool designPending = false;

    struct Drag
    {
        int band = -1;
        bool begun = false;
        bool qMode = false;          // Resonant pass: vertical = resonance
        juce::Point<float> start, virt, last;
        float startQ = 0.707f, startGain = 0.0f, startDb = 0.0f;
    } drag;

    struct Audition
    {
        bool on = false;
        int band = -1;
        float q = 4.0f;
        juce::Point<float> virt, last;
    } audition;
    float freeAuditionQ = 4.0f;

    std::vector<juce::RangedAudioParameter*> gestureParams;
    int wheelBand = -1;
    float wheelNorm = 0.0f, wheelAccum = 0.0f;
    bool wheelActive = false;
};

//==============================================================================
EqDisplay::EqDisplay (EqModel& m, EditorServices& s, std::function<int()> channelSet)
    : impl (std::make_unique<Impl> (*this, m, s, std::move (channelSet)))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (false);
    setTooltip ("Double-click to add a band. Drag a band to move it (Shift: fine, Ctrl: one direction only), mouse wheel "
                "for its width (Q). Alt-drag to listen to one frequency range. Right-click a band for its shape and options.");
}

EqDisplay::~EqDisplay()
{
    stopTimer();
    impl->endGesture();
    impl->stopAudition();
}

void EqDisplay::parametersChanged()
{
    impl->recompute();
    repaint();
}

void EqDisplay::tick()
{
    auto& d = *impl;
    bool pending = false;

    if (d.model.isDigital())
        pending = static_cast<ps::Equalizer&> (d.model.processor.getChain().getModule (d.model.type)).isDesignPending();

    d.designPending = pending;
    d.recompute();   // cheap when nothing changed; follows option / sample-rate changes

    repaint();
}

juce::String EqDisplay::getTooltip()
{
    // No tooltip over a node or while editing: the node readout already says everything.
    if (impl->hover >= 0 || impl->drag.band >= 0 || impl->audition.on)
        return {};

    return juce::SettableTooltipClient::getTooltip();
}

juce::Point<float> EqDisplay::getNodePosition (int band) const
{
    return impl->nodePos (impl->activeSet(), juce::jlimit (0, ps::kEqBands - 1, band));
}

juce::Point<float> EqDisplay::pointFor (float hz, float db) const
{
    return { impl->fx.hzToX (hz), impl->dy.dbToY (db) };
}

int EqDisplay::getSelectedBand() const noexcept { return impl->selected; }

void EqDisplay::setSelectedBand (int band)
{
    impl->selected = juce::jlimit (0, ps::kEqBands - 1, band);
    repaint();
}

int EqDisplay::getDisplayRangeDb() const noexcept { return impl->range.range; }

void EqDisplay::setDisplayRangeDb (int r)
{
    impl->range.range = r;
    rememberedRange (impl->model.type) = r;
    impl->range.repaint();
    impl->layout();
    repaint();
}

void EqDisplay::paint (juce::Graphics& g) { impl->paint (g); }

void EqDisplay::resized()
{
    impl->layout();
    impl->recompute();
}

void EqDisplay::mouseMove (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.mouseInside = true;
    d.mousePos = e.position;
    const int h = d.hitNode (e.position);

    if (h != d.hover)
        d.hover = h;

    setMouseCursor (h >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void EqDisplay::mouseExit (const juce::MouseEvent&)
{
    impl->mouseInside = false;
    impl->hover = -1;
    repaint();
}

void EqDisplay::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const int node = d.hitNode (e.position);

    if (e.mods.isPopupMenu())
    {
        if (node >= 0)
        {
            d.select (node);
            d.showNodeMenu (node);
        }
        else
        {
            d.showBackgroundMenu (e.position);
        }

        return;
    }

    if (! isLeftClick (e))
        return;

    // A wheel burst still open ends before anything else starts.
    if (d.wheelActive)
    {
        stopTimer();
        d.wheelActive = false;
        d.endGesture();
    }

    if (e.mods.isAltDown())
    {
        if (node >= 0)
            d.select (node);

        d.startAudition (e.position, node);
        return;
    }

    if (node < 0)
        return;

    d.select (node);
    const auto& band = d.model.band (d.activeSet(), node);
    const auto traits = eqShapeTraits (band.shape);
    d.drag = {};
    d.drag.band = node;
    d.drag.start = d.nodePos (d.activeSet(), node);
    d.drag.virt = d.drag.start;
    d.drag.last = e.position;
    d.drag.qMode = traits.pass && traits.q;
    d.drag.startQ = band.q;
    d.drag.startGain = band.gainDb;
    d.drag.startDb = d.dy.yToDb (d.drag.start.y);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void EqDisplay::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.mousePos = e.position;
    const bool fine = e.mods.isShiftDown();

    if (d.audition.on)
    {
        const auto delta = e.position - d.audition.last;
        d.audition.last = e.position;
        d.audition.virt.x = juce::jlimit (d.plot.getX(), d.plot.getRight(), d.audition.virt.x + delta.x * (fine ? 0.1f : 1.0f));
        d.sendAudition();
        return;
    }

    if (d.drag.band < 0)
        return;

    const auto delta = e.position - d.drag.last;
    d.drag.last = e.position;
    d.drag.virt += delta * (fine ? 0.1f : 1.0f);
    d.drag.virt.x = juce::jlimit (d.plot.getX(), d.plot.getRight(), d.drag.virt.x);
    // (vertical: relative to where the drag started, so a node drawn at the edge of a small display range does not
    //  jump; the virtual position stops where the value hits its limit, so there is no dead zone on the way back)

    auto target = d.drag.virt;

    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        const auto total = target - d.drag.start;

        if (std::abs (total.x) >= std::abs (total.y))
            target.y = d.drag.start.y;
        else
            target.x = d.drag.start.x;
    }

    const int b = d.drag.band;
    const auto traits = eqShapeTraits (d.model.band (d.activeSet(), b).shape);

    if (! d.drag.begun)
    {
        if (e.getDistanceFromDragStart() < 2)
            return;

        d.drag.begun = true;

        if (traits.gain)
            d.beginGesture (b, { "freq", "gain" });
        else if (d.drag.qMode)
            d.beginGesture (b, { "freq", "q" });
        else
            d.beginGesture (b, { "freq" });
    }

    d.setBand (b, "freq", freqClamp (d.fx.xToHz (target.x)));

    if (traits.gain)
    {
        const float scale = d.model.amountScale();

        if (scale > 0.01f)
        {
            const float gain = d.drag.startGain + (d.dy.yToDb (target.y) - d.drag.startDb) / scale;
            const float clamped = juce::jlimit (-30.0f, 15.0f, gain);

            if (clamped != gain && target.y == d.drag.virt.y)
                d.drag.virt.y = d.dy.dbToY (d.drag.startDb + (clamped - d.drag.startGain) * scale);

            d.setBand (b, "gain", clamped);
        }
    }
    else if (d.drag.qMode)
    {
        const float dbDelta = d.dy.yToDb (target.y) - d.drag.startDb;
        const float newQ = d.drag.startQ * std::pow (10.0f, dbDelta / 20.0f);
        const float clamped = juce::jlimit (0.1f, 24.0f, newQ);

        if (clamped != newQ && target.y == d.drag.virt.y)
            d.drag.virt.y = d.dy.dbToY (d.drag.startDb + 20.0f * std::log10 (clamped / juce::jmax (0.01f, d.drag.startQ)));

        d.setBand (b, "q", clamped);
    }

    d.model.update();
    parametersChanged();
}

void EqDisplay::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (d.audition.on)
    {
        if (d.audition.band < 0)
            d.freeAuditionQ = d.audition.q;

        d.stopAudition();
    }

    if (d.drag.band >= 0)
    {
        d.endGesture();
        d.drag = {};
    }

    d.hover = d.hitNode (e.position);
    setMouseCursor (d.hover >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void EqDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e) || e.mods.isAltDown())
        return;

    const int node = d.hitNode (e.position);

    if (node < 0)
    {
        if (d.plot.contains (e.position))
            d.addBandAt (e.position);

        return;
    }

    const auto& band = d.model.band (d.activeSet(), node);
    const auto traits = eqShapeTraits (band.shape);

    if (traits.gain && band.gainDb != 0.0f)
        d.applyBandValues (node, { { "gain", 0.0f } }, d.bandLabel (node) + " Gain: 0.0 dB");
    else if (traits.pass && traits.q)
        d.applyBandValues (node, { { "q", Impl::defaultOf (d.model.bandId (d.activeSet(), node, "q")) } },
                           d.bandLabel (node) + " Q: default");

    d.model.update();
    parametersChanged();
}

void EqDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    auto& d = *impl;
    const float delta = (std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX)
                        * (wheel.isReversed ? -1.0f : 1.0f);

    if (std::abs (delta) < 1.0e-4f)
        return;

    const bool fine = e.mods.isShiftDown() || e.mods.isCtrlDown();

    // Audition: the wheel sets the width of the range heard.
    if (d.audition.on)
    {
        d.audition.q = juce::jlimit (0.2f, 12.0f, d.audition.q * std::pow (2.0f, delta * (fine ? 0.5f : 2.0f)));
        d.sendAudition();
        return;
    }

    if (d.drag.band >= 0)
        return;

    const int node = d.wheelActive ? d.wheelBand : d.hitNode (e.position);

    if (node < 0)
        return;

    const auto& band = d.model.band (d.activeSet(), node);
    const auto traits = eqShapeTraits (band.shape);
    const char* name = traits.pass ? (traits.slope ? "slope" : nullptr) : (traits.q ? "q" : nullptr);

    if (name == nullptr)
    {
        d.services.showHint (eqShapeShortName (band.shape) + " has no width to change");
        return;
    }

    if (! d.wheelActive)
    {
        d.select (node);
        d.wheelActive = true;
        d.wheelBand = node;
        d.wheelAccum = 0.0f;

        if (auto* prm = d.model.bandParam (d.activeSet(), node, name))
            d.wheelNorm = prm->getValue();

        d.beginGesture (node, { name });
    }

    if (traits.pass)
    {
        // Slope: one step per wheel notch.
        d.wheelAccum += delta;
        int step = 0;

        while (d.wheelAccum >= 0.2f)  { ++step; d.wheelAccum -= 0.2f; }
        while (d.wheelAccum <= -0.2f) { --step; d.wheelAccum += 0.2f; }

        if (step != 0)
        {
            const int slope = juce::jlimit (0, (int) ps::EqSlope::Count - 1, (int) band.slope + step);
            d.setBand (node, "slope", (float) slope);
        }
    }
    else if (auto* prm = d.model.bandParam (d.activeSet(), node, name))
    {
        // Wheel up = narrower (higher Q), like most equalizers.
        d.wheelNorm = juce::jlimit (0.0f, 1.0f, d.wheelNorm + delta * (fine ? 0.012f : 0.07f));
        d.setBand (node, name, prm->convertFrom0to1 (d.wheelNorm));
    }

    d.model.update();
    parametersChanged();
    startTimer (kWheelGestureMs);   // the burst ends (one undo step) shortly after the last wheel event
}

void EqDisplay::timerCallback()
{
    stopTimer();
    auto& d = *impl;

    if (d.wheelActive)
    {
        d.wheelActive = false;
        d.wheelBand = -1;
        d.endGesture();
    }
}
} // namespace ps::ui
