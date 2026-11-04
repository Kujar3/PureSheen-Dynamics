#include "ui/DynamicEqDisplay.h"

#include "dsp/DynamicEq.h"
#include "ui/Controls.h"
#include "ui/EqModel.h"
#include "ui/ImagerExciterCommon.h"

#include <cmath>

// Dynamic EQ display (owner: ui-deq). See DynamicEqDisplay.h for what is drawn and the mouse map.
//
// Curves: every band is evaluated with ps::dynEqBandResponse (the module's own static response, the Equalizer's design
// code) on a grid of points spaced evenly in x (so every frequency scale works). Per band the rest and full-trigger
// curves are cached until the band changes; the live curve is re-evaluated only when the band's live gain moved by
// more than 0.01 dB, so a resting display costs nothing and a moving one a few hundred evaluations per tick.
// Live values come from ps::DynamicEqMeters (the instance of the shown channel set: the T/S twin for Sustain), read in
// tick() (30 Hz). Gains glide over ~2 ticks, detector levels rise at once and fall 30 dB/s. While the module is not
// running (removed, powered off) the live curve sits at rest.
namespace ps::ui
{
namespace
{
constexpr float kNodeRadius = 8.5f;
constexpr float kHitRadius = 12.0f;
constexpr float kSpectrumMinDb = -90.0f, kSpectrumMaxDb = 0.0f;   // spectrum and level (threshold) scale
constexpr float kThrGrab = 6.0f;
constexpr int kRanges[] { 6, 12, 18, 30 };
constexpr juce::uint32 kWheelGestureMs = 450;

int& rememberedRange()
{
    static int range = 12;
    return range;
}

float freqClamp (float hz) { return juce::jlimit (20.0f, 20000.0f, hz); }
float gainClamp (float db) { return juce::jlimit (-30.0f, 15.0f, db); }
float safeDb (float v) { return std::isfinite (v) ? juce::jlimit (-200.0f, 60.0f, v) : -200.0f; }

bool sameShape (const ps::DynamicEqBandParams& a, const ps::DynamicEqBandParams& b) noexcept
{
    return a.on == b.on && a.shape == b.shape && a.freqHz == b.freqHz && a.gainDb == b.gainDb && a.q == b.q
           && a.direction == b.direction && a.offsetDb == b.offsetDb && a.dynamic == b.dynamic;
}

// Width in octaves of a band-pass of quality q (between its -3 dB points).
float octavesForQ (float q)
{
    const double x = 1.0 / (2.0 * (double) juce::jmax (0.05f, q));
    return (float) (2.0 / std::log (2.0) * std::log (x + std::sqrt (x * x + 1.0)));
}

juce::String qText (float q) { return "Q " + juce::String (q, q < 10.0f ? 2 : 1); }

juce::String dbShort (float db)
{
    return (db > 0.05f ? "+" : "") + juce::String (std::abs (db) < 0.05f ? 0.0f : db, 1);
}

//==============================================================================
// "6 | 12 | 18 | 30 dB" display range switch (top-right corner), as in the Equalizer display.
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

// Popup menu at a screen position (menus open where the click was).
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

struct BandCurves
{
    ps::DynamicEqBandParams key;
    bool valid = false;
    float restGain = 0.0f, trigGain = 0.0f;
    std::vector<float> rest, trig, live;
    bool liveValid = false;
    float liveGain = 0.0f;
};
} // namespace

//==============================================================================
struct DynamicEqDisplay::Impl
{
    Impl (DynamicEqDisplay& o, DynamicEqModel& m, EditorServices& s, std::function<int()> cs)
        : owner (o), model (m), services (s), channelSetFn (std::move (cs))
    {
        range.range = rememberedRange();
        range.colour = moduleColour (ps::ModuleType::DynamicEq);
        range.onChange = [this] (int r) { owner.setDisplayRangeDb (r); };
        owner.addAndMakeVisible (range);

        for (auto& set : live.detectorDb)
            for (auto& v : set)
                v = -200.0f;

        selected = firstEnabledBand (0);
        readMeters (true);
    }

    //==========================================================================
    // Geometry
    int activeSet() const { return model.isSplit() ? (channelSetFn != nullptr ? (channelSetFn() == 1 ? 1 : 0) : 0) : 0; }

    juce::Colour setColour (int set) const
    {
        return set == 1 && model.isSplit() ? Colours::sideChannel : moduleColour (ps::ModuleType::DynamicEq);
    }

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
        dy.area = snapEqZeroLine (plot.withTrimmedTop (18.0f).withTrimmedBottom (22.0f));
        lv.minDb = kSpectrumMinDb;
        lv.maxDb = kSpectrumMaxDb;
        lv.area = plot.withTrimmedBottom (18.0f);

        const int w = range.idealWidth();
        range.setBounds (juce::Rectangle<int> ((int) plot.getRight() - w - 8, (int) plot.getY() + 7, w, 20));
    }

    int rangeDb() const { return range.range; }

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

