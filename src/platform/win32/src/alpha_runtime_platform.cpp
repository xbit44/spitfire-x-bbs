#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"
#include "sfx/application/external_events.hpp"
#include "sfx/application/external_event_files.hpp"

#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/backup_collections.hpp"

#include <Aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <set>
#include <system_error>
#include <vector>

namespace sfx::platform::win32 {
namespace {

using application::StorageErrorCode;
using application::StorageStatus;

struct LocalFreeDeleter {
    void operator()(void* value) const noexcept { if (value != nullptr) LocalFree(value); }
};

using LocalPointer = std::unique_ptr<void, LocalFreeDeleter>;

class SecurityAttributes final {
public:
    explicit SecurityAttributes(const std::wstring& owner_sid) {
        const std::wstring sddl = L"D:P(A;;GA;;;" + owner_sid +
            L")(A;;GA;;;SY)(A;;GA;;;BA)";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!owner_sid.empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            descriptor_.reset(descriptor);
            attributes_.nLength = sizeof(attributes_);
            attributes_.lpSecurityDescriptor = descriptor_.get();
            attributes_.bInheritHandle = FALSE;
        }
    }
    SECURITY_ATTRIBUTES* get() noexcept {
        return descriptor_ ? &attributes_ : nullptr;
    }
private:
    LocalPointer descriptor_{nullptr};
    SECURITY_ATTRIBUTES attributes_{};
};

StorageStatus failure(const StorageErrorCode code, std::string operation) {
    return {code, std::move(operation)};
}

bool absolute_local_path(const std::filesystem::path& value) {
    if (!value.is_absolute()) return false;
    const std::wstring native = value.native();
    return native.rfind(L"\\\\", 0) != 0 && native.rfind(L"\\\\?\\", 0) != 0 &&
        native.rfind(L"\\\\.\\", 0) != 0;
}

bool has_reparse_component(std::filesystem::path value) {
    for (;;) {
        const DWORD attributes = GetFileAttributesW(value.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
        const auto parent = value.parent_path();
        if (parent.empty() || parent == value) return false;
        value = parent;
    }
}

std::filesystem::path configuration_path(const std::filesystem::path& root) {
    return root / L"Config" / L"SFX.conf";
}

bool read_exact(HANDLE handle, void* destination, DWORD bytes);
bool write_exact(HANDLE handle, const void* source, DWORD bytes);

std::filesystem::path pending_configuration_path(const std::filesystem::path& root) {
    return root / L"Config" / L"SFX.pending.conf";
}

std::filesystem::path scheduler_configuration_path(const std::filesystem::path& root,std::string_view job=application::kBackupJobId) {
    if(job==application::kMessageRetentionJobId)return root/L"Config"/L"SFXMessageRetention.conf";
    if(job==application::kMessageIntegrityJobId)return root/L"Config"/L"SFXMessageIntegrity.conf";
    if(job!=application::kBackupJobId)return {};
    return root / L"Config" / L"SFXScheduler.conf";
}

std::filesystem::path scheduler_state_path(const std::filesystem::path& root,std::string_view job=application::kBackupJobId) {
    if(job==application::kMessageRetentionJobId)return root/L"Data"/L"SFXMessageRetention.state";
    if(job==application::kMessageIntegrityJobId)return root/L"Data"/L"SFXMessageIntegrity.state";
    if(job!=application::kBackupJobId)return {};
    return root / L"Data" / L"SFXScheduler.state";
}

ConfigurationFileReadResult read_configuration_at(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {GetLastError() == ERROR_FILE_NOT_FOUND
                ? ConfigurationFileState::missing : ConfigurationFileState::io_failure,
            std::nullopt,
            GetLastError() == ERROR_FILE_NOT_FOUND ? "configuration_missing" : "configuration_open_failed"};
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    LARGE_INTEGER size{};
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(application::kOperationalConfigurationMaximumBytes)) {
        CloseHandle(file);
        return {ConfigurationFileState::invalid, std::nullopt, "configuration_invalid_file"};
    }
    std::string serialized(static_cast<std::size_t>(size.QuadPart), '\0');
    const bool read = read_exact(file, serialized.data(), static_cast<DWORD>(serialized.size()));
    CloseHandle(file);
    if (!read) return {ConfigurationFileState::io_failure, std::nullopt, "configuration_read_failed"};
    auto parsed = application::parse_operational_configuration(serialized);
    if (!parsed.valid()) return {ConfigurationFileState::invalid, std::nullopt, "configuration_invalid"};
    return {ConfigurationFileState::loaded, std::move(parsed.configuration), "configuration_loaded"};
}

StorageStatus publish_protected_text(const std::filesystem::path& final_path,
    const std::wstring& owner_sid, const std::string_view serialized,
    const std::string_view operation) {
    const std::filesystem::path pending_path = final_path.native() + L".writing";
    const HANDLE pending = CreateFileW(pending_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (pending == INVALID_HANDLE_VALUE) {
        return failure(StorageErrorCode::io, "create pending " + std::string(operation));
    }
    const bool written = write_exact(pending, serialized.data(),
        static_cast<DWORD>(serialized.size())) && FlushFileBuffers(pending) != FALSE;
    CloseHandle(pending);
    if (!written) {
        DeleteFileW(pending_path.c_str());
        return failure(StorageErrorCode::io, "write pending " + std::string(operation));
    }
    WindowsStorageDaclPolicy policy(owner_sid);
    const auto protected_status = policy.apply_for_publication(pending_path, StorageDaclObjectKind::file, final_path);
    if (!protected_status) {
        DeleteFileW(pending_path.c_str());
        return protected_status;
    }
    if (!MoveFileExW(pending_path.c_str(), final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending_path.c_str());
        return failure(StorageErrorCode::io, "publish " + std::string(operation));
    }
    const auto verified = policy.assess(final_path, StorageDaclObjectKind::file);
    return verified.status;
}

std::optional<std::string> read_small_text(const std::filesystem::path& path,
    const DWORD maximum) noexcept {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > maximum) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string value(static_cast<std::size_t>(size.QuadPart), '\0');
    const bool read = read_exact(file, value.data(), static_cast<DWORD>(value.size()));
    CloseHandle(file);
    return read ? std::optional<std::string>(std::move(value)) : std::nullopt;
}

template <typename Value, typename Parser>
SchedulerFileReadResult<Value> read_scheduler_file(
    const std::filesystem::path& path, const std::wstring& owner_sid,
    Parser parser, const char* label) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return {error == ERROR_FILE_NOT_FOUND
                    ? ConfigurationFileState::missing
                    : ConfigurationFileState::io_failure,
                std::nullopt,
                error == ERROR_FILE_NOT_FOUND
                    ? std::string(label) + "_missing"
                    : std::string(label) + "_open_failed"};
    }
    WindowsStorageDaclPolicy policy(owner_sid);
    if (!policy.assess(path, StorageDaclObjectKind::file).status) {
        return {ConfigurationFileState::invalid, std::nullopt,
                std::string(label) + "_protection_invalid"};
    }
    const auto text = read_small_text(
        path, static_cast<DWORD>(application::kSchedulerDocumentMaximumBytes));
    if (!text) {
        return {ConfigurationFileState::invalid, std::nullopt,
                std::string(label) + "_invalid_file"};
    }
    auto parsed = parser(*text);
    if (!parsed) {
        return {ConfigurationFileState::invalid, std::nullopt,
                std::string(label) + "_invalid"};
    }
    return {ConfigurationFileState::loaded, std::move(parsed),
            std::string(label) + "_loaded"};
}

