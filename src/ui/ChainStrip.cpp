#include "ui/ChainStrip.h"

#include "ui/Controls.h"

#include <map>

namespace ps::ui
{
namespace
{
constexpr int kGap = 8, kCompactGap = 6, kDenseGap = 4;
constexpr int kPlusW = 58, kCompactPlusW = 44;
constexpr float kFullTileMinW = 104.0f;   // below this the tiles switch to the compact layout (up to 18 modules)
constexpr float kCompactTileMinW = 50.0f;
constexpr float kDenseTileMinW = 44.0f;   // more than kDenseFrom modules (phase 3: up to 18) at 980 px
constexpr int kDenseFrom = 15;
constexpr float kEase = 0.38f;

enum class Part { none, body, led, solo, delta, close, plus };

juce::String prefixOf (ps::ModuleType t) { return ps::moduleInfo (t).idPrefix; }
} // namespace

//==============================================================================
struct ChainStrip::Impl
{
    Impl (ChainStrip& o, EditorContext& c) : owner (o), ctx (c), proc (c.getProcessor()) {}

    juce::Rectangle<float> tilesArea() const
    {
        return owner.getLocalBounds().toFloat().reduced (10.0f, 8.0f);
    }

    // Full tiles (104..232 px) while they fit; with many modules (phase 2: up to 14) a compact tile (name on two
    // lines, LED / S / Delta below, a thin difference bar) down to 50 px, so every module stays visible from 980 px;
    // from 15 modules (phase 3: up to 18) the compact tiles pack tighter (4 px gaps, down to 44 px).
    float naturalWidth (int gapPx, int plusPx) const
    {
        const auto area = tilesArea();
        const int n = juce::jmax (1, (int) order.size());
        return (area.getWidth() - (float) plusPx - (float) gapPx * (float) n) / (float) n;
    }

    bool compact() const { return naturalWidth (kGap, kPlusW) < kFullTileMinW; }
    bool dense() const { return (int) order.size() >= kDenseFrom; }
    int compactGap() const { return dense() ? kDenseGap : kCompactGap; }
    float gap() const { return (float) (compact() ? compactGap() : kGap); }
    float plusWidth() const { return (float) (compact() ? kCompactPlusW : kPlusW); }

    float tileWidth() const
    {
        if (! compact())
            return juce::jlimit (kFullTileMinW, 232.0f, naturalWidth (kGap, kPlusW));
        return juce::jlimit (dense() ? kDenseTileMinW : kCompactTileMinW, kFullTileMinW,
                             naturalWidth (compactGap(), kCompactPlusW));
    }

    float slotX (int slot) const { return tilesArea().getX() + (float) slot * (tileWidth() + gap()); }

    juce::Rectangle<float> tileRectAt (float x) const
    {
        const auto area = tilesArea();
        return { x, area.getY(), tileWidth(), area.getHeight() };
    }

    juce::Rectangle<float> plusRect() const
    {
        const auto area = tilesArea();
        const float x = slotX ((int) order.size());
        return { x, area.getY(), plusWidth(), area.getHeight() };
    }

    // Target x of every tile (by type) for the current drag state.
    std::map<int, float> targets() const
    {
        std::map<int, float> t;

        if (! dragging)
        {
            for (size_t i = 0; i < order.size(); ++i)
                t[(int) order[i]] = slotX ((int) i);
            return t;
        }

        int slot = 0;

        for (size_t i = 0; i < order.size(); ++i)
        {
            if ((int) i == dragIndex)
                continue;

            if (slot == insertIndex)
                ++slot;

            t[(int) order[i]] = slotX (slot);
            ++slot;
        }

        return t;
    }

    void snapPositions()
    {
        displayX = targets();
        indicatorX = slotX (insertIndex);
    }

