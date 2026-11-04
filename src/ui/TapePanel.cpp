#include "ui/TapePanel.h"

#include "dsp/Tape.h"
#include "ui/ImagerExciterCommon.h"
#include "ui/TapeViews.h"

#include <cmath>

// Tape panel. Owner: mod-tape. Layout and interactions: see TapePanel.h.
//
// Meters: TapeMeters (level into the tape, saturation; both peak values with a 300 ms release in the DSP), read at
// 30 Hz. The response view redraws only when a parameter, the size or the frequency scale changes.
namespace ps::ui
{
namespace
{
constexpr int kGap = 8;
constexpr int kSpeedH = 26;

// Captions of a knob's two ends, under it ("Under" / "Over").
void setEndCaptions (stereo::Card& card, juce::Rectangle<int> knob, const juce::String& left, const juce::String& right)
{
    const auto f = font (11.0f, 600);
    const int lw = (int) textWidth (f, left) + 4, rw = (int) textWidth (f, right) + 4;
    const auto row = juce::Rectangle<int> (knob.getX(), knob.getBottom() - 2, knob.getWidth(), 15);
    card.setCaptions ({ { left, row.withWidth (lw) }, { right, row.withLeft (row.getRight() - rw) } });
}
} // namespace

//==============================================================================
struct TapePanel::Impl
{
    Impl (TapePanel& o, PureSheenProcessor& p, EditorServices& s)
        : owner (o), processor (p), colour (moduleColour (ps::ModuleType::Tape)),
          tapeCard ("Tape", colour), speed (p, "tape.speed", { "7.5 ips", "15 ips", "30 ips" }),
          drive (p, "tape.drive", "Drive"),
          response (p, s),
          emphCard ("Emphasis", colour), lowEmph (p, "tape.lowEmph", "Low Emphasis"),
          highEmph (p, "tape.highEmph", "High Emphasis"),
          biasCard ("Bias", colour), bias (p, "tape.bias", "Bias"),
          harmCard ("Harmonics", colour), harmonics (p, "tape.harmonics", "Harmonics")
    {
        speed.setOnColour (colour);
        for (auto* k : { &lowEmph, &highEmph, &bias, &harmonics })
            k->setColour (colour);
        drive.setColour (colour);
        meter.setColour (colour);
        response.setColour (colour);

        speed.setTooltip ("Tape speed. Faster tape keeps more highs and moves the head bump up; slower tape saturates "
                          "the highs earlier.");
        drive.setTooltip ("Drive: how hard the signal hits the tape. More drive = more saturation and softer peaks; the "
                          "output level is partly compensated and can rise at high drive.\nDrag to change (Ctrl: "
                          "fine), double-click to type a value, right-click to reset.");
        meter.setTooltip ("Level into tape: the peak level reaching the tape after Drive (the highs count more, as on a "
                          "real machine). Saturation: how much the tape squashes those peaks.");
        response.setTooltip ("Response at low levels, relative to 1 kHz: the low end and the highs set by High Emphasis "
                             "and Bias. The overall level and sample-rate dependent top roll-off are excluded. "
                             "Saturation comes on top of this as you drive harder.");
        lowEmph.setTooltip ("Low Emphasis: the head bump, a resonant lift in the lows at a frequency set by the speed. 0 "
                            "removes the bump (the roll-off below it stays). Its peak gain also depends on the speed.");
        highEmph.setTooltip ("High Emphasis: compensation of the high-frequency loss of the playback head. 4 = flat, lower "
                             "values roll the highs off gently, higher values add sheen.");
        bias.setTooltip ("Bias: the character of the saturation. Negative (under-biased) = brighter, the highs distort "
                         "earlier; positive (over-biased) = softer and more compressed, near +10 the dynamics are "
                         "strongly limited.");
        harmonics.setTooltip ("Harmonics: adds even-order harmonics (warmth) to the tape saturation. They grow with Drive.");

        owner.addAndMakeVisible (tapeCard);
        tapeCard.addAndMakeVisible (speed);
        tapeCard.addAndMakeVisible (drive);
        tapeCard.addAndMakeVisible (meter);

        owner.addAndMakeVisible (response);

        owner.addAndMakeVisible (emphCard);
        emphCard.addAndMakeVisible (lowEmph);
        emphCard.addAndMakeVisible (highEmph);

        owner.addAndMakeVisible (biasCard);
        biasCard.addAndMakeVisible (bias);
        owner.addAndMakeVisible (harmCard);
        harmCard.addAndMakeVisible (harmonics);
        update();
    }

    ps::Tape* module() const { return dynamic_cast<ps::Tape*> (&processor.getChain().getModule (ps::ModuleType::Tape)); }

