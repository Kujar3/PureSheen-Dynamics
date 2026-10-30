#include "ui/HeaderBar.h"

#include "ui/Controls.h"

#include <map>

namespace ps::ui
{
namespace
{
const char* const kSnapshotNames[] = { "A", "B", "C", "D" };

//==============================================================================
// The global preset name: "< Warm Glue* >" style chip (the arrows are separate buttons). Click opens the preset list.
class PresetName final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PresetName()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Presets: click for the list of global presets (whole chain and every setting)");
    }

    void set (const juce::String& n, bool modified)
    {
        if (n != name || modified != isModified)
        {
            name = n;
            isModified = modified;
            repaint();
        }
    }

    void setMenuOpen (bool open) { menuOpen = open; repaint(); }

    std::function<void()> onOpen;

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool hover = isMouseOver (true);
        drawChip (g, b, 7.0f, menuOpen ? Colours::panelHover : (hover ? Colours::panelHover.withAlpha (0.85f) : Colours::background.darker (0.1f)),
                  menuOpen ? Colours::accent.withAlpha (0.7f) : Colours::outline.brighter (hover ? 0.18f : 0.0f));

        auto text = b.reduced (12.0f, 0.0f).withTrimmedRight (14.0f);
        g.setFont (font (10.0f, 700).withExtraKerningFactor (0.08f));
        g.setColour (Colours::textSubtle);
        const auto label = juce::String ("PRESET");
        const float labelW = textWidth (font (10.0f, 700).withExtraKerningFactor (0.08f), label) + 2.0f;

        if (text.getWidth() > 140.0f)
        {
            g.drawText (label, text.removeFromLeft (labelW), juce::Justification::centredLeft, false);
            text.removeFromLeft (8.0f);
        }

        const auto nameFont = font (13.0f, 600);
        g.setFont (nameFont);
        g.setColour (Colours::text);
        const auto shown = name + (isModified ? " *" : "");
        g.drawFittedText (shown, text.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

        const float cx = b.getRight() - 12.0f, cy = b.getCentreY();
        juce::Path chevron;
        chevron.startNewSubPath (cx - 3.0f, cy - 1.4f);
        chevron.lineTo (cx, cy + 1.6f);
        chevron.lineTo (cx + 3.0f, cy - 1.4f);
        g.setColour (hover || menuOpen ? Colours::text : Colours::textDim);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if ((isLeftClick (e) || e.mods.isPopupMenu()) && ! menuOpen && onOpen != nullptr)
            onOpen();
    }

private:
    juce::String name { "Default" };
    bool isModified = false, menuOpen = false;
};

//==============================================================================
// One A / B / C / D snapshot button. Click: recall (or store when empty). Right-click: store / recall / clear.
class SnapshotButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SnapshotButton (int s) : slot (s)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
    }

    void set (bool stored, bool active)
    {
        if (stored != hasData || active != isActive)
        {
            hasData = stored;
            isActive = active;
            setTooltip (juce::String ("Snapshot ") + kSnapshotNames[slot] + (hasData ? ": click to recall it, right-click to store over it or clear it."
                                                                          : ": empty. Click to store the current settings here."));
            repaint();
        }
    }

    std::function<void (int slot, bool rightClick)> onClick;

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        const bool hover = isMouseOver (true);
        const float r = 6.0f;

        if (isActive)
        {
            drawChip (g, b, r, blend (Colours::panelRaised, Colours::accent, 0.3f), Colours::accent.withAlpha (0.9f));
        }
        else if (hasData)
        {
            drawChip (g, b, r, hover ? Colours::panelHover : Colours::panelRaised, Colours::outline.brighter (hover ? 0.25f : 0.1f));
        }
        else
        {
            g.setColour (hover ? Colours::panelHover.withAlpha (0.6f) : juce::Colours::transparentBlack);
            g.fillRoundedRectangle (b, r);
            juce::Path outline, dashed;
            outline.addRoundedRectangle (b.reduced (0.5f), r);
            const float dashes[] = { 3.0f, 2.5f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (Colours::outline.brighter (hover ? 0.35f : 0.15f));
            g.fillPath (dashed);
        }

        g.setFont (font (12.0f, 700));
        g.setColour (isActive ? Colours::accent.brighter (0.4f) : (hasData ? Colours::text : (hover ? Colours::textDim : Colours::textFaint.brighter (0.2f))));
        g.drawText (kSnapshotNames[slot], b, juce::Justification::centred, false);

        if (hasData && ! isActive)
        {
            g.setColour (Colours::accent.withAlpha (0.8f));
            g.fillEllipse (juce::Rectangle<float> (3.5f, 3.5f).withCentre ({ b.getRight() - 4.5f, b.getY() + 4.5f }));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()) || onClick == nullptr)
            return;

        if (e.mods.isPopupMenu())
            onClick (slot, true);
        else if (isLeftClick (e))
            onClick (slot, false);
    }

