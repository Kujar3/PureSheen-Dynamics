#include "ui/ClassicLimiterViews.h"

#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"

#include <cmath>

// Classic Limiter panel building blocks (mod-clim). See ClassicLimiterViews.h.
namespace ps::ui::clim
{
namespace
{
constexpr float kLevelMin = -12.0f, kLevelMax = 0.0f;   // clim.thr / clim.ceiling range (Params.h)
constexpr float kPixelsForFullRange = 220.0f;           // as ParamKnob
constexpr float kMeterReleaseDbPerTick = 0.6f;          // meter fall: 18 dB/s at 30 Hz

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

const char* targetId (LevelLink::Target t) { return t == LevelLink::Target::threshold ? "clim.thr" : "clim.ceiling"; }
} // namespace

//==============================================================================
// LevelLink
//==============================================================================
LevelLink::LevelLink (PureSheenProcessor& p) : processor (p)
{
    thrParam = p.getParameter ("clim.thr");
    ceilParam = p.getParameter ("clim.ceiling");
}

LevelLink::~LevelLink()
{
    stopTimer();
    end();
}

bool LevelLink::isLinked() const { return processor.getParameterValue ("clim.link") >= 0.5f; }
float LevelLink::get (Target t) const { return processor.getParameterValue (targetId (t)); }

juce::RangedAudioParameter* LevelLink::param (Target t) const noexcept
{
    return t == Target::threshold ? thrParam : ceilParam;
}

void LevelLink::begin (Target t)
{
    if (active)
        end();

    stopTimer();
    wheelGesture = false;

    if (thrParam == nullptr || ceilParam == nullptr)
        return;

    target = t;
    startThr = get (Target::threshold);
    startCeil = get (Target::ceiling);
    linkedGesture = isLinked();
    active = true;

    if (target == Target::threshold || linkedGesture)
        thrParam->beginChangeGesture();

    if (target == Target::ceiling || linkedGesture)
        ceilParam->beginChangeGesture();
}

void LevelLink::apply (float value)
{
    const bool thr = target == Target::threshold;
    const float start = thr ? startThr : startCeil;
    const float other = thr ? startCeil : startThr;
    float lo = kLevelMin - start, hi = kLevelMax - start;

    // With Link on, Threshold and Ceiling move by the same amount; the move stops where either reaches its limit.
    if (linkedGesture)
    {
        lo = juce::jmax (lo, kLevelMin - other);
        hi = juce::jmin (hi, kLevelMax - other);
    }

    const float d = juce::jlimit (lo, juce::jmax (lo, hi), value - start);
    auto set = [] (juce::RangedAudioParameter* p, float v)
    {
        const float norm = p->convertTo0to1 (juce::jlimit (kLevelMin, kLevelMax, v));

        if (std::abs (norm - p->getValue()) > 1.0e-7f)
            p->setValueNotifyingHost (norm);
    };

    set (param (target), start + d);

    if (linkedGesture)
        set (param (thr ? Target::ceiling : Target::threshold), other + d);
}

void LevelLink::dragTo (float value)
{
    if (active)
        apply (value);
}

void LevelLink::end()
{
    stopTimer();
    if (! active)
        return;

    active = false;
    wheelGesture = false;

    if (target == Target::threshold || linkedGesture)
        thrParam->endChangeGesture();

    if (target == Target::ceiling || linkedGesture)
        ceilParam->endChangeGesture();
}

void LevelLink::wheel (Target t, float deltaDb)
{
    if (! wheelGesture || ! active || target != t)
    {
        begin (t);

        if (! active)
            return;

        wheelGesture = true;
        wheelValue = t == Target::threshold ? startThr : startCeil;
    }

    wheelValue = juce::jlimit (kLevelMin, kLevelMax, wheelValue + deltaDb);
    apply (wheelValue);
    wheelValue = get (target); // reversing at either linked limit responds immediately
    startTimer (450);
}

void LevelLink::timerCallback()
{
    stopTimer();

    if (wheelGesture)
        end();
}

void LevelLink::setComplete (Target t, float value)
{
    begin (t);

    if (! active)
        return;

    apply (value);
    end();
}

//==============================================================================
// LevelKnob
//==============================================================================
LevelKnob::LevelKnob (PureSheenProcessor& p, LevelLink& l, LevelLink::Target t, const juce::String& caption)
    : ParamKnob (p, targetId (t), caption), processor (p), link (l), target (t)
{
}

void LevelKnob::mouseDown (const juce::MouseEvent& e)
{
    ownDrag = false;

    // A wheel burst on the other knob may still own the shared gesture.
    // End it before this drag chooses its target, even if Link was just switched off.
    if (isLeftClick (e))
        link.end();

    if (! link.isLinked())
    {
        ParamKnob::mouseDown (e);
        return;
    }

    if (e.mods.isPopupMenu())
    {
        showLinkedMenu();
        return;
    }

    if (! isLeftClick (e))
        return;

    ownDrag = true;
    dragValue = link.get (target);
    lastPos = e.position;
}

void LevelKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! ownDrag)
    {
        ParamKnob::mouseDrag (e);
        return;
    }

    if (! isLeftClick (e))
        return;

    const auto d = e.position - lastPos;
    lastPos = e.position;
    const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
    dragValue = juce::jlimit (kLevelMin, kLevelMax,
                              dragValue - d.y / kPixelsForFullRange * (kLevelMax - kLevelMin) * (fine ? 0.1f : 1.0f));

    if (! link.isActive())
        link.begin (target);

    link.dragTo (dragValue);
    dragValue = link.get (target);
    repaint();
}