        for (auto& set : curves)
            for (auto& c : set)
                c.valid = c.liveValid = false;
    }

    void evaluate (const ps::DynamicEqBandParams& band, float gainDb, std::vector<float>& out) const
    {
        out.resize (gridX.size());
        const double sr = gridSampleRate;
        const double maxHz = sr * 0.4999;

        for (size_t i = 0; i < gridX.size(); ++i)
        {
            const double hz = juce::jmin ((double) gridHz[i], maxHz);
            const double mag = std::abs (ps::dynEqBandResponse (band, (double) gainDb, hz, sr));
            out[i] = (float) (20.0 * std::log10 (juce::jmax (mag, 1.0e-7)));
        }
    }

    float liveGainOf (int set, int b) const
    {
        const auto& c = curves[set][b];
        return live.running ? live.gainDb[set][b] : c.restGain;
    }

    // Static curves (when a band changed) and the live curves (when a live gain moved), then the composites.
    void recompute()
    {
        ensureGrid();
        const auto& p = model.get();
        const int sets = model.isSplit() ? 2 : 1;

        for (int set = 0; set < 2; ++set)
        {
            compositeRest[set].assign (gridX.size(), 0.0f);
            compositeLive[set].assign (gridX.size(), 0.0f);

            if (set >= sets)
                continue;

            for (int b = 0; b < ps::kDynEqBands; ++b)
            {
                const auto& band = p.bands[set][b];
                auto& c = curves[set][b];

                if (! band.on)
                {
                    c.key = band;
                    continue;
                }

                if (! c.valid || ! sameShape (c.key, band))
                {
                    c.key = band;
                    c.valid = true;
                    c.liveValid = false;
                    c.restGain = (float) ps::dynEqRestGainDb (band);
                    c.trigGain = (float) ps::dynEqTriggeredGainDb (band);
                    evaluate (band, c.restGain, c.rest);

                    if (std::abs (c.trigGain - c.restGain) < 0.005f)
                        c.trig = c.rest;
                    else
                        evaluate (band, c.trigGain, c.trig);
                }

                const float g = liveGainOf (set, b);

                if (! c.liveValid || std::abs (g - c.liveGain) > 0.01f)
                {
                    c.liveValid = true;
                    c.liveGain = g;

                    if (std::abs (g - c.restGain) < 0.01f)
                        c.live = c.rest;
                    else if (std::abs (g - c.trigGain) < 0.01f)
                        c.live = c.trig;
                    else
                        evaluate (band, g, c.live);
                }

                for (size_t i = 0; i < gridX.size(); ++i)
                {
                    compositeRest[set][i] += c.rest[i];
                    compositeLive[set][i] += c.live[i];
                }
            }
        }
    }

    // Reads the module's meters (the instance of each channel set) and smooths them for the eye.
    void readMeters (bool snap)
    {
        auto& proc = model.processor;
        live.running = proc.isModuleInChain (ps::ModuleType::DynamicEq) && proc.getParameterValue ("deq.on") >= 0.5f;
        const auto& p = model.get();

        for (int set = 0; set < ps::kNumChannelSets; ++set)
        {
            const auto* module = dynamic_cast<const ps::DynamicEq*> (&moduleForChannelSet (proc, ps::ModuleType::DynamicEq, set));
            const int pset = model.isSplit() ? set : 0;

            for (int b = 0; b < ps::kDynEqBands; ++b)
            {
                const auto& band = p.bands[pset][b];
                float gainTarget = (float) ps::dynEqRestGainDb (band), dynTarget = 0.0f, detTarget = -200.0f;

                if (module != nullptr && live.running && band.on)
                {
                    const auto& m = module->getMeters();
                    gainTarget = safeDb (m.bandGainDb[set][b].load());
                    dynTarget = safeDb (m.dynamicDb[set][b].load());
                    detTarget = safeDb (m.detectorDb[set][b].load());
                }

                auto glide = [snap] (float& v, float target)
                {
                    target = juce::jlimit (-60.0f, 30.0f, target);
                    v = snap || std::abs (target - v) < 0.01f ? target : v + (target - v) * 0.55f;
                };

                glide (live.gainDb[set][b], band.on ? gainTarget : 0.0f);
                glide (live.dynamicDb[set][b], dynTarget);
                float& d = live.detectorDb[set][b];
                d = snap || detTarget >= d ? detTarget : juce::jmax (detTarget, d - 1.0f);
            }
        }
    }

    float bandDbAtX (const std::vector<float>& db, float x) const
    {
        if (db.size() != gridX.size() || gridX.size() < 2)
            return 0.0f;

        const float t = (x - gridX.front()) / (gridX.back() - gridX.front()) * (float) (gridX.size() - 1);
        const int i = juce::jlimit (0, (int) gridX.size() - 2, (int) std::floor (t));
        const float f = juce::jlimit (0.0f, 1.0f, t - (float) i);
        return db[(size_t) i] + f * (db[(size_t) i + 1] - db[(size_t) i]);
    }

    float clampY (float y) const { return juce::jlimit (dy.area.getY(), dy.area.getBottom(), y); }

    float nodeDb (const ps::DynamicEqBandParams& band) const { return gainClamp (band.offsetDb + band.gainDb); }

    juce::Point<float> nodePos (int set, int b) const
    {
        const auto& band = model.band (set, b);
        return { fx.hzToX (band.freqHz), clampY (dy.dbToY (nodeDb (band))) };
    }

    int hitNode (juce::Point<float> pos) const
    {
        const int set = activeSet();
        int best = -1;
        float bestD = kHitRadius;

        for (int b = 0; b < ps::kDynEqBands; ++b)
        {
            if (! model.band (set, b).on)
                continue;

            float d = nodePos (set, b).getDistanceFrom (pos);

            if (b == selected)
                d -= 1.5f;

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
        for (int b = 0; b < ps::kDynEqBands; ++b)
            if (model.band (set, b).on)
                return b;

        return 0;
    }

    void select (int b)
    {
        b = juce::jlimit (0, ps::kDynEqBands - 1, b);

        if (b != selected)
        {
            selected = b;

            if (owner.onSelectionChanged != nullptr)
                owner.onSelectionChanged (b);
        }

        owner.repaint();
    }

    // The band whose threshold bar is drawn (and can be dragged): the dragged / hovered / selected band.
    // A hovered band without a threshold (static) leaves the selected band's bar in place.
    int thresholdBand() const
    {
        const auto hasBar = [this] (int b)
        {
            if (b < 0)
                return false;

            const auto& band = model.band (activeSet(), b);
            return band.on && band.dynamic;
        };

        for (int b : { thrDrag.band, drag.band, hover, selected })
            if (b >= 0)
            {
                if (hasBar (b))
                    return b;

                if (b == thrDrag.band || b == drag.band)
                    return -1;
            }

        return -1;
    }

    // x range of a band's -3 dB width (shelves: from the corner to the edge of the plot), at least 28 px wide.
    juce::Range<float> bandSpan (const ps::DynamicEqBandParams& band) const
    {
        const auto traits = dynEqShapeTraits (band.shape);
        const float xc = fx.hzToX (band.freqHz);
        float x0, x1;

        if (traits.lowShelf)
        {
            x0 = plot.getX();
            x1 = xc;
        }
        else if (traits.highShelf)
        {
            x0 = xc;
            x1 = plot.getRight();
        }
        else
        {
            const float half = octavesForQ (band.shape == ps::DynEqShape::BandShelf ? band.q * 0.6f : band.q) * 0.5f;
            x0 = fx.hzToX (freqClamp (band.freqHz * std::pow (2.0f, -half)));
            x1 = fx.hzToX (freqClamp (band.freqHz * std::pow (2.0f, half)));
        }

        if (x1 - x0 < 28.0f)
        {
            const float c = traits.lowShelf ? x1 - 14.0f : (traits.highShelf ? x0 + 14.0f : 0.5f * (x0 + x1));
            x0 = c - 14.0f;
            x1 = c + 14.0f;
        }

        return { juce::jmax (plot.getX(), x0), juce::jmin (plot.getRight(), x1) };
    }

    float thresholdY (float thr) const { return lv.dbToY (juce::jlimit (kSpectrumMinDb, kSpectrumMaxDb, thr)); }

    bool hitThreshold (juce::Point<float> pos) const
    {
        const int b = thresholdBand();

        if (b < 0 || b != selected)
            return false;

        const auto& band = model.band (activeSet(), b);
        const auto span = bandSpan (band);
        return pos.x >= span.getStart() - 4.0f && pos.x <= span.getEnd() + 4.0f
               && std::abs (pos.y - thresholdY (band.thresholdDb)) <= kThrGrab;
    }

    //==========================================================================
    // Edits (the shown set, plus the other one when linked)
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

    void checkEditRouting()
    {
        const int set = activeSet();
        const auto mode = model.channelMode();
        const bool linked = model.isLinked();
        if (set == editSet && mode == editMode && linked == editLinked)
            return;

        // A wheel burst may still be open when the user changes tabs, or the host changes mode / Link.
        // Finish the original targets before allowing an event to address the newly shown parameters.
        endWheel();
        endGesture();
        drag = {};
        thrDrag = {};
        stopMonitoring();
        editSet = set;
        editMode = mode;
        editLinked = linked;
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

    juce::String bandLabel (int b) const { return model.bandLabel (activeSet(), b); }

    // Defaults of every band setting except on / freq (a new or reset band).
    std::vector<std::pair<const char*, float>> bandDefaults (int b) const
    {
        std::vector<std::pair<const char*, float>> values;
        const int set = activeSet();

        for (const char* name : { "shape", "gain", "q", "dir", "thr", "ratio", "knee", "att", "rel", "auto", "offset", "dyn", "sc" })
            values.emplace_back (name, dynEqDefaultOf (model.bandId (set, b, name)));

        return values;
    }

    void addBandAt (juce::Point<float> pos)
    {
        const int set = activeSet();
        int b = -1;

        for (int i = 0; i < ps::kDynEqBands; ++i)
            if (! model.band (set, i).on)
            {
                b = i;
                break;
            }

        if (b < 0)
        {
            services.showHint ("All 6 bands are in use");
            return;
        }

        const float hz = freqClamp (fx.xToHz (pos.x));
        const auto shape = dynEqDefaultShapeForNewNode (model.get().bands[set], (double) hz);
        const float gain = gainClamp (std::round (dy.yToDb (pos.y) * 10.0f) / 10.0f);
        auto values = bandDefaults (b);

        for (auto& [name, v] : values)
        {
            if (juce::String (name) == "shape") v = (float) shape;
            if (juce::String (name) == "gain")  v = gain;
        }

        values.emplace_back ("on", 1.0f);
        values.emplace_back ("freq", hz);
        applyBandValues (b, values, "Added " + bandLabel (b) + ": " + dynEqShapeFullName (shape) + " at " + fmt::hz (hz));
        model.update();
        owner.parametersChanged();
        select (b);
    }

    //==========================================================================
    // Menus
    void showNodeMenu (int b)
    {
        const auto& band = model.band (activeSet(), b);

        juce::PopupMenu menu;
        menu.addSectionHeader ("Band " + juce::String (b + 1) + "  " + dynEqShapeFullName (band.shape));
        menu.addSubMenu ("Shape", dynEqShapeMenu (band.shape));

        juce::PopupMenu direction;
        direction.addItem (100, "Down: moves toward the gain when loud", true, band.direction == ps::DynEqDirection::Down);
        direction.addItem (101, "Up: moves away from the gain when loud", true, band.direction == ps::DynEqDirection::Up);
        menu.addSubMenu ("Direction", direction, band.dynamic);

        juce::PopupMenu sidechain;

        for (int i = 0; i < (int) ps::DynEqSidechain::Count; ++i)
        {
            const auto sc = (ps::DynEqSidechain) i;
            auto label = dynEqSidechainName (sc);

            if (sc == ps::DynEqSidechain::OtherChannel && ! model.hasOtherChannel())
                label << " (M/S, L/R only)";

            sidechain.addItem (200 + i, label, true, band.sidechain == sc);

            if (i == 0 || i == (int) ps::DynEqSidechain::Band6)
                sidechain.addSeparator();
        }

        menu.addSubMenu ("Listen to", sidechain, band.dynamic);
        menu.addSeparator();
        menu.addItem (3, "Dynamic", true, band.dynamic);
        menu.addItem (4, "Auto scale attack / release", band.dynamic, band.autoScale);
        menu.addSeparator();
        menu.addItem (2, "Reset band");
        menu.addItem (1, "Turn band off");

        showMenuAt (menu, owner, juce::Desktop::getMousePosition(), [this, b] (int result)
        {
            if (result <= 0)
                return;

            const auto& current = model.band (activeSet(), b);

            if (result == 1)
                applyBandValues (b, { { "on", 0.0f } }, "Turned off " + bandLabel (b));
            else if (result == 2)
                applyBandValues (b, bandDefaults (b), "Reset " + bandLabel (b));
            else if (result == 3)
                applyBandValues (b, { { "dyn", current.dynamic ? 0.0f : 1.0f } },
                                 bandLabel (b) + (current.dynamic ? " Dynamic: off" : " Dynamic: on"));
            else if (result == 4)
                applyBandValues (b, { { "auto", current.autoScale ? 0.0f : 1.0f } },
                                 bandLabel (b) + (current.autoScale ? " Auto Scale: off" : " Auto Scale: on"));
            else if (result >= 1 + (int) ps::DynEqShape::Bell && result <= (int) ps::DynEqShape::Count)
            {
                const auto shape = (ps::DynEqShape) (result - 1);

                if (shape != current.shape)
                    applyBandValues (b, { { "shape", (float) shape } }, bandLabel (b) + " Shape: " + dynEqShapeFullName (shape));
            }
            else if (result == 100 || result == 101)
            {
                const int dir = result - 100;

                if (dir != (int) current.direction)
                    applyBandValues (b, { { "dir", (float) dir } }, bandLabel (b) + (dir == 0 ? " Direction: Down" : " Direction: Up"));
            }
            else if (result >= 200 && result < 200 + (int) ps::DynEqSidechain::Count)
            {
                const auto sc = (ps::DynEqSidechain) (result - 200);

                if (sc != current.sidechain)
                    applyBandValues (b, { { "sc", (float) sc } }, bandLabel (b) + " Sidechain: " + dynEqSidechainName (sc));
            }

            model.update();
            owner.parametersChanged();
        }, 220);
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

    void showThresholdMenu (int b)
    {
        const auto id = model.bandId (activeSet(), b, "thr");
        const float def = dynEqDefaultOf (id);
        juce::PopupMenu menu;
        menu.addSectionHeader ("Band " + juce::String (b + 1) + " threshold");
        menu.addItem (1, "Reset to default (" + fmt::db (def, 1, false) + ")");
        menu.addItem (2, "Enter value...");

        showMenuAt (menu, owner, juce::Desktop::getMousePosition(), [this, b, def] (int result)
        {
            if (result == 1)
                applyBandValues (b, { { "thr", def } }, bandLabel (b) + " Threshold: " + fmt::db (def, 1, false));
            else if (result == 2)
                promptThreshold (b);
        }, 180);
    }

    void promptThreshold (int b)
    {
        const auto id = model.bandId (activeSet(), b, "thr");
        const auto* spec = stereo::specOf (id);

        if (spec == nullptr)
            return;

        juce::Component::SafePointer<juce::Component> safe (&owner);
        promptForText (owner, bandLabel (b) + " Threshold",
                       "Enter a value (" + juce::String (spec->minValue) + " to " + juce::String (spec->maxValue) + " dB)",
                       fmt::paramEditValue (model.processor, id), [this, safe, spec, b] (juce::String text)
                       {
                           float v = 0.0f;

                           if (safe != nullptr && parseValueText (*spec, text, v))
                           {
                               v = juce::jlimit (spec->minValue, spec->maxValue, v);
                               applyBandValues (b, { { "thr", v } }, bandLabel (b) + " Threshold: " + fmt::db (v, 1, false));
                           }
                       });
    }

    //==========================================================================
    // Monitoring: Alt-drag audition (empty display) and band solo (Alt-click a node)
    void startAudition (juce::Point<float> pos)
    {
        audition.on = true;
        audition.virt = pos;
        audition.last = pos;
        audition.q = freeAuditionQ;
        sendMonitor();
    }

    void sendMonitor()
    {
        auto m = model.processor.getMonitor (ps::ModuleType::DynamicEq);
        m.auditionOn = audition.on;
        m.auditionHz = freqClamp (fx.xToHz (audition.virt.x));
        m.auditionQ = juce::jlimit (0.2f, 12.0f, audition.q);
        m.auditionChannelSet = activeSet();
        m.soloBand = soloBand;
        m.soloChannelSet = soloBand >= 0 && model.channelMode() == ps::ChannelMode::TransientSustain ? activeSet() : -1;
        model.processor.setMonitor (ps::ModuleType::DynamicEq, m);
        owner.repaint();
    }

    void stopMonitoring()
    {
        if (! audition.on && soloBand < 0)
            return;

        audition.on = false;
        soloBand = -1;
        sendMonitor();
    }

    //==========================================================================
    // Painting
    juce::Path curvePath (const std::vector<float>& db) const
    {
        juce::Path p;
        const float lo = dy.area.getY() - 40.0f, hi = dy.area.getBottom() + 40.0f;

        for (size_t i = 0; i < gridX.size() && i < db.size(); ++i)
        {
            const float y = juce::jlimit (lo, hi, dy.dbToY (db[i]));

            if (i == 0)
                p.startNewSubPath (gridX[i], y);
            else
                p.lineTo (gridX[i], y);
        }

        return p;
    }

    // Region between two curves (a band's rest and full-trigger responses).
    juce::Path rangePath (const std::vector<float>& a, const std::vector<float>& b) const
    {
        juce::Path p;
        const size_t n = juce::jmin (gridX.size(), a.size(), b.size());

        if (n < 2)
            return p;

        const float lo = dy.area.getY() - 40.0f, hi = dy.area.getBottom() + 40.0f;
        p.startNewSubPath (gridX[0], juce::jlimit (lo, hi, dy.dbToY (a[0])));

        for (size_t i = 1; i < n; ++i)
            p.lineTo (gridX[i], juce::jlimit (lo, hi, dy.dbToY (a[i])));

        for (size_t i = n; i-- > 0;)
            p.lineTo (gridX[i], juce::jlimit (lo, hi, dy.dbToY (b[i])));

        p.closeSubPath();
        return p;
    }

    void paintNode (juce::Graphics& g, int b, juce::Point<float> c, bool isSel, bool isHover,
                    const ps::DynamicEqBandParams& band) const
    {
        const auto colour = eqBandColour (b);
        const float r = kNodeRadius + (isHover || isSel ? 1.0f : 0.0f);

        // Direction arrow: where the curve goes when the level rises (Up: above, Down: below the node).
        if (band.dynamic)
        {
            const bool up = band.direction == ps::DynEqDirection::Up;
            const float ay = up ? c.y - r - 5.0f : c.y + r + 5.0f;
            juce::Path arrow;
            const float s = up ? -1.0f : 1.0f;
            arrow.addTriangle (c.x - 4.5f, ay - 2.0f * s, c.x + 4.5f, ay - 2.0f * s, c.x, ay + 3.5f * s);
            g.setColour (Colours::background.withAlpha (0.7f));
            g.strokePath (arrow, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (colour.withAlpha (isSel || isHover ? 1.0f : 0.85f));
            g.fillPath (arrow);
        }

        paintEqNode (g, b, c, kNodeRadius, isSel, isHover);
    }

    // The band's travel (rest .. full trigger) as a rail at the node's frequency, with a dot at the live gain.
    void paintRail (juce::Graphics& g, int set, int b) const
    {
        const auto& band = model.band (set, b);
        const auto& c = curves[set][b];

        if (! band.dynamic || ! c.valid || std::abs (c.trigGain - c.restGain) < 0.05f)
            return;

        const float x = fx.hzToX (band.freqHz);
        const float y0 = clampY (dy.dbToY (c.restGain)), y1 = clampY (dy.dbToY (c.trigGain));
        const auto colour = eqBandColour (b);
        g.setColour (colour.withAlpha (0.38f));
        g.fillRoundedRectangle (juce::Rectangle<float> (x - 1.25f, juce::jmin (y0, y1), 2.5f, std::abs (y1 - y0)), 1.25f);

        // rest end: a small tick
        g.fillRoundedRectangle (juce::Rectangle<float> (8.0f, 2.0f).withCentre ({ x, y0 }), 1.0f);

        const float yl = clampY (dy.dbToY (liveGainOf (set, b)));
        const auto dot = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x, yl });
        g.setColour (Colours::background.withAlpha (0.85f));
        g.fillEllipse (dot.expanded (1.5f));
        g.setColour (colour.brighter (0.25f));
        g.fillEllipse (dot);
    }

    void paintThreshold (juce::Graphics& g, int set, int b) const
    {
        const auto& band = model.band (set, b);
        const auto colour = eqBandColour (b);
        const auto span = bandSpan (band);
        const float x0 = span.getStart(), w = span.getLength();
        const float bottom = lv.area.getBottom();
        const float thrY = std::round (thresholdY (band.thresholdDb));
        const float det = live.detectorDb[set][b];
        const bool active = thrDrag.band == b || (thrDrag.band < 0 && hoverThr);

        // Level ruler (dBFS) along the right edge: the scale of the threshold and the detector level.
        {
            g.setFont (font (9.5f, 600));

            for (float db : { -20.0f, -40.0f, -60.0f, -80.0f })
            {
                const float y = std::round (lv.dbToY (db));
                g.setColour (Colours::outline.withAlpha (0.55f));
                g.fillRect (juce::Rectangle<float> (plot.getRight() - 5.0f, y, 4.0f, 1.0f));
                g.setColour (Colours::textSubtle.withAlpha (0.9f));
                g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (plot.getRight() - 40.0f, y - 6.0f, 33.0f, 12.0f),
                            juce::Justification::centredRight, false);
            }
        }

        // Detector level: translucent column over the band's width, the part above the threshold in the band colour.
        if (det > kSpectrumMinDb)
        {
            const float y = lv.dbToY (juce::jmin (det, kSpectrumMaxDb));
            g.setGradientFill (juce::ColourGradient::vertical (juce::Colours::white.withAlpha (0.07f), y,
                                                               juce::Colours::white.withAlpha (0.0f), bottom));
            g.fillRect (juce::Rectangle<float> (x0, y, w, bottom - y));

            if (y < thrY - 1.0f)
            {
                g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (0.17f), y, colour.withAlpha (0.03f), thrY));
                g.fillRect (juce::Rectangle<float> (x0, y, w, thrY - y));
            }

            g.setColour ((y < thrY ? colour.brighter (0.3f) : juce::Colours::white).withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (x0, y - 0.5f, w, 1.0f));
        }

        // Threshold bar
        if (active)
        {
            g.setColour (colour.withAlpha (0.16f));
            g.fillRect (juce::Rectangle<float> (x0, thrY - 5.0f, w, 10.0f));
        }

        g.setColour (colour.withAlpha (active ? 1.0f : 0.9f));
        g.fillRect (juce::Rectangle<float> (x0, thrY - (active ? 1.0f : 0.75f), w, active ? 2.0f : 1.5f));
        const auto grab = juce::Rectangle<float> (active ? 22.0f : 16.0f, active ? 7.0f : 5.0f).withCentre ({ x0 + w * 0.5f, thrY });
        g.fillRoundedRectangle (grab, grab.getHeight() * 0.5f);

        // Label chip "Thr -30.0"
        const auto f = font (10.5f, 700);
        const auto text = "Thr " + dbShort (band.thresholdDb);
        const float tw = textWidth (f, text) + 12.0f;
        auto chip = juce::Rectangle<float> (tw, 15.0f).withPosition (x0 + w + 4.0f, thrY - 7.5f);

        if (chip.getRight() > plot.getRight() - 44.0f)
            chip.setX (x0 - 4.0f - tw);

        if (chip.getX() < plot.getX() + 40.0f)
            chip.setPosition (x0 + w * 0.5f - tw * 0.5f, thrY - 20.0f);

        chip = chip.constrainedWithin (plot.reduced (4.0f));
        g.setColour (Colours::background.withAlpha (0.85f));
        g.fillRoundedRectangle (chip, 4.0f);
        g.setColour (colour.withAlpha (active ? 1.0f : 0.6f));
        g.drawRoundedRectangle (chip.reduced (0.5f), 4.0f, 1.0f);
        g.setFont (f);
        g.setColour (colour.brighter (0.2f));
        g.drawText (text, chip, juce::Justification::centred, false);
    }

    void paintReadout (juce::Graphics& g, int set, int b, juce::Point<float> node) const
    {
        const auto& band = model.band (set, b);
        const auto colour = eqBandColour (b);

        juce::StringArray parts;
        parts.add (fmt::hz (band.freqHz));
        parts.add (fmt::db (band.gainDb));

        if (dynEqShapeTraits (band.shape).q)
            parts.add (qText (band.q));

        if (std::abs (band.offsetDb) >= 0.05f)
            parts.add ("offset " + dbShort (band.offsetDb));

        juce::String second;

        if (band.dynamic)
            second = "Thr " + fmt::db (band.thresholdDb, 1, false) + "   " + fmt::ratio (band.ratio) + "   now "
                     + fmt::db (liveGainOf (set, b));

        const auto title = juce::String (b + 1) + "  " + dynEqShapeShortName (band.shape) + "  "
                           + (band.dynamic ? (band.direction == ps::DynEqDirection::Down ? "Down" : "Up") : "Static");
        const auto line = parts.joinIntoString ("   ");
        const auto titleFont = font (11.5f, 700), lineFont = font (11.5f, 600);
        const float w = juce::jmax (textWidth (titleFont, title), textWidth (lineFont, line), textWidth (lineFont, second)) + 20.0f;
        const float h = second.isEmpty() ? 38.0f : 53.0f;

        auto box = juce::Rectangle<float> (w, h).withCentre ({ node.x, node.y - kNodeRadius - 14.0f - h * 0.5f });

        if (box.getY() < plot.getY() + 4.0f)
            box.setY (node.y + kNodeRadius + 14.0f);

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
        g.drawText (line, inner.removeFromTop (15.0f), juce::Justification::centredLeft, false);

        if (second.isNotEmpty())
        {
            g.setColour (Colours::textDim);
            g.drawText (second, inner.removeFromTop (15.0f), juce::Justification::centredLeft, false);
        }
    }

    void paintPill (juce::Graphics& g, const juce::String& text, float centreX) const
    {
        const auto f = font (11.0f, 700);
        const float tw = textWidth (f, text) + 16.0f;
        auto pill = juce::Rectangle<float> (tw, 20.0f).withCentre ({ centreX, plot.getY() + 40.0f }).constrainedWithin (plot.reduced (6.0f));
        g.setColour (Colours::minor.withAlpha (0.9f));
        g.fillRoundedRectangle (pill, 10.0f);
        g.setColour (Colours::background);
        g.setFont (f);
        g.drawText (text, pill, juce::Justification::centred, false);
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
        FreqAxis specAxis = fx;
        specAxis.area = lv.area;
        g.beginTransparencyLayer (0.6f);
        services.getModulePreSpectrum().draw (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb, Colours::input, options.spectrumFill);
        g.endTransparencyLayer();
        g.beginTransparencyLayer (0.42f);
        services.getModulePostSpectrum().draw (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb,
                                               blend (setColour (activeSet()), Colours::input, 0.3f), false);
        g.endTransparencyLayer();
        services.drawReferenceSpectrum (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb);   // (Track Referencing)

        const int set = activeSet();
        const auto colour = setColour (set);
        const auto& p = model.get();

        // The other channel set's live curve, dimmed (M/S, L/R, T/S)
        if (model.isSplit())
        {
            const int other = 1 - set;
            bool any = false;

            for (int i = 0; i < ps::kDynEqBands; ++i)
                any = any || p.bands[other][i].on;

            if (any)
            {
                g.setColour (setColour (other).withAlpha (0.42f));
                const float dashes[] { 5.0f, 4.0f };
                juce::Path dashed;
                juce::PathStrokeType (1.5f).createDashedStroke (dashed, curvePath (compositeLive[other]), dashes, 2);
                g.fillPath (dashed);
            }
        }

        // Per-band travel ranges (rest .. full trigger) and live areas
        const int focus = drag.band >= 0 ? drag.band : (hover >= 0 ? hover : selected);

        for (int i = 0; i < ps::kDynEqBands; ++i)
        {
            const auto& c = curves[set][i];

            if (! p.bands[set][i].on || ! c.valid)
                continue;

            const bool isFocus = i == focus;
            const auto bc = eqBandColour (i);

            if (std::abs (c.trigGain - c.restGain) >= 0.05f)
            {
                g.setColour (bc.withAlpha (isFocus ? 0.17f : 0.075f));
                g.fillPath (rangePath (c.rest, c.trig));

                if (isFocus)
                {
                    const float dashes[] { 4.0f, 3.0f };
                    juce::Path dashed;
                    juce::PathStrokeType (1.1f).createDashedStroke (dashed, curvePath (c.trig), dashes, 2);
                    g.setColour (bc.withAlpha (0.7f));
                    g.fillPath (dashed);
                }
            }

            fillEqBand (g, gridX, c.live, dy, bc, isFocus);
        }

        // Threshold and detector level of the focused dynamic band
        if (const int tb = thresholdBand(); tb >= 0 && ! audition.on)
            paintThreshold (g, set, tb);

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
            paintPill (g, "Listening around " + fmt::hz (hz), xc);
        }
        else if (soloBand >= 0)
        {
            paintPill (g, "Listening to what band " + juce::String (soloBand + 1) + " hears", fx.hzToX (p.bands[set][soloBand].freqHz));
        }

        // Composite curves of the shown set: rest (thin, dashed) and live (thick)
        bool anyOn = false, anyMoving = false;

        for (int i = 0; i < ps::kDynEqBands; ++i)
        {
            anyOn = anyOn || p.bands[set][i].on;
            anyMoving = anyMoving || (p.bands[set][i].on && curves[set][i].valid
                                      && std::abs (curves[set][i].trigGain - curves[set][i].restGain) >= 0.05f);
        }

        if (anyOn)
        {
            if (anyMoving)
            {
                const float dashes[] { 3.0f, 4.0f };
                juce::Path dashed;
                juce::PathStrokeType (1.2f).createDashedStroke (dashed, curvePath (compositeRest[set]), dashes, 2);
                g.setColour (Colours::text.withAlpha (0.42f));
                g.fillPath (dashed);
            }

            strokeEqCurve (g, curvePath (compositeLive[set]), colour);
        }

        // Rails, then nodes (the focused one on top)
        for (int i = 0; i < ps::kDynEqBands; ++i)
            if (p.bands[set][i].on)
                paintRail (g, set, i);

        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < ps::kDynEqBands; ++i)
            {
                if (! p.bands[set][i].on || (i == focus) != (pass == 1))
                    continue;

                paintNode (g, i, nodePos (set, i), i == selected, i == hover || i == drag.band, p.bands[set][i]);
            }

        // Readout of the hovered / dragged node, or the cursor position
        const int readout = drag.band >= 0 ? drag.band : hover;

        if (readout >= 0 && p.bands[set][readout].on && ! audition.on)
            paintReadout (g, set, readout, nodePos (set, readout));
        else if (mouseInside && hover < 0 && drag.band < 0 && thrDrag.band < 0 && ! hoverThr && ! audition.on
                 && plot.contains (mousePos))
        {
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
            auto legend = juce::Rectangle<float> ((float) range.getX() - 240.0f, (float) range.getY(), 230.0f, (float) range.getHeight());
            auto item = [&] (const juce::String& text, juce::Colour c, bool dashed)
            {
                const auto f = font (10.5f, 600);
                const float w = textWidth (f, text) + 16.0f;
                auto r = legend.removeFromRight (w);
                legend.removeFromRight (10.0f);
                auto swatch = r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, dashed ? 1.5f : 3.0f);
                g.setColour (c);

                if (dashed)
                {
                    g.fillRect (swatch.withWidth (3.0f));
                    g.fillRect (swatch.withTrimmedLeft (6.0f));
                }
                else
                {
                    g.fillRoundedRectangle (swatch, 1.5f);
                }

                g.setFont (f);
                g.setColour (Colours::textDim);
                g.drawText (text, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
            };

            if (anyMoving)
                item ("Rest", Colours::text.withAlpha (0.6f), true);

            item ("Output", setColour (set), false);
            item ("Input", Colours::input, false);
        }

        // Digital mode: band kernels still being designed
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
            const auto sub = juce::String ("Double-click anywhere on the display to add a dynamic band");
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

    void endWheel()
    {
        if (! wheel.active)
            return;

        wheel.active = false;
        wheel.band = -1;
        endGesture();
    }

    //==========================================================================
    DynamicEqDisplay& owner;
    DynamicEqModel& model;
    EditorServices& services;
    std::function<int()> channelSetFn;
    RangeSelector range;

    juce::Rectangle<float> plot;
    FreqAxis fx;
    DbAxis dy, lv;
    std::vector<float> gridX, gridHz;
    double gridSampleRate = 0.0;
    BandCurves curves[2][ps::kDynEqBands];
    std::vector<float> compositeRest[2], compositeLive[2];
    DynamicEqLive live;

    int selected = 0, hover = -1;
    bool hoverThr = false;
    juce::Point<float> mousePos;
    bool mouseInside = false;
    bool designPending = false;

    struct Drag
    {
        int band = -1;
        bool begun = false;
        juce::Point<float> start, virt, last;
        float startGain = 0.0f, startDb = 0.0f;
    } drag;

    struct ThresholdDrag
    {
        int band = -1;
        bool begun = false;
        float db = 0.0f, lastY = 0.0f;
    } thrDrag;

    struct Audition
    {
        bool on = false;
        float q = 4.0f;
        juce::Point<float> virt, last;
    } audition;
    float freeAuditionQ = 4.0f;
    int soloBand = -1;

    struct Wheel
    {
        bool active = false;
        int band = -1;
        const char* name = nullptr;
        float norm = 0.0f;
        juce::uint32 lastMs = 0;
    } wheel;

    std::vector<juce::RangedAudioParameter*> gestureParams;
    int editSet = -1;
    ps::ChannelMode editMode = ps::ChannelMode::Stereo;
    bool editLinked = false;
};

