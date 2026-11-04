#include "ui/SpectralDisplay.h"

#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"

#include <algorithm>
#include <array>
#include <cmath>

// Spectrum view of the spectral panels (owner: ui-spectral). Contract: SpectralDisplay.h.
//
// Geometry: one dark rounded surface; the top kHeaderH pixels hold the region pill or the zone segments (aligned with
// their x ranges), the rest is the plot. Every x position comes from the user's frequency axis (makeFreqAxis), so
// Log / Mel / Extended Log all work. Parameter edits go through one host gesture per drag (begin on the first
// movement, end on mouse up), so a drag is one undo step; the A/B Link twins move in the same gesture.
namespace ps::ui
{
namespace
{
constexpr float kGrab = 6.0f;              // +- px around a cutoff line
constexpr float kHandleW = 10.0f, kHandleH = 34.0f;
constexpr float kSoloW = 18.0f;
constexpr float kMinPillW = 120.0f;
constexpr float kZoneDragPx = 220.0f;      // drag distance for 0..100 %

juce::String hzShort (float hz)
{
    if (hz >= 999.5f)
    {
        const float k = hz / 1000.0f;
        return juce::String (k, k >= 9.95f ? 0 : 1) + "k";
    }

    return juce::String (juce::roundToInt (hz));
}

const ps::ParamSpec* specFor (const juce::String& id)
{
    return id.isNotEmpty() ? stereo::specOf (id) : nullptr;
}

// The step drawFrequencyGrid uses for a dB range (so the right scale labels sit on its lines).
float gridStep (float range)
{
    float step = 3.0f;

    for (float s : { 3.0f, 6.0f, 10.0f, 12.0f, 20.0f, 24.0f, 30.0f })
    {
        step = s;

        if (range / s <= 8.0f)
            break;
    }

    return step;
}

//==============================================================================
// Up to two parameters (each with an optional linked twin) edited in one host gesture.
class Gesture
{
public:
    explicit Gesture (PureSheenProcessor& p) : processor (p) {}
    ~Gesture() { end(); }

    void start (const juce::String& id0, const juce::String& linked0, const juce::String& id1 = {},
                const juce::String& linked1 = {})
    {
        end();
        const juce::String ids[2] { id0, id1 }, links[2] { linked0, linked1 };

        for (int i = 0; i < 2; ++i)
        {
            slots[(size_t) i] = {};

            if (ids[i].isEmpty())
                continue;

            auto& s = slots[(size_t) i];
            s.param = processor.getParameter (ids[i]);
            s.linked = links[i].isNotEmpty() && links[i] != ids[i] ? processor.getParameter (links[i]) : nullptr;
            s.startValue = s.param != nullptr ? s.param->convertFrom0to1 (s.param->getValue()) : 0.0f;
        }

        armed = slots[0].param != nullptr;
        open = false;
    }

    float startValue (int i) const noexcept { return slots[(size_t) i].startValue; }
    bool isArmed() const noexcept { return armed; }
    bool isOpen() const noexcept { return open; }

    void set (int i, float plain)
    {
        auto& s = slots[(size_t) i];

        if (! armed || s.param == nullptr)
            return;

        if (! open)
        {
            open = true;

            for (auto& t : slots)
            {
                if (t.param != nullptr)
                    t.param->beginChangeGesture();

                if (t.linked != nullptr)
                    t.linked->beginChangeGesture();
            }
        }

        s.param->setValueNotifyingHost (s.param->convertTo0to1 (plain));

        if (s.linked != nullptr)
            s.linked->setValueNotifyingHost (s.linked->convertTo0to1 (plain));
    }

    void end()
    {
        if (open)
            for (auto& t : slots)
            {
                if (t.param != nullptr)
                    t.param->endChangeGesture();

                if (t.linked != nullptr)
                    t.linked->endChangeGesture();
            }

        open = false;
        armed = false;
        slots = {};
    }

private:
    struct Slot
    {
        juce::RangedAudioParameter* param = nullptr;
        juce::RangedAudioParameter* linked = nullptr;
        float startValue = 0.0f;
    };

    PureSheenProcessor& processor;
    std::array<Slot, 2> slots {};
    bool armed = false, open = false;
};

enum class Target { none, lowHandle, highHandle, pill, solo, zone };
} // namespace

//==============================================================================
struct SpectralDisplay::Impl
{
    Impl (SpectralDisplay& o, PureSheenProcessor& p, EditorServices& s, ps::ModuleType t)
        : owner (o), processor (p), services (s), type (t), accent (moduleColour (t)), gesture (p)
    {
    }

    //==============================================================================
    float param (const juce::String& id) const { return id.isNotEmpty() ? processor.getParameterValue (id) : 0.0f; }

    juce::Rectangle<float> inner() const { return owner.getLocalBounds().toFloat().reduced (1.0f); }
    juce::Rectangle<float> headerArea() const { return inner().removeFromTop ((float) kHeaderH); }
    juce::Rectangle<float> plotArea() const { return inner().withTrimmedTop ((float) kHeaderH); }

    FreqAxis axis() const
    {
        return makeFreqAxis (services.getOptions(), plotArea(), processor.getChain().getSampleRate());
    }

    DbAxis leftAxis() const
    {
        DbAxis a;
        a.minDb = leftMin;
        a.maxDb = leftMax;
        a.area = plotArea();
        return a;
    }

    DbAxis rightAxis() const
    {
        if (! hasRight)
            return leftAxis();

        DbAxis a;
        a.minDb = (leftMin - rightZero) * rightScale;
        a.maxDb = (leftMax - rightZero) * rightScale;
        a.area = plotArea();
        return a;
    }

    float xOf (float hz) const
    {
        const auto plot = plotArea();
        return juce::jlimit (plot.getX(), plot.getRight(), axis().hzToX (hz));
    }

    //==============================================================================
    // Region geometry
    bool regionOn() const { return lowId.isNotEmpty() && highId.isNotEmpty(); }
    float lowHz() const { return param (lowId); }
    float highHz() const { return param (highId); }

    juce::Rectangle<float> handle (bool high) const
    {
        const auto plot = plotArea();
        const float x = xOf (high ? highHz() : lowHz());
        // At the plot's edges the grab handle stays fully visible (the line itself is drawn at the true x).
        const float cx = juce::jlimit (plot.getX() + kHandleW * 0.5f + 1.0f, plot.getRight() - kHandleW * 0.5f - 1.0f, x);
        return juce::Rectangle<float> (kHandleW, kHandleH).withCentre ({ cx, plot.getCentreY() });
    }

