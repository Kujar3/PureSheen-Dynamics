#include "ui/RefWaveform.h"

#include "ui/Controls.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <optional>
#include <tuple>

// Waveform strip of the Reference page (see RefWaveform.h). Drawing ported from RefMatch's strip (overview peaks,
// short-term loudness silhouette, loop region, ruler), extended to five loops A-E with their own colours.
// Choices: a drag with no letter armed draws into the first empty letter (all five set: the active one, else A), so
// "drag to loop" works without knowing about the letters; only set loops can be activated; a committed loop is
// drawn where the mouse left it until Track Referencing reports the change (no flicker back).
namespace ps::ui
{
namespace
{
using TR = psplug::TrackReferencing;
namespace Col = Colours;

constexpr float kEdgeGrabPx = 6.0f;           // distance from a loop edge that grabs it
constexpr float kDragThresholdPx = 4.0f;      // a press that moves further is a drag (loop), else a click (seek)
constexpr float kSilhouetteRangeLu = 14.0f;   // loudness from the centre line (bottom) to the full height (top)
constexpr float kSilhouetteHeadroomLu = 1.5f; // top of the range above the track's 95th-percentile short-term loudness
constexpr double kShortTermWindow = 3.0;      // seconds per short-term value (drawn at the window's centre)
constexpr juce::uint32 kCommittedHoldMs = 1000;
const char* const kLetters[] { "A", "B", "C", "D", "E" };

juce::String timeText (double seconds)
{
    const int s = juce::jmax (0, (int) std::floor (seconds + 1.0e-6));
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

float phaseOf (juce::uint32 periodMs)
{
    return (float) (juce::Time::getMillisecondCounter() % periodMs) / (float) periodMs;
}

void drawDashedRoundedRect (juce::Graphics& g, juce::Rectangle<float> r, float radius, float thickness, float dash = 3.0f)
{
    juce::Path outline, dashed;
    outline.addRoundedRectangle (r, radius);
    const float dashes[] = { dash, dash * 0.8f };
    juce::PathStrokeType (thickness).createDashedStroke (dashed, outline, dashes, 2);
    g.fillPath (dashed);
}
} // namespace

//==============================================================================
struct RefWaveform::Impl
{
    Impl (RefWaveform& o, TR& r) : owner (o), refs (r) {}

    enum class Drag { none, pending, create, startEdge, endEdge };

    // ---- geometry ---------------------------------------------------------------------------------------------
    void layout()
    {
        auto inner = owner.getLocalBounds().toFloat().reduced (10.0f, 8.0f);
        const float gutterW = owner.getWidth() < 640 ? 132.0f : 150.0f;
        gutter = inner.removeFromLeft (gutterW);
        inner.removeFromLeft (10.0f);
        ruler = inner.removeFromBottom (14.0f);
        inner.removeFromBottom (2.0f);
        wave = inner.withTrimmedTop (3.0f);   // room for the playhead's head and the loop letters

        const float size = juce::jmin (24.0f, (gutter.getWidth() - 4.0f * 4.0f) / 5.0f);
        const float y = gutter.getBottom() - size;
        for (int i = 0; i < TR::kNumLoops; ++i)
            letterBoxes[(size_t) i] = { gutter.getX() + (float) i * (size + 4.0f), y, size, size };
        cachedFor = {};
    }

    bool hasEntry() const noexcept { return entry.id != 0; }
    bool hasTrack() const noexcept { return hasEntry() && draw != nullptr && length() > 0.0; }

    double length() const noexcept
    {
        if (entry.lengthSeconds > 0.0)
            return entry.lengthSeconds;
        return draw != nullptr ? draw->lengthSeconds : 0.0;
    }

    float xForTime (double t) const noexcept
    {
        const double len = length();
        return len > 0.0 ? wave.getX() + (float) (juce::jlimit (0.0, len, t) / len) * wave.getWidth() : wave.getX();
    }

    double timeForX (float x) const noexcept
    {
        return wave.getWidth() > 0.0f ? juce::jlimit (0.0, length(), (double) ((x - wave.getX()) / wave.getWidth()) * length())
                                      : 0.0;
    }

    // A loop as shown: the one being dragged / just committed replaces the processor's for that letter.
    bool loopShown (int letter, double& start, double& end) const noexcept
    {
        if (local.has_value() && localLetter == letter)
        {
            start = local->first;
            end = local->second;
            return true;
        }
        const auto& l = entry.loops[(size_t) letter];
        start = l.start;
        end = l.end;
        return l.set;
    }

    int activeShown() const noexcept { return local.has_value() ? localLetter : entry.activeLoop; }

    int firstEmptyLetter() const noexcept
    {
        for (int i = 0; i < TR::kNumLoops; ++i)
            if (! entry.loops[(size_t) i].set)
                return i;
        return entry.activeLoop >= 0 ? entry.activeLoop : 0;
    }

    int letterAt (juce::Point<float> p) const
    {
        for (int i = 0; i < TR::kNumLoops; ++i)
            if (letterBoxes[(size_t) i].expanded (2.0f).contains (p))
                return i;
        return -1;
    }

