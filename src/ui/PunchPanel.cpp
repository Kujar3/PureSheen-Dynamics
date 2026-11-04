#include "ui/PunchPanel.h"

#include "dsp/Punch.h"
#include "ui/Controls.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/MultibandDisplay.h"
#include "ui/PunchViews.h"

#include <array>
#include <cmath>

// Punch panel. Owner: ui-punch. Layout and interactions: see PunchPanel.h.
//
// Live values: every tick (30 Hz) the panel exchanges PunchMeters gainMinDb / gainMaxDb (+200 / -200) for every
// channel set and band and pushes the range into a PunchGainHistory; the traces in the band display draw the last 90
// entries (3 s). The card readouts show the larger-magnitude extreme, held for ~0.3 s and then gliding, so a hit
// stays readable. A tick without a new block repeats the current gain (gainDb).
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr int kTracePoints = 90;              // 3 s at 30 Hz
constexpr float kTraceDb = 12.0f;             // +-12 dB ...
constexpr float kTraceSpan = 0.7f;            // ... over 70 % of the overlay's height
constexpr float kTopKeepOut = 10.0f;          // under the band header strip
constexpr float kBottomKeepOut = 54.0f;       // the display's frequency labels, legend and band count pill
constexpr float kLevelLabelsW = 22.0f;        // the display's level labels at the plot's left edge
constexpr int kHoldTicks = 9;

juce::String syncTooltip()
{
    return "Envelope (synced): the note value the punch recovers over, at the host tempo (1/64 triplet .. 1/2 dotted).";
}

juce::String msTooltip()
{
    return "Envelope: how long a hit takes to settle back. Short values act on the attack only; long values shape "
           "whole notes.";
}
} // namespace

