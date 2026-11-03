#include "ui/ReferencePanel.h"

#include "plugin/TrackReferencing.h"
#include "ui/Controls.h"
#include "ui/RefWaveform.h"

#include <cmath>

// The Reference page (see ReferencePanel.h). Polls Track Referencing at the editor's 30 Hz tick: the list and the
// waveform only when getInfoVersion() changes, the profiles and the comparison only when getAnalysisVersion() changes
// (the views ease towards them every tick), the playhead and the A/B fade every tick (small repaints).
// Choices: clicking a missing / unreadable reference selects it and opens the file chooser to find it (its loops are
// kept); the Master | Reference switch is the same A/B as the header's REF pill; the controls wrap onto two rows
// next to a taller switch when the page is narrow.
namespace ps::ui
{
namespace
{
using TR = psplug::TrackReferencing;
namespace Col = Colours;

constexpr int kRowH = 36;

juce::String timeText (double seconds)
{
    const int s = juce::jmax (0, (int) std::floor (seconds + 0.5));
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

const TR::Entry* selectedEntry (const TR::Info& info)
{
    return info.selected >= 0 && info.selected < (int) info.entries.size() ? &info.entries[(size_t) info.selected] : nullptr;
}

void drawDashed (juce::Graphics& g, juce::Rectangle<float> r, float radius, float thickness)
{
    juce::Path outline, dashed;
    outline.addRoundedRectangle (r, radius);
    const float dashes[] = { 6.0f, 5.0f };
    juce::PathStrokeType (thickness).createDashedStroke (dashed, outline, dashes, 2);
    g.fillPath (dashed);
}

// Arrow down into a tray (the drop icon).
void drawDropIcon (juce::Graphics& g, juce::Rectangle<float> icon, juce::Colour tint)
{
    g.setColour (tint.withAlpha (0.16f));
    g.fillEllipse (icon);
    g.setColour (tint.withAlpha (0.55f));
    g.drawEllipse (icon.reduced (0.6f), 1.2f);
    const auto c = icon.getCentre();
    const float s = icon.getWidth() * 0.2f;
    juce::Path p;
    p.startNewSubPath (c.x, c.y - s * 1.3f);
    p.lineTo (c.x, c.y + s * 0.5f);
    p.startNewSubPath (c.x - s * 0.8f, c.y - s * 0.3f);
    p.lineTo (c.x, c.y + s * 0.5f);
    p.lineTo (c.x + s * 0.8f, c.y - s * 0.3f);
    p.startNewSubPath (c.x - s * 1.4f, c.y + s * 0.6f);
    p.lineTo (c.x - s * 1.4f, c.y + s * 1.3f);
    p.lineTo (c.x + s * 1.4f, c.y + s * 1.3f);
    p.lineTo (c.x + s * 1.4f, c.y + s * 0.6f);
    g.setColour (tint.brighter (0.25f));
    g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
// The reference list: one row per reference, "+ Add" in the header. Scrolls with the wheel when it does not fit.
class RefList final : public juce::Component, public juce::TooltipClient
{
public:
    RefList()
    {
        setTitle ("Reference list");
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        add.setTooltip ("Add reference tracks (WAV, MP3, FLAC, AIFF, OGG; up to 10). Pick released songs you like in a "
                        "similar style. You can also drop files anywhere on PureSheen.");
        add.onClick = [this] { if (onAdd != nullptr) onAdd(); };
        addAndMakeVisible (add);
    }

    std::function<void (int id)> onSelect, onRemove, onRelink;
    std::function<void()> onAdd;

    void set (const TR::Info& info)
    {
        entries = info.entries;
        const auto* sel = selectedEntry (info);
        selectedId = sel != nullptr ? sel->id : 0;
        add.setEnabled ((int) entries.size() < TR::kMaxReferences);
        scroll = juce::jlimit (0.0f, maxScroll(), scroll);
        repaint();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (10, 8);
        header = b.removeFromTop (26).toFloat();
        b.removeFromTop (6);
        listArea = b.toFloat();
        add.setBounds (header.removeFromRight (66.0f).withSizeKeepingCentre (66.0f, 24.0f).toNearestInt());
        scroll = juce::jlimit (0.0f, maxScroll(), scroll);
    }

    void paint (juce::Graphics& g) override
    {
        drawPanel (g, getLocalBounds().toFloat());
        auto h = header;
        drawSectionTitle (g, "References", h.toNearestInt());
        g.setFont (font (10.5f));
        g.setColour (Col::textSubtle);
        g.drawText (juce::String ((int) entries.size()) + " / " + juce::String (TR::kMaxReferences),
                    h.withTrimmedLeft (textWidth (font (10.5f, 600).withExtraKerningFactor (0.09f), "REFERENCES") + 10.0f),
                    juce::Justification::centredLeft, false);

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (listArea.toNearestInt());
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            const auto row = rowBounds (i);
            if (row.intersects (listArea))
                paintRow (g, i, row);
        }

        // Under the rows, while there is room: how to add more
        const float rowsBottom = listArea.getY() + (float) ((int) entries.size() * kRowH) - scroll;
        if ((int) entries.size() < TR::kMaxReferences && listArea.getBottom() - rowsBottom > 70.0f)
        {
            g.setFont (font (11.0f));
            g.setColour (Col::textSubtle);
            g.drawFittedText ("Drop more songs anywhere on PureSheen, or click + Add.",
                              juce::Rectangle<float> (listArea.getX() + 8.0f, rowsBottom + 12.0f, listArea.getWidth() - 16.0f, 32.0f).toNearestInt(),
                              juce::Justification::centredTop, 2, 0.9f);
        }

        if (maxScroll() > 0.0f)
        {
            // scroll position, a thin bar at the right edge
            const float frac = listArea.getHeight() / ((float) entries.size() * (float) kRowH);
            const float barH = juce::jmax (20.0f, listArea.getHeight() * frac);
            const float y = listArea.getY() + (listArea.getHeight() - barH) * (scroll / maxScroll());
            g.setColour (Col::textFaint.withAlpha (0.6f));
            g.fillRoundedRectangle (juce::Rectangle<float> (listArea.getRight() - 3.0f, y, 3.0f, barH), 1.5f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int r = rowAt (e.position);
        const bool x = r >= 0 && removeArea (r).expanded (3.0f).contains (e.position);
        if (r != hoverRow || x != hoverRemove)
        {
            hoverRow = r;
            hoverRemove = x;
            repaint();
        }
        setMouseCursor (r >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverRow = -1;
        hoverRemove = false;
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        scroll = juce::jlimit (0.0f, maxScroll(), scroll - w.deltaY * 120.0f);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e))
            return;
        const int r = rowAt (e.position);
        if (r < 0)
            return;
        const auto& entry = entries[(size_t) r];
        if (removeArea (r).expanded (3.0f).contains (e.position))
        {
            if (onRemove != nullptr)
                onRemove (entry.id);
            return;
        }
        if (onSelect != nullptr && entry.id != selectedId)
            onSelect (entry.id);
        if ((entry.state == TR::State::Missing || entry.state == TR::State::Error) && onRelink != nullptr)
            onRelink (entry.id);
    }

    juce::String getTooltip() override
    {
        const auto p = getMouseXYRelative().toFloat();
        const int r = rowAt (p);
        if (r < 0)
            return header.contains (p) ? "Your reference tracks. Click one to compare your master with it." : juce::String();
        const auto& e = entries[(size_t) r];
        if (removeArea (r).expanded (3.0f).contains (p))
            return "Remove \"" + e.name + "\" from the list (the file itself is not touched)";
        juce::String tip = e.name + "\n" + e.file.getFullPathName() + "\n\n";
        switch (e.state)
        {
            case TR::State::Loading: tip << "Loading: decoding and measuring the track (once)."; break;
            case TR::State::Error:   tip << "Could not be loaded: " << e.error << ". Click to pick another file (its loops are kept)."; break;
            case TR::State::Missing: tip << "The file was not found (moved, renamed, or another computer). Click to find it."; break;
            case TR::State::Ready:
                tip << timeText (e.lengthSeconds) << " long, " << (ps::hasValue (e.integratedLufs) ? juce::String (e.integratedLufs, 1) + " LUFS"
                                                                                                    : juce::String ("-- LUFS"))
                    << " integrated, true peak "
                    << (ps::hasValue (e.truePeakDb) ? juce::String (e.truePeakDb, 1) + " dBTP" : juce::String ("--")) << ". "
                    << (e.id == selectedId ? "This is the reference you compare with." : "Click to compare with this one.");
                break;
        }
        return tip;
    }

private:
    juce::Rectangle<float> rowBounds (int i) const
    {
        return { listArea.getX(), listArea.getY() + (float) (i * kRowH) - scroll, listArea.getWidth(), (float) kRowH - 2.0f };
    }

    juce::Rectangle<float> removeArea (int i) const
    {
        const auto row = rowBounds (i);
        return juce::Rectangle<float> (16.0f, 16.0f).withCentre ({ row.getRight() - 13.0f, row.getCentreY() });
    }

    int rowAt (juce::Point<float> p) const
    {
        if (! listArea.contains (p))
            return -1;
        const int r = (int) ((p.y - listArea.getY() + scroll) / (float) kRowH);
        return r >= 0 && r < (int) entries.size() ? r : -1;
    }

    float maxScroll() const { return juce::jmax (0.0f, (float) ((int) entries.size() * kRowH) - listArea.getHeight()); }

    void paintRow (juce::Graphics& g, int i, juce::Rectangle<float> row) const
    {
        const auto& e = entries[(size_t) i];
        const bool sel = e.id == selectedId, hover = hoverRow == i;
        if (sel || hover)
        {
            g.setColour (sel ? blend (Col::panelRaised, Col::reference, 0.16f) : Col::panelHover.withAlpha (0.7f));
            g.fillRoundedRectangle (row, 6.0f);
        }
        if (sel)
        {
            g.setColour (Col::reference);
            g.fillRoundedRectangle (juce::Rectangle<float> (row.getX(), row.getY() + 7.0f, 3.0f, row.getHeight() - 14.0f), 1.5f);
        }

        // Number
        const auto badge = juce::Rectangle<float> (19.0f, 19.0f).withCentre ({ row.getX() + 17.0f, row.getCentreY() });
        if (sel)
        {
            g.setColour (Col::reference);
            g.fillEllipse (badge);
        }
        else
        {
            g.setColour (Col::outline.brighter (0.2f));
            g.drawEllipse (badge.reduced (0.5f), 1.0f);
        }
        g.setFont (font (10.5f, 700));
        g.setColour (sel ? Col::background : Col::textDim);
        g.drawText (juce::String (i + 1), badge, juce::Justification::centred, false);

        auto text = row.withTrimmedLeft (34.0f).withTrimmedRight (hover ? 26.0f : 8.0f).reduced (0.0f, 3.0f);
        auto line1 = text.removeFromTop (text.getHeight() * 0.5f + 1.0f);
        auto line2 = text;

        g.setFont (font (12.5f, 600));
        g.setColour (e.state == TR::State::Ready || e.state == TR::State::Loading ? Col::text : Col::textDim);
        g.drawFittedText (e.name, line1.toNearestInt(), juce::Justification::bottomLeft, 1, 0.85f);

        g.setFont (font (11.0f));
        switch (e.state)
        {
            case TR::State::Ready:
            {
                g.setColour (Col::textSubtle);
                g.drawText (timeText (e.lengthSeconds), line2, juce::Justification::centredLeft, false);
                const auto lufs = ps::hasValue (e.integratedLufs) ? juce::String (e.integratedLufs, 1) + " LUFS" : juce::String ("-- LUFS");
                g.setColour (sel ? Col::reference.brighter (0.1f) : Col::textDim);
                g.drawText (lufs, line2, juce::Justification::centredRight, false);
                // the set loops as small coloured letters after the length
                float x = line2.getX() + textWidth (font (11.0f), timeText (e.lengthSeconds)) + 8.0f;
                for (int l = 0; l < TR::kNumLoops; ++l)
                {
                    if (! e.loops[(size_t) l].set)
                        continue;
                    const auto dot = juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ x + 2.5f, line2.getCentreY() });
                    g.setColour (loopColour (l).withAlpha (e.activeLoop == l ? 1.0f : 0.55f));
                    g.fillEllipse (dot);
                    x += 8.0f;
                }
                break;
            }
            case TR::State::Loading:
            {
                const auto bar = line2.withSizeKeepingCentre (line2.getWidth(), 3.0f).withWidth (line2.getWidth() * 0.55f);
                g.setColour (Col::outline);
                g.fillRoundedRectangle (bar, 1.5f);
                g.setColour (Col::reference);
                g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * juce::jlimit (0.0f, 1.0f, e.progress))), 1.5f);
                g.setColour (Col::textSubtle);
                g.drawText (juce::String (juce::roundToInt (e.progress * 100.0f)) + " %", line2, juce::Justification::centredRight, false);
                break;
            }
            case TR::State::Error:
                g.setColour (Col::major.brighter (0.1f));
                g.drawFittedText ("Could not read it - click to pick another", line2.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
                break;
            case TR::State::Missing:
                g.setColour (Col::minor);
                g.drawFittedText ("Missing - click to find it", line2.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
                break;
        }

        if (hover)
        {
            const auto x = removeArea (i);
            if (hoverRemove)
            {
                g.setColour (Col::panelHover.brighter (0.1f));
                g.fillEllipse (x);
            }
            g.setColour (hoverRemove ? Col::text : Col::textSubtle);
            const auto c = x.reduced (4.5f);
            g.drawLine ({ c.getTopLeft(), c.getBottomRight() }, 1.4f);
            g.drawLine ({ c.getBottomLeft(), c.getTopRight() }, 1.4f);
        }
    }

