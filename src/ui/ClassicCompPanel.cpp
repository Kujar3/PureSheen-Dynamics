#include "ui/ClassicCompPanel.h"

#include "dsp/ClassicComp.h"
#include "ui/Controls.h"
#include "ui/EqModel.h"
#include "ui/ImagerExciterCommon.h"

#include <array>
#include <cmath>
#include <vector>

// Classic Compressor panel (mod-ccmp). Layout and interactions: see ClassicCompPanel.h.
//
// Meters: ClassicCompMeters of the module (index = channel set; Stereo uses set A). The transfer ball sits at input =
// detector level - reduction, output = detector level (the detector reads the gain-stage output, so its level IS the
// output level and the input is the output plus the reduction). The gain-reduction needle eases toward the meter at 30
// Hz (half the distance per tick), the hold tick follows the module's 1 s hold.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr auto kType = ps::ModuleType::ClassicComp;

juce::String setParamId (int set, const juce::String& name) { return juce::String ("ccmp.") + (set == 1 ? "b." : "a.") + name; }

// One undoable edit of up to two parameters (and their Link twins) set to absolute values: every gesture opens on the
// first change and all close together on mouse up, so a node drag in two directions is one undo step.
class NodeGesture
{
public:
    explicit NodeGesture (PureSheenProcessor& p) : processor (p) {}
    ~NodeGesture() { end(); }

    void start (const juce::String& idA, const juce::String& linkedA, const juce::String& idB = {}, const juce::String& linkedB = {})
    {
        end();
        slots[0] = { processor.getParameter (idA), linkedA.isNotEmpty() ? processor.getParameter (linkedA) : nullptr };
        slots[1] = { idB.isNotEmpty() ? processor.getParameter (idB) : nullptr,
                     linkedB.isNotEmpty() ? processor.getParameter (linkedB) : nullptr };
    }

    void set (int slot, float plain)
    {
        auto& s = slots[(size_t) slot];
        if (s.param == nullptr)
            return;
        if (! s.open)
        {
            s.open = true;
            s.param->beginChangeGesture();
            if (s.linked != nullptr)
                s.linked->beginChangeGesture();
        }
        s.param->setValueNotifyingHost (s.param->convertTo0to1 (plain));
        if (s.linked != nullptr)
            s.linked->setValueNotifyingHost (s.linked->convertTo0to1 (plain));
    }

    void end()
    {
        for (auto& s : slots)
        {
            if (s.open && s.param != nullptr)
            {
                s.param->endChangeGesture();
                if (s.linked != nullptr)
                    s.linked->endChangeGesture();
            }
            s = {};
        }
    }

    bool isArmed() const noexcept { return slots[0].param != nullptr; }

private:
    struct Slot
    {
        juce::RangedAudioParameter* param = nullptr;
        juce::RangedAudioParameter* linked = nullptr;
        bool open = false;
    };
    PureSheenProcessor& processor;
    std::array<Slot, 2> slots {};
};

// The set's parameters as the DSP reads them (for the static helpers).
ps::ClassicCompSetParams readSet (const PureSheenProcessor& p, int set)
{
    ps::ClassicCompSetParams s;
    s.thresholdDb = p.getParameterValue (setParamId (set, "thr"));
    s.ratio = p.getParameterValue (setParamId (set, "ratio"));
    s.attackMs = p.getParameterValue (setParamId (set, "attack"));
    s.releaseMs = p.getParameterValue (setParamId (set, "release"));
    s.mode = (ps::ClassicCompMode) juce::jlimit (0, 2, juce::roundToInt (p.getParameterValue (setParamId (set, "mode"))));
    s.makeupDb = p.getParameterValue (setParamId (set, "makeup"));
    s.autoGain = p.getParameterValue (setParamId (set, "autoGain")) >= 0.5f;
    s.scHighPassHz = p.getParameterValue (setParamId (set, "sc.hpf"));
    s.scBellHz = p.getParameterValue (setParamId (set, "sc.bellHz"));
    s.scBellDb = p.getParameterValue (setParamId (set, "sc.bellDb"));
    s.scShelfHz = p.getParameterValue (setParamId (set, "sc.shelfHz"));
    s.scShelfDb = p.getParameterValue (setParamId (set, "sc.shelfDb"));
    return s;
}

// "Enter value" prompt for one parameter (plain value; "k" multiplies by 1000) and its Link twin.
void promptParam (juce::Component& parent, PureSheenProcessor& p, const juce::String& id, const juce::String& linkedId,
                  const juce::String& title)
{
    const auto* spec = stereo::specOf (id);
    if (spec == nullptr)
        return;
    const float lo = spec->minValue, hi = spec->maxValue;
    promptForText (parent, title,
                   "Enter a value (" + juce::String (lo) + " to " + juce::String (hi) + " " + juce::String (spec->unit) + ")",
                   fmt::paramValue (p, id), [&p, id, linkedId, lo, hi] (juce::String text)
                   {
                       const auto t = text.trim().toLowerCase();
                       if (! t.containsAnyOf ("0123456789"))
                           return;
                       float v = t.retainCharacters ("-+.0123456789").getFloatValue();
                       if (t.containsChar ('k'))
                           v *= 1000.0f;
                       stereo::setParam (p, id, linkedId, juce::jlimit (lo, hi, v));
                   });
}

