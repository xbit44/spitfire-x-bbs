#pragma once
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/application/file_storage.hpp"
#include <functional>
#include <memory>

namespace sfx::platform::linux_host {
// Installed by the owning Runtime. Native dev/inode values are temporary
// custody evidence; this port supplies the portable journal identity. A missing
// or unverified binding must fail, never silently adopt a replacement file.
class IFilePayloadBindings {
public:
    virtual ~IFilePayloadBindings() = default;
    virtual application::FileObjectIdentity existing(const std::filesystem::path&,int fd) = 0;
    virtual application::FileObjectIdentity created(const std::filesystem::path&,int fd) = 0;
    virtual void moved(const std::filesystem::path& from,const std::filesystem::path& to,
        int fd,const application::FileObjectIdentity&) = 0;
    virtual void removed(const std::filesystem::path&,const application::FileObjectIdentity&) = 0;
    // A sealed, hashed current object; namespace evidence, not a business commit.
    virtual void checkpoint(const std::filesystem::path&,int,const application::FilePayloadIdentity&) {}
    virtual bool protects_import_permissions() const noexcept {return false;}
    virtual bool can_move(const std::filesystem::path&,const std::filesystem::path&) const {return true;}
    virtual std::filesystem::path resolve_path(const std::filesystem::path& path) const {return path;}
};
inline constexpr std::uint64_t kFilePayloadFreeReserve = 64ULL*1024*1024;
class FilePayloadLease;
class FilePayloadEnumeration;
struct FilePayloadNamePage {std::vector<std::string> names;bool finished{};};
// Paths pass through the installation's authenticated portable path mapping
// before opening or checking protected/overlapping directories.
// Does not create directories or relax their permissions. POSIX flock protects
// cooperating SFX clients; it is not Windows mandatory deny-sharing. Each
// operation also verifies the held object, current name and mutation stamp.
class FilePayloadDirectory final {
public:
    struct Impl;
    static FilePayloadDirectory area(const std::filesystem::path& installation,
        const std::filesystem::path& configured,const std::vector<std::filesystem::path>& others,
        const std::vector<std::filesystem::path>& protected_paths,std::shared_ptr<IFilePayloadBindings>);
    static FilePayloadDirectory private_store(const std::filesystem::path&,
        std::shared_ptr<IFilePayloadBindings>,std::function<std::uint64_t()> available_ceiling = {},
        // Native fault seam only; never supplied by caller/browser input.
        std::function<void()> before_namespace_capture = {});
    static FilePayloadDirectory import_source(const std::filesystem::path&,
        const std::vector<std::filesystem::path>& protected_paths,
        std::shared_ptr<IFilePayloadBindings>,bool explicit_move=false);
    const std::filesystem::path& path() const noexcept;
    void verify() const;
    // Explicit whole-area deletion must reject every unexpected entry rather
    // than silently omit links, nested directories or unsupported filenames.
    std::vector<std::string> strict_names(std::size_t maximum=100000) const;
    std::filesystem::path resolve_path(const std::filesystem::path&) const;
    std::vector<std::string> names(std::size_t maximum=10000) const;
    FilePayloadEnumeration enumerate() const;
    std::uint64_t available_bytes() const;
    void require_space(std::uint64_t) const;
    bool absent(std::string_view) const;
    std::string volume() const;
    FilePayloadLease read(std::string_view,bool permit_mutation=false) const;
    // Sysop in-place import only, after this area was validated. Allows the
    // import binding capability to inspect a new read-only-public source;
    // ordinary private/managed reads retain their strict permission checks.
    FilePayloadDirectory for_in_place_import(std::shared_ptr<IFilePayloadBindings>) const;
    FilePayloadLease create(std::string_view,std::uint64_t maximum_bytes) const;
    FilePayloadLease resume_private(std::string_view,std::uint64_t maximum_bytes,
        const application::FilePayloadIdentity&,
        const std::function<bool(std::uint64_t,std::uint64_t)>& progress = {}) const;
    FilePayloadLease resume_owned_copy(std::string_view,const application::FileObjectIdentity&,
        const FilePayloadLease&,
        const std::function<bool(std::uint64_t,std::uint64_t)>& progress = {}) const;
    FilePayloadLease resume_owned_bytes(std::string_view,const application::FilePayloadIdentity&,
        std::string_view regenerated,
        const std::function<bool(std::uint64_t,std::uint64_t)>& progress = {}) const;
private:
    explicit FilePayloadDirectory(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
    friend class FilePayloadLease;
    friend class FilePayloadEnumeration;
    friend class FtnProcessedExpiry;
};
class FilePayloadEnumeration final {
public:
    ~FilePayloadEnumeration();
    FilePayloadEnumeration(FilePayloadEnumeration&&) noexcept;
    FilePayloadEnumeration& operator=(FilePayloadEnumeration&&) noexcept;
    FilePayloadEnumeration(const FilePayloadEnumeration&)=delete;
    FilePayloadNamePage next(std::size_t limit=100,const std::function<bool()>& keep_going = {});
private:
    struct Impl;
    explicit FilePayloadEnumeration(std::shared_ptr<FilePayloadDirectory::Impl>);
    std::unique_ptr<Impl> impl_;
    friend class FilePayloadDirectory;
};
class FilePayloadLease final {
public:
    struct Impl;
    ~FilePayloadLease();
    FilePayloadLease(FilePayloadLease&&) noexcept;
    FilePayloadLease& operator=(FilePayloadLease&&) noexcept;
    FilePayloadLease(const FilePayloadLease&)=delete;
    FilePayloadLease& operator=(const FilePayloadLease&)=delete;
    std::string name() const;
    const std::filesystem::path& path() const noexcept;
    std::vector<std::uint8_t> read(std::uint64_t offset,std::size_t maximum=65536) const;
    void append(const std::uint8_t*,std::size_t);
    // Native supervisor only. Receives require a fresh, journal-bound private
    // stage and real filesystem preallocation before a write-only descriptor
    // can escape. Keep this lease alive, close the parent's returned descriptor
    // and reap every child holding it before finish_receive_transfer_after_reap.
    // Completion seals any partial bytes; it never means protocol acceptance.
    FileDescriptor begin_receive_transfer();
    void finish_receive_transfer_after_reap();
    // Separate open description with reduced access, never a writable dup.
    // The owner retains this lease and checks its identity after child reap.
    FileDescriptor send_transfer_descriptor() const;
    void flush();
    application::FileObjectIdentity object_identity() const;
    application::FileObjectIdentity native_object_identity() const;
    std::uint64_t byte_count() const;
    application::FilePayloadIdentity identity(
        const std::function<bool(std::uint64_t,std::uint64_t)>& progress = {}) const;
    void rename_to(const FilePayloadDirectory&,std::string_view);
    // Native move is usable only within the same filesystem and binding scope.
    // Callers otherwise retain staging and perform a separately journaled copy.
    bool can_rename_to(const FilePayloadDirectory&) const;
    void remove_exact();
private:
    explicit FilePayloadLease(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
    friend class FilePayloadDirectory;
    friend class FtnProcessedExpiry;
};
} // namespace sfx::platform::linux_host
