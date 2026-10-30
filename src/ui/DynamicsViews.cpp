#include "ui/DynamicsViews.h"

#include "ui/Controls.h"

#include <cmath>

// Views of the Dynamics panel. Owner: ui-dynamics. See DynamicsViews.h.
//
// The transfer curve is the DSP's static gain computer, evaluated with ps::GainCurve (Envelope.h) exactly as
// Dynamics.cpp applies it: compressor curve on the detector level, limiter curve on (detector level + compressor
// gain), wet gain = compressor + limiter + auto-gain makeup, parallel mix = (1 - mix) + mix * wet (linear amplitude,
// the dry and wet band are the same signal), band gain excluded. The auto-gain makeup follows the formula documented
// at the top of Dynamics.cpp (pink reference level per band, Gaussian-weighted average of the static gain).
namespace ps::ui
{
namespace
{
const juce::String kPrefix ("dyn");

float paramValue (const PureSheenProcessor& p, const juce::String& id) { return p.getParameterValue (id); }

juce::String dbText (float db, int decimals = 1)
{
    if (db < -150.0f)
        return "-inf";

    const auto number = juce::String (db, decimals);
    return (db > 0.0f && number.getDoubleValue() > 0.0 ? "+" : "") + number + " dB";
}

void fillShadowedCircle (juce::Graphics& g, juce::Point<float> c, float r, juce::Colour fill, juce::Colour ring, bool hollow)
{
    const auto circle = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillEllipse (circle.translated (0.0f, 1.2f).expanded (0.5f));

    if (hollow)
    {
        g.setColour (Colours::background);
        g.fillEllipse (circle);
        g.setColour (ring);
        g.drawEllipse (circle.reduced (0.75f), 1.5f);
    }
    else
    {
        g.setColour (fill);
        g.fillEllipse (circle);
        g.setColour (ring);
        g.drawEllipse (circle.reduced (0.5f), 1.0f);
    }
}
} // namespace

//==============================================================================
juce::String dynamicsBandParamId (int channelSet, int band, const juce::String& name)
{
    return kPrefix + (channelSet == 1 ? ".b.b" : ".a.b") + juce::String (band + 1) + "." + name;
}

float DynamicsBandState::gainDb (float x) const noexcept
{
    if (! bandOn)
        return 0.0f;

    const float gc = compOn ? comp.gainDb (x) : 0.0f;
    const float gl = limOn ? lim.gainDb (x + gc) : 0.0f;
    const float wet = gc + gl + makeupDb;
    const float lin = (1.0f - mix) + mix * std::pow (10.0f, wet * 0.05f);
    return 20.0f * std::log10 (juce::jmax (lin, 1.0e-6f));
}

DynamicsBandState DynamicsBandState::read (const PureSheenProcessor& p, int set, int band)
{
    auto v = [&] (const char* name) { return paramValue (p, dynamicsBandParamId (set, band, name)); };

    DynamicsBandState s;
    s.bandOn = v ("on") >= 0.5f;
    s.compOn = v ("comp.on") >= 0.5f;
    s.limOn = v ("lim.on") >= 0.5f;
    s.comp.thresholdDb = v ("comp.thr");
    s.comp.ratio = v ("comp.ratio");
    s.comp.kneeDb = v ("comp.knee");
    s.comp.upwardExpands = false;
    s.lim.thresholdDb = v ("lim.thr");
    s.lim.ratio = v ("lim.ratio");
    s.lim.kneeDb = v ("lim.knee");
    s.lim.upwardExpands = true;
    s.mix = juce::jlimit (0.0f, 1.0f, v ("mix") * 0.01f);

    if (s.bandOn && paramValue (p, kPrefix + ".autoGain") >= 0.5f)
    {
        const int n = juce::jlimit (1, ps::kMaxBands, juce::roundToInt (paramValue (p, kPrefix + ".bands")));
        auto xover = [&] (int i) { return paramValue (p, kPrefix + ".xover" + juce::String (i)); };
        const float lo = band == 0 ? 20.0f : xover (band);
        const float hi = band >= n - 1 ? 20000.0f : xover (band + 1);
        const float phi = juce::jmax (0.01f, std::log2 (juce::jmax (hi, lo * 1.0001f) / lo) / std::log2 (1000.0f));
        const bool rms = juce::roundToInt (paramValue (p, kPrefix + ".detect")) == (int) ps::DetectionMode::Rms;
        const float ref = -18.0f + 10.0f * std::log10 (phi) + (rms ? 0.0f : 3.0f);
        double sum = 0.0, weights = 0.0;

        for (int k = -24; k <= 24; ++k)
        {
            const float d = 0.5f * (float) k;
            const double w = std::exp (-(double) (d * d) / (2.0 * 16.0));
            const float level = ref + d;
            const float gc = s.compOn ? s.comp.gainDb (level) : 0.0f;
            const float gl = s.limOn ? s.lim.gainDb (level + gc) : 0.0f;
            sum += w * (double) (gc + gl);
            weights += w;
        }

        s.makeupDb = juce::jlimit (-24.0f, 24.0f, (float) (-sum / weights));
    }

    return s;
}

//==============================================================================
void ParamGesture::start (const juce::String& paramId, const juce::String& linkedParamId)
{
    end();
    param = processor.getParameter (paramId);
    linked = linkedParamId.isNotEmpty() && linkedParamId != paramId ? processor.getParameter (linkedParamId) : nullptr;
    open = false;
}

void ParamGesture::set (float plain)
{
    if (param == nullptr)
        return;

    if (! open)
    {
        open = true;
        param->beginChangeGesture();

        if (linked != nullptr)
            linked->beginChangeGesture();
    }

    param->setValueNotifyingHost (param->convertTo0to1 (plain));

    if (linked != nullptr)
        linked->setValueNotifyingHost (linked->convertTo0to1 (plain));
}

void ParamGesture::end()
{
    if (open && param != nullptr)
    {
        param->endChangeGesture();

        if (linked != nullptr)
            linked->endChangeGesture();
    }

    open = false;
    param = nullptr;
    linked = nullptr;
}

float ParamGesture::get() const
{
    return param != nullptr ? param->convertFrom0to1 (param->getValue()) : 0.0f;
}

void ParamGesture::setComplete (PureSheenProcessor& p, const juce::String& id, const juce::String& linkedId, float v)
{
    if (linkedId.isEmpty() || linkedId == id || p.getParameter (linkedId) == nullptr)
    {
        p.setParameterValue (id, v);
        return;
    }

    juce::String name = id;

    if (const int index = ps::paramIndex (id.toStdString()); index >= 0)
    {
        const auto& spec = ps::paramSpecs()[(size_t) index];
        name = juce::String (spec.name).upToLastOccurrenceOf (" (", false, false) + ": "
               + juce::String (v, 1) + (spec.unit.empty() ? juce::String() : " " + juce::String (spec.unit));
    }

    p.setParameterValues ({ { id, v }, { linkedId, v } }, name);
}

float ParamGesture::defaultOf (const juce::String& id)
{
    if (const int index = ps::paramIndex (id.toStdString()); index >= 0)
        return ps::paramSpecs()[(size_t) index].defaultValue;

    return 0.0f;
}

//==============================================================================
// DynamicsTransferGraph
//==============================================================================
DynamicsTransferGraph::DynamicsTransferGraph (PureSheenProcessor& p) : processor (p), gesture (p)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    colour = moduleColour (ps::ModuleType::Dynamics);
    setTooltip ("Transfer curve of the selected band: input level (left to right) against output level. Drag C "
                "(compressor) or L (limiter) left or right to move its threshold; double-click resets it. The dot "
                "follows the band's level.");
}

