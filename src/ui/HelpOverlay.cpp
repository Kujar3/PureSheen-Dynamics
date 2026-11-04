#include "ui/HelpOverlay.h"

#include "ui/Controls.h"

namespace ps::ui
{
namespace
{
juce::AttributedString richText (const juce::String& markup, float size, juce::Colour normal, juce::Colour strong)
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (2.0f);
    int start = 0;
    bool bold = false;

    for (;;)
    {
        const int index = markup.indexOf (start, "**");
        const auto piece = index < 0 ? markup.substring (start) : markup.substring (start, index);

        if (piece.isNotEmpty())
            s.append (piece, font (size, bold ? 600 : 400), bold ? strong : normal);

        if (index < 0)
            break;

        start = index + 2;
        bold = ! bold;
    }

    return s;
}

struct Step
{
    juce::String title, body;
    juce::Colour colour;
};

constexpr float kPad = 28.0f, kHeaderH = 84.0f;
} // namespace

//==============================================================================
struct HelpOverlay::Impl
{
    explicit Impl (PureSheenProcessor& p)
    {
        const auto& product = p.getProduct();
        steps = {
            { "Put it last on the Master",
              "Add PureSheen as the **last plugin on FL Studio's Master** track and play the loudest part of your song. "
              "At its default settings it only catches peaks above **-1 dB**.", Colours::accent },
            { "Build your chain",
              "The strip at the top is the signal path, left to right. **Click** a module to edit it, **drag** it to change "
              "the order, **+** adds one, **x** removes it. The light switches a module on or off.",
              moduleColour (ps::ModuleType::Equalizer1) },
            { "Shape the sound",
              "**EQ** for tone, **Dynamics** for punch and control, **Imager** for width, **Exciter** for warmth and air, "
              "**Maximizer** for the final loudness. Small moves go a long way.", moduleColour (ps::ModuleType::Dynamics) },
            { "Compare fairly",
              "Turn on **Gain Match**, then **Bypass**: you hear the difference at the same loudness, so louder never "
              "fools you. **S** plays one module alone, the **triangle** (Delta) plays only what it changes.",
              moduleColour (ps::ModuleType::Imager) },
        };

        tips = {
            "**Assistant** - the sparkle switch in the header. It listens to your track, builds a complete chain and gives "
            "you a few macros (tone, dynamics, width, loudness) that keep the loudness steady while you move them.",
            "**Reference** - drop songs you like on PureSheen. **REF** plays one at your master's loudness, the "
            "Reference page shows where they differ and loops A - E pick the part to compare.",
            "**Claude** - Options > Claude lets Claude Desktop read the meters and adjust PureSheen for you. It is off until "
            "you turn it on, and every change it makes is listed in the History, where you can undo it.",
            "**Undo and History** - every change is listed; click an entry to go back to that point.",
            "**A B C D** - store up to four versions of your master and click between them to compare.",
            "**Knobs** - drag to change, **Ctrl**-drag for fine steps, double-click to type a value, right-click to reset.",
            "**Mid / Side** - Mid is the centre (vocal, kick, bass), Side is the edges (width, reverb, room).",
            "**T/S** - Transient / Sustain: the hits and what rings on after them get their own settings.",
            "**Integrated LUFS** - the loudness of the whole song. Streaming plays songs at about -14 LUFS.",
            "**True peak** - keep a finished master below **-1 dBTP** so it does not distort as MP3 or on streaming.",
            "**Dither** - only when you export at a lower bit depth (16 bit for CD), as the very last step.",
        };

        moreModules = {
            { "**Dynamic EQ** - EQ bands that only move when a sound gets too loud (or too quiet).",
              moduleColour (ps::ModuleType::DynamicEq) },
            { "**Match EQ** - makes the tone of your song follow a reference you capture or load.",
              moduleColour (ps::ModuleType::MatchEq) },
            { "**Tone Balance** - keeps the tonal balance close to a genre target as the song goes on.",
              moduleColour (ps::ModuleType::ToneBalance) },
            { "**Definition** - brings out detail and openness above 300 Hz.", moduleColour (ps::ModuleType::Definition) },
            { "**Smoother** - softens harsh, ringing peaks in a range you choose.", moduleColour (ps::ModuleType::Smoother) },
            { "**Punch** - makes hits stand out more, or sit back, in up to four bands.",
              moduleColour (ps::ModuleType::Punch) },
            { "**Low Contrast** - tightens the low end (kick against bass) or makes it rounder.",
              moduleColour (ps::ModuleType::LowContrast) },
            { "**Bass Shaper** - level, punch, sustain and peaks of everything below a cutoff.",
              moduleColour (ps::ModuleType::BassShaper) },
            { "**Classic Compressor** - smooth glue compression with an old-school feel.",
              moduleColour (ps::ModuleType::ClassicComp) },
            { "**Passive EQ** - broad, musical boosts and cuts at a few classic frequencies.",
              moduleColour (ps::ModuleType::PassiveEq) },
            { "**Classic Limiter** - slow and fast limiting stages; lower the threshold to get louder.",
              moduleColour (ps::ModuleType::ClassicLimiter) },
            { "**Tape** - the warmth, low-end bump and soft highs of recording to tape.",
              moduleColour (ps::ModuleType::Tape) },
            { "**De-limiter** - brings back the peaks a limiter took from a finished master. Learn sets the threshold.",
              moduleColour (ps::ModuleType::DeLimiter) },
        };

        close.roundBackground = true;
        close.setTooltip ("Close");

        if (product.isLocked())
            useSingleModuleText (p);
    }

