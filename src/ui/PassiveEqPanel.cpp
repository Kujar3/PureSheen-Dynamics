#include "ui/PassiveEqPanel.h"

#include "dsp/PassiveEqDesign.h"
#include "ui/Controls.h"
#include "ui/EqModel.h"
#include "ui/ImagerExciterCommon.h"

#include <array>
#include <cmath>
#include <vector>

// Passive EQ panel. Owner: mod-peq. Layout and interactions: see PassiveEqPanel.h.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr float kSpectrumMinDb = -90.0f, kSpectrumMaxDb = 0.0f;
constexpr float kRangeDb = 24.0f;   // the response view shows +-24 dB (measured: low boost 16.6, low cut -19.5, high cut
                                    // -22.5 at 10 on 5 kHz, a narrow high boost 10 about +17)
constexpr const char* kDragHelp = "\nDrag to change (Ctrl: fine), double-click to type a value, right-click to reset.";

// Control groups: knob arcs and the curve's section areas share their colour.
enum Group { kGroupLow, kGroupHighBoost, kGroupHighCut, kGroupLowMid, kGroupMidCut, kGroupHighMid, kGroupTube };

juce::Colour groupColour (int group)
{
    switch (group)
    {
        case kGroupLow:       return juce::Colour (0xfff2a95f);   // orange
        case kGroupHighBoost: return juce::Colour (0xff63b4f0);   // sky
        case kGroupHighCut:   return juce::Colour (0xff5ed0b4);   // teal
        case kGroupLowMid:    return juce::Colour (0xffe6cf66);   // butter
        case kGroupMidCut:    return juce::Colour (0xffef8077);   // coral
        case kGroupHighMid:   return juce::Colour (0xff93d27c);   // green
        default:              return moduleColour (ps::ModuleType::PassiveEq);
    }
}

int groupOf (int section)
{
    switch (section)
    {
        case ps::peq::kLowBoost:
        case ps::peq::kLowCut:    return kGroupLow;
        case ps::peq::kHighBoost: return kGroupHighBoost;
        case ps::peq::kHighCut:   return kGroupHighCut;
        case ps::peq::kLowMid:    return kGroupLowMid;
        case ps::peq::kMidCut:    return kGroupMidCut;
        default:                  return kGroupHighMid;
    }
}

juce::String setId (int set, const char* name) { return juce::String ("peq.") + (set == 1 ? "b." : "a.") + name; }

// One channel set's values, read from the processor (message thread).
ps::PassiveEqSetParams readSet (const PureSheenProcessor& p, int set)
{
    auto f = [&] (const char* name) { return p.getParameterValue (setId (set, name)); };
    auto i = [&] (const char* name) { return juce::roundToInt (f (name)); };
    ps::PassiveEqSetParams s;
    s.lowHzIndex = i ("lowHz");
    s.lowBoost = f ("lowBoost");
    s.lowCut = f ("lowCut");
    s.highHzIndex = i ("highHz");
    s.highBoost = f ("highBoost");
    s.highQ = f ("highQ");
    s.highCutHzIndex = i ("highCutHz");
    s.highCut = f ("highCut");
    s.lowMidHzIndex = i ("lmHz");
    s.lowMidBoost = f ("lmBoost");
    s.midHzIndex = i ("midHz");
    s.midCut = f ("midCut");
    s.highMidHzIndex = i ("hmHz");
    s.highMidBoost = f ("hmBoost");
    return s;
}

bool sameSet (const ps::PassiveEqSetParams& a, const ps::PassiveEqSetParams& b)
{
    return a.lowHzIndex == b.lowHzIndex && a.lowBoost == b.lowBoost && a.lowCut == b.lowCut && a.highHzIndex == b.highHzIndex
           && a.highBoost == b.highBoost && a.highQ == b.highQ && a.highCutHzIndex == b.highCutHzIndex
           && a.highCut == b.highCut && a.lowMidHzIndex == b.lowMidHzIndex && a.lowMidBoost == b.lowMidBoost
           && a.midHzIndex == b.midHzIndex && a.midCut == b.midCut && a.highMidHzIndex == b.highMidHzIndex
           && a.highMidBoost == b.highMidBoost;
}

//==============================================================================
// A knob on a choice parameter: the stock knob plus a detent dot per choice around its arc (the current one lit).
class StepKnob final : public ParamKnob
{
public:
    StepKnob (PureSheenProcessor& p, const juce::String& id, const juce::String& caption)
        : ParamKnob (p, id, caption), processor (p)
    {
    }

