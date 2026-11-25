#include "sfx/application/configuration.hpp"

#include "sfx/core/node.hpp"

namespace sfx::application {

ConfigurationValidation validate(const FoundationConfiguration& configuration) {
    ConfigurationValidation result;
    if (configuration.version != kFoundationConfigurationVersion) {
        result.errors.push_back({"version", "unsupported_version"});
    }
    if (configuration.logical_nodes == 0 || configuration.logical_nodes > core::kMaximumLogicalNodes) {
        result.errors.push_back({"logical_nodes", "outside_1_128"});
    }
    if (!configuration.security.is_configured(configuration.security.new_user_level())) {
        result.errors.push_back({"security.new_user_level", "not_configured"});
    }
    return result;
}

ConfigurationValidation ConfigurationManager::publish(FoundationConfiguration candidate) {
    auto result = validate(candidate);
    if (result.valid()) {
        active_ = std::move(candidate);
        ++generation_;
    }
    return result;
}

const std::optional<FoundationConfiguration>& ConfigurationManager::active() const noexcept { return active_; }
std::uint64_t ConfigurationManager::generation() const noexcept { return generation_; }

}  // namespace sfx::application

