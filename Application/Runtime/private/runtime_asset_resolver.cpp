#include "runtime_asset_resolver.hpp"

#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include "texture.hpp"

namespace aeg::application::runtime {
namespace {
template <typename T>
uint64_t stable_value(T value) noexcept {
    return value.value();
}

pipeline::RenderGraphTexture as_pipeline_texture(render_graph::RenderGraphTexture value) noexcept {
    return pipeline::RenderGraphTexture{value.index, value.generation, value.graph_id};
}

pipeline::RenderGraphBuffer as_pipeline_buffer(render_graph::RenderGraphBuffer value) noexcept {
    return pipeline::RenderGraphBuffer{value.index, value.generation, value.graph_id};
}

render_graph::RenderGraphBuffer as_render_graph_buffer(pipeline::RenderGraphBuffer value) noexcept {
    return render_graph::RenderGraphBuffer{value.index, value.generation, value.graph_id};
}

render_graph::RenderGraphTexture as_render_graph_texture(pipeline::RenderGraphTexture value) noexcept {
    return render_graph::RenderGraphTexture{value.index, value.generation, value.graph_id};
}
}  // namespace

RuntimeAssetResolver::RuntimeAssetResolver(asset::AssetPackageReader package) noexcept
    : package_(std::make_shared<asset::AssetPackageReader>(std::move(package))),
      cache_(std::make_unique<RuntimeAssetCache>(package_)) {}

RuntimeAssetResolver::RuntimeAssetResolver(RuntimeAssetResolver&& other) noexcept
    : package_(std::move(other.package_)),
      cache_(std::move(other.cache_)),
      handles_(std::move(other.handles_)),
      meshes_(std::move(other.meshes_)),
      materials_(std::move(other.materials_)),
      next_renderer_id_(other.next_renderer_id_),
      upload_context_(other.upload_context_),
      gpu_meshes_(std::move(other.gpu_meshes_)),
      gpu_textures_(std::move(other.gpu_textures_)),
      gpu_texture_resources_(std::move(other.gpu_texture_resources_)),
      gpu_materials_(std::move(other.gpu_materials_)),
      gpu_material_resources_(std::move(other.gpu_material_resources_)),
      failed_gpu_materials_(std::move(other.failed_gpu_materials_)),
      failed_material_parameter_buffers_(std::move(other.failed_material_parameter_buffers_)),
      gpu_objects_(std::move(other.gpu_objects_)),
      gpu_object_frame_numbers_(std::move(other.gpu_object_frame_numbers_)),
      gpu_lighting_(std::move(other.gpu_lighting_)) {
    // A moved-from resolver must never retain the borrowed host pointers or
    // stale maps, otherwise its destructor could attempt a second retirement.
    other.upload_context_ = {};
    other.meshes_.clear();
    other.materials_.clear();
    other.gpu_meshes_.clear();
    other.gpu_textures_.clear();
    other.gpu_texture_resources_.clear();
    other.gpu_materials_.clear();
    other.gpu_material_resources_.clear();
    other.failed_gpu_materials_.clear();
    other.failed_material_parameter_buffers_.clear();
    other.gpu_objects_.clear();
    other.gpu_object_frame_numbers_.clear();
    other.gpu_lighting_.clear();
    other.handles_ = asset::AssetHandleTable{};
    other.next_renderer_id_ = 1;
}

RuntimeAssetResolver::~RuntimeAssetResolver() {
    // Hosts own the frame/device ordering. When destruction happens at a safe
    // boundary, release our handles; an active frame is intentionally left for
    // the host's explicit retirement path rather than forcing a hidden wait.
    if (upload_context_.device && upload_context_.frame_context)
        (void)retire_gpu_resources();
}

foundation::ResultValue<RuntimeAssetResolver> RuntimeAssetResolver::open(const std::filesystem::path& package_path) {
    auto package = asset::AssetPackageReader::open(package_path);
    if (!package)
        return {package.result, {}};
    return {foundation::Result::success(), RuntimeAssetResolver{std::move(package.value)}};
}

foundation::Result RuntimeAssetResolver::mount(asset::AssetPackageReader package) noexcept {
    if (!gpu_meshes_.empty() || !gpu_textures_.empty() || !gpu_texture_resources_.empty() || !gpu_materials_.empty() ||
        !gpu_material_resources_.empty() || !failed_gpu_materials_.empty() ||
        !failed_material_parameter_buffers_.empty() || !gpu_objects_.empty() || !gpu_lighting_.empty()) {
        return foundation::invalid_state("Runtime GPU assets must be retired before mounting a package");
    }
    package_ = std::make_shared<asset::AssetPackageReader>(std::move(package));
    cache_ = std::make_unique<RuntimeAssetCache>(package_);
    meshes_.clear();
    materials_.clear();
    gpu_meshes_.clear();
    gpu_textures_.clear();
    gpu_texture_resources_.clear();
    gpu_materials_.clear();
    gpu_material_resources_.clear();
    failed_gpu_materials_.clear();
    failed_material_parameter_buffers_.clear();
    gpu_objects_.clear();
    gpu_object_frame_numbers_.clear();
    gpu_lighting_.clear();
    handles_ = asset::AssetHandleTable{};
    next_renderer_id_ = 1;
    return foundation::Result::success();
}

foundation::Result RuntimeAssetResolver::retire_gpu_resources() noexcept {
    const bool has_gpu_resources = !gpu_meshes_.empty() || !gpu_textures_.empty() || !gpu_materials_.empty() ||
                                   !failed_gpu_materials_.empty() || !failed_material_parameter_buffers_.empty() ||
                                   !gpu_objects_.empty() || !gpu_lighting_.empty();
    if (!has_gpu_resources) {
        return foundation::Result::success();
    }
    if (!upload_context_.device || !upload_context_.frame_context) {
        return foundation::invalid_state("Runtime GPU retirement requires an active upload context");
    }
    if (upload_context_.frame_context->is_frame_active()) {
        return foundation::invalid_state("Runtime GPU retirement requires no active frame");
    }
    // drain() waits every submitted slot and retires frame-local descriptors
    // before persistent resolver-owned handles are destroyed.
    if (const foundation::Result result = upload_context_.frame_context->drain(); !result) {
        return result;
    }
    if (upload_context_.graph) {
        upload_context_.graph->reset();
    }

    foundation::Result first_error = foundation::Result::success();
    const auto retired = [&first_error](foundation::Result result) {
        if (!result && result.code != foundation::ResultCode::kNotFound && first_error)
            first_error = result;
        return result || result.code == foundation::ResultCode::kNotFound;
    };
    for (auto iterator = gpu_meshes_.begin(); iterator != gpu_meshes_.end();) {
        auto& mesh = iterator->second;
        const bool vertex_done = retired(upload_context_.device->destroy_buffer(mesh.vertex_buffer));
        const bool index_done = retired(upload_context_.device->destroy_buffer(mesh.index_buffer));
        if (vertex_done)
            mesh.vertex_buffer = {};
        if (index_done)
            mesh.index_buffer = {};
        if (vertex_done && index_done)
            iterator = gpu_meshes_.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = gpu_materials_.begin(); iterator != gpu_materials_.end();) {
        auto& material = iterator->second;
        const rhi::BufferHandle parameter_buffer = material.parameter_buffer.buffer;
        // Destroy the descriptor first; its binding 2 write references the
        // caller-owned parameter buffer.
        const foundation::Result result = resource::destroy_texture_material(*upload_context_.device, material);
        bool buffer_done = !parameter_buffer.is_valid();
        if (retired(result) && parameter_buffer.is_valid()) {
            rhi::BufferHandle pending = parameter_buffer;
            const foundation::Result buffer_result = upload_context_.device->destroy_buffer(pending);
            buffer_done = retired(buffer_result);
            if (!buffer_done)
                material.parameter_buffer = {parameter_buffer, 0, sizeof(resource::TextureMaterialParameters)};
        }
        if (retired(result) && buffer_done)
            iterator = gpu_materials_.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = failed_gpu_materials_.begin(); iterator != failed_gpu_materials_.end();) {
        auto& material = iterator->second;
        const rhi::BufferHandle parameter_buffer = material.parameter_buffer.buffer;
        const foundation::Result result = resource::destroy_texture_material(*upload_context_.device, material);
        bool buffer_done = !parameter_buffer.is_valid();
        if (retired(result) && parameter_buffer.is_valid()) {
            rhi::BufferHandle pending = parameter_buffer;
            const foundation::Result buffer_result = upload_context_.device->destroy_buffer(pending);
            buffer_done = retired(buffer_result);
            if (!buffer_done)
                material.parameter_buffer = {parameter_buffer, 0, sizeof(resource::TextureMaterialParameters)};
        }
        if (retired(result) && buffer_done)
            iterator = failed_gpu_materials_.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = failed_material_parameter_buffers_.begin();
         iterator != failed_material_parameter_buffers_.end();) {
        rhi::BufferHandle buffer = iterator->second;
        const foundation::Result result = upload_context_.device->destroy_buffer(buffer);
        if (retired(result))
            iterator = failed_material_parameter_buffers_.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = gpu_textures_.begin(); iterator != gpu_textures_.end();) {
        auto& texture = iterator->second;
        bool view_done = true;
        bool texture_done = true;
        if (upload_context_.texture_uploader) {
            const foundation::Result result = upload_context_.texture_uploader->destroy_texture(texture);
            view_done = !texture.sampled_view.is_valid();
            texture_done = !texture.texture.is_valid();
            retired(result);
        } else {
            const foundation::Result view_result = upload_context_.device->destroy_texture_view(texture.sampled_view);
            const foundation::Result texture_result = upload_context_.device->destroy_texture(texture.texture);
            view_done = retired(view_result);
            texture_done = retired(texture_result);
            if (view_done)
                texture.sampled_view = {};
            if (texture_done)
                texture.texture = {};
        }
        if (view_done && texture_done)
            iterator = gpu_textures_.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = gpu_texture_resources_.begin(); iterator != gpu_texture_resources_.end();) {
        if (gpu_textures_.find(iterator->first) == gpu_textures_.end())
            iterator = gpu_texture_resources_.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = gpu_material_resources_.begin(); iterator != gpu_material_resources_.end();) {
        if (gpu_materials_.find(iterator->first) == gpu_materials_.end())
            iterator = gpu_material_resources_.erase(iterator);
        else
            ++iterator;
    }
    for (auto iterator = gpu_objects_.begin(); iterator != gpu_objects_.end();) {
        auto& object = iterator->second;
        const bool set_done = retired(upload_context_.device->destroy_binding_set(object.binding_set));
        const bool buffer_done = retired(upload_context_.device->destroy_buffer(object.transform_buffer));
        if (set_done)
            object.binding_set = {};
        if (buffer_done)
            object.transform_buffer = {};
        if (set_done && buffer_done) {
            const uint64_t key = iterator->first;
            iterator = gpu_objects_.erase(iterator);
            gpu_object_frame_numbers_.erase(key);
        } else
            ++iterator;
    }
    for (auto iterator = gpu_lighting_.begin(); iterator != gpu_lighting_.end();) {
        auto& lighting = iterator->second;
        const bool set_done = retired(upload_context_.device->destroy_binding_set(lighting.binding_set));
        const bool buffer_done = retired(upload_context_.device->destroy_buffer(lighting.lighting_buffer));
        if (set_done)
            lighting.binding_set = {};
        if (buffer_done)
            lighting.lighting_buffer = {};
        if (set_done && buffer_done)
            iterator = gpu_lighting_.erase(iterator);
        else
            ++iterator;
    }
    return first_error;
}

uint64_t RuntimeAssetResolver::renderer_key(asset::AssetId id) noexcept {
    uint64_t value = 14695981039346656037ull;
    for (const uint8_t byte : id.value.bytes) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return value == 0 ? 1 : value;
}

foundation::ResultValue<renderer::MeshAssetId> RuntimeAssetResolver::bind_mesh(asset::AssetId id) {
    if (!package_)
        return {foundation::invalid_state("Runtime asset package is not mounted"), {}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked || cooked.value.kind != asset::AssetKind::kMesh) {
        return {cooked ? foundation::invalid_argument("Asset is not a mesh") : cooked.result, {}};
    }
    for (const auto& [key, existing] : meshes_)
        if (existing == id)
            return {foundation::Result::success(), renderer::MeshAssetId{key}};
    const uint64_t key = next_renderer_id_++;
    meshes_[key] = id;
    return {foundation::Result::success(), renderer::MeshAssetId{key}};
}

foundation::ResultValue<renderer::MaterialInstanceId> RuntimeAssetResolver::bind_material(asset::AssetId id) {
    if (!package_)
        return {foundation::invalid_state("Runtime asset package is not mounted"), {}};
    const auto cooked = package_->read_cooked(id);
    if (!cooked || cooked.value.kind != asset::AssetKind::kMaterial) {
        return {cooked ? foundation::invalid_argument("Asset is not a material") : cooked.result, {}};
    }
    for (const auto& [key, existing] : materials_)
        if (existing == id)
            return {foundation::Result::success(), renderer::MaterialInstanceId{key}};
    const uint64_t key = next_renderer_id_++;
    materials_[key] = id;
    return {foundation::Result::success(), renderer::MaterialInstanceId{key}};
}

foundation::ResultValue<asset::AssetId> RuntimeAssetResolver::mesh_asset(renderer::MeshAssetId id) const {
    const auto found = meshes_.find(stable_value(id));
    if (found == meshes_.end())
        return {foundation::not_found("Mesh renderer identity is not bound"), {}};
    return {foundation::Result::success(), found->second};
}

foundation::ResultValue<asset::AssetId> RuntimeAssetResolver::material_asset(renderer::MaterialInstanceId id) const {
    const auto found = materials_.find(stable_value(id));
    if (found == materials_.end())
        return {foundation::not_found("Material renderer identity is not bound"), {}};
    return {foundation::Result::success(), found->second};
}

foundation::ResultValue<asset::MeshPayload> RuntimeAssetResolver::decode_mesh(renderer::MeshAssetId id) const {
    if (!package_)
        return {foundation::invalid_state("Runtime asset package is not mounted"), {}};
    const auto asset_id = mesh_asset(id);
    if (!asset_id)
        return {asset_id.result, {}};
    const auto cooked = package_->read_cooked(asset_id.value);
    if (!cooked)
        return {cooked.result, {}};
    return asset::decode_mesh_payload(cooked.value.payload);
}

foundation::ResultValue<renderer::RenderMeshResource> RuntimeAssetResolver::resolve_mesh(
    renderer::MeshAssetId mesh) const {
    const auto asset_id = mesh_asset(mesh);
    if (!asset_id)
        return {asset_id.result, {}};
    if (!cache_)
        return {foundation::invalid_state("Runtime asset cache is not mounted"), {}};
    const auto decoded = cache_->load_mesh(asset_id.value);
    if (!decoded)
        return {decoded.result, {}};
    const uint64_t key = stable_value(mesh);
    if (!upload_context_.device || !upload_context_.frame_context || !upload_context_.graph ||
        !upload_context_.uploader || !upload_context_.frame_context->is_frame_active())
        return {foundation::not_ready("Mesh upload requires an active frame context"), {}};
    if (const auto found = gpu_meshes_.find(key); found != gpu_meshes_.end()) {
        auto& cached = found->second;
        if (cached.vertex_resource.is_valid() && cached.index_resource.is_valid() &&
            upload_context_.graph->contains_buffer(as_render_graph_buffer(cached.vertex_resource)) &&
            upload_context_.graph->contains_buffer(as_render_graph_buffer(cached.index_resource)))
            return {foundation::Result::success(), cached};
        const auto vertex = upload_context_.graph->import_buffer(
            cached.vertex_buffer, cached.vertex_buffer_desc,
            rhi::ResourceState{rhi::ResourceUsage::kVertexInput, rhi::ShaderStage::kVertex, rhi::QueueClass::kGraphics,
                               false, rhi::PipelineStage::kVertexInput});
        if (!vertex)
            return {vertex.result, {}};
        const auto index = upload_context_.graph->import_buffer(
            cached.index_buffer, cached.index_buffer_desc,
            rhi::ResourceState{rhi::ResourceUsage::kIndexInput, rhi::ShaderStage::kVertex, rhi::QueueClass::kGraphics,
                               false, rhi::PipelineStage::kVertexInput});
        if (!index)
            return {index.result, {}};
        cached.vertex_resource = as_pipeline_buffer(vertex.value);
        cached.index_resource = as_pipeline_buffer(index.value);
        return {foundation::Result::success(), cached};
    }
    const auto& payload = decoded.value.payload;
    const auto vertex_bytes = std::as_bytes(std::span<const RuntimeMeshVertex>{decoded.value.vertices});
    const auto index_bytes = std::as_bytes(std::span<const uint32_t>{payload.indices});
    const resource::MeshData mesh_data{
        vertex_bytes, sizeof(RuntimeMeshVertex), static_cast<uint32_t>(decoded.value.vertices.size()),
        index_bytes,  rhi::IndexFormat::kUint32, static_cast<uint32_t>(payload.indices.size())};
    const auto upload = upload_context_.uploader->upload_mesh(*upload_context_.frame_context, *upload_context_.graph,
                                                              mesh_data, "Runtime.Mesh");
    if (!upload)
        return {upload.result, {}};
    renderer::RenderMeshResource resource{};
    resource.vertex_buffer = upload.value.mesh.vertex_buffer;
    resource.vertex_buffer_desc.size = upload.value.mesh.vertex_buffer_size;
    resource.vertex_buffer_desc.capabilities =
        rhi::BufferCapability::kVertex | rhi::BufferCapability::kTransferDestination;
    resource.vertex_buffer_desc.memory = rhi::MemoryClass::kDeviceLocal;
    resource.vertex_buffer_desc.initial_usage = rhi::ResourceUsage::kCopyDestination;
    resource.vertex_buffer_desc.initial_stages = rhi::PipelineStage::kCopy;
    resource.vertex_buffer_desc.initial_queue = rhi::QueueClass::kCopy;
    resource.index_buffer = upload.value.mesh.index_buffer;
    resource.index_buffer_desc.size = upload.value.mesh.index_buffer_size;
    resource.index_buffer_desc.capabilities =
        rhi::BufferCapability::kIndex | rhi::BufferCapability::kTransferDestination;
    resource.index_buffer_desc.memory = rhi::MemoryClass::kDeviceLocal;
    resource.index_buffer_desc.initial_usage = rhi::ResourceUsage::kCopyDestination;
    resource.index_buffer_desc.initial_stages = rhi::PipelineStage::kCopy;
    resource.index_buffer_desc.initial_queue = rhi::QueueClass::kCopy;
    resource.vertex_stride = upload.value.mesh.vertex_stride;
    resource.vertex_count = upload.value.mesh.vertex_count;
    resource.index_count = upload.value.mesh.index_count;
    resource.index_format = upload.value.mesh.index_format;
    resource.vertex_layout = {};
    resource.vertex_layout.position_format = rhi::Format::kRgba32Float;
    resource.vertex_layout.uv_offset = sizeof(float) * 4u;
    resource.vertex_layout.normal_offset = sizeof(float) * 6u;
    resource.vertex_resource = as_pipeline_buffer(upload.value.vertex_resource);
    resource.index_resource = as_pipeline_buffer(upload.value.index_resource);
    gpu_meshes_.emplace(key, resource);
    return {foundation::Result::success(), resource};
}

foundation::ResultValue<renderer::RenderMaterialResource> RuntimeAssetResolver::resolve_material(
    renderer::MaterialInstanceId material) const {
    const auto asset_id = material_asset(material);
    if (!asset_id)
        return {asset_id.result, {}};
    if (!cache_)
        return {foundation::invalid_state("Runtime asset cache is not mounted"), {}};
    const auto decoded = cache_->load_material(asset_id.value);
    if (!decoded)
        return {decoded.result, {}};
    std::vector<std::byte> authored_parameters;
    if (!decoded.value.payload.parameters.empty()) {
        if (upload_context_.material_shader_program == nullptr)
            return {foundation::invalid_argument(
                        "Runtime material authored parameters require a host shader reflection contract"),
                    {}};
        const auto packed =
            shader::pack_parameter_block(*upload_context_.material_shader_program, decoded.value.payload.parameters);
        if (!packed)
            return {packed.result, {}};
        authored_parameters = packed.value;
    }
    // The payload carries a shader UUID, but the current runtime has no
    // reflection/packing table that can translate arbitrary logical texture
    // bindings into the renderer's descriptor slots. Keep the accepted ABI
    // explicit instead of silently treating every payload binding as slot 0.
    if (decoded.value.payload.shader == asset::AssetId{}) {
        return {foundation::invalid_argument("Runtime material requires a valid shader asset reference"), {}};
    }
    if (upload_context_.material_shader == asset::AssetId{}) {
        return {foundation::not_ready("Material GPU contract requires a host shader"), {}};
    }
    if (decoded.value.payload.shader != upload_context_.material_shader) {
        return {foundation::invalid_argument("Runtime material shader does not match the host shader contract"), {}};
    }
    if (decoded.value.payload.textures.size() != 1u) {
        return {foundation::invalid_argument("Runtime material requires exactly one texture binding"), {}};
    }
    if (decoded.value.payload.textures.front().binding_id != 1u) {
        return {foundation::invalid_argument("Runtime material texture binding requires canonical binding id 1 until "
                                             "shader reflection is available"),
                {}};
    }
    const uint64_t key = stable_value(material);
    if (!upload_context_.device || !upload_context_.frame_context || !upload_context_.graph ||
        !upload_context_.texture_uploader || !upload_context_.frame_context->is_frame_active()) {
        return {foundation::not_ready("Material upload requires an active frame context"), {}};
    }
    if (!upload_context_.material_pipeline.is_valid() || !upload_context_.material_binding_layout.is_valid()) {
        return {foundation::not_ready("Material GPU contract requires a pipeline and binding layout"), {}};
    }
    const asset::AssetId texture_id = decoded.value.payload.textures.front().texture;
    const uint64_t texture_key = renderer_key(texture_id);
    auto texture_asset = cache_->load_texture(texture_id);
    if (!texture_asset)
        return {texture_asset.result, {}};

    pipeline::RenderGraphTexture texture_resource{};
    auto texture_found = gpu_texture_resources_.find(texture_key);
    if (texture_found == gpu_texture_resources_.end()) {
        const asset::TexturePayload& payload = texture_asset.value.payload;
        if (payload.width == 0 || payload.height == 0 || payload.channels == 0 || payload.pixels.empty()) {
            return {foundation::invalid_argument("Runtime texture payload is empty"), {}};
        }
        rhi::Format format = rhi::Format::kUnknown;
        switch (payload.channels) {
            case 1:
                format = rhi::Format::kR8Unorm;
                break;
            case 4:
                format = rhi::Format::kRgba8Unorm;
                break;
            default:
                return {foundation::invalid_argument("Runtime texture supports one or four channels"), {}};
        }
        rhi::TextureDesc desc{};
        desc.dimension = rhi::TextureDimension::kTex2D;
        desc.extent = rhi::Extent3D{payload.width, payload.height, 1};
        desc.format = format;
        desc.mip_levels = 1;
        desc.array_layers = 1;
        desc.capabilities = rhi::TextureCapability::kSampled;
        desc.name = rhi::DebugName{"Runtime.Texture"};
        const resource::TextureSubresourceData subresource{
            std::span<const std::byte>{payload.pixels.data(), payload.pixels.size()}, payload.width * payload.channels,
            static_cast<uint32_t>(payload.pixels.size())};
        const resource::TextureData texture_data{desc,
                                                 std::span<const resource::TextureSubresourceData>{&subresource, 1}};
        const auto upload = upload_context_.texture_uploader->upload_texture(
            *upload_context_.frame_context, *upload_context_.graph, texture_data, "Runtime.Texture");
        if (!upload || !upload.value.is_valid())
            return {upload ? foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                         "Texture uploader returned an invalid upload")
                           : upload.result,
                    {}};
        gpu_textures_.emplace(texture_key, upload.value.texture);
        texture_resource = as_pipeline_texture(upload.value.target);
        gpu_texture_resources_.emplace(texture_key, texture_resource);
    } else if (upload_context_.graph->contains_texture(as_render_graph_texture(texture_found->second))) {
        texture_resource = texture_found->second;
    }
    const auto texture_found_gpu = gpu_textures_.find(texture_key);
    if (texture_found_gpu == gpu_textures_.end() || !texture_found_gpu->second.sampled_view.is_valid()) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "Texture upload did not produce a sampled view"),
                {}};
    }

    if (!texture_resource.is_valid()) {
        // RenderGraphTexture handles are generation-scoped. Re-import the
        // persistent RHI texture after a graph reset with its last stable
        // sampled-read state.
        rhi::TextureDesc graph_desc{};
        graph_desc.dimension = rhi::TextureDimension::kTex2D;
        graph_desc.extent = texture_found_gpu->second.extent;
        graph_desc.format = texture_found_gpu->second.format;
        graph_desc.mip_levels = texture_found_gpu->second.mip_levels;
        graph_desc.array_layers = texture_found_gpu->second.array_layers;
        graph_desc.capabilities = rhi::TextureCapability::kSampled;
        graph_desc.initial_usage = rhi::ResourceUsage::kSampledRead;
        graph_desc.initial_stages = rhi::PipelineStage::kFragmentShader;
        graph_desc.initial_queue = rhi::QueueClass::kGraphics;
        graph_desc.name = rhi::DebugName{"Runtime.TextureImported"};
        const auto imported = upload_context_.graph->import_texture(
            texture_found_gpu->second.texture, texture_found_gpu->second.sampled_view, graph_desc,
            rhi::ResourceState{rhi::ResourceUsage::kSampledRead, rhi::ShaderStage::kFragment,
                               rhi::QueueClass::kGraphics, false, rhi::PipelineStage::kFragmentShader});
        if (!imported)
            return {imported.result, {}};
        texture_resource = as_pipeline_texture(imported.value);
        gpu_texture_resources_[texture_key] = texture_resource;
    }

    resource::TextureMaterial material_value{};
    rhi::BufferDesc parameter_desc{};
    const auto material_found = gpu_materials_.find(key);
    if (material_found == gpu_materials_.end()) {
        parameter_desc.size =
            authored_parameters.empty() ? sizeof(resource::TextureMaterialParameters) : authored_parameters.size();
        parameter_desc.capabilities = rhi::BufferCapability::kUniform;
        parameter_desc.memory = rhi::MemoryClass::kUpload;
        parameter_desc.initial_usage = rhi::ResourceUsage::kUniformRead;
        parameter_desc.initial_stages = rhi::PipelineStage::kAllCommands;
        parameter_desc.initial_queue = rhi::QueueClass::kGraphics;
        parameter_desc.name = rhi::DebugName{"Runtime.Material.Parameters"};
        const auto parameter_result = upload_context_.device->create_buffer(parameter_desc);
        if (!parameter_result)
            return {parameter_result.result, {}};
        const resource::TextureMaterialParameters parameters{};
        const std::span<const std::byte> initial_bytes =
            authored_parameters.empty()
                ? std::as_bytes(std::span{&parameters, 1})
                : std::span<const std::byte>{authored_parameters.data(), authored_parameters.size()};
        if (const foundation::Result write_result =
                upload_context_.device->write_buffer(parameter_result.value, 0, initial_bytes);
            !write_result) {
            rhi::BufferHandle failed_buffer = parameter_result.value;
            if (const foundation::Result destroy_result = upload_context_.device->destroy_buffer(failed_buffer);
                !destroy_result) {
                failed_material_parameter_buffers_.emplace(key, failed_buffer);
            }
            return {write_result, {}};
        }
        resource::TextureMaterialDesc material_desc{};
        material_desc.texture_view = texture_found_gpu->second.sampled_view;
        material_desc.sampler = upload_context_.material_sampler;
        material_desc.binding_layout = upload_context_.material_binding_layout;
        material_desc.binding_group = upload_context_.material_binding_group;
        material_desc.name = rhi::DebugName{"Runtime.Material"};
        const auto material_result = resource::create_texture_material(*upload_context_.device, material_desc);
        if (!material_result) {
            rhi::BufferHandle failed_buffer = parameter_result.value;
            if (const foundation::Result destroy_result = upload_context_.device->destroy_buffer(failed_buffer);
                !destroy_result) {
                failed_material_parameter_buffers_.emplace(key, failed_buffer);
            }
            return {material_result.result, {}};
        }
        material_value = material_result.value;
        material_value.parameter_buffer = {parameter_result.value, 0, parameter_desc.size};
        if (const foundation::Result bind_result = resource::update_texture_material_parameters(
                *upload_context_.device, material_value, material_value.parameter_buffer);
            !bind_result) {
            failed_gpu_materials_.emplace(key, material_value);
            return {bind_result, {}};
        }
    } else {
        material_value = material_found->second;
        const auto material_resource = gpu_material_resources_.find(key);
        if (material_resource != gpu_material_resources_.end())
            parameter_desc = material_resource->second.material_parameter_buffer_desc;
    }
    if (!material_value.parameter_buffer.buffer.is_valid() || parameter_desc.size == 0) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "Runtime material parameter buffer is incomplete"),
                {}};
    }
    const auto imported_parameter =
        upload_context_.graph->import_buffer(material_value.parameter_buffer.buffer, parameter_desc);
    if (!imported_parameter) {
        if (material_found == gpu_materials_.end())
            failed_gpu_materials_.emplace(key, material_value);
        return {imported_parameter.result, {}};
    }
    renderer::RenderMaterialResource result{};
    result.pipeline = upload_context_.material_pipeline;
    result.binding_set = material_value.binding_set;
    result.binding_group = material_value.binding_group;
    result.material_texture = texture_resource;
    result.material_parameter_buffer = material_value.parameter_buffer.buffer;
    result.material_parameter_buffer_desc = parameter_desc;
    result.material_parameter_resource = pipeline::RenderGraphBuffer{
        imported_parameter.value.index, imported_parameter.value.generation, imported_parameter.value.graph_id};
    result.material_parameter_offset = material_value.parameter_buffer.offset;
    result.material_parameter_size = material_value.parameter_buffer.size;
    result.depth_pipeline = upload_context_.depth_pipeline;
    result.gbuffer_pipeline = upload_context_.gbuffer_pipeline;
    result.shadow_pipeline = upload_context_.shadow_pipeline;
    result.clustered_pipeline = upload_context_.clustered_pipeline;
    if (!result.is_valid()) {
        if (material_found == gpu_materials_.end())
            failed_gpu_materials_.emplace(key, material_value);
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "Runtime material produced an invalid renderer resource"),
                {}};
    }
    if (material_found == gpu_materials_.end())
        gpu_materials_.emplace(key, material_value);
    gpu_material_resources_[key] = result;
    return {foundation::Result::success(), result};
}

