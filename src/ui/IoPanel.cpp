#include "ui/IoPanel.h"

#include "ui/Controls.h"

#include "core/TruePeak.h"

#include <array>

namespace ps::ui
{
namespace
{
using MeterType = UiOptions::MeterType;
using MeterScale = UiOptions::MeterScale;

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr float kFallDbPerSecond = 20.0f;
constexpr int kSubBlocks = 512;   // 10 ms each: up to 5.12 s of RMS window
constexpr int kHoldTicks = 600;   // peak-hold window memory: 20 s of 30 Hz ticks (longer holds never fall)

float toDb (float linear) { return linear > 1.0e-10f ? 20.0f * std::log10 (linear) : -200.0f; }

//==============================================================================
// Meter scale: maps a level (dBFS or LUFS) to 0..1 and lists the ticks.
struct Scale
{
    float ref = 0.0f;         // level labelled "0" (LU / K scales); 0 for dBFS scales
    float minDb = -60.0f, maxDb = 0.0f;
    bool relative = false;    // labels relative to ref (LU / K)
    int kind = 0;             // 0 linear, 1 non-linear
    std::vector<float> ticks; // absolute levels

    float norm (float db) const
    {
        if (db <= minDb)
            return 0.0f;

        if (kind == 1)
        {
            // dB non-linear: the top 24 dB get 60 % of the height.
            const float d = juce::jlimit (minDb, maxDb, db);
            if (d >= -24.0f) return 0.4f + 0.6f * (d + 24.0f) / 24.0f;
            if (d >= -48.0f) return 0.15f + 0.25f * (d + 48.0f) / 24.0f;
            return 0.15f * (d - minDb) / (-48.0f - minDb);
        }

        return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    }

    juce::String label (float tick) const
    {
        const float v = relative ? tick - ref : tick;
        const int i = juce::roundToInt (v);
        return (relative && i > 0 ? "+" : "") + juce::String (i);
    }
};

Scale makeScale (const UiOptions& o)
{
    Scale s;

    if (o.meterType == MeterType::KSystem)
    {
        const float k = (float) o.kSystemType;
        s.ref = -k;
        s.relative = true;
        s.minDb = -k - 30.0f;
        s.maxDb = 0.0f;

        for (float rel : { k, 8.0f, 4.0f, 0.0f, -4.0f, -8.0f, -12.0f, -20.0f, -30.0f })
            if (rel <= k + 0.01f)
                s.ticks.push_back (s.ref + rel);

        return s;
    }

    switch (o.meterScale)
    {
        case MeterScale::DbLinear:
            s.minDb = -60.0f;
            s.ticks = { 0, -6, -12, -18, -24, -30, -40, -50, -60 };
            break;

        case MeterScale::DbNonLinear:
            s.kind = 1;
            s.minDb = -72.0f;
            s.ticks = { 0, -3, -6, -9, -12, -18, -24, -36, -48, -60 };
            break;

        case MeterScale::Bs1771:
            s.ref = -24.0f, s.relative = true, s.minDb = -45.0f, s.maxDb = -15.0f;
            for (float lu = 9.0f; lu >= -21.0f; lu -= 3.0f)
                s.ticks.push_back (s.ref + lu);
            break;

        case MeterScale::Ebu9:
            s.ref = -23.0f, s.relative = true, s.minDb = -41.0f, s.maxDb = -14.0f;
            for (float lu = 9.0f; lu >= -18.0f; lu -= 3.0f)
                s.ticks.push_back (s.ref + lu);
            break;

        case MeterScale::Ebu18:
            s.ref = -23.0f, s.relative = true, s.minDb = -59.0f, s.maxDb = -5.0f;
            for (float lu = 18.0f; lu >= -36.0f; lu -= 6.0f)
                s.ticks.push_back (s.ref + lu);
            break;
    }

    return s;
}

// Colour of a level on a scale: calm below the reference zone, amber close to the top, coral at the very top.
juce::Colour levelColour (const Scale& s, float db)
{
    if (s.relative)
    {
        const float rel = db - s.ref;
        if (rel > 4.0f) return Colours::major;
        if (rel > 0.0f) return Colours::minor;
        return Colours::good;
    }

    if (db > -1.0f)  return Colours::major;
    if (db > -9.0f)  return blend (Colours::good, Colours::minor, (db + 9.0f) / 8.0f);
    return Colours::good;
}

//==============================================================================
struct Side
{
    void prepare (double sr)
    {
        sampleRate = sr;
        subLength = juce::jmax (1, (int) std::round (sr * 0.01));
        tp[0].prepare (sr);
        tp[1].prepare (sr);
        reset();
    }

