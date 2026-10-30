#include "ui/MatchEqPanel.h"

#include "ui/Controls.h"
#include "ui/MatchDisplay.h"
#include "ui/MatchEqModel.h"

#include <cmath>

// Match EQ panel (owner: ui-meq). See MatchEqPanel.h.
//
// Layout (content area):  [ display ........................................................ ]
//                         [ REFERENCE ][ CURRENT ][ FINE TUNE (4 or 6 knobs) ][ FILTER ]
// The card buttons shrink to icons (with tooltips) when a card is narrow (980x600).
//
// Spectrum actions go straight to the module instance(s) (ps::MatchEq::startCapture / stopCapture / clearSpectrum);
// the processor turns every spectrum change into one undo step at its next housekeeping tick. Only one capture runs
// per instance: starting the other role's capture first stops (and keeps) the running one.
namespace ps::ui
{
namespace
{
constexpr int kGap = 10;
constexpr int kTitleH = 28;
constexpr int kButtonH = 26;
constexpr int kRowH = 22;

void noFocus (juce::Component& c)
{
    c.setWantsKeyboardFocus (false);
    c.setMouseClickGrabsKeyboardFocus (false);
}

//==============================================================================
// Card button: rounded chip with an icon and (when there is room) a label. Left clicks only; `on` = filled with the
// accent (Capture while capturing reads "Stop").
class ActionButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Icon { record, stop, folder, clear, cancel };

    ActionButton (const juce::String& t, Icon i) : text (t), icon (i)
    {
        noFocus (*this);
        setRepaintsOnMouseActivity (true);
    }

    void setText (const juce::String& t)  { if (t != text) { text = t; repaint(); } }
    void setIcon (Icon i)                 { if (i != icon) { icon = i; repaint(); } }
    void setOn (bool o)                   { if (o != on) { on = o; repaint(); } }
    void setColour (juce::Colour c)       { if (c != colour) { colour = c; repaint(); } }
    void setIconOnly (bool b)             { if (b != iconOnly) { iconOnly = b; repaint(); } }

    int idealWidth (bool withText) const
    {
        return withText ? (int) std::ceil (textWidth (font (11.5f, 600), text)) + 35 : kButtonH + 4;
    }

    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool enabled = isEnabled();
        const bool hover = enabled && isMouseOver (true);
        const bool down = hover && isMouseButtonDown();

        juce::Colour fill = Colours::panelRaised, outline = Colours::outline;

        if (on)
        {
            fill = blend (Colours::panelRaised, colour, down ? 0.42f : (hover ? 0.36f : 0.3f));
            outline = colour.withAlpha (0.85f);
        }
        else if (hover)
        {
            fill = down ? Colours::panelHover.darker (0.1f) : Colours::panelHover;
            outline = Colours::outline.brighter (0.2f);
        }

        if (! enabled)
            fill = fill.withMultipliedAlpha (0.5f), outline = outline.withMultipliedAlpha (0.5f);

        drawChip (g, b, 6.0f, fill, outline);

        const auto textColour = ! enabled ? Colours::textFaint
                                          : (on ? Colours::text : (hover ? Colours::text : Colours::textDim.brighter (0.1f)));
        const auto f = font (11.5f, 600);
        const float iconSize = 12.0f;
        const bool showText = ! iconOnly && text.isNotEmpty();
        const float contentW = iconSize + (showText ? 7.0f + textWidth (f, text) : 0.0f);
        float x = b.getCentreX() - contentW * 0.5f;

        if (showText && contentW > b.getWidth() - 12.0f)
            x = b.getX() + 8.0f;

        const auto iconBox = juce::Rectangle<float> (iconSize, iconSize).withPosition (x, b.getCentreY() - iconSize * 0.5f);
        paintIcon (g, iconBox, enabled ? (icon == Icon::record || icon == Icon::stop ? colour : textColour) : Colours::textFaint,
                   enabled);