//==============================================================================
struct PunchPanel::Impl
{
    Impl (PunchPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::Punch)),
          display (p, s, ps::ModuleType::Punch),
          settingsCard ("Settings", colour),
          amount (p, "pnch.amount", "Amount"),
          envelope (p, "pnch.env", "Envelope"),
          sync (p, "pnch.sync", "Sync"),
          stereoLink (p, "pnch.stLink", "Stereo Link"),
          autoGain (p, "pnch.autoGain", "Auto Gain"),
          linkBands (p, "pnch.bandLink", "Link Bands"),
          xoverType (p, "pnch.xoverType", "Crossover"),
          drag (p)
    {
        for (auto& set : readout)
            for (auto& v : set)
                v = 0.0f;

        amount.setColour (colour);
        envelope.setColour (colour);

        for (auto* t : { &sync, &stereoLink, &autoGain, &linkBands })
            t->setOnColour (colour);

        amount.setTooltip ("Amount: scales every band's punch (0 % = no effect, 100 % = the bands' own settings).");
        envelope.setTooltip (msTooltip());
        sync.setTooltip ("Sync: set the envelope as a note value at the host tempo instead of milliseconds.");
        stereoLink.setTooltip ("Stereo Link: one gain for left and right, from the middle of the mix, so the image "
                               "stays put. Stereo mode only.");
        autoGain.setTooltip ("Auto Gain: keeps each band's long-term level where it was, so you hear the change in "
                             "punch, not in loudness.");
        linkBands.setTooltip ("Link Bands: dragging one band's amount moves every band by the same amount.");
        xoverType.setTooltip ("Crossover type. Analog: no latency, phase-aligned bands. Digital: linear phase (adds "
                              "latency). Hybrid: steeper phase-aligned slopes.");

        owner.addAndMakeVisible (display);
        owner.addAndMakeVisible (settingsCard);

        for (auto* c : std::initializer_list<juce::Component*> { &amount, &envelope, &sync, &stereoLink, &autoGain,
                                                                 &linkBands, &xoverType })
            settingsCard.addAndMakeVisible (*c);

        for (int b = 0; b < ps::kMaxBands; ++b)
        {
            auto card = std::make_unique<PunchBandCard> (p, b, colour);
            card->onSelect = [this] (int band) { selectBand (band); };
            card->getKnob().getGroup = [this, b] (std::vector<juce::String>& ids, std::vector<juce::String>& twins)
            {
                groupFor (b, ids, twins, false);
            };
            owner.addChildComponent (*card);
            cards[(size_t) b] = std::move (card);
        }

        // Band display hooks.
        display.paintBandOverlay = [this] (juce::Graphics& g, int band, juce::Rectangle<float> area)
        {
            const int solo = display.getSoloBand();

            if (solo >= 0 && solo != band)
            {
                g.beginTransparencyLayer (0.4f);
                paintBand (g, band, area);
                g.endTransparencyLayer();
            }
            else
            {
                paintBand (g, band, area);
            }
        };
        display.onBandSelected = [this] (int) { updateSelection(); };
        display.onBandMouseDown = [this] (int band, const juce::MouseEvent& e) { return bandMouseDown (band, e); };
        display.onBandMouseDrag = [this] (int band, const juce::MouseEvent& e) { bandMouseDrag (band, e); };
        display.onBandMouseUp = [this] (int, const juce::MouseEvent&) { endDrag(); };
        display.onBandDoubleClick = [this] (int band, const juce::MouseEvent&) { return enterBandValue (band); };
        display.onBandPopupMenu = [this] (int band, const juce::MouseEvent&) { return bandMenu (band); };
        display.onBandMouseMove = [this] (int band, const juce::MouseEvent&) { setHoverBand (band); };
        display.getBandCursor = [] (int, juce::Point<float>) { return juce::MouseCursor (juce::MouseCursor::UpDownResizeCursor); };

        rebind();
    }

    ~Impl()
    {
        drag.end();

        // Band solo is a monitoring request that is invisible once the panel is gone: it ends with the panel.
        display.clearSolo();
    }

    //==========================================================================
    // State
    int numBands() const
    {
        return juce::jlimit (1, ps::kMaxBands, juce::roundToInt (processor.getParameterValue ("pnch.bands")));
    }

    bool isSplit() const { return isDualChannelMode (currentChannelMode (processor, ps::ModuleType::Punch)); }
    int currentSet() const { return isSplit() ? owner.getChannelSet() : 0; }
    bool isLinked() const { return isSplit() && processor.getParameterValue ("pnch.link") >= 0.5f; }
    bool bandsLinked() const { return processor.getParameterValue ("pnch.bandLink") >= 0.5f; }
    bool isSynced() const { return processor.getParameterValue ("pnch.sync") >= 0.5f; }
    juce::Colour setColour() const { return currentSet() == 1 ? Colours::sideChannel : colour; }
    int selectedBand() const { return juce::jlimit (0, numBands() - 1, display.getSelectedBand()); }

    static juce::String bandId (int set, int band, const char* name)
    {
        return juce::String ("pnch.") + (set == 1 ? "b" : "a") + ".b" + juce::String (band + 1) + "." + name;
    }

    juce::String twinId (int band, const char* name) const
    {
        return isLinked() ? bandId (1 - currentSet(), band, name) : juce::String();
    }

    float amountOf (int band) const { return processor.getParameterValue (bandId (currentSet(), band, "amount")); }
    bool bandOn (int band) const { return processor.getParameterValue (bandId (currentSet(), band, "on")) >= 0.5f; }

    // The amount parameters a drag on `band` moves: that band first, then (Link Bands) every other visible band.
    void groupFor (int band, std::vector<juce::String>& ids, std::vector<juce::String>& twins, bool always) const
    {
        ids.clear();
        twins.clear();

        if (! always && ! bandsLinked())
            return;

        const int n = numBands();
        ids.push_back (bandId (currentSet(), band, "amount"));
        twins.push_back (twinId (band, "amount"));

        if (bandsLinked())
        {
            for (int b = 0; b < n; ++b)
            {
                if (b == band)
                    continue;

                ids.push_back (bandId (currentSet(), b, "amount"));
                twins.push_back (twinId (b, "amount"));
            }
        }
    }

    juce::String rangeText (int band) const
    {
        const int n = numBands();

        if (n <= 1)
            return "Full range";

        auto xover = [this] (int i)   // as the DSP reads them: clamped and ascending
        {
            float hz = 20.0f;

            for (int k = 0; k <= i; ++k)
                hz = juce::jmax (hz, juce::jlimit (20.0f, 20000.0f, processor.getParameterValue ("pnch.xover" + juce::String (k + 1))));

            return hz;
        };

        const float lo = band == 0 ? 20.0f : xover (band - 1);
        const float hi = band == n - 1 ? 20000.0f : xover (band);
        return punchHzText (lo) + " - " + punchHzText (hi);
    }

    //==========================================================================
    void rebind()
    {
        endDrag();
        const int set = currentSet();
        const int other = isLinked() ? 1 - set : -1;
        const auto c = setColour();

        for (int b = 0; b < ps::kMaxBands; ++b)
        {
            cards[(size_t) b]->bind (set, other, c);
            cards[(size_t) b]->setRange (rangeText (b));
        }

        display.setAccentColour (c);
        display.setChannelSet (set);

        const bool synced = isSynced();
        envelope.setParamId (synced ? "pnch.syncNote" : "pnch.env");
        envelope.setTooltip (synced ? syncTooltip() : msTooltip());

        const bool stereo = ! isSplit();
        stereoLink.setEnabled (stereo);
        stereoLink.setAlpha (stereo ? 1.0f : 0.5f);
        stereoLink.setTooltip (stereo ? "Stereo Link: one gain for left and right, from the middle of the mix, so the "
                                        "image stays put."
                                      : "Stereo Link acts in Stereo mode only (Mid and Side are processed separately).");

        shownSet = set;
        shownLinked = isLinked();
        shownSplit = isSplit();
        shownSynced = synced;
        updateSelection();
        display.repaint();
    }

    void selectBand (int band)
    {
        if (band != display.getSelectedBand())
            display.setSelectedBand (band);

        updateSelection();
    }

    void updateSelection()
    {
        const int sel = selectedBand();

        for (int b = 0; b < ps::kMaxBands; ++b)
            cards[(size_t) b]->setSelected (b == sel);
    }

    //==========================================================================
    // Meters
    void readMeters()
    {
        auto* punch = dynamic_cast<ps::Punch*> (&moduleForChannelSet (processor, ps::ModuleType::Punch, 0));
        const bool running = punch != nullptr && processor.isModuleInChain (ps::ModuleType::Punch)
                             && processor.getParameterValue ("pnch.on") >= 0.5f && processor.getChain().getSampleRate() > 0.0;
        const int n = numBands();
        PunchGainHistory::Point points[ps::kNumChannelSets][ps::kMaxBands];

        for (int s = 0; s < ps::kNumChannelSets; ++s)
        {
            for (int b = 0; b < ps::kMaxBands; ++b)
            {
                auto& p = points[s][b];
                auto& r = readout[(size_t) s][(size_t) b];
                auto& hold = holdTicks[(size_t) s][(size_t) b];

                if (! running || b >= n)
                {
                    p = {};
                    r = 0.0f;
                    hold = 0;
                    lastLo[(size_t) s][(size_t) b] = lastHi[(size_t) s][(size_t) b] = 0.0f;
                    continue;
                }

                auto& m = punch->getMeters();
                const float lo = m.gainMinDb[s][b].exchange (200.0f);
                const float hi = m.gainMaxDb[s][b].exchange (-200.0f);
                const float now = m.gainDb[s][b].load();
                auto safe = [] (float v) { return std::isfinite (v) ? juce::jlimit (-60.0f, 60.0f, v) : 0.0f; };

                if (lo <= hi && lo < 199.0f && hi > -199.0f)
                {
                    p.lo = safe (lo);
                    p.hi = safe (hi);
                }
                else
                {
                    p.lo = p.hi = safe (now);
                }

                p.valid = true;
                lastLo[(size_t) s][(size_t) b] = p.lo;
                lastHi[(size_t) s][(size_t) b] = p.hi;

                // Readout: the larger-magnitude extreme of this tick, held, then gliding toward the current one.
                const float peak = std::abs (p.lo) > std::abs (p.hi) ? p.lo : p.hi;

                if (std::abs (peak) >= std::abs (r) || ((peak > 0.0f) != (r > 0.0f) && std::abs (peak) > 0.5f))
                {
                    r = peak;
                    hold = kHoldTicks;
                }
                else if (hold > 0)
                {
                    --hold;
                }
                else
                {
                    r += 0.3f * (peak - r);
                }
            }
        }

        history.push (points);
        metersValid = running;

        const int set = currentSet();
        const bool ag = processor.getParameterValue ("pnch.autoGain") >= 0.5f;

        for (int b = 0; b < ps::kMaxBands; ++b)
        {
            PunchBandCard::Live live;
            live.valid = running && b < n;
            live.bandOn = bandOn (b);
            live.autoGain = ag;

            if (live.valid)
            {
                const auto& m = punch->getMeters();
                auto safe = [] (float v) { return std::isfinite (v) ? juce::jlimit (-60.0f, 60.0f, v) : 0.0f; };
                live.gainDb = readout[(size_t) set][(size_t) b];
                live.lo = lastLo[(size_t) set][(size_t) b];
                live.hi = lastHi[(size_t) set][(size_t) b];
                live.microInDb = juce::jmax (0.0f, safe (m.microInDb[set][b].load()));
                live.microOutDb = juce::jmax (0.0f, safe (m.microOutDb[set][b].load()));
                live.makeupDb = safe (m.makeupDb[set][b].load());
            }

            cards[(size_t) b]->setLive (live);
        }
    }

    //==========================================================================
    // Overlay in the band display
    juce::Rectangle<float> overlayArea (int band, juce::Rectangle<float> area) const
    {
        auto r = area.withTrimmedTop (kTopKeepOut).withTrimmedBottom (kBottomKeepOut);

        if (band == 0)
            r.removeFromLeft (kLevelLabelsW);

        return r.reduced (juce::jmin (8.0f, r.getWidth() * 0.06f), 0.0f);
    }

    float yForAmount (juce::Rectangle<float> r, float percent) const
    {
        return r.getCentreY() - juce::jlimit (-100.0f, 100.0f, percent) / 100.0f * r.getHeight() * 0.5f;
    }

    float yForGain (juce::Rectangle<float> r, float db) const
    {
        return r.getCentreY() - juce::jlimit (-1.0f, 1.0f, db / kTraceDb) * r.getHeight() * 0.5f * kTraceSpan;
    }

    void paintBand (juce::Graphics& g, int band, juce::Rectangle<float> area)
    {
        if (band < 0 || band >= ps::kMaxBands || area.getWidth() < 8.0f)
            return;

        const auto r = overlayArea (band, area);

        if (r.getHeight() < 24.0f || r.getWidth() < 6.0f)
            return;

        const bool on = bandOn (band);
        const float a = amountOf (band);
        const bool selected = display.getSelectedBand() == band;
        const bool dragging = drag.isArmed() && dragBand == band;
        const bool groupDragging = drag.isArmed() && bandsLinked();
        const bool hover = hoverBand == band;
        const bool focused = selected || hover || dragging || (groupDragging && dragBand >= 0);
        const float zeroY = yForAmount (r, 0.0f);
        const float valueY = yForAmount (r, a);
        const auto c = setColour();
        const auto tint = a < 0.0f ? Colours::info : c;

        // Centre line (no change).
        g.setColour (Colours::text.withAlpha (0.18f));
        const float dashes[] = { 4.0f, 4.0f };
        g.drawDashedLine ({ r.getX(), zeroY, r.getRight(), zeroY }, dashes, 2, 1.0f);

        // The amount as a soft column from the centre line.
        if (std::abs (a) > 0.05f)
        {
            const auto fill = juce::Rectangle<float>::leftTopRightBottom (r.getX(), juce::jmin (zeroY, valueY), r.getRight(),
                                                                          juce::jmax (zeroY, valueY));
            const float strong = focused ? 0.30f : 0.2f;
            g.setGradientFill (juce::ColourGradient::vertical (tint.withAlpha (a > 0.0f ? strong : 0.04f), fill.getY(),
                                                               tint.withAlpha (a > 0.0f ? 0.04f : strong), fill.getBottom()));
            g.fillRoundedRectangle (fill, 3.0f);
            g.setColour (tint.withAlpha (focused ? 0.95f : 0.7f));
            g.fillRoundedRectangle (juce::Rectangle<float> (r.getX(), valueY - 1.0f, r.getWidth(), 2.0f), 1.0f);
        }

        // Live gain trace.
        if (on)
            paintTrace (g, band, r, (drag.isArmed() && dragBand != band && ! groupDragging) ? 0.35f : 1.0f);

        // Value pill on the amount line.
        {
            const auto text = punchAmountText (a);
            const auto f = font (11.5f, 700);
            const float pillW = juce::jmin (r.getWidth(), textWidth (f, text) + 18.0f);
            auto pill = juce::Rectangle<float> (pillW, 20.0f).withCentre ({ r.getCentreX(), valueY });
            pill = pill.withY (juce::jlimit (r.getY() - 6.0f, r.getBottom() - 14.0f, pill.getY()));
            g.setColour (Colours::background.withAlpha (0.92f));
            g.fillRoundedRectangle (pill, 10.0f);
            g.setColour ((std::abs (a) > 0.05f ? tint : Colours::textDim).withAlpha (dragging || hover ? 1.0f : 0.75f));
            g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, dragging ? 1.6f : 1.0f);
            g.setFont (f);
            g.setColour (dragging || hover || selected ? Colours::text : Colours::text.withAlpha (0.85f));
            g.drawText (text, pill, juce::Justification::centred, false);
        }

        // Current gain at the trace's newest end.
        if (on && metersValid && r.getWidth() >= 76.0f)
        {
            const float db = readout[(size_t) currentSet()][(size_t) band];
            const auto text = punchDbText (db);
            const auto f = font (10.5f, 700);
            const float w = textWidth (f, text) + 12.0f;
            const auto chip = juce::Rectangle<float> (r.getRight() - w, r.getY() - 4.0f, w, 16.0f);
            g.setColour (Colours::background.withAlpha (0.8f));
            g.fillRoundedRectangle (chip, 4.0f);
            g.setFont (f);
            g.setColour (std::abs (db) < 0.05f ? Colours::textSubtle : (db < 0.0f ? Colours::gainReduction : Colours::good));
            g.drawText (text, chip, juce::Justification::centred, false);
        }

        if (! on)
        {
            g.setColour (Colours::background.withAlpha (0.45f));
            g.fillRect (area);
            g.setFont (font (11.5f, 700).withExtraKerningFactor (0.06f));
            g.setColour (Colours::textSubtle);
            g.drawText ("BAND OFF", area.withTrimmedTop (area.getHeight() * 0.18f).withHeight (16.0f), juce::Justification::centred, false);
        }
    }

    void paintTrace (juce::Graphics& g, int band, juce::Rectangle<float> r, float alpha)
    {
        const int set = currentSet();
        const float step = r.getWidth() / (float) (kTracePoints - 1);

        // Contiguous runs of valid points (newest at the right edge).
        juce::Path fill, upper, lower;
        bool any = false;
        int age = 0;

        while (age < kTracePoints)
        {
            while (age < kTracePoints && ! history.get (set, band, age).valid)
                ++age;

            if (age >= kTracePoints)
                break;

            const int first = age;

            while (age < kTracePoints && history.get (set, band, age).valid)
                ++age;

            const int last = age - 1;   // oldest of the run
            juce::Path run;

            for (int i = first; i <= last; ++i)
            {
                const auto p = history.get (set, band, i);
                const float x = r.getRight() - (float) i * step;
                const float y = yForGain (r, p.hi);

                if (i == first)
                {
                    run.startNewSubPath (x, y);
                    upper.startNewSubPath (x, y);
                }
                else
                {
                    run.lineTo (x, y);
                    upper.lineTo (x, y);
                }
            }

            for (int i = last; i >= first; --i)
            {
                const auto p = history.get (set, band, i);
                const float x = r.getRight() - (float) i * step;
                const float y = yForGain (r, p.lo) + 0.75f;   // a hairline even for a steady gain
                run.lineTo (x, y);

                if (i == last)
                    lower.startNewSubPath (x, y);
                else
                    lower.lineTo (x, y);
            }

            run.closeSubPath();
            fill.addPath (run);
            any = true;
        }

        if (! any)
            return;

        const float zeroY = yForGain (r, 0.0f);
        const auto stroke = juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        for (int half = 0; half < 2; ++half)
        {
            juce::Graphics::ScopedSaveState state (g);
            const auto clip = half == 0 ? juce::Rectangle<float>::leftTopRightBottom (r.getX() - 2.0f, r.getY() - 2.0f, r.getRight() + 2.0f, zeroY)
                                        : juce::Rectangle<float>::leftTopRightBottom (r.getX() - 2.0f, zeroY, r.getRight() + 2.0f, r.getBottom() + 2.0f);
            g.reduceClipRegion (clip.getSmallestIntegerContainer());
            const auto tc = half == 0 ? Colours::good : Colours::gainReduction;
            g.setColour (tc.withAlpha (0.28f * alpha));
            g.fillPath (fill);
            g.setColour (tc.withAlpha (0.95f * alpha));
            g.strokePath (half == 0 ? upper : lower, stroke);
            g.strokePath (half == 0 ? lower : upper, juce::PathStrokeType (1.0f));
        }
    }

    //==========================================================================
    // Mouse in the band display
    bool bandMouseDown (int band, const juce::MouseEvent& e)
    {
        if (band < 0 || band >= numBands() || ! isLeftClick (e))
            return false;

        selectBand (band);

        if (e.getNumberOfClicks() >= 2)   // the double-click types a value (onBandDoubleClick): no drag
            return true;

        std::vector<juce::String> ids, twins;
        groupFor (band, ids, twins, true);
        drag.start (ids, twins);
        dragBand = band;
        dragPixels = juce::jmax (40.0f, overlayArea (band, display.getBandArea (band)).getHeight());
        display.repaint();
        return true;
    }

    void bandMouseDrag (int band, const juce::MouseEvent& e)
    {
        if (band != dragBand || ! drag.isArmed())
            return;

        const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
        // The overlay's height = the whole -100 .. +100 % range, so the value line follows the mouse.
        drag.dragBy (-(float) e.getDistanceFromDragStartY() / dragPixels * 200.0f * (fine ? 0.2f : 1.0f));
        display.repaint();
    }

    void endDrag()
    {
        if (drag.isArmed())
        {
            drag.end();
            display.repaint();
        }

        dragBand = -1;
    }

    bool enterBandValue (int band)
    {
        if (band < 0 || band >= numBands())
            return false;

        endDrag();
        stereo::promptParamValue (display, processor, bandId (currentSet(), band, "amount"), twinId (band, "amount"),
                                  "Band " + juce::String (band + 1) + " Amount");
        return true;
    }

    bool bandMenu (int band)
    {
        if (band < 0 || band >= numBands())
            return false;

        selectBand (band);
        const auto id = bandId (currentSet(), band, "amount");
        const auto twin = twinId (band, "amount");
        const auto onId = bandId (currentSet(), band, "on");
        const auto onTwin = twinId (band, "on");
        const bool on = bandOn (band);
        const auto title = "Band " + juce::String (band + 1) + " Amount";

        juce::PopupMenu menu;
        menu.addSectionHeader (title + " (" + punchAmountText (amountOf (band)) + ")");
        menu.addItem (1, "Reset to 0 %");
        menu.addItem (2, "Enter value...");
        menu.addSeparator();
        menu.addItem (3, "Band on", true, on);

        juce::Component::SafePointer<PunchPanel> safe (&owner);
        auto& p = processor;
        showMenu (menu, display, [safe, &p, id, twin, onId, onTwin, on, title] (int result)
        {
            if (safe == nullptr)
                return;

            if (result == 1)
            {
                stereo::setParam (p, id, twin, 0.0f);
            }
            else if (result == 2)
            {
                stereo::promptParamValue (*safe, p, id, twin, title);
            }
            else if (result == 3)
            {
                stereo::setParam (p, onId, onTwin, on ? 0.0f : 1.0f);
            }
        });
        return true;
    }

    void setHoverBand (int band)
    {
        if (band == hoverBand)
            return;

        hoverBand = band;
        display.repaint();

        if (band >= 0 && ! hoverHintShown)
        {
            hoverHintShown = true;
            owner.showHeaderHint ("Drag a band up for more punch, down to soften the hits");
        }
    }

    //==========================================================================
    void tick()
    {
        display.tick();

        if (currentSet() != shownSet || isLinked() != shownLinked || isSplit() != shownSplit || isSynced() != shownSynced)
            rebind();

        const int n = numBands();

        if (n != shownBands)
        {
            shownBands = n;
            layoutCards();
            updateSelection();
        }

        // Band ranges follow the crossovers.
        for (int b = 0; b < n; ++b)
            cards[(size_t) b]->setRange (rangeText (b));

        if (display.getSelectedBand() != shownSelected)
        {
            shownSelected = display.getSelectedBand();
            updateSelection();
        }

        readMeters();

        for (auto& card : cards)
            card->getKnob().tickWheel();
    }

    //==========================================================================
    // Layout
    void layout (juce::Rectangle<int> content)
    {
        auto c = content.reduced (12, 6).withTrimmedBottom (6);
        const int h = c.getHeight(), w = c.getWidth();
        const int cardsH = juce::jlimit (170, 240, juce::roundToInt ((float) h * 0.37f));
        compact = cardsH < 186;

        cardsRow = c.removeFromBottom (cardsH);
        c.removeFromBottom (kGap);
        display.setBounds (c);

        const int settingsW = juce::jlimit (270, 360, juce::roundToInt ((float) w * 0.31f));
        settingsCard.setBounds (cardsRow.removeFromLeft (settingsW));
        cardsRow.removeFromLeft (kGap);

        layoutSettings();
        layoutCards();
    }

    void layoutSettings()
    {
        auto body = settingsCard.getBody();
        const int toggleW = juce::jlimit (96, 132, body.getWidth() * 2 / 5);

        // The crossover type needs ~160 px for its caption: when the knob column is narrower, it spans the card's
        // width under both columns.
        const bool xoverFullWidth = body.getWidth() - toggleW - 10 < 160;
        juce::Rectangle<int> xover;

        if (xoverFullWidth)
        {
            xover = body.removeFromBottom (24);
            body.removeFromBottom (6);
        }

        auto toggles = body.removeFromRight (toggleW);
        body.removeFromRight (10);

        // Toggles: an even column.
        {
            const int rowH = toggles.getHeight() >= 4 * 24 + 3 * 4 ? 24 : 22;
            const int gap = juce::jlimit (2, 10, (toggles.getHeight() - 4 * rowH) / 3);
            auto col = toggles.withSizeKeepingCentre (toggles.getWidth(), juce::jmin (toggles.getHeight(), 4 * rowH + 3 * gap));

            for (auto* t : { &sync, &stereoLink, &autoGain, &linkBands })
            {
                t->setBounds (col.removeFromTop (rowH));
                col.removeFromTop (gap);
            }
        }

        // Knobs (the crossover type under them when it fits there).
        if (! xoverFullWidth)
        {
            xover = body.removeFromBottom (24);
            body.removeFromBottom (6);
        }

        xoverType.setBounds (xover);
        const int kh = juce::jmin (body.getHeight(), compact ? 70 : 84);
        const int kw = juce::jmin (92, body.getWidth() / 2);
        const bool small = compact || kh < 72;
        amount.setCompact (small);
        envelope.setCompact (small);
        auto knobs = body.withSizeKeepingCentre (kw * 2, kh);
        amount.setBounds (knobs.removeFromLeft (kw));
        envelope.setBounds (knobs);
    }

    void layoutCards()
    {
        const int n = numBands();
        auto row = cardsRow;

        if (row.isEmpty())
            return;

        // Up to 4 cards share the row evenly (a lone band's card uses its wide layout).
        const int each = (row.getWidth() - (n - 1) * kGap) / n;

        for (int b = 0; b < ps::kMaxBands; ++b)
        {
            auto& card = *cards[(size_t) b];
            card.setVisible (b < n);

            if (b >= n)
                continue;

            card.setCompact (compact);
            card.setBounds (row.removeFromLeft (b == n - 1 ? row.getWidth() : each));
            row.removeFromLeft (kGap);
        }
    }

    //==========================================================================
    PunchPanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    MultibandDisplay display;
    stereo::Card settingsCard;
    ParamKnob amount, envelope;
    ParamToggle sync, stereoLink, autoGain, linkBands;
    ParamChoice xoverType;
    std::array<std::unique_ptr<PunchBandCard>, ps::kMaxBands> cards;

    PunchAmountGesture drag;
    int dragBand = -1, hoverBand = -1;
    float dragPixels = 200.0f;
    bool hoverHintShown = false;

    PunchGainHistory history;
    std::array<std::array<float, ps::kMaxBands>, ps::kNumChannelSets> readout {}, lastLo {}, lastHi {};
    std::array<std::array<int, ps::kMaxBands>, ps::kNumChannelSets> holdTicks {};
    bool metersValid = false;

    juce::Rectangle<int> cardsRow;
    bool compact = false;
    int shownSet = -1, shownBands = -1, shownSelected = -1;
    bool shownLinked = false, shownSplit = false, shownSynced = false;
};

//==============================================================================
PunchPanel::PunchPanel (PureSheenProcessor& p, EditorServices& s) : ModulePanel (p, s, ps::ModuleType::Punch)
{
    impl = std::make_unique<Impl> (*this, p, s);
    neverTakeKeyboardFocus (*this);

    if (! getLocalBounds().isEmpty())
        impl->layout (getContentArea());
}

PunchPanel::~PunchPanel() = default;

MultibandDisplay& PunchPanel::getBandDisplay() noexcept { return impl->display; }

void PunchPanel::tick()
{
    ModulePanel::tick();
    impl->tick();
}

void PunchPanel::frameTick() { impl->display.repaint(); }

void PunchPanel::refresh()
{
    ModulePanel::refresh();
    impl->display.refresh();
    impl->rebind();
    impl->layout (getContentArea());
}

void PunchPanel::channelSetChanged()
{
    impl->rebind();
}

void PunchPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    if (impl != nullptr)
        impl->layout (contentArea);
}
} // namespace ps::ui