    PillButton add { "+ Add" };
    std::vector<TR::Entry> entries;
    int selectedId = 0, hoverRow = -1;
    bool hoverRemove = false;
    float scroll = 0.0f;
    juce::Rectangle<float> header, listArea;
};

//==============================================================================
// The big Master | Reference switch (A/B). The highlight follows the processor's A/B fade; the reference side's
// sub-label shows the level match.
class ListenSwitch final : public juce::Component, public juce::TooltipClient
{
public:
    ListenSwitch()
    {
        setTitle ("Master or reference");
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
    }

    std::function<void (bool reference)> onChoose;

    void set (float listenPosition, bool available, const juce::String& referenceSub, const juce::String& tip)
    {
        if (std::abs (listenPosition - position) > 0.004f || available != isAvailable || referenceSub != refSub)
        {
            position = listenPosition;
            isAvailable = available;
            refSub = referenceSub;
            repaint();
        }
        tooltip = tip;
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = juce::jmin (10.0f, b.getHeight() * 0.5f);
        drawChip (g, b, radius, Col::background.darker (0.1f), Col::outline);

        const float t = juce::jlimit (0.0f, 1.0f, position);
        const float half = b.getWidth() * 0.5f;
        const auto knob = juce::Rectangle<float> (b.getX() + half * t, b.getY(), half, b.getHeight()).reduced (3.0f);
        const auto tint = blend (Col::output, Col::reference, t);
        drawChip (g, knob, radius - 2.0f, blend (Col::panelRaised, tint, 0.28f), tint.withAlpha (0.85f));

        const auto mouse = getMouseXYRelative().toFloat();
        for (int side = 0; side < 2; ++side)
        {
            auto area = juce::Rectangle<float> (b.getX() + half * (float) side, b.getY(), half, b.getHeight()).reduced (8.0f, 4.0f);
            const bool on = side == 0 ? t < 0.5f : t >= 0.5f;
            const bool enabled = side == 0 || isAvailable;
            const bool hover = isMouseOver (true) && area.contains (mouse) && ! on && enabled;
            const auto colour = side == 0 ? Col::output : Col::reference;
            const bool twoLines = area.getHeight() >= 26.0f;
            auto titleArea = twoLines ? area.removeFromTop (area.getHeight() * 0.55f) : area;
            g.setFont (font (juce::jmin (13.5f, titleArea.getHeight() * 0.75f), 700).withExtraKerningFactor (0.05f));
            g.setColour (! enabled ? Col::textFaint : (on ? colour.brighter (0.25f) : (hover ? Col::text : Col::textDim)));
            g.drawFittedText (side == 0 ? "MASTER" : "REFERENCE", titleArea.toNearestInt(),
                              twoLines ? juce::Justification::centredBottom : juce::Justification::centred, 1, 0.75f);
            if (twoLines)
            {
                g.setFont (font (10.5f, 600));
                g.setColour (! enabled ? Col::textFaint : (on ? Col::text.withAlpha (0.85f) : Col::textSubtle));
                g.drawFittedText (side == 0 ? juce::String ("your output") : refSub, area.toNearestInt(), juce::Justification::centredTop, 1, 0.8f);
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e) || ! getLocalBounds().contains (e.getPosition()) || onChoose == nullptr)
            return;
        onChoose (e.position.x >= (float) getWidth() * 0.5f);
    }

