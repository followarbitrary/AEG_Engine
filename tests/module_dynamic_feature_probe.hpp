#pragma once
#include "dynamic_module.hpp"
#include "../Engine/Module/private/dynamic_library.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "module_dynamic_feature_probe.h"
#include "concurrency.hpp"

namespace dynamic_feature_test {
using namespace aeg;
using namespace aeg::module;
using render_feature_test::require;
inline AegDynamicBindProbe bind_probe(const aeg::module::detail::DynamicLibrary& library) {
    const auto symbol = library.symbol("aeg_dynamic_test_bind_probe");
    require(static_cast<bool>(symbol), "real DLL instrumentation export is missing");
    return reinterpret_cast<AegDynamicBindProbe>(symbol.value);
}
inline AegDynamicBindProbe bind_probe(const DynamicModuleHandle& handle) {
    auto borrowed = aeg::module::detail::DynamicLibrary::adopt(handle.native_handle);
    const auto result = bind_probe(borrowed);
    static_cast<void>(borrowed.detach());
    return result;
}
struct Probe final {
    uint64_t factories{}, executes{}, validators{}, writes{};
    bool gated{};
    concurrency::ThreadId thread{};
    concurrency::CompletionSource entered{}, release{};
    static void AEG_ABI_CALL factory(void* data) { ++static_cast<Probe*>(data)->factories; }
    static void AEG_ABI_CALL execute(void* data, uint64_t id) {
        auto& self = *static_cast<Probe*>(data);
        ++self.executes;
        if (self.gated && id == dynamic_feature_test::kLogic.value()) {
            self.thread = concurrency::current_thread_id();
            static_cast<void>(self.entered.complete());
            static_cast<void>(self.release.completion().wait());
        }
    }
    static void AEG_ABI_CALL validate(void* data, uint64_t) { ++static_cast<Probe*>(data)->validators; }
    static AegAbiResult AEG_ABI_CALL write(void* data, AegAbiBytes) {
        ++static_cast<Probe*>(data)->writes;
        return {sizeof(AegAbiResult), AEG_ABI_OK, {nullptr, 0}};
    }
    AegDynamicFeatureProbe view() { return {this, factory, execute, validate}; }
};
struct ReleaseOnExit final {
    Probe& probe;
    ~ReleaseOnExit() { static_cast<void>(probe.release.complete()); }
};
}  // namespace dynamic_feature_test
