#include "../public/game_module.hpp"
namespace aeg::module {
namespace {
class StaticModule final : public Module {
public:
    explicit StaticModule(ModuleDefinition d) : definition_(std::move(d)) {}
    const ModuleDescriptor& descriptor() const noexcept override { return definition_.descriptor; }
    foundation::Result register_module(ModuleRegistrationContext& context) override {
        for (const auto& phase : definition_.phases)
            if (auto r = context.register_phase(phase); !r)
                return r;
        for (auto& pipeline : definition_.pipelines)
            if (auto r = context.register_pipeline(std::move(pipeline)); !r)
                return r;
        for (auto& feature : definition_.features)
            if (auto r = context.register_feature(std::move(feature)); !r)
                return r;
        for (auto& pass : definition_.passes)
            if (auto r = context.register_render_pass(std::move(pass)); !r)
                return r;
        for (const auto& type : definition_.reflected_types)
            if (auto r = context.register_type(type); !r)
                return r;
        for (const auto& binding : definition_.bindings)
            if (auto r = context.bind_feature(binding); !r)
                return r;
        return foundation::Result::success();
    }
    foundation::Result unregister_module() override { return foundation::Result::success(); }

private:
    ModuleDefinition definition_;
};
}  // namespace
foundation::Result register_static_module(ModuleRegistry& registry, ModuleDefinition definition) {
    if (!definition.status || !definition.descriptor.id.is_valid())
        return foundation::invalid_argument("module definition is invalid");
    for (const auto& entry : definition.catalog.entries())
        if (entry.owner_module != definition.descriptor.id)
            return foundation::invalid_argument("module catalog entry is owned by another module");
    return registry.register_module([definition = std::move(definition)]() mutable {
        return std::make_unique<StaticModule>(std::move(definition));
    });
}
}  // namespace aeg::module