    // ---- cached paths -----------------------------------------------------------------------------------------
    void updateCache()
    {
        const auto key = std::make_tuple (draw.get(), wave, length());
        if (key == cachedFor)
            return;

        cachedFor = key;
        wavePath.clear();
        loudnessPath.clear();
        silhouettePath.clear();
        hasLoudness = false;

        if (! hasTrack() || wave.isEmpty())
            return;

        // Waveform: min / max of the overview per pixel column (filled polygon, top edge then bottom edge back).
        const auto& ov = draw->overview;
        const int columns = (int) juce::jmin (ov.minimum.size(), ov.maximum.size());
        const int pixels = juce::jmax (1, (int) wave.getWidth());
        const float centreY = wave.getCentreY(), half = wave.getHeight() * 0.5f - 1.0f;

        if (columns > 0)
        {
            std::vector<float> lo ((size_t) pixels), hi ((size_t) pixels);
            for (int px = 0; px < pixels; ++px)
            {
                const int c0 = juce::jlimit (0, columns - 1, (int) ((double) px / pixels * columns));
                const int c1 = juce::jlimit (c0 + 1, columns, (int) ((double) (px + 1) / pixels * columns));
                float mn = 0.0f, mx = 0.0f;
                for (int c = c0; c < c1; ++c)
                {
                    mn = juce::jmin (mn, ov.minimum[(size_t) c]);
                    mx = juce::jmax (mx, ov.maximum[(size_t) c]);
                }
                lo[(size_t) px] = juce::jlimit (-1.0f, 1.0f, mn);
                hi[(size_t) px] = juce::jlimit (-1.0f, 1.0f, mx);
            }

            const float minHalfPx = 0.6f;   // silence still shows a hairline
            wavePath.startNewSubPath (wave.getX(), centreY - juce::jmax (minHalfPx, hi[0] * half));
            for (int px = 0; px < pixels; ++px)
                wavePath.lineTo (wave.getX() + (float) px + 0.5f, centreY - juce::jmax (minHalfPx, hi[(size_t) px] * half));
            for (int px = pixels - 1; px >= 0; --px)
                wavePath.lineTo (wave.getX() + (float) px + 0.5f, centreY + juce::jmax (minHalfPx, -lo[(size_t) px] * half));
            wavePath.closeSubPath();
        }

        // Short-term loudness as a mirrored silhouette (the song's structure: intro, breakdowns, drops) with the
        // loudness line on its top edge. The range is per track: the loud parts (95th percentile + headroom) nearly
        // fill the height, kSilhouetteRangeLu below that is the centre line.
        const auto& curve = draw->shortTermCurve;
        const double len = length();
        float top = -2.0f, bottom = -40.0f;
        {
            std::vector<float> values;
            values.reserve (curve.size());
            for (const float v : curve)
                if (ps::hasValue (v) && v > -70.0f)
                    values.push_back (v);
            if (values.size() >= 10)
            {
                const auto k = (size_t) std::floor (0.95 * (double) (values.size() - 1));
                std::nth_element (values.begin(), values.begin() + (std::ptrdiff_t) k, values.end());
                top = juce::jmin (0.0f, values[k] + kSilhouetteHeadroomLu);
                bottom = top - kSilhouetteRangeLu;
            }
        }

        // Loudest value per pixel column (element i is the 3 s window ending at (i + 1) * 0.1 s: drawn at its centre).
        // The first and last 1.5 s use the first / last measured window, so a track does not look silent at its ends.
        const int numValues = (int) curve.size();
        int firstKnown = 0;
        while (firstKnown < numValues && ! ps::hasValue (curve[(size_t) firstKnown]))
            ++firstKnown;
        if (firstKnown >= numValues)
            return;   // no loudness values (shorter than 3 s): the waveform alone

        const int lastIndex = numValues - 1;
        auto indexAt = [] (double t) { return (int) std::ceil ((t + kShortTermWindow * 0.5) * 10.0 - 1.0e-6) - 1; };
        std::vector<float> halfPx ((size_t) pixels, -1.0f);   // half-height in px, -1 = unknown
        const float maxHalf = wave.getHeight() * 0.5f - 1.0f;

        for (int px = 0; px < pixels; ++px)
        {
            const double t0 = len * (double) px / (double) pixels, t1 = len * (double) (px + 1) / (double) pixels;
            int i0 = juce::jmax (0, indexAt (t0)), i1 = juce::jmin (numValues, indexAt (t1));
            if (i1 <= i0)   // fewer values than pixels: the nearest one
            {
                i0 = juce::jlimit (0, numValues, (int) std::lround (((t0 + t1) * 0.5 + kShortTermWindow * 0.5) * 10.0) - 1);
                i1 = juce::jmin (numValues, i0 + 1);
            }
            if (i1 <= firstKnown)
            {
                i0 = firstKnown;
                i1 = firstKnown + 1;
            }
            else if (i0 > lastIndex)
            {
                i0 = lastIndex;
                i1 = numValues;
            }

            float loudest = ps::kNoValue;
            for (int i = i0; i < i1; ++i)
                if (ps::hasValue (curve[(size_t) i]))
                    loudest = ps::hasValue (loudest) ? juce::jmax (loudest, curve[(size_t) i]) : curve[(size_t) i];

            if (ps::hasValue (loudest))
            {
                const float norm = juce::jlimit (0.0f, 1.0f, (loudest - bottom) / (top - bottom));
                halfPx[(size_t) px] = juce::jmax (1.0f, norm * maxHalf);
                hasLoudness = true;
            }
        }

        if (! hasLoudness)
            return;

        auto halfAt = [&] (int px) { return halfPx[(size_t) px] < 0.0f ? 0.6f : halfPx[(size_t) px]; };
        auto xAt = [this] (int px) { return wave.getX() + (float) px + 0.5f; };

        silhouettePath.startNewSubPath (wave.getX(), centreY - halfAt (0));
        for (int px = 0; px < pixels; ++px)
            silhouettePath.lineTo (xAt (px), centreY - halfAt (px));
        silhouettePath.lineTo (wave.getRight(), centreY - halfAt (pixels - 1));
        silhouettePath.lineTo (wave.getRight(), centreY + halfAt (pixels - 1));
        for (int px = pixels - 1; px >= 0; --px)
            silhouettePath.lineTo (xAt (px), centreY + halfAt (px));
        silhouettePath.lineTo (wave.getX(), centreY + halfAt (0));
        silhouettePath.closeSubPath();

        bool penDown = false;
        for (int px = 0; px < pixels; ++px)
        {
            if (halfPx[(size_t) px] < 0.0f)
            {
                penDown = false;
                continue;
            }
            const juce::Point<float> p (xAt (px), centreY - halfPx[(size_t) px]);
            if (penDown)
                loudnessPath.lineTo (p);
            else
                loudnessPath.startNewSubPath (p);
            penDown = true;
        }
    }

