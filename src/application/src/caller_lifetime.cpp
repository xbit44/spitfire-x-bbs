#include "sfx/application/caller_lifetime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sfx::application {

DurableSessionCleanup::DurableSessionCleanup(IAccountStorage& accounts, IClock& clock,
    Identifier128 session, Identifier128 audit, std::int64_t started,
    std::shared_ptr<CallerCleanupHealth> health,std::function<SessionAccountingSnapshot()> accounting)
    : accounts_(&accounts), clock_(&clock),
      request_{session, 0, started, SessionOutcomeCode::runtime_error, audit},
      started_(started), health_(std::move(health)),accounting_(std::move(accounting)) {
    if (!session.valid() || !audit.valid() || !valid_utc_seconds(started))
        throw std::invalid_argument("invalid durable cleanup identity");
}

DurableSessionCleanup::~DurableSessionCleanup() {
    if (accounts_) (void)finalize(0, SessionOutcomeCode::runtime_error);
}
DurableSessionCleanup::DurableSessionCleanup(DurableSessionCleanup&& other) noexcept
    : accounts_(std::exchange(other.accounts_, nullptr)), clock_(other.clock_),
      request_(other.request_), started_(other.started_), health_(std::move(other.health_)),
      confirmed_(other.confirmed_), attempted_(other.attempted_), succeeded_(other.succeeded_),accounting_(std::move(other.accounting_)) {}
DurableSessionCleanup& DurableSessionCleanup::operator=(DurableSessionCleanup&& other) noexcept {
    if (this != &other) {
        if (accounts_) (void)finalize(0, SessionOutcomeCode::runtime_error);
        accounts_ = std::exchange(other.accounts_, nullptr);
        clock_ = other.clock_; request_ = other.request_; started_ = other.started_;
        health_ = std::move(other.health_); confirmed_ = other.confirmed_;
        attempted_ = other.attempted_; succeeded_ = other.succeeded_;
        accounting_=std::move(other.accounting_);
    }
    return *this;
}
bool DurableSessionCleanup::finalize(std::int64_t seconds, SessionOutcomeCode outcome) noexcept {
    if (attempted_ || !accounts_) return succeeded_;
    attempted_ = true;
    request_.total_authenticated_seconds = std::max<std::int64_t>(0, seconds);
    if(accounting_) {
        try {
            auto snapshot=accounting_();
            request_.total_authenticated_seconds=std::max(request_.total_authenticated_seconds,snapshot.total_seconds);
            request_.daily_seconds=std::move(snapshot.daily_seconds);
            if(snapshot.outcome) outcome=*snapshot.outcome;
        } catch(...) {if(health_) health_->failed.store(true);return false;}
    }
    request_.completed_utc_seconds = std::max(started_, clock_->unix_seconds());
    request_.outcome = outcome;
    // Exact immutable request on every attempt, including ambiguous I/O/throw.
    // No new audit identity, changing timestamp, or unbounded retry on exit.
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        try {
            const auto result = accounts_->finalize_session(request_);
            if (result.succeeded() || (!confirmed_ && result.status.code == StorageErrorCode::not_found)) {
                succeeded_ = true;
                return true;
            }
            const auto code = result.status.code;
            if (code != StorageErrorCode::busy && code != StorageErrorCode::backpressure &&
                code != StorageErrorCode::io && code != StorageErrorCode::internal &&
                code != StorageErrorCode::shutting_down) break;
        } catch (...) { /* Same request may have committed; retry it only. */ }
    }
    if (health_) health_->failed.store(true);
    return false;
}

void DurableSessionCleanup::commit_failed(StorageErrorCode code) noexcept {
    // These typed rejections guarantee no committed session. I/O, busy, worker
    // failure and exceptions remain guarded because the outcome may be ambiguous.
    if (!confirmed_ && (code == StorageErrorCode::full || code == StorageErrorCode::conflict ||
        code == StorageErrorCode::constraint || code == StorageErrorCode::invalid_argument ||
        code == StorageErrorCode::not_found)) {
        attempted_ = succeeded_ = true;
    }
}

} // namespace sfx::application
