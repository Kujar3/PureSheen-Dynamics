#include "ui/MaximizerPanel.h"

#include "ui/Controls.h"
#include "ui/MaximizerWidgets.h"

#include "dsp/Maximizer.h"

#include <cmath>

// Maximizer panel (owner: ui-max). See MaximizerPanel.h for the layout.
//
// Where the levels come from (the editor is the single consumer of every chain FIFO, so this panel pulls none):
//   - gain reduction: MaximizerMeters (current L / R, and the peak since the last read via exchange())
//   - upward gain: MaximizerMeters::upwardGainDb
//   - module output: the chain's output peak meters (IoMeters::outputPeak, exchanged here; nothing else reads them),
//     minus the output gain faders. Exact while the Maximizer is the last module (its usual place).
//   - module input: derived from the limiter's gain structure (Output Level is a ceiling, not an output gain):
//     out = in + Gain + upward + GR, so in = out - Gain - upward - GR (Soft Clip / Transient Emphasis are not part
//     of GR, so the estimate reads a little high while they shave peaks).
// The history advances with the audio (output FIFO write count, read without consuming): one column per 1/30 s of
// audio, so it stands still when the host stops processing.
namespace ps::ui
{
namespace
{
constexpr float kSilenceDb = -200.0f;

const juce::StringArray kModeNames { "Low Latency", "Smooth", "Transparent", "Aggressive", "Spectral", "Multiband" };

const char* const kModeText[] = {
    "The lowest latency and CPU. A clean, fast limiter for tracking and live use.",
    "Reacts fast to transients and slowly to sustained bass. Smooth and musical.",
    "Like Smooth, but keeps more of the punch of the transients. A good default.",
    "The most loudness, with a choice of sound.",
    "Turns down only the frequency bands that cause each peak.",
    "Four bands with their own timing. The bands that cause each peak are reduced first.",
};

const char* const kAggressiveText[] = {
    "Clipping: the loudest, hardest sound. Fast peaks are shaved off.",
    "Crisp: bright and forward, keeps the high end lively.",
    "Balanced: an even mix of loudness and punch.",
    "Pumping: a slower release that lets the mix breathe with the beat.",
};

const char* const kSpectralText[] = {
    "Classic: smooth, even limiting across the bands.",
    "Modern: louder and tighter, keeps the low end firm.",
    "Transient: keeps the attack of drums and percussion.",
};

float toDb (float linear) noexcept
{
    return linear > 1.0e-10f && std::isfinite (linear) ? 20.0f * std::log10 (linear) : kSilenceDb;
}

void paintCard (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& shortTitle,
                int rightReserve)
{
    const auto b = area.toFloat().reduced (0.5f);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                       Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

    const auto f = font (11.0f, 700).withExtraKerningFactor (0.08f);
    auto titleArea = juce::Rectangle<int> (area.getX() + 12, area.getY(), area.getWidth() - 24 - rightReserve, 28);
    auto text = title.toUpperCase();

    if (shortTitle.isNotEmpty() && textWidth (f, text) > (float) titleArea.getWidth())
        text = shortTitle.toUpperCase();

    g.setFont (f);
    g.setColour (Colours::text.withAlpha (0.9f));
    g.drawFittedText (text, titleArea.translated (0, 1), juce::Justification::centredLeft, 1, 0.8f);
}
} // namespace

//==============================================================================
struct MaximizerPanel::Impl
{
    Impl (MaximizerPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), services (s), colour (moduleColour (ps::ModuleType::Maximizer)), levels (p),
          display (levels, colour), gainCol (MaxMeterColumn::Kind::gain, levels, colour),
          outCol (MaxMeterColumn::Kind::output, levels, colour),
          modes (p, "max.mode", kModeNames, 1, MaxChoiceGrid::Style::list, colour),
          aggressive (p, "max.style3", { "Clipping", "Crisp", "Balanced", "Pumping" }, 2, MaxChoiceGrid::Style::chips, colour),
          spectral (p, "max.style4", { "Classic", "Modern", "Transient" }, 2, MaxChoiceGrid::Style::chips, colour),
          link (p, "max.link", "Link"), truePeak (p, "max.truePeak", "True Peak"),
          character (p, "max.character", "Character"), upward (p, "max.upward", "Upward"),
          siTransient (p, "max.si.trans", "Transient"), siSustain (p, "max.si.sus", "Sustain"),
          clipOn (p, "max.clip.on", colour),
          clipMode (p, "max.clip.mode", { "Light", "Moderate", "Heavy" }, 1, MaxChoiceGrid::Style::chips, colour),
          clipAmount (p, "max.clip.amount", "Amount"),
          transOn (p, "max.trans.on", colour), transAmount (p, "max.trans.amount", "Amount"),
          learn (colour), learnTarget (p, "max.learnTarget", "Target"), trim (p, "max.trim", "Trim")
    {
        levels.onChange = [this] { pollLevels(); };

        juce::StringArray modeTips;
        for (int i = 0; i < kModeNames.size(); ++i)
            modeTips.add (kModeNames[i] + ": " + kModeText[i]);
        modes.setItemTooltips (modeTips);
        modes.setTooltip ("Limiter mode");

        juce::StringArray aggrTips, specTips;
        for (auto* t : kAggressiveText) aggrTips.add (t);
        for (auto* t : kSpectralText) specTips.add (t);
        aggressive.setItemTooltips (aggrTips);
        spectral.setItemTooltips (specTips);
        clipMode.setItemTooltips ({ "Light: drives the clipper +1.5 dB, rounds off only the peaks near full scale.",
                                    "Moderate: drives it +3 dB, softens from 2.5 dB below full scale, warmer.",
                                    "Heavy: drives it +6 dB, softens from 4 dB below full scale, strongly saturated." });

        for (auto* k : { &character, &upward, &siTransient, &siSustain, &clipAmount, &transAmount, &learnTarget, &trim })
            k->setColour (colour);

        link.setOnColour (colour);
        truePeak.setOnColour (colour);

        link.setTooltip ("Link Gain and Output Level: they move together, raising one lowers the other by the same "
                         "amount.");
        truePeak.setTooltip ("True Peak: also catches the peaks between samples (as a converter or an MP3 encoder "
                             "would see them), so the output never goes over Output Level.");
        character.setTooltip ("Character: how fast the limiter reacts. Low values are fast and clip-like, high values "
                              "are slow and smooth.\nDrag to change (Ctrl: fine), double-click to type a value, right-click to reset.");
        upward.setTooltip ("Upward Compress: lifts the quiet passages by up to this many dB before the limiter. "
                           "Full-scale peaks stay where they are.\nDrag to change (Ctrl: fine), double-click to type a value, right-click to reset.");
        siTransient.setTooltip ("Stereo Independence for transients: 0 % limits both channels together, higher "
                                "values let left and right react on their own (wider, less stable image).");
        siSustain.setTooltip ("Stereo Independence for sustained sounds: 0 % limits both channels together, higher "
                              "values let left and right react on their own.");
        clipOn.setTooltip ("Soft Clip on / off: rounds off the peaks before the limiter (louder, warmer).");
        clipAmount.setTooltip ("Soft Clip amount: how much of the clipped signal is mixed in.");
        transOn.setTooltip ("Transient Emphasis on / off: brings the attacks forward before limiting.");
        transAmount.setTooltip ("Transient Emphasis amount: how much the attacks are brought forward.");
        learnTarget.setTooltip ("Learn target: the loudness (LUFS) that Learn Input Gain aims for.\nDrag to change "
                                "(Ctrl: fine), double-click to type a value, right-click to reset.");
        trim.setTooltip ("Trim: turns the signal down before the limiter. Learn uses it for a track louder than its "
                         "target, such as a film mix going to -23 LUFS.\nDrag to change (Ctrl: fine), double-click to "
                         "type a value, right-click to reset.");

        learn.onClick = [this]
        {
            if (processor.isMaximizerLearning())
            {
                processor.cancelMaximizerLearn();
                sawLearning = false;
                owner.showHeaderHint ("Learn cancelled: Gain unchanged");
                return;
            }

            processor.startMaximizerLearn();
            owner.showHeaderHint ("Learning: play the loudest part of the song (click again to cancel)");
        };

        for (juce::Component* c : std::initializer_list<juce::Component*> {
                 &display, &gainCol, &outCol, &grCol, &modes, &aggressive, &spectral, &link, &truePeak, &character,
                 &upward, &siTransient, &siSustain, &clipOn, &clipMode, &clipAmount, &transOn, &transAmount, &learn,
                 &learnTarget, &trim })
        {
            owner.addAndMakeVisible (c);
            c->setWantsKeyboardFocus (false);
            c->setMouseClickGrabsKeyboardFocus (false);
        }

        // Discard whatever the meters collected before the panel opened.
        auto& chain = processor.getChain();
        meters().peakGainReductionDb.exchange (0.0f);
        chain.getIoMeters().outputPeak[0].exchange (0.0f);
        chain.getIoMeters().outputPeak[1].exchange (0.0f);
        lastWriteCount = chain.getOutputFifo().getWriteCount();

        pollLevels();
        updateModeVisibility();
    }

