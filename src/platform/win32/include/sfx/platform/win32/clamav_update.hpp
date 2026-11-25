#pragma once
#include "sfx/platform/win32/clamav_installation.hpp"
#include <array>
#include <optional>
namespace sfx::platform::win32 {
enum class ClamAvUpdateMode { automatic, manual };
enum class ClamAvDefinitionChange { unknown, initial, updated, current };
struct ClamAvDatabaseVersion {
    std::uint32_t version{}, signatures{};
    std::int64_t published{};
};
// Display metadata only. Parsing a header NEVER authenticates a database.
std::optional<ClamAvDatabaseVersion> clamav_database_header(const std::vector<std::uint8_t>&) noexcept;
std::string clamav_change_text(ClamAvDefinitionChange);
struct ClamAvStatus {
    bool installed{},verified{},definitions_present{},definitions_verified{},history_available{};
    std::int64_t last_attempt{},last_success{},last_change{};
    ClamAvUpdateMode last_mode{ClamAvUpdateMode::automatic};
    std::string version,diagnostic,last_result;
    ClamAvDefinitionChange change{ClamAvDefinitionChange::unknown};
    std::array<std::optional<ClamAvDatabaseVersion>,3> databases;
};
struct ClamAvLogView { bool available{},missing{},truncated{};std::string text; };
// Fixed leaf, protected ancestors, shared read, bounded tail; no updater lock.
ClamAvLogView read_clamav_update_log(const std::filesystem::path&,const std::wstring&) noexcept;
// Read-only, bounded inspection. Legacy/missing/corrupt history is unknown,
// never a fabricated successful update. Ready requires exact saved hashes of
// all three databases from a successful vendor check.
ClamAvStatus inspect_clamav_status(const std::filesystem::path&,const std::wstring&,
    const std::wstring&,const std::function<bool()>& cancelled={}) noexcept;
bool valid_clamav_update_status(const std::vector<std::uint8_t>&) noexcept;
struct ClamAvUpdateResult {
    bool attempted{},succeeded{},cancelled{},process_reaped{true},cleanup_complete{true};
    std::uint32_t exit_code{0xffffffffU};
    std::string diagnostic;
    ClamAvDefinitionChange change{ClamAvDefinitionChange::unknown};
    std::uint32_t daily_version{};
};
// Ordinary Setup coordinator for the first attempt; Runtime is the sole daily
// owner thereafter. Persisted UTC-day admission precedes launch, including failed
// attempts; restarts do not repeatedly contact the vendor. No daemon or service.
ClamAvUpdateResult update_clamav_definitions(const std::filesystem::path& root,
    const std::wstring& owner,const std::wstring& installation_id,std::int64_t now,
    const std::function<bool()>& cancelled={},const std::function<void(std::uint64_t)>& activity={},
    std::uint32_t deadline_milliseconds=180000,
    ClamAvUpdateMode mode=ClamAvUpdateMode::automatic);
} // namespace sfx::platform::win32