bool system_time_less(const SYSTEMTIME& left, const SYSTEMTIME& right) noexcept {
    const std::array<WORD, 6> lhs{{left.wYear, left.wMonth, left.wDay,
                                  left.wHour, left.wMinute, left.wSecond}};
    const std::array<WORD, 6> rhs{{right.wYear, right.wMonth, right.wDay,
                                  right.wHour, right.wMinute, right.wSecond}};
    return lhs < rhs;
}

bool add_local_day(SYSTEMTIME& value) noexcept {
    SYSTEMTIME date = value;
    date.wHour = 12;
    date.wMinute = date.wSecond = date.wMilliseconds = 0;
    FILETIME file{};
    ULARGE_INTEGER ticks{};
    if (!SystemTimeToFileTime(&date, &file)) return false;
    ticks.LowPart = file.dwLowDateTime;
    ticks.HighPart = file.dwHighDateTime;
    ticks.QuadPart += 24ULL * 60ULL * 60ULL * 10000000ULL;
    file.dwLowDateTime = ticks.LowPart;
    file.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME next{};
    if (!FileTimeToSystemTime(&file, &next)) return false;
    value.wYear = next.wYear;
    value.wMonth = next.wMonth;
    value.wDay = next.wDay;
    return true;
}

bool unix_to_system_time(const std::int64_t seconds, SYSTEMTIME& value) noexcept {
    if (seconds < -11644473600LL ||
        seconds > (std::numeric_limits<std::int64_t>::max() / 10000000LL) -
                      11644473600LL) return false;
    const std::int64_t raw = (seconds + 11644473600LL) * 10000000LL;
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<ULONGLONG>(raw);
    FILETIME file{ticks.LowPart, ticks.HighPart};
    return FileTimeToSystemTime(&file, &value) != FALSE;
}

