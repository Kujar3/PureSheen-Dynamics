#include "ui/BassScope.h"

#include "dsp/BassShaper.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/LowEndWidgets.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// Bass Shaper oscilloscope. Owner: ui-lowend. Contract: BassScope.h.
namespace ps::ui
{
namespace
{
constexpr float kSeconds = 4.0f;
constexpr float kFloorDb = -48.0f;        // amplitude axis: 0 .. -48 dBFS per polarity
constexpr int kDecimation = 16;           // samples per scope pair (BassShaper.h)
constexpr int kRingSize = 1 << 16;        // >= 4 s at 192 kHz / 16 (48000 pairs)
constexpr int kPullSize = 1 << 16;        // the FIFOs' capacity: one pull always empties them
constexpr float kLineGrab = 6.0f;
const juce::String kSusThr ("bass.sus.thr"), kSusOn ("bass.sus.on"), kPeakThr ("bass.peak.thr"), kPeakOn ("bass.peak.on");

//==============================================================================
// The single consumer of the Bass Shaper's scope / mask FIFOs (and of its lowPeakDb exchange), shared by every scope
// of one processor.
class ScopeFeed
{
public:
    struct Entry { float mn = 0.0f, mx = 0.0f, w = 0.0f, env = 0.0f; };
    struct Pair { unsigned long long index; float first, second; };

    explicit ScopeFeed (PureSheenProcessor& p) : processor (p)
    {
        ring.resize ((size_t) kRingSize);
        for (auto* v : { &a, &b, &c, &d })
            v->resize ((size_t) kPullSize);
        pendingScope.reserve ((size_t) kPullSize + 64);
        pendingMask.reserve ((size_t) kPullSize + 64);
    }

    static std::shared_ptr<ScopeFeed> forProcessor (PureSheenProcessor& p)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        auto& r = registry();
        r.erase (std::remove_if (r.begin(), r.end(), [] (const auto& e) { return e.second.expired(); }), r.end());

        for (auto& e : r)
            if (e.first == &p)
                if (auto feed = e.second.lock())
                    return feed;

        auto feed = std::make_shared<ScopeFeed> (p);
        r.emplace_back (&p, feed);
        return feed;
    }

    // Pulls whatever arrived. Returns true when new entries were added (or the held peak changed).
    bool update()
    {
        auto* shaper = dynamic_cast<ps::BassShaper*> (&processor.getChain().getModule (ps::ModuleType::BassShaper));

        if (shaper == nullptr)
            return false;

        const int ns = shaper->getScopeFifo().pull (a.data(), b.data(), kPullSize);
        const int nm = shaper->getMaskFifo().pull (c.data(), d.data(), kPullSize);
        const auto scopeEnd = shaper->getScopeFifo().getReadCount();
        const auto maskEnd = shaper->getMaskFifo().getReadCount();

        if (scopeEnd < lastScopeEnd || maskEnd < lastMaskEnd)
        {
            pendingScope.clear();
            pendingMask.clear();
            total = 0;
        }
        lastScopeEnd = scopeEnd;
        lastMaskEnd = maskEnd;

        for (int i = 0; i < ns; ++i)
            pendingScope.push_back ({ scopeEnd - (unsigned long long) (ns - i), a[(size_t) i], b[(size_t) i] });

        for (int i = 0; i < nm; ++i)
            pendingMask.push_back ({ maskEnd - (unsigned long long) (nm - i), c[(size_t) i], d[(size_t) i] });

        // A pull may land between the two writes, and overflow can skip a different number in each FIFO.
        // Match absolute sample indices so an overwritten frame never shifts the mask onto another waveform.
        size_t si = 0, mi = 0;
        bool added = false;
        while (si < pendingScope.size() && mi < pendingMask.size())
        {
            const auto& scope = pendingScope[si];
            const auto& mask = pendingMask[mi];
            if (scope.index < mask.index) { ++si; continue; }
            if (mask.index < scope.index) { ++mi; continue; }

            if (primed)
            {
                auto& e = ring[(size_t) (total & (unsigned long long) (kRingSize - 1))];
                e.mn = finite (scope.first);
                e.mx = finite (scope.second);
                e.w = juce::jlimit (0.0f, 1.0f, finite (mask.first));
                e.env = finite (mask.second);
                ++total;
                added = true;
            }
            ++si;
            ++mi;
        }

        // Drop the entire pre-existing backlog on first attachment, including an unmatched trailing frame.
        if (! primed)
            si = pendingScope.size(), mi = pendingMask.size();
        primed = true;
        pendingScope.erase (pendingScope.begin(), pendingScope.begin() + (std::ptrdiff_t) si);
        pendingMask.erase (pendingMask.begin(), pendingMask.begin() + (std::ptrdiff_t) mi);

        for (auto* pending : { &pendingScope, &pendingMask })
            if (pending->size() > (size_t) kPullSize)
                pending->erase (pending->begin(), pending->end() - kPullSize);

        // Low-band peak (exchanged here only), held for ~1.5 s of updates.
        const float p = shaper->getMeters().lowPeakDb.exchange (-200.0f);
        const double now = juce::Time::getMillisecondCounterHiRes();
        bool peakChanged = false;

        if (std::isfinite (p) && p >= heldPeak)
        {
            peakChanged = std::abs (p - heldPeak) > 0.05f;
            heldPeak = p;
            heldPeakMs = now;
        }
        else if (now - heldPeakMs > 1500.0)
        {
            const float next = std::isfinite (p) ? p : -200.0f;
            peakChanged = std::abs (next - heldPeak) > 0.05f;
            heldPeak = next;
            heldPeakMs = now;
        }

        return added || peakChanged;
    }

