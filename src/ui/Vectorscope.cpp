#include "ui/Vectorscope.h"

#include "ui/ImagerExciterCommon.h"

#include <array>
#include <cmath>
#include <vector>

// Vectorscope of the final output. Owner: ui-stereo. See Vectorscope.h for the feed and the display modes.
//
// Level scale: the distance from the centre is the sample's level in dB over a 48 dB range (0 dB = full-scale mono at
// the rim), so quiet passages stay visible and loud masters do not pile up at the rim. The angle is exact.
namespace ps::ui
{
namespace
{
constexpr float kRangeDb = 48.0f;
constexpr int kQueueCapacity = 1 << 15;
constexpr int kRays = 61;                 // Polar Level directions over 180 degrees
constexpr float kInvSqrt2 = 0.70710678f;
constexpr int kHistory = 240;             // correlation history: 8 s at 30 Hz
constexpr float kNoData = 9.0f;

// Normalised radius (0..1) of a magnitude where 1.0 = full-scale mono (|M| = sqrt 2 with M = (L + R) / sqrt 2).
inline float radiusFor (float magnitude) noexcept
{
    const float db = stereo::toDb (magnitude * kInvSqrt2);
    return juce::jlimit (0.0f, 1.0f, (db + kRangeDb) / kRangeDb);
}

std::vector<std::pair<const PureSheenProcessor*, Vectorscope*>>& registry()
{
    static std::vector<std::pair<const PureSheenProcessor*, Vectorscope*>> scopes;
    return scopes;
}

Vectorscope::Mode& rememberedMode()
{
    static Vectorscope::Mode mode = Vectorscope::Mode::PolarSample;   // remembered while the host runs
    return mode;
}
} // namespace

//==============================================================================
struct Vectorscope::Impl
{
    Impl (Vectorscope& o, const PureSheenProcessor& p, juce::Colour c) : owner (o), processor (p), colour (c)
    {
        queueL.assign ((size_t) kQueueCapacity, 0.0f);
        queueR.assign ((size_t) kQueueCapacity, 0.0f);
        rays.fill (0.0f);
        history.fill (kNoData);
        mode = rememberedMode();
    }

    void push (const float* l, const float* r, int n)
    {
        if (l == nullptr || n <= 0)
            return;

        feed = true;

        if (r == nullptr)
            r = l;

        // Keep only the newest kQueueCapacity samples.
        if (n >= kQueueCapacity)
        {
            l += n - kQueueCapacity;
            r += n - kQueueCapacity;
            n = kQueueCapacity;
            queued = 0;
        }
        else if (queued + n > kQueueCapacity)
        {
            const int drop = queued + n - kQueueCapacity;
            std::copy (queueL.begin() + drop, queueL.begin() + queued, queueL.begin());
            std::copy (queueR.begin() + drop, queueR.begin() + queued, queueR.begin());
            queued -= drop;
        }

        std::copy (l, l + n, queueL.begin() + queued);
        std::copy (r, r + n, queueR.begin() + queued);
        queued += n;
    }

    //==========================================================================
    void layout()
    {
        const auto b = owner.getLocalBounds();
        meterArea = b.withTrimmedTop (b.getHeight() - kMeterHeight);

        // The scope takes the height its mode needs (square for Lissajous, a half disc for the polar modes); spare
        // height goes to the correlation history strip.
        const int avail = juce::jmax (1, b.getHeight() - kMeterHeight - 6);
        const int ideal = mode == Mode::Lissajous ? b.getWidth() + 4 : b.getWidth() / 2 + 52;
        const int maxTrace = juce::jmax (90, avail * 3 / 10);
        int scopeH = juce::jmax (juce::jmin (avail, ideal), avail - maxTrace - 6);

        if (avail - scopeH < 54)
            scopeH = avail;

        scopeArea = b.withHeight (scopeH);
        traceArea = scopeH < avail ? juce::Rectangle<int> (b.getX(), scopeArea.getBottom() + 6, b.getWidth(), avail - scopeH - 6)
                                   : juce::Rectangle<int>();

        // The dot image is rendered at the display's pixel density (host scaling, editor zoom).
        pixelScale = juce::jlimit (1.0f, 3.0f, juce::Component::getApproximateScaleFactorForComponent (&owner));
        const int w = juce::jmax (1, juce::roundToInt ((float) scopeArea.getWidth() * pixelScale));
        const int h = juce::jmax (1, juce::roundToInt ((float) scopeArea.getHeight() * pixelScale));

        if (image.getWidth() != w || image.getHeight() != h)
        {
            image = juce::Image (juce::Image::ARGB, w, h, true);
            accumulation.assign ((size_t) (w * h), 0.0f);
        }

        updateGeometry();
    }