//==============================================================================
DynamicEqDisplay::DynamicEqDisplay (DynamicEqModel& m, EditorServices& s, std::function<int()> channelSet)
    : impl (std::make_unique<Impl> (*this, m, s, std::move (channelSet)))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (false);
    setTooltip ("Double-click to add a band. Drag a band to move it (Shift: fine, Ctrl: one direction only), mouse wheel "
                "for its width (Q), Ctrl + wheel for its threshold. Drag the threshold bar of the selected band up or "
                "down. Alt-drag to listen to one frequency range, Alt-click a band to hear what it listens to. "
                "Right-click a band for its shape, direction and options.");
}

DynamicEqDisplay::~DynamicEqDisplay()
{
    impl->endWheel();
    impl->endGesture();
    impl->stopMonitoring();
}

const DynamicEqLive& DynamicEqDisplay::getLive() const noexcept { return impl->live; }

void DynamicEqDisplay::parametersChanged()
{
    impl->checkEditRouting();
    impl->recompute();
    repaint();
}

void DynamicEqDisplay::tick()
{
    auto& d = *impl;
    d.checkEditRouting();
    d.readMeters (false);

    bool pending = false;

    if (d.model.isDigital())
        if (const auto* deq = dynamic_cast<const ps::DynamicEq*> (&d.model.processor.getChain().getModule (ps::ModuleType::DynamicEq)))
            pending = deq->isDesignPending();

    d.designPending = pending;

    if (d.wheel.active && juce::Time::getMillisecondCounter() - d.wheel.lastMs > kWheelGestureMs)
        d.endWheel();   // the wheel burst ends (one undo step) shortly after the last wheel event

    d.recompute();
    repaint();
}