    const Entry& entry (unsigned long long index) const { return ring[(size_t) (index & (unsigned long long) (kRingSize - 1))]; }

    unsigned long long total = 0;
    float heldPeak = -200.0f;

private:
    static float finite (float v) noexcept { return std::isfinite (v) ? v : 0.0f; }

    static std::vector<std::pair<const PureSheenProcessor*, std::weak_ptr<ScopeFeed>>>& registry()
    {
        static std::vector<std::pair<const PureSheenProcessor*, std::weak_ptr<ScopeFeed>>> r;
        return r;
    }

    PureSheenProcessor& processor;
    std::vector<Entry> ring;
    std::vector<float> a, b, c, d;
    std::vector<Pair> pendingScope, pendingMask;
    unsigned long long lastScopeEnd = 0, lastMaskEnd = 0;
    double heldPeakMs = 0.0;
    bool primed = false;
};

enum class Line { none, susUp, susDown, peakUp, peakDown };

bool isSustain (Line l) noexcept { return l == Line::susUp || l == Line::susDown; }
} // namespace

//==============================================================================
struct BassScope::Impl
{
    Impl (BassScope& o, PureSheenProcessor& p)
        : owner (o), processor (p), feed (ScopeFeed::forProcessor (p)), colour (moduleColour (ps::ModuleType::BassShaper)),
          susParam (p.getParameter (kSusThr)), peakParam (p.getParameter (kPeakThr))
    {
    }

    ~Impl() { endGesture(); }

    //==========================================================================
    // Geometry
    juce::Rectangle<float> well() const { return owner.getLocalBounds().toFloat(); }
    juce::Rectangle<float> headerArea() const { return well().removeFromTop (28.0f); }

    juce::Rectangle<float> plot() const
    {
        return well().withTrimmedTop (28.0f).withTrimmedBottom (18.0f).withTrimmedLeft (34.0f).withTrimmedRight (6.0f);
    }

    float halfHeight() const { return plot().getHeight() * 0.5f - 4.0f; }

