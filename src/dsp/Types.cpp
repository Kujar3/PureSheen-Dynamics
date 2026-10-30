#include "dsp/Types.h"

#include <cstddef>

namespace ps
{
const ModuleInfo& moduleInfo (ModuleType type)
{
    static const ModuleInfo infos[] = {
        { "eq1", "Equalizer 1", "EQ 1" },
        { "eq2", "Equalizer 2", "EQ 2" },
        { "dyn", "Dynamics", "Dynamics" },
        { "img", "Imager", "Imager" },
        { "exc", "Exciter", "Exciter" },
        { "max", "Maximizer", "Maximizer" },
        { "deq", "Dynamic EQ", "Dyn EQ" },
        { "meq", "Match EQ", "Match EQ" },
        { "tbal", "Tone Balance", "Tone Balance" },
        { "defn", "Definition", "Definition" },
        { "smth", "Smoother", "Smoother" },
        { "pnch", "Punch", "Punch" },
        { "lowc", "Low Contrast", "Low Contrast" },
        { "bass", "Bass Shaper", "Bass Shaper" },
    };

    static_assert (sizeof (infos) / sizeof (infos[0]) == (size_t) kNumModuleTypes, "one ModuleInfo per ModuleType");

    const int index = (int) type;
    return infos[index >= 0 && index < kNumModuleTypes ? index : 0];
}
} // namespace ps