    juce::Rectangle<float> pill() const
    {
        if (! regionOn())
            return {};

        const auto h = headerArea().reduced (2.0f, 4.0f);
        float x0 = xOf (lowHz()), x1 = xOf (highHz());

        if (x1 - x0 < kMinPillW)
        {
            const float c = (x0 + x1) * 0.5f;
            x0 = c - kMinPillW * 0.5f;
            x1 = c + kMinPillW * 0.5f;
        }

        if (x0 < h.getX())          { x1 += h.getX() - x0; x0 = h.getX(); }
        if (x1 > h.getRight())      { x0 -= x1 - h.getRight(); x1 = h.getRight(); }

        return { juce::jmax (h.getX(), x0), h.getY(), juce::jmin (h.getWidth(), x1 - x0), h.getHeight() };
    }

    juce::Rectangle<float> soloButton() const
    {
        if (! withSolo)
            return {};

        const auto p = pill();

        if (p.getWidth() < 60.0f)
            return {};

        return juce::Rectangle<float> (kSoloW, kSoloW).withCentre ({ p.getRight() - 4.0f - kSoloW * 0.5f, p.getCentreY() });
    }

    juce::Rectangle<float> zoneHeader (int i) const
    {
        if (i < 0 || i >= (int) zones.size())
            return {};

        const auto h = headerArea().reduced (0.0f, 4.0f);
        const float x0 = xOf (zones[(size_t) i].lowHz), x1 = xOf (zones[(size_t) i].highHz);
        const float left = i == 0 ? h.getX() + 2.0f : x0 + 2.0f;
        const float right = i == (int) zones.size() - 1 ? h.getRight() - 2.0f : x1 - 2.0f;
        return { left, h.getY(), juce::jmax (0.0f, right - left), h.getHeight() };
    }

    juce::Rectangle<float> zoneArea (int i) const
    {
        const auto plot = plotArea();
        const auto& z = zones[(size_t) i];
        const float x0 = i == 0 ? plot.getX() : xOf (z.lowHz);
        const float x1 = i == (int) zones.size() - 1 ? plot.getRight() : xOf (z.highHz);
        return { x0, plot.getY(), juce::jmax (0.0f, x1 - x0), plot.getHeight() };
    }

    std::pair<Target, int> hitTest (juce::Point<float> p) const
    {
        if (regionOn())
        {
            if (! soloButton().isEmpty() && soloButton().expanded (2.0f).contains (p))
                return { Target::solo, -1 };

            if (pill().contains (p))
                return { Target::pill, -1 };

            if (plotArea().expanded (2.0f, 0.0f).contains (p))
            {
                const float xl = xOf (lowHz()), xh = xOf (highHz());
                const float dl = std::abs (p.x - xl), dh = std::abs (p.x - xh);
                const bool nearLow = dl <= kGrab || handle (false).expanded (3.0f).contains (p);
                const bool nearHigh = dh <= kGrab || handle (true).expanded (3.0f).contains (p);

                if (nearLow && nearHigh)
                    return { (p.x > xh || (dh < dl)) ? Target::highHandle : Target::lowHandle, -1 };

                if (nearLow)
                    return { Target::lowHandle, -1 };

                if (nearHigh)
                    return { Target::highHandle, -1 };
            }
        }

        for (int i = 0; i < (int) zones.size(); ++i)
            if (zoneHeader (i).contains (p))
                return { Target::zone, i };

        return { Target::none, -1 };
    }

