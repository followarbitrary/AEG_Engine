#include <filesystem>
#include <fstream>
#include <iostream>

#include "dynamic_module.hpp"
int main(int argc, char** argv) {
    using namespace aeg::module;
    aeg::pipeline::PipelineRegistry pipelines;
    ModuleRegistry modules(pipelines);
    DynamicModuleLoader loader(modules);
    DynamicModuleHandle handle;
    const auto path = std::filesystem::temp_directory_path() / "aeg_dynamic_manifest.json";
    {
        std::ofstream out(path);
        out << "{\"build_id\":\"b\",\"contract_digest\":\"d\"}";
    }
    auto bundle = validate_module_bundle(path);
    std::filesystem::remove(path);
    if (!bundle)
        return 1;
    bundle.value.binary_path = path.parent_path() / "missing-game-module.dll";
    if (loader.load(bundle.value, handle))
        return 2;
    bundle.value.binary_path.clear();
    if (!loader.load(bundle.value, handle) || !handle.loaded || handle.api.required_capabilities != 0)
        return 2;
    if (loader.load(bundle.value, handle))
        return 3;
    {
        const auto lease = loader.acquire(handle);
        if (!lease || !lease.value.valid())
            return 4;
        const auto blocked = loader.unload(handle, 1);
        if (blocked.code != aeg::foundation::ResultCode::kTimeout || !handle.loaded)
            return 5;
    }
    if (!loader.unload(handle) || handle.loaded)
        return 6;
    if (argc > 1) {
        const auto staged = validate_module_bundle(argv[1]);
        if (!staged) {
            std::cerr << "staged bundle validation failed: " << staged.result.message << '\n';
            return 7;
        }
        if (staged.value.binary_path.empty() || staged.value.binary_sha256.empty())
            return 7;
        DynamicModuleHandle real_handle;
        if (const auto loaded = loader.load(staged.value, real_handle);
            !loaded || !real_handle.loaded || real_handle.native_handle == nullptr)
            return 8;
        if (real_handle.api.abi_major != AEG_MODULE_ABI_MAJOR)
            return 9;
        if (const auto unloaded = loader.unload(real_handle);
            !unloaded || real_handle.loaded || real_handle.native_handle != nullptr || real_handle.api.struct_size != 0)
            return 9;
    }
    std::cout << "dynamic module lifetime smoke passed\n";
    return 0;
}
