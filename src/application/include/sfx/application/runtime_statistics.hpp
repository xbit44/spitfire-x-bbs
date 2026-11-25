#pragma once

#include "sfx/application/local_control_protocol.hpp"

#include <functional>
#include <mutex>

namespace sfx::application {

// One coherent committed snapshot, serialized from read through publication.
// This lock is private to statistics: never a node, pool, or operational-log lock.
class RuntimeStatisticsPublisher final {
public:
    using Read = std::function<StorageResult<AlphaRuntimeStatistics>()>;
    explicit RuntimeStatisticsPublisher(AlphaRuntimeStatistics initial = {}) : value_(initial) {}
    StorageResult<std::uint64_t> refresh(const Read& read) {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            auto result = read();
            if (result && result.value && result.value->successful_logons.available &&
                result.value->caller_seconds.available && result.value->new_users.available) {
                value_ = *result.value;
                return {StorageStatus::success(), value_.successful_logons.today};
            }
        } catch (...) {}
        value_ = {}; // Unavailable is not zero success or an apparently fresh old value.
        return {{StorageErrorCode::io, "runtime statistics unavailable"}, {}};
    }
    AlphaRuntimeStatistics snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }
private:
    mutable std::mutex mutex_;
    AlphaRuntimeStatistics value_;
};

} // namespace sfx::application
