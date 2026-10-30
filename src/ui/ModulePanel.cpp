#include "ui/ModulePanel.h"

#include "ui/Controls.h"

#include <map>

// ModulePanel base (header row) and GenericModulePanel. Owner: ui-shell.
namespace ps::ui
{
namespace
{
constexpr int kHeaderHeight = 34;
constexpr juce::uint32 kHintMs = 3200;

juce::String prefixOf (ps::ModuleType t) { return ps::moduleInfo (t).idPrefix; }

bool hasChannelModes (ps::ModuleType t) { return ps::supportedChannelModes (t).size() > 1; }

ps::ChannelMode currentMode (const PureSheenProcessor& p, ps::ModuleType t)
{
    const auto modes = ps::supportedChannelModes (t);

    if (modes.size() < 2)
        return ps::ChannelMode::Stereo;

    const int index = juce::jlimit (0, (int) modes.size() - 1, juce::roundToInt (p.getParameterValue (prefixOf (t) + ".chMode")));
    return modes[(size_t) index];
}

// The two channel sets are shown separately only in M/S and L/R (Transient/Sustain is reserved, treated as Stereo).
bool isSplitMode (ps::ChannelMode m) { return m == ps::ChannelMode::MidSide || m == ps::ChannelMode::LeftRight; }

juce::String channelSetName (ps::ChannelMode m, int set)
{
    if (m == ps::ChannelMode::MidSide)
        return set == 0 ? "Mid" : "Side";

    return set == 0 ? "Left" : "Right";
}

juce::String modeShortLabel (ps::ChannelMode m)
{
    switch (m)
    {
        case ps::ChannelMode::Stereo:           return "Stereo";
        case ps::ChannelMode::MidSide:          return "M/S";
        case ps::ChannelMode::LeftRight:        return "L/R";
        case ps::ChannelMode::TransientSustain: return "T/S";
    }

    return {};
}

//==============================================================================
// "Mid | Side" tabs: a click on the name selects the channel set, the LED switches the channel on / off, "S" solos it.
class ChannelTabs final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ChannelTabs (PureSheenProcessor& p, ModulePanel& o) : processor (p), owner (o)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Channel sets. Click a name to edit that channel, the light switches it on or off, S plays it alone.");
    }

    void setMode (ps::ChannelMode m)
    {
        if (m != mode)
        {
            mode = m;
            repaint();
        }
    }

    juce::Rectangle<float> tabArea (int i) const
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        return b.withWidth (b.getWidth() * 0.5f).translated (b.getWidth() * 0.5f * (float) i, 0.0f);
    }

    juce::Rectangle<float> ledArea (int i) const   { return tabArea (i).removeFromLeft (20.0f).withSizeKeepingCentre (8.0f, 8.0f).translated (2.0f, 0.0f); }
    juce::Rectangle<float> soloArea (int i) const  { return tabArea (i).removeFromRight (22.0f).withSizeKeepingCentre (16.0f, 16.0f).translated (-2.0f, 0.0f); }

    void paint (juce::Graphics& g) override
    {
        const auto prefix = prefixOf (owner.getModuleType());
        const auto colour = moduleColour (owner.getModuleType());
        const int selected = owner.getChannelSet();
        const int solo = processor.getChannelSolo (owner.getModuleType());
        const auto b = getLocalBounds().toFloat().reduced (0.5f);

        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (Colours::outline.withAlpha (0.9f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

        for (int i = 0; i < 2; ++i)
        {
            const auto tab = tabArea (i).reduced (2.0f);
            const bool isSel = i == selected;
            const bool on = processor.getParameterValue (prefix + (i == 0 ? ".a.on" : ".b.on")) >= 0.5f;
            const auto setColour = i == 0 ? colour : Colours::sideChannel;

            if (isSel)
            {
                g.setGradientFill (juce::ColourGradient::vertical (blend (Colours::panelHover, setColour, 0.28f), tab.getY(),
                                                                   blend (Colours::panelRaised, setColour, 0.16f), tab.getBottom()));
                g.fillRoundedRectangle (tab, 5.0f);
                g.setColour (setColour.withAlpha (0.7f));
                g.drawRoundedRectangle (tab.reduced (0.5f), 5.0f, 1.0f);
            }

            drawLed (g, ledArea (i), on, setColour);

            auto text = tab.withTrimmedLeft (18.0f).withTrimmedRight (20.0f);
            g.setFont (font (12.0f, isSel ? 700 : 600));
            g.setColour (isSel ? Colours::text : Colours::textDim);
            g.drawFittedText (channelSetName (mode, i), text.toNearestInt(), juce::Justification::centred, 1, 0.8f);

            const auto s = soloArea (i);
            const bool soloOn = solo == i;
            g.setColour (soloOn ? Colours::minor : Colours::panelRaised);
            g.fillRoundedRectangle (s, 3.5f);
            g.setColour (soloOn ? Colours::minor.brighter (0.2f) : Colours::outline.brighter (0.15f));
            g.drawRoundedRectangle (s.reduced (0.5f), 3.5f, 1.0f);
            g.setFont (font (10.0f, 700));
            g.setColour (soloOn ? Colours::background : Colours::textDim);
            g.drawText ("S", s, juce::Justification::centred, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e) || ! getLocalBounds().contains (e.getPosition()))
            return;

        const auto type = owner.getModuleType();
        const auto prefix = prefixOf (type);

        for (int i = 0; i < 2; ++i)
        {
            if (! tabArea (i).contains (e.position))
                continue;

            if (ledArea (i).expanded (5.0f).contains (e.position))
            {
                const auto id = prefix + (i == 0 ? ".a.on" : ".b.on");
                processor.setParameterValue (id, processor.getParameterValue (id) >= 0.5f ? 0.0f : 1.0f);
            }
            else if (soloArea (i).expanded (3.0f).contains (e.position))
            {
                processor.setChannelSolo (type, processor.getChannelSolo (type) == i ? -1 : i);
            }
            else
            {
                owner.setChannelSet (i);
            }

            repaint();
            return;
        }
    }

private:
    PureSheenProcessor& processor;
    ModulePanel& owner;
    ps::ChannelMode mode = ps::ChannelMode::MidSide;
};

void promptForPresetName (juce::Component& parent, std::function<void (juce::String)> done)
{
    auto* window = new juce::AlertWindow ("Save preset", "Name of the new preset:", juce::MessageBoxIconType::NoIcon, &parent);
    window->addTextEditor ("name", "My preset");
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, done] (int result)
    {
        const auto name = window->getTextEditorContents ("name").trim();

        if (result == 1 && name.isNotEmpty() && done != nullptr)
            done (juce::File::createLegalFileName (name));
    }), true);
}
} // namespace

