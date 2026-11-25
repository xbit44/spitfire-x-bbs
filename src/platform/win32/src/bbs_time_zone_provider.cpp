#include "sfx/platform/win32/bbs_time_zone_provider.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sfx::platform::win32 {
namespace {

constexpr wchar_t kTimeZonesRegistryPath[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones";

class RegistryKey final {
public:
    RegistryKey() = default;
    explicit RegistryKey(HKEY key) noexcept : key_(key) {}
    ~RegistryKey() { if (key_ != nullptr) RegCloseKey(key_); }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    RegistryKey(RegistryKey&& other) noexcept : key_(other.key_) { other.key_ = nullptr; }
    RegistryKey& operator=(RegistryKey&& other) noexcept {
        if (this != &other) {
            if (key_ != nullptr) RegCloseKey(key_);
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }
    HKEY get() const noexcept { return key_; }
private:
    HKEY key_{nullptr};
};

std::optional<std::wstring> utf8_to_wide(const std::string_view value) {
    if (value.empty() || value.size() > 128 ||
        value.find('\0') != std::string_view::npos ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return std::nullopt;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0 || required > 128) return std::nullopt;
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), required) != required) return std::nullopt;
    return output;
}

std::optional<std::string> wide_to_utf8(const std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::nullopt;
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0 || required > 512) return std::nullopt;
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), required, nullptr, nullptr) != required)
        return std::nullopt;
    return output;
}

std::optional<RegistryKey> open_key(
    HKEY parent, const wchar_t* path, const REGSAM access = KEY_READ) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(parent, path, 0, access, &key) != ERROR_SUCCESS) return std::nullopt;
    return RegistryKey(key);
}

std::optional<std::wstring> resolve_canonical_key(const std::wstring_view requested) {
    auto root = open_key(HKEY_LOCAL_MACHINE, kTimeZonesRegistryPath);
    if (!root.has_value()) return std::nullopt;
    DWORD index = 0;
    std::array<wchar_t, 256> name{};
    for (;;) {
        DWORD length = static_cast<DWORD>(name.size());
        const LSTATUS status = RegEnumKeyExW(root->get(), index++, name.data(),
            &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) return std::nullopt;
        if (status != ERROR_SUCCESS || length == 0 || length >= name.size()) return std::nullopt;
        const std::wstring_view candidate(name.data(), length);
        if (CompareStringOrdinal(candidate.data(), static_cast<int>(candidate.size()),
                requested.data(), static_cast<int>(requested.size()), TRUE) == CSTR_EQUAL) {
            return std::wstring(candidate);
        }
    }
}

struct RegistryTzi {
    LONG bias;
    LONG standard_bias;
    LONG daylight_bias;
    SYSTEMTIME standard_date;
    SYSTEMTIME daylight_date;
};

static_assert(sizeof(RegistryTzi) == 44, "Windows REG_TZI_FORMAT layout changed");