    void reset()
    {
        for (auto& r : ring)
            r.fill (0.0);
        ringPos = 0;
        ringFilled = 0;
        subCount = 0;
        subSum = { 0.0, 0.0 };
        blockPeak = { 0.0f, 0.0f };
        shownPeak = { -200.0f, -200.0f };
        hold = { -200.0f, -200.0f };
        for (auto& h : holdRing)
            h.fill (-200.0f);
        rmsDb = { -200.0f, -200.0f };
        maxPeakDb = -200.0f;
        maxTruePeakDb = -200.0f;
        tp[0].reset();
        tp[1].reset();
        overTruePeak = false;
    }

    void resetHolds()
    {
        hold = { -200.0f, -200.0f };
        for (auto& h : holdRing)
            h.fill (-200.0f);
        overTruePeak = false;
    }

    void process (const float* l, const float* r, int n, bool midSide)
    {
        if (l == nullptr || n <= 0)
            return;

        if (r == nullptr)
            r = l;

        for (int i = 0; i < n; ++i)
        {
            const float a = midSide ? 0.5f * (l[i] + r[i]) : l[i];
            const float b = midSide ? 0.5f * (l[i] - r[i]) : r[i];
            blockPeak[0] = juce::jmax (blockPeak[0], std::abs (a));
            blockPeak[1] = juce::jmax (blockPeak[1], std::abs (b));
            maxSample = juce::jmax (maxSample, std::abs (l[i]), std::abs (r[i]));
            subSum[0] += (double) a * a;
            subSum[1] += (double) b * b;

            if (++subCount >= subLength)
            {
                ring[0][(size_t) ringPos] = subSum[0];
                ring[1][(size_t) ringPos] = subSum[1];
                ringPos = (ringPos + 1) % kSubBlocks;
                ringFilled = juce::jmin (kSubBlocks, ringFilled + 1);
                subSum = { 0.0, 0.0 };
                subCount = 0;
            }
        }

        tp[0].process (l, nullptr, n);
        tp[1].process (r, nullptr, n);
    }

    void tick (const UiOptions& o)
    {
        const float fall = kFallDbPerSecond * kTickSeconds;
        const int holdLimit = o.peakHoldMs >= 1.0e8f ? std::numeric_limits<int>::max()
                                                     : juce::jmax (1, (int) (o.peakHoldMs / 1000.0f / kTickSeconds));
        const int windowBlocks = juce::jlimit (1, kSubBlocks, (int) std::round (o.rmsWindowMs / 10.0f));

        for (int c = 0; c < 2; ++c)
        {
            const float p = toDb (blockPeak[(size_t) c]);
            blockPeak[(size_t) c] = 0.0f;
            shownPeak[(size_t) c] = juce::jmax (p, shownPeak[(size_t) c] - fall);

            // Peak hold = the highest tick peak of the last holdLimit ticks (a sliding window, so channels with the
            // same material hold the same level); when that drops, the shown hold falls towards it.
            holdRing[(size_t) c][(size_t) holdPos] = p;
            float target = -200.0f;

            if (holdLimit >= kHoldTicks)
            {
                target = juce::jmax (hold[(size_t) c], p);
            }
            else
            {
                for (int k = 0; k <= holdLimit; ++k)
                    target = juce::jmax (target, holdRing[(size_t) c][(size_t) ((holdPos - k + kHoldTicks) % kHoldTicks)]);
            }

            hold[(size_t) c] = target >= hold[(size_t) c] ? target : juce::jmax (target, hold[(size_t) c] - fall * 1.5f);

            const int count = juce::jmin (windowBlocks, ringFilled);
            double sum = 0.0;

            for (int k = 0; k < count; ++k)
                sum += ring[(size_t) c][(size_t) ((ringPos - 1 - k + kSubBlocks) % kSubBlocks)];

            rmsDb[(size_t) c] = count > 0 ? (float) (10.0 * std::log10 (sum / ((double) count * subLength) + 1.0e-20)) : -200.0f;
        }

        holdPos = (holdPos + 1) % kHoldTicks;
        maxPeakDb = juce::jmax (maxPeakDb, toDb (maxSample));
        maxSample = 0.0f;
        const float tpDb = toDb (juce::jmax (tp[0].truePeakLinear(), tp[1].truePeakLinear()));
        overTruePeak = overTruePeak || tpDb > 0.0f;
        maxTruePeakDb = tpDb;
    }

