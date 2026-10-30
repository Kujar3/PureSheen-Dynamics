#include "ui/LowRegionDisplay.h"

#include "dsp/LowContrast.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/LowEndWidgets.h"

#include <array>
#include <cmath>

// Low Contrast display. Owner: ui-lowend. Contract: LowRegionDisplay.h.
//
// Geometry: one dark rounded surface like the multiband display: a kHeaderH strip on top (the region's header with its
// S button, aligned with the region's x range), the plot below. The region edges are stored in Hz ("lowc.low",
// "lowc.high", shared by both channel sets); the module keeps high >= low x 2^(1/3) (readLowContrastParams), so the
// display draws and edits with that rule: dragging one edge into the other pushes the other along in the same gesture.
namespace ps::ui
{
namespace
{
constexpr float kHeaderH = 28.0f;
constexpr float kMinHz = 16.0f, kMaxHz = 1000.0f;
constexpr float kRegionMin = 20.0f, kRegionMax = 300.0f;
constexpr float kThird = 1.25992105f;     // 2^(1/3): minimum region width and the taper length
constexpr float kLevelMin = -96.0f, kLevelMax = 0.0f;
constexpr float kDiffRange = 12.0f;       // difference curve axis +-12 dB
constexpr float kEdgeGrab = 6.0f;
const juce::String kLowId ("lowc.low"), kHighId ("lowc.high");

// The module's region weight: 1 inside [lo, hi], a raised cosine down to 0 over 1/3 octave outside (log f).
float regionWeight (float hz, float lo, float hi) noexcept
{
    if (hz >= lo && hz <= hi)
        return 1.0f;

    const float t = 3.0f * std::abs (std::log2 (hz < lo ? lo / hz : hz / hi));
    return t >= 1.0f ? 0.0f : 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * t));
}

enum class Target { none, low, high, region, solo };
} // namespace

//==============================================================================
struct LowRegionDisplay::Impl
{
    Impl (LowRegionDisplay& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), services (s), colour (moduleColour (ps::ModuleType::LowContrast)),
          lowParam (p.getParameter (kLowId)), highParam (p.getParameter (kHighId))
    {
        curve.fill (0.0f);
    }

    ~Impl()
    {
        endGestures();

        // Leaving the panel ends monitoring even if an Alt audition was temporarily overriding a latched solo.
        if (getSolo())
            setSolo (false);
    }

    //==========================================================================
    // Parameters
    float low() const { return juce::jlimit (kRegionMin, kRegionMax, processor.getParameterValue (kLowId)); }
    float highRaw() const { return juce::jlimit (kRegionMin, kRegionMax, processor.getParameterValue (kHighId)); }
    // The high edge the module uses.
    float high() const { return juce::jmin (kRegionMax, juce::jmax (highRaw(), low() * kThird)); }

    void setValue (juce::RangedAudioParameter* param, bool& open, float hz)
    {
        if (param == nullptr)
            return;

        if (! open)
        {
            open = true;
            param->beginChangeGesture();
        }

        const float norm = param->convertTo0to1 (hz);

        if (std::abs (norm - param->getValue()) > 1.0e-7f)
            param->setValueNotifyingHost (norm);
    }

    void endGestures()
    {
        if (lowOpen && lowParam != nullptr)
            lowParam->endChangeGesture();

        if (highOpen && highParam != nullptr)
            highParam->endChangeGesture();

        lowOpen = highOpen = false;
    }

    // Moves one edge (pushing the other when needed), all inside the open gesture.
    void moveLow (float hz)
    {
        hz = juce::jlimit (kRegionMin, kRegionMax / kThird, hz);
        setValue (lowParam, lowOpen, hz);

        if (highRaw() < hz * kThird)
            setValue (highParam, highOpen, hz * kThird);
    }

    void moveHigh (float hz)
    {
        hz = juce::jlimit (kRegionMin * kThird, kRegionMax, hz);
        setValue (highParam, highOpen, hz);

        if (low() > hz / kThird)
            setValue (lowParam, lowOpen, hz / kThird);
    }