    void setDotColour (juce::Colour c)
    {
        dotColour = c;
        setColour (c);
    }

    void setCompactLook (bool c)
    {
        compact = c;
        setCompact (c);
    }

    void paint (juce::Graphics& g) override
    {
        ParamKnob::paint (g);

        const auto* spec = stereo::specOf (getParamId());
        if (spec == nullptr || spec->choices.size() < 2)
            return;

        // the stock knob's geometry (Widgets.cpp): caption 16 px, value 17 px, diameter up to 64 px (compact: 13 / 14 /
        // 46); the dots sit just outside its arc
        auto r = getLocalBounds().toFloat();
        r.removeFromTop ((compact ? 13.0f : 16.0f) + 2.0f);
        r.removeFromBottom ((compact ? 14.0f : 17.0f) + 1.0f);
        const float d = juce::jmax (8.0f, juce::jmin (r.getWidth(), r.getHeight(), compact ? 46.0f : 64.0f));
        const auto centre = r.getCentre();
        const float dotR = d * 0.5f + 3.5f;
        const int n = (int) spec->choices.size();
        const int current = juce::jlimit (0, n - 1, juce::roundToInt (processor.getParameterValue (getParamId())));
        constexpr float start = -2.35f, end = 2.35f;   // the stock knob's arc

        for (int i = 0; i < n; ++i)
        {
            const auto p = centre.getPointOnCircumference (dotR, start + (end - start) * (float) i / (float) (n - 1));
            const bool on = i == current;
            g.setColour (on ? dotColour : Colours::textFaint);
            const float s = on ? 3.6f : 2.2f;
            g.fillEllipse (juce::Rectangle<float> (s, s).withCentre (p));
        }
    }

private:
    PureSheenProcessor& processor;
    juce::Colour dotColour = Colours::accent;
    bool compact = false;
};

//==============================================================================
// The surface of a knob row: group captions (coloured dot + title) over their knobs, thin separators between groups.
class KnobRow final : public juce::Component
{
public:
    struct Group
    {
        juce::String title;
        juce::Colour colour;
        std::vector<juce::Component*> knobs;
        int x0 = 0, x1 = 0;   // filled by resized
    };

    explicit KnobRow (std::vector<Group> g) : groups (std::move (g))
    {
        for (auto& grp : groups)
            for (auto* k : grp.knobs)
                addAndMakeVisible (k);
    }

    static constexpr int kCaptionH = 18, kPadX = 10, kPadTop = 7, kPadBottom = 5, kGroupGap = 22;