juce::String DynamicEqDisplay::getTooltip()
{
    if (impl->hover >= 0 || impl->hoverThr || impl->drag.band >= 0 || impl->thrDrag.band >= 0 || impl->audition.on)
        return {};

    return juce::SettableTooltipClient::getTooltip();
}

juce::Point<float> DynamicEqDisplay::getNodePosition (int band) const
{
    return impl->nodePos (impl->activeSet(), juce::jlimit (0, ps::kDynEqBands - 1, band));
}

juce::Point<float> DynamicEqDisplay::getThresholdHandlePosition() const
{
    const auto& d = *impl;
    const auto& band = d.model.band (d.activeSet(), d.selected);

    if (! band.on || ! band.dynamic)
        return { -1.0f, -1.0f };

    return { d.bandSpan (band).getStart() + d.bandSpan (band).getLength() * 0.5f, d.thresholdY (band.thresholdDb) };
}

int DynamicEqDisplay::getSelectedBand() const noexcept { return impl->selected; }

void DynamicEqDisplay::setSelectedBand (int band)
{
    impl->selected = juce::jlimit (0, ps::kDynEqBands - 1, band);
    repaint();
}

int DynamicEqDisplay::getDisplayRangeDb() const noexcept { return impl->range.range; }

void DynamicEqDisplay::setDisplayRangeDb (int r)
{
    impl->range.range = r;
    rememberedRange() = r;
    impl->range.repaint();
    impl->layout();
    impl->recompute();
    repaint();
}