    // Signed amplitude -> y on the dB-warped symmetric axis.
    float yForAmplitude (float v) const
    {
        const auto p = plot();
        const float mag = std::abs (v);
        const float db = mag > 1.0e-6f ? 20.0f * std::log10 (mag) : -200.0f;
        const float h = juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / -kFloorDb);
        return p.getCentreY() - (v >= 0.0f ? 1.0f : -1.0f) * h * halfHeight();
    }

    float yForDb (float db, bool upper) const
    {
        const float h = juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / -kFloorDb);
        return plot().getCentreY() - (upper ? 1.0f : -1.0f) * h * halfHeight();
    }

    float dbForY (float y) const
    {
        const float h = std::abs (plot().getCentreY() - y) / juce::jmax (1.0f, halfHeight());
        return kFloorDb + juce::jlimit (0.0f, 1.0f, h) * -kFloorDb;
    }

    float susDb() const { return processor.getParameterValue (kSusThr); }
    float peakDb() const { return processor.getParameterValue (kPeakThr); }
    bool susOn() const { return processor.getParameterValue (kSusOn) >= 0.5f; }
    bool peakOn() const { return processor.getParameterValue (kPeakOn) >= 0.5f; }

    float lineY (Line l) const
    {
        switch (l)
        {
            case Line::susUp:    return yForDb (susDb(), true);
            case Line::susDown:  return yForDb (susDb(), false);
            case Line::peakUp:   return yForDb (peakDb(), true);
            case Line::peakDown: return yForDb (peakDb(), false);
            case Line::none:
            default: break;
        }

        return 0.0f;
    }

    // Threshold labels at the right end of the upper lines.
    juce::Rectangle<float> labelFor (Line l) const
    {
        const auto p = plot();
        const auto f = font (10.5f, 700);
        const bool sus = isSustain (l);
        const float w = textWidth (f, labelText (sus)) + 14.0f;
        const float yp = lineY (Line::peakUp), ys = lineY (Line::susUp);
        float y = sus ? ys : yp;
        // The two labels never overlap: the lower one moves below its line when they get close.
        float cy = y - 10.0f;

        if (sus && std::abs (ys - yp) < 22.0f)
            cy = juce::jmax (ys, yp) + 10.0f;

        cy = juce::jlimit (p.getY() + 9.0f, p.getBottom() - 9.0f, cy);
        return juce::Rectangle<float> (w, 18.0f).withCentre ({ p.getRight() - 6.0f - w * 0.5f, cy });
    }

    juce::String labelText (bool sus) const
    {
        const float v = sus ? susDb() : peakDb();
        const bool on = sus ? susOn() : peakOn();
        return juce::String (sus ? "Sustain " : "Peak ") + juce::String (v, 1) + " dB" + (on ? "" : " (off)");
    }

    Line lineAt (juce::Point<float> pos) const
    {
        const auto p = plot();

        if (! p.expanded (0.0f, 4.0f).contains (pos))
            return Line::none;

        if (labelFor (Line::peakUp).contains (pos))
            return Line::peakUp;

        if (labelFor (Line::susUp).contains (pos))
            return Line::susUp;

        Line best = Line::none;
        float bestD = kLineGrab + 0.01f;

        for (auto l : { Line::peakUp, Line::peakDown, Line::susUp, Line::susDown })
        {
            const float d = std::abs (pos.y - lineY (l));

            if (d < bestD)
            {
                bestD = d;
                best = l;
            }
        }

        return best;
    }

    //==========================================================================
    // Gesture
    void beginDrag (Line l, juce::Point<float> pos)
    {
        endGesture();
        drag = l;
        dragY = lineY (l);
        lastMouseY = pos.y;
    }

    void dragTo (const juce::MouseEvent& e)
    {
        auto* param = isSustain (drag) ? susParam : peakParam;

        if (param == nullptr)
            return;

        const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();
        const float dy = e.position.y - lastMouseY;
        lastMouseY = e.position.y;

        if (std::abs (dy) < 1.0e-4f)
            return;

        const bool upper = drag == Line::susUp || drag == Line::peakUp;
        const auto p = plot();
        dragY += dy * (fine ? 0.15f : 1.0f);
        dragY = upper ? juce::jlimit (p.getCentreY() - halfHeight(), p.getCentreY(), dragY)
                      : juce::jlimit (p.getCentreY(), p.getCentreY() + halfHeight(), dragY);

        const auto& range = param->getNormalisableRange();
        const float db = juce::jlimit (range.start, range.end, dbForY (dragY));

        if (! gestureOpen)
        {
            gestureOpen = true;
            param->beginChangeGesture();
            // Editing a section's threshold switches the section on (same undo step).
            powerGesture = lowend::beginSectionPower (processor, isSustain (drag) ? kSusOn : kPeakOn);
        }

        param->setValueNotifyingHost (param->convertTo0to1 (db));
        owner.repaint();
    }

    void endGesture()
    {
        if (gestureOpen)
        {
            if (auto* param = isSustain (drag) ? susParam : peakParam)
                param->endChangeGesture();

            if (powerGesture != nullptr)
                powerGesture->endChangeGesture();
        }

        gestureOpen = false;
        powerGesture = nullptr;
        drag = Line::none;
    }

    //==========================================================================
    // Columns: one per pixel of the plot, rebuilt when new data arrived or the size changed.
    void rebuildColumns()
    {
        const int w = juce::jmax (1, (int) plot().getWidth());
        columns.assign ((size_t) w, {});
        const double sr = processor.getChain().getSampleRate();
        signal = false;

        if (sr <= 0.0 || feed->total == 0)
            return;

        const double perSecond = sr / (double) kDecimation;
        const double inView = juce::jmin ((double) kRingSize - 1.0, perSecond * (double) kSeconds);
        const double perColumn = inView / (double) w;
        const auto newest = feed->total;   // exclusive
        const double oldest = (double) newest - inView;
        // The envelope of a column looks back at least 12 ms (a half period at 40 Hz), so a narrow column never lands
        // between two half waves of the bass (no striping at large sizes).
        const double peakSpan = juce::jmax (perColumn, 0.012 * perSecond);

        for (int x = 0; x < w; ++x)
        {
            const double s0 = oldest + perColumn * (double) x;
            const double s1 = s0 + perColumn;
            auto i0 = (long long) std::floor (s0);
            auto i1 = juce::jmax (i0 + 1, (long long) std::floor (s1));
            i0 = juce::jmax (i0, (long long) newest - (long long) (kRingSize - 1));

            if (i1 <= 0 || i0 >= (long long) newest)
                continue;

            i0 = juce::jmax (0LL, i0);
            i1 = juce::jmin ((long long) newest, i1);
            Column col;
            col.valid = i1 > i0;
            col.mn = 1.0e9f;
            col.mx = -1.0e9f;
            float wSum = 0.0f;

            for (auto i = i0; i < i1; ++i)
                wSum += feed->entry ((unsigned long long) i).w;

            const auto p0 = juce::jmax (juce::jmax (0LL, (long long) newest - (long long) (kRingSize - 1)),
                                        juce::jmin (i0, (long long) std::floor (s1 - peakSpan)));

            for (auto i = p0; i < i1; ++i)
            {
                const auto& e = feed->entry ((unsigned long long) i);
                col.mn = juce::jmin (col.mn, e.mn);
                col.mx = juce::jmax (col.mx, e.mx);
            }

            if (! col.valid)
                continue;

            col.w = wSum / (float) (i1 - i0);
            signal = signal || juce::jmax (std::abs (col.mn), std::abs (col.mx)) > 0.001f;
            columns[(size_t) x] = col;
        }
    }

    //==========================================================================
    void paint (juce::Graphics& g)
    {
        const auto b = well();
        stereo::drawWell (g, b, 7.0f);
        paintHeader (g);

        const auto p = plot();
        const float cy = p.getCentreY();

        // Amplitude grid (both polarities) and labels
        for (float db : { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f })
        {
            for (bool upper : { true, false })
            {
                const float y = std::round (yForDb (db, upper));
                g.setColour (Colours::outline.withAlpha (db == 0.0f ? 0.55f : 0.3f));
                g.fillRect (juce::Rectangle<float> (p.getX(), y, p.getWidth(), 1.0f));

                if (p.getHeight() > 120.0f || upper)
                {
                    g.setFont (font (10.0f, 600));
                    g.setColour (Colours::textSubtle);
                    g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (b.getX() + 2.0f, y - 6.0f, 28.0f, 12.0f),
                                juce::Justification::centredRight, false);
                }
            }
        }

        g.setColour (Colours::outline.withAlpha (0.75f));
        g.fillRect (juce::Rectangle<float> (p.getX(), std::round (cy), p.getWidth(), 1.0f));

        // Time ruler
        for (int s = 0; s <= (int) kSeconds; ++s)
        {
            const float x = p.getRight() - (float) s / kSeconds * p.getWidth();
            g.setColour (Colours::outline.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (std::round (x), p.getY(), 1.0f, p.getHeight()));
            g.setFont (font (10.0f, 600));
            g.setColour (Colours::textSubtle);
            const auto text = s == 0 ? juce::String ("now") : "-" + juce::String (s) + " s";
            auto r = juce::Rectangle<float> (44.0f, 14.0f).withCentre ({ x, p.getBottom() + 10.0f });

            if (s == 0)
                r = r.withRightX (p.getRight());

            g.drawText (text, r, s == 0 ? juce::Justification::centredRight : juce::Justification::centred, false);
        }

        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (p.toNearestInt());
            paintWave (g);
            paintLines (g);
        }

        if (! signal)
        {
            g.setFont (font (12.0f, 600));
            g.setColour (Colours::textSubtle.withAlpha (0.8f));
            g.drawText ("Play audio to see the low band", p.withTrimmedTop (p.getHeight() * 0.18f).withHeight (20.0f),
                        juce::Justification::centred, false);
        }
    }

    void paintHeader (juce::Graphics& g) const
    {
        auto h = headerArea().reduced (12.0f, 0.0f).withTrimmedTop (2.0f);
        const auto titleFont = font (11.0f, 700).withExtraKerningFactor (0.08f);
        g.setFont (titleFont);
        g.setColour (Colours::text.withAlpha (0.9f));
        const juce::String title ("LOW BAND");
        g.drawText (title, h, juce::Justification::centredLeft, false);
        h.removeFromLeft (textWidth (titleFont, title) + 10.0f);

        // Peak readout (right), legend (between)
        const auto pk = feed->heldPeak;
        const juce::String peakText = pk > -150.0f ? "Peak " + fmt::db (pk, 1, false) : juce::String ("Peak --");
        const auto pf = font (11.0f, 700);
        const float pw = textWidth (pf, peakText) + 4.0f;
        g.setFont (pf);
        g.setColour (pk > -150.0f ? Colours::text.withAlpha (0.88f) : Colours::textFaint);
        g.drawText (peakText, h.removeFromRight (pw), juce::Justification::centredRight, false);
        h.removeFromRight (14.0f);

        const auto f = font (10.5f, 600);

        auto item = [&] (const juce::String& text, juce::Colour c, int style)
        {
            const float w = textWidth (f, text) + 20.0f;

            if (h.getWidth() < w)
                return;

            auto r = h.removeFromRight (w);
            h.removeFromRight (8.0f);
            auto mark = r.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 2.0f);

            if (style == 0)
            {
                g.setColour (c);
                g.fillRoundedRectangle (mark.withSizeKeepingCentre (10.0f, 8.0f), 2.0f);
            }
            else if (style == 1)
            {
                g.setColour (c);
                const float dashes[] = { 3.0f, 2.0f };
                g.drawDashedLine ({ mark.getX(), mark.getCentreY(), mark.getRight(), mark.getCentreY() }, dashes, 2, 1.6f);
            }
            else
            {
                g.setColour (c);
                g.fillRect (mark.withSizeKeepingCentre (14.0f, 1.6f));
            }

            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
        };

        item ("Peak", Colours::major, 2);
        item ("Sustain", Colours::info, 1);
        item ("Hits", colour, 0);
        item ("Body", Colours::input.withAlpha (0.7f), 0);
    }

    void paintWave (juce::Graphics& g) const
    {
        // A symmetric envelope (the peak of each column, both polarities), the hits tinted from the mask.
        const auto p = plot();
        const auto grey = Colours::input.withAlpha (0.4f);
        juce::Path top, bottom;
        bool started = false;

        for (size_t i = 0; i < columns.size(); ++i)
        {
            const auto& c = columns[i];

            if (! c.valid)
            {
                started = false;
                continue;
            }

            const float x = p.getX() + (float) i;
            const float peak = juce::jmax (std::abs (c.mn), std::abs (c.mx));
            const float yTop = yForAmplitude (peak), yBottom = yForAmplitude (-peak);
            const float t = juce::jlimit (0.0f, 1.0f, c.w * 1.6f);
            g.setColour (blend (grey, colour.withAlpha (0.92f), t));
            g.fillRect (juce::Rectangle<float> (x, yTop, 1.0f, juce::jmax (1.0f, yBottom - yTop)));

            if (! started)
            {
                top.startNewSubPath (x, yTop);
                bottom.startNewSubPath (x, yBottom);
                started = true;
            }
            else
            {
                top.lineTo (x, yTop);
                bottom.lineTo (x, yBottom);
            }
        }

        g.setColour (Colours::text.withAlpha (0.35f));
        g.strokePath (top, juce::PathStrokeType (1.0f));
        g.strokePath (bottom, juce::PathStrokeType (1.0f));
    }

    void paintLines (juce::Graphics& g) const
    {
        const auto p = plot();

        // Sustain first (dashed), the Peak lines above it (solid).
        for (bool sus : { true, false })
        {
            const bool on = sus ? susOn() : peakOn();
            const auto c = sus ? Colours::info : Colours::major;
            const auto up = sus ? Line::susUp : Line::peakUp;
            const auto down = sus ? Line::susDown : Line::peakDown;
            const bool active = drag == up || drag == down || (drag == Line::none && (hover == up || hover == down));
            const float alpha = active ? 1.0f : (on ? 0.85f : 0.35f);

            for (auto l : { up, down })
            {
                const float y = lineY (l);

                if (active)
                {
                    g.setColour (c.withAlpha (0.16f));
                    g.fillRect (juce::Rectangle<float> (p.getX(), y - 3.0f, p.getWidth(), 6.0f));
                }

                g.setColour (c.withAlpha (alpha));

                if (sus)
                {
                    const float dashes[] = { 6.0f, 4.0f };
                    g.drawDashedLine ({ p.getX(), y, p.getRight(), y }, dashes, 2, active ? 2.0f : 1.4f);
                }
                else
                {
                    g.fillRect (juce::Rectangle<float> (p.getX(), y - (active ? 1.0f : 0.7f), p.getWidth(), active ? 2.0f : 1.4f));
                }
            }

            const auto r = labelFor (up);
            g.setColour (Colours::background.withAlpha (0.9f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (c.withAlpha (active ? 1.0f : (on ? 0.8f : 0.4f)));
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, active ? 1.4f : 1.0f);
            g.setFont (font (10.5f, 700));
            g.setColour (on || active ? Colours::text : Colours::textSubtle);
            g.drawText (labelText (sus), r, juce::Justification::centred, false);
        }
    }

    struct Column
    {
        float mn = 0.0f, mx = 0.0f, w = 0.0f;
        bool valid = false;
    };

    BassScope& owner;
    PureSheenProcessor& processor;
    std::shared_ptr<ScopeFeed> feed;
    juce::Colour colour;
    juce::RangedAudioParameter* susParam = nullptr;
    juce::RangedAudioParameter* peakParam = nullptr;
    juce::RangedAudioParameter* powerGesture = nullptr;

    std::vector<Column> columns;
    bool signal = false;
    unsigned long long builtTotal = ~0ULL;
    int builtWidth = -1;

    Line hover = Line::none, drag = Line::none;
    bool gestureOpen = false;
    float dragY = 0.0f, lastMouseY = 0.0f;
    float shownSus = 1.0f, shownPeak = 1.0f, shownPeakDb = 1.0f;
    bool shownSusOn = false, shownPeakOn = false;
};