    void resized() override
    {
        auto r = getLocalBounds().reduced (kPadX, 0).withTrimmedTop (kPadTop).withTrimmedBottom (kPadBottom);
        captionY = r.getY();
        r.removeFromTop (kCaptionH);

        int count = 0;
        for (auto& grp : groups)
            count += (int) grp.knobs.size();

        const int gaps = (int) groups.size() - 1;
        const int knobW = juce::jlimit (40, 112, (r.getWidth() - gaps * kGroupGap) / juce::jmax (1, count));
        const int total = knobW * count + gaps * kGroupGap;
        int x = r.getX() + juce::jmax (0, (r.getWidth() - total) / 2);

        for (auto& grp : groups)
        {
            grp.x0 = x;
            for (auto* k : grp.knobs)
            {
                k->setBounds (x, r.getY(), knobW, r.getHeight());
                x += knobW;
            }
            grp.x1 = x;
            x += kGroupGap;
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setGradientFill (juce::ColourGradient::vertical (Colours::panelRaised.withAlpha (0.55f), b.getY(),
                                                           Colours::panelRaised.withAlpha (0.25f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (Colours::outline.withAlpha (0.6f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

        const auto titleFont = font (10.5f, 600).withExtraKerningFactor (0.09f);
        for (size_t i = 0; i < groups.size(); ++i)
        {
            const auto& grp = groups[i];
            const auto text = grp.title.toUpperCase();
            const float room = (float) (grp.x1 - grp.x0) - 4.0f;
            const float tw = juce::jmin (textWidth (titleFont, text) + 2.0f, room - 11.0f);
            const float w = tw + 11.0f;
            const auto row = juce::Rectangle<float> (0.5f * (float) (grp.x0 + grp.x1) - 0.5f * w, (float) captionY, w, (float) kCaptionH);
            g.setColour (grp.colour);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ row.getX() + 3.0f, row.getCentreY() }));
            g.setFont (titleFont);
            g.setColour (Colours::textDim);
            g.drawFittedText (text, row.withTrimmedLeft (11.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);

            if (i + 1 < groups.size())
            {
                const float sx = std::round ((float) grp.x1 + 0.5f * (float) kGroupGap);
                g.setColour (Colours::outline.withAlpha (0.55f));
                g.fillRect (juce::Rectangle<float> (sx, (float) captionY + 3.0f, 1.0f,
                                                    juce::jmax (0.0f, (float) (getHeight() - captionY) - 13.0f)));
            }
        }
    }

private:
    std::vector<Group> groups;
    int captionY = 0;
};

//==============================================================================
// The response view: spectra, the combined curve of the shown set over soft section areas, the other set dashed.
class ResponseView final : public juce::Component
{
public:
    ResponseView (PureSheenProcessor& p, EditorServices& s) : processor (p), services (s)
    {
        setInterceptsMouseClicks (true, false);
    }

    // Shown set, whether the mode has two sets, the set names and whether Link is on.
    void setState (int set, bool isSplit, const juce::String& name, bool isLinked)
    {
        if (set != shownSet || isSplit != split || name != setName || isLinked != linked)
        {
            shownSet = set;
            split = isSplit;
            setName = name;
            linked = isLinked;
            repaint();
        }
    }

    // Re-reads both sets; recomputes the curves when anything changed.
    void update()
    {
        const auto& options = services.getOptions();
        const double sr = sampleRate();
        const auto axis = makeFreqAxis (options, plot, processor.getChain().getSampleRate());
        const int n = juce::jlimit (128, 720, (int) (plot.getWidth() * 0.5f));
        bool dirty = n != (int) xs.size() || axis.minHz != fx.minHz || axis.maxHz != fx.maxHz || axis.mel != fx.mel
                     || axis.area != fx.area || sr != gridRate;

        if (dirty)
        {
            fx = axis;
            gridRate = sr;
            xs.resize ((size_t) n);
            hz.resize ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                xs[(size_t) i] = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
                hz[(size_t) i] = fx.xToHz (xs[(size_t) i]);
            }
        }

        for (int set = 0; set < 2; ++set)
        {
            const auto values = readSet (processor, set);
            if (! dirty && valid[set] && sameSet (values, params[set]))
                continue;
            params[set] = values;
            valid[set] = true;
            compute (set);
            repaint();
        }
    }

    void resized() override
    {
        plot = getLocalBounds().toFloat().reduced (1.0f);
        dy.minDb = -kRangeDb;
        dy.maxDb = kRangeDb;
        dy.area = snapEqZeroLine (plot.withTrimmedTop (18.0f).withTrimmedBottom (22.0f));
        update();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        mouse = e.position;
        inside = true;
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        inside = false;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (xs.empty())
            update();

        const auto b = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.3f), b.getY(),
                                                           Colours::background.darker (0.08f), b.getBottom()));
        g.fillRoundedRectangle (b, 7.0f);

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (plot, 6.0f);
        g.reduceClipRegion (clip);

        drawFrequencyGrid (g, fx, dy, true);

        // spectra (their own scale, behind everything)
        const auto& options = services.getOptions();
        FreqAxis specAxis = fx;
        specAxis.area = plot.withTrimmedBottom (18.0f);
        g.beginTransparencyLayer (0.6f);
        services.getModulePreSpectrum().draw (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb, Colours::input, options.spectrumFill);
        g.endTransparencyLayer();
        g.beginTransparencyLayer (0.42f);
        services.getModulePostSpectrum().draw (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb,
                                               blend (setColour (shownSet), Colours::input, 0.3f), false);
        g.endTransparencyLayer();
        services.drawReferenceSpectrum (g, specAxis, kSpectrumMinDb, kSpectrumMaxDb);   // (Track Referencing)

        const int set = split ? shownSet : 0;
        const auto colour = setColour (set);

        // the other channel set's curve, dashed
        if (split && anyActive (1 - set))
        {
            g.setColour (setColour (1 - set).withAlpha (0.42f));
            const float dashes[] { 5.0f, 4.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, curvePath (total[1 - set]), dashes, 2);
            g.fillPath (dashed);
        }

        // section areas, then the combined curve
        for (int k = 0; k < ps::peq::kNumSections; ++k)
            if (ps::peq::isSectionActive (k, params[set]))
                fillEqBand (g, xs, sections[set][(size_t) k], dy, groupColour (groupOf (k)), false);

        strokeEqCurve (g, curvePath (total[set]), colour);

        // a dot at each active section's frequency, on the combined curve
        for (int k = 0; k < ps::peq::kNumSections; ++k)
        {
            if (! ps::peq::isSectionActive (k, params[set]))
                continue;
            const float f = (float) ps::peq::sectionHz (k, params[set], gridRate);
            if (f < fx.minHz || f > fx.maxHz)
                continue;
            const float x = fx.hzToX (f);
            const float y = dy.dbToY (curveDbAt (set, x));
            const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y });
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillEllipse (dot.expanded (1.5f));
            g.setColour (groupColour (groupOf (k)));
            g.fillEllipse (dot);
        }

        // set name (two-set modes), legend
        if (split && setName.isNotEmpty())
        {
            const auto text = setName + (linked ? " (linked)" : "");
            const auto f = font (11.0f, 700);
            auto r = juce::Rectangle<float> (textWidth (f, text) + 18.0f, 18.0f).withPosition (plot.getX() + 44.0f, plot.getY() + 6.0f);
            g.setColour (colour.withAlpha (0.16f));
            g.fillRoundedRectangle (r, 9.0f);
            g.setColour (colour);
            g.setFont (f);
            g.drawText (text, r, juce::Justification::centred, false);
        }

        {
            auto legend = juce::Rectangle<float> (plot.getRight() - 178.0f, plot.getY() + 6.0f, 170.0f, 18.0f);
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

        // cursor readout
        if (inside && plot.contains (mouse))
        {
            const float x = juce::jlimit (xs.front(), xs.back(), mouse.x);
            const auto text = fmt::hz (fx.xToHz (mouse.x)) + "   " + fmt::db (curveDbAt (set, x));
            const auto f = font (10.5f, 600);
            auto r = juce::Rectangle<float> (textWidth (f, text) + 4.0f, 14.0f).withPosition (mouse.x + 12.0f, mouse.y - 20.0f);
            if (r.getRight() > plot.getRight() - 4.0f)
                r.setX (mouse.x - 12.0f - r.getWidth());
            r = r.constrainedWithin (plot.reduced (4.0f));
            g.setFont (f);
            g.setColour (Colours::textDim);
            g.drawText (text, r, juce::Justification::centredLeft, false);
        }
    }