void DynamicEqDisplay::paint (juce::Graphics& g) { impl->paint (g); }

void DynamicEqDisplay::resized()
{
    impl->layout();
    impl->recompute();
}

void DynamicEqDisplay::mouseMove (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.mouseInside = true;
    d.mousePos = e.position;
    d.hover = d.hitNode (e.position);
    d.hoverThr = d.hover < 0 && d.hitThreshold (e.position);
    setMouseCursor (d.hover >= 0 ? juce::MouseCursor::PointingHandCursor
                                 : (d.hoverThr ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor));
    repaint();
}

void DynamicEqDisplay::mouseExit (const juce::MouseEvent&)
{
    impl->mouseInside = false;
    impl->hover = -1;
    impl->hoverThr = false;
    repaint();
}

void DynamicEqDisplay::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.model.update();
    d.checkEditRouting();
    const int node = d.hitNode (e.position);

    if (e.mods.isPopupMenu())
    {
        if (node >= 0)
        {
            d.select (node);
            d.showNodeMenu (node);
        }
        else if (d.hitThreshold (e.position))
        {
            d.showThresholdMenu (d.selected);
        }
        else
        {
            d.showBackgroundMenu (e.position);
        }

        return;
    }

    if (! isLeftClick (e))
        return;

    d.endWheel();   // a wheel burst still open ends before anything else starts

    if (e.mods.isAltDown())
    {
        if (node >= 0)
        {
            d.select (node);
            d.soloBand = node;
            d.sendMonitor();
        }
        else
        {
            d.startAudition (e.position);
        }

        return;
    }

    if (node < 0)
    {
        if (d.hitThreshold (e.position))
        {
            d.thrDrag = {};
            d.thrDrag.band = d.selected;
            d.thrDrag.db = d.model.band (d.activeSet(), d.selected).thresholdDb;
            d.thrDrag.lastY = e.position.y;
            repaint();
        }

        return;
    }

    d.select (node);
    const auto& band = d.model.band (d.activeSet(), node);
    d.drag = {};
    d.drag.band = node;
    d.drag.start = d.nodePos (d.activeSet(), node);
    d.drag.virt = d.drag.start;
    d.drag.last = e.position;
    d.drag.startGain = band.gainDb;
    d.drag.startDb = d.dy.yToDb (d.drag.start.y);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void DynamicEqDisplay::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.model.update();
    d.checkEditRouting();
    if (! isLeftClick (e))
        return;
    d.mousePos = e.position;
    const bool fine = e.mods.isShiftDown();

    if (d.audition.on)
    {
        const auto delta = e.position - d.audition.last;
        d.audition.last = e.position;
        d.audition.virt.x = juce::jlimit (d.plot.getX(), d.plot.getRight(), d.audition.virt.x + delta.x * (fine ? 0.1f : 1.0f));
        d.sendMonitor();
        return;
    }

    if (d.thrDrag.band >= 0)
    {
        const float dyPx = e.position.y - d.thrDrag.lastY;
        d.thrDrag.lastY = e.position.y;

        if (std::abs (dyPx) < 1.0e-4f)
            return;

        if (! d.thrDrag.begun)
        {
            d.thrDrag.begun = true;
            d.beginGesture (d.thrDrag.band, { "thr" });
        }

        const float dbPerPx = (d.lv.maxDb - d.lv.minDb) / juce::jmax (1.0f, d.lv.area.getHeight());
        d.thrDrag.db = juce::jlimit (-130.0f, 0.0f, d.thrDrag.db - dyPx * dbPerPx * (fine || e.mods.isCtrlDown() ? 0.1f : 1.0f));
        d.setBand (d.thrDrag.band, "thr", d.thrDrag.db);
        d.model.update();
        parametersChanged();
        return;
    }

    if (d.drag.band < 0)
        return;

    const auto delta = e.position - d.drag.last;
    d.drag.last = e.position;
    d.drag.virt += delta * (fine ? 0.1f : 1.0f);
    d.drag.virt.x = juce::jlimit (d.plot.getX(), d.plot.getRight(), d.drag.virt.x);
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

    if (! d.drag.begun)
    {
        if (e.getDistanceFromDragStart() < 2)
            return;

        d.drag.begun = true;
        d.beginGesture (b, { "freq", "gain" });
    }

    d.setBand (b, "freq", freqClamp (d.fx.xToHz (target.x)));

    // Gain: relative to where the drag started (no jump when the node sits at the edge of a small display range); the
    // virtual position stops where the value hits its limit, so there is no dead zone on the way back.
    const float gain = d.drag.startGain + (d.dy.yToDb (target.y) - d.drag.startDb);
    const float clamped = gainClamp (gain);

    if (clamped != gain && target.y == d.drag.virt.y)
        d.drag.virt.y = d.dy.dbToY (d.drag.startDb + (clamped - d.drag.startGain));

    d.setBand (b, "gain", clamped);
    d.model.update();
    parametersChanged();
}