    void moveRegion (float factor)
    {
        float lo = startLow * factor, hi = startHigh * factor;

        if (lo < kRegionMin)
        {
            hi *= kRegionMin / lo;
            lo = kRegionMin;
        }

        if (hi > kRegionMax)
        {
            lo *= kRegionMax / hi;
            hi = kRegionMax;
        }

        setValue (lowParam, lowOpen, juce::jlimit (kRegionMin, kRegionMax, lo));
        setValue (highParam, highOpen, juce::jlimit (kRegionMin, kRegionMax, hi));
    }

    float defaultOf (const juce::String& id) const { return stereo::defaultValueOf (id); }

    void resetEdge (bool isLow)
    {
        float lo = low(), hi = highRaw();

        if (isLow)
            lo = defaultOf (kLowId);
        else
            hi = defaultOf (kHighId);

        if (hi < lo * kThird)
        {
            if (isLow)
                hi = juce::jmin (kRegionMax, lo * kThird);
            else
                lo = juce::jmax (kRegionMin, hi / kThird);
        }

        processor.setParameterValues ({ { kLowId, lo }, { kHighId, hi } }, "Low Contrast region: " + lowend::hzText (lo) + " - "
                                                                                   + lowend::hzText (hi));
    }

    void resetRegion()
    {
        processor.setParameterValues ({ { kLowId, defaultOf (kLowId) }, { kHighId, defaultOf (kHighId) } },
                                      "Low Contrast region reset");
    }

    bool getSolo() const { return processor.getMonitor (ps::ModuleType::LowContrast).soloRegion; }

    void setSolo (bool on)
    {
        auto m = processor.getMonitor (ps::ModuleType::LowContrast);
        const int part = on && currentChannelMode (processor, ps::ModuleType::LowContrast) == ps::ChannelMode::TransientSustain
                             ? channelSet : -1;

        if (m.soloRegion != on || m.soloChannelSet != part)
        {
            m.soloRegion = on;
            m.soloChannelSet = part;
            processor.setMonitor (ps::ModuleType::LowContrast, m);
        }

        shownSolo = on;
        owner.repaint();
    }

    //==========================================================================
    // Geometry
    juce::Rectangle<float> inner() const { return owner.getLocalBounds().toFloat().reduced (1.0f); }
    juce::Rectangle<float> headerArea() const { return inner().removeFromTop (kHeaderH); }
    juce::Rectangle<float> plotArea() const { return inner().withTrimmedTop (kHeaderH); }

    FreqAxis axis() const
    {
        FreqAxis a;
        a.minHz = kMinHz;
        a.maxHz = kMaxHz;
        a.area = plotArea();
        return a;
    }

    DbAxis levelAxis() const
    {
        DbAxis a;
        a.minDb = kLevelMin;
        a.maxDb = kLevelMax;
        a.area = plotArea();
        return a;
    }

    float diffY (float db) const
    {
        const auto p = plotArea();
        const float t = juce::jlimit (-kDiffRange, kDiffRange, db) / kDiffRange;
        return p.getCentreY() - t * p.getHeight() * 0.5f;
    }

    float lowX() const { return axis().hzToX (low()); }
    float highX() const { return axis().hzToX (high()); }

    juce::Rectangle<float> headerStrip() const
    {
        const auto h = headerArea();
        float x0 = lowX(), x1 = highX();
        const float minW = 150.0f;

        if (x1 - x0 < minW)
        {
            const float c = 0.5f * (x0 + x1);
            x0 = c - minW * 0.5f;
            x1 = c + minW * 0.5f;
        }

        x0 = juce::jmax (h.getX(), x0);
        x1 = juce::jmin (h.getRight(), juce::jmax (x1, x0 + minW));
        x0 = juce::jmin (x0, x1 - minW);
        return juce::Rectangle<float> (x0, h.getY(), x1 - x0, h.getHeight()).reduced (2.0f, 4.0f).withTrimmedTop (-1.0f);
    }