    void updateGeometry()
    {
        const auto area = scopeArea.toFloat();

        if (mode == Mode::Lissajous)
        {
            const float side = juce::jmax (10.0f, juce::jmin (area.getWidth(), area.getHeight()) - 34.0f);
            radius = side * 0.5f;
            centre = area.getCentre();
        }
        else
        {
            radius = juce::jmax (10.0f, juce::jmin (area.getWidth() * 0.5f - 18.0f, area.getHeight() - 40.0f));
            centre = { area.getCentreX(), area.getY() + (area.getHeight() + radius) * 0.5f + 6.0f };
        }
    }

    // Local (component) point of a sample pair, in pixels relative to scopeArea's origin.
    bool samplePoint (float l, float r, float& x, float& y) const noexcept
    {
        float m = (l + r) * kInvSqrt2;
        float s = (r - l) * kInvSqrt2;
        const float mag = std::sqrt (m * m + s * s);

        if (mag < 1.0e-6f)
            return false;

        if (mode == Mode::PolarSample && m < 0.0f)
        {
            m = -m;
            s = -s;
        }

        const float rad = radiusFor (mag) * radius / mag;
        x = centre.x - (float) scopeArea.getX() + s * rad;
        y = centre.y - (float) scopeArea.getY() - m * rad;
        return true;
    }

    //==========================================================================
    void tick()
    {
        const double sr = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;

        if (std::abs (juce::jlimit (1.0f, 3.0f, juce::Component::getApproximateScaleFactorForComponent (&owner)) - pixelScale) > 0.01f)
            layout();

        const int n = queued;
        queued = 0;

        // Meters (one-pole, 300 ms).
        const double a = 1.0 - std::exp (-1.0 / (0.3 * sr));

        for (int i = 0; i < n; ++i)
        {
            const double l = queueL[(size_t) i], r = queueR[(size_t) i];
            sLR += a * (l * r - sLR);
            sLL += a * (l * l - sLL);
            sRR += a * (r * r - sRR);
        }

        if (n > 0)
        {
            // Flush denormal-range residue after long silence.
            if (sLL < 1.0e-20) sLL = 0.0;
            if (sRR < 1.0e-20) sRR = 0.0;
            if (std::abs (sLR) < 1.0e-20) sLR = 0.0;
        }

        const double energy = sLL * sRR;
        signal = juce::jmax (sLL, sRR) > 1.0e-7;
        correlation = energy > 1.0e-16 ? (float) juce::jlimit (-1.0, 1.0, sLR / std::sqrt (energy)) : 0.0f;
        const double rl = std::sqrt (sLL), rr = std::sqrt (sRR);
        balance = rl + rr > 1.0e-6 ? (float) ((rr - rl) / (rr + rl)) : 0.0f;

        if (feed)
        {
            history[(size_t) historyPos] = signal ? correlation : kNoData;
            historyPos = (historyPos + 1) % kHistory;
        }

        if (mode == Mode::PolarLevel)
            updateRays (n);
        else
            updateDots (n, sr);

        owner.repaint();
    }