    double sampleRate = 48000.0;
    int subLength = 480, subCount = 0, ringPos = 0, ringFilled = 0;
    std::array<double, 2> subSum {};
    std::array<std::array<double, kSubBlocks>, 2> ring {};
    std::array<float, 2> blockPeak {}, shownPeak { -200.0f, -200.0f }, hold { -200.0f, -200.0f }, rmsDb { -200.0f, -200.0f };
    std::array<std::array<float, kHoldTicks>, 2> holdRing = makeHoldRing();
    int holdPos = 0;

    static std::array<std::array<float, kHoldTicks>, 2> makeHoldRing()
    {
        std::array<std::array<float, kHoldTicks>, 2> r {};
        for (auto& h : r)
            h.fill (-200.0f);
        return r;
    }
    float maxSample = 0.0f, maxPeakDb = -200.0f, maxTruePeakDb = -200.0f;
    bool overTruePeak = false;
    ps::TruePeakMeter tp[2];
};

//==============================================================================
// Dither shortcut: LED (dither on), "Dither", bit depth. Click opens the Dither panel.
class DitherButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DitherButton (PureSheenProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Dither: settings for the final bit depth (for example 16 bit for a CD). Click to open.");
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = proc.getParameterValue ("dith.on") >= 0.5f;
        const bool hover = isMouseOver (true);
        const float r = b.getHeight() * 0.5f;
        drawChip (g, b, r, selected ? blend (Colours::panelHover, Colours::accent, 0.2f) : (hover ? Colours::panelHover : Colours::panelRaised),
                  selected ? Colours::accent.withAlpha (0.8f) : Colours::outline.brighter (hover ? 0.2f : 0.0f));

        const float led = juce::jmin (7.0f, b.getHeight() * 0.32f);
        drawLed (g, juce::Rectangle<float> (led, led).withCentre ({ b.getX() + r + 1.0f, b.getCentreY() }), on, Colours::accent);

        static const char* bits[] = { "24", "20", "16", "12", "8" };
        const int bi = juce::jlimit (0, 4, juce::roundToInt (proc.getParameterValue ("dith.bits")));
        auto text = b.withTrimmedLeft (r + led + 6.0f).withTrimmedRight (8.0f);
        const auto labelFont = font (juce::jmin (12.0f, b.getHeight() * 0.5f), 600);
        const auto status = on ? juce::String (bits[bi]) + "b" : juce::String ("off");
        const bool roomForStatus = text.getWidth() >= textWidth (labelFont, "Dither") + textWidth (font (10.5f, 700), status) + 6.0f;
        g.setFont (labelFont);
        g.setColour (hover || selected ? Colours::text : Colours::textDim.brighter (0.1f));
        g.drawFittedText ("Dither", text.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

        if (roomForStatus)
        {
            g.setFont (font (10.5f, 700));
            g.setColour (on ? Colours::accent : Colours::textFaint.brighter (0.2f));
            g.drawText (status, text, juce::Justification::centredRight, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isLeftClick (e) && getLocalBounds().contains (e.getPosition()) && onClick != nullptr)
            onClick();
    }

    std::function<void()> onClick;
    bool selected = false;

private:
    PureSheenProcessor& proc;
};
} // namespace

//==============================================================================
struct IoPanel::Impl
{
    Impl (IoPanel& o, EditorContext& c)
        : owner (o), ctx (c), proc (c.getProcessor()),
          inL (proc, "in.gainL", false, "L"), inR (proc, "in.gainR", false, "R"),
          outL (proc, "out.gainL", false, "L"), outR (proc, "out.gainR", false, "R"),
          gainMatch (proc, "io.gainMatch", "Gain Match"), bypass (proc, "io.bypass", "Bypass"),
          mono (proc, "io.mono", "Mono"), swap (proc, "io.swap", "Swap"), dither (proc)
    {
        for (auto* f : { &inL, &inR, &outL, &outR })
        {
            owner.addAndMakeVisible (*f);
            f->setColour (Colours::accent);
        }

        gainMatch.setTooltip ("Gain Match: compare at equal loudness. When you bypass, the bypassed sound is turned to "
                              "the same loudness as the processed one, so you judge the sound and not the volume "
                              "(Options > Gain Match chooses which side is levelled).");
        bypass.setTooltip ("Bypass: hear the unprocessed input (latency kept, so the timing does not jump)");
        bypass.setOnColour (Colours::minor);
        mono.setTooltip ("Mono: sum left and right to check the mix in mono");
        mono.setOnColour (Colours::info);
        swap.setTooltip ("Swap: swap the left and right channels");
        swap.setOnColour (Colours::info);

        for (juce::Component* comp : std::initializer_list<juce::Component*> { &gainMatch, &bypass, &mono, &swap,
                                                                            &inLink, &outLink, &reset })
            owner.addAndMakeVisible (comp);

        if (proc.getProduct().isSuite())   // the shortcut to the suite's Dither panel
            owner.addAndMakeVisible (dither);

        matchValue.setInterceptsMouseClicks (false, false);
        matchValue.setJustificationType (juce::Justification::centredRight);
        matchValue.setFont (font (11.0f, 700));
        matchValue.setColour (juce::Label::textColourId, Colours::accent);
        owner.addAndMakeVisible (matchValue);

        inLink.setClickingTogglesState (false);
        outLink.setClickingTogglesState (false);
        inLink.setTooltip ("Link the left and right input gain");
        outLink.setTooltip ("Link the left and right output gain");
        inLink.onClick = [this] { ctx.setGainLinked (false, ! ctx.isGainLinked (false)); updateLinks(); };
        outLink.onClick = [this] { ctx.setGainLinked (true, ! ctx.isGainLinked (true)); updateLinks(); };
        reset.setTooltip ("Reset the readouts: peaks, clip lights, integrated loudness and LRA");
        reset.onClick = [this] { owner.resetReadouts(); };

        dither.onClick = [this]
        {
            if (ctx.getSelection() != -1)
            {
                ctx.select (-1);
            }
            else
            {
                const auto order = proc.getChainOrder();
                ctx.select (order.empty() ? -1 : (int) order.front());
            }
        };

        updateLinks();
        scale = makeScale (ctx.getUiOptions());
    }