private:
    double sampleRate() const
    {
        const double sr = processor.getChain().getSampleRate();
        return sr > 1000.0 ? sr : 48000.0;
    }

    juce::Colour setColour (int set) const
    {
        return set == 1 && split ? Colours::sideChannel : moduleColour (ps::ModuleType::PassiveEq);
    }

    bool anyActive (int set) const
    {
        for (int k = 0; k < ps::peq::kNumSections; ++k)
            if (ps::peq::isSectionActive (k, params[set]))
                return true;
        return false;
    }

    void compute (int set)
    {
        auto& tot = total[set];
        tot.assign (xs.size(), 0.0f);
        const double maxHz = gridRate * 0.4999;
        for (int k = 0; k < ps::peq::kNumSections; ++k)
        {
            auto& sec = sections[set][(size_t) k];
            sec.assign (xs.size(), 0.0f);
            // every section counts towards the total: the high cut's section also carries the high boost's upper
            // skirt while its own amount is 0 (an inactive section is otherwise exactly 0 dB)
            const auto c = ps::peq::designSection (k, params[set], gridRate);
            for (size_t i = 0; i < xs.size(); ++i)
            {
                const float v = (float) c.magnitudeDb (juce::jmin ((double) hz[i], maxHz), gridRate);
                sec[i] = std::isfinite (v) ? v : 0.0f;
                tot[i] += sec[i];
            }
        }
    }

    float curveDbAt (int set, float x) const
    {
        const auto& tot = total[set];
        if (tot.size() < 2 || xs.size() != tot.size())
            return 0.0f;
        const float t = juce::jlimit (0.0f, 1.0f, (x - xs.front()) / juce::jmax (1.0f, xs.back() - xs.front()));
        const float pos = t * (float) (tot.size() - 1);
        const auto i = (size_t) juce::jmin ((int) pos, (int) tot.size() - 2);
        const float f = pos - (float) i;
        return tot[i] + (tot[i + 1] - tot[i]) * f;
    }

    juce::Path curvePath (const std::vector<float>& db) const
    {
        juce::Path p;
        const float lo = dy.area.getY() - 40.0f, hi = dy.area.getBottom() + 40.0f;
        for (size_t i = 0; i < xs.size() && i < db.size(); ++i)
        {
            const float y = juce::jlimit (lo, hi, dy.dbToY (db[i]));
            if (i == 0)
                p.startNewSubPath (xs[i], y);
            else
                p.lineTo (xs[i], y);
        }
        return p;
    }

    PureSheenProcessor& processor;
    EditorServices& services;
    juce::Rectangle<float> plot;
    FreqAxis fx;
    DbAxis dy;
    double gridRate = 0.0;
    std::vector<float> xs, hz;
    ps::PassiveEqSetParams params[2];
    bool valid[2] { false, false };
    std::array<std::vector<float>, ps::peq::kNumSections> sections[2];
    std::vector<float> total[2];
    int shownSet = 0;
    bool split = false, linked = false, inside = false;
    juce::String setName;
    juce::Point<float> mouse;
};