void DynamicEqDisplay::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (d.audition.on)
        d.freeAuditionQ = d.audition.q;

    d.stopMonitoring();

    if (d.drag.band >= 0 || d.thrDrag.band >= 0)
    {
        d.endGesture();
        d.drag = {};
        d.thrDrag = {};
    }

    d.hover = d.hitNode (e.position);
    d.hoverThr = d.hover < 0 && d.hitThreshold (e.position);
    setMouseCursor (d.hover >= 0 ? juce::MouseCursor::PointingHandCursor
                                 : (d.hoverThr ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor));
    repaint();
}

void DynamicEqDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e) || e.mods.isAltDown())
        return;

    const int node = d.hitNode (e.position);

    if (node < 0)
    {
        if (d.hitThreshold (e.position))
        {
            d.promptThreshold (d.selected);
        }
        else if (d.plot.contains (e.position))
        {
            d.addBandAt (e.position);
        }

        d.model.update();
        parametersChanged();
        return;
    }

    if (d.model.band (d.activeSet(), node).gainDb != 0.0f)
        d.applyBandValues (node, { { "gain", 0.0f } }, d.bandLabel (node) + " Gain: 0.0 dB");

    d.model.update();
    parametersChanged();
}

void DynamicEqDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    auto& d = *impl;
    d.model.update();
    d.checkEditRouting();
    const float delta = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);

    if (std::abs (delta) < 1.0e-4f)
        return;

    const bool fine = e.mods.isShiftDown();

    // Audition: the wheel sets the width of the range heard.
    if (d.audition.on)
    {
        d.audition.q = juce::jlimit (0.2f, 12.0f, d.audition.q * std::pow (2.0f, delta * (fine ? 0.5f : 2.0f)));
        d.sendMonitor();
        return;
    }

    if (d.drag.band >= 0 || d.thrDrag.band >= 0)
        return;

    const int node = d.wheel.active ? d.wheel.band : d.hitNode (e.position);

    if (node < 0)
        return;

    const auto& band = d.model.band (d.activeSet(), node);
    const char* name = e.mods.isCtrlDown() || e.mods.isCommandDown() ? "thr" : "q";

    if (d.wheel.active && d.wheel.name != nullptr && juce::String (d.wheel.name) != name)
        d.endWheel();   // switched between Q and threshold: a new undo step

    if (juce::String (name) == "q" && ! dynEqShapeTraits (band.shape).q)
    {
        d.services.showHint (dynEqShapeShortName (band.shape) + " has no width to change");
        return;
    }

    if (! d.wheel.active)
    {
        d.select (node);
        d.wheel.active = true;
        d.wheel.band = node;
        d.wheel.name = name;

        if (auto* prm = d.model.bandParam (d.activeSet(), node, name))
            d.wheel.norm = prm->getValue();

        d.beginGesture (node, { name });
    }

    if (auto* prm = d.model.bandParam (d.activeSet(), node, name))
    {
        // Wheel up = narrower (higher Q) / higher threshold.
        d.wheel.norm = juce::jlimit (0.0f, 1.0f, d.wheel.norm + delta * (fine ? 0.012f : 0.07f));
        d.setBand (node, name, prm->convertFrom0to1 (d.wheel.norm));
    }

    d.wheel.lastMs = juce::Time::getMillisecondCounter();
    d.model.update();
    parametersChanged();
}

