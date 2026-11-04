#pragma once

#include "dsp/Types.h"

#include <vector>

// Which plugin this processor is: the suite, or a single-module plugin locked to one module. Owner: plugin.
// The table comes from installer/products.csv through CMake (ProductTable.inc), the one list of plugins.
namespace psplug
{
enum class ProductKind { suite, module, dither };

struct Product
{
    const char* id;          // "suite", "eq", ... (the installer/products.csv id)
    const char* name;        // "PureSheen EQ": what hosts show, as JucePlugin_Name in the plugin's format wrappers
    ProductKind kind;
    ps::ModuleType module;   // the locked module (kind module only)

    bool isSuite() const noexcept { return kind == ProductKind::suite; }
    bool isLocked() const noexcept { return kind != ProductKind::suite; }
    bool isDither() const noexcept { return kind == ProductKind::dither; }

    // The name without the brand: "EQ", "Classic Compressor", "Dynamics" for the suite.
    const char* title() const noexcept { return name + 10; }   // past "PureSheen "

    // Where the header's preset bar works, as a preset scope: -1 = global presets in the suite, the locked module in a
    // single-module plugin. The Dither plugin has no presets (see hasPresetBar).
    int presetScope() const noexcept { return kind == ProductKind::module ? (int) module : -1; }
    bool hasPresetBar() const noexcept { return kind != ProductKind::dither; }
};

const Product& suiteProduct() noexcept;
const std::vector<Product>& allProducts();                // the suite first, in the order of the list
const Product& productById (const char* id) noexcept;     // the suite for an unknown id
} // namespace psplug