    // A single-module plugin: the same help, cut down to what the plugin has.
    void useSingleModuleText (PureSheenProcessor& p)
    {
        const auto& product = p.getProduct();
        const juce::String name (product.name);
        title = "Getting started with " + name;

        juce::String what;
        juce::Colour colour = Colours::accent;
        if (product.isDither())
        {
            subtitle = "Dither for the final export.";
            what = "**Dither** - use it only when you export at a lower bit depth than you work in (16 bit for CD), and "
                   "only once. Choose the bit depth of the file you will export. **Bypass** turns it off.";
        }
        else
        {
            const auto m = product.module;
            colour = moduleColour (m);
            subtitle = "One PureSheen module as its own plugin. Its presets are shared with PureSheen Dynamics.";
            if (m == ps::ModuleType::Equalizer1)
                what = "**EQ** - lifts or cuts frequencies to shape the tone. Small moves go a long way.";
            else if (m == ps::ModuleType::Dynamics)
                what = "**Multiband Dynamics** - compresses and limits separate frequency bands, for punch and control.";
            else if (m == ps::ModuleType::Imager)
                what = "**Imager** - sets the stereo width of separate frequency bands.";
            else if (m == ps::ModuleType::Exciter)
                what = "**Exciter** - adds harmonics to separate frequency bands, for warmth and air.";
            else if (m == ps::ModuleType::Maximizer)
                what = "**Maximizer** - raises the loudness and keeps the peaks below a ceiling. Put it last.";
            else
                for (const auto& more : moreModules)
                    if (more.text.startsWith (juce::String ("**") + product.title() + "**"))
                        what = more.text;
        }

        steps = {
            { "Put it on a track",
              "Add " + name + " to a mixer track, or to FL Studio's **Master** track to work on the whole song, and play "
              "the loudest part.", Colours::accent },
            { "What it does", what, colour },
            { "Compare fairly",
              product.isDither()
                  ? juce::String ("Turn on **Gain Match**, then **Bypass**: you hear the difference at the same loudness, so "
                                  "louder never fools you.")
                  : juce::String ("Turn on **Gain Match**, then **Bypass**: you hear the difference at the same loudness, so "
                                  "louder never fools you. The **triangle** (Delta) plays only what the plugin changes."),
              moduleColour (ps::ModuleType::Imager) },
        };

        tips.clear();
        tips.add ("**Undo and History** - every change is listed; click an entry to go back to that point.");
        tips.add ("**A B C D** - store up to four versions of your settings and click between them to compare.");
        tips.add ("**Knobs** - drag to change, **Ctrl**-drag for fine steps, double-click to type a value, right-click to reset.");
        if (! product.isDither() && p.getParameter (juce::String (ps::moduleInfo (product.module).idPrefix) + ".chMode") != nullptr)
            tips.add ("**Mid / Side** - Mid is the centre (vocal, kick, bass), Side is the edges (width, reverb, room).");
        if (! product.isDither() && ps::supportsTransientSustain (product.module))
            tips.add ("**T/S** - Transient / Sustain: the hits and what rings on after them get their own settings.");
        tips.add ("**Integrated LUFS** - the loudness of the whole song. Streaming plays songs at about -14 LUFS.");
        tips.add ("**True peak** - keep a finished master below **-1 dBTP** so it does not distort as MP3 or on streaming.");

        moreModules.clear();
    }

