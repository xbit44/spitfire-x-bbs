#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace sfx::platform::win32::installation {

struct CloseHandleDeleter { void operator()(void* value) const noexcept; };
struct LocalFreeDeleter { void operator()(void* value) const noexcept; };
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
using Descriptor = std::unique_ptr<void, LocalFreeDeleter>;
using Path = std::filesystem::path;
// Preserve the machine diagnostic while giving local Setup the exact parent.
class AncestorSecurityError final : public std::runtime_error {
public:
    AncestorSecurityError(const char* code, const Path& path) : std::runtime_error(code), path_(path) {}
    const Path& path() const noexcept { return path_; }
private:
    Path path_;
};
inline constexpr std::array<const wchar_t*, 12> programs{
    L"SFXControlPanel.exe", L"SFXRuntime.exe", L"Tools\\zip.exe", L"Tools\\unzip.exe",
    L"SFXConfig.exe", L"Tools\\sexyz.exe", L"Tools\\SFXQWKTransfer.exe",
    L"Tools\\SFXFileTransfer.exe", L"SFXFileManager.exe", L"SFXMaintenance.exe",
    L"SFXDOSHost.exe", L"SFXDOOR.EXE"};
inline constexpr std::size_t legacy_program_count = 2;
inline constexpr std::size_t archive_program_count = 4;
inline constexpr std::size_t configuration_program_count = 5;
inline constexpr std::size_t message_program_count = 6; // immutable version4 receipt
inline constexpr std::size_t packet_program_count = 7; // immutable version5 receipt
inline constexpr std::size_t file_program_count = 9; // immutable version6 receipt
inline constexpr std::size_t maintenance_program_count = 10; // immutable version7 receipt
inline constexpr std::size_t current_program_count = programs.size();
inline constexpr std::size_t control_panel_program_index = 0;
inline constexpr std::size_t runtime_program_index = 1;
inline constexpr std::size_t configuration_program_index = 4;
inline constexpr std::size_t file_transfer_program_index = 7;
// Stable appended role, unavailable until Setup commits its complete new
// program generation. Never reinterpret an older 2/4/5/6/7-program receipt.
inline constexpr std::size_t file_manager_program_index = 8;
inline constexpr std::size_t maintenance_program_index = 9;
inline constexpr std::size_t dos_host_program_index = 10;
inline constexpr std::size_t dos_launcher_program_index = 11;
inline constexpr std::array<const wchar_t*, 10> mutable_directories{
    L"Config", L"Data", L"Display", L"Files", L"Doors", L"Backup", L"Logs", L"Work", L"BotGate", L"Node"};
inline constexpr std::size_t maximum_receipt_bytes = 4096;

