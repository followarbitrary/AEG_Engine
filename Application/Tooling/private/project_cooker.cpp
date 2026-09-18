#include "project_cooker.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <system_error>

#include "asset_dependency_packer.hpp"
#include "asset_metadata.hpp"
#include "cooked_artifact_store.hpp"
#include "cooked_asset.hpp"
#include "game_build_manifest.hpp"
#include "module_bundle.hpp"
#include "pipeline_asset_importer.hpp"
#include "project_context.hpp"
#include "scene_payload.hpp"

namespace aeg::tooling {
namespace {
pipeline::DocumentDiagnostic diagnostic(std::string code, std::string message) {
    return {std::move(code), std::move(message), {}, {}, 0, 0};
}

asset::AssetId runtime_id(const AssetId& source) {
    asset::AssetId result{};
    result.value.bytes = source.value.bytes;
    return result;
}

foundation::ResultValue<AssetId> source_product(const std::filesystem::path& file, AssetKind expected) {
    const auto metadata = AssetMetaCodec{}.load(file.string() + ".meta");
    if (!metadata)
        return {metadata.result, {}};
    if (metadata.value.products.size() != 1 || metadata.value.products.front().kind != expected ||
        metadata.value.products.front().id == AssetId{}) {
        return {foundation::invalid_argument("Startup asset metadata must declare one product of the expected kind"),
                {}};
    }
    return {foundation::Result::success(), metadata.value.products.front().id};
}

foundation::ResultValue<std::string> digest_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return {foundation::not_found("Cannot read file for digest: " + file.string()), {}};
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return module::module_content_sha256(bytes);
}

foundation::Result ensure_scene_cooked(const std::filesystem::path& source, const AssetId& expected,
                                       const std::filesystem::path& cache_root) {
    const auto metadata = AssetMetaCodec{}.load(source.string() + ".meta");
    if (!metadata)
        return metadata.result;
    if (metadata.value.products.size() != 1 || metadata.value.products.front().id != expected ||
        metadata.value.products.front().kind != AssetKind::kScene)
        return foundation::invalid_argument("Startup scene metadata does not match the project manifest");
    std::ifstream input(source, std::ios::binary);
    if (!input)
        return foundation::not_found("Startup scene source is missing");
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    asset::ScenePayload payload{};
    payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                                    reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
    const auto encoded = asset::encode_scene_payload(payload);
    if (!encoded)
        return encoded.result;
    CookedAsset cooked{expected, AssetKind::kScene, 1, {}, encoded.value};
    const auto envelope = CookedAssetCodec{}.encode(cooked);
    if (!envelope)
        return envelope.result;
    const auto published = CookedArtifactStore(cache_root)
                               .publish(metadata.value.source, {{expected, AssetKind::kScene, 1, envelope.value}});
    return published ? foundation::Result::success() : published.result;
}

foundation::Result ensure_pipeline_cooked(const std::filesystem::path& source, const AssetId& expected,
                                          const pipeline::TypeCatalog& catalog,
                                          const std::filesystem::path& cache_root) {
    const auto metadata = AssetMetaCodec{}.load(source.string() + ".meta");
    if (!metadata)
        return metadata.result;
    auto catalog_copy = std::make_shared<pipeline::TypeCatalog>(catalog);
    PipelineAssetImporter importer(std::move(catalog_copy));
    const auto cooked = importer.cook(source, metadata.value);
    if (!cooked)
        return cooked.result;
    std::vector<CookedArtifact> artifacts;
    artifacts.reserve(cooked.value.size());
    for (const auto& asset : cooked.value) {
        if (asset.id != expected || asset.kind != AssetKind::kPipeline)
            return foundation::invalid_argument("Startup pipeline metadata does not match the project manifest");
        const auto envelope = CookedAssetCodec{}.encode(asset);
        if (!envelope)
            return envelope.result;
        artifacts.push_back({asset.id, asset.kind, asset.schema_version, envelope.value});
    }
    const auto published = CookedArtifactStore(cache_root).publish(metadata.value.source, artifacts);
    return published ? foundation::Result::success() : published.result;
}
}  // namespace

