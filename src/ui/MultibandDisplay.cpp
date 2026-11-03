#include "ui/MultibandDisplay.h"

#include "ui/Controls.h"

#include <cmath>

// Band display shared by the multiband panels. Owner: ui-dynamics. Contract: MultibandDisplay.h.
//
// Geometry: one dark rounded surface. The top kHeaderH pixels hold the band header strips (aligned with the bands'
// x ranges), the rest is the plot (frequency grid, spectra, band tint, panel overlays, crossover lines with their grab
// handles, the band count control at the bottom right). Crossovers are stored in Hz; every x position is derived from
// the user's frequency axis (makeFreqAxis), so Log / Mel / Extended Log all work.
namespace ps::ui
{
namespace
{
constexpr float kHeaderH = 28.0f;
constexpr float kXoverGrab = 6.0f;                     // +- px around a crossover line
constexpr float kMinXoverRatio = 1.12246204831f;       // 2^(1/6): crossovers stay 1/6 octave apart
constexpr float kMinHz = 20.0f, kMaxHz = 20000.0f;

juce::String hzLong (float hz)
{
    if (hz < 999.5f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    const float k = hz / 1000.0f;
    return (k < 9.95f ? juce::String (k, 1) : juce::String (juce::roundToInt (k))) + " kHz";
}

juce::String hzShort (float hz)
{
    if (hz < 999.5f)
        return juce::String (juce::roundToInt (hz));

    const float k = hz / 1000.0f;
    return (k < 9.95f ? juce::String (k, 1) : juce::String (juce::roundToInt (k))) + "k";
}

// "1.2k", "1200", "1.2 kHz", "850 hz" -> Hz. False when it is not a number.
bool parseHz (juce::String text, float& hz)
{
    text = text.trim().toLowerCase().removeCharacters (" ");

    if (text.endsWith ("hz"))
        text = text.dropLastCharacters (2);

    double scale = 1.0;

    if (text.endsWith ("k"))
    {
        scale = 1000.0;
        text = text.dropLastCharacters (1);
    }

    if (text.isEmpty() || ! text.containsOnly ("0123456789.,"))
        return false;

    hz = (float) (text.replaceCharacter (',', '.').getDoubleValue() * scale);
    return hz > 0.0f;
}
} // namespace

//==============================================================================
struct MultibandDisplay::Impl
{
    Impl (MultibandDisplay& o, PureSheenProcessor& p, EditorServices& s, ps::ModuleType t)
        : owner (o), processor (p), services (s), type (t), prefix (ps::moduleInfo (t).idPrefix), colour (moduleColour (t))
    {
        bandsParam = processor.getParameter (prefix + ".bands");

        for (int i = 0; i < ps::kMaxCrossovers; ++i)
            xoverParams[i] = processor.getParameter (xoverId (i));
    }

    ~Impl()
    {
        endXoverGesture();
    }

    //==============================================================================
    // Parameters
    juce::String xoverId (int i) const { return prefix + ".xover" + juce::String (i + 1); }

    int numBands() const noexcept
    {
        if (bandsParam == nullptr)
            return 1;

        return juce::jlimit (1, ps::kMaxBands, juce::roundToInt (processor.getParameterValue (prefix + ".bands")));
    }

    float rawXoverHz (int i) const
    {
        return xoverParams[i] != nullptr ? processor.getParameterValue (xoverId (i)) : 1000.0f;
    }

    // The crossover the DSP uses (ps::readCrossovers): clamped to 20 Hz..20 kHz and never below the one before it,
    // so stored values that are out of order (host automation, presets) are drawn where the audio splits.
    float xoverHz (int i) const
    {
        float hz = 20.0f;

        for (int k = 0; k <= i; ++k)
            hz = juce::jmax (hz, juce::jlimit (20.0f, 20000.0f, rawXoverHz (k)));

        return hz;
    }

    // Allowed range of crossover i given its visible neighbours.
    juce::Range<float> xoverLimits (int i) const
    {
        const int n = numBands();
        const float lo = i > 0 ? xoverHz (i - 1) * kMinXoverRatio : kMinHz;
        const float hi = i + 2 < n ? xoverHz (i + 1) / kMinXoverRatio : kMaxHz;
        return { juce::jmin (lo, hi), juce::jmax (lo, hi) };
    }

    float clampXover (int i, float hz) const
    {
        const auto r = xoverLimits (i);
        return juce::jlimit (r.getStart(), r.getEnd(), hz);
    }

