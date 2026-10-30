#include "ui/DitherPanel.h"

#include "ui/Controls.h"

namespace ps::ui
{
namespace
{
constexpr int kHeaderH = 34;

void drawCard (juce::Graphics& g, juce::Rectangle<float> b, const juce::String& title)
{
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                       Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);
    g.setFont (font (11.0f, 700).withExtraKerningFactor (0.08f));
    g.setColour (Colours::text.withAlpha (0.9f));
    g.drawText (title.toUpperCase(), b.reduced (12.0f, 0.0f).withHeight (30.0f).translated (0.0f, 1.0f),
                juce::Justification::centredLeft, false);
}
} // namespace

struct DitherPanel::Impl
{
    Impl (DitherPanel& o, EditorContext& c)
        : owner (o), ctx (c), proc (c.getProcessor()),
          power (proc, "dith.on", "Dither on"),
          bits (proc, "dith.bits", { "24", "20", "16", "12", "8" }),
          amount (proc, "dith.amount"),
          shape (proc, "dith.shape", "Noise shaping"),
          harm (proc, "dith.harm", "Harmonic suppression"),
          limit (proc, "dith.limit", "Limit peaks"),
          blank (proc, "dith.blank", "Auto-blank"),
          dc (proc, "dith.dc", "DC filter")
    {
        power.setTooltip ("Dither on: adds the dither and lowers the output to the chosen bit depth");
        bits.setTooltip ("Bit depth of the file you will export (16 bit for CD, 24 bit for streaming masters)");
        amount.setTooltip ("Amount of dither noise. Off: plain rounding (use Harmonic suppression then).");
        shape.setTooltip ("Noise shaping: moves the dither noise to the highest frequencies, where the ear is least "
                          "sensitive (about 14 dB quieter in the 1 - 6 kHz range at 100 %).");
        harm.setTooltip ("Harmonic suppression (only with Amount Off): moves rounding distortion above 16 kHz without "
                         "adding a noise floor");
        limit.setTooltip ("Limit peaks: keeps the shaped noise small so it cannot create new peaks");
        blank.setTooltip ("Auto-blank: true digital silence after 0.7 s of silence at the input (clean fade-outs)");
        dc.setTooltip ("DC filter: a 1 Hz high-pass before the Maximizer removes DC offset (the meter on the right)");
        reset.setTooltip ("Reset the dither settings");
        reset.onClick = [this]
        {
            std::vector<std::pair<juce::String, float>> values;

            for (const auto& s : ps::paramSpecs())
                if (juce::String (s.id).startsWith ("dith."))
                    values.emplace_back (juce::String (s.id), s.defaultValue);

            proc.setParameterValues (values, "Reset Dither");
        };

        shape.setColour (Colours::accent);

        for (juce::Component* comp : std::initializer_list<juce::Component*> { &power, &bits, &amount, &shape, &harm, &limit,
                                                                            &blank, &dc, &reset })
            owner.addAndMakeVisible (comp);
    }