    ps::MaximizerMeters& meters()
    {
        auto& mx = static_cast<ps::Maximizer&> (processor.getChain().getModule (ps::ModuleType::Maximizer));
        return mx.getMeters();   // non-const: exchange() reads the peak since the last read
    }

    //==========================================================================
    void pollLevels()
    {
        const float gain = levels.getGain(), ceiling = levels.getCeiling();
        display.setLines (gain, ceiling);
        gainCol.setHandle (gain, ceiling);
        outCol.setHandle (gain, ceiling);
    }

    int currentMode() const
    {
        return juce::jlimit (0, kModeNames.size() - 1, juce::roundToInt (processor.getParameterValue ("max.mode")));
    }

    void updateModeVisibility()
    {
        const int mode = currentMode();
        shownMode = mode;
        aggressive.setVisible (mode == (int) ps::MaximizerMode::Aggressive);
        spectral.setVisible (mode == (int) ps::MaximizerMode::Spectral);
        layoutModeCard();
        owner.repaint (modeCard);
    }

    //==========================================================================
    void tick()
    {
        auto& chain = processor.getChain();
        auto& m = meters();
        auto& io = chain.getIoMeters();

        const float gain = levels.getGain();
        const bool active = processor.getParameterValue ("max.on") >= 0.5f && processor.getParameterValue ("io.bypass") < 0.5f;

        // Gain reduction (<= 0 dB; the sign convention of the peak meter is not assumed).
        auto grOf = [] (float v) { return std::isfinite (v) ? -std::abs (v) : 0.0f; };
        const float grL = juce::jmin (0.0f, grOf (m.gainReductionDb[0].load()));
        const float grR = juce::jmin (0.0f, grOf (m.gainReductionDb[1].load()));
        const float grPeak = active ? juce::jmin (grOf (m.peakGainReductionDb.exchange (0.0f)), grL, grR) : 0.0f;
        const float up = active ? juce::jmax (0.0f, std::isfinite (m.upwardGainDb.load()) ? m.upwardGainDb.load() : 0.0f) : 0.0f;

        // Module output (chain output peaks minus the output faders) and the input it implies.
        float out[2], in[2];
        const char* outGainIds[2] { "out.gainL", "out.gainR" };

        for (int c = 0; c < 2; ++c)
        {
            out[c] = toDb (io.outputPeak[c].exchange (0.0f));

            if (out[c] > kSilenceDb)
                out[c] -= processor.getParameterValue (outGainIds[c]);

            const float grc = juce::jmin (grPeak, c == 0 ? grL : grR);
            in[c] = out[c] <= kSilenceDb ? kSilenceDb : (active ? out[c] - gain - up - grc : out[c]);
        }

        gainCol.setLevels (in[0], in[1]);
        outCol.setLevels (out[0], out[1]);
        grCol.setGainReduction (active ? grL : 0.0f, active ? grR : 0.0f, grPeak);

        // History: one column per 1/30 s of processed audio.
        const auto writeCount = chain.getOutputFifo().getWriteCount();
        const auto newSamples = writeCount >= lastWriteCount ? writeCount - lastWriteCount : 0ULL;
        lastWriteCount = writeCount;
        const double sr = chain.getSampleRate();
        bool pushed = false;

        if (sr > 0.0 && newSamples > 0)
        {
            const double columnLength = sr / (double) MaxHistoryDisplay::kColumnsPerSecond;
            sampleAccumulator += (double) newSamples;

            if (sampleAccumulator > columnLength * 8.0)   // after a stall: do not flood the history
                sampleAccumulator = columnLength;

            MaxHistoryEntry e;
            e.outputDb = juce::jmax (out[0], out[1]);
            e.unlimitedDb = e.outputDb <= kSilenceDb ? kSilenceDb : e.outputDb - grPeak;
            e.grDb = grPeak;

            while (sampleAccumulator >= columnLength)
            {
                sampleAccumulator -= columnLength;
                display.push (e);
                pushed = true;
            }
        }

        // Parameters (host automation, undo, presets).
        pollLevels();
        modes.refreshValue();
        aggressive.refreshValue();
        spectral.refreshValue();
        clipMode.refreshValue();
        clipOn.refreshValue();
        transOn.refreshValue();

        if (currentMode() != shownMode)
            updateModeVisibility();

        // Mode card stats: the module's latency and the current upward lift.
        {
            const auto& mx = processor.getChain().getModule (ps::ModuleType::Maximizer);
            const float latencyMs = sr > 0.0 ? (float) (1000.0 * (double) mx.getLatencySamples() / sr) : 0.0f;

            if (! juce::approximatelyEqual (latencyMs, shownLatencyMs) || std::abs (up - shownUpward) > 0.05f)
            {
                shownLatencyMs = latencyMs;
                shownUpward = up;
                owner.repaint (statsArea);
            }
        }

        // Header loudness readout (output, short-term).
        const float st = io.outputShortTermLufs.load();
        display.setLegendRight (st > -150.0f ? "Short-term  " + fmt::lufs (st) : juce::String());

        tickLearn();

        if (pushed)
            display.repaint();
    }

