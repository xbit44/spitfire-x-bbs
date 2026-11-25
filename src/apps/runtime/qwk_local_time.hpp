#pragma once
#include "sfx/application/phase3a_private_adapters.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#endif
#include <algorithm>
#include <vector>
namespace sfx::apps::runtime {
// Entry-time conversion in the configured BBS zone, not workstation local time.
// Missing/repeated civil times fail closed; no invented DST-fold choice.
inline std::optional<std::int64_t> qwk_local_entry_utc(std::string_view text,
    const application::BbsLocalTimeAdapters& local,const application::IBbsTimeZoneRuleProvider& zone){
    if(text.size()!=16||text[4]!='-'||text[7]!='-'||text[10]!=' '||text[13]!=':')return {};
    const auto number=[&](std::size_t at,std::size_t count)->std::optional<unsigned>{unsigned v{};for(std::size_t n=0;n<count;++n){const char c=text[at+n];if(c<'0'||c>'9')return {};v=v*10+static_cast<unsigned>(c-'0');}return v;};
    const auto y=number(0,4),m=number(5,2),d=number(8,2),h=number(11,2),minute=number(14,2);if(!y||!m||!d||!h||!minute||*y<1970||*y>9998||*m<1||*m>12||*d<1||*d>31||*h>23||*minute>59)return {};
#ifdef _WIN32
    SYSTEMTIME civil{};civil.wYear=static_cast<WORD>(*y);civil.wMonth=static_cast<WORD>(*m);civil.wDay=static_cast<WORD>(*d);civil.wHour=static_cast<WORD>(*h);civil.wMinute=static_cast<WORD>(*minute);FILETIME file{};SYSTEMTIME back{};
    if(!SystemTimeToFileTime(&civil,&file)||!FileTimeToSystemTime(&file,&back)||back.wYear!=civil.wYear||back.wMonth!=civil.wMonth||back.wDay!=civil.wDay||back.wHour!=civil.wHour||back.wMinute!=civil.wMinute)return {};
    ULARGE_INTEGER ticks{};ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;if(ticks.QuadPart<116444736000000000ULL)return {};const auto naive=static_cast<std::int64_t>((ticks.QuadPart-116444736000000000ULL)/10000000ULL);
#else
    static_assert(sizeof(time_t)>=8,"Native Linux BBS requires64-bit civil time");
    std::tm civil{};civil.tm_year=static_cast<int>(*y)-1900;civil.tm_mon=static_cast<int>(*m)-1;
    civil.tm_mday=static_cast<int>(*d);civil.tm_hour=static_cast<int>(*h);civil.tm_min=static_cast<int>(*minute);
    const auto seconds=timegm(&civil);std::tm back{};
    if(seconds<0||!gmtime_r(&seconds,&back)||back.tm_year!=static_cast<int>(*y)-1900||back.tm_mon!=static_cast<int>(*m)-1||
        back.tm_mday!=static_cast<int>(*d)||back.tm_hour!=static_cast<int>(*h)||back.tm_min!=static_cast<int>(*minute))return {};
    const auto naive=static_cast<std::int64_t>(seconds);
#endif
    std::vector<std::int64_t> candidates;
    for(int year=static_cast<int>(*y)-1;year<=static_cast<int>(*y)+1;++year){const auto rules=zone.rules_for_year(local.configuration().canonical_windows_registry_key,year);if(!rules)return {};
        for(const auto extra:{rules->standard_bias_minutes,rules->daylight_bias_minutes}){const auto bias=static_cast<std::int64_t>(rules->bias_minutes)+extra;if(bias<-1440||bias>1440)return {};const auto utc=naive+bias*60;const auto roundtrip=local.format_utc_seconds(utc);if(roundtrip&&roundtrip->size()>=16&&roundtrip->substr(0,16)==text&&std::find(candidates.begin(),candidates.end(),utc)==candidates.end())candidates.push_back(utc);}
    }
    return candidates.size()==1?std::optional<std::int64_t>{candidates.front()}:std::nullopt;
}
} // namespace sfx::apps::runtime
