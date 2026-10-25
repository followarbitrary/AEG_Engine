#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <imgui.h>

#include "feature_contract_metadata.hpp"

namespace aeg::application {

inline const char* parameter_asset_kind_name(foundation::AssetKind kind) {
    switch (kind) {
    case foundation::AssetKind::kMesh: return "Mesh";
    case foundation::AssetKind::kTexture: return "Texture";
    case foundation::AssetKind::kMaterial: return "Material";
    case foundation::AssetKind::kScene: return "Scene";
    case foundation::AssetKind::kFont: return "Font";
    case foundation::AssetKind::kFeatureGraph: return "FeatureGraph";
    case foundation::AssetKind::kShader: return "Shader";
    }
    return "Unknown";
}

namespace parameter_widget_detail {
/** A finite, correctly typed editing buffer; it need not satisfy the author schema. */
inline foundation::ParameterValue editable_seed(const feature::ParameterMetadata& metadata) {
    auto schema = metadata;
    schema.default_value.reset();
    const bool valid = static_cast<bool>(feature::validate_parameter_metadata(schema));
    const auto* range = std::get_if<feature::NumericRange>(&metadata.constraint);
    const double minimum = valid && range ? range->min : 0;
    switch (metadata.type) {
    case feature::ParameterType::kBool: return false;
    case feature::ParameterType::kInt32: case feature::ParameterType::kInt64: {
        const double limit = metadata.type == feature::ParameterType::kInt32 ? 2147483648.0 : 9223372036854775808.0;
        return std::isfinite(minimum) && minimum >= -limit && minimum < limit
                   ? static_cast<int64_t>(minimum) : int64_t{0};
    }
    case feature::ParameterType::kUInt32: case feature::ParameterType::kUInt64: {
        const double limit = metadata.type == feature::ParameterType::kUInt32 ? 4294967296.0 : 18446744073709551616.0;
        return std::isfinite(minimum) && minimum >= 0 && minimum < limit
                   ? static_cast<uint64_t>(minimum) : uint64_t{0};
    }
    case feature::ParameterType::kFloat: case feature::ParameterType::kDouble: return minimum;
    case feature::ParameterType::kString: return std::string{};
    case feature::ParameterType::kEnum: {
        const auto* choices = std::get_if<feature::EnumChoices>(&metadata.constraint);
        return choices && !choices->labels.empty() ? choices->labels.front() : std::string{};
    }
    case feature::ParameterType::kAssetReference: return foundation::AssetReference{};
    default: {
        const auto* vector = std::get_if<feature::VectorConstraint>(&metadata.constraint);
        const double component = valid && vector && vector->component_range ? vector->component_range->min : 0;
        const size_t dimension = metadata.type == feature::ParameterType::kVector2 ? 2 :
                                 metadata.type == feature::ParameterType::kVector3 ? 3 : 4;
        return std::vector<double>(dimension, component);
    }
    }
}
} // namespace parameter_widget_detail

/** Authoring actions accept only initial values that pass the contract validator.
 *  Unit vectors first try continuous/projected candidates, then at most 256 grid
 *  components using pairs of squared components (dimensions are at most four).
 *  Exhausting this bounded search is Unsupported, never proof of an invalid schema.
 */
inline foundation::ResultValue<foundation::ParameterValue> try_initial_parameter_value(
    const feature::ParameterMetadata& metadata) {
    if (const auto valid = feature::validate_parameter_metadata(metadata); !valid) return {valid, {}};
    auto initial = metadata.default_value ? *metadata.default_value : parameter_widget_detail::editable_seed(metadata);
    const auto accepts = [&](const foundation::ParameterValue& value) {
        return static_cast<bool>(feature::validate_parameter_value(value, metadata));
    };
    if (accepts(initial)) return {foundation::Result::success(), std::move(initial)};
    const auto* constraint = std::get_if<feature::VectorConstraint>(&metadata.constraint);
    auto* vector = std::get_if<std::vector<double>>(&initial);
    if (!constraint || !constraint->normalized || !vector)
        return {foundation::unsupported("No automatic initial value satisfies this constraint; enter a value manually"), {}};
    const auto dimension = vector->size();
    const feature::NumericRange range = constraint->component_range.value_or(feature::NumericRange{-1, 1});
    const auto project = [&](double target) {
        const double clamped = (std::clamp)(target, range.min, range.max);
        if (range.step == 0) return clamped;
        const long double position = (static_cast<long double>(clamped) - range.min) / range.step;
        if (!std::isfinite(position)) return clamped;
        const long double projected = static_cast<long double>(range.min) + std::round(position) * range.step;
        if (!std::isfinite(projected) || projected < range.min || projected > range.max) return clamped;
        return static_cast<double>(projected);
    };
    const double equal = 1 / std::sqrt(static_cast<double>(dimension));
    for (const double sign : {1.0, -1.0}) {
        vector->assign(dimension, sign * equal);
        if (accepts(initial)) return {foundation::Result::success(), std::move(initial)};
        vector->assign(dimension, project(sign * equal));
        if (accepts(initial)) return {foundation::Result::success(), std::move(initial)};
        vector->assign(dimension, project(0));
        vector->front() = project(sign);
        if (accepts(initial)) return {foundation::Result::success(), std::move(initial)};
    }
    if (range.step > 0) {
        constexpr size_t component_budget = 256;
        constexpr double unit_limit = 1.000001; // The contract's unit-length tolerance.
        const long double low = (std::max)(range.min, -unit_limit);
        const long double high = (std::min)(range.max, unit_limit);
        const long double first = std::ceil((low - range.min) / range.step);
        const long double last = std::floor((high - range.min) / range.step);
        const long double count = last - first + 1;
        if (std::isfinite(first) && std::isfinite(last) && count > 0 && count <= component_budget) {
            std::vector<double> components;
            auto component_metadata = metadata;
            component_metadata.default_value.reset();
            std::get<feature::VectorConstraint>(component_metadata.constraint).normalized = false;
            for (size_t index = 0; index < static_cast<size_t>(count); ++index) {
                const double component = static_cast<double>(static_cast<long double>(range.min) +
                                         (first + static_cast<long double>(index)) * range.step);
                if (std::isfinite(component) &&
                    feature::validate_parameter_value(std::vector<double>(dimension, component), component_metadata))
                    components.push_back(component);
            }
            struct Partial { long double squared; std::array<double, 2> values; };
            const auto partials = [&](size_t length) {
                std::vector<Partial> values;
                for (size_t first_index = 0; first_index < components.size(); ++first_index) {
                    const double a = components[first_index];
                    if (length == 1) values.push_back({static_cast<long double>(a) * a, {a, 0}});
                    else for (size_t second_index = first_index; second_index < components.size(); ++second_index) {
                        const double b = components[second_index];
                        values.push_back({static_cast<long double>(a) * a + static_cast<long double>(b) * b, {a, b}});
                    }
                }
                std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.squared < b.squared; });
                return values;
            };
            const size_t left_size = dimension / 2;
            const auto left = partials(left_size);
            const auto right = partials(dimension - left_size);
            for (const auto& a : left) {
                const auto near = std::lower_bound(right.begin(), right.end(), 1 - a.squared,
                    [](const auto& value, long double target) { return value.squared < target; });
                const size_t position = static_cast<size_t>(near - right.begin());
                const size_t begin = position > 0 ? position - 1 : 0;
                const size_t end = (std::min)(right.size(), position + 2);
                for (size_t index = begin; index < end; ++index) {
                    for (size_t component = 0; component < left_size; ++component) (*vector)[component] = a.values[component];
                    for (size_t component = left_size; component < dimension; ++component)
                        (*vector)[component] = right[index].values[component - left_size];
                    if (accepts(initial)) return {foundation::Result::success(), std::move(initial)};
                }
            }
        }
    }
    return {foundation::unsupported("Automatic unit-vector initial value exceeds the supported search; enter components manually"), {}};
}