    juce::String getTooltip() override { return tooltip; }

private:
    float position = 0.0f;
    bool isAvailable = false;
    juce::String refSub, tooltip;
};
} // namespace

//==============================================================================
struct ReferencePanel::Impl
{
    Impl (ReferencePanel& o, EditorContext& c)
        : owner (o), ctx (c), refs (c.getProcessor().getTrackReferencing()), waveform (refs)
    {
        list.onAdd = [this] { browse(); };
        list.onSelect = [this] (int id) { refs.select (id); tick(); };
        list.onRemove = [this] (int id) { refs.removeReference (id); tick(); };
        list.onRelink = [this] (int id) { relink (id); };
        waveform.onRelink = [this] {
            const auto info = refs.getInfo();
            if (const auto* e = selectedEntry (info))
                relink (e->id);
        };

        listen.onChoose = [this] (bool reference) {
            if (reference && ! referenceReady)
            {
                ctx.notify (selectedId == 0 ? "Add a reference track first" : "The reference is not ready yet");
                return;
            }
            refs.setListening (reference);
        };

        levelMatch.setTooltip ("Level match: PureSheen turns the reference up or down so it plays as loud as your master "
                               "(your master is never changed). Loud songs always sound better, so match them to hear "
                               "the real differences.");
        levelMatch.onOpenMenu = [this] (MenuChip& chip) { showLevelMatchMenu (chip); };
        window.setTooltip ("Analysis window of your master: Recent follows the last 30 seconds or so (good while you "
                           "tweak), Since reset averages everything played since you pressed Reset (a whole song).");
        window.onOpenMenu = [this] (MenuChip& chip) { showWindowMenu (chip); };

        sync.setText ("Sync");
        sync.setTooltip ("Follow the DAW position: while your DAW plays, the reference plays from the same time (inside "
                         "the active loop it wraps around). Off: it plays on its own from where it is.");
        sync.onClick = [this] { auto s = refs.getSettings(); s.sync = ! s.sync; refs.setSettings (s); };
        stopped.setText ("Play when stopped");
        stopped.setTooltip ("Keep playing the reference while your DAW is stopped, so you can listen to it on its own.");
        stopped.onClick = [this] { auto s = refs.getSettings(); s.playWhenStopped = ! s.playWhenStopped; refs.setSettings (s); };
        reset.setText ("Reset");
        reset.setTooltip ("Reset the analysis of your master (loudness, spectrum, stereo), e.g. after you changed the "
                          "chain or moved to another part of the song.");
        reset.onClick = [this] { refs.resetMixAnalysis(); ctx.notify ("The analysis of your master was reset"); };

        for (juce::Component* comp : std::initializer_list<juce::Component*> { &list, &waveform, &listen, &levelMatch, &window,
                                                                              &sync, &stopped, &reset, &comparison })
            owner.addAndMakeVisible (comp);
    }