std::optional<std::int64_t> system_time_to_unix(const SYSTEMTIME& value) noexcept {
    FILETIME file{};
    if (!SystemTimeToFileTime(&value, &file)) return std::nullopt;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file.dwLowDateTime;
    ticks.HighPart = file.dwHighDateTime;
    if (ticks.QuadPart < 116444736000000000ULL) return std::nullopt;
    return static_cast<std::int64_t>(
        (ticks.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

bool read_exact(const HANDLE handle, void* destination, const DWORD bytes) {
    auto* cursor = static_cast<std::uint8_t*>(destination);
    DWORD remaining = bytes;
    while (remaining != 0) {
        DWORD received = 0;
        if (!ReadFile(handle, cursor, remaining, &received, nullptr) || received == 0) return false;
        cursor += received;
        remaining -= received;
    }
    return true;
}

bool write_exact(const HANDLE handle, const void* source, const DWORD bytes) {
    const auto* cursor = static_cast<const std::uint8_t*>(source);
    DWORD remaining = bytes;
    while (remaining != 0) {
        DWORD sent = 0;
        if (!WriteFile(handle, cursor, remaining, &sent, nullptr) || sent == 0) return false;
        cursor += sent;
        remaining -= sent;
    }
    return true;
}

std::uint32_t read_u32_le(const std::uint8_t* value) noexcept {
    return static_cast<std::uint32_t>(value[0]) |
        (static_cast<std::uint32_t>(value[1]) << 8U) |
        (static_cast<std::uint32_t>(value[2]) << 16U) |
        (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::uint64_t fnv1a64(const std::wstring& value) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (const wchar_t character : value) {
        const std::uint16_t word = static_cast<std::uint16_t>(character);
        result ^= static_cast<std::uint8_t>(word & 0xffU);
        result *= 1099511628211ULL;
        result ^= static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        result *= 1099511628211ULL;
    }
    return result;
}

bool same_sid_string(const PSID sid, const std::wstring& expected) {
    wchar_t* converted = nullptr;
    if (sid == nullptr || !ConvertSidToStringSidW(sid, &converted)) return false;
    LocalPointer holder(converted);
    return _wcsicmp(converted, expected.c_str()) == 0;
}

bool process_user_sid_matches(
    const DWORD process_id,
    const std::wstring& expected_sid) noexcept {
    const HANDLE raw_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (raw_process == nullptr) return false;
    const auto close_handle = [](void* value) { if (value != nullptr) CloseHandle(value); };
    const std::unique_ptr<void, decltype(close_handle)> process(raw_process, close_handle);
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(raw_process, TOKEN_QUERY, &raw_token)) return false;
    const std::unique_ptr<void, decltype(close_handle)> token(raw_token, close_handle);
    DWORD bytes = 0;
    (void)GetTokenInformation(raw_token, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<std::uint8_t> buffer(bytes);
    if (!GetTokenInformation(raw_token, TokenUser, buffer.data(), bytes, &bytes)) return false;
    return same_sid_string(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid,
        expected_sid);
}

}  // namespace

std::filesystem::path alpha_installation_root() {
#ifdef SFX_ALPHA_TEST_INSTALL_ROOT
    // CMake represents an absolute cache path with forward slashes. Normalize
    // the test-only override to the same canonical DOS spelling used by the
    // protected-installation receipt before any exact handle/path check.
    return installation::canonical_root(std::filesystem::path(SFX_ALPHA_TEST_INSTALL_ROOT));
#else
    const auto binary = installation::module_path();
    installation::require(_wcsicmp(binary.parent_path().filename().c_str(), L"Bin") == 0,
        "installation_launch_from_protected_bin_required");
    return installation::canonical_root(binary.parent_path().parent_path());
#endif
}

InstallationObjectNames installation_object_names(
    const std::filesystem::path& installation_root) {
    const std::uint64_t hash = fnv1a64(lower(installation_root.native()));
    std::wostringstream encoded;
    encoded << std::hex << std::setw(16) << std::setfill(L'0') << hash;
    return {L"\\\\.\\pipe\\SFX-Control-" + encoded.str(),
        L"Global\\SFXRuntime-" + encoded.str(),
        L"\\\\.\\pipe\\SFX-Configuration-" + encoded.str(),
        L"Global\\SFXConfiguration-" + encoded.str()};
}

StorageStatus provision_alpha_installation_tree(
    const std::filesystem::path& root, const std::wstring& owner) {
    // Compatibility name only: ordinary startup now verifies; Setup alone creates.
    try {
        installation::verify_runtime_installation(root, owner);
        WindowsStorageDaclPolicy policy(owner);
        const auto receipt=installation::read_receipt(root,owner);
        for (const auto* leaf : installation::mutable_directories_for(receipt)) {
            if (has_reparse_component(root / leaf) ||
                !policy.assess(root / leaf, StorageDaclObjectKind::directory).status)
                return failure(StorageErrorCode::permission_denied, "installation_mutable_boundary_invalid");
        }
        return StorageStatus::success();
    } catch (const std::exception& e) {
        return failure(StorageErrorCode::permission_denied, e.what());
    }
}

StorageStatus assess_alpha_bin_directory(
    const std::filesystem::path& root, const std::wstring& owner) {
    try {
        const auto receipt = installation::read_receipt(root, owner);
        installation::verify_structure(receipt);
        return StorageStatus::success();
    } catch (const std::exception& e) {
        return failure(StorageErrorCode::permission_denied, e.what());
    }
}

ConfigurationFileReadResult read_operational_configuration_file(
    const std::filesystem::path& installation_root) {
    return read_configuration_at(configuration_path(installation_root));
}

StorageStatus reconcile_security_snapshot(const std::filesystem::path& root,
    const std::wstring& owner, const application::LocalSecuritySnapshot& truth,
    application::OperationalConfiguration& configuration) {
    const auto& policy = truth.policy;
    const auto definition = application::security_policy_definition(truth.levels,
        policy.new_user_level, policy.sysop_threshold);
    if (truth.code != application::LocalAccountResultCode::ok || definition.empty() ||
        policy.revision < 1 || policy.published_revision != policy.revision ||
        policy.published_policy_hash != policy.policy_hash ||
        !application::valid_sha256_text(policy.policy_hash) ||
        configuration.security_policy_revision > policy.revision ||
        (configuration.security_policy_revision == policy.revision &&
            configuration.security_policy_hash != policy.policy_hash))
        return failure(StorageErrorCode::conflict,"security_snapshot_binding_mismatch");
    const auto path = root / L"Config" / L"SFXSecurity.conf";
    const std::string prefix = "SFX_SECURITY_POLICY=1\nrevision=";
    const auto expected = prefix + std::to_string(policy.revision) + "\nhash=" + policy.policy_hash +
        "\n" + definition;
    const auto attributes = GetFileAttributesW(path.c_str());
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
    WindowsStorageDaclPolicy security(owner);
    if (!security.assess(root / L"Config", StorageDaclObjectKind::directory).status ||
        !security.assess(root / L"Config" / L"SFX.conf", StorageDaclObjectKind::file).status)
        return failure(StorageErrorCode::permission_denied,"security_snapshot_boundary");
    bool stale = missing;
    if (!missing) {
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !security.assess(path,StorageDaclObjectKind::file).status)
            return failure(StorageErrorCode::permission_denied,"security_snapshot_file_unavailable");
        const auto existing = read_small_text(path, 1024U*1024U);
        if (!existing || existing->compare(0,prefix.size(),prefix) != 0)
            return failure(StorageErrorCode::conflict,"security_snapshot_invalid");
        const auto end = existing->find('\n',prefix.size());
        std::int64_t revision = 0;
        if (end == std::string::npos) return failure(StorageErrorCode::conflict,"security_snapshot_invalid");
        const auto parsed = std::from_chars(existing->data()+prefix.size(),existing->data()+end,revision);
        if (parsed.ec != std::errc{} || parsed.ptr != existing->data()+end || revision < 1 ||
            revision > policy.revision || (revision == policy.revision && *existing != expected))
            return failure(StorageErrorCode::conflict,"security_snapshot_mismatch");
        stale = revision < policy.revision;
    }
    if (stale) {
        const auto staging = path.native() + L".writing";
        if (GetFileAttributesW(staging.c_str()) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
            return failure(StorageErrorCode::conflict,"security_snapshot_staging_present");
        const auto saved = publish_protected_text(path,owner,expected,"security policy snapshot");
        if (!saved) return saved;
        if (read_small_text(path,1024U*1024U) != expected)
            return failure(StorageErrorCode::io,"security_snapshot_verify_failed");
    }
    if (configuration.security_policy_revision < policy.revision) {
        if (configuration.configuration_revision == std::numeric_limits<std::uint64_t>::max())
            return failure(StorageErrorCode::conflict,"security_snapshot_configuration_revision_exhausted");
        auto replacement = configuration;
        ++replacement.configuration_revision;
        replacement.security_policy_revision = policy.revision;
        replacement.security_policy_hash = policy.policy_hash;
        const auto published = publish_operational_configuration_file(root,owner,replacement);
        if (!published) return published;
        configuration = std::move(replacement);
    }
    return StorageStatus::success();
}

bool stock_docs_bootstrap_metadata(const std::filesystem::path& root,
    const std::filesystem::path& candidate,const std::wstring& owner) noexcept {
    try {
        if(candidate.parent_path()!=root/L"Config")return false;
        const auto name=candidate.filename().native();
        const std::wstring prefix=L"stock-docs-legacy-";
        const bool current=name==L"stock-docs.v1";
        const bool legacy=name.size()==prefix.size()+64+3&&name.rfind(prefix,0)==0&&name.substr(name.size()-3)==L".v1";
        if(!current&&!legacy)return false;
        WindowsStorageDaclPolicy policy(owner);
        auto held=installation::open_object(candidate,false,GENERIC_READ|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(held.get(),&info)||info.nNumberOfLinks!=1||info.nFileSizeHigh||
            info.nFileSizeLow<=11||info.nFileSizeLow>16384||
            (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
            !policy.assess(candidate,StorageDaclObjectKind::file).status)return false;
        std::vector<std::uint8_t> bytes(info.nFileSizeLow);DWORD read{};
        if(!ReadFile(held.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size())return false;
        if(legacy){const auto hash=installation::sha256(bytes);if(name.substr(prefix.size(),64)!=std::wstring(hash.begin(),hash.end()))return false;}
        const std::string text(bytes.begin(),bytes.end());if(text.rfind("SFX-DOCS-1\n",0)!=0)return false;
        std::set<std::string> paths;std::size_t at=11;
        while(at<text.size()){
            const auto end=text.find('\n',at),bar=text.find('|',at);
            if(end==std::string::npos||bar==std::string::npos||bar>=end)return false;
            auto path=text.substr(at,bar-at);const auto hash=text.substr(bar+1,end-bar-1);
            if(path.empty()||path.size()>512||(path!="README.txt"&&path.rfind("Docs/",0)!=0)||
                hash.size()!=64||hash.find_first_not_of("0123456789ABCDEF")!=std::string::npos)return false;
            for(unsigned char c:path)if(c<32||c>=127||c=='\\'||c==':'||c=='|')return false;
            std::size_t part{};
            while(part<=path.size()){
                const auto slash=path.find('/',part);const auto component=path.substr(part,slash==std::string::npos?path.size()-part:slash-part);
                if(component.empty()||component=="."||component==".."||component.back()=='.'||component.back()==' ')return false;
                if(slash==std::string::npos)break;part=slash+1;
            }
            std::transform(path.begin(),path.end(),path.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
            if(!paths.insert(path).second)return false;at=end+1;
        }
        return !paths.empty();
    }catch(...){return false;}
}

application::StorageResult<bool> alpha_installation_has_persistent_state(
    const std::filesystem::path& installation_root, const std::wstring& owner_sid,
    const std::function<bool(const std::filesystem::path&)>& verified_component_metadata) {
    WindowsStorageDaclPolicy policy(owner_sid);
    const auto rejected = [] {
        return application::StorageResult<bool>{failure(StorageErrorCode::permission_denied,
            "installation_state_inspection_failed"), std::nullopt};
    };
    if (!absolute_local_path(installation_root) || has_reparse_component(installation_root) ||
        !assess_alpha_bin_directory(installation_root, owner_sid)) return rejected();
    bool present = false;
    std::vector<installation::Handle> pre_setup_backup_leases;
    for (const wchar_t* leaf : {L"Config", L"Data", L"Backup", L"Logs"}) {
        const auto directory = installation_root / leaf;
        if (has_reparse_component(directory) ||
            !policy.assess(directory, StorageDaclObjectKind::directory).status) return rejected();
        const auto pattern = directory / L"*";
        // Pin each parent against replacement while inspecting it. Do not follow
        // or read discovered objects: even an empty/renamed identity is evidence.
        const HANDLE parent = CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (parent == INVALID_HANDLE_VALUE) return rejected();
        WIN32_FIND_DATAW data{};
        const HANDLE search = FindFirstFileW(pattern.c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            CloseHandle(parent);
            if (error == ERROR_FILE_NOT_FOUND) continue;
            return rejected();
        }
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            // Optional Setup component metadata is not an initialized BBS.
            // Only the host's exact, read-only verifier may classify it; no
            // blanket name/empty-directory exemption or error waiver.
            if (verified_component_metadata &&
                verified_component_metadata(directory / data.cFileName)) continue;
            // Setup's empty private backup and intake destinations are layout metadata,
            // not initialized BBS data. Recognize ONLY these exact empty,
            // nonredirected, privately protected children. Every uncertain or
            // nonempty object remains conservative persistent-state evidence.
            if (((wcscmp(leaf, L"Backup") == 0 &&
                (_wcsicmp(data.cFileName, L"Upgrades") == 0 ||
                 _wcsicmp(data.cFileName, L"Scheduled") == 0 ||
                 _wcsicmp(data.cFileName, L"Recovery") == 0)) ||
                (wcscmp(leaf, L"Data") == 0 &&
                 (_wcsicmp(data.cFileName, L"FileIntake") == 0 ||
                  _wcsicmp(data.cFileName, L"FileRemoved") == 0))) &&
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                try {
                    const auto path = directory / data.cFileName;
                    auto child = installation::open_object(path, true,
                        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE);
                    const bool setup_recovery=wcscmp(leaf,L"Backup")==0&&_wcsicmp(data.cFileName,L"Recovery")==0;
                    auto structural=installation::structural_descriptor(owner_sid);
                    if (policy.assess(path, StorageDaclObjectKind::directory).status ||
                        (setup_recovery&&installation::exact_security(child.get(),structural.get()))) {
                        WIN32_FIND_DATAW member{};
                        const auto nested = FindFirstFileW((path / L"*").c_str(), &member);
                        if (nested == INVALID_HANDLE_VALUE) {
                            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
                        } else {
                            bool empty = true;
                            do {
                                if (wcscmp(member.cFileName, L".") && wcscmp(member.cFileName, L"..")) {
                                    empty = false; break;
                                }
                            } while (FindNextFileW(nested, &member));
                            const auto nested_error = GetLastError();
                            FindClose(nested);
                            if (empty && nested_error == ERROR_NO_MORE_FILES) continue;
                            if (!empty && wcscmp(leaf, L"Backup") == 0 &&
                                wcscmp(data.cFileName, L"Upgrades") == 0 &&
                                verified_pre_setup_upgrade_backups(installation_root, owner_sid,
                                    pre_setup_backup_leases)) continue;
                        }
                    }
                } catch (...) {
                    // No permission repair, recursion or uncertain setup waiver.
                }
            }
            // The just-acquired instance guard creates this empty lock itself;
            // it is not evidence of initialized BBS storage. A nonempty or
            // redirected object still fails conservative classification.
            if (wcscmp(leaf, L"Data") == 0 && _wcsicmp(data.cFileName, L"SFXRuntime.lock") == 0 &&
                !data.nFileSizeHigh && !data.nFileSizeLow &&
                !(data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
            if (wcscmp(leaf, L"Config") == 0 &&
                _wcsicmp(data.cFileName, L"SFXControlPanel.ini") == 0 &&
                !(data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
            // Setup creates protected, empty optional list files
            // before the first Control Panel launch. It is installer metadata,
            // not evidence that first-run storage/configuration ever started.
            // Hold the exact file against writes/replacement while verifying its
            // zero length and private policy. A nonempty, redirected, insecure or
            // unreadable list remains conservative persistent-state evidence.
            if (wcscmp(leaf, L"Config") == 0 &&
                (_wcsicmp(data.cFileName,L"JOKER.DAT")==0 || _wcsicmp(data.cFileName,L"otherbbs.txt")==0) &&
                !data.nFileSizeHigh && !data.nFileSizeLow &&
                !(data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                try {
                    const auto path = directory / data.cFileName;
                    auto joker = installation::open_object(path, false,
                        GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ);
                    LARGE_INTEGER size{};
                    if (GetFileSizeEx(joker.get(), &size) && size.QuadPart == 0 &&
                        policy.assess(path, StorageDaclObjectKind::file).status) continue;
                } catch (...) {
                    // Fall through: uncertain metadata must never enable setup.
                }
            }
            present = true;
        } while (FindNextFileW(search, &data));
        const auto error = GetLastError();
        FindClose(search);
        CloseHandle(parent);
        if (error != ERROR_NO_MORE_FILES) return rejected();
    }
    return {StorageStatus::success(), present};
}

ConfigurationFileReadResult read_pending_operational_configuration_file(
    const std::filesystem::path& installation_root) {
    return read_configuration_at(pending_configuration_path(installation_root));
}

StorageStatus publish_operational_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::OperationalConfiguration& configuration) {
    const std::string serialized = application::serialize_operational_configuration(configuration);
    if (serialized.empty()) {
        return failure(StorageErrorCode::invalid_argument, "validate operational configuration publication");
    }
    const std::filesystem::path final_path = configuration_path(installation_root);
    const std::filesystem::path pending_path = final_path.parent_path() / L"SFX.conf.pending";
    const HANDLE pending = CreateFileW(pending_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (pending == INVALID_HANDLE_VALUE) {
        return failure(StorageErrorCode::io, "create pending operational configuration");
    }
    const bool written = write_exact(pending, serialized.data(), static_cast<DWORD>(serialized.size())) &&
        FlushFileBuffers(pending) != FALSE;
    CloseHandle(pending);
    if (!written) {
        DeleteFileW(pending_path.c_str());
        return failure(StorageErrorCode::io, "write pending operational configuration");
    }
    WindowsStorageDaclPolicy policy(installation_owner_sid);
    const auto protected_status = policy.apply_for_publication(pending_path, StorageDaclObjectKind::file, final_path);
    if (!protected_status) {
        DeleteFileW(pending_path.c_str());
        return protected_status;
    }
    if (!MoveFileExW(pending_path.c_str(), final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return failure(StorageErrorCode::io, "publish operational configuration atomically");
    }
    const auto verified_dacl = policy.assess(final_path, StorageDaclObjectKind::file);
    if (!verified_dacl.status) return verified_dacl.status;
    const auto read_back = read_operational_configuration_file(installation_root);
    if (read_back.state != ConfigurationFileState::loaded || !read_back.configuration ||
        !application::equivalent_operational_configuration(configuration, *read_back.configuration)) {
        return failure(StorageErrorCode::io, "verify operational configuration publication");
    }
    return StorageStatus::success();
}

StorageStatus publish_pending_operational_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::OperationalConfiguration& configuration) {
    const std::string serialized = application::serialize_operational_configuration(configuration);
    if (serialized.empty()) {
        return failure(StorageErrorCode::invalid_argument,
            "validate pending operational configuration");
    }
    return publish_protected_text(pending_configuration_path(installation_root),
        installation_owner_sid, serialized, "pending operational configuration");
}

StorageStatus remove_pending_operational_configuration_file(
    const std::filesystem::path& installation_root) {
    if (DeleteFileW(pending_configuration_path(installation_root).c_str())) return StorageStatus::success();
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? StorageStatus::success() : failure(StorageErrorCode::io, "configuration_pending_remove_failed");
}

SchedulerConfigurationFileReadResult read_scheduler_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,const std::string_view job) {
    if(job!=application::kBackupJobId && !application::message_maintenance_job(job))return {ConfigurationFileState::invalid,{},"scheduler_job_unknown"};
    auto result=read_scheduler_file<application::SchedulerConfiguration>(
        scheduler_configuration_path(installation_root,job), installation_owner_sid,
        application::parse_scheduler_configuration, "scheduler_configuration");
    if(result.value && result.value->job_id!=job)return {ConfigurationFileState::invalid,{},"scheduler_job_binding_invalid"};
    return result;
}

SchedulerStateFileReadResult read_scheduler_state_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,const std::string_view job) {
    if(job!=application::kBackupJobId && !application::message_maintenance_job(job))return {ConfigurationFileState::invalid,{},"scheduler_job_unknown"};
    auto result=read_scheduler_file<application::SchedulerState>(
        scheduler_state_path(installation_root,job), installation_owner_sid,
        application::parse_scheduler_state, "scheduler_state");
    if(result.value && result.value->job_id!=job)return {ConfigurationFileState::invalid,{},"scheduler_job_binding_invalid"};
    return result;
}

StorageStatus publish_scheduler_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::SchedulerConfiguration& configuration) {
    const auto text = application::serialize_scheduler_configuration(configuration);
    if (text.empty()) {
        return failure(StorageErrorCode::invalid_argument,
                       "validate scheduler configuration publication");
    }
    const auto published = publish_protected_text(
        scheduler_configuration_path(installation_root,configuration.job_id), installation_owner_sid,
        text, "scheduler configuration");
    if (!published) return published;
    const auto read_back = read_scheduler_configuration_file(
        installation_root, installation_owner_sid,configuration.job_id);
    return read_back.state == ConfigurationFileState::loaded && read_back.value &&
                   application::serialize_scheduler_configuration(*read_back.value) == text
               ? StorageStatus::success()
               : failure(StorageErrorCode::io,
                         "verify scheduler configuration publication");
}

StorageStatus publish_scheduler_state_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::SchedulerState& state) {
    const auto text = application::serialize_scheduler_state(state);
    if (text.empty()) {
        return failure(StorageErrorCode::invalid_argument,
                       "validate scheduler state publication");
    }
    const auto published = publish_protected_text(
        scheduler_state_path(installation_root,state.job_id), installation_owner_sid,
        text, "scheduler state");
    if (!published) return published;
    const auto read_back = read_scheduler_state_file(
        installation_root, installation_owner_sid,state.job_id);
    return read_back.state == ConfigurationFileState::loaded && read_back.value &&
                   application::serialize_scheduler_state(*read_back.value) == text
               ? StorageStatus::success()
               : failure(StorageErrorCode::io,
                         "verify scheduler state publication");
}

std::optional<std::int64_t> next_local_daily_run(
    const std::int64_t after_utc, const std::uint8_t local_hour,
    const std::uint8_t local_minute) noexcept {
    if (local_hour >= 24 || local_minute >= 60) return std::nullopt;
    SYSTEMTIME utc_after{};
    SYSTEMTIME local_after{};
    if (!unix_to_system_time(after_utc, utc_after) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc_after, &local_after)) {
        return std::nullopt;
    }
    SYSTEMTIME candidate = local_after;
    candidate.wHour = local_hour;
    candidate.wMinute = local_minute;
    candidate.wSecond = candidate.wMilliseconds = 0;
    if (!system_time_less(local_after, candidate) && !add_local_day(candidate)) {
        return std::nullopt;
    }
    for (unsigned day = 0; day < 3; ++day) {
        SYSTEMTIME attempted = candidate;
        // A spring-forward schedule in the missing local interval advances to
        // the first representable minute. Fall-back ambiguity is resolved by
        // Windows consistently for the machine's active time-zone rules.
        for (unsigned minute = 0; minute <= 180; ++minute) {
            SYSTEMTIME utc_candidate{};
            if (TzSpecificLocalTimeToSystemTime(nullptr, &attempted,
                                                &utc_candidate)) {
                const auto value = system_time_to_unix(utc_candidate);
                if (value && *value > after_utc) return value;
                break;
            }
            FILETIME file{};
            if (!SystemTimeToFileTime(&attempted, &file)) break;
            ULARGE_INTEGER ticks{};
            ticks.LowPart = file.dwLowDateTime;
            ticks.HighPart = file.dwHighDateTime;
            ticks.QuadPart += 60ULL * 10000000ULL;
            file.dwLowDateTime = ticks.LowPart;
            file.dwHighDateTime = ticks.HighPart;
            if (!FileTimeToSystemTime(&file, &attempted)) break;
        }
        if (!add_local_day(candidate)) return std::nullopt;
    }
    return std::nullopt;
}

std::optional<application::ControlPanelUiPreferences> read_control_panel_ui_preferences(
    const std::filesystem::path& installation_root) noexcept {
    const auto text = read_small_text(installation_root / L"Config" / L"SFXControlPanel.ini", 1024);
    return text ? application::parse_control_panel_ui_preferences(*text) : std::nullopt;
}

StorageStatus publish_control_panel_ui_preferences(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::ControlPanelUiPreferences& preferences) {
    return publish_protected_text(installation_root / L"Config" / L"SFXControlPanel.ini",
        installation_owner_sid, application::serialize_control_panel_ui_preferences(preferences),
        "Control Panel UI preferences");
}

bool read_node_one_enabled(const std::filesystem::path& installation_root) noexcept {
    const auto path = installation_root / L"Config" / L"SFXNode1.state";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        return true;
    const auto text = read_small_text(path, 128);
    return text && *text == "SFX_NODE_CONTROL=1\nnode1_enabled=1\n";
}

namespace {
std::string node_controls_text(const application::NodeEnablement& enabled) {
    std::string text = "SFX_NODE_CONTROL=3\n";
    for (std::size_t index = 0; index < enabled.size(); ++index)
        text += "node" + std::to_string(index + 1) + "_enabled=" + (enabled[index] ? "1\n" : "0\n");
    return text;
}
std::optional<application::NodeEnablement> parse_node_controls(std::string_view text) {
    const bool legacy = text.substr(0, 19) == "SFX_NODE_CONTROL=2\n";
    const std::string_view header = legacy ? "SFX_NODE_CONTROL=2\n" : "SFX_NODE_CONTROL=3\n";
    if (text.substr(0, header.size()) != header) return std::nullopt;
    text.remove_prefix(header.size());
    auto flags = application::default_node_enablement();
    const std::size_t count = legacy ? 4U : flags.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto prefix = "node" + std::to_string(index + 1) + "_enabled=";
        if (text.size() < prefix.size() + 2 || text.substr(0, prefix.size()) != prefix ||
            (text[prefix.size()] != '0' && text[prefix.size()] != '1') ||
            text[prefix.size() + 1] != '\n') return std::nullopt;
        flags[index] = text[prefix.size()] == '1';
        text.remove_prefix(prefix.size() + 2);
    }
    return text.empty() ? std::optional<application::NodeEnablement>(flags) : std::nullopt;
}
}

StorageStatus publish_alpha_node_controls(const std::filesystem::path& root,
    const std::wstring& owner_sid, const application::NodeEnablement& enabled) {
    const auto directory = root / L"Config";
    const auto path = directory / L"SFXNodes.state";
    const auto pending_path = directory / L"SFXNodes.state.writing";
    WindowsStorageDaclPolicy policy(owner_sid);
    if (!absolute_local_path(root) || has_reparse_component(directory) ||
        !policy.assess(directory, StorageDaclObjectKind::directory).status)
        return failure(StorageErrorCode::permission_denied, "node_controls_directory_invalid");
    // Pin the protected parent against replacement until publication verification.
    const HANDLE parent = CreateFileW(directory.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (parent == INVALID_HANDLE_VALUE)
        return failure(StorageErrorCode::io, "node_controls_directory_open_failed");
    struct CloseHandleGuard { HANDLE handle; ~CloseHandleGuard() { CloseHandle(handle); } } parent_guard{parent};
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            !policy.assess(path, StorageDaclObjectKind::file).status)
            return failure(StorageErrorCode::permission_denied, "node_controls_file_invalid");
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return failure(StorageErrorCode::io, "node_controls_file_unavailable");
    }
    SecurityAttributes security(owner_sid);
    if (!security.get()) return failure(StorageErrorCode::permission_denied, "node_controls_security_invalid");
    const auto serialized = node_controls_text(enabled);
    // Never follow/truncate an existing staging object. An interrupted write is
    // deliberately fail-closed and remains available for offline examination.
    const HANDLE pending = CreateFileW(pending_path.c_str(), GENERIC_WRITE, 0, security.get(),
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (pending == INVALID_HANDLE_VALUE) return failure(StorageErrorCode::io, "node_controls_pending_create_failed");
    const bool written = write_exact(pending, serialized.data(), static_cast<DWORD>(serialized.size())) &&
        FlushFileBuffers(pending) != FALSE;
    CloseHandle(pending);
    if (!written || !policy.apply_for_publication(pending_path, StorageDaclObjectKind::file, path)) {
        DeleteFileW(pending_path.c_str());
        return failure(StorageErrorCode::io, "node_controls_pending_write_failed");
    }
    if (!MoveFileExW(pending_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending_path.c_str());
        return failure(StorageErrorCode::io, "node_controls_publish_failed");
    }
    const auto read_back = read_small_text(path, 4096);
    if (!policy.assess(path, StorageDaclObjectKind::file).status || !read_back || *read_back != serialized)
        return failure(StorageErrorCode::io, "node_controls_verify_failed");
    return StorageStatus::success();
}

application::StorageResult<application::NodeEnablement> load_alpha_node_controls(
    const std::filesystem::path& root, const std::wstring& owner_sid) {
    const auto path = root / L"Config" / L"SFXNodes.state";
    WindowsStorageDaclPolicy policy(owner_sid);
    if (!absolute_local_path(root) || has_reparse_component(path.parent_path()) ||
        !policy.assess(path.parent_path(), StorageDaclObjectKind::directory).status)
        return {failure(StorageErrorCode::permission_denied, "node_controls_directory_invalid"), {}};
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto text = read_small_text(path, 4096);
        const auto parsed = text ? parse_node_controls(*text) : std::nullopt;
        if (!policy.assess(path, StorageDaclObjectKind::file).status || !parsed)
            return {failure(StorageErrorCode::io, "node_controls_invalid"), {}};
        return {StorageStatus::success(), parsed};
    }
    if (GetLastError() != ERROR_FILE_NOT_FOUND)
        return {failure(StorageErrorCode::io, "node_controls_open_failed"), {}};
    auto enabled = application::default_node_enablement();
    const auto legacy_path = root / L"Config" / L"SFXNode1.state";
    if (GetFileAttributesW(legacy_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto text = read_small_text(legacy_path, 128);
        if (!policy.assess(legacy_path, StorageDaclObjectKind::file).status || !text ||
            (*text != "SFX_NODE_CONTROL=1\nnode1_enabled=1\n" &&
             *text != "SFX_NODE_CONTROL=1\nnode1_enabled=0\n"))
            return {failure(StorageErrorCode::io, "legacy_node_controls_invalid"), {}};
        enabled.fill(*text == "SFX_NODE_CONTROL=1\nnode1_enabled=1\n");
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return {failure(StorageErrorCode::io, "legacy_node_controls_open_failed"), {}};
    }
    const auto published = publish_alpha_node_controls(root, owner_sid, enabled);
    return {published, published ? std::optional<application::NodeEnablement>(enabled) : std::nullopt};
}

StorageStatus publish_node_one_enabled(const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid, const bool enabled) {
    const std::string serialized = std::string("SFX_NODE_CONTROL=1\nnode1_enabled=") +
        (enabled ? "1\n" : "0\n");
    return publish_protected_text(installation_root / L"Config" / L"SFXNode1.state",
        installation_owner_sid, serialized, "Node 1 control state");
}

RuntimeInstanceGuard::~RuntimeInstanceGuard() {
    if (lock_file_ != INVALID_HANDLE_VALUE) CloseHandle(lock_file_);
    if (mutex_ != nullptr) CloseHandle(mutex_);
}

StorageStatus RuntimeInstanceGuard::acquire(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid) {
    if (acquired()) return failure(StorageErrorCode::conflict, "runtime instance already acquired");
    SecurityAttributes security(installation_owner_sid);
    if (security.get() == nullptr) {
        return failure(StorageErrorCode::permission_denied, "build runtime instance security descriptor");
    }
    const auto names = installation_object_names(installation_root);
    mutex_ = CreateMutexW(security.get(), FALSE, names.mutex_name.c_str());
    if (mutex_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex_ != nullptr) { CloseHandle(mutex_); mutex_ = nullptr; }
        return failure(StorageErrorCode::conflict, "reject duplicate authoritative runtime mutex");
    }
    const std::filesystem::path path = installation_root / L"Data" / L"SFXRuntime.lock";
    lock_file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, security.get(),
        OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (lock_file_ == INVALID_HANDLE_VALUE) {
        CloseHandle(mutex_); mutex_ = nullptr;
        return failure(StorageErrorCode::conflict, "reject duplicate authoritative runtime lock");
    }
    WindowsStorageDaclPolicy policy(installation_owner_sid);
    const auto protected_status = policy.apply(path, StorageDaclObjectKind::file);
    if (!protected_status) {
        CloseHandle(lock_file_); lock_file_ = INVALID_HANDLE_VALUE;
        CloseHandle(mutex_); mutex_ = nullptr;
        return protected_status;
    }
    return StorageStatus::success();
}

bool RuntimeInstanceGuard::acquired() const noexcept {
    return mutex_ != nullptr && lock_file_ != INVALID_HANDLE_VALUE;
}

OfflineInstallationGuard::~OfflineInstallationGuard() {
    if (mutex_ != nullptr) CloseHandle(mutex_);
}

StorageStatus OfflineInstallationGuard::acquire(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid) {
    if (acquired())
        return failure(StorageErrorCode::conflict,
            "offline installation guard already acquired");
    SecurityAttributes security(installation_owner_sid);
    if (security.get() == nullptr)
        return failure(StorageErrorCode::permission_denied,
            "offline installation guard security invalid");
    const auto names = installation_object_names(installation_root);
    mutex_ = CreateMutexW(security.get(), FALSE, names.mutex_name.c_str());
    if (mutex_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex_ != nullptr) { CloseHandle(mutex_); mutex_ = nullptr; }
        return failure(StorageErrorCode::conflict,
            "Runtime or another offline transaction owns this installation");
    }
    return StorageStatus::success();
}

bool OfflineInstallationGuard::acquired() const noexcept {
    return mutex_ != nullptr;
}

NamedPipeConnection::NamedPipeConnection(const HANDLE handle) noexcept : handle_(handle) {}
NamedPipeConnection::~NamedPipeConnection() { disconnect(); }
NamedPipeConnection::NamedPipeConnection(NamedPipeConnection&& other) noexcept
    : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }
NamedPipeConnection& NamedPipeConnection::operator=(NamedPipeConnection&& other) noexcept {
    if (this != &other) {
        disconnect();
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
}
bool NamedPipeConnection::valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
HANDLE NamedPipeConnection::native_handle() const noexcept { return handle_; }

bool NamedPipeConnection::read_frame(application::LocalControlFrame& frame) const {
    if (!valid()) return false;
    std::vector<std::uint8_t> bytes(application::kLocalControlFrameHeaderBytes);
    if (!read_exact(handle_, bytes.data(), static_cast<DWORD>(bytes.size()))) return false;
    const std::uint32_t payload_size = read_u32_le(bytes.data() + 16U);
    if (payload_size > application::kLocalControlMaximumPayloadBytes) return false;
    bytes.resize(bytes.size() + payload_size);
    if (payload_size != 0 && !read_exact(handle_,
            bytes.data() + application::kLocalControlFrameHeaderBytes, payload_size)) return false;
    auto decoded = application::decode_local_control_frame(bytes);
    SecureZeroMemory(bytes.data(), bytes.size());
    if (decoded.code != application::LocalControlFrameDecodeCode::complete || !decoded.frame)
        return false;
    frame = std::move(*decoded.frame);
    return true;
}

bool NamedPipeConnection::write_frame(const application::LocalControlFrame& frame) const {
    if (!valid()) return false;
    auto bytes = application::encode_local_control_frame(frame);
    const bool result = !bytes.empty() &&
        write_exact(handle_, bytes.data(), static_cast<DWORD>(bytes.size())) &&
        FlushFileBuffers(handle_) != FALSE;
    if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
    return result;
}

void NamedPipeConnection::disconnect() noexcept {
    if (!valid()) return;
    // A completed write_frame already confirms delivery. An aborted write or
    // an unresponsive peer must not turn destruction into another blocking
    // server-end drain outside the service's cancellable control-I/O scope.
    (void)DisconnectNamedPipe(handle_);
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
}

NamedPipeConnection accept_local_control_client(
    const std::wstring& pipe_name,
    const std::wstring& installation_owner_sid,
    const DWORD expected_client_process_id,
    const DWORD timeout_millis,
    std::string* const diagnostic_code) {
    const auto reject = [diagnostic_code](const char* const code) {
        if (diagnostic_code != nullptr) *diagnostic_code = code;
        return NamedPipeConnection{};
    };
    SecurityAttributes security(installation_owner_sid);
    if (security.get() == nullptr) return reject("pipe_security_descriptor_invalid");
    const HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 8192, 8192, 5000, security.get());
    if (pipe == INVALID_HANDLE_VALUE) return reject("pipe_creation_failed");
    const ULONGLONG deadline = GetTickCount64() + timeout_millis;
    BOOL connected = FALSE;
    for (;;) {
        if (ConnectNamedPipe(pipe, nullptr)) {
            connected = TRUE;
            break;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
            break;
        }
        if (error != ERROR_PIPE_LISTENING || GetTickCount64() >= deadline) break;
        Sleep(25);
    }
    if (!connected) { CloseHandle(pipe); return reject("pipe_accept_timeout_or_failure"); }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        DisconnectNamedPipe(pipe); CloseHandle(pipe); return reject("pipe_wait_mode_failed");
    }
    ULONG client_process_id = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_process_id)) {
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
        return reject("pipe_client_process_unavailable");
    }
    if (expected_client_process_id != 0 &&
        client_process_id != expected_client_process_id) {
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
        return reject("pipe_client_process_mismatch");
    }
    if (!process_user_sid_matches(client_process_id, installation_owner_sid)) {
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
        return reject("pipe_client_sid_mismatch");
    }
    if (diagnostic_code != nullptr) *diagnostic_code = "pipe_client_authenticated";
    return NamedPipeConnection(pipe);
}

