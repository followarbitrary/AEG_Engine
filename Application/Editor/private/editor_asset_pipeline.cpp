/**
 * @file editor_asset_pipeline.cpp
 * @brief Implements CPU asset decoding, proprietary cache files, and watching.
 */
#include "editor_asset_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"
#include "toml.hpp"

namespace aeg::application::asset_pipeline {
namespace {
using foundation::Result;
using foundation::ResultCode;

constexpr uint32_t kBakedVersion = 1;
constexpr std::array<char, 8> kMeshMagic{{'A', 'E', 'G', 'M', 'E', 'S', 'H', 1}};
constexpr std::array<char, 8> kTextureMagic{{'A', 'E', 'G', 'T', 'E', 'X', 'T', 1}};
constexpr std::array<char, 8> kMaterialMagic{{'A', 'E', 'G', 'M', 'A', 'T', 1, 0}};
constexpr std::array<char, 8> kFontMagic{{'A', 'E', 'G', 'F', 'O', 'N', 'T', 1}};
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct BakedHeader final {
    std::array<char, 8> magic{};
    uint32_t version{0};
    uint64_t asset_id{0};
    uint64_t source_hash{0};
    uint64_t payload_size{0};
};

struct MeshHeader final {
    uint32_t vertex_stride{0};
    uint32_t vertex_count{0};
    uint32_t index_count{0};
    uint32_t index_width{0};
    float center[3]{};
    float radius{0.0f};
};

struct TextureHeader final {
    uint32_t width{0};
    uint32_t height{0};
};

struct MaterialHeader final {
    uint64_t texture_asset_id{0};
    float tint[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

[[nodiscard]] std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

[[nodiscard]] uint64_t hash_file(const std::filesystem::path& path, uint64_t seed) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return 0;
    }
    uint64_t hash = kFnvOffset ^ seed;
    std::array<char, 8192> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= kFnvPrime;
        }
    }
    return hash;
}

[[nodiscard]] bool checked_size(uint64_t lhs, uint64_t rhs, uint64_t& output) noexcept {
    if (rhs != 0 && lhs > std::numeric_limits<uint64_t>::max() / rhs) {
        return false;
    }
    output = lhs * rhs;
    return true;
}

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] std::filesystem::path baked_path(const std::filesystem::path& root, const EditorAssetRecord& record) {
    std::string extension = ".aegbin";
    switch (record.kind) {
        case EditorAssetKind::kMesh:
            extension = ".aegmesh";
            break;
        case EditorAssetKind::kTexture:
            extension = ".aegtex";
            break;
        case EditorAssetKind::kMaterial:
            extension = ".aegmatbin";
            break;
        case EditorAssetKind::kFont:
            extension = ".aegfont";
            break;
        default:
            break;
    }
    return root / (std::to_string(record.id) + extension);
}

template <typename T>
[[nodiscard]] bool read_value(std::span<const std::byte> bytes, std::size_t& offset, T& value) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

template <typename T>
[[nodiscard]] bool append_value(std::vector<std::byte>& bytes, const T& value) {
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(T));
    return true;
}

[[nodiscard]] Result write_file_atomically(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return Result::failure(ResultCode::kUnknown, "Cannot create AEG derived-data directory");
    }
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return Result::failure(ResultCode::kUnknown, "Cannot open AEG derived-data temporary file");
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            return Result::failure(ResultCode::kUnknown, "Cannot write AEG derived-data temporary file");
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code replace_error;
        std::filesystem::remove(path, replace_error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        return Result::failure(ResultCode::kUnknown, "Cannot atomically publish AEG derived data");
    }
    return Result::success();
}