    void setXoverComplete (int i, float hz)
    {
        if (xoverParams[i] != nullptr)
            processor.setParameterValue (xoverId (i), clampXover (i, hz));
    }

    float defaultXover (int i) const
    {
        if (auto* p = xoverParams[i])
            return p->convertFrom0to1 (p->getDefaultValue());

        return 1000.0f;
    }

    int getSolo() const { return processor.getMonitor (type).soloBand; }

    void setSolo (int band)
    {
        auto m = processor.getMonitor (type);
        const int part = band >= 0 && currentChannelMode (processor, type) == ps::ChannelMode::TransientSustain
                             ? channelSet : -1;

        if (m.soloBand != band || m.soloChannelSet != part)
        {
            m.soloBand = band;
            m.soloChannelSet = part;
            processor.setMonitor (type, m);
        }

        shownSolo = band;
        owner.repaint();
    }

    void setBands (int count)
    {
        if (bandsParam == nullptr)
            return;

        const int current = numBands();
        count = juce::jlimit (1, ps::kMaxBands, count);

        if (count == current)
            return;

        std::vector<std::pair<juce::String, float>> changes { { prefix + ".bands", (float) count } };

        // A new crossover keeps its stored value only when it is ascending; otherwise it moves to the (log) middle
        // between the crossover below it and 20 kHz.
        if (count > current)
        {
            const int i = count - 2;
            const float below = i > 0 ? xoverHz (i - 1) : kMinHz;
            float hz = xoverHz (i);

            if (hz < below * kMinXoverRatio * 1.001f || hz > kMaxHz / kMinXoverRatio)
                hz = i > 0 ? std::sqrt (below * kMaxHz) : defaultXover (i);

            changes.push_back ({ xoverId (i), hz });
        }

        juce::String name = prefix + " bands";

        if (const int index = ps::paramIndex ((prefix + ".bands").toStdString()); index >= 0)
            name = ps::paramSpecs()[(size_t) index].name;

        processor.setParameterValues (changes, name + ": " + juce::String (count));
        owner.repaint();
    }

    //==============================================================================
    // Geometry
    juce::Rectangle<float> inner() const { return owner.getLocalBounds().toFloat().reduced (1.0f); }
    juce::Rectangle<float> headerArea() const { return inner().removeFromTop (kHeaderH); }
    juce::Rectangle<float> plotArea() const { return inner().withTrimmedTop (kHeaderH); }

    FreqAxis axis() const
    {
        return makeFreqAxis (services.getOptions(), plotArea(), processor.getChain().getSampleRate());
    }

    DbAxis levelAxis() const
    {
        DbAxis a;
        a.minDb = minDb;
        a.maxDb = maxDb;
        a.area = plotArea();
        return a;
    }

    float xoverX (int i) const
    {
        const auto plot = plotArea();
        return juce::jlimit (plot.getX(), plot.getRight(), axis().hzToX (xoverHz (i)));
    }

    juce::Rectangle<float> bandArea (int band) const
    {
        const auto plot = plotArea();
        const int n = numBands();

        if (band < 0 || band >= n)
            return {};

        const float left = band == 0 ? plot.getX() : xoverX (band - 1);
        const float right = band == n - 1 ? plot.getRight() : xoverX (band);
        return { left, plot.getY(), juce::jmax (0.0f, right - left), plot.getHeight() };
    }

    juce::Rectangle<float> headerStrip (int band) const
    {
        const auto b = bandArea (band);

        if (b.isEmpty())
            return {};

        const auto h = headerArea();
        return juce::Rectangle<float> (b.getX(), h.getY(), b.getWidth(), h.getHeight()).reduced (2.0f, 4.0f).withTrimmedTop (-1.0f);
    }

    juce::Rectangle<float> soloButton (int band) const
    {
        const auto s = headerStrip (band);

        if (s.getWidth() < 34.0f)
            return {};

        return s.withTrimmedRight (4.0f).removeFromRight (18.0f).withSizeKeepingCentre (18.0f, 16.0f);
    }

    juce::Rectangle<float> countPill() const
    {
        if (bandsParam == nullptr)
            return {};

        const auto plot = plotArea();
        return { plot.getRight() - 104.0f, plot.getBottom() - 44.0f, 96.0f, 22.0f };
    }

    juce::Rectangle<float> minusButton() const { return countPill().removeFromLeft (24.0f); }
    juce::Rectangle<float> plusButton() const  { return countPill().removeFromRight (24.0f); }