    //==============================================================================
    // Painting
    void paintZones (juce::Graphics& g) const
    {
        const auto plot = plotArea();

        for (int i = 0; i < (int) zones.size(); ++i)
        {
            const auto& z = zones[(size_t) i];
            const float v = juce::jlimit (0.0f, 100.0f, param (z.paramId));

            if (v < 99.5f)
            {
                g.setColour (Colours::background.withAlpha (0.42f * (1.0f - v / 100.0f)));
                g.fillRect (zoneArea (i));
            }

            if (i > 0)
            {
                const float x = std::round (xOf (z.lowHz));
                juce::Path line;
                line.startNewSubPath (x + 0.5f, plot.getY());
                line.lineTo (x + 0.5f, plot.getBottom());
                juce::Path dashed;
                const float dashes[] { 4.0f, 4.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
                g.setColour (Colours::textFaint.withAlpha (0.8f));
                g.fillPath (dashed);
            }
        }
    }

    void paintZoneHeaders (juce::Graphics& g) const
    {
        for (int i = 0; i < (int) zones.size(); ++i)
        {
            const auto r = zoneHeader (i);

            if (r.getWidth() < 24.0f)
                continue;

            const auto& z = zones[(size_t) i];
            const float v = juce::jlimit (0.0f, 100.0f, param (z.paramId));
            const bool hover = hoverTarget == Target::zone && hoverIndex == i;
            const bool active = dragTarget == Target::zone && dragIndex == i;
            const bool lit = hover || active;

            g.setColour ((lit ? Colours::panelHover : Colours::panelRaised).withAlpha (lit ? 0.95f : 0.62f));
            g.fillRoundedRectangle (r, 5.0f);

            // Strength: a fill from the left, the value's share of the segment.
            const auto fill = r.reduced (1.0f).withWidth (juce::jmax (0.0f, (r.getWidth() - 2.0f) * v / 100.0f));

            if (fill.getWidth() > 1.0f)
            {
                g.setGradientFill (juce::ColourGradient::horizontal (accent.withAlpha (lit ? 0.30f : 0.20f), fill.getX(),
                                                                     accent.withAlpha (lit ? 0.18f : 0.10f), fill.getRight()));
                g.fillRoundedRectangle (fill, 4.0f);

                // The edge marker, unless it would cut through the value text at the right end of the segment.
                const float valueLeft = r.getRight() - 8.0f - textWidth (font (11.0f, 600), juce::String (juce::roundToInt (v)) + " %") - 4.0f;
                if (fill.getRight() < valueLeft || fill.getRight() > r.getRight() - 4.0f)
                {
                    g.setColour (accent.withAlpha (lit ? 1.0f : 0.75f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (fill.getRight() - 2.0f, r.getY() + 3.0f, 2.0f, r.getHeight() - 6.0f), 1.0f);
                }
            }

            g.setColour ((lit ? accent : Colours::outline).withAlpha (lit ? 0.9f : 0.7f));
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);

            auto text = r.reduced (8.0f, 0.0f);
            const auto nameFont = font (11.5f, 700);
            const auto valueFont = font (11.0f, 600);
            const auto valueText = juce::String (juce::roundToInt (v)) + " %";
            const float valueW = textWidth (valueFont, valueText);
            const juce::String range = i == 0 ? "< " + hzShort (z.highHz)
                                              : (i == (int) zones.size() - 1 ? "> " + hzShort (z.lowHz)
                                                                              : hzShort (z.lowHz) + "-" + hzShort (z.highHz));
            const auto rangeFont = font (10.5f, 600);

            g.setFont (valueFont);
            g.setColour (lit ? Colours::text : Colours::text.withAlpha (0.85f));

            if (text.getWidth() > valueW + 10.0f)
            {
                g.drawText (valueText, text.removeFromRight (valueW + 2.0f), juce::Justification::centredRight, false);
                text.removeFromRight (6.0f);
            }

            const float nameW = textWidth (nameFont, z.name);

            if (text.getWidth() >= nameW)
            {
                g.setFont (nameFont);
                g.setColour (lit ? Colours::text : Colours::textDim);
                g.drawText (z.name, text.removeFromLeft (nameW + 2.0f), juce::Justification::centredLeft, false);
                text.removeFromLeft (6.0f);

                if (text.getWidth() >= textWidth (rangeFont, range))
                {
                    g.setFont (rangeFont);
                    g.setColour (Colours::textSubtle);
                    g.drawText (range, text, juce::Justification::centredLeft, false);
                }
            }
        }
    }

    void paintVeil (juce::Graphics& g) const
    {
        if (veilHz <= 0.0f)
            return;

        const auto plot = plotArea();
        const float x = xOf (veilHz);

        if (x <= plot.getX() + 1.0f)
            return;

        const auto r = plot.withRight (x);
        g.setColour (Colours::background.withAlpha (0.62f));
        g.fillRect (r);

        // Fine diagonal hatch.
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (r.toNearestInt());
            g.setColour (juce::Colours::white.withAlpha (0.035f));

            for (float k = r.getX() - r.getHeight(); k < r.getRight(); k += 9.0f)
                g.drawLine (k, r.getBottom(), k + r.getHeight(), r.getY(), 1.0f);
        }

        g.setColour (Colours::outline.withAlpha (0.9f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getY(), 1.0f, plot.getHeight()));

        if (veilCaption.isNotEmpty() && r.getWidth() > 44.0f)
        {
            g.setFont (font (10.5f, 600));
            g.setColour (Colours::textSubtle);
            g.drawFittedText (veilCaption, r.reduced (6.0f, 0.0f).withTrimmedTop (10.0f).withHeight (30.0f).toNearestInt(),
                              juce::Justification::centredTop, 2, 0.85f);
        }
    }

    void paintRegionShade (juce::Graphics& g) const
    {
        if (! regionOn())
            return;

        const auto plot = plotArea();
        const float xl = xOf (lowHz()), xh = xOf (highHz());
        const bool solo = shownSolo;
        g.setColour (Colours::background.withAlpha (solo ? 0.66f : 0.48f));

        if (xl > plot.getX())
            g.fillRect (plot.withRight (xl));

        if (xh < plot.getRight())
            g.fillRect (plot.withLeft (xh));

        // A faint tint inside the region.
        g.setGradientFill (juce::ColourGradient::vertical (accent.withAlpha (solo ? 0.12f : 0.07f), plot.getY(),
                                                           accent.withAlpha (0.015f), plot.getBottom()));
        g.fillRect (juce::Rectangle<float> (xl, plot.getY(), juce::jmax (0.0f, xh - xl), plot.getHeight()));
        g.setColour (accent.withAlpha (0.6f));
        g.fillRect (juce::Rectangle<float> (xl, plot.getY(), juce::jmax (0.0f, xh - xl), 2.0f));
    }