    void updateLinks()
    {
        const bool li = ctx.isGainLinked (false), lo = ctx.isGainLinked (true);
        inLink.setToggleState (li, juce::dontSendNotification);
        outLink.setToggleState (lo, juce::dontSendNotification);
        inL.setLinkedParamId (li ? "in.gainR" : "");
        inR.setLinkedParamId (li ? "in.gainL" : "");
        outL.setLinkedParamId (lo ? "out.gainR" : "");
        outR.setLinkedParamId (lo ? "out.gainL" : "");
    }

    bool compact() const { return owner.getHeight() < 560; }

    //==========================================================================
    void paintMeters (juce::Graphics& g, const Side& side, juce::Rectangle<float> column, bool isOutput) const
    {
        const auto& o = ctx.getUiOptions();
        const auto& m = proc.getChain().getIoMeters();
        const bool loudnessType = o.meterType == MeterType::Momentary || o.meterType == MeterType::ShortTerm
                                  || o.meterType == MeterType::Integrated;
        const float barW = (column.getWidth() - 4.0f) * 0.5f;

        float lufs = -200.0f;

        if (loudnessType)
        {
            if (o.meterType == MeterType::Momentary)
                lufs = isOutput ? m.outputMomentaryLufs.load() : m.inputMomentaryLufs.load();
            else if (o.meterType == MeterType::ShortTerm)
                lufs = isOutput ? m.outputShortTermLufs.load() : m.inputShortTermLufs.load();
            else
                lufs = isOutput ? m.outputIntegratedLufs.load() : m.inputIntegratedLufs.load();
        }

        for (int c = 0; c < 2; ++c)
        {
            const auto bar = juce::Rectangle<float> (column.getX() + (float) c * (barW + 4.0f), column.getY(), barW, column.getHeight());
            const bool clip = (isOutput ? m.outputClip[c].load() : m.inputClip[c].load()) > 0.5f
                              || (o.detectTruePeaks && side.overTruePeak) || side.hold[(size_t) c] > 0.0f;
            const auto clipArea = bar.withHeight (4.0f);
            const auto meter = bar.withTrimmedTop (6.0f);

            g.setColour (clip ? Colours::major : juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (clipArea, 1.5f);

            g.setColour (juce::Colours::black.withAlpha (0.38f));
            g.fillRoundedRectangle (meter, 3.0f);

            // Value to draw as the solid bar and as the translucent peak bar.
            float solid = -200.0f, ghost = -200.0f;

            switch (o.meterType)
            {
                case MeterType::Peak:     solid = side.shownPeak[(size_t) c]; break;
                case MeterType::Rms:      solid = side.rmsDb[(size_t) c]; break;
                case MeterType::PeakRms:
                case MeterType::KSystem:  solid = side.rmsDb[(size_t) c]; ghost = side.shownPeak[(size_t) c]; break;
                case MeterType::Momentary:
                case MeterType::ShortTerm:
                case MeterType::Integrated: solid = lufs; break;
            }

            auto fillTo = [&] (float db, float alpha)
            {
                const float t = scale.norm (db);

                if (t <= 0.0f)
                    return;

                const auto f = meter.withTop (meter.getBottom() - meter.getHeight() * t);
                juce::ColourGradient grad (levelColour (scale, scale.minDb), 0.0f, meter.getBottom(),
                                           levelColour (scale, scale.maxDb), 0.0f, meter.getY(), false);
                grad.clearColours();

                for (float d : { scale.minDb, scale.relative ? scale.ref - 0.1f : -9.0f, scale.relative ? scale.ref + 4.0f : -1.0f, scale.maxDb })
                    grad.addColour (juce::jlimit (0.0, 1.0, (double) scale.norm (d)), levelColour (scale, d).withAlpha (alpha));

                g.setGradientFill (grad);
                g.fillRoundedRectangle (f, 2.5f);
            };

            fillTo (ghost, 0.32f);
            fillTo (solid, 0.95f);

            // Peak hold marker.
            if (! loudnessType && side.hold[(size_t) c] > scale.minDb)
            {
                const float y = meter.getBottom() - meter.getHeight() * scale.norm (side.hold[(size_t) c]);
                g.setColour (levelColour (scale, side.hold[(size_t) c]).brighter (0.3f));
                g.fillRect (juce::Rectangle<float> (meter.getX(), y - 1.0f, meter.getWidth(), 2.0f));
            }
        }
    }

    void paintScale (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        const auto meter = area.withTrimmedTop (6.0f);
        g.setFont (font (9.5f, 600));

        float lastY = -100.0f;

        for (float t : scale.ticks)
        {
            const float y = meter.getBottom() - meter.getHeight() * scale.norm (t);

            if (std::abs (y - lastY) < 11.0f)
                continue;

            lastY = y;
            g.setColour (Colours::textFaint);
            g.fillRect (juce::Rectangle<float> (area.getX(), y, 3.0f, 1.0f));
            g.fillRect (juce::Rectangle<float> (area.getRight() - 3.0f, y, 3.0f, 1.0f));
            g.setColour (Colours::textSubtle);
            g.drawText (scale.label (t), juce::Rectangle<float> (area.getX(), y - 6.0f, area.getWidth(), 12.0f),
                        juce::Justification::centred, false);
        }
    }

    juce::String peakText (float db) const
    {
        if (db < -150.0f)
            return "-inf";

        return juce::String (db, 1);
    }

    void paint (juce::Graphics& g) const
    {
        const auto b = owner.getLocalBounds().toFloat();
        drawPanel (g, b);
        const auto& o = ctx.getUiOptions();
        const auto& m = proc.getChain().getIoMeters();

        // Column titles.
        g.setFont (font (10.5f, 700).withExtraKerningFactor (0.09f));
        g.setColour (Colours::textDim);
        g.drawText ("INPUT", inTitle, juce::Justification::centredLeft, false);
        g.drawText ("OUTPUT", outTitle, juce::Justification::centredLeft, false);

        // Peak-hold numbers above the bars.
        auto values = [&] (juce::Rectangle<float> area, const Side& side)
        {
            const float w = (area.getWidth() - 4.0f) * 0.5f;

            for (int c = 0; c < 2; ++c)
            {
                const float v = side.hold[(size_t) c];
                g.setFont (font (10.0f, 700));
                g.setColour (v > 0.0f ? Colours::major : (v > -1.0f ? Colours::minor : Colours::text.withAlpha (0.85f)));
                g.drawFittedText (peakText (v), juce::Rectangle<float> (area.getX() + (float) c * (w + 4.0f), area.getY(), w, area.getHeight()).toNearestInt(),
                                  juce::Justification::centred, 1, 0.7f);
            }
        };
        values (inValues, input);
        values (outValues, output);

        paintMeters (g, input, inMeter, false);
        paintMeters (g, output, outMeter, true);
        paintScale (g, scaleArea);

        // Channel labels.
        g.setFont (font (10.0f, 700));
        g.setColour (Colours::textSubtle);
        const juce::String a = o.meterMidSide ? "M" : "L", s = o.meterMidSide ? "S" : "R";

        for (auto area : { inLabels, outLabels })
        {
            g.drawText (a, area.withWidth (area.getWidth() * 0.5f), juce::Justification::centred, false);
            g.drawText (s, area.withTrimmedLeft (area.getWidth() * 0.5f), juce::Justification::centred, false);
        }

        // Unit caption under the scale.
        g.setFont (font (9.0f, 700));
        const bool loudness = o.meterType == MeterType::Momentary || o.meterType == MeterType::ShortTerm || o.meterType == MeterType::Integrated;
        g.drawText (o.meterType == MeterType::KSystem ? "K-" + juce::String (o.kSystemType)
                                                      : (scale.relative ? juce::String ("LU") : (loudness ? juce::String ("LUFS") : juce::String ("dB"))),
                    unitArea, juce::Justification::centred, false);

        // Readouts.
        g.setFont (font (10.5f, 700).withExtraKerningFactor (0.09f));
        g.setColour (Colours::textDim);
        g.drawText ("LEVELS", readoutTitle, juce::Justification::centredLeft, false);
        g.setFont (font (9.5f, 700).withExtraKerningFactor (0.06f));
        g.setColour (Colours::textSubtle);
        g.drawText ("IN", readoutCols[0], juce::Justification::centredRight, false);
        g.drawText ("OUT", readoutCols[1], juce::Justification::centredRight, false);

        struct Row { const char* label; juce::String in, out; juce::Colour outColour; };
        auto lufsText = [] (float v) { return v < -150.0f ? juce::String ("--") : juce::String (v, 1); };
        auto dbText = [] (float v) { return v < -150.0f ? juce::String ("-inf") : juce::String (v, 1); };
        const float outTp = output.maxTruePeakDb;
        std::vector<Row> rows;

        if (! compact())
        {
            rows.push_back ({ "Peak", dbText (input.maxPeakDb), dbText (output.maxPeakDb),
                              output.maxPeakDb > 0.0f ? Colours::major : Colours::text });
            rows.push_back ({ "RMS", dbText (juce::jmax (input.rmsDb[0], input.rmsDb[1])), dbText (juce::jmax (output.rmsDb[0], output.rmsDb[1])), Colours::text });
        }

        rows.push_back ({ "Momentary", lufsText (m.inputMomentaryLufs.load()), lufsText (m.outputMomentaryLufs.load()), Colours::text });
        rows.push_back ({ "Short-term", lufsText (m.inputShortTermLufs.load()), lufsText (m.outputShortTermLufs.load()), Colours::text });
        rows.push_back ({ "Integrated", lufsText (m.inputIntegratedLufs.load()), lufsText (m.outputIntegratedLufs.load()), Colours::accent.brighter (0.2f) });
        rows.push_back ({ "True peak", dbText (input.maxTruePeakDb), dbText (outTp),
                          outTp > 0.0f ? Colours::major : (outTp > -1.0f ? Colours::minor : Colours::text) });
        rows.push_back ({ "LRA", "", hasLra ? juce::String (m.outputLra.load(), 1) : juce::String ("--"), Colours::text });

        const float rowH = compact() ? 15.0f : 17.0f;
        auto y = readoutTitle.getBottom() + 2.0f;

        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto row = juce::Rectangle<float> (readoutTitle.getX(), y, readoutTitle.getWidth(), rowH);

            if (i % 2 == 0)
            {
                g.setColour (juce::Colours::white.withAlpha (0.025f));
                g.fillRoundedRectangle (row.expanded (4.0f, 0.0f), 3.0f);
            }

            g.setFont (font (11.0f, 600));
            g.setColour (Colours::textDim);
            g.drawText (rows[i].label, row, juce::Justification::centredLeft, false);
            g.setFont (font (11.0f, 600));
            g.setColour (Colours::text.withAlpha (0.75f));
            g.drawText (rows[i].in, row.withX (readoutCols[0].getX()).withWidth (readoutCols[0].getWidth()), juce::Justification::centredRight, false);
            g.setFont (font (11.0f, 700));
            g.setColour (rows[i].outColour);
            g.drawText (rows[i].out, row.withX (readoutCols[1].getX()).withWidth (readoutCols[1].getWidth()), juce::Justification::centredRight, false);
            y += rowH;
        }

        // Group captions.
        g.setFont (font (10.5f, 700).withExtraKerningFactor (0.09f));
        g.setColour (Colours::textDim);
        g.drawText ("MONITOR", monitorTitle, juce::Justification::centredLeft, false);
    }

