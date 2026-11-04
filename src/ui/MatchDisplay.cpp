#include "ui/MatchDisplay.h"

#include "ui/Controls.h"

#include <cmath>

// Match EQ display (owner: ui-meq). See MatchDisplay.h for what it shows and the mouse map.
//
// Scales: the grid and its labels belong to the matched curve (+/- range dB around 0, like the Equalizer's display).
// The two spectra are drawn on their own relative scale (kSpectraSpanDb over the plot height), centred on the mean of
// the first one that exists, so their shapes (not their absolute level, which matching ignores) can be compared.
namespace ps::ui
{
namespace
{
constexpr int kRanges[] { 6, 12, 24 };
constexpr float kSpectraSpanDb = 60.0f;        // dB of spectrum over the height of the curve axis
constexpr float kSpectraCentre = 0.36f;        // where the spectra's mean sits (fraction of the height from the top)
constexpr float kAnalyserMinDb = -90.0f, kAnalyserMaxDb = 0.0f;
constexpr float kHandleGrab = 6.0f;            // +- px around a cutoff line
constexpr float kMinRegionRatio = 1.122462048f;   // 2^(1/6): readMatchEqParams keeps hi >= lo * 2^(1/6)
constexpr float kTaperOctaves = 0.5f;          // computeMatchCurve step 4

int& rememberedRange()
{
    static int range = 12;   // UI state for the session, not a parameter
    return range;
}

juce::String hzLong (float hz)
{
    if (hz < 999.5f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    const float k = hz / 1000.0f;
    return (k < 9.95f ? juce::String (k, 1) : juce::String (juce::roundToInt (k))) + " kHz";
}

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

    const double v = text.replaceCharacter (',', '.').getDoubleValue() * scale;

    if (! (v > 0.0) || ! std::isfinite (v))
        return false;

    hz = (float) v;
    return true;
}

// Shows a popup menu at the mouse (the display is large: menus open where the click was).
void showMenuAtMouse (juce::PopupMenu& menu, juce::Component& owner, std::function<void (int)> done, int minWidth)
{
    menu.setLookAndFeel (&owner.getLookAndFeel());
    const auto pos = juce::Desktop::getMousePosition();
    const auto options = juce::PopupMenu::Options()
                             .withTargetScreenArea (juce::Rectangle<int> (pos.x, pos.y, 1, 1))
                             .withMinimumWidth (minWidth);

    menu.showMenuAsync (options, [safe = juce::Component::SafePointer<juce::Component> (&owner), done = std::move (done)] (int result)
    {
        if (safe != nullptr && done != nullptr)
            done (result);
    });
}

//==============================================================================
// "6 | 12 | 24 dB" range switch (same look as the Equalizer display's).
class RangeSwitch final : public juce::Component, public juce::SettableTooltipClient
{
public:
    RangeSwitch()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Display range of the matched curve (+/- dB). Only changes the view, not the sound.");
    }

    static int idealWidth() { return 26 + 3 * 26; }

    juce::Rectangle<float> segment (int i) const
    {
        auto b = getLocalBounds().toFloat().withTrimmedLeft (26.0f).reduced (0.5f);
        const float w = b.getWidth() / 3.0f;
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

        for (int i = 0; i < 3; ++i)
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

        for (int i = 0; i < 3; ++i)
            if (segment (i).contains (e.position) && onChange != nullptr)
                onChange (kRanges[i]);
    }

    int range = 12;
    juce::Colour colour = Colours::accent;
    std::function<void (int)> onChange;
};
} // namespace

//==============================================================================
struct MatchDisplay::Impl
{
    Impl (MatchDisplay& o, MatchEqModel& m, EditorServices& s) : owner (o), model (m), services (s)
    {
        range.range = rememberedRange();
        range.colour = moduleColour (ps::ModuleType::MatchEq);
        range.onChange = [this] (int r) { owner.setDisplayRangeDb (r); };
        owner.addAndMakeVisible (range);
    }

    ~Impl()
    {
        endGesture();
        stopAudition();
        closeEntry();
    }

    //==========================================================================
    // Geometry
    void layout()
    {
        plot = owner.getLocalBounds().toFloat().reduced (1.0f);
        dy.minDb = -(float) range.range;
        dy.maxDb = (float) range.range;
        dy.area = plot.withTrimmedTop (18.0f).withTrimmedBottom (22.0f);

        const int w = RangeSwitch::idealWidth();
        range.setBounds (juce::Rectangle<int> ((int) plot.getRight() - w - 8, (int) plot.getY() + 7, w, 20));
    }

    FreqAxis axis() const { return makeFreqAxis (services.getOptions(), plot, model.processor.getChain().getSampleRate()); }

    float cutoffHz (bool high) const
    {
        const auto& sp = model.getSetParams();
        return high ? sp.highHz : sp.lowHz;
    }