    void tickLearn()
    {
        const bool learning = processor.isMaximizerLearning();

        if (learning)
        {
            const float progress = juce::jlimit (0.0f, 1.0f, processor.getMaximizerLearnProgress());
            sawLearning = true;
            learn.setState (MaxLearnButton::State::learning, progress);
            display.setStatus ("Listening to the input...  " + juce::String (juce::roundToInt (progress * 100.0f)) + " %");
            return;
        }

        if (sawLearning)
        {
            sawLearning = false;
            doneTicks = 75;
            hintCountdown = 4;   // the processor writes the learned gain on its next housekeeping pass
        }

        if (hintCountdown > 0 && --hintCountdown == 0)
            owner.showHeaderHint ("Learn set Gain to " + fmt::db (levels.getGain()));

        learn.setState (doneTicks > 0 ? MaxLearnButton::State::done : MaxLearnButton::State::idle, 1.0f);
        doneTicks = juce::jmax (0, doneTicks - 1);
        display.setStatus ({});
    }

    //==========================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto r = content.reduced (12, 0).withTrimmedTop (10).withTrimmedBottom (12);
        const int W = r.getWidth(), H = r.getHeight();
        const float sw = juce::jlimit (0.0f, 1.0f, (float) (W - 740) / 800.0f);
        const float sh = juce::jlimit (0.0f, 1.0f, (float) (H - 400) / 500.0f);