DynamicsTransferGraph::~DynamicsTransferGraph() = default;

void DynamicsTransferGraph::setTarget (int channelSet, int bandIndex, int linked, juce::Colour c, const juce::String& setName)
{
    if (channelSet != set || bandIndex != band)
        gesture.end(), dragNode = -1;

    set = channelSet;
    band = bandIndex;
    linkedSet = linked;
    colour = c;
    setLabel = setName;
    repaint();
}

void DynamicsTransferGraph::setLive (float detectorDb, float gainDb, bool hasSignal)
{
    liveDb = detectorDb;
    liveGainDb = gainDb;
    live = hasSignal;
    repaint();
}

juce::Rectangle<float> DynamicsTransferGraph::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromTop (28.0f);
    r.removeFromLeft (30.0f);
    r.removeFromRight (10.0f);
    r.removeFromBottom (20.0f);

    // Square: 1 dB in = 1 dB out on screen, so slopes read as ratios. Centred in the free space.
    const float side = juce::jmax (10.0f, juce::jmin (r.getWidth(), r.getHeight()));
    return r.withSizeKeepingCentre (side, side).withY (r.getY() + juce::jmax (0.0f, (r.getHeight() - side) * 0.35f));
}

float DynamicsTransferGraph::xOf (float db) const
{
    const auto p = plotArea();
    return p.getX() + (db - kMinDb) / (kMaxDb - kMinDb) * p.getWidth();
}