private:
    int slot;
    bool hasData = false, isActive = false;
};
} // namespace

//==============================================================================
struct HeaderBar::Impl
{
    Impl (HeaderBar& o, EditorContext& c) : owner (o), ctx (c), proc (c.getProcessor())
    {
        prev.setTooltip ("Previous preset");
        prev.onClick = [this] { proc.loadNextPreset (-1); update(); };
        next.setTooltip ("Next preset");
        next.onClick = [this] { proc.loadNextPreset (1); update(); };
        presetName.onOpen = [this] { showPresetMenu(); };
        save.setTooltip ("Save the whole chain and every setting as a preset");
        save.onClick = [this] { savePresetAs(); };
        presetMenu.setTooltip ("Preset options");
        presetMenu.onClick = [this] { showPresetOptions(); };

        undo.setTooltip ("Undo the last change");
        undo.onClick = [this] { proc.undo(); update(); };
        redo.setTooltip ("Redo");
        redo.onClick = [this] { proc.redo(); update(); };
        history.setTooltip ("History: every change you made. Click an entry to go back to that point.");
        history.onClick = [this] { ctx.toggleHistory(); };

        for (int i = 0; i < PureSheenProcessor::kNumSnapshots; ++i)
        {
            snapshots[(size_t) i] = std::make_unique<SnapshotButton> (i);
            snapshots[(size_t) i]->onClick = [this] (int slot, bool right) { snapshotClicked (slot, right); };
        }

        options.setTooltip ("Options: meters, spectrum, gain match, history, tooltips");
        options.onClick = [this] { showOptionsMenu(); };
        help.setTooltip ("How to use PureSheen");
        help.onClick = [this] { ctx.toggleHelp(); };

        for (juce::Component* comp : std::initializer_list<juce::Component*> { &prev, &presetName, &next, &save, &presetMenu,
                                                                            &undo, &redo, &history, &options, &help })
            owner.addAndMakeVisible (comp);

        for (auto& s : snapshots)
            owner.addAndMakeVisible (*s);
    }

    //==========================================================================
    void showPresetMenu()
    {
        const auto presets = proc.getPresets (-1);
        juce::PopupMenu menu, user;
        std::map<juce::String, juce::PopupMenu> categories;
        const auto current = proc.getCurrentPresetName();

        for (size_t i = 0; i < presets.size(); ++i)
        {
            juce::PopupMenu::Item item (presets[i].name);
            item.itemID = (int) i + 100;
            item.isTicked = presets[i].name == current;

            if (presets[i].factory)
                categories[presets[i].category.isNotEmpty() ? presets[i].category : juce::String ("Factory")].addItem (std::move (item));
            else
                user.addItem (std::move (item));
        }

        menu.addSectionHeader ("Global presets");

        if (categories.empty() && user.getNumItems() == 0)
            menu.addItem (juce::PopupMenu::Item ("No presets yet").setEnabled (false));

        for (auto& [name, sub] : categories)
            menu.addSubMenu (name, sub);

        if (user.getNumItems() > 0)
            menu.addSubMenu ("User", user);

        menu.addSeparator();
        menu.addItem (1, "Save preset...");

        presetName.setMenuOpen (true);
        showMenu (menu, presetName, [this, presets] (int result)
        {
            presetName.setMenuOpen (false);

            if (result == 1)
                savePresetAs();
            else if (result >= 100 && result - 100 < (int) presets.size())
                proc.loadPreset (-1, presets[(size_t) (result - 100)]);

            update();
        }, presetName.getWidth());
    }