    juce::Rectangle<float> xoverHandle (int i) const
    {
        const auto plot = plotArea();
        const float cy = plot.getBottom() - juce::jmin (64.0f, plot.getHeight() * 0.28f);
        return juce::Rectangle<float> (10.0f, 26.0f).withCentre ({ xoverX (i), cy });
    }

    int xoverAt (juce::Point<float> p) const
    {
        const auto plot = plotArea();

        if (! plot.expanded (0.0f, 2.0f).contains (p))
            return -1;

        const int n = numBands();
        int best = -1;
        float bestD = kXoverGrab + 0.01f;

        for (int i = 0; i + 1 < n; ++i)
        {
            float d = std::abs (p.x - xoverX (i));

            if (xoverHandle (i).expanded (3.0f).contains (p))
                d = 0.0f;

            if (d < bestD)
            {
                bestD = d;
                best = i;
            }
        }

        return best;
    }

    int bandAtX (float x) const
    {
        const int n = numBands();

        for (int b = 0; b < n - 1; ++b)
            if (x < xoverX (b))
                return b;

        return n - 1;
    }

    int bandAt (juce::Point<float> p) const
    {
        return plotArea().contains (p) ? bandAtX (p.x) : -1;
    }

    int headerAt (juce::Point<float> p) const
    {
        if (! headerArea().contains (p))
            return -1;

        return bandAtX (p.x);
    }

    //==============================================================================
    // Crossover gestures
    void beginXoverDrag (int i, const juce::MouseEvent& e)
    {
        dragXover = i;
        dragX = xoverX (i);
        lastMouseX = e.position.x;
        gestureOpen = false;
        owner.repaint();
    }

    void dragXoverTo (const juce::MouseEvent& e)
    {
        auto* param = xoverParams[dragXover];

        if (param == nullptr)
            return;

        const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
        const float dx = e.position.x - lastMouseX;
        lastMouseX = e.position.x;

        if (std::abs (dx) < 1.0e-4f)
            return;

        const auto fx = axis();
        dragX += dx * (fine ? 0.12f : 1.0f);
        const float hz = clampXover (dragXover, fx.xToHz (dragX));
        dragX = fx.hzToX (hz);

        if (! gestureOpen)
        {
            gestureOpen = true;
            param->beginChangeGesture();
        }

        param->setValueNotifyingHost (param->convertTo0to1 (hz));
        owner.repaint();
    }

    void endXoverGesture()
    {
        if (gestureOpen && dragXover >= 0 && xoverParams[dragXover] != nullptr)
            xoverParams[dragXover]->endChangeGesture();

        gestureOpen = false;
        dragXover = -1;
    }

    void showXoverMenu (int i)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Crossover " + juce::String (i + 1) + "-" + juce::String (i + 2) + ": " + hzLong (xoverHz (i)));
        menu.addItem (1, "Reset to default (" + hzLong (clampXover (i, defaultXover (i))) + ")");
        menu.addItem (2, "Enter value...");

        juce::Component::SafePointer<MultibandDisplay> safe (&owner);
        showMenu (menu, owner, [safe, i] (int result)
        {
            if (safe == nullptr)
                return;

            auto& d = *safe->impl;

            if (result == 1)
                d.setXoverComplete (i, d.defaultXover (i));
            else if (result == 2)
                d.openEntry (i);
        });
    }

    void openEntry (int i)
    {
        closeEntry();
        entryXover = i;
        entry = std::make_unique<juce::TextEditor>();
        auto& t = *entry;
        t.setJustification (juce::Justification::centred);
        t.setFont (font (12.5f, 600));
        t.setIndents (4, 3);
        t.setSelectAllWhenFocused (true);
        t.setColour (juce::TextEditor::backgroundColourId, Colours::background);
        t.setColour (juce::TextEditor::textColourId, Colours::text);
        t.setColour (juce::TextEditor::outlineColourId, colour);
        t.setColour (juce::TextEditor::focusedOutlineColourId, colour);
        t.setText (hzLong (xoverHz (i)), false);

        juce::Component::SafePointer<MultibandDisplay> safe (&owner);
        t.onReturnKey = [safe]
        {
            if (safe == nullptr || safe->impl->entry == nullptr)
                return;

            auto& d = *safe->impl;
            float hz = 0.0f;

            if (parseHz (d.entry->getText(), hz) && d.entryXover >= 0)
                d.setXoverComplete (d.entryXover, hz);

            d.closeEntrySoon();
        };
        t.onEscapeKey = [safe] { if (safe != nullptr) safe->impl->closeEntrySoon(); };
        t.onFocusLost = [safe] { if (safe != nullptr) safe->impl->closeEntrySoon(); };

        const auto h = xoverHandle (i);
        auto r = juce::Rectangle<float> (84.0f, 22.0f).withCentre ({ h.getCentreX(), h.getY() - 18.0f });
        r = r.constrainedWithin (plotArea().reduced (2.0f));
        owner.addAndMakeVisible (t);
        t.setBounds (r.toNearestInt());
        t.grabKeyboardFocus();
        t.selectAll();
    }