//==============================================================================
struct KnobSpec
{
    const char* name;      // per-set parameter name ("lowBoost"); empty for the shared Drive
    const char* caption;
    int group;
    bool stepped;
    const char* help;
};

const KnobSpec kKnobs[] {
    { "lowHz", "Freq", kGroupLow, true, "Low frequency: where the low boost and the low cut act (20, 30, 45, 60 or 100 Hz)." },
    { "lowBoost", "Boost", kGroupLow, false, "Low boost: a broad lift of everything below the low frequency (0 to 10 dB "
                                              "on the classic markings; the real lift grows faster than the marking)." },
    { "lowCut", "Cut", kGroupLow, false, "Low cut: lowers the lows from just above the low frequency (0 to 10 dB on the "
                                          "classic markings, deeper in reality). With the boost it adds the classic dip "
                                          "above the boost, which tightens the low end." },
    { "highHz", "Freq", kGroupHighBoost, true, "High boost frequency: the centre of the high boost (3 to 16 kHz)." },
    { "highBoost", "Boost", kGroupHighBoost, false, "High boost: a bell-shaped lift around the high boost frequency (0 "
                                                    "to 10 dB on the classic markings). It gets a little broader the "
                                                    "more you boost." },
    { "highQ", "Q", kGroupHighBoost, false, "Q of the high boost: 0 is broad and smooth, 10 is narrower and more "
                                           "focused." },
    { "highCutHz", "Freq", kGroupHighCut, true, "High cut frequency: where the high cut shelf starts (5, 10 or 20 kHz)." },
    { "highCut", "Cut", kGroupHighCut, false, "High cut: a gentle shelf that softens everything above the high cut "
                                              "frequency (0 to 10 dB on the classic markings)." },
    { "lmHz", "Freq", kGroupLowMid, true, "Low-mid frequency: the centre of the low-mid boost (200 Hz to 1 kHz)." },
    { "lmBoost", "Boost", kGroupLowMid, false, "Low-mid boost: body and warmth (0 to 10 dB on the classic markings). "
                                               "It gets broader as you turn it up." },
    { "midHz", "Freq", kGroupMidCut, true, "Mid cut frequency: the centre of the mid cut (200 Hz to 7 kHz)." },
    { "midCut", "Cut", kGroupMidCut, false, "Mid cut: takes out boxiness or harshness (0 to 10 dB on the classic "
                                            "markings). It gets narrower as you turn it up." },
    { "hmHz", "Freq", kGroupHighMid, true, "High-mid frequency: the centre of the high-mid boost (1.5 to 5 kHz)." },
    { "hmBoost", "Boost", kGroupHighMid, false, "High-mid boost: presence and clarity (0 to 10 dB on the classic "
                                                "markings). It gets broader as you turn it up." },
    { "", "Drive", kGroupTube, false, "Drive: the tube make-up stage, shared by both channels. At 0 it is clean and "
                                      "exact; turning it up adds warm, mostly even harmonics at the same level." },
};
constexpr int kNumKnobs = (int) (sizeof (kKnobs) / sizeof (kKnobs[0]));
} // namespace