        if (showText)
        {
            g.setFont (f);
            g.setColour (textColour);
            g.drawFittedText (text, juce::Rectangle<float> (iconBox.getRight() + 7.0f, b.getY(), b.getRight() - iconBox.getRight() - 11.0f,
                                                            b.getHeight()).toNearestInt(),
                              juce::Justification::centredLeft, 1, 0.8f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || ! isLeftClick (e) || ! getLocalBounds().contains (e.getPosition()))
            return;

        if (onClick != nullptr)
            onClick();
    }

private:
    void paintIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool enabled) const
    {
        const auto centre = r.getCentre();
        g.setColour (c);

        switch (icon)
        {
            case Icon::record:
            {
                const auto dot = juce::Rectangle<float> (9.0f, 9.0f).withCentre (centre);

                if (enabled)
                {
                    g.setColour (c.withAlpha (0.25f));
                    g.fillEllipse (dot.expanded (2.0f));
                    g.setColour (c);
                }

                g.fillEllipse (dot);
                break;
            }
            case Icon::stop:
                g.fillRoundedRectangle (juce::Rectangle<float> (9.0f, 9.0f).withCentre (centre), 2.0f);
                break;
            case Icon::folder:
            {
                juce::Path p;
                const auto f = r.reduced (0.5f, 1.5f);
                p.startNewSubPath (f.getX(), f.getBottom());
                p.lineTo (f.getX(), f.getY());
                p.lineTo (f.getX() + f.getWidth() * 0.4f, f.getY());
                p.lineTo (f.getX() + f.getWidth() * 0.52f, f.getY() + 2.0f);
                p.lineTo (f.getRight(), f.getY() + 2.0f);
                p.lineTo (f.getRight(), f.getBottom());
                p.closeSubPath();
                g.strokePath (p, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            case Icon::clear:
            case Icon::cancel:
            {
                const auto x = juce::Rectangle<float> (8.0f, 8.0f).withCentre (centre);
                juce::Path p;
                p.startNewSubPath (x.getTopLeft());
                p.lineTo (x.getBottomRight());
                p.startNewSubPath (x.getTopRight());
                p.lineTo (x.getBottomLeft());
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
        }
    }

    juce::String text;
    Icon icon;
    bool on = false, iconOnly = false;
    juce::Colour colour = Colours::accent;
};

void drawCard (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle,
               juce::Colour swatch, juce::Colour subtitleColour)
{
    if (area.isEmpty())
        return;

    const auto b = area.toFloat().reduced (0.5f);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                       Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Colours::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

    auto row = juce::Rectangle<int> (area.getX() + 12, area.getY(), area.getWidth() - 24, kTitleH);

    if (! swatch.isTransparent())
    {
        const auto line = row.removeFromLeft (12).toFloat().withSizeKeepingCentre (12.0f, 3.0f).translated (0.0f, 1.0f);
        g.setColour (swatch);
        g.fillRoundedRectangle (line, 1.5f);
        row.removeFromLeft (7);
    }

    const auto titleFont = font (11.0f, 700).withExtraKerningFactor (0.08f);
    const auto upper = title.toUpperCase();
    g.setFont (titleFont);
    g.setColour (Colours::text.withAlpha (0.9f));
    g.drawText (upper, row.translated (0, 1), juce::Justification::centredLeft, true);

    const auto rest = row.withTrimmedLeft ((int) textWidth (titleFont, upper) + 10);
    const auto subFont = font (10.5f, 600);

    if (subtitle.isNotEmpty() && textWidth (subFont, subtitle) <= (float) rest.getWidth())
    {
        g.setFont (subFont);
        g.setColour (subtitleColour);
        g.drawText (subtitle, rest.translated (0, 1), juce::Justification::centredRight, false);
    }
}

juce::String secondsText (double s)
{
    if (s >= 600.0)
        return juce::String (juce::roundToInt (s / 60.0)) + " min";

    if (s >= 90.0)
    {
        const int total = juce::roundToInt (s);
        return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2) + " min";
    }

    return (s < 10.0 ? juce::String (s, 1) : juce::String (juce::roundToInt (s))) + " s";
}

juce::String rateText (double sr)
{
    if (sr <= 0.0)
        return {};

    const double k = sr / 1000.0;
    return (std::abs (k - std::round (k)) < 0.05 ? juce::String (juce::roundToInt (k)) : juce::String (k, 1)) + " kHz";
}
} // namespace

//==============================================================================
struct MatchEqPanel::Impl
{
    Impl (MatchEqPanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), services (s), model (p), display (model, s),
          refCapture ("Capture", ActionButton::Icon::record), refLoad ("Load file...", ActionButton::Icon::folder),
          refClear ("Clear", ActionButton::Icon::clear), refCancel ("Cancel", ActionButton::Icon::cancel),
          curCapture ("Capture", ActionButton::Icon::record), curClear ("Clear", ActionButton::Icon::clear),
          amount (p, "meq.a.amount", "Amount"), smooth (p, "meq.a.smooth", "Smoothing"),
          low (p, "meq.a.lo", "Low Cutoff"), high (p, "meq.a.hi", "High Cutoff"),
          boost (p, "meq.a.boost", "Limit Boost"), cut (p, "meq.a.cut", "Limit Cut"),
          phase (p, "meq.phase", { "Linear", "Minimum" }), firSize (p, "meq.firSize", "FIR size")
    {
        refCapture.setColour (MatchDisplay::referenceColour());
        curCapture.setColour (MatchDisplay::currentColour());

        refCapture.onClick = [this] { toggleCapture (ps::MatchRole::Reference); };
        curCapture.onClick = [this] { toggleCapture (ps::MatchRole::Current); };
        refClear.onClick = [this] { clear (ps::MatchRole::Reference); };
        curClear.onClick = [this] { clear (ps::MatchRole::Current); };
        refLoad.onClick = [this] { chooseFile(); };
        refCancel.onClick = [this] { cancelLoad(); };

        refLoad.setTooltip ("Load a reference from an audio file (WAV, AIFF, FLAC, MP3, Ogg). Its long-term spectrum "
                            "becomes the Reference.");
        refClear.setTooltip ("Clear the Reference spectrum (undoable)");
        refCancel.setTooltip ("Stop analysing the file (the current Reference stays)");
        curClear.setTooltip ("Clear the Current spectrum (undoable)");

        amount.setTooltip ("How much of the matched curve is applied: 0 % = none, 100 % = the full difference. Values "
                           "under 50 % usually sound most natural.");
        smooth.setTooltip ("Smoothing of both spectra before they are compared: low = precise (narrow peaks and dips), "
                           "high = broad, gentle tonal moves.");
        low.setTooltip ("Low cutoff: below this frequency the curve fades to 0 dB (also the left handle on the display).");
        high.setTooltip ("High cutoff: above this frequency the curve fades to 0 dB (also the right handle on the display).");
        boost.setTooltip ("Limit boost: the most the curve may raise any frequency (soft limit).");
        cut.setTooltip ("Limit cut: the most the curve may lower any frequency (soft limit).");
        phase.setTooltip ("Linear: no phase shift, more latency. Minimum: almost no latency, natural phase shift like "
                          "an analog EQ.");
        firSize.setTooltip ("Filter length: longer = more precise in the low end, more latency (Linear phase).");

        for (auto* k : { &amount, &smooth, &low, &high, &boost, &cut })
            k->setCompact (true);

        for (juce::Component* c : std::initializer_list<juce::Component*> { &display, &refCapture, &refLoad, &refClear, &refCancel,
                                                                             &curCapture, &curClear, &amount, &smooth, &low, &high,
                                                                             &boost, &cut, &phase, &firSize })
        {
            noFocus (*c);
            owner.addAndMakeVisible (c);
        }

        refCancel.setVisible (false);
        model.update (owner.getChannelSet());
        rebind();
        updateButtons();
    }