foundation::ResultValue<renderer::RenderObjectResource> RuntimeAssetResolver::resolve_object(
    const renderer::RenderInstance& instance, const renderer::RenderView& view) const {
    if (!upload_context_.device || !upload_context_.frame_context || !upload_context_.frame_context->is_frame_active())
        return {foundation::not_ready("Object upload requires an active frame context"), {}};
    if (!upload_context_.object_binding_layout.is_valid())
        return {foundation::not_ready("Object GPU contract requires a binding layout"), {}};

    // Include the active frame slot in the key. A slot can be reused only after
    // its prior GPU work retires, so this keeps bindings stable for all draws in
    // the current frame while avoiding mutation of an in-flight descriptor set.
    uint64_t key = view.id.value();
    key ^= instance.object.value() + 0x9e3779b97f4a7c15ull + (key << 6u) + (key >> 2u);
    key ^= static_cast<uint64_t>(upload_context_.frame_context->frame_slot_index()) * 0x517cc1b727220a95ull;
    const uint64_t frame_number = upload_context_.frame_context->frame_number() - 1u;
    const auto found = gpu_objects_.find(key);
    const auto frame_found = gpu_object_frame_numbers_.find(key);

    math::Mat4 inverse_model_view{};
    if (!math::try_inverse(view.camera.view * instance.world_transform, inverse_model_view))
        return {foundation::invalid_argument("Object model-view transform is not invertible"), {}};
    renderer::RenderObjectConstants constants{};
    constants.model_view_projection = view.camera.view_projection() * instance.world_transform;
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            constants.normal_matrix.at(row, column) = inverse_model_view.at(column, row);

    if (found != gpu_objects_.end()) {
        if (frame_found != gpu_object_frame_numbers_.end() && frame_found->second == frame_number)
            return {foundation::Result::success(), found->second};
        if (const auto result = upload_context_.device->write_buffer(found->second.transform_buffer, 0,
                                                                     std::as_bytes(std::span{&constants, 1}));
            !result)
            return {result, {}};
        try {
            gpu_object_frame_numbers_[key] = frame_number;
        } catch (const std::bad_alloc&) {
            return {foundation::out_of_memory("Runtime object frame cache allocation failed"), {}};
        }
        return {foundation::Result::success(), found->second};
    }

    rhi::BufferDesc desc{};
    desc.size = sizeof(renderer::RenderObjectConstants);
    desc.capabilities = rhi::BufferCapability::kUniform;
    desc.memory = rhi::MemoryClass::kUpload;
    desc.initial_usage = rhi::ResourceUsage::kUniformRead;
    desc.initial_stages = rhi::PipelineStage::kAllCommands;
    desc.initial_queue = rhi::QueueClass::kGraphics;
    desc.name = rhi::DebugName{"Runtime.Object.Buffer"};
    const auto buffer = upload_context_.device->create_buffer(desc);
    if (!buffer)
        return {buffer.result, {}};
    const auto set = upload_context_.device->create_binding_set({upload_context_.object_binding_layout,
                                                                 rhi::BindingSetLifetime::kPersistent,
                                                                 rhi::DebugName{"Runtime.Object.Set"}});
    if (!set) {
        (void)upload_context_.device->destroy_buffer(buffer.value);
        return {set.result, {}};
    }
    rhi::BindingWrite write{};
    write.location = {0, 0};
    write.resource.type = rhi::BindingType::kUniformBuffer;
    write.resource.buffer_range = {buffer.value, 0, sizeof(renderer::RenderObjectConstants)};
    if (const auto result = upload_context_.device->update_binding_set(set.value, std::span{&write, 1}); !result) {
        (void)upload_context_.device->destroy_binding_set(set.value);
        (void)upload_context_.device->destroy_buffer(buffer.value);
        return {result, {}};
    }

    if (const auto result =
            upload_context_.device->write_buffer(buffer.value, 0, std::as_bytes(std::span{&constants, 1}));
        !result) {
        (void)upload_context_.device->destroy_binding_set(set.value);
        (void)upload_context_.device->destroy_buffer(buffer.value);
        return {result, {}};
    }
    renderer::RenderObjectResource resource{};
    resource.transform_buffer = buffer.value;
    resource.transform_buffer_desc = desc;
    resource.binding_layout = upload_context_.object_binding_layout;
    resource.binding_set = set.value;
    resource.binding_group = 0;
    resource.transform_offset = 0;
    resource.transform_size = sizeof(renderer::RenderObjectConstants);
    try {
        gpu_objects_.emplace(key, resource);
        gpu_object_frame_numbers_.emplace(key, frame_number);
    } catch (const std::bad_alloc&) {
        gpu_objects_.erase(key);
        gpu_object_frame_numbers_.erase(key);
        (void)upload_context_.device->destroy_binding_set(set.value);
        (void)upload_context_.device->destroy_buffer(buffer.value);
        return {foundation::out_of_memory("Runtime object resource cache allocation failed"), {}};
    }
    return {foundation::Result::success(), resource};
}

