#include "ui/MaximizerWidgets.h"

#include "ui/Controls.h"

#include <cmath>

// Maximizer panel building blocks (owner: ui-max). See MaximizerWidgets.h.
namespace ps::ui
{
namespace
{
constexpr float kGainMin = 0.0f, kGainMax = 20.0f;
constexpr float kCeilingMin = -20.0f, kCeilingMax = 0.0f;
constexpr float kSilenceDb = -200.0f;

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

bool isFine (const juce::MouseEvent& e) noexcept
{
    return e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
}

// Drag resolution: 0.1 dB steps, unrestricted while fine-dragging.
float quantise (float v, bool fine) noexcept
{
    return fine ? v : std::round (v * 10.0f) / 10.0f;
}

float wheelDelta (const juce::MouseWheelDetails& w) noexcept
{
    const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX;
    return w.isReversed ? -d : d;
}

juce::String numberText (float db)
{
    if (std::abs (db) < 0.05f)
        db = 0.0f;

    return (db > 0.0f ? "+" : "") + juce::String (db, 1);
}

// Big value with a small unit after it, centred in `area`.
void drawBigValue (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& value, const juce::String& unit,
                   float size, juce::Colour colour)
{
    auto big = font (size, 700);
    auto small = font (juce::jmax (10.0f, size * 0.46f), 600);
    float wv = textWidth (big, value), wu = unit.isNotEmpty() ? textWidth (small, unit) + 3.0f : 0.0f;

    if (wv + wu > area.getWidth())
    {
        const float k = juce::jmax (0.6f, area.getWidth() / (wv + wu));
        big = font (size * k, 700);
        small = font (juce::jmax (9.0f, size * 0.46f * k), 600);
        wv = textWidth (big, value);
        wu = unit.isNotEmpty() ? textWidth (small, unit) + 3.0f : 0.0f;
    }

    const float x = area.getCentreX() - (wv + wu) * 0.5f;
    g.setFont (big);
    g.setColour (colour);
    g.drawText (value, juce::Rectangle<float> (x, area.getY(), wv + 2.0f, area.getHeight()), juce::Justification::centredLeft, false);

    if (unit.isNotEmpty())
    {
        // Baseline-align the unit with the number.
        const float baseline = area.getCentreY() + big.getAscent() * 0.5f - big.getDescent() * 0.25f;
        g.setFont (small);
        g.setColour (colour.withMultipliedAlpha (0.6f));
        g.drawText (unit, juce::Rectangle<float> (x + wv + 3.0f, baseline - small.getAscent(), wu + 2.0f, small.getHeight()),
                    juce::Justification::topLeft, false);
    }
}

void drawCaption (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour colour)
{
    g.setFont (font (10.5f, 700).withExtraKerningFactor (0.08f));
    g.setColour (colour);
    g.drawFittedText (text, area.toNearestInt(), juce::Justification::centred, 1, 0.8f);
}

// Grip handle: a small pill with three ridges.
void drawHandle (juce::Graphics& g, juce::Rectangle<float> pill, juce::Colour fill, bool hot)
{
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillRoundedRectangle (pill.translated (0.0f, 1.0f), pill.getHeight() * 0.5f);
    const auto c = hot ? fill.brighter (0.25f) : fill;
    g.setGradientFill (juce::ColourGradient::vertical (c.brighter (0.12f), pill.getY(), c.darker (0.12f), pill.getBottom()));
    g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
    g.setColour (juce::Colours::black.withAlpha (0.35f));

    for (int i = -1; i <= 1; ++i)
    {
        const float x = pill.getCentreX() + (float) i * 3.0f;
        g.fillRect (juce::Rectangle<float> (x - 0.5f, pill.getY() + 3.5f, 1.0f, pill.getHeight() - 7.0f));
    }
}
} // namespace

//==============================================================================
// MaxLevelEditor
//==============================================================================
MaxLevelEditor::MaxLevelEditor (PureSheenProcessor& p) : processor (p)
{
    gainParam = p.getParameter ("max.gain");
    ceilingParam = p.getParameter ("max.ceiling");
}

MaxLevelEditor::~MaxLevelEditor()
{
    stopTimer();
    end();
}

float MaxLevelEditor::getGain() const    { return processor.getParameterValue ("max.gain"); }
float MaxLevelEditor::getCeiling() const { return processor.getParameterValue ("max.ceiling"); }
bool MaxLevelEditor::isLinked() const    { return processor.getParameterValue ("max.link") >= 0.5f; }

void MaxLevelEditor::begin (Target t)
{
    beginInternal (t, isLinked());
}

void MaxLevelEditor::beginInternal (Target t, bool withLink)
{
    if (active)
        end();

    stopTimer();
    wheelGesture = false;

    if (gainParam == nullptr || ceilingParam == nullptr)
        return;

    target = t;
    startGain = getGain();
    startCeiling = getCeiling();
    linkedGesture = withLink;
    active = true;

    if (target == Target::gain || linkedGesture)
        gainParam->beginChangeGesture();

    if (target == Target::ceiling || linkedGesture)
        ceilingParam->beginChangeGesture();
}

void MaxLevelEditor::apply (float value)
{
    float g = startGain, c = startCeiling;

    // With Link on, Gain and Output Level move inversely; the move stops where either one reaches its limit.
    if (target == Target::gain)
    {
        float lo = kGainMin - startGain, hi = kGainMax - startGain;

        if (linkedGesture)
        {
            lo = juce::jmax (lo, startCeiling - kCeilingMax);
            hi = juce::jmin (hi, startCeiling - kCeilingMin);
        }

        const float d = juce::jlimit (lo, juce::jmax (lo, hi), value - startGain);
        g = startGain + d;

        if (linkedGesture)
            c = startCeiling - d;
    }
    else
    {
        float lo = kCeilingMin - startCeiling, hi = kCeilingMax - startCeiling;

        if (linkedGesture)
        {
            lo = juce::jmax (lo, startGain - kGainMax);
            hi = juce::jmin (hi, startGain - kGainMin);
        }

        const float d = juce::jlimit (lo, juce::jmax (lo, hi), value - startCeiling);
        c = startCeiling + d;

        if (linkedGesture)
            g = startGain - d;
    }

    if (target == Target::gain || linkedGesture)
        gainParam->setValueNotifyingHost (gainParam->convertTo0to1 (juce::jlimit (kGainMin, kGainMax, g)));

    if (target == Target::ceiling || linkedGesture)
        ceilingParam->setValueNotifyingHost (ceilingParam->convertTo0to1 (juce::jlimit (kCeilingMin, kCeilingMax, c)));

    if (onChange != nullptr)
        onChange();
}

void MaxLevelEditor::dragTo (float targetValue)
{
    if (active)
        apply (targetValue);
}

void MaxLevelEditor::end()
{
    if (! active)
        return;

    active = false;
    wheelGesture = false;

    if (target == Target::gain || linkedGesture)
        gainParam->endChangeGesture();

    if (target == Target::ceiling || linkedGesture)
        ceilingParam->endChangeGesture();
}

void MaxLevelEditor::wheel (Target t, float deltaDb)
{
    if (! wheelGesture || ! active || target != t)
    {
        begin (t);

        if (! active)
            return;

        wheelGesture = true;
        wheelValue = t == Target::gain ? startGain : startCeiling;
    }

    wheelValue = t == Target::gain ? juce::jlimit (kGainMin, kGainMax, wheelValue + deltaDb)
                                   : juce::jlimit (kCeilingMin, kCeilingMax, wheelValue + deltaDb);
    apply (wheelValue);
    startTimer (450);
}

void MaxLevelEditor::timerCallback()
{
    stopTimer();

    if (wheelGesture)
        end();
}

void MaxLevelEditor::setComplete (Target t, float value)
{
    begin (t);

    if (! active)
        return;

    apply (value);
    end();
}

void MaxLevelEditor::resetToDefault (Target t)
{
    auto* param = t == Target::gain ? gainParam : ceilingParam;

    if (param == nullptr)
        return;

    const float def = param->convertFrom0to1 (param->getDefaultValue());

    // A reset puts only that control back (no linked move).
    beginInternal (t, false);

    if (! active)
        return;

    apply (def);
    end();
}

void MaxLevelEditor::showMenu (Target t, juce::Component& comp)
{
    const bool gain = t == Target::gain;
    juce::PopupMenu menu;
    menu.addSectionHeader (gain ? "Maximizer Gain" : "Maximizer Output Level");
    menu.addItem (1, gain ? "Reset to default (0.0 dB)" : "Reset to default (-1.0 dB)");
    menu.addItem (2, "Enter value...");

    ps::ui::showMenu (menu, comp, [this, t, gain, safe = juce::Component::SafePointer<juce::Component> (&comp)] (int result)
    {
        if (result == 1)
        {
            resetToDefault (t);
        }
        else if (result == 2 && safe != nullptr)
        {
            const auto current = juce::String (gain ? getGain() : getCeiling(), 1);
            promptForText (*safe, gain ? "Gain" : "Output Level",
                           gain ? "Gain in dB (0 to 20):" : "Output Level in dB (-20 to 0):", current,
                           [this, t] (juce::String text)
                           {
                               const auto number = text.retainCharacters ("+-0123456789.");

                               if (number.containsAnyOf ("0123456789"))
                                   setComplete (t, (float) number.getDoubleValue());
                           });
        }
    });
}

//==============================================================================
// MaxHistoryDisplay
//==============================================================================
MaxHistoryDisplay::MaxHistoryDisplay (MaxLevelEditor& e, juce::Colour c) : editor (e), accent (c)
{
    noFocus (*this);
    setTooltip ("Level history. Grey: the music after Gain, before limiting. Colour: the output. Amber, from the top: "
                "the gain reduction. Drag the Output line to set Output Level, double-click it to reset.");
}

void MaxHistoryDisplay::setScale (const MaxLevelScale& s) { scale = s; repaint(); }
void MaxHistoryDisplay::setSeconds (float s)               { seconds = juce::jlimit (1.0f, 15.0f, s); repaint(); }

void MaxHistoryDisplay::setLines (float g, float c)
{
    if (! juce::approximatelyEqual (g, gainDb) || ! juce::approximatelyEqual (c, ceilingDb))
    {
        gainDb = g;
        ceilingDb = c;
        repaint();
    }
}

void MaxHistoryDisplay::push (const MaxHistoryEntry& e)
{
    head = (head + 1) % kCapacity;
    ring[(size_t) head] = e;
    count = juce::jmin (kCapacity, count + 1);
}

void MaxHistoryDisplay::clear()
{
    count = 0;
    repaint();
}

void MaxHistoryDisplay::setStatus (const juce::String& text)
{
    if (text != status)
    {
        status = text;
        repaint();
    }
}

void MaxHistoryDisplay::setLegendRight (const juce::String& text)
{
    if (text != legendRight)
    {
        legendRight = text;
        repaint (getLocalBounds().withHeight ((int) scale.top));
    }
}

const MaxHistoryEntry& MaxHistoryDisplay::entryAt (int age) const noexcept
{
    return ring[(size_t) ((head - age + kCapacity * 2) % kCapacity)];
}

juce::Rectangle<float> MaxHistoryDisplay::plotArea() const
{
    return { 0.0f, scale.top, (float) getWidth(), scale.height() };
}

void MaxHistoryDisplay::paint (juce::Graphics& g)
{
    const auto plot = plotArea();

    if (plot.getHeight() < 20.0f || plot.getWidth() < 40.0f)
        return;

    const float pxDb = scale.pixelsPerDb();
    const auto gr = Colours::gainReduction;
    const auto plotBg = Colours::background.darker (0.3f);

    //--------------------------------------------------------------------------
    // Header band: legend left, loudness right.
    {
        auto legend = juce::Rectangle<float> (2.0f, 0.0f, plot.getWidth() - 4.0f, scale.top - 6.0f);
        legend = legend.withTrimmedTop (juce::jmax (0.0f, legend.getHeight() - 18.0f));
        const auto f = font (11.0f, 600);

        auto item = [&] (const juce::String& text, juce::Colour c, bool line)
        {
            const float w = textWidth (f, text) + 18.0f;

            if (w > legend.getWidth())
                return;

            auto r = legend.removeFromLeft (w);
            legend.removeFromLeft (10.0f);
            const auto sw = r.removeFromLeft (12.0f).withSizeKeepingCentre (12.0f, line ? 3.0f : 8.0f);
            g.setColour (c);
            g.fillRoundedRectangle (sw, line ? 1.5f : 2.0f);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r.withTrimmedLeft (6.0f), juce::Justification::centredLeft, false);
        };

        item ("Before limiting", Colours::input.withAlpha (0.7f), false);
        item ("Output", accent, false);
        item ("Gain reduction", gr, true);

        if (legendRight.isNotEmpty() && legend.getWidth() > 60.0f)
        {
            g.setFont (font (11.0f, 600));
            g.setColour (Colours::textSubtle);
            g.drawFittedText (legendRight, legend.toNearestInt(), juce::Justification::centredRight, 1, 0.8f);
        }
    }

