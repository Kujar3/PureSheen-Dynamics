#include "ui/PunchViews.h"

#include "ui/Controls.h"

#include <cmath>

// Parts of the Punch panel. Owner: ui-punch. See PunchViews.h.
namespace ps::ui
{
namespace
{
constexpr float kGainBarDb = 12.0f;       // the band card's live gain bar spans -12 .. +12 dB
constexpr float kMicroBarDb = 16.0f;      // micro-dynamics bars span 0 .. 16 dB (std dev of the relative level)
constexpr float kKnobPixels = 220.0f;     // same drag sensitivity as ParamKnob (pixels for the whole range)

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

juce::Colour gainColour (float db) { return db < 0.0f ? Colours::gainReduction : Colours::good; }
} // namespace

juce::String punchAmountText (float percent)
{
    const int v = juce::roundToInt (percent);
    return (v > 0 ? "+" : "") + juce::String (v) + " %";
}

juce::String punchDbText (float db)
{
    if (std::abs (db) < 0.05f)
        return "0.0 dB";

    return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
}

juce::String punchHzText (float hz)
{
    if (hz < 999.5f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    const float k = hz / 1000.0f;
    return (k < 9.95f ? juce::String (k, 1) : juce::String (juce::roundToInt (k))) + " kHz";
}

//==============================================================================
PunchAmountGesture::PunchAmountGesture (PureSheenProcessor& p) : processor (p) {}

PunchAmountGesture::~PunchAmountGesture() { end(); }

void PunchAmountGesture::start (const std::vector<juce::String>& ids, const std::vector<juce::String>& twins)
{
    end();

    for (size_t i = 0; i < ids.size(); ++i)
    {
        Item item;
        item.param = processor.getParameter (ids[i]);

        if (item.param == nullptr)
            continue;

        if (i < twins.size() && twins[i].isNotEmpty() && twins[i] != ids[i])
            item.twin = processor.getParameter (twins[i]);

        item.start = item.param->convertFrom0to1 (item.param->getValue());
        items.push_back (item);
    }
}

void PunchAmountGesture::dragBy (float deltaPlain)
{
    if (items.empty())
        return;

    for (auto& item : items)
    {
        const auto& range = item.param->getNormalisableRange();
        const float plain = range.snapToLegalValue (juce::jlimit (range.start, range.end, item.start + deltaPlain));
        const float norm = item.param->convertTo0to1 (plain);
        const bool changes = std::abs (norm - item.param->getValue()) > 1.0e-7f
                             || (item.twin != nullptr && std::abs (item.twin->convertTo0to1 (plain) - item.twin->getValue()) > 1.0e-7f);

        if (! changes)
            continue;

        if (! active)
        {
            active = true;

            for (auto& it : items)
            {
                it.param->beginChangeGesture();

                if (it.twin != nullptr)
                    it.twin->beginChangeGesture();
            }
        }

        item.param->setValueNotifyingHost (norm);

        if (item.twin != nullptr)
            item.twin->setValueNotifyingHost (item.twin->convertTo0to1 (plain));
    }
}

void PunchAmountGesture::end()
{
    if (active)
    {
        for (auto& item : items)
        {
            item.param->endChangeGesture();

            if (item.twin != nullptr)
                item.twin->endChangeGesture();
        }
    }

    active = false;
    items.clear();
}

//==============================================================================
void PunchGainHistory::push (const Point (&values)[ps::kNumChannelSets][ps::kMaxBands]) noexcept
{
    head = (head + 1) % kCapacity;

    for (int s = 0; s < ps::kNumChannelSets; ++s)
        for (int b = 0; b < ps::kMaxBands; ++b)
            points[(size_t) s][(size_t) b][(size_t) head] = values[s][b];

    count = juce::jmin (kCapacity, count + 1);
}

PunchGainHistory::Point PunchGainHistory::get (int set, int band, int age) const noexcept
{
    if (age < 0 || age >= count || set < 0 || set >= ps::kNumChannelSets || band < 0 || band >= ps::kMaxBands)
        return {};

    return points[(size_t) set][(size_t) band][(size_t) ((head - age + kCapacity) % kCapacity)];
}

void PunchGainHistory::clear() noexcept
{
    count = 0;
}

//==============================================================================
PunchBandKnob::PunchBandKnob (PureSheenProcessor& p, const juce::String& paramId, const juce::String& caption)
    : ParamKnob (p, paramId, caption), processor (p), gesture (p)
{
}

PunchBandKnob::~PunchBandKnob()
{
    gesture.end();
}

bool PunchBandKnob::armGroup()
{
    if (getGroup == nullptr)
        return false;

    std::vector<juce::String> ids, twins;
    getGroup (ids, twins);

    if (ids.size() < 2)
        return false;

    gesture.start (ids, twins);
    return gesture.isArmed();
}

void PunchBandKnob::mouseDown (const juce::MouseEvent& e)
{
    if (groupWheel)
    {
        gesture.end();
        groupWheel = false;
    }

    // Right clicks always open the knob's own menu; plain knob behaviour without Link Bands.
    if (! isLeftClick (e) || ! armGroup())
    {
        groupDrag = false;
        ParamKnob::mouseDown (e);
        return;
    }

    groupDrag = true;
    dragDelta = 0.0f;
    lastY = e.position.y;
}

void PunchBandKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! groupDrag)
    {
        ParamKnob::mouseDrag (e);
        return;
    }