//==============================================================================
// Gain-reduction needle on an arc: 0 dB at the right end, -20 dB at the left, square-root scale (small reductions get
// room, as mastering needs).
class GrArcMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr float kRangeDb = 20.0f;

    void setValues (float grDb, float holdDb, float sidechainDb, float autoGainDb, bool active, bool autoGainOn,
                    juce::Colour c)
    {
        // Only the arc is limited to 20 dB; the numeric readout must retain deeper reductions.
        targetDb = active ? juce::jmin (0.0f, std::isfinite (grDb) ? grDb : 0.0f) : 0.0f;
        needleDb += 0.5f * (targetDb - needleDb);
        if (std::abs (needleDb - targetDb) < 0.01f)
            needleDb = targetDb;
        hold = active && std::isfinite (holdDb) ? juce::jlimit (-kRangeDb, 0.0f, holdDb) : 0.0f;
        scDb = sidechainDb;
        agDb = autoGainDb;
        live = active;
        agOn = autoGainOn;
        colour = c;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float readoutH = juce::jlimit (34.0f, 52.0f, b.getHeight() * 0.2f);
        const float infoH = b.getHeight() > 190.0f ? 34.0f : 18.0f;

        // The arc spans 220 degrees: its ends sit 0.34 r below the pivot, 0.94 r to the sides; the labels need room
        // outside it. The arc, the readout and the two info rows are centred as one group.
        constexpr float kSpan = juce::MathConstants<float>::pi * 220.0f / 180.0f;
        const float halfSpan = kSpan * 0.5f;
        const float below = -std::cos (halfSpan);
        constexpr float padX = 30.0f, padTop = 20.0f;
        const float rW = (b.getWidth() - 2.0f * padX) * 0.5f / std::sin (halfSpan);
        const float rH1 = b.getHeight() - padTop - 8.0f - readoutH - infoH;
        const float rH2 = (b.getHeight() - padTop - 16.0f) / (1.0f + below);
        const float r = juce::jlimit (20.0f, 170.0f, juce::jmin (rW, rH1, rH2));
        const float group = padTop + r + juce::jmax (8.0f + readoutH + infoH, below * r + 16.0f);
        const juce::Point<float> pivot (b.getCentreX(), b.getY() + juce::jmax (0.0f, 0.5f * (b.getHeight() - group)) + padTop + r);

        auto angleOf = [&] (float db) {
            const float p = std::sqrt (juce::jlimit (0.0f, 1.0f, -db / kRangeDb));
            return halfSpan - p * kSpan;
        };
        auto pointAt = [&] (float angle, float radius) {
            return juce::Point<float> (pivot.x + radius * std::sin (angle), pivot.y - radius * std::cos (angle));
        };

        // track
        const float thick = juce::jlimit (5.0f, 9.0f, r * 0.08f);
        juce::Path track;
        track.addCentredArc (pivot.x, pivot.y, r, r, 0.0f, -halfSpan, halfSpan, true);
        g.setColour (Colours::background.darker (0.2f));
        g.strokePath (track, juce::PathStrokeType (thick + 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colours::outline.withAlpha (0.8f));
        g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // reduction fill from 0 dB toward the needle
        const auto grColour = Colours::gainReduction;
        if (needleDb < -0.05f)
        {
            juce::Path fill;
            fill.addCentredArc (pivot.x, pivot.y, r, r, 0.0f, angleOf (needleDb), halfSpan, true);
            g.setColour (grColour.withAlpha (0.18f));
            g.strokePath (fill, juce::PathStrokeType (thick + 6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (grColour);
            g.strokePath (fill, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // ticks and labels
        const float ticks[] { 0.0f, -1.0f, -2.0f, -3.0f, -4.0f, -6.0f, -8.0f, -10.0f, -15.0f, -20.0f };
        const float labelled[] { 0.0f, -1.0f, -3.0f, -6.0f, -10.0f, -20.0f };
        g.setFont (font (10.0f, 600));
        for (const float t : ticks)
        {
            const float a = angleOf (t);
            bool hasLabel = false;
            for (const float l : labelled)
                hasLabel = hasLabel || l == t;
            g.setColour (hasLabel ? Colours::textSubtle : Colours::textFaint);
            g.drawLine (juce::Line<float> (pointAt (a, r + thick * 0.5f + 2.0f), pointAt (a, r + thick * 0.5f + (hasLabel ? 6.0f : 4.0f))), 1.2f);
            if (hasLabel)
            {
                const auto c = pointAt (a, r + thick * 0.5f + 13.0f);
                g.drawText (juce::String (juce::roundToInt (t)), juce::Rectangle<float> (28.0f, 12.0f).withCentre (c),
                            juce::Justification::centred, false);
            }
        }

        // hold tick
        if (live && hold < -0.1f)
        {
            const float a = angleOf (hold);
            g.setColour (grColour.brighter (0.3f));
            g.drawLine (juce::Line<float> (pointAt (a, r - thick * 0.5f - 3.0f), pointAt (a, r + thick * 0.5f + 1.0f)), 2.0f);
        }

        // needle
        {
            const float a = angleOf (needleDb);
            const auto tip = pointAt (a, r - thick * 0.5f - 4.0f);
            const auto base = pointAt (a, r * 0.16f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawLine (juce::Line<float> (base.translated (0.0f, 1.5f), tip.translated (0.0f, 1.5f)), 2.6f);
            g.setColour (live ? Colours::text : Colours::textFaint);
            g.drawLine (juce::Line<float> (base, tip), 2.0f);
            g.setColour ((live ? grColour : Colours::textFaint).withAlpha (0.9f));
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (tip));
            const float pr = juce::jlimit (5.0f, 9.0f, r * 0.07f);
            g.setColour (Colours::panelRaised);
            g.fillEllipse (juce::Rectangle<float> (pr * 2.0f, pr * 2.0f).withCentre (pivot));
            g.setColour (colour.withAlpha (0.8f));
            g.drawEllipse (juce::Rectangle<float> (pr * 2.0f, pr * 2.0f).withCentre (pivot), 1.5f);
        }

        // large readout under the pivot
        auto rest = b.withTop (juce::jmin (pivot.y + 8.0f, b.getBottom() - readoutH - infoH));
        auto readout = rest.removeFromTop (readoutH);
        {
            const float big = juce::jlimit (20.0f, 32.0f, readoutH * 0.68f);
            const juce::String value = live ? juce::String (needleDb > -0.05f ? 0.0f : needleDb, 1) : juce::String ("--");
            const auto vf = font (big, 700);
            const auto uf = font (12.0f, 600);
            const float vw = textWidth (vf, value), uw = textWidth (uf, " dB");
            const float x0 = readout.getCentreX() - 0.5f * (vw + uw);
            g.setFont (vf);
            g.setColour (live && needleDb < -0.05f ? grColour : Colours::text);
            g.drawText (value, juce::Rectangle<float> (x0, readout.getY(), vw + 2.0f, readout.getHeight()),
                        juce::Justification::centredLeft, false);
            g.setFont (uf);
            g.setColour (Colours::textSubtle);
            g.drawText (" dB", juce::Rectangle<float> (x0 + vw, readout.getY() + big * 0.12f, uw + 4.0f, readout.getHeight()),
                        juce::Justification::centredLeft, false);
        }

        // detector level and auto gain
        g.setFont (font (11.0f, 600));
        auto info = rest.removeFromTop (infoH);
        const juce::String scText = live && scDb > -99.0f ? juce::String (scDb, 1) + " dB" : juce::String ("--");
        const juce::String agText = ! agOn ? juce::String ("Off") : (live ? fmt::db (agDb) : juce::String ("--"));
        auto row = [&] (juce::Rectangle<float> area, const juce::String& label, const juce::String& value, juce::Colour vc) {
            g.setColour (Colours::textSubtle);
            g.drawText (label, area, juce::Justification::centredLeft, false);
            g.setColour (vc);
            g.drawText (value, area, juce::Justification::centredRight, false);
        };
        if (infoH > 20.0f)
        {
            const auto w = info.reduced (juce::jmax (0.0f, (info.getWidth() - 200.0f) * 0.5f), 0.0f);
            row (w.withHeight (17.0f), "Detector", scText, Colours::text);
            row (w.withTrimmedTop (17.0f), "Auto gain", agText, agOn ? colour : Colours::textFaint);
        }
        else
        {
            const auto halfW = info.getWidth() * 0.5f;
            row (info.withWidth (halfW - 6.0f), "Det", scText, Colours::text);
            row (info.withTrimmedLeft (halfW + 6.0f), "Auto", agText, agOn ? colour : Colours::textFaint);
        }
    }

private:
    float targetDb = 0.0f, needleDb = 0.0f, hold = 0.0f, scDb = -100.0f, agDb = 0.0f;
    bool live = false, agOn = false;
    juce::Colour colour = Colours::accent;
};

//==============================================================================
// Static transfer curve with the threshold node (drag left / right) and the live ball.
class TransferView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr float kMinDb = -48.0f, kMaxDb = 6.0f;

    explicit TransferView (PureSheenProcessor& p) : processor (p), gesture (p)
    {
        setTooltip ("Transfer curve: output level against input level for a steady tone (the knee follows the Mode).\n"
                    "Drag the dot to set the threshold; double-click resets it, right-click for a menu. The ball shows "
                    "the current level.");
    }

    void setTarget (int s, int linked, juce::Colour c)
    {
        if (s != set || linked != linkedSet || c != colour)
        {
            gesture.end();
            dragging = false;
            set = s;
            linkedSet = linked;
            colour = c;
            repaint();
        }
    }

    void setLive (float inDb, float outDb, bool has)
    {
        const auto st = readSet (processor, set);
        const std::array<float, 3> curve { st.thresholdDb, st.ratio, (float) st.mode };
        if (curve != shownCurve || has != live || std::abs (inDb - liveIn) > 0.05f || std::abs (outDb - liveOut) > 0.05f)
        {
            shownCurve = curve;
            live = has;
            liveIn = inDb;
            liveOut = outDb;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        stereo::drawWell (g, b);
        const auto plot = plotArea();
        const auto st = readSet (processor, set);

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
                g.drawText (label, juce::Rectangle<float> (plot.getX() - 28.0f, y - 6.0f, 24.0f, 12.0f),
                            juce::Justification::centredRight, false);
                if (plot.getWidth() > 110.0f)
                    g.drawText (label, juce::Rectangle<float> (x - 15.0f, plot.getBottom() + 3.0f, 30.0f, 12.0f),
                                juce::Justification::centred, false);
            }
        }

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.toNearestInt().expanded (1));

        {
            const float dashes[] = { 3.0f, 4.0f };
            g.setColour (Colours::textFaint.withAlpha (0.8f));
            g.drawDashedLine (juce::Line<float> (xOf (kMinDb), yOf (kMinDb), xOf (kMaxDb), yOf (kMaxDb)), dashes, 2, 1.0f);
        }

        const auto node = nodePosition (st);
        {
            const float dashes[] = { 2.0f, 3.0f };
            g.setColour (colour.withAlpha (0.4f));
            g.drawDashedLine (juce::Line<float> (node.x, plot.getY(), node.x, plot.getBottom()), dashes, 2, 1.0f);
        }

        juce::Path curve, fill;
        const int x0 = (int) std::floor (plot.getX()), x1 = (int) std::ceil (plot.getRight());
        for (int px = x0; px <= x1; ++px)
        {
            const float in = dbOfX ((float) px);
            const float y = juce::jlimit (plot.getY() - 40.0f, plot.getBottom() + 40.0f,
                                          yOf ((float) ps::ClassicComp::staticCurveDb (st, (double) in)));
            if (px == x0)
            {
                curve.startNewSubPath ((float) px, y);
                fill.startNewSubPath ((float) px, plot.getBottom());
            }
            else
            {
                curve.lineTo ((float) px, y);
            }
            fill.lineTo ((float) px, y);
        }
        fill.lineTo ((float) x1, plot.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (0.16f), plot.getY(), colour.withAlpha (0.02f), plot.getBottom()));
        g.fillPath (fill);
        g.setColour (colour.withAlpha (0.18f));
        g.strokePath (curve, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colour);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // live ball
        if (live && liveIn > kMinDb)
        {
            const juce::Point<float> c (xOf (juce::jmin (liveIn, kMaxDb)), yOf (juce::jmin (liveOut, kMaxDb)));
            g.setColour (Colours::text.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (c));
            g.setColour (Colours::text);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (c));
        }

        // threshold node
        {
            const bool hot = hover || dragging;
            const float rr = hot ? 7.0f : 6.0f;
            const auto box = juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (node);
            if (hot)
            {
                g.setColour (colour.withAlpha (0.2f));
                g.fillEllipse (box.expanded (5.0f));
            }
            g.setColour (Colours::background);
            g.fillEllipse (box);
            g.setColour (colour);
            g.drawEllipse (box.reduced (0.9f), 1.8f);
            g.fillEllipse (box.reduced (rr * 0.55f));
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHover (nearNode (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHover (false); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            stereo::showParamMenu (*this, processor, setParamId (set, "thr"), linkedId(), "Threshold");
            return;
        }
        if (! isLeftClick (e) || ! nearNode (e.position))
            return;
        gesture.start (setParamId (set, "thr"), linkedId());
        startDb = processor.getParameterValue (setParamId (set, "thr"));
        startX = e.position.x;
        dragging = true;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging || ! isLeftClick (e))
            return;
        const float scale = e.mods.isCtrlDown() ? 0.25f : 1.0f;
        const float db = startDb + (e.position.x - startX) * scale * (kMaxDb - kMinDb) / plotArea().getWidth();
        gesture.set (0, juce::jlimit (-40.0f, 0.0f, db));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        gesture.end();
        dragging = false;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (isLeftClick (e) && nearNode (e.position))
            stereo::setParam (processor, setParamId (set, "thr"), linkedId(), stereo::defaultValueOf (setParamId (set, "thr")));
    }

private:
    juce::Rectangle<float> plotArea() const
    {
        auto b = getLocalBounds().toFloat();
        b.removeFromLeft (32.0f);
        b.removeFromBottom (b.getWidth() > 140.0f ? 18.0f : 8.0f);
        b.removeFromTop (8.0f);
        b.removeFromRight (10.0f);
        // square plot (1 dB = 1 dB on both axes), centred
        const float side = juce::jmin (b.getWidth(), b.getHeight());
        return b.withSizeKeepingCentre (side, side);
    }
    float xOf (float db) const { const auto p = plotArea(); return p.getX() + (db - kMinDb) / (kMaxDb - kMinDb) * p.getWidth(); }
    float yOf (float db) const { const auto p = plotArea(); return p.getBottom() - (db - kMinDb) / (kMaxDb - kMinDb) * p.getHeight(); }
    float dbOfX (float x) const { const auto p = plotArea(); return kMinDb + (x - p.getX()) / p.getWidth() * (kMaxDb - kMinDb); }
    juce::Point<float> nodePosition (const ps::ClassicCompSetParams& st) const
    {
        const float t = st.thresholdDb;
        return { xOf (t), yOf ((float) ps::ClassicComp::staticCurveDb (st, (double) t)) };
    }
    bool nearNode (juce::Point<float> p) const { return p.getDistanceFrom (nodePosition (readSet (processor, set))) < 11.0f; }
    void setHover (bool h) { if (h != hover) { hover = h; repaint(); } }
    juce::String linkedId() const { return linkedSet >= 0 ? setParamId (linkedSet, "thr") : juce::String(); }

    PureSheenProcessor& processor;
    NodeGesture gesture;
    int set = 0, linkedSet = -1;
    juce::Colour colour = Colours::accent;
    float liveIn = -100.0f, liveOut = -100.0f, startDb = 0.0f, startX = 0.0f;
    std::array<float, 3> shownCurve {};
    bool live = false, hover = false, dragging = false;
};