    struct Block
    {
        juce::TextLayout layout;
        juce::Rectangle<float> area;
    };

    float layoutContent (float cardW, float size)
    {
        stepBlocks.clear();
        tipBlocks.clear();
        stepTitleYs.clear();

        const float colGap = 32.0f;
        const float leftW = (cardW - 2.0f * kPad - colGap) * 0.52f;
        const float rightW = cardW - 2.0f * kPad - colGap - leftW;
        rightX = kPad + leftW + colGap;
        const float indent = 36.0f;
        const auto body = Colours::textDim.brighter (0.15f), strong = Colours::text;

        float y = kHeaderH;

        for (const auto& s : steps)
        {
            stepTitleYs.push_back (y);
            Block b;
            b.layout.createLayout (richText (s.body, size, body, strong), leftW - indent);
            b.area = { kPad + indent, y + 21.0f, leftW - indent, b.layout.getHeight() };
            y = b.area.getBottom() + 14.0f;
            stepBlocks.push_back (std::move (b));
        }

        const float leftBottom = y;
        y = kHeaderH;
        tipsTitleY = y;
        y += 22.0f;

        for (const auto& t : tips)
        {
            Block b;
            b.layout.createLayout (richText (t, size - 0.5f, body, strong), rightW - 14.0f);
            b.area = { rightX + 14.0f, y, rightW - 14.0f, b.layout.getHeight() };
            y = b.area.getBottom() + 7.0f;
            tipBlocks.push_back (std::move (b));
        }

        // More modules (click +): under the steps (the left column), or under the tips when that is clearly shorter
        moreBlocks.clear();
        if (moreModules.empty())
            return layoutLicence (cardW, juce::jmax (leftBottom, y));
        const bool left = leftBottom <= y + 40.0f;
        const float colX = left ? kPad : rightX, colW = left ? leftW : rightW;
        float my = (left ? leftBottom : y) + 8.0f;
        moreTitleX = colX;
        moreTitleY = my;
        my += 22.0f;
        for (const auto& m : moreModules)
        {
            Block b;
            b.layout.createLayout (richText (m.text, size - 1.0f, body, strong), colW - 14.0f);
            b.area = { colX + 14.0f, my, colW - 14.0f, b.layout.getHeight() };
            my = b.area.getBottom() + 4.0f;
            moreBlocks.push_back (std::move (b));
        }

        return layoutLicence (cardW, juce::jmax (left ? my : leftBottom, left ? y : my));
    }

    // The AGPL's notice in the program itself (its section 0, "Appropriate Legal Notices"). The source line says the same
    // as $sourceNote in scripts/package-all.ps1: both get the repository's address once it is public.
    float layoutLicence (float cardW, float contentBottom)
    {
        const auto text = productName + " - Copyright (C) 2026 Kujar3. Free software: you can share and change it under the GNU "
                          "AGPL v3. It comes with no warranty.\n"
                          "Licence: gnu.org/licenses/agpl-3.0.html  -  Source code: ask Kujar3, the author  -  "
                          "The installer adds LICENSE.txt and THIRD-PARTY-NOTICES.txt";
        licenceBlock.layout.createLayout (richText (text, 11.0f, Colours::textDim, Colours::text), cardW - 2.0f * kPad);
        licenceBlock.area = { kPad, contentBottom + 12.0f, cardW - 2.0f * kPad, licenceBlock.layout.getHeight() };
        return licenceBlock.area.getBottom() + 16.0f;
    }

