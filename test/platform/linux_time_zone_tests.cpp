#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "caller_runtime_helpers.hpp"
#include <atomic>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace app=sfx::application;
namespace host=sfx::platform::linux_host;
using Provider=host::LinuxBbsTimeZoneRuleProvider;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
class Clock final:public app::IClock {
public:std::int64_t unix_seconds()const noexcept override{return 1794139200;}std::uint64_t monotonic_millis()const noexcept override{return 0;}
} clock_value;
std::int64_t utc(int year,int month,int day,int hour=0,int minute=0,int second=0){
    std::tm value{};value.tm_year=year-1900;value.tm_mon=month-1;value.tm_mday=day;value.tm_hour=hour;value.tm_min=minute;value.tm_sec=second;
    return timegm(&value);
}
std::optional<std::string> format(std::string_view zone,std::int64_t value){
    auto provider=Provider::create(zone);if(!provider)return {};
    app::BbsLocalTimeAdapters local(provider->configuration(),*provider,clock_value);return local.format_utc_seconds(value);
}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal user and private bundled data required");
    const auto* environment=std::getenv("TZ");const std::string before=environment?environment:"";const bool had=environment!=nullptr;
    const std::string locale_before=std::setlocale(LC_ALL,nullptr);
    check(!Provider::create("UTC"),"no implicit host timezone before initialization");
    check(!Provider::initialize(std::filesystem::path(argv[1])/"missing"),"missing database fails explicitly");
    check(Provider::initialize(argv[1]),"private pinned timezone rules initialized");
    check(Provider::initialize(argv[1]),"same process identity can reuse database");
    check(!Provider::initialize(std::filesystem::path(argv[1])/"other"),"different process database cannot silently replace active rules");
    check(Provider::available_zones().size()>300,"real IANA zone catalog");
    check(!Provider::create("Not a real Windows Zone")&&!Provider::create("../../etc/passwd")&&!Provider::create("/etc/localtime"),"unknown and path inputs refused");
    check(format("UTC",utc(2026,11,8,12,34,56))=="2026-11-08 12:34:56","UTC projection");
    check(format("America/Los_Angeles",utc(2026,3,8,9,59,59))=="2026-03-08 01:59:59","before spring gap");
    check(format("America/Los_Angeles",utc(2026,3,8,10))=="2026-03-08 03:00:00","spring gap follows actual transition");
    auto pacific=Provider::create("America/Los_Angeles");
    check(bool(pacific),"caller BBS-day provider");
    app::BbsLocalTimeAdapters caller_time(pacific->configuration(),*pacific,clock_value);
    const auto short_day=sfx::apps::runtime::bbs_today_range(caller_time,utc(2026,3,8,12));
    check(short_day && (*short_day)[0]==utc(2026,3,8,8) && (*short_day)[1]==utc(2026,3,9,7),
        "shared caller statistics use actual 23-hour BBS day");
    const auto long_day=sfx::apps::runtime::bbs_today_range(caller_time,utc(2026,11,1,12));
    check(long_day && (*long_day)[0]==utc(2026,11,1,7) && (*long_day)[1]==utc(2026,11,2,8),
        "shared caller statistics use actual 25-hour BBS day");
    check(format("America/Los_Angeles",utc(2026,11,1,8,59,59))=="2026-11-01 01:59:59","before autumn fold");
    check(format("America/Los_Angeles",utc(2026,11,1,9))=="2026-11-01 01:00:00","autumn fold follows actual transition");
    check(host::next_bbs_daily_run("America/Los_Angeles",utc(2026,3,8,9,59),2,30)==utc(2026,3,9,9,30),"daily run skips nonexistent civil time");
    check(host::next_bbs_daily_run("America/Los_Angeles",utc(2026,11,1,8,31),1,30)==utc(2026,11,1,9,30),"daily next real minute handles repeated civil time");
    check(host::next_bbs_maintenance_run("America/Los_Angeles",utc(2026,11,1,8,31),1,30,{},utc(2026,11,1,8,30))==utc(2026,11,2,9,30),"consumed local date excludes duplicate maintenance across fold");
    check(host::next_bbs_maintenance_run("UTC",utc(2026,11,8,12),2,0,1,0)==utc(2026,11,9,2),"weekly maintenance weekday preserved");
    check(!host::next_bbs_daily_run("UTC",utc(2026,11,8),24,0)&&!host::next_bbs_maintenance_run("UTC",utc(2026,11,8),2,0,7,0),"invalid scheduler inputs refused");
    for(int year:{2040,2100}){
        check(format("America/Los_Angeles",utc(year,1,1,12))==std::to_string(year)+"-01-01 04:00:00","future standard time beyond host transition cutoff");
        check(format("America/Los_Angeles",utc(year,7,1,12))==std::to_string(year)+"-07-01 05:00:00","future DST beyond host transition cutoff");
    }
    check(format("Australia/Lord_Howe",utc(2040,1,1))=="2040-01-01 11:00:00","southern summer half-hour DST region");
    check(format("Australia/Lord_Howe",utc(2040,7,1))=="2040-07-01 10:30:00","southern standard half-hour offset");
    check(format("Asia/Kathmandu",utc(2026,1,31,20))=="2026-02-01 01:45:00","quarter-hour offset and month boundary");
    check(format("Pacific/Apia",utc(2011,12,30,10))=="2011-12-31 00:00:00","date-line skip from actual historical rules");
    check(format("America/Havana",utc(2026,11,8,12))=="2026-11-08 08:00:00","pinned2026f Cuba rule is used");
    const auto legacy=Provider::create("Pacific Standard Time");
    check(legacy&&legacy->configuration().canonical_windows_registry_key=="Pacific Standard Time"&&legacy->iana_zone()=="America/Los_Angeles","CLDR mapping explicit and saved key preserved");
    check(format("Pacific Standard Time",utc(2040,7,1,12))==format("America/Los_Angeles",utc(2040,7,1,12)),"legacy key uses mapped IANA rules");
    check(Provider::windows_key_for_zone("America/Los_Angeles")=="Pacific Standard Time","exact CLDR Pacific mapping produces Windows-compatible saved key");
    check(Provider::windows_key_for_zone("Pacific Standard Time")=="Pacific Standard Time"&&Provider::windows_key_for_zone("Etc/UTC")=="UTC","existing Windows key and exact UTC mapping preserved");
    check(!Provider::windows_key_for_zone("Antarctica/Troll")&&!Provider::windows_key_for_zone("not/a/zone"),"unmapped IANA zone never guessed from current offset");
    check(!legacy->utc_offset_seconds("UTC",utc(2026,11,8)),"wrong provider key refused");
    class Broken final:public app::IBbsTimeZoneRuleProvider {
    public:bool provides_utc_offsets()const noexcept override{return true;}
        std::optional<app::BbsTimeZoneYearRules> rules_for_year(std::string_view,std::int32_t)const override{return app::BbsTimeZoneYearRules{};}
    } broken;
    app::BbsLocalTimeAdapters refused({"UTC"},broken,clock_value);
    check(!refused.format_utc_seconds(utc(2026,11,8)),"exact-provider failure does not fall back to year rules");
    std::atomic<bool> concurrent{true};std::vector<std::thread> readers;
    for(unsigned i=0;i<4;++i)readers.emplace_back([&]{for(unsigned n=0;n<1000;++n)
        if(format("America/Los_Angeles",utc(2040,7,1,12))!="2040-07-01 05:00:00"||format("Asia/Kathmandu",utc(2026,1,31,20))!="2026-02-01 01:45:00")concurrent=false;});
    for(auto& reader:readers)reader.join();
    check(concurrent,"concurrent independent region projections");
    const auto* after=std::getenv("TZ");
    check(had==(after!=nullptr)&&before==(after?after:"")&&locale_before==std::setlocale(LC_ALL,nullptr),"process TZ and locale unchanged");
    std::cout<<"PASS "<<checks<<" Linux IANA/CLDR/shared formatter and scheduler checks; 2040/2100 DST;8000 concurrent projections\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