void LevelKnob::mouseUp (const juce::MouseEvent& e)
{
    if (! ownDrag)
    {
        ParamKnob::mouseUp (e);
        return;
    }

    ownDrag = false;
    link.end();
    repaint();
}

void LevelKnob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! link.isLinked())
    {
        ParamKnob::mouseDoubleClick (e);
        return;
    }

    if (isLeftClick (e))
    {
        const auto* spec = &ps::paramSpecs()[(size_t) ps::paramIndex (targetId (target))];
        link.setComplete (target, spec->defaultValue);
    }
}

void LevelKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (ownDrag)
        return;

    if (! link.isLinked())
    {
        ParamKnob::mouseWheelMove (e, w);
        return;
    }

    const float delta = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);

    if (std::abs (delta) < 1.0e-4f)
        return;

    const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();
    link.wheel (target, delta * (fine ? 0.01f : 0.06f) * (kLevelMax - kLevelMin));
}

void LevelKnob::showLinkedMenu()
{
    const bool thr = target == LevelLink::Target::threshold;
    const auto* spec = &ps::paramSpecs()[(size_t) ps::paramIndex (targetId (target))];
    const float def = spec->defaultValue;
    juce::PopupMenu menu;
    menu.addSectionHeader (juce::String (thr ? "Threshold" : "Ceiling") + " (linked)");
    menu.addItem (1, "Reset to default (" + fmt::db (def, 1, false) + ")");
    menu.addItem (2, "Enter value...");

    ps::ui::showMenu (menu, *this, [this, thr, def, safe = juce::Component::SafePointer<juce::Component> (this)] (int result)
    {
        if (safe == nullptr)
            return;

        if (result == 1)
        {
            link.setComplete (target, def);
        }
        else if (result == 2)
        {
            promptForText (*safe, thr ? "Threshold" : "Ceiling",
                           thr ? "Threshold in dB (-12 to 0), Ceiling follows:" : "Ceiling in dB (-12 to 0), Threshold follows:",
                           juce::String (link.get (target), 1),
                           [this, safe] (juce::String text)
                           {
                               const auto number = text.retainCharacters ("+-0123456789.");

                               if (safe != nullptr && number.containsAnyOf ("0123456789"))
                                   link.setComplete (target, (float) number.getDoubleValue());
                           });
        }
    });
}