    struct More
    {
        juce::String text;
        juce::Colour colour;
    };

    juce::String title { "Getting started with PureSheen" };
    juce::String subtitle { "A complete mastering chain: each module does one job. Add, reorder and shape them until the song sounds finished." };
    std::vector<Step> steps;
    juce::StringArray tips;
    std::vector<More> moreModules;
    std::vector<Block> stepBlocks, tipBlocks, moreBlocks;
    Block licenceBlock;
    juce::String productName;
    std::vector<float> stepTitleYs;
    float tipsTitleY = 0.0f, rightX = 0.0f, moreTitleX = 0.0f, moreTitleY = 0.0f;
    juce::Rectangle<float> card;
    IconButton close { "Close help", IconButton::Icon::close };
};

HelpOverlay::HelpOverlay (PureSheenProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setTitle ("Help");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    impl->productName = p.getName();
    impl->close.onClick = [this] { if (onClose != nullptr) onClose(); };
    addAndMakeVisible (impl->close);
}

HelpOverlay::~HelpOverlay() = default;

void HelpOverlay::resized()
{
    auto& d = *impl;
    const float cardW = juce::jmin ((float) getWidth() - 48.0f, 940.0f);
    float size = 13.0f;
    float h = d.layoutContent (cardW, size);

    while (h > (float) getHeight() - 40.0f && size > 11.0f)
    {
        size -= 0.5f;
        h = d.layoutContent (cardW, size);
    }

    d.card = juce::Rectangle<float> (cardW, juce::jmin (h, (float) getHeight() - 24.0f)).withCentre (getLocalBounds().toFloat().getCentre());
    d.close.setBounds (juce::Rectangle<int> ((int) d.card.getRight() - 44, (int) d.card.getY() + 16, 28, 28));
}

void HelpOverlay::paint (juce::Graphics& g)
{
    auto& d = *impl;
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillAll();

    juce::Path shape;
    shape.addRoundedRectangle (d.card, 14.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 34, { 0, 10 }).drawForPath (g, shape);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panel.brighter (0.06f), d.card.getY(), Colours::panel, d.card.getBottom()));
    g.fillPath (shape);
    g.setColour (Colours::outline);
    g.drawRoundedRectangle (d.card.reduced (0.5f), 14.0f, 1.0f);

    const auto origin = d.card.getTopLeft();
    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (shape);
    g.addTransform (juce::AffineTransform::translation (origin.x, origin.y));

    drawLogo (g, { kPad, 24.0f, 36.0f, 36.0f });
    g.setFont (font (21.0f, 700));
    g.setColour (Colours::text);
    g.drawText (d.title, juce::Rectangle<float> (kPad + 50.0f, 20.0f, d.card.getWidth() - 140.0f, 26.0f),
                juce::Justification::centredLeft, false);
    g.setFont (font (13.0f));
    g.setColour (Colours::textDim);
    g.drawText (d.subtitle,
                juce::Rectangle<float> (kPad + 50.0f, 46.0f, d.card.getWidth() - kPad * 2.0f - 50.0f, 18.0f),
                juce::Justification::centredLeft, true);

    for (size_t i = 0; i < d.steps.size(); ++i)
    {
        const float y = d.stepTitleYs[i];
        const auto badge = juce::Rectangle<float> (24.0f, 24.0f).withPosition (kPad, y - 2.0f);
        g.setColour (d.steps[i].colour.withAlpha (0.18f));
        g.fillEllipse (badge);
        g.setColour (d.steps[i].colour);
        g.drawEllipse (badge.reduced (0.6f), 1.2f);
        g.setFont (font (12.0f, 700));
        g.drawText (juce::String ((int) i + 1), badge, juce::Justification::centred, false);
        g.setFont (font (14.0f, 600));
        g.setColour (Colours::text);
        g.drawText (d.steps[i].title, juce::Rectangle<float> (kPad + 36.0f, y - 2.0f, 400.0f, 22.0f), juce::Justification::centredLeft, false);
        d.stepBlocks[i].layout.draw (g, d.stepBlocks[i].area);
    }

    drawSectionTitle (g, "Good to know", juce::Rectangle<float> (d.rightX, d.tipsTitleY - 2.0f, 300.0f, 18.0f).toNearestInt());

    for (auto& b : d.tipBlocks)
    {
        g.setColour (Colours::accent.withAlpha (0.8f));
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ d.rightX + 3.0f, b.area.getY() + 8.0f }));
        b.layout.draw (g, b.area);
    }

    if (! d.moreModules.empty())
        drawSectionTitle (g, "More modules (click + in the strip)",
                          juce::Rectangle<float> (d.moreTitleX, d.moreTitleY - 2.0f, 320.0f, 18.0f).toNearestInt());

    for (size_t i = 0; i < d.moreBlocks.size() && i < d.moreModules.size(); ++i)
    {
        const auto& b = d.moreBlocks[i];
        g.setColour (d.moreModules[i].colour);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ d.moreTitleX + 3.0f, b.area.getY() + 7.5f }));
        b.layout.draw (g, b.area);
    }

    d.licenceBlock.layout.draw (g, d.licenceBlock.area);
}