    //--------------------------------------------------------------------------
    // Plot background
    g.setGradientFill (juce::ColourGradient::vertical (plotBg, plot.getY(), Colours::background.darker (0.08f), plot.getBottom()));
    g.fillRoundedRectangle (plot, 7.0f);

    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (plot.reduced (1.0f), 6.0f);
        g.reduceClipRegion (clip);

        const float columnsVisible = seconds * (float) kColumnsPerSecond;
        const float colW = plot.getWidth() / columnsVisible;
        const float right = plot.getRight() - 1.0f;
        const float y0 = scale.dbToY (0.0f);
        const float ceilY = scale.dbToY (ceilingDb);

        // Above full scale: a faint warm wash.
        if (y0 > plot.getY() + 1.0f)
        {
            g.setColour (Colours::major.withAlpha (0.045f));
            g.fillRect (juce::Rectangle<float> (plot.getX(), plot.getY(), plot.getWidth(), y0 - plot.getY()));
        }

        // Grid: dB rows below 0 dBFS, one line per second.
        const float stepDb = pxDb * 3.0f >= 16.0f ? 3.0f : 6.0f;

        for (float db = 0.0f; db >= scale.minDb + 0.01f; db -= stepDb)
        {
            const float y = std::round (scale.dbToY (db)) + 0.5f;
            const bool major = std::fmod (-db, 6.0f) < 0.01f;
            g.setColour (Colours::outline.withAlpha (db == 0.0f ? 0.8f : (major ? 0.42f : 0.22f)));
            g.fillRect (juce::Rectangle<float> (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f));
        }

