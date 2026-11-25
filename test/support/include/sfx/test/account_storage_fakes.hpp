#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "sfx/application/account_storage.hpp"

namespace sfx::test {

class DeterministicIdentifierSource final : public application::IIdentifierGenerator {
public:
    explicit DeterministicIdentifierSource(std::uint32_t next = 1) noexcept : next_(next) {}

    application::Identifier128 generate() override { return next(); }

    application::Identifier128 next() noexcept {
        const std::uint32_t value = next_++;
        application::Identifier128 identifier;
        identifier.bytes[0] = 0x53;
        identifier.bytes[1] = 0x46;
        identifier.bytes[2] = 0x58;
        identifier.bytes[3] = 0x42;
        identifier.bytes[12] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
        identifier.bytes[13] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        identifier.bytes[14] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        identifier.bytes[15] = static_cast<std::uint8_t>(value & 0xffU);
        return identifier;
    }

private:
    std::uint32_t next_;
};

class FakeAccountStorage final : public application::IAccountStorage {
public:
    application::StorageStatus mutation_status;
    application::MutationResult mutation_result;
    application::StorageResult<application::FirstRunState> first_run_result;
    application::StorageResult<application::AccountRecord> account_result;
    std::deque<application::StorageResult<application::AccountRecord>> account_results;
    application::StorageResult<application::CredentialEnvelope> credential_result;
    application::StorageResult<std::vector<application::AccountRecord>> account_list_result;
    application::StorageResult<std::vector<application::SessionRecord>> session_list_result;
    application::StorageResult<std::vector<application::AuditRecord>> audit_list_result;
    application::StorageResult<std::vector<application::SecurityLevelRecord>> level_list_result;
    application::StorageResult<application::SecurityLevelRecord> level_result;
    application::StorageResult<application::SecurityPolicyRecord> policy_result;
    std::deque<application::StorageResult<application::SecurityPolicyRecord>> policy_results;
    application::StorageResult<application::RetentionPolicyRecord> retention_policy_result;
    std::vector<std::string> calls;
    std::vector<application::FinalizeSessionRequest> finalize_requests;
    std::function<application::MutationResult(const application::FinalizeSessionRequest&)> finalize_hook;

    application::StorageResult<application::FirstRunState> first_run_state() override {
        calls.emplace_back("first_run_state");
        return first_run_result;
    }
    application::MutationResult stage_initial_sysop(
        const application::StageInitialSysopRequest&) override {
        return mutation("stage_initial_sysop");
    }
    application::MutationResult complete_first_run(
        const application::CompleteFirstRunRequest&) override {
        return mutation("complete_first_run");
    }
    application::MutationResult create_account(const application::CreateAccountRequest&) override {
        return mutation("create_account");
    }
    application::MutationResult rename_account(const application::RenameAccountRequest&) override {
        return mutation("rename_account");
    }
    application::MutationResult update_profile(const application::UpdateProfileRequest&) override {
        return mutation("update_profile");
    }
    application::MutationResult save_account(const application::SaveAccountRequest&) override {
        return mutation("save_account");
    }
    application::MutationResult set_purge_allowed(
        const application::SetPurgeAllowedRequest&) override {
        return mutation("set_purge_allowed");
    }
    application::MutationResult replace_credential(
        const application::ReplaceCredentialRequest&) override {
        return mutation("replace_credential");
    }
    application::MutationResult assign_security_level(
        const application::AssignAccountSecurityLevelRequest&) override {
        return mutation("assign_security_level");
    }
    application::MutationResult change_account_state(
        const application::ChangeAccountStateRequest&) override {
        return mutation("change_account_state");
    }
    application::MutationResult tombstone_account(
        const application::TombstoneAccountRequest&) override {
        return mutation("tombstone_account");
    }
    application::StorageStatus publish_security_policy(
        const application::PublishSecurityPolicyRequest&) override {
        calls.emplace_back("publish_security_policy");
        return mutation_status;
    }
    application::StorageStatus publish_retention_policy(
        const application::PublishRetentionPolicyRequest&) override {
        calls.emplace_back("publish_retention_policy");
        return mutation_status;
    }
    application::MutationResult record_session_started(
        const application::StartSessionRequest&) override {
        return mutation("record_session_started");
    }
    application::StorageStatus persist_session_progress(
        const application::PersistSessionProgressRequest&) override {
        calls.emplace_back("persist_session_progress");
        return mutation_status;
    }
    application::MutationResult finalize_session(
        const application::FinalizeSessionRequest& request) override {
        finalize_requests.push_back(request);
        if (finalize_hook) return finalize_hook(request);
        return mutation("finalize_session");
    }
    application::StorageResult<application::AccountRecord> find_account_by_name(
        std::string_view) override {
        calls.emplace_back("find_account_by_name");
        return account_result;
    }
    application::StorageResult<application::AccountRecord> find_account_by_id(
        application::AccountId) override {
        calls.emplace_back("find_account_by_id");
        if (!account_results.empty()) {
            auto result = std::move(account_results.front());
            account_results.pop_front();
            return result;
        }
        return account_result;
    }
    application::StorageResult<application::CredentialEnvelope> load_credential(
        application::AccountId) override {
        calls.emplace_back("load_credential");
        return credential_result;
    }
    application::StorageResult<std::vector<application::AccountRecord>> list_accounts(
        const application::AccountListQuery&) override {
        calls.emplace_back("list_accounts");
        return account_list_result;
    }
    application::StorageResult<std::vector<application::AccountRecord>> inactivity_candidates(
        const application::InactivityCandidateQuery&) override {
        calls.emplace_back("inactivity_candidates");
        return account_list_result;
    }
    application::StorageResult<std::vector<application::SessionRecord>> session_history(
        const application::HistoryQuery&) override {
        calls.emplace_back("session_history");
        return session_list_result;
    }
    application::StorageResult<std::vector<application::AuditRecord>> audit_history(
        const application::AuditHistoryQuery&) override {
        calls.emplace_back("audit_history");
        return audit_list_result;
    }
    application::StorageResult<std::vector<application::SecurityLevelRecord>> security_levels() override {
        calls.emplace_back("security_levels");
        return level_list_result;
    }
    application::StorageResult<application::SecurityLevelRecord> security_level(
        std::uint16_t) override {
        calls.emplace_back("security_level");
        return level_result;
    }
    application::StorageResult<application::SecurityPolicyRecord> security_policy() override {
        calls.emplace_back("security_policy");
        if (!policy_results.empty()) {
            auto result = std::move(policy_results.front());
            policy_results.pop_front();
            return result;
        }
        return policy_result;
    }
    application::StorageResult<application::RetentionPolicyRecord> retention_policy() override {
        calls.emplace_back("retention_policy");
        return retention_policy_result;
    }

private:
    application::MutationResult mutation(std::string call) {
        calls.push_back(std::move(call));
        return mutation_result;
    }
};

}  // namespace sfx::test