//==============================================================================
struct ModulePanel::Impl
{
    Impl (ModulePanel& o, PureSheenProcessor& p, ps::ModuleType t)
        : owner (o), processor (p), type (t), tabs (p, o)
    {
        const auto prefix = prefixOf (type);

        presetChip.setText ({}, "Presets");
        presetChip.setTooltip ("Presets for this module only");
        presetChip.onOpenMenu = [this] (MenuChip& chip) { showPresetMenu (chip); };

        if (hasChannelModes (type))
        {
            juce::StringArray labels;

            for (auto m : ps::supportedChannelModes (type))
                if (m != ps::ChannelMode::TransientSustain)   // reserved for a later version
                    labels.add (modeShortLabel (m));

            modeSegments = std::make_unique<ParamSegments> (p, prefix + ".chMode", labels);
            modeSegments->setOnColour (moduleColour (type));
            modeSegments->setTooltip ("Channel mode. Stereo: both channels together. M/S: Mid (centre) and Side (edges) "
                                      "get their own settings. L/R: left and right get their own settings.");
            link = std::make_unique<ParamToggle> (p, prefix + ".link", "Link");
            link->setOnColour (moduleColour (type));
            link->setTooltip ("Link: edits change both channels at once.");
        }

        delta.setText ("Delta");
        delta.setOnColour (Colours::minor);
        delta.setTooltip ("Delta: hear only what this module changes (the difference between its output and input).");
        delta.onClick = [this]
        {
            processor.setDelta (type, ! processor.getDelta (type));
            delta.setOn (processor.getDelta (type));
        };

        learn.setText ("Learn");
        learn.setTooltip ("Learn: moves the crossovers to the quietest points of the music that is playing.");
        learn.onClick = [this]
        {
            processor.learnCrossovers (type);
            owner.showHeaderHint ("Crossovers learned from the music");
        };

        reset.setTooltip ("Reset this module to its default settings");
        reset.onClick = [this]
        {
            processor.resetModule (type);
            owner.showHeaderHint ("Module reset to defaults");
        };

        owner.addAndMakeVisible (presetChip);
        owner.addChildComponent (tabs);
        owner.addAndMakeVisible (delta);
        owner.addChildComponent (learn);
        owner.addAndMakeVisible (reset);

        if (modeSegments != nullptr)
        {
            owner.addAndMakeVisible (*modeSegments);
            owner.addChildComponent (*link);
        }
    }

    void showPresetMenu (MenuChip& chip)
    {
        const auto presets = processor.getPresets ((int) type);
        juce::PopupMenu menu, factory, user;
        std::map<juce::String, juce::PopupMenu> categories;

        for (size_t i = 0; i < presets.size(); ++i)
        {
            const auto& pr = presets[i];
            juce::PopupMenu::Item item (pr.name);
            item.itemID = (int) i + 100;
            item.isTicked = pr.name == presetName;

            if (pr.factory)
                categories[pr.category.isNotEmpty() ? pr.category : juce::String ("Factory")].addItem (std::move (item));
            else
                user.addItem (std::move (item));
        }

        menu.addSectionHeader (juce::String (ps::moduleInfo (type).name) + " presets");

        if (categories.empty() && user.getNumItems() == 0)
            menu.addItem (juce::PopupMenu::Item ("No presets yet").setEnabled (false));

        for (auto& [name, sub] : categories)
            menu.addSubMenu (name, sub);

        if (user.getNumItems() > 0)
            menu.addSubMenu ("User", user);

        menu.addSeparator();
        menu.addItem (1, "Save current settings as preset...");
        menu.addItem (2, "Reset module to defaults");

        chip.setMenuOpen (true);
        showMenu (menu, chip, [this, presets] (int result)
        {
            presetChip.setMenuOpen (false);

            if (result == 1)
            {
                promptForPresetName (owner, [this] (juce::String name)
                {
                    if (processor.savePreset ((int) type, name))
                    {
                        presetName = name;
                        presetChip.setText ({}, name);
                        owner.showHeaderHint ("Saved preset \"" + name + "\"");
                    }
                    else
                    {
                        owner.showHeaderHint ("Could not save the preset");
                    }
                });
            }
            else if (result == 2)
            {
                processor.resetModule (type);
            }
            else if (result >= 100 && result - 100 < (int) presets.size())
            {
                const auto& pr = presets[(size_t) (result - 100)];

                if (processor.loadPreset ((int) type, pr))
                {
                    presetName = pr.name;
                    presetChip.setText ({}, pr.name);
                }
            }
        }, 180);
    }

