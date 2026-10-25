/** @brief Checks the actual Runtime public consumer cannot access upload internals. */
#include <iostream>
#include <type_traits>
#include <utility>

#include "runtime_bootstrap.hpp"

template <typename T>
concept CompleteType = requires { sizeof(T); };

int main() {
    bool exposed = false;
#if __has_include("runtime_asset_resolver.hpp")
    std::cerr << "Runtime public consumers can include the private asset resolver\n";
    exposed = true;
#endif
#if __has_include("runtime_asset_upload_host.hpp")
    std::cerr << "Runtime public consumers can include the private upload host\n";
    exposed = true;
#endif
#if __has_include("runtime_scene_loader.hpp")
    std::cerr << "Runtime public consumers inherit the private Runtime include directory\n";
    exposed = true;
#endif
    using UploadHost = std::remove_pointer_t<decltype(
        std::declval<aeg::application::runtime::RuntimeFrameHostResources>().upload_context)>;
    if constexpr (CompleteType<UploadHost>) {
        std::cerr << "Runtime public bootstrap exposes the full ShaderProgram/RHI upload host\n";
        exposed = true;
    }
#if __has_include("renderer.hpp")
    std::cerr << "Runtime consumers inherit the Renderer public include directory\n";
    exposed = true;
#endif
#if __has_include("renderer_material_contract.hpp")
    std::cerr << "Runtime consumers inherit the Renderer material contract include directory\n";
    exposed = true;
#endif
#if __has_include("renderer_profile_resource_contract.hpp")
    std::cerr << "Runtime consumers inherit the Renderer profile contract include directory\n";
    exposed = true;
#endif
#if __has_include("renderer_shader_contract.hpp")
    std::cerr << "Runtime consumers inherit the Renderer shader contract include directory\n";
    exposed = true;
#endif
#if __has_include("renderer_variants.hpp")
    std::cerr << "Runtime consumers inherit the Renderer variants include directory\n";
    exposed = true;
#endif
#if __has_include("pipeline.hpp")
    std::cerr << "Runtime consumers inherit the Pipeline public include directory\n";
    exposed = true;
#endif
#if __has_include("render_feature_runtime.hpp")
    std::cerr << "Runtime consumers inherit the RenderGraph runtime include directory\n";
    exposed = true;
#endif
#if __has_include("rhi.hpp")
    std::cerr << "Runtime consumers inherit the RHI public include directory\n";
    exposed = true;
#endif
#if __has_include("rhi_struct.hpp")
    std::cerr << "Runtime consumers inherit the RHI struct include directory\n";
    exposed = true;
#endif
#if __has_include("shader_contract.hpp")
    std::cerr << "Runtime consumers inherit the Shader public include directory\n";
    exposed = true;
#endif
#if __has_include("resource_uploader.hpp")
    std::cerr << "Runtime consumers inherit the Resource uploader include directory\n";
    exposed = true;
#endif
#if __has_include("texture_uploader.hpp")
    std::cerr << "Runtime consumers inherit the Texture uploader include directory\n";
    exposed = true;
#endif
#if __has_include("material.hpp")
    std::cerr << "Runtime consumers inherit the material resource include directory\n";
    exposed = true;
#endif
#if __has_include("mesh.hpp")
    std::cerr << "Runtime consumers inherit the mesh resource include directory\n";
    exposed = true;
#endif
#if __has_include("texture.hpp")
    std::cerr << "Runtime consumers inherit the texture resource include directory\n";
    exposed = true;
#endif
#if __has_include("shader_asset.hpp")
    std::cerr << "Runtime consumers inherit the Shader asset include directory\n";
    exposed = true;
#endif
#if __has_include("frame_context.hpp")
    std::cerr << "Runtime consumers inherit the RenderGraph frame include directory\n";
    exposed = true;
#endif
#if __has_include("render_graph.hpp")
    std::cerr << "Runtime consumers inherit the RenderGraph public include directory\n";
    exposed = true;
#endif
#if __has_include("concurrency.hpp")
    std::cerr << "Runtime consumers inherit the Concurrency public include directory\n";
    exposed = true;
#endif
    if (exposed)
        return 1;
    std::cout << "runtime upload host boundary smoke passed\n";
    return 0;
}