//==============================================================================
// HistoryView
//==============================================================================
HistoryView::HistoryView (juce::Colour c) : accent (c)
{
    noFocus (*this);
    setTooltip ("Gain reduction over the last seconds, 0 dB at the top. Colour: the slow stage's smooth limiting. Amber "
                "below it: what the fast peak stage takes on top to keep the peaks under the ceiling. The two meters on "
                "the right show both stages now.");
}

void HistoryView::push (float muDb, float totalDb)
{
    auto& col = ring[(size_t) head];
    col.mu = juce::jlimit (-60.0f, 0.0f, std::isfinite (muDb) ? muDb : 0.0f);
    col.total = juce::jlimit (-60.0f, col.mu, std::isfinite (totalDb) ? totalDb : col.mu);
    head = (head + 1) % kCapacity;
    count = juce::jmin (count + 1, kCapacity);

    // Range: 12 dB, 24 dB while anything visible goes deeper than 11 dB.
    float deepest = 0.0f;
    const int visible = juce::jmin (count, (int) (seconds * (float) kColumnsPerSecond) + 2);

    for (int a = 0; a < visible; ++a)
        deepest = juce::jmin (deepest, columnAt (a).total);

    rangeDb = deepest < -11.0f ? 24.0f : 12.0f;
    repaint();
}

void HistoryView::clear()
{
    count = 0;
    head = 0;
    rangeDb = 12.0f;
    repaint();
}

void HistoryView::setMeters (float muDb, float wallDb, bool valid)
{
    // instant attack (deeper), limited fall back toward 0 dB
    auto fall = [] (float shown, float v)
    {
        v = std::isfinite (v) ? juce::jlimit (-60.0f, 0.0f, v) : 0.0f;
        return v < shown ? v : juce::jmin (v, shown + kMeterReleaseDbPerTick);
    };

    const float m = valid ? fall (shownMu, muDb) : 0.0f;
    const float w = valid ? fall (shownWall, wallDb) : 0.0f;

    if (! juce::approximatelyEqual (m, shownMu) || ! juce::approximatelyEqual (w, shownWall) || valid != metersValid)
    {
        shownMu = m;
        shownWall = w;
        metersValid = valid;
        repaint (meterArea().expanded (2.0f, 24.0f).getSmallestIntegerContainer());
    }
}

void HistoryView::setSeconds (float s)
{
    seconds = juce::jlimit (1.0f, 15.0f, s);
    repaint();
}

const HistoryView::Column& HistoryView::columnAt (int age) const noexcept
{
    return ring[(size_t) ((head - 1 - age + 2 * kCapacity) % kCapacity)];
}

// Title row 26 px; below the plot 32 px (the time ruler; the meters' names and values); meters on the right.
juce::Rectangle<float> HistoryView::meterArea() const
{
    auto r = getLocalBounds().toFloat().reduced (12.0f, 10.0f);
    r.removeFromTop (26.0f);
    r.removeFromBottom (32.0f);
    const float w = juce::jlimit (64.0f, 84.0f, (float) getWidth() * 0.12f);
    return r.removeFromRight (w);
}

juce::Rectangle<float> HistoryView::plotArea() const
{
    auto r = getLocalBounds().toFloat().reduced (12.0f, 10.0f);
    r.removeFromTop (26.0f);
    r.removeFromBottom (32.0f);
    r.removeFromRight (meterArea().getWidth() + 10.0f);
    return r;
}

float HistoryView::dbToY (float db, juce::Rectangle<float> plot) const noexcept
{
    const float top = plot.getY() + 4.0f, bottom = plot.getBottom() - 4.0f;
    return top + juce::jlimit (0.0f, 1.0f, -db / rangeDb) * (bottom - top);
}