NamedPipeConnection connect_local_control_server(
    const std::wstring& pipe_name,
    const DWORD expected_server_process_id,
    const DWORD timeout_millis,
    const std::function<bool()>& cancelled) {
    const ULONGLONG deadline = GetTickCount64() + timeout_millis;
    for (;;) {
        if(cancelled&&cancelled())return {};
        if (WaitNamedPipeW(pipe_name.c_str(), 100)) {
            const HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                ULONG server_process_id = 0;
                if (GetNamedPipeServerProcessId(pipe, &server_process_id) &&
                    server_process_id == expected_server_process_id) {
                    return NamedPipeConnection(pipe);
                }
                CloseHandle(pipe);
                return {};
            }
        }
        if (GetTickCount64() >= deadline) return {};
        Sleep(25);
    }
}

application::StorageStatus invalidate_restored_message_verification(const std::filesystem::path& root,const std::wstring& owner){
    const auto* job=application::kMessageIntegrityJobId;
    auto configuration=read_scheduler_configuration_file(root,owner,job);
    if(configuration.state==ConfigurationFileState::missing){
        const auto published=publish_scheduler_configuration_file(root,owner,application::default_message_scheduler_configuration(job));
        if(!published)return published;
        configuration=read_scheduler_configuration_file(root,owner,job);
    }
    if(!configuration.value)return {application::StorageErrorCode::io,"restored integrity configuration unavailable; restore must roll back"};
    const auto saved=read_scheduler_state_file(root,owner,job);
    if(!saved.value && saved.state!=ConfigurationFileState::missing)return {application::StorageErrorCode::io,"restored integrity state unsafe or invalid; restore must roll back"};
    auto state=saved.value.value_or(application::default_message_scheduler_state(job));
    if(!saved.value)state.configuration_revision=configuration.value->revision;
    // Preserve enabled/Off/day/time/history and the original result as history.
    // Only this new current-store prerequisite changes; clearing requires a
    // fresh complete current check after restart, never the restored old receipt.
    state.requires_fresh_check=true;
    return publish_scheduler_state_file(root,owner,state);
}