    // Drawn x of a handle (kept a few pixels inside the plot so a handle at 20 Hz / 20 kHz stays grabbable).
    float handleX (bool high) const
    {
        const float hz = dragHandle == (high ? 1 : 0) ? dragHz : cutoffHz (high);
        return juce::jlimit (plot.getX() + 6.0f, plot.getRight() - 6.0f, axis().hzToX (hz));
    }

    juce::Rectangle<float> handleTab (bool high) const
    {
        const float cy = plot.getBottom() - juce::jmin (64.0f, plot.getHeight() * 0.28f);
        return juce::Rectangle<float> (10.0f, 26.0f).withCentre ({ handleX (high), cy });
    }

    // -1 none, 0 low, 1 high
    int handleAt (juce::Point<float> p) const
    {
        if (! plot.contains (p))
            return -1;

        int best = -1;
        float bestD = kHandleGrab;

        for (int h = 0; h < 2; ++h)
        {
            float d = std::abs (p.x - handleX (h == 1));

            if (handleTab (h == 1).expanded (3.0f).contains (p))
                d = 0.0f;

            if (d < bestD || (d == bestD && best < 0))
            {
                bestD = d;
                best = h;
            }
        }

        // Both handles together at one edge: the one that can move toward the mouse wins.
        if (best >= 0 && std::abs (handleX (false) - handleX (true)) < 2.0f * kHandleGrab)
            best = p.x < handleX (false) ? 0 : 1;

        return best;
    }

    // y of a spectrum value (already level-normalised, dB) at frequency hz on the spectra scale.
    float spectrumY (float db, float hz) const
    {
        const float tilt = services.getOptions().spectrumTiltDbPerOct;
        const float v = db + tilt * std::log2 (juce::jmax (1.0f, hz) / 1000.0f);
        const float pxPerDb = dy.area.getHeight() / kSpectraSpanDb;
        return dy.area.getY() + dy.area.getHeight() * kSpectraCentre - (v - spectraCentre) * pxPerDb;
    }

    // Mean (tilted) level over 60 Hz .. 12 kHz of the first line that exists (the spectra scale's centre).
    float targetCentre() const
    {
        const MatchEqModel::Line* order[] { &model.referenceLine(), &model.currentLine(), &model.runningLine() };
        const float tilt = services.getOptions().spectrumTiltDbPerOct;

        for (const auto* line : order)
        {
            if (! line->valid)
                continue;

            double sum = 0.0;
            int n = 0;

            for (int i = 0; i < ps::kMatchPoints; ++i)
            {
                const double hz = ps::matchGridHz (i);
                const float v = line->db[(size_t) i];

                if (hz < 60.0 || hz > 12000.0 || v <= ps::kMatchNoValue)
                    continue;

                sum += (double) v + (double) tilt * std::log2 (hz / 1000.0);
                ++n;
            }

            if (n > 0)
                return (float) (sum / n);
        }

        return spectraCentre;
    }

    //==========================================================================
    // Cutoff edits (the shown set, plus the other one when linked)
    juce::RangedAudioParameter* paramFor (int s, bool high) const { return model.setParam (s, high ? "hi" : "lo"); }

    float clampCutoff (bool high, float hz) const
    {
        const auto& sp = model.getSetParams();

        if (high)
            return juce::jlimit (juce::jmin (20000.0f, sp.lowHz * kMinRegionRatio), 20000.0f, hz);

        return juce::jlimit (20.0f, juce::jmax (20.0f, sp.highHz / kMinRegionRatio), hz);
    }

    void beginGesture (bool high)
    {
        endGesture();

        for (int s : model.editSets())
            if (auto* p = paramFor (s, high))
            {
                p->beginChangeGesture();
                gestureParams.push_back (p);
            }
    }

    void endGesture()
    {
        for (auto* p : gestureParams)
            p->endChangeGesture();

        gestureParams.clear();
    }

    void setCutoffDuringGesture (float hz)
    {
        for (auto* p : gestureParams)
        {
            const float norm = p->convertTo0to1 (p->getNormalisableRange().snapToLegalValue (hz));

            if (std::abs (p->getValue() - norm) > 1.0e-7f)
                p->setValueNotifyingHost (norm);
        }
    }

    juce::String cutoffLabel (bool high) const
    {
        const auto name = model.isLinked() || ! model.isDual() ? juce::String() : model.setName (model.getSet());
        return juce::String ("Match EQ ") + (high ? "High Cutoff" : "Low Cutoff") + (name.isNotEmpty() ? " (" + name + ")" : juce::String());
    }

    // One undo step (menus, double-click, typed value).
    void setCutoffComplete (bool high, float hz)
    {
        hz = clampCutoff (high, hz);
        std::vector<std::pair<juce::String, float>> values;

        for (int s : model.editSets())
            values.emplace_back (model.setId (s, high ? "hi" : "lo"), hz);

        model.processor.setParameterValues (values, cutoffLabel (high) + ": " + hzLong (hz));
        owner.repaint();
    }