    bool animate()
    {
        bool moving = false;

        for (auto& [type, x] : targets())
        {
            auto it = displayX.find (type);

            if (it == displayX.end())
            {
                displayX[type] = x;
                continue;
            }

            const float d = x - it->second;

            if (std::abs (d) > 0.4f)
            {
                it->second += d * kEase;
                moving = true;
            }
            else
            {
                it->second = x;
            }
        }

        const float target = slotX (insertIndex);

        if (std::abs (target - indicatorX) > 0.4f)
        {
            indicatorX += (target - indicatorX) * kEase;
            moving = true;
        }
        else
        {
            indicatorX = target;
        }

        return moving;
    }

    struct Hit
    {
        int index = -1;
        Part part = Part::none;
    };

    // compact tiles: LED, S and Delta share the bottom row evenly
    juce::Rectangle<float> compactCell (juce::Rectangle<float> t, int i) const
    {
        const float step = (t.getWidth() - 8.0f) / 3.0f;
        const float w = juce::jmin (18.0f, step - 1.0f);
        return { t.getX() + 4.0f + step * ((float) i + 0.5f) - w * 0.5f, t.getBottom() - 21.0f, w, 14.0f };
    }

    juce::Rectangle<float> ledRect (juce::Rectangle<float> t) const
    {
        if (compact()) return compactCell (t, 0);
        return { t.getX() + 10.0f, t.getY() + 8.0f, 18.0f, 18.0f };
    }
    juce::Rectangle<float> closeRect (juce::Rectangle<float> t) const
    {
        if (compact()) return { t.getRight() - 16.0f, t.getY() + 3.0f, 13.0f, 13.0f };
        return { t.getRight() - 24.0f, t.getY() + 6.0f, 18.0f, 18.0f };
    }
    juce::Rectangle<float> soloRect (juce::Rectangle<float> t) const
    {
        if (compact()) return compactCell (t, 1);
        return { t.getX() + 10.0f, t.getBottom() - 23.0f, 22.0f, 16.0f };
    }
    juce::Rectangle<float> deltaRect (juce::Rectangle<float> t) const
    {
        if (compact()) return compactCell (t, 2);
        return { t.getX() + 36.0f, t.getBottom() - 23.0f, 22.0f, 16.0f };
    }
    juce::Rectangle<float> meterRect (juce::Rectangle<float> t) const
    {
        if (compact()) return { t.getX() + 6.0f, t.getBottom() - 5.0f, t.getWidth() - 12.0f, 2.0f };
        return { t.getX() + 66.0f, t.getBottom() - 23.0f, juce::jmax (10.0f, t.getWidth() - 76.0f), 16.0f };
    }

    Hit hitTest (juce::Point<float> p) const
    {
        if (plusRect().contains (p))
            return { -1, Part::plus };

        for (size_t i = 0; i < order.size(); ++i)
        {
            auto it = displayX.find ((int) order[i]);
            const auto t = tileRectAt (it != displayX.end() ? it->second : slotX ((int) i));

            if (! t.contains (p))
                continue;

            if (ledRect (t).expanded (3.0f).contains (p))   return { (int) i, Part::led };
            if (closeRect (t).expanded (2.0f).contains (p)) return { (int) i, Part::close };
            if (soloRect (t).expanded (2.0f).contains (p))  return { (int) i, Part::solo };
            if (deltaRect (t).expanded (2.0f).contains (p)) return { (int) i, Part::delta };
            return { (int) i, Part::body };
        }

        return {};
    }

    //==========================================================================
    void paintTile (juce::Graphics& g, ps::ModuleType type, juce::Rectangle<float> t, bool floating, int index) const
    {
        const auto colour = moduleColour (type);
        const bool selected = ctx.getSelection() == (int) type;
        const bool power = proc.getParameterValue (prefixOf (type) + ".on") >= 0.5f;
        const bool dim = ! power && ctx.getUiOptions().dimBypassed;
        const bool hovered = ! floating && hover.index == index && ! dragging;
        const bool solo = proc.getSoloModule() == (int) type;
        const bool delta = proc.getDelta (type);
        const float r = 8.0f;

        if (floating)
        {
            juce::Path shape;
            shape.addRoundedRectangle (t, r);
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 18, { 0, 6 }).drawForPath (g, shape);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.fillRoundedRectangle (t.translated (0.0f, 1.5f), r);
        }