foundation::ResultValue<ProjectCookResult> cook_project(const ProjectCookRequest& request) {
    ProjectCookResult result{};
    if (request.project_file.empty() || request.module_manifest.empty() || request.output_directory.empty() ||
        request.configuration.empty()) {
        result.diagnostics.push_back(
            diagnostic("cook.request", "Project cook requires project, module, output and configuration"));
        return {foundation::invalid_argument(result.diagnostics.back().message), std::move(result)};
    }
    const auto module = module::validate_module_bundle(request.module_manifest);
    if (!module) {
        result.diagnostics.push_back(diagnostic("cook.module", module.result.message));
        return {module.result, std::move(result)};
    }
    const auto project = application::project::ProjectFileCodec{}.load(request.project_file);
    if (!project) {
        result.diagnostics.push_back(diagnostic("cook.project", project.result.message));
        return {project.result, std::move(result)};
    }
    const auto paths = application::project::ProjectPaths::resolve(request.project_file, project.value.config.paths);
    if (!paths)
        return {paths.result, std::move(result)};

    std::vector<AssetId> roots;
    const auto scene_path =
        paths.value.directory(application::project::ProjectDirectory::kRoot) / project.value.config.startup_scene;
    const auto scene = source_product(scene_path, AssetKind::kScene);
    if (!scene) {
        result.diagnostics.push_back(diagnostic("cook.startup_scene", scene.result.message));
        return {scene.result, std::move(result)};
    }
    roots.push_back(scene.value);
    for (const auto& pipeline_path : project.value.config.startup_pipelines) {
        const auto path = paths.value.directory(application::project::ProjectDirectory::kRoot) / pipeline_path;
        const auto pipeline = source_product(path, AssetKind::kPipeline);
        if (!pipeline) {
            result.diagnostics.push_back(diagnostic("cook.startup_pipeline", pipeline.result.message));
            return {pipeline.result, std::move(result)};
        }
        roots.push_back(pipeline.value);
    }

    std::error_code error;
    std::filesystem::create_directories(request.output_directory, error);
    if (error)
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot create cook output directory"),
            std::move(result)};
    const auto cache_root = paths.value.directory(application::project::ProjectDirectory::kCache);
    CookedArtifactIndex index(cache_root);
    if (const auto refresh = index.refresh(); !refresh) {
        result.diagnostics.push_back(diagnostic("cook.cache", refresh.message));
        return {refresh, std::move(result)};
    }
    // A clean external project has no Editor-derived cache yet.  Cook the
    // startup closure here using the same metadata/catalog validation as the
    // Editor asset pipeline, then refresh the immutable index before packing.
    if (!index.read(scene.value)) {
        if (const auto cook = ensure_scene_cooked(scene_path, scene.value, cache_root); !cook) {
            result.diagnostics.push_back(diagnostic("cook.startup_scene", cook.message));
            return {cook, std::move(result)};
        }
    }
    const auto package = request.output_directory / "game.aegbin";
    for (const auto& pipeline_path : project.value.config.startup_pipelines) {
        const auto source = paths.value.directory(application::project::ProjectDirectory::kRoot) / pipeline_path;
        const auto product = source_product(source, AssetKind::kPipeline);
        if (!product)
            return {product.result, std::move(result)};
        if (!index.read(product.value)) {
            if (const auto cook =
                    ensure_pipeline_cooked(source, product.value, module.value.metadata.catalog, cache_root);
                !cook) {
                result.diagnostics.push_back(diagnostic("cook.startup_pipeline", cook.message));
                return {cook, std::move(result)};
            }
        }
    }
    if (const auto refresh = index.refresh(); !refresh) {
        result.diagnostics.push_back(diagnostic("cook.cache", refresh.message));
        return {refresh, std::move(result)};
    }
    if (const auto packed =
            AssetDependencyPacker{}.write(package, roots, [&index](AssetId id) { return index.read(id); });
        !packed) {
        result.diagnostics.push_back(diagnostic("cook.dependencies", packed.message));
        return {packed, std::move(result)};
    }

    asset::GameBuildManifest manifest{};
    manifest.project_uuid = project.value.config.project_uuid;
    manifest.module_id = project.value.config.module.id;
    manifest.build_id = module.value.metadata.build_id;
    manifest.module_contract_digest = module.value.metadata.contract_digest;
    const auto package_digest = digest_file(package);
    if (!package_digest) {
        result.diagnostics.push_back(diagnostic("cook.package_digest", package_digest.result.message));
        return {package_digest.result, std::move(result)};
    }
    manifest.package_digest = package_digest.value;
    manifest.startup_scene = runtime_id(roots.front());
    for (std::size_t i = 1; i < roots.size(); ++i)
        manifest.startup_pipelines.push_back(runtime_id(roots[i]));
    manifest.target = project.value.config.target;
    manifest.configuration = request.configuration;
    manifest.package_name = package.filename().string();
    const auto encoded_manifest = asset::encode_game_build_manifest(manifest);
    if (!encoded_manifest)
        return {encoded_manifest.result, std::move(result)};
    result.game_manifest = request.output_directory / "game.manifest.bin";
    std::ofstream binary(result.game_manifest, std::ios::binary | std::ios::trunc);
    binary.write(reinterpret_cast<const char*>(encoded_manifest.value.data()),
                 static_cast<std::streamsize>(encoded_manifest.value.size()));
    if (!binary)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot write game manifest"),
                std::move(result)};

    // A small text sidecar is retained for the bootstrap command-line reader;
    // the binary manifest above remains the portable contract.
    std::ofstream sidecar(request.output_directory / "game.manifest", std::ios::trunc);
    sidecar << "package = \"" << package.filename().string() << "\"\n"
            << "build_id = \"" << manifest.build_id << "\"\n"
            << "target = \"" << manifest.target << "\"\n"
            << "configuration = \"" << manifest.configuration << "\"\n";
    if (!sidecar)
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot write game manifest sidecar"),
            std::move(result)};
    result.package = package;
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::tooling