    // The editor's own callbacks run inside it: remove it asynchronously.
    void closeEntrySoon()
    {
        if (entry != nullptr)
            entry->setVisible (false);

        juce::Component::SafePointer<MultibandDisplay> safe (&owner);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->impl->closeEntry();
        });
    }

    void closeEntry()
    {
        if (entry != nullptr)
        {
            owner.removeChildComponent (entry.get());
            entry.reset();
        }

        entryXover = -1;
    }

    //==============================================================================
    // Painting
    void paintHeader (juce::Graphics& g) const
    {
        const int n = numBands();
        const int solo = shownSolo;

        for (int b = 0; b < n; ++b)
        {
            const auto r = headerStrip (b);

            if (r.getWidth() < 6.0f)
                continue;

            const bool sel = b == selected;
            const bool hover = b == hoverHeader || b == hoverBand;
            const bool soloed = solo == b;
            const bool muted = solo >= 0 && ! soloed;

            if (sel)
            {
                g.setGradientFill (juce::ColourGradient::vertical (blend (Colours::panelHover, colour, 0.30f), r.getY(),
                                                                   blend (Colours::panelRaised, colour, 0.16f), r.getBottom()));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (colour.withAlpha (0.85f));
                g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
            }
            else
            {
                g.setColour ((hover ? Colours::panelHover : Colours::panelRaised).withAlpha (hover ? 0.9f : 0.62f));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (Colours::outline.withAlpha (hover ? 0.95f : 0.7f));
                g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
            }

            // S button
            auto text = r.reduced (8.0f, 0.0f);
            const auto s = soloButton (b);

            if (! s.isEmpty())
            {
                text.setRight (s.getX() - 4.0f);
                const bool sHover = hoverSolo == b;
                g.setColour (soloed ? Colours::minor : (sHover ? Colours::panelHover.brighter (0.1f) : Colours::background.withAlpha (0.45f)));
                g.fillRoundedRectangle (s, 4.0f);
                g.setColour (soloed ? Colours::minor.brighter (0.2f) : Colours::outline.brighter (sHover ? 0.35f : 0.1f));
                g.drawRoundedRectangle (s.reduced (0.5f), 4.0f, 1.0f);
                g.setFont (font (10.0f, 700));
                g.setColour (soloed ? Colours::background : (sHover ? Colours::text : Colours::textDim));
                g.drawText ("S", s, juce::Justification::centred, false);
            }

            // Name + range, shortened to fit.
            const float lo = b == 0 ? kMinHz : xoverHz (b - 1);
            const float hi = b == n - 1 ? kMaxHz : xoverHz (b);
            const auto nameFont = font (11.5f, 700);
            const auto rangeFont = font (11.0f, 600);
            const juce::String longName = "Band " + juce::String (b + 1);
            const juce::String shortName = juce::String (b + 1);
            const juce::String longRange = hzLong (lo) + " - " + hzLong (hi);
            const juce::String shortRange = hzShort (lo) + "-" + hzShort (hi);
            const float w = text.getWidth();
            const auto nameColour = muted ? Colours::textFaint : (sel ? Colours::text : Colours::textDim);
            const auto rangeColour = muted ? Colours::textFaint : (sel ? Colours::text.withAlpha (0.78f) : Colours::textSubtle);

            juce::String name = longName, range = longRange;

            if (textWidth (nameFont, name) + 10.0f + textWidth (rangeFont, range) > w)
                range = shortRange;

            if (textWidth (nameFont, name) + 8.0f + textWidth (rangeFont, range) > w)
                name = shortName;

            if (textWidth (nameFont, name) + 8.0f + textWidth (rangeFont, range) > w)
            {
                range.clear();
                name = textWidth (nameFont, longName) <= w ? longName : shortName;
            }

            if (textWidth (nameFont, name) > w)
                continue;

            g.setFont (nameFont);
            g.setColour (nameColour);
            const float nameW = textWidth (nameFont, name);
            g.drawText (name, text.withWidth (nameW + 2.0f), juce::Justification::centredLeft, false);

            if (range.isNotEmpty())
            {
                g.setFont (rangeFont);
                g.setColour (rangeColour);
                g.drawText (range, text.withTrimmedLeft (nameW + 8.0f), juce::Justification::centredLeft, false);
            }
        }
    }

    void paintCountPill (juce::Graphics& g) const
    {
        const auto pill = countPill();

        if (pill.isEmpty() || pill.getX() < plotArea().getX() + 40.0f)
            return;

        const int n = numBands();
        g.setColour (Colours::background.withAlpha (0.82f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (Colours::outline.withAlpha (0.9f));
        g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);

        auto button = [&] (juce::Rectangle<float> r, bool plus, bool enabled, bool hover)
        {
            const auto c = r.reduced (3.0f);

            if (enabled && hover)
            {
                g.setColour (Colours::panelHover);
                g.fillEllipse (c);
            }

            g.setColour (enabled ? (hover ? Colours::text : Colours::textDim) : Colours::textFaint.withAlpha (0.6f));
            const auto centre = c.getCentre();
            g.fillRoundedRectangle (juce::Rectangle<float> (8.0f, 1.6f).withCentre (centre), 0.8f);

            if (plus)
                g.fillRoundedRectangle (juce::Rectangle<float> (1.6f, 8.0f).withCentre (centre), 0.8f);
        };

        button (minusButton(), false, n > 1, hoverCount == 0);
        button (plusButton(), true, n < ps::kMaxBands, hoverCount == 1);

        g.setFont (font (11.0f, 600));
        g.setColour (Colours::text.withAlpha (0.9f));
        g.drawText (juce::String (n) + (n == 1 ? " band" : " bands"), pill.reduced (24.0f, 0.0f), juce::Justification::centred, false);
    }

    void paintLegend (juce::Graphics& g) const
    {
        if (! showSpectrum)
            return;

        const auto pill = countPill();
        const auto plot = plotArea();
        auto legend = juce::Rectangle<float> (plot.getX() + 8.0f, pill.isEmpty() ? plot.getBottom() - 40.0f : pill.getY() + 4.0f,
                                              (pill.isEmpty() ? plot.getRight() - 8.0f : pill.getX() - 10.0f) - plot.getX() - 8.0f, 14.0f);
        const auto f = font (10.5f, 600);

        if (plot.getWidth() <= 420.0f)
            return;

        const std::pair<juce::String, juce::Colour> items[] { { "Output", colour }, { "Input", Colours::input } };
        float total = 0.0f;
        int shown = 0;

        for (const auto& it : items)
        {
            const float w = textWidth (f, it.first) + 16.0f + (shown > 0 ? 10.0f : 0.0f);

            if (total + w > legend.getWidth())
                break;

            total += w;
            ++shown;
        }

        if (shown == 0)
            return;

        // A soft backdrop keeps the legend readable over the spectrum (same as the phase 2 spectral displays).
        auto row = legend.removeFromRight (total);
        g.setColour (Colours::background.withAlpha (0.72f));
        g.fillRoundedRectangle (row.expanded (8.0f, 4.0f), 7.0f);

        for (int i = 0; i < shown; ++i)
        {
            const float w = textWidth (f, items[i].first) + 16.0f;
            auto r = row.removeFromRight (w);
            row.removeFromRight (10.0f);
            g.setColour (items[i].second);
            g.fillRoundedRectangle (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 3.0f), 1.5f);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (items[i].first, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
        }
    }

    void paintCrossovers (juce::Graphics& g) const
    {
        const auto plot = plotArea();
        const int n = numBands();

        for (int i = 0; i + 1 < n; ++i)
        {
            const float x = xoverX (i);
            const bool active = i == dragXover || (i == hoverXover && dragXover < 0);
            const auto c = active ? colour.brighter (0.25f) : colour;

            // Line: a soft glow when active.
            if (active)
            {
                g.setColour (c.withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (x - 3.0f, plot.getY(), 6.0f, plot.getHeight()));
            }

            g.setColour (c.withAlpha (active ? 0.95f : 0.6f));
            g.fillRect (juce::Rectangle<float> (x - (active ? 1.0f : 0.6f), plot.getY(), active ? 2.0f : 1.2f, plot.getHeight()));

            // Grab handle with grip lines.
            const auto h = xoverHandle (i);
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

            // Frequency bubble while hovered / dragged.
            if (active)
            {
                const auto text = hzLong (xoverHz (i));
                const auto f = font (11.5f, 700);
                const float w = textWidth (f, text) + 14.0f;
                auto bubble = juce::Rectangle<float> (w, 20.0f).withY (h.getCentreY() - 10.0f);
                bubble.setX (x + 10.0f + w <= plot.getRight() - 2.0f ? x + 10.0f : x - 10.0f - w);
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

    void paint (juce::Graphics& g)
    {
        const auto b = owner.getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), b.getY(),
                                                           Colours::background.darker (0.05f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);

        // Header row: a slightly lighter band with a separator.
        const auto header = headerArea();
        g.setColour (juce::Colours::white.withAlpha (0.018f));
        g.fillRect (header.withTrimmedTop (6.0f));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (header.getX(), header.getBottom() - 1.0f, header.getWidth(), 1.0f));

        const auto plot = plotArea();
        const int n = numBands();
        const auto fx = axis();
        const auto dy = levelAxis();

        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addRoundedRectangle (plot.getX(), plot.getY(), plot.getWidth(), plot.getHeight(), 6.0f, 6.0f,
                                      false, false, true, true);
            g.reduceClipRegion (clip);

            // Band tint: the selected band (and a whisper of the hovered one).
            for (int band = 0; band < n; ++band)
            {
                const auto r = bandArea (band);

                if (band == selected)
                {
                    g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (0.13f), r.getY(),
                                                                       colour.withAlpha (0.035f), r.getBottom()));
                    g.fillRect (r);
                    g.setColour (colour.withAlpha (0.7f));
                    g.fillRect (r.withHeight (2.0f));
                }
                else if (band == hoverBand && dragXover < 0)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.025f));
                    g.fillRect (r);
                }
            }

            drawFrequencyGrid (g, fx, dy, true);

            if (showSpectrum)
            {
                const auto& options = services.getOptions();
                auto& pre = services.getModulePreSpectrum();
                auto& post = services.getModulePostSpectrum();
                pre.draw (g, fx, minDb, maxDb, Colours::input.withAlpha (0.42f), options.spectrumFill);
                post.draw (g, fx, minDb, maxDb, colour.withAlpha (0.9f), false);
                services.drawReferenceSpectrum (g, fx, minDb, maxDb);   // (Track Referencing)

                if (! pre.hasData() && ! post.hasData())
                {
                    g.setFont (font (12.0f, 600));
                    g.setColour (Colours::textSubtle.withAlpha (0.8f));
                    g.drawText ("Play audio to see the spectrum", plot.withTrimmedBottom (plot.getHeight() * 0.3f),
                                juce::Justification::centred, false);
                }
            }

            // Solo: the other bands go dark.
            if (shownSolo >= 0 && shownSolo < n)
                for (int band = 0; band < n; ++band)
                    if (band != shownSolo)
                    {
                        g.setColour (Colours::background.withAlpha (0.55f));
                        g.fillRect (bandArea (band));
                    }

            if (owner.paintBandOverlay != nullptr)
            {
                for (int band = 0; band < n; ++band)
                {
                    const auto r = bandArea (band);

                    if (r.getWidth() < 1.0f)
                        continue;

                    juce::Graphics::ScopedSaveState bandState (g);
                    g.reduceClipRegion (r.getSmallestIntegerContainer());
                    owner.paintBandOverlay (g, band, r);
                }
            }

            paintCrossovers (g);
            paintLegend (g);
            paintCountPill (g);
        }

        paintHeader (g);

        g.setColour (Colours::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);
    }

    //==============================================================================
    void updateHover (const juce::MouseEvent& e)
    {
        const auto p = e.position;
        const int newXover = dragXover >= 0 ? dragXover : xoverAt (p);
        const int newHeader = headerAt (p);
        int newSolo = -1;

        if (newHeader >= 0 && soloButton (newHeader).expanded (2.0f).contains (p))
            newSolo = newHeader;

        int newCount = -1;

        if (! countPill().isEmpty() && countPill().getX() >= plotArea().getX() + 40.0f)
        {
            if (minusButton().contains (p))      newCount = 0;
            else if (plusButton().contains (p))  newCount = 1;
            else if (countPill().contains (p))   newCount = 2;
        }

        const int newBand = (newXover < 0 && newCount < 0) ? bandAt (p) : -1;

        if (newXover != hoverXover || newHeader != hoverHeader || newSolo != hoverSolo || newCount != hoverCount || newBand != hoverBand)
        {
            hoverXover = newXover;
            hoverHeader = newHeader;
            hoverSolo = newSolo;
            hoverCount = newCount;
            hoverBand = newBand;
            owner.repaint();
        }

        if (hoverXover >= 0)
            owner.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else if (hoverCount == 0 || hoverCount == 1 || hoverSolo >= 0 || hoverHeader >= 0)
            owner.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        else if (hoverBand >= 0 && owner.getBandCursor != nullptr)
            owner.setMouseCursor (owner.getBandCursor (hoverBand, p));
        else
            owner.setMouseCursor (juce::MouseCursor::NormalCursor);

        if (owner.onBandMouseMove != nullptr)
            owner.onBandMouseMove (hoverBand, e);
    }

    void select (int band, bool notify)
    {
        band = juce::jlimit (0, numBands() - 1, band);

        if (band != selected)
        {
            selected = band;
            owner.repaint();

            if (notify && owner.onBandSelected != nullptr)
                owner.onBandSelected (band);
        }
    }

    //==============================================================================
    MultibandDisplay& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    ps::ModuleType type;
    juce::String prefix;
    juce::Colour colour;
    juce::RangedAudioParameter* bandsParam = nullptr;
    juce::RangedAudioParameter* xoverParams[ps::kMaxCrossovers] {};

    int selected = 0;
    int channelSet = 0;
    bool showSpectrum = true;
    float minDb = -84.0f, maxDb = 0.0f;

    // Interaction state
    int hoverXover = -1, hoverHeader = -1, hoverSolo = -1, hoverCount = -1, hoverBand = -1;
    int dragXover = -1;
    float dragX = 0.0f, lastMouseX = 0.0f;
    bool gestureOpen = false;
    int handlerBand = -1;            // the panel owns the current gesture
    int pressedCount = -1;           // band count button pressed (acts on release)
    int pressedSolo = -1;            // header S pressed
    bool altSolo = false;
    int altPreviousSolo = -1;
    int shownSolo = -1, shownBands = 1;
    float shownXover[ps::kMaxCrossovers] {};

    std::unique_ptr<juce::TextEditor> entry;
    int entryXover = -1;
};

