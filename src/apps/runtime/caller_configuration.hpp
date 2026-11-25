#pragma once
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/application/phase3a_private_adapters.hpp"

namespace sfx::apps::runtime {
// Both native hosts bind caller policy to the same published database revision.
// An unpublished or mismatched policy cannot admit callers.
inline std::shared_ptr<const application::Phase3aConfigurationSnapshot>
make_caller_configuration(adapters::storage::SqliteStorage& storage,
    const application::OperationalConfiguration& candidate,
    const std::string& sysop_name, const std::string& runtime_version) {
    const auto saved_policy = storage.security_policy();
    const auto saved_levels = storage.security_levels();
    if (!saved_policy || !saved_policy.value || !saved_levels || !saved_levels.value ||
        saved_policy.value->revision != candidate.security_policy_revision ||
        saved_policy.value->policy_hash != candidate.security_policy_hash ||
        saved_policy.value->published_revision != saved_policy.value->revision ||
        saved_policy.value->published_policy_hash != saved_policy.value->policy_hash)
        return {};
    std::vector<core::SecurityLevel> levels;
    for (const auto& entry : *saved_levels.value) {
        if (entry.state != application::SecurityLevelState::active) continue;
        const auto level = core::SecurityLevel::create(entry.level);
        if (!level) return {};
        levels.push_back(*level);
    }
    const auto new_user = core::SecurityLevel::create(saved_policy.value->new_user_level);
    const auto sysop = core::SecurityLevel::create(saved_policy.value->sysop_threshold);
    if (!new_user || !sysop) return {};
    const auto security = core::SecurityPolicy::create(std::move(levels), *new_user, *sysop);
    if (!security) return {};
    application::Phase3aConfigurationValues values;
    values.foundation.logical_nodes = candidate.logical_nodes;
    values.foundation.ownership_mode = application::RuntimeOwnershipMode::desktop;
    values.foundation.security = *security;
    values.foundation.telnet.bot_gate_enabled = false;
    values.foundation.ssh.bot_gate_enabled = false;
    values.bootstrap_complete = true;
    values.allow_new_users = candidate.allow_new_users;
    values.real_names_required = false;
    values.collect_optional_real_name = false;
    values.location_mode = application::RegistrationFieldMode::off;
    values.birth_date_mode = application::RegistrationFieldMode::off;
    values.sex_mode = application::RegistrationFieldMode::off;
    values.email_mode = application::EmailRegistrationMode::off;
    values.transfer_protocol_question_enabled = true;
    values.password_mode = candidate.password_mode;
    values.security_policy = {candidate.security_policy_revision,
        candidate.security_policy_hash, saved_policy.value->new_user_level,
        saved_policy.value->sysop_threshold};
    values.connection_limits.maximum_connections_per_source = candidate.maximum_connections_per_source;
    values.connection_limits.maximum_sessions_per_account = candidate.maximum_sessions_per_account;
    values.connection_limits.maximum_logical_nodes = candidate.logical_nodes;
    values.prelogin_identity = {candidate.bbs_name, sysop_name,
        runtime_version};
    auto configured = application::Phase3aConfigurationSnapshot::create(
        std::move(values));
    return configured.valid()
        ? std::make_shared<const application::Phase3aConfigurationSnapshot>(
            std::move(*configured.snapshot))
        : std::shared_ptr<const application::Phase3aConfigurationSnapshot>{};
}
} // namespace sfx::apps::runtime