    void layoutHeader()
    {
        auto r = owner.getLocalBounds().removeFromTop (kHeaderHeight).reduced (12, 0);
        r.removeFromTop (2);
        const int h = 24;
        auto centred = [h] (juce::Rectangle<int> a) { return a.withSizeKeepingCentre (a.getWidth(), h); };

        // Right side: Reset, Delta, Learn.
        reset.setBounds (r.removeFromRight (28).withSizeKeepingCentre (28, 26));
        r.removeFromRight (6);
        delta.setBounds (centred (r.removeFromRight (58)));
        r.removeFromRight (6);
        const bool showLearn = owner.hasLearn();
        learn.setVisible (showLearn);

        if (showLearn)
        {
            learn.setBounds (centred (r.removeFromRight (60)));
            r.removeFromRight (6);
        }

        rightEdge = r.getRight();

        // Left side: name, presets, channel mode, A/B tabs, Link.
        const auto nameFont = font (14.5f, 700);
        const juce::String fullName (ps::moduleInfo (type).name);
        const bool split = isSplitMode (currentMode (processor, type));
        int numModes = 0;

        if (modeSegments != nullptr)
            for (auto m : ps::supportedChannelModes (type))
                numModes += m != ps::ChannelMode::TransientSustain ? 1 : 0;   // (T/S is not offered yet)

        int segW = numModes * 52;
        int chipW = 128;
        const int tabsW = 172, linkW = 62;

        auto needed = [&] (const juce::String& name)
        {
            return 18 + (int) textWidth (nameFont, name) + 14 + chipW + 10 + (segW > 0 ? segW + 10 : 0)
                   + (split ? tabsW + 8 + linkW + 10 : 0);
        };

        displayName = fullName;

        if (needed (displayName) > r.getWidth())
            displayName = ps::moduleInfo (type).shortName;

        if (needed (displayName) > r.getWidth())
            chipW = 104, segW = numModes * 44;

        const int nameW = 18 + (int) std::ceil (textWidth (nameFont, displayName));
        nameArea = r.removeFromLeft (nameW);
        r.removeFromLeft (14);
        presetChip.setBounds (centred (r.removeFromLeft (chipW)));
        r.removeFromLeft (10);

        if (modeSegments != nullptr)
        {
            modeSegments->setBounds (centred (r.removeFromLeft (segW)));
            r.removeFromLeft (10);
        }

        tabs.setVisible (split);

        if (link != nullptr)
            link->setVisible (split);

        if (split)
        {
            tabs.setBounds (centred (r.removeFromLeft (tabsW)));
            r.removeFromLeft (8);
            link->setBounds (centred (r.removeFromLeft (linkW)));
            r.removeFromLeft (10);
        }

        hintArea = juce::Rectangle<int> (r.getX(), 0, juce::jmax (0, rightEdge - r.getX()), kHeaderHeight);
    }

    void tick()
    {
        const auto mode = currentMode (processor, type);
        const bool split = isSplitMode (mode);

        if (split != tabs.isVisible() || mode != shownMode)
        {
            shownMode = mode;
            tabs.setMode (mode);
            layoutHeader();

            if (! split && owner.getChannelSet() != 0)
                owner.setChannelSet (0);
        }

        if (split)
            tabs.repaint();

        delta.setOn (processor.getDelta (type));

        const bool bypassed = processor.getParameterValue (prefixOf (type) + ".on") < 0.5f;

        if (bypassed != shownBypassed)
        {
            shownBypassed = bypassed;
            owner.repaint();
        }

        if (hint.isNotEmpty())
        {
            if (juce::Time::getMillisecondCounter() - hintShownMs > kHintMs)
                hint.clear();

            owner.repaint (hintArea);
        }
    }

    ModulePanel& owner;
    PureSheenProcessor& processor;
    ps::ModuleType type;
    int channelSet = 0;

    MenuChip presetChip;
    std::unique_ptr<ParamSegments> modeSegments;
    ChannelTabs tabs;
    std::unique_ptr<ParamToggle> link;
    PillButton delta, learn;
    IconButton reset { "Reset module", IconButton::Icon::reset };

    juce::String presetName, displayName, hint;
    juce::uint32 hintShownMs = 0;
    juce::Rectangle<int> nameArea, hintArea;
    int rightEdge = 0;
    ps::ChannelMode shownMode = ps::ChannelMode::Stereo;
    bool shownBypassed = false;
};

//==============================================================================
ModulePanel::ModulePanel (PureSheenProcessor& p, EditorServices& s, ps::ModuleType type)
    : processor (p), services (s), impl (std::make_unique<Impl> (*this, p, type))
{
    setTitle (ps::moduleInfo (type).name);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    impl->shownMode = currentMode (p, type);
    impl->tabs.setMode (impl->shownMode);
    impl->shownBypassed = p.getParameterValue (prefixOf (type) + ".on") < 0.5f;
}

ModulePanel::~ModulePanel() = default;

ps::ModuleType ModulePanel::getModuleType() const noexcept { return impl->type; }
int ModulePanel::getChannelSet() const noexcept { return impl->channelSet; }

void ModulePanel::setChannelSet (int set)
{
    const int clamped = set == 1 ? 1 : 0;

    if (clamped == impl->channelSet)
        return;

    impl->channelSet = clamped;
    impl->tabs.repaint();
    channelSetChanged();
}

juce::String ModulePanel::paramId (const juce::String& name, bool perChannelSet) const
{
    const auto prefix = prefixOf (impl->type);

    if (perChannelSet)
        return prefix + (impl->channelSet == 0 ? ".a." : ".b.") + name;

    return prefix + "." + name;
}

void ModulePanel::tick() { impl->tick(); }

void ModulePanel::refresh()
{
    impl->layoutHeader();
    impl->delta.setOn (processor.getDelta (impl->type));
    repaint();
}

void ModulePanel::showHeaderHint (const juce::String& text)
{
    impl->hint = text;
    impl->hintShownMs = juce::Time::getMillisecondCounter();
    repaint (impl->hintArea);
}