    void savePresetAs()
    {
        const auto current = proc.getCurrentPresetName();
        promptForText (owner, "Save preset", "Name of the preset (saves the whole chain and every setting):",
                       current == "Default" ? juce::String ("My master") : current,
                       [this, safe = juce::Component::SafePointer<HeaderBar> (&owner)] (juce::String name)
                       {
                           if (safe == nullptr)
                               return;

                           const auto legal = juce::File::createLegalFileName (name);
                           ctx.notify (proc.savePreset (-1, legal) ? "Saved preset \"" + legal + "\""
                                                                   : juce::String ("Could not save the preset"));
                           update();
                       });
    }

    void showPresetOptions()
    {
        juce::PopupMenu menu;
        const auto presets = proc.getPresets (-1);
        const auto current = proc.getCurrentPresetName();
        const PureSheenProcessor::PresetInfo* currentUser = nullptr;

        for (const auto& p : presets)
            if (! p.factory && p.name == current)
                currentUser = &p;

        menu.addItem (1, "Save preset...");
        menu.addItem (2, "Delete \"" + current + "\"", currentUser != nullptr);
        menu.addSeparator();
        menu.addItem (3, "Reset everything to default");
        menu.addItem (4, "Open the presets folder");

        const auto userInfo = currentUser != nullptr ? *currentUser : PureSheenProcessor::PresetInfo {};
        presetMenu.setToggleState (true, juce::dontSendNotification);
        showMenu (menu, presetMenu, [this, userInfo] (int result)
        {
            presetMenu.setToggleState (false, juce::dontSendNotification);

            switch (result)
            {
                case 1: savePresetAs(); break;
                case 2:
                    if (proc.deletePreset (userInfo))
                        ctx.notify ("Deleted preset \"" + userInfo.name + "\"");
                    break;
                case 3: resetEverything(); break;
                case 4:
                {
                    const auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                            .getChildFile ("PureSheen Dynamics").getChildFile ("Presets");
                    if (folder.isDirectory())
                        folder.startAsProcess();
                    else
                        ctx.notify ("No user presets yet - save one first");
                    break;
                }
                default: break;
            }

            update();
        });
    }

    void resetEverything()
    {
        std::vector<std::pair<juce::String, float>> values;

        for (const auto& s : ps::paramSpecs())
            values.emplace_back (juce::String (s.id), s.defaultValue);

        proc.setParameterValues (values, "Reset everything");
        proc.setChainOrder ({ ps::kDefaultChain.begin(), ps::kDefaultChain.end() });
        ctx.notify ("Everything reset to the default chain");
    }

    void snapshotClicked (int slot, bool rightClick)
    {
        const juce::String name = kSnapshotNames[slot];

        if (! rightClick)
        {
            if (proc.hasSnapshot (slot))
            {
                proc.recallSnapshot (slot);
                ctx.notify ("Recalled snapshot " + name);
            }
            else
            {
                proc.storeSnapshot (slot);
                ctx.notify ("Stored the current settings in snapshot " + name);
            }

            update();
            return;
        }

        juce::PopupMenu menu;
        menu.addSectionHeader ("Snapshot " + name);
        menu.addItem (1, "Store current settings in " + name);
        menu.addItem (2, "Recall " + name, proc.hasSnapshot (slot));
        menu.addItem (3, "Clear " + name, proc.hasSnapshot (slot));

        showMenu (menu, *snapshots[(size_t) slot], [this, slot, name] (int result)
        {
            if (result == 1)      { proc.storeSnapshot (slot); ctx.notify ("Stored snapshot " + name); }
            else if (result == 2) { proc.recallSnapshot (slot); ctx.notify ("Recalled snapshot " + name); }
            else if (result == 3) { proc.clearSnapshot (slot); }
            update();
        });
    }