[[nodiscard]] Result read_baked(const std::filesystem::path& path, const EditorAssetRecord& record, CpuAsset& output,
                                bool require_source_identity = true) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return foundation::not_found("Matching AEG derived data is not available");
    }
    const std::string encoded((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::span<const std::byte> bytes_span(reinterpret_cast<const std::byte*>(encoded.data()), encoded.size());
    const std::vector<std::byte> bytes(bytes_span.begin(), bytes_span.end());
    if (bytes.size() < sizeof(BakedHeader)) {
        return foundation::invalid_argument("AEG derived data header is truncated");
    }
    BakedHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const auto expected_magic = record.kind == EditorAssetKind::kMesh
                                    ? kMeshMagic
                                    : (record.kind == EditorAssetKind::kTexture
                                           ? kTextureMagic
                                           : (record.kind == EditorAssetKind::kMaterial ? kMaterialMagic : kFontMagic));
    if (header.magic != expected_magic || header.version != kBakedVersion ||
        (require_source_identity && (header.asset_id != record.id || header.source_hash != record.content_hash)) ||
        header.payload_size != bytes.size() - sizeof(BakedHeader)) {
        return foundation::invalid_argument("AEG derived data does not match the current source asset");
    }
    output = {};
    output.asset_id = record.id;
    output.source_hash = record.content_hash;
    output.kind = record.kind;
    output.from_baked_format = true;
    std::size_t offset = sizeof(BakedHeader);
    const std::span<const std::byte> payload = std::span<const std::byte>(bytes).subspan(offset);
    if (record.kind == EditorAssetKind::kMesh) {
        MeshHeader mesh{};
        if (!read_value(payload, offset = 0, mesh)) {
            return foundation::invalid_argument("AEG mesh payload header is truncated");
        }
        uint64_t vertex_bytes = 0;
        uint64_t index_bytes = 0;
        if ((mesh.index_width != sizeof(uint16_t) && mesh.index_width != sizeof(uint32_t)) ||
            !checked_size(mesh.vertex_stride, mesh.vertex_count, vertex_bytes) ||
            !checked_size(mesh.index_width, mesh.index_count, index_bytes) ||
            vertex_bytes > std::numeric_limits<uint64_t>::max() - index_bytes ||
            sizeof(MeshHeader) > std::numeric_limits<uint64_t>::max() - vertex_bytes - index_bytes ||
            sizeof(MeshHeader) + vertex_bytes + index_bytes != payload.size()) {
            return foundation::invalid_argument("AEG mesh payload sizes are invalid");
        }
        output.vertex_stride = mesh.vertex_stride;
        output.vertex_count = mesh.vertex_count;
        output.index_count = mesh.index_count;
        output.indices_are_uint32 = mesh.index_width == sizeof(uint32_t);
        output.bounds_center = {mesh.center[0], mesh.center[1], mesh.center[2]};
        output.bounds_radius = mesh.radius;
        output.vertex_data.resize(static_cast<std::size_t>(vertex_bytes));
        output.index_data.resize(static_cast<std::size_t>(index_bytes));
        std::memcpy(output.vertex_data.data(), payload.data() + sizeof(MeshHeader), vertex_bytes);
        std::memcpy(output.index_data.data(), payload.data() + sizeof(MeshHeader) + vertex_bytes, index_bytes);
    } else if (record.kind == EditorAssetKind::kTexture) {
        TextureHeader texture{};
        if (!read_value(payload, offset = 0, texture)) {
            return foundation::invalid_argument("AEG texture payload header is truncated");
        }
        uint64_t pixel_bytes = 0;
        if (!checked_size(static_cast<uint64_t>(texture.width) * texture.height, 4, pixel_bytes) ||
            sizeof(TextureHeader) + pixel_bytes != payload.size()) {
            return foundation::invalid_argument("AEG texture payload size is invalid");
        }
        output.width = texture.width;
        output.height = texture.height;
        output.pixels.resize(static_cast<std::size_t>(pixel_bytes));
        std::memcpy(output.pixels.data(), payload.data() + sizeof(TextureHeader), pixel_bytes);
    } else if (record.kind == EditorAssetKind::kMaterial) {
        MaterialHeader material{};
        if (!read_value(payload, offset = 0, material) || payload.size() != sizeof(MaterialHeader)) {
            return foundation::invalid_argument("AEG material payload is invalid");
        }
        output.texture_asset_id = material.texture_asset_id;
        std::copy(std::begin(material.tint), std::end(material.tint), output.tint.begin());
    } else if (record.kind == EditorAssetKind::kFont) {
        output.font_data.assign(payload.begin(), payload.end());
    }
    output.success = true;
    return Result::success();
}

struct ObjIndex final {
    int position{-1};
    int uv{-1};
    int normal{-1};
    friend bool operator==(const ObjIndex&, const ObjIndex&) = default;
};

struct ObjIndexHash final {
    std::size_t operator()(const ObjIndex& value) const noexcept {
        return (static_cast<std::size_t>(value.position + 1) * 73856093u) ^
               (static_cast<std::size_t>(value.uv + 1) * 19349663u) ^
               (static_cast<std::size_t>(value.normal + 1) * 83492791u);
    }
};

[[nodiscard]] bool parse_int(std::string_view value, int& output) noexcept {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

[[nodiscard]] bool parse_float(std::string_view value, float& output) noexcept {
    char* end = nullptr;
    const std::string copy(value);
    output = std::strtof(copy.c_str(), &end);
    return end != copy.c_str() && *end == '\0' && finite(output);
}

[[nodiscard]] int obj_index(int value, std::size_t count) noexcept {
    if (value > 0) {
        return value - 1 < static_cast<int>(count) ? value - 1 : -1;
    }
    if (value < 0 && static_cast<std::size_t>(-value) <= count) {
        return static_cast<int>(count) + value;
    }
    return -1;
}

[[nodiscard]] bool parse_obj_corner(std::string_view token, std::size_t positions, std::size_t uvs, std::size_t normals,
                                    ObjIndex& output) {
    std::array<std::string_view, 3> parts{};
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= token.size() && count < parts.size()) {
        const std::size_t end = token.find('/', start);
        parts[count++] = token.substr(start, end == std::string_view::npos ? token.size() - start : end - start);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (count == 0 || parts[0].empty()) {
        return false;
    }
    int position = 0;
    if (!parse_int(parts[0], position)) {
        return false;
    }
    output.position = obj_index(position, positions);
    if (output.position < 0) {
        return false;
    }
    if (count > 1 && !parts[1].empty()) {
        int uv = 0;
        if (!parse_int(parts[1], uv)) {
            return false;
        }
        output.uv = obj_index(uv, uvs);
        if (output.uv < 0) {
            return false;
        }
    }
    if (count > 2 && !parts[2].empty()) {
        int normal = 0;
        if (!parse_int(parts[2], normal)) {
            return false;
        }
        output.normal = obj_index(normal, normals);
        if (output.normal < 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result decode_obj(const std::filesystem::path& path, CpuAsset& output) {
    std::ifstream stream(path);
    if (!stream) {
        return foundation::not_found("OBJ file could not be opened");
    }
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<float, 3>> normals;
    std::vector<ObjIndex> corners;
    std::string line;
    while (std::getline(stream, line)) {
        const std::string_view value(line);
        if (value.starts_with("v ")) {
            std::istringstream input(std::string(value.substr(2)));
            std::array<float, 3> position{};
            if (!(input >> position[0] >> position[1] >> position[2]) || !finite(position[0]) || !finite(position[1]) ||
                !finite(position[2])) {
                return foundation::invalid_argument("OBJ contains an invalid position");
            }
            positions.push_back(position);
        } else if (value.starts_with("vt ")) {
            std::istringstream input(std::string(value.substr(3)));
            std::array<float, 2> uv{};
            if (!(input >> uv[0] >> uv[1]) || !finite(uv[0]) || !finite(uv[1])) {
                return foundation::invalid_argument("OBJ contains an invalid UV");
            }
            uvs.push_back(uv);
        } else if (value.starts_with("vn ")) {
            std::istringstream input(std::string(value.substr(3)));
            std::array<float, 3> normal{};
            if (!(input >> normal[0] >> normal[1] >> normal[2]) || !finite(normal[0]) || !finite(normal[1]) ||
                !finite(normal[2])) {
                return foundation::invalid_argument("OBJ contains an invalid normal");
            }
            normals.push_back(normal);
        } else if (value.starts_with("f ")) {
            std::istringstream input(std::string(value.substr(2)));
            std::string token;
            std::vector<ObjIndex> face;
            while (input >> token) {
                ObjIndex corner{};
                if (!parse_obj_corner(token, positions.size(), uvs.size(), normals.size(), corner)) {
                    return foundation::invalid_argument("OBJ face contains an invalid corner");
                }
                face.push_back(corner);
            }
            if (face.size() < 3) {
                return foundation::invalid_argument("OBJ face has fewer than three corners");
            }
            for (std::size_t index = 1; index + 1 < face.size(); ++index) {
                corners.push_back(face[0]);
                corners.push_back(face[index]);
                corners.push_back(face[index + 1]);
            }
        }
    }
    if (positions.empty() || corners.empty()) {
        return foundation::invalid_argument("OBJ must contain positions and faces");
    }

    struct Vertex final {
        float position[4];
        float uv[2];
        float normal[3];
    };
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<ObjIndex, uint32_t, ObjIndexHash> lookup;
    std::vector<std::array<float, 3>> accumulated_normals;
    vertices.reserve(corners.size());
    indices.reserve(corners.size());
    for (std::size_t corner_index = 0; corner_index < corners.size(); corner_index += 3) {
        const auto& a = positions[static_cast<std::size_t>(corners[corner_index].position)];
        const auto& b = positions[static_cast<std::size_t>(corners[corner_index + 1].position)];
        const auto& c = positions[static_cast<std::size_t>(corners[corner_index + 2].position)];
        const std::array<float, 3> edge0{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const std::array<float, 3> edge1{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const std::array<float, 3> face_normal{edge0[1] * edge1[2] - edge0[2] * edge1[1],
                                               edge0[2] * edge1[0] - edge0[0] * edge1[2],
                                               edge0[0] * edge1[1] - edge0[1] * edge1[0]};
        for (std::size_t local = 0; local < 3; ++local) {
            const ObjIndex key = corners[corner_index + local];
            const auto found = lookup.find(key);
            uint32_t vertex_index = 0;
            if (found == lookup.end()) {
                const auto& position = positions[static_cast<std::size_t>(key.position)];
                Vertex vertex{{position[0], position[1], position[2], 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
                if (key.uv >= 0) {
                    const auto& uv = uvs[static_cast<std::size_t>(key.uv)];
                    vertex.uv[0] = uv[0];
                    vertex.uv[1] = 1.0f - uv[1];
                }
                if (key.normal >= 0) {
                    const auto& normal = normals[static_cast<std::size_t>(key.normal)];
                    std::copy(normal.begin(), normal.end(), std::begin(vertex.normal));
                }
                vertex_index = static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertex);
                accumulated_normals.push_back({0.0f, 0.0f, 0.0f});
                lookup.emplace(key, vertex_index);
            } else {
                vertex_index = found->second;
            }
            if (key.normal < 0) {
                for (std::size_t component = 0; component < 3; ++component) {
                    accumulated_normals[vertex_index][component] += face_normal[component];
                }
            }
            indices.push_back(vertex_index);
        }
    }
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        if (std::abs(vertices[index].normal[0]) + std::abs(vertices[index].normal[1]) +
                std::abs(vertices[index].normal[2]) <=
            std::numeric_limits<float>::epsilon()) {
            auto normal = accumulated_normals[index];
            const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length > std::numeric_limits<float>::epsilon()) {
                normal[0] /= length;
                normal[1] /= length;
                normal[2] /= length;
            } else {
                normal = {0.0f, 0.0f, 1.0f};
            }
            std::copy(normal.begin(), normal.end(), std::begin(vertices[index].normal));
        }
    }
    if (vertices.size() > std::numeric_limits<uint32_t>::max() ||
        indices.size() > std::numeric_limits<uint32_t>::max()) {
        return foundation::invalid_argument("OBJ is too large for the editor mesh contract");
    }
    std::array<float, 3> minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max()};
    std::array<float, 3> maximum{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                 std::numeric_limits<float>::lowest()};
    for (const Vertex& vertex : vertices) {
        for (std::size_t component = 0; component < 3; ++component) {
            minimum[component] = std::min(minimum[component], vertex.position[component]);
            maximum[component] = std::max(maximum[component], vertex.position[component]);
        }
    }
    output.vertex_stride = sizeof(Vertex);
    output.vertex_count = static_cast<uint32_t>(vertices.size());
    output.index_count = static_cast<uint32_t>(indices.size());
    output.indices_are_uint32 = vertices.size() > std::numeric_limits<uint16_t>::max();
    output.vertex_data.resize(vertices.size() * sizeof(Vertex));
    std::memcpy(output.vertex_data.data(), vertices.data(), output.vertex_data.size());
    if (output.indices_are_uint32) {
        output.index_data.resize(indices.size() * sizeof(uint32_t));
        std::memcpy(output.index_data.data(), indices.data(), output.index_data.size());
    } else {
        output.index_data.resize(indices.size() * sizeof(uint16_t));
        auto* destination = reinterpret_cast<uint16_t*>(output.index_data.data());
        for (std::size_t index = 0; index < indices.size(); ++index) {
            destination[index] = static_cast<uint16_t>(indices[index]);
        }
    }
    output.bounds_center = {(minimum[0] + maximum[0]) * 0.5f, (minimum[1] + maximum[1]) * 0.5f,
                            (minimum[2] + maximum[2]) * 0.5f};
    for (const Vertex& vertex : vertices) {
        const float dx = vertex.position[0] - output.bounds_center[0];
        const float dy = vertex.position[1] - output.bounds_center[1];
        const float dz = vertex.position[2] - output.bounds_center[2];
        output.bounds_radius = std::max(output.bounds_radius, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return Result::success();
}

[[nodiscard]] Result decode_image(const std::filesystem::path& path, CpuAsset& output) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return foundation::not_found("Image file could not be opened");
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return foundation::invalid_argument("Image file is empty or too large for the vendored decoder");
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        const std::string reason = stbi_failure_reason() == nullptr ? "unknown decoder error" : stbi_failure_reason();
        return Result::failure(ResultCode::kInvalidArgument, "Image decode failed: " + reason);
    }
    const uint64_t pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4u) {
        stbi_image_free(decoded);
        return foundation::out_of_memory("Decoded image is too large");
    }
    output.width = static_cast<uint32_t>(width);
    output.height = static_cast<uint32_t>(height);
    output.pixels.resize(static_cast<std::size_t>(pixel_count) * 4u);
    std::memcpy(output.pixels.data(), decoded, output.pixels.size());
    stbi_image_free(decoded);
    return Result::success();
}

[[nodiscard]] Result decode_material(const std::filesystem::path& project_root, const EditorAssetRecord& record,
                                     CpuAsset& output) {
    try {
        const toml::value document =
            toml::parse(record.relative_path.empty() ? std::string{} : (project_root / record.relative_path).string());
        const toml::value* tint_value = nullptr;
        if (document.contains("Tint")) {
            tint_value = &toml::find(document, "Tint");
        } else if (document.contains("tint")) {
            tint_value = &toml::find(document, "tint");
        }
        if (tint_value != nullptr && tint_value->is_array()) {
            const auto& values = tint_value->as_array();
            if (values.size() != 4) {
                return foundation::invalid_argument(".aegmat Tint must contain four values");
            }
            for (std::size_t index = 0; index < values.size(); ++index) {
                output.tint[index] = static_cast<float>(values[index].as_floating());
                if (!finite(output.tint[index]) || output.tint[index] < 0.0f || output.tint[index] > 1.0f) {
                    return foundation::invalid_argument(".aegmat Tint values must be within [0, 1]");
                }
            }
        }
        std::string texture;
        if (document.contains("texture") && toml::find(document, "texture").is_string()) {
            texture = toml::find<std::string>(document, "texture");
        } else if (document.contains("Texture") && toml::find(document, "Texture").is_string()) {
            texture = toml::find<std::string>(document, "Texture");
        }
        if (!texture.empty()) {
            std::string normalized = texture;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            const std::filesystem::path material_path = project_root / record.relative_path;
            const std::filesystem::path project_relative_path = project_root / normalized;
            const std::filesystem::path sibling_path = material_path.parent_path() / normalized;
            std::error_code path_error;
            const std::filesystem::path texture_path =
                std::filesystem::exists(project_relative_path, path_error) && !path_error ? project_relative_path
                                                                                          : sibling_path;
            const std::string relative = std::filesystem::relative(texture_path, project_root).generic_string();
            uint64_t id = 14695981039346656037ull;
            for (const unsigned char character : relative) {
                id ^= character;
                id *= 1099511628211ull;
            }
            output.texture_asset_id = id | 0x8000000000000000ull;
        }
        return Result::success();
    } catch (const std::exception& exception) {
        return Result::failure(ResultCode::kInvalidArgument, std::string(".aegmat parse failed: ") + exception.what());
    }
}

}  // namespace

EditorAssetPipeline::~EditorAssetPipeline() {
    shutdown();
}

foundation::Result EditorAssetPipeline::initialize(const std::filesystem::path& project_root) {
    shutdown();
    std::error_code error;
    project_root_ = std::filesystem::weakly_canonical(project_root, error);
    if (error) {
        project_root_ = std::filesystem::absolute(project_root, error);
    }
    derived_root_ = project_root_ / ".aeg" / "DerivedData";
    std::filesystem::create_directories(derived_root_, error);
    if (error) {
        return Result::failure(ResultCode::kUnknown, "Cannot create project DerivedData directory");
    }
    stopping_ = false;
    worker_ = std::thread([this] { worker_loop(); });
    watcher_ = std::thread([this] { watcher_loop(); });
    return Result::success();
}

foundation::Result EditorAssetPipeline::synchronize(std::span<const EditorAssetRecord> records) {
    std::scoped_lock lock(mutex_);
    records_.assign(records.begin(), records.end());
    for (const EditorAssetRecord& record : records_) {
        if (record.status == EditorAssetStatus::kImported && record.kind != EditorAssetKind::kUnknown) {
            const uint64_t revision = next_revision_++;
            latest_revisions_[record.id] = revision;
            work_.push_back({record, false, revision});
        }
    }
    condition_.notify_one();
    return Result::success();
}

foundation::Result EditorAssetPipeline::queue_record(const EditorAssetRecord& record, bool force) {
    if (record.id == 0 || record.relative_path.empty()) {
        return foundation::invalid_argument("Cannot queue an invalid asset record");
    }
    std::scoped_lock lock(mutex_);
    if (!force && std::any_of(work_.begin(), work_.end(), [&record](const WorkItem& item) {
            return item.record.id == record.id && item.record.content_hash == record.content_hash;
        })) {
        return Result::success();
    }
    const uint64_t revision = next_revision_++;
    latest_revisions_[record.id] = revision;
    work_.push_back({record, force, revision});
    condition_.notify_one();
    return Result::success();
}

foundation::Result EditorAssetPipeline::request_reimport(uint64_t asset_id) {
    std::scoped_lock lock(mutex_);
    const auto found = std::find_if(records_.begin(), records_.end(),
                                    [asset_id](const EditorAssetRecord& record) { return record.id == asset_id; });
    if (found == records_.end()) {
        return foundation::not_found("Asset ID is not present in the import pipeline");
    }
    const uint64_t revision = next_revision_++;
    latest_revisions_[found->id] = revision;
    work_.push_back({*found, true, revision});
    condition_.notify_one();
    return Result::success();
}

foundation::Result EditorAssetPipeline::load_fast_path(uint64_t asset_id, CpuAsset& output) const {
    try {
        output = {};
        EditorAssetRecord record{};
        {
            std::scoped_lock lock(mutex_);
            const auto found =
                std::find_if(records_.begin(), records_.end(),
                             [asset_id](const EditorAssetRecord& value) { return value.id == asset_id; });
            if (found == records_.end()) {
                return foundation::not_found("Asset ID is not present in the import pipeline");
            }
            record = *found;
        }
        if (record.status == EditorAssetStatus::kFailed || record.status == EditorAssetStatus::kMissing ||
            record.status == EditorAssetStatus::kUnsupported) {
            return foundation::not_found("Asset is not importable");
        }
        return decode_record(project_root_, derived_root_, record, output, true);
    } catch (const std::bad_alloc&) {
        output = {};
        return foundation::out_of_memory("Asset fast-path loading allocation failed");
    } catch (const std::exception& exception) {
        output = {};
        return foundation::unknown_exception(exception);
    } catch (...) {
        output = {};
        return foundation::Result::failure(ResultCode::kUnknown);
    }
}

foundation::Result EditorAssetPipeline::poll(std::vector<CpuAsset>& completed) {
    if (background_failure_.exchange(false)) {
        return Result::failure(ResultCode::kUnknown, "Editor asset background task failed");
    }
    try {
        std::scoped_lock lock(mutex_);
        completed.insert(completed.end(), std::make_move_iterator(completed_.begin()),
                         std::make_move_iterator(completed_.end()));
        completed_.clear();
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Asset completion queue allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    } catch (...) {
        return Result::failure(ResultCode::kUnknown);
    }
}

void EditorAssetPipeline::shutdown() noexcept {
    stopping_ = true;
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (watcher_.joinable()) {
        watcher_.join();
    }
    std::scoped_lock lock(mutex_);
    records_.clear();
    work_.clear();
    completed_.clear();
    latest_revisions_.clear();
}

void EditorAssetPipeline::worker_loop() {
    while (!stopping_) {
        try {
            WorkItem item{};
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !work_.empty(); });
                if (stopping_) {
                    return;
                }
                item = std::move(work_.front());
                work_.erase(work_.begin());
            }
            CpuAsset asset{};
            // Forced reimports must execute the source importer so stale baked data
            // cannot mask importer failures (including injected allocation failures).
            const Result result = decode_record(project_root_, derived_root_, item.record, asset, !item.force);
            if (!result) {
                asset = {};
                asset.asset_id = item.record.id;
                asset.source_hash = item.record.content_hash;
                asset.kind = item.record.kind;
                asset.diagnostic = result.message;
            } else if (!asset.from_baked_format) {
                const auto source_hash_matches = [this, &item] {
                    const std::filesystem::path source = project_root_ / item.record.relative_path;
                    return hash_file(source, static_cast<uint8_t>(item.record.kind)) == item.record.content_hash;
                };
                // A queued job may outlive one or more editor-side source edits.
                // Never publish bytes decoded from a newer file under an older
                // Asset ID/hash pair.
                if (!source_hash_matches()) {
                    continue;
                }
                const Result bake_result = write_baked(derived_root_, asset);
                if (!bake_result) {
                    asset.diagnostic = "Import succeeded but AEG bake failed: " + bake_result.message;
                } else {
                    if (!source_hash_matches()) {
                        continue;
                    }
                    CpuAsset baked{};
                    const Result baked_read = read_baked(baked_path(derived_root_, item.record), item.record, baked);
                    if (baked_read) {
                        asset = std::move(baked);
                    } else {
                        asset.diagnostic =
                            "Import succeeded but baked AEG data could not be reloaded: " + baked_read.message;
                    }
                }
            }
            if (!asset.from_baked_format) {
                const std::filesystem::path source = project_root_ / item.record.relative_path;
                if (hash_file(source, static_cast<uint8_t>(item.record.kind)) != item.record.content_hash) {
                    continue;
                }
            }
            {
                std::scoped_lock lock(mutex_);
                const auto revision = latest_revisions_.find(item.record.id);
                if (revision == latest_revisions_.end() || revision->second != item.revision) {
                    continue;
                }
                completed_.push_back(std::move(asset));
            }
        } catch (...) {
            background_failure_.store(true);
        }
    }
}

