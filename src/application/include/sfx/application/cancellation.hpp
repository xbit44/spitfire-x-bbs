#pragma once

#include "sfx/application/time.hpp"

#include <atomic>
#include <memory>

namespace sfx::application {

class CancellationToken final {
public:
    bool is_cancelled() const noexcept { return state_->load(); }
    bool should_stop(std::uint64_t now_millis, Deadline deadline) const noexcept {
        return is_cancelled() || deadline.reached(now_millis);
    }

private:
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state) : state_(std::move(state)) {}
    std::shared_ptr<std::atomic_bool> state_;
    friend class CancellationSource;
};

class CancellationSource final {
public:
    CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}
    CancellationToken token() const { return CancellationToken(state_); }
    void cancel() noexcept { state_->store(true); }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

}  // namespace sfx::application

