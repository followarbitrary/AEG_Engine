#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

#include "../Engine/Module/private/feature_callback_codec.hpp"
#include "../Engine/Module/private/module_abi.hpp"
#include "../Engine/Module/abi/feature_module_api.h"

using namespace aeg;
using namespace aeg::feature;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void reference(const RenderTextureReference& actual, const RenderTextureReference& expected) {
    require(actual.id == expected.id && actual.version == expected.version && actual.external == expected.external,
            "texture reference fields changed");
}
void scene(const RenderSceneReference& actual, const RenderSceneReference& expected) {
    require(actual.id == expected.id && actual.version == expected.version, "scene reference fields changed");
}
void color(const RenderColorAttachment& actual, const RenderColorAttachment& expected) {
    reference(actual.target, expected.target);
    require(actual.load == expected.load && actual.clear_color == expected.clear_color, "color attachment fields changed");
}
void depth(const RenderDepthAttachment& actual, const RenderDepthAttachment& expected) {
    reference(actual.target, expected.target);
    require(actual.load == expected.load && actual.clear_depth == expected.clear_depth, "depth attachment fields changed");
}
void optional_reference(const std::optional<RenderTextureReference>& actual,
                        const std::optional<RenderTextureReference>& expected) {
    require(actual.has_value() == expected.has_value(), "optional texture presence changed");
    if (actual) reference(*actual, *expected);
}
void operation(const RenderOperation& actual, const RenderOperation& expected) {
    require(actual.index() == expected.index(), "operation tag changed");
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        const auto& other = std::get<T>(expected);
        if constexpr (std::is_same_v<T, RenderClearColor>) {
            reference(value.target, other.target); require(value.color == other.color, "clear values changed");
        } else if constexpr (std::is_same_v<T, RenderCopyTexture>) {
            reference(value.source, other.source); reference(value.destination, other.destination);
            require(value.width == other.width && value.height == other.height, "copy extent changed");
        } else if constexpr (std::is_same_v<T, RenderDrawScene>) {
            scene(value.scene, other.scene); reference(value.target, other.target);
            require(value.load == other.load && value.clear_color == other.clear_color &&
                    value.depth.has_value() == other.depth.has_value(), "scene attachment fields changed");
            if (value.depth) depth(*value.depth, *other.depth);
            optional_reference(value.directional_shadow, other.directional_shadow);
        } else if constexpr (std::is_same_v<T, RenderDrawDepthScene>) {
            scene(value.scene, other.scene); depth(value.depth, other.depth);
            require(value.projection == other.projection, "depth projection changed");
        } else if constexpr (std::is_same_v<T, RenderDrawGBufferScene>) {
            scene(value.scene, other.scene); color(value.albedo, other.albedo); color(value.normal, other.normal);
            color(value.material, other.material); depth(value.depth, other.depth);
        } else if constexpr (std::is_same_v<T, RenderDeferredLightingScene>) {
            scene(value.scene, other.scene); reference(value.albedo, other.albedo); reference(value.normal, other.normal);
            reference(value.depth, other.depth); color(value.hdr, other.hdr);
            optional_reference(value.directional_shadow, other.directional_shadow);
        } else if constexpr (std::is_same_v<T, RenderPostProcessScene>) {
            scene(value.scene, other.scene); reference(value.hdr, other.hdr); reference(value.depth, other.depth);
            color(value.output, other.output);
            require(value.settings.has_value() == other.settings.has_value(), "postprocess presence changed");
            if (value.settings) {
                const auto& a = *value.settings; const auto& b = *other.settings;
                require(a.sky_zenith == b.sky_zenith && a.sky_horizon == b.sky_horizon && a.sky_ground == b.sky_ground &&
                        a.exposure == b.exposure && a.gamma == b.gamma && a.sky_enabled == b.sky_enabled &&
                        a.tone_mapping == b.tone_mapping, "postprocess values changed");
            }
        } else if constexpr (std::is_same_v<T, RenderLightCullingScene>) {
            scene(value.scene, other.scene); reference(value.camera_depth, other.camera_depth);
            require(value.output.id == other.output.id && value.output.version == other.output.version &&
                    value.output.export_port == other.output.export_port, "light declaration changed");
            optional_reference(value.directional_shadow, other.directional_shadow);
        } else if constexpr (std::is_same_v<T, RenderDrawClusteredScene>) {
            scene(value.scene, other.scene); require(value.light_list == other.light_list, "light provenance changed");
            color(value.hdr, other.hdr); depth(value.depth, other.depth);
            optional_reference(value.directional_shadow, other.directional_shadow);
        }
    }, actual);
}
foundation::ResultValue<StateDelta> success(StateDelta delta = {}) {
    return {foundation::Result::success(), std::move(delta)};
}
std::vector<uint8_t> encode(const foundation::ResultValue<StateDelta>& result) {
    const auto bytes = module::detail::encode_feature_callback_result(result);
    require(bool(bytes), "response encoding failed"); return bytes.value;
}
void patch(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.at(offset + n) = static_cast<uint8_t>(value >> (n * 8));
}
void full_response() {
    StateDelta delta{}; delta.expected_version = StateVersion(0x123456789abcdef0ull);
    delta.payload_revision = 0xfedcba9876543210ull; delta.next_state = std::string("next\0state", 10);
    delta.process_completed = true; delta.output_payload = {0, 7, 255}; delta.state_payload = {{9, 0, 8}};
    PortMessage message{};
    message.source_port = PortId(101); message.target_port = PortId(102); message.schema_version = 19;
    message.sequence = 103; message.causality_token = 104; message.timestamp_ticks = 105;
    message.payload = {0, 9, 255}; message.schema = ValueSchemaId(106); message.kind = EdgeKind::kReceipt;
    message.transaction_id = 107; message.deadline_ticks = 23;
    message.delivery_id = MessageDeliveryId(900); message.source.instance = FeatureInstanceId(901);
    auto private_marker = std::make_shared<int>(7);
    message.render_source = {private_marker, reinterpret_cast<const detail::RenderLightListEvidence*>(private_marker.get())};
    delta.output_messages = {message};
    const RenderTextureReference a{RenderResourceId(11), 21, false}, b{RenderResourceId(12), 22, true};
    const RenderTextureReference c{RenderResourceId(13), 23, true}, shadow{RenderResourceId(14), 24, true};
    const RenderSceneReference view{RenderResourceId(15), 25};
    const RenderColorAttachment ca{a, RenderAttachmentLoad::kLoad, {0.1f, 0.2f, 0.3f, 0.4f}};
    const RenderColorAttachment cb{b, RenderAttachmentLoad::kClear, {0.5f, 0.6f, 0.7f, 0.8f}};
    const RenderDepthAttachment da{c, RenderAttachmentLoad::kLoad, .25f};
    RenderPostProcessSettings settings{{.1f,.2f,.3f},{.4f,.5f,.6f},{.7f,.8f,.9f},2.f,1.5f,true,true};
    RenderIntent intent{}; intent.textures = {{a.id, a.version, 111, 222, RenderTextureLifetime::kHistory}};
    intent.operations = {RenderClearColor{a, {1.f, 2.f, 3.f, 4.f}}, RenderCopyTexture{a, b, 123, 456},
        RenderDrawScene{view, b, cb.load, cb.clear_color, da, shadow},
        RenderDrawDepthScene{view, da, RenderDepthSceneProjection::kDirectionalShadow},
        RenderDrawGBufferScene{view, ca, cb, ca, da}, RenderDeferredLightingScene{view, a, b, c, cb, shadow},
        RenderPostProcessScene{view, a, c, cb, settings},
        RenderLightCullingScene{view, c, {RenderResourceId(31), 32, PortId(33)}, shadow},
        RenderDrawClusteredScene{view, {RenderResourceId(34),35,InvocationId(36),RuntimeGeneration(37)}, ca, da, shadow}};
    intent.final_output = b; intent.depth_output = c; delta.render_intent = intent;
    const auto bytes = encode(success(delta)); const auto decoded = module::detail::decode_feature_callback_result(bytes);
    require(bool(decoded), "full response decoding failed"); const auto& actual = decoded.value;
    require(actual.expected_version == delta.expected_version && actual.payload_revision == delta.payload_revision &&
            actual.next_state == delta.next_state && actual.process_completed && actual.output_payload == delta.output_payload &&
            actual.state_payload == delta.state_payload, "delta/state payload fields changed");
    require(actual.output_messages.size() == 1, "message count changed"); const auto& m = actual.output_messages.front();
    require(m.source_port == message.source_port && m.target_port == message.target_port && m.schema_version == message.schema_version &&
            m.sequence == message.sequence && m.causality_token == message.causality_token && m.timestamp_ticks == message.timestamp_ticks &&
            m.payload == message.payload && m.schema == message.schema && m.kind == message.kind &&
            m.transaction_id == message.transaction_id && m.deadline_ticks == message.deadline_ticks,
            "authored message fields changed");
    require(!m.delivery_id.is_valid() && m.source == MessageSource{} && m.render_source.use_count() == 0,
            "author wire promoted host-only delivery/source evidence");
    require(actual.render_intent && actual.render_intent->schema_version == 1 && actual.render_intent->textures.size() == 1 &&
            actual.render_intent->operations.size() == 9, "intent envelope changed");
    const auto& texture = actual.render_intent->textures.front();
    require(texture.id == a.id && texture.version == a.version && texture.width == 111 && texture.height == 222 &&
            texture.lifetime == RenderTextureLifetime::kHistory, "texture declaration changed");
    for (std::size_t n = 0; n < 9; ++n) operation(actual.render_intent->operations[n], intent.operations[n]);
    optional_reference(actual.render_intent->final_output, intent.final_output);
    optional_reference(actual.render_intent->depth_output, intent.depth_output);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        require(!module::detail::decode_feature_callback_result(std::span(bytes).first(n)), "truncated response accepted");
}
void optional_and_error() {
    for (unsigned variant = 0; variant < 3; ++variant) {
        StateDelta delta{};
        if (variant) { delta.state_payload.emplace(); delta.render_intent.emplace(); }
        if (variant == 2) {
            RenderPostProcessScene post{}; post.settings.reset();
            RenderDrawClusteredScene clustered{};
            clustered.light_list = {RenderResourceId(1),2,InvocationId(3),RuntimeGeneration(4)};
            delta.render_intent->operations = {RenderDrawScene{}, RenderDeferredLightingScene{}, post,
                RenderLightCullingScene{}, clustered};
        }
        const auto result = module::detail::decode_feature_callback_result(encode(success(delta)));
        require(bool(result) && result.value.state_payload.has_value() == (variant != 0) &&
                result.value.render_intent.has_value() == (variant != 0), "absent/engaged-empty intent or state changed");
        if (variant) require(result.value.state_payload->empty(), "explicit empty state became payload");
        if (variant == 2) for (std::size_t n = 0; n < 5; ++n)
            operation(result.value.render_intent->operations[n], delta.render_intent->operations[n]);
    }
    const auto failure = foundation::Result::failure(foundation::ResultCode::kNotReady, std::string("wait\0detail", 11));
    StateDelta ignored{}; ignored.state_payload = {{42}};
    const auto decoded = module::detail::decode_feature_callback_result(encode({failure, ignored}));
    require(!decoded && decoded.result.code == failure.code && decoded.result.message == failure.message &&
            !decoded.value.state_payload, "callback failure/diagnostic/payload semantics changed");
}
void malformed() {
    const auto empty = encode(success());
    for (const auto [offset, value] : std::array<std::pair<std::size_t,uint32_t>,8>{{
             {0,0}, {4,0}, {8,14}, {36,2}, {40,0xffffffffu}, {44,0xffffffffu}, {48,2}, {52,2}}}) {
        auto bytes = empty; patch(bytes, offset, value);
        require(!module::detail::decode_feature_callback_result(bytes), "invalid response tag/count/optional accepted");
    }
    auto trailing = empty; trailing.push_back(0);
    require(!module::detail::decode_feature_callback_result(trailing), "trailing response bytes accepted");
    StateDelta delta{}; delta.render_intent.emplace();
    delta.render_intent->operations = {RenderClearColor{{RenderResourceId(1),2,true},{1,2,3,4}}};
    auto unknown_operation = encode(success(delta)); patch(unknown_operation, 68, 9);
    require(!module::detail::decode_feature_callback_result(unknown_operation), "unknown render operation accepted");
    auto unknown_schema = encode(success(delta)); patch(unknown_schema, 56, 2);
    require(!module::detail::decode_feature_callback_result(unknown_schema), "unknown intent wire schema accepted");
    std::vector<uint8_t> excessive(16 * 1024 * 1024 + 1);
    require(!module::detail::decode_feature_callback_result(excessive), "excessive wire packet accepted");
    delta.output_payload = std::move(excessive);
    require(!module::detail::encode_feature_callback_result(success(std::move(delta))), "excessive callback response encoded");
}
uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0;
    for (unsigned n = 0; n < 4; ++n) value |= uint32_t(bytes.at(offset + n)) << (n * 8);
    return value;
}
bool outbox_response() {
    StateDelta delta{};
    const std::vector<OutboxIntent> expected{
        {SideEffectId(0x123456789abcdef0ull), 7, {0, 9, 255, 0}},
        {SideEffectId((std::numeric_limits<uint64_t>::max)()), (std::numeric_limits<uint32_t>::max)(), {42, 0, 17}}};
    delta.outbox = expected;
    auto bytes = encode(success(delta));
    delta.outbox.front().payload.assign(1, 77);
    const auto decoded = module::detail::decode_feature_callback_result(bytes);
    std::cout << "outbox_wire_version=" << read_u32(bytes, 4) << " decoded="
              << (decoded ? decoded.value.outbox.size() : 0) << " expected=" << expected.size()
              << " bytes=" << bytes.size() << '\n';
    if (!decoded || decoded.value.outbox.size() != expected.size()) return false;
    std::fill(bytes.begin(), bytes.end(), 0);
    for (size_t n = 0; n < expected.size(); ++n) {
        const auto& actual = decoded.value.outbox[n];
        require(actual.effect == expected[n].effect && actual.version == expected[n].version &&
            actual.payload == expected[n].payload, "outbox values/order or owned response bytes changed");
    }
    const auto empty = module::detail::decode_feature_callback_result(encode(success()));
    require(empty && empty.value.outbox.empty(), "empty outbox did not round-trip");
    StateDelta ignored{}; ignored.outbox = expected;
    const auto failure = foundation::Result::failure(foundation::ResultCode::kNotReady, "outbox callback failed");
    const auto rejected = module::detail::decode_feature_callback_result(encode({failure, std::move(ignored)}));
    require(!rejected && rejected.result.code == failure.code && rejected.value.outbox.empty(),
        "failed callback exported an outbox intent");
    return true;
}
void outbox_limits_and_versions() {
    constexpr uint32_t kCurrentWireVersion = 2;
    const auto empty = encode(success());
    require(read_u32(empty, 4) == kCurrentWireVersion, "outbox wire was not versioned");
    // The final count is the only addition to the old empty successful response.
    auto legacy = empty;
    require(legacy.size() >= 4 && read_u32(legacy, legacy.size() - 4) == 0, "empty outbox count is invalid");
    legacy.resize(legacy.size() - 4); patch(legacy, 4, 1);
    require(!module::detail::decode_feature_callback_result(legacy), "legacy callback wire was accepted");
    auto future = empty; patch(future, 4, kCurrentWireVersion + 1);
    require(!module::detail::decode_feature_callback_result(future), "future callback wire was accepted");

    StateDelta one{}; one.outbox = {{SideEffectId(1), 1, {}}};
    const auto encoded = encode(success(one));
    const size_t entry = empty.size();
    require(encoded.size() == entry + 16, "outbox entry wire shape changed");
    auto invalid_effect = encoded; patch(invalid_effect, entry, 0); patch(invalid_effect, entry + 4, 0);
    require(!module::detail::decode_feature_callback_result(invalid_effect), "zero outbox effect decoded");
    auto invalid_version = encoded; patch(invalid_version, entry + 8, 0);
    require(!module::detail::decode_feature_callback_result(invalid_version), "zero outbox version decoded");
    one.outbox[0].effect = {};
    require(!module::detail::encode_feature_callback_result(success(one)), "zero outbox effect encoded");
    one.outbox[0].effect = SideEffectId(1); one.outbox[0].version = 0;
    require(!module::detail::encode_feature_callback_result(success(one)), "zero outbox version encoded");
    one.outbox[0].version = 1;
    one.outbox[0].payload.assign(kMaxOutboxIntentBytes, 0xA5);
    auto maximum_payload = module::detail::decode_feature_callback_result(encode(success(one)));
    require(maximum_payload && maximum_payload.value.outbox.size() == 1 &&
        maximum_payload.value.outbox[0].payload == one.outbox[0].payload, "maximum outbox payload was rejected or changed");
    one.outbox[0].payload.push_back(0);
    require(!module::detail::encode_feature_callback_result(success(one)), "excessive outbox payload encoded");
    auto excessive_payload = encoded;
    patch(excessive_payload, entry + 12, kMaxOutboxIntentBytes + 1);
    excessive_payload.resize(excessive_payload.size() + kMaxOutboxIntentBytes + 1);
    require(!module::detail::decode_feature_callback_result(excessive_payload), "complete excessive outbox payload decoded");

    StateDelta maximum_count{};
    for (uint32_t n = 0; n < kMaxOutboxIntentsPerCommit; ++n)
        maximum_count.outbox.push_back({SideEffectId(uint64_t(n) + 1), n + 1, {}});
    const auto maximum_bytes = encode(success(maximum_count));
    const auto maximum = module::detail::decode_feature_callback_result(maximum_bytes);
    require(maximum && maximum.value.outbox.size() == kMaxOutboxIntentsPerCommit,
        "maximum outbox count was rejected or changed");
    for (size_t n = 0; n < maximum_count.outbox.size(); ++n)
        require(maximum.value.outbox[n].effect == maximum_count.outbox[n].effect &&
            maximum.value.outbox[n].version == maximum_count.outbox[n].version,
            "maximum-count outbox ordering changed");
    maximum_count.outbox.push_back({SideEffectId(1), 1, {}});
    require(!module::detail::encode_feature_callback_result(success(maximum_count)), "excessive outbox count encoded");
    auto excessive_count = maximum_bytes;
    patch(excessive_count, entry - 4, kMaxOutboxIntentsPerCommit + 1);
    excessive_count.insert(excessive_count.end(), encoded.begin() + entry, encoded.end());
    require(!module::detail::decode_feature_callback_result(excessive_count), "complete excessive outbox count decoded");

    StateDelta excessive_total{};
    excessive_total.outbox.assign(AEG_FEATURE_ABI_MAX_BYTES / kMaxOutboxIntentBytes,
        {SideEffectId(1), 1, std::vector<uint8_t>(kMaxOutboxIntentBytes)});
    require(!module::detail::encode_feature_callback_result(success(std::move(excessive_total))),
        "aggregate outbox response exceeded the existing packet byte limit");
}
void execution_versions() {
    // These table callbacks are never executed; only the real host ABI validator negotiates them.
    unsigned endpoint_calls = 0;
    const auto write = +[](void* data, AegAbiContext, AegAbiByteSink) -> AegAbiResult {
        ++*static_cast<unsigned*>(data); return {sizeof(AegAbiResult), AEG_ABI_UNSUPPORTED, {nullptr, 0}};
    };
    const auto describe = +[](void* data, AegAbiContext, uint32_t, AegFeatureCallbackInfo*) -> AegAbiResult {
        ++*static_cast<unsigned*>(data); return {sizeof(AegAbiResult), AEG_ABI_UNSUPPORTED, {nullptr, 0}};
    };
    const auto execute = +[](void* data, AegAbiContext, uint64_t, const AegFeatureInvocation*, AegAbiByteSink) -> AegAbiResult {
        ++*static_cast<unsigned*>(data); return {sizeof(AegAbiResult), AEG_ABI_UNSUPPORTED, {nullptr, 0}};
    };
    const auto validate = +[](void* data, AegAbiContext, uint64_t, AegAbiBytes, AegAbiByteSink) -> AegAbiResult {
        ++*static_cast<unsigned*>(data); return {sizeof(AegAbiResult), AEG_ABI_UNSUPPORTED, {nullptr, 0}};
    };
    AegFeatureExecutionApi table{sizeof(AegFeatureExecutionApi), 2, &endpoint_calls, {}, 0, 0,
        write, describe, execute, validate};
    AegAbiModuleApi api{sizeof(AegAbiModuleApi), AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, 0, AEG_CAP_FEATURES,
        nullptr, &table, nullptr};
    require(AEG_FEATURE_EXECUTION_API_VERSION == 2 && module::validate_module_api(api, AEG_CAP_ALL),
        "current outbox execution contract was not negotiated");
    table.version = 1;
    require(!module::validate_module_api(api, AEG_CAP_ALL), "legacy Feature execution contract was accepted");
    table.version = 3;
    require(!module::validate_module_api(api, AEG_CAP_ALL), "future Feature execution contract was accepted");
    require(endpoint_calls == 0, "execution contract validation entered a callback");
}
}
int main() {
    try {
        full_response(); optional_and_error(); malformed();
        if (!outbox_response()) return 73;
        outbox_limits_and_versions(); execution_versions();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Feature callback codec smoke passed\n";
}