    // ---- painting ---------------------------------------------------------------------------------------------
    void paintGutter (juce::Graphics& g) const
    {
        auto r = gutter;
        const int active = activeShown();
        double start = 0.0, end = 0.0;
        const bool loop = active >= 0 && loopShown (active, start, end);

        auto line1 = r.removeFromTop (17.0f);
        g.setFont (font (12.5f, 700));
        if (loop)
        {
            const juce::String name = juce::String ("Loop ") + kLetters[active];
            g.setColour (loopColour (active));
            g.drawText (name, line1, juce::Justification::centredLeft, false);
            g.setFont (font (11.5f, 600));
            g.setColour (Col::textDim);
            g.drawText (timeText (end - start) + " long", line1.withTrimmedLeft (textWidth (font (12.5f, 700), name) + 7.0f),
                        juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour (Col::text);
            g.drawText ("Whole track", line1, juce::Justification::centredLeft, false);
        }

        g.setFont (font (11.5f));
        g.setColour (Col::textDim);
        g.drawText (timeText (playhead) + " / " + timeText (length()), r.removeFromTop (16.0f), juce::Justification::centredLeft, false);

        // Hint between the readouts and the letters (when there is room)
        auto hintArea = r.withBottom (letterBoxes[0].getY() - 2.0f);
        if (hintArea.getHeight() >= 13.0f)
        {
            g.setFont (font (10.5f));
            g.setColour (armed >= 0 ? loopColour (armed).brighter (0.1f) : Col::textSubtle);
            const juce::String hint = armed >= 0 ? juce::String ("Drag on the wave to set ") + kLetters[armed]
                                                 : juce::String ("Loops: drag on the wave");
            g.drawText (hint, hintArea.removeFromBottom (14.0f), juce::Justification::centredLeft, true);
        }

        for (int i = 0; i < TR::kNumLoops; ++i)
            paintLetter (g, i);
    }

    void paintLetter (juce::Graphics& g, int i) const
    {
        const auto b = letterBoxes[(size_t) i];
        const auto c = loopColour (i);
        double s = 0.0, e = 0.0;
        const bool set = loopShown (i, s, e);
        const bool isActive = activeShown() == i;
        const bool hot = hoverLetter == i;
        const float radius = 5.0f;

        if (isActive)
        {
            drawChip (g, b, radius, c.withAlpha (0.92f), c.brighter (0.3f));
        }
        else if (set)
        {
            drawChip (g, b, radius, blend (hot ? Col::panelHover : Col::panelRaised, c, 0.2f), c.withAlpha (hot ? 0.9f : 0.6f));
        }
        else
        {
            g.setColour (hot ? Col::panelHover.withAlpha (0.7f) : juce::Colours::transparentBlack);
            g.fillRoundedRectangle (b, radius);
            g.setColour (armed == i ? c : Col::outline.brighter (hot ? 0.35f : 0.12f));
            drawDashedRoundedRect (g, b.reduced (0.5f), radius, armed == i ? 1.5f : 1.0f);
        }

        g.setFont (font (12.0f, 700));
        g.setColour (isActive ? Col::background : (set ? c.brighter (0.25f) : (armed == i ? c : (hot ? Col::textDim : Col::textFaint.brighter (0.25f)))));
        g.drawText (kLetters[i], b, juce::Justification::centred, false);

        if (armed == i)
        {
            // armed: a breathing dot in the corner
            const float a = 0.55f + 0.45f * std::sin (phaseOf (1200) * juce::MathConstants<float>::twoPi);
            g.setColour (c.withAlpha (a));
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ b.getRight() - 3.0f, b.getY() + 3.0f }));
        }
    }