    juce::Rectangle<float> soloButton() const
    {
        return headerStrip().withTrimmedRight (4.0f).removeFromRight (18.0f).withSizeKeepingCentre (18.0f, 16.0f);
    }

    juce::Rectangle<float> edgeHandle (bool isLow) const
    {
        const auto p = plotArea();
        const float cy = p.getBottom() - juce::jmin (64.0f, p.getHeight() * 0.28f);
        return juce::Rectangle<float> (10.0f, 26.0f).withCentre ({ isLow ? lowX() : highX(), cy });
    }

    // Value chips above the edges, pushed apart when the edges are close.
    std::pair<juce::Rectangle<float>, juce::Rectangle<float>> chips() const
    {
        const auto p = plotArea();
        const auto f = font (11.0f, 700);
        const float wl = textWidth (f, lowend::hzText (low())) + 14.0f;
        const float wh = textWidth (f, lowend::hzText (high())) + 14.0f;
        auto a = juce::Rectangle<float> (wl, 20.0f).withCentre ({ lowX(), p.getY() + 16.0f });
        auto b = juce::Rectangle<float> (wh, 20.0f).withCentre ({ highX(), p.getY() + 16.0f });
        const float overlap = a.getRight() + 6.0f - b.getX();

        if (overlap > 0.0f)
        {
            a = a.translated (-overlap * 0.5f, 0.0f);
            b = b.translated (overlap * 0.5f, 0.0f);
        }

        const auto bounds = p.reduced (4.0f, 0.0f);

        if (a.getX() < bounds.getX())
        {
            const float d = bounds.getX() - a.getX();
            a = a.translated (d, 0.0f);
            b = b.translated (juce::jmax (0.0f, a.getRight() + 6.0f - b.getX()), 0.0f);
        }

        if (b.getRight() > bounds.getRight())
        {
            const float d = b.getRight() - bounds.getRight();
            b = b.translated (-d, 0.0f);
            a = a.translated (-juce::jmax (0.0f, a.getRight() + 6.0f - b.getX()), 0.0f);
        }

        return { a, b };
    }

    Target targetAt (juce::Point<float> pos) const
    {
        if (soloButton().expanded (2.0f).contains (pos))
            return Target::solo;

        const auto p = plotArea();
        const bool inHeader = headerStrip().contains (pos);

        if (! p.expanded (0.0f, 2.0f).contains (pos) && ! inHeader)
            return Target::none;

        const float lx = lowX(), hx = highX();
        const auto c = chips();

        if (edgeHandle (true).expanded (3.0f).contains (pos) || c.first.contains (pos))
            return Target::low;

        if (edgeHandle (false).expanded (3.0f).contains (pos) || c.second.contains (pos))
            return Target::high;

        if (! inHeader)
        {
            const float dl = std::abs (pos.x - lx), dh = std::abs (pos.x - hx);

            if (dl <= kEdgeGrab || dh <= kEdgeGrab)
                return dl <= dh ? Target::low : Target::high;
        }

        if (pos.x > lx && pos.x < hx)
            return Target::region;

        return inHeader ? Target::region : Target::none;
    }

    //==========================================================================
    // Meters
    void pollCurve()
    {
        const auto type = ps::ModuleType::LowContrast;
        const bool running = processor.isModuleInChain (type) && processor.getParameterValue ("lowc.on") >= 0.5f
                             && processor.getChain().getSampleRate() > 0.0;
        const auto* lc = dynamic_cast<const ps::LowContrast*> (&moduleForChannelSet (processor, type, channelSet));
        const bool valid = running && lc != nullptr;
        bool changed = valid != curveValid;
        curveValid = valid;

        if (valid)
        {
            const int s = juce::jlimit (0, ps::kNumChannelSets - 1, channelSet);
            const auto& m = lc->getMeters();

            for (int i = 0; i < ps::LowContrastMeters::kPoints; ++i)
            {
                float v = m.diffDb[s][i].load();
                v = std::isfinite (v) ? juce::jlimit (-24.0f, 24.0f, v) : 0.0f;

                if (std::abs (v - curve[(size_t) i]) > 0.01f)
                    changed = true;

                curve[(size_t) i] = v;
            }
        }

        if (changed)
            owner.repaint();
    }