        const float pxPerSecond = colW * (float) kColumnsPerSecond;

        for (int s = 1; (float) s < seconds; ++s)
        {
            const float x = std::round (right - (float) s * pxPerSecond) + 0.5f;
            g.setColour (Colours::outline.withAlpha (0.2f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getY(), 1.0f, plot.getHeight()));
        }

        //----------------------------------------------------------------------
        // Level paths (newest on the right).
        const int n = juce::jmin (count, (int) std::ceil (columnsVisible) + 2);

        if (n >= 2)
        {
            auto levelPath = [&] (auto valueOf, bool closed)
            {
                juce::Path p;
                const float bottom = plot.getBottom() + 2.0f;

                for (int a = 0; a < n; ++a)
                {
                    const float x = right - (float) a * colW;
                    const float y = scale.dbToY (valueOf (entryAt (a)));

                    if (a == 0)
                    {
                        p.startNewSubPath (x, closed ? bottom : y);

                        if (closed)
                            p.lineTo (x, y);
                    }
                    else
                    {
                        p.lineTo (x, y);
                    }
                }

                if (closed)
                {
                    p.lineTo (right - (float) (n - 1) * colW, bottom);
                    p.closeSubPath();
                }

                return p;
            };

            auto unlimitedOf = [] (const MaxHistoryEntry& e) { return e.unlimitedDb; };
            auto outputOf = [] (const MaxHistoryEntry& e) { return e.outputDb; };

            // Before limiting (grey); the part above Output Level (what the limiter takes away) in amber.
            const auto unlimitedFill = levelPath (unlimitedOf, true);
            g.setGradientFill (juce::ColourGradient::vertical (Colours::input.withAlpha (0.26f), plot.getY(),
                                                               Colours::input.withAlpha (0.08f), plot.getBottom()));
            g.fillPath (unlimitedFill);

            if (ceilY > plot.getY())
            {
                juce::Graphics::ScopedSaveState over (g);
                g.reduceClipRegion (juce::Rectangle<float> (plot.getX(), plot.getY(), plot.getWidth(), ceilY - plot.getY()).toNearestInt());
                g.setColour (gr.withAlpha (0.22f));
                g.fillPath (unlimitedFill);
            }

            g.setColour (Colours::input.withAlpha (0.6f));
            g.strokePath (levelPath (unlimitedOf, false), juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));

