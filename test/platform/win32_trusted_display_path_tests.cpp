#include "sfx/platform/win32/trusted_display_asset_reader.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace app = sfx::application;
namespace platform = sfx::platform::win32;

std::size_t assertions = 0;

void require(const bool condition, const char* const message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

class Handle final {
public:
    explicit Handle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

struct JunctionReparseData {
    DWORD tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_name_offset;
    USHORT substitute_name_length;
    USHORT print_name_offset;
    USHORT print_name_length;
    wchar_t path_buffer[1];
};

void write_file(const std::filesystem::path& path, const std::string_view bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0U, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(file.valid(), "test asset file created");
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const DWORD offered = static_cast<DWORD>(
            (std::min)(bytes.size() - offset,
                static_cast<std::size_t>(64U * 1024U)));
        DWORD written = 0U;
        require(WriteFile(file.get(), bytes.data() + offset, offered,
                    &written, nullptr) != FALSE &&
                written == offered,
            "test asset file written completely");
        offset += written;
    }
}

bool make_junction(
    const std::filesystem::path& link,
    const std::filesystem::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    Handle directory(CreateFileW(link.c_str(), GENERIC_WRITE, 0U, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!directory.valid()) return false;

    const std::wstring substitute = L"\\??\\" +
        std::filesystem::absolute(target).wstring();
    const std::wstring printable =
        std::filesystem::absolute(target).wstring();
    const std::size_t substitute_bytes =
        substitute.size() * sizeof(wchar_t);
    const std::size_t printable_bytes =
        printable.size() * sizeof(wchar_t);
    const std::size_t path_bytes = substitute_bytes + sizeof(wchar_t) +
        printable_bytes + sizeof(wchar_t);
    const std::size_t total =
        offsetof(JunctionReparseData, path_buffer) + path_bytes;
    std::vector<std::byte> storage(total);
    auto* const data =
        reinterpret_cast<JunctionReparseData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->reserved = 0U;
    data->substitute_name_offset = 0U;
    data->substitute_name_length =
        static_cast<USHORT>(substitute_bytes);
    data->print_name_offset =
        static_cast<USHORT>(substitute_bytes + sizeof(wchar_t));
    data->print_name_length =
        static_cast<USHORT>(printable_bytes);
    std::memcpy(data->path_buffer,
        substitute.data(), substitute_bytes);
    data->path_buffer[substitute.size()] = L'\0';
    std::memcpy(
        reinterpret_cast<std::byte*>(data->path_buffer) +
            data->print_name_offset,
        printable.data(), printable_bytes);
    data->path_buffer[
        data->print_name_offset / sizeof(wchar_t) +
            printable.size()] = L'\0';
    constexpr std::size_t reparse_header_size =
        sizeof(DWORD) + sizeof(USHORT) + sizeof(USHORT);
    data->data_length =
        static_cast<USHORT>(total - reparse_header_size);
    DWORD returned = 0U;
    return DeviceIoControl(directory.get(), FSCTL_SET_REPARSE_POINT,
               data, static_cast<DWORD>(total), nullptr, 0U,
               &returned, nullptr) != FALSE;
}

class ReplacementRaceHook final : public platform::ITrustedDisplayReadHook {
public:
    enum class Target {
        root,
        file
    };

    ReplacementRaceHook(Target target, std::filesystem::path destination)
        : target_(target), destination_(std::move(destination)) {}

    void on_stage(
        const platform::TrustedDisplayReadStage stage,
        const std::wstring_view trusted_root,
        const std::wstring_view opened_path) override {
        const bool selected =
            (target_ == Target::root &&
                stage == platform::TrustedDisplayReadStage::root_revalidated) ||
            (target_ == Target::file &&
                stage == platform::TrustedDisplayReadStage::file_verified);
        if (!selected || attempted_) return;
        attempted_ = true;
        const std::wstring source =
            target_ == Target::root ? std::wstring(trusted_root)
                                    : std::wstring(opened_path);
        succeeded_ = MoveFileExW(source.c_str(), destination_.c_str(),
            MOVEFILE_REPLACE_EXISTING) != FALSE;
        error_ = succeeded_ ? ERROR_SUCCESS : GetLastError();
        if (succeeded_ && target_ == Target::root &&
            CreateDirectoryW(source.c_str(), nullptr) != FALSE) {
            const auto attacker_file =
                std::filesystem::path(source) / L"RACE.ASC";
            Handle file(CreateFileW(attacker_file.c_str(), GENERIC_WRITE,
                0U, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            constexpr std::string_view attacker_bytes{"replacement bytes"};
            DWORD written = 0U;
            replacement_installed_ = file.valid() &&
                WriteFile(file.get(), attacker_bytes.data(),
                    static_cast<DWORD>(attacker_bytes.size()), &written,
                    nullptr) != FALSE &&
                written == attacker_bytes.size();
        }
    }

    bool attempted() const noexcept { return attempted_; }
    bool succeeded() const noexcept { return succeeded_; }
    DWORD error() const noexcept { return error_; }
    bool replacement_installed() const noexcept {
        return replacement_installed_;
    }

private:
    Target target_;
    std::filesystem::path destination_;
    bool attempted_{false};
    bool succeeded_{false};
    bool replacement_installed_{false};
    DWORD error_{ERROR_SUCCESS};
};

class SwapRestoreRaceHook final
    : public platform::ITrustedDisplayReadHook {
public:
    SwapRestoreRaceHook(
        std::filesystem::path displaced_original,
        std::filesystem::path displaced_hostile_candidate)
        : displaced_original_(std::move(displaced_original)),
          displaced_hostile_candidate_(
              std::move(displaced_hostile_candidate)) {}

    bool widen_candidate_sharing_for_race_test() const noexcept override {
        return true;
    }

    void on_stage(
        const platform::TrustedDisplayReadStage stage,
        const std::wstring_view trusted_root,
        const std::wstring_view opened_path) override {
        if (stage == platform::TrustedDisplayReadStage::root_revalidated &&
            !swap_attempted_) {
            swap_attempted_ = true;
            approved_root_ = std::wstring(trusted_root);
            original_displaced_ = MoveFileExW(approved_root_.c_str(),
                displaced_original_.c_str(), MOVEFILE_REPLACE_EXISTING) !=
                FALSE;
            if (!original_displaced_ ||
                CreateDirectoryW(approved_root_.c_str(), nullptr) == FALSE) {
                return;
            }
            const auto hostile_file =
                std::filesystem::path(approved_root_) / L"RACE.ASC";
            Handle file(CreateFileW(hostile_file.c_str(), GENERIC_WRITE,
                0U, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            constexpr std::string_view hostile_bytes{
                "swap-restore hostile bytes"};
            DWORD written = 0U;
            hostile_installed_ = file.valid() &&
                WriteFile(file.get(), hostile_bytes.data(),
                    static_cast<DWORD>(hostile_bytes.size()), &written,
                    nullptr) != FALSE &&
                written == hostile_bytes.size();
            return;
        }
        if (stage == platform::TrustedDisplayReadStage::candidate_opened &&
            hostile_installed_ && !restore_attempted_) {
            restore_attempted_ = true;
            hostile_candidate_displaced_ = MoveFileExW(
                std::wstring(opened_path).c_str(),
                displaced_hostile_candidate_.c_str(),
                MOVEFILE_REPLACE_EXISTING) != FALSE;
            candidate_displacement_error_ = hostile_candidate_displaced_
                ? ERROR_SUCCESS
                : GetLastError();
            if (hostile_candidate_displaced_) {
                hostile_root_removed_ =
                    RemoveDirectoryW(approved_root_.c_str()) != FALSE;
                hostile_root_removal_error_ = hostile_root_removed_
                    ? ERROR_SUCCESS
                    : GetLastError();
            }
            if (hostile_root_removed_) {
                original_restored_ = MoveFileExW(
                    displaced_original_.c_str(), approved_root_.c_str(),
                    MOVEFILE_REPLACE_EXISTING) != FALSE;
                original_restore_error_ = original_restored_
                    ? ERROR_SUCCESS
                    : GetLastError();
            }
        }
    }

    bool swap_attempted() const noexcept { return swap_attempted_; }
    bool original_displaced() const noexcept {
        return original_displaced_;
    }
    bool hostile_installed() const noexcept {
        return hostile_installed_;
    }
    bool restore_attempted() const noexcept {
        return restore_attempted_;
    }
    bool hostile_candidate_displaced() const noexcept {
        return hostile_candidate_displaced_;
    }
    DWORD candidate_displacement_error() const noexcept {
        return candidate_displacement_error_;
    }
    bool hostile_root_removed() const noexcept {
        return hostile_root_removed_;
    }
    DWORD hostile_root_removal_error() const noexcept {
        return hostile_root_removal_error_;
    }
    bool original_restored() const noexcept {
        return original_restored_;
    }
    DWORD original_restore_error() const noexcept {
        return original_restore_error_;
    }

private:
    std::filesystem::path displaced_original_;
    std::filesystem::path displaced_hostile_candidate_;
    std::wstring approved_root_;
    bool swap_attempted_{false};
    bool original_displaced_{false};
    bool hostile_installed_{false};
    bool restore_attempted_{false};
    bool hostile_candidate_displaced_{false};
    bool hostile_root_removed_{false};
    bool original_restored_{false};
    DWORD candidate_displacement_error_{ERROR_SUCCESS};
    DWORD hostile_root_removal_error_{ERROR_SUCCESS};
    DWORD original_restore_error_{ERROR_SUCCESS};
};

bool available(const app::TrustedDisplayAssetReadResult& result) {
    return result.code == app::TrustedDisplayAssetReadCode::available;
}

void ordinary_and_invalid_paths(const std::filesystem::path& root) {
    write_file(root / L"WELCOME1.ASC", "trusted bytes");
    auto reader =
        platform::Win32TrustedDisplayAssetReader::open(root.wstring());
    require(reader != nullptr, "ordinary fixed rooted reader opens");

    const auto mixed_case = reader->read_asset(
        "welcome1.asc", app::kMaximumTrustedDisplaySourceBytes);
    require(available(mixed_case) && mixed_case.bytes == "trusted bytes" &&
            !mixed_case.stable_file_identity.empty(),
        "mixed-case verified-handle read returns exact bytes and identity");
    require(mixed_case.modified_utc_100ns.has_value(),"verified reader returns file modification time");
    const auto repeat = reader->read_asset(
        "WELCOME1.ASC", app::kMaximumTrustedDisplaySourceBytes);
    require(available(repeat) &&
            repeat.stable_file_identity ==
                mixed_case.stable_file_identity,
        "repeat read reports deterministic physical identity");
    require(repeat.modified_utc_100ns==mixed_case.modified_utc_100ns,"same verified snapshot retains timestamp");
    write_file(root / L"chat-emote.txt", "LAUGH|NONE|{caller} laughs.\r\n");
    const auto emotes=reader->read_asset("chat-emote.txt",32768);
    require(available(emotes)&&emotes.bytes=="LAUGH|NONE|{caller} laughs.\r\n"&&!emotes.stable_file_identity.empty(),
        "fixed stock emote leaf uses the verified bounded file reader");
    for(const auto name:{"../chat-emote.txt","dir/chat-emote.txt","chat-emote.txt:secret","chat-emote.txt.","chat-other.txt"})
        require(reader->read_asset(name,32768).code==app::TrustedDisplayAssetReadCode::invalid,
            "fixed emote exception cannot admit paths, aliases, streams or other hyphenated names");
    require(reader->read_asset("chat-emote.txt",2).code==app::TrustedDisplayAssetReadCode::invalid,
        "fixed emote leaf retains the exact caller size bound");
    {
        Handle change(CreateFileW((root/L"WELCOME1.ASC").c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        require(change.valid(),"isolated fixture timestamp handle opens");
        const std::uint64_t ticks=116444736000000000ULL+1700000000ULL*10000000ULL+1234;
        FILETIME modified{static_cast<DWORD>(ticks),static_cast<DWORD>(ticks>>32)};
        require(SetFileTime(change.get(),nullptr,nullptr,&modified)!=FALSE,"same-byte rewrite fixture updates modification time");
    }
    const auto dated=reader->read_asset("WELCOME1.ASC",app::kMaximumTrustedDisplaySourceBytes);
    require(available(dated) && dated.bytes==mixed_case.bytes && dated.modified_utc_100ns==17000000000001234LL,"exact same-handle 100ns timestamp survives without changing content");

    const std::array<std::string_view, 18> invalid{
        "../WELCOME1.ASC", "..\\WELCOME1.ASC", "/WELCOME1.ASC",
        "\\WELCOME1.ASC", "C:\\WELCOME1.ASC", "\\\\server\\share\\X.ASC",
        "\\\\?\\C:\\X.ASC", "\\\\.\\C:\\X.ASC", "WELCOME1.ASC:secret",
        "WELCOME1.ASC.", "WELCOME1.ASC ", "DIR/WELCOME1.ASC",
        "DIR\\WELCOME1.ASC", "CON.ASC", "NUL.ASC", "COM1.ASC",
        "LPT9.ASC", ""};
    for (const auto candidate : invalid) {
        require(reader->read_asset(candidate,
                    app::kMaximumTrustedDisplaySourceBytes).code ==
                app::TrustedDisplayAssetReadCode::invalid,
            "unsafe logical asset path rejected before open");
    }
    require(reader->read_asset("WELCOME1.ASC", 0U).code ==
            app::TrustedDisplayAssetReadCode::invalid,
        "zero read bound rejected");
    require(reader->read_asset("WELCOME1.ASC",
                app::kMaximumTrustedDisplaySourceBytes + 1U).code ==
            app::TrustedDisplayAssetReadCode::invalid,
        "unapproved read bound rejected");
    require(reader->read_asset("MISSING.ASC",
                app::kMaximumTrustedDisplaySourceBytes).code ==
            app::TrustedDisplayAssetReadCode::not_found,
        "missing asset remains a typed not-found result");
}

void bounds_and_unavailable(const std::filesystem::path& root) {
    write_file(root / L"SMALL.ASC", "123456");
    write_file(root / L"OVERSIZE.ASC",
        std::string(app::kMaximumTrustedDisplaySourceBytes + 1U, 'X'));
    auto reader =
        platform::Win32TrustedDisplayAssetReader::open(root.wstring());
    require(reader != nullptr, "bounded reader opens");
    require(reader->read_asset("SMALL.ASC", 5U).code ==
            app::TrustedDisplayAssetReadCode::invalid,
        "asset larger than caller bound is invalid without partial data");
    require(reader->read_asset("OVERSIZE.ASC",
                app::kMaximumTrustedDisplaySourceBytes).code ==
            app::TrustedDisplayAssetReadCode::invalid,
        "asset larger than package limit is invalid without partial data");

    Handle locked(CreateFileW((root / L"SMALL.ASC").c_str(), GENERIC_READ,
        0U, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(locked.valid(), "exclusive test lock acquired");
    require(reader->read_asset("SMALL.ASC",
                app::kMaximumTrustedDisplaySourceBytes).code ==
            app::TrustedDisplayAssetReadCode::unavailable,
        "sharing denial is a typed unavailable result");
}

void reparse_confinement(
    const std::filesystem::path& base,
    const std::filesystem::path& root) {
    const auto outside = base / L"OUTSIDE";
    require(CreateDirectoryW(outside.c_str(), nullptr) != FALSE ||
            GetLastError() == ERROR_ALREADY_EXISTS,
        "outside test directory exists");
    write_file(outside / L"OUTSIDE.ASC", "outside bytes");

    const auto junction_file = root / L"JUNCTION.ASC";
    require(make_junction(junction_file, outside),
        "test junction created");
    auto reader =
        platform::Win32TrustedDisplayAssetReader::open(root.wstring());
    require(reader != nullptr, "reader remains valid with a child junction");
    require(!available(reader->read_asset("JUNCTION.ASC",
                app::kMaximumTrustedDisplaySourceBytes)),
        "child junction can never produce display bytes");

    const auto root_junction = base / L"ROOT-JUNCTION";
    require(make_junction(root_junction, root),
        "root junction created");
    require(platform::Win32TrustedDisplayAssetReader::open(
                root_junction.wstring()) == nullptr,
        "junction trusted root is rejected");

    const auto parent_junction = base / L"PARENT-JUNCTION";
    require(make_junction(parent_junction, base),
        "parent-component junction created");
    require(platform::Win32TrustedDisplayAssetReader::open(
                (parent_junction / root.filename()).wstring()) == nullptr,
        "reparse point in trusted-root ancestry is rejected");

    const auto file_link = root / L"SYMLINK.ASC";
    bool symlink_created = CreateSymbolicLinkW(
        file_link.c_str(), (outside / L"OUTSIDE.ASC").c_str(), 0U) != FALSE;
    if (!symlink_created) {
        constexpr DWORD allow_unprivileged = 0x2U;
        symlink_created = CreateSymbolicLinkW(file_link.c_str(),
            (outside / L"OUTSIDE.ASC").c_str(), allow_unprivileged) != FALSE;
    }
    if (symlink_created) {
        require(!available(reader->read_asset("SYMLINK.ASC",
                    app::kMaximumTrustedDisplaySourceBytes)),
            "file symlink can never produce display bytes");
    }
    require(make_junction(base / L"SYMLINK-COVERAGE", outside),
        "reparse-object fallback coverage is active");
}

void replacement_races(
    const std::filesystem::path& base,
    const std::filesystem::path& root) {
    write_file(root / L"RACE.ASC", "original trusted bytes");
    ReplacementRaceHook file_hook(ReplacementRaceHook::Target::file,
        base / L"moved-file.ASC");
    auto file_reader = platform::Win32TrustedDisplayAssetReader::open(
        root.wstring(), &file_hook);
    require(file_reader != nullptr, "file-race reader opens");
    const auto file_result = file_reader->read_asset(
        "RACE.ASC", app::kMaximumTrustedDisplaySourceBytes);
    require(file_hook.attempted(),
        "verified-file replacement hook reached");
    require(!file_hook.succeeded(),
        "verified file handle prevents replacement race");
    require(file_hook.error() != ERROR_SUCCESS,
        "blocked file replacement reports a failure code");
    require(available(file_result) &&
            file_result.bytes == "original trusted bytes",
        "verified handle supplies original bytes after blocked replacement");

    const auto root_race = base / L"ROOT-RACE";
    require(CreateDirectoryW(root_race.c_str(), nullptr) != FALSE,
        "isolated root-race directory created");
    write_file(root_race / L"RACE.ASC", "original trusted bytes");
    ReplacementRaceHook root_hook(ReplacementRaceHook::Target::root,
        base / L"moved-root");
    auto root_reader = platform::Win32TrustedDisplayAssetReader::open(
        root_race.wstring(), &root_hook);
    require(root_reader != nullptr, "root-race reader opens");
    const auto root_result = root_reader->read_asset(
        "RACE.ASC", app::kMaximumTrustedDisplaySourceBytes);
    require(root_hook.attempted(),
        "verified-root replacement hook reached");
    if (root_hook.succeeded()) {
        require(root_hook.replacement_installed(),
            "root replacement race installs deterministic hostile root");
        require(!available(root_result) && root_result.bytes.empty(),
            "moved-and-replaced trusted root fails closed without bytes");
        const auto retry = root_reader->read_asset(
            "RACE.ASC", app::kMaximumTrustedDisplaySourceBytes);
        require(!available(retry) && retry.bytes.empty(),
            "retained moved root cannot accept later replacement bytes");
    } else {
        require(root_hook.error() != ERROR_SUCCESS,
            "blocked root replacement reports a failure code");
        require(available(root_result) &&
                root_result.bytes == "original trusted bytes",
            "blocked root race preserves original verified read");
    }

    const auto swap_root = base / L"ROOT-SWAP-RESTORE";
    require(CreateDirectoryW(swap_root.c_str(), nullptr) != FALSE,
        "isolated swap-restore root created");
    write_file(swap_root / L"RACE.ASC", "swap-restore original bytes");
    SwapRestoreRaceHook swap_hook(base / L"swap-original-root",
        base / L"swap-hostile-candidate.ASC");
    auto swap_reader = platform::Win32TrustedDisplayAssetReader::open(
        swap_root.wstring(), &swap_hook);
    require(swap_reader != nullptr, "swap-restore reader opens");
    const auto swap_result = swap_reader->read_asset(
        "RACE.ASC", app::kMaximumTrustedDisplaySourceBytes);
    require(swap_hook.swap_attempted(),
        "swap-restore race reaches initial root swap");
    require(swap_hook.original_displaced(),
        "swap-restore race displaces original root");
    require(swap_hook.hostile_installed(),
        "swap-restore race installs hostile root before candidate open");
    require(swap_hook.restore_attempted(),
        "swap-restore race reaches candidate-open restore boundary");
    const auto candidate_displacement_failure =
        "swap-restore race displaces the open hostile candidate "
        "GetLastError=" +
        std::to_string(swap_hook.candidate_displacement_error());
    require(swap_hook.hostile_candidate_displaced(),
        candidate_displacement_failure.c_str());
    const auto hostile_root_removal_failure =
        "swap-restore race removes the hostile replacement root "
        "GetLastError=" +
        std::to_string(swap_hook.hostile_root_removal_error());
    require(swap_hook.hostile_root_removed(),
        hostile_root_removal_failure.c_str());
    const auto original_restore_failure =
        "swap-restore race restores approved root before final checks "
        "GetLastError=" +
        std::to_string(swap_hook.original_restore_error());
    require(swap_hook.original_restored(),
        original_restore_failure.c_str());
    require(!available(swap_result) && swap_result.bytes.empty(),
        "post-read candidate path rejects restored-root hostile handle");
    const auto swap_retry = swap_reader->read_asset(
        "RACE.ASC", app::kMaximumTrustedDisplaySourceBytes);
    require(!available(swap_retry) && swap_retry.bytes.empty(),
        "compromised reader has no usable retry result");
    auto reopened = platform::Win32TrustedDisplayAssetReader::open(
        swap_root.wstring());
    require(reopened != nullptr, "restored approved root can be reopened");
    const auto reopened_result = reopened->read_asset(
        "RACE.ASC", app::kMaximumTrustedDisplaySourceBytes);
    require(available(reopened_result) &&
            reopened_result.bytes == "swap-restore original bytes",
        "fresh reader uses restored original root rather than hostile bytes");
}

void invalid_root_forms(const std::filesystem::path& root) {
    require(platform::Win32TrustedDisplayAssetReader::open(L"relative") ==
            nullptr,
        "relative root rejected");
    require(platform::Win32TrustedDisplayAssetReader::open(
                L"\\\\server\\share\\DISPLAY") == nullptr,
        "UNC root rejected");
    require(platform::Win32TrustedDisplayAssetReader::open(
                L"\\\\?\\C:\\DISPLAY") == nullptr,
        "extended root rejected");
    require(platform::Win32TrustedDisplayAssetReader::open(
                L"\\\\.\\C:\\DISPLAY") == nullptr,
        "device root rejected");
    require(platform::Win32TrustedDisplayAssetReader::open(
                root.wstring() + L".") == nullptr,
        "trailing-dot root alias rejected");
    require(platform::Win32TrustedDisplayAssetReader::open(
                root.wstring() + L" ") == nullptr,
        "trailing-space root alias rejected");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2,
            "trusted-display path tests require a build-local directory");
        const auto requested = std::filesystem::absolute(argv[1]);
        require(CreateDirectoryW(requested.c_str(), nullptr) != FALSE ||
                GetLastError() == ERROR_ALREADY_EXISTS,
            "build-local test parent exists");
        // Repeated qualification retains prior evidence and Windows reuses PIDs.
        // Exclusively create a fresh bounded candidate; never delete/reuse an old
        // fixture or interpret its stale files as this execution's test setup.
        const auto prefix = L"window6b-display-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-";
        std::filesystem::path base;
        bool created = false;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            base = requested / (prefix + std::to_wstring(attempt));
            if (CreateDirectoryW(base.c_str(), nullptr)) { created = true; break; }
            if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        }
        require(created,
            "unique build-local test directory created");
        const auto root = base / L"DISPLAY";
        require(CreateDirectoryW(root.c_str(), nullptr) != FALSE,
            "trusted display root created");

        std::cout << "window6b-win32 ordinary-invalid-paths\n";
        ordinary_and_invalid_paths(root);
        std::cout << "window6b-win32 bounds-unavailable\n";
        bounds_and_unavailable(root);
        std::cout << "window6b-win32 reparse-confinement\n";
        reparse_confinement(base, root);
        std::cout << "window6b-win32 replacement-races\n";
        replacement_races(base, root);
        std::cout << "window6b-win32 invalid-root-forms\n";
        invalid_root_forms(root);

        std::cout
            << "all SFX Window 6B Win32 trusted-display path tests passed "
            << "assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "SFX Window 6B Win32 trusted-display path test failure: "
            << error.what() << '\n';
        return 1;
    }
}