//==============================================================================
BassScope::BassScope (PureSheenProcessor& p) : impl (std::make_unique<Impl> (*this, p))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTooltip ("The low band (below the cutoff) over the last 4 seconds: hits in colour, the sustained body in grey.\n"
                "Drag the dashed Sustain lines or the solid Peak lines to set the thresholds (this also switches the "
                "section on). Double-click a line to reset it.");
}

BassScope::~BassScope() = default;

void BassScope::setColour (juce::Colour c)
{
    impl->colour = c;
    repaint();
}

void BassScope::update()
{
    auto& d = *impl;
    bool changed = d.feed->update();

    if (d.feed->total != d.builtTotal || (int) d.plot().getWidth() != d.builtWidth)
    {
        d.builtTotal = d.feed->total;
        d.builtWidth = (int) d.plot().getWidth();
        d.rebuildColumns();
        changed = true;
    }

    const float sus = d.susDb(), peak = d.peakDb();
    const bool susOn = d.susOn(), peakOn = d.peakOn();

    if (std::abs (sus - d.shownSus) > 1.0e-3f || std::abs (peak - d.shownPeak) > 1.0e-3f || susOn != d.shownSusOn
        || peakOn != d.shownPeakOn || std::abs (d.feed->heldPeak - d.shownPeakDb) > 0.05f)
    {
        d.shownSus = sus;
        d.shownPeak = peak;
        d.shownSusOn = susOn;
        d.shownPeakOn = peakOn;
        d.shownPeakDb = d.feed->heldPeak;
        changed = true;
    }

    if (changed)
        repaint();
}