            // Output (module colour)
            g.setGradientFill (juce::ColourGradient::vertical (accent.withAlpha (0.6f), ceilY,
                                                               accent.withAlpha (0.08f), plot.getBottom()));
            g.fillPath (levelPath (outputOf, true));
            g.setColour (accent.withAlpha (0.95f));
            g.strokePath (levelPath (outputOf, false), juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));

            // Gain reduction: a curtain from the top edge, 1 dB of reduction = 1 dB of the scale.
            bool anyGr = false;

            for (int a = 0; a < n && ! anyGr; ++a)
                anyGr = entryAt (a).grDb < -0.05f;

            if (anyGr)
            {
                juce::Path fill, edge;
                const float top = plot.getY();
                fill.startNewSubPath (right, top - 2.0f);

                for (int a = 0; a < n; ++a)
                {
                    const float x = right - (float) a * colW;
                    const float y = top + juce::jmin (plot.getHeight(), -juce::jmin (0.0f, entryAt (a).grDb) * pxDb);
                    fill.lineTo (x, y);

                    if (a == 0)
                        edge.startNewSubPath (x, y);
                    else
                        edge.lineTo (x, y);
                }

                fill.lineTo (right - (float) (n - 1) * colW, top - 2.0f);
                fill.closeSubPath();

                g.setGradientFill (juce::ColourGradient::vertical (gr.withAlpha (0.5f), top, gr.withAlpha (0.16f),
                                                                   top + juce::jmax (24.0f, 10.0f * pxDb)));
                g.fillPath (fill);
                g.setColour (gr);
                g.strokePath (edge, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
            }
        }

        // Older history fades out on the left.
        const float fadeW = juce::jmin (60.0f, plot.getWidth() * 0.12f);
        g.setGradientFill (juce::ColourGradient::horizontal (plotBg, plot.getX(), plotBg.withAlpha (0.0f), plot.getX() + fadeW));
        g.fillRect (plot.withWidth (fadeW));

        // dB labels (left).
        g.setFont (font (10.0f, 600));
        g.setColour (Colours::textSubtle);

        for (float db = 0.0f; db >= scale.minDb + 2.0f; db -= 6.0f)
        {
            const float y = scale.dbToY (db);

            if (y < plot.getY() + 12.0f || y > plot.getBottom() - 8.0f)
                continue;

            g.drawText (db == 0.0f ? juce::String ("0 dB") : juce::String (juce::roundToInt (db)),
                        juce::Rectangle<float> (plot.getX() + 6.0f, y - 12.0f, 40.0f, 11.0f), juce::Justification::bottomLeft, false);
        }

        //----------------------------------------------------------------------
        // Output Level line with its label.
        const bool hot = hover || dragging;
        g.setColour (accent.withAlpha (hot ? 0.35f : 0.2f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), ceilY - 2.5f, plot.getWidth(), 5.0f));
        g.setColour (hot ? accent.brighter (0.3f) : accent);
        g.fillRect (juce::Rectangle<float> (plot.getX(), ceilY - (hot ? 1.0f : 0.75f), plot.getWidth(), hot ? 2.0f : 1.5f));

        {
            const auto f = font (11.0f, 700);
            const auto text = "Output " + fmt::db (ceilingDb);
            const float w = textWidth (f, text) + 14.0f, h = 17.0f;
            float ly = ceilY - h - 4.0f;

            if (ly < plot.getY() + 2.0f)
                ly = ceilY + 4.0f;

            const auto r = juce::Rectangle<float> (plot.getRight() - w - 8.0f, ly, w, h);
            g.setColour (plotBg.withAlpha (0.85f));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (accent.withAlpha (hot ? 0.9f : 0.5f));
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
            g.setFont (f);
            g.setColour (hot ? accent.brighter (0.3f) : accent);
            g.drawText (text, r, juce::Justification::centred, false);
        }

        if (count == 0)
        {
            g.setFont (font (12.5f, 600));
            g.setColour (Colours::textSubtle);
            g.drawText ("Play audio to see the levels", plot.withTrimmedTop (plot.getHeight() * 0.3f), juce::Justification::centred, false);
        }

        if (status.isNotEmpty())
        {
            const auto f = font (12.5f, 700);
            const float w = juce::jmin (plot.getWidth() - 20.0f, textWidth (f, status) + 40.0f);
            const auto pill = juce::Rectangle<float> (plot.getCentreX() - w * 0.5f, plot.getY() + plot.getHeight() * 0.42f, w, 30.0f);
            g.setColour (Colours::panel.withAlpha (0.94f));
            g.fillRoundedRectangle (pill, 15.0f);
            g.setColour (accent.withAlpha (0.8f));
            g.drawRoundedRectangle (pill.reduced (0.5f), 15.0f, 1.0f);
            g.setColour (accent);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ pill.getX() + 18.0f, pill.getCentreY() }));
            g.setFont (f);
            g.setColour (Colours::text);
            g.drawFittedText (status, pill.withTrimmedLeft (28.0f).withTrimmedRight (12.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
        }
    }

    g.setColour (Colours::outline.withAlpha (0.65f));
    g.drawRoundedRectangle (plot.reduced (0.5f), 7.0f, 1.0f);

    //--------------------------------------------------------------------------
    // Time ruler under the plot.
    const float footerH = (float) getHeight() - plot.getBottom();

    if (footerH >= 12.0f)
    {
        const float pxPerSecond = plot.getWidth() / seconds;
        g.setFont (font (10.0f, 600));

        for (int s = 0; (float) s < seconds; ++s)
        {
            const float x = plot.getRight() - 1.0f - (float) s * pxPerSecond;
            const auto text = s == 0 ? juce::String ("now") : juce::String (-s) + " s";
            const auto r = juce::Rectangle<float> (x - 40.0f, plot.getBottom() + 3.0f, 40.0f, juce::jmin (14.0f, footerH - 3.0f));
            g.setColour (Colours::textSubtle);
            g.drawText (text, r, juce::Justification::centredRight, false);
            g.setColour (Colours::outline.withAlpha (0.6f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getBottom() + 1.0f, 1.0f, 3.0f));
        }
    }
}

bool MaxHistoryDisplay::isOnCeilingLine (juce::Point<float> p) const
{
    return plotArea().expanded (0.0f, 4.0f).contains (p) && std::abs (p.y - scale.dbToY (ceilingDb)) <= 7.0f;
}

void MaxHistoryDisplay::updateHover (bool h)
{
    if (h != hover)
    {
        hover = h;
        setMouseCursor (h ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void MaxHistoryDisplay::mouseMove (const juce::MouseEvent& e) { updateHover (isOnCeilingLine (e.position)); }

void MaxHistoryDisplay::mouseExit (const juce::MouseEvent&)
{
    if (! dragging)
        updateHover (false);
}

void MaxHistoryDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (! isOnCeilingLine (e.position))
        return;

    if (e.mods.isPopupMenu())
    {
        editor.showMenu (MaxLevelEditor::Target::ceiling, *this);
        return;
    }

    if (! isLeftClick (e))
        return;

    dragging = true;
    dragStartY = e.position.y;
    dragStartValue = ceilingDb;
    editor.begin (MaxLevelEditor::Target::ceiling);
    repaint();
}

void MaxHistoryDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || ! isLeftClick (e))
        return;

    const bool fine = isFine (e);
    const float ddb = (e.position.y - dragStartY) / juce::jmax (0.5f, scale.pixelsPerDb()) * (fine ? 0.25f : 1.0f);
    editor.dragTo (quantise (dragStartValue - ddb, fine));
}