//==============================================================================
// Detection filter: the sidechain curve with the high-pass (frequency), bell and shelf (frequency and gain) nodes.
class SidechainView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr float kRangeDb = 12.0f;

    explicit SidechainView (PureSheenProcessor& p) : processor (p), gesture (p)
    {
        setTooltip ("Detection filter: shapes what the compressor listens to (not what you hear). High-pass: less "
                    "reaction to the low end. Bell and shelf: more (up) or less (down) reaction around their frequency.\n"
                    "Drag a node; double-click resets it, right-click for a menu.");
    }

    void setTarget (int s, int linked, juce::Colour c)
    {
        if (s != set || linked != linkedSet || c != colour)
        {
            gesture.end();
            dragNode = -1;
            set = s;
            linkedSet = linked;
            colour = c;
            repaint();
        }
    }

    // Repaints when a sidechain value moved (30 Hz check).
    void update()
    {
        const auto st = readSet (processor, set);
        const std::array<float, 5> now { st.scHighPassHz, st.scBellHz, st.scBellDb, st.scShelfHz, st.scShelfDb };
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        stereo::drawWell (g, b);
        const auto st = readSet (processor, set);
        const auto fx = freqAxis();
        const auto dy = dbAxis();
        drawFrequencyGrid (g, fx, dy, fx.area.getWidth() > 150.0f);

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (dy.area.toNearestInt());

        std::vector<float> xs, db;
        const int x0 = (int) std::floor (fx.area.getX()), x1 = (int) std::ceil (fx.area.getRight());
        xs.reserve ((size_t) (x1 - x0 + 1));
        db.reserve ((size_t) (x1 - x0 + 1));
        juce::Path curve;
        for (int px = x0; px <= x1; ++px)
        {
            const float hz = fx.xToHz ((float) px);
            const float v = (float) ps::ClassicComp::detectorResponseDb (st, (double) hz);
            xs.push_back ((float) px);
            db.push_back (v);
            const float y = juce::jlimit (dy.area.getY() - 20.0f, dy.area.getBottom() + 20.0f, dy.dbToY (v));
            if (px == x0)
                curve.startNewSubPath ((float) px, y);
            else
                curve.lineTo ((float) px, y);
        }
        fillEqBand (g, xs, db, dy, colour, true);
        strokeEqCurve (g, curve, colour);

        static const char* const letters[] { "H", "B", "S" };
        for (int n = 0; n < 3; ++n)
        {
            const auto c = nodePosition (n, st);
            const bool hot = n == hoverNode || n == dragNode;
            const float r = hot ? 8.0f : 7.0f;
            const auto box = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c);
            if (hot)
            {
                g.setColour (colour.withAlpha (0.2f));
                g.fillEllipse (box.expanded (5.0f));
            }
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.fillEllipse (box.translated (0.0f, 1.2f).expanded (0.6f));
            if (hot)
            {
                g.setColour (colour);
                g.fillEllipse (box);
                g.setColour (Colours::background);
            }
            else
            {
                g.setColour (Colours::background.withAlpha (0.9f));
                g.fillEllipse (box);
                g.setColour (colour);
                g.drawEllipse (box.reduced (0.9f), 1.8f);
            }
            g.setFont (font (10.0f, 700));
            g.drawText (letters[n], box, juce::Justification::centred, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHover (nodeAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int n = nodeAt (e.position);
        if (e.mods.isPopupMenu())
        {
            if (n >= 0)
                showNodeMenu (n);
            return;
        }
        if (! isLeftClick (e) || n < 0)
            return;
        dragNode = n;
        const auto st = readSet (processor, set);
        startHz = n == 0 ? st.scHighPassHz : (n == 1 ? st.scBellHz : st.scShelfHz);
        startDb = n == 1 ? st.scBellDb : st.scShelfDb;
        startPos = e.position;
        gesture.start (id (freqName (n), set), linkedOf (freqName (n)),
                       n == 0 ? juce::String() : id (gainName (n), set), n == 0 ? juce::String() : linkedOf (gainName (n)));
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragNode < 0 || ! isLeftClick (e))
            return;
        const float scale = e.mods.isCtrlDown() ? 0.25f : 1.0f;
        const auto fx = freqAxis();
        const auto dy = dbAxis();
        const float octavesPerPx = std::log2 (fx.maxHz / fx.minHz) / juce::jmax (1.0f, fx.area.getWidth());
        const auto* fs = stereo::specOf (id (freqName (dragNode), set));
        if (fs != nullptr)
        {
            const float hz = startHz * std::exp2 ((e.position.x - startPos.x) * scale * octavesPerPx);
            gesture.set (0, juce::jlimit (fs->minValue, fs->maxValue, hz));
        }
        if (dragNode > 0)
        {
            const float dbPerPx = 2.0f * kRangeDb / juce::jmax (1.0f, dy.area.getHeight());
            const float v = startDb - (e.position.y - startPos.y) * scale * dbPerPx;
            gesture.set (1, juce::jlimit (-8.0f, 8.0f, v));
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        gesture.end();
        dragNode = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int n = isLeftClick (e) ? nodeAt (e.position) : -1;
        if (n >= 0)
            resetNode (n);
    }

private:
    static const char* freqName (int n) { return n == 0 ? "sc.hpf" : (n == 1 ? "sc.bellHz" : "sc.shelfHz"); }
    static const char* gainName (int n) { return n == 1 ? "sc.bellDb" : "sc.shelfDb"; }
    static juce::String id (const char* name, int s) { return setParamId (s, name); }
    juce::String linkedOf (const char* name) const { return linkedSet >= 0 ? id (name, linkedSet) : juce::String(); }

    FreqAxis freqAxis() const
    {
        FreqAxis fx;
        fx.minHz = 14.0f;      // room for the high-pass node at its lowest setting (20 Hz)
        fx.maxHz = 20000.0f;
        fx.area = getLocalBounds().toFloat().reduced (6.0f, 4.0f);
        return fx;
    }

    DbAxis dbAxis() const
    {
        DbAxis dy;
        dy.minDb = -kRangeDb;
        dy.maxDb = kRangeDb;
        dy.area = snapEqZeroLine (getLocalBounds().toFloat().reduced (6.0f, 4.0f).withTrimmedBottom (14.0f));
        return dy;
    }

    juce::Point<float> nodePosition (int n, const ps::ClassicCompSetParams& st) const
    {
        const auto fx = freqAxis();
        const auto dy = dbAxis();
        if (n == 0)
            return { fx.hzToX (st.scHighPassHz),
                     dy.dbToY (juce::jmax (-kRangeDb, (float) ps::ClassicComp::detectorResponseDb (st, (double) st.scHighPassHz))) };
        if (n == 1)
            return { fx.hzToX (st.scBellHz), dy.dbToY (st.scBellDb) };
        return { fx.hzToX (st.scShelfHz), dy.dbToY (st.scShelfDb) };
    }

    int nodeAt (juce::Point<float> p) const
    {
        const auto st = readSet (processor, set);
        int best = -1;
        float bestD = 12.0f;
        for (int n = 0; n < 3; ++n)
        {
            const float d = p.getDistanceFrom (nodePosition (n, st));
            if (d < bestD)
            {
                bestD = d;
                best = n;
            }
        }
        return best;
    }

    void setHover (int n) { if (n != hoverNode) { hoverNode = n; repaint(); } }

    void resetNode (int n)
    {
        const auto f = id (freqName (n), set);
        if (n == 0)
        {
            stereo::setParam (processor, f, linkedOf (freqName (n)), stereo::defaultValueOf (f));
            return;
        }
        const auto gId = id (gainName (n), set);
        std::vector<std::pair<juce::String, float>> list { { f, stereo::defaultValueOf (f) }, { gId, stereo::defaultValueOf (gId) } };
        if (linkedSet >= 0)
        {
            list.push_back ({ id (freqName (n), linkedSet), stereo::defaultValueOf (f) });
            list.push_back ({ id (gainName (n), linkedSet), stereo::defaultValueOf (gId) });
        }
        processor.setParameterValues (list, n == 1 ? "Classic Comp: reset the detection bell" : "Classic Comp: reset the detection shelf");
    }

    void showNodeMenu (int n)
    {
        static const char* const titles[] { "Detection high-pass", "Detection bell", "Detection shelf" };
        juce::PopupMenu menu;
        menu.addSectionHeader (titles[n]);
        menu.addItem (1, "Reset node");
        menu.addItem (2, "Enter frequency...");
        if (n > 0)
            menu.addItem (3, "Enter gain...");
        juce::Component::SafePointer<SidechainView> safe (this);
        showMenu (menu, *this, [safe, n] (int result)
        {
            if (safe == nullptr)
                return;
            auto& v = *safe;
            if (result == 1)
                v.resetNode (n);
            else if (result == 2)
                promptParam (v, v.processor, id (freqName (n), v.set), v.linkedOf (freqName (n)), juce::String (titles[n]) + " frequency");
            else if (result == 3)
                promptParam (v, v.processor, id (gainName (n), v.set), v.linkedOf (gainName (n)), juce::String (titles[n]) + " gain");
        });
    }

    PureSheenProcessor& processor;
    NodeGesture gesture;
    int set = 0, linkedSet = -1, hoverNode = -1, dragNode = -1;
    juce::Colour colour = Colours::accent;
    std::array<float, 5> shown {};
    float startHz = 1000.0f, startDb = 0.0f;
    juce::Point<float> startPos;
};
} // namespace