void require(bool condition, const char* diagnostic);
bool elevated();
Handle process_token(DWORD process_id = 0);
std::wstring token_sid(HANDLE token);
std::wstring operator_sid();
std::wstring unique_id();
Path module_path();
// No UNC/device/relative/ADS/dot/short-name aliases; an existing parent on NTFS.
Path canonical_root(const Path& input);
// Read-only ancestor validation. The ordinary token is never used to execute.
std::vector<Handle> protect_ancestors(const Path& root, HANDLE ordinary_token);
Handle protect_installer_cache_parent(const Path& cache, HANDLE ordinary_token);
Handle open_object(const Path& path, bool directory,
    DWORD access = GENERIC_READ | READ_CONTROL,
    DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
std::uint64_t file_id(HANDLE file);
DWORD volume_serial(HANDLE file);
bool exact_security(HANDLE file, void* expected);
Descriptor structural_descriptor(const std::wstring& owner);
Descriptor mutable_descriptor(const std::wstring& owner);
// Exact fixed private directory policy; ordering of its three allow ACEs is immaterial.
// Never used for protected structural/program security.
bool exact_mutable_descriptor(void* actual, const std::wstring& owner);
void create_directory(const Path& path, void* descriptor);
void create_file(const Path& path, const std::vector<std::uint8_t>& bytes, void* descriptor);
std::vector<std::uint8_t> read_file(const Path& path, std::size_t maximum);
std::string sha256(const std::vector<std::uint8_t>& bytes);
std::string file_sha256(const Path& path);

struct DoorComponentIdentity {
    bool registered{false}, introduced{false};
    Path path;
    DWORD volume{};
    std::uint64_t file_id{};
    std::string sha256, compatibility;
};
struct Receipt {
    std::wstring id;
    Path root;
    std::wstring owner;
    std::uint64_t generation{1};
    bool committed{false};
    DWORD volume{0};
    std::uint64_t root_id{0}, bin_id{0};
    std::size_t program_count{legacy_program_count};
    std::array<std::uint64_t, current_program_count> program_ids{};
    std::array<std::string, current_program_count> program_hashes;
    DoorComponentIdentity door_component;
    bool ftn_layout{false};
    DWORD ftn_volume{};
    std::uint64_t ftn_directory_id{};
};

// Version9 explicitly binds the durable FTN root. Version1..8 meanings and
// canonical bytes are unchanged, including version8's DOS component binding.
// Absence of the component grants no DOS launch capability. This is
// not BBS configuration.
std::vector<std::uint8_t> encode_receipt(const Receipt& receipt);
Receipt decode_receipt(const std::vector<std::uint8_t>& bytes);
std::vector<const wchar_t*> mutable_directories_for(const Receipt&);
void verify_ftn_directory(const Receipt&);
Receipt read_receipt(const Path& root, const std::wstring& owner);
struct RegisteredInstallation {
    Receipt receipt;
    bool available{false};
};
// Protected machine records for this operator, including unavailable roots so
// a missing drive never becomes an implicit fresh install. Empty ids discovers
// all records; explicit ids bound native qualification to disposable fixtures.
std::vector<RegisteredInstallation> registered_installations(
    const std::vector<std::wstring>& ids = {}, bool include_history = false,
    const Path& exact_recorded_root = {});
Path suggested_installation_root(const std::vector<RegisteredInstallation>& installations);
// Group authenticated records by folder; this does not validate or authorize
// any incoming record. Discovery establishes availability before grouping.
std::vector<RegisteredInstallation> unique_installation_targets(std::vector<RegisteredInstallation> records);
bool recorded_installation_root(const std::vector<RegisteredInstallation>& installations,const Path& root);
// Elevated Config-only recovery entry. Discovers the owner from the protected
// receipt, validates its binding/security and the exact current Config image.
// Does not change Runtime ownership or permit elevated Runtime startup.
Receipt verify_configuration_recovery_entry(const Path& root);
void verify_structure(const Receipt& receipt, bool verify_programs = true);
struct StructureVerificationTiming {
    enum class Detail { aggregate, open_security, native_identity, file_read, sha };
    bool program{false};
    std::size_t role{0};
    std::uint64_t microseconds{0};
    bool completed{false};
    Detail detail{Detail::aggregate};
};
struct StructureVerificationCallbacks {
    std::function<void()> checkpoint;
    std::function<void(const StructureVerificationTiming&)> timing;
};
// Observation only; the original overload and every identity/hash check remain.
void verify_structure(const Receipt& receipt, bool verify_programs,
                      const StructureVerificationCallbacks& callbacks);
void verify_installed_program_process(const Receipt& receipt, DWORD process_id,
    std::size_t program_index);
// Full hash qualification under read-only native pins. The pins deny content
// replacement/writes for this lease; each use still checks the current receipt,
// layout, descriptors and live process. No elapsed-time or pathname-only cache.
class InstallationVerificationLease final {
public:
    explicit InstallationVerificationLease(const Receipt&);
    ~InstallationVerificationLease();
    InstallationVerificationLease(const InstallationVerificationLease&)=delete;
    InstallationVerificationLease& operator=(const InstallationVerificationLease&)=delete;
    void verify_process(const Receipt&,DWORD,std::size_t) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// These three calls require elevation. Fresh creation never adopts an old key.
void create_receipt(const Receipt& receipt,bool resume_preparation=false);
void replace_receipt(const Receipt& expected, const Receipt& replacement);
void remove_receipt(const Receipt& expected);
Receipt inspect_generation(const Path& root, const std::wstring& owner,
    const std::wstring& id, std::uint64_t generation, bool committed,
    std::size_t program_count = current_program_count,bool ftn_layout = false);
// Normal product entry: no permission repair or directory creation.
void verify_runtime_installation(const Path& root, const std::wstring& owner);

} // namespace sfx::platform::win32::installation
