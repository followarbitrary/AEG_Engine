#include "feature_callback_codec.hpp"

#include <bit>
#include <new>
#include <type_traits>

#include "../abi/feature_module_api.h"
#include "serialization.hpp"

namespace aeg::module::detail {
namespace {
using namespace feature;
constexpr uint32_t kMagic = 0x31524641; // AFR1, little endian.
constexpr uint32_t kVersion = 2;
static_assert(kVersion == AEG_FEATURE_EXECUTION_API_VERSION);
struct CodecFailure { foundation::Result result; };
[[noreturn]] void invalid(const char* message) { throw CodecFailure{foundation::invalid_argument(message)}; }
void checked(foundation::Result result) { if (!result) throw CodecFailure{std::move(result)}; }
template <typename T> T checked(foundation::ResultValue<T> result) {
    checked(std::move(result.result)); return std::move(result.value);
}
template <typename T> struct IsOptional : std::false_type {};
template <typename T> struct IsOptional<std::optional<T>> : std::true_type {};
template <typename T> struct IsVector : std::false_type {};
template <typename T, typename A> struct IsVector<std::vector<T, A>> : std::true_type {};
template <typename T> struct IsVariant : std::false_type {};
template <typename... T> struct IsVariant<std::variant<T...>> : std::true_type {};
template <typename T> constexpr uint32_t enum_max() {
    if constexpr (std::is_same_v<T, EdgeKind>) return static_cast<uint32_t>(EdgeKind::kReceipt);
    else if constexpr (std::is_same_v<T, RenderTextureLifetime>) return static_cast<uint32_t>(RenderTextureLifetime::kHistory);
    else if constexpr (std::is_same_v<T, RenderAttachmentLoad>) return static_cast<uint32_t>(RenderAttachmentLoad::kClear);
    else if constexpr (std::is_same_v<T, RenderDepthSceneProjection>) return static_cast<uint32_t>(RenderDepthSceneProjection::kDirectionalShadow);
    else { static_assert(sizeof(T) == 0, "unknown Module wire enum"); }
}

// One field traversal serves both directions. Semantic contract checks remain
// in Feature/Runtime validators; these functions only define the fixed wire.
template <typename Archive, typename T> void fields(Archive& ar, T& v) {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<V, StateDelta>)
        ar(v.expected_version, v.payload_revision, v.next_state, v.process_completed,
           v.output_payload, v.output_messages, v.state_payload, v.render_intent, v.outbox);
    else if constexpr (std::is_same_v<V, OutboxIntent>) {
        ar(v.effect, v.version);
        if (!v.effect.is_valid() || v.version == 0) invalid("Feature callback outbox effect/version is invalid");
        ar.bounded_bytes(v.payload, kMaxOutboxIntentBytes);
    }
    else if constexpr (std::is_same_v<V, PortMessage>)
        ar(v.source_port, v.target_port, v.schema_version, v.sequence, v.causality_token,
           v.timestamp_ticks, v.payload, v.schema, v.kind, v.transaction_id, v.deadline_ticks);
    else if constexpr (std::is_same_v<V, RenderIntent>) {
        ar(v.schema_version);
        if (v.schema_version != 1) invalid("Feature callback render intent schema is unsupported");
        ar(v.textures, v.operations, v.final_output, v.depth_output);
    }
    else if constexpr (std::is_same_v<V, RenderTextureDeclaration>)
        ar(v.id, v.version, v.width, v.height, v.lifetime);
    else if constexpr (std::is_same_v<V, RenderTextureReference>) ar(v.id, v.version, v.external);
    else if constexpr (std::is_same_v<V, RenderSceneReference>) ar(v.id, v.version);
    else if constexpr (std::is_same_v<V, RenderColorAttachment>) ar(v.target, v.load, v.clear_color);
    else if constexpr (std::is_same_v<V, RenderDepthAttachment>) ar(v.target, v.load, v.clear_depth);
    else if constexpr (std::is_same_v<V, RenderClearColor>) ar(v.target, v.color);
    else if constexpr (std::is_same_v<V, RenderCopyTexture>) ar(v.source, v.destination, v.width, v.height);
    else if constexpr (std::is_same_v<V, RenderDrawScene>)
        ar(v.scene, v.target, v.load, v.clear_color, v.depth, v.directional_shadow);
    else if constexpr (std::is_same_v<V, RenderDrawDepthScene>) ar(v.scene, v.depth, v.projection);
    else if constexpr (std::is_same_v<V, RenderDrawGBufferScene>) ar(v.scene, v.albedo, v.normal, v.material, v.depth);
    else if constexpr (std::is_same_v<V, RenderDeferredLightingScene>)
        ar(v.scene, v.albedo, v.normal, v.depth, v.hdr, v.directional_shadow);
    else if constexpr (std::is_same_v<V, RenderPostProcessScene>) ar(v.scene, v.hdr, v.depth, v.output, v.settings);
    else if constexpr (std::is_same_v<V, RenderPostProcessSettings>)
        ar(v.sky_zenith, v.sky_horizon, v.sky_ground, v.exposure, v.gamma, v.sky_enabled, v.tone_mapping);
    else if constexpr (std::is_same_v<V, RenderLightListDeclaration>) ar(v.id, v.version, v.export_port);
    else if constexpr (std::is_same_v<V, RenderLightCullingScene>)
        ar(v.scene, v.camera_depth, v.output, v.directional_shadow);
    else if constexpr (std::is_same_v<V, RenderDrawClusteredScene>)
        ar(v.scene, v.light_list, v.hdr, v.depth, v.directional_shadow);
    else static_assert(sizeof(T) == 0, "unknown Module wire value");
}

class Writer final {
public:
    template <typename... T> void operator()(const T&... values) { (value(values), ...); }
    std::span<const uint8_t> data() const { return writer_.data(); }
    void bounded_bytes(const std::vector<uint8_t>& bytes, uint32_t limit) {
        if (bytes.size() > limit) invalid("Feature callback outbox payload is excessive");
        (*this)(bytes);
    }
private:
    void room(std::size_t bytes) const {
        if (bytes > AEG_FEATURE_ABI_MAX_BYTES - writer_.size()) invalid("Feature callback response exceeds16MiB");
    }
    template <typename T> void value(const T& v) {
        if constexpr (std::is_same_v<T, uint32_t>) { room(4); checked(writer_.write_u32(v)); }
        else if constexpr (std::is_same_v<T, uint64_t>) { room(8); checked(writer_.write_u64(v)); }
        else if constexpr (std::is_same_v<T, bool>) (*this)(uint32_t(v));
        else if constexpr (std::is_same_v<T, float>) (*this)(std::bit_cast<uint32_t>(v));
        else if constexpr (std::is_enum_v<T>) {
            const auto tag = static_cast<uint32_t>(v);
            if (tag > enum_max<T>()) invalid("Feature callback enum is unknown");
            (*this)(tag);
        } else if constexpr (std::is_same_v<T, RenderLightListReference>) {
            const auto bytes = checked(encode_render_light_list_reference(v));
            room(bytes.size()); checked(writer_.write_bytes(bytes));
        } else if constexpr (std::is_same_v<T, std::string>) {
            room(4); room(v.size() + 4); checked(writer_.write_string(v, AEG_FEATURE_ABI_MAX_BYTES));
        } else if constexpr (IsVector<T>::value) {
            if (v.size() > AEG_FEATURE_ABI_MAX_BYTES) invalid("Feature callback array is excessive");
            if constexpr (std::is_same_v<typename T::value_type, OutboxIntent>)
                if (v.size() > kMaxOutboxIntentsPerCommit) invalid("Feature callback outbox count is excessive");
            (*this)(static_cast<uint32_t>(v.size()));
            if constexpr (std::is_same_v<typename T::value_type, uint8_t>) {
                room(v.size()); checked(writer_.write_bytes(v));
            } else for (const auto& entry : v) (*this)(entry);
        } else if constexpr (IsOptional<T>::value) {
            (*this)(v.has_value()); if (v) (*this)(*v);
        } else if constexpr (IsVariant<T>::value) {
            (*this)(static_cast<uint32_t>(v.index())); std::visit([this](const auto& operation) { (*this)(operation); }, v);
        } else if constexpr (requires { std::tuple_size<T>::value; }) {
            for (const auto& entry : v) (*this)(entry);
        } else if constexpr (requires { v.value(); }) (*this)(v.value());
        else fields(*this, v);
    }
    serialization::BinaryWriter writer_{};
};

class Reader final {
public:
    explicit Reader(std::span<const uint8_t> bytes) : reader_(bytes) {}
    template <typename... T> void operator()(T&... values) { (value(values), ...); }
    void finish() const { if (reader_.remaining()) invalid("Feature callback response has trailing bytes"); }
    void bounded_bytes(std::vector<uint8_t>& bytes, uint32_t limit) {
        uint32_t count{}; (*this)(count);
        if (count > limit) invalid("Feature callback outbox payload is excessive");
        const auto source = checked(reader_.read_bytes(count));
        bytes.assign(source.begin(), source.end());
    }
private:
    template <typename Variant, std::size_t Index = 0> void operation(uint32_t tag, Variant& v) {
        if constexpr (Index < std::variant_size_v<Variant>) {
            if (tag == Index) { v.template emplace<Index>(); (*this)(std::get<Index>(v)); }
            else operation<Variant, Index + 1>(tag, v);
        } else invalid("Feature callback render operation is unknown");
    }
    template <typename T> void value(T& v) {
        if constexpr (std::is_same_v<T, uint32_t>) v = checked(reader_.read_u32());
        else if constexpr (std::is_same_v<T, uint64_t>) v = checked(reader_.read_u64());
        else if constexpr (std::is_same_v<T, bool>) {
            uint32_t flag{}; (*this)(flag); if (flag > 1) invalid("Feature callback bool/optional flag is invalid"); v = flag != 0;
        } else if constexpr (std::is_same_v<T, float>) {
            uint32_t bits{}; (*this)(bits); v = std::bit_cast<float>(bits);
        } else if constexpr (std::is_enum_v<T>) {
            uint32_t tag{}; (*this)(tag); if (tag > enum_max<T>()) invalid("Feature callback enum is unknown"); v = static_cast<T>(tag);
        } else if constexpr (std::is_same_v<T, RenderLightListReference>)
            v = checked(decode_render_light_list_reference(checked(reader_.read_bytes(32))));
        else if constexpr (std::is_same_v<T, std::string>) v = checked(reader_.read_string(AEG_FEATURE_ABI_MAX_BYTES));
        else if constexpr (IsVector<T>::value) {
            uint32_t count{}; (*this)(count);
            if constexpr (std::is_same_v<typename T::value_type, uint8_t>) {
                const auto bytes = checked(reader_.read_bytes(count)); v.assign(bytes.begin(), bytes.end());
            } else {
                if constexpr (std::is_same_v<typename T::value_type, OutboxIntent>) {
                    if (count > kMaxOutboxIntentsPerCommit || count > reader_.remaining() / 16)
                        invalid("Feature callback outbox count is truncated/excessive");
                } else if (count > reader_.remaining() / 4) invalid("Feature callback array count is truncated/excessive");
                // Do not reserve an author-supplied count before parsing entries.
                for (uint32_t n = 0; n < count; ++n) { typename T::value_type entry{}; (*this)(entry); v.push_back(std::move(entry)); }
            }
        } else if constexpr (IsOptional<T>::value) {
            bool present{}; (*this)(present); if (present) { v.emplace(); (*this)(*v); } else v.reset();
        } else if constexpr (IsVariant<T>::value) { uint32_t tag{}; (*this)(tag); operation(tag, v); }
        else if constexpr (requires { std::tuple_size<T>::value; }) for (auto& entry : v) (*this)(entry);
        else if constexpr (requires { v.value(); }) { uint64_t id{}; (*this)(id); v = T(id); }
        else fields(*this, v);
    }
    serialization::BinaryReader reader_;
};
} // namespace

