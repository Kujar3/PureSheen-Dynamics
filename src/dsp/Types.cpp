#include "dsp/Types.h"

namespace ps
{
const ModuleInfo& moduleInfo (ModuleType type)
{
    static const ModuleInfo infos[kNumModuleTypes] = {
        { "eq1", "Equalizer 1", "EQ 1" },
        { "eq2", "Equalizer 2", "EQ 2" },
        { "dyn", "Dynamics", "Dynamics" },
        { "img", "Imager", "Imager" },
        { "exc", "Exciter", "Exciter" },
        { "max", "Maximizer", "Maximizer" },
    };

    const int index = (int) type;
    return infos[index >= 0 && index < kNumModuleTypes ? index : 0];
}
} // namespace ps