//==============================================================================
MultibandDisplay::MultibandDisplay (PureSheenProcessor& p, EditorServices& s, ps::ModuleType t)
{
    impl = std::make_unique<Impl> (*this, p, s, t);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (false);
    impl->shownSolo = impl->getSolo();
    impl->shownBands = impl->numBands();
}

void MultibandDisplay::setChannelSet (int set)
{
    impl->channelSet = set == 1 ? 1 : 0;
    if (impl->getSolo() >= 0)
        impl->setSolo (impl->getSolo());
}

MultibandDisplay::~MultibandDisplay()
{
    if (impl->altSolo)
        impl->setSolo (impl->altPreviousSolo);

    impl->closeEntry();
}

void MultibandDisplay::clearSolo()
{
    impl->altSolo = false;
    impl->setSolo (-1);
}

void MultibandDisplay::tick()
{
    auto& d = *impl;
    const int n = d.numBands();
    const int solo = d.getSolo();

    if (n != d.shownBands)
    {
        d.shownBands = n;

        if (d.selected >= n)
            d.select (n - 1, true);

        // A hidden band cannot stay soloed.
        if (solo >= n)
            d.setSolo (-1);
    }

    d.shownSolo = d.getSolo();
    repaint();
}

void MultibandDisplay::refresh()
{
    auto& d = *impl;
    d.shownBands = d.numBands();
    d.shownSolo = d.getSolo();

    if (d.selected >= d.shownBands)
        d.select (d.shownBands - 1, true);

    repaint();
}