    void paintBitMeter (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        const auto& m = proc.getChain().getDitherMeters();
        const int depthIndex = juce::jlimit (0, 4, juce::roundToInt (proc.getParameterValue ("dith.bits")));
        const int depth = ps::kDitherBitDepth[(size_t) depthIndex];
        const bool on = proc.getParameterValue ("dith.on") >= 0.5f;

        auto labels = area.removeFromLeft (18.0f);
        area.removeFromBottom (16.0f);
        const float cellW = area.getWidth() / 24.0f;
        const float rowH = juce::jlimit (12.0f, 34.0f, (area.getHeight() - 6.0f) * 0.5f);
        area = area.withSizeKeepingCentre (area.getWidth(), rowH * 2.0f + 6.0f);
        const auto numbers = juce::Rectangle<float> (area.getX(), area.getBottom() + 2.0f, area.getWidth(), 14.0f);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto row = juce::Rectangle<float> (area.getX(), area.getY() + (float) ch * (rowH + 6.0f), area.getWidth(), rowH);
            const auto active = m.activeBits[ch].load (std::memory_order_relaxed);
            const auto peak = m.peakBits[ch].load (std::memory_order_relaxed);

            g.setFont (font (10.5f, 700));
            g.setColour (Colours::textSubtle);
            g.drawText (ch == 0 ? "L" : "R", labels.withY (row.getY()).withHeight (rowH), juce::Justification::centredLeft, false);

            for (int bit = 0; bit < 24; ++bit)
            {
                const auto cell = juce::Rectangle<float> (row.getX() + (float) bit * cellW, row.getY(), cellW, rowH).reduced (1.0f);
                const bool used = bit < (on ? depth : 24);
                // DitherMeters: bit k of the mask = bit k of the sample word, 0 = MSB.
                const bool lit = ((active >> bit) & 1u) != 0;
                const bool held = ((peak >> bit) & 1u) != 0;

                g.setColour (used ? juce::Colours::black.withAlpha (0.35f) : juce::Colours::black.withAlpha (0.15f));
                g.fillRoundedRectangle (cell, 2.0f);

                if (lit)
                {
                    g.setColour ((used ? Colours::accent : Colours::major).withAlpha (0.95f));
                    g.fillRoundedRectangle (cell, 2.0f);
                }
                else if (held)
                {
                    g.setColour ((used ? Colours::accent : Colours::major).withAlpha (0.3f));
                    g.fillRoundedRectangle (cell, 2.0f);
                }

                if (! used)
                {
                    g.setColour (Colours::outline.withAlpha (0.4f));
                    g.drawRoundedRectangle (cell.reduced (0.5f), 2.0f, 1.0f);
                }
            }
        }

        // Depth marker and bit numbers.
        const float x = area.getX() + (float) depth * cellW;

        if (on && depth < 24)
        {
            g.setColour (Colours::accent.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (x - 1.0f, area.getY() - 4.0f, 2.0f, rowH * 2.0f + 14.0f));
        }

        g.setFont (font (9.5f, 600));
        g.setColour (Colours::textSubtle);