    //==========================================================================
    // Painting
    void paintRegion (juce::Graphics& g, const FreqAxis& fx) const
    {
        const auto p = plotArea();
        const float lo = low(), hi = high();
        const float lx = lowX(), hx = highX();
        const bool active = drag == Target::region || (hover == Target::region && drag == Target::none);

        // Tint with the taper: full inside, raised-cosine fade over 1/3 octave outside.
        const float x0 = juce::jmax (p.getX(), fx.hzToX (lo / kThird)), x1 = juce::jmin (p.getRight(), fx.hzToX (hi * kThird));

        for (float x = std::floor (x0); x < x1; x += 2.0f)
        {
            const float w = regionWeight (fx.xToHz (x + 1.0f), lo, hi);

            if (w <= 0.001f)
                continue;

            g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha ((active ? 0.17f : 0.13f) * w), p.getY(),
                                                               colour.withAlpha (0.03f * w), p.getBottom()));
            g.fillRect (juce::Rectangle<float> (x, p.getY(), 2.0f, p.getHeight()));
        }

        g.setColour (colour.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (lx, p.getY(), juce::jmax (0.0f, hx - lx), 2.0f));

        // Where the taper ends (the region's influence stops there)
        g.setColour (colour.withAlpha (0.35f));
        const float dashes[] = { 3.0f, 4.0f };

        for (float hz : { lo / kThird, hi * kThird })
        {
            const float x = fx.hzToX (hz);

            if (x > p.getX() + 1.0f && x < p.getRight() - 1.0f)
                g.drawDashedLine ({ x, p.getY() + 30.0f, x, p.getBottom() }, dashes, 2, 1.0f);
        }
    }

    void paintCurve (juce::Graphics& g, const FreqAxis& fx) const
    {
        if (! curveValid)
            return;

        const auto p = plotArea();
        const float zeroY = diffY (0.0f);
        juce::Path line, area;
        bool any = false;

        // The module's change is exactly 0 beyond the taper: the curve spans the region and its taper only.
        const float lastHz = (float) ps::lowContrastDisplayHz (ps::LowContrastMeters::kPoints - 1);
        const float fromHz = juce::jmax ((float) ps::lowContrastDisplayHz (0), low() / kThird);
        const float toHz = juce::jmin (lastHz, high() * kThird);
        const float x0 = fx.hzToX (fromHz), x1 = fx.hzToX (toHz);
        line.startNewSubPath (x0, zeroY);
        area.startNewSubPath (x0, zeroY);

        for (int i = 0; i < ps::LowContrastMeters::kPoints; ++i)
        {
            const float hz = (float) ps::lowContrastDisplayHz (i);

            if (hz <= fromHz || hz >= toHz)
                continue;

            const float x = fx.hzToX (hz);
            const float y = diffY (curve[(size_t) i]);
            any = any || std::abs (curve[(size_t) i]) > 0.05f;
            line.lineTo (x, y);
            area.lineTo (x, y);
        }

        line.lineTo (x1, zeroY);
        area.lineTo (x1, zeroY);
        area.closeSubPath();

        // Zero line of the difference axis
        const float dashes[] = { 5.0f, 4.0f };
        g.setColour (Colours::good.withAlpha (0.35f));
        g.drawDashedLine ({ p.getX(), zeroY, p.getRight(), zeroY }, dashes, 2, 1.0f);

        if (! any)
            return;

        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (juce::Rectangle<float> (p.getX(), p.getY(), p.getWidth(), zeroY - p.getY()).toNearestInt());
            g.setGradientFill (juce::ColourGradient::vertical (Colours::good.withAlpha (0.45f), diffY (kDiffRange * 0.5f),
                                                               Colours::good.withAlpha (0.08f), zeroY));
            g.fillPath (area);
        }
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (juce::Rectangle<float> (p.getX(), zeroY, p.getWidth(), p.getBottom() - zeroY).toNearestInt());
            g.setGradientFill (juce::ColourGradient::vertical (Colours::minor.withAlpha (0.08f), zeroY,
                                                               Colours::minor.withAlpha (0.45f), diffY (-kDiffRange * 0.5f)));
            g.fillPath (area);
        }

        g.setColour (Colours::background.withAlpha (0.6f));
        g.strokePath (line, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colours::text.withAlpha (0.92f));
        g.strokePath (line, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void paintDiffScale (juce::Graphics& g) const
    {
        const auto p = plotArea();
        g.setFont (font (10.0f, 600));

        for (float db : { 12.0f, 6.0f, 0.0f, -6.0f })
        {
            const float y = diffY (db);
            const auto text = db > 0.0f ? "+" + juce::String (juce::roundToInt (db)) : juce::String (juce::roundToInt (db));
            g.setColour ((db >= 0.0f ? Colours::good : Colours::minor).withAlpha (db == 0.0f ? 0.9f : 0.75f));
            g.drawText (text, juce::Rectangle<float> (p.getRight() - 44.0f, db >= 12.0f ? y + 2.0f : y - 12.0f, 40.0f, 11.0f),
                        juce::Justification::centredRight, false);
        }
    }

    void paintEdges (juce::Graphics& g) const
    {
        const auto p = plotArea();

        for (bool isLow : { true, false })
        {
            const float x = isLow ? lowX() : highX();
            const auto t = isLow ? Target::low : Target::high;
            const bool active = drag == t || (hover == t && drag == Target::none);
            const auto c = active ? colour.brighter (0.25f) : colour;

            if (active)
            {
                g.setColour (c.withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (x - 3.0f, p.getY(), 6.0f, p.getHeight()));
            }

            g.setColour (c.withAlpha (active ? 0.95f : 0.65f));
            g.fillRect (juce::Rectangle<float> (x - (active ? 1.0f : 0.6f), p.getY(), active ? 2.0f : 1.2f, p.getHeight()));
            lowend::drawGrabHandle (g, edgeHandle (isLow), c, active);
        }

        const auto c = chips();
        const bool la = drag == Target::low || (hover == Target::low && drag == Target::none);
        const bool ha = drag == Target::high || (hover == Target::high && drag == Target::none);
        lowend::drawValueChip (g, c.first, lowend::hzText (low()), colour, la || drag == Target::region);
        lowend::drawValueChip (g, c.second, lowend::hzText (high()), colour, ha || drag == Target::region);
    }

    void paintHeader (juce::Graphics& g) const
    {
        const auto r = headerStrip();
        const bool hot = hover == Target::region || drag == Target::region;
        g.setGradientFill (juce::ColourGradient::vertical (blend (Colours::panelHover, colour, hot ? 0.38f : 0.30f), r.getY(),
                                                           blend (Colours::panelRaised, colour, 0.16f), r.getBottom()));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (colour.withAlpha (0.85f));
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);

        const auto s = soloButton();
        const bool soloed = shownSolo;
        const bool sHover = hover == Target::solo;
        g.setColour (soloed ? Colours::minor : (sHover ? Colours::panelHover.brighter (0.1f) : Colours::background.withAlpha (0.45f)));
        g.fillRoundedRectangle (s, 4.0f);
        g.setColour (soloed ? Colours::minor.brighter (0.2f) : Colours::outline.brighter (sHover ? 0.35f : 0.1f));
        g.drawRoundedRectangle (s.reduced (0.5f), 4.0f, 1.0f);
        g.setFont (font (10.0f, 700));
        g.setColour (soloed ? Colours::background : (sHover ? Colours::text : Colours::textDim));
        g.drawText ("S", s, juce::Justification::centred, false);

        auto text = r.reduced (8.0f, 0.0f).withRight (s.getX() - 4.0f);
        const auto nameFont = font (11.5f, 700);
        const auto rangeFont = font (11.0f, 600);
        juce::String name = "Action region";
        juce::String range = lowend::hzText (low()) + " - " + lowend::hzText (high());

        if (textWidth (nameFont, name) + 8.0f + textWidth (rangeFont, range) > text.getWidth())
            name = "Region";

        if (textWidth (nameFont, name) + 8.0f + textWidth (rangeFont, range) > text.getWidth())
            range = juce::String (juce::roundToInt (low())) + "-" + juce::String (juce::roundToInt (high()));

        const float nameW = textWidth (nameFont, name);
        g.setFont (nameFont);
        g.setColour (Colours::text);
        g.drawText (name, text.withWidth (nameW + 2.0f), juce::Justification::centredLeft, false);
        g.setFont (rangeFont);
        g.setColour (Colours::text.withAlpha (0.78f));
        g.drawText (range, text.withTrimmedLeft (nameW + 8.0f), juce::Justification::centredLeft, false);
    }

    void paintLegend (juce::Graphics& g) const
    {
        const auto p = plotArea();

        if (p.getWidth() < 420.0f)
            return;

        auto legend = juce::Rectangle<float> (p.getX() + 8.0f, p.getBottom() - 36.0f, p.getWidth() - 16.0f, 14.0f);
        const auto f = font (10.5f, 600);

        // A soft backdrop keeps the legend readable over the spectra (same as the other spectrum displays).
        const float total = textWidth (f, "Contrast") + textWidth (f, "Output") + textWidth (f, "Input") + 3.0f * 16.0f + 20.0f;
        g.setColour (Colours::background.withAlpha (0.72f));
        g.fillRoundedRectangle (juce::Rectangle<float> (legend.getRight() - total, legend.getY(), total, legend.getHeight())
                                    .expanded (8.0f, 4.0f), 7.0f);

        auto item = [&] (const juce::String& text, juce::Colour c)
        {
            const float w = textWidth (f, text) + 16.0f;
            auto r = legend.removeFromRight (w);
            legend.removeFromRight (10.0f);
            g.setColour (c);
            g.fillRoundedRectangle (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 3.0f), 1.5f);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
        };

        item ("Contrast", Colours::text.withAlpha (0.9f));
        item ("Output", colour);
        item ("Input", Colours::input);
    }

    void paint (juce::Graphics& g)
    {
        const auto b = owner.getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), b.getY(),
                                                           Colours::background.darker (0.05f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);

        const auto header = headerArea();
        g.setColour (juce::Colours::white.withAlpha (0.018f));
        g.fillRect (header.withTrimmedTop (6.0f));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (header.getX(), header.getBottom() - 1.0f, header.getWidth(), 1.0f));

        const auto plot = plotArea();
        const auto fx = axis();

        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addRoundedRectangle (plot.getX(), plot.getY(), plot.getWidth(), plot.getHeight(), 6.0f, 6.0f,
                                      false, false, true, true);
            g.reduceClipRegion (clip);

            paintRegion (g, fx);
            drawFrequencyGrid (g, fx, levelAxis(), true);

            // Extra labels of this short axis (the grid labels only the 1-2-5 decades)
            g.setFont (font (10.0f, 600));
            g.setColour (Colours::textSubtle);

            for (float hz : { 30.0f, 300.0f })
                g.drawText (juce::String (juce::roundToInt (hz)),
                            juce::Rectangle<float> (std::round (fx.hzToX (hz)) + 3.0f, plot.getBottom() - 14.0f, 30.0f, 12.0f),
                            juce::Justification::centredLeft, false);

            const auto& options = services.getOptions();
            auto& pre = services.getModulePreSpectrum();
            auto& post = services.getModulePostSpectrum();
            pre.draw (g, fx, kLevelMin, kLevelMax, Colours::input.withAlpha (0.42f), options.spectrumFill);
            post.draw (g, fx, kLevelMin, kLevelMax, colour.withAlpha (0.9f), false);

            if (! pre.hasData() && ! post.hasData())
            {
                g.setFont (font (12.0f, 600));
                g.setColour (Colours::textSubtle.withAlpha (0.8f));
                g.drawText ("Play audio to see the spectrum", plot.withTrimmedBottom (plot.getHeight() * 0.3f),
                            juce::Justification::centred, false);
            }

            // Region solo: everything outside the region goes dark.
            if (shownSolo)
            {
                g.setColour (Colours::background.withAlpha (0.55f));
                g.fillRect (plot.withRight (lowX()));
                g.fillRect (plot.withLeft (highX()));
            }

            paintCurve (g, fx);
            paintDiffScale (g);
            paintEdges (g);
            paintLegend (g);
        }

        paintHeader (g);

        g.setColour (Colours::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);
    }

    //==========================================================================
    void updateHover (juce::Point<float> pos)
    {
        const auto t = drag != Target::none ? drag : targetAt (pos);

        if (t != hover)
        {
            hover = t;
            owner.repaint();
        }

        switch (t)
        {
            case Target::low:
            case Target::high:   owner.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
            case Target::region: owner.setMouseCursor (juce::MouseCursor::DraggingHandCursor); break;
            case Target::solo:
            case Target::none:
            default:             owner.setMouseCursor (juce::MouseCursor::NormalCursor); break;
        }
    }

    void showEdgeMenu (bool isLow)
    {
        const auto& id = isLow ? kLowId : kHighId;
        stereo::showParamMenu (owner, processor, id, {}, (isLow ? "Low cutoff: " : "High cutoff: ")
                                                          + lowend::hzText (isLow ? low() : high()));
    }

    void showRegionMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Action region: " + lowend::hzText (low()) + " - " + lowend::hzText (high()));
        menu.addItem (1, "Reset region (" + lowend::hzText (defaultOf (kLowId)) + " - " + lowend::hzText (defaultOf (kHighId)) + ")");
        menu.addItem (2, "Solo region", true, getSolo());

        juce::Component::SafePointer<LowRegionDisplay> safe (&owner);
        showMenu (menu, owner, [safe] (int result)
        {
            if (safe == nullptr)
                return;

            if (result == 1)
                safe->impl->resetRegion();
            else if (result == 2)
                safe->impl->setSolo (! safe->impl->getSolo());
        });
    }

    LowRegionDisplay& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Colour colour;
    juce::RangedAudioParameter* lowParam = nullptr;
    juce::RangedAudioParameter* highParam = nullptr;

    std::array<float, ps::LowContrastMeters::kPoints> curve {};
    bool curveValid = false;
    int channelSet = 0;

    Target hover = Target::none, drag = Target::none;
    bool lowOpen = false, highOpen = false;
    float dragX = 0.0f, lastMouseX = 0.0f, startLow = 20.0f, startHigh = 250.0f, startX = 0.0f;
    bool altSolo = false, soloBeforeAlt = false, shownSolo = false;
    float shownLow = -1.0f, shownHigh = -1.0f;
};