void ModulePanel::paint (juce::Graphics& g)
{
    auto& d = *impl;
    const auto colour = moduleColour (d.type);
    const auto b = getLocalBounds().toFloat();
    drawPanel (g, b);

    // Header band: a faint wash of the module colour, a separator under it.
    {
        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (b.reduced (1.0f), kCornerRadius - 1.0f);
        g.reduceClipRegion (clip);
        const auto header = b.withHeight ((float) kHeaderHeight);
        g.setGradientFill (juce::ColourGradient::horizontal (colour.withAlpha (0.10f), header.getX(),
                                                             colour.withAlpha (0.0f), header.getX() + header.getWidth() * 0.6f));
        g.fillRect (header);
    }

    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.fillRect (juce::Rectangle<float> (b.getX() + 1.0f, (float) kHeaderHeight, b.getWidth() - 2.0f, 1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.025f));
    g.fillRect (juce::Rectangle<float> (b.getX() + 1.0f, (float) kHeaderHeight + 1.0f, b.getWidth() - 2.0f, 1.0f));

    // Name with a colour dot.
    auto name = d.nameArea.toFloat();
    const auto dot = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ name.getX() + 5.0f, name.getCentreY() + 1.0f });
    g.setColour (colour.withAlpha (0.3f));
    g.fillEllipse (dot.expanded (3.0f));
    g.setColour (colour);
    g.fillEllipse (dot);
    g.setFont (font (14.5f, 700));
    g.setColour (Colours::text);
    g.drawText (d.displayName, name.withTrimmedLeft (18.0f).translated (0.0f, 1.0f), juce::Justification::centredLeft, false);

    if (d.hint.isNotEmpty() && d.hintArea.getWidth() > 40)
    {
        const float age = (float) (juce::Time::getMillisecondCounter() - d.hintShownMs) / (float) kHintMs;
        const float alpha = juce::jlimit (0.0f, 1.0f, (1.0f - age) * 4.0f);
        g.setFont (font (12.0f, 600));
        g.setColour (colour.brighter (0.3f).withAlpha (alpha));
        g.drawFittedText (d.hint, d.hintArea.reduced (8, 0).translated (0, 1), juce::Justification::centredRight, 1, 0.8f);
    }
    else if (d.shownBypassed && d.hintArea.getWidth() > 80)
    {
        g.setFont (font (11.5f, 700).withExtraKerningFactor (0.08f));
        g.setColour (Colours::minor.withAlpha (0.9f));
        g.drawText ("BYPASSED", d.hintArea.reduced (8, 0).translated (0, 1), juce::Justification::centredRight, false);
    }
}

void ModulePanel::paintOverChildren (juce::Graphics& g)
{
    if (! impl->shownBypassed || ! services.getOptions().dimBypassed)
        return;

    const auto area = getContentArea().toFloat().reduced (2.0f, 1.0f).withTrimmedBottom (1.0f);
    g.setColour (Colours::background.withAlpha (0.42f));
    g.fillRoundedRectangle (area, kCornerRadius - 2.0f);
}

void ModulePanel::resized()
{
    impl->layoutHeader();
    layoutContent (getContentArea());
}

juce::Rectangle<int> ModulePanel::getContentArea() const noexcept
{
    return getLocalBounds().withTrimmedTop (kHeaderHeight + 2);
}

//==============================================================================
// GenericModulePanel
//==============================================================================
namespace
{
struct SectionInfo
{
    const char* key;
    const char* title;
    const char* captionPrefix;
};

const SectionInfo kSections[] = {
    { "comp", "Compressor", "Comp " },
    { "lim", "Limiter", "Limiter " },
    { "clip", "Soft Clip", "Soft Clip " },
    { "trans", "Transient Emphasis", "Transient Emphasis " },
    { "si", "Stereo Independence", "Stereo Independence " },
    { "stz", "Stereoize", "Stereoize " },
    { "rec", "Recover Sides", "Recover Sides " },
    { "post", "Post Filter", "Post Filter " },
};

const SectionInfo* sectionFor (const juce::String& key)
{
    for (const auto& s : kSections)
        if (key == s.key)
            return &s;

    return nullptr;
}

enum class ControlKind { knob, toggle, choice, segments };

struct Control
{
    juce::String suffix;          // id after "<prefix>." or "<prefix>.<a|b>."
    bool perSet = false;
    ControlKind kind = ControlKind::knob;
    juce::String caption;
    std::unique_ptr<juce::Component> component;
    juce::Rectangle<int> captionArea;   // segments: caption drawn by the card
};

struct Section
{
    juce::String title;   // empty = the group's main section
    std::vector<Control*> controls;
    int titleY = -1;
};

juce::String idFor (ps::ModuleType type, const Control& c, int set)
{
    return prefixOf (type) + (c.perSet ? (set == 0 ? ".a." : ".b.") : ".") + c.suffix;
}

//==============================================================================
class GroupCard final : public juce::Component
{
public:
    GroupCard (juce::String t, int bandIndex, juce::Colour c) : title (std::move (t)), band (bandIndex), colour (c)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    static constexpr int kKnobW = 66, kKnobH = 80, kRowH = 24, kPad = 12, kTitleH = 30, kGap = 6;

    int columnsFor (int numKnobs) const
    {
        if (band > 0)
            return juce::jlimit (1, 3, numKnobs <= 4 ? 2 : 3);

        return juce::jlimit (1, 4, numKnobs);
    }

    int idealWidth() const
    {
        int maxKnobs = 0;

        for (const auto& s : sections)
        {
            int k = 0;
            for (auto* c : s.controls)
                k += c->kind == ControlKind::knob ? 1 : 0;
            maxKnobs = juce::jmax (maxKnobs, k);
        }

        const int cols = columnsFor (juce::jmax (1, maxKnobs));
        return juce::jmax (band > 0 ? 168 : 200, 2 * kPad + cols * kKnobW + (cols - 1) * kGap);
    }

