#include "ui/Widgets.h"

#include "ui/BassShaperPanel.h"
#include "ui/ClassicCompPanel.h"
#include "ui/ClassicLimiterPanel.h"
#include "ui/DeLimiterPanel.h"
#include "ui/DefinitionPanel.h"
#include "ui/DynamicEqPanel.h"
#include "ui/DynamicsPanel.h"
#include "ui/EqPanel.h"
#include "ui/ExciterPanel.h"
#include "ui/ImagerPanel.h"
#include "ui/LowContrastPanel.h"
#include "ui/MatchEqPanel.h"
#include "ui/MaximizerPanel.h"
#include "ui/PassiveEqPanel.h"
#include "ui/PunchPanel.h"
#include "ui/SmootherPanel.h"
#include "ui/TapePanel.h"
#include "ui/ToneBalancePanel.h"

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
        case ps::ModuleType::DynamicEq:   return std::make_unique<DynamicEqPanel> (p, s);
        case ps::ModuleType::MatchEq:     return std::make_unique<MatchEqPanel> (p, s);
        case ps::ModuleType::ToneBalance: return std::make_unique<ToneBalancePanel> (p, s);
        case ps::ModuleType::Definition:  return std::make_unique<DefinitionPanel> (p, s);
        case ps::ModuleType::Smoother:    return std::make_unique<SmootherPanel> (p, s);
        case ps::ModuleType::Punch:       return std::make_unique<PunchPanel> (p, s);
        case ps::ModuleType::LowContrast: return std::make_unique<LowContrastPanel> (p, s);
        case ps::ModuleType::BassShaper:  return std::make_unique<BassShaperPanel> (p, s);
        case ps::ModuleType::ClassicComp:    return std::make_unique<ClassicCompPanel> (p, s);
        case ps::ModuleType::PassiveEq:      return std::make_unique<PassiveEqPanel> (p, s);
        case ps::ModuleType::ClassicLimiter: return std::make_unique<ClassicLimiterPanel> (p, s);
        case ps::ModuleType::Tape:           return std::make_unique<TapePanel> (p, s);
        case ps::ModuleType::DeLimiter:      return std::make_unique<DeLimiterPanel> (p, s);
        case ps::ModuleType::Count:
        default: break;
    }
    return nullptr;
}
} // namespace ps::ui
