#include "render_feature_history.hpp"
#include "render_feature_history_access.hpp"
#include "render_texture_allocation.hpp"
#include <algorithm>
#include <unordered_map>

namespace aeg::render_graph {
namespace {
using namespace feature;
struct Key {
    RenderFeatureHistoryKey history;
    FeatureInstanceId instance;
    RenderResourceId resource;
    friend bool operator==(const Key&, const Key&) = default;
};
struct Hash {
    size_t operator()(const Key& key) const noexcept {
        return std::hash<uint64_t>{}(key.history.value) ^
            (StableIdHash<FeatureInstanceIdTag>{}(key.instance) + 0x9e3779b9u) ^
            (StableIdHash<RenderResourceIdTag>{}(key.resource) + 0x85ebca6bu);
    }
};
struct Entry {
    RenderResourceOwner owner;
    RenderTextureDeclaration declaration;
    std::shared_ptr<TextureAllocationOwner> allocation;
};
using Entries = std::unordered_map<Key, Entry, Hash>;
using Owners = std::unordered_map<FeatureInstanceId, RenderResourceOwner, StableIdHash<FeatureInstanceIdTag>>;
struct HistoryKeyHash {
    size_t operator()(RenderFeatureHistoryKey key) const noexcept { return std::hash<uint64_t>{}(key.value); }
};
using OwnerSets = std::unordered_map<RenderFeatureHistoryKey, Owners, HistoryKeyHash>;
rhi::TextureDesc history_description(const RenderTextureDeclaration& declaration) {
    rhi::TextureDesc desc{};
    desc.extent = {declaration.width, declaration.height, 1};
    desc.format = rhi::Format::kRgba8Unorm;
    desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kTransferDestination;
    return desc;
}
}
struct RenderFeatureHistory::State {
    explicit State(rhi::Device& value) : device(value) {}
    rhi::Device& device;
    uint64_t revision{1};
    OwnerSets owners;
    Entries entries;
};
struct RenderFeatureHistoryTransaction::Impl {
    std::weak_ptr<RenderFeatureHistory::State> state;
    uint64_t revision;
    RenderFeatureHistoryKey key{};
    OwnerSets owners;
    Entries entries;
    Entries declared;
    bool consumed{false};
};
RenderFeatureHistory::RenderFeatureHistory(rhi::Device& device) : state_(std::make_shared<State>(device)) {}
RenderFeatureHistory::~RenderFeatureHistory() { clear(); }
void RenderFeatureHistory::clear() noexcept {
    ++state_->revision;
    state_->owners.clear();
    state_->entries.clear();
}
size_t RenderFeatureHistory::size() const noexcept { return state_->entries.size(); }
bool RenderFeatureHistory::uses_device(const rhi::Device& device) const noexcept { return &state_->device == &device; }
foundation::Result RenderFeatureHistory::reconcile(std::span<const feature::RenderResourceOwner> owners) {
    return reconcile({}, owners);
}
foundation::Result RenderFeatureHistory::reconcile(RenderFeatureHistoryKey key,
    std::span<const feature::RenderResourceOwner> owners) {
    Owners next;
    try {
        for (const auto& owner : owners) {
            if (!owner.instance_id.is_valid() || !owner.node_id.is_valid() || owner.definition_digest == 0 ||
                owner.resource_epoch == 0 || !next.emplace(owner.instance_id, owner).second)
                return foundation::invalid_argument("history owner snapshot contains invalid or duplicate owners");
        }
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory, "history owner snapshot allocation failed");
    }
    const auto current = state_->owners.find(key);
    if (current != state_->owners.end() && next == current->second) return foundation::Result::success();
    if (next.empty()) {
        std::erase_if(state_->entries, [&](const auto& pair) { return pair.first.history == key; });
        if (current != state_->owners.end()) state_->owners.erase(current);
    } else {
        try {
            if (current != state_->owners.end()) {
                current->second.swap(next);
            } else {
                auto inserted = state_->owners.emplace(key, Owners{});
                inserted.first->second.swap(next);
            }
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                               "history owner scope allocation failed");
        }
        const auto& retained = state_->owners.find(key)->second;
        std::erase_if(state_->entries, [&](const auto& pair) {
            if (pair.first.history != key) return false;
            const auto found = retained.find(pair.first.instance);
            return found == retained.end() || found->second != pair.second.owner;
        });
    }
    ++state_->revision;
    return foundation::Result::success();
}
foundation::ResultValue<RenderFeatureHistoryTexture> RenderFeatureHistory::find_texture(
    feature::FeatureInstanceId instance, feature::RenderResourceId resource) const {
    return find_texture({}, instance, resource);
}
foundation::ResultValue<RenderFeatureHistoryTexture> RenderFeatureHistory::find_texture(
    RenderFeatureHistoryKey key, feature::FeatureInstanceId instance, feature::RenderResourceId resource) const {
    const auto found = state_->entries.find({key, instance, resource});
    if (found == state_->entries.end()) return {foundation::not_found("Feature history is not initialized"), {}};
    return {foundation::Result::success(), {found->second.allocation->texture, found->second.allocation}};
}
std::shared_ptr<RenderFeatureHistoryTransaction> RenderFeatureHistory::stage() {
    return stage({});
}
std::shared_ptr<RenderFeatureHistoryTransaction> RenderFeatureHistory::stage(RenderFeatureHistoryKey key) {
    auto impl = std::make_unique<RenderFeatureHistoryTransaction::Impl>();
    impl->state = state_;
    impl->revision = state_->revision;
    impl->key = key;
    impl->owners = state_->owners;
    impl->entries = state_->entries;
    return std::make_shared<RenderFeatureHistoryTransaction>(std::move(impl));
}
RenderFeatureHistoryTransaction::RenderFeatureHistoryTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RenderFeatureHistoryTransaction::~RenderFeatureHistoryTransaction() = default;
foundation::Result RenderFeatureHistoryTransaction::retain_declarations(const feature::RenderFeatureOutput& output) {
    const auto owners = impl_->owners.find(impl_->key);
    const auto found = owners == impl_->owners.end() ? Owners::const_iterator{} : owners->second.find(output.instance_id);
    if (owners == impl_->owners.end() || found == owners->second.end() || found->second != output.resource_owner ||
        output.resource_owner.node_id != output.node_id)
        return foundation::invalid_state("render output does not match its live history owner");
    std::erase_if(impl_->entries, [&](const auto& pair) {
        return pair.first.history == impl_->key && pair.first.instance == output.instance_id &&
            std::none_of(output.intent.textures.begin(), output.intent.textures.end(),
            [&](const auto& declaration) { return declaration.id == pair.first.resource && declaration.lifetime == RenderTextureLifetime::kHistory; });
    });
    return foundation::Result::success();
}
foundation::Result RenderFeatureHistoryTransaction::declare(const feature::RenderResourceOwner& owner,
    const feature::RenderTextureDeclaration& declaration) {
    const auto owners = impl_->owners.find(impl_->key);
    const auto known = owners == impl_->owners.end() ? Owners::const_iterator{} : owners->second.find(owner.instance_id);
    if (owners == impl_->owners.end() || known == owners->second.end() || known->second != owner)
        return foundation::invalid_state("history owner is missing or retired");
    const Key key{impl_->key, owner.instance_id, declaration.id};
    const auto existing = impl_->entries.find(key);
    if (existing != impl_->entries.end() && existing->second.declaration.version == declaration.version &&
        (existing->second.declaration.width != declaration.width || existing->second.declaration.height != declaration.height))
        return foundation::invalid_argument("history shape change requires a new resource version");
    impl_->declared.emplace(key, Entry{owner, declaration, {}});
    return foundation::Result::success();
}
foundation::ResultValue<RenderHistoryAllocation> RenderFeatureHistoryTransaction::read(
    const feature::RenderResourceOwner& owner, feature::RenderResourceId resource) const {
    const Key key{impl_->key, owner.instance_id, resource};
    const auto declaration = impl_->declared.find(key);
    const auto found = impl_->entries.find(key);
    if (declaration == impl_->declared.end() || found == impl_->entries.end() || found->second.owner != owner ||
        found->second.declaration.version != declaration->second.declaration.version)
        return {foundation::not_ready("Feature history has no initialized contents at this version"), {}};
    return {foundation::Result::success(), {found->second.allocation->texture, found->second.allocation,
                                          history_description(found->second.declaration)}};
}
foundation::ResultValue<RenderHistoryAllocation> RenderFeatureHistoryTransaction::write(
    const feature::RenderResourceOwner& owner, feature::RenderResourceId resource) {
    if (const auto result = prepare(); !result) return {result, {}};
    const auto state = impl_->state.lock();
    const Key key{impl_->key, owner.instance_id, resource};
    auto entry = impl_->declared.at(key);
    const auto desc = history_description(entry.declaration);
    auto allocation = std::make_shared<TextureAllocationOwner>(state->device);
    const auto created = state->device.create_texture(desc);
    if (!created) return {created.result, {}};
    allocation->texture = created.value;
    entry.allocation = allocation;
    impl_->entries.insert_or_assign(key, std::move(entry));
    return {foundation::Result::success(), {allocation->texture, allocation, desc}};
}
std::weak_ptr<const void> RenderFeatureHistoryTransaction::transaction_domain() const noexcept {
    return impl_->state;
}
foundation::Result RenderFeatureHistoryTransaction::prepare() const {
    const auto state = impl_->state.lock();
    if (impl_->consumed || !state || state->revision != impl_->revision)
        return foundation::invalid_state("Feature history transaction is stale or its store has retired");
    return foundation::Result::success();
}
void RenderFeatureHistoryTransaction::submitted() noexcept {
    impl_->consumed = true;
    const auto state = impl_->state.lock();
    if (!state || state->revision != impl_->revision) return;
    state->entries.swap(impl_->entries);
    ++state->revision;
}
void RenderFeatureHistoryTransaction::failed() noexcept { impl_->consumed = true; }

namespace detail {

bool RenderFeatureHistoryRuntimeAccess::uses_device(
    const RenderFeatureHistory& history, const rhi::Device& device) noexcept {
    return history.uses_device(device);
}

std::shared_ptr<RenderFeatureHistoryTransaction> RenderFeatureHistoryRuntimeAccess::stage(
    RenderFeatureHistory& history, RenderFeatureHistoryKey key) {
    return history.stage(key);
}

}  // namespace detail
}