    // Lays the controls out for a width; returns the card height.
    int layout (int width)
    {
        int y = kTitleH;
        const int inner = width - 2 * kPad;

        for (auto& s : sections)
        {
            bool any = false;
            for (auto* c : s.controls)
                any = any || c->component->isVisible();

            if (! any)
                continue;

            if (s.title.isNotEmpty())
            {
                s.titleY = y + 2;
                y += 20;
            }

            // Toggles: two per row; choices / segments: one per row; then the knob grid.
            std::vector<Control*> toggles, rows, knobs;

            for (auto* c : s.controls)
            {
                if (! c->component->isVisible())           continue;
                if (c->kind == ControlKind::toggle)        toggles.push_back (c);
                else if (c->kind == ControlKind::knob)     knobs.push_back (c);
                else                                        rows.push_back (c);
            }

            for (size_t i = 0; i < toggles.size(); i += 2)
            {
                const bool pair = i + 1 < toggles.size();
                const int w = pair ? (inner - kGap) / 2 : inner;
                toggles[i]->component->setBounds (kPad, y, w, kRowH);

                if (pair)
                    toggles[i + 1]->component->setBounds (kPad + w + kGap, y, w, kRowH);

                y += kRowH + kGap;
            }

            for (auto* c : rows)
            {
                if (c->kind == ControlKind::segments)
                {
                    const int captionW = juce::jmin (inner / 2, (int) textWidth (font (11.5f, 600), c->caption) + 10);
                    c->captionArea = { kPad, y, captionW, kRowH };
                    c->component->setBounds (kPad + captionW, y, inner - captionW, kRowH);
                }
                else
                {
                    c->component->setBounds (kPad, y, inner, kRowH);
                }

                y += kRowH + kGap;
            }

            if (! knobs.empty())
            {
                const int cols = juce::jmax (1, juce::jmin ((int) knobs.size(), (inner + kGap) / (kKnobW + kGap)));
                const int cellW = (inner - (cols - 1) * kGap) / cols;

                for (size_t i = 0; i < knobs.size(); ++i)
                {
                    const int col = (int) i % cols, row = (int) i / cols;
                    knobs[i]->component->setBounds (kPad + col * (cellW + kGap), y + 2 + row * (kKnobH + 4), cellW, kKnobH);
                }

                const int rowsN = ((int) knobs.size() + cols - 1) / cols;
                y += 2 + rowsN * (kKnobH + 4);
            }

            y += 4;
        }

        return y + kPad - 6;
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                           Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (Colours::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

        auto titleArea = juce::Rectangle<int> (kPad, 0, getWidth() - 2 * kPad, kTitleH);

        if (band > 0)
        {
            const auto badge = juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ (float) kPad + 9.0f, (float) kTitleH * 0.5f + 1.0f });
            g.setColour (colour.withAlpha (0.2f));
            g.fillRoundedRectangle (badge, 5.0f);
            g.setColour (colour);
            g.setFont (font (11.0f, 700));
            g.drawText (juce::String (band), badge, juce::Justification::centred, false);
            titleArea.removeFromLeft (26);
        }

        g.setFont (font (11.0f, 700).withExtraKerningFactor (0.08f));
        g.setColour (Colours::text.withAlpha (0.9f));
        g.drawText (title.toUpperCase(), titleArea.translated (0, 1), juce::Justification::centredLeft, true);

        if (subtitle.isNotEmpty())
        {
            g.setFont (font (11.0f, 600));
            g.setColour (Colours::textSubtle);
            g.drawText (subtitle, titleArea.translated (0, 1), juce::Justification::centredRight, true);
        }

        for (const auto& s : sections)
        {
            if (s.title.isEmpty() || s.titleY < 0)
                continue;

            const auto area = juce::Rectangle<int> (kPad, s.titleY, getWidth() - 2 * kPad, 16);
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.fillRect (area.getX(), area.getCentreY(), 6, 1);
            g.setFont (font (10.5f, 600).withExtraKerningFactor (0.06f));
            g.setColour (Colours::textDim);
            const int w = (int) textWidth (font (10.5f, 600).withExtraKerningFactor (0.06f), s.title.toUpperCase()) + 4;
            g.drawText (s.title.toUpperCase(), area.withTrimmedLeft (10), juce::Justification::centredLeft, false);
            g.setColour (Colours::outline.withAlpha (0.5f));
            g.fillRect (area.getX() + 14 + w, area.getCentreY(), juce::jmax (0, area.getWidth() - 14 - w), 1);
        }

        for (const auto& s : sections)
            for (auto* c : s.controls)
                if (c->kind == ControlKind::segments && c->component->isVisible())
                {
                    g.setFont (font (11.5f, 600));
                    g.setColour (Colours::textDim);
                    g.drawFittedText (c->caption, c->captionArea, juce::Justification::centredLeft, 1, 0.8f);
                }
    }

    juce::String title, subtitle;
    int band = 0;
    juce::Colour colour;
    std::vector<Section> sections;
};