//==============================================================================
struct PassiveEqPanel::Impl
{
    Impl (PassiveEqPanel& o, PureSheenProcessor& p, EditorServices& s) : owner (o), processor (p), view (p, s)
    {
        for (int i = 0; i < kNumKnobs; ++i)
        {
            const auto& k = kKnobs[i];
            const juce::String id = k.name[0] != 0 ? setId (0, k.name) : juce::String ("peq.drive");
            if (k.stepped)
            {
                auto knob = std::make_unique<StepKnob> (p, id, k.caption);
                knob->setDotColour (groupColour (k.group));
                stepKnobs.push_back (knob.get());
                knobs.push_back (std::move (knob));
            }
            else
            {
                knobs.push_back (std::make_unique<ParamKnob> (p, id, k.caption));
                knobs.back()->setColour (groupColour (k.group));
            }
        }

        auto group = [&] (const char* title, int colourGroup, std::initializer_list<int> indices)
        {
            KnobRow::Group g { title, groupColour (colourGroup), {} };
            for (int i : indices)
                g.knobs.push_back (knobs[(size_t) i].get());
            return g;
        };

        programRow = std::make_unique<KnobRow> (std::vector<KnobRow::Group> {
            group ("Low", kGroupLow, { 0, 1, 2 }), group ("High Boost", kGroupHighBoost, { 3, 4, 5 }),
            group ("High Cut", kGroupHighCut, { 6, 7 }) });
        midRow = std::make_unique<KnobRow> (std::vector<KnobRow::Group> {
            group ("Low-Mid", kGroupLowMid, { 8, 9 }), group ("Mid Cut", kGroupMidCut, { 10, 11 }),
            group ("High-Mid", kGroupHighMid, { 12, 13 }), group ("Tube", kGroupTube, { 14 }) });

        owner.addAndMakeVisible (view);
        owner.addAndMakeVisible (*programRow);
        owner.addAndMakeVisible (*midRow);
        rebind();
    }

    ps::ChannelMode mode() const { return currentChannelMode (processor, ps::ModuleType::PassiveEq); }
    bool isSplit() const { return isDualChannelMode (mode()); }
    int currentSet() const { return isSplit() ? owner.getChannelSet() : 0; }
    bool isLinked() const { return isSplit() && processor.getParameterValue ("peq.link") >= 0.5f; }

    void rebind()
    {
        const int set = currentSet();
        const bool linked = isLinked();
        for (int i = 0; i < kNumKnobs; ++i)
        {
            const auto& k = kKnobs[i];
            auto& knob = *knobs[(size_t) i];
            if (k.name[0] != 0)
            {
                knob.setParamId (setId (set, k.name));
                knob.setLinkedParamId (linked ? setId (1 - set, k.name) : juce::String());
            }
            knob.setTooltip (juce::String (k.help) + kDragHelp);
        }

        shownSet = set;
        shownLinked = linked;
        shownMode = (int) mode();
        updateView();
    }

    void updateView()
    {
        view.setState (currentSet(), isSplit(), channelSetName (mode(), currentSet()), isLinked());
        view.update();
    }

    void update()
    {
        if (currentSet() != shownSet || isLinked() != shownLinked || (int) mode() != shownMode)
            rebind();
        else
            updateView();
    }

    void layout (juce::Rectangle<int> content)
    {
        auto r = content.reduced (12, 6).withTrimmedBottom (6);
        const bool compact = r.getHeight() < 470;
        const int knobH = compact ? 80 : 102;
        const int rowH = KnobRow::kPadTop + KnobRow::kCaptionH + knobH + KnobRow::kPadBottom;

        for (auto& k : knobs)
            k->setCompact (compact);
        for (auto* k : stepKnobs)
            k->setCompactLook (compact);

        midRow->setBounds (r.removeFromBottom (rowH));
        r.removeFromBottom (kGap);
        programRow->setBounds (r.removeFromBottom (rowH));
        r.removeFromBottom (kGap);
        view.setBounds (r);
    }

    PassiveEqPanel& owner;
    PureSheenProcessor& processor;
    ResponseView view;
    std::vector<std::unique_ptr<ParamKnob>> knobs;
    std::vector<StepKnob*> stepKnobs;
    std::unique_ptr<KnobRow> programRow, midRow;
    int shownSet = -1, shownMode = -1;
    bool shownLinked = false;
};

//==============================================================================
PassiveEqPanel::PassiveEqPanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::PassiveEq), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

PassiveEqPanel::~PassiveEqPanel() = default;

int PassiveEqPanel::getNumControls() const noexcept { return kNumKnobs; }

void PassiveEqPanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void PassiveEqPanel::frameTick() { impl->view.repaint(); }

void PassiveEqPanel::refresh()
{
    ModulePanel::refresh();
    impl->rebind();
}

void PassiveEqPanel::channelSetChanged() { impl->rebind(); }

void PassiveEqPanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