void MaxHistoryDisplay::mouseUp (const juce::MouseEvent& e)
{
    if (! dragging)
        return;

    dragging = false;
    editor.end();
    updateHover (isOnCeilingLine (e.position));
    repaint();
}

void MaxHistoryDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && isOnCeilingLine (e.position))
        editor.resetToDefault (MaxLevelEditor::Target::ceiling);
}

void MaxHistoryDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! dragging && isOnCeilingLine (e.position))
        editor.wheel (MaxLevelEditor::Target::ceiling, wheelDelta (w) * (isFine (e) ? 0.8f : 4.0f));
}

//==============================================================================
// MaxMeterColumn
//==============================================================================
MaxMeterColumn::MaxMeterColumn (Kind k, MaxLevelEditor& e, juce::Colour c) : kind (k), editor (e), accent (c)
{
    noFocus (*this);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setTitle (k == Kind::gain ? "Gain" : "Output Level");
    setTooltip (k == Kind::gain
                    ? "Gain: how hard the music is pushed into the limiter. Drag up or down (Ctrl: fine), double-click "
                      "to reset, right-click to type a value. The meter shows the module input; the handle marks the "
                      "threshold (Output Level minus Gain): everything above it is limited."
                    : "Output Level: the highest level the output may reach. Drag up or down (Ctrl: fine), "
                      "double-click to reset, right-click to type a value. The meter shows the module output.");
}

void MaxMeterColumn::setScale (const MaxLevelScale& s) { scale = s; dirty = true; repaint(); }
void MaxMeterColumn::setHeaderHeight (int h)           { headerHeight = h; repaint(); }

MaxLevelEditor::Target MaxMeterColumn::target() const noexcept
{
    return kind == Kind::gain ? MaxLevelEditor::Target::gain : MaxLevelEditor::Target::ceiling;
}

float MaxMeterColumn::handleDb() const noexcept
{
    // The limiter starts working where input + Gain reaches Output Level: threshold = Output Level - Gain.
    return kind == Kind::gain ? ceilingDb - gainDb : ceilingDb;
}

void MaxMeterColumn::setLevels (float dbL, float dbR)
{
    const float in[2] { dbL, dbR };

    for (int c = 0; c < 2; ++c)
    {
        const float before = shown[c], beforeHold = hold[c];
        const float v = std::isfinite (in[c]) ? in[c] : kSilenceDb;
        shown[c] = v >= shown[c] ? v : juce::jmax (v, shown[c] - 0.9f);   // ~27 dB/s fall at 30 Hz

        if (v >= hold[c])
        {
            hold[c] = v;
            holdTicks[c] = 45;
        }
        else if (--holdTicks[c] <= 0)
        {
            hold[c] = juce::jmax (v, hold[c] - 1.2f);
        }

        const float floor = scale.minDb - 1.0f;

        if (! juce::approximatelyEqual (juce::jmax (floor, before), juce::jmax (floor, shown[c]))
            || ! juce::approximatelyEqual (juce::jmax (floor, beforeHold), juce::jmax (floor, hold[c])))
            dirty = true;
    }

    if (dirty)
    {
        dirty = false;
        repaint();
    }
}

void MaxMeterColumn::setHandle (float g, float c)
{
    if (! juce::approximatelyEqual (g, gainDb) || ! juce::approximatelyEqual (c, ceilingDb))
    {
        gainDb = g;
        ceilingDb = c;
        repaint();
    }
}

void MaxMeterColumn::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const bool hot = isMouseOverOrDragging() || dragging || (editor.isActive() && editor.getActiveTarget() == target());
    const bool isGain = kind == Kind::gain;

    // Header: caption + big value.
    const float hh = (float) headerHeight;
    drawCaption (g, { 0.0f, 0.0f, b.getWidth(), 14.0f }, isGain ? "GAIN" : "OUTPUT", hot ? Colours::text : Colours::textDim);
    const auto valueColour = isGain ? Colours::text : accent;
    drawBigValue (g, { 0.0f, 14.0f, b.getWidth(), hh - 18.0f }, numberText (isGain ? gainDb : ceilingDb), "dB",
                  juce::jlimit (17.0f, 30.0f, hh * 0.52f), hot ? valueColour.brighter (0.2f) : valueColour);

    // Meter: two bars on the shared scale.
    const auto plot = juce::Rectangle<float> (0.0f, scale.top, b.getWidth(), scale.height());

    if (plot.getHeight() < 10.0f)
        return;

    const float barW = juce::jlimit (6.0f, 12.0f, b.getWidth() * 0.13f);
    const float gap = 3.0f;
    const float x0 = plot.getCentreX() - (barW * 2.0f + gap) * 0.5f + (isGain ? -4.0f : 4.0f);
    const float handleY = scale.dbToY (handleDb());

    // Faint ticks every 6 dB on the outer side.
    g.setColour (Colours::outline.withAlpha (0.8f));

    for (float db = 0.0f; db >= scale.minDb; db -= 6.0f)
    {
        const float y = std::round (scale.dbToY (db));
        const float tx = isGain ? x0 - 7.0f : x0 + barW * 2.0f + gap + 3.0f;
        g.fillRect (juce::Rectangle<float> (tx, y, 4.0f, 1.0f));
    }

    for (int c = 0; c < 2; ++c)
    {
        const auto bar = juce::Rectangle<float> (x0 + (float) c * (barW + gap), plot.getY(), barW, plot.getHeight());
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRoundedRectangle (bar, 2.5f);

        const float level = shown[c];

        if (level > scale.minDb)
        {
            const float y = scale.dbToY (level);
            auto fill = bar.withTop (y);

            if (isGain)
            {
                // Module input: grey below the threshold, amber above it (the part that gets limited).
                g.setGradientFill (juce::ColourGradient::vertical (Colours::input.withAlpha (0.95f), plot.getY(),
                                                                   Colours::input.withAlpha (0.45f), plot.getBottom()));
                g.fillRoundedRectangle (fill.withTop (juce::jmax (y, handleY)), 2.0f);

                if (y < handleY)
                {
                    g.setColour (Colours::gainReduction.withAlpha (0.95f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (bar.getX(), y, barW, handleY - y), 2.0f);
                }
            }
            else
            {
                g.setGradientFill (juce::ColourGradient::vertical (accent.brighter (0.15f), plot.getY(),
                                                                   accent.withAlpha (0.45f), plot.getBottom()));
                g.fillRoundedRectangle (fill, 2.0f);
            }
        }

        if (hold[c] > scale.minDb)
        {
            const float y = scale.dbToY (hold[c]);
            g.setColour ((isGain ? (y < handleY ? Colours::gainReduction : Colours::text) : accent.brighter (0.35f)).withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (bar.getX(), y - 1.0f, barW, 2.0f));
        }
    }

    // Handle: a line across the column and a grip pill on the side that faces the history display.
    const auto handleColour = isGain ? Colours::text : accent;
    g.setColour (handleColour.withAlpha (hot ? 0.95f : 0.7f));
    g.fillRect (juce::Rectangle<float> (0.0f, handleY - 0.75f, b.getWidth(), 1.5f));
    const float pillW = juce::jlimit (18.0f, 26.0f, b.getWidth() * 0.3f);
    const auto pill = juce::Rectangle<float> (isGain ? b.getWidth() - pillW : 0.0f, handleY - 7.0f, pillW, 14.0f);
    drawHandle (g, pill, handleColour, hot);
}