//==============================================================================
LowRegionDisplay::LowRegionDisplay (PureSheenProcessor& p, EditorServices& s) : impl (std::make_unique<Impl> (*this, p, s))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTooltip ("Action region: Low Contrast works only between the two edges (and fades out over 1/3 octave beyond "
                "them).\nDrag an edge or the region, double-click to reset, right-click for more. S or Alt-click: hear "
                "only the region.");
    impl->shownSolo = impl->getSolo();
}

LowRegionDisplay::~LowRegionDisplay() = default;

void LowRegionDisplay::tick()
{
    auto& d = *impl;
    d.pollCurve();

    const bool solo = d.getSolo();
    if (solo)
        d.setSolo (true);
    const float lo = d.low(), hi = d.highRaw();

    if (solo != d.shownSolo || std::abs (lo - d.shownLow) > 1.0e-3f || std::abs (hi - d.shownHigh) > 1.0e-3f)
    {
        d.shownSolo = solo;
        d.shownLow = lo;
        d.shownHigh = hi;
        repaint();
    }
}

void LowRegionDisplay::setChannelSet (int set)
{
    if (set != impl->channelSet)
    {
        impl->channelSet = set;
        impl->pollCurve();
        repaint();
    }

    if (impl->getSolo())
        impl->setSolo (true);
}

