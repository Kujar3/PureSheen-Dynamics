#include "ui/Theme.h"

#include <iostream>

// PureSheenDev icon <out.png> [size]: renders the app / plugin icon on a transparent background.
// resources/icon/logo_512.png (ICON_BIG) and installer/puresheen.ico come from this. It is the editor's droplet logo
// (ui/Controls.cpp drawLogo: same outline and colours), redrawn with soft highlights that hold up at large sizes.
namespace
{
const juce::Colour kLavender { 0xffb9a6f5 };

juce::Path dropletPath (juce::Rectangle<float> a)
{
    auto pt = [a] (float x, float y) { return juce::Point<float> (a.getX() + x * a.getWidth(), a.getY() + y * a.getHeight()); };
    juce::Path p;
    p.startNewSubPath (pt (0.5f, 0.03f));
    p.cubicTo (pt (0.40f, 0.19f), pt (0.15f, 0.40f), pt (0.15f, 0.62f));
    p.cubicTo (pt (0.15f, 0.81f), pt (0.31f, 0.97f), pt (0.5f, 0.97f));
    p.cubicTo (pt (0.69f, 0.97f), pt (0.85f, 0.81f), pt (0.85f, 0.62f));
    p.cubicTo (pt (0.85f, 0.40f), pt (0.60f, 0.19f), pt (0.5f, 0.03f));
    p.closeSubPath();
    return p;
}

// A soft (blurred) filled shape: DropShadow's blur applied to the shape itself.
void fillSoft (juce::Graphics& g, const juce::Path& shape, juce::Colour colour, float blurRadius)
{
    juce::DropShadow (colour, juce::jmax (1, juce::roundToInt (blurRadius)), { 0, 0 }).drawForPath (g, shape);
}

juce::Path strokeShape (const juce::Path& curve, float width)
{
    juce::Path out;
    juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (out, curve);
    return out;
}

void drawIcon (juce::Graphics& g, juce::Rectangle<float> a)
{
    using ps::ui::Colours::accent;
    const float s = a.getWidth();
    auto pt = [a, s] (float x, float y) { return juce::Point<float> (a.getX() + x * s, a.getY() + y * a.getHeight()); };
    const auto drop = dropletPath (a);

    // Mint glow around the drop and a soft contact shadow below it.
    fillSoft (g, drop, accent.withAlpha (0.30f), s * 0.07f);
    juce::Path contact;
    contact.addEllipse (juce::Rectangle<float> (s * 0.46f, s * 0.06f).withCentre (pt (0.5f, 0.95f)));
    fillSoft (g, contact, juce::Colours::black.withAlpha (0.28f), s * 0.035f);

    // Body: bright mint top-left to a deeper teal bottom-right.
    g.setGradientFill (juce::ColourGradient (accent.brighter (0.35f), pt (0.30f, 0.22f).x, pt (0.30f, 0.22f).y,
                                             juce::Colour (0xff2f8f7b), pt (0.80f, 1.0f).x, pt (0.80f, 1.0f).y, false));
    g.fillPath (drop);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (drop);

        // Lavender sheen pooling in the lower right (radial, no hard edge).
        juce::ColourGradient sheen (kLavender.withAlpha (0.85f), pt (0.74f, 0.80f).x, pt (0.74f, 0.80f).y,
                                    kLavender.withAlpha (0.0f), pt (0.74f, 0.80f).x - s * 0.48f, pt (0.74f, 0.80f).y, true);
        sheen.addColour (0.45, kLavender.withAlpha (0.45f));
        g.setGradientFill (sheen);
        g.fillRect (a);

        // Depth: darker towards the bottom and along the right edge.
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), pt (0.5f, 0.55f).x, pt (0.5f, 0.55f).y,
                                                 juce::Colours::black.withAlpha (0.20f), pt (0.5f, 0.98f).x, pt (0.5f, 0.98f).y, false));
        g.fillRect (a);

        // Gel highlight: a soft curved streak along the upper-left edge, brightest at its top.
        juce::Path streak;
        streak.startNewSubPath (pt (0.27f, 0.70f));
        streak.quadraticTo (pt (0.24f, 0.47f), pt (0.43f, 0.24f));
        fillSoft (g, strokeShape (streak, s * 0.055f), juce::Colours::white.withAlpha (0.70f), s * 0.018f);
        juce::Path tip;
        tip.startNewSubPath (pt (0.31f, 0.47f));
        tip.quadraticTo (pt (0.34f, 0.35f), pt (0.43f, 0.25f));
        fillSoft (g, strokeShape (tip, s * 0.035f), juce::Colours::white.withAlpha (0.85f), s * 0.008f);

        // Secondary reflection along the lower-right rim.
        juce::Path rim;
        rim.startNewSubPath (pt (0.56f, 0.915f));
        rim.quadraticTo (pt (0.76f, 0.88f), pt (0.80f, 0.68f));
        fillSoft (g, strokeShape (rim, s * 0.022f), juce::Colours::white.withAlpha (0.45f), s * 0.012f);
    }

    g.setColour (juce::Colours::white.withAlpha (0.22f));
    g.strokePath (drop, juce::PathStrokeType (juce::jmax (0.8f, s * 0.008f)));
}
} // namespace

int runIcon (const juce::File& output, int size)
{
    size = juce::jlimit (16, 2048, size);
    juce::Image image (juce::Image::ARGB, size, size, true);
    {
        juce::Graphics g (image);
        // The droplet spans 70 % of the logo area's width; leave room for its glow and contact shadow.
        const float s = (float) size;
        drawIcon (g, juce::Rectangle<float> (s * 0.90f, s * 0.90f).withCentre ({ s * 0.5f, s * 0.48f }));
    }

    output.getParentDirectory().createDirectory();
    output.deleteFile();
    juce::FileOutputStream stream (output);
    juce::PNGImageFormat png;
    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::cerr << "icon: cannot write " << output.getFullPathName() << "\n";
        return 1;
    }
    std::cout << "icon: wrote " << output.getFullPathName() << " (" << size << " px)\n";
    return 0;
}