    void paintLoop (juce::Graphics& g, int i, bool isActive) const
    {
        double s = 0.0, e = 0.0;
        if (! loopShown (i, s, e))
            return;

        const auto c = loopColour (i);
        const float x0 = xForTime (s), x1 = juce::jmax (x0 + 1.0f, xForTime (e));
        const auto region = juce::Rectangle<float> (x0, wave.getY() - 1.0f, x1 - x0, wave.getHeight() + 2.0f);
        g.setColour (c.withAlpha (isActive ? 0.10f : 0.05f));
        g.fillRect (region);
        g.setColour (c.withAlpha (isActive ? 0.85f : 0.5f));
        g.fillRect (region.withHeight (isActive ? 3.0f : 2.0f));

        if (isActive)
        {
            for (const float x : { x0, x1 })
            {
                g.setColour (c.brighter (0.2f));
                g.fillRect (juce::Rectangle<float> (x - 1.0f, wave.getY() - 1.0f, 2.0f, wave.getHeight() + 2.0f));
                const auto tab = juce::Rectangle<float> (6.0f, juce::jmin (16.0f, wave.getHeight() * 0.4f)).withCentre ({ x, wave.getCentreY() });
                g.fillRoundedRectangle (tab, 3.0f);
                g.setColour (Col::panel.withAlpha (0.8f));
                g.fillRect (tab.withSizeKeepingCentre (1.0f, tab.getHeight() * 0.5f));
            }
        }
        else
        {
            g.setColour (c.withAlpha (0.45f));
            g.fillRect (juce::Rectangle<float> (x0, wave.getY(), 1.0f, wave.getHeight()));
            g.fillRect (juce::Rectangle<float> (x1 - 1.0f, wave.getY(), 1.0f, wave.getHeight()));
        }

        // Letter tag at the start of the region
        const auto tag = juce::Rectangle<float> (13.0f, 13.0f).withPosition (x0 + 2.0f, wave.getY() + 3.0f);
        if (tag.getRight() <= wave.getRight() + 1.0f)
        {
            g.setColour (isActive ? c : Col::panel.withAlpha (0.9f));
            g.fillRoundedRectangle (tag, 3.0f);
            g.setColour (isActive ? Col::background : c);
            g.setFont (font (9.5f, 700));
            g.drawText (kLetters[i], tag, juce::Justification::centred, false);
        }
    }

    void paintWave (juce::Graphics& g) const
    {
        g.setColour (Col::background.withAlpha (0.55f));
        g.fillRoundedRectangle (wave.expanded (0.0f, 1.0f), 4.0f);
        g.setColour (Col::outline.withAlpha (0.5f));
        g.fillRect (wave.getX(), wave.getCentreY() - 0.5f, wave.getWidth(), 1.0f);

        // Peaks faint and flat (a loud master peaks near full scale everywhere), the loudness silhouette on top
        g.setColour (Col::reference.withAlpha (hasLoudness ? 0.13f : 0.55f));
        g.fillPath (wavePath);
        if (hasLoudness)
        {
            juce::ColourGradient grad (Col::reference.withAlpha (0.36f), 0.0f, wave.getY(), Col::reference.withAlpha (0.36f), 0.0f,
                                       wave.getBottom(), false);
            grad.addColour (0.5, Col::reference.withAlpha (0.5f));
            g.setGradientFill (grad);
            g.fillPath (silhouettePath);
        }

        // Loops: the others first, then the active one (dims the rest of the track)
        const int active = activeShown();
        double s = 0.0, e = 0.0;
        if (active >= 0 && loopShown (active, s, e))
        {
            const float x0 = xForTime (s), x1 = xForTime (e);
            g.setColour (Col::panel.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (wave.getX(), wave.getY() - 1.0f, x0 - wave.getX(), wave.getHeight() + 2.0f));
            g.fillRect (juce::Rectangle<float> (x1, wave.getY() - 1.0f, wave.getRight() - x1, wave.getHeight() + 2.0f));
        }
        for (int i = 0; i < TR::kNumLoops; ++i)
            if (i != active)
                paintLoop (g, i, false);
        if (active >= 0)
            paintLoop (g, active, true);

        // Short-term loudness line
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (loudnessPath, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Col::text.withAlpha (0.8f));
        g.strokePath (loudnessPath, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (hover && drag == Drag::none && hoverX >= wave.getX() && hoverX <= wave.getRight() && hoverLetter < 0)
        {
            g.setColour (Col::text.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (hoverX - 0.5f, wave.getY(), 1.0f, wave.getHeight()));
        }

        paintPlayhead (g);
    }

    juce::Rectangle<float> playheadArea (double t) const
    {
        const float x = xForTime (t);
        return { x - 6.0f, wave.getY() - 4.0f, 12.0f, wave.getHeight() + 6.0f };
    }

    void paintPlayhead (juce::Graphics& g) const
    {
        const float x = xForTime (playhead);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (x - 1.5f, wave.getY(), 3.0f, wave.getHeight()));
        g.setColour (sounding ? Col::text : Col::textDim.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float> (x - 0.75f, wave.getY(), 1.5f, wave.getHeight()));
        juce::Path tri;
        tri.addTriangle (x - 4.5f, wave.getY() - 3.0f, x + 4.5f, wave.getY() - 3.0f, x, wave.getY() + 2.5f);
        g.fillPath (tri);
    }

