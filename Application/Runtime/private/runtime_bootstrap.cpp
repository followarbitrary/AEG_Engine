#include "runtime_bootstrap.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include "asset_package_reader.hpp"
#include "game_build_manifest.hpp"
#include "module.hpp"
#include "module_api.h"
#include "module_bundle.hpp"
#include "pipeline_asset.hpp"
#include "pipeline_authoring.hpp"
#include "render_pass.hpp"
#include "scene_payload.hpp"

namespace aeg::application::runtime {
namespace {
struct Manifest final {
    std::filesystem::path package{};
    std::string build_id{};
    std::string target{};
    std::string configuration{};
};

foundation::ResultValue<std::string> file_digest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("Runtime package is missing"), {}};
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return module::module_content_sha256(bytes);
}

foundation::ResultValue<asset::GameBuildManifest> read_binary_manifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {foundation::not_found("Binary game manifest was not found"), {}};
    const auto end = input.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > 64u * 1024u * 1024u)
        return {foundation::invalid_argument("Binary game manifest size is invalid"), {}};
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), end);
    if (!input)
        return {foundation::invalid_argument("Binary game manifest cannot be read"), {}};
    return asset::decode_game_build_manifest(bytes);
}

foundation::ResultValue<Manifest> read_manifest(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        return {foundation::not_found("Game manifest was not found"), {}};
    Manifest result{};
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        const auto trim = [](std::string& text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            const auto last = text.find_last_not_of(" \t\r\n");
            text = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
        };
        trim(key);
        trim(value);
        if (!value.empty() && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (key == "package")
            result.package = value;
        else if (key == "build_id")
            result.build_id = value;
        else if (key == "target")
            result.target = value;
        else if (key == "configuration")
            result.configuration = value;
    }
    if (result.package.empty())
        return {foundation::invalid_argument("Game manifest has no package"), {}};
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace

struct RuntimeBootstrap::Impl final {
    RuntimeBootRequest request{};
    RuntimeBuildIdentity identity{};
    std::unique_ptr<asset::AssetPackageReader> package{};
    std::unique_ptr<module::ModuleRegistry> modules{};
    std::unique_ptr<pipeline::PipelineRegistry> pipelines{};
    std::unique_ptr<reflection::TypeRegistry> reflection{};
    std::unique_ptr<pipeline::RenderPassRegistry> passes{};
    pipeline::TypeCatalog catalog{};
    std::vector<pipeline::PreparedPipelineDocument> startup_pipelines{};
    std::unique_ptr<pipeline::PipelineScheduler> scheduler{};
    std::vector<std::unique_ptr<pipeline::PipelineScopeHost>> pipeline_hosts{};
    uint32_t frames{};
    bool initialized{};
};

RuntimeBootstrap::RuntimeBootstrap() : impl_(std::make_unique<Impl>()) {}
RuntimeBootstrap::~RuntimeBootstrap() {
    (void)shutdown();
}

foundation::Result RuntimeBootstrap::initialize(const RuntimeBootRequest& request, module::ModuleDefinition definition,
                                                const RuntimeBuildIdentity& identity) {
    if (!impl_ || impl_->initialized)
        return foundation::invalid_state("Runtime bootstrap is already initialized");
    if (identity.build_id.empty() || identity.target.empty() || identity.configuration.empty())
        return foundation::invalid_argument("Runtime build identity is incomplete");
    if (!definition.status || !definition.descriptor.id.is_valid())
        return foundation::invalid_argument("Static game module definition is invalid");

    RuntimeBootRequest effective = request;
    // Keep a host-owned copy for package validation and preparation.  The
    // definition itself is moved into ModuleRegistry below, so runtime
    // startup never needs to retain a pointer into the caller's object.
    const pipeline::TypeCatalog catalog = definition.catalog;
    std::filesystem::path manifest = effective.manifest_path;
    if (manifest.empty())
        manifest = std::filesystem::current_path() / "game.manifest";
    foundation::ResultValue<Manifest> manifest_result{};
    if (manifest.extension() == ".bin") {
        manifest_result.result = foundation::Result::success();
    } else {
        manifest_result = read_manifest(manifest);
        if (!manifest_result)
            return manifest_result.result;
    }
    const auto binary_path = manifest.extension() == ".bin" ? manifest : manifest.parent_path() / "game.manifest.bin";
    if (std::filesystem::exists(binary_path)) {
        const auto binary = read_binary_manifest(binary_path);
        if (!binary)
            return binary.result;
        if (binary.value.build_id != identity.build_id || binary.value.target != identity.target ||
            binary.value.configuration != identity.configuration ||
            pipeline::ModuleId(binary.value.module_id).value() != definition.descriptor.id.value() ||
            binary.value.module_contract_digest.empty())
            return foundation::invalid_argument("Binary game manifest identity does not match executable");
        module::ModuleMetadata module_metadata{};
        module_metadata.module = definition.descriptor;
        module_metadata.descriptor = definition.descriptor;
        module_metadata.catalog = definition.catalog;
        module_metadata.abi_major = AEG_MODULE_ABI_MAJOR;
        module_metadata.abi_minor = AEG_MODULE_ABI_MINOR;
        const auto contract = module::module_contract_sha256(module_metadata);
        if (!contract || contract.value != binary.value.module_contract_digest) {
            return foundation::invalid_argument(
                "Binary game manifest module contract does not match executable (expected " +
                binary.value.module_contract_digest + ", executable " +
                (contract ? contract.value : std::string("<unavailable>")) + ")");
        }
        if (manifest_result.value.build_id != binary.value.build_id ||
            (!manifest_result.value.target.empty() && manifest_result.value.target != binary.value.target) ||
            (!manifest_result.value.configuration.empty() &&
             manifest_result.value.configuration != binary.value.configuration))
            return foundation::invalid_argument("Text and binary game manifests disagree");
        manifest_result.value.package = binary.value.package_name;
    }
    if (!manifest_result.value.build_id.empty() && manifest_result.value.build_id != identity.build_id)
        return foundation::invalid_argument("Game manifest build identity does not match executable");
    if (!manifest_result.value.target.empty() && manifest_result.value.target != identity.target)
        return foundation::invalid_argument("Game manifest target does not match executable");
    if (!manifest_result.value.configuration.empty() && manifest_result.value.configuration != identity.configuration)
        return foundation::invalid_argument("Game manifest configuration does not match executable");
    if (effective.package_path.empty())
        effective.package_path = manifest.parent_path() / manifest_result.value.package;

    const auto package = asset::AssetPackageReader::open(effective.package_path);
    if (!package)
        return package.result;
    if (std::filesystem::exists(binary_path)) {
        const auto binary = read_binary_manifest(binary_path);
        if (!binary)
            return binary.result;
        const auto package_digest = file_digest(effective.package_path);
        if (!package_digest)
            return package_digest.result;
        if (package_digest.value != binary.value.package_digest)
            return foundation::invalid_argument("Asset package digest does not match game manifest");
        const auto scene = package.value.read_cooked(binary.value.startup_scene);
        if (!scene || scene.value.kind != asset::AssetKind::kScene)
            return foundation::not_found("Startup scene is missing from asset package");
        const auto decoded_scene = asset::decode_scene_payload(scene.value.payload);
        if (!decoded_scene)
            return decoded_scene.result;
        for (const auto& reference : decoded_scene.value.references) {
            const auto referenced = package.value.read(reference.asset);
            if (!referenced)
                return referenced.result;
        }
        for (const auto id : binary.value.startup_pipelines) {
            const auto pipeline = package.value.read_cooked(id);
            if (!pipeline || pipeline.value.kind != asset::AssetKind::kPipeline)
                return foundation::not_found("Startup pipeline is missing from asset package");
        }
    }
    impl_->pipelines = std::make_unique<pipeline::PipelineRegistry>();
    impl_->reflection = std::make_unique<reflection::TypeRegistry>();
    impl_->passes = std::make_unique<pipeline::RenderPassRegistry>();
    impl_->modules =
        std::make_unique<module::ModuleRegistry>(*impl_->pipelines, impl_->reflection.get(), impl_->passes.get());
    if (const auto result = module::register_static_module(*impl_->modules, std::move(definition)); !result) {
        impl_->modules.reset();
        impl_->passes.reset();
        impl_->reflection.reset();
        impl_->pipelines.reset();
        return result;
    }
    impl_->catalog = catalog;
    if (std::filesystem::exists(binary_path)) {
        const auto binary = read_binary_manifest(binary_path);
        if (!binary)
            return binary.result;
        impl_->startup_pipelines.reserve(binary.value.startup_pipelines.size());
        for (const auto id : binary.value.startup_pipelines) {
            const auto cooked = package.value.read_cooked(id);
            if (!cooked || cooked.value.kind != asset::AssetKind::kPipeline)
                return foundation::not_found("Startup pipeline is missing from asset package");
            const auto decoded = asset::decode_pipeline_asset(cooked.value.payload);
            if (!decoded)
                return decoded.result;
            const auto prepared =
                pipeline::prepare_pipeline_document(decoded.value, impl_->catalog, *impl_->pipelines, *impl_->passes);
            if (!prepared)
                return prepared.result;
            impl_->startup_pipelines.push_back(std::move(prepared.value));
        }
    }
    // A prepared document is still data until each authored Pipeline instance
    // is bound to a Scope host, compiled against the registered module types,
    // and activated. Keep this work in the runtime bootstrap so static games
    // and dynamic/editor hosts consume the same prepared configuration.
    impl_->scheduler = std::make_unique<pipeline::PipelineScheduler>(pipeline::PipelineSchedulerDesc{1, 1, 1, 1});
    for (const auto& document : impl_->startup_pipelines) {
        for (const auto& configuration : document.configurations) {
            if (!configuration) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return foundation::invalid_argument("prepared startup Pipeline configuration is null");
            }
            auto host = std::make_unique<pipeline::PipelineScopeHost>(configuration->scope, configuration->instance);
            if (const auto result = host->bind_pipeline(configuration->pipeline); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            if (const auto result = host->set_configuration(configuration); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            if (const auto result = host->rebuild(*impl_->pipelines); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            if (const auto result = host->activate({}); !result) {
                impl_->pipeline_hosts.clear();
                impl_->scheduler.reset();
                return result;
            }
            impl_->pipeline_hosts.push_back(std::move(host));
        }
    }
    impl_->request = std::move(effective);
    impl_->identity = identity;
    impl_->package = std::make_unique<asset::AssetPackageReader>(std::move(package.value));
    impl_->initialized = true;
    return foundation::Result::success();
}

foundation::Result RuntimeBootstrap::tick() {
    if (!impl_ || !impl_->initialized)
        return foundation::invalid_state("Runtime bootstrap is not initialized");
    for (const auto& host : impl_->pipeline_hosts) {
        if (!host || !impl_->scheduler) {
            return foundation::invalid_state("runtime Pipeline host is not initialized");
        }
        pipeline::PipelineExecutionContext context(nullptr, host->scope(), &host->artifacts());
        if (const auto result = host->execute(*impl_->scheduler, context); !result) {
            return result;
        }
    }
    ++impl_->frames;
    if (!impl_->request.trace_path.empty()) {
        std::ofstream trace(impl_->request.trace_path, std::ios::app);
        if (!trace)
            return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write runtime trace");
        trace << "frame " << impl_->frames << "\n";
    }
    return foundation::Result::success();
}

foundation::Result RuntimeBootstrap::shutdown() {
    if (!impl_)
        return foundation::Result::success();
    foundation::Result first_error = foundation::Result::success();
    for (auto iterator = impl_->pipeline_hosts.rbegin(); iterator != impl_->pipeline_hosts.rend(); ++iterator) {
        if (*iterator && (*iterator)->is_active()) {
            if (const auto result = (*iterator)->deactivate({}); !result && first_error) {
                first_error = result;
            }
        }
    }
    impl_->pipeline_hosts.clear();
    impl_->scheduler.reset();
    if (!impl_->initialized)
        return first_error;
    impl_->modules.reset();
    impl_->passes.reset();
    impl_->reflection.reset();
    impl_->pipelines.reset();
    impl_->startup_pipelines.clear();
    impl_->catalog = {};
    impl_->package.reset();
    impl_->initialized = false;
    return first_error;
}

uint32_t RuntimeBootstrap::frame_count() const noexcept {
    return impl_ ? impl_->frames : 0;
}

std::size_t RuntimeBootstrap::startup_pipeline_count() const noexcept {
    return impl_ ? impl_->startup_pipelines.size() : 0;
}

int run_game(int argc, char** argv, module::ModuleDefinition definition, const RuntimeBuildIdentity& identity,
             GameConfigurationCallback configure) {
    const auto report_failure = [](const foundation::Result& result) {
        std::cerr << "AEG runtime startup failed [" << foundation::result_code_name(result.code)
                  << "]: " << result.message << '\n';
        return static_cast<int>(result.code) + 1;
    };
    RuntimeBootRequest request{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--manifest" && index + 1 < argc)
            request.manifest_path = argv[++index];
        else if (argument == "--package" && index + 1 < argc)
            request.package_path = argv[++index];
        else if (argument == "--frames" && index + 1 < argc) {
            try {
                const unsigned long frames = std::stoul(argv[++index]);
                if (frames > UINT32_MAX) {
                    std::cerr << "AEG runtime argument error: --frames exceeds UINT32_MAX\n";
                    return 2;
                }
                request.max_frames = static_cast<uint32_t>(frames);
            } catch (...) {
                std::cerr << "AEG runtime argument error: --frames must be an unsigned integer\n";
                return 2;
            }
        } else if (argument == "--headless")
            request.headless = true;
        else if (argument == "--offscreen")
            request.offscreen = true;
        else if (argument == "--trace" && index + 1 < argc)
            request.trace_path = argv[++index];
        else if (argument == "--readback" && index + 1 < argc)
            request.readback_path = argv[++index];
    }
    if (configure) {
        if (const auto result = configure(request); !result)
            return report_failure(result);
    }
    RuntimeBootstrap bootstrap;
    if (const auto result = bootstrap.initialize(request, std::move(definition), identity); !result)
        return report_failure(result);
    const uint32_t frames = request.max_frames == 0 ? 1 : request.max_frames;
    for (uint32_t frame = 0; frame < frames; ++frame)
        if (const auto result = bootstrap.tick(); !result)
            return report_failure(result);
    if (const auto result = bootstrap.shutdown(); !result)
        return report_failure(result);
    return 0;
}
}  // namespace aeg::application::runtime