bool BassScope::hasSignal() const noexcept { return impl->signal; }
float BassScope::getLowPeakDb() const noexcept { return impl->feed->heldPeak; }

bool BassScope::getLatestValues (float& minimum, float& maximum, float& mask) const noexcept
{
    if (impl->feed->total == 0)
        return false;

    const auto& e = impl->feed->entry (impl->feed->total - 1);
    minimum = e.mn;
    maximum = e.mx;
    mask = e.w;
    return true;
}

float BassScope::getThresholdY (bool sustain, bool upper) const noexcept
{
    return impl->lineY (sustain ? (upper ? Line::susUp : Line::susDown) : (upper ? Line::peakUp : Line::peakDown));
}

void BassScope::paint (juce::Graphics& g) { impl->paint (g); }

void BassScope::resized()
{
    impl->builtWidth = -1;
    impl->rebuildColumns();
    impl->builtWidth = (int) impl->plot().getWidth();
}

void BassScope::mouseMove (const juce::MouseEvent& e)
{
    const auto l = impl->lineAt (e.position);

    if (l != impl->hover)
    {
        impl->hover = l;
        setMouseCursor (l == Line::none ? juce::MouseCursor::NormalCursor : juce::MouseCursor::UpDownResizeCursor);
        repaint();
    }
}