    //==========================================================================
    void showOptionsMenu()
    {
        auto o = ctx.getUiOptions();
        using O = UiOptions;
        juce::PopupMenu menu, meters, mType, mScale, mHold, mRms, mK, spectrum, sType, sScale, sAvg, sTilt, sFft, hist, gm;

        const char* meterTypes[] = { "Peak", "RMS", "Peak + RMS", "K-System", "Momentary (400 ms)", "Short-term (3 s)", "Integrated" };
        for (int i = 0; i < 7; ++i)
            mType.addItem (1000 + i, meterTypes[i], true, (int) o.meterType == i);

        const char* scales[] = { "dB (linear)", "dB (non-linear)", "BS.1771", "EBU +9", "EBU +18" };
        for (int i = 0; i < 5; ++i)
            mScale.addItem (1100 + i, scales[i], true, (int) o.meterScale == i);

        const float holds[] = { 5.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 1.0e9f };
        const char* holdNames[] = { "5 ms", "250 ms", "500 ms", "1 s", "2 s", "5 s", "Infinite" };
        for (int i = 0; i < 7; ++i)
            mHold.addItem (1200 + i, holdNames[i], true, std::abs (o.peakHoldMs - holds[i]) < 1.0f);

        const float rmsWindows[] = { 10.0f, 50.0f, 300.0f, 1475.0f, 2650.0f, 3825.0f, 5000.0f };
        for (int i = 0; i < 7; ++i)
            mRms.addItem (1300 + i, juce::String (juce::roundToInt (rmsWindows[i])) + " ms", true, std::abs (o.rmsWindowMs - rmsWindows[i]) < 1.0f);

        const int kTypes[] = { 12, 14, 20 };
        for (int i = 0; i < 3; ++i)
            mK.addItem (1400 + i, "K-" + juce::String (kTypes[i]), true, o.kSystemType == kTypes[i]);

        meters.addSubMenu ("Meter type", mType);
        meters.addSubMenu ("Meter scale", mScale);
        meters.addItem (1500, "Show Mid / Side instead of Left / Right", true, o.meterMidSide);
        meters.addSubMenu ("Peak hold", mHold);
        meters.addSubMenu ("RMS window", mRms);
        meters.addSubMenu ("K-System type", mK);
        meters.addItem (1501, "Detect true peaks", true, o.detectTruePeaks);
        meters.addItem (1502, "Reset integrated loudness and LRA");

        const char* specTypes[] = { "Linear", "1/3 octave", "Critical bands", "Octave" };
        for (int i = 0; i < 4; ++i)
            sType.addItem (2000 + i, specTypes[i], true, (int) o.spectrumType == i);

        const char* freqScales[] = { "Log", "Mel", "Extended log" };
        for (int i = 0; i < 3; ++i)
            sScale.addItem (2100 + i, freqScales[i], true, (int) o.freqScale == i);

        const float avgs[] = { 0.0f, 1.0f, 3.0f, 5.0f, 10.0f, -1.0f };
        const char* avgNames[] = { "Real time", "1 s", "3 s", "5 s", "10 s", "Infinite" };
        for (int i = 0; i < 6; ++i)
            sAvg.addItem (2200 + i, avgNames[i], true, std::abs (o.spectrumAveragingS - avgs[i]) < 0.01f);

        const float tilts[] = { 0.0f, 1.5f, 3.0f, 4.5f, 6.0f };
        for (int i = 0; i < 5; ++i)
            sTilt.addItem (2300 + i, juce::String (tilts[i], 1) + " dB/oct", true, std::abs (o.spectrumTiltDbPerOct - tilts[i]) < 0.01f);

        const int ffts[] = { 2048, 4096, 8192, 16384 };
        for (int i = 0; i < 4; ++i)
            sFft.addItem (2400 + i, juce::String (ffts[i]) + " points", true, o.fftSize == ffts[i]);

        spectrum.addSubMenu ("Display", sType);
        spectrum.addSubMenu ("Frequency scale", sScale);
        spectrum.addSubMenu ("Averaging", sAvg);
        spectrum.addSubMenu ("Tilt", sTilt);
        spectrum.addSubMenu ("FFT size", sFft);
        spectrum.addItem (2500, "Peak hold", true, o.spectrumPeakHold);
        spectrum.addItem (2501, "Filled spectrum", true, o.spectrumFill);

        const bool modern = proc.getParameterValue ("io.gainMatchModern") >= 0.5f;
        gm.addItem (3000, "Classic: level the bypassed signal to the processed one", true, ! modern);
        gm.addItem (3001, "Modern: level the processed output to the input", true, modern);

        const int depths[] = { 50, 100, 200, 500, 1000 };
        for (int i = 0; i < 5; ++i)
            hist.addItem (3100 + i, juce::String (depths[i]) + " steps", true, o.historyDepth == depths[i]);

        menu.addSectionHeader ("Options");
        menu.addSubMenu ("Meters", meters);
        menu.addSubMenu ("Spectrum", spectrum);
        menu.addSubMenu ("Gain Match", gm);
        menu.addSubMenu ("Undo history length", hist);
        menu.addSeparator();
        menu.addItem (3200, "Show tooltips", true, o.showTooltips);
        menu.addItem (3201, "Dim bypassed modules", true, o.dimBypassed);

        options.setToggleState (true, juce::dontSendNotification);
        showMenu (menu, options, [this] (int r)
        {
            options.setToggleState (false, juce::dontSendNotification);

            if (r <= 0)
                return;

            auto n = ctx.getUiOptions();
            const float holdsL[] = { 5.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 1.0e9f };
            const float rmsL[] = { 10.0f, 50.0f, 300.0f, 1475.0f, 2650.0f, 3825.0f, 5000.0f };
            const int kL[] = { 12, 14, 20 };
            const float avgL[] = { 0.0f, 1.0f, 3.0f, 5.0f, 10.0f, -1.0f };
            const float tiltL[] = { 0.0f, 1.5f, 3.0f, 4.5f, 6.0f };
            const int fftL[] = { 2048, 4096, 8192, 16384 };
            const int depthL[] = { 50, 100, 200, 500, 1000 };

            if (r >= 1000 && r < 1007)      n.meterType = (O::MeterType) (r - 1000);
            else if (r >= 1100 && r < 1105) n.meterScale = (O::MeterScale) (r - 1100);
            else if (r >= 1200 && r < 1207) n.peakHoldMs = holdsL[r - 1200];
            else if (r >= 1300 && r < 1307) n.rmsWindowMs = rmsL[r - 1300];
            else if (r >= 1400 && r < 1403) n.kSystemType = kL[r - 1400];
            else if (r == 1500)             n.meterMidSide = ! n.meterMidSide;
            else if (r == 1501)             n.detectTruePeaks = ! n.detectTruePeaks;
            else if (r == 1502)             { proc.getChain().resetIntegratedLoudness(); ctx.notify ("Integrated loudness and LRA reset"); return; }
            else if (r >= 2000 && r < 2004) n.spectrumType = (O::SpectrumType) (r - 2000);
            else if (r >= 2100 && r < 2103) n.freqScale = (O::FreqScale) (r - 2100);
            else if (r >= 2200 && r < 2206) n.spectrumAveragingS = avgL[r - 2200];
            else if (r >= 2300 && r < 2305) n.spectrumTiltDbPerOct = tiltL[r - 2300];
            else if (r >= 2400 && r < 2404) n.fftSize = fftL[r - 2400];
            else if (r == 2500)             n.spectrumPeakHold = ! n.spectrumPeakHold;
            else if (r == 2501)             n.spectrumFill = ! n.spectrumFill;
            else if (r == 3000 || r == 3001) { proc.setParameterValue ("io.gainMatchModern", r == 3001 ? 1.0f : 0.0f); return; }
            else if (r >= 3100 && r < 3105) n.historyDepth = depthL[r - 3100];
            else if (r == 3200)             n.showTooltips = ! n.showTooltips;
            else if (r == 3201)             n.dimBypassed = ! n.dimBypassed;

            ctx.applyOptions (n);
        });
    }