void HelpOverlay::mouseUp (const juce::MouseEvent& e)
{
    if (isLeftClick (e) && ! impl->card.contains (e.position) && onClose != nullptr)
        onClose();
}

//==============================================================================
namespace
{
constexpr int kRowH = 26;

class HistoryList final : public juce::Component
{
public:
    explicit HistoryList (PureSheenProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setRepaintsOnMouseActivity (true);
    }

    void refresh()
    {
        entries = proc.getHistory();
        position = proc.getHistoryPosition();
        setSize (getWidth(), juce::jmax (kRowH, (int) entries.size() * kRowH));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int hover = isMouseOver() ? getMouseXYRelative().y / kRowH : -1;

        for (int i = 0; i < (int) entries.size(); ++i)
        {
            const auto row = juce::Rectangle<float> (0.0f, (float) (i * kRowH), (float) getWidth(), (float) kRowH).reduced (4.0f, 1.0f);
            const bool current = i == position;
            const bool future = i > position;

            if (current)
            {
                g.setColour (blend (Colours::panelRaised, Colours::accent, 0.22f));
                g.fillRoundedRectangle (row, 5.0f);
                g.setColour (Colours::accent.withAlpha (0.7f));
                g.drawRoundedRectangle (row.reduced (0.5f), 5.0f, 1.0f);
            }
            else if (i == hover)
            {
                g.setColour (Colours::panelHover);
                g.fillRoundedRectangle (row, 5.0f);
            }

            g.setFont (font (10.5f, 700));
            g.setColour (current ? Colours::accent : Colours::textFaint.brighter (0.25f));
            g.drawText (juce::String (i), row.withWidth (30.0f), juce::Justification::centredRight, false);
            g.setFont (font (12.0f, current ? 700 : 600));
            g.setColour (current ? Colours::text : (future ? Colours::textSubtle : Colours::text.withAlpha (0.88f)));
            g.drawFittedText (entries[(size_t) i].description, row.withTrimmedLeft (40.0f).withTrimmedRight (8.0f).toNearestInt(),
                              juce::Justification::centredLeft, 1, 0.85f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e))
            return;

        const int i = e.getPosition().y / kRowH;

        if (i >= 0 && i < (int) entries.size() && i != position)
        {
            proc.jumpToHistory (i);
            refresh();
        }
    }

private:
    PureSheenProcessor& proc;
    std::vector<PureSheenProcessor::HistoryEntry> entries;
    int position = 0;
};
} // namespace

