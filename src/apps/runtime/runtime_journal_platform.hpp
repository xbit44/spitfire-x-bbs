#pragma once
#ifdef _WIN32
#include "sfx/platform/win32/durable_journal_files.hpp"
#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#else
#include "sfx/platform/linux/durable_journal_files.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <unistd.h>
#endif

namespace sfx::apps::runtime {
#ifdef _WIN32
using NativeDurableJournalFiles=platform::win32::WindowsDurableJournalFiles;
using NativeBbsTimeZoneRuleProvider=platform::win32::WindowsRegistryBbsTimeZoneRuleProvider;
#else
using NativeDurableJournalFiles=platform::linux_host::LinuxDurableJournalFiles;
using NativeBbsTimeZoneRuleProvider=platform::linux_host::LinuxBbsTimeZoneRuleProvider;
#endif
inline application::NewCallerNames native_new_caller_names(const std::filesystem::path& root,const std::wstring& owner) noexcept {
#ifdef _WIN32
    return platform::win32::load_new_caller_names(root,owner);
#else
    if(geteuid()==0||owner!=std::to_wstring(geteuid()))return application::NewCallerNames::unavailable();
    return platform::linux_host::load_new_caller_names(root);
#endif
}
} // namespace sfx::apps::runtime
