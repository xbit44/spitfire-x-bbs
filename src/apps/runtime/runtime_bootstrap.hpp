#pragma once

#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/authentication_service.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include <functional>

namespace sfx::apps::runtime {

// Shared shipping bootstrap transaction. The owning host serializes calls and
// checks setup/recovery state before entry. OS publication and verified package
// seeds stay at this boundary; credentials and first-run rules stay shared.
struct RuntimeBootstrapPorts {
    std::function<bool()> strong_passwords_ready;
    std::function<std::optional<std::string>(std::string_view)> canonical_time_zone;
    std::function<std::int64_t()> now_utc_seconds;
    std::function<application::AuditContext(application::AuditReasonCode,
        std::optional<application::AccountId>)> account_audit;
    std::function<application::AuthenticationAuditContext(std::optional<application::AccountId>)> authentication_audit;
    std::function<application::StorageStatus(const application::OperationalConfiguration&)> publish_configuration;
    std::function<bool()> verified_starter_doors;
};

inline application::AlphaControlResponse run_runtime_bootstrap_transaction(
    adapters::storage::SqliteStorage& storage,
    application::AuthenticationSecurityService& authentication,
    application::AlphaBootstrapRequest& request,
    std::optional<application::OperationalConfiguration>& published_configuration,
    application::AlphaRuntimePresentationState& presentation_state,
    std::string& diagnostic_code, const RuntimeBootstrapPorts& ports) {
    using application::AlphaRuntimePresentationState;
    using application::LocalControlResponseCode;
    if (!ports.strong_passwords_ready || !ports.canonical_time_zone ||
        !ports.now_utc_seconds || !ports.account_audit || !ports.authentication_audit ||
        !ports.publish_configuration || !ports.verified_starter_doors)
        return {LocalControlResponseCode::configuration_failure, "bootstrap_ports_unavailable"};
    const auto normalized_name = application::normalize_account_name(request.sysop_name);
    if (!normalized_name) {
        return {LocalControlResponseCode::invalid_request, "sysop_name_invalid"};
    }
    if (request.configuration.password_mode == application::PasswordMode::strong && !ports.strong_passwords_ready()) {
        return {LocalControlResponseCode::configuration_failure,
            "strong_password_blocklist_unavailable"};
    }
    if (!request.configuration.telnet.enabled) {
        return {LocalControlResponseCode::configuration_failure,
            "telnet_listener_required_for_alpha"};
    }
    if (request.configuration.logical_nodes < 1 || request.configuration.logical_nodes > application::kAlphaNodePoolCapacity)
        return {LocalControlResponseCode::configuration_failure, "alpha_node_count_requires_1_to_128"};
    auto zone = ports.canonical_time_zone(request.configuration.bbs_time_zone);
    if (!zone) return {LocalControlResponseCode::configuration_failure, "bbs_time_zone_invalid"};
    request.configuration.bbs_time_zone = *zone;
    const auto policy = storage.security_policy();
    if (!policy || !policy.value) {
        return {LocalControlResponseCode::storage_failure, "security_policy_unavailable"};
    }
    request.configuration.security_policy_revision = policy.value->revision;
    request.configuration.security_policy_hash = policy.value->policy_hash;
    if (!application::validate_operational_configuration(request.configuration).valid()) {
        return {LocalControlResponseCode::configuration_failure, "configuration_invalid"};
    }
    if (published_configuration && !application::equivalent_operational_configuration(*published_configuration, request.configuration)) {
        return {LocalControlResponseCode::recovery_required,
            "published_configuration_does_not_match_resume"};
    }

    auto first_run = storage.first_run_state();
    if (!first_run || !first_run.value) {
        return {LocalControlResponseCode::storage_failure, "installation_state_unavailable"};
    }
    application::AccountId sysop_account_id = 0;
    if (!first_run.value->staged_sysop_account_id) {
        application::CredentialEnvelope credential;
        application::CancellationSource cancellation;
        const auto created = authentication.create_password_credential(
            request.sysop_password,
            {request.configuration.password_mode, *normalized_name, request.configuration.bbs_name},
            ports.now_utc_seconds(), false, cancellation.token(), credential);
        if (!created.succeeded()) {
            return {LocalControlResponseCode::invalid_request, "sysop_password_rejected"};
        }
        application::StageInitialSysopRequest stage;
        stage.display_name = *normalized_name;
        stage.credential = std::move(credential);
        stage.audit = ports.account_audit(application::AuditReasonCode::initial_setup, std::nullopt);
        const auto staged = storage.stage_initial_sysop(stage);
        if (!staged || !staged.account_id) {
            return {LocalControlResponseCode::storage_failure, "initial_sysop_stage_failed"};
        }
        sysop_account_id = *staged.account_id;
    } else {
        sysop_account_id = *first_run.value->staged_sysop_account_id;
        const auto account = storage.find_account_by_id(sysop_account_id);
        if (!account || !account.value || !account.value->display_name ||
            *account.value->display_name != *normalized_name) {
            return {LocalControlResponseCode::recovery_required, "staged_sysop_identity_mismatch"};
        }
        application::CancellationSource cancellation;
        const auto verified = authentication.verify_account_password(sysop_account_id,
            request.sysop_password, ports.now_utc_seconds(), ports.authentication_audit(sysop_account_id),
            cancellation.token());
        if (!verified.authenticated()) {
            return {LocalControlResponseCode::invalid_request, "staged_sysop_password_rejected"};
        }
    }

    const auto system = storage.system_password();
    if (system.status.code == application::StorageErrorCode::not_found) {
        application::CancellationSource cancellation;
        application::SystemPasswordReplacementOperation replace;
        replace.expected_system_revision = 0;
        replace.new_password = request.system_password;
        replace.policy = {request.configuration.password_mode, *normalized_name,
            request.configuration.bbs_name};
        replace.audit = ports.authentication_audit(sysop_account_id);
        const auto replaced = authentication.replace_system_password(replace, cancellation.token());
        if (!replaced.succeeded()) {
            return {LocalControlResponseCode::invalid_request, "system_password_rejected"};
        }
    } else if (system && system.value) {
        application::CancellationSource cancellation;
        const auto verified = authentication.verify_system_password(request.system_password,
            ports.now_utc_seconds(), ports.authentication_audit(sysop_account_id), cancellation.token());
        if (!verified.authenticated()) {
            return {LocalControlResponseCode::invalid_request, "system_password_resume_rejected"};
        }
    } else {
        return {LocalControlResponseCode::storage_failure, "system_credential_state_unavailable"};
    }

    if (!published_configuration) {
        const auto published = ports.publish_configuration(request.configuration);
        if (!published) {
            presentation_state = AlphaRuntimePresentationState::recovery_required;
            diagnostic_code = published.operation;
            return {LocalControlResponseCode::configuration_failure, diagnostic_code};
        }
        published_configuration = request.configuration;
    }

    first_run = storage.first_run_state();
    if (!first_run || !first_run.value) {
        return {LocalControlResponseCode::storage_failure, "installation_state_refresh_failed"};
    }
    if (!first_run.value->complete) {
        application::CompleteFirstRunRequest complete;
        complete.staged_sysop_account_id = sysop_account_id;
        complete.expected_policy_revision = policy.value->revision;
        complete.expected_policy_hash = policy.value->policy_hash;
        complete.published_policy_hash = request.configuration.security_policy_hash;
        try {
            complete.starter_doors_eligible=ports.verified_starter_doors();
        } catch(const std::exception&) {
            return {LocalControlResponseCode::recovery_required,"starter_component_verification_failed"};
        }
        complete.audit = ports.account_audit(
            application::AuditReasonCode::initial_setup, sysop_account_id);
        if (!storage.complete_first_run(complete)) {
            presentation_state = AlphaRuntimePresentationState::recovery_required;
            diagnostic_code = "bootstrap_completion_failed";
            return {LocalControlResponseCode::storage_failure, diagnostic_code};
        }
    }
    return {LocalControlResponseCode::ok, "bootstrap_transaction_complete"};
}

} // namespace sfx::apps::runtime
