/** @file material_payload.cpp */
#include "material_payload.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace aeg::asset {
namespace {
constexpr uint32_t kMagic = 0x314d4741u;  // AGM1
constexpr uint32_t kMaxCount = 1'000'000;
constexpr uint32_t kMaxString = 16u * 1024u * 1024u;

struct Reader final {
    std::span<const std::byte> bytes{};
    std::size_t offset{};
    template <typename T>
    bool value(T& output) {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
            return false;
        std::memcpy(&output, bytes.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }
    bool string(std::string& output) {
        uint32_t size = 0;
        if (!value(size) || size > kMaxString || size > bytes.size() - offset)
            return false;
        output.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
        offset += size;
        return true;
    }
    bool uuid(AssetId& output) {
        for (auto& byte : output.value.bytes) {
            if (!value(byte))
                return false;
        }
        return true;
    }
};

bool parameter(Reader& reader, pipeline::ParameterValue& output) {
    uint8_t tag = 0;
    if (!reader.value(tag) || tag > 6)
        return false;
    switch (tag) {
        case 0: {
            uint8_t value = 0;
            if (!reader.value(value) || value > 1)
                return false;
            output = value != 0;
            return true;
        }
        case 1: {
            int64_t value = 0;
            if (!reader.value(value))
                return false;
            output = value;
            return true;
        }
        case 2: {
            uint64_t value = 0;
            if (!reader.value(value))
                return false;
            output = value;
            return true;
        }
        case 3: {
            double value = 0;
            if (!reader.value(value) || !std::isfinite(value))
                return false;
            output = value;
            return true;
        }
        case 4: {
            std::string value;
            if (!reader.string(value))
                return false;
            output = std::move(value);
            return true;
        }
        case 5: {
            uint32_t count = 0;
            if (!reader.value(count) || count > kMaxCount ||
                count > (reader.bytes.size() - reader.offset) / sizeof(double))
                return false;
            std::vector<double> value(count);
            for (double& item : value)
                if (!reader.value(item) || !std::isfinite(item))
                    return false;
            output = std::move(value);
            return true;
        }
        case 6: {
            std::string value;
            if (!reader.string(value) || value.empty())
                return false;
            output = pipeline::AssetReference{std::move(value)};
            return true;
        }
        default:
            return false;
    }
}
}  // namespace

foundation::ResultValue<MaterialPayload> decode_material_payload(std::span<const std::byte> bytes) {
    Reader reader{bytes};
    uint32_t magic = 0, schema = 0, texture_count = 0, parameter_count = 0;
    MaterialPayload result{};
    if (!reader.value(magic) || !reader.value(schema) || magic != kMagic || schema == 0) {
        return {foundation::invalid_argument("Material payload header is invalid"), {}};
    }
    result.schema_version = schema;
    if (!reader.uuid(result.shader) || !reader.value(texture_count) || texture_count > kMaxCount) {
        return {foundation::invalid_argument("Material payload header is invalid"), {}};
    }
    result.textures.reserve(texture_count);
    for (uint32_t i = 0; i < texture_count; ++i) {
        MaterialTextureBinding binding{};
        if (!reader.value(binding.binding_id) || binding.binding_id == 0 || !reader.uuid(binding.texture)) {
            return {foundation::invalid_argument("Material payload texture binding is invalid"), {}};
        }
        if (!std::none_of(result.textures.begin(), result.textures.end(), [&](const MaterialTextureBinding& existing) {
                return existing.binding_id == binding.binding_id;
            })) {
            return {foundation::invalid_argument("Material payload contains duplicate texture bindings"), {}};
        }
        result.textures.push_back(binding);
    }
    if (!reader.value(parameter_count) || parameter_count > kMaxCount) {
        return {foundation::invalid_argument("Material payload parameter count is invalid"), {}};
    }
    for (uint32_t i = 0; i < parameter_count; ++i) {
        uint64_t id = 0;
        pipeline::ParameterValue value{};
        if (!reader.value(id) || id == 0 || !parameter(reader, value) ||
            !result.parameters.emplace(id, std::move(value)).second) {
            return {foundation::invalid_argument("Material payload parameter is invalid"), {}};
        }
    }
    if (reader.offset != reader.bytes.size()) {
        return {foundation::invalid_argument("Material payload has trailing bytes"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::asset
