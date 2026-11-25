#include "sfx/application/node_pool.hpp"
#include "sfx/application/phase3a_storage.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace sfx::application {

bool operator==(const NodeAssignmentIdentity& left, const NodeAssignmentIdentity& right) noexcept {
    return left.runtime_epoch == right.runtime_epoch && left.node == right.node &&
        left.generation == right.generation;
}

struct NodePoolState {
    struct Slot {
        std::uint64_t generation{0};
        bool enabled{false};
        bool occupied{false};
        bool quarantined{false};
        std::string source;
        std::optional<CancellationSource> cancellation;
    };
    std::mutex mutex;
    const Identifier128 epoch;
    const std::uint16_t capacity;
    const std::uint16_t source_limit;
    const std::vector<SourceConnectionLimit> source_exceptions;
    const std::uint64_t generation_limit;
    bool stopped{false};
    std::array<Slot, kAlphaNodePoolCapacity> slots;

    NodePoolState(Identifier128 epoch_value, std::uint16_t count,
        std::uint16_t source_count, std::uint64_t limit, std::vector<SourceConnectionLimit> exceptions)
        : epoch(epoch_value), capacity(count), source_limit(source_count), source_exceptions(std::move(exceptions)), generation_limit(limit) {
        for (std::uint16_t i = 0; i < capacity; ++i) slots[i].enabled = true;
    }
    std::uint16_t limit_for(std::string_view source) const {
        for (const auto& item : source_exceptions) if (item.address == source) return item.maximum_connections;
        return source_limit;
    }
    Slot* matching(NodeAssignmentIdentity id) noexcept {
        if (id.runtime_epoch != epoch || !id.node || id.node > capacity || !id.generation) return nullptr;
        auto& slot = slots[id.node - 1U];
        return slot.occupied && slot.generation == id.generation ? &slot : nullptr;
    }
};

NodeCallbackGate::NodeCallbackGate(std::weak_ptr<NodePoolState> state, NodeAssignmentIdentity id)
    : state_(std::move(state)), identity_(id) {}

bool NodeCallbackGate::invoke(const std::function<void()>& publish) const {
    const auto state = state_.lock();
    if (!state || !publish) return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto* slot = state->matching(identity_);
    if (state->stopped || !slot || slot->quarantined || slot->cancellation->token().is_cancelled()) return false;
    publish();
    return true;
}

NodeLease::NodeLease(std::shared_ptr<NodePoolState> state, NodeAssignmentIdentity id)
    : state_(std::move(state)), identity_(id) {}
NodeLease::~NodeLease() { abandon(); }
NodeLease::NodeLease(NodeLease&& other) noexcept
    : state_(std::move(other.state_)), identity_(std::exchange(other.identity_, {})) {}
NodeLease& NodeLease::operator=(NodeLease&& other) noexcept {
    if (this != &other) {
        abandon();
        state_ = std::move(other.state_);
        identity_ = std::exchange(other.identity_, {});
    }
    return *this;
}
void NodeLease::abandon() noexcept {
    if (!state_) return;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (auto* slot = state_->matching(identity_)) {
            slot->cancellation->cancel();
            slot->quarantined = true;
        }
    }
    state_.reset();
    identity_ = {};
}
std::optional<CancellationToken> NodeLease::cancellation() const {
    if (!state_) return std::nullopt;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto* slot = state_->matching(identity_);
    return slot ? std::optional<CancellationToken>(slot->cancellation->token()) : std::nullopt;
}
NodeCallbackGate NodeLease::callbacks() const { return NodeCallbackGate(state_, identity_); }
bool NodeLease::release_after_cleanup() {
    if (!state_) return false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto* slot = state_->matching(identity_);
        if (!slot || slot->quarantined) return false;
        slot->cancellation->cancel(); // Retained tokens remain cancelled forever.
        slot->cancellation.reset();
        slot->source.clear();
        slot->occupied = false;
    }
    state_.reset();
    identity_ = {};
    return true;
}

FourNodePool::FourNodePool(Identifier128 epoch, std::uint16_t capacity,
    std::uint16_t source_limit, std::uint64_t generation_limit,
    std::array<std::uint64_t, kAlphaNodePoolCapacity> previous_generations,
    std::vector<SourceConnectionLimit> source_exceptions) {
    if (!epoch.valid() || !capacity || capacity > kAlphaNodePoolCapacity ||
        !source_limit || source_limit > 128 || !generation_limit || !valid_source_connection_limits(source_exceptions))
        throw std::invalid_argument("invalid node pool limits/epoch");
    // A lower generation ceiling allows deterministic exhaustion qualification;
    // it can only retire slots earlier, never wrap or reuse an identity.
    state_ = std::make_shared<NodePoolState>(epoch, capacity, source_limit, generation_limit,std::move(source_exceptions));
    for (std::size_t i = 0; i < previous_generations.size(); ++i) {
        if (previous_generations[i] > generation_limit)
            throw std::invalid_argument("node generation seed exceeds limit");
        state_->slots[i].generation = previous_generations[i];
    }
}
FourNodePool::~FourNodePool() { stop(); }