void HistoryView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    drawPanel (g, bounds.reduced (0.5f));

    const auto plot = plotArea();
    const auto meters = meterArea();

    if (plot.getWidth() < 60.0f || plot.getHeight() < 40.0f)
        return;

    const auto gr = Colours::gainReduction;

    //--------------------------------------------------------------------------
    // Title row: title, legend
    {
        auto row = juce::Rectangle<float> (bounds.getX() + 12.0f, bounds.getY() + 8.0f, bounds.getWidth() - 24.0f, 22.0f);
        const auto titleF = font (10.5f, 600);
        const juce::String title ("GAIN REDUCTION");
        const float titleW = textWidth (titleF, title) * 1.12f + 8.0f;
        drawSectionTitle (g, title, row.removeFromLeft (titleW).toNearestInt());
        row.removeFromLeft (10.0f);

        const auto f = font (11.0f, 600);
        auto legend = row;

        auto item = [&] (const juce::String& text, juce::Colour c)
        {
            const float w = textWidth (f, text) + 18.0f;

            if (w > legend.getWidth())
                return;

            auto r = legend.removeFromLeft (w);
            legend.removeFromLeft (10.0f);
            const auto sw = r.removeFromLeft (12.0f).withSizeKeepingCentre (12.0f, 8.0f);
            g.setColour (c);
            g.fillRoundedRectangle (sw, 2.0f);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r.withTrimmedLeft (6.0f), juce::Justification::centredLeft, false);
        };

        item ("Slow stage", accent);
        item ("Fast stage", gr);
    }

    //--------------------------------------------------------------------------
    // Plot
    stereo::drawWell (g, plot, 7.0f);

    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (plot.reduced (1.0f), 6.0f);
        g.reduceClipRegion (clip);

        const float columnsVisible = seconds * (float) kColumnsPerSecond;
        const float colW = plot.getWidth() / columnsVisible;
        const float right = plot.getRight() - 1.0f;
        // Rows every 1 / 3 / 6 dB, whichever keeps them >= 30 / 16 px apart; labels every 3 dB at most.
        const float pxDb = (dbToY (-rangeDb, plot) - dbToY (0.0f, plot)) / rangeDb;
        const float stepDb = pxDb >= 30.0f ? 1.0f : (pxDb * 3.0f >= 16.0f ? 3.0f : 6.0f);
        const float labelStep = juce::jmax (3.0f, stepDb);

        // Grid rows (labels on the left inside the plot) and one line per second.
        g.setFont (font (10.5f, 400));

        for (float db = 0.0f; db >= -rangeDb - 0.01f; db -= stepDb)
        {
            const float y = std::round (dbToY (db, plot)) + 0.5f;
            const bool major = std::fmod (-db, 6.0f) < 0.01f;
            const bool labelled = std::fmod (-db, labelStep) < 0.01f;
            g.setColour (Colours::outline.withAlpha (db == 0.0f ? 0.8f : (major ? 0.42f : (labelled ? 0.3f : 0.16f))));
            g.fillRect (juce::Rectangle<float> (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f));

            if (db < 0.0f && labelled)
            {
                g.setColour (Colours::textSubtle);
                g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getX() + 6.0f, y - 13.0f, 40.0f, 12.0f),
                            juce::Justification::centredLeft, false);
            }
        }

        const float pxPerSecond = colW * (float) kColumnsPerSecond;

        for (int s = 1; (float) s < seconds; ++s)
        {
            const float x = std::round (right - (float) s * pxPerSecond) + 0.5f;
            g.setColour (Colours::outline.withAlpha (0.2f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getY(), 1.0f, plot.getHeight()));
        }

        const int n = juce::jmin (count, (int) std::ceil (columnsVisible) + 2);

        if (n >= 2)
        {
            const float y0 = dbToY (0.0f, plot);

            auto curtain = [&] (bool total, bool closed)
            {
                juce::Path p;

                for (int a = 0; a < n; ++a)
                {
                    const auto& c = columnAt (a);
                    const float x = right - (float) a * colW;
                    const float y = dbToY (total ? c.total : c.mu, plot);

                    if (a == 0)
                    {
                        p.startNewSubPath (x, closed ? y0 : y);

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
                    p.lineTo (right - (float) (n - 1) * colW, y0);
                    p.closeSubPath();
                }

                return p;
            };

            // Total (fast-stage share visible below the slow stage), then the slow stage over it.
            g.setGradientFill (juce::ColourGradient::vertical (gr.withAlpha (0.18f), y0, gr.withAlpha (0.42f), plot.getBottom()));
            g.fillPath (curtain (true, true));
            g.setColour (gr.withAlpha (0.85f));
            g.strokePath (curtain (true, false), juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));

            g.setGradientFill (juce::ColourGradient::vertical (accent.withAlpha (0.22f), y0, accent.withAlpha (0.55f), plot.getBottom()));
            g.fillPath (curtain (false, true));
            g.setColour (accent);
            g.strokePath (curtain (false, false), juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
        }
        else
        {
            g.setFont (font (11.5f, 400));
            g.setColour (Colours::textFaint.brighter (0.2f));
            g.drawText (metersValid ? "No gain reduction yet" : "Not processing", plot, juce::Justification::centred, false);
        }
    }

    // Time ruler
    {
        g.setFont (font (10.5f, 400));
        g.setColour (Colours::textSubtle);
        const float colW = plot.getWidth() / (seconds * (float) kColumnsPerSecond);
        const float pxPerSecond = colW * (float) kColumnsPerSecond;
        const float y = plot.getBottom() + 4.0f;

        for (int s = 0; (float) s < seconds; ++s)
        {
            const float x = plot.getRight() - (float) s * pxPerSecond;
            const auto text = s == 0 ? juce::String ("now") : "-" + juce::String (s) + " s";
            const float w = 40.0f;
            auto r = juce::Rectangle<float> (x - w * 0.5f, y, w, 14.0f);

            if (s == 0)
                r = r.withX (x - w);

            g.drawText (text, r, s == 0 ? juce::Justification::centredRight : juce::Justification::centred, false);
        }
    }

    //--------------------------------------------------------------------------
    // Meters: slow and fast stage, hanging from 0 dB on the plot's scale
    {
        const float barW = juce::jmin (16.0f, (meters.getWidth() - 12.0f) * 0.5f);
        const float gap = meters.getWidth() - 2.0f * barW;
        const float top = dbToY (0.0f, plot), bottom = dbToY (-rangeDb, plot);
        const juce::String names[2] { "SLOW", "FAST" };
        const float values[2] { shownMu, shownWall };
        const juce::Colour colours[2] { accent, gr };

        for (int i = 0; i < 2; ++i)
        {
            const float cx = meters.getX() + barW * 0.5f + (float) i * (barW + gap * 0.5f) + gap * 0.25f;
            const auto bar = juce::Rectangle<float> (cx - barW * 0.5f, top, barW, bottom - top);
            g.setColour (Colours::background.darker (0.3f));
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (Colours::outline.withAlpha (0.6f));
            g.drawRoundedRectangle (bar.reduced (0.5f), 3.0f, 1.0f);

            if (metersValid && values[i] < -0.05f)
            {
                const float y = dbToY (values[i], plot);
                g.setColour (colours[i]);
                g.fillRoundedRectangle (bar.withBottom (y).reduced (2.0f, 1.0f).withTop (top + 1.0f), 2.0f);
            }

            const float labelW = gap * 0.5f + barW;
            g.setFont (font (11.0f, 600));
            g.setColour (metersValid ? Colours::text : Colours::textFaint);
            const auto text = metersValid ? juce::String (values[i] > -0.05f ? 0.0f : values[i], 1) : juce::String ("--");
            g.drawFittedText (text, juce::Rectangle<float> (cx - labelW * 0.5f, plot.getBottom() + 3.0f, labelW, 14.0f).toNearestInt(),
                              juce::Justification::centred, 1, 0.8f);
            g.setFont (font (10.0f, 600));
            g.setColour (Colours::textSubtle);
            g.drawFittedText (names[i], juce::Rectangle<float> (cx - labelW * 0.5f, plot.getBottom() + 17.0f, labelW, 13.0f).toNearestInt(),
                              juce::Justification::centred, 1, 0.8f);
        }
    }
}
} // namespace ps::ui::clim
