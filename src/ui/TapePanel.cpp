// STUB (contracts): replaced by mod-tape
#include "ui/TapePanel.h"

namespace ps::ui
{
TapePanel::TapePanel (PureSheenProcessor& p, EditorServices& s) : GenericModulePanel (p, s, ps::ModuleType::Tape) {}
TapePanel::~TapePanel() = default;
} // namespace ps::ui