        for (int bit : { 1, 4, 8, 12, 16, 20, 24 })
            g.drawText (juce::String (bit), juce::Rectangle<float> (area.getX() + (float) (bit - 1) * cellW - 6.0f, numbers.getY() + 1.0f, cellW + 12.0f, 12.0f),
                        juce::Justification::centred, false);
    }

    void paintDcMeter (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        const auto& m = proc.getChain().getDitherMeters();
        const float rowH = juce::jmin (20.0f, (area.getHeight() - 6.0f) * 0.5f);

        for (int ch = 0; ch < 2; ++ch)
        {
            const float dcValue = m.dcOffset[ch].load();
            auto row = juce::Rectangle<float> (area.getX(), area.getY() + (float) ch * (rowH + 6.0f), area.getWidth(), rowH);
            g.setFont (font (10.5f, 700));
            g.setColour (Colours::textSubtle);
            g.drawText (ch == 0 ? "L" : "R", row.removeFromLeft (18.0f), juce::Justification::centredLeft, false);

            const auto valueArea = row.removeFromRight (70.0f);
            const auto bar = row.withSizeKeepingCentre (row.getWidth(), 8.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (bar, 4.0f);

            // Log scale: -90 dBFS (centre) .. -20 dBFS (edge), signed.
            const float db = std::abs (dcValue) > 1.0e-9f ? 20.0f * std::log10 (std::abs (dcValue)) : -200.0f;
            const float t = juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 70.0f);
            const float cx = bar.getCentreX();
            const float x1 = cx + (dcValue >= 0.0f ? 1.0f : -1.0f) * t * bar.getWidth() * 0.5f;

            if (t > 0.0f)
            {
                g.setColour (db > -40.0f ? Colours::minor : Colours::accent);
                g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (cx, x1), bar.getY(), std::abs (x1 - cx), bar.getHeight()), 3.0f);
            }

            g.setColour (Colours::textFaint);
            g.fillRect (juce::Rectangle<float> (cx - 0.5f, bar.getY() - 3.0f, 1.0f, bar.getHeight() + 6.0f));

            g.setFont (font (11.0f, 700));
            g.setColour (db > -40.0f ? Colours::minor : Colours::text);
            g.drawText (db < -150.0f ? juce::String ("none") : juce::String (dcValue * 100.0f, 3) + " %", valueArea,
                        juce::Justification::centredRight, false);
        }
    }

    void paint (juce::Graphics& g) const
    {
        const auto b = owner.getLocalBounds().toFloat();
        drawPanel (g, b);

        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addRoundedRectangle (b.reduced (1.0f), kCornerRadius - 1.0f);
            g.reduceClipRegion (clip);
            g.setGradientFill (juce::ColourGradient::horizontal (Colours::accent.withAlpha (0.10f), b.getX(),
                                                                 Colours::accent.withAlpha (0.0f), b.getX() + b.getWidth() * 0.6f));
            g.fillRect (b.withHeight ((float) kHeaderH));
        }

        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (b.getX() + 1.0f, (float) kHeaderH, b.getWidth() - 2.0f, 1.0f));

        const auto dot = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ 17.0f, (float) kHeaderH * 0.5f + 1.0f });
        g.setColour (Colours::accent.withAlpha (0.3f));
        g.fillEllipse (dot.expanded (3.0f));
        g.setColour (Colours::accent);
        g.fillEllipse (dot);
        g.setFont (font (14.5f, 700));
        g.setColour (Colours::text);
        g.drawText ("Dither", juce::Rectangle<float> (30.0f, 1.0f, 200.0f, (float) kHeaderH), juce::Justification::centredLeft, false);
        g.setFont (font (11.5f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText ("Last stage of the output, after every module",
                    juce::Rectangle<float> (30.0f + textWidth (font (14.5f, 700), "Dither") + 14.0f, 1.0f, 320.0f, (float) kHeaderH),
                    juce::Justification::centredLeft, false);

        drawCard (g, settingsCard, "Settings");
        drawCard (g, bitCard, "Bit meter");
        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText ("click to clear the hold", bitCard.reduced (12.0f, 0.0f).withHeight (31.0f),
                    juce::Justification::centredRight, true);
        drawCard (g, dcCard, "DC offset");
        drawCard (g, spectrumCard, "Output spectrum");
        g.setFont (font (11.0f, 600));
        g.setColour (Colours::textSubtle);
        g.drawText ("the dither noise sits at the very bottom", spectrumCard.reduced (12.0f, 0.0f).withHeight (31.0f),
                    juce::Justification::centredRight, true);
        paintSpectrum (g, spectrumArea);

        g.setFont (font (11.5f, 600));
        g.setColour (Colours::textDim);
        g.drawText ("Bit depth", bitsLabel, juce::Justification::centredLeft, false);
        g.drawText ("Amount", amountLabel, juce::Justification::centredLeft, false);

        paintBitMeter (g, bitArea);
        paintDcMeter (g, dcArea);

        // Plain-language note.
        juce::AttributedString note;
        note.setWordWrap (juce::AttributedString::byWord);
        note.setLineSpacing (2.0f);
        note.append ("When to use it: ", font (12.0f, 700), Colours::text);
        note.append ("turn dither on only when you export at a lower bit depth than you work in (for example a 16 bit "
                     "WAV for CD). It adds a tiny noise so quiet parts and fade-outs stay smooth instead of turning "
                     "grainy. Use it once, as the very last step - never twice.", font (12.0f), Colours::textDim);
        juce::TextLayout layout;
        layout.createLayout (note, noteArea.getWidth());
        layout.draw (g, noteArea);
    }

    void layout()
    {
        auto r = owner.getLocalBounds();
        auto header = r.removeFromTop (kHeaderH).reduced (12, 0);
        reset.setBounds (header.removeFromRight (28).withSizeKeepingCentre (28, 26));
        header.removeFromRight (8);
        power.setBounds (header.removeFromRight (110).withSizeKeepingCentre (110, 24));

        r = r.withTrimmedTop (2).reduced (12, 10);
        const int noteH = r.getHeight() > 400 ? 50 : 0;
        noteArea = r.removeFromBottom (noteH).toFloat().reduced (4.0f, 2.0f);
        r.removeFromBottom (noteH > 0 ? 6 : 0);
        const int gap = 10, rowH = 26;

        // Row 1: settings (bit depth / amount / harmonic suppression | noise shaping | limit, blank, DC filter).
        auto settings = r.removeFromTop (32 + 3 * rowH + 2 * 8 + 12);
        settingsCard = settings.toFloat();
        r.removeFromTop (gap);
        auto inner = settings.reduced (12, 0).withTrimmedTop (32).withTrimmedBottom (12);
        auto colA = inner.removeFromLeft (inner.getWidth() * 47 / 100);
        inner.removeFromLeft (14);
        auto colC = inner.removeFromRight (juce::jmin (220, inner.getWidth() * 55 / 100));
        inner.removeFromRight (14);

        auto row = colA.removeFromTop (rowH);
        bitsLabel = row.removeFromLeft (76).toFloat();
        bits.setBounds (row);
        colA.removeFromTop (8);
        row = colA.removeFromTop (rowH);
        amountLabel = row.removeFromLeft (76).toFloat();
        amount.setBounds (row);
        colA.removeFromTop (8);
        harm.setBounds (colA.removeFromTop (rowH).withTrimmedLeft (76));

        shape.setBounds (inner.withSizeKeepingCentre (juce::jmin (inner.getWidth(), 110), inner.getHeight()));

        limit.setBounds (colC.removeFromTop (rowH));
        colC.removeFromTop (8);
        blank.setBounds (colC.removeFromTop (rowH));
        colC.removeFromTop (8);
        dc.setBounds (colC.removeFromTop (rowH));

        // Row 2: bit meter over DC offset (left), output spectrum (right).
        auto left = r.removeFromLeft ((r.getWidth() - gap) * 52 / 100);
        r.removeFromLeft (gap);
        spectrumCard = r.toFloat();
        spectrumArea = r.reduced (12, 0).withTrimmedTop (34).withTrimmedBottom (12).toFloat();

        const int dcH = 92;
        auto dcR = left.removeFromBottom (dcH);
        left.removeFromBottom (gap);
        bitCard = left.toFloat();
        bitArea = left.reduced (14, 0).withTrimmedTop (34).withTrimmedBottom (12).toFloat();
        dcCard = dcR.toFloat();
        dcArea = dcR.reduced (14, 0).withTrimmedTop (34).withTrimmedBottom (10).toFloat();
    }

    void paintSpectrum (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        auto& analyser = ctx.getServices().getOutputSpectrum();
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRoundedRectangle (area, 5.0f);
        const auto fx = makeFreqAxis (ctx.getUiOptions(), area, proc.getChain().getSampleRate());
        DbAxis dy;
        dy.minDb = -150.0f;
        dy.maxDb = -10.0f;
        dy.area = area;
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());
        drawFrequencyGrid (g, fx, dy, true);
        analyser.draw (g, fx, dy.minDb, dy.maxDb, Colours::accent, true);

        if (! analyser.hasData())
        {
            g.setFont (font (12.0f, 600));
            g.setColour (Colours::textSubtle);
            g.drawText ("Play audio to see the output", area, juce::Justification::centred, false);
        }
    }

    DitherPanel& owner;
    EditorContext& ctx;
    PureSheenProcessor& proc;

    ParamToggle power;
    ParamSegments bits, amount;
    ParamKnob shape;
    ParamToggle harm, limit, blank, dc;
    IconButton reset { "Reset dither", IconButton::Icon::reset };

    juce::Rectangle<float> settingsCard, bitCard, dcCard, spectrumCard, bitArea, dcArea, spectrumArea, bitsLabel, amountLabel, noteArea;
};

DitherPanel::DitherPanel (EditorContext& ctx) : impl (std::make_unique<Impl> (*this, ctx))
{
    setTitle ("Dither");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

DitherPanel::~DitherPanel() = default;

void DitherPanel::tick()
{
    // Harmonic suppression only applies with Amount = Off.
    const bool amountOff = juce::roundToInt (impl->proc.getParameterValue ("dith.amount")) == (int) ps::DitherAmount::Off;
    impl->harm.setEnabled (amountOff);
    repaint (impl->bitCard.getUnion (impl->dcCard).getUnion (impl->spectrumCard).toNearestInt());
}

void DitherPanel::paint (juce::Graphics& g) { impl->paint (g); }
void DitherPanel::resized() { impl->layout(); }

void DitherPanel::mouseUp (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && impl->bitCard.contains (e.position))
    {
        impl->proc.getChain().resetDitherPeakBits();
        repaint (impl->bitCard.toNearestInt());
    }
}

} // namespace ps::ui
