#include "module_export.hpp"
#include "module_world_motion_fixture.hpp"

namespace {
aeg::module::FeatureModuleDefinition world_motion_factory() {
    return editor_world_motion_test::definition();
}
}

extern "C" AEG_MODULE_EXPORT void AEG_ABI_CALL aeg_world_motion_test_set_gate(uint32_t released) {
    editor_world_motion_test::callback_released.store(released != 0, std::memory_order_release);
}

extern "C" AEG_MODULE_EXPORT uint32_t AEG_ABI_CALL aeg_world_motion_test_get_call_count() {
    return editor_world_motion_test::callback_count.load(std::memory_order_acquire);
}

AEG_EXPORT_GAME_MODULE_API(world_motion_factory);