void EditorAssetPipeline::watcher_loop() {
    while (!stopping_) {
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            if (stopping_) {
                return;
            }
            std::vector<EditorAssetRecord> changed;
            {
                std::scoped_lock lock(mutex_);
                for (EditorAssetRecord& record : records_) {
                    const std::filesystem::path path = project_root_ / record.relative_path;
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(path, error) || error) {
                        if (record.status != EditorAssetStatus::kMissing || record.content_hash != 0) {
                            record.status = EditorAssetStatus::kMissing;
                            record.error = "Asset file is missing from the project Assets directory.";
                            record.modified_ticks = 0;
                            record.content_hash = 0;
                            changed.push_back(record);
                        }
                        continue;
                    }
                    const auto ticks = std::filesystem::last_write_time(path, error);
                    if (error) {
                        continue;
                    }
                    const int64_t current_ticks = static_cast<int64_t>(ticks.time_since_epoch().count());
                    if (current_ticks == record.modified_ticks) {
                        continue;
                    }
                    record.modified_ticks = current_ticks;
                    record.content_hash = hash_file(path, static_cast<uint8_t>(record.kind));
                    changed.push_back(record);
                }
            }
            for (const EditorAssetRecord& record : changed) {
                static_cast<void>(queue_record(record, true));
            }
        } catch (...) {
            background_failure_.store(true);
        }
    }
}