void MaxMeterColumn::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        editor.showMenu (target(), *this);
        return;
    }

    if (! isLeftClick (e))
        return;

    dragging = true;
    dragStartY = e.position.y;
    dragStartValue = kind == Kind::gain ? gainDb : ceilingDb;
    editor.begin (target());
    repaint();
}

void MaxMeterColumn::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || ! isLeftClick (e))
        return;

    const bool fine = isFine (e);
    const float ddb = (e.position.y - dragStartY) / juce::jmax (0.5f, scale.pixelsPerDb()) * (fine ? 0.25f : 1.0f);
    editor.dragTo (quantise (kind == Kind::gain ? dragStartValue + ddb : dragStartValue - ddb, fine));
}

void MaxMeterColumn::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    editor.end();
    repaint();
}

void MaxMeterColumn::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isLeftClick (e))
        editor.resetToDefault (target());
}

void MaxMeterColumn::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! dragging)
        editor.wheel (target(), wheelDelta (w) * (isFine (e) ? 0.8f : 4.0f));
}

//==============================================================================
// MaxGrColumn
//==============================================================================
MaxGrColumn::MaxGrColumn()
{
    noFocus (*this);
    setTitle ("Gain reduction");
    setTooltip ("Gain reduction of the limiter, left and right. The number is the largest reduction so far: click to "
                "reset it.");
}

void MaxGrColumn::setScale (const MaxLevelScale& s) { scale = s; repaint(); }
void MaxGrColumn::setHeaderHeight (int h)           { headerHeight = h; repaint(); }

void MaxGrColumn::setGainReduction (float grL, float grR, float grPeakThisTick)
{
    const float in[2] { grL, grR };
    bool changed = false;

    for (int c = 0; c < 2; ++c)
    {
        const float v = std::isfinite (in[c]) ? juce::jmin (0.0f, in[c]) : 0.0f;
        const float before = shown[c];
        shown[c] = v <= shown[c] ? v : juce::jmin (v, shown[c] + 0.7f);   // instant attack, ~21 dB/s release
        changed = changed || ! juce::approximatelyEqual (before, shown[c]);
    }

    if (std::isfinite (grPeakThisTick) && grPeakThisTick < heldPeak - 0.005f)
    {
        heldPeak = grPeakThisTick;
        changed = true;
    }

    if (changed)
        repaint();
}

void MaxGrColumn::resetPeak()
{
    heldPeak = 0.0f;
    repaint();
}

void MaxGrColumn::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const float hh = (float) headerHeight;
    const auto gr = Colours::gainReduction;
    const bool active = heldPeak < -0.05f;

    drawCaption (g, { 0.0f, 0.0f, b.getWidth(), 14.0f }, "GR", isMouseOver() ? Colours::text : Colours::textDim);
    drawBigValue (g, { 0.0f, 14.0f, b.getWidth(), hh - 18.0f }, active ? juce::String (heldPeak, 1) : juce::String ("0.0"), {},
                  juce::jlimit (14.0f, 19.0f, hh * 0.36f), active ? gr : Colours::textFaint.brighter (0.3f));

    const auto plot = juce::Rectangle<float> (0.0f, scale.top, b.getWidth(), scale.height());

    if (plot.getHeight() < 10.0f)
        return;

    const float y0 = plot.getY();
    const float barW = juce::jlimit (6.0f, 11.0f, b.getWidth() * 0.22f);
    auto grToY = [&] (float db) { return y0 + juce::jmin (plot.getHeight(), -juce::jmin (0.0f, db) * scale.pixelsPerDb()); };
    const float gap = 3.0f;
    const float x0 = plot.getCentreX() - (barW * 2.0f + gap) * 0.5f;

    for (int c = 0; c < 2; ++c)
    {
        const auto bar = juce::Rectangle<float> (x0 + (float) c * (barW + gap), y0, barW, plot.getBottom() - y0);
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRoundedRectangle (bar, 2.5f);

        if (shown[c] < -0.02f)
        {
            const float y = grToY (shown[c]);
            g.setGradientFill (juce::ColourGradient::vertical (gr.brighter (0.1f), y0, gr.withAlpha (0.7f), juce::jmax (y, y0 + 1.0f)));
            g.fillRoundedRectangle (bar.withBottom (juce::jmax (y, y0 + 1.0f)), 2.0f);
        }

        if (heldPeak < -0.05f)
        {
            const float y = grToY (heldPeak);
            g.setColour (gr.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (bar.getX(), y - 0.5f, barW, 1.0f));
        }
    }
}