//==============================================================================
struct ClassicCompPanel::Impl
{
    Impl (ClassicCompPanel& o, PureSheenProcessor& p)
        : owner (o), processor (p), colour (moduleColour (kType)),
          grCard ("Gain Reduction", colour), transferCard ("Transfer", colour), scCard ("Detection Filter", colour),
          compCard ("Compressor", colour), transfer (p), sidechain (p),
          threshold (p, "ccmp.a.thr", "Threshold"), ratio (p, "ccmp.a.ratio", "Ratio"), attack (p, "ccmp.a.attack", "Attack"),
          release (p, "ccmp.a.release", "Release"), makeup (p, "ccmp.a.makeup", "Makeup"),
          mode (p, "ccmp.a.mode", { "Sharp", "Balanced", "Smooth" }), autoGain (p, "ccmp.a.autoGain", "Auto Gain")
    {
        grMeter.setTooltip ("Gain reduction: how far the compressor turns the level down right now (the tick marks the "
                            "deepest point of the last second). Detector = the level the compressor reacts to.");
        threshold.setTooltip ("Threshold: the level where compression starts. Lower = more compression.\nDrag to change "
                              "(Ctrl: fine), double-click to reset.");
        ratio.setTooltip ("Ratio: how strongly levels above the threshold are turned down (4:1 = 4 dB more input gives 1 dB "
                          "more output).\nDrag to change (Ctrl: fine), double-click to reset.");
        attack.setTooltip ("Attack: how fast the compressor reacts to a louder sound. Slower lets more of the hits "
                           "through.\nDrag to change (Ctrl: fine), double-click to reset.");
        release.setTooltip ("Release: how fast it recovers after a short hit. Long stretches of compression recover "
                            "slower on their own, which avoids pumping.\nDrag to change (Ctrl: fine), double-click to reset.");
        makeup.setTooltip ("Makeup: output gain after the compressor (added on top of Auto Gain).\nDrag to change (Ctrl: "
                           "fine), double-click to reset.");
        mode.setTooltip ("Mode: Sharp = crisp, a narrow knee, lets transients through, no colour. Balanced = a medium knee "
                         "with a little colour. Smooth = a wide knee, catches transients, a fuller, warmer sound.");
        autoGain.setTooltip ("Auto Gain: keeps the output as loud as the input while you compress (adapts over a few "
                             "seconds), so you hear the change in dynamics, not in level.");
        listen.setText ("Listen");
        listen.setOnColour (Colours::minor);
        listen.setTooltip ("Listen: play the input through the detection filter, the signal the compressor reacts to.");
        listen.onClick = [this]
        {
            auto m = processor.getMonitor (kType);
            m.soloDetectionFilter = ! m.soloDetectionFilter;
            processor.setMonitor (kType, m);
            listen.setOn (m.soloDetectionFilter);
        };

        owner.addAndMakeVisible (grCard);
        grCard.addAndMakeVisible (grMeter);
        owner.addAndMakeVisible (transferCard);
        transferCard.addAndMakeVisible (transfer);
        owner.addAndMakeVisible (scCard);
        scCard.addAndMakeVisible (sidechain);
        scCard.addAndMakeVisible (listen);
        scCard.setTitleReserve (76);
        owner.addAndMakeVisible (compCard);
        for (auto* k : knobs())
            compCard.addAndMakeVisible (*k);
        compCard.addAndMakeVisible (mode);
        compCard.addAndMakeVisible (autoGain);
        rebind();
    }

