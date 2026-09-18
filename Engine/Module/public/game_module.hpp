#pragma once
#include <concepts>
#include <exception>
#include <new>
#include <type_traits>

#include "module.hpp"
#include "pipeline_authoring.hpp"

namespace aeg::module {
struct ModuleDefinition final {
    foundation::Result status{};
    ModuleDescriptor descriptor{};
    pipeline::TypeCatalog catalog{};
    std::vector<pipeline::PipelineFactory> pipelines{};
    std::vector<pipeline::FeatureFactory> features{};
    std::vector<pipeline::RenderPassFactory> passes{};
    std::vector<reflection::TypeDescriptor> reflected_types{};
    std::vector<pipeline::PhaseDescriptor> phases{};
    std::vector<pipeline::FeatureBindingDescriptor> bindings{};
};

[[nodiscard]] foundation::Result register_static_module(ModuleRegistry&, ModuleDefinition definition);

namespace detail {
template <typename T>
[[nodiscard]] inline foundation::Result validate_authoring_entry(const ModuleDefinition& definition,
                                                                 const pipeline::TypeCatalogEntry& entry,
                                                                 const pipeline::PipelineDescriptor& descriptor) {
    if (entry.key.kind != pipeline::AuthoringTypeKind::kPipeline || entry.key.id != descriptor.id.value() ||
        entry.version != descriptor.version || entry.owner_module != definition.descriptor.id ||
        descriptor.owner_module != definition.descriptor.id) {
        return foundation::invalid_argument("pipeline descriptor and authoring entry disagree");
    }
    return foundation::Result::success();
}

template <typename T>
[[nodiscard]] inline foundation::Result validate_authoring_entry(const ModuleDefinition& definition,
                                                                 const pipeline::TypeCatalogEntry& entry,
                                                                 const pipeline::FeatureDescriptor& descriptor) {
    if (entry.key.kind != pipeline::AuthoringTypeKind::kFeature || entry.key.id != descriptor.id.value() ||
        entry.version != descriptor.version || entry.owner_module != definition.descriptor.id ||
        descriptor.owner_module != definition.descriptor.id) {
        return foundation::invalid_argument("feature descriptor and authoring entry disagree");
    }
    return foundation::Result::success();
}

template <typename T>
[[nodiscard]] inline foundation::Result validate_authoring_entry(const ModuleDefinition& definition,
                                                                 const pipeline::TypeCatalogEntry& entry,
                                                                 const pipeline::RenderPassDescriptor& descriptor) {
    if (entry.key.kind != pipeline::AuthoringTypeKind::kRenderPass || entry.key.id != descriptor.type.value() ||
        entry.version != descriptor.version || entry.owner_module != definition.descriptor.id ||
        descriptor.owner_module != definition.descriptor.id) {
        return foundation::invalid_argument("render pass descriptor and authoring entry disagree");
    }
    return foundation::Result::success();
}
}  // namespace detail

template <std::derived_from<pipeline::Pipeline> T>
[[nodiscard]] foundation::Result add_pipeline(ModuleDefinition& definition, pipeline::TypeCatalogEntry entry) {
    if (!definition.status) {
        return definition.status;
    }
    try {
        static_assert(std::is_default_constructible_v<T>, "registered Pipeline must be default constructible");
        T preview;
        if (const auto result = detail::validate_authoring_entry<T>(definition, entry, preview.descriptor()); !result) {
            definition.status = result;
            return definition.status;
        }
        if (const auto result = definition.catalog.add(entry); !result) {
            definition.status = result;
            return definition.status;
        }
        definition.pipelines.emplace_back([] { return std::make_unique<T>(); });
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        definition.status = foundation::out_of_memory("pipeline registration allocation failed");
        return definition.status;
    } catch (const std::exception& exception) {
        definition.status = foundation::unknown_exception(exception);
        return definition.status;
    }
}

template <pipeline::SupportedGameFeature T>
[[nodiscard]] foundation::Result add_feature(ModuleDefinition& definition, pipeline::TypeCatalogEntry entry) {
    if (!definition.status) {
        return definition.status;
    }
    try {
        static_assert(std::is_default_constructible_v<T>, "registered Feature must be default constructible");
        T preview;
        if (const auto result = detail::validate_authoring_entry<T>(definition, entry, preview.descriptor()); !result) {
            definition.status = result;
            return definition.status;
        }
        if (const auto result = definition.catalog.add(entry); !result) {
            definition.status = result;
            return definition.status;
        }
        definition.features.emplace_back([] { return std::make_unique<T>(); });
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        definition.status = foundation::out_of_memory("feature registration allocation failed");
        return definition.status;
    } catch (const std::exception& exception) {
        definition.status = foundation::unknown_exception(exception);
        return definition.status;
    }
}

template <std::derived_from<pipeline::RenderPass> T>
[[nodiscard]] foundation::Result add_render_pass(ModuleDefinition& definition, pipeline::TypeCatalogEntry entry) {
    if (!definition.status) {
        return definition.status;
    }
    try {
        static_assert(std::is_default_constructible_v<T>, "registered RenderPass must be default constructible");
        T preview;
        if (const auto result = detail::validate_authoring_entry<T>(definition, entry, preview.descriptor()); !result) {
            definition.status = result;
            return definition.status;
        }
        if (const auto result = definition.catalog.add(entry); !result) {
            definition.status = result;
            return definition.status;
        }
        definition.passes.emplace_back([] { return std::make_unique<T>(); });
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        definition.status = foundation::out_of_memory("render pass registration allocation failed");
        return definition.status;
    } catch (const std::exception& exception) {
        definition.status = foundation::unknown_exception(exception);
        return definition.status;
    }
}

#define AEG_REGISTER_PIPELINE(definition, Type, entry) (::aeg::module::add_pipeline<Type>((definition), (entry)))
#define AEG_REGISTER_FEATURE(definition, Type, entry) (::aeg::module::add_feature<Type>((definition), (entry)))
#define AEG_REGISTER_RENDER_PASS(definition, Type, entry) (::aeg::module::add_render_pass<Type>((definition), (entry)))
}  // namespace aeg::module