        auto fill = selected ? blend (Colours::panelHover, colour, 0.14f) : (hovered ? Colours::panelHover : Colours::panelRaised);
        g.setGradientFill (juce::ColourGradient::vertical (fill.brighter (0.05f), t.getY(), fill.darker (0.06f), t.getBottom()));
        g.fillRoundedRectangle (t, r);

        if (selected)
        {
            g.setColour (colour.withAlpha (0.22f));
            g.drawRoundedRectangle (t.expanded (1.5f), r + 1.5f, 2.0f);
            g.setColour (colour.withAlpha (0.9f));
            g.drawRoundedRectangle (t.reduced (0.5f), r, 1.3f);
        }
        else
        {
            g.setColour (Colours::outline.brighter (hovered ? 0.2f : 0.0f).withAlpha (0.9f));
            g.drawRoundedRectangle (t.reduced (0.5f), r, 1.0f);
        }

        // Colour bar on the left edge.
        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addRoundedRectangle (t, r);
            g.reduceClipRegion (clip);
            g.setColour (colour.withAlpha (dim ? 0.35f : 0.95f));
            g.fillRect (t.withWidth (3.0f));
        }

        const float alpha = dim ? 0.45f : 1.0f;
        const bool small = compact();

        drawLed (g, ledRect (t).withSizeKeepingCentre (8.0f, 8.0f), power, colour);