    //==========================================================================
    void browse()
    {
        if ((int) refs.getInfo().entries.size() >= TR::kMaxReferences)
        {
            ctx.notify ("The reference list is full (10 tracks)");
            return;
        }
        chooser = std::make_unique<juce::FileChooser> ("Add reference tracks", lastFolder, TR::getSupportedWildcard());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this, safe = juce::Component::SafePointer<ReferencePanel> (&owner)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr)
                                      return;
                                  juce::StringArray paths;
                                  for (const auto& f : fc.getResults())
                                      paths.add (f.getFullPathName());
                                  if (! paths.isEmpty())
                                  {
                                      lastFolder = juce::File (paths[0]).getParentDirectory();
                                      ReferencePanel::addFiles (ctx, paths);
                                      tick();
                                  }
                              });
    }

    void relink (int id)
    {
        juce::String name;
        juce::File where = lastFolder;
        for (const auto& e : refs.getInfo().entries)
            if (e.id == id)
            {
                name = e.name;
                if (e.file.getParentDirectory().isDirectory())
                    where = e.file.getParentDirectory();
            }
        chooser = std::make_unique<juce::FileChooser> ("Find \"" + name + "\"", where, TR::getSupportedWildcard());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, id, safe = juce::Component::SafePointer<ReferencePanel> (&owner)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr)
                                      return;
                                  const auto f = fc.getResult();
                                  if (! f.existsAsFile())
                                      return;
                                  if (! TR::isSupportedFile (f))
                                  {
                                      ctx.notify ("That file type is not supported - use WAV, MP3, FLAC, AIFF or OGG");
                                      return;
                                  }
                                  lastFolder = f.getParentDirectory();
                                  refs.relink (id, f);
                                  tick();
                              });
    }

    void showLevelMatchMenu (MenuChip& chip)
    {
        const auto s = refs.getSettings();
        juce::PopupMenu menu;
        menu.addSectionHeader ("Level match");
        menu.addItem (1, "Integrated - the average loudness (recommended)", true, s.levelMatch == ps::LevelMatch::Integrated);
        menu.addItem (2, "Short-term - the last 3 seconds, as they play", true, s.levelMatch == ps::LevelMatch::ShortTerm);
        menu.addItem (3, "Off - the reference plays at its own level", true, s.levelMatch == ps::LevelMatch::Off);
        chip.setMenuOpen (true);
        showMenu (menu, chip, [this, &chip] (int r) {
            chip.setMenuOpen (false);
            if (r <= 0)
                return;
            auto n = refs.getSettings();
            n.levelMatch = r == 1 ? ps::LevelMatch::Integrated : (r == 2 ? ps::LevelMatch::ShortTerm : ps::LevelMatch::Off);
            refs.setSettings (n);
            tick();
        });
    }

    void showWindowMenu (MenuChip& chip)
    {
        const auto s = refs.getSettings();
        juce::PopupMenu menu;
        menu.addSectionHeader ("Analysis of your master");
        menu.addItem (1, "Recent - about the last 30 seconds", true, s.averaging == ps::Averaging::Recent);
        menu.addItem (2, "Since reset - everything since Reset", true, s.averaging == ps::Averaging::SinceReset);
        chip.setMenuOpen (true);
        showMenu (menu, chip, [this, &chip] (int r) {
            chip.setMenuOpen (false);
            if (r <= 0)
                return;
            auto n = refs.getSettings();
            n.averaging = r == 1 ? ps::Averaging::Recent : ps::Averaging::SinceReset;
            refs.setSettings (n);
            tick();
        });
    }

    //==========================================================================
    void tick()
    {
        const int version = refs.getInfoVersion();
        bool snap = false;
        if (version != infoVersion)
        {
            infoVersion = version;
            const auto info = refs.getInfo();
            const auto* sel = selectedEntry (info);
            const int newId = sel != nullptr ? sel->id : 0;
            const int newLoop = sel != nullptr ? sel->activeLoop : -1;
            snap = newId != selectedId || newLoop != activeLoop;
            selectedId = newId;
            activeLoop = newLoop;
            referenceReady = sel != nullptr && sel->state == TR::State::Ready;
            data.hasReference = sel != nullptr;
            data.referencePending = sel != nullptr && (sel->state == TR::State::Loading || sel->analysing);
            data.referenceProblem = sel == nullptr ? juce::String()
                                  : (sel->state == TR::State::Missing ? juce::String ("The reference file is missing")
                                  : (sel->state == TR::State::Error ? juce::String ("The reference could not be loaded") : juce::String()));
            selectedState = sel != nullptr ? sel->state : TR::State::Loading;
            list.set (info);
            waveform.setEntry (sel, version);

            const bool nowEmpty = info.entries.empty();
            if (nowEmpty != empty)
            {
                empty = nowEmpty;
                layout();
            }
            analysisVersion = -1;   // re-read the profiles for the new selection / region
        }

        const int av = refs.getAnalysisVersion();
        if (av != analysisVersion)
        {
            analysisVersion = av;
            data.mix = refs.getMixProfile();
            data.reference = refs.getReferenceProfile();
            data.comparison = refs.getComparison();
        }
        data.referenceShortTermLufs = refs.getReferenceShortTermLufs();
        comparison.setData (data, snap);
        waveform.tick();
        updateControls();
    }

    void updateControls()
    {
        const auto s = refs.getSettings();
        const float gainDb = refs.getMatchGainDb();
        juce::String sub, tip;
        if (selectedId == 0)
            sub = "none yet";
        else if (! referenceReady)
            sub = selectedState == TR::State::Missing ? "file missing" : (selectedState == TR::State::Error ? "could not load" : "loading...");
        else if (s.levelMatch == ps::LevelMatch::Off)
            sub = "own level";
        else
            sub = "matched " + juce::String (ps::formatSigned (gainDb, 1)) + " dB";

        tip = "Master | Reference: what you hear. Master is PureSheen's output as it is; Reference plays the selected "
              "reference track instead (the header's REF pill does the same).";
        if (referenceReady && s.levelMatch != ps::LevelMatch::Off)
        {
            if (std::abs (gainDb) < 0.05f)
                tip << " The reference plays at the same loudness as your master.";
            else
                tip << " Reference turned " << (gainDb < 0.0f ? "down " : "up ") << juce::String (std::abs (gainDb), 1)
                    << " dB to match your master.";
        }
        listen.set (refs.getListenPosition(), referenceReady, sub, tip);

        const char* matchNames[] { "Off", "Integrated", "Short-term" };
        const int matchWidth = levelMatch.getIdealWidth(), windowWidth = window.getIdealWidth();
        levelMatch.setText ("Match", matchNames[juce::jlimit (0, 2, (int) s.levelMatch)]);
        window.setText ("Window", s.averaging == ps::Averaging::Recent ? "Recent 30 s" : "Since reset");
        sync.setOn (s.sync);
        stopped.setOn (s.playWhenStopped);
        // The chips are sized from their text: lay out again when it changed their width (the first update too).
        if (! empty && (levelMatch.getIdealWidth() != matchWidth || window.getIdealWidth() != windowWidth))
            layout();
    }

    //==========================================================================
    void layout()
    {
        auto r = owner.getLocalBounds().reduced (12);
        for (auto* c : owner.getChildren())
            c->setVisible (! empty);

        if (empty)
        {
            dropZone = r.toFloat();
            owner.setTooltip ("Add reference tracks: drop audio files here (or anywhere on PureSheen), or click to "
                              "browse. Up to 10 songs; PureSheen compares your master with the one you select.");
            owner.repaint();
            return;
        }
        dropZone = {};
        owner.setTooltip ({});

        list.setBounds (r.removeFromLeft (juce::jlimit (180, 250, r.getWidth() * 26 / 100)));
        r.removeFromLeft (12);
        waveform.setBounds (r.removeFromTop (juce::jlimit (88, 128, r.getHeight() * 18 / 100)));
        r.removeFromTop (8);

        // Controls: one row when everything fits, else the switch next to two rows of controls.
        const int gap = 8;
        const int matchW = levelMatch.getIdealWidth() + 4, windowW = juce::jmax (window.getIdealWidth() + 4, 150);
        const int syncW = sync.getIdealWidth (26.0f) + 6, stoppedW = stopped.getIdealWidth (26.0f) + 6, resetW = reset.getIdealWidth (26.0f) + 6;
        const int restOneRow = matchW + windowW + syncW + stoppedW + resetW + 5 * gap;
        if (r.getWidth() - restOneRow >= 200)
        {
            auto row = r.removeFromTop (40);
            listen.setBounds (row.removeFromLeft (juce::jmin (260, row.getWidth() - restOneRow)));
            row.removeFromLeft (gap + 4);
            const std::initializer_list<std::pair<juce::Component*, int>> items {
                { &levelMatch, matchW }, { &window, windowW }, { &sync, syncW }, { &stopped, stoppedW }, { &reset, resetW } };
            for (const auto& [comp, w] : items)
            {
                comp->setBounds (row.removeFromLeft (w).withSizeKeepingCentre (w, 28));
                row.removeFromLeft (gap);
            }
        }
        else
        {
            auto rows = r.removeFromTop (62);
            listen.setBounds (rows.removeFromLeft (juce::jlimit (150, 230, rows.getWidth() - juce::jmax (matchW + windowW, syncW + stoppedW + resetW + gap) - 2 * gap - 4)));
            rows.removeFromLeft (gap + 4);
            auto top = rows.removeFromTop (28);
            rows.removeFromTop (6);
            auto bottom = rows;
            levelMatch.setBounds (top.removeFromLeft (matchW));
            top.removeFromLeft (gap);
            window.setBounds (top.removeFromLeft (juce::jmin (windowW, top.getWidth())));
            sync.setBounds (bottom.removeFromLeft (syncW));
            bottom.removeFromLeft (gap);
            stopped.setBounds (bottom.removeFromLeft (stoppedW));
            bottom.removeFromLeft (gap);
            reset.setBounds (bottom.removeFromLeft (juce::jmin (resetW, bottom.getWidth())));
        }
        r.removeFromTop (8);
        comparison.setBounds (r);
        owner.repaint();
    }

    void paintEmpty (juce::Graphics& g) const
    {
        const bool hot = owner.isMouseOver (true);
        g.setColour (Col::reference.withAlpha (hot ? 0.08f : 0.04f));
        g.fillRoundedRectangle (dropZone, 10.0f);
        g.setColour (Col::reference.withAlpha (hot ? 0.7f : 0.38f));
        drawDashed (g, dropZone.reduced (0.75f), 10.0f, 1.5f);

        const auto c = dropZone.getCentre();
        const float iconD = juce::jlimit (44.0f, 64.0f, dropZone.getHeight() * 0.13f);
        drawDropIcon (g, juce::Rectangle<float> (iconD, iconD).withCentre (c.translated (0.0f, -iconD * 0.9f)), Col::reference);

        auto text = juce::Rectangle<float> (dropZone.getWidth() - 40.0f, 80.0f).withCentre (c.translated (0.0f, 26.0f));
        g.setFont (font (18.0f, 700));
        g.setColour (Col::text);
        g.drawFittedText ("Drop reference tracks here", text.removeFromTop (26.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
        g.setFont (font (13.0f));
        g.setColour (Col::textDim);
        g.drawFittedText ("WAV, MP3, FLAC, AIFF, OGG - up to 10 songs - or click to browse", text.removeFromTop (22.0f).toNearestInt(),
                          juce::Justification::centred, 1, 0.8f);
        g.setFont (font (12.0f));
        g.setColour (Col::textSubtle);
        g.drawFittedText ("Pick released songs you like in a similar style. PureSheen plays them at the same loudness as your "
                          "master and shows where they differ.",
                          text.withTrimmedTop (6.0f).toNearestInt(), juce::Justification::centredTop, 2, 0.85f);
    }

    ReferencePanel& owner;
    EditorContext& ctx;
    TR& refs;

    RefList list;
    RefWaveform waveform;
    ListenSwitch listen;
    MenuChip levelMatch, window;
    PillButton sync, stopped, reset;
    RefComparison comparison;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastFolder;

    RefViewData data;
    int infoVersion = -1, analysisVersion = -1, selectedId = 0, activeLoop = -1;
    bool referenceReady = false, empty = false;
    TR::State selectedState = TR::State::Loading;
    juce::Rectangle<float> dropZone;
};

//==============================================================================
ReferencePanel::ReferencePanel (EditorContext& c) : impl (std::make_unique<Impl> (*this, c))
{
    setTitle ("Reference");
    setRepaintsOnMouseActivity (true);
}

ReferencePanel::~ReferencePanel() = default;

void ReferencePanel::tick() { impl->tick(); }
void ReferencePanel::browse() { impl->browse(); }
void ReferencePanel::showComparisonView (RefComparison::View v) { impl->comparison.showView (v); }
void ReferencePanel::armLoop (int letter) { impl->waveform.setArmedLetter (letter); }
void ReferencePanel::resized() { impl->layout(); }

void ReferencePanel::paint (juce::Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    if (impl->empty)
        impl->paintEmpty (g);
}

void ReferencePanel::mouseUp (const juce::MouseEvent& e)
{
    if (impl->empty && isLeftClick (e) && impl->dropZone.contains (e.position))
        impl->browse();
}

int ReferencePanel::addFiles (EditorContext& ctx, const juce::StringArray& paths)
{
    auto& refs = ctx.getProcessor().getTrackReferencing();
    int added = 0, unsupported = 0;
    bool full = false;
    for (const auto& path : paths)
    {
        const juce::File f (path);
        if (! f.existsAsFile() || ! TR::isSupportedFile (f))
        {
            ++unsupported;
            continue;
        }
        const bool known = refs.findByPath (f.getFullPathName()) != 0;
        if (! known && (int) refs.getInfo().entries.size() >= TR::kMaxReferences)
        {
            full = true;
            continue;
        }
        if (refs.addReference (f, added == 0) != 0)
            ++added;
        else
            full = true;
    }

    if (full)
        ctx.notify ("The reference list is full (10 tracks)");
    else if (unsupported > 0 && added == 0)
        ctx.notify (unsupported == 1 ? "That file type is not supported - use WAV, MP3, FLAC, AIFF or OGG"
                                     : "Those files are not supported - use WAV, MP3, FLAC, AIFF or OGG");
    else if (unsupported > 0)
        ctx.notify ("Added " + juce::String (added) + (added == 1 ? " reference" : " references") + " - skipped "
                    + juce::String (unsupported) + " that " + (unsupported == 1 ? "is not" : "are not") + " WAV, MP3, FLAC, AIFF or OGG");
    return added;
}
} // namespace ps::ui