    ~Impl()
    {
        chooser.reset();
    }

    //==========================================================================
    // Bindings
    void rebind()
    {
        const int set = model.getSet();
        const auto colour = model.setColour();
        const bool linked = model.isLinked();

        auto bind = [&] (ParamKnob& k, const char* name)
        {
            k.setParamId (model.setId (set, name));
            k.setLinkedParamId (linked ? model.setId (1 - set, name) : juce::String());
            k.setColour (colour);
        };

        bind (amount, "amount");
        bind (smooth, "smooth");
        bind (low, "lo");
        bind (high, "hi");
        bind (boost, "boost");
        bind (cut, "cut");
        phase.setOnColour (moduleColour (ps::ModuleType::MatchEq));
        owner.repaint();
    }

    //==========================================================================
    // Spectrum actions
    void toggleCapture (ps::MatchRole role)
    {
        const auto parts = model.partsForSpectrumActions();
        ps::MatchRole running = ps::MatchRole::Reference;
        const bool stopping = model.shownModule().isCapturing (&running) && running == role;

        bool tooShort = false;

        for (int part : parts)
        {
            auto& m = model.module (part);
            ps::MatchRole r = ps::MatchRole::Reference;

            if (m.isCapturing (&r))
                tooShort = ! stopKeepingPrevious (m, r) || tooShort;   // the other role's capture is kept too

            if (! stopping)
                m.startCapture (role);
        }

        if (tooShort)
        {
            services.showHint ("Nothing captured: play at least 1 s of audio through the plug-in while capturing. "
                               "The previous spectrum is kept.");
        }
        else if (! stopping)
        {
            const bool ref = role == ps::MatchRole::Reference;
            services.showHint (ref ? "Play the reference track now, then press Stop"
                                   : "Play your track through the plug-in now, then press Stop");
        }

        refresh();
    }