int MultibandDisplay::getNumBands() const noexcept { return impl->numBands(); }
int MultibandDisplay::getSelectedBand() const noexcept { return impl->selected; }
void MultibandDisplay::setSelectedBand (int band) { impl->select (band, false); }
juce::Rectangle<float> MultibandDisplay::getBandArea (int band) const noexcept { return impl->bandArea (band); }
juce::Rectangle<float> MultibandDisplay::getPlotArea() const noexcept { return impl->plotArea(); }
float MultibandDisplay::xForFrequency (float hz) const noexcept { return impl->axis().hzToX (hz); }
float MultibandDisplay::frequencyForX (float x) const noexcept { return impl->axis().xToHz (x); }

void MultibandDisplay::setShowSpectrum (bool shouldShow)
{
    impl->showSpectrum = shouldShow;
    repaint();
}

void MultibandDisplay::setAccentColour (juce::Colour c)
{
    if (c != impl->colour)
    {
        impl->colour = c;
        repaint();
    }
}

void MultibandDisplay::setLevelRange (float minDb, float maxDb)
{
    impl->minDb = juce::jmin (minDb, maxDb - 1.0f);
    impl->maxDb = maxDb;
    repaint();
}

DbAxis MultibandDisplay::getLevelAxis() const noexcept { return impl->levelAxis(); }
juce::Rectangle<float> MultibandDisplay::getBandHeaderArea (int band) const noexcept { return impl->headerStrip (band); }
int MultibandDisplay::getSoloBand() const noexcept { return impl->shownSolo; }
int MultibandDisplay::getHoverBand() const noexcept { return impl->hoverBand; }