foundation::ResultValue<std::vector<uint8_t>> encode_feature_callback_result(
    const foundation::ResultValue<StateDelta>& result) {
    try {
        Writer writer;
        const auto code = static_cast<uint32_t>(result.result.code);
        if (code > static_cast<uint32_t>(foundation::ResultCode::kCancelled)) invalid("Feature callback result code is unknown");
        writer(kMagic, kVersion, code, result.result.message);
        if (result) writer(result.value);
        return {foundation::Result::success(), {writer.data().begin(), writer.data().end()}};
    } catch (const CodecFailure& error) { return {error.result, {}}; }
      catch (const std::bad_alloc&) { return {foundation::out_of_memory("Feature callback encode allocation failed"), {}}; }
      catch (const std::exception& error) { return {foundation::unknown_exception(error), {}}; }
}
foundation::ResultValue<StateDelta> decode_feature_callback_result(std::span<const uint8_t> bytes) {
    if (bytes.size() > AEG_FEATURE_ABI_MAX_BYTES)
        return {foundation::invalid_argument("Feature callback response exceeds16MiB"), {}};
    try {
        Reader reader(bytes); uint32_t magic{}, version{}, code{}; std::string message;
        reader(magic, version, code, message);
        if (magic != kMagic || version != kVersion || code > static_cast<uint32_t>(foundation::ResultCode::kCancelled))
            invalid("Feature callback response header is invalid");
        StateDelta delta{};
        if (code == static_cast<uint32_t>(foundation::ResultCode::kSuccess)) reader(delta);
        reader.finish();
        return {{static_cast<foundation::ResultCode>(code), std::move(message)}, std::move(delta)};
    } catch (const CodecFailure& error) { return {error.result, {}}; }
      catch (const std::bad_alloc&) { return {foundation::out_of_memory("Feature callback decode allocation failed"), {}}; }
      catch (const std::exception& error) { return {foundation::unknown_exception(error), {}}; }
}
} // namespace aeg::module::detail
