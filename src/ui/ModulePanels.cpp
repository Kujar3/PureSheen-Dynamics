#include "ui/Widgets.h"

#include "ui/DynamicsPanel.h"
#include "ui/EqPanel.h"
#include "ui/ExciterPanel.h"
#include "ui/ImagerPanel.h"
#include "ui/MaximizerPanel.h"

namespace ps::ui
{
std::unique_ptr<ModulePanel> createModulePanel (PureSheenProcessor& p, EditorServices& s, ps::ModuleType type)
{
    switch (type)
    {
        case ps::ModuleType::Equalizer1:
        case ps::ModuleType::Equalizer2: return std::make_unique<EqPanel> (p, s, type);
        case ps::ModuleType::Dynamics:   return std::make_unique<DynamicsPanel> (p, s);
        case ps::ModuleType::Imager:     return std::make_unique<ImagerPanel> (p, s);
        case ps::ModuleType::Exciter:    return std::make_unique<ExciterPanel> (p, s);
        case ps::ModuleType::Maximizer:  return std::make_unique<MaximizerPanel> (p, s);
        case ps::ModuleType::Count:
        default: break;
    }
    return nullptr;
}
} // namespace ps::ui