    void updateDots (int n, double sr)
    {
        const int w = image.getWidth(), h = image.getHeight();

        if (w < 2 || h < 2 || accumulation.empty())
            return;

        const float decay = std::exp (-(1.0f / 30.0f) / 0.12f);

        for (auto& v : accumulation)
            v *= decay;

        // Constant brightness whatever the sample rate (and about the same look at any pixel density).
        const float weight = (float) (0.55 * 48000.0 / sr) * std::pow (pixelScale, 1.5f);

        for (int i = 0; i < n; ++i)
        {
            float x = 0.0f, y = 0.0f;

            if (! samplePoint (queueL[(size_t) i], queueR[(size_t) i], x, y))
                continue;

            x *= pixelScale;
            y *= pixelScale;

            const int x0 = (int) std::floor (x), y0 = (int) std::floor (y);

            if (x0 < 0 || y0 < 0 || x0 >= w - 1 || y0 >= h - 1)
                continue;

            const float fx = x - (float) x0, fy = y - (float) y0;
            float* row0 = accumulation.data() + (size_t) (y0 * w + x0);
            float* row1 = row0 + w;
            row0[0] += weight * (1.0f - fx) * (1.0f - fy);
            row0[1] += weight * fx * (1.0f - fy);
            row1[0] += weight * (1.0f - fx) * fy;
            row1[1] += weight * fx * fy;
        }

        // Colourise: module colour, towards white where dense.
        const juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
        const auto base = colour, hot = colour.interpolatedWith (juce::Colours::white, 0.75f);

        for (int y = 0; y < h; ++y)
        {
            const float* src = accumulation.data() + (size_t) (y * w);

            for (int x = 0; x < w; ++x)
            {
                const float v = src[x];
                auto* pixel = reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (x, y));

                if (v < 0.004f)
                {
                    pixel->setARGB (0, 0, 0, 0);
                    continue;
                }

                const float alpha = 1.0f - std::exp (-v * 1.6f);
                const auto c = base.interpolatedWith (hot, juce::jlimit (0.0f, 1.0f, (v - 0.6f) * 0.25f));
                const auto a8 = (juce::uint8) juce::jlimit (0, 255, (int) (alpha * 255.0f));
                pixel->setARGB (a8, (juce::uint8) ((c.getRed() * a8) / 255), (juce::uint8) ((c.getGreen() * a8) / 255),
                                (juce::uint8) ((c.getBlue() * a8) / 255));
            }
        }
    }

    void updateRays (int n)
    {
        std::array<double, kRays> power {};

        for (int i = 0; i < n; ++i)
        {
            float m = (queueL[(size_t) i] + queueR[(size_t) i]) * kInvSqrt2;
            float s = (queueR[(size_t) i] - queueL[(size_t) i]) * kInvSqrt2;

            if (m < 0.0f)
            {
                m = -m;
                s = -s;
            }

            const float p = m * m + s * s;

            if (p < 1.0e-14f)
                continue;

            const float angle = std::atan2 (m, s);   // 0 (right, out of phase) .. pi
            const int bin = juce::jlimit (0, kRays - 1, juce::roundToInt (angle / juce::MathConstants<float>::pi * (float) (kRays - 1)));
            power[(size_t) bin] += p;
        }

        std::array<float, kRays> target {};

        for (int b = 0; b < kRays; ++b)
        {
            // Light spread over the neighbours so a mono source reads as a beam, not a single hairline.
            const double centreP = power[(size_t) b];
            const double side = (b > 0 ? power[(size_t) b - 1] : 0.0) + (b + 1 < kRays ? power[(size_t) b + 1] : 0.0);
            const double p = n > 0 ? (0.6 * centreP + 0.2 * side) / (double) n : 0.0;
            target[(size_t) b] = p > 0.0 ? radiusFor ((float) std::sqrt (p)) : 0.0f;
        }

        for (int b = 0; b < kRays; ++b)
        {
            auto& r = rays[(size_t) b];
            r = target[(size_t) b] > r ? target[(size_t) b] : juce::jmax (target[(size_t) b], r - 0.035f);
        }
    }