    void paintRuler (juce::Graphics& g) const
    {
        const double len = length();
        if (len <= 0.0)
            return;

        const auto f = font (10.5f);
        const double steps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = steps[std::size (steps) - 1];
        for (const double st : steps)
            if (st / len * wave.getWidth() >= 64.0)
            {
                step = st;
                break;
            }

        std::vector<juce::Rectangle<float>> taken;
        auto labelArea = [&] (const juce::String& text, float centreX, const juce::Font& lf)
        {
            const float w = textWidth (lf, text) + 2.0f;
            const float x = juce::jlimit (wave.getX(), wave.getRight() - w, centreX - w * 0.5f);
            return juce::Rectangle<float> (x, ruler.getY() + 1.0f, w, ruler.getHeight() - 1.0f);
        };
        auto drawLabel = [&] (const juce::String& text, juce::Rectangle<float> area, juce::Colour colour, const juce::Font& lf)
        {
            taken.push_back (area);
            g.setFont (lf);
            g.setColour (colour);
            g.drawText (text, area, juce::Justification::centred, false);
        };

        // The active loop's ends in bold, in its colour; one "start - end" label when they would overlap
        const int active = activeShown();
        double s = 0.0, e = 0.0;
        if (active >= 0 && loopShown (active, s, e))
        {
            const auto bold = font (10.5f, 700);
            const auto colour = loopColour (active).brighter (0.2f);
            const auto a = labelArea (timeText (s), xForTime (s), bold), b = labelArea (timeText (e), xForTime (e), bold);
            if (a.expanded (4.0f, 0.0f).intersects (b))
            {
                const auto text = timeText (s) + " - " + timeText (e);
                drawLabel (text, labelArea (text, (xForTime (s) + xForTime (e)) * 0.5f, bold), colour, bold);
            }
            else
            {
                drawLabel (timeText (s), a, colour, bold);
                drawLabel (timeText (e), b, colour, bold);
            }
        }

        for (double t = step; t < len - step * 0.25; t += step)
        {
            const float x = xForTime (t);
            g.setColour (Col::outline.brighter (0.15f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, ruler.getY() - 1.0f, 1.0f, 3.0f));
            const auto text = timeText (t);
            const auto area = labelArea (text, x, f);
            bool free = true;
            for (const auto& r : taken)
                free = free && ! r.expanded (10.0f, 0.0f).intersects (area);
            if (free)
                drawLabel (text, area, Col::textSubtle, f);
        }
    }