//==============================================================================
// GAIN column
//==============================================================================
DynamicEqGainColumn::DynamicEqGainColumn (DynamicEqModel& m) : model (m)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTooltip ("Gain of each band right now (offset + dynamic). The faint range is how far the band can travel, from "
                "rest to fully triggered. Click a bar to select its band.");
}

void DynamicEqGainColumn::update (int s, const DynamicEqLive& live, int selectedBand)
{
    set = s & 1;
    selected = selectedBand;

    for (int b = 0; b < ps::kDynEqBands; ++b)
    {
        const auto& band = model.band (set, b);
        on[b] = band.on;
        rest[b] = (float) ps::dynEqRestGainDb (band);
        trig[b] = (float) ps::dynEqTriggeredGainDb (band);
        gain[b] = band.on ? (live.running ? live.gainDb[set][b] : rest[b]) : 0.0f;
    }

    repaint();
}

juce::Rectangle<float> DynamicEqGainColumn::barArea (int band) const
{
    auto r = getLocalBounds().toFloat().reduced (8.0f, 0.0f);
    r.removeFromTop (36.0f);    // title row
    r.removeFromBottom (26.0f);
    r.removeFromLeft (24.0f);
    const float gap = 3.0f;
    const float w = juce::jmin (18.0f, (r.getWidth() - gap * (float) (ps::kDynEqBands - 1)) / (float) ps::kDynEqBands);
    const float total = w * (float) ps::kDynEqBands + gap * (float) (ps::kDynEqBands - 1);
    const float x = r.getX() + (r.getWidth() - total) * 0.5f + (float) band * (w + gap);
    return { x, r.getY(), w, r.getHeight() };
}

