#pragma once
#include "sfx/application/file_storage.hpp"
#include "sfx/application/local_account_administration.hpp"
#include <memory>

namespace sfx::application {
inline constexpr std::size_t kLocalFileConfigurationPageSize=12;
enum class FileConfigurationPart : std::uint8_t { policy, groups, areas, security_rules, caller_exemption };
struct LocalFileConfigurationQuery {
    FileConfigurationPart part{FileConfigurationPart::policy};
    std::int64_t catalog_revision{},after_id{};
    FileGroupId group; // optional filter, areas only
    AccountId account{}; // exact exemption query only
};
struct LocalFileConfigurationSnapshot {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    FileConfigurationPart part{FileConfigurationPart::policy};
    FilePolicy policy{};
    std::vector<FileGroup> groups{};
    std::vector<FileAreaConfiguration> areas{};
    std::vector<FileSecurityRule> security_rules{};
    std::optional<FileCallerExemption> exemption{};
    std::int64_t next_after{};
};
struct LocalFileConfigurationResult {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    FileChangeOutcome result{};
    std::string detail{}; // bounded fixed host wording; no secrets or exception text
    std::uint32_t area_count{},catalog_files{},disk_files{};
};
// A real native directory capability, never a Boolean asserted over IPC. The
// lease survives until the storage CAS transaction has completed. No caller or
// Config UI may substitute a path-validation/success assertion for this port.
class IFileConfigurationLease { public: virtual ~IFileConfigurationLease()=default; virtual void verify() const {} };
class IFileConfigurationPaths {
public:
    virtual ~IFileConfigurationPaths()=default;
    virtual std::unique_ptr<IFileConfigurationLease> acquire(const FileConfiguration&,
        const FileConfigurationChange&)=0;
    virtual LocalFileConfigurationResult group_deletion(IFileStorage&,const FileConfigurationChange&) { return {LocalAccountResultCode::policy_rejected,{},"Stop the BBS before deleting a File Group."}; }
};
bool valid_file_configuration_path(std::string_view) noexcept;
bool valid_file_configuration_change(const FileConfigurationChange&) noexcept;
bool valid_file_configuration_query(const LocalFileConfigurationQuery&) noexcept;
bool valid_file_configuration_snapshot(const LocalFileConfigurationSnapshot&) noexcept;
// Authority is established by the authenticated local native host, not decoded.
class LocalFileAdministration final {
public:
    LocalFileAdministration(IFileStorage& storage,LocalAccountAuthority authority,
        IFileConfigurationPaths* paths=nullptr):storage_(storage),authority_(authority),paths_(paths){}
    LocalFileConfigurationSnapshot snapshot(const LocalFileConfigurationQuery&);
    LocalFileConfigurationResult execute(const FileConfigurationChange&);
private:
    IFileStorage& storage_;
    LocalAccountAuthority authority_;
    IFileConfigurationPaths* paths_;
};
} // namespace sfx::application