    void paintPill (juce::Graphics& g, const juce::String& text, juce::Colour colour, bool spinner) const
    {
        const auto f = font (11.0f, 600);
        const float w = juce::jmin (wave.getWidth() - 12.0f, textWidth (f, text) + (spinner ? 32.0f : 20.0f));
        const auto pill = juce::Rectangle<float> (wave.getRight() - w - 6.0f, wave.getY() + 5.0f, w, 20.0f);
        g.setColour (Col::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (pill, 10.0f);
        g.setColour (colour.withAlpha (0.65f));
        g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, 1.0f);
        auto t = pill.reduced (10.0f, 0.0f);
        if (spinner)
        {
            const float angle = phaseOf (1000) * juce::MathConstants<float>::twoPi;
            juce::Path arc;
            arc.addCentredArc (pill.getX() + 12.0f, pill.getCentreY(), 4.5f, 4.5f, angle, 0.0f, 4.2f, true);
            g.setColour (colour);
            g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            t.removeFromLeft (12.0f);
        }
        g.setFont (f);
        g.setColour (Col::text);
        g.drawFittedText (text, t.toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    // No overview yet: loading (progress), error, missing, or no reference at all.
    void paintMessage (juce::Graphics& g) const
    {
        const auto area = owner.getLocalBounds().toFloat().reduced (10.0f, 8.0f);
        const bool problem = entry.state == TR::State::Error || entry.state == TR::State::Missing;
        const auto tint = problem ? (entry.state == TR::State::Missing ? Col::minor : Col::major) : Col::reference;
        g.setColour (tint.withAlpha (hover && problem ? 0.10f : 0.045f));
        g.fillRoundedRectangle (area, 7.0f);
        g.setColour (tint.withAlpha (hover && problem ? 0.7f : 0.35f));
        drawDashedRoundedRect (g, area.reduced (0.75f), 7.0f, 1.3f, 6.0f);

        juce::String line1, line2;
        if (! hasEntry())
        {
            line1 = "No reference selected";
            line2 = "Pick one in the list, or add one";
        }
        else if (entry.state == TR::State::Missing)
        {
            line1 = "\"" + entry.name + "\" was not found";
            line2 = "Missing - click to find it";
        }
        else if (entry.state == TR::State::Error)
        {
            line1 = "Could not load \"" + entry.name + "\"";
            line2 = (entry.error.isNotEmpty() ? entry.error + " - " : juce::String()) + "click to pick another file";
        }
        else
        {
            line1 = "Loading \"" + entry.name + "\"...";
            line2 = "Decoding and measuring the track: " + juce::String (juce::roundToInt (entry.progress * 100.0f)) + " %";
        }

        auto text = area.reduced (16.0f, 0.0f).withSizeKeepingCentre (area.getWidth() - 32.0f, 44.0f);
        g.setFont (font (14.0f, 600));
        g.setColour (Col::text);
        g.drawFittedText (line1, text.removeFromTop (22.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
        g.setFont (font (12.0f));
        g.setColour (problem ? tint.brighter (0.2f) : Col::textDim);
        g.drawFittedText (line2, text.removeFromTop (18.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);

        if (hasEntry() && entry.state == TR::State::Loading)
        {
            const auto bar = juce::Rectangle<float> (juce::jmin (260.0f, area.getWidth() - 60.0f), 4.0f)
                                 .withCentre ({ area.getCentreX(), text.getY() + 8.0f });
            g.setColour (Col::outline);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (Col::reference);
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (4.0f, bar.getWidth() * juce::jlimit (0.0f, 1.0f, entry.progress))), 2.0f);
        }
    }

    // ---- interaction ------------------------------------------------------------------------------------------
    // The loop edge under x: the active loop first, then the others. Returns the letter (-1 = none).
    int edgeAt (float x, bool& isStart) const
    {
        const int active = activeShown();
        auto test = [&] (int i) {
            double s = 0.0, e = 0.0;
            if (! loopShown (i, s, e))
                return false;
            const float d0 = std::abs (x - xForTime (s)), d1 = std::abs (x - xForTime (e));
            if (juce::jmin (d0, d1) > kEdgeGrabPx)
                return false;
            isStart = d0 <= d1;
            return true;
        };
        if (active >= 0 && test (active))
            return active;
        for (int i = 0; i < TR::kNumLoops; ++i)
            if (i != active && test (i))
                return i;
        return -1;
    }

    void commitLocal()
    {
        if (! local.has_value() || localLetter < 0)
            return;
        localVersion = infoVersion;
        localCommitMs = juce::Time::getMillisecondCounter();
        refs.setLoop (entry.id, localLetter, local->first, local->second);
    }

    RefWaveform& owner;
    TR& refs;

    TR::Entry entry;
    int infoVersion = -1;
    std::shared_ptr<const psplug::RefDrawData> draw;
    double playhead = 0.0;
    bool sounding = false;

    juce::Rectangle<float> gutter, wave, ruler;
    std::array<juce::Rectangle<float>, TR::kNumLoops> letterBoxes {};
    juce::Path wavePath, loudnessPath, silhouettePath;
    bool hasLoudness = false;
    std::tuple<const psplug::RefDrawData*, juce::Rectangle<float>, double> cachedFor {};

    bool hover = false;
    float hoverX = 0.0f;
    int hoverLetter = -1, armed = -1;
    Drag drag = Drag::none;
    float downX = 0.0f;
    double anchorTime = 0.0;
    std::optional<std::pair<double, double>> local;   // being dragged, or committed and not yet confirmed
    int localLetter = -1, localVersion = -1;
    juce::uint32 localCommitMs = 0;
};

//==============================================================================
RefWaveform::RefWaveform (psplug::TrackReferencing& refs) : impl (std::make_unique<Impl> (*this, refs))
{
    setTitle ("Reference waveform");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

RefWaveform::~RefWaveform() = default;

void RefWaveform::setEntry (const psplug::TrackReferencing::Entry* e, int infoVersion)
{
    auto& d = *impl;
    const int newId = e != nullptr ? e->id : 0;
    if (newId == d.entry.id && infoVersion == d.infoVersion)
        return;

    if (newId != d.entry.id)
    {
        // A drag or an armed letter on the old reference must not touch the new one.
        d.drag = Impl::Drag::none;
        d.local.reset();
        d.armed = -1;
    }

    d.entry = e != nullptr ? *e : psplug::TrackReferencing::Entry {};
    d.infoVersion = infoVersion;
    d.draw = newId != 0 ? d.refs.getDrawData (newId) : nullptr;

    // A committed loop stays on screen until Track Referencing reports the change.
    if (d.local.has_value() && d.drag == Impl::Drag::none && d.localVersion != infoVersion)
        d.local.reset();

    if (d.armed >= 0 && d.entry.loops[(size_t) d.armed].set)
        d.armed = -1;   // the armed letter was set (by a drag here, or elsewhere)

    repaint();
}

void RefWaveform::tick()
{
    auto& d = *impl;
    if (d.local.has_value() && d.drag == Impl::Drag::none
        && juce::Time::getMillisecondCounter() - d.localCommitMs > kCommittedHoldMs)
    {
        d.local.reset();
        repaint();
    }

    const double old = d.playhead;
    const bool oldSounding = d.sounding;
    d.playhead = d.hasTrack() ? d.refs.getPlayheadSeconds() : 0.0;
    d.sounding = d.refs.isReferenceSounding();

    // Only animations repaint every tick; static states (missing, error, no selection) repaint on list changes.
    using S = psplug::TrackReferencing::State;
    const bool decoding = d.hasEntry() && (d.entry.state == S::Loading || (d.entry.state == S::Ready && ! d.entry.resident));
    if (decoding || d.entry.analysing || d.armed >= 0 || oldSounding != d.sounding)
    {
        repaint();   // spinners, progress, the armed letter's dot
        return;
    }

    if (std::abs (old - d.playhead) > 1.0e-4)
    {
        repaint (d.playheadArea (old).getSmallestIntegerContainer());
        repaint (d.playheadArea (d.playhead).getSmallestIntegerContainer());
        repaint (d.gutter.withHeight (34.0f).getSmallestIntegerContainer());
    }
}

int RefWaveform::getArmedLetter() const noexcept { return impl->armed; }

void RefWaveform::setArmedLetter (int letter)
{
    impl->armed = letter >= 0 && letter < psplug::TrackReferencing::kNumLoops ? letter : -1;
    repaint();
}

void RefWaveform::resized() { impl->layout(); }

void RefWaveform::paint (juce::Graphics& g)
{
    auto& d = *impl;
    drawPanel (g, getLocalBounds().toFloat());

    if (! d.hasTrack())
    {
        d.paintMessage (g);
        return;
    }

    d.updateCache();
    d.paintGutter (g);
    d.paintWave (g);
    d.paintRuler (g);

    using S = psplug::TrackReferencing::State;
    if (d.entry.state == S::Loading)
        d.paintPill (g, "Loading the audio again...", Col::reference, true);
    else if (d.entry.analysing)
        d.paintPill (g, d.entry.activeLoop >= 0 ? "Measuring the loop..." : "Measuring the track...", Col::reference, true);
    else if (d.entry.state == S::Error || d.entry.state == S::Missing)
        d.paintPill (g, (d.entry.state == S::Missing ? juce::String ("File missing") : d.entry.error) + " - click here to find it",
                     Col::major, false);
}

void RefWaveform::mouseMove (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.hover = true;
    d.hoverX = e.position.x;
    const int letter = d.hasTrack() ? d.letterAt (e.position) : -1;
    d.hoverLetter = letter;

    bool isStart = false;
    if (! d.hasTrack())
        setMouseCursor (d.hasEntry() && d.entry.state != psplug::TrackReferencing::State::Loading ? juce::MouseCursor::PointingHandCursor
                                                                                                 : juce::MouseCursor::NormalCursor);
    else if (letter >= 0)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else if (d.wave.expanded (0.0f, 4.0f).contains (e.position) || d.ruler.contains (e.position))
        setMouseCursor (d.edgeAt (e.position.x, isStart) >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                                               : juce::MouseCursor::PointingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void RefWaveform::mouseExit (const juce::MouseEvent&)
{
    impl->hover = false;
    impl->hoverLetter = -1;
    repaint();
}

void RefWaveform::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    using S = psplug::TrackReferencing::State;

    if (e.mods.isPopupMenu())
    {
        // Right-click a letter: clear that loop.
        const int letter = d.hasTrack() ? d.letterAt (e.position) : -1;
        if (letter >= 0)
        {
            if (d.armed == letter)
                d.armed = -1;
            if (d.entry.loops[(size_t) letter].set)
                d.refs.clearLoop (d.entry.id, letter);
            repaint();
        }
        return;
    }

    if (! isLeftClick (e))
        return;

    if (! d.hasTrack())
    {
        if (d.hasEntry() && (d.entry.state == S::Missing || d.entry.state == S::Error) && onRelink != nullptr)
            onRelink();
        return;
    }

    if ((d.entry.state == S::Missing || d.entry.state == S::Error) && e.position.y < d.wave.getY() + 28.0f
        && e.position.x > d.wave.getRight() - d.wave.getWidth() * 0.5f && onRelink != nullptr)
    {
        onRelink();
        return;
    }

    const int letter = d.letterAt (e.position);
    if (letter >= 0)
    {
        if (d.entry.loops[(size_t) letter].set)
        {
            d.armed = -1;
            d.refs.setActiveLoop (d.entry.id, d.entry.activeLoop == letter ? -1 : letter);
        }
        else
        {
            d.armed = d.armed == letter ? -1 : letter;
        }
        repaint();
        return;
    }

    if (! (d.wave.expanded (0.0f, 4.0f).contains (e.position) || d.ruler.contains (e.position)))
        return;

    d.downX = e.position.x;
    bool isStart = false;
    const int edge = d.armed < 0 ? d.edgeAt (e.position.x, isStart) : -1;
    if (edge >= 0)
    {
        d.drag = isStart ? Impl::Drag::startEdge : Impl::Drag::endEdge;
        d.localLetter = edge;
        const auto& l = d.entry.loops[(size_t) edge];
        d.local = std::make_pair (l.start, l.end);
    }
    else
    {
        d.drag = Impl::Drag::pending;
    }
}

void RefWaveform::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;
    if (! isLeftClick (e) || d.drag == Impl::Drag::none)
        return;

    const double t = d.timeForX (e.position.x);
    const double minLen = psplug::TrackReferencing::kMinLoopSeconds;

    switch (d.drag)
    {
        case Impl::Drag::pending:
            if (std::abs (e.position.x - d.downX) < kDragThresholdPx)
                return;
            d.drag = Impl::Drag::create;
            d.anchorTime = d.timeForX (d.downX);
            d.localLetter = d.armed >= 0 ? d.armed : d.firstEmptyLetter();
            [[fallthrough]];

        case Impl::Drag::create:
            d.local = std::make_pair (juce::jmin (d.anchorTime, t), juce::jmax (d.anchorTime, t));
            break;

        case Impl::Drag::startEdge:
            if (d.local.has_value())
                d.local->first = juce::jlimit (0.0, juce::jmax (0.0, d.local->second - minLen), t);
            break;

        case Impl::Drag::endEdge:
            if (d.local.has_value())
                d.local->second = juce::jlimit (juce::jmin (d.length(), d.local->first + minLen), d.length(), t);
            break;

        case Impl::Drag::none:
            break;
    }

    d.hoverX = e.position.x;
    repaint();
}

void RefWaveform::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;
    // The release can come at a position no drag event reported (released outside the strip): apply it first.
    if (isLeftClick (e) && d.drag != Impl::Drag::none && d.drag != Impl::Drag::pending)
        mouseDrag (e);

    const auto mode = d.drag;
    d.drag = Impl::Drag::none;

    if (! isLeftClick (e) || mode == Impl::Drag::none)
        return;

    if (mode == Impl::Drag::pending)
    {
        d.refs.seek (d.timeForX (e.position.x));
        return;
    }

    if (d.local.has_value())
    {
        if (d.local->second - d.local->first >= psplug::TrackReferencing::kMinLoopSeconds)
        {
            d.commitLocal();
            d.armed = -1;
        }
        else
        {
            d.local.reset();   // too short: a click
            d.refs.seek (d.timeForX (e.position.x));
        }
    }
    repaint();
}

juce::String RefWaveform::getTooltip()
{
    auto& d = *impl;
    using S = psplug::TrackReferencing::State;

    if (! d.hasTrack())
    {
        if (! d.hasEntry())
            return "The waveform of the selected reference shows here.";
        if (d.entry.state == S::Missing)
            return "This reference file was not found (moved, renamed, or the project was opened on another computer). "
                   "Click to find it; its loops are kept.";
        if (d.entry.state == S::Error)
            return "This file could not be loaded: " + d.entry.error + ". Click to pick another file; its loops are kept.";
        return "Loading the reference: decoding and measuring it once takes a few seconds.";
    }

    const auto pos = getMouseXYRelative().toFloat();
    const int letter = d.letterAt (pos);
    if (letter >= 0)
    {
        const juce::String name (kLetters[letter]);
        if (! d.entry.loops[(size_t) letter].set)
            return "Loop " + name + " (empty): click to arm it, then drag across the waveform to choose the part it plays.";
        if (d.entry.activeLoop == letter)
            return "Loop " + name + " is playing: click again to play the whole track. Drag its edges on the waveform to "
                   "move them. Right-click to clear it.";
        return "Loop " + name + ": click to play this part (looped) and compare it. Right-click to clear it.";
    }

    if (d.gutter.contains (pos))
        return "What plays: the whole track, or the active loop A to E (it plays looped, and the comparison measures only "
               "that part). Loop the same section as your song (the drop, the chorus) for the fairest comparison.";

    bool isStart = false;
    if (d.edgeAt (pos.x, isStart) >= 0)
        return "Drag to move this end of the loop";

    return timeText (d.timeForX (pos.x)) + ": click to jump here. Drag across the waveform to set a loop. The shape and "
           "its white edge show the loudness over time; the loud parts are the best ones to compare.";
}
} // namespace ps::ui