void LowRegionDisplay::setAccentColour (juce::Colour c)
{
    if (c != impl->colour)
    {
        impl->colour = c;
        repaint();
    }
}

bool LowRegionDisplay::isRegionSoloed() const noexcept { return impl->shownSolo; }
void LowRegionDisplay::setRegionSolo (bool on) { impl->setSolo (on); }
juce::Rectangle<float> LowRegionDisplay::getPlotArea() const noexcept { return impl->plotArea(); }
float LowRegionDisplay::xForFrequency (float hz) const noexcept { return impl->axis().hzToX (hz); }

void LowRegionDisplay::paint (juce::Graphics& g) { impl->paint (g); }

void LowRegionDisplay::mouseMove (const juce::MouseEvent& e) { impl->updateHover (e.position); }

void LowRegionDisplay::mouseExit (const juce::MouseEvent&)
{
    if (impl->drag == Target::none && impl->hover != Target::none)
    {
        impl->hover = Target::none;
        repaint();
    }
}

void LowRegionDisplay::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const auto t = d.targetAt (e.position);

    if (e.mods.isPopupMenu())
    {
        if (t == Target::low || t == Target::high)
            d.showEdgeMenu (t == Target::low);
        else if (t == Target::region || t == Target::solo)
            d.showRegionMenu();

        return;
    }

    if (! isLeftClick (e))
        return;

    // Alt + click: hear only the region while the button is held.
    if (e.mods.isAltDown() && d.plotArea().contains (e.position))
    {
        d.altSolo = true;
        d.soloBeforeAlt = d.getSolo();
        d.setSolo (true);
        return;
    }

    if (t == Target::solo || t == Target::none || e.getNumberOfClicks() >= 2)
        return;

    d.drag = t;
    d.hover = t;
    d.lastMouseX = e.position.x;
    d.startX = e.position.x;
    d.startLow = d.low();
    d.startHigh = d.high();
    d.dragX = t == Target::low ? d.lowX() : (t == Target::high ? d.highX() : e.position.x);
    repaint();
}