bool fill_secure_random(void* destination, const std::size_t bytes) noexcept {
    return destination != nullptr && bytes > 0 && bytes <= std::numeric_limits<ULONG>::max() &&
        BCryptGenRandom(nullptr, static_cast<PUCHAR>(destination), static_cast<ULONG>(bytes),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

}  // namespace sfx::platform::win32

namespace sfx::application {
namespace {
std::filesystem::path external_event_path(const std::filesystem::path& root, ExternalEventFile file) {
    return root / (file == ExternalEventFile::catalog ? L"Config\\SFXExternalEvents.conf" : L"Data\\SFXExternalEvents.state");
}
}
std::optional<std::string> read_external_event_file(const std::filesystem::path& root,
    const std::wstring& owner, ExternalEventFile file) {
    using namespace platform::win32;
    const auto path = external_event_path(root, file);
    WindowsStorageDaclPolicy policy(owner);
    if (!policy.assess(path.parent_path(), StorageDaclObjectKind::directory).status) return {};
    const auto attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? std::optional<std::string>{""} : std::nullopt;
    if ((attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !policy.assess(path, StorageDaclObjectKind::file).status) return {};
    return read_small_text(path, static_cast<DWORD>(kExternalEventFileLimit));
}
bool publish_external_event_file(const std::filesystem::path& root, const std::wstring& owner,
    ExternalEventFile file, std::string_view document) {
    using namespace platform::win32;
    const auto path = external_event_path(root, file);
    if (document.empty() || document.size() > kExternalEventFileLimit ||
        !read_external_event_file(root, owner, file)) return false;
    const std::filesystem::path pending = path.native() + L".writing";
    const auto attrs = GetFileAttributesW(pending.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES ||
        GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    return publish_protected_text(path, owner, document, "External Events") &&
        read_external_event_file(root, owner, file) == document;
}
} // namespace sfx::application
