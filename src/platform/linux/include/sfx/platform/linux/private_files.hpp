#pragma once
#include "sfx/platform/linux/child_process.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/account_storage.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::platform::linux_host {
class FileDescriptor final {
public:
    explicit FileDescriptor(int value=-1) noexcept : value_(value) {}
    ~FileDescriptor();
    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;
    FileDescriptor(const FileDescriptor&)=delete;
    FileDescriptor& operator=(const FileDescriptor&)=delete;
    int get() const noexcept { return value_; }
    int release() noexcept { const int result=value_;value_=-1;return result; }
    explicit operator bool() const noexcept { return value_>=0; }
private:
    int value_;
};

// Walk every component without following links. Files opened through this
// descriptor stay relative to the checked directory even if a name moves.
FileDescriptor open_owned_directory(const std::filesystem::path&, bool private_access=true);
bool private_regular_file(int fd, std::uint64_t maximum) noexcept;
std::vector<std::uint8_t> read_private_file(int directory, std::string_view name,
    std::size_t maximum);
// Nonsecret installed resources may be readable by other users, never writable.
// Keep these distinct from credential, database and session-file readers.
FileDescriptor open_owned_resource_directory(int parent, std::string_view name);
std::vector<std::uint8_t> read_owned_resource(int directory, std::string_view name,
    std::size_t maximum);
void publish_private_file(int directory, std::string_view name,
    const std::vector<std::uint8_t>& bytes, bool replace);
bool valid_private_name(std::string_view) noexcept;

class LinuxLocalStoragePathPolicy final : public adapters::storage::IStoragePathPolicy {
public:
    adapters::storage::StorageLocationAssessment assess(const std::filesystem::path&) const override;
    application::StorageStatus create_database_file_exclusive(const std::filesystem::path&) const override;
    application::StorageStatus protect_and_verify_database_file(const std::filesystem::path&) const override;
};
class LinuxSecureIdentifierGenerator final : public application::IIdentifierGenerator {
public:
    application::Identifier128 generate() override;
};

// The locked inode is kept on disk; unlinking a live lock would permit a second
// owner to lock a replacement inode. Exclusion lasts for this descriptor's life.
class InstallationLock final {
public:
    bool acquire(const std::filesystem::path& directory, std::string_view name);
    void release() noexcept;
    explicit operator bool() const noexcept { return static_cast<bool>(file_); }
private:
    FileDescriptor file_;
};

} // namespace sfx::platform::linux_host