void LowRegionDisplay::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (d.drag == Target::none || ! isLeftClick (e))
        return;

    const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
    const float dx = e.position.x - d.lastMouseX;
    d.lastMouseX = e.position.x;

    if (std::abs (dx) < 1.0e-4f)
        return;

    const auto fx = d.axis();
    d.dragX += dx * (fine ? 0.12f : 1.0f);

    if (d.drag == Target::low)
        d.moveLow (fx.xToHz (d.dragX));
    else if (d.drag == Target::high)
        d.moveHigh (fx.xToHz (d.dragX));
    else
        d.moveRegion (fx.xToHz (d.dragX) / fx.xToHz (d.startX));

    // Keep the drag position on the edge when a limit stopped it (no dead zone when turning back).
    if (d.drag == Target::low)
        d.dragX = juce::jlimit (fx.hzToX (kRegionMin), fx.hzToX (kRegionMax / kThird), d.dragX);
    else if (d.drag == Target::high)
        d.dragX = juce::jlimit (fx.hzToX (kRegionMin * kThird), fx.hzToX (kRegionMax), d.dragX);
    else
        d.dragX = d.startX + (fx.hzToX (d.low()) - fx.hzToX (d.startLow));

    repaint();
}

void LowRegionDisplay::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (d.altSolo)
    {
        d.altSolo = false;
        d.setSolo (d.soloBeforeAlt);
        return;
    }

    if (d.drag != Target::none)
    {
        d.endGestures();
        d.drag = Target::none;
        d.updateHover (e.position);
        repaint();
        return;
    }

    if (isLeftClick (e) && d.targetAt (e.position) == Target::solo && d.targetAt (e.getMouseDownPosition().toFloat()) == Target::solo)
        d.setSolo (! d.getSolo());
}

void LowRegionDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e) || e.mods.isAltDown())
        return;

    const auto t = d.targetAt (e.position);
    d.endGestures();
    d.drag = Target::none;

    if (t == Target::low || t == Target::high)
        d.resetEdge (t == Target::low);
    else if (t == Target::region)
        d.resetRegion();
}

} // namespace ps::ui
