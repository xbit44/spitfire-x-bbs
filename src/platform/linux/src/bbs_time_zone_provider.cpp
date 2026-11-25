#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <date/tz.h>
#include <algorithm>
#include <array>
#include <mutex>

namespace sfx::platform::linux_host {
namespace {
std::mutex database_mutex;
std::filesystem::path database_path;
bool database_ready{};
constexpr std::pair<std::string_view,std::string_view> windows_zones[]={
#include "windows_zones.inc"
};
bool valid_name(std::string_view value) {
    return !value.empty()&&value.size()<=128&&value.front()!='/'&&value.find("..") == value.npos&&
        std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||
            (c>='0'&&c<='9')||c=='/'||c=='_'||c=='-'||c=='+';});
}
class Clock final:public application::IClock {
public:
    explicit Clock(std::int64_t value):value_(value){}
    std::int64_t unix_seconds() const noexcept override{return value_;}
    std::uint64_t monotonic_millis() const noexcept override{return 0;}
private:std::int64_t value_;
};
}
bool LinuxBbsTimeZoneRuleProvider::initialize(const std::filesystem::path& root) noexcept {
    try {
        std::lock_guard<std::mutex> lock(database_mutex);
        if(database_ready)return root==database_path;
        // Verify every parser input, including optional historical filenames:
        // none may redirect to an untrusted path. Installer supplies private
        // files and the package manifest binds their exact shipped hashes.
        auto directory=open_owned_directory(root);
        for(const auto* name:{"version","africa","antarctica","asia","australasia","backward",
                "etcetera","europe","northamerica","southamerica","leapseconds"})
            if(read_private_file(directory.get(),name,2U*1024U*1024U).empty())return false;
        for(const auto* name:{"pacificnew","systemv"})
            if(std::filesystem::symlink_status(root/name).type()!=std::filesystem::file_type::not_found)return false;
        date::set_install(root.string());
        const auto& db=date::get_tzdb();
        if(db.version!="2026f"||db.zones.empty())return false;
        database_path=root;database_ready=true;return true;
    }catch(...){return false;}
}
std::unique_ptr<LinuxBbsTimeZoneRuleProvider> LinuxBbsTimeZoneRuleProvider::create(std::string_view zone) {
    try{
        if(zone.empty()||zone.size()>128)return {};
        std::string resolved(zone);
        for(const auto& mapping:windows_zones)if(mapping.first==zone){resolved=mapping.second;break;}
        if(!valid_name(resolved))return {};
        std::lock_guard<std::mutex> lock(database_mutex);if(!database_ready)return {};
        const auto* found=date::locate_zone(resolved);
        return std::unique_ptr<LinuxBbsTimeZoneRuleProvider>(new LinuxBbsTimeZoneRuleProvider(std::string(zone),found->name()));
    }catch(...){return {};}
}
std::vector<std::string> LinuxBbsTimeZoneRuleProvider::available_zones() {
    std::lock_guard<std::mutex> lock(database_mutex);if(!database_ready)return {};
    std::vector<std::string> zones;for(const auto& zone:date::get_tzdb().zones)zones.push_back(zone.name());return zones;
}
std::optional<std::string> LinuxBbsTimeZoneRuleProvider::windows_key_for_zone(std::string_view value) {
    const auto provider=create(value);if(!provider)return {};
    std::optional<std::string> result;
    for(const auto& mapping:windows_zones) {
        if(mapping.first==value)return std::string(mapping.first);
        if(mapping.second==provider->iana_zone()) {
            if(result&&*result!=mapping.first)return {};
            result=std::string(mapping.first);
        }
    }
    return result;
}
std::optional<std::int32_t> LinuxBbsTimeZoneRuleProvider::utc_offset_seconds(std::string_view key,std::int64_t utc) const {
    try{
        if(key!=configuration_.canonical_windows_registry_key||!application::valid_utc_seconds(utc))return {};
        const auto offset=date::locate_zone(iana_zone_)->get_info(
            date::sys_seconds{std::chrono::seconds{utc}}).offset.count();
        if(offset < -86400 || offset > 86400)return {};
        return static_cast<std::int32_t>(offset);
    }catch(...){return {};}
}
std::optional<std::int64_t> next_bbs_maintenance_run(std::string_view zone,std::int64_t after,
    std::uint8_t hour,std::uint8_t minute,std::optional<std::uint8_t> weekday,std::int64_t consumed) noexcept {
    try{
        if(!application::valid_utc_seconds(after)||after<=0||consumed<0||hour>=24||minute>=60||(weekday&&*weekday>6))return {};
        auto provider=LinuxBbsTimeZoneRuleProvider::create(zone);if(!provider)return {};
        Clock clock(after);application::BbsLocalTimeAdapters local(provider->configuration(),*provider,clock);
        std::optional<application::CivilDate> excluded;
        if(consumed){const auto prior=local.project_utc_seconds(consumed);if(!prior)return {};excluded=prior->date;}
        auto candidate=std::max(after,consumed);candidate=candidate-candidate%60+60;
        for(std::size_t n=0;n<10U*24U*60U;++n,candidate+=60){
            const auto value=local.project_utc_seconds(candidate);if(!value)return {};
            const auto& day=value->date;
            if(excluded&&day.year==excluded->year&&day.month==excluded->month&&day.day==excluded->day)continue;
            if(value->hour!=hour||value->minute!=minute||value->second!=0)continue;
            if(weekday){const date::year_month_day civil{date::year{day.year},date::month{day.month},date::day{day.day}};
                if(date::weekday{date::sys_days{civil}}.c_encoding()!=*weekday)continue;}
            return candidate;
        }
    }catch(...){}
    return {};
}
std::optional<std::int64_t> next_bbs_daily_run(std::string_view zone,std::int64_t after,std::uint8_t hour,std::uint8_t minute) noexcept {
    return next_bbs_maintenance_run(zone,after,hour,minute,std::nullopt,0);
}
} // namespace sfx::platform::linux_host
