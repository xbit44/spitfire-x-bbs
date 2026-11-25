#include "sfx/application/lifecycle.hpp"

namespace sfx::application {

RuntimeState RuntimeLifecycle::state() const noexcept { return state_; }
bool RuntimeLifecycle::accepts_new_callers() const noexcept { return state_ == RuntimeState::online; }
const std::optional<Deadline>& RuntimeLifecycle::drain_deadline() const noexcept { return drain_deadline_; }

bool RuntimeLifecycle::request_start() noexcept {
    if (state_ != RuntimeState::stopped) {
        return false;
    }
    state_ = RuntimeState::starting;
    return true;
}

bool RuntimeLifecycle::report_started() noexcept {
    if (state_ != RuntimeState::starting) {
        return false;
    }
    state_ = RuntimeState::online;
    return true;
}

bool RuntimeLifecycle::request_shutdown(
    const ShutdownRequest& request,
    const std::uint64_t now_millis) noexcept {
    if (state_ != RuntimeState::online && state_ != RuntimeState::draining) {
        return false;
    }
    if (request.disposition == ShutdownDisposition::now || request.active_work.empty()) {
        state_ = RuntimeState::stopping;
        drain_deadline_.reset();
        return true;
    }
    if (request.drain_deadline.reached(now_millis)) {
        return false;
    }
    state_ = RuntimeState::draining;
    drain_deadline_ = request.drain_deadline;
    return true;
}

bool RuntimeLifecycle::observe_active_work(
    const ActiveWork active_work,
    const std::uint64_t now_millis) noexcept {
    if (state_ != RuntimeState::draining || !drain_deadline_.has_value()) {
        return false;
    }
    if (active_work.empty() || drain_deadline_->reached(now_millis)) {
        state_ = RuntimeState::stopping;
        drain_deadline_.reset();
    }
    return true;
}

bool RuntimeLifecycle::cancel_shutdown() noexcept {
    if (state_ != RuntimeState::draining) {
        return false;
    }
    state_ = RuntimeState::online;
    drain_deadline_.reset();
    return true;
}

bool RuntimeLifecycle::report_stopped() noexcept {
    if (state_ != RuntimeState::stopping) {
        return false;
    }
    state_ = RuntimeState::stopped;
    drain_deadline_.reset();
    return true;
}

void RuntimeLifecycle::report_failure() noexcept {
    state_ = RuntimeState::recovery_required;
    drain_deadline_.reset();
}

bool RuntimeLifecycle::acknowledge_recovery() noexcept {
    if (state_ != RuntimeState::recovery_required) {
        return false;
    }
    state_ = RuntimeState::stopped;
    return true;
}

}  // namespace sfx::application