    ~Impl()
    {
        // the detector solo is not saved and invisible once the panel is gone: end it with the panel
        auto m = processor.getMonitor (kType);
        if (m.soloDetectionFilter)
        {
            m.soloDetectionFilter = false;
            processor.setMonitor (kType, m);
        }
    }

    std::array<ParamKnob*, 5> knobs() { return { &threshold, &ratio, &attack, &release, &makeup }; }

    bool isSplit() const { return isDualChannelMode (currentChannelMode (processor, kType)); }
    bool isLinked() const { return isSplit() && processor.getParameterValue ("ccmp.link") >= 0.5f; }
    int activeSet() const { return isSplit() ? owner.getChannelSet() : 0; }

    void rebind()
    {
        const int set = activeSet();
        const int other = isLinked() ? 1 - set : -1;
        const auto c = set == 1 ? Colours::sideChannel : colour;
        auto bind = [&] (auto& control, const char* name) {
            control.setParamId (setParamId (set, name));
            control.setLinkedParamId (other >= 0 ? setParamId (other, name) : juce::String());
        };
        bind (threshold, "thr");
        bind (ratio, "ratio");
        bind (attack, "attack");
        bind (release, "release");
        bind (makeup, "makeup");
        bind (mode, "mode");
        bind (autoGain, "autoGain");
        for (auto* k : knobs())
            k->setColour (c);
        mode.setOnColour (c);
        autoGain.setOnColour (c);
        transfer.setTarget (set, other, c);
        sidechain.setTarget (set, other, c);
        for (auto* card : { &grCard, &transferCard, &scCard, &compCard })
            card->setColour (c);
        const auto setName = isSplit() ? channelSetName (currentChannelMode (processor, kType), set) : juce::String();
        grCard.setSubtitle (setName);
        compCard.setSubtitle (setName);
        shownSet = set;
        shownLinked = other >= 0;
        shownSplit = isSplit();
    }