foundation::ResultValue<renderer::RenderLightingResource> RuntimeAssetResolver::resolve_lighting(
    const renderer::RenderSceneSnapshot& snapshot, const renderer::RenderView&) const {
    if (!upload_context_.device || !upload_context_.frame_context || !upload_context_.graph ||
        !upload_context_.uploader || !upload_context_.frame_context->is_frame_active()) {
        return {foundation::not_ready("Lighting upload requires an active frame context"), {}};
    }
    if (!upload_context_.lighting_binding_layout.is_valid()) {
        return {foundation::not_ready("Lighting GPU contract requires a binding layout"), {}};
    }
    const auto constants = renderer::make_render_lighting_constants(snapshot.lights);
    if (!constants) {
        return {constants.result, {}};
    }

    const uint64_t frame_number = upload_context_.frame_context->frame_number() - 1u;
    const auto found = gpu_lighting_.find(frame_number);
    if (found != gpu_lighting_.end()) {
        const auto imported =
            upload_context_.graph->import_buffer(found->second.lighting_buffer, found->second.lighting_buffer_desc);
        if (!imported) {
            return {imported.result, {}};
        }
        found->second.lighting_resource =
            pipeline::RenderGraphBuffer{imported.value.index, imported.value.generation, imported.value.graph_id};
        return {foundation::Result::success(), found->second};
    }

    rhi::BufferDesc destination{};
    destination.size = sizeof(renderer::RenderLightingConstants);
    // Keep the fixed lighting UBO copyable so diagnostics/readback and future
    // frame capture can inspect the same device-local resource without a
    // second mirror allocation.
    destination.capabilities = rhi::BufferCapability::kUniform | rhi::BufferCapability::kTransferSource;
    destination.memory = rhi::MemoryClass::kDeviceLocal;
    destination.initial_usage = rhi::ResourceUsage::kUniformRead;
    destination.initial_stages = rhi::PipelineStage::kFragmentShader;
    destination.initial_queue = rhi::QueueClass::kGraphics;
    destination.name = rhi::DebugName{"Runtime.Lighting.Constants"};
    const auto upload = upload_context_.uploader->upload_buffer(
        *upload_context_.frame_context, *upload_context_.graph,
        resource::BufferUploadDesc{std::as_bytes(std::span{&constants.value, 1}), destination, 0, 4});
    if (!upload) {
        return {upload.result, {}};
    }
    const auto set = upload_context_.device->create_binding_set({upload_context_.lighting_binding_layout,
                                                                 rhi::BindingSetLifetime::kPersistent,
                                                                 rhi::DebugName{"Runtime.Lighting.Set"}});
    if (!set) {
        rhi::BufferHandle buffer = upload.value.destination;
        (void)upload_context_.device->destroy_buffer(buffer);
        return {set.result, {}};
    }
    rhi::BindingWrite write{};
    write.location = {2, 0};
    write.resource.type = rhi::BindingType::kUniformBuffer;
    write.resource.buffer_range = {upload.value.destination, 0, sizeof(renderer::RenderLightingConstants)};
    if (const foundation::Result result = upload_context_.device->update_binding_set(set.value, std::span{&write, 1});
        !result) {
        (void)upload_context_.device->destroy_binding_set(set.value);
        rhi::BufferHandle buffer = upload.value.destination;
        (void)upload_context_.device->destroy_buffer(buffer);
        return {result, {}};
    }

    renderer::RenderLightingResource resource{};
    resource.binding_set = set.value;
    resource.binding_group = 2;
    resource.lighting_buffer = upload.value.destination;
    resource.lighting_buffer_desc = destination;
    resource.lighting_resource = pipeline::RenderGraphBuffer{upload.value.target.index, upload.value.target.generation,
                                                             upload.value.target.graph_id};
    resource.lighting_offset = upload.value.destination_offset;
    resource.lighting_size = sizeof(renderer::RenderLightingConstants);
    if (!resource.is_valid()) {
        (void)upload_context_.device->destroy_binding_set(set.value);
        rhi::BufferHandle buffer = upload.value.destination;
        (void)upload_context_.device->destroy_buffer(buffer);
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "Runtime lighting produced an invalid renderer resource"),
                {}};
    }
    try {
        gpu_lighting_.emplace(frame_number, resource);
    } catch (const std::bad_alloc&) {
        (void)upload_context_.device->destroy_binding_set(set.value);
        rhi::BufferHandle buffer = upload.value.destination;
        (void)upload_context_.device->destroy_buffer(buffer);
        return {foundation::out_of_memory("Runtime lighting resource cache allocation failed"), {}};
    }
    return {foundation::Result::success(), resource};
}

