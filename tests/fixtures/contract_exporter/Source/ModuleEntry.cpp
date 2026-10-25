#include "ModuleFactory.hpp"
#include "module_export.hpp"

// One source list also builds the host tool; its main belongs to the tool.
#if !defined(AEG_FEATURE_CONTRACT_EXPORTER)
AEG_EXPORT_GAME_MODULE_API(exporter_fixture::make_module);
#endif