//==============================================================================
class SpectrumView final : public juce::Component
{
public:
    SpectrumView (PureSheenProcessor& p, EditorServices& s, ps::ModuleType t) : processor (p), services (s), type (t)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setInterceptsMouseClicks (false, false);
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.25f), b.getY(),
                                                           Colours::background.darker (0.05f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (Colours::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

        const auto plot = b.reduced (1.0f, 1.0f);
        const auto& options = services.getOptions();
        const auto fx = makeFreqAxis (options, plot, processor.getChain().getSampleRate());
        DbAxis dy;
        dy.minDb = -84.0f;
        dy.maxDb = 0.0f;
        dy.area = plot;

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (plot, 6.0f);
        g.reduceClipRegion (clip);

        drawFrequencyGrid (g, fx, dy, true);

        const auto colour = moduleColour (type);
        auto& pre = services.getModulePreSpectrum();
        auto& post = services.getModulePostSpectrum();
        pre.draw (g, fx, dy.minDb, dy.maxDb, Colours::input.withAlpha (0.55f), options.spectrumFill);
        post.draw (g, fx, dy.minDb, dy.maxDb, colour, false);

        // Crossovers of multiband modules.
        const auto prefix = prefixOf (type);

        if (processor.getParameter (prefix + ".bands") != nullptr)
        {
            const int bands = juce::jlimit (1, ps::kMaxBands, juce::roundToInt (processor.getParameterValue (prefix + ".bands")));
            float lastX = plot.getX();

            for (int i = 0; i < bands; ++i)
            {
                const float x = i + 1 < bands ? fx.hzToX (processor.getParameterValue (prefix + ".xover" + juce::String (i + 1)))
                                              : plot.getRight();

                if (i + 1 < bands)
                {
                    g.setColour (colour.withAlpha (0.75f));
                    const float dashes[] = { 4.0f, 3.0f };
                    g.drawDashedLine (juce::Line<float> (x, plot.getY() + 4.0f, x, plot.getBottom() - 4.0f), dashes, 2, 1.2f);
                }

                const auto label = juce::Rectangle<float> (lastX, plot.getY() + 24.0f, x - lastX, 16.0f);

                if (label.getWidth() > 26.0f)
                {
                    g.setFont (font (10.5f, 700));
                    g.setColour (colour.withAlpha (0.85f));
                    g.drawText ("BAND " + juce::String (i + 1), label, juce::Justification::centred, false);
                }

                lastX = x;
            }
        }

        if (! post.hasData() && ! pre.hasData())
        {
            g.setFont (font (12.0f, 600));
            g.setColour (Colours::textSubtle);
            g.drawText ("Play audio to see the spectrum", plot, juce::Justification::centred, false);
        }

        // Legend
        auto legend = plot.reduced (10.0f, 8.0f).removeFromTop (14.0f);
        auto item = [&] (const juce::String& text, juce::Colour c)
        {
            const auto f = font (10.5f, 600);
            const float w = textWidth (f, text) + 16.0f;
            auto r = legend.removeFromRight (w);
            legend.removeFromRight (10.0f);
            g.setColour (c);
            g.fillRoundedRectangle (r.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 3.0f), 1.5f);
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r.withTrimmedLeft (5.0f), juce::Justification::centredLeft, false);
        };
        item ("Output", colour);
        item ("Input", Colours::input);
    }

private:
    PureSheenProcessor& processor;
    EditorServices& services;
    ps::ModuleType type;
};

struct CardsContent final : public juce::Component
{
    CardsContent()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }
};

juce::String bandRangeText (const PureSheenProcessor& p, const juce::String& prefix, int band, int numBands)
{
    auto xover = [&] (int i) { return p.getParameterValue (prefix + ".xover" + juce::String (i)); };
    const float lo = band == 1 ? 20.0f : xover (band - 1);
    const float hi = band == numBands ? 20000.0f : xover (band);
    auto shortHz = [] (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + "k" : juce::String (juce::roundToInt (hz));
    };
    return shortHz (lo) + " - " + shortHz (hi) + " Hz";
}
} // namespace

//==============================================================================
struct GenericModulePanel::GenericImpl
{
    GenericImpl (GenericModulePanel& o, PureSheenProcessor& p, EditorServices& s, ps::ModuleType t)
        : owner (o), processor (p), type (t), spectrum (p, s, t)
    {
        build();
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (8);
        viewport.setWantsKeyboardFocus (false);
        viewport.setMouseClickGrabsKeyboardFocus (false);
        owner.addAndMakeVisible (spectrum);
        owner.addAndMakeVisible (viewport);
    }