    static float defaultCutoff (bool high) { return high ? 20000.0f : 20.0f; }

    void showHandleMenu (bool high)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (juce::String (high ? "High cutoff: " : "Low cutoff: ") + hzLong (cutoffHz (high)));
        menu.addItem (1, "Reset to default (" + hzLong (defaultCutoff (high)) + ")");
        menu.addItem (2, "Enter value...");

        showMenuAtMouse (menu, owner, [this, high] (int result)
        {
            if (result == 1)
                setCutoffComplete (high, defaultCutoff (high));
            else if (result == 2)
                openEntry (high);
        }, 190);
    }

    void showBackgroundMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Reset both cutoffs (20 Hz - 20 kHz)");
        menu.addSeparator();
        menu.addSectionHeader ("Display range");

        for (int r : kRanges)
            menu.addItem (10 + r, "+/- " + juce::String (r) + " dB", true, r == range.range);

        showMenuAtMouse (menu, owner, [this] (int result)
        {
            if (result == 1)
            {
                std::vector<std::pair<juce::String, float>> values;

                for (int s : model.editSets())
                {
                    values.emplace_back (model.setId (s, "lo"), 20.0f);
                    values.emplace_back (model.setId (s, "hi"), 20000.0f);
                }

                model.processor.setParameterValues (values, "Match EQ: reset the cutoffs");
            }
            else if (result > 10)
            {
                owner.setDisplayRangeDb (result - 10);
            }
        }, 200);
    }

    //==========================================================================
    // Typed cutoff value (a text editor needs keyboard focus while it is open; it closes on Return / Escape / focus loss)
    void openEntry (bool high)
    {
        closeEntry();
        entryHigh = high;
        entry = std::make_unique<juce::TextEditor>();
        auto& t = *entry;
        const auto colour = model.setColour();
        t.setJustification (juce::Justification::centred);
        t.setFont (font (12.5f, 600));
        t.setIndents (4, 3);
        t.setSelectAllWhenFocused (true);
        t.setColour (juce::TextEditor::backgroundColourId, Colours::background);
        t.setColour (juce::TextEditor::textColourId, Colours::text);
        t.setColour (juce::TextEditor::outlineColourId, colour);
        t.setColour (juce::TextEditor::focusedOutlineColourId, colour);
        t.setText (fmt::editNumber (cutoffHz (high)), false);

        juce::Component::SafePointer<MatchDisplay> safe (&owner);
        t.onReturnKey = [safe]
        {
            if (safe == nullptr || safe->impl->entry == nullptr)
                return;

            auto& d = *safe->impl;
            float hz = 0.0f;

            if (parseHz (d.entry->getText(), hz))
                d.setCutoffComplete (d.entryHigh, hz);

            d.closeEntrySoon();
        };
        t.onEscapeKey = [safe] { if (safe != nullptr) safe->impl->closeEntrySoon(); };
        t.onFocusLost = [safe] { if (safe != nullptr) safe->impl->closeEntrySoon(); };

        const auto tab = handleTab (high);
        auto r = juce::Rectangle<float> (84.0f, 22.0f).withCentre ({ tab.getCentreX(), tab.getY() - 18.0f });
        r = r.constrainedWithin (plot.reduced (2.0f));
        owner.addAndMakeVisible (t);
        t.setBounds (r.toNearestInt());
        t.grabKeyboardFocus();
        t.selectAll();
    }

    void closeEntrySoon()
    {
        if (entry != nullptr)
            entry->setVisible (false);

        juce::Component::SafePointer<MatchDisplay> safe (&owner);
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
    }

    //==========================================================================
    // Audition (Alt-drag): band-pass of the module input around the mouse
    void startAudition (juce::Point<float> pos)
    {
        audition = true;
        auditionX = pos.x;
        lastAuditionX = pos.x;
        sendAudition();
    }

    void sendAudition()
    {
        auto m = model.processor.getMonitor (ps::ModuleType::MatchEq);
        m.auditionOn = audition;
        m.auditionHz = juce::jlimit (20.0f, 20000.0f, axis().xToHz (auditionX));
        m.auditionQ = 4.0f;
        m.auditionChannelSet = model.getSet();
        model.processor.setMonitor (ps::ModuleType::MatchEq, m);
        owner.repaint();
    }

    void stopAudition()
    {
        if (! audition)
            return;

        audition = false;
        sendAudition();
    }

    //==========================================================================
    // Painting
    juce::Path linePath (const MatchEqModel::Line& line, const FreqAxis& fx) const
    {
        juce::Path p;
        bool drawing = false;
        const float lo = plot.getY() - 40.0f, hi = plot.getBottom() + 40.0f;

        for (int i = 0; i < ps::kMatchPoints; ++i)
        {
            const float hz = (float) ps::matchGridHz (i);
            const float v = line.db[(size_t) i];

            if (hz < fx.minHz * 0.97f || hz > fx.maxHz * 1.03f || v <= ps::kMatchNoValue)
            {
                drawing = false;
                continue;
            }

            const float x = fx.hzToX (hz);
            const float y = juce::jlimit (lo, hi, spectrumY (v, hz));

            if (! drawing)
                p.startNewSubPath (x, y);
            else
                p.lineTo (x, y);

            drawing = true;
        }

        return p;
    }

    juce::Path curvePath (const FreqAxis& fx, bool closeToZero) const
    {
        juce::Path p;
        const auto& curve = model.getCurve();
        const float lo = dy.area.getY() - 40.0f, hi = dy.area.getBottom() + 40.0f;
        float firstX = 0.0f, lastX = 0.0f;
        bool started = false;

        for (int i = 0; i < ps::kMatchPoints; ++i)
        {
            const float hz = (float) ps::matchGridHz (i);

            if (hz < fx.minHz * 0.97f || hz > fx.maxHz * 1.03f)
                continue;

            const float x = fx.hzToX (hz);
            const float y = juce::jlimit (lo, hi, dy.dbToY (curve[(size_t) i]));

            if (! started)
            {
                p.startNewSubPath (x, y);
                firstX = x;
                started = true;
            }
            else
            {
                p.lineTo (x, y);
            }

            lastX = x;
        }

        if (closeToZero && started)
        {
            const float zero = dy.dbToY (0.0f);
            p.lineTo (lastX, zero);
            p.lineTo (firstX, zero);
            p.closeSubPath();
        }

        return p;
    }

    void paintRegion (juce::Graphics& g, const FreqAxis& fx) const
    {
        const float veil = 0.5f;
        const auto shade = Colours::background.darker (0.35f);

        for (int h = 0; h < 2; ++h)
        {
            const bool high = h == 1;
            const float edgeHz = dragHandle == h ? dragHz : cutoffHz (high);
            const float edgeX = fx.hzToX (edgeHz);
            const float taperX = fx.hzToX (edgeHz * std::pow (2.0f, high ? kTaperOctaves : -kTaperOctaves));

            if (! high && edgeX > plot.getX())
            {
                const float x0 = juce::jmax (plot.getX(), taperX);
                g.setColour (shade.withAlpha (veil));
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (plot.getX(), plot.getY(), x0, plot.getBottom()));

                if (edgeX > x0)
                {
                    g.setGradientFill (juce::ColourGradient (shade.withAlpha (veil), x0, 0.0f, shade.withAlpha (veil * 0.3f), edgeX, 0.0f, false));
                    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x0, plot.getY(), edgeX, plot.getBottom()));
                }
            }
            else if (high && edgeX < plot.getRight())
            {
                const float x1 = juce::jmin (plot.getRight(), taperX);
                g.setColour (shade.withAlpha (veil));
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x1, plot.getY(), plot.getRight(), plot.getBottom()));

                if (x1 > edgeX)
                {
                    g.setGradientFill (juce::ColourGradient (shade.withAlpha (veil * 0.3f), edgeX, 0.0f, shade.withAlpha (veil), x1, 0.0f, false));
                    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (edgeX, plot.getY(), x1, plot.getBottom()));
                }
            }
        }
    }

    void paintHandles (juce::Graphics& g) const
    {
        const auto colour = model.setColour();

        for (int h = 0; h < 2; ++h)
        {
            const bool high = h == 1;
            const float x = handleX (high);
            const bool active = dragHandle == h || (hoverHandle == h && dragHandle < 0);
            const auto c = active ? colour.brighter (0.25f) : colour;

            if (active)
            {
                g.setColour (c.withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (x - 3.0f, plot.getY(), 6.0f, plot.getHeight()));
            }

            // (a handle resting at its end of the range draws a fainter line: it sits over the axis labels)
            const float hz = dragHandle == h ? dragHz : cutoffHz (high);
            const bool atEnd = high ? hz >= 19900.0f : hz <= 20.5f;
            g.setColour (c.withAlpha (active ? 0.95f : (atEnd ? 0.3f : 0.6f)));
            g.fillRect (juce::Rectangle<float> (x - (active ? 1.0f : 0.6f), plot.getY(), active ? 2.0f : 1.2f, plot.getHeight()));

            // Grab tab with grip lines (same as the multiband crossovers).
            const auto t = handleTab (high);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (t.translated (0.0f, 1.5f), 4.0f);
            g.setGradientFill (juce::ColourGradient::vertical (active ? c : blend (Colours::panelHover, c, 0.35f), t.getY(),
                                                               active ? c.darker (0.25f) : blend (Colours::panelRaised, c, 0.2f), t.getBottom()));
            g.fillRoundedRectangle (t, 4.0f);
            g.setColour (active ? c.brighter (0.3f) : c.withAlpha (0.9f));
            g.drawRoundedRectangle (t.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (active ? Colours::background.withAlpha (0.75f) : Colours::text.withAlpha (0.55f));

            for (int k = -1; k <= 1; ++k)
                g.fillRect (juce::Rectangle<float> (t.getCentreX() - 2.5f, t.getCentreY() + (float) k * 4.0f - 0.5f, 5.0f, 1.0f));

            // Frequency chip next to the tab (toward the inside of the region): dim, bright while active.
            const auto text = hzLong (dragHandle == h ? dragHz : cutoffHz (high));
            const auto f = font (active ? 11.5f : 10.5f, active ? 700 : 600);
            const float w = textWidth (f, text) + 14.0f;
            auto chip = juce::Rectangle<float> (w, active ? 20.0f : 18.0f).withCentre ({ 0.0f, t.getCentreY() });
            const bool right = high ? (x - 10.0f - w < plot.getX() + 2.0f) : (x + 10.0f + w <= plot.getRight() - 2.0f);
            chip.setX (right ? x + 10.0f : x - 10.0f - w);
            g.setColour (Colours::background.withAlpha (active ? 0.94f : 0.86f));
            g.fillRoundedRectangle (chip, 5.0f);
            g.setColour (c.withAlpha (active ? 0.9f : 0.45f));
            g.drawRoundedRectangle (chip.reduced (0.5f), 5.0f, 1.0f);
            g.setFont (f);
            g.setColour (active ? Colours::text : Colours::textDim);
            g.drawText (text, chip, juce::Justification::centred, false);
        }
    }

    void paintLegend (juce::Graphics& g) const
    {
        auto legend = juce::Rectangle<float> (plot.getX() + 40.0f, (float) range.getY(), (float) range.getX() - 12.0f - plot.getX() - 40.0f,
                                              (float) range.getHeight());
        legend.removeFromLeft (statusWidth);
        const auto f = font (10.5f, 600);

        auto item = [&] (const juce::String& text, juce::Colour c, bool dashed)
        {
            const float w = textWidth (f, text) + 16.0f;

            if (legend.getWidth() < w)
                return;

            auto r = legend.removeFromRight (w);
            legend.removeFromRight (10.0f);
            const auto swatch = r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 3.0f);
            g.setColour (c);

            if (dashed)
            {
                g.fillRect (swatch.withWidth (4.0f));
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

        item ("Match", blend (model.setColour(), Colours::text, 0.25f), false);
        item ("Current", currentColour(), false);
        item ("Reference", referenceColour(), false);
        item ("Output", Colours::input, false);
    }

    // Top-left status chip (capture / file load / design); returns its width (0 = none).
    float paintStatus (juce::Graphics& g) const
    {
        juce::String text;
        juce::Colour dot = model.setColour();
        float progress = -1.0f;

        if (model.isCapturing())
        {
            const bool ref = model.getCaptureRole() == ps::MatchRole::Reference;
            dot = ref ? referenceColour() : currentColour();
            const double s = model.getCaptureSeconds();
            text = juce::String (ref ? "Capturing reference" : "Capturing current")
                   + (s < 0.05 ? juce::String ("  waiting for audio") : "  " + juce::String (s, 1) + " s");

            if (model.getCaptureGaps() > 0)
                text << "  (" << model.getCaptureGaps() << (model.getCaptureGaps() == 1 ? " gap)" : " gaps)");
        }
        else if (model.isLoading())
        {
            dot = referenceColour();
            progress = model.getLoadProgress();
            text = "Analysing reference file  " + juce::String (juce::roundToInt (progress * 100.0f)) + " %";
        }
        else if (designPending && model.isCurveActive())
        {
            text = "Designing filter...";
        }

        if (text.isEmpty())
            return 0.0f;

        const auto f = font (10.5f, 600);
        const float w = textWidth (f, text) + 30.0f;
        const auto pill = juce::Rectangle<float> (w, 20.0f).withPosition (plot.getX() + 40.0f, (float) range.getY());
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (pill, 10.0f);

        if (progress >= 0.0f)
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (pill.withWidth (pill.getWidth() * juce::jlimit (0.0f, 1.0f, progress)).toNearestInt());
            g.setColour (dot.withAlpha (0.22f));
            g.fillRoundedRectangle (pill, 10.0f);
        }

        g.setColour (dot.withAlpha (0.55f));
        g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, 1.0f);
        const float pulse = 0.45f + 0.55f * (0.5f + 0.5f * std::sin ((float) (juce::Time::getMillisecondCounter() % 100000) * 0.008f));
        g.setColour (dot.withAlpha (model.isCapturing() || progress >= 0.0f || designPending ? pulse : 1.0f));
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ pill.getX() + 12.0f, pill.getCentreY() }));
        g.setFont (f);
        g.setColour (Colours::text.withAlpha (0.9f));
        g.drawText (text, pill.withTrimmedLeft (22.0f).withTrimmedRight (8.0f), juce::Justification::centredLeft, false);
        return w + 12.0f;
    }

    void paintEmptyState (juce::Graphics& g) const
    {
        const bool hasRef = model.has (ps::MatchRole::Reference);
        const bool hasCur = model.has (ps::MatchRole::Current);

        if (audition || model.isCapturing() || model.isLoading())
            return;

        const auto setName = model.isDual() ? model.setName (model.getSet()) : juce::String();
        juce::String title, sub;

        if (hasRef && hasCur)
        {
            // Both exist, but the shown channel has no level in one of them (e.g. the Side of a mono reference).
            const bool refLine = model.referenceLine().valid, curLine = model.currentLine().valid;

            if ((refLine && curLine) || model.isCurveActive() == false)
                return;

            const auto channel = setName.isNotEmpty() && model.getMode() != ps::ChannelMode::TransientSustain ? setName : juce::String ("audio");
            title = "The " + juce::String (refLine ? "current" : "reference") + " spectrum has no " + channel + " signal";
            sub = model.getMode() == ps::ChannelMode::MidSide && model.getSet() == 1
                      ? juce::String ("The Side curve stays flat (mono material has no Side)")
                      : juce::String ("This curve stays flat");
        }
        else if (! hasRef)
        {
            title = hasCur ? juce::String ("No reference yet") : juce::String ("Nothing to match yet");
            sub = "Capture or load a reference to start";
        }
        else
        {
            title = "Reference ready";
            sub = "Now capture your track: play it through the plug-in and press Capture under Current";
        }

        if (model.getMode() == ps::ChannelMode::TransientSustain && setName.isNotEmpty())
            title << " (" << setName << ")";

        const auto titleFont = font (13.0f, 700), subFont = font (11.5f, 600);
        const float maxW = dy.area.getWidth() - 40.0f;
        const float w = juce::jmin (maxW, juce::jmax (textWidth (titleFont, title), textWidth (subFont, sub)) + 36.0f);
        const bool twoLines = textWidth (subFont, sub) + 36.0f > w;
        const float h = twoLines ? 66.0f : 50.0f;
        const auto box = juce::Rectangle<float> (w, h).withCentre (dy.area.getCentre().translated (0.0f, dy.area.getHeight() * 0.06f));
        g.setColour (Colours::background.withAlpha (0.82f));
        g.fillRoundedRectangle (box, 9.0f);
        g.setColour (Colours::outline.withAlpha (0.7f));
        g.drawRoundedRectangle (box.reduced (0.5f), 9.0f, 1.0f);
        g.setFont (titleFont);
        g.setColour (Colours::text.withAlpha (0.9f));
        g.drawText (title, box.withHeight (26.0f).translated (0.0f, 4.0f), juce::Justification::centred, false);
        g.setFont (subFont);
        g.setColour (Colours::textDim);
        g.drawFittedText (sub, box.withTrimmedTop (26.0f).withTrimmedBottom (6.0f).reduced (14.0f, 0.0f).toNearestInt(),
                          juce::Justification::centred, 2, 0.9f);
    }

    void paint (juce::Graphics& g)
    {
        const auto b = owner.getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.3f), b.getY(),
                                                           Colours::background.darker (0.08f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (plot, 6.0f);
        g.reduceClipRegion (clip);

        const auto fx = axis();
        drawFrequencyGrid (g, fx, dy, true);

        // Live output spectrum (its own scale, in the background)
        {
            const auto& options = services.getOptions();
            auto& post = services.getModulePostSpectrum();
            FreqAxis specAxis = fx;
            specAxis.area = plot.withTrimmedBottom (18.0f);
            g.beginTransparencyLayer (0.4f);
            post.draw (g, specAxis, kAnalyserMinDb, kAnalyserMaxDb, Colours::input, options.spectrumFill);
            g.endTransparencyLayer();
            services.drawReferenceSpectrum (g, specAxis, kAnalyserMinDb, kAnalyserMaxDb);   // (Track Referencing)
        }

        paintRegion (g, fx);

        // Spectra
        const bool capturing = model.isCapturing();
        const auto runRole = model.getCaptureRole();
        const float dashes[] { 5.0f, 4.0f };

        auto stroke = [&] (const MatchEqModel::Line& line, juce::Colour c, bool dashed, float width)
        {
            if (! line.valid)
                return;

            const auto path = linePath (line, fx);

            if (dashed)
            {
                juce::Path d;
                juce::PathStrokeType (1.2f).createDashedStroke (d, path, dashes, 2);
                g.setColour (c.withAlpha (0.45f));
                g.fillPath (d);
                return;
            }

            g.setColour (c.withAlpha (0.16f));
            g.strokePath (path, juce::PathStrokeType (width + 3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (c.withAlpha (0.92f));
            g.strokePath (path, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        stroke (model.referenceLine(), referenceColour(), capturing && runRole == ps::MatchRole::Reference, 1.6f);
        stroke (model.currentLine(), currentColour(), capturing && runRole == ps::MatchRole::Current, 1.6f);

        if (capturing && model.runningLine().valid)
        {
            const auto c = runRole == ps::MatchRole::Reference ? referenceColour() : currentColour();
            const float pulse = 0.5f + 0.5f * std::sin ((float) (juce::Time::getMillisecondCounter() % 100000) * 0.006f);
            const auto path = linePath (model.runningLine(), fx);
            g.setColour (c.withAlpha (0.12f + 0.12f * pulse));
            g.strokePath (path, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (c);
            g.strokePath (path, juce::PathStrokeType (1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Matched curve (installed)
        if (model.isCurveActive())
        {
            const auto colour = model.setColour();
            g.setColour (colour.withAlpha (0.10f));
            g.fillPath (curvePath (fx, true));
            const auto path = curvePath (fx, false);
            g.setColour (colour.withAlpha (0.2f));
            g.strokePath (path, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (blend (colour, Colours::text, 0.25f));
            g.strokePath (path, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Audition range
        if (audition)
        {
            const float hz = juce::jlimit (20.0f, 20000.0f, fx.xToHz (auditionX));
            const float half = 0.36f * 0.5f;   // Q 4 = 0.36 octave between the -3 dB points
            const float x0 = fx.hzToX (hz * std::pow (2.0f, -half)), x1 = fx.hzToX (hz * std::pow (2.0f, half));
            const float xc = fx.hzToX (hz);
            juce::ColourGradient grad (Colours::minor.withAlpha (0.02f), x0, 0.0f, Colours::minor.withAlpha (0.02f), x1, 0.0f, false);
            grad.addColour (0.5, Colours::minor.withAlpha (0.16f));
            g.setGradientFill (grad);
            g.fillRect (juce::Rectangle<float> (x0, plot.getY(), juce::jmax (2.0f, x1 - x0), plot.getHeight()));
            g.setColour (Colours::minor.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (xc - 0.5f, plot.getY(), 1.0f, plot.getHeight()));

            const auto text = "Listening around " + hzLong (hz);
            const auto f = font (11.0f, 700);
            const float tw = textWidth (f, text) + 16.0f;
            auto pill = juce::Rectangle<float> (tw, 20.0f).withCentre ({ xc, plot.getY() + 40.0f }).constrainedWithin (plot.reduced (6.0f));
            g.setColour (Colours::minor.withAlpha (0.9f));
            g.fillRoundedRectangle (pill, 10.0f);
            g.setColour (Colours::background);
            g.setFont (f);
            g.drawText (text, pill, juce::Justification::centred, false);
        }

        paintHandles (g);

        // Cursor readout (frequency and the curve there)
        if (mouseInside && hoverHandle < 0 && dragHandle < 0 && ! audition && plot.contains (mousePos))
        {
            const float hz = fx.xToHz (mousePos.x);
            auto text = hzLong (hz);

            if (model.isCurveActive())
            {
                const double idx = juce::jlimit (0.0, (double) ps::kMatchPoints - 1.001, 48.0 * std::log2 ((double) hz / 10.0));
                const int i = (int) idx;
                const float t = (float) (idx - i);
                const auto& curve = model.getCurve();
                text << "   match " << fmt::db (curve[(size_t) i] + t * (curve[(size_t) i + 1] - curve[(size_t) i]));
            }

            const auto f = font (10.5f, 600);
            auto r = juce::Rectangle<float> (textWidth (f, text) + 4.0f, 14.0f).withPosition (mousePos.x + 12.0f, mousePos.y - 20.0f);

            if (r.getRight() > plot.getRight() - 4.0f)
                r.setX (mousePos.x - 12.0f - r.getWidth());

            r = r.constrainedWithin (plot.reduced (4.0f));
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r, juce::Justification::centredLeft, false);
        }

        statusWidth = paintStatus (g);
        paintLegend (g);
        paintEmptyState (g);
    }

    //==========================================================================
    MatchDisplay& owner;
    MatchEqModel& model;
    EditorServices& services;
    RangeSwitch range;

    juce::Rectangle<float> plot;
    DbAxis dy;
    float spectraCentre = -40.0f;
    bool centreValid = false;
    bool designPending = false;
    float statusWidth = 0.0f;

    int hoverHandle = -1, dragHandle = -1;
    float dragHz = 0.0f, dragX = 0.0f, lastMouseX = 0.0f;
    bool gestureOpen = false;
    std::vector<juce::RangedAudioParameter*> gestureParams;

    bool audition = false;
    float auditionX = 0.0f, lastAuditionX = 0.0f;

    juce::Point<float> mousePos;
    bool mouseInside = false;

    std::unique_ptr<juce::TextEditor> entry;
    bool entryHigh = false;
};

//==============================================================================
MatchDisplay::MatchDisplay (MatchEqModel& m, EditorServices& s) : impl (std::make_unique<Impl> (*this, m, s))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setOpaque (false);
    setTooltip ("Reference and Current spectra with the matched curve. Drag the handles to set the frequency range that "
                "is matched (double-click: type a value). Alt-drag to listen to one frequency range. Right-click for the "
                "display range.");
}

MatchDisplay::~MatchDisplay() = default;

juce::Colour MatchDisplay::referenceColour() { return juce::Colour (0xffef8fa8); }   // rose
juce::Colour MatchDisplay::currentColour()   { return Colours::info; }               // sky

void MatchDisplay::update (int changes)
{
    auto& d = *impl;
    if ((changes & MatchEqModel::modeChanged) != 0)
    {
        d.endGesture();
        d.gestureOpen = false;
        d.dragHandle = -1;
        d.stopAudition();
    }
    const float target = d.targetCentre();

    if (! d.centreValid || (changes & (MatchEqModel::modeChanged | MatchEqModel::spectraChanged)) != 0)
    {
        d.spectraCentre = target;
        d.centreValid = true;
    }
    else
    {
        d.spectraCentre += (target - d.spectraCentre) * 0.15f;   // glide while a running spectrum settles
    }

    d.designPending = d.model.isCurveActive() && d.model.isDesignPending();
    repaint();
}

int MatchDisplay::getDisplayRangeDb() const noexcept { return impl->range.range; }

void MatchDisplay::setDisplayRangeDb (int r)
{
    impl->range.range = r;
    rememberedRange() = r;
    impl->range.repaint();
    impl->layout();
    repaint();
}

float MatchDisplay::getCutoffX (bool high) const { return impl->handleX (high); }
juce::Rectangle<float> MatchDisplay::getPlotArea() const { return impl->plot; }

juce::String MatchDisplay::getTooltip()
{
    if (impl->hoverHandle >= 0 || impl->dragHandle >= 0 || impl->audition)
        return {};

    return juce::SettableTooltipClient::getTooltip();
}

void MatchDisplay::paint (juce::Graphics& g) { impl->paint (g); }

void MatchDisplay::resized()
{
    impl->layout();
    impl->closeEntry();
}

void MatchDisplay::mouseMove (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.mouseInside = true;
    d.mousePos = e.position;
    d.hoverHandle = d.handleAt (e.position);
    setMouseCursor (d.hoverHandle >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void MatchDisplay::mouseExit (const juce::MouseEvent&)
{
    impl->mouseInside = false;
    impl->hoverHandle = -1;
    repaint();
}

void MatchDisplay::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const int h = d.handleAt (e.position);

    if (e.mods.isPopupMenu())
    {
        if (h >= 0)
            d.showHandleMenu (h == 1);
        else
            d.showBackgroundMenu();

        return;
    }

    if (! isLeftClick (e))
        return;

    if (e.mods.isAltDown())
    {
        d.startAudition (e.position);
        return;
    }

    if (h < 0)
        return;

    d.dragHandle = h;
    d.dragHz = d.cutoffHz (h == 1);
    d.dragX = d.axis().hzToX (d.dragHz);
    d.lastMouseX = e.position.x;
    d.gestureOpen = false;
    repaint();
}

void MatchDisplay::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.mousePos = e.position;

    if (! isLeftClick (e))
        return;

    if (d.audition)
    {
        const float fine = e.mods.isShiftDown() ? 0.1f : 1.0f;
        d.auditionX = juce::jlimit (d.plot.getX(), d.plot.getRight(), d.auditionX + (e.position.x - d.lastAuditionX) * fine);
        d.lastAuditionX = e.position.x;
        d.sendAudition();
        return;
    }

    if (d.dragHandle < 0)
        return;

    const float dx = e.position.x - d.lastMouseX;
    d.lastMouseX = e.position.x;

    if (std::abs (dx) < 1.0e-4f)
        return;

    const bool high = d.dragHandle == 1;
    const bool fine = e.mods.isShiftDown() || e.mods.isCtrlDown() || e.mods.isCommandDown();
    const auto fx = d.axis();
    d.dragX += dx * (fine ? 0.12f : 1.0f);
    const float hz = d.clampCutoff (high, fx.xToHz (d.dragX));
    d.dragX = fx.hzToX (hz);
    d.dragHz = hz;

    if (! d.gestureOpen)
    {
        d.gestureOpen = true;
        d.beginGesture (high);
    }

    d.setCutoffDuringGesture (hz);
    repaint();
}

void MatchDisplay::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.stopAudition();

    if (d.dragHandle >= 0)
    {
        if (d.gestureOpen)
            d.endGesture();

        d.gestureOpen = false;
        d.dragHandle = -1;
    }

    d.hoverHandle = d.handleAt (e.position);
    setMouseCursor (d.hoverHandle >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void MatchDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! isLeftClick (e) || e.mods.isAltDown())
        return;

    auto& d = *impl;
    const int h = d.handleAt (e.position);

    if (h >= 0)
        d.openEntry (h == 1);
}
} // namespace ps::ui
