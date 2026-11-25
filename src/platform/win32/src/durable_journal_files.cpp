#include "sfx/platform/win32/durable_journal_files.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/application/operational_log.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

namespace sfx::platform::win32 {
namespace {
namespace app = application;
namespace install = installation;
using Kind = app::DurableJournalKind;
const wchar_t* leaf(Kind kind) { return kind == Kind::runtime ? L"SFXRuntime.log" : kind == Kind::callers ? L"CALLERS.LOG" : L"HEYSYSOP.LOG"; }
std::wstring base_name(Kind kind) { return kind==Kind::runtime ? L"SFXRuntime" : kind==Kind::callers ? L"CALLERS" : L"HEYSYSOP"; }
std::string header(Kind kind) { return kind==Kind::runtime ? "" : kind == Kind::callers ? "; SFX CALLERS.LOG v1\r\n" : "; SFX HEYSYSOP.LOG v1\r\n"; }
std::size_t slot(Kind kind) {
    install::require(kind == Kind::callers || kind == Kind::attention || kind==Kind::runtime, "journal_kind_invalid");
    return kind == Kind::callers ? 0U : kind==Kind::attention ? 1U : 2U;
}
app::JournalFileState state(HANDLE handle) {
    LARGE_INTEGER size{};
    install::require(GetFileSizeEx(handle, &size) && size.QuadPart >= 0 &&
        static_cast<std::uint64_t>(size.QuadPart) <= app::kDurableJournalMaximumFileBytes, "journal_size_invalid");
    return {std::to_string(install::volume_serial(handle)) + ':' + std::to_string(install::file_id(handle)),
        static_cast<std::uint64_t>(size.QuadPart)};
}
std::string read_at(HANDLE handle, std::uint64_t offset, std::size_t bytes) {
    install::require(bytes <= app::kCallerJournalRecordBytes && offset <= app::kDurableJournalMaximumFileBytes &&
        bytes <= app::kDurableJournalMaximumFileBytes - offset, "journal_read_bound");
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    install::require(SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != FALSE, "journal_seek_failed");
    std::string result(bytes, '\0'); std::size_t done{};
    while (done < bytes) {
        DWORD count{};
        install::require(ReadFile(handle, result.data() + done, static_cast<DWORD>(bytes - done), &count, nullptr) && count,
            "journal_read_failed");
        done += count;
    }
    return result;
}
void write_suffix(HANDLE handle, std::string_view text) {
    LARGE_INTEGER end{};
    install::require(SetFilePointerEx(handle, end, nullptr, FILE_END) != FALSE, "journal_append_seek_failed");
    while (!text.empty()) {
        DWORD written{};
        install::require(WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written,
            "journal_append_failed");
        text.remove_prefix(written);
    }
    // Also flush when recovery finds the entire block already present: a prior
    // failed/uncertain flush is not a durable acknowledgement.
    install::require(FlushFileBuffers(handle) != FALSE, "journal_flush_failed");
}
std::vector<install::Handle> boundary(const std::filesystem::path& root,
    const wchar_t* directory, const std::wstring& owner) {
    // Runtime validates the permanent installation at startup. Each operation
    // additionally rejects redirected ancestors and retains exact path objects
    // until its content handle has been opened and checked. No permission repair.
    std::vector<std::filesystem::path> chain;
    for (auto path = root / directory; !path.empty();) {
        chain.push_back(path); const auto parent = path.parent_path();
        if (parent == path) break; path = parent;
    }
    std::reverse(chain.begin(), chain.end());
    std::vector<install::Handle> handles;
    for (const auto& path : chain) handles.push_back(install::open_object(path, true,
        READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE));
    WindowsStorageDaclPolicy policy(owner);
    install::require(policy.assess(root / directory, StorageDaclObjectKind::directory).status.succeeded(),
        "journal_boundary_security_invalid");
    return handles;
}
std::wstring stamp() {
    SYSTEMTIME utc{}; GetSystemTime(&utc); wchar_t text[32]{};
    const auto length = swprintf_s(text, L"%04u%02u%02uT%02u%02u%02u%03uZ", utc.wYear,
        utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    install::require(length == 19, "journal_archive_time_invalid"); return text;
}
bool archive_name(const std::wstring& value, const std::wstring& base) {
    const auto prefix = base + L'-';
    if (value.size() != prefix.size() + 19 + 1 + 32 + 4 || value.compare(0, prefix.size(), prefix)) return false;
    const auto begin = prefix.size();
    for (std::size_t i = 0; i < 19; ++i) {
        const auto c = value[begin + i];
        if (i == 8 ? c != L'T' : i == 18 ? c != L'Z' : (c < L'0' || c > L'9')) return false;
    }
    if (value[begin + 19] != L'-' || value.substr(value.size() - 4) != L".LOG") return false;
    for (std::size_t i = begin + 20; i < begin + 52; ++i)
        if (!((value[i] >= L'0' && value[i] <= L'9') || (value[i] >= L'A' && value[i] <= L'F'))) return false;
    return true;
}
bool complete_file(HANDLE file,Kind kind) {
    const auto info=state(file);
    if (!info.bytes) return kind==Kind::runtime;
    const auto prefix=header(kind);
    if (info.bytes<prefix.size() || read_at(file,0,prefix.size())!=prefix) return false;
    if (info.bytes==prefix.size()) return true;
    const std::string tail=kind==Kind::callers ? "\r\n---\r\n" : "\r\n";
    if (info.bytes<tail.size() || read_at(file,info.bytes-tail.size(),tail.size())!=tail) return false;
    if (kind!=Kind::runtime) return true;
    std::string pending;
    std::uint64_t sequence=0;
    for (std::uint64_t offset=0;offset<info.bytes;) {
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(8192,info.bytes-offset));
        pending+=read_at(file,offset,count); offset+=count;
        for (;;) {
            const auto end=pending.find('\n');
            if (end==pending.npos) break;
            auto line=std::string_view(pending.data(),end);
            if (!line.empty() && line.back()=='\r') line.remove_suffix(1);
            if (!app::decode_operational_record(line,++sequence)) return false;
            pending.erase(0,end+1);
        }
        if (pending.size()>app::kOperationalRecordMaximum+1) return false;
    }
    return pending.empty();
}
bool prune_archives(const std::filesystem::path& directory, const std::wstring& owner, Kind kind,std::size_t retention) {
    const std::wstring base = base_name(kind);
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((directory / (base + L"-*.LOG")).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    std::vector<std::wstring> archives;
    std::size_t examined{};
    try {
        do {
            if (++examined > 256) { FindClose(search); return false; }
            if (archive_name(found.cFileName, base)) archives.emplace_back(found.cFileName);
        } while (FindNextFileW(search, &found));
    } catch (...) { FindClose(search); throw; }
    const auto error = GetLastError(); FindClose(search);
    if (error != ERROR_NO_MORE_FILES) return false;
    std::sort(archives.begin(), archives.end());
    while (archives.size() > retention) {
        const auto candidate = directory / archives.front();
        auto handle = install::open_object(candidate, false, GENERIC_READ | READ_CONTROL | DELETE, FILE_SHARE_READ);
        WindowsStorageDaclPolicy policy(owner);
        if (!policy.assess(candidate, StorageDaclObjectKind::file).status) return false;
        if (!complete_file(handle.get(),kind)) return false;
        FILE_DISPOSITION_INFO remove{}; remove.DeleteFile = TRUE;
        if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo, &remove, sizeof(remove))) return false;
        handle.reset(); archives.erase(archives.begin());
    }
    return true;
}
}
class WindowsDurableJournalFiles::Impl final {
public:
    Impl(std::filesystem::path root, std::wstring owner) : root_(std::move(root)), owner_(std::move(owner)) {}
    HANDLE open(Kind kind) {
        auto guard = boundary(root_, L"Logs", owner_);
        auto& held = files_[slot(kind)];
        const auto path = root_ / L"Logs" / leaf(kind);
        if (!held) {
            const auto attributes = GetFileAttributesW(path.c_str());
            bool created = false;
            if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
                install::Handle fresh(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                    FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                install::require(fresh.get() != INVALID_HANDLE_VALUE, "journal_create_failed");
                fresh.reset(); created = true;
            }
            WindowsStorageDaclPolicy policy(owner_);
            // Ordinary mutable file protection only. Existing file security is
            // assessed, not repaired. No structural boundary is rewritten.
            if (created) install::require(policy.apply(path, StorageDaclObjectKind::file).succeeded(), "journal_file_protection_failed");
            install::require(policy.assess(path, StorageDaclObjectKind::file).status.succeeded(), "journal_file_security_invalid");
            auto candidate = install::open_object(path, false, GENERIC_READ | GENERIC_WRITE | READ_CONTROL, FILE_SHARE_READ);
            auto info = state(candidate.get());
            if (!info.bytes && created) write_suffix(candidate.get(), header(kind));
            info = state(candidate.get());
            if (info.bytes) install::require(info.bytes >= header(kind).size() &&
                read_at(candidate.get(), 0, header(kind).size()) == header(kind), "journal_header_invalid");
            if (kind==Kind::runtime) install::require(complete_file(candidate.get(),kind),"runtime_log_history_invalid");
            held = std::move(candidate); // cache only a completely validated handle
        }
        // Reject an unexpectedly changed namespace even when a previous content
        // handle is still valid. A read-only check is compatible with our writer.
        auto current = install::open_object(path, false, FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE);
        install::require(install::file_id(current.get()) == install::file_id(held.get()) &&
            install::volume_serial(current.get()) == install::volume_serial(held.get()), "journal_identity_changed");
        // Retry unfinished pruning after a failure/restart before any further
        // append/rotation. A failure cannot accumulate unbounded extra archives.
        if (prune_needed_[slot(kind)]) {
            install::require(prune_archives(root_ / L"Logs", owner_, kind,retention_[slot(kind)]), "journal_retention_unavailable");
            prune_needed_[slot(kind)]=false;
        }
        return held.get();
    }
    std::filesystem::path root_;
    std::wstring owner_;
    std::array<install::Handle, 3> files_;
    std::array<std::size_t,3> retention_{10,10,10};
    std::array<bool,3> prune_needed_{true,true,true};
};
WindowsDurableJournalFiles::WindowsDurableJournalFiles(std::filesystem::path root, std::wstring owner)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(owner))) {}
WindowsDurableJournalFiles::~WindowsDurableJournalFiles() = default;
bool WindowsDurableJournalFiles::configure(const app::LoggingConfiguration& value) noexcept {
    if (!app::valid_logging_configuration(value)) return false;
    const std::array<std::size_t,3> next{value.callers.archives,value.attention.archives,value.runtime.archives};
    if (impl_->retention_!=next) { impl_->retention_=next; impl_->prune_needed_={true,true,true}; }
    return true;
}
std::optional<app::JournalFileState> WindowsDurableJournalFiles::inspect(Kind kind) {
    try { return state(impl_->open(kind)); } catch (...) { return {}; }
}
std::optional<std::string> WindowsDurableJournalFiles::read(Kind kind, std::uint64_t offset, std::size_t bytes) {
    try { const auto file = impl_->open(kind); const auto info = state(file);
        if (offset > info.bytes || bytes > info.bytes - offset) return {};
        return read_at(file, offset, bytes);
    } catch (...) { return {}; }
}
bool WindowsDurableJournalFiles::append(Kind kind, const app::JournalFileState& expected, std::string_view text) {
    try {
        const auto file = impl_->open(kind); const auto actual = state(file);
        if (expected.identity != actual.identity || expected.bytes != actual.bytes ||
            text.size() > (kind == Kind::callers ? app::kCallerJournalRecordBytes : kind==Kind::runtime ?
                app::kOperationalRecordMaximum+2 : app::kAttentionJournalRecordBytes) ||
            text.size() > app::kDurableJournalMaximumFileBytes - actual.bytes) return false;
        write_suffix(file, text); return true;
    } catch (...) { return false; }
}
bool WindowsDurableJournalFiles::archive_and_reopen(Kind kind) {
    try {
        const auto file = impl_->open(kind); const auto info = state(file);
        const auto tail = kind == Kind::callers ? std::string("\r\n---\r\n") : std::string("\r\n");
        if (info.bytes > header(kind).size() &&
            (info.bytes < tail.size() || read_at(file, info.bytes - tail.size(), tail.size()) != tail)) return false;
        if (!FlushFileBuffers(file)) return false;
        auto guard = boundary(impl_->root_, L"Logs", impl_->owner_);
        const auto directory = impl_->root_ / L"Logs";
        const std::wstring base = base_name(kind);
        const auto archive = directory / (base + L'-' + stamp() + L'-' + install::unique_id() + L".LOG");
        impl_->files_[slot(kind)].reset();
        // No replacement flag: timestamp/nonce collision preserves both objects.
        if (!MoveFileExW((directory / leaf(kind)).c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
        impl_->prune_needed_[slot(kind)]=true;
        (void)impl_->open(kind);
        return true;
    } catch (...) { return false; }
}
app::NewCallerNames load_new_caller_names(const std::filesystem::path& root, const std::wstring& owner) noexcept {
    try {
        auto guard = boundary(root, L"Config", owner);
        const auto path = root / L"Config" / L"JOKER.DAT";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
            return app::NewCallerNames::parse("");
        auto file = install::open_object(path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
        WindowsStorageDaclPolicy policy(owner);
        if (!policy.assess(path, StorageDaclObjectKind::file).status) return app::NewCallerNames::unavailable();
        LARGE_INTEGER bytes{};
        if (!GetFileSizeEx(file.get(), &bytes) || bytes.QuadPart < 0 || bytes.QuadPart > app::NewCallerNames::maximum_bytes)
            return app::NewCallerNames::unavailable();
        std::string text(static_cast<std::size_t>(bytes.QuadPart), '\0'); DWORD read{};
        if (!text.empty() && (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size()))
            return app::NewCallerNames::unavailable();
        return app::NewCallerNames::parse(text);
    } catch (...) { return app::NewCallerNames::unavailable(); }
}
} // namespace sfx::platform::win32