    if (! isLeftClick (e))
        return;

    const float dy = e.position.y - lastY;
    lastY = e.position.y;
    const bool fine = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
    dragDelta -= dy / kKnobPixels * 200.0f * (fine ? 0.1f : 1.0f);
    gesture.dragBy (dragDelta);
    repaint();
}

void PunchBandKnob::mouseUp (const juce::MouseEvent& e)
{
    if (! groupDrag)
    {
        ParamKnob::mouseUp (e);
        return;
    }

    groupDrag = false;
    gesture.end();
    repaint();
}

void PunchBandKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (groupDrag)
        return;

    if (! groupWheel && ! armGroup())
    {
        ParamKnob::mouseWheelMove (e, wheel);
        return;
    }

    const float delta = (std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX)
                        * (wheel.isReversed ? -1.0f : 1.0f);

    if (std::abs (delta) < 1.0e-4f)
        return;

    if (! groupWheel)
    {
        groupWheel = true;
        wheelDelta = 0.0f;
    }

    const bool fine = e.mods.isCtrlDown() || e.mods.isShiftDown();
    wheelDelta += delta * (fine ? 0.01f : 0.06f) * 200.0f;   // ParamKnob's wheel steps, in percent
    gesture.dragBy (wheelDelta);
    lastWheelMs = nowMs();
}

void PunchBandKnob::tickWheel()
{
    if (groupWheel && nowMs() - lastWheelMs > 450.0)
    {
        groupWheel = false;
        gesture.end();
    }
}

void PunchBandKnob::cancelGroupGesture()
{
    gesture.end();
    groupDrag = false;
    groupWheel = false;
}

//==============================================================================
PunchBandCard::PunchBandCard (PureSheenProcessor& p, int b, juce::Colour c)
    : stereo::Card ("Band " + juce::String (b + 1), c), processor (p), band (b), colour (c),
      amount (p, bandId (0, "amount"), "Amount"),
      onToggle (p, bandId (0, "on"), "On")
{
    setBadge (band + 1);
    amount.setColour (c);
    amount.setTooltip ("Punch amount of this band. Up: the hits stand out more (punchier). Down: the hits are "
                       "softened (denser). Double-click: 0 %.");
    onToggle.setOnColour (c);
    onToggle.setTooltip ("Band on / off (off: the band passes unprocessed)");
    addAndMakeVisible (amount);
    addAndMakeVisible (onToggle);
}

PunchBandCard::~PunchBandCard() = default;

juce::String PunchBandCard::bandId (int set, const char* name) const
{
    return juce::String ("pnch.") + (set == 1 ? "b" : "a") + ".b" + juce::String (band + 1) + "." + name;
}

void PunchBandCard::bind (int set, int linkedSet, juce::Colour c)
{
    if (set != boundSet || linkedSet != boundLinked)
    {
        amount.cancelGroupGesture();
        boundSet = set;
        boundLinked = linkedSet;
        amount.setParamId (bandId (set, "amount"));
        amount.setLinkedParamId (linkedSet >= 0 ? bandId (linkedSet, "amount") : juce::String());
        onToggle.setParamId (bandId (set, "on"));
        onToggle.setLinkedParamId (linkedSet >= 0 ? bandId (linkedSet, "on") : juce::String());
    }

    if (c != colour)
    {
        colour = c;
        amount.setColour (c);
        onToggle.setOnColour (c);
        setColour (c);
        repaint();
    }
}

void PunchBandCard::setRange (const juce::String& range)
{
    if (range != rangeText)
    {
        rangeText = range;
        setSubtitle (wide ? rangeText : juce::String());
        repaint();
    }
}

void PunchBandCard::setSelected (bool s)
{
    if (s != selected)
    {
        selected = s;
        repaint();
    }
}

void PunchBandCard::setCompact (bool c)
{
    if (c != compact)
    {
        compact = c;
        resized();
        repaint();
    }
}