struct HistoryPopup::Impl
{
    explicit Impl (PureSheenProcessor& p) : proc (p), list (p)
    {
        viewport.setViewedComponent (&list, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (8);
        viewport.setWantsKeyboardFocus (false);
        clear.setText ("Clear");
        clear.setTooltip ("Forget the history (keeps the current settings)");
        clear.onClick = [this] { proc.clearHistory(); list.refresh(); };
    }

    PureSheenProcessor& proc;
    HistoryList list;
    juce::Viewport viewport;
    PillButton clear;
    juce::Rectangle<float> card;
};

HistoryPopup::HistoryPopup (PureSheenProcessor& p) : impl (std::make_unique<Impl> (p))
{
    setTitle ("History");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    addAndMakeVisible (impl->viewport);
    addAndMakeVisible (impl->clear);
}

HistoryPopup::~HistoryPopup() = default;

void HistoryPopup::open (juce::Rectangle<int> anchor)
{
    auto& d = *impl;
    d.list.refresh();
    const int rows = juce::jlimit (3, 14, (int) d.proc.getHistory().size());
    const float w = 320.0f, h = 52.0f + (float) (rows * kRowH) + 12.0f;
    float x = (float) anchor.getCentreX() - w * 0.5f;
    x = juce::jlimit (8.0f, juce::jmax (8.0f, (float) getWidth() - w - 8.0f), x);
    d.card = { x, (float) anchor.getBottom() + 6.0f, w, juce::jmin (h, (float) getHeight() - (float) anchor.getBottom() - 16.0f) };
    resized();

    // Show the current entry.
    const int pos = d.proc.getHistoryPosition();
    d.viewport.setViewPosition (0, juce::jmax (0, pos * kRowH - d.viewport.getHeight() / 2));
    setVisible (true);
    toFront (false);
}

void HistoryPopup::refresh()
{
    impl->list.refresh();
}

void HistoryPopup::resized()
{
    auto& d = *impl;
    const auto c = d.card.toNearestInt();
    d.clear.setBounds (c.getRight() - 70, c.getY() + 12, 58, 22);
    d.viewport.setBounds (c.reduced (6, 0).withTrimmedTop (44).withTrimmedBottom (8));
    d.list.setSize (d.viewport.getWidth() - 8, d.list.getHeight());
}

void HistoryPopup::paint (juce::Graphics& g)
{
    auto& d = *impl;
    juce::Path shape;
    shape.addRoundedRectangle (d.card, 10.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 24, { 0, 8 }).drawForPath (g, shape);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panel.brighter (0.07f), d.card.getY(), Colours::panel, d.card.getBottom()));
    g.fillPath (shape);
    g.setColour (Colours::outline.brighter (0.1f));
    g.drawRoundedRectangle (d.card.reduced (0.5f), 10.0f, 1.0f);

    // Little arrow towards the History button.
    g.setFont (font (13.0f, 700));
    g.setColour (Colours::text);
    g.drawText ("History", d.card.reduced (16.0f, 0.0f).withHeight (44.0f).translated (0.0f, 1.0f), juce::Justification::centredLeft, false);
    g.setFont (font (11.0f, 600));
    g.setColour (Colours::textSubtle);
    g.drawText ("click to go back", d.card.reduced (16.0f, 0.0f).withHeight (44.0f).withTrimmedLeft (64.0f).translated (0.0f, 1.5f),
                juce::Justification::centredLeft, false);
    g.setColour (Colours::outline.withAlpha (0.7f));
    g.fillRect (d.card.reduced (10.0f, 0.0f).withY (d.card.getY() + 43.0f).withHeight (1.0f));
}

void HistoryPopup::mouseUp (const juce::MouseEvent& e)
{
    if (! impl->card.contains (e.position) && onClose != nullptr)
        onClose();
}

} // namespace ps::ui
