#pragma once

#include <array>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "feature_types.hpp"
#include "result.hpp"

namespace aeg::feature {
struct FeatureContract;

enum class RenderIntentCapability : uint64_t {
    kCreateTransient = 1ull << 0,
    kClearColor = 1ull << 1,
    kCopyTexture = 1ull << 2,
    kWriteExternal = 1ull << 3,
    kReadExternal = 1ull << 4,
    kExportOutput = 1ull << 5,
    kCreateHistory = 1ull << 6,
    kDrawScene = 1ull << 7,
    kCullLights = 1ull << 8,
    kReadLightList = 1ull << 9,
};
[[nodiscard]] constexpr uint64_t to_mask(RenderIntentCapability capability) noexcept {
    return static_cast<uint64_t>(capability);
}
inline constexpr uint64_t kAllRenderIntentCapabilities = (1ull << 10) - 1;
inline constexpr SideEffectId kRenderIntentEffect{"Render.Intent"};

/** Local identities belong to one Feature instance; external identities require host bindings. */
struct RenderTextureReference final {
    RenderResourceId id{};
    uint64_t version{1};
    bool external{false};
};
enum class RenderTextureLifetime : uint8_t { kTransient, kHistory };
/** Single-mip/layer, single-sample RGBA8; history holds latest successfully submitted contents. */
struct RenderTextureDeclaration final {
    RenderResourceId id{};
    uint64_t version{1};
    uint32_t width{0};
    uint32_t height{0};
    RenderTextureLifetime lifetime{RenderTextureLifetime::kTransient};
};
/** Clears the complete color subresource at mip 0 / layer 0. */
struct RenderClearColor final {
    RenderTextureReference target{};
    std::array<float, 4> color{0.f, 0.f, 0.f, 1.f};
};
/** Copies a region from the origin of mip 0 / layer 0, preserving the destination outside it. */
struct RenderCopyTexture final {
    RenderTextureReference source{};
    RenderTextureReference destination{};
    uint32_t width{0};
    uint32_t height{0};
};
/** Host scene slot identity/version; the host supplies a fresh immutable snapshot each frame. */
struct RenderSceneReference final {
    RenderResourceId id{};
    uint64_t version{1};
};
enum class RenderAttachmentLoad : uint8_t { kLoad, kClear };
/** External scene color attachment with explicit preservation or initialization. */
struct RenderColorAttachment final {
    RenderTextureReference target{};
    RenderAttachmentLoad load{RenderAttachmentLoad::kClear};
    std::array<float, 4> clear_color{};
};
/** Optional external D32Float attachment, with explicit depth initialization or preservation. */
struct RenderDepthAttachment final {
    RenderTextureReference target{};
    RenderAttachmentLoad load{RenderAttachmentLoad::kClear};
    float clear_depth{1.f};
};
/** Draws the host's fixed material ABI into external color and optional depth attachments. */
struct RenderDrawScene final {
    RenderSceneReference scene{};
    RenderTextureReference target{};
    RenderAttachmentLoad load{RenderAttachmentLoad::kClear};
    std::array<float, 4> clear_color{0.f, 0.f, 0.f, 1.f};
    std::optional<RenderDepthAttachment> depth{};
    /** Optional external D32 map fitted from this same scene's directional light and casters. */
    std::optional<RenderTextureReference> directional_shadow{};
};
enum class RenderDepthSceneProjection : uint8_t { kCamera, kDirectionalShadow };
/** Draws the fixed depth-only mesh ABI using the declared camera/light projection. */
struct RenderDrawDepthScene final {
    RenderSceneReference scene{};
    RenderDepthAttachment depth{};
    RenderDepthSceneProjection projection{RenderDepthSceneProjection::kCamera};
};
/** Fixed Albedo/Normal/Material MRT mesh ABI; native formats and resources belong to the engine. */
struct RenderDrawGBufferScene final {
    RenderSceneReference scene{};
    RenderColorAttachment albedo{};
    RenderColorAttachment normal{};
    RenderColorAttachment material{};
    RenderDepthAttachment depth{};
};
/** Fixed full-screen lighting ABI; geometry is consumed from the external GBuffer inputs. */
struct RenderDeferredLightingScene final {
    RenderSceneReference scene{};
    RenderTextureReference albedo{};
    RenderTextureReference normal{};
    RenderTextureReference depth{};
    RenderColorAttachment hdr{};
    std::optional<RenderTextureReference> directional_shadow{};
};
/** Author-owned values shared by scene defaults and explicit full-screen consumers. */
struct RenderPostProcessSettings final {
    std::array<float, 3> sky_zenith{0.08f, 0.2f, 0.5f};
    std::array<float, 3> sky_horizon{0.5f, 0.65f, 0.9f};
    std::array<float, 3> sky_ground{0.025f, 0.025f, 0.025f};
    float exposure{1.f};
    float gamma{1.f};
    bool sky_enabled{false};
    bool tone_mapping{false};
};
[[nodiscard]] foundation::Result validate_post_process_settings(const RenderPostProcessSettings& settings);
/** Consumes HDR/depth and writes LDR; absent settings use the immutable scene View defaults. */
struct RenderPostProcessScene final {
    RenderSceneReference scene{};
    RenderTextureReference hdr{};
    RenderTextureReference depth{};
    RenderColorAttachment output{};
    std::optional<RenderPostProcessSettings> settings{};
};
inline constexpr ValueSchemaId kRenderLightListSchema{"Render.LightList.v1"};
/** Logical value only; Runtime-delivered source evidence authorizes cross-owner use. */
struct RenderLightListReference final {
    RenderResourceId id{};
    uint64_t version{1};
    InvocationId producer_invocation{};
    RuntimeGeneration producer_generation{};
    [[nodiscard]] bool is_valid() const noexcept {
        return id.is_valid() && version != 0 && producer_invocation.is_valid() && producer_generation.is_valid();
    }
    friend bool operator==(const RenderLightListReference&, const RenderLightListReference&) = default;
};
[[nodiscard]] foundation::ResultValue<std::vector<uint8_t>> encode_render_light_list_reference(
    const RenderLightListReference& reference);
[[nodiscard]] foundation::ResultValue<RenderLightListReference> decode_render_light_list_reference(
    std::span<const uint8_t> payload);
struct RenderLightListDeclaration final {
    RenderResourceId id{};
    uint64_t version{1};
    /** Invalid means private to this Feature; otherwise a declared LightList output port. */
    PortId export_port{};
};
/** Independent Compute producer; camera depth and optional shadow fit belong to the same scene. */
struct RenderLightCullingScene final {
    RenderSceneReference scene{};
    RenderTextureReference camera_depth{};
    RenderLightListDeclaration output{};
    std::optional<RenderTextureReference> directional_shadow{};
};
/** Independent Forward consumer of one complete engine-owned light-list bundle. */
struct RenderDrawClusteredScene final {
    RenderSceneReference scene{};
    RenderLightListReference light_list{};
    RenderColorAttachment hdr{};
    /** Must preserve the camera depth used by its producer. */
    RenderDepthAttachment depth{};
    std::optional<RenderTextureReference> directional_shadow{};
};
using RenderOperation = std::variant<RenderClearColor, RenderCopyTexture, RenderDrawScene, RenderDrawDepthScene,
                                     RenderDrawGBufferScene, RenderDeferredLightingScene, RenderPostProcessScene,
                                     RenderLightCullingScene, RenderDrawClusteredScene>;

/** Owned domain intent, with no RHI handles, graph builders, author Pass names or callbacks. */
struct RenderIntent final {
    uint32_t schema_version{1};
    std::vector<RenderTextureDeclaration> textures{};
    std::vector<RenderOperation> operations{};
    std::optional<RenderTextureReference> final_output{};
    /** Explicit external D32 graph root; color final_output keeps its color role. */
    std::optional<RenderTextureReference> depth_output{};
};

/** Shared owner-commit and RenderGraph preflight validation; host bindings are checked by RenderGraph. */
[[nodiscard]] foundation::Result validate_render_intent(const RenderIntent& intent, const FeatureContract& contract);
}
