#pragma once
#include "sfx/application/local_control_protocol.hpp"
#include <limits>

namespace sfx::apps::runtime {
struct RuntimeConfigurationRejection {
    std::string diagnostic;
    std::string detail;
};
// The editor may change operational settings, not forge policy or ownership.
// Both hosts use the same shipping revision and candidate checks; OS timezone
// availability and live service activation stay with the owning host.
inline std::optional<RuntimeConfigurationRejection> validate_runtime_configuration_update(
    const application::AlphaConfigurationUpdateRequest& request,
    const application::OperationalConfiguration& saved) {
    const auto& candidate=request.configuration;
    if (request.expected_revision!=saved.configuration_revision ||
        request.expected_revision==std::numeric_limits<std::uint64_t>::max() ||
        candidate.configuration_revision!=request.expected_revision+1U)
        return RuntimeConfigurationRejection{"configuration_revision_conflict",
            "Refused - configuration changed; reload before saving"};
    if (!application::validate_operational_configuration(candidate).valid() ||
        candidate.logical_nodes<1 || candidate.logical_nodes>application::kAlphaNodePoolCapacity ||
        candidate.ownership_mode!=saved.ownership_mode ||
        candidate.security_policy_revision!=saved.security_policy_revision ||
        candidate.security_policy_hash!=saved.security_policy_hash)
        return RuntimeConfigurationRejection{"configuration_update_invalid",
            "Refused - one or more settings are invalid or outside this editor"};
    return std::nullopt;
}
} // namespace sfx::apps::runtime
