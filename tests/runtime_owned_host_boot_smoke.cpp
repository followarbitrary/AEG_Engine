/** @file runtime_owned_host_boot_smoke.cpp
 *  @brief Verifies the Windows owned runtime host creates its shader/upload contract.
 */
#include <iostream>

#include "frame_context.hpp"
#include "renderer.hpp"
#include "runtime_boot_fixture.hpp"
#include "shadow_rendering.hpp"
#include "world_scope.hpp"

int main() {
    runtime_test::Fixture fixture{};
    if (!runtime_test::prepare(fixture))
        return 1;

    const auto shader_id = aeg::asset::AssetId::parse("11111111-1111-1111-1111-111111111111");
    if (!shader_id) {
        runtime_test::cleanup(fixture);
        return 2;
    }

    aeg::application::runtime::RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.package_path = fixture.root / "game.aegbin";
    request.owned_host = true;
    request.width = 320;
    request.height = 240;
    request.material_shader = shader_id.value;
#ifdef AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH
    request.material_shader_package = AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH;
#else
    runtime_test::cleanup(fixture);
    return 3;
#endif

    aeg::application::runtime::RuntimeBootstrap bootstrap;
    const auto initialized = bootstrap.initialize(request, std::move(fixture.definition), fixture.identity);
    if (!initialized) {
        std::cerr << "owned host initialize failed: " << initialized.message << '\n';
        runtime_test::cleanup(fixture);
        return 4;
    }
    const auto* host = bootstrap.frame_host_resources();
    if (host == nullptr || !host->require_resolved_render_resources || host->upload_context == nullptr ||
        host->upload_context->device == nullptr || host->upload_context->frame_context == nullptr ||
        host->upload_context->graph == nullptr || host->upload_context->uploader == nullptr ||
        host->upload_context->texture_uploader == nullptr || !host->upload_context->material_pipeline.is_valid() ||
        !host->upload_context->material_binding_layout.is_valid() ||
        host->upload_context->material_shader != shader_id.value || !host->upload_context->deferred_lighting_resolver ||
        !host->upload_context->shadow_resolver || !host->upload_context->light_culling_resolver ||
        !host->upload_context->post_process_resolver || !host->upload_context->shadow_pipeline.is_valid() ||
        !host->upload_context->clustered_pipeline.is_valid() || !host->upload_context->depth_pipeline.is_valid() ||
        !host->upload_context->gbuffer_pipeline.is_valid()) {
        runtime_test::cleanup(fixture);
        return 5;
    }

    auto& upload = *host->upload_context;
    auto& frame_context = *host->frame_context;
    if (!frame_context.begin_frame()) {
        runtime_test::cleanup(fixture);
        return 6;
    }
    const auto* world_scope = bootstrap.startup_world_scope();
    const auto world = world_scope != nullptr && world_scope->handle().is_valid() ? world_scope->handle()
                                                                                  : aeg::ecs::WorldHandle{0, 1};
    if (!world.is_valid()) {
        (void)frame_context.abort_frame();
        runtime_test::cleanup(fixture);
        return 7;
    }
    aeg::renderer::RenderSceneSnapshot snapshot{};
    snapshot.frame_number = frame_context.frame_number();
    snapshot.world_revision = 1;
    snapshot.world = world;
    snapshot.view.world = snapshot.world;
    snapshot.view.id = aeg::renderer::make_render_view_id(snapshot.world, 0);
    snapshot.view.output_extent = {320, 240};
    snapshot.view.camera.view = aeg::math::identity();
    snapshot.view.camera.projection = aeg::math::identity();
    snapshot.view.camera.near_plane = 0.1f;
    snapshot.view.camera.far_plane = 100.0f;
    aeg::renderer::RenderShadowView shadow{};
    shadow.view = snapshot.view;
    shadow.constants.clip_to_shadow = aeg::math::identity();
    shadow.constants.parameters = {1.0f, 1.0f, 0.0f, 0.0f};
    {
        // These frame resources retain the owned shader-resource implementations
        // through callbacks. Release the values before Bootstrap::shutdown()
        // tears down the device that owns their pipelines.
        const auto deferred = upload.deferred_lighting_resolver(snapshot.view);
        const auto shadow_resource = upload.shadow_resolver(shadow);
        const auto culling = upload.light_culling_resolver(snapshot, shadow);
        const auto post_process = upload.post_process_resolver(snapshot.view);
        if (!deferred || !deferred.value.pipeline.is_valid() || !shadow_resource ||
            !shadow_resource.value.buffer.is_valid() || !culling || !culling.value.pipeline.is_valid() ||
            !post_process || !post_process.value.pipeline.is_valid()) {
            (void)frame_context.abort_frame();
            runtime_test::cleanup(fixture);
            return 8;
        }
    }
    if (!frame_context.abort_frame()) {
        runtime_test::cleanup(fixture);
        return 9;
    }
    if (!bootstrap.tick()) {
        runtime_test::cleanup(fixture);
        return 10;
    }
    if (const auto shutdown = bootstrap.shutdown(); !shutdown) {
        std::cerr << "owned host shutdown failed: " << shutdown.message << '\n';
        runtime_test::cleanup(fixture);
        return 11;
    }
    runtime_test::cleanup(fixture);
    std::cout << "runtime owned-host graphics bootstrap passed\n";
    return 0;
}
