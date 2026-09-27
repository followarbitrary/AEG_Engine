/**
 * @file asset_dependency_packer.cpp
 * @brief Dependency-closure package builder implementation.
 */
#include "asset_dependency_packer.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

#include "material_payload.hpp"
#include "scene_payload.hpp"

namespace aeg::tooling {
foundation::Result AssetDependencyPacker::write(const std::filesystem::path& output, std::span<const AssetId> roots,
                                                const CookedAssetResolver& resolve) const {
    if (output.empty() || roots.empty() || !resolve) {
        return foundation::invalid_argument("package output, roots, and resolver are required");
    }

    std::vector<AssetPackageEntry> entries;
    std::map<AssetId, uint8_t> state;
    std::vector<AssetId> stack;
    std::map<AssetId, AssetKind> expected_kinds;
    std::map<AssetId, AssetKind> actual_kinds;
    const auto register_expected = [&](AssetId id, AssetKind kind) -> foundation::Result {
        const auto [expected, inserted] = expected_kinds.emplace(id, kind);
        if (!inserted && expected->second != kind)
            return foundation::invalid_argument("cooked asset dependency has conflicting asset kinds for UUID " +
                                                id.to_string());
        const auto actual = actual_kinds.find(id);
        if (actual != actual_kinds.end() && actual->second != kind)
            return foundation::invalid_argument("cooked asset dependency kind mismatch for UUID " + id.to_string());
        return foundation::Result::success();
    };
    std::function<foundation::Result(AssetId)> visit = [&](AssetId id) -> foundation::Result {
        const auto state_it = state.find(id);
        if (state_it != state.end() && state_it->second == 2) {
            const auto expected = expected_kinds.find(id);
            const auto actual = actual_kinds.find(id);
            if (expected != expected_kinds.end() && actual != actual_kinds.end() && expected->second != actual->second)
                return foundation::invalid_argument("cooked asset dependency kind mismatch for UUID " + id.to_string());
            return foundation::Result::success();
        }
        if (state_it != state.end() && state_it->second == 1) {
            std::ostringstream message;
            message << "cooked asset dependency cycle: ";
            const auto begin = std::find(stack.begin(), stack.end(), id);
            for (auto it = begin; it != stack.end(); ++it) {
                if (it != begin)
                    message << " -> ";
                message << it->to_string();
            }
            message << " -> " << id.to_string();
            return foundation::invalid_argument(message.str());
        }
        state[id] = 1;
        stack.push_back(id);
        const auto bytes = resolve(id);
        if (!bytes) {
            stack.pop_back();
            return bytes.result;
        }
        const auto asset = CookedAssetCodec{}.decode(bytes.value);
        if (!asset) {
            stack.pop_back();
            return asset.result;
        }
        if (asset.value.id != id) {
            stack.pop_back();
            return foundation::invalid_argument("cooked asset resolver returned bytes for the wrong asset identity");
        }
        actual_kinds[id] = asset.value.kind;
        if (const auto expected = expected_kinds.find(id);
            expected != expected_kinds.end() && asset.value.kind != expected->second) {
            stack.pop_back();
            return foundation::invalid_argument("cooked asset dependency kind mismatch for UUID " + id.to_string());
        }
        if (asset.value.kind == AssetKind::kScene) {
            const auto scene = aeg::asset::decode_scene_payload(asset.value.payload);
            if (!scene && aeg::asset::has_scene_payload_header(asset.value.payload)) {
                stack.pop_back();
                return foundation::invalid_argument("Scene cooked payload is malformed: " + scene.result.message);
            }
            if (scene)
                for (const auto& reference : scene.value.references) {
                    if (std::find(asset.value.runtime_dependencies.begin(), asset.value.runtime_dependencies.end(),
                                  reference.asset) == asset.value.runtime_dependencies.end()) {
                        stack.pop_back();
                        return foundation::invalid_argument("Scene reference UUID is absent from runtime dependencies");
                    }
                    if (const foundation::Result result = register_expected(reference.asset, reference.kind); !result) {
                        stack.pop_back();
                        return result;
                    }
                }
        } else if (asset.value.kind == AssetKind::kMaterial) {
            const auto material = aeg::asset::decode_material_payload(asset.value.payload);
            if (!material) {
                stack.pop_back();
                return foundation::invalid_argument("Material cooked payload is malformed: " + material.result.message);
            }
            const auto has_dependency = [&](AssetId dependency) {
                return std::find(asset.value.runtime_dependencies.begin(), asset.value.runtime_dependencies.end(),
                                 dependency) != asset.value.runtime_dependencies.end();
            };
            if (!has_dependency(material.value.shader)) {
                stack.pop_back();
                return foundation::invalid_argument("Material shader UUID is absent from runtime dependencies");
            }
            if (const foundation::Result result = register_expected(material.value.shader, AssetKind::kShader);
                !result) {
                stack.pop_back();
                return result;
            }
            for (const auto& texture : material.value.textures) {
                if (!has_dependency(texture.texture)) {
                    stack.pop_back();
                    return foundation::invalid_argument("Material texture UUID is absent from runtime dependencies");
                }
                if (const foundation::Result result = register_expected(texture.texture, AssetKind::kTexture);
                    !result) {
                    stack.pop_back();
                    return result;
                }
            }
        }
        for (const AssetId dependency : asset.value.runtime_dependencies) {
            if (const foundation::Result result = visit(dependency); !result) {
                stack.pop_back();
                return result;
            }
        }
        stack.pop_back();
        state[id] = 2;
        entries.push_back({asset.value.id, asset.value.kind, asset.value.schema_version, std::move(bytes.value)});
        return foundation::Result::success();
    };
    for (const AssetId root : roots) {
        if (const foundation::Result result = visit(root); !result) {
            return result;
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const AssetPackageEntry& left, const AssetPackageEntry& right) { return left.id < right.id; });
    return AssetPackageWriter{}.write(output, std::move(entries));
}
}  // namespace aeg::tooling