foundation::Result EditorAssetPipeline::decode_record(const std::filesystem::path& project_root,
                                                      const std::filesystem::path& derived_root,
                                                      const EditorAssetRecord& record, CpuAsset& output,
                                                      bool allow_baked) {
    if (record.status == EditorAssetStatus::kMissing || record.status == EditorAssetStatus::kUnsupported ||
        record.status == EditorAssetStatus::kFailed) {
        return foundation::not_found("Asset is not importable");
    }
    if (allow_baked) {
        const Result baked = read_baked(baked_path(derived_root, record), record, output);
        if (baked) {
            return baked;
        }
    }
    output = {};
    output.asset_id = record.id;
    output.source_hash = record.content_hash;
    output.kind = record.kind;
    const std::filesystem::path source = project_root / record.relative_path;
    const std::string extension = lower(source.extension().string());
    const bool owned_source = (record.kind == EditorAssetKind::kMesh && extension == ".aegmesh") ||
                              (record.kind == EditorAssetKind::kTexture && extension == ".aegtex") ||
                              (record.kind == EditorAssetKind::kMaterial && extension == ".aegmatbin") ||
                              (record.kind == EditorAssetKind::kFont && extension == ".aegfont");
    if (owned_source) {
        return read_baked(source, record, output, false);
    }
    Result result = Result::failure(ResultCode::kUnsupported, "No importer is registered for this asset type");
    if (record.kind == EditorAssetKind::kMesh && extension == ".obj") {
        result = decode_obj(source, output);
    } else if (record.kind == EditorAssetKind::kTexture) {
        result = decode_image(source, output);
    } else if (record.kind == EditorAssetKind::kMaterial) {
        result = decode_material(project_root, record, output);
    } else if (record.kind == EditorAssetKind::kFont) {
        std::ifstream stream(source, std::ios::binary);
        if (!stream) {
            result = foundation::not_found("Font file could not be opened");
        } else {
            const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            output.font_data.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                                    reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
            result = output.font_data.empty() ? foundation::invalid_argument("Font file is empty") : Result::success();
        }
    }
    if (!result) {
        return result;
    }
    output.success = true;
    return Result::success();
}