    void build()
    {
        const auto prefix = prefixOf (type);
        const auto shortName = juce::String (ps::moduleInfo (type).shortName);
        const bool channelModes = hasChannelModes (type);
        const auto colour = moduleColour (type);
        isMultiband = processor.getParameter (prefix + ".bands") != nullptr;

        // Group key: 0 = settings, -1 = crossover, -2, -3 ... = module sub-sections ("Soft Clip", "Stereoize",
        // "Detection"), n = band n. Cards are shown settings, sub-sections, crossover, bands.
        std::map<int, std::unique_ptr<GroupCard>> groupMap;
        std::map<int, std::map<juce::String, int>> sectionIndex;
        std::map<juce::String, int> sectionKeys;

        auto groupFor = [&] (int key, const juce::String& sectionCardTitle) -> GroupCard&
        {
            auto& slot = groupMap[key];

            if (slot == nullptr)
            {
                const juce::String title = key > 0 ? "Band " + juce::String (key)
                                                   : (key == -1 ? juce::String ("Crossover")
                                                                : (key == 0 ? juce::String ("Settings") : sectionCardTitle));
                slot = std::make_unique<GroupCard> (title, juce::jmax (0, key), colour);
                slot->sections.push_back ({});   // main section
                sectionIndex[key][{}] = 0;
            }

            return *slot;
        };

        for (const auto& spec : ps::paramSpecs())
        {
            if (spec.module != (int) type)
                continue;

            juce::String rest = juce::String (spec.id).fromFirstOccurrenceOf (".", false, false);

            // Slot parameters live in the chain strip / header.
            if (rest == "on")
                continue;

            if (channelModes && (rest == "chMode" || rest == "a.on" || rest == "b.on" || rest == "link"))
                continue;

            auto control = std::make_unique<Control>();

            if (rest.startsWith ("b."))
                continue;   // set B: the same controls rebind (set A builds them)

            if (rest.startsWith ("a."))
            {
                control->perSet = true;
                rest = rest.substring (2);
            }

            control->suffix = rest;

            int key = 0;
            juce::String local = rest;

            if (rest.startsWith ("b") && rest.length() > 2 && juce::CharacterFunctions::isDigit (rest[1]) && rest.containsChar ('.'))
            {
                key = rest.substring (1).getIntValue();
                local = rest.fromFirstOccurrenceOf (".", false, false);
            }
            else if (rest == "bands" || rest.startsWith ("xover"))
            {
                key = -1;
            }

            // Sub-section by the local id prefix ("comp.thr" -> Compressor).
            juce::String sectionTitle, captionPrefix;

            if (local.containsChar ('.'))
                if (const auto* info = sectionFor (local.upToFirstOccurrenceOf (".", false, false)))
                    sectionTitle = info->title, captionPrefix = info->captionPrefix;

            if (key == 0 && local.startsWith ("det"))
                sectionTitle = "Detection", captionPrefix = "Detection ";

            // Caption: host name without the module name, band and set.
            juce::String caption (spec.name);

            if (caption.startsWith (shortName + " "))
                caption = caption.substring (shortName.length() + 1);

            if (key > 0)
                caption = caption.fromFirstOccurrenceOf ("Band " + juce::String (key) + " ", false, false);

            if (caption.endsWith (" (A)") || caption.endsWith (" (B)"))
                caption = caption.dropLastCharacters (4);

            if (captionPrefix.isNotEmpty() && caption.startsWith (captionPrefix))
                caption = caption.substring (captionPrefix.length());

            if (caption == "On")
                caption = sectionTitle.isNotEmpty() ? sectionTitle : (key > 0 ? juce::String ("Band on") : juce::String ("On"));

            if (key == -1 && caption.startsWith ("Crossover ") && rest.startsWith ("xover")
                && juce::CharacterFunctions::isDigit (rest.getLastCharacter()))
            {
                const int n = caption.substring (10).getIntValue();
                caption = "Split " + juce::String (n) + "-" + juce::String (n + 1);
            }
            else if (key == -1 && caption.startsWith ("Crossover "))
            {
                caption = caption.substring (10);
            }

            if (caption == "Detection")
                caption = "Mode";
            else if (caption == "Upward Compress")
                caption = "Upward";

            // Module-level sub-sections get their own card.
            juce::String cardTitle;

            if (key == 0 && sectionTitle.isNotEmpty())
            {
                if (sectionKeys.find (sectionTitle) == sectionKeys.end())
                    sectionKeys[sectionTitle] = -2 - (int) sectionKeys.size();

                key = sectionKeys[sectionTitle];
                cardTitle = sectionTitle;

                if (caption == sectionTitle)
                    caption = spec.kind == ps::ParamKind::Bool ? "On" : "Amount";

                sectionTitle.clear();
            }

            control->caption = caption;

            // Widget kind.
            switch (spec.kind)
            {
                case ps::ParamKind::Bool:
                    control->kind = ControlKind::toggle;
                    break;

                case ps::ParamKind::Choice:
                {
                    bool shortLabels = spec.choices.size() <= 3;

                    for (const auto& c : spec.choices)
                        shortLabels = shortLabels && c.size() <= 9;

                    control->kind = shortLabels ? ControlKind::segments : ControlKind::choice;
                    break;
                }

                case ps::ParamKind::Float:
                default:
                    control->kind = ControlKind::knob;
                    break;
            }

            const auto id = idFor (type, *control, 0);

            switch (control->kind)
            {
                case ControlKind::knob:
                {
                    auto knob = std::make_unique<ParamKnob> (processor, id, caption);
                    knob->setColour (colour);
                    control->component = std::move (knob);
                    break;
                }
                case ControlKind::toggle:
                {
                    auto toggle = std::make_unique<ParamToggle> (processor, id, caption);
                    toggle->setOnColour (colour);
                    control->component = std::move (toggle);
                    break;
                }
                case ControlKind::segments:
                {
                    auto seg = std::make_unique<ParamSegments> (processor, id);
                    seg->setOnColour (colour);
                    control->component = std::move (seg);
                    break;
                }
                case ControlKind::choice:
                default:
                    control->component = std::make_unique<ParamChoice> (processor, id, caption);
                    break;
            }

            auto& group = groupFor (key, cardTitle);
            auto& sections = sectionIndex[key];

            if (sections.find (sectionTitle) == sections.end())
            {
                sections[sectionTitle] = (int) group.sections.size();
                group.sections.push_back ({ sectionTitle, {}, -1 });
            }

            group.sections[(size_t) sections[sectionTitle]].controls.push_back (control.get());
            group.addAndMakeVisible (*control->component);
            controls.push_back (std::move (control));
        }

        auto rank = [] (int key) { return key == 0 ? 0 : (key <= -2 ? 100 - key : (key == -1 ? 1000 : 2000 + key)); };
        std::vector<int> keys;

        for (auto& entry : groupMap)
            keys.push_back (entry.first);

        std::sort (keys.begin(), keys.end(), [&] (int a, int b) { return rank (a) < rank (b); });

        for (int key : keys)
        {
            cardKeys.push_back (key);
            content.addAndMakeVisible (*groupMap[key]);
            cards.push_back (std::move (groupMap[key]));
        }

        rebind();
    }

    void rebind()
    {
        const int set = owner.getChannelSet();
        const bool split = isSplitMode (currentMode (processor, type));
        const bool linked = split && processor.getParameterValue (prefixOf (type) + ".link") >= 0.5f;

        for (auto& c : controls)
        {
            if (! c->perSet)
                continue;

            const auto id = idFor (type, *c, set);
            const auto other = linked ? idFor (type, *c, 1 - set) : juce::String();

            if (auto* k = dynamic_cast<ParamKnob*> (c->component.get()))       { k->setParamId (id); k->setLinkedParamId (other); k->setColour (set == 1 && split ? Colours::sideChannel : moduleColour (type)); }
            else if (auto* t = dynamic_cast<ParamToggle*> (c->component.get())) { t->setParamId (id); t->setLinkedParamId (other); }
            else if (auto* s = dynamic_cast<ParamSegments*> (c->component.get())) { s->setParamId (id); s->setLinkedParamId (other); }
            else if (auto* ch = dynamic_cast<ParamChoice*> (c->component.get())) { ch->setParamId (id); ch->setLinkedParamId (other); }
        }

        shownLinked = linked;
        shownSet = set;
    }