float DynamicsTransferGraph::yOf (float db) const
{
    const auto p = plotArea();
    return p.getBottom() - (db - kMinDb) / (kMaxDb - kMinDb) * p.getHeight();
}

float DynamicsTransferGraph::dbOfX (float x) const
{
    const auto p = plotArea();
    return kMinDb + (x - p.getX()) / juce::jmax (1.0f, p.getWidth()) * (kMaxDb - kMinDb);
}

juce::String DynamicsTransferGraph::idFor (int node, int s) const
{
    return dynamicsBandParamId (s, band, node == 0 ? "comp.thr" : "lim.thr");
}

juce::Point<float> DynamicsTransferGraph::nodePosition (int node, const DynamicsBandState& st) const
{
    float inputDb = st.comp.thresholdDb;

    if (node == 1)
    {
        // The limiter sees the compressor's output: find the input level whose compressed level reaches its
        // threshold (x + gc (x) is non-decreasing, bisection).
        const float target = st.lim.thresholdDb;

        if (! st.compOn)
        {
            inputDb = target;
        }
        else
        {
            float lo = -200.0f, hi = 60.0f;

            for (int i = 0; i < 48; ++i)
            {
                const float mid = 0.5f * (lo + hi);

                if (mid + st.comp.gainDb (mid) < target)
                    lo = mid;
                else
                    hi = mid;
            }

            inputDb = 0.5f * (lo + hi);
        }
    }

    return { xOf (inputDb), yOf (st.outputDb (inputDb)) };
}

int DynamicsTransferGraph::nodeAt (juce::Point<float> p) const
{
    const auto st = DynamicsBandState::read (processor, set, band);
    int best = -1;
    float bestD = 10.0f;

    for (int node = 1; node >= 0; --node)
    {
        const float d = nodePosition (node, st).getDistanceFrom (p);

        if (d < bestD)
        {
            bestD = d;
            best = node;
        }
    }

    return best;
}

