#include <iostream>
#include <memory>
#include <type_traits>

#include <world_scope.hpp>

template <class Scope>
constexpr bool exposes_raw_pipeline = requires(Scope& scope) { scope.pipeline_host(); } ||
    requires(Scope& scope) { scope.has_pipeline(); } || requires(Scope& scope) { scope.bind_pipeline({}); };

template <class Scope>
int verify_world_consumer(std::shared_ptr<aeg::ecs::World> state) {
    if constexpr (std::is_constructible_v<Scope, std::shared_ptr<aeg::ecs::World>>) {
        Scope scope(std::move(state));
        if (!scope.initialize()) return 1;
        const auto snapshot = scope.commit(19, 3);
        auto frame = scope.create_frame_scope(19, 7);
        if (!snapshot || !frame || !frame.value.capture()) return 1;
        const auto captured = frame.value.read_snapshot();
        return captured && captured.value.world() == scope.handle() && captured.value.revision() == 1 &&
            frame.value.key().view_id == 7 ? 0 : 1;
    } else {
        std::cerr << "World consumers require a legacy Pipeline registry to construct their aggregate\n";
        return 1;
    }
}

int main() {
    int failures = 0;
#if __has_include(<pipeline.hpp>)
    std::cerr << "World consumers inherit the legacy Pipeline execution include directory\n";
    ++failures;
#endif
    if constexpr (exposes_raw_pipeline<aeg::world::WorldScope>) {
        std::cerr << "World public API exposes raw Pipeline business injection\n";
        ++failures;
    }
    if constexpr (!std::is_same_v<decltype(std::declval<aeg::world::WorldFrameScope>().read_snapshot()),
                                   aeg::foundation::ResultValue<aeg::ecs::WorldSnapshot>>) {
        std::cerr << "World views expose a generic Pipeline Artifact instead of an owning typed snapshot\n";
        ++failures;
    }
    aeg::ecs::WorldManager worlds;
    const auto handle = worlds.create_world({"PublicWorldBoundary", true});
    if (!handle) return 1;
    const auto state = worlds.find(handle.value);
    if (!state) return 1;
    failures += verify_world_consumer<aeg::world::WorldScope>(state.value);
    return failures == 0 ? 0 : 1;
}