    void layout()
    {
        auto r = owner.getLocalBounds().reduced (10, 10);
        const bool small = compact();
        const int colW = (r.getWidth() - 24) / 2;

        // Bottom-up: monitor toggles, readouts.
        const int toggleH = 24, gap = 6;
        auto toggles = r.removeFromBottom (3 * toggleH + 2 * gap);
        {
            auto row = toggles.removeFromTop (toggleH);
            gainMatch.setBounds (row);
            matchValue.setBounds (row.withTrimmedRight (10).withLeft (row.getRight() - 64));
            toggles.removeFromTop (gap);
            row = toggles.removeFromTop (toggleH);
            bypass.setBounds (row.removeFromLeft ((row.getWidth() - gap) / 2));
            row.removeFromLeft (gap);
            mono.setBounds (row);
            toggles.removeFromTop (gap);
            row = toggles.removeFromTop (toggleH);
            if (proc.getProduct().isSuite())
            {
                swap.setBounds (row.removeFromLeft ((row.getWidth() - gap) / 2));
                row.removeFromLeft (gap);
                dither.setBounds (row);
            }
            else
            {
                swap.setBounds (row);
            }
        }
        monitorTitle = r.removeFromBottom (20).toFloat();
        r.removeFromBottom (small ? 4 : 8);

        const int numRows = small ? 5 : 7;
        const float rowH = small ? 15.0f : 17.0f;
        auto readouts = r.removeFromBottom ((int) (20.0f + rowH * (float) numRows + 2.0f));
        readoutTitle = readouts.removeFromTop (20).toFloat().withTrimmedRight (0.0f);
        reset.setBounds (readoutTitle.toNearestInt().withLeft (readoutTitle.toNearestInt().getX() + 46).withWidth (20).withSizeKeepingCentre (20, 20));
        readoutCols[0] = juce::Rectangle<float> (readoutTitle.getRight() - 96.0f, readoutTitle.getY(), 44.0f, 20.0f);
        readoutCols[1] = juce::Rectangle<float> (readoutTitle.getRight() - 48.0f, readoutTitle.getY(), 48.0f, 20.0f);
        r.removeFromBottom (small ? 6 : 10);

        // Top: titles with link buttons, values, meters, channel labels, gain faders.
        auto titles = r.removeFromTop (20);
        inTitle = titles.withWidth (colW).toFloat();
        outTitle = titles.withX (titles.getRight() - colW).withWidth (colW).toFloat();
        inLink.setBounds (inTitle.toNearestInt().removeFromRight (20).withSizeKeepingCentre (20, 20));
        outLink.setBounds (outTitle.toNearestInt().removeFromRight (20).withSizeKeepingCentre (20, 20));
        r.removeFromTop (2);

        auto faders = r.removeFromBottom (2 * 20 + 4);
        {
            auto left = faders.withWidth (colW), right = faders.withX (faders.getRight() - colW).withWidth (colW);
            inL.setBounds (left.removeFromTop (20));
            left.removeFromTop (4);
            inR.setBounds (left);
            outL.setBounds (right.removeFromTop (20));
            right.removeFromTop (4);
            outR.setBounds (right);
        }
        r.removeFromBottom (6);

        auto labels = r.removeFromBottom (14);
        inLabels = labels.withWidth (colW).toFloat();
        outLabels = labels.withX (labels.getRight() - colW).withWidth (colW).toFloat();
        unitArea = juce::Rectangle<float> ((float) labels.getX() + (float) colW, (float) labels.getY(), 24.0f, 14.0f);

        auto vals = r.removeFromTop (14);
        inValues = vals.withWidth (colW).toFloat();
        outValues = vals.withX (vals.getRight() - colW).withWidth (colW).toFloat();
        r.removeFromTop (3);

        inMeter = r.withWidth (colW).toFloat();
        outMeter = r.withX (r.getRight() - colW).withWidth (colW).toFloat();
        scaleArea = juce::Rectangle<float> ((float) r.getX() + (float) colW, (float) r.getY(), 24.0f, (float) r.getHeight());
    }