    int visibleBands() const
    {
        if (! isMultiband)
            return 1000;

        return juce::jlimit (1, ps::kMaxBands, juce::roundToInt (processor.getParameterValue (prefixOf (type) + ".bands")));
    }

    void layout()
    {
        auto area = contentArea.reduced (12, 10);
        const int specH = owner.spectrumHeightFor (area.getHeight());
        spectrum.setVisible (specH > 0);

        if (specH > 0)
        {
            spectrum.setBounds (area.removeFromTop (specH));
            area.removeFromTop (10);
        }

        viewport.setBounds (area.withTrimmedRight (-6));
        const int width = juce::jmax (100, area.getWidth() - 4);
        const int bands = visibleBands();
        const int gap = 10;

        // Flow layout: cards left to right, wrapping; cards of one row share the row height and the spare width.
        struct Row { std::vector<GroupCard*> cards; int width = 0; };
        std::vector<Row> rows (1);
        const auto prefix = prefixOf (type);

        for (auto& c : controls)
            if (c->suffix.startsWith ("xover") && juce::CharacterFunctions::isDigit (c->suffix.getLastCharacter()))
                c->component->setVisible (c->suffix.getTrailingIntValue() < bands);

        for (size_t i = 0; i < cards.size(); ++i)
        {
            auto* card = cards[i].get();
            const int key = cardKeys[i];
            const bool visible = key <= 0 || key <= bands;
            card->setVisible (visible);

            if (! visible)
                continue;

            if (key > 0 && isMultiband)
                card->subtitle = bandRangeText (processor, prefix, key, bands);

            const int w = juce::jmin (width, card->idealWidth());

            if (! rows.back().cards.empty() && rows.back().width + gap + w > width)
                rows.emplace_back();

            rows.back().width += (rows.back().cards.empty() ? 0 : gap) + w;
            rows.back().cards.push_back (card);
        }

        int y = 0;

        for (auto& row : rows)
        {
            if (row.cards.empty())
                continue;

            // Share the spare width (bands rows fill the line; a lone settings card keeps its size up to 1.6x).
            const int spare = width - row.width;
            int x = 0, rowH = 0;
            std::vector<int> widths;

            for (auto* card : row.cards)
            {
                int w = juce::jmin (width, card->idealWidth());
                const int extra = spare / (int) row.cards.size();
                w += row.cards.size() == 1 ? juce::jmin (extra, w * 6 / 10) : extra;
                widths.push_back (w);
            }

            for (size_t i = 0; i < row.cards.size(); ++i)
                rowH = juce::jmax (rowH, row.cards[i]->layout (widths[i]));

            for (size_t i = 0; i < row.cards.size(); ++i)
            {
                row.cards[i]->setBounds (x, y, widths[i], rowH);
                row.cards[i]->layout (widths[i]);
                x += widths[i] + gap;
            }

            y += rowH + gap;
        }

        content.setSize (width, juce::jmax (0, y - gap));
        shownBands = bands;
    }

    GenericModulePanel& owner;
    PureSheenProcessor& processor;
    ps::ModuleType type;
    SpectrumView spectrum;
    juce::Viewport viewport;
    CardsContent content;
    std::vector<std::unique_ptr<Control>> controls;
    std::vector<std::unique_ptr<GroupCard>> cards;
    std::vector<int> cardKeys;
    juce::Rectangle<int> contentArea;
    bool isMultiband = false, shownLinked = false;
    int shownBands = -1, shownSet = 0;
};

GenericModulePanel::GenericModulePanel (PureSheenProcessor& p, EditorServices& s, ps::ModuleType type)
    : ModulePanel (p, s, type)
{
    generic = std::make_unique<GenericImpl> (*this, p, s, type);
}

GenericModulePanel::~GenericModulePanel() = default;

int GenericModulePanel::getNumControls() const noexcept { return (int) generic->controls.size(); }

bool GenericModulePanel::hasLearn() const
{
    return processor.getParameter (juce::String (ps::moduleInfo (getModuleType()).idPrefix) + ".bands") != nullptr;
}

int GenericModulePanel::spectrumHeightFor (int contentHeight) const
{
    return contentHeight < 260 ? 0 : juce::jlimit (110, 250, (int) ((float) contentHeight * 0.34f));
}

void GenericModulePanel::layoutContent (juce::Rectangle<int> contentArea)
{
    generic->contentArea = contentArea;
    generic->layout();
}

void GenericModulePanel::tick()
{
    ModulePanel::tick();
    auto& g = *generic;

    const bool split = isSplitMode (currentMode (processor, getModuleType()));
    const bool linked = split && processor.getParameterValue (juce::String (ps::moduleInfo (getModuleType()).idPrefix) + ".link") >= 0.5f;

    if (linked != g.shownLinked)
        g.rebind();

    if (g.visibleBands() != g.shownBands)
        g.layout();
    else if (g.isMultiband)
        for (size_t i = 0; i < g.cards.size(); ++i)
            if (g.cardKeys[i] > 0 && g.cards[i]->isVisible())
            {
                const auto text = bandRangeText (processor, ps::moduleInfo (getModuleType()).idPrefix, g.cardKeys[i], g.shownBands);
                if (text != g.cards[i]->subtitle)
                {
                    g.cards[i]->subtitle = text;
                    g.cards[i]->repaint();
                }
            }

    if (g.spectrum.isVisible())
        g.spectrum.repaint();
}

void GenericModulePanel::refresh()
{
    ModulePanel::refresh();
    generic->rebind();
    generic->layout();
}

void GenericModulePanel::channelSetChanged()
{
    generic->rebind();
}

} // namespace ps::ui