    // Stops the capture of `m` (role r). A capture with less than 1 s of audible audio publishes an empty spectrum; then
    // the previous spectrum is put back (in the same message-thread call, so the processor records no undo step).
    // Returns false when the capture was too short.
    static bool stopKeepingPrevious (ps::MatchEq& m, ps::MatchRole r)
    {
        const auto previous = m.getSpectrum (r);
        m.stopCapture();

        if (m.getSpectrum (r).valid)
            return true;

        if (previous.valid)
            m.setSpectrum (r, previous);

        return false;
    }

    void clear (ps::MatchRole role)
    {
        for (int part : model.partsForSpectrumActions())
        {
            auto& m = model.module (part);
            ps::MatchRole r = ps::MatchRole::Reference;

            if (m.isCapturing (&r) && r == role)
                m.stopCapture();

            m.clearSpectrum (role);
        }

        if (role == ps::MatchRole::Reference)
            loadError.clear();

        refresh();
    }

    void chooseFile()
    {
        if (model.isLoading() || chooser != nullptr)
            return;

        static juce::File lastFolder;   // session memory of the last folder
        const auto start = lastFolder.isDirectory() ? lastFolder
                                                    : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        chooser = std::make_unique<juce::FileChooser> ("Load a reference track", start,
                                                       "*.wav;*.wave;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        juce::Component::SafePointer<MatchEqPanel> safe (&owner);

        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr)
                return;

            auto& d = *safe->impl;
            const auto file = fc.getResult();

            if (file.existsAsFile())
            {
                lastFolder = file.getParentDirectory();
                d.startLoad (file);
            }

            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr)
                    safe->impl->chooser.reset();
            });
        });
    }

    void startLoad (const juce::File& file)
    {
        loadError.clear();
        const int part = model.getPart();

        if (! processor.startMatchReferenceFile (file, part, model.partsForSpectrumActions().size() > 1))
        {
            loadError = processor.getMatchReferenceFileError();

            if (loadError.isEmpty())
                loadError = "This file cannot be read";

            services.showHint (loadError);
            refresh();
            return;
        }

        loadWatch = true;
        loadWatchTicks = 0;
        loadFileName = file.getFileNameWithoutExtension();
        refresh();
    }

    void cancelLoad()
    {
        processor.cancelMatchReferenceFile();
        loadWatch = false;
        refresh();
    }

    // Display the loader's result. The processor owns linked publication, including when this panel is closed.
    void watchLoad()
    {
        if (loadWatch)
        {
            ++loadWatchTicks;

            if (! model.isLoading() && loadWatchTicks > 2)
            {
                loadWatch = false;
                loadError = processor.getMatchReferenceFileError();

                if (loadError.isNotEmpty())
                {
                    services.showHint (loadError);
                }
            }
        }

    }

    //==========================================================================
    // Card state
    void updateButtons()
    {
        const bool capturing = model.isCapturing();
        const bool capRef = capturing && model.getCaptureRole() == ps::MatchRole::Reference;
        const bool capCur = capturing && model.getCaptureRole() == ps::MatchRole::Current;
        const bool loading = model.isLoading();

        refCapture.setOn (capRef);
        refCapture.setText (capRef ? "Stop" : "Capture");
        refCapture.setIcon (capRef ? ActionButton::Icon::stop : ActionButton::Icon::record);
        refCapture.setTooltip (capRef ? "Stop capturing: the average so far becomes the Reference"
                                      : "Capture the Reference: press, play the reference track through the plug-in, "
                                        "then press Stop");
        refCapture.setEnabled (! loading);

        curCapture.setOn (capCur);
        curCapture.setText (capCur ? "Stop" : "Capture");
        curCapture.setIcon (capCur ? ActionButton::Icon::stop : ActionButton::Icon::record);
        curCapture.setTooltip (capCur ? "Stop capturing: the average so far becomes the Current spectrum"
                                      : "Capture the Current spectrum: press, play your track through the plug-in, "
                                        "then press Stop");

        refLoad.setEnabled (! loading && chooser == nullptr);
        refClear.setEnabled (! loading && (model.has (ps::MatchRole::Reference) || capRef));
        curClear.setEnabled (model.has (ps::MatchRole::Current) || capCur);

        if (loading != refCancel.isVisible())
        {
            refCancel.setVisible (loading);
            layout();
        }
    }

    juce::Colour roleColour (ps::MatchRole r) const
    {
        return r == ps::MatchRole::Reference ? MatchDisplay::referenceColour() : MatchDisplay::currentColour();
    }

    // Status word in a card's title row.
    juce::String roleStatus (ps::MatchRole r, juce::Colour& colourOut) const
    {
        colourOut = Colours::textSubtle;

        if (model.isCapturing() && model.getCaptureRole() == r)
        {
            colourOut = roleColour (r);
            return "Capturing";
        }

        if (r == ps::MatchRole::Reference && model.isLoading())
        {
            colourOut = roleColour (r);
            return "Loading";
        }

        if (model.has (r))
        {
            colourOut = Colours::good;
            return "Ready";
        }

        return "Empty";
    }

    void paintRoleInfo (juce::Graphics& g, ps::MatchRole r, juce::Rectangle<int> area) const
    {
        if (area.getHeight() < 12)
            return;

        const bool ref = r == ps::MatchRole::Reference;
        const auto& s = model.spectrum (r);
        const auto nameFont = font (12.0f, 600), detailFont = font (11.0f, 600);
        auto line1 = area.removeFromTop (17);
        auto line2 = area.removeFromTop (15);
        juce::String first, second;
        juce::Colour firstColour = Colours::text, secondColour = Colours::textSubtle;

        if (model.isCapturing() && model.getCaptureRole() == r)
        {
            const double secs = model.getCaptureSeconds();
            first = secs < 0.05 ? juce::String ("Waiting for audio...") : "Listening  " + secondsText (secs);
            firstColour = roleColour (r).brighter (0.2f);
            const int gaps = model.getCaptureGaps();
            second = gaps == 0 ? juce::String ("No gaps") : juce::String (gaps) + (gaps == 1 ? " gap (audio was dropped)" : " gaps (audio was dropped)");
            secondColour = gaps == 0 ? Colours::textSubtle : Colours::minor;
        }
        else if (ref && model.isLoading())
        {
            // Progress bar
            const auto bar = line1.toFloat().withSizeKeepingCentre (line1.toFloat().getWidth(), 6.0f).translated (0.0f, 1.0f);
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (roleColour (r));
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (6.0f, bar.getWidth() * juce::jlimit (0.0f, 1.0f, model.getLoadProgress()))), 3.0f);
            second = "Analysing " + loadFileName + "  " + juce::String (juce::roundToInt (model.getLoadProgress() * 100.0f)) + " %";
        }
        else if (s.valid)
        {
            first = MatchEqModel::sourceName (s);
            juce::StringArray parts;

            if (s.seconds > 0.0)
                parts.add (secondsText (s.seconds) + " of audio");

            if (s.sampleRate > 0.0)
                parts.add (rateText (s.sampleRate));

            if (parts.isEmpty())
                parts.add (juce::String (s.source.data()) == "synth" ? "From a preset" : "Stored curve");

            second = parts.joinIntoString (", ");
        }
        else
        {
            first = "No spectrum yet";
            firstColour = Colours::textSubtle;
            second = ref ? "Capture it or load a file" : "Capture it from your track";
        }

        if (ref && loadError.isNotEmpty() && ! model.isLoading() && ! (model.isCapturing() && model.getCaptureRole() == r))
        {
            second = loadError;
            secondColour = Colours::major;
        }

        if (first.isNotEmpty())
        {
            g.setFont (nameFont);
            g.setColour (firstColour);
            g.drawText (first, line1, juce::Justification::centredLeft, true);
        }

        if (second.isNotEmpty() && line2.getHeight() >= 12)
        {
            g.setFont (detailFont);
            g.setColour (secondColour);
            g.drawFittedText (second, line2, juce::Justification::centredLeft, 1, 0.85f);
        }
    }

    // The two info lines under a card's buttons: centred in the space that is left (a little above the middle).
    static juce::Rectangle<int> infoArea (juce::Rectangle<int> below)
    {
        below.removeFromBottom (6);
        const int h = juce::jmin (below.getHeight(), 34);
        const int top = juce::jmax (6, (below.getHeight() - h) / 2 - 2);
        return below.withTrimmedTop (top).withHeight (h);
    }

    //==========================================================================
    void layout()
    {
        auto r = contentArea.reduced (12, 10);

        if (r.isEmpty())
            return;

        const int cardsH = r.getHeight() >= 520 ? 132 : 118;
        auto strip = r.removeFromBottom (cardsH);
        r.removeFromBottom (kGap);
        display.setBounds (r);

        const int w = strip.getWidth();
        const int filterW = juce::jlimit (140, 220, juce::roundToInt ((float) w * 0.15f));
        const bool sixKnobs = w >= 1120;
        const int knobs = sixKnobs ? 6 : 4;
        const int knobCell = juce::jlimit (56, 76, juce::roundToInt ((float) w * 0.072f));
        const int fineW = knobs * knobCell + 24;
        const int rest = w - filterW - fineW - 3 * kGap;
        const int refW = juce::roundToInt ((float) rest * 0.61f);

        refCard = strip.removeFromLeft (refW);
        strip.removeFromLeft (kGap);
        curCard = strip.removeFromLeft (rest - refW);
        strip.removeFromLeft (kGap);
        fineCard = strip.removeFromLeft (fineW);
        strip.removeFromLeft (kGap);
        filterCard = strip;

        // Reference
        {
            auto body = refCard.withTrimmedTop (kTitleH).reduced (12, 0);
            auto row = body.removeFromTop (kButtonH);
            refInfo = infoArea (body);
            const int avail = row.getWidth();
            const bool loading = refCancel.isVisible();
            const int capW = refCapture.idealWidth (true);
            const bool allText = capW + refLoad.idealWidth (true) + refClear.idealWidth (true) + 12 <= avail;
            const bool captureText = capW + 2 * refLoad.idealWidth (false) + 12 <= avail;

            refCapture.setIconOnly (! captureText);
            refLoad.setIconOnly (! allText);
            refClear.setIconOnly (! allText);
            refCapture.setBounds (row.removeFromLeft (captureText ? capW : refCapture.idealWidth (false)));
            row.removeFromLeft (6);
            const auto rest2 = row;
            refLoad.setBounds (row.removeFromLeft (refLoad.idealWidth (allText)));
            row.removeFromLeft (6);
            refClear.setBounds (row.removeFromLeft (refClear.idealWidth (allText)));

            // While a file is analysed, Cancel (with its label when it fits) takes the place of Load and Clear.
            refLoad.setVisible (! loading);
            refClear.setVisible (! loading);
            const bool cancelText = refCancel.idealWidth (true) <= rest2.getWidth();
            refCancel.setIconOnly (! cancelText);
            refCancel.setBounds (rest2.withWidth (juce::jmin (rest2.getWidth(), refCancel.idealWidth (cancelText))));
        }

        // Current
        {
            auto body = curCard.withTrimmedTop (kTitleH).reduced (12, 0);
            auto row = body.removeFromTop (kButtonH);
            curInfo = infoArea (body);
            const int avail = row.getWidth();
            const int capW = curCapture.idealWidth (true);
            const bool allText = capW + curClear.idealWidth (true) + 6 <= avail;
            const bool captureText = capW + curClear.idealWidth (false) + 6 <= avail;
            curCapture.setIconOnly (! captureText);
            curClear.setIconOnly (! allText);
            curCapture.setBounds (row.removeFromLeft (captureText ? capW : curCapture.idealWidth (false)));
            row.removeFromLeft (6);
            curClear.setBounds (row.removeFromLeft (allText ? curClear.idealWidth (true) : curClear.idealWidth (false)));
        }

        // Fine Tune
        {
            auto body = fineCard.withTrimmedTop (kTitleH - 4).reduced (12, 0).withTrimmedBottom (8);
            low.setVisible (sixKnobs);
            high.setVisible (sixKnobs);
            const bool shortCaptions = knobCell < 64;
            boost.setCaption (shortCaptions ? "Boost" : "Limit Boost");
            cut.setCaption (shortCaptions ? "Cut" : "Limit Cut");
            low.setCaption (shortCaptions ? "Low" : "Low Cutoff");
            high.setCaption (shortCaptions ? "High" : "High Cutoff");
            smooth.setCaption (knobCell < 62 ? "Smooth" : "Smoothing");
            ParamKnob* order6[] { &amount, &smooth, &low, &high, &boost, &cut };
            ParamKnob* order4[] { &amount, &smooth, &boost, &cut };
            auto cells = body.withSizeKeepingCentre (knobs * knobCell, body.getHeight());

            for (int i = 0; i < knobs; ++i)
                (sixKnobs ? order6[i] : order4[i])->setBounds (cells.removeFromLeft (knobCell));
        }

        // Filter
        {
            auto body = filterCard.withTrimmedTop (kTitleH).reduced (12, 0).withTrimmedBottom (10);
            phase.setBounds (body.removeFromTop (kRowH));
            body.removeFromTop (8);
            firSize.setBounds (body.removeFromTop (kRowH));
            body.removeFromTop (6);
            filterInfo = body;
        }

        owner.repaint();
    }

    void paint (juce::Graphics& g) const
    {
        juce::Colour refStatusColour, curStatusColour;
        const auto refStatus = roleStatus (ps::MatchRole::Reference, refStatusColour);
        const auto curStatus = roleStatus (ps::MatchRole::Current, curStatusColour);
        drawCard (g, refCard, "Reference", refStatus, MatchDisplay::referenceColour(), refStatusColour);
        drawCard (g, curCard, "Current", curStatus, MatchDisplay::currentColour(), curStatusColour);

        // Fine Tune: the shown set, and the range of the installed curve
        juce::String fineSub;

        if (model.isDual() && ! model.isLinked())
            fineSub = model.setName (model.getSet());
        else if (model.isDual())
            fineSub = "Linked";

        if (model.isCurveActive() && (model.getCurveMax() > 0.05f || model.getCurveMin() < -0.05f))
        {
            const auto range = fmt::db (model.getCurveMax(), 1) + " / " + fmt::db (model.getCurveMin(), 1);
            fineSub = fineSub.isNotEmpty() ? fineSub + "  " + range : "Curve " + range;
        }

        drawCard (g, fineCard, "Fine Tune", fineSub, juce::Colours::transparentBlack,
                  model.isDual() && model.getSet() == 1 ? Colours::sideChannel : Colours::textSubtle);

        // Filter: latency
        const int latency = model.getLatencySamples();
        const double ms = 1000.0 * (double) latency / model.getSampleRate();
        const auto latencyText = ms < 0.05 ? juce::String ("No latency")
                                           : "Latency " + (ms < 10.0 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms))) + " ms";
        drawCard (g, filterCard, "Filter", {}, juce::Colours::transparentBlack, Colours::textSubtle);

        if (! filterInfo.isEmpty())
        {
            g.setFont (font (11.0f, 600));
            g.setColour (Colours::textSubtle);
            g.drawFittedText (latencyText, filterInfo, juce::Justification::centredLeft, 1, 0.85f);
        }

        paintRoleInfo (g, ps::MatchRole::Reference, refInfo);
        paintRoleInfo (g, ps::MatchRole::Current, curInfo);
    }

    //==========================================================================
    void refresh()
    {
        const int changes = model.update (owner.getChannelSet());

        if ((changes & MatchEqModel::modeChanged) != 0)
            rebind();

        updateButtons();
        display.update (changes);
        owner.repaint (refCard.getUnion (curCard).getUnion (fineCard).getUnion (filterCard));
    }

    void tick()
    {
        const int changes = model.update (owner.getChannelSet());

        if ((changes & MatchEqModel::modeChanged) != 0)
            rebind();

        watchLoad();
        updateButtons();
        display.update (changes);

        if ((changes & ~MatchEqModel::runningChanged) != 0 || model.isLoading() || model.isCapturing()
            || model.getLatencySamples() != shownLatency)
        {
            shownLatency = model.getLatencySamples();
            owner.repaint (refCard.getUnion (curCard).getUnion (fineCard).getUnion (filterCard));
        }
    }

    //==========================================================================
    MatchEqPanel& owner;
    PureSheenProcessor& processor;
    EditorServices& services;
    MatchEqModel model;
    MatchDisplay display;

    ActionButton refCapture, refLoad, refClear, refCancel, curCapture, curClear;
    ParamKnob amount, smooth, low, high, boost, cut;
    ParamSegments phase;
    ParamChoice firSize;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::String loadError, loadFileName;
    bool loadWatch = false;
    int loadWatchTicks = 0;
    int shownLatency = -1;

    juce::Rectangle<int> contentArea, refCard, curCard, fineCard, filterCard, refInfo, curInfo, filterInfo;
};

//==============================================================================
MatchEqPanel::MatchEqPanel (PureSheenProcessor& p, EditorServices& s) : ModulePanel (p, s, ps::ModuleType::MatchEq)
{
    impl = std::make_unique<Impl> (*this, p, s);
    neverTakeKeyboardFocus (*this);
}

MatchEqPanel::~MatchEqPanel() = default;

void MatchEqPanel::tick()
{
    ModulePanel::tick();

    if (impl != nullptr)
        impl->tick();
}

void MatchEqPanel::frameTick()
{
    if (impl != nullptr)
        impl->display.repaint();
}

void MatchEqPanel::refresh()
{
    ModulePanel::refresh();

    if (impl != nullptr)
        impl->refresh();
}

void MatchEqPanel::channelSetChanged()
{
    if (impl != nullptr)
        impl->refresh();
}

void MatchEqPanel::paint (juce::Graphics& g)
{
    ModulePanel::paint (g);

    if (impl != nullptr)
        impl->paint (g);
}

void MatchEqPanel::layoutContent (juce::Rectangle<int> contentArea)
{
    if (impl == nullptr)
        return;

    impl->contentArea = contentArea;
    impl->layout();
}
} // namespace ps::ui