void DynamicsTransferGraph::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), b.getY(),
                                                       Colours::background.darker (0.05f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

    const auto st = DynamicsBandState::read (processor, set, band);
    const auto plot = plotArea();
    const auto limColour = Colours::major;

    // Title row: "TRANSFER  Band 2 (Mid)", live readout on the right.
    {
        auto title = b.reduced (10.0f, 0.0f).removeFromTop (28.0f).translated (0.0f, 1.0f);
        const auto f = font (10.5f, 700).withExtraKerningFactor (0.08f);
        g.setFont (f);
        g.setColour (Colours::text.withAlpha (0.9f));
        const juce::String t ("TRANSFER");
        g.drawText (t, title, juce::Justification::centredLeft, false);
        const float tw = textWidth (f, t) + 8.0f;
        g.setFont (font (11.0f, 600));
        g.setColour (colour);
        const juce::String bandText = "Band " + juce::String (band + 1) + (setLabel.isNotEmpty() ? " " + setLabel : juce::String());
        g.drawText (bandText, title.withTrimmedLeft (tw), juce::Justification::centredLeft, false);

        if (live && title.getWidth() > 280.0f)
        {
            g.setFont (font (11.0f, 600));
            g.setColour (Colours::textSubtle);
            const auto readout = "In " + dbText (liveDb) + "   Gain " + (liveGainDb > 0.05f ? "+" : "") + juce::String (liveGainDb, 1) + " dB";
            g.drawText (readout, title, juce::Justification::centredRight, false);
        }
    }

    // Grid: 6 dB lines, labels every 12 dB.
    g.setFont (font (10.0f, 600));

    for (float db = kMinDb; db <= kMaxDb + 0.01f; db += 6.0f)
    {
        const bool major = std::fmod (std::abs (db), 12.0f) < 0.01f;
        const bool zero = std::abs (db) < 0.01f;
        const float x = std::round (xOf (db)), y = std::round (yOf (db));
        g.setColour (Colours::outline.withAlpha (zero ? 0.7f : (major ? 0.42f : 0.2f)));
        g.fillRect (juce::Rectangle<float> (x, plot.getY(), 1.0f, plot.getHeight()));
        g.fillRect (juce::Rectangle<float> (plot.getX(), y, plot.getWidth(), 1.0f));

        if (major)
        {
            g.setColour (zero ? Colours::textDim : Colours::textSubtle);
            const auto label = juce::String (juce::roundToInt (db));
            g.drawText (label, juce::Rectangle<float> (plot.getX() - 30.0f, y - 6.0f, 26.0f, 12.0f), juce::Justification::centredRight, false);

            if (db < kMaxDb - 3.0f && plot.getWidth() > 120.0f)
                g.drawText (label, juce::Rectangle<float> (x - 15.0f, plot.getBottom() + 4.0f, 30.0f, 12.0f), juce::Justification::centred, false);
        }
    }

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (plot.toNearestInt().expanded (1));

    // 1:1 reference
    {
        const float dashes[] = { 3.0f, 4.0f };
        g.setColour (Colours::textFaint.withAlpha (0.8f));
        g.drawDashedLine (juce::Line<float> (xOf (kMinDb), yOf (kMinDb), xOf (kMaxDb), yOf (kMaxDb)), dashes, 2, 1.0f);
    }

    // Threshold guides
    auto guide = [&] (float inputX, juce::Colour c, bool on)
    {
        const float dashes[] = { 2.0f, 3.0f };
        g.setColour (c.withAlpha (on ? 0.4f : 0.18f));
        g.drawDashedLine (juce::Line<float> (inputX, plot.getY(), inputX, plot.getBottom()), dashes, 2, 1.0f);
    };

    const auto cPos = nodePosition (0, st);
    const auto lPos = nodePosition (1, st);
    guide (cPos.x, colour, st.compOn && st.bandOn);
    guide (lPos.x, limColour, st.limOn && st.bandOn);

    // Curve (one point per pixel column), with a soft fill under it.
    juce::Path curve;
    const int x0 = (int) std::floor (plot.getX()), x1 = (int) std::ceil (plot.getRight());

    for (int px = x0; px <= x1; ++px)
    {
        const float in = dbOfX ((float) px);
        const float y = juce::jlimit (plot.getY() - 40.0f, plot.getBottom() + 40.0f, yOf (st.outputDb (in)));

        if (px == x0)
            curve.startNewSubPath ((float) px, y);
        else
            curve.lineTo ((float) px, y);
    }

    {
        juce::Path fill (curve);
        fill.lineTo ((float) x1, plot.getBottom());
        fill.lineTo ((float) x0, plot.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (0.16f), plot.getY(), colour.withAlpha (0.02f), plot.getBottom()));
        g.fillPath (fill);
    }

    g.setColour (colour.withAlpha (0.25f));
    g.strokePath (curve, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (st.bandOn ? colour : Colours::textDim);
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Live: the static operating point (dot on the curve) and the gain actually applied right now (ring).
    if (live && st.bandOn)
    {
        const float in = juce::jlimit (kMinDb, kMaxDb, liveDb);
        const auto onCurve = juce::Point<float> (xOf (in), yOf (st.outputDb (in)));
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (juce::Rectangle<float> (onCurve.x - 0.5f, onCurve.y, 1.0f, plot.getBottom() - onCurve.y));

        // The meter's gain is compressor + limiter; add the makeup and the parallel mix like the static curve does.
        const float wet = liveGainDb + st.makeupDb;
        const float appliedDb = 20.0f * std::log10 (juce::jmax (1.0e-6f, (1.0f - st.mix) + st.mix * std::pow (10.0f, wet * 0.05f)));
        const auto applied = juce::Point<float> (onCurve.x, yOf (in + appliedDb));

        if (std::abs (applied.y - onCurve.y) > 2.0f)
        {
            g.setColour (Colours::text.withAlpha (0.55f));
            g.drawEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (applied), 1.2f);
        }

        g.setColour (colour.withAlpha (0.3f));
        g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (onCurve));
        g.setColour (juce::Colours::white);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (onCurve));
    }

    // Nodes
    auto node = [&] (int index, juce::Point<float> pos, juce::Colour c, bool on, const juce::String& letter)
    {
        const bool active = index == dragNode || (index == hoverNode && dragNode < 0);
        const float r = active ? 7.5f : 6.5f;

        if (active)
        {
            g.setColour (c.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (r * 3.2f, r * 3.2f).withCentre (pos));
        }

        fillShadowedCircle (g, pos, r, on ? c : Colours::panelRaised, on ? c.brighter (0.35f) : c.withAlpha (0.7f), ! on);
        g.setFont (font (9.5f, 700));
        g.setColour (on ? Colours::background : c);
        g.drawText (letter, juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos).translated (0.0f, 0.5f), juce::Justification::centred, false);

        if (active)
        {
            const auto text = dbText (index == 0 ? st.comp.thresholdDb : st.lim.thresholdDb);
            const auto f = font (11.0f, 700);
            const float w = textWidth (f, text) + 12.0f;
            auto bubble = juce::Rectangle<float> (w, 18.0f).withCentre ({ pos.x, pos.y - 20.0f });
            bubble = bubble.constrainedWithin (plot.reduced (2.0f));
            g.setColour (Colours::background.withAlpha (0.92f));
            g.fillRoundedRectangle (bubble, 5.0f);
            g.setColour (c.withAlpha (0.85f));
            g.drawRoundedRectangle (bubble.reduced (0.5f), 5.0f, 1.0f);
            g.setFont (f);
            g.setColour (Colours::text);
            g.drawText (text, bubble, juce::Justification::centred, false);
        }
    };

    if (st.bandOn && std::abs (st.makeupDb) >= 0.05f)
    {
        g.setFont (font (10.5f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText ("Auto gain " + dbText (st.makeupDb), plot.reduced (8.0f, 6.0f), juce::Justification::bottomRight, false);
    }

    node (1, lPos, limColour, st.limOn && st.bandOn, "L");
    node (0, cPos, colour, st.compOn && st.bandOn, "C");

    if (! st.bandOn)
    {
        g.setFont (font (12.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText ("Band bypassed", plot.withTrimmedTop (plot.getHeight() * 0.55f), juce::Justification::centredTop, false);
    }
}

void DynamicsTransferGraph::mouseMove (const juce::MouseEvent& e)
{
    const int n = dragNode >= 0 ? dragNode : nodeAt (e.position);

    if (n != hoverNode)
    {
        hoverNode = n;
        repaint();
    }

    setMouseCursor (n >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void DynamicsTransferGraph::mouseExit (const juce::MouseEvent&)
{
    if (hoverNode >= 0 && dragNode < 0)
    {
        hoverNode = -1;
        repaint();
    }
}

void DynamicsTransferGraph::mouseDown (const juce::MouseEvent& e)
{
    const int n = nodeAt (e.position);

    if (n < 0)
        return;

    if (e.mods.isPopupMenu())
    {
        const auto id = idFor (n, set);
        const auto linkedId = linkedSet >= 0 ? idFor (n, linkedSet) : juce::String();
        const auto onId = dynamicsBandParamId (set, band, n == 0 ? "comp.on" : "lim.on");
        const auto onLinked = linkedSet >= 0 ? dynamicsBandParamId (linkedSet, band, n == 0 ? "comp.on" : "lim.on") : juce::String();
        const bool on = processor.getParameterValue (onId) >= 0.5f;
        juce::PopupMenu menu;
        menu.addSectionHeader (n == 0 ? "Compressor threshold" : "Limiter threshold");
        menu.addItem (1, "Reset to default (" + dbText (ParamGesture::defaultOf (id)) + ")");
        menu.addItem (2, n == 0 ? "Compressor on" : "Limiter on", true, on);
        juce::Component::SafePointer<DynamicsTransferGraph> safe (this);
        showMenu (menu, *this, [safe, id, linkedId, onId, onLinked, on] (int result)
        {
            if (safe == nullptr)
                return;

            if (result == 1)
                ParamGesture::setComplete (safe->processor, id, linkedId, ParamGesture::defaultOf (id));
            else if (result == 2)
                ParamGesture::setComplete (safe->processor, onId, onLinked, on ? 0.0f : 1.0f);
        });
        return;
    }

    if (! isLeftClick (e))
        return;

    const auto st = DynamicsBandState::read (processor, set, band);
    dragNode = n;
    dragDb = dbOfX (nodePosition (n, st).x);
    lastX = e.position.x;
    gesture.start (idFor (n, set), linkedSet >= 0 ? idFor (n, linkedSet) : juce::String());
    repaint();
}

void DynamicsTransferGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragNode < 0 || ! isLeftClick (e))
        return;

    const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
    const float dx = e.position.x - lastX;
    lastX = e.position.x;

    if (std::abs (dx) < 1.0e-4f)
        return;

    const auto plot = plotArea();
    dragDb += dx / juce::jmax (1.0f, plot.getWidth()) * (kMaxDb - kMinDb) * (fine ? 0.1f : 1.0f);
    dragDb = juce::jlimit (-80.0f, 10.0f, dragDb);

    // C: the threshold is the input level. L: the threshold is the compressor's output at that input level.
    const auto st = DynamicsBandState::read (processor, set, band);
    float thr = dragDb;

    if (dragNode == 1 && st.compOn)
        thr = dragDb + st.comp.gainDb (dragDb);

    gesture.set (juce::jlimit (-60.0f, 0.0f, thr));
    repaint();
}

void DynamicsTransferGraph::mouseUp (const juce::MouseEvent&)
{
    if (dragNode >= 0)
    {
        gesture.end();
        dragNode = -1;
        repaint();
    }
}

void DynamicsTransferGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! isLeftClick (e))
        return;

    const int n = nodeAt (e.position);

    if (n < 0)
        return;

    gesture.end();
    dragNode = -1;
    const auto id = idFor (n, set);
    ParamGesture::setComplete (processor, id, linkedSet >= 0 ? idFor (n, linkedSet) : juce::String(), ParamGesture::defaultOf (id));
}

//==============================================================================
// DynamicsGrMeters
//==============================================================================
DynamicsGrMeters::DynamicsGrMeters()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    colour = moduleColour (ps::ModuleType::Dynamics);
    setTooltip ("Gain change per band: amber = reduction, mint = upward gain. Click a meter to select its band.");
}