    void paintHandles (juce::Graphics& g) const
    {
        if (! regionOn())
            return;

        const auto plot = plotArea();

        for (bool high : { false, true })
        {
            const auto me = high ? Target::highHandle : Target::lowHandle;
            const bool active = dragTarget == me || (hoverTarget == me && dragTarget == Target::none)
                                || dragTarget == Target::pill;
            const float hz = high ? highHz() : lowHz();
            const float x = xOf (hz);
            const auto c = active ? accent.brighter (0.25f) : accent;

            if (active)
            {
                g.setColour (c.withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (x - 3.0f, plot.getY(), 6.0f, plot.getHeight()));
            }

            g.setColour (c.withAlpha (active ? 0.95f : 0.65f));
            g.fillRect (juce::Rectangle<float> (x - (active ? 1.0f : 0.6f), plot.getY(), active ? 2.0f : 1.2f, plot.getHeight()));

            const auto h = handle (high);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (h.translated (0.0f, 1.5f), 4.0f);
            g.setGradientFill (juce::ColourGradient::vertical (active ? c : blend (Colours::panelHover, c, 0.35f), h.getY(),
                                                               active ? c.darker (0.25f) : blend (Colours::panelRaised, c, 0.2f), h.getBottom()));
            g.fillRoundedRectangle (h, 4.0f);
            g.setColour (active ? c.brighter (0.3f) : c.withAlpha (0.9f));
            g.drawRoundedRectangle (h.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (active ? Colours::background.withAlpha (0.75f) : Colours::text.withAlpha (0.55f));

            for (int k = -1; k <= 1; ++k)
                g.fillRect (juce::Rectangle<float> (h.getCentreX() - 2.5f, h.getCentreY() + (float) k * 4.0f - 0.5f, 5.0f, 1.0f));

            if (active)
            {
                const auto text = fmt::hz (hz);
                const auto f = font (11.5f, 700);
                const float w = textWidth (f, text) + 14.0f;
                auto bubble = juce::Rectangle<float> (w, 20.0f).withY (h.getY() - 26.0f);
                const bool right = high ? (x + 10.0f + w <= plot.getRight() - 2.0f) : (x - 10.0f - w < plot.getX() + 2.0f);
                bubble.setX (right ? x + 8.0f : x - 8.0f - w);
                g.setColour (Colours::background.withAlpha (0.92f));
                g.fillRoundedRectangle (bubble, 5.0f);
                g.setColour (c.withAlpha (0.9f));
                g.drawRoundedRectangle (bubble.reduced (0.5f), 5.0f, 1.0f);
                g.setFont (f);
                g.setColour (Colours::text);
                g.drawText (text, bubble, juce::Justification::centred, false);
            }
        }
    }

    void paintPill (juce::Graphics& g) const
    {
        const auto p = pill();

        if (p.isEmpty())
            return;

        const bool hover = hoverTarget == Target::pill || dragTarget == Target::pill;
        const bool solo = shownSolo;

        g.setGradientFill (juce::ColourGradient::vertical (blend (Colours::panelHover, accent, hover ? 0.36f : 0.26f), p.getY(),
                                                           blend (Colours::panelRaised, accent, hover ? 0.2f : 0.13f), p.getBottom()));
        g.fillRoundedRectangle (p, 5.0f);
        g.setColour (accent.withAlpha (hover ? 1.0f : 0.8f));
        g.drawRoundedRectangle (p.reduced (0.5f), 5.0f, 1.0f);

        auto text = p.reduced (8.0f, 0.0f);
        const auto s = soloButton();

        if (! s.isEmpty())
        {
            text.setRight (s.getX() - 4.0f);
            const bool sHover = hoverTarget == Target::solo;
            g.setColour (solo ? Colours::minor : (sHover ? Colours::panelHover.brighter (0.1f) : Colours::background.withAlpha (0.45f)));
            g.fillRoundedRectangle (s, 4.0f);
            g.setColour (solo ? Colours::minor.brighter (0.2f) : Colours::outline.brighter (sHover ? 0.35f : 0.1f));
            g.drawRoundedRectangle (s.reduced (0.5f), 4.0f, 1.0f);
            g.setFont (font (10.0f, 700));
            g.setColour (solo ? Colours::background : (sHover ? Colours::text : Colours::textDim));
            g.drawText ("S", s, juce::Justification::centred, false);
        }

        const auto nameFont = font (11.5f, 700);
        const auto rangeFont = font (11.0f, 600);
        const juce::String name = solo ? "Region solo" : "Region";
        const juce::String longRange = fmt::hz (lowHz()) + " - " + fmt::hz (highHz());
        const juce::String shortRange = hzShort (lowHz()) + " - " + hzShort (highHz());
        const float w = text.getWidth();

        juce::String showName = name, range = longRange;

        if (textWidth (nameFont, showName) + 8.0f + textWidth (rangeFont, range) > w)
            range = shortRange;

        if (textWidth (nameFont, showName) + 8.0f + textWidth (rangeFont, range) > w)
            showName.clear();

        if (textWidth (rangeFont, range) > w)
            range = shortRange;

        float x = text.getX();

        if (showName.isNotEmpty())
        {
            g.setFont (nameFont);
            g.setColour (solo ? Colours::minor : Colours::text);
            const float nw = textWidth (nameFont, showName);
            g.drawText (showName, text.withWidth (nw + 2.0f), juce::Justification::centredLeft, false);
            x += nw + 8.0f;
        }

        g.setFont (rangeFont);
        g.setColour (Colours::text.withAlpha (0.8f));
        g.drawText (range, text.withLeft (x), juce::Justification::centredLeft, false);
    }

    void paintRightScale (juce::Graphics& g) const
    {
        if (! hasRight)
            return;

        const auto plot = plotArea();
        const auto left = leftAxis();
        const float step = gridStep (leftMax - leftMin);
        const float first = std::ceil (leftMin / step) * step;
        const auto captionFont = font (10.0f, 700).withExtraKerningFactor (0.06f);
        const bool showCaption = rightCaption.isNotEmpty() && plot.getWidth() > 360.0f;
        const float captionW = showCaption ? textWidth (captionFont, rightCaption.toUpperCase()) + 4.0f : 0.0f;
        const auto captionArea = juce::Rectangle<float> (plot.getRight() - 6.0f - captionW, plot.getY() + 4.0f, captionW, 12.0f);
        g.setFont (font (10.0f, 600));

        for (float db = first; db <= leftMax + 0.01f; db += step)
        {
            const float y = std::round (left.dbToY (db));

            if (y < plot.getY() + 1.0f || y > plot.getBottom() - 1.0f)
                continue;

            const float r = (db - rightZero) * rightScale;
            const bool zero = std::abs (r) < 0.01f;
            const juce::String text = zero ? juce::String ("0") : (r > 0.0f ? "+" : "") + juce::String (r, std::abs (r - std::round (r)) < 0.05f ? 0 : 1);
            // (Left of a cutoff handle parked at the right edge; never under the caption.)
            const float inset = regionOn() ? 18.0f : 4.0f;
            const auto labelArea = juce::Rectangle<float> (plot.getRight() - inset - 30.0f, y - 13.0f, 30.0f, 12.0f);

            if (showCaption && labelArea.intersects (captionArea.expanded (0.0f, 2.0f)))
                continue;

            g.setColour (accent.withAlpha (zero ? 0.95f : 0.7f));
            g.drawText (text, labelArea, juce::Justification::centredRight, false);
        }

        // The correction's zero line.
        const float y0 = std::round (left.dbToY (rightZero));

        if (y0 > plot.getY() + 1.0f && y0 < plot.getBottom() - 1.0f)
        {
            juce::Path line;
            line.startNewSubPath (plot.getX(), y0 + 0.5f);
            line.lineTo (plot.getRight(), y0 + 0.5f);
            juce::Path dashed;
            const float dashes[] { 6.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
            g.setColour (accent.withAlpha (0.32f));
            g.fillPath (dashed);
        }

        if (showCaption)
        {
            g.setFont (captionFont);
            g.setColour (accent.withAlpha (0.75f));
            g.drawText (rightCaption.toUpperCase(), captionArea, juce::Justification::centredRight, false);
        }
    }

    void paintLegend (juce::Graphics& g) const
    {
        if (legend.empty())
            return;

        const auto plot = plotArea();
        const auto f = font (10.5f, 600);
        const float available = plot.getWidth() - 56.0f - (hasRight ? 30.0f : 0.0f);
        float total = 0.0f;
        int shown = 0;

        for (auto it = legend.rbegin(); it != legend.rend(); ++it)
        {
            const float w = textWidth (f, it->first) + 16.0f + (shown > 0 ? 10.0f : 0.0f);

            if (total + w > available)
                break;

            total += w;
            ++shown;
        }

        if (shown == 0)
            return;

        // A soft backdrop keeps the legend readable over the curves.
        auto row = juce::Rectangle<float> (total, 14.0f).withPosition (plot.getRight() - 8.0f - (hasRight ? 30.0f : 0.0f) - total,
                                                                       plot.getBottom() - 34.0f);
        g.setColour (Colours::background.withAlpha (0.72f));
        g.fillRoundedRectangle (row.expanded (8.0f, 4.0f), 7.0f);

        int index = 0;

        for (auto it = legend.rbegin(); it != legend.rend() && index < shown; ++it, ++index)
        {
            const float w = textWidth (f, it->first) + 16.0f;
            auto r = row.removeFromRight (w);
            row.removeFromRight (10.0f);
            g.setColour (it->second);
            g.fillRoundedRectangle (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 3.0f), 1.5f);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (it->first, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
        }
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

            drawFrequencyGrid (g, fx, leftAxis(), true);

            if (showPre || showPost)
            {
                const auto& options = services.getOptions();
                auto& pre = services.getModulePreSpectrum();
                auto& post = services.getModulePostSpectrum();

                // (The analyser draws its line at a fixed opacity: a transparency layer makes a faint backdrop.)
                auto drawFaded = [&] (SpectrumAnalyser& a, juce::Colour c, float opacity, bool fill)
                {
                    if (opacity >= 0.99f)
                    {
                        a.draw (g, fx, specMin, specMax, c, fill);
                        return;
                    }

                    g.beginTransparencyLayer (opacity);
                    a.draw (g, fx, specMin, specMax, c, fill);
                    g.endTransparencyLayer();
                };

                if (showPre)
                    drawFaded (pre, Colours::input, preOpacity, options.spectrumFill);

                if (showPost)
                    drawFaded (post, accent, postOpacity, postFill && options.spectrumFill);

                services.drawReferenceSpectrum (g, fx, specMin, specMax);   // (Track Referencing)
            }

            paintZones (g);
            paintVeil (g);
            paintRegionShade (g);

            if (owner.paintOverlay != nullptr)
                owner.paintOverlay (g);

            paintRightScale (g);
            paintHandles (g);
            paintLegend (g);

            if (status.isNotEmpty())
            {
                const auto f = font (12.0f, 600);
                const float w = textWidth (f, status) + 28.0f;
                const auto box = juce::Rectangle<float> (juce::jmin (w, plot.getWidth() - 20.0f), 26.0f)
                                     .withCentre (plot.getCentre().translated (0.0f, -plot.getHeight() * 0.12f));
                g.setColour (Colours::background.withAlpha (0.82f));
                g.fillRoundedRectangle (box, 13.0f);
                g.setColour (Colours::outline.withAlpha (0.8f));
                g.drawRoundedRectangle (box.reduced (0.5f), 13.0f, 1.0f);
                g.setFont (f);
                g.setColour (Colours::textDim);
                g.drawFittedText (status, box.toNearestInt().reduced (8, 0), juce::Justification::centred, 1, 0.8f);
            }
        }

        paintZoneHeaders (g);
        paintPill (g);

        if (headerText.isNotEmpty() && zones.empty())
        {
            const auto f = font (11.0f, 600);
            const auto p = pill();
            auto area = header.reduced (10.0f, 0.0f);
            const float w = textWidth (f, headerText) + 4.0f;

            // Left of the pill if it fits there, else right of it.
            juce::Rectangle<float> r;

            if (p.isEmpty() || p.getX() - area.getX() >= w + 12.0f)
                r = area.withWidth (w);
            else if (area.getRight() - p.getRight() >= w + 12.0f)
                r = area.withLeft (area.getRight() - w);

            if (! r.isEmpty())
            {
                g.setFont (f);
                g.setColour (Colours::textSubtle);
                g.drawText (headerText, r, juce::Justification::centredLeft, false);
            }
        }

        g.setColour (Colours::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);
    }

    //==============================================================================
    // Edits
    void setSolo (bool on)
    {
        auto m = processor.getMonitor (type);
        const int part = on && currentChannelMode (processor, type) == ps::ChannelMode::TransientSustain
                             ? channelSet : -1;

        if (m.soloRegion != on || m.soloChannelSet != part)
        {
            m.soloRegion = on;
            m.soloChannelSet = part;
            processor.setMonitor (type, m);
        }

        shownSolo = on;
        owner.repaint();
    }

    juce::String regionName() const
    {
        return ps::moduleInfo (type).shortName + juce::String (" region");
    }

    void resetRegion()
    {
        const auto* lo = specFor (lowId);
        const auto* hi = specFor (highId);

        if (lo == nullptr || hi == nullptr)
            return;

        std::vector<std::pair<juce::String, float>> changes { { lowId, lo->defaultValue }, { highId, hi->defaultValue } };

        if (linkedLowId.isNotEmpty())
            changes.push_back ({ linkedLowId, lo->defaultValue });

        if (linkedHighId.isNotEmpty())
            changes.push_back ({ linkedHighId, hi->defaultValue });

        processor.setParameterValues (changes, regionName() + ": default");

        if (owner.onRegionChanged != nullptr)
            owner.onRegionChanged();
    }

    void dragTo (const juce::MouseEvent& e)
    {
        if (! gesture.isArmed())
            return;

        const auto fx = axis();

        if (dragTarget == Target::lowHandle || dragTarget == Target::highHandle)
        {
            const bool high = dragTarget == Target::highHandle;
            const auto* spec = specFor (high ? highId : lowId);

            if (spec == nullptr)
                return;

            float hz = fx.xToHz (e.position.x);

            if (high)
                hz = juce::jmax (hz, lowHz() * minRatio);
            else
                hz = juce::jmin (hz, highHz() / minRatio);

            gesture.set (0, juce::jlimit (spec->minValue, spec->maxValue, hz));
        }
        else if (dragTarget == Target::pill)
        {
            const auto* lo = specFor (lowId);
            const auto* hi = specFor (highId);

            if (lo == nullptr || hi == nullptr)
                return;

            const float startLo = gesture.startValue (0), startHi = gesture.startValue (1);
            const float from = fx.xToHz ((float) e.getMouseDownX()), to = fx.xToHz (e.position.x);
            float ratio = to / juce::jmax (1.0f, from);
            ratio = juce::jlimit (lo->minValue / startLo, hi->maxValue / startHi, ratio);
            gesture.set (0, juce::jlimit (lo->minValue, lo->maxValue, startLo * ratio));
            gesture.set (1, juce::jlimit (hi->minValue, hi->maxValue, startHi * ratio));
        }
        else if (dragTarget == Target::zone)
        {
            const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();
            const float delta = ((float) e.getDistanceFromDragStartX() - (float) e.getDistanceFromDragStartY()) / kZoneDragPx * 100.0f;
            gesture.set (0, juce::jlimit (0.0f, 100.0f, gesture.startValue (0) + delta * (fine ? 0.2f : 1.0f)));
        }

        if (owner.onRegionChanged != nullptr && dragTarget != Target::zone)
            owner.onRegionChanged();

        owner.repaint();
    }

    // A typed cutoff keeps the region at least minRatio wide (the handles' own limit).
    std::function<float (float)> lowConstraint()  { return [this] (float hz) { return juce::jmin (hz, highHz() / minRatio); }; }
    std::function<float (float)> highConstraint() { return [this] (float hz) { return juce::jmax (hz, lowHz() * minRatio); }; }

    void showMenuFor (Target t, int index)
    {
        if (t == Target::lowHandle)
            stereo::showParamMenu (owner, processor, lowId, linkedLowId, regionName() + " low cutoff", lowConstraint());
        else if (t == Target::highHandle)
            stereo::showParamMenu (owner, processor, highId, linkedHighId, regionName() + " high cutoff", highConstraint());
        else if (t == Target::zone && index >= 0 && index < (int) zones.size())
            stereo::showParamMenu (owner, processor, zones[(size_t) index].paramId, zones[(size_t) index].linkedId,
                                   zones[(size_t) index].name + " amount");
        else if (t == Target::pill)
        {
            juce::PopupMenu menu;
            menu.addSectionHeader (regionName());
            menu.addItem (1, "Reset region");

            if (withSolo)
                menu.addItem (2, "Solo the region", true, shownSolo);

            juce::Component::SafePointer<SpectralDisplay> safe (&owner);
            showMenu (menu, owner, [this, safe] (int result)
            {
                if (safe == nullptr)
                    return;

                if (result == 1)
                    resetRegion();
                else if (result == 2)
                    setSolo (! shownSolo);
            });
        }
    }

    void updateHover (juce::Point<float> p)
    {
        const auto hit = hitTest (p);

        if (hit.first != hoverTarget || hit.second != hoverIndex)
        {
            hoverTarget = hit.first;
            hoverIndex = hit.second;
            owner.repaint();
        }

        juce::String tip;

        switch (hoverTarget)
        {
            case Target::lowHandle:
            case Target::highHandle: tip = "Drag to set where the action region starts / ends. Double-click to type a value, "
                                           "right-click for a menu."; break;
            case Target::pill:       tip = withSolo ? "Drag to move the whole region. Alt + click: solo the region while "
                                                      "the mouse is held." : "Drag to move the whole region."; break;
            case Target::solo:       tip = "Solo the region: hear only the input inside the action region (before "
                                           "processing)."; break;
            case Target::zone:       tip = hoverIndex >= 0 && hoverIndex < (int) zones.size() && zones[(size_t) hoverIndex].tooltip.isNotEmpty()
                                               ? zones[(size_t) hoverIndex].tooltip
                                               : juce::String ("Drag to set how much this region is corrected. Double-click to type a value, right-click to reset.");
                                     break;
            case Target::none:
            default:                 tip = baseTooltip; break;
        }

        if (tip != owner.getTooltip())
            owner.setTooltip (tip);
    }

    //==============================================================================
    SpectralDisplay& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    ps::ModuleType type;
    juce::Colour accent;

    float leftMin = -84.0f, leftMax = 0.0f;
    juce::String leftCaption;
    bool hasRight = false;
    float rightZero = 0.0f, rightScale = 1.0f;
    juce::String rightCaption;
    bool showPre = true, showPost = true, specSet = false, postFill = false;
    float preOpacity = 0.55f, postOpacity = 0.9f;
    float specMin = -84.0f, specMax = 0.0f;
    float veilHz = 0.0f;
    juce::String veilCaption;
    std::vector<std::pair<juce::String, juce::Colour>> legend;
    juce::String status, headerText, baseTooltip;

    juce::String lowId, highId, linkedLowId, linkedHighId;
    float minRatio = 1.12246204831f;
    bool withSolo = false;
    std::vector<Zone> zones;

    Gesture gesture;
    Target hoverTarget = Target::none, dragTarget = Target::none;
    int hoverIndex = -1, dragIndex = -1;
    bool shownSolo = false, altSolo = false, soloBeforeAlt = false;
    int channelSet = 0;
    std::vector<float> shownValues;
};

//==============================================================================
SpectralDisplay::SpectralDisplay (PureSheenProcessor& p, EditorServices& s, ps::ModuleType t)
    : impl (std::make_unique<Impl> (*this, p, s, t))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (false);
}

SpectralDisplay::~SpectralDisplay()
{
    impl->gesture.end();

    // Region solo belongs to the open panel: leaving it (another module, editor closed) ends the solo.
    if (impl->withSolo && (impl->altSolo || impl->processor.getMonitor (impl->type).soloRegion))
    {
        auto m = impl->processor.getMonitor (impl->type);
        m.soloRegion = false;
        m.soloChannelSet = -1;
        impl->processor.setMonitor (impl->type, m);
    }
}

void SpectralDisplay::tick()
{
    auto& d = *impl;
    bool changed = false;

    if (d.withSolo)
    {
        const bool solo = d.processor.getMonitor (d.type).soloRegion;
        if (solo)
            d.setSolo (true);   // channel-mode automation can also change the monitored part
        changed = solo != d.shownSolo;
        d.shownSolo = solo;
    }

    // Parameter values the display shows (region, zones): repaint when one moved (automation, undo, the knobs).
    std::vector<float> now;
    now.reserve (2 + d.zones.size());

    if (d.regionOn())
    {
        now.push_back (d.lowHz());
        now.push_back (d.highHz());
    }

    for (const auto& z : d.zones)
        now.push_back (d.param (z.paramId));

    if (now != d.shownValues)
    {
        d.shownValues = std::move (now);
        changed = true;
    }

    if (changed)
        repaint();
}

void SpectralDisplay::setChannelSet (int set)
{
    impl->channelSet = set == 1 ? 1 : 0;
    if (impl->withSolo && impl->processor.getMonitor (impl->type).soloRegion)
        impl->setSolo (true);
}

void SpectralDisplay::setAccentColour (juce::Colour c)
{
    if (c != impl->accent)
    {
        impl->accent = c;
        repaint();
    }
}

juce::Colour SpectralDisplay::getAccentColour() const noexcept { return impl->accent; }

void SpectralDisplay::setLeftScale (float minDb, float maxDb, const juce::String& caption)
{
    impl->leftMin = minDb;
    impl->leftMax = juce::jmax (minDb + 1.0f, maxDb);
    impl->leftCaption = caption;

    if (! impl->specSet)
    {
        impl->specMin = impl->leftMin;
        impl->specMax = impl->leftMax;
    }

    repaint();
}

void SpectralDisplay::setRightScale (float leftDbAtZero, float rightPerLeftDb, const juce::String& caption)
{
    impl->hasRight = true;
    impl->rightZero = leftDbAtZero;
    impl->rightScale = rightPerLeftDb;
    impl->rightCaption = caption;
    repaint();
}

void SpectralDisplay::clearRightScale()
{
    impl->hasRight = false;
    repaint();
}

void SpectralDisplay::setSpectra (bool showInput, bool showOutput)
{
    impl->showPre = showInput;
    impl->showPost = showOutput;
    repaint();
}

void SpectralDisplay::setSpectrumStyle (float inputOpacity, float outputOpacity, bool outputFill)
{
    impl->preOpacity = juce::jlimit (0.0f, 1.0f, inputOpacity);
    impl->postOpacity = juce::jlimit (0.0f, 1.0f, outputOpacity);
    impl->postFill = outputFill;
    repaint();
}

void SpectralDisplay::setSpectrumRange (float minDb, float maxDb)
{
    impl->specSet = true;
    impl->specMin = minDb;
    impl->specMax = juce::jmax (minDb + 1.0f, maxDb);
}

void SpectralDisplay::setVeilBelow (float hz, const juce::String& caption)
{
    impl->veilHz = hz;
    impl->veilCaption = caption;
    repaint();
}

void SpectralDisplay::setLegend (std::vector<std::pair<juce::String, juce::Colour>> items)
{
    impl->legend = std::move (items);
    repaint();
}

void SpectralDisplay::setStatusText (const juce::String& text)
{
    if (text != impl->status)
    {
        impl->status = text;
        repaint();
    }
}

void SpectralDisplay::setBaseTooltip (const juce::String& text)
{
    impl->baseTooltip = text;

    if (impl->hoverTarget == Target::none)
        setTooltip (text);
}

void SpectralDisplay::setHeaderText (const juce::String& text)
{
    if (text != impl->headerText)
    {
        impl->headerText = text;
        repaint();
    }
}

void SpectralDisplay::setRegion (const juce::String& lowId, const juce::String& highId, const juce::String& linkedLowId,
                                 const juce::String& linkedHighId, float minRatio, bool withSolo)
{
    auto& d = *impl;

    if (d.lowId == lowId && d.highId == highId && d.linkedLowId == linkedLowId && d.linkedHighId == linkedHighId
        && d.withSolo == withSolo && d.minRatio == minRatio)
        return;

    if (d.dragTarget != Target::none)
    {
        d.gesture.end();
        d.dragTarget = Target::none;
    }

    d.lowId = lowId;
    d.highId = highId;
    d.linkedLowId = linkedLowId;
    d.linkedHighId = linkedHighId;
    d.minRatio = juce::jmax (1.0f, minRatio);
    d.withSolo = withSolo;
    d.shownSolo = withSolo && d.processor.getMonitor (d.type).soloRegion;
    repaint();
}

void SpectralDisplay::clearRegion() { setRegion ({}, {}, {}, {}, impl->minRatio, false); }
bool SpectralDisplay::hasRegion() const noexcept { return impl->regionOn(); }
float SpectralDisplay::getRegionLowHz() const { return impl->lowHz(); }
float SpectralDisplay::getRegionHighHz() const { return impl->highHz(); }
bool SpectralDisplay::isRegionSoloed() const { return impl->shownSolo; }

void SpectralDisplay::setZones (std::vector<Zone> z)
{
    auto& d = *impl;

    // Same parameters (a refresh while dragging): keep the gesture running.
    bool same = z.size() == d.zones.size();

    for (size_t i = 0; same && i < z.size(); ++i)
        same = z[i].paramId == d.zones[i].paramId && z[i].linkedId == d.zones[i].linkedId
               && z[i].lowHz == d.zones[i].lowHz && z[i].highHz == d.zones[i].highHz;

    if (! same && d.dragTarget == Target::zone)
    {
        d.gesture.end();
        d.dragTarget = Target::none;
    }

    d.zones = std::move (z);
    repaint();
}

juce::Rectangle<float> SpectralDisplay::getPlotArea() const noexcept { return impl->plotArea(); }
FreqAxis SpectralDisplay::getFreqAxis() const { return impl->axis(); }
DbAxis SpectralDisplay::getLeftAxis() const { return impl->leftAxis(); }
DbAxis SpectralDisplay::getRightAxis() const { return impl->rightAxis(); }
float SpectralDisplay::xForHz (float hz) const { return impl->axis().hzToX (hz); }
juce::Rectangle<float> SpectralDisplay::getHandleArea (bool high) const { return impl->regionOn() ? impl->handle (high) : juce::Rectangle<float>(); }
juce::Rectangle<float> SpectralDisplay::getRegionPillArea() const { return impl->pill(); }
juce::Rectangle<float> SpectralDisplay::getSoloButtonArea() const { return impl->regionOn() ? impl->soloButton() : juce::Rectangle<float>(); }
juce::Rectangle<float> SpectralDisplay::getZoneHeaderArea (int zone) const { return impl->zoneHeader (zone); }

namespace
{
// Appends a smooth curve through pts (Catmull-Rom -> cubic Bezier, control points clamped in y between their ends so
// the curve never overshoots a flat run). `connect`: lineTo the first point instead of starting a new sub-path.
void appendSmooth (juce::Path& path, const std::vector<juce::Point<float>>& pts, bool connect)
{
    if (pts.empty())
        return;

    if (connect)
        path.lineTo (pts.front());
    else
        path.startNewSubPath (pts.front());

    const size_t n = pts.size();

    for (size_t i = 0; i + 1 < n; ++i)
    {
        const auto p0 = pts[i > 0 ? i - 1 : i], p1 = pts[i], p2 = pts[i + 1], p3 = pts[i + 2 < n ? i + 2 : i + 1];
        auto c1 = p1 + (p2 - p0) / 6.0f;
        auto c2 = p2 - (p3 - p1) / 6.0f;
        const float lo = juce::jmin (p1.y, p2.y), hi = juce::jmax (p1.y, p2.y);
        c1.y = juce::jlimit (lo, hi, c1.y);
        c2.y = juce::jlimit (lo, hi, c2.y);
        path.cubicTo (c1, c2, p2);
    }
}

std::vector<juce::Point<float>> toPoints (const FreqAxis& fx, const DbAxis& axis, const float* hz, const float* db, int count)
{
    std::vector<juce::Point<float>> pts;
    pts.reserve ((size_t) juce::jmax (0, count));
    const float top = axis.area.getY() - 60.0f, bottom = axis.area.getBottom() + 60.0f;

    for (int i = 0; i < count; ++i)
    {
        if (! std::isfinite (db[i]) || hz[i] <= 0.0f)
            continue;

        pts.push_back ({ fx.hzToX (hz[i]), juce::jlimit (top, bottom, axis.dbToY (db[i])) });
    }

    return pts;
}
} // namespace

juce::Path SpectralDisplay::makeCurvePath (const float* hz, const float* db, int count, const DbAxis& axis) const
{
    juce::Path path;
    appendSmooth (path, toPoints (impl->axis(), axis, hz, db, count), false);
    return path;
}

juce::Path SpectralDisplay::makeBandPath (const float* hz, const float* lowDb, const float* highDb, int count,
                                          const DbAxis& axis) const
{
    const auto fx = impl->axis();
    auto upper = toPoints (fx, axis, hz, highDb, count);
    auto lower = toPoints (fx, axis, hz, lowDb, count);
    std::reverse (lower.begin(), lower.end());

    juce::Path path;

    if (upper.size() < 2 || lower.size() < 2)
        return path;

    appendSmooth (path, upper, false);
    appendSmooth (path, lower, true);
    path.closeSubPath();
    return path;
}

void SpectralDisplay::paint (juce::Graphics& g) { impl->paint (g); }
void SpectralDisplay::resized() {}

void SpectralDisplay::mouseMove (const juce::MouseEvent& e)
{
    impl->updateHover (e.position);
}

void SpectralDisplay::mouseExit (const juce::MouseEvent&)
{
    auto& d = *impl;

    if (d.hoverTarget != Target::none)
    {
        d.hoverTarget = Target::none;
        d.hoverIndex = -1;
        repaint();
    }
}

void SpectralDisplay::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const auto hit = d.hitTest (e.position);