        // Name (compact: up to two lines across the top of the tile)
        const auto nameArea = small ? juce::Rectangle<float> (t.getX() + 6.0f, t.getY() + 3.0f,
                                                              t.getWidth() - (hovered ? 20.0f : 9.0f), 28.0f)
                                    : juce::Rectangle<float> (t.getX() + 30.0f, t.getY() + 7.0f,
                                                              t.getWidth() - 30.0f - (hovered ? 26.0f : 8.0f), 20.0f);
        g.setFont (font (small ? 11.0f : 12.5f, 700));
        g.setColour (Colours::text.withMultipliedAlpha (selected ? 1.0f : 0.92f * alpha));
        if (small)
            g.drawFittedText (ps::moduleInfo (type).shortName, nameArea.toNearestInt(), juce::Justification::centredLeft, 2, 0.75f);
        else
            g.drawFittedText (ps::moduleInfo (type).shortName, nameArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

        // Remove x on hover.
        if (hovered)
        {
            const auto c = closeRect (t);
            const bool overClose = hover.part == Part::close;
            g.setColour (overClose ? Colours::major.withAlpha (0.25f) : Colours::panelRaised);
            g.fillEllipse (c);
            g.setColour (overClose ? Colours::major : Colours::textDim);
            IconButton::paintIcon (g, IconButton::Icon::close, c.withSizeKeepingCentre (small ? 12.0f : 16.0f, small ? 12.0f : 16.0f));
        }

        // Solo
        {
            const auto s = soloRect (t);
            const bool over = hovered && hover.part == Part::solo;
            g.setColour (solo ? Colours::minor : (over ? Colours::panelHover.brighter (0.1f) : Colours::background.withAlpha (0.55f)));
            g.fillRoundedRectangle (s, 4.0f);
            g.setColour (solo ? Colours::minor.brighter (0.2f) : Colours::outline.brighter (over ? 0.3f : 0.1f));
            g.drawRoundedRectangle (s.reduced (0.5f), 4.0f, 1.0f);
            g.setFont (font (small ? 9.5f : 10.5f, 700));
            g.setColour (solo ? Colours::background : Colours::textDim.withMultipliedAlpha (alpha));
            g.drawText ("S", s, juce::Justification::centred, false);
        }

        // Delta (a small triangle glyph)
        {
            const auto s = deltaRect (t);
            const bool over = hovered && hover.part == Part::delta;
            g.setColour (delta ? Colours::info : (over ? Colours::panelHover.brighter (0.1f) : Colours::background.withAlpha (0.55f)));
            g.fillRoundedRectangle (s, 4.0f);
            g.setColour (delta ? Colours::info.brighter (0.2f) : Colours::outline.brighter (over ? 0.3f : 0.1f));
            g.drawRoundedRectangle (s.reduced (0.5f), 4.0f, 1.0f);
            const auto c = s.getCentre();
            const float k = small ? 0.8f : 1.0f;
            juce::Path tri;
            tri.addTriangle (c.x, c.y - 4.0f * k, c.x - 4.5f * k, c.y + 3.5f * k, c.x + 4.5f * k, c.y + 3.5f * k);
            g.setColour (delta ? Colours::background : Colours::textDim.withMultipliedAlpha (alpha));
            g.strokePath (tri, juce::PathStrokeType (1.3f, juce::PathStrokeType::mitered));
        }

        // Difference meter: output minus input level, -6 .. +6 dB around the centre.
        {
            const auto m = meterRect (t);
            const float diff = proc.getChain().getIoMeters().moduleDifferenceDb[(size_t) type].load();
            const bool showText = ! small && m.getWidth() > 54.0f;
            auto bar = showText ? m.withTrimmedRight (36.0f) : m;
            bar = bar.withSizeKeepingCentre (bar.getWidth(), small ? 2.0f : 5.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (bar, 2.5f);
            const float cx = bar.getCentreX();
            const float v = juce::jlimit (-1.0f, 1.0f, diff / 6.0f);

            if (std::abs (v) > 0.005f && power)
            {
                const float x0 = juce::jmin (cx, cx + v * bar.getWidth() * 0.5f);
                const float x1 = juce::jmax (cx, cx + v * bar.getWidth() * 0.5f);
                g.setColour ((diff > 0.0f ? colour : Colours::gainReduction).withAlpha (alpha));
                g.fillRoundedRectangle (juce::Rectangle<float> (x0, bar.getY(), juce::jmax (1.5f, x1 - x0), bar.getHeight()), 2.0f);
            }

            g.setColour (Colours::textFaint);
            g.fillRect (juce::Rectangle<float> (cx - 0.5f, bar.getY() - (small ? 1.0f : 2.0f), 1.0f,
                                                bar.getHeight() + (small ? 2.0f : 4.0f)));

            if (showText)
            {
                g.setFont (font (10.5f, 600));
                g.setColour (Colours::textDim.withMultipliedAlpha (alpha));
                const auto text = power && std::abs (diff) >= 0.05f ? (diff > 0.0f ? "+" : "") + juce::String (diff, 1)
                                                                   : juce::String ("0.0");
                g.drawText (text, m.withLeft (m.getRight() - 34.0f), juce::Justification::centredRight, false);
            }
        }
    }

    void paint (juce::Graphics& g) const
    {
        const auto b = owner.getLocalBounds().toFloat();
        drawPanel (g, b);

        if (order.empty())
        {
            g.setFont (font (12.5f, 600));
            g.setColour (Colours::textDim);
            g.drawText ("The chain is empty - click + to add a module",
                        tilesArea().withTrimmedLeft (plusWidth() + 16.0f), juce::Justification::centredLeft, false);
        }

        // Flow arrows between tiles (behind them).
        for (size_t i = 0; i + 1 < order.size() && ! dragging; ++i)
        {
            const float x = slotX ((int) i + 1) - gap() * 0.5f;
            const float y = tilesArea().getCentreY();
            juce::Path arrow;
            arrow.startNewSubPath (x - 2.0f, y - 3.0f);
            arrow.lineTo (x + 1.5f, y);
            arrow.lineTo (x - 2.0f, y + 3.0f);
            g.setColour (Colours::textFaint);
            g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (dragging)
        {
            // Landing slot: dashed mint outline and a glowing marker on its left edge.
            const auto slot = tileRectAt (indicatorX);
            juce::Path outline, dashed;
            outline.addRoundedRectangle (slot.reduced (1.0f), 8.0f);
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (Colours::accent.withAlpha (0.08f));
            g.fillRoundedRectangle (slot.reduced (1.0f), 8.0f);
            g.setColour (Colours::accent.withAlpha (0.65f));
            g.fillPath (dashed);
            const auto marker = juce::Rectangle<float> (3.0f, slot.getHeight() + 6.0f)
                                    .withCentre ({ slot.getX() - gap() * 0.5f, slot.getCentreY() });
            g.setColour (Colours::accent.withAlpha (0.25f));
            g.fillRoundedRectangle (marker.expanded (3.0f, 0.0f), 3.0f);
            g.setColour (Colours::accent);
            g.fillRoundedRectangle (marker, 1.5f);
        }

        for (size_t i = 0; i < order.size(); ++i)
        {
            if (dragging && (int) i == dragIndex)
                continue;

            auto it = displayX.find ((int) order[i]);
            paintTile (g, order[i], tileRectAt (it != displayX.end() ? it->second : slotX ((int) i)), false, (int) i);
        }

        // "+" tile
        {
            const auto p = plusRect();
            const bool over = hover.part == Part::plus && ! dragging;
            const bool canAdd = (int) order.size() < ps::kNumModuleTypes;
            juce::Path outline, dashed;
            outline.addRoundedRectangle (p.reduced (0.5f), 8.0f);
            const float dashes[] = { 4.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (over && canAdd ? Colours::panelHover.withAlpha (0.8f) : Colours::background.withAlpha (0.25f));
            g.fillRoundedRectangle (p, 8.0f);
            g.setColour (over && canAdd ? Colours::accent.withAlpha (0.8f) : Colours::outline.brighter (0.25f));
            g.fillPath (dashed);
            g.setColour (canAdd ? (over ? Colours::accent : Colours::textDim) : Colours::textFaint);
            IconButton::paintIcon (g, IconButton::Icon::plus, p.withSizeKeepingCentre (16.0f, 16.0f).translated (0.0f, -6.0f));
            g.setFont (font (10.5f, 600));
            g.drawText ("Add", p.withTrimmedTop (p.getHeight() * 0.55f), juce::Justification::centredTop, false);
        }

        if (dragging && dragIndex >= 0 && dragIndex < (int) order.size())
        {
            const float x = juce::jlimit (tilesArea().getX() - 6.0f, plusRect().getX() - tileWidth() + 6.0f, mouseX - grabOffset);
            paintTile (g, order[(size_t) dragIndex], tileRectAt (x).translated (0.0f, -2.0f), true, dragIndex);
        }
    }

    //==========================================================================
    void showAddMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Add a module");
        int count = 0;

        for (int t = 0; t < ps::kNumModuleTypes; ++t)
        {
            const auto type = (ps::ModuleType) t;

            if ((t == ps::kNumCoreModuleTypes || t == ps::kFirstVintageModuleType) && count > 0)
                menu.addSeparator();

            if (! proc.isModuleInChain (type))
            {
                menu.addItem (t + 1, ps::moduleInfo (type).name);
                ++count;
            }
        }

        if (count == 0)
            menu.addItem (juce::PopupMenu::Item ("Every module is in the chain").setEnabled (false));

        showMenu (menu, owner, [this] (int result)
        {
            if (result <= 0)
                return;

            const auto type = (ps::ModuleType) (result - 1);
            proc.addModule (type);
            ctx.select ((int) type);
            owner.refresh();
        }, 180);
    }

    void showTileMenu (int index)
    {
        if (index < 0 || index >= (int) order.size())
            return;

        const auto type = order[(size_t) index];
        const bool power = proc.getParameterValue (prefixOf (type) + ".on") >= 0.5f;
        juce::PopupMenu menu;
        menu.addSectionHeader (ps::moduleInfo (type).name);
        menu.addItem (1, "Power on", true, power);
        menu.addItem (2, "Solo", true, proc.getSoloModule() == (int) type);
        menu.addItem (3, "Delta (hear what it changes)", true, proc.getDelta (type));
        menu.addSeparator();
        menu.addItem (4, "Move left", index > 0);
        menu.addItem (5, "Move right", index + 1 < (int) order.size());
        menu.addItem (6, "Reset to defaults");
        menu.addItem (7, "Remove from the chain");

        showMenu (menu, owner, [this, type, power] (int result)
        {
            auto o = proc.getChainOrder();
            const auto it = std::find (o.begin(), o.end(), type);
            const int i = it != o.end() ? (int) std::distance (o.begin(), it) : -1;

            switch (result)
            {
                case 1: proc.setParameterValue (prefixOf (type) + ".on", power ? 0.0f : 1.0f); break;
                case 2: proc.setSoloModule (proc.getSoloModule() == (int) type ? -1 : (int) type); break;
                case 3: proc.setDelta (type, ! proc.getDelta (type)); break;
                case 4: if (i > 0) { std::swap (o[(size_t) i], o[(size_t) i - 1]); proc.setChainOrder (o); } break;
                case 5: if (i >= 0 && i + 1 < (int) o.size()) { std::swap (o[(size_t) i], o[(size_t) i + 1]); proc.setChainOrder (o); } break;
                case 6: proc.resetModule (type); break;
                case 7: proc.removeModule (type); break;
                default: break;
            }

            owner.refresh();
        });
    }

    void finishDrag()
    {
        if (! dragging)
            return;

        auto newOrder = order;
        const auto type = newOrder[(size_t) dragIndex];
        newOrder.erase (newOrder.begin() + dragIndex);
        newOrder.insert (newOrder.begin() + juce::jlimit (0, (int) newOrder.size(), insertIndex), type);

        // Keep the dragged tile where it was dropped so it eases into its slot.
        displayX[(int) type] = mouseX - grabOffset;
        dragging = false;
        dragIndex = -1;

        if (newOrder != order)
        {
            proc.setChainOrder (newOrder);
            order = proc.getChainOrder();
        }
    }

    void updateInsertIndex()
    {
        const float left = mouseX - grabOffset;
        const int n = (int) order.size();
        insertIndex = juce::jlimit (0, juce::jmax (0, n - 1),
                                    juce::roundToInt ((left - tilesArea().getX()) / (tileWidth() + gap())));
    }

    ChainStrip& owner;
    EditorContext& ctx;
    PureSheenProcessor& proc;

    std::vector<ps::ModuleType> order;
    std::map<int, float> displayX;
    Hit hover, press;
    bool dragging = false;
    int dragIndex = -1, insertIndex = 0;
    float grabOffset = 0.0f, mouseX = 0.0f, indicatorX = 0.0f;
    juce::Point<float> pressPos;
};

//==============================================================================
ChainStrip::ChainStrip (EditorContext& ctx) : impl (std::make_unique<Impl> (*this, ctx))
{
    setTitle ("Module chain");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    refresh();
}

ChainStrip::~ChainStrip() = default;

void ChainStrip::refresh()
{
    auto& d = *impl;
    const auto newOrder = d.proc.getChainOrder();

    if (newOrder != d.order)
    {
        d.order = newOrder;

        if (d.dragging)
            d.dragging = false, d.dragIndex = -1;

        // New tiles appear in place; existing ones slide.
        for (auto it = d.displayX.begin(); it != d.displayX.end();)
        {
            if (std::find (d.order.begin(), d.order.end(), (ps::ModuleType) it->first) == d.order.end())
                it = d.displayX.erase (it);
            else
                ++it;
        }
    }

    repaint();
}

void ChainStrip::tick()
{
    impl->animate();
    repaint();
}

juce::Rectangle<int> ChainStrip::getTileBounds (int index) const
{
    return impl->tileRectAt (impl->slotX (index)).toNearestInt();
}

void ChainStrip::simulateDrag (int fromIndex, float toX, bool release)
{
    auto& d = *impl;

    if (fromIndex < 0 || fromIndex >= (int) d.order.size())
        return;

    d.dragging = true;
    d.dragIndex = fromIndex;
    d.grabOffset = d.tileWidth() * 0.5f;
    d.mouseX = toX;
    d.updateInsertIndex();
    d.snapPositions();

    if (release)
    {
        d.finishDrag();
        d.snapPositions();
    }

    repaint();
}

juce::String ChainStrip::getTooltip()
{
    auto& d = *impl;
    const auto hit = d.hitTest (getMouseXYRelative().toFloat());

    switch (hit.part)
    {
        case Part::plus:  return "Add a module to the chain";
        case Part::led:   return "Power: switch this module on or off (it keeps its latency, so the timing never jumps)";
        case Part::solo:  return "Solo: hear only what this module does (the other modules are bypassed; Options can keep "
                                 "the Maximizer on, so the level stays close to the master)";
        case Part::delta: return "Delta: hear only the difference this module makes";
        case Part::close: return "Remove from the chain";
        case Part::body:
        {
            const auto order = d.proc.getChainOrder();
            const auto name = hit.index >= 0 && hit.index < (int) order.size()
                                  ? juce::String (ps::moduleInfo (order[(size_t) hit.index]).name) + ": " : juce::String();
            return name + "Click to edit, drag to change the order, right-click for more. The bar shows how much "
                          "louder (+) or quieter (-) the module makes the music.";
        }
        case Part::none:
        default: break;
    }

    return {};
}

void ChainStrip::paint (juce::Graphics& g) { impl->paint (g); }

void ChainStrip::resized() { impl->snapPositions(); }

void ChainStrip::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = impl->hitTest (e.position);

    if (hit.index != impl->hover.index || hit.part != impl->hover.part)
    {
        impl->hover = hit;
        repaint();
    }
}

void ChainStrip::mouseExit (const juce::MouseEvent&)
{
    impl->hover = {};
    repaint();
}

void ChainStrip::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.press = d.hitTest (e.position);
    d.pressPos = e.position;

