#include "../../src/apps/runtime/qwk_local_time.hpp"
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Clock final:app::IClock {std::int64_t unix_seconds()const noexcept override{return 1791920000;}std::uint64_t monotonic_millis()const noexcept override{return 1;}};
struct Zone final:app::IBbsTimeZoneRuleProvider {
    bool available=true;
    std::optional<app::BbsTimeZoneYearRules> rules_for_year(std::string_view,std::int32_t)const override{if(!available)return {};app::BbsTimeZoneYearRules r;r.bias_minutes=480;r.daylight_bias_minutes=-60;r.daylight_transition={3,2,0,2,0,0,0};r.standard_transition={11,1,0,2,0,0,0};return r;}
};
int main()try{
    Clock clock;Zone zone;app::BbsLocalTimeAdapters local({"Pacific Standard Time"},zone,clock);
    for(const auto text:{"2026-01-15 12:34","2026-07-15 12:34","2024-02-29 12:00","2026-03-08 01:59","2026-03-08 03:00","2026-11-01 00:59","2026-11-01 02:00"}){
        const auto utc=sfx::apps::runtime::qwk_local_entry_utc(text,local,zone);check(utc.has_value(),"valid unambiguous configured-zone entry time");const auto formatted=local.format_utc_seconds(*utc);check(formatted&&formatted->substr(0,16)==text,"exact BBS-local roundtrip, independent workstation zone");
    }
    for(const auto text:{"2026-03-08 02:00","2026-03-08 02:30","2026-11-01 01:00","2026-11-01 01:30","2026-02-29 12:00","2026-02-31 12:00","2026-13-01 12:00","2026-00-01 12:00","2026-01-00 12:00","2026-01-01 24:00","2026-01-01 12:60","2026-01-01T12:00","2026-01-01 12:00Z","1969-12-31 12:00","2026-x1-01 12:00",""})check(!sfx::apps::runtime::qwk_local_entry_utc(text,local,zone),"invalid/missing/repeated civil time cannot invent entry boundary");
    zone.available=false;check(!sfx::apps::runtime::qwk_local_entry_utc("2026-01-15 12:34",local,zone),"missing configured year rules fail closed");
    std::cout<<"PASS "<<checks<<" QWK configured-zone entry-time checks; simulated zone, not installed caller UI\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL entry time after "<<checks<<": "<<e.what()<<'\n';return 1;}