        const int cardH = juce::roundToInt (118.0f + 38.0f * sh);
        auto bottom = r.removeFromBottom (cardH);
        r.removeFromBottom (10);
        auto top = r;

        const int headerH = juce::roundToInt (46.0f + 10.0f * sh);
        const int footerH = 30;
        const int modeW = juce::roundToInt (128.0f + 72.0f * sw);
        const int gainW = juce::roundToInt (78.0f + 28.0f * sw);
        const int grW = juce::roundToInt (44.0f + 8.0f * sw);
        const int outW = juce::roundToInt (84.0f + 22.0f * sw);

        modeCard = top.removeFromRight (modeW);
        top.removeFromRight (12);
        auto gainArea = top.removeFromLeft (gainW);
        top.removeFromLeft (10);
        auto outArea = top.removeFromRight (outW);
        top.removeFromRight (6);
        auto grArea = top.removeFromRight (grW);
        top.removeFromRight (8);
        auto displayArea = top;

        const int plotBottom = top.getHeight() - footerH;
        MaxLevelScale scale;
        scale.top = (float) headerH;
        scale.bottom = (float) plotBottom;
        scale.minDb = plotBottom - headerH < 230 ? -30.0f : -36.0f;
        scale.maxDb = 6.0f;

        display.setBounds (displayArea);
        display.setScale (scale);
        display.setSeconds (5.0f);

