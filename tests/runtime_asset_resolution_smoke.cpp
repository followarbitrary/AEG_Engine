#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "runtime_asset_cache.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
namespace {
using namespace aeg;
foundation::AssetId id(const char* v) {
    return foundation::AssetId::parse(v).value;
}
void u32(std::vector<std::byte>& d, size_t o, uint32_t v) {
    std::memcpy(d.data() + o, &v, 4);
}
std::vector<std::byte> mesh() {
    std::vector<std::byte> d(16 + 32 * 3 + 12);
    u32(d, 0, 3);
    u32(d, 4, 3);
    u32(d, 8, 32);
    u32(d, 16 + 32 * 3 + 4, 1);
    u32(d, 16 + 32 * 3 + 8, 2);
    return d;
}
std::vector<std::byte> tex() {
    std::vector<std::byte> d(16, std::byte{0xff});
    u32(d, 0, 1);
    u32(d, 4, 1);
    u32(d, 8, 4);
    return d;
}
std::vector<std::byte> mat(foundation::AssetId t, foundation::AssetId shader) {
    std::vector<std::byte> d(4 + 4 + 16 + 4 + 8 + 16 + 4);
    u32(d, 0, 0x314d4741u);
    u32(d, 4, 1);
    std::memcpy(d.data() + 8, shader.value.bytes.data(), 16);
    u32(d, 24, 1);
    uint64_t b = 1;
    std::memcpy(d.data() + 28, &b, 8);
    std::memcpy(d.data() + 36, t.value.bytes.data(), 16);
    return d;
}
}  // namespace
int main() {
    using namespace aeg;
    auto a = id("00112233-4455-6677-8899-aabbccddeeff"), b = id("10112233-4455-6677-8899-aabbccddeeff"),
         t = id("20112233-4455-6677-8899-aabbccddeeff"), bt = id("30112233-4455-6677-8899-aabbccddeeff"),
         m = id("40112233-4455-6677-8899-aabbccddeeff"), mt = id("50112233-4455-6677-8899-aabbccddeeff"),
         mm = id("60112233-4455-6677-8899-aabbccddeeff"), bm = id("70112233-4455-6677-8899-aabbccddeeff"),
         shader = id("90112233-4455-6677-8899-aabbccddeeff");
    std::vector<tooling::AssetPackageEntry> es;
    auto add = [&](foundation::AssetId x, foundation::AssetKind k, std::vector<std::byte> p) {
        auto c = asset::encode_cooked_asset({x, k, 1, {}, std::move(p)});
        if (!c)
            return false;
        es.push_back({x, k, 1, c.value});
        return true;
    };
    if (!add(a, foundation::AssetKind::kMesh, mesh()) || !add(b, foundation::AssetKind::kMesh, mesh()) ||
        !add(t, foundation::AssetKind::kTexture, tex()) || !add(bt, foundation::AssetKind::kTexture, {std::byte{1}}) ||
        !add(m, foundation::AssetKind::kMaterial, mat(t, shader)) ||
        !add(mm, foundation::AssetKind::kMaterial, mat(mt, shader)) || !add(bm, foundation::AssetKind::kMesh, {std::byte{1}}))
        return 1;
    auto p = std::filesystem::temp_directory_path() / "aeg_runtime_asset_resolution.aegbin";
    if (!tooling::AssetPackageWriter{}.write(p, es))
        return 2;
    auto pkg = asset::AssetPackageReader::open(p);
    if (!pkg)
        return 3;
    auto sp = std::make_shared<asset::AssetPackageReader>(pkg.value);
    application::runtime::RuntimeAssetCache c{sp};
    auto x = c.load_mesh(a), y = c.load_mesh(a), z = c.load_mesh(b);
    if (!x || !y || !z || x.value.handle != y.value.handle || z.value.handle.value != x.value.handle.value + 1)
        return 4;
    if (!c.load_texture(t) || !c.load_material(m))
        return 5;
    auto w = c.load_texture(a);
    if (w || w.result.code != foundation::ResultCode::kInvalidArgument)
        return 6;
    auto i = c.load_mesh(bm), r = c.load_mesh(bm);
    if (i || r || i.result.code != foundation::ResultCode::kInvalidArgument || i.result.message != r.result.message)
        return 7;
    if (!c.retry(bm, foundation::AssetKind::kMesh) || c.load_mesh(bm))
        return 8;
    auto q = c.load_texture(bt);
    if (q || q.result.code != foundation::ResultCode::kInvalidArgument)
        return 9;
    auto n = c.load_material(mm);
    if (n || n.result.code != foundation::ResultCode::kNotFound)
        return 10;
    application::runtime::RuntimeAssetResolver resolver{pkg.value};
    auto ba = resolver.bind_mesh(a), ra = resolver.bind_mesh(a), bb = resolver.bind_mesh(b);
    if (!ba || !ra || !bb || ba.value != ra.value || bb.value.value() != ba.value.value() + 1)
        return 11;
    if (resolver.bind_material(a))
        return 12;
    auto bm_id = resolver.bind_material(m);
    if (!bm_id)
        return 13;
    auto no_gpu = resolver.resolve_material(bm_id.value);
    if (no_gpu || no_gpu.result.code != foundation::ResultCode::kNotReady)
        return 14;
    renderer::RenderInstance object_instance{};
    renderer::RenderView object_view{};
    auto no_object_gpu = resolver.resolve_object(object_instance, object_view);
    if (no_object_gpu || no_object_gpu.result.code != foundation::ResultCode::kNotReady ||
        no_object_gpu.result.message != "Object upload requires an active frame context")
        return 15;
    if (!resolver.retire_gpu_resources())
        return 16;
    std::filesystem::remove(p);
    std::cout << "runtime asset cache resolution passed\n";
    return 0;
}