foundation::ResultValue<renderer::RenderDeferredLightingResource> RuntimeAssetResolver::resolve_deferred_lighting(
    const renderer::RenderView& view) const {
    if (upload_context_.deferred_lighting_resolver)
        return upload_context_.deferred_lighting_resolver(view);
    if (!upload_context_.deferred_lighting.is_valid()) {
        return {foundation::not_ready("Deferred lighting host contract is incomplete"), {}};
    }
    return {foundation::Result::success(), upload_context_.deferred_lighting};
}

foundation::ResultValue<renderer::RenderShadowResource> RuntimeAssetResolver::resolve_shadow(
    const renderer::RenderShadowView& view) const {
    if (!upload_context_.shadow_resolver) {
        return {foundation::not_ready("Shadow host callback is unavailable"), {}};
    }
    return upload_context_.shadow_resolver(view);
}

foundation::ResultValue<renderer::RenderLightCullingResource> RuntimeAssetResolver::resolve_light_culling(
    const renderer::RenderSceneSnapshot& snapshot, const renderer::RenderShadowView& shadow) const {
    if (!upload_context_.light_culling_resolver) {
        return {foundation::not_ready("Light-culling host callback is unavailable"), {}};
    }
    return upload_context_.light_culling_resolver(snapshot, shadow);
}

foundation::ResultValue<renderer::RenderPostProcessResource> RuntimeAssetResolver::resolve_post_process(
    const renderer::RenderView& view) const {
    if (!upload_context_.post_process_resolver) {
        return {foundation::not_ready("Post-process host callback is unavailable"), {}};
    }
    return upload_context_.post_process_resolver(view);
}
}  // namespace aeg::application::runtime
