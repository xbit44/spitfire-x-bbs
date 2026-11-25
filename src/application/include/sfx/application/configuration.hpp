#pragma once

#include "sfx/application/lifecycle.hpp"
#include "sfx/core/security.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <string_view>

namespace sfx::application {

inline constexpr std::uint32_t kFoundationConfigurationVersion = 1;

struct ListenerParticipation {
    bool bot_gate_enabled{false};
};

struct FoundationConfiguration {
    std::uint32_t version{kFoundationConfigurationVersion};
    std::uint16_t logical_nodes{0};
    RuntimeOwnershipMode ownership_mode{RuntimeOwnershipMode::desktop};
    core::SecurityPolicy security{core::SecurityPolicy::fresh_install()};
    ListenerParticipation telnet{true};
    ListenerParticipation ssh{false};
};

struct ConfigurationError {
    std::string field;
    std::string code;
};

struct ConfigurationValidation {
    std::vector<ConfigurationError> errors;
    bool valid() const noexcept { return errors.empty(); }
};

class IConfigurationSource {
public:
    virtual ~IConfigurationSource() = default;
    virtual std::optional<FoundationConfiguration> load() = 0;
};

class IProtectedSecretSource {
public:
    virtual ~IProtectedSecretSource() = default;
    virtual std::optional<std::string> load_secret(std::string_view identity) = 0;
};

ConfigurationValidation validate(const FoundationConfiguration& configuration);

class ConfigurationManager final {
public:
    ConfigurationValidation publish(FoundationConfiguration candidate);
    const std::optional<FoundationConfiguration>& active() const noexcept;
    std::uint64_t generation() const noexcept;

private:
    std::optional<FoundationConfiguration> active_;
    std::uint64_t generation_{0};
};

}  // namespace sfx::application

