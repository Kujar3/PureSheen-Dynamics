#include "plugin/PluginProcessor.h"

// The one source file compiled per plugin: PS_PRODUCT_ID (installer/products.csv) says which plugin this binary is.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PureSheenProcessor (psplug::productById (PS_PRODUCT_ID));
}