        for (auto* col : { &gainCol, &outCol })
            col->setHeaderHeight (headerH);

        gainCol.setBounds (gainArea.withHeight (plotBottom));
        outCol.setBounds (outArea.withHeight (plotBottom));
        grCol.setBounds (grArea.withHeight (plotBottom));
        grCol.setHeaderHeight (headerH);
        gainCol.setScale (scale);
        outCol.setScale (scale);
        grCol.setScale (scale);

        const int toggleY = gainArea.getY() + plotBottom + 6;
        link.setBounds (gainArea.getX(), toggleY, gainArea.getWidth(), 22);
        truePeak.setBounds (outArea.getX() - 4, toggleY, outArea.getWidth() + 4, 22);

        layoutModeCard();
        layoutCards (bottom);
    }

    void layoutModeCard()
    {
        if (modeCard.isEmpty())
            return;

        auto inner = modeCard.reduced (8, 0).withTrimmedTop (30).withTrimmedBottom (8);
        const int rowH = juce::jlimit (20, 30, (int) ((float) (modeCard.getHeight() - 40) * 0.6f / 6.0f));
        const int listH = 6 * rowH + 5 * 3;
        modes.setBounds (inner.removeFromTop (listH));
        inner.removeFromTop (8);

        styleTitleArea = {};
        const int mode = currentMode();
        auto* styles = mode == (int) ps::MaximizerMode::Aggressive ? &aggressive
                                                                    : (mode == (int) ps::MaximizerMode::Spectral ? &spectral : nullptr);

        if (styles != nullptr && inner.getHeight() >= 60)
        {
            styleTitleArea = inner.removeFromTop (16).reduced (4, 0);
            inner.removeFromTop (4);
            const int chipH = juce::jlimit (20, 26, rowH - 2);
            const int rows = (styles->getNumItems() + 1) / 2;
            styles->setColumns (2);
            styles->setBounds (inner.removeFromTop (rows * chipH + (rows - 1) * 4).reduced (2, 0));
            inner.removeFromTop (8);
        }
        else if (styles != nullptr)
        {
            // Very short panel: the chips replace the summary text.
            styles->setColumns (styles->getNumItems());
            styles->setBounds (inner.removeFromTop (juce::jmin (inner.getHeight(), 22)).reduced (2, 0));
        }

        statsArea = {};

        if (inner.getHeight() >= 110)
            statsArea = inner.removeFromBottom (46).reduced (4, 0);

        descriptionArea = inner.reduced (4, 0);
    }