void DynamicsGrMeters::setValues (int numBands, const float* gainDb, int selectedBand, int soloBand, juce::Colour c)
{
    bands = juce::jlimit (1, ps::kMaxBands, numBands);
    selected = selectedBand;
    solo = soloBand;
    colour = c;

    for (int b = 0; b < ps::kMaxBands; ++b)
    {
        const float v = b < bands ? gainDb[b] : 0.0f;
        values[b] = v;

        // Hold the largest change (either direction) for ~1.5 s, then let it fall back.
        if (std::abs (v) >= std::abs (holds[b]))
        {
            holds[b] = v;
            holdTicks[b] = 45;
        }
        else if (holdTicks[b] > 0)
        {
            --holdTicks[b];
        }
        else
        {
            holds[b] += (v - holds[b]) * 0.15f;
        }
    }

    repaint();
}

void DynamicsGrMeters::resetHolds()
{
    for (int b = 0; b < ps::kMaxBands; ++b)
    {
        holds[b] = values[b] = 0.0f;
        holdTicks[b] = 0;
    }

    repaint();
}

juce::Rectangle<float> DynamicsGrMeters::meterArea (int band) const
{
    auto r = getLocalBounds().toFloat().reduced (8.0f, 0.0f);
    r.removeFromTop (44.0f);    // title row + value row
    r.removeFromBottom (26.0f);
    r.removeFromLeft (22.0f);
    const float gap = 6.0f;
    const float w = juce::jmin (30.0f, (r.getWidth() - gap * (float) (bands - 1)) / (float) bands);
    const float total = w * (float) bands + gap * (float) (bands - 1);
    const float x = r.getX() + (r.getWidth() - total) * 0.5f + (float) band * (w + gap);
    return { x, r.getY(), w, r.getHeight() };
}