    void update()
    {
        if (activeSet() != shownSet || isLinked() != shownLinked || isSplit() != shownSplit)
            rebind();

        const int set = activeSet();
        const bool running = processor.isModuleInChain (kType) && processor.getParameterValue ("ccmp.on") >= 0.5f
                             && processor.getChain().getSampleRate() > 0.0;
        const auto* comp = dynamic_cast<const ps::ClassicComp*> (&processor.getChain().getModule (kType));
        const bool valid = running && comp != nullptr;
        const bool agOn = processor.getParameterValue (setParamId (set, "autoGain")) >= 0.5f;
        const auto c = set == 1 ? Colours::sideChannel : colour;

        float gr = 0.0f, hold = 0.0f, sc = -100.0f, ag = 0.0f;
        if (valid)
        {
            const auto& m = comp->getMeters();
            gr = m.gainReductionDb[set].load();
            hold = m.gainReductionHoldDb[set].load();
            sc = m.sidechainDb[set].load();
            ag = m.autoGainDb[set].load();
        }
        grMeter.setValues (gr, hold, sc, ag, valid, agOn, c);
        transfer.setLive (sc - gr, sc, valid && std::isfinite (sc) && sc > -80.0f);
        sidechain.update();
        transferCard.setSubtitle (fmt::ratio (processor.getParameterValue (setParamId (set, "ratio"))) + "  "
                                  + fmt::db (processor.getParameterValue (setParamId (set, "thr"))));

        const bool soloing = processor.getMonitor (kType).soloDetectionFilter;
        if (soloing != listen.isOn())
            listen.setOn (soloing);
    }

