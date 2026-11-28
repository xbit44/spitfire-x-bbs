#include "sfx/platform/win32/trusted_display_asset_reader.hpp"
#include "sfx/application/ansi_playback_settings.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace sfx::platform::win32 {
namespace {

class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~ScopedHandle() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        const HANDLE value = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return value;
    }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) CloseHandle(handle_);
        handle_ = value;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct FileIdentity {
    std::uint64_t high{0};
    std::uint64_t low{0};

    bool operator==(const FileIdentity& other) const noexcept {
        return high == other.high && low == other.low;
    }
};

std::optional<FileIdentity> file_identity(const HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        return std::nullopt;
    }
    return FileIdentity{
        static_cast<std::uint64_t>(information.dwVolumeSerialNumber),
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
            static_cast<std::uint64_t>(information.nFileIndexLow)};
}

std::optional<DWORD> file_attributes(const HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        return std::nullopt;
    }
    return information.dwFileAttributes;
}

std::optional<std::int64_t> modified_time(const HANDLE handle) noexcept {
    FILETIME time{};
    if(!GetFileTime(handle,nullptr,nullptr,&time)) return std::nullopt;
    const auto ticks=(static_cast<std::uint64_t>(time.dwHighDateTime)<<32U)|time.dwLowDateTime;
    constexpr std::uint64_t epoch=116444736000000000ULL;
    if(ticks<epoch || ticks-epoch>static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
    return static_cast<std::int64_t>(ticks-epoch);
}

std::optional<std::wstring> final_path(const HANDLE handle) {
    const DWORD required =
        GetFinalPathNameByHandleW(handle, nullptr, 0U, FILE_NAME_NORMALIZED);
    if (required == 0U || required > 32768U) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, value.data(), required, FILE_NAME_NORMALIZED);
    if (written == 0U || written >= required) return std::nullopt;
    value.resize(written);
    while (value.size() > 4U &&
        (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

bool ordinal_equal(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()), TRUE) ==
        CSTR_EQUAL;
}

bool ordinal_prefix(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size() &&
        CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()),
            prefix.data(), static_cast<int>(prefix.size()), TRUE) ==
        CSTR_EQUAL;
}

bool safe_root_component(const std::wstring_view component) noexcept {
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ') {
        return false;
    }
    for (const wchar_t character : component) {
        if (character < 0x20 || character == L'/' ||
            character == L':' || character == L'*' ||
            character == L'?' || character == L'"' ||
            character == L'<' || character == L'>' ||
            character == L'|') {
            return false;
        }
    }
    return true;
}

std::optional<std::wstring> canonical_drive_rooted_path(
    const std::wstring& input) {
    if (input.size() < 3U ||
        !((input[0] >= L'A' && input[0] <= L'Z') ||
            (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' ||
        (input[2] != L'\\' && input[2] != L'/') ||
        input.rfind(L"\\\\", 0U) == 0U ||
        input.rfind(L"\\\\?\\", 0U) == 0U ||
        input.rfind(L"\\\\.\\", 0U) == 0U) {
        return std::nullopt;
    }

    std::wstring raw = input;
    std::replace(raw.begin(), raw.end(), L'/', L'\\');
    while (raw.size() > 3U && raw.back() == L'\\') raw.pop_back();
    std::size_t raw_start = 3U;
    while (raw_start < raw.size()) {
        const auto raw_end = raw.find(L'\\', raw_start);
        const auto component = std::wstring_view(raw).substr(raw_start,
            raw_end == std::wstring::npos
                ? raw.size() - raw_start
                : raw_end - raw_start);
        if (!safe_root_component(component)) return std::nullopt;
        if (raw_end == std::wstring::npos) break;
        raw_start = raw_end + 1U;
    }

    const DWORD required =
        GetFullPathNameW(input.c_str(), 0U, nullptr, nullptr);
    if (required == 0U || required > 32768U) return std::nullopt;
    std::wstring path(required, L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), required, path.data(), nullptr);
    if (written == 0U || written >= required) return std::nullopt;
    path.resize(written);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3U && path.back() == L'\\') path.pop_back();

    std::size_t start = 3U;
    while (start < path.size()) {
        const auto end = path.find(L'\\', start);
        const auto component = std::wstring_view(path).substr(start,
            end == std::wstring::npos ? path.size() - start : end - start);
        if (!safe_root_component(component)) return std::nullopt;
        if (end == std::wstring::npos) break;
        start = end + 1U;
    }
    return path;
}

