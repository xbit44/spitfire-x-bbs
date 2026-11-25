#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sfx/application/phase3a_storage.hpp"

namespace sfx::test {

class FakePhase3aStorage final : public application::IPhase3aStorage {
public:
    application::StorageResult<application::TransferPreferenceRecord> preference_result;
    application::MutationResult mutation_result;
    application::StorageStatus status_result;
    application::StorageResult<application::FailedLoginNoticeBatch> claim_result;
    application::StorageResult<std::size_t> recovery_result;
    std::vector<std::string> calls;
    std::optional<application::UpdateTransferPreferenceRequest> update_request;
    std::optional<application::EnqueueFailedLoginNoticeRequest> enqueue_request;
    std::vector<application::EnqueueFailedLoginNoticeRequest> enqueue_requests;
    std::optional<application::ClaimFailedLoginNoticesRequest> claim_request;
    std::optional<application::AcknowledgeFailedLoginNoticesRequest> acknowledge_request;
    std::vector<application::AcknowledgeFailedLoginNoticesRequest> acknowledge_requests;
    std::optional<application::RecoverFailedLoginClaimsRequest> recovery_request;
    bool dynamic_claim_result{false};
    std::vector<application::FailedLoginNoticeRecord> dynamic_notices;

    application::StorageResult<application::TransferPreferenceRecord> transfer_preference(
        application::AccountId) override {
        calls.emplace_back("transfer_preference");
        return preference_result;
    }

    application::MutationResult update_transfer_preference(
        const application::UpdateTransferPreferenceRequest& request) override {
        calls.emplace_back("update_transfer_preference");
        update_request = request;
        return mutation_result;
    }

    application::StorageStatus enqueue_failed_login_notice(
        const application::EnqueueFailedLoginNoticeRequest& request) override {
        calls.emplace_back("enqueue_failed_login_notice");
        enqueue_request = request;
        enqueue_requests.push_back(request);
        return status_result;
    }

    application::StorageResult<application::FailedLoginNoticeBatch> claim_failed_login_notices(
        const application::ClaimFailedLoginNoticesRequest& request) override {
        calls.emplace_back("claim_failed_login_notices");
        claim_request = request;
        if (dynamic_claim_result) {
            application::FailedLoginNoticeBatch batch;
            batch.claim_id = request.claim_id;
            batch.session_id = request.session_id;
            batch.notices = dynamic_notices;
            return {application::StorageStatus::success(), std::move(batch)};
        }
        return claim_result;
    }

    application::StorageStatus acknowledge_failed_login_notices(
        const application::AcknowledgeFailedLoginNoticesRequest& request) override {
        calls.emplace_back("acknowledge_failed_login_notices");
        acknowledge_request = request;
        acknowledge_requests.push_back(request);
        return status_result;
    }

    application::StorageResult<std::size_t> recover_abandoned_failed_login_claims(
        const application::RecoverFailedLoginClaimsRequest& request) override {
        calls.emplace_back("recover_abandoned_failed_login_claims");
        recovery_request = request;
        return recovery_result;
    }
};

}  // namespace sfx::test