    void layoutCards (juce::Rectangle<int> row)
    {
        constexpr int gap = 8, pad = 10, titleH = 28;
        const int avail = row.getWidth() - 4 * gap;
        int kw = 66;
        const int chipsW = 80, ringW = 64;
        std::array<int, 5> ideal {};

        auto computeIdeal = [&]
        {
            ideal = { 2 * kw + 6 + 2 * pad, 2 * kw + 6 + 2 * pad, chipsW + 8 + kw + 2 * pad, juce::jmax (kw + 2 * pad, 136),
                      ringW + 8 + 2 * kw + 6 + 2 * pad };
            int sum = 0;
            for (int w : ideal) sum += w;
            return sum;
        };

        while (computeIdeal() > avail && kw > 52)
            --kw;

        const int sum = computeIdeal();
        const int extra = juce::jmax (0, avail - sum);
        int x = row.getX();

        for (size_t i = 0; i < cards.size(); ++i)
        {
            int w = ideal[i] + (sum > 0 ? extra * ideal[i] / sum : 0);

            if (i + 1 == cards.size())
                w = row.getRight() - x;

            cards[i] = { x, row.getY(), w, row.getHeight() };
            x += w + gap;
        }

        auto body = [&] (size_t i) { return cards[i].withTrimmedTop (titleH).reduced (pad, 0).withTrimmedBottom (8); };
        auto knobIn = [] (juce::Rectangle<int> cell) { return cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), 96), cell.getHeight()); };
        auto twoKnobs = [&] (juce::Rectangle<int> b, juce::Component& a, juce::Component& c)
        {
            const int cw = (b.getWidth() - 6) / 2;
            a.setBounds (knobIn (b.removeFromLeft (cw)));
            b.removeFromLeft (6);
            c.setBounds (knobIn (b));
        };

        twoKnobs (body (0), character, upward);
        twoKnobs (body (1), siTransient, siSustain);

        {
            auto b = body (2);
            const int cw = juce::jmin (b.getWidth() - kw - 8, chipsW + (b.getWidth() - (chipsW + 8 + kw)) / 3);
            const int chipsH = juce::jmin (b.getHeight() - 6, 3 * 26 + 2 * 4);
            clipMode.setBounds (b.removeFromLeft (cw).withSizeKeepingCentre (cw, chipsH));
            b.removeFromLeft (8);
            clipAmount.setBounds (knobIn (b));
        }

        transAmount.setBounds (knobIn (body (3)));

        {
            auto b = body (4);
            const int cw = juce::jmin (ringW, (b.getWidth() - 16) / 3);
            learn.setBounds (b.removeFromLeft (cw).withSizeKeepingCentre (juce::jmin (cw, 90), b.getHeight()));
            b.removeFromLeft (8);
            twoKnobs (b, learnTarget, trim);
        }

        auto switchArea = [&] (size_t i) { return juce::Rectangle<int> (cards[i].getRight() - pad - 32, cards[i].getY() + 6, 32, 18); };
        clipOn.setBounds (switchArea (2));
        transOn.setBounds (switchArea (3));
    }

    //==========================================================================
    void paint (juce::Graphics& g)
    {
        if (modeCard.isEmpty())
            return;

        paintCard (g, modeCard, "Mode", {}, 0);

        static const char* const titles[] = { "Limiter", "Stereo Independence", "Soft Clip", "Transient Emphasis", "Learn Input Gain" };
        static const char* const shortTitles[] = { "", "Stereo", "", "Transients", "Learn" };
        static const int reserve[] = { 0, 0, 36, 36, 0 };

        for (size_t i = 0; i < cards.size(); ++i)
            paintCard (g, cards[i], titles[i], shortTitles[i], reserve[i]);

        const int mode = currentMode();

        if (! styleTitleArea.isEmpty())
        {
            const auto f = font (10.5f, 600).withExtraKerningFactor (0.06f);
            const juce::String t ("STYLE");
            const int w = (int) textWidth (f, t) + 4;
            const auto a = styleTitleArea;
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.fillRect (a.getX(), a.getCentreY(), 6, 1);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (t, a.withTrimmedLeft (10), juce::Justification::centredLeft, false);
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.fillRect (a.getX() + 14 + w, a.getCentreY(), juce::jmax (0, a.getWidth() - 14 - w), 1);
        }

        if (! statsArea.isEmpty())
        {
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.fillRect (statsArea.getX(), statsArea.getY(), statsArea.getWidth(), 1);
            auto rows = statsArea.withTrimmedTop (6);

            auto row = [&] (const juce::String& name, const juce::String& value)
            {
                auto r = rows.removeFromTop (18);
                g.setFont (font (11.0f, 600));
                g.setColour (Colours::textSubtle);
                g.drawText (name, r, juce::Justification::centredLeft, false);
                g.setFont (font (11.5f, 700));
                g.setColour (Colours::text.withAlpha (0.9f));
                g.drawText (value, r, juce::Justification::centredRight, false);
            };

            row ("Latency", fmt::ms (shownLatencyMs));
            row ("Upward lift", fmt::db (shownUpward));
        }

        // Summary of the selected mode (and style) in the space left.
        if (descriptionArea.getHeight() >= 28 && descriptionArea.getWidth() > 60)
        {
            juce::String text (kModeText[mode]);

            if (mode == (int) ps::MaximizerMode::Aggressive)
                text = kAggressiveText[aggressive.getSelectedIndex()];
            else if (mode == (int) ps::MaximizerMode::Spectral)
                text = kSpectralText[spectral.getSelectedIndex()];

            const auto f = font (11.5f);
            const int lines = juce::jmax (1, descriptionArea.getHeight() / 15);
            g.setFont (f);
            g.setColour (Colours::textSubtle);
            g.drawFittedText (text, descriptionArea, juce::Justification::topLeft, lines, 0.9f);
        }
    }

    MaximizerPanel& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Colour colour;

    MaxLevelEditor levels;
    MaxHistoryDisplay display;
    MaxMeterColumn gainCol, outCol;
    MaxGrColumn grCol;
    MaxChoiceGrid modes, aggressive, spectral;
    ParamToggle link, truePeak;
    ParamKnob character, upward, siTransient, siSustain;
    MaxSwitch clipOn;
    MaxChoiceGrid clipMode;
    ParamKnob clipAmount;
    MaxSwitch transOn;
    ParamKnob transAmount;
    MaxLearnButton learn;
    ParamKnob learnTarget, trim;

    juce::Rectangle<int> modeCard, styleTitleArea, descriptionArea, statsArea;
    float shownLatencyMs = 0.0f, shownUpward = 0.0f;
    std::array<juce::Rectangle<int>, 5> cards {};
    int shownMode = -1;
    unsigned long long lastWriteCount = 0;
    double sampleAccumulator = 0.0;
    bool sawLearning = false;
    int doneTicks = 0, hintCountdown = 0;
};

//==============================================================================
MaximizerPanel::MaximizerPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::Maximizer), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

MaximizerPanel::~MaximizerPanel() = default;

void MaximizerPanel::tick()
{
    ModulePanel::tick();
    impl->tick();
}

void MaximizerPanel::refresh()
{
    ModulePanel::refresh();
    impl->pollLevels();
    impl->updateModeVisibility();
}

void MaximizerPanel::paint (juce::Graphics& g)
{
    ModulePanel::paint (g);
    impl->paint (g);
}

int MaximizerPanel::getNumControls() const noexcept
{
    // gain (column + display line), output level (column + display line), link, true peak, mode, two style sets,
    // character, upward, stereo independence x2, soft clip on / mode / amount, transient emphasis on / amount,
    // learn target
    return 17;
}

void MaximizerPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    impl->layout (contentArea);
}

} // namespace ps::ui
