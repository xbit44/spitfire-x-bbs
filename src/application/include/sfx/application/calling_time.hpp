#pragma once
#include "sfx/application/caller_utilities.hpp"
#include <map>
namespace sfx::application {
// Elapsed charging is monotonic. Wall time supplies calendar-day labels only;
// it is never used to infer disconnected time or recover elapsed duration.
class CallerTimeAccounting final {
public:
    CallerTimeAccounting(std::uint64_t started_millis,CallerLocalFormatter formatter)
        :started_millis_(started_millis),format_(std::move(formatter)) {}
    bool advance(std::uint64_t now_millis,std::int64_t utc_now);
    std::int64_t total_seconds() const noexcept {return total_;}
    std::vector<std::pair<std::string,std::int64_t>> daily_seconds() const {return {daily_.begin(),daily_.end()};}
private:
    std::uint64_t started_millis_;
    CallerLocalFormatter format_;
    std::int64_t total_{};
    std::map<std::string,std::int64_t> daily_;
};
std::optional<std::int64_t> caller_remaining_seconds(const SecurityLevelRecord& level,
    std::int64_t session_seconds,std::int64_t used_today) noexcept;
} // namespace sfx::application
