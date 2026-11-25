#include "sfx/application/calling_time.hpp"
#include <algorithm>
#include <limits>
namespace sfx::application {
bool CallerTimeAccounting::advance(std::uint64_t now_millis,std::int64_t utc_now) {
    if(!format_ || now_millis<started_millis_ || !valid_utc_seconds(utc_now)) return false;
    const auto elapsed=(now_millis-started_millis_)/1000U;
    if(elapsed>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    const auto next=static_cast<std::int64_t>(elapsed);
    if(next<total_) return false;
    auto remaining=next-total_;
    if(utc_now<kMinimumUtcSeconds+remaining) return false;
    auto cursor=utc_now-remaining;
    auto daily=daily_;
    do {
        const auto stamp=format_(cursor);
        if(!stamp || stamp->size()<10 || !valid_iso_civil_date(stamp->substr(0,10))) return false;
        const auto day=stamp->substr(0,10);
        if(!remaining) {daily.try_emplace(day,0);break;}
        const auto range=caller_local_day_range(format_,cursor,false);
        if(!range || range->second<=cursor) return false;
        const auto seconds=std::min(remaining,range->second-cursor);
        if(daily[day]>std::numeric_limits<std::int64_t>::max()-seconds) return false;
        daily[day]+=seconds;remaining-=seconds;cursor+=seconds;
    } while(remaining);
    if(daily.size()>4096) return false;
    daily_=std::move(daily);total_=next;return true;
}
std::optional<std::int64_t> caller_remaining_seconds(const SecurityLevelRecord& level,
    std::int64_t session_seconds,std::int64_t used_today) noexcept {
    if(level.level==999) return std::nullopt;
    std::optional<std::int64_t> remaining;
    if(level.minutes_per_call) remaining=std::max<std::int64_t>(0,static_cast<std::int64_t>(*level.minutes_per_call)*60-std::max<std::int64_t>(0,session_seconds));
    if(level.minutes_per_day) {
        const auto daily=std::max<std::int64_t>(0,static_cast<std::int64_t>(*level.minutes_per_day)*60-std::max<std::int64_t>(0,used_today));
        remaining=remaining?std::min(*remaining,daily):daily;
    }
    return remaining;
}
} // namespace sfx::application