    //==========================================================================
    void paintGrid (juce::Graphics& g) const
    {
        const auto gridColour = Colours::outline.withAlpha (0.55f);
        const auto labelFont = font (10.5f, 600);
        g.setFont (labelFont);

        if (mode == Mode::Lissajous)
        {
            const auto sq = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
            g.setColour (gridColour.withAlpha (0.35f));

            for (float db : { -12.0f, -24.0f, -36.0f })
            {
                const float r = radius * (db + kRangeDb) / kRangeDb;
                g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre), 1.0f);
            }

            g.setColour (gridColour);
            g.drawEllipse (sq, 1.0f);
            g.drawLine (centre.x, sq.getY(), centre.x, sq.getBottom(), 1.0f);
            g.drawLine (sq.getX(), centre.y, sq.getRight(), centre.y, 1.0f);
            const float d = radius * kInvSqrt2;
            const float dashes[] = { 3.0f, 3.0f };
            g.drawDashedLine ({ centre.x - d, centre.y - d, centre.x + d, centre.y + d }, dashes, 2, 1.0f);
            g.drawDashedLine ({ centre.x + d, centre.y - d, centre.x - d, centre.y + d }, dashes, 2, 1.0f);

            g.setColour (Colours::textSubtle);
            g.drawText ("M", juce::Rectangle<float> (20.0f, 14.0f).withCentre ({ centre.x, sq.getY() - 9.0f }), juce::Justification::centred, false);
            g.drawText ("L", juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ centre.x - d - 8.0f, centre.y - d - 8.0f }), juce::Justification::centred, false);
            g.drawText ("R", juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ centre.x + d + 8.0f, centre.y - d - 8.0f }), juce::Justification::centred, false);
            g.drawText ("+S", juce::Rectangle<float> (20.0f, 14.0f).withCentre ({ sq.getRight() + 2.0f, centre.y + 10.0f }), juce::Justification::centred, false);
            g.drawText ("-S", juce::Rectangle<float> (20.0f, 14.0f).withCentre ({ sq.getX() - 2.0f, centre.y + 10.0f }), juce::Justification::centred, false);
            return;
        }

        // Half disc: rim, level arcs, the L / M / R directions and the out-of-phase baseline.
        auto arc = [&] (float r, float thickness)
        {
            juce::Path p;
            p.addCentredArc (centre.x, centre.y, r, r, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
            g.strokePath (p, juce::PathStrokeType (thickness));
        };

        // Out-of-phase zone tint near the baseline (below the L / R diagonals is "wider than the speakers").
        {
            juce::Path zone;
            zone.startNewSubPath (centre);
            zone.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::MathConstants<float>::halfPi * 0.5f,
                                juce::MathConstants<float>::halfPi, false);
            zone.closeSubPath();
            juce::Path zone2;
            zone2.startNewSubPath (centre);
            zone2.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, -juce::MathConstants<float>::halfPi,
                                 -juce::MathConstants<float>::halfPi * 0.5f, false);
            zone2.closeSubPath();
            g.setColour (Colours::major.withAlpha (0.045f));
            g.fillPath (zone);
            g.fillPath (zone2);
        }

        g.setColour (gridColour.withAlpha (0.35f));

        for (float db : { -12.0f, -24.0f, -36.0f })
            arc (radius * (db + kRangeDb) / kRangeDb, 1.0f);

        g.setColour (gridColour);
        arc (radius, 1.2f);
        g.drawLine (centre.x - radius, centre.y, centre.x + radius, centre.y, 1.0f);

        const float d = radius * kInvSqrt2;
        const float dashes[] = { 3.0f, 3.0f };
        g.drawDashedLine ({ centre.x, centre.y, centre.x - d, centre.y - d }, dashes, 2, 1.0f);
        g.drawDashedLine ({ centre.x, centre.y, centre.x + d, centre.y - d }, dashes, 2, 1.0f);
        g.drawDashedLine ({ centre.x, centre.y, centre.x, centre.y - radius }, dashes, 2, 1.0f);

        g.setColour (Colours::textSubtle);
        g.drawText ("M", juce::Rectangle<float> (20.0f, 14.0f).withCentre ({ centre.x, centre.y - radius - 9.0f }), juce::Justification::centred, false);
        g.drawText ("L", juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ centre.x - d - 9.0f, centre.y - d - 9.0f }), juce::Justification::centred, false);
        g.drawText ("R", juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ centre.x + d + 9.0f, centre.y - d - 9.0f }), juce::Justification::centred, false);
        g.setColour (Colours::textFaint);
        g.setFont (font (10.0f, 600));
        g.drawText ("-S", juce::Rectangle<float> (20.0f, 12.0f).withCentre ({ centre.x - radius + 8.0f, centre.y + 9.0f }), juce::Justification::centred, false);
        g.drawText ("+S", juce::Rectangle<float> (20.0f, 12.0f).withCentre ({ centre.x + radius - 8.0f, centre.y + 9.0f }), juce::Justification::centred, false);
    }

    void paintRays (juce::Graphics& g) const
    {
        juce::Path envelope;
        bool started = false;

        for (int b = 0; b < kRays; ++b)
        {
            const float angle = (float) b / (float) (kRays - 1) * juce::MathConstants<float>::pi;   // 0 = +S (right)
            const float r = rays[(size_t) b] * radius;
            const juce::Point<float> tip { centre.x + std::cos (angle) * r, centre.y - std::sin (angle) * r };

            if (! started)
            {
                envelope.startNewSubPath (centre);
                started = true;
            }

            envelope.lineTo (tip);

            if (rays[(size_t) b] > 0.01f)
            {
                g.setColour (colour.withAlpha (0.25f + 0.65f * rays[(size_t) b]));
                g.drawLine ({ centre, tip }, 2.0f);
            }
        }

        envelope.closeSubPath();
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.35f), centre.x, centre.y,
                                                 colour.withAlpha (0.08f), centre.x, centre.y - radius, true));
        g.fillPath (envelope);
        g.setColour (colour.withAlpha (0.9f));
        g.strokePath (envelope, juce::PathStrokeType (1.2f));
    }

    void paintTrace (juce::Graphics& g) const
    {
        if (traceArea.getHeight() < 20)
            return;

        const auto area = traceArea.toFloat();
        stereo::drawWell (g, area);
        const auto plot = area.reduced (8.0f, 7.0f).withTrimmedLeft (18.0f);
        const auto yFor = [&] (float c) { return plot.getCentreY() - c * plot.getHeight() * 0.5f; };

        g.setColour (Colours::outline.withAlpha (0.5f));
        g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), plot.getX(), plot.getRight());
        g.setColour (Colours::outline.withAlpha (0.25f));
        g.drawHorizontalLine (juce::roundToInt (yFor (1.0f)), plot.getX(), plot.getRight());
        g.drawHorizontalLine (juce::roundToInt (yFor (-1.0f)), plot.getX(), plot.getRight());

        g.setFont (font (10.0f, 600));
        g.setColour (Colours::textFaint);
        const auto label = [&] (const char* text, float y)
        {
            g.drawText (text, juce::Rectangle<float> (22.0f, 12.0f).withRightX (plot.getX() - 3.0f).withCentre ({ plot.getX() - 14.0f, y }),
                        juce::Justification::centredRight, false);
        };
        label ("+1", juce::jmax (yFor (1.0f), area.getY() + 7.0f));
        label ("0", yFor (0.0f));
        label ("-1", juce::jmin (yFor (-1.0f), area.getBottom() - 7.0f));

        juce::Path line, negative;
        const float step = plot.getWidth() / (float) (kHistory - 1);
        bool open = false;

        for (int i = 0; i < kHistory; ++i)
        {
            const float c = history[(size_t) ((historyPos + i) % kHistory)];

            if (c > 1.5f)
            {
                open = false;
                continue;
            }

            const float x = plot.getX() + step * (float) i;
            const float y = yFor (juce::jlimit (-1.0f, 1.0f, c));

            if (! open)
                line.startNewSubPath (x, y);
            else
                line.lineTo (x, y);

            open = true;

            if (c < 0.0f)
                negative.addRectangle (x - step * 0.5f, yFor (0.0f), step, y - yFor (0.0f));
        }

        g.setColour (Colours::major.withAlpha (0.35f));
        g.fillPath (negative);
        g.setColour (Colours::good.withAlpha (0.9f));
        g.strokePath (line, juce::PathStrokeType (1.3f));

        g.setFont (font (10.0f, 600).withExtraKerningFactor (0.05f));
        g.setColour (Colours::textSubtle);
        g.drawText ("CORRELATION HISTORY", plot.reduced (4.0f, 0.0f), juce::Justification::bottomLeft, false);
    }

    void paintMeters (juce::Graphics& g) const
    {
        auto area = meterArea.toFloat();
        const float rowH = area.getHeight() * 0.5f;

        auto row = [&] (juce::Rectangle<float> r, const juce::String& name, const juce::String& leftLabel,
                        const juce::String& rightLabel, float value, bool correlationRow)
        {
            const auto labelFont = font (10.5f, 600);
            g.setFont (labelFont);
            g.setColour (Colours::textDim);
            auto nameArea = r.removeFromLeft (70.0f);
            g.drawText (name, nameArea, juce::Justification::centredLeft, false);

            auto valueArea = r.removeFromRight (38.0f);
            const auto track = r.reduced (16.0f, 0.0f).withSizeKeepingCentre (r.getWidth() - 32.0f, 6.0f);

            g.setColour (Colours::textFaint);
            g.drawText (leftLabel, juce::Rectangle<float> (16.0f, r.getHeight()).withRightX (track.getX() - 2.0f).withY (r.getY()),
                        juce::Justification::centredRight, false);
            g.drawText (rightLabel, juce::Rectangle<float> (16.0f, r.getHeight()).withX (track.getRight() + 3.0f).withY (r.getY()),
                        juce::Justification::centredLeft, false);

            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillRoundedRectangle (track, 3.0f);
            g.setColour (Colours::outline.withAlpha (0.8f));
            g.drawRoundedRectangle (track.reduced (0.5f), 3.0f, 1.0f);

            const float mid = track.getCentreX();
            g.setColour (Colours::textFaint);
            g.fillRect (juce::Rectangle<float> (1.0f, track.getHeight() + 6.0f).withCentre ({ mid, track.getCentreY() }));

            if (signal)
            {
                const float x = mid + juce::jlimit (-1.0f, 1.0f, value) * track.getWidth() * 0.5f;
                const auto fill = juce::Rectangle<float>::leftTopRightBottom (juce::jmin (mid, x), track.getY() + 1.0f,
                                                                              juce::jmax (mid, x), track.getBottom() - 1.0f);
                const auto c = correlationRow ? (value < 0.0f ? Colours::major : (value < 0.3f ? Colours::minor : Colours::good))
                                              : colour;
                g.setColour (c.withAlpha (0.85f));
                g.fillRoundedRectangle (fill, 2.0f);
                g.setColour (Colours::text);
                g.fillRoundedRectangle (juce::Rectangle<float> (3.0f, track.getHeight() + 6.0f).withCentre ({ x, track.getCentreY() }), 1.5f);
            }

            g.setFont (font (11.0f, 700));
            g.setColour (signal ? Colours::text : Colours::textFaint);
            const auto text = ! signal ? juce::String ("--")
                                       : (correlationRow ? juce::String (value, 2)
                                                         : (std::abs (value) < 0.01f ? juce::String ("C")
                                                                                     : (value < 0.0f ? "L" : "R") + juce::String (juce::roundToInt (std::abs (value) * 100.0f))));
            g.drawText (text, valueArea, juce::Justification::centredRight, false);
        };

        row (area.removeFromTop (rowH), "Correlation", "-1", "+1", correlation, true);
        row (area, "Balance", "L", "R", balance, false);
    }

    Vectorscope& owner;
    const PureSheenProcessor& processor;
    juce::Colour colour;
    Mode mode = Mode::PolarSample;

    std::vector<float> queueL, queueR;
    int queued = 0;
    bool feed = false, signal = false;

    juce::Image image;
    float pixelScale = 1.0f;
    std::vector<float> accumulation;
    std::array<float, kRays> rays {};
    std::array<float, kHistory> history {};
    int historyPos = 0;

    double sLR = 0.0, sLL = 0.0, sRR = 0.0;
    float correlation = 0.0f, balance = 0.0f;

    juce::Rectangle<int> scopeArea, meterArea, traceArea;
    juce::Point<float> centre;
    float radius = 10.0f;
};