    //==========================================================================
    void update()
    {
        presetName.set (proc.getCurrentPresetName(), proc.isPresetModified());
        undo.setEnabled (proc.canUndo());
        redo.setEnabled (proc.canRedo());
        history.setToggleState (ctx.isHistoryOpen(), juce::dontSendNotification);
        help.setToggleState (ctx.isHelpOpen(), juce::dontSendNotification);

        const int active = proc.getActiveSnapshot();

        for (int i = 0; i < PureSheenProcessor::kNumSnapshots; ++i)
            snapshots[(size_t) i]->set (proc.hasSnapshot (i), active == i);
    }

    void paintBrand (juce::Graphics& g) const
    {
        auto area = brandArea.toFloat();
        drawLogo (g, area.removeFromLeft (30.0f).withSizeKeepingCentre (28.0f, 28.0f));

        if (! showWordmark)
            return;

        auto text = area.withTrimmedLeft (9.0f);
        const auto f = font (18.5f, 700);
        g.setFont (f);
        g.setColour (Colours::text);
        g.drawText ("Pure", text, juce::Justification::centredLeft, false);
        g.setColour (Colours::accent);
        g.drawText ("Sheen", text.withTrimmedLeft (textWidth (f, "Pure")), juce::Justification::centredLeft, false);

        if (showSubtitle)
        {
            g.setFont (font (10.0f, 700).withExtraKerningFactor (0.18f));
            g.setColour (Colours::textSubtle);
            g.drawText ("DYNAMICS", text.withTrimmedLeft (textWidth (f, "PureSheen") + 8.0f).translated (0.0f, 1.5f),
                        juce::Justification::centredLeft, false);
        }
    }