foundation::Result EditorAssetPipeline::write_baked(const std::filesystem::path& derived_root, const CpuAsset& asset) {
    BakedHeader header{};
    header.version = kBakedVersion;
    header.asset_id = asset.asset_id;
    header.source_hash = asset.source_hash;
    header.magic = asset.kind == EditorAssetKind::kMesh
                       ? kMeshMagic
                       : (asset.kind == EditorAssetKind::kTexture
                              ? kTextureMagic
                              : (asset.kind == EditorAssetKind::kMaterial ? kMaterialMagic : kFontMagic));
    std::vector<std::byte> payload;
    if (asset.kind == EditorAssetKind::kMesh) {
        MeshHeader mesh{};
        mesh.vertex_stride = asset.vertex_stride;
        mesh.vertex_count = asset.vertex_count;
        mesh.index_count = asset.index_count;
        mesh.index_width = asset.indices_are_uint32 ? sizeof(uint32_t) : sizeof(uint16_t);
        std::copy(asset.bounds_center.begin(), asset.bounds_center.end(), std::begin(mesh.center));
        mesh.radius = asset.bounds_radius;
        static_cast<void>(append_value(payload, mesh));
        payload.insert(payload.end(), asset.vertex_data.begin(), asset.vertex_data.end());
        payload.insert(payload.end(), asset.index_data.begin(), asset.index_data.end());
    } else if (asset.kind == EditorAssetKind::kTexture) {
        TextureHeader texture{asset.width, asset.height};
        static_cast<void>(append_value(payload, texture));
        payload.insert(payload.end(), asset.pixels.begin(), asset.pixels.end());
    } else if (asset.kind == EditorAssetKind::kMaterial) {
        MaterialHeader material{};
        material.texture_asset_id = asset.texture_asset_id;
        std::copy(asset.tint.begin(), asset.tint.end(), std::begin(material.tint));
        static_cast<void>(append_value(payload, material));
    } else if (asset.kind == EditorAssetKind::kFont) {
        payload.insert(payload.end(), asset.font_data.begin(), asset.font_data.end());
    } else {
        return foundation::unsupported("Asset kind cannot be baked");
    }
    header.payload_size = payload.size();
    std::vector<std::byte> bytes;
    bytes.reserve(sizeof(header) + payload.size());
    static_cast<void>(append_value(bytes, header));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return write_file_atomically(baked_path(derived_root, EditorAssetRecord{.id = asset.asset_id, .kind = asset.kind}),
                                 bytes);
}

}  // namespace aeg::application::asset_pipeline