void MaxGrColumn::mouseUp (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && getLocalBounds().contains (e.getPosition()))
        resetPeak();
}

//==============================================================================
// MaxChoiceGrid
//==============================================================================
MaxChoiceGrid::MaxChoiceGrid (PureSheenProcessor& p, const juce::String& id, juce::StringArray l, int cols, Style s,
                              juce::Colour c)
    : processor (p), paramId (id), labels (std::move (l)), columns (juce::jmax (1, cols)), style (s), accent (c)
{
    noFocus (*this);

    if (const auto* param = p.getParameter (id))
        setTitle (param->getName (64));

    shownIndex = getSelectedIndex();
}

void MaxChoiceGrid::setColumns (int c)
{
    c = juce::jlimit (1, juce::jmax (1, labels.size()), c);

    if (c != columns)
    {
        columns = c;
        repaint();
    }
}

void MaxChoiceGrid::setItemTooltips (juce::StringArray tips) { itemTips = std::move (tips); }

int MaxChoiceGrid::getSelectedIndex() const
{
    return juce::jlimit (0, juce::jmax (0, labels.size() - 1), juce::roundToInt (processor.getParameterValue (paramId)));
}

void MaxChoiceGrid::refreshValue()
{
    const int index = getSelectedIndex();

    if (index != shownIndex)
    {
        shownIndex = index;
        repaint();
    }
}

juce::Rectangle<float> MaxChoiceGrid::cell (int index) const
{
    const int rows = (labels.size() + columns - 1) / columns;
    const float gap = style == Style::list ? 3.0f : 4.0f;
    const auto b = getLocalBounds().toFloat();
    const float w = (b.getWidth() - gap * (float) (columns - 1)) / (float) columns;
    const float h = (b.getHeight() - gap * (float) (rows - 1)) / (float) juce::jmax (1, rows);
    const int col = index % columns, row = index / columns;
    return { (w + gap) * (float) col, (h + gap) * (float) row, w, h };
}

int MaxChoiceGrid::indexAt (juce::Point<float> p) const
{
    for (int i = 0; i < labels.size(); ++i)
        if (cell (i).contains (p))
            return i;

    return -1;
}

void MaxChoiceGrid::paint (juce::Graphics& g)
{
    const int selected = getSelectedIndex();

    for (int i = 0; i < labels.size(); ++i)
    {
        const auto r = cell (i).reduced (0.5f);
        const bool sel = i == selected, hov = i == hoverIndex;

        if (style == Style::list)
        {
            if (sel)
            {
                g.setGradientFill (juce::ColourGradient::horizontal (blend (Colours::panelRaised, accent, 0.30f), r.getX(),
                                                                     blend (Colours::panelRaised, accent, 0.12f), r.getRight()));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (accent.withAlpha (0.65f));
                g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
                g.setColour (accent);
                g.fillRoundedRectangle (juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + 5.0f, 3.0f, r.getHeight() - 10.0f), 1.5f);
            }
            else if (hov)
            {
                g.setColour (Colours::panelHover.withAlpha (0.8f));
                g.fillRoundedRectangle (r, 5.0f);
            }

            g.setFont (font (juce::jlimit (11.5f, 13.0f, r.getHeight() * 0.5f), sel ? 700 : 600));
            g.setColour (sel ? Colours::text : (hov ? Colours::text : Colours::textDim));
            g.drawFittedText (labels[i], r.withTrimmedLeft (14.0f).withTrimmedRight (4.0f).toNearestInt(),
                              juce::Justification::centredLeft, 1, 0.8f);
        }
        else
        {
            juce::Colour fill = Colours::panelRaised, outline = Colours::outline;

            if (sel)
            {
                fill = blend (Colours::panelRaised, accent, 0.25f);
                outline = accent.withAlpha (0.75f);
            }
            else if (hov)
            {
                fill = Colours::panelHover;
                outline = Colours::outline.brighter (0.2f);
            }

            drawChip (g, r, juce::jmin (6.0f, r.getHeight() * 0.5f), fill, outline);
            g.setFont (font (juce::jlimit (10.5f, 12.0f, r.getHeight() * 0.48f), sel ? 700 : 600));
            g.setColour (sel ? accent.brighter (0.35f) : (hov ? Colours::text : Colours::textDim.brighter (0.1f)));
            g.drawFittedText (labels[i], r.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.75f);
        }
    }
}

void MaxChoiceGrid::mouseMove (const juce::MouseEvent& e)
{
    const int index = indexAt (e.position);

    if (index != hoverIndex)
    {
        hoverIndex = index;
        setTooltip (index >= 0 && index < itemTips.size() ? itemTips[index] : juce::String());
        repaint();
    }
}

void MaxChoiceGrid::mouseExit (const juce::MouseEvent&)
{
    if (hoverIndex != -1)
    {
        hoverIndex = -1;
        repaint();
    }
}

void MaxChoiceGrid::mouseUp (const juce::MouseEvent& e)
{
    if (! isLeftClick (e))
        return;

    const int index = indexAt (e.position);

    if (index >= 0 && index != getSelectedIndex())
    {
        processor.setParameterValue (paramId, (float) index);
        refreshValue();
    }
}

//==============================================================================
// MaxSwitch
//==============================================================================
MaxSwitch::MaxSwitch (PureSheenProcessor& p, const juce::String& id, juce::Colour c) : processor (p), paramId (id), accent (c)
{
    noFocus (*this);

    if (const auto* param = p.getParameter (id))
        setTitle (param->getName (64));

    shownOn = isOn();
}

bool MaxSwitch::isOn() const { return processor.getParameterValue (paramId) >= 0.5f; }

void MaxSwitch::refreshValue()
{
    if (isOn() != shownOn)
    {
        shownOn = isOn();
        repaint();
    }
}