    if (e.mods.isPopupMenu())
    {
        d.showMenuFor (hit.first, hit.second);
        return;
    }

    if (! isLeftClick (e) || e.getNumberOfClicks() >= 2)
        return;

    d.hoverTarget = hit.first;
    d.hoverIndex = hit.second;

    switch (hit.first)
    {
        case Target::solo:
            d.setSolo (! d.shownSolo);
            return;

        case Target::pill:
            if (e.mods.isAltDown() && d.withSolo)
            {
                d.altSolo = true;
                d.soloBeforeAlt = d.shownSolo;
                d.setSolo (true);
                return;
            }

            d.gesture.start (d.lowId, d.linkedLowId, d.highId, d.linkedHighId);
            break;

        case Target::lowHandle:  d.gesture.start (d.lowId, d.linkedLowId); break;
        case Target::highHandle: d.gesture.start (d.highId, d.linkedHighId); break;
        case Target::zone:
            d.gesture.start (d.zones[(size_t) hit.second].paramId, d.zones[(size_t) hit.second].linkedId);
            break;

        case Target::none:
        default:
            return;
    }

    d.dragTarget = hit.first;
    d.dragIndex = hit.second;
    repaint();
}

void SpectralDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (isLeftClick (e))
        impl->dragTo (e);
}