void MultibandDisplay::paint (juce::Graphics& g) { impl->paint (g); }
void MultibandDisplay::resized() { repaint(); }

void MultibandDisplay::mouseMove (const juce::MouseEvent& e) { impl->updateHover (e); }

void MultibandDisplay::mouseExit (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.hoverXover = d.hoverHeader = d.hoverSolo = d.hoverCount = d.hoverBand = -1;
    setMouseCursor (juce::MouseCursor::NormalCursor);

    if (onBandMouseMove != nullptr)
        onBandMouseMove (-1, e);

    repaint();
}

void MultibandDisplay::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.handlerBand = -1;
    d.pressedCount = -1;
    d.pressedSolo = -1;
    d.updateHover (e);

    const auto p = e.position;

    // Right-click / ctrl-click: menus only, never an edit.
    if (e.mods.isPopupMenu())
    {
        if (const int x = d.xoverAt (p); x >= 0)
            d.showXoverMenu (x);
        else if (const int band = d.bandAt (p); band >= 0 && onBandPopupMenu != nullptr)
            onBandPopupMenu (band, e);

        return;
    }

    if (! isLeftClick (e))
        return;

    // Band count buttons.
    if (d.hoverCount == 0 || d.hoverCount == 1)
    {
        d.pressedCount = d.hoverCount;
        return;
    }

    if (d.hoverCount == 2)
        return;

    // Header strips: S = solo (on release), elsewhere = select.
    if (const int h = d.headerAt (p); h >= 0)
    {
        if (d.soloButton (h).expanded (2.0f).contains (p))
            d.pressedSolo = h;
        else
            d.select (h, true);

        return;
    }

    if (const int x = d.xoverAt (p); x >= 0)
    {
        d.beginXoverDrag (x, e);
        return;
    }

    const int band = d.bandAt (p);

    if (band < 0)
        return;

    if (e.mods.isAltDown())
    {
        d.altSolo = true;
        d.altPreviousSolo = d.getSolo();
        d.setSolo (band);
        d.select (band, true);
        return;
    }

    if (onBandMouseDown != nullptr && onBandMouseDown (band, e))
    {
        d.handlerBand = band;
        return;
    }

    d.select (band, true);
}