void MaxSwitch::paint (juce::Graphics& g)
{
    const bool on = isOn();
    const bool hov = isMouseOver();
    const auto b = getLocalBounds().toFloat();
    const float h = juce::jmin (b.getHeight(), 16.0f);
    const auto track = juce::Rectangle<float> (h * 1.85f, h).withCentre (b.getCentre());

    if (on)
    {
        g.setGradientFill (juce::ColourGradient::vertical (accent.brighter (0.1f), track.getY(), accent.darker (0.15f), track.getBottom()));
        g.fillRoundedRectangle (track, h * 0.5f);
    }
    else
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (track, h * 0.5f);
        g.setColour (Colours::outline.brighter (hov ? 0.35f : 0.15f));
        g.drawRoundedRectangle (track.reduced (0.5f), h * 0.5f, 1.0f);
    }

    const float d = h - 4.0f;
    const auto knob = juce::Rectangle<float> (d, d).withCentre ({ on ? track.getRight() - h * 0.5f : track.getX() + h * 0.5f,
                                                                  track.getCentreY() });
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.fillEllipse (knob.translated (0.0f, 1.0f));
    g.setColour (on ? Colours::background.brighter (0.05f) : (hov ? Colours::text : Colours::textDim));
    g.fillEllipse (knob);
}

void MaxSwitch::mouseUp (const juce::MouseEvent& e)
{
    if (! isLeftClick (e) || ! getLocalBounds().contains (e.getPosition()))
        return;

    processor.setParameterValue (paramId, isOn() ? 0.0f : 1.0f);
    refreshValue();
}

//==============================================================================
// MaxLearnButton
//==============================================================================
MaxLearnButton::MaxLearnButton (juce::Colour c) : accent (c)
{
    noFocus (*this);
    setTitle ("Learn Input Gain");
    setTooltip ("Learn Input Gain: play the loudest part of the song. It listens for 5 seconds, then sets Gain so the "
                "output reaches the target loudness.");
}

void MaxLearnButton::setState (State s, float p)
{
    p = juce::jlimit (0.0f, 1.0f, p);

    if (s != state || std::abs (p - progress) > 0.002f)
    {
        state = s;
        progress = p;
        repaint();
    }
}

void MaxLearnButton::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const float labelH = 16.0f;
    const float d = juce::jmax (16.0f, juce::jmin (b.getWidth() - 4.0f, b.getHeight() - labelH - 4.0f, 64.0f));
    const auto circle = juce::Rectangle<float> (d, d).withCentre ({ b.getCentreX(), b.getY() + 2.0f + d * 0.5f
                                                                     + juce::jmax (0.0f, (b.getHeight() - labelH - 4.0f - d) * 0.5f) });
    const bool hov = isMouseOver() && state != State::learning;
    const float ringW = juce::jlimit (2.5f, 4.0f, d * 0.07f);
    const auto centre = circle.getCentre();
    const float r = d * 0.5f - ringW * 0.5f - 1.0f;

    // Body
    const auto body = circle.reduced (ringW + 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.fillEllipse (body.translated (0.0f, 1.5f));
    g.setGradientFill (juce::ColourGradient::vertical (hov ? Colours::panelHover.brighter (0.12f) : Colours::panelHover, body.getY(),
                                                       Colours::panelRaised.darker (0.12f), body.getBottom()));
    g.fillEllipse (body);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    // Ring track + progress
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, r, r, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.strokePath (track, juce::PathStrokeType (ringW + 1.5f));
    g.setColour (state == State::idle ? accent.withAlpha (hov ? 0.85f : 0.5f) : Colours::outline);
    g.strokePath (track, juce::PathStrokeType (ringW));

    if (state != State::idle)
    {
        const float end = state == State::done ? juce::MathConstants<float>::twoPi : juce::MathConstants<float>::twoPi * progress;

        if (end > 0.01f)
        {
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, r, r, 0.0f, 0.0f, end, true);
            g.setColour (accent.withAlpha (0.25f));
            g.strokePath (arc, juce::PathStrokeType (ringW + 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (accent);
            g.strokePath (arc, juce::PathStrokeType (ringW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // Centre: target icon / percent / check mark.
    const float ir = body.getWidth() * 0.5f;

    if (state == State::learning)
    {
        g.setFont (font (juce::jlimit (10.0f, 15.0f, ir * 0.62f), 700));
        g.setColour (Colours::text);
        g.drawText (juce::String (juce::roundToInt (progress * 100.0f)) + "%", body, juce::Justification::centred, false);
    }
    else if (state == State::done)
    {
        juce::Path tick;
        tick.startNewSubPath (centre.x - ir * 0.38f, centre.y + ir * 0.02f);
        tick.lineTo (centre.x - ir * 0.08f, centre.y + ir * 0.32f);
        tick.lineTo (centre.x + ir * 0.42f, centre.y - ir * 0.3f);
        g.setColour (accent);
        g.strokePath (tick, juce::PathStrokeType (juce::jmax (2.0f, ir * 0.14f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        const auto c = hov ? accent.brighter (0.3f) : accent;
        g.setColour (c);
        g.drawEllipse (juce::Rectangle<float> (ir * 0.95f, ir * 0.95f).withCentre (centre), juce::jmax (1.5f, ir * 0.09f));
        g.fillEllipse (juce::Rectangle<float> (ir * 0.32f, ir * 0.32f).withCentre (centre));

        for (int i = 0; i < 4; ++i)
        {
            const float a = juce::MathConstants<float>::halfPi * (float) i;
            const auto p0 = centre.getPointOnCircumference (ir * 0.5f, a);
            const auto p1 = centre.getPointOnCircumference (ir * 0.72f, a);
            g.drawLine ({ p0, p1 }, juce::jmax (1.5f, ir * 0.09f));
        }
    }

    // Label
    const auto labelArea = juce::Rectangle<float> (b.getX(), circle.getBottom() + 2.0f, b.getWidth(), labelH);
    g.setFont (font (11.5f, 600));
    g.setColour (state == State::idle ? (hov ? Colours::text : Colours::textDim) : accent.brighter (0.2f));
    g.drawFittedText (state == State::learning ? juce::String ("Listening") : (state == State::done ? juce::String ("Done")
                                                                                                    : juce::String ("Learn")),
                      labelArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);
}

void MaxLearnButton::mouseUp (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && getLocalBounds().contains (e.getPosition()) && state != State::learning && onClick != nullptr)
        onClick();
}

} // namespace ps::ui