float DynamicEqGainColumn::yOf (float db) const
{
    const auto first = barArea (0);
    return first.getY() + (15.0f - juce::jlimit (-30.0f, 15.0f, db)) / 45.0f * first.getHeight();
}

void DynamicEqGainColumn::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), b.getY(),
                                                       Colours::background.darker (0.05f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

    // Title + the selected band's gain
    {
        auto title = b.reduced (10.0f, 0.0f).removeFromTop (28.0f).translated (0.0f, 1.0f);
        g.setFont (font (10.5f, 700).withExtraKerningFactor (0.08f));
        g.setColour (Colours::text.withAlpha (0.9f));
        g.drawText ("GAIN", title, juce::Justification::centredLeft, false);

        const int s = juce::jlimit (0, ps::kDynEqBands - 1, selected);

        if (on[s])
        {
            const float v = gain[s];
            g.setFont (font (11.0f, 700));
            g.setColour (eqBandColour (s));
            g.drawText ((std::abs (v) < 0.05f ? juce::String ("0.0") : (v > 0.0f ? "+" : "") + juce::String (v, 1)) + " dB",
                        title, juce::Justification::centredRight, false);
        }
    }

    const float zeroY = yOf (0.0f);
    g.setFont (font (9.5f, 600));

    for (float db : { 15.0f, 0.0f, -15.0f, -30.0f })
    {
        const float y = std::round (yOf (db));
        g.setColour (Colours::textSubtle);
        g.drawText (db > 0.0f ? "+15" : juce::String (juce::roundToInt (db)),
                    juce::Rectangle<float> (b.getX() + 2.0f, y - 6.0f, 26.0f, 12.0f), juce::Justification::centredRight, false);
        g.setColour (Colours::outline.withAlpha (std::abs (db) < 0.01f ? 0.8f : 0.3f));
        g.fillRect (juce::Rectangle<float> (b.getX() + 31.0f, y, b.getWidth() - 39.0f, 1.0f));
    }

    for (int band = 0; band < ps::kDynEqBands; ++band)
    {
        const auto m = barArea (band);
        const bool sel = band == selected;
        const auto c = eqBandColour (band);

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (m, 3.0f);

        if (on[band])
        {
            // travel range
            if (std::abs (trig[band] - rest[band]) >= 0.05f)
            {
                const float y0 = yOf (juce::jmax (rest[band], trig[band])), y1 = yOf (juce::jmin (rest[band], trig[band]));
                g.setColour (c.withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (m.getX() + 1.0f, y0, m.getWidth() - 2.0f, y1 - y0));
            }

            const float v = gain[band];

            if (std::abs (v) > 0.01f)
            {
                const float y = yOf (v);
                const auto bar = v < 0.0f ? juce::Rectangle<float> (m.getX(), zeroY, m.getWidth(), y - zeroY)
                                          : juce::Rectangle<float> (m.getX(), y, m.getWidth(), zeroY - y);
                g.setGradientFill (juce::ColourGradient::vertical (c.withAlpha (0.95f), bar.getY(), c.withAlpha (0.6f), bar.getBottom()));
                g.fillRect (bar.reduced (2.5f, 0.0f));
            }
        }

        g.setColour (sel && on[band] ? c.withAlpha (0.4f) : Colours::outline.withAlpha (0.7f));
        g.drawRoundedRectangle (m.reduced (0.5f), 3.0f, 1.0f);

        const auto badge = juce::Rectangle<float> (juce::jmax (15.0f, m.getWidth() + 2.0f), 16.0f).withCentre ({ m.getCentreX(), m.getBottom() + 13.0f });

        if (sel)
        {
            g.setColour (on[band] ? c.withAlpha (0.3f) : Colours::panelRaised);
            g.fillRoundedRectangle (badge, 4.0f);
            g.setColour (on[band] ? c : Colours::textDim);
            g.drawRoundedRectangle (badge.reduced (0.5f), 4.0f, 1.0f);
        }

        g.setFont (font (10.5f, 700));
        g.setColour (on[band] ? (sel ? c.brighter (0.25f) : c) : Colours::textFaint);
        g.drawText (juce::String (band + 1), badge, juce::Justification::centred, false);
    }
}

void DynamicEqGainColumn::mouseUp (const juce::MouseEvent& e)
{
    if (! isLeftClick (e) || onSelect == nullptr)
        return;

    for (int band = 0; band < ps::kDynEqBands; ++band)
        if (barArea (band).expanded (2.0f, 30.0f).contains (e.position))
        {
            onSelect (band);
            return;
        }
}
} // namespace ps::ui