void SpectralDisplay::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (d.altSolo)
    {
        d.altSolo = false;
        d.setSolo (d.soloBeforeAlt);
    }

    if (d.dragTarget != Target::none)
    {
        d.gesture.end();
        d.dragTarget = Target::none;
        d.dragIndex = -1;
        repaint();
    }

    d.updateHover (e.position);
}

void SpectralDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e))
        return;

    d.gesture.end();
    d.dragTarget = Target::none;
    const auto hit = d.hitTest (e.position);

    switch (hit.first)
    {
        case Target::lowHandle:
            stereo::promptParamValue (*this, d.processor, d.lowId, d.linkedLowId, d.regionName() + " low cutoff", d.lowConstraint());
            break;

        case Target::highHandle:
            stereo::promptParamValue (*this, d.processor, d.highId, d.linkedHighId, d.regionName() + " high cutoff", d.highConstraint());
            break;

        case Target::pill:
            d.resetRegion();
            break;

        case Target::zone:
            stereo::promptParamValue (*this, d.processor, d.zones[(size_t) hit.second].paramId, d.zones[(size_t) hit.second].linkedId,
                                      d.zones[(size_t) hit.second].name + " amount");
            break;

        case Target::solo:
        case Target::none:
        default:
            return;
    }

    if (onRegionChanged != nullptr && hit.first != Target::zone)
        onRegionChanged();

    repaint();
}

juce::MouseCursor SpectralDisplay::getMouseCursor()
{
    switch (impl->dragTarget != Target::none ? impl->dragTarget : impl->hoverTarget)
    {
        case Target::lowHandle:
        case Target::highHandle:
        case Target::pill:  return juce::MouseCursor::LeftRightResizeCursor;
        case Target::zone:  return juce::MouseCursor::UpDownLeftRightResizeCursor;
        case Target::solo:  return juce::MouseCursor::PointingHandCursor;
        case Target::none:
        default: break;
    }

    return juce::MouseCursor::NormalCursor;
}
} // namespace ps::ui
