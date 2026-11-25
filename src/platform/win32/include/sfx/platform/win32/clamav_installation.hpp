#pragma once
#include "sfx/platform/win32/file_scanner.hpp"

namespace sfx::platform::win32 {
inline constexpr char kClamAvPortableVersion[] = "1.4.7";
inline constexpr char kClamAvPortableArchivePrefix[] = "clamav-1.4.7.win.win32/";
inline constexpr std::uint64_t kClamAvPortableBytes = 170787113;
inline constexpr char kClamAvPortableSha256[] = "580F250D16FC9130360C1DD8B452A6ED5388A8E59A344E7770D852E15268CFF3";
// Setup's ordinary coordinator and elevated worker BOTH verify the exact ZIP.
// No download, system service, registry ownership or execution of vendor code.
void verify_clamav_package(const std::filesystem::path& zip);
void provision_clamav_engine(const std::filesystem::path& root,const std::wstring& owner,
    const std::wstring& installation_id,const std::filesystem::path& zip,
    const std::function<void()>& tick={}, const std::wstring& predecessor_id={});
bool clamav_installed(const std::filesystem::path& root);
bool clamav_upgrade_required(const std::filesystem::path& root, const std::wstring& owner,
    const std::wstring& installation_id, const std::wstring& predecessor_id={}) noexcept;
void verify_clamav_installation(const std::filesystem::path& root,const std::wstring& owner,
    const std::wstring& installation_id);
struct ClamAvRebindResult { bool receipt_changed{}, configuration_changed{}, configuration_preserved{}; };
// Consented offline worker only. Accepts the predecessor proved by Prepare's
// protected history. Old configuration paths are compared as text only.
ClamAvRebindResult rebind_clamav_receipt(const std::filesystem::path& root, const std::wstring& owner,
    const std::wstring& installation_id, const std::wstring& predecessor_id,
    const std::filesystem::path& source_root = {}, const std::function<void()>& tick = {});
// Ordinary coordinator only. Creates missing private component roles/config;
// never overwrites an existing modified configuration or adopts unknown paths.
void initialize_clamav_data(const std::filesystem::path& root,const std::wstring& owner);
std::string clamav_update_configuration(const std::filesystem::path& root);
// Strict read-only recognition of this verified component's known private
// metadata only. Unknown children, redirected paths and unsafe files refuse.
bool clamav_bootstrap_metadata(const std::filesystem::path& root,
    const std::filesystem::path& candidate,const std::wstring& owner) noexcept;
// One-time Win7 migration. All exact owned objects are validated and opened
// before anything is removed. No broad tree deletion or implicit destructor work.
class ClamAvRetirement final {
public:
    static std::unique_ptr<ClamAvRetirement> prepare(const std::filesystem::path&,
        const std::wstring& owner,const std::wstring& installation_id);
    ~ClamAvRetirement();
    void retire();
private:
    ClamAvRetirement();
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::win32