NodeAllocation FourNodePool::allocate(std::string_view source,bool bypass_source_limit) {
    auto canonical = canonicalize_source_ip(source);
    if (!canonical) return {NodeAllocationCode::invalid_source, std::nullopt};
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped) return {NodeAllocationCode::stopped, std::nullopt};
    std::uint16_t source_count = 0;
    for (const auto& slot : state_->slots) {
        if (slot.occupied && slot.source == canonical->text) ++source_count;
    }
    if (!bypass_source_limit && source_count >= state_->limit_for(canonical->text)) return {NodeAllocationCode::source_limit, std::nullopt};
    for (std::uint16_t i = 0; i < state_->capacity; ++i) {
        auto& slot = state_->slots[i];
        if (!slot.enabled || slot.occupied || slot.quarantined) continue;
        if (slot.generation == state_->generation_limit) {
            slot.quarantined = true;
            continue;
        }
        // All allocating operations precede the atomic state transition. A
        // bad_alloc leaves this slot, its generation and source ledger intact.
        CancellationSource cancellation;
        NodeLease lease(state_, {state_->epoch, static_cast<std::uint16_t>(i + 1U), slot.generation + 1U});
        slot.source = std::move(canonical->text);
        slot.cancellation = std::move(cancellation);
        ++slot.generation;
        slot.occupied = true;
        return {NodeAllocationCode::allocated, std::move(lease)};
    }
    return {NodeAllocationCode::full, std::nullopt};
}
NodeAllocation FourNodePool::allocate_exact(const std::uint16_t node,
    std::string_view source, bool bypass_source_limit) {
    if (node == 0 || node > state_->capacity)
        return {NodeAllocationCode::invalid_node, std::nullopt};
    auto canonical = canonicalize_source_ip(source);
    if (!canonical) return {NodeAllocationCode::invalid_source, std::nullopt};
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopped) return {NodeAllocationCode::stopped, std::nullopt};
    std::uint16_t source_count = 0;
    for (const auto& slot : state_->slots) {
        if (slot.occupied && slot.source == canonical->text) ++source_count;
    }
    if (!bypass_source_limit && source_count >= state_->limit_for(canonical->text))
        return {NodeAllocationCode::source_limit, std::nullopt};
    auto& slot = state_->slots[node - 1U];
    if (!slot.enabled || slot.occupied || slot.quarantined)
        return {NodeAllocationCode::full, std::nullopt};
    if (slot.generation == state_->generation_limit) {
        slot.quarantined = true;
        return {NodeAllocationCode::full, std::nullopt};
    }
    CancellationSource cancellation;
    NodeLease lease(state_, {state_->epoch, node, slot.generation + 1U});
    slot.source = std::move(canonical->text);
    slot.cancellation = std::move(cancellation);
    ++slot.generation;
    slot.occupied = true;
    return {NodeAllocationCode::allocated, std::move(lease)};
}
bool FourNodePool::cancel(NodeAssignmentIdentity id) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto* slot = state_->matching(id);
    if (!slot || slot->quarantined) return false;
    slot->cancellation->cancel();
    return true;
}
bool FourNodePool::set_enabled(std::uint16_t node, bool enabled) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!node || node > state_->capacity || state_->stopped) return false;
    auto& slot = state_->slots[node - 1U];
    if (slot.quarantined) return false;
    slot.enabled = enabled;
    if (!enabled && slot.cancellation) slot.cancellation->cancel();
    return true;
}
void FourNodePool::stop() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopped = true;
    for (auto& slot : state_->slots) if (slot.cancellation) slot.cancellation->cancel();
}
NodePoolSnapshot FourNodePool::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    NodePoolSnapshot result;
    result.stopped = state_->stopped;
    for (std::uint16_t i = 0; i < kAlphaNodePoolCapacity; ++i) {
        const auto& slot = state_->slots[i];
        result.slots[i] = {{state_->epoch, static_cast<std::uint16_t>(i + 1U), slot.generation},
            slot.enabled, slot.occupied, slot.cancellation && slot.cancellation->token().is_cancelled(),
            slot.quarantined, slot.source};
        if (slot.occupied) ++result.active;
    }
    return result;
}

} // namespace sfx::application