void MultibandDisplay::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e))
        return;

    if (d.dragXover >= 0)
        d.dragXoverTo (e);
    else if (d.handlerBand >= 0 && onBandMouseDrag != nullptr)
        onBandMouseDrag (d.handlerBand, e);
}

void MultibandDisplay::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (d.dragXover >= 0)
    {
        d.endXoverGesture();
        repaint();
    }

    if (d.handlerBand >= 0)
    {
        const int band = d.handlerBand;
        d.handlerBand = -1;

        if (onBandMouseUp != nullptr)
            onBandMouseUp (band, e);
    }

    if (d.altSolo)
    {
        d.altSolo = false;
        d.setSolo (d.altPreviousSolo);
    }

    if (d.pressedCount >= 0)
    {
        const bool inside = (d.pressedCount == 0 ? d.minusButton() : d.plusButton()).contains (e.position);

        if (inside)
            d.setBands (d.numBands() + (d.pressedCount == 0 ? -1 : 1));

        d.pressedCount = -1;
    }

    if (d.pressedSolo >= 0)
    {
        const int band = d.pressedSolo;
        d.pressedSolo = -1;

        if (d.soloButton (band).expanded (2.0f).contains (e.position))
            d.setSolo (d.getSolo() == band ? -1 : band);
    }

    d.updateHover (e);
}

void MultibandDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e) || e.mods.isAltDown())
        return;

    if (const int x = d.xoverAt (e.position); x >= 0)
    {
        d.endXoverGesture();
        d.setXoverComplete (x, d.defaultXover (x));
        return;
    }

    if (const int band = d.bandAt (e.position); band >= 0 && onBandDoubleClick != nullptr)
        onBandDoubleClick (band, e);
}
} // namespace ps::ui