ScopedHandle open_directory(const std::wstring& path) noexcept {
    return ScopedHandle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

bool verified_directory(const HANDLE handle) noexcept {
    const auto attributes = file_attributes(handle);
    return attributes.has_value() &&
        (*attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
        (*attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

std::optional<ScopedHandle> open_verified_root(
    const std::wstring& canonical_root) {
    std::wstring current = canonical_root.substr(0U, 3U);
    ScopedHandle current_handle = open_directory(current);
    if (!current_handle.valid() || !verified_directory(current_handle.get())) {
        return std::nullopt;
    }
    std::size_t start = 3U;
    while (start < canonical_root.size()) {
        const auto end = canonical_root.find(L'\\', start);
        const auto count = end == std::wstring::npos
            ? canonical_root.size() - start
            : end - start;
        if (current.size() > 3U) current += L'\\';
        current.append(canonical_root, start, count);
        ScopedHandle next = open_directory(current);
        if (!next.valid() || !verified_directory(next.get())) {
            return std::nullopt;
        }
        current_handle = std::move(next);
        if (end == std::wstring::npos) break;
        start = end + 1U;
    }
    return std::optional<ScopedHandle>{std::move(current_handle)};
}

std::wstring widen_ascii(const std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if (character > 0x7fU) return {};
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

application::TrustedDisplayAssetReadCode code_for_open_error(
    const DWORD error) noexcept {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return application::TrustedDisplayAssetReadCode::not_found;
    }
    return application::TrustedDisplayAssetReadCode::unavailable;
}

bool same_root_identity(
    const HANDLE handle,
    const FileIdentity expected) noexcept {
    const auto current = file_identity(handle);
    return current.has_value() && *current == expected &&
        verified_directory(handle);
}

bool same_root_at_approved_path(
    const HANDLE handle,
    const FileIdentity expected,
    const std::wstring_view approved_final_path) {
    if (!same_root_identity(handle, expected)) return false;
    const auto current_final_path = final_path(handle);
    return current_final_path.has_value() &&
        ordinal_equal(*current_final_path, approved_final_path);
}

bool candidate_at_approved_path(
    const HANDLE handle,
    const std::wstring_view approved_root,
    const std::wstring_view approved_leaf) {
    const auto current_final_path = final_path(handle);
    if (!current_final_path.has_value()) return false;
    const std::wstring prefix =
        std::wstring(approved_root) + L"\\";
    if (!ordinal_prefix(*current_final_path, prefix)) return false;
    const std::wstring_view current_leaf =
        std::wstring_view(*current_final_path).substr(prefix.size());
    return current_leaf.find(L'\\') == std::wstring_view::npos &&
        current_leaf.find(L'/') == std::wstring_view::npos &&
        ordinal_equal(current_leaf, approved_leaf);
}

}  // namespace

std::unique_ptr<Win32TrustedDisplayAssetReader>
Win32TrustedDisplayAssetReader::open(
    std::wstring trusted_root,
    ITrustedDisplayReadHook* const test_hook) {
    try {
        const auto canonical = canonical_drive_rooted_path(trusted_root);
        if (!canonical.has_value()) return nullptr;
        auto opened = open_verified_root(*canonical);
        if (!opened.has_value()) return nullptr;
        const auto identity = file_identity(opened->get());
        const auto verified_final = final_path(opened->get());
        if (!identity.has_value() || !verified_final.has_value()) {
            return nullptr;
        }
        const HANDLE retained = opened->release();
        return std::unique_ptr<Win32TrustedDisplayAssetReader>(
            new Win32TrustedDisplayAssetReader(*canonical, *verified_final,
                retained, identity->high, identity->low, test_hook));
    } catch (...) {
        return nullptr;
    }
}

Win32TrustedDisplayAssetReader::Win32TrustedDisplayAssetReader(
    std::wstring canonical_root,
    std::wstring final_root,
    void* const root_handle,
    const std::uint64_t root_identity_high,
    const std::uint64_t root_identity_low,
    ITrustedDisplayReadHook* const test_hook) noexcept
    : canonical_root_(std::move(canonical_root)),
      final_root_(std::move(final_root)),
      root_handle_(root_handle),
      root_identity_high_(root_identity_high),
      root_identity_low_(root_identity_low),
      test_hook_(test_hook) {}

Win32TrustedDisplayAssetReader::~Win32TrustedDisplayAssetReader() {
    if (root_handle_ != nullptr &&
        root_handle_ != static_cast<void*>(INVALID_HANDLE_VALUE)) {
        CloseHandle(static_cast<HANDLE>(root_handle_));
    }
}

application::TrustedDisplayAssetReadResult
Win32TrustedDisplayAssetReader::read_asset(
    const std::string_view logical_identity,
    const std::size_t maximum_bytes) {
    using application::TrustedDisplayAssetReadCode;
    if (invalidated_.load(std::memory_order_acquire)) {
        return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
    }
    // These two fixed stock leaves contain hyphens. Do not widen the ordinary
    // include/macro filename grammar or permit arbitrary paths here.
    if ((!application::valid_include_filename(logical_identity) &&
            logical_identity != "bg-prelog.ans" && logical_identity != "chat-emote.txt") ||
        maximum_bytes == 0U ||
        maximum_bytes > application::kMaximumTrustedDisplaySourceBytes) {
        return {TrustedDisplayAssetReadCode::invalid, {}, {}, {}};
    }
    try {
        const FileIdentity expected_root{
            root_identity_high_, root_identity_low_};
        if (!same_root_at_approved_path(
                static_cast<HANDLE>(root_handle_), expected_root,
                final_root_)) {
            return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
        }

        auto current_root = open_verified_root(canonical_root_);
        if (!current_root.has_value() ||
            !same_root_at_approved_path(
                current_root->get(), expected_root, final_root_)) {
            return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
        }
        if (test_hook_ != nullptr) {
            test_hook_->on_stage(TrustedDisplayReadStage::root_revalidated,
                canonical_root_, canonical_root_);
        }

        const std::wstring leaf = widen_ascii(logical_identity);
        if (leaf.empty()) {
            return {TrustedDisplayAssetReadCode::invalid, {}, {}, {}};
        }
        const std::wstring candidate = canonical_root_ + L"\\" + leaf;
        DWORD candidate_sharing = FILE_SHARE_READ;
        if (test_hook_ != nullptr &&
            test_hook_->widen_candidate_sharing_for_race_test()) {
            candidate_sharing |= FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        }
        ScopedHandle file(CreateFileW(candidate.c_str(),
            GENERIC_READ | FILE_READ_ATTRIBUTES, candidate_sharing,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!file.valid()) {
            return {code_for_open_error(GetLastError()), {}, {}, {}};
        }

        const auto attributes = file_attributes(file.get());
        const auto opened_final = final_path(file.get());
        const auto identity = file_identity(file.get());
        const auto modified = modified_time(file.get());
        if (!attributes.has_value() || !opened_final.has_value() ||
            !identity.has_value() ||
            (*attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            (*attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return {TrustedDisplayAssetReadCode::invalid, {}, {}, {}};
        }

        if (!candidate_at_approved_path(
                file.get(), final_root_, leaf)) {
            return {TrustedDisplayAssetReadCode::invalid, {}, {}, {}};
        }
        if (test_hook_ != nullptr) {
            test_hook_->on_stage(TrustedDisplayReadStage::candidate_opened,
                canonical_root_, candidate);
        }
        if (!same_root_at_approved_path(
                current_root->get(), expected_root, final_root_) ||
            !same_root_at_approved_path(
                static_cast<HANDLE>(root_handle_), expected_root,
                final_root_)) {
            return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
            static_cast<unsigned long long>(size.QuadPart) >
                static_cast<unsigned long long>(maximum_bytes)) {
            return {TrustedDisplayAssetReadCode::invalid, {}, {}, {}};
        }
        if (test_hook_ != nullptr) {
            test_hook_->on_stage(TrustedDisplayReadStage::file_verified,
                canonical_root_, candidate);
        }

        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t remaining = bytes.size() - offset;
            const DWORD offered = static_cast<DWORD>(
                std::min<std::size_t>(remaining, 64U * 1024U));
            DWORD read = 0;
            if (!ReadFile(file.get(), bytes.data() + offset, offered,
                    &read, nullptr) ||
                read == 0U || read > offered) {
                return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
            }
            offset += read;
        }

        const auto final_identity = file_identity(file.get());
        const bool final_candidate_path =
            candidate_at_approved_path(file.get(), final_root_, leaf);
        const bool final_current_root =
            same_root_at_approved_path(
                current_root->get(), expected_root, final_root_);
        const bool final_retained_root =
            same_root_at_approved_path(
                static_cast<HANDLE>(root_handle_), expected_root,
                final_root_);
        if (!final_identity.has_value() || !(*final_identity == *identity) ||
            modified_time(file.get())!=modified ||
            !final_candidate_path || !final_current_root ||
            !final_retained_root) {
            invalidated_.store(true, std::memory_order_release);
            return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
        }
        const auto boundary_identity = file_identity(file.get());
        if (!boundary_identity.has_value() ||
            !(*boundary_identity == *identity) ||
            modified_time(file.get())!=modified ||
            !candidate_at_approved_path(file.get(), final_root_, leaf) ||
            !same_root_at_approved_path(
                current_root->get(), expected_root, final_root_) ||
            !same_root_at_approved_path(
                static_cast<HANDLE>(root_handle_), expected_root,
                final_root_)) {
            invalidated_.store(true, std::memory_order_release);
            return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
        }
        const std::string stable =
            std::to_string(identity->high) + ":" +
            std::to_string(identity->low);
        return {TrustedDisplayAssetReadCode::available,
            std::string(logical_identity), stable, std::move(bytes), modified,
            application::ansi_playback_bps(candidate)};
    } catch (...) {
        return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
    }
}

}  // namespace sfx::platform::win32