void DynamicsGrMeters::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), b.getY(),
                                                       Colours::background.darker (0.05f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

    g.setFont (font (10.5f, 700).withExtraKerningFactor (0.08f));
    g.setColour (Colours::text.withAlpha (0.9f));
    g.drawText ("GAIN", b.reduced (10.0f, 0.0f).removeFromTop (28.0f).translated (0.0f, 1.0f), juce::Justification::centredLeft, false);

    if (bands < 1)
        return;

    const auto first = meterArea (0);
    const float top = first.getY(), bottom = first.getBottom();
    auto yOf = [&] (float db) { return top + (kMaxDb - juce::jlimit (kMinDb, kMaxDb, db)) / (kMaxDb - kMinDb) * (bottom - top); };
    const float zeroY = yOf (0.0f);

    // Scale
    g.setFont (font (9.5f, 600));

    for (float db : { 6.0f, 0.0f, -6.0f, -12.0f, -18.0f, -24.0f })
    {
        const float y = std::round (yOf (db));
        g.setColour (Colours::textSubtle);
        g.drawText (db > 0.0f ? "+6" : juce::String (juce::roundToInt (db)),
                    juce::Rectangle<float> (b.getX() + 4.0f, y - 6.0f, 22.0f, 12.0f), juce::Justification::centredRight, false);
        g.setColour (Colours::outline.withAlpha (std::abs (db) < 0.01f ? 0.8f : 0.3f));
        g.fillRect (juce::Rectangle<float> (b.getX() + 28.0f, y, b.getWidth() - 36.0f, 1.0f));
    }

    for (int band = 0; band < bands; ++band)
    {
        const auto m = meterArea (band);
        const bool sel = band == selected;
        const bool muted = solo >= 0 && solo != band;

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (m, 3.0f);

        const float v = values[band];

        if (std::abs (v) > 0.01f)
        {
            const float y = yOf (v);
            const auto bar = v < 0.0f ? juce::Rectangle<float> (m.getX(), zeroY, m.getWidth(), y - zeroY)
                                      : juce::Rectangle<float> (m.getX(), y, m.getWidth(), zeroY - y);
            const auto c = (v < 0.0f ? Colours::gainReduction : Colours::good).withMultipliedAlpha (muted ? 0.4f : 1.0f);
            g.setGradientFill (juce::ColourGradient::vertical (c.withAlpha (0.95f * c.getFloatAlpha()), bar.getY(),
                                                               c.withAlpha (0.6f * c.getFloatAlpha()), bar.getBottom()));
            g.fillRect (bar.reduced (2.0f, 0.0f));
        }

        if (std::abs (holds[band]) > 0.05f)
        {
            const float y = std::round (yOf (holds[band]));
            g.setColour ((holds[band] < 0.0f ? Colours::gainReduction : Colours::good).brighter (0.3f));
            g.fillRect (juce::Rectangle<float> (m.getX() + 2.0f, y - 0.5f, m.getWidth() - 4.0f, 1.5f));
        }

        g.setColour (sel ? colour.withAlpha (0.9f) : Colours::outline.withAlpha (0.7f));
        g.drawRoundedRectangle (m.reduced (0.5f), 3.0f, sel ? 1.5f : 1.0f);

        // Band number badge and the value
        const auto badge = juce::Rectangle<float> (18.0f, 16.0f).withCentre ({ m.getCentreX(), m.getBottom() + 13.0f });
        g.setColour (sel ? colour.withAlpha (0.25f) : Colours::panelRaised.withAlpha (0.6f));
        g.fillRoundedRectangle (badge, 4.0f);
        g.setFont (font (10.5f, 700));
        g.setColour (sel ? colour : Colours::textDim);
        g.drawText (juce::String (band + 1), badge, juce::Justification::centred, false);

        g.setFont (font (9.5f, 600));
        g.setColour (std::abs (holds[band]) > 0.05f ? Colours::text.withAlpha (0.85f) : Colours::textSubtle);
        const auto valueText = std::abs (holds[band]) < 0.05f ? juce::String ("0.0") : juce::String (holds[band], 1);
        g.drawFittedText (valueText, juce::Rectangle<float> (m.getX() - 6.0f, top - 15.0f, m.getWidth() + 12.0f, 12.0f).toNearestInt(),
                          juce::Justification::centred, 1, 0.7f);
    }
}

void DynamicsGrMeters::mouseUp (const juce::MouseEvent& e)
{
    if (! isLeftClick (e) || onSelect == nullptr)
        return;

    for (int band = 0; band < bands; ++band)
        if (meterArea (band).expanded (3.0f, 30.0f).contains (e.position))
        {
            onSelect (band);
            return;
        }
}
} // namespace ps::ui