    HeaderBar& owner;
    EditorContext& ctx;
    PureSheenProcessor& proc;

    IconButton prev { "Previous preset", IconButton::Icon::prev };
    IconButton next { "Next preset", IconButton::Icon::next };
    PresetName presetName;
    IconButton save { "Save preset", IconButton::Icon::save };
    IconButton presetMenu { "Preset options", IconButton::Icon::options };
    IconButton undo { "Undo", IconButton::Icon::undo };
    IconButton redo { "Redo", IconButton::Icon::redo };
    IconButton history { "History", IconButton::Icon::history };
    std::array<std::unique_ptr<SnapshotButton>, PureSheenProcessor::kNumSnapshots> snapshots;
    IconButton options { "Options", IconButton::Icon::sliders };
    IconButton help { "Help", IconButton::Icon::help };

    juce::Rectangle<int> brandArea;
    std::vector<int> separators;
    bool showWordmark = true, showSubtitle = true;
};

//==============================================================================
HeaderBar::HeaderBar (EditorContext& ctx) : impl (std::make_unique<Impl> (*this, ctx))
{
    setTitle ("Header");   // (the editor calls update() once everything exists)
}

HeaderBar::~HeaderBar() = default;

void HeaderBar::update() { impl->update(); }

juce::Rectangle<int> HeaderBar::getHistoryButtonBounds() const { return impl->history.getBounds(); }

void HeaderBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (juce::Colour (0xff232b34), b.getY(), juce::Colour (0xff1d232b), b.getBottom()));
    g.fillRect (b);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (b.withTop (b.getBottom() - 1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.03f));
    g.fillRect (b.withHeight (1.0f));

    for (int x : impl->separators)
    {
        g.setColour (Colours::outline.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> ((float) x, b.getY() + 13.0f, 1.0f, b.getHeight() - 26.0f));
    }

    if (! impl->snapshots.empty())
    {
        // "SNAPSHOTS" caption left of the A..D group.
        const auto first = impl->snapshots[0]->getBounds();
        g.setFont (font (9.5f, 700).withExtraKerningFactor (0.1f));
        g.setColour (Colours::textSubtle);
        const auto label = juce::Rectangle<int> (first.getX() - 64, 0, 58, getHeight());

        if (label.getX() > impl->history.getRight() + 16)
            g.drawText ("SNAPSHOTS", label, juce::Justification::centredRight, false);
    }

    impl->paintBrand (g);
}

void HeaderBar::resized()
{
    auto& d = *impl;
    auto r = getLocalBounds().reduced (14, 0);
    const int icon = 28, gap = 4;
    int groupGap = 14;

    const auto f = font (18.5f, 700);
    const int wordW = (int) std::ceil (textWidth (f, "PureSheen"));
    const int subW = (int) std::ceil (textWidth (font (10.0f, 700).withExtraKerningFactor (0.18f), "DYNAMICS")) + 8;
    const int fixedRight = 4 * 26 + 3 * 5 + 64 + 2 * icon + gap;   // snapshots + label + options + help
    const int middleTools = 3 * icon + 2 * gap;                     // undo, redo, history
    const int presetMin = 210, presetMax = 330;
    const int presetTools = 2 * icon + 3 * icon + 4 * gap;          // prev, next, save, menu

    d.showWordmark = true;
    d.showSubtitle = true;
    auto brandWidth = [&] { return 30 + (d.showWordmark ? 9 + wordW + (d.showSubtitle ? subW : 0) : 0); };
    auto required = [&] { return brandWidth() + 4 * groupGap + presetMin + presetTools + middleTools + fixedRight; };

    if (required() > r.getWidth()) d.showSubtitle = false;
    if (required() > r.getWidth()) groupGap = 10;
    if (required() > r.getWidth()) d.showWordmark = false;

    d.separators.clear();
    d.brandArea = r.removeFromLeft (brandWidth());
    r.removeFromLeft (groupGap);
    d.separators.push_back (r.getX());
    r.removeFromLeft (groupGap);

    // Right: help, options | snapshots
    d.help.setBounds (r.removeFromRight (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromRight (gap);
    d.options.setBounds (r.removeFromRight (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromRight (groupGap);
    d.separators.push_back (r.getRight());
    r.removeFromRight (groupGap);

    for (int i = PureSheenProcessor::kNumSnapshots - 1; i >= 0; --i)
    {
        d.snapshots[(size_t) i]->setBounds (r.removeFromRight (26).withSizeKeepingCentre (26, 26));
        if (i > 0)
            r.removeFromRight (5);
    }

    r.removeFromRight (64);   // "SNAPSHOTS" label

    // Middle: preset bar, then undo / redo / history.
    const int presetW = juce::jlimit (presetMin, presetMax, r.getWidth() - presetTools - middleTools - 2 * groupGap - 20);
    d.prev.setBounds (r.removeFromLeft (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromLeft (gap);
    d.presetName.setBounds (r.removeFromLeft (presetW).withSizeKeepingCentre (presetW, 30));
    r.removeFromLeft (gap);
    d.next.setBounds (r.removeFromLeft (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromLeft (gap + 2);
    d.save.setBounds (r.removeFromLeft (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromLeft (gap);
    d.presetMenu.setBounds (r.removeFromLeft (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromLeft (groupGap);
    d.separators.push_back (r.getX());
    r.removeFromLeft (groupGap);

    d.undo.setBounds (r.removeFromLeft (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromLeft (gap);
    d.redo.setBounds (r.removeFromLeft (icon).withSizeKeepingCentre (icon, icon));
    r.removeFromLeft (gap);
    d.history.setBounds (r.removeFromLeft (icon).withSizeKeepingCentre (icon, icon));
}

} // namespace ps::ui