void PunchBandCard::setLive (const Live& l)
{
    live = l;
    auto differs = [] (float a, float b, float tolerance) { return std::abs (a - b) > tolerance; };
    const bool changed = live.valid != shown.valid || live.bandOn != shown.bandOn || live.autoGain != shown.autoGain
                         || differs (live.gainDb, shown.gainDb, 0.04f) || differs (live.lo, shown.lo, 0.1f)
                         || differs (live.hi, shown.hi, 0.1f) || differs (live.microInDb, shown.microInDb, 0.04f)
                         || differs (live.microOutDb, shown.microOutDb, 0.04f) || differs (live.makeupDb, shown.makeupDb, 0.04f);

    if (changed)
    {
        shown = live;
        repaint();
    }

    const float alpha = live.bandOn ? 1.0f : 0.5f;

    if (std::abs (amount.getAlpha() - alpha) > 0.01f)
        amount.setAlpha (alpha);
}

void PunchBandCard::resized()
{
    wide = getWidth() >= 250;
    setTitle (getWidth() >= 150 ? "Band " + juce::String (band + 1) : juce::String());   // narrow: the badge names it
    setTitleReserve (52);
    setSubtitle (wide ? rangeText : juce::String());
    onToggle.setBounds (getTitleRowRight (46));

    auto body = getBody();

    if (wide)
    {
        // Knob on the left, live readouts on the right.
        rangeArea = {};
        const int knobW = juce::jmin (100, body.getWidth() / 3);
        auto left = body.removeFromLeft (knobW);
        body.removeFromLeft (14);
        const int kh = juce::jmin (left.getHeight(), compact ? 66 : 84);
        amount.setCompact (compact || kh < 72);
        amount.setBounds (left.withSizeKeepingCentre (knobW, kh));
        const int blockH = juce::jmin (body.getHeight(), 112);
        body = body.withSizeKeepingCentre (body.getWidth(), blockH).withWidth (juce::jmin (body.getWidth(), 340));
        gainArea = body.removeFromTop (body.getHeight() / 2 - 2);
        body.removeFromTop (4);
        microArea = body;
        return;
    }

    rangeArea = body.removeFromTop (14);
    body.removeFromTop (2);
    const bool roomy = body.getHeight() >= 128;   // bars under the knob, else text lines

    if (roomy)
    {
        microArea = body.removeFromBottom (32);
        body.removeFromBottom (6);
        gainArea = body.removeFromBottom (26);
        body.removeFromBottom (4);
    }
    else
    {
        microArea = body.removeFromBottom (14);
        gainArea = body.removeFromBottom (15);
        body.removeFromBottom (2);
    }

    const int kh = juce::jmin (body.getHeight(), compact ? 66 : 80);
    amount.setCompact (compact || kh < 72);
    amount.setBounds (body.withSizeKeepingCentre (juce::jmin (body.getWidth(), 92), kh));
}