    void layout (juce::Rectangle<int> content)
    {
        auto r = content.reduced (12, 6).withTrimmedBottom (6);
        const int H = r.getHeight();
        const int bottomH = juce::jlimit (150, 196, juce::roundToInt ((float) H * 0.36f));
        compCard.setBounds (r.removeFromBottom (bottomH));
        r.removeFromBottom (kGap);

        const int W = r.getWidth();
        const int grW = juce::jlimit (190, 300, juce::roundToInt ((float) W * 0.26f));
        const int trW = juce::jlimit (210, 380, juce::jmin (juce::roundToInt ((float) W * 0.3f), r.getHeight() + 30));
        grCard.setBounds (r.removeFromLeft (grW));
        r.removeFromLeft (kGap);
        transferCard.setBounds (r.removeFromLeft (trW));
        r.removeFromLeft (kGap);
        scCard.setBounds (r);

        grMeter.setBounds (grCard.getBody());
        transfer.setBounds (transferCard.getBody());
        listen.setBounds (scCard.getTitleRowRight (juce::jmin (68, listen.getIdealWidth (22.0f) + 4)));
        sidechain.setBounds (scCard.getBody());

        // Compressor card: five knobs, then Mode and Auto Gain in a column on the right
        auto body = compCard.getBody();
        const int sideW = juce::jlimit (210, 300, juce::roundToInt ((float) body.getWidth() * 0.3f));
        auto side = body.removeFromRight (sideW);
        body.removeFromRight (kGap * 2);
        {
            const int rowH = 26;
            const int captionH = 16;
            auto col = side.withSizeKeepingCentre (side.getWidth(), juce::jmin (side.getHeight(), captionH + rowH + 12 + rowH));
            modeCaption = col.removeFromTop (captionH);   // card-relative, as getBody()
            mode.setBounds (col.removeFromTop (rowH));
            col.removeFromTop (12);
            autoGain.setBounds (col.removeFromTop (rowH));
            compCard.setCaptions ({ { "Mode", modeCaption } });
        }
        const int kw = juce::jmin (110, body.getWidth() / 5);
        const int kh = juce::jmin (body.getHeight(), 112);
        auto row = body.withSizeKeepingCentre (kw * 5, kh);
        for (auto* k : knobs())
            k->setBounds (row.removeFromLeft (kw));
    }

    ClassicCompPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    stereo::Card grCard, transferCard, scCard, compCard;
    GrArcMeter grMeter;
    TransferView transfer;
    SidechainView sidechain;
    PillButton listen;
    ParamKnob threshold, ratio, attack, release, makeup;
    ParamSegments mode;
    ParamToggle autoGain;
    juce::Rectangle<int> modeCaption;

    int shownSet = -1;
    bool shownLinked = false, shownSplit = false;
};

//==============================================================================
ClassicCompPanel::ClassicCompPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, kType), impl (std::make_unique<Impl> (*this, p))
{
    neverTakeKeyboardFocus (*this);
}

ClassicCompPanel::~ClassicCompPanel() = default;

int ClassicCompPanel::getNumControls() const noexcept
{
    // threshold (knob + transfer node), ratio, attack, release, makeup, mode, auto gain, detection high-pass / bell
    // frequency / bell gain / shelf frequency / shelf gain (nodes)
    return 12;
}

void ClassicCompPanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void ClassicCompPanel::refresh()
{
    ModulePanel::refresh();
    impl->rebind();
    impl->update();
}

void ClassicCompPanel::channelSetChanged()
{
    impl->rebind();
    impl->update();
}

void ClassicCompPanel::paint (juce::Graphics& g) { ModulePanel::paint (g); }

void ClassicCompPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