//==============================================================================
Vectorscope::Vectorscope (const PureSheenProcessor& p, juce::Colour colour) : impl (std::make_unique<Impl> (*this, p, colour))
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
    registry().push_back ({ &p, this });
}

Vectorscope::~Vectorscope()
{
    auto& r = registry();
    r.erase (std::remove_if (r.begin(), r.end(), [this] (const auto& e) { return e.second == this; }), r.end());
}

void Vectorscope::publishOutput (const PureSheenProcessor& processor, const float* left, const float* right, int numSamples)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (numSamples <= 0)
        return;

    for (auto& e : registry())
        if (e.first == &processor)
            e.second->pushSamples (left, right, numSamples);
}

void Vectorscope::pushSamples (const float* left, const float* right, int numSamples) { impl->push (left, right, numSamples); }

void Vectorscope::tick() { impl->tick(); }

void Vectorscope::setMode (Mode m)
{
    if (m == impl->mode)
        return;

    impl->mode = m;
    rememberedMode() = m;
    std::fill (impl->accumulation.begin(), impl->accumulation.end(), 0.0f);
    impl->image.clear (impl->image.getBounds());
    impl->rays.fill (0.0f);
    impl->layout();
    repaint();
}

Vectorscope::Mode Vectorscope::getMode() const noexcept { return impl->mode; }
bool Vectorscope::hasFeed() const noexcept { return impl->feed; }
bool Vectorscope::hasSignal() const noexcept { return impl->signal; }
float Vectorscope::getCorrelation() const noexcept { return impl->correlation; }
float Vectorscope::getBalance() const noexcept { return impl->balance; }

void Vectorscope::resized() { impl->layout(); }

void Vectorscope::paint (juce::Graphics& g)
{
    auto& d = *impl;
    stereo::drawWell (g, d.scopeArea.toFloat());

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (d.scopeArea.reduced (1));
        d.paintGrid (g);

        if (d.mode == Mode::PolarLevel)
            d.paintRays (g);
        else if (d.image.isValid())
            g.drawImage (d.image, d.scopeArea.toFloat());

        if (! d.feed || ! d.signal)
        {
            g.setFont (font (11.5f, 600));
            g.setColour (Colours::textSubtle);
            const juce::String text ("Play audio to see the stereo image");
            g.drawText (text, d.scopeArea.toFloat().withTrimmedTop (d.scopeArea.getHeight() * 0.62f).withHeight (18.0f),
                        juce::Justification::centred, false);
        }
    }

    d.paintTrace (g);
    d.paintMeters (g);
}

} // namespace ps::ui