void PunchBandCard::paint (juce::Graphics& g)
{
    stereo::Card::paint (g);

    if (selected)
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (colour.withAlpha (0.07f));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (colour.withAlpha (0.75f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.2f);
    }

    const bool valid = shown.valid && shown.bandOn;
    const float dim = shown.bandOn ? 1.0f : 0.5f;

    if (! rangeArea.isEmpty())
    {
        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawFittedText (rangeText, rangeArea, juce::Justification::centredLeft, 1, 0.8f);
    }

    const bool bars = wide || gainArea.getHeight() >= 24;
    const auto captionFont = font (10.5f, 600);
    const auto valueFont = font (bars && wide ? 13.0f : 11.5f, 700);

    // Live gain: caption + value, then a bipolar bar (-12 .. +12 dB) with the last tick's range and the readout.
    {
        auto r = gainArea;
        auto top = r.removeFromTop (bars ? juce::jmin (r.getHeight(), wide ? 20 : 15) : r.getHeight());
        const auto valueText = valid ? punchDbText (shown.gainDb) : juce::String ("--");
        const auto vc = ! valid ? Colours::textFaint : (std::abs (shown.gainDb) < 0.05f ? Colours::text.withAlpha (0.85f)
                                                                                         : gainColour (shown.gainDb));
        g.setFont (captionFont);
        g.setColour (Colours::textSubtle.withMultipliedAlpha (dim));
        g.drawText (shown.bandOn ? "Gain" : "Off", top, juce::Justification::centredLeft, false);
        g.setFont (valueFont);
        g.setColour (vc);
        g.drawText (valueText, top, juce::Justification::centredRight, false);

        if (bars && r.getHeight() >= 4)
        {
            const auto track = r.toFloat().withSizeKeepingCentre ((float) r.getWidth(), 6.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (track, 3.0f);
            const float mid = track.getCentreX();
            auto xOf = [&] (float db) { return mid + juce::jlimit (-1.0f, 1.0f, db / kGainBarDb) * track.getWidth() * 0.5f; };

            if (valid)
            {
                const float x0 = xOf (juce::jmin (shown.lo, 0.0f)), x1 = xOf (juce::jmax (shown.hi, 0.0f));

                if (shown.lo < -0.05f)
                {
                    g.setColour (Colours::gainReduction.withAlpha (0.45f));
                    g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (x0, track.getY(), mid, track.getBottom()), 2.0f);
                }

                if (shown.hi > 0.05f)
                {
                    g.setColour (Colours::good.withAlpha (0.45f));
                    g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (mid, track.getY(), x1, track.getBottom()), 2.0f);
                }

                const float x = xOf (shown.gainDb);
                g.setColour (std::abs (shown.gainDb) < 0.05f ? Colours::text.withAlpha (0.7f) : gainColour (shown.gainDb));
                g.fillRoundedRectangle (juce::Rectangle<float> (3.0f, 10.0f).withCentre ({ x, track.getCentreY() }), 1.5f);
            }

            g.setColour (Colours::textFaint);
            g.fillRect (juce::Rectangle<float> (1.0f, 10.0f).withCentre ({ mid, track.getCentreY() }));
        }
    }

    // Micro-dynamics in / out (how much the level moves around its average, before and after the band).
    {
        auto r = microArea;
        const bool on = shown.valid;
        const auto inText = on ? juce::String (shown.microInDb, 1) : juce::String ("--");
        const auto outText = on ? juce::String (shown.microOutDb, 1) : juce::String ("--");

        if (! bars || r.getHeight() < 24)
        {
            // One text line: "Micro 6.1 > 8.3 dB"
            g.setFont (captionFont);
            g.setColour (Colours::textSubtle.withMultipliedAlpha (dim));
            g.drawText ("Micro", r, juce::Justification::centredLeft, false);
            g.setFont (font (11.0f, 700));
            g.setColour (on ? Colours::text.withAlpha (0.85f * dim) : Colours::textFaint);
            g.drawText (on ? inText + " > " + outText + " dB" : juce::String ("--"), r, juce::Justification::centredRight, false);
            return;
        }

        if (wide)
        {
            auto heading = r.removeFromTop (juce::jmin (r.getHeight(), 18));
            g.setFont (captionFont);
            g.setColour (Colours::textSubtle.withMultipliedAlpha (dim));
            g.drawText ("Micro-dynamics", heading, juce::Justification::centredLeft, false);

            if (shown.autoGain && on)
            {
                g.setColour (Colours::textSubtle);
                g.drawText ("Make-up " + punchDbText (shown.makeupDb), heading, juce::Justification::centredRight, false);
            }
        }

        const int rowH = juce::jmax (12, r.getHeight() / 2);
        const juce::String labels[2] = { "In", "Out" };
        const float values[2] = { shown.microInDb, shown.microOutDb };
        const juce::String texts[2] = { inText, outText };
        const juce::Colour colours[2] = { Colours::input, colour };
        const auto labelFont = font (10.5f, 600);
        const int labelW = 26, valueW = wide ? 52 : 30;

        for (int i = 0; i < 2; ++i)
        {
            auto row = r.removeFromTop (rowH);
            g.setFont (labelFont);
            g.setColour (Colours::textSubtle.withMultipliedAlpha (dim));
            g.drawText (labels[i], row.removeFromLeft (labelW), juce::Justification::centredLeft, false);
            g.setFont (font (11.0f, 700));
            g.setColour (on ? Colours::text.withAlpha (0.88f * dim) : Colours::textFaint);
            g.drawText (wide && on ? texts[i] + " dB" : texts[i], row.removeFromRight (valueW), juce::Justification::centredRight, false);
            row.removeFromRight (6);

            const auto track = row.toFloat().withSizeKeepingCentre ((float) row.getWidth(), 4.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (track, 2.0f);

            if (on && values[i] > 0.01f)
            {
                const float w = track.getWidth() * juce::jlimit (0.0f, 1.0f, values[i] / kMicroBarDb);
                g.setColour (colours[i].withAlpha (0.85f * dim));
                g.fillRoundedRectangle (track.withWidth (juce::jmax (3.0f, w)), 2.0f);
            }
        }
    }
}

void PunchBandCard::mouseUp (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && getLocalBounds().contains (e.getPosition()) && onSelect != nullptr)
        onSelect (band);
}

} // namespace ps::ui
