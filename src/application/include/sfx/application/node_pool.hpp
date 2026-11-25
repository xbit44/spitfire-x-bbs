#pragma once

#include "sfx/application/account_storage.hpp"
#include "sfx/application/cancellation.hpp"
#include "sfx/application/authentication_security.hpp"

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace sfx::application {

inline constexpr std::uint16_t kAlphaNodePoolCapacity = 128;
inline constexpr std::uint16_t kDefaultLogicalNodes = 4;
using NodeEnablement = std::array<bool, kAlphaNodePoolCapacity>;
inline NodeEnablement default_node_enablement() {
    NodeEnablement enabled{};
    enabled.fill(true);
    return enabled;
}

struct NodeAssignmentIdentity {
    Identifier128 runtime_epoch;
    std::uint16_t node{0};
    std::uint64_t generation{0};
    bool valid() const noexcept { return runtime_epoch.valid() && node != 0 && generation != 0; }
    friend bool operator==(const NodeAssignmentIdentity& left,
        const NodeAssignmentIdentity& right) noexcept;
};

struct NodePoolState;

// An observer capability, not an owning lease. Validation and the bounded
// publication execute under the SAME lock as release/reassignment. Callbacks
// must not block, call pool methods, or destroy a lease/pool. Captured external
// objects still require their own lifetime ownership; this is not a thread join.
class NodeCallbackGate final {
public:
    bool invoke(const std::function<void()>& publish) const;
private:
    NodeCallbackGate(std::weak_ptr<NodePoolState> state, NodeAssignmentIdentity identity);
    std::weak_ptr<NodePoolState> state_;
    NodeAssignmentIdentity identity_;
    friend class NodeLease;
};

// Single-owner, move-only reservation. The owner must close/join/erase all
// connection resources BEFORE release_after_cleanup(). Dropping an unreleased
// lease cancels and quarantines its slot; it never falsely publishes FREE.
// Different leases may be used concurrently; do not concurrently move/release
// the same lease object. Cancellation and observer capabilities are shareable.
class NodeLease final {
public:
    ~NodeLease();
    NodeLease(NodeLease&& other) noexcept;
    NodeLease& operator=(NodeLease&& other) noexcept;
    NodeLease(const NodeLease&) = delete;
    NodeLease& operator=(const NodeLease&) = delete;
    NodeAssignmentIdentity identity() const noexcept { return identity_; }
    std::optional<CancellationToken> cancellation() const;
    NodeCallbackGate callbacks() const;
    bool release_after_cleanup();
private:
    NodeLease(std::shared_ptr<NodePoolState> state, NodeAssignmentIdentity identity);
    void abandon() noexcept;
    std::shared_ptr<NodePoolState> state_;
    NodeAssignmentIdentity identity_;
    friend class FourNodePool;
};

enum class NodeAllocationCode {
    allocated,
    invalid_source,
    invalid_node,
    source_limit,
    full,
    stopped,
};
struct NodeAllocation {
    NodeAllocationCode code{NodeAllocationCode::full};
    std::optional<NodeLease> lease;
};

struct NodeSlotSnapshot {
    NodeAssignmentIdentity identity;
    bool enabled{false};
    bool occupied{false};
    bool cancelled{false};
    bool quarantined{false};
    std::string source;
};
struct NodePoolSnapshot {
    bool stopped{false};
    std::uint16_t active{0};
    std::array<NodeSlotSnapshot, kAlphaNodePoolCapacity> slots;
};

// Runtime ingress shares this pool across both transports and node workers.
// Supply a fresh Runtime epoch from IIdentifierGenerator for each Runtime.
// The historical type name is retained for existing composition/tests. Storage
// is bounded at 128; the default remains four nodes/four connections per source.
class FourNodePool final {
public:
    explicit FourNodePool(Identifier128 runtime_epoch,
        std::uint16_t capacity = kDefaultLogicalNodes,
        std::uint16_t per_source_limit = 4,
        std::uint64_t generation_limit = std::numeric_limits<std::uint64_t>::max(),
        std::array<std::uint64_t, kAlphaNodePoolCapacity> previous_generations = {},
        std::vector<SourceConnectionLimit> source_exceptions = {});
    ~FourNodePool();
    FourNodePool(const FourNodePool&) = delete;
    FourNodePool& operator=(const FourNodePool&) = delete;
    NodeAllocation allocate(std::string_view source, bool bypass_source_limit=false);
    // Reserves only the named slot under the same mutex as lowest-free public
    // allocation. The returned lease retains the ordinary cancellation,
    // generation, quarantine and verified-cleanup contract.
    NodeAllocation allocate_exact(std::uint16_t node, std::string_view source, bool bypass_source_limit=false);
    bool cancel(NodeAssignmentIdentity identity);
    bool set_enabled(std::uint16_t node, bool enabled);
    void stop();
    NodePoolSnapshot snapshot() const;
private:
    std::shared_ptr<NodePoolState> state_;
};

} // namespace sfx::application