/** Safe editing fallback only. Use try_initial_parameter_value when authoring a value. */
inline foundation::ParameterValue initial_parameter_value(const feature::ParameterMetadata& metadata) {
    if (metadata.default_value) return *metadata.default_value;
    const auto initial = try_initial_parameter_value(metadata);
    return initial ? initial.value : parameter_widget_detail::editable_seed(metadata);
}

/** Dynamic text storage shared by both authoring panels; no fixed-size truncation. */
inline bool draw_parameter_text(const char* label, std::string& text) {
    std::vector<char> buffer((std::max)(size_t{64}, text.size() + 1), '\0');
    std::copy(text.begin(), text.end(), buffer.begin());
    const auto resize = [](ImGuiInputTextCallbackData* data) {
        auto& storage = *static_cast<std::vector<char>*>(data->UserData);
        storage.resize(static_cast<size_t>(data->BufSize));
        data->Buf = storage.data();
        return 0;
    };
    const bool changed = ImGui::InputText(label, buffer.data(), buffer.size(),
                                         ImGuiInputTextFlags_CallbackResize, resize, &buffer);
    if (changed) text.assign(buffer.data());
    return changed;
}

/** Edits only a value. The caller owns validation and the authoring transaction. */
inline bool draw_parameter_value(const char* label, foundation::ParameterValue& value,
                                 const feature::ParameterMetadata& metadata) {
    const auto initial = parameter_widget_detail::editable_seed(metadata);
    if (value.index() != initial.index()) value = initial;
    switch (metadata.type) {
    case feature::ParameterType::kBool:
        return ImGui::Checkbox(label, &std::get<bool>(value));
    case feature::ParameterType::kInt32: case feature::ParameterType::kInt64:
        return ImGui::InputScalar(label, ImGuiDataType_S64, &std::get<int64_t>(value));
    case feature::ParameterType::kUInt32: case feature::ParameterType::kUInt64:
        return ImGui::InputScalar(label, ImGuiDataType_U64, &std::get<uint64_t>(value));
    case feature::ParameterType::kFloat: case feature::ParameterType::kDouble: {
        const auto* range = std::get_if<feature::NumericRange>(&metadata.constraint);
        return ImGui::InputDouble(label, &std::get<double>(value), range ? range->step : 0);
    }
    case feature::ParameterType::kString:
        return draw_parameter_text(label, std::get<std::string>(value));
    case feature::ParameterType::kEnum: {
        const auto* choices = std::get_if<feature::EnumChoices>(&metadata.constraint);
        auto& current = std::get<std::string>(value);
        bool changed = false;
        if (ImGui::BeginCombo(label, current.empty() ? "Select" : current.c_str())) {
            if (choices) for (const auto& choice : choices->labels) {
                if (ImGui::Selectable(choice.c_str(), choice == current)) { current = choice; changed = true; }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    case feature::ParameterType::kAssetReference:
        return draw_parameter_text(label, std::get<foundation::AssetReference>(value).uuid);
    default: {
        auto& vector = std::get<std::vector<double>>(value);
        if (vector.empty()) vector = std::get<std::vector<double>>(initial);
        bool changed = false;
        ImGui::PushID(label);
        for (size_t index = 0; index < vector.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            changed |= ImGui::InputDouble("Component", &vector[index]);
            ImGui::PopID();
        }
        ImGui::PopID();
        return changed;
    }
    }
}
} // namespace aeg::application