std::optional<RegistryTzi> read_tzi(HKEY key, const wchar_t* value_name) {
    RegistryTzi value{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    if (RegQueryValueExW(key, value_name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes != sizeof(value) ||
        value.standard_date.wYear != 0 || value.daylight_date.wYear != 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<DWORD> read_dword(HKEY key, const wchar_t* value_name) {
    DWORD value = 0;
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    if (RegQueryValueExW(key, value_name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS ||
        type != REG_DWORD || bytes != sizeof(value)) return std::nullopt;
    return value;
}

application::BbsTimeZoneTransition transition(const SYSTEMTIME& value) noexcept {
    return {static_cast<std::uint8_t>(value.wMonth),
        static_cast<std::uint8_t>(value.wDay),
        static_cast<std::uint8_t>(value.wDayOfWeek),
        static_cast<std::uint8_t>(value.wHour),
        static_cast<std::uint8_t>(value.wMinute),
        static_cast<std::uint8_t>(value.wSecond), value.wMilliseconds};
}

application::BbsTimeZoneYearRules rules(const RegistryTzi& value) noexcept {
    return {value.bias, value.standard_bias, value.daylight_bias,
        transition(value.standard_date), transition(value.daylight_date)};
}

}  // namespace

class WindowsRegistryBbsTimeZoneRuleProvider::Impl final {
public:
    Impl(application::BbsTimeZoneConfiguration value, std::wstring key)
        : configuration(std::move(value)), canonical_key(std::move(key)) {}

    application::BbsTimeZoneConfiguration configuration;
    std::wstring canonical_key;
};

std::unique_ptr<WindowsRegistryBbsTimeZoneRuleProvider>
WindowsRegistryBbsTimeZoneRuleProvider::create(
    const std::string_view configured_utf8_registry_key) {
    const auto requested = utf8_to_wide(configured_utf8_registry_key);
    if (!requested.has_value()) return {};
    const auto canonical = resolve_canonical_key(*requested);
    if (!canonical.has_value()) return {};
    const auto canonical_utf8 = wide_to_utf8(*canonical);
    if (!canonical_utf8.has_value()) return {};
    application::BbsTimeZoneConfiguration configuration{*canonical_utf8};
    if (!configuration.valid()) return {};
    return std::unique_ptr<WindowsRegistryBbsTimeZoneRuleProvider>(
        new WindowsRegistryBbsTimeZoneRuleProvider(std::make_unique<Impl>(
            std::move(configuration), *canonical)));
}

WindowsRegistryBbsTimeZoneRuleProvider::WindowsRegistryBbsTimeZoneRuleProvider(
    std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WindowsRegistryBbsTimeZoneRuleProvider::~WindowsRegistryBbsTimeZoneRuleProvider() = default;

const application::BbsTimeZoneConfiguration&
WindowsRegistryBbsTimeZoneRuleProvider::configuration() const noexcept {
    return impl_->configuration;
}

std::optional<application::BbsTimeZoneYearRules>
WindowsRegistryBbsTimeZoneRuleProvider::rules_for_year(
    const std::string_view canonical_windows_registry_key,
    const std::int32_t utc_year) const {
    if (canonical_windows_registry_key != impl_->configuration.canonical_windows_registry_key ||
        utc_year < 1 || utc_year > 9999) return std::nullopt;
    auto root = open_key(HKEY_LOCAL_MACHINE, kTimeZonesRegistryPath);
    if (!root.has_value()) return std::nullopt;
    auto zone = open_key(root->get(), impl_->canonical_key.c_str());
    if (!zone.has_value()) return std::nullopt;
    const auto base = read_tzi(zone->get(), L"TZI");
    if (!base.has_value()) return std::nullopt;

    auto dynamic = open_key(zone->get(), L"Dynamic DST");
    if (!dynamic.has_value()) return rules(*base);
    const auto first = read_dword(dynamic->get(), L"FirstEntry");
    const auto last = read_dword(dynamic->get(), L"LastEntry");
    if (!first.has_value() || !last.has_value() || *first < 1 || *last > 9999 ||
        *first > *last) return std::nullopt;
    DWORD selected = static_cast<DWORD>(utc_year);
    if (selected < *first) selected = *first;
    if (selected > *last) selected = *last;
    const std::wstring value_name = std::to_wstring(selected);
    const auto dynamic_value = read_tzi(dynamic->get(), value_name.c_str());
    return dynamic_value.has_value()
        ? std::optional<application::BbsTimeZoneYearRules>(rules(*dynamic_value))
        : std::nullopt;
}

std::optional<std::int64_t> next_bbs_daily_run(
    const std::string_view canonical_windows_registry_key,
    const std::int64_t after_utc, const std::uint8_t local_hour,
    const std::uint8_t local_minute) noexcept {
    class FixedClock final : public application::IClock {
    public:
        explicit FixedClock(const std::int64_t value) : value_(value) {}
        std::int64_t unix_seconds() const noexcept override { return value_; }
        std::uint64_t monotonic_millis() const noexcept override { return 0; }
    private:
        std::int64_t value_;
    };
    try {
        if (after_utc <= 0 || local_hour >= 24 || local_minute >= 60)
            return std::nullopt;
        auto rules = WindowsRegistryBbsTimeZoneRuleProvider::create(
            canonical_windows_registry_key);
        if (!rules) return std::nullopt;
        FixedClock clock(after_utc);
        application::BbsLocalTimeAdapters local(
            rules->configuration(), *rules, clock);
        std::int64_t candidate = after_utc - (after_utc % 60) + 60;
        for (std::size_t minute = 0; minute < 60U * 72U; ++minute,
                         candidate += 60) {
            const auto text = local.format_utc_seconds(candidate);
            if (!text || text->size() < 19U) return std::nullopt;
            if ((*text)[11] == static_cast<char>('0' + local_hour / 10) &&
                (*text)[12] == static_cast<char>('0' + local_hour % 10) &&
                (*text)[14] == static_cast<char>('0' + local_minute / 10) &&
                (*text)[15] == static_cast<char>('0' + local_minute % 10) &&
                (*text)[17] == '0' && (*text)[18] == '0') return candidate;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::int64_t> next_bbs_maintenance_run(
    const std::string_view zone,const std::int64_t after,const std::uint8_t hour,
    const std::uint8_t minute,const std::optional<std::uint8_t> weekday,
    const std::int64_t consumed) noexcept {
    class Clock final:public application::IClock{
    public: explicit Clock(std::int64_t value):value_(value){}
        std::int64_t unix_seconds()const noexcept override{return value_;}
        std::uint64_t monotonic_millis()const noexcept override{return 0;}
    private:std::int64_t value_;
    };
    try{
        if(after<=0 || consumed<0 || hour>=24 || minute>=60 || (weekday && *weekday>6))return {};
        auto provider=WindowsRegistryBbsTimeZoneRuleProvider::create(zone);if(!provider)return {};
        class CachedRules final:public application::IBbsTimeZoneRuleProvider{
        public:explicit CachedRules(const WindowsRegistryBbsTimeZoneRuleProvider& source):source_(source){}
            std::optional<application::BbsTimeZoneYearRules> rules_for_year(std::string_view key,std::int32_t year)const override{
                if(key!=source_.configuration().canonical_windows_registry_key)return {};
                auto found=years_.find(year);if(found==years_.end())found=years_.emplace(year,source_.rules_for_year(key,year)).first;
                return found->second;
            }
        private:const WindowsRegistryBbsTimeZoneRuleProvider& source_;
            mutable std::map<std::int32_t,std::optional<application::BbsTimeZoneYearRules>> years_;
        } cached(*provider);
        Clock clock(after);application::BbsLocalTimeAdapters local(provider->configuration(),cached,clock);
        std::string excluded;
        if(consumed){const auto text=local.format_utc_seconds(consumed);if(!text || text->size()<19)return {};excluded=text->substr(0,10);}
        // A complete week plus DST gap and consumed-day exclusion. No invented
        // nonexistent local instant and no workstation clock/timezone mutation.
        auto candidate=std::max(after,consumed);candidate=candidate-candidate%60+60;
        for(std::size_t n=0;n<10U*24U*60U;++n,candidate+=60){
            const auto text=local.format_utc_seconds(candidate);if(!text || text->size()<19)return {};
            if(text->compare(0,10,excluded)==0 || (*text)[11]!='0'+hour/10 || (*text)[12]!='0'+hour%10 ||
                (*text)[14]!='0'+minute/10 || (*text)[15]!='0'+minute%10 || (*text)[17]!='0' || (*text)[18]!='0')continue;
            if(weekday){
                const auto digits=[&](std::size_t at,std::size_t count){WORD value{};for(std::size_t i=0;i<count;++i){const auto ch=(*text)[at+i];if(ch<'0' || ch>'9')throw std::runtime_error("invalid civil date");value=static_cast<WORD>(value*10+ch-'0');}return value;};
                SYSTEMTIME civil{};civil.wYear=digits(0,4);civil.wMonth=digits(5,2);civil.wDay=digits(8,2);
                FILETIME file{};SYSTEMTIME normalized{};
                if(!SystemTimeToFileTime(&civil,&file) || !FileTimeToSystemTime(&file,&normalized))return {};
                if(normalized.wDayOfWeek!=*weekday)continue;
            }
            return candidate;
        }
    }catch(...){}
    return {};
}

}  // namespace sfx::platform::win32