    if (e.mods.isPopupMenu())
    {
        if (d.press.index >= 0)
            d.showTileMenu (d.press.index);
        d.press = {};
        return;
    }

    if (! isLeftClick (e))
        d.press = {};
}

void ChainStrip::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e) || d.press.index < 0 || d.press.part != Part::body)
        return;

    if (! d.dragging && e.position.getDistanceFrom (d.pressPos) < 5.0f)
        return;

    if (! d.dragging)
    {
        d.dragging = true;
        d.dragIndex = d.press.index;
        auto it = d.displayX.find ((int) d.order[(size_t) d.dragIndex]);
        const float tileX = it != d.displayX.end() ? it->second : d.slotX (d.dragIndex);
        d.grabOffset = d.pressPos.x - tileX;
        d.insertIndex = d.dragIndex;
        d.indicatorX = d.slotX (d.insertIndex);
    }

    d.mouseX = e.position.x;
    d.updateInsertIndex();
    repaint();
}

void ChainStrip::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (d.dragging)
    {
        d.finishDrag();
        repaint();
        return;
    }

    const auto press = d.press;
    d.press = {};

    if (! isLeftClick (e))
        return;

    const auto hit = d.hitTest (e.position);

    if (hit.part != press.part || hit.index != press.index)
        return;

    if (hit.part == Part::plus)
    {
        d.showAddMenu();
        return;
    }

    if (hit.index < 0 || hit.index >= (int) d.order.size())
        return;

    const auto type = d.order[(size_t) hit.index];

    switch (hit.part)
    {
        case Part::led:
        {
            const auto id = prefixOf (type) + ".on";
            d.proc.setParameterValue (id, d.proc.getParameterValue (id) >= 0.5f ? 0.0f : 1.0f);
            break;
        }
        case Part::solo:  d.proc.setSoloModule (d.proc.getSoloModule() == (int) type ? -1 : (int) type); break;
        case Part::delta: d.proc.setDelta (type, ! d.proc.getDelta (type)); break;
        case Part::close:
            d.proc.removeModule (type);
            d.ctx.notify (juce::String ("Removed ") + ps::moduleInfo (type).name + " - Undo brings it back");
            break;
        case Part::body:  d.ctx.select ((int) type); break;
        case Part::plus:
        case Part::none:
        default: break;
    }

    refresh();
}

} // namespace ps::ui