    //==========================================================================
    void update()
    {
        const auto p = tape::readParams (processor);
        tapeCard.setSubtitle (juce::String (ps::kTapeSpeedIps[(size_t) p.speed], p.speed == ps::TapeSpeed::Ips7_5 ? 1 : 0)
                              + " ips");
        biasCard.setSubtitle (p.bias < -0.05f ? "Under-biased" : (p.bias > 0.05f ? "Over-biased" : "Nominal"));
        biasCard.setBodyNote (p.bias < -0.05f ? "Brighter; the highs distort earlier."
                                              : (p.bias > 0.05f ? (p.bias > 7.0f ? "Soft and strongly compressed: the dynamics are held."
                                                                                 : "Softer and more compressed.")
                                                                : "The machine as aligned."));
        harmCard.setSubtitle (p.harmonics < 0.05f ? "Off" : juce::String());
        harmCard.setBodyNote ("Even-order warmth on the saturation; it grows with Drive.");
        emphCard.setSubtitle (std::abs (p.highEmphasis - 4.0f) < 0.05f ? "Highs flat" : juce::String());

        const bool running = processor.isModuleInChain (ps::ModuleType::Tape) && processor.getParameterValue ("tape.on") >= 0.5f
                             && processor.getChain().getSampleRate() > 0.0;
        const auto* t = module();
        if (running && t != nullptr)
        {
            const auto& m = t->getMeters();
            const float level = m.inputDriveDb.load();
            meter.setValues (level, m.saturationDb.load(), level > -99.0f);
        }
        else
        {
            meter.setValues (-100.0f, 0.0f, false);
        }
        response.update();
    }

    //==========================================================================
    void layout (juce::Rectangle<int> content)
    {
        auto r = content.reduced (12, 6).withTrimmedBottom (6);
        const int W = r.getWidth();
        const int leftW = juce::jlimit (210, 300, juce::roundToInt ((float) W * 0.24f));
        const int rightW = juce::jlimit (190, 280, juce::roundToInt ((float) W * 0.21f));

        auto left = r.removeFromLeft (leftW);
        r.removeFromLeft (kGap);
        auto right = r.removeFromRight (rightW);
        r.removeFromRight (kGap);
        auto centre = r;

        // Left: speed, drive, meter
        {
            tapeCard.setBounds (left);
            auto body = tapeCard.getBody();
            speed.setBounds (body.removeFromTop (kSpeedH));
            body.removeFromTop (kGap);
            meter.setBounds (body.removeFromBottom (tape::SaturationMeter::kIdealHeight));
            body.removeFromBottom (kGap);
            const int kw = juce::jmin (body.getWidth(), 240);
            drive.setBounds (body.withSizeKeepingCentre (kw, juce::jmin (body.getHeight(), kw + 10)));
        }

        // Centre: response above the emphasis knobs
        {
            const int emphH = juce::jlimit (150, 210, juce::roundToInt ((float) centre.getHeight() * 0.36f));
            emphCard.setBounds (centre.removeFromBottom (emphH));
            centre.removeFromBottom (kGap);
            response.setBounds (centre);

            auto body = emphCard.getBody();
            const int kw = juce::jmin (120, (body.getWidth() - 24) / 2);
            const int kh = juce::jmin (body.getHeight(), 116);
            auto knobs = body.withSizeKeepingCentre (kw * 2 + juce::jmin (80, body.getWidth() - kw * 2), kh);
            lowEmph.setBounds (knobs.removeFromLeft (kw));
            highEmph.setBounds (knobs.removeFromRight (kw));
        }

        // Right: bias above harmonics, each knob centred over its note
        {
            const int h = (right.getHeight() - kGap) / 2;
            biasCard.setBounds (right.removeFromTop (h));
            right.removeFromTop (kGap);
            harmCard.setBounds (right);

            auto place = [] (stereo::Card& card, ParamKnob& knob, int captionH) {
                auto body = card.getBody();
                body.removeFromBottom (juce::jmin (body.getHeight() / 3, 40));   // the note
                const int kw = juce::jmin (body.getWidth(), 116);
                const int kh = juce::jmin (body.getHeight() - captionH, 104);
                knob.setBounds (body.withSizeKeepingCentre (kw, juce::jmax (40, kh)).translated (0, -captionH / 2));
            };
            place (biasCard, bias, 14);
            place (harmCard, harmonics, 0);
            setEndCaptions (biasCard, bias.getBounds(), "Under", "Over");
        }
    }

    TapePanel& owner;
    PureSheenProcessor& processor;
    juce::Colour colour;

    stereo::Card tapeCard;
    ParamSegments speed;
    tape::DriveKnob drive;
    tape::SaturationMeter meter;

    tape::ResponseView response;

    stereo::Card emphCard;
    ParamKnob lowEmph, highEmph;

    stereo::Card biasCard;
    ParamKnob bias;
    stereo::Card harmCard;
    ParamKnob harmonics;
};

//==============================================================================
TapePanel::TapePanel (PureSheenProcessor& p, EditorServices& s)
    : ModulePanel (p, s, ps::ModuleType::Tape), impl (std::make_unique<Impl> (*this, p, s))
{
    neverTakeKeyboardFocus (*this);
}

TapePanel::~TapePanel() = default;

tape::ResponseView& TapePanel::getResponseView() noexcept { return impl->response; }

int TapePanel::getNumControls() const noexcept
{
    // speed, drive, bias, harmonics, low emphasis, high emphasis
    return 6;
}

void TapePanel::tick()
{
    ModulePanel::tick();
    impl->update();
}

void TapePanel::refresh()
{
    ModulePanel::refresh();
    impl->update();
}

void TapePanel::paint (juce::Graphics& g) { ModulePanel::paint (g); }

void TapePanel::layoutContent (juce::Rectangle<int> contentArea) { impl->layout (contentArea); }

} // namespace ps::ui