    IoPanel& owner;
    EditorContext& ctx;
    PureSheenProcessor& proc;

    Side input, output;
    Scale scale;
    bool hasLra = false;

    ParamFader inL, inR, outL, outR;
    ParamToggle gainMatch, bypass, mono, swap;
    DitherButton dither;
    juce::Label matchValue;
    IconButton inLink { "Link input gain", IconButton::Icon::link };
    IconButton outLink { "Link output gain", IconButton::Icon::link };
    IconButton reset { "Reset readouts", IconButton::Icon::reset };

    juce::Rectangle<float> inTitle, outTitle, inValues, outValues, inMeter, outMeter, scaleArea, inLabels, outLabels,
        unitArea, readoutTitle, monitorTitle;
    std::array<juce::Rectangle<float>, 2> readoutCols;
};

//==============================================================================
IoPanel::IoPanel (EditorContext& ctx) : impl (std::make_unique<Impl> (*this, ctx))
{
    setTitle ("Input and output");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setSampleRate (48000.0);
}

IoPanel::~IoPanel() = default;

void IoPanel::setSampleRate (double sr)
{
    if (sr <= 0.0)
        return;

    if (std::abs (sr - impl->input.sampleRate) > 0.5 || impl->input.subLength <= 0)
    {
        impl->input.prepare (sr);
        impl->output.prepare (sr);
    }
}

void IoPanel::processInput (const float* l, const float* r, int n)
{
    impl->input.process (l, r, n, impl->ctx.getUiOptions().meterMidSide);
}

void IoPanel::processOutput (const float* l, const float* r, int n)
{
    impl->output.process (l, r, n, impl->ctx.getUiOptions().meterMidSide);
}

void IoPanel::tick()
{
    auto& d = *impl;
    const auto& o = d.ctx.getUiOptions();
    d.input.tick (o);
    d.output.tick (o);
    d.hasLra = d.proc.getChain().getIoMeters().outputIntegratedLufs.load() > -150.0f;

    const bool match = d.proc.getParameterValue ("io.gainMatch") >= 0.5f;
    const float matchDb = d.proc.getChain().getIoMeters().gainMatchDb.load();
    d.matchValue.setText (match ? fmt::db (matchDb, 1, true) : juce::String(), juce::dontSendNotification);
    d.dither.repaint();
    repaint();
}

void IoPanel::resetReadouts()
{
    auto& d = *impl;
    auto& meters = d.proc.getChain().getIoMeters();   // the UI resets the clip latches (Chain.h)

    for (int c = 0; c < 2; ++c)
    {
        meters.inputClip[c].store (0.0f);
        meters.outputClip[c].store (0.0f);
    }

    d.proc.getChain().resetIntegratedLoudness();
    d.input.reset();
    d.output.reset();
    repaint();
}

void IoPanel::optionsChanged()
{
    impl->scale = makeScale (impl->ctx.getUiOptions());
    impl->input.reset();
    impl->output.reset();
    repaint();
}

void IoPanel::setDitherSelected (bool s)
{
    if (impl->dither.selected != s)
    {
        impl->dither.selected = s;
        impl->dither.repaint();
    }
}

void IoPanel::paint (juce::Graphics& g) { impl->paint (g); }
void IoPanel::resized() { impl->layout(); }

void IoPanel::mouseUp (const juce::MouseEvent& e)
{
    // A click on the meters (or the clip lights) resets the peak holds and clip lights.
    if (! isLeftClick (e))
        return;

    const auto p = e.position;

    if (impl->inMeter.expanded (2.0f).contains (p) || impl->outMeter.expanded (2.0f).contains (p)
        || impl->inValues.contains (p) || impl->outValues.contains (p))
    {
        auto& meters = impl->proc.getChain().getIoMeters();

        for (int c = 0; c < 2; ++c)
        {
            meters.inputClip[c].store (0.0f);
            meters.outputClip[c].store (0.0f);
        }

        impl->input.resetHolds();
        impl->output.resetHolds();
        repaint();
    }
}

} // namespace ps::ui