void BassScope::mouseExit (const juce::MouseEvent&)
{
    if (impl->drag == Line::none && impl->hover != Line::none)
    {
        impl->hover = Line::none;
        repaint();
    }
}

void BassScope::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const auto l = d.lineAt (e.position);

    if (l == Line::none)
        return;

    if (e.mods.isPopupMenu())
    {
        const bool sus = isSustain (l);
        stereo::showParamMenu (*this, d.processor, sus ? kSusThr : kPeakThr, {},
                               juce::String (sus ? "Sustain threshold: " : "Peak threshold: ")
                                   + fmt::paramValue (d.processor, sus ? kSusThr : kPeakThr));
        return;
    }

    if (! isLeftClick (e) || e.getNumberOfClicks() >= 2)
        return;

    d.beginDrag (l, e.position);
    repaint();
}

void BassScope::mouseDrag (const juce::MouseEvent& e)
{
    if (impl->drag != Line::none && isLeftClick (e))
        impl->dragTo (e);
}

void BassScope::mouseUp (const juce::MouseEvent& e)
{
    impl->endGesture();
    impl->hover = impl->lineAt (e.position);
    repaint();
}

void BassScope::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e))
        return;

    const auto l = d.lineAt (e.position);

    if (l == Line::none)
        return;

    d.endGesture();
    const auto& id = isSustain (l) ? kSusThr : kPeakThr;
    d.processor.setParameterValue (id, stereo::defaultValueOf (id));
    repaint();
}

} // namespace ps::ui
