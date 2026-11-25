#include "sfx/archive/archive_engine.hpp"
#include "sfx/archive/managed_journal_snapshot.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sfx::archive {
namespace {

constexpr std::size_t io_buffer_bytes = 64U * 1024U;
constexpr const char* info_header = "SFX-BACKUP-1";
constexpr const char* manifest_header = "SFX-BACKUP-MANIFEST-1";

struct ArchiveDeleter final {
    void operator()(struct archive* value) const noexcept {
        if (value != nullptr) archive_free(value);
    }
};

struct EntryDeleter final {
    void operator()(struct archive_entry* value) const noexcept {
        if (value != nullptr) archive_entry_free(value);
    }
};

using ArchivePtr = std::unique_ptr<struct archive, ArchiveDeleter>;
using EntryPtr = std::unique_ptr<struct archive_entry, EntryDeleter>;

Result failure(const Error error, std::string detail) {
    return Result{error, std::move(detail)};
}

bool is_cancelled(const CancelCheck& check) {
    return check && check();
}

std::string archive_error(struct archive* value) {
    const char* text = value == nullptr ? nullptr : archive_error_string(value);
    return text == nullptr ? "archive operation failed" : text;
}

std::string safe_diagnostic_text(const std::string& value, const std::size_t limit = 384) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char byte : value) {
        std::string part;
        if (byte >= 32 && byte < 127 && byte != '"' && byte != '\\') part += static_cast<char>(byte);
        else {
            part = "\\x";
            part += hex[byte >> 4];
            part += hex[byte & 15];
        }
        if (result.size() + part.size() > limit) { result += "..."; break; }
        result += part;
    }
    return result;
}

Result source_failure(const Error error, const std::string& reason,
                      const std::string& member, const std::uint64_t expected,
                      const std::uint64_t observed, const std::string& extra = {}) {
    auto detail = reason + "; file=\"" + safe_diagnostic_text(member) +
                  "\"; expected_bytes=" + std::to_string(expected) +
                  "; read_bytes=" + std::to_string(observed);
    if (!extra.empty()) detail += "; " + safe_diagnostic_text(extra, 256);
    return failure(error, std::move(detail));
}

bool checked_add(std::uint64_t& value, const std::uint64_t addition,
                 const std::uint64_t maximum) {
    if (addition > maximum || value > maximum - addition) return false;
    value += addition;
    return true;
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

bool equals_case_insensitive_ascii(const std::string& left,
                                   const std::string& right) {
    return ascii_lower(left) == ascii_lower(right);
}

bool reserved_windows_component(const std::string& component) {
    auto value = ascii_lower(component);
    const auto dot = value.find('.');
    if (dot != std::string::npos) value.resize(dot);
    if (value == "con" || value == "prn" || value == "aux" || value == "nul") {
        return true;
    }
    if (value.size() == 4 &&
        ((value.rfind("com", 0) == 0) || (value.rfind("lpt", 0) == 0)) &&
        value[3] >= '1' && value[3] <= '9') {
        return true;
    }
    return false;
}

Result validate_archive_path(const std::string& path, const Limits& limits) {
    if (path.empty() || path.size() > limits.maximum_path_bytes ||
        path.front() == '/' || path.front() == '\\' || path.back() == '/' ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
        return failure(Error::unsafe_path, "unsafe archive path");
    }

    std::size_t depth = 0;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto length = (end == std::string::npos ? path.size() : end) - begin;
        if (length == 0) return failure(Error::unsafe_path, "empty path component");
        const std::string component = path.substr(begin, length);
        if (component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ' ||
            reserved_windows_component(component)) {
            return failure(Error::unsafe_path, "unsafe path component");
        }
        for (const unsigned char byte : component) {
            if (byte < 0x20U || byte == 0x7fU) {
                return failure(Error::unsafe_path, "control character in path");
            }
        }
        ++depth;
        if (depth > limits.maximum_path_depth) {
            return failure(Error::limit_exceeded, "archive path depth limit exceeded");
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}

std::string collision_key(const std::string& path) {
#ifdef _WIN32
    if (path.empty() || path.size() > static_cast<std::size_t>(INT_MAX)) return {};
    const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               path.data(),
                                               static_cast<int>(path.size()),
                                               nullptr, 0);
    if (wide_size <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), wide.data(), wide_size) !=
        wide_size) {
        return {};
    }
    const int folded_size = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE,
                                         wide.data(), wide_size, nullptr, 0);
    if (folded_size <= 0) return {};
    std::wstring folded(static_cast<std::size_t>(folded_size), L'\0');
    if (LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, wide.data(), wide_size,
                     folded.data(), folded_size) != folded_size) {
        return {};
    }
    const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                              folded.data(), folded_size, nullptr,
                                              0, nullptr, nullptr);
    if (utf8_size <= 0) return {};
    std::string result(static_cast<std::size_t>(utf8_size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, folded.data(),
                            folded_size, result.data(), utf8_size, nullptr,
                            nullptr) != utf8_size) {
        return {};
    }
    return result;
#else
    return path;
#endif
}

bool is_reparse_or_symlink(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)) ||
           static_cast<bool>(error);
#endif
}

std::string to_archive_path(const std::filesystem::path& relative) {
    return relative.generic_u8string();
}

std::string hexadecimal(const unsigned char* bytes, const std::size_t count) {
    static constexpr char alphabet[] = "0123456789ABCDEF";
    std::string result(count * 2U, '0');
    for (std::size_t index = 0; index < count; ++index) {
        result[index * 2U] = alphabet[(bytes[index] >> 4U) & 0x0fU];
        result[index * 2U + 1U] = alphabet[bytes[index] & 0x0fU];
    }
    return result;
}

Result hash_file(const std::filesystem::path& path, const std::uint64_t expected_size,
                 std::string& digest, const CancelCheck& cancelled,const Progress& progress = {}) {
    if (sodium_init() < 0) return failure(Error::source_io, "SHA-256 unavailable");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return failure(Error::source_io, "cannot open source file");
    crypto_hash_sha256_state state{};
    if (crypto_hash_sha256_init(&state) != 0) {
        return failure(Error::source_io, "cannot initialize SHA-256");
    }
    std::array<char, io_buffer_bytes> buffer{};
    std::uint64_t bytes = 0;
    while (stream) {
        if (is_cancelled(cancelled)) return failure(Error::cancelled, "cancelled");
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count <= 0) break;
        bytes += static_cast<std::uint64_t>(count);
        if (bytes > expected_size ||
            crypto_hash_sha256_update(
                &state, reinterpret_cast<const unsigned char*>(buffer.data()),
                static_cast<unsigned long long>(count)) != 0) {
            return failure(Error::source_changed, "source changed while hashing");
        }
        if(progress)progress(bytes,expected_size,{});
    }
    if (stream.bad() || bytes != expected_size) {
        return failure(Error::source_changed, "source changed while hashing");
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> output{};
    if (crypto_hash_sha256_final(&state, output.data()) != 0) {
        return failure(Error::source_io, "cannot finalize SHA-256");
    }
    digest = hexadecimal(output.data(), output.size());
    sodium_memzero(output.data(), output.size());
    return {};
}

std::string hash_memory(const std::string& value) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> output{};
    if (sodium_init() < 0 ||
        crypto_hash_sha256(output.data(),
                           reinterpret_cast<const unsigned char*>(value.data()),
                           static_cast<unsigned long long>(value.size())) != 0) {
        return {};
    }
    const auto result = hexadecimal(output.data(), output.size());
    sodium_memzero(output.data(), output.size());
    return result;
}

bool safe_metadata_value(const std::string& value) {
    if (value.empty() || value.size() > 1024) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '=';
    });
}

std::string serialize_metadata(const Metadata& metadata) {
    std::ostringstream output;
    output << info_header << '\n'
           << "backup_type=" << metadata.backup_type << '\n'
           << "transaction_id=" << metadata.transaction_id << '\n'
           << "sfx_version=" << metadata.sfx_version << '\n'
           << "source_generation=" << metadata.source_generation << '\n'
           << "created_local=" << metadata.created.local << '\n'
           << "created_utc_offset=" << metadata.created.utc_offset << '\n'
           << "created_utc=" << metadata.created.utc << '\n';
    return output.str();
}

Result validate_metadata(const Metadata& metadata) {
    if (!safe_metadata_value(metadata.backup_type) ||
        !safe_metadata_value(metadata.transaction_id) ||
        !safe_metadata_value(metadata.sfx_version) ||
        !safe_metadata_value(metadata.source_generation) ||
        !safe_metadata_value(metadata.created.filename_component) ||
        !safe_metadata_value(metadata.created.local) ||
        !safe_metadata_value(metadata.created.utc_offset) ||
        !safe_metadata_value(metadata.created.utc)) {
        return failure(Error::metadata_invalid, "invalid backup metadata");
    }
    return {};
}

Result parse_metadata(const std::string& text, Metadata& metadata) {
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != info_header) {
        return failure(Error::metadata_invalid, "backup information header missing");
    }
    std::unordered_map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos || equals == 0 ||
            !fields.emplace(line.substr(0, equals), line.substr(equals + 1)).second) {
            return failure(Error::metadata_invalid, "invalid backup information field");
        }
    }
    const std::array<const char*, 7> names{{
        "backup_type", "transaction_id", "sfx_version", "source_generation",
        "created_local", "created_utc_offset", "created_utc"}};
    if (fields.size() != names.size()) {
        return failure(Error::metadata_invalid, "backup information fields incomplete");
    }
    for (const char* name : names) {
        if (fields.find(name) == fields.end()) {
            return failure(Error::metadata_invalid, "backup information field missing");
        }
    }
    metadata.backup_type = fields["backup_type"];
    metadata.transaction_id = fields["transaction_id"];
    metadata.sfx_version = fields["sfx_version"];
    metadata.source_generation = fields["source_generation"];
    metadata.created.local = fields["created_local"];
    metadata.created.utc_offset = fields["created_utc_offset"];
    metadata.created.utc = fields["created_utc"];
    metadata.created.filename_component.clear();
    return validate_metadata(Metadata{metadata.backup_type, metadata.transaction_id,
                                      metadata.sfx_version, metadata.source_generation,
                                      Timestamp{"inspection", metadata.created.local,
                                                metadata.created.utc_offset,
                                                metadata.created.utc}});
}

std::string serialize_manifest(const std::vector<FileRecord>& records) {
    std::ostringstream output;
    const bool modes=std::any_of(records.begin(),records.end(),[](const auto& record){return record.owner_executable;});
    output << (modes?"SFX-BACKUP-MANIFEST-2":manifest_header) << '\n';
    for (const auto& record : records) {
        output << record.path << '\t' << record.size << '\t' << record.sha256;
        if(modes)output << '\t' << (record.owner_executable?'1':'0');
        output << '\n';
    }
    return output.str();
}

Result parse_decimal(const std::string& text, std::uint64_t& value) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return failure(Error::manifest_invalid, "invalid manifest size");
    }
    value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9' ||
            value > (std::numeric_limits<std::uint64_t>::max() -
                     static_cast<std::uint64_t>(ch - '0')) /
                        10U) {
            return failure(Error::manifest_invalid, "invalid manifest size");
        }
        value = value * 10U + static_cast<std::uint64_t>(ch - '0');
    }
    return {};
}

Result parse_manifest(const std::string& text, const Limits& limits,
                      std::vector<FileRecord>& records) {
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || (line != manifest_header && line != "SFX-BACKUP-MANIFEST-2")) {
        return failure(Error::manifest_invalid, "backup manifest header missing");
    }
    const bool modes=line=="SFX-BACKUP-MANIFEST-2";
    std::set<std::string> keys;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return failure(Error::manifest_invalid, "blank manifest record");
        const auto first = line.find('\t');
        const auto second = first == std::string::npos
                                ? std::string::npos
                                : line.find('\t', first + 1);
        const auto third=second==std::string::npos?std::string::npos:line.find('\t',second+1);
        if (first == std::string::npos || second == std::string::npos ||
            (modes?(third==std::string::npos||line.find('\t',third+1)!=std::string::npos):third!=std::string::npos)) {
            return failure(Error::manifest_invalid, "invalid manifest record");
        }
        FileRecord record;
        record.path = line.substr(0, first);
        const auto path_result = validate_archive_path(record.path, limits);
        if (!path_result.ok()) return path_result;
        const auto size_result = parse_decimal(
            line.substr(first + 1, second - first - 1), record.size);
        if (!size_result.ok()) return size_result;
        record.sha256 = line.substr(second + 1,modes?third-second-1:std::string::npos);
        if(modes){const auto execute=line.substr(third+1);if(execute!="0"&&execute!="1")return failure(Error::manifest_invalid,"invalid manifest owner-execute bit");record.owner_executable=execute=="1";}
        if (record.sha256.size() != crypto_hash_sha256_BYTES * 2U ||
            !std::all_of(record.sha256.begin(), record.sha256.end(), [](const char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
            })) {
            return failure(Error::manifest_invalid, "invalid manifest SHA-256");
        }
        const auto key = collision_key(record.path);
        if (key.empty() || !keys.insert(key).second) {
            return failure(Error::manifest_invalid, "duplicate manifest path");
        }
        records.push_back(std::move(record));
        if (records.size() > limits.maximum_entries) {
            return failure(Error::limit_exceeded, "manifest entry limit exceeded");
        }
    }
    return {};
}

struct SourceFile final {
    std::filesystem::path native_path;
    FileRecord record;
    std::time_t modified{};
    bool native_mode_captured{};
};

bool excluded_root(const std::filesystem::path& relative,
                   const std::vector<std::string>& excluded) {
    if (relative.empty()) return false;
    const auto first = to_archive_path(*relative.begin());
    return std::any_of(excluded.begin(), excluded.end(), [&](const std::string& name) {
        return equals_case_insensitive_ascii(first, name);
    });
}

bool included_root(const std::filesystem::path& relative,
                   const std::vector<std::string>& included) {
    if (included.empty() || relative.empty()) return true;
    const auto first = to_archive_path(*relative.begin());
    return std::any_of(included.begin(), included.end(), [&](const std::string& name) {
        return equals_case_insensitive_ascii(first, name);
    });
}

bool excluded_relative_path(const std::filesystem::path& relative,
                            const std::vector<std::string>& excluded) {
    const auto candidate = to_archive_path(relative);
    return std::any_of(excluded.begin(), excluded.end(), [&](const std::string& path) {
        return equals_case_insensitive_ascii(candidate, path);
    });
}

bool wildcard_match(const std::string& pattern, const std::string& value) {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::string::npos;
    std::size_t retry = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' ||
             std::tolower(static_cast<unsigned char>(pattern[pattern_index])) ==
                 std::tolower(static_cast<unsigned char>(value[value_index])))) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            star = pattern_index++;
            retry = value_index;
        } else if (star != std::string::npos) {
            pattern_index = star + 1;
            value_index = ++retry;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool selected_path(const std::string& path,
                   const std::vector<std::string>& included,
                   const std::vector<std::string>& excluded) {
    const bool selected = included.empty() ||
        std::any_of(included.begin(), included.end(), [&](const std::string& pattern) {
            return wildcard_match(pattern, path);
        });
    return selected &&
        !std::any_of(excluded.begin(), excluded.end(), [&](const std::string& pattern) {
            return wildcard_match(pattern, path);
        });
}

std::time_t modification_time(const std::filesystem::path& path) {
    std::error_code error;
    const auto file_time = std::filesystem::last_write_time(path, error);
    if (error) return std::time(nullptr);
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(system_time);
}

Result collect_sources(const CreateBackupRequest& request,
                       std::vector<SourceFile>& sources,
                       std::uint64_t& total_bytes) {
    std::error_code error;
    if (!std::filesystem::is_directory(request.source_root, error) || error ||
        is_reparse_or_symlink(request.source_root)) {
        return failure(Error::invalid_request, "source root is not a safe directory");
    }

    std::set<std::string> keys;
    const auto add_source = [&](const std::filesystem::path& native_path,
                                const std::string& archive_path) -> Result {
        if (is_reparse_or_symlink(native_path)) {
            return failure(Error::unsafe_path, "backup source override contains a link");
        }
        const auto status = std::filesystem::status(native_path, error);
        if (error || !std::filesystem::is_regular_file(status)) {
            return failure(Error::source_io, "cannot inspect backup source file");
        }
        SourceFile source;
        source.native_path = native_path;
        source.record.path = archive_path;
        const auto path_result = validate_archive_path(source.record.path, request.limits);
#ifdef __linux__
        source.record.owner_executable=(status.permissions()&std::filesystem::perms::owner_exec)!=std::filesystem::perms::none;
        source.native_mode_captured=true;
#endif
        if (!path_result.ok()) return path_result;
        const auto key = collision_key(source.record.path);
        if (key.empty() || !keys.insert(key).second) {
            return failure(Error::unsafe_path, "backup contains a case-colliding path");
        }
        source.record.size = std::filesystem::file_size(native_path, error);
        if (error) return failure(Error::source_io, "cannot read backup source file size");
        if (source.record.size > request.limits.maximum_file_bytes ||
            !checked_add(total_bytes, source.record.size,
                         request.limits.maximum_total_bytes)) {
            return failure(Error::limit_exceeded, "backup size limit exceeded");
        }
        source.modified = modification_time(source.native_path);
        const auto hash_result = hash_file(source.native_path, source.record.size,
                                           source.record.sha256,
                                           request.cancelled,[&](auto bytes,auto,const auto&){if(request.backup_progress)request.backup_progress({BackupStage::inventory,total_bytes-source.record.size+bytes,0,sources.size(),0});});
        if (!hash_result.ok()) return hash_result;
        sources.push_back(std::move(source));
        if (sources.size() + 2U > request.limits.maximum_entries) {
            return failure(Error::limit_exceeded, "backup entry limit exceeded");
        }
        return {};
    };

    std::filesystem::recursive_directory_iterator iterator(
        request.source_root, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) return failure(Error::source_io, "cannot enumerate source root");
    for (; iterator != end; iterator.increment(error)) {
        if (error) return failure(Error::source_io, "cannot enumerate source root");
        if (is_cancelled(request.cancelled)) return failure(Error::cancelled, "cancelled");
        const auto relative = std::filesystem::u8path(archive_relative(
            to_archive_path(iterator->path().lexically_relative(request.source_root)), request.physical_layout));
        if (!included_root(relative, request.included_root_names) ||
            excluded_root(relative, request.excluded_root_names) ||
            excluded_relative_path(relative, request.excluded_relative_paths)
#if defined(__linux__)
            || (request.linux_journal_snapshot_set &&
                is_linux_journal_snapshot_member(to_archive_path(relative)))
#endif
            ) {
            if (iterator->is_directory(error) && !error) iterator.disable_recursion_pending();
            error.clear();
            continue;
        }
        if (is_reparse_or_symlink(iterator->path())) {
            return failure(Error::unsafe_path, "source contains a reparse point or link");
        }
        const auto status = iterator->status(error);
        if (error) return failure(Error::source_io, "cannot inspect source entry");
        if (std::filesystem::is_directory(status)) continue;
        if (!std::filesystem::is_regular_file(status)) {
            return failure(Error::unsafe_path, "source contains a non-regular file");
        }
        const auto added = add_source(iterator->path(), to_archive_path(relative));
        if (!added.ok()) return added;
    }
    for (const auto& override_source : request.source_overrides) {
        const auto added = add_source(override_source.native_path,
                                      override_source.archive_path);
        if (!added.ok()) return added;
    }
    std::sort(sources.begin(), sources.end(), [](const SourceFile& left,
                                                  const SourceFile& right) {
        const auto left_key = ascii_lower(left.record.path);
        const auto right_key = ascii_lower(right.record.path);
        return left_key == right_key ? left.record.path < right.record.path
                                     : left_key < right_key;
    });
    return {};
}

Result collect_zip_sources(const CreateZipRequest& request,
                           std::vector<SourceFile>& sources,
                           std::uint64_t& total_bytes) {
    std::error_code error;
    if (!std::filesystem::is_directory(request.source_root, error) || error ||
        is_reparse_or_symlink(request.source_root) || request.input_paths.empty()) {
        return failure(Error::invalid_request, "source root or input list is invalid");
    }
    for (const auto& pattern : request.included_patterns) {
        if (pattern.empty() || pattern.size() > request.limits.maximum_path_bytes ||
            pattern.find('\\') != std::string::npos || pattern.find(':') != std::string::npos) {
            return failure(Error::unsafe_path, "unsafe include pattern");
        }
    }
    for (const auto& pattern : request.excluded_patterns) {
        if (pattern.empty() || pattern.size() > request.limits.maximum_path_bytes ||
            pattern.find('\\') != std::string::npos || pattern.find(':') != std::string::npos) {
            return failure(Error::unsafe_path, "unsafe exclude pattern");
        }
    }

    std::set<std::string> keys;
    const auto add_file = [&](const std::filesystem::path& native_path,
                              const std::filesystem::path& relative) -> Result {
        const auto archive_path = to_archive_path(relative.lexically_normal());
        const auto path_result = validate_archive_path(archive_path, request.limits);
        if (!path_result.ok()) return path_result;
        if (!selected_path(archive_path, request.included_patterns,
                           request.excluded_patterns)) return {};
        const auto key = collision_key(archive_path);
        if (key.empty()) return failure(Error::unsafe_path, "invalid Unicode input path");
        if (!keys.insert(key).second) return {};
        if (is_reparse_or_symlink(native_path)) {
            return failure(Error::unsafe_path, "input contains a reparse point or link");
        }
        const auto status = std::filesystem::status(native_path, error);
        if (error || !std::filesystem::is_regular_file(status)) {
            return failure(Error::source_io, "required input is not a regular readable file");
        }
        SourceFile source;
        source.native_path = native_path;
        source.record.path = archive_path;
        source.record.size = std::filesystem::file_size(native_path, error);
        if (error) return failure(Error::source_io, "cannot read input size");
        if (source.record.size > request.limits.maximum_file_bytes ||
            !checked_add(total_bytes, source.record.size,
                         request.limits.maximum_total_bytes)) {
            return failure(Error::limit_exceeded, "ZIP input size limit exceeded");
        }
        source.modified = modification_time(native_path);
        const auto hash_result = hash_file(native_path, source.record.size,
                                           source.record.sha256,
                                           request.cancelled);
        if (!hash_result.ok()) return hash_result;
        sources.push_back(std::move(source));
        if (sources.size() > request.limits.maximum_entries) {
            return failure(Error::limit_exceeded, "ZIP entry limit exceeded");
        }
        return {};
    };

    for (const auto& raw : request.input_paths) {
        if (is_cancelled(request.cancelled)) return failure(Error::cancelled, "cancelled");
        if (raw.empty() || raw.is_absolute() || raw.has_root_name() || raw.has_root_directory()) {
            return failure(Error::unsafe_path, "input must be relative to the working directory");
        }
        const auto relative = raw.lexically_normal();
        const auto relative_text = to_archive_path(relative);
        const auto path_result = validate_archive_path(relative_text, request.limits);
        if (!path_result.ok()) return path_result;
        auto current = request.source_root;
        for (const auto& component : relative) {
            current /= component;
            if (is_reparse_or_symlink(current)) {
                return failure(Error::unsafe_path, "input path contains a reparse point or link");
            }
        }
        const auto source = request.source_root / relative;
        const auto status = std::filesystem::status(source, error);
        if (error || !std::filesystem::exists(status)) {
            return failure(Error::source_io, "required input is missing");
        }
        if (std::filesystem::is_regular_file(status)) {
            const auto added = add_file(source, relative);
            if (!added.ok()) return added;
            continue;
        }
        if (!std::filesystem::is_directory(status) || !request.recursive) {
            return failure(Error::invalid_request,
                           "directories require recursive creation (-r)");
        }
        std::filesystem::recursive_directory_iterator iterator(
            source, std::filesystem::directory_options::none, error);
        const std::filesystem::recursive_directory_iterator end;
        if (error) return failure(Error::source_io, "cannot enumerate input directory");
        for (; iterator != end; iterator.increment(error)) {
            if (error) return failure(Error::source_io, "cannot enumerate input directory");
            if (is_cancelled(request.cancelled)) return failure(Error::cancelled, "cancelled");
            if (is_reparse_or_symlink(iterator->path())) {
                return failure(Error::unsafe_path, "input contains a reparse point or link");
            }
            const auto item_status = iterator->status(error);
            if (error) return failure(Error::source_io, "cannot inspect input");
            if (std::filesystem::is_directory(item_status)) continue;
            if (!std::filesystem::is_regular_file(item_status)) {
                return failure(Error::unsafe_path, "input contains a non-regular file");
            }
            const auto child = iterator->path().lexically_relative(request.source_root);
            const auto added = add_file(iterator->path(), child);
            if (!added.ok()) return added;
        }
    }
    if (sources.empty()) return failure(Error::source_io, "no input files selected");
    std::sort(sources.begin(), sources.end(), [](const SourceFile& left,
                                                  const SourceFile& right) {
        const auto left_key = ascii_lower(left.record.path);
        const auto right_key = ascii_lower(right.record.path);
        return left_key == right_key ? left.record.path < right.record.path
                                     : left_key < right_key;
    });
    return {};
}

int open_archive_for_write(struct archive* writer,
                           const std::filesystem::path& path) {
#ifdef _WIN32
    return archive_write_open_filename_w(writer, path.c_str());
#else
    return archive_write_open_filename(writer, path.c_str());
#endif
}

int open_archive_for_read(struct archive* reader,
                          const std::filesystem::path& path) {
#ifdef _WIN32
    return archive_read_open_filename_w(reader, path.c_str(), io_buffer_bytes);
#else
    return archive_read_open_filename(reader, path.c_str(), io_buffer_bytes);
#endif
}

Result write_memory_entry(struct archive* writer, const std::string& path,
                          const std::string& content, const std::time_t modified) {
    EntryPtr entry(archive_entry_new());
    if (!entry) return failure(Error::archive_io, "cannot allocate archive entry");
    archive_entry_set_pathname_utf8(entry.get(), path.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0600);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(content.size()));
    archive_entry_set_mtime(entry.get(), modified, 0);
    if (archive_write_header(writer, entry.get()) != ARCHIVE_OK) {
        return failure(Error::archive_io, archive_error(writer));
    }
    if (!content.empty() &&
        archive_write_data(writer, content.data(), content.size()) !=
            static_cast<la_ssize_t>(content.size())) {
        return failure(Error::archive_io, archive_error(writer));
    }
    if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
        return failure(Error::archive_io, archive_error(writer));
    }
    return {};
}

Result write_source_entry(struct archive* writer, const SourceFile& source,
                          const CancelCheck& cancelled,const Progress& progress = {}) {
#ifdef __linux__
    const auto mode_matches=[&]{std::error_code error;const auto status=std::filesystem::symlink_status(source.native_path,error);return !error&&std::filesystem::is_regular_file(status)&&((status.permissions()&std::filesystem::perms::owner_exec)!=std::filesystem::perms::none)==source.record.owner_executable;};
    // Generic ZIP input records do not promise native modes. Backup records do.
    if(source.native_mode_captured&&!mode_matches())return source_failure(Error::source_changed,"source execute permission changed",source.record.path,source.record.size,0);
#endif
    EntryPtr entry(archive_entry_new());
    if (!entry) return failure(Error::archive_io, "cannot allocate archive entry");
    archive_entry_set_pathname_utf8(entry.get(), source.record.path.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), source.record.owner_executable?0700:0600);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(source.record.size));
    archive_entry_set_mtime(entry.get(), source.modified, 0);
    if (archive_write_header(writer, entry.get()) != ARCHIVE_OK) {
        return source_failure(Error::archive_io, "cannot write archive member header", source.record.path,
                              source.record.size, 0, archive_error(writer));
    }

    std::ifstream input(source.native_path, std::ios::binary);
    if (!input) return source_failure(Error::source_io, "cannot reopen source file", source.record.path, source.record.size, 0);
    crypto_hash_sha256_state state{};
    if (crypto_hash_sha256_init(&state) != 0) {
        return source_failure(Error::source_io, "cannot initialize source SHA-256", source.record.path, source.record.size, 0);
    }
    std::array<char, io_buffer_bytes> buffer{};
    std::uint64_t bytes = 0;
    while (input) {
        if (is_cancelled(cancelled)) return failure(Error::cancelled, "cancelled");
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        bytes += static_cast<std::uint64_t>(count);
        if (bytes > source.record.size) {
            return source_failure(Error::source_changed, "source grew during archive creation",
                                  source.record.path, source.record.size, bytes);
        }
        if (crypto_hash_sha256_update(
                &state, reinterpret_cast<const unsigned char*>(buffer.data()),
                static_cast<unsigned long long>(count)) != 0) {
            return source_failure(Error::source_io, "source SHA-256 update failed",
                                  source.record.path, source.record.size, bytes);
        }
        const auto written = archive_write_data(writer, buffer.data(), static_cast<std::size_t>(count));
        if (written != count) {
            return source_failure(Error::archive_io, "cannot write source into archive",
                                  source.record.path, source.record.size, bytes,
                                  "archive_return=" + std::to_string(written) + ": " + archive_error(writer));
        }
        if(progress)progress(bytes,source.record.size,source.record.path);
    }
    if (input.bad()) {
        return source_failure(Error::source_io, "cannot read source during archive creation",
                              source.record.path, source.record.size, bytes);
    }
    if (bytes != source.record.size) {
        return source_failure(Error::source_changed, "source size changed during archive creation",
                              source.record.path, source.record.size, bytes);
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (crypto_hash_sha256_final(&state, digest.data()) != 0) {
        sodium_memzero(digest.data(), digest.size());
        return source_failure(Error::source_io, "source SHA-256 finalization failed",
                              source.record.path, source.record.size, bytes);
    }
    if (hexadecimal(digest.data(), digest.size()) != source.record.sha256) {
        sodium_memzero(digest.data(), digest.size());
        return source_failure(Error::source_changed, "source hash changed during archive creation",
                              source.record.path, source.record.size, bytes);
    }
    sodium_memzero(digest.data(), digest.size());
#ifdef __linux__
    if(source.native_mode_captured&&!mode_matches())return source_failure(Error::source_changed,"source execute permission changed",source.record.path,source.record.size,bytes);
#endif
    if (archive_write_finish_entry(writer) != ARCHIVE_OK) {
        return source_failure(Error::archive_io, "cannot finish archive member", source.record.path,
                              source.record.size, bytes, archive_error(writer));
    }
    return {};
}

bool flush_file(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    return ok;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) return false;
    const bool ok = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return ok;
#endif
}

bool publish_create_new(const std::filesystem::path& partial,
                        const std::filesystem::path& destination) {
#ifdef _WIN32
    return MoveFileW(partial.c_str(), destination.c_str()) != FALSE;
#else
    std::error_code error;
    std::filesystem::create_hard_link(partial, destination, error);
    if (error) return false;
    if (!std::filesystem::remove(partial, error) || error) {
        std::error_code cleanup_error;
        std::filesystem::remove(destination, cleanup_error);
        return false;
    }
    return true;
#endif
}

std::string read_entry_content(struct archive* reader, const std::uint64_t size,
                               Result& result) {
    if (size > 1024U * 1024U) {
        result = failure(Error::metadata_invalid, "backup control file too large");
        return {};
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto count = archive_read_data(reader, content.data() + offset,
                                             content.size() - offset);
        if (count <= 0) {
            result = failure(Error::archive_invalid, archive_error(reader));
            return {};
        }
        offset += static_cast<std::size_t>(count);
    }
    char extra{};
    if (archive_read_data(reader, &extra, 1) != 0) {
        result = failure(Error::archive_invalid, "archive entry exceeds declared size");
        return {};
    }
    return content;
}

struct ScannedZip final {
    ZipInspection inspection;
    std::string info;
    std::string manifest;
};

ScannedZip scan_zip(const std::filesystem::path& path, const Limits& limits,
    const CancelCheck& cancelled = {},const BackupProgress& progress = {}) {
    ScannedZip scanned;
    std::uint64_t verified_bytes{},verified_files{};
    ArchivePtr reader(archive_read_new());
    if (!reader || archive_read_support_filter_none(reader.get()) != ARCHIVE_OK ||
        archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        open_archive_for_read(reader.get(), path) != ARCHIVE_OK) {
        scanned.inspection.result =
            failure(Error::archive_invalid, archive_error(reader.get()));
        return scanned;
    }

    std::set<std::string> keys;
    struct archive_entry* entry = nullptr;
    for (;;) {
        if(is_cancelled(cancelled)){scanned.inspection.result=failure(Error::cancelled,"cancelled");return scanned;}
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK) {
            scanned.inspection.result =
                failure(Error::archive_invalid, archive_error(reader.get()));
            return scanned;
        }
        if ((archive_format(reader.get()) & ARCHIVE_FORMAT_BASE_MASK) !=
                ARCHIVE_FORMAT_ZIP ||
            archive_entry_is_encrypted(entry) == 1 ||
            archive_entry_hardlink(entry) != nullptr ||
            archive_entry_symlink(entry) != nullptr) {
            scanned.inspection.result =
                failure(Error::archive_invalid, "unsupported or encrypted ZIP entry");
            return scanned;
        }
        const char* name = archive_entry_pathname_utf8(entry);
        if (name == nullptr) {
            scanned.inspection.result =
                failure(Error::unsafe_path, "ZIP entry path is not valid UTF-8");
            return scanned;
        }
        std::string entry_path(name);
        while (!entry_path.empty() && entry_path.back() == '/') entry_path.pop_back();
        const auto path_result = validate_archive_path(entry_path, limits);
        if (!path_result.ok()) {
            scanned.inspection.result = path_result;
            return scanned;
        }
        const auto key = collision_key(entry_path);
        if (key.empty() || !keys.insert(key).second) {
            scanned.inspection.result =
                failure(Error::unsafe_path, "duplicate or case-colliding ZIP entry");
            return scanned;
        }
        if (keys.size() > limits.maximum_entries) {
            scanned.inspection.result =
                failure(Error::limit_exceeded, "ZIP entry limit exceeded");
            return scanned;
        }

        const auto file_type = archive_entry_filetype(entry);
        if (file_type == AE_IFDIR) {
            ++scanned.inspection.directory_count;
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
                scanned.inspection.result =
                    failure(Error::archive_invalid, archive_error(reader.get()));
                return scanned;
            }
            continue;
        }
        if (file_type != AE_IFREG || archive_entry_size(entry) < 0) {
            scanned.inspection.result =
                failure(Error::archive_invalid, "ZIP contains a non-file entry");
            return scanned;
        }
        const auto size = static_cast<std::uint64_t>(archive_entry_size(entry));
        if (size > limits.maximum_file_bytes ||
            !checked_add(scanned.inspection.total_bytes, size,
                         limits.maximum_total_bytes)) {
            scanned.inspection.result =
                failure(Error::limit_exceeded, "ZIP size limit exceeded");
            return scanned;
        }

        if (entry_path == backup_info_name || entry_path == backup_manifest_name) {
            Result read_result;
            auto content = read_entry_content(reader.get(), size, read_result);
            if (!read_result.ok()) {
                scanned.inspection.result = read_result;
                return scanned;
            }
            if (entry_path == backup_info_name) scanned.info = std::move(content);
            else scanned.manifest = std::move(content);
            FileRecord record{entry_path, size,
                              hash_memory(entry_path == backup_info_name
                                              ? scanned.info
                                              : scanned.manifest)};
            record.owner_executable=(archive_entry_perm(entry)&0100)!=0;
            if (record.sha256.empty()) {
                scanned.inspection.result =
                    failure(Error::archive_invalid, "cannot hash ZIP entry");
                return scanned;
            }
            scanned.inspection.files.push_back(std::move(record));
            continue;
        }

        if (sodium_init() < 0) {
            scanned.inspection.result =
                failure(Error::archive_invalid, "SHA-256 unavailable");
            return scanned;
        }
        crypto_hash_sha256_state hash{};
        crypto_hash_sha256_init(&hash);
        std::array<unsigned char, io_buffer_bytes> buffer{};
        std::uint64_t bytes = 0;
        for (;;) {
            if(is_cancelled(cancelled)){scanned.inspection.result=failure(Error::cancelled,"cancelled");return scanned;}
            const auto count = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (count == 0) break;
            const auto count_bytes = count < 0 ? 0U : static_cast<std::uint64_t>(count);
            if (count < 0 || count_bytes > size || bytes > size - count_bytes ||
                crypto_hash_sha256_update(
                    &hash, buffer.data(), static_cast<unsigned long long>(count)) != 0) {
                scanned.inspection.result =
                    failure(Error::archive_invalid, archive_error(reader.get()));
                return scanned;
            }
            bytes += count_bytes;
            if(progress)progress({BackupStage::verification,verified_bytes+bytes,0,verified_files,0});
        }
        if (bytes != size) {
            scanned.inspection.result =
                failure(Error::archive_invalid, "ZIP entry size mismatch");
            return scanned;
        }
        std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
        crypto_hash_sha256_final(&hash, digest.data());
        scanned.inspection.files.push_back(
            FileRecord{entry_path, size, hexadecimal(digest.data(), digest.size()),(archive_entry_perm(entry)&0100)!=0});
        sodium_memzero(digest.data(), digest.size());
        verified_bytes+=bytes;++verified_files;
        if(progress)progress({BackupStage::verification,verified_bytes,0,verified_files,0});
    }
    if (archive_read_has_encrypted_entries(reader.get()) == 1) {
        scanned.inspection.result =
            failure(Error::archive_invalid, "encrypted ZIPs are not supported");
        return scanned;
    }
    if (archive_read_close(reader.get()) != ARCHIVE_OK) {
        scanned.inspection.result =
            failure(Error::archive_invalid, archive_error(reader.get()));
        return scanned;
    }
    scanned.inspection.result = {};
    return scanned;
}

bool safe_destination_ancestors(const std::filesystem::path& root,
                                const std::filesystem::path& parent) {
    std::error_code error;
    auto current = root;
    const auto relative = parent.lexically_relative(root);
    for (const auto& component : relative) {
        current /= component;
        if (std::filesystem::exists(current, error)) {
            if (error || !std::filesystem::is_directory(current, error) || error ||
                is_reparse_or_symlink(current)) {
                return false;
            }
        } else {
            error.clear();
            if (!std::filesystem::create_directory(current, error) || error) return false;
        }
    }
    return true;
}

class CreateNewFile final {
public:
    explicit CreateNewFile(const std::filesystem::path& path) : path_(path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    }

    ~CreateNewFile() { close(); }
    CreateNewFile(const CreateNewFile&) = delete;
    CreateNewFile& operator=(const CreateNewFile&) = delete;

    bool valid() const noexcept {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }

    bool write(const void* data, std::size_t bytes) {
        const auto* cursor = static_cast<const unsigned char*>(data);
        while (bytes != 0) {
#ifdef _WIN32
            const auto portion = static_cast<DWORD>(std::min<std::size_t>(
                bytes, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(handle_, cursor, portion, &written, nullptr) ||
                written != portion) {
                return false;
            }
#else
            const auto count = ::write(descriptor_, cursor, bytes);
            if (count <= 0) return false;
            const auto written = static_cast<std::size_t>(count);
#endif
            cursor += written;
            bytes -= written;
        }
        return true;
    }

    bool finish() {
#ifdef _WIN32
        const bool ok = FlushFileBuffers(handle_) != FALSE;
#else
        const bool ok = ::fsync(descriptor_) == 0;
#endif
        close();
        return ok;
    }

private:
    void close() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

    std::filesystem::path path_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

} // namespace

bool decode_backup_manifest(const std::string& text,std::vector<FileRecord>& records){
    records.clear();if(text.size()>32U*1024U*1024U)return false;
    if(!parse_manifest(text,{},records).ok()){records.clear();return false;}return true;
}
Timestamp current_timestamp() {
    Timestamp result;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    std::tm utc{};
#ifdef _WIN32
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
    TIME_ZONE_INFORMATION zone{};
    const DWORD zone_id = GetTimeZoneInformation(&zone);
    LONG bias = zone.Bias;
    if (zone_id == TIME_ZONE_ID_DAYLIGHT) bias += zone.DaylightBias;
    else if (zone_id == TIME_ZONE_ID_STANDARD) bias += zone.StandardBias;
    const long offset_minutes = -bias;
#else
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
    std::tm local_copy = local;
    std::tm utc_copy = utc;
    utc_copy.tm_isdst = local_copy.tm_isdst;
    const long offset_minutes = static_cast<long>(
        std::difftime(std::mktime(&local_copy),
                      std::mktime(&utc_copy)) /
        60.0);
#endif
    std::ostringstream filename;
    filename << std::put_time(&local, "%Y%m%d-%H%M%S");
    result.filename_component = filename.str();
    std::ostringstream local_text;
    local_text << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    result.local = local_text.str();
    const char sign = offset_minutes < 0 ? '-' : '+';
    const long absolute_minutes = offset_minutes < 0 ? -offset_minutes : offset_minutes;
    std::ostringstream offset;
    offset << sign << std::setw(2) << std::setfill('0') << absolute_minutes / 60
           << ':' << std::setw(2) << absolute_minutes % 60;
    result.utc_offset = offset.str();
    std::ostringstream utc_text;
    utc_text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    result.utc = utc_text.str();
    return result;
}

std::filesystem::path backup_filename(const Timestamp& timestamp) {
    return std::filesystem::u8path("SFX-setup-backup-" +
                                   timestamp.filename_component + ".zip");
}

Result create_backup(const CreateBackupRequest& request) {
    if(!valid_physical_layout(request.physical_layout))return failure(Error::invalid_request,"invalid backup physical layout");
    if(request.compression_level<0||request.compression_level>9)return failure(Error::invalid_request,"invalid backup compression level");
    const auto metadata_result = validate_metadata(request.metadata);
    if (!metadata_result.ok()) return metadata_result;
    if (request.partial_path.empty() || request.destination_path.empty() ||
        request.partial_path == request.destination_path ||
        request.partial_path.parent_path() != request.destination_path.parent_path() ||
        request.partial_path.extension() != ".partial") {
        return failure(Error::invalid_request,
                       "partial and final backup paths must be distinct siblings");
    }
    std::error_code error;
    if (std::filesystem::exists(request.partial_path, error) || error) {
        return failure(Error::destination_exists, "partial backup path already exists");
    }
    if (std::filesystem::exists(request.destination_path, error) || error) {
        return failure(Error::destination_exists, "backup path already exists");
    }
    if (!std::filesystem::is_directory(request.destination_path.parent_path(), error) ||
        error || is_reparse_or_symlink(request.destination_path.parent_path())) {
        return failure(Error::invalid_request, "backup directory is not safe");
    }

    std::vector<SourceFile> sources;
    std::uint64_t total_bytes = 0;
    if(request.backup_progress)request.backup_progress({BackupStage::inventory});
    auto result = collect_sources(request, sources, total_bytes);
    if (!result.ok()) return result;
    if(request.backup_progress)request.backup_progress({BackupStage::inventory,total_bytes,total_bytes,sources.size(),sources.size()});

    const std::string information = serialize_metadata(request.metadata);
    std::vector<FileRecord> manifest_records;
    manifest_records.reserve(sources.size() + 1U);
    manifest_records.push_back(FileRecord{backup_info_name,
                                          static_cast<std::uint64_t>(information.size()),
                                          hash_memory(information)});
    if (manifest_records.front().sha256.empty()) {
        return failure(Error::archive_io, "cannot hash backup information");
    }
    for (const auto& source : sources) manifest_records.push_back(source.record);
    const std::string manifest = serialize_manifest(manifest_records);

    ArchivePtr writer(archive_write_new());
    const auto compression_level=std::to_string(request.compression_level);
    if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(), "zip", "compression",
                                        "deflate") != ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(),"zip","compression-level",compression_level.c_str())!=ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(), "zip", "hdrcharset",
                                        "UTF-8") != ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(), "zip", "zip64", "1") !=
            ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(), "zip", "encryption", nullptr) !=
            ARCHIVE_OK ||
        open_archive_for_write(writer.get(), request.partial_path) != ARCHIVE_OK) {
        std::filesystem::remove(request.partial_path, error);
        return failure(Error::archive_io, archive_error(writer.get()));
    }

    const auto modified = std::time(nullptr);
    result = write_memory_entry(writer.get(), backup_info_name, information, modified);
    if (result.ok()) {
        result = write_memory_entry(writer.get(), backup_manifest_name, manifest, modified);
    }
    std::uint64_t completed = 0;
    std::uint64_t written_files{};
    if(request.backup_progress)request.backup_progress({BackupStage::writing,0,total_bytes,0,sources.size()});
    for (const auto& source : sources) {
        if (!result.ok()) break;
        const auto name=ascii_lower(source.record.path);
        const bool stored=request.compression_level==0||(request.store_clamav_definitions&&
            name.compare(0,12,"data/clamav/")==0&&name.size()>4&&name.compare(name.size()-4,4,".cvd")==0);
        if((stored?archive_write_zip_set_compression_store(writer.get()):archive_write_zip_set_compression_deflate(writer.get()))!=ARCHIVE_OK){result=failure(Error::archive_io,"cannot set backup entry compression");break;}
        result = write_source_entry(writer.get(), source, request.cancelled,[&](auto bytes,auto,const auto&){
            if(request.backup_progress)request.backup_progress({BackupStage::writing,completed+bytes,total_bytes,written_files,sources.size()});});
        if (result.ok()) {
            completed += source.record.size;
            ++written_files;
            if(request.progress)request.progress(completed, total_bytes, source.record.path);
            if(request.backup_progress)request.backup_progress({BackupStage::writing,completed,total_bytes,written_files,sources.size()});
        }
    }
    if (result.ok() && archive_write_close(writer.get()) != ARCHIVE_OK) {
        result = failure(Error::archive_io, archive_error(writer.get()));
    }
    writer.reset();
    if (!result.ok()) {
        std::filesystem::remove(request.partial_path, error);
        return result;
    }
    if (!flush_file(request.partial_path)) {
        std::filesystem::remove(request.partial_path, error);
        return failure(Error::archive_io, "cannot flush partial backup");
    }

    if(request.backup_progress)request.backup_progress({BackupStage::verification,0,total_bytes,0,sources.size()});
    const auto inspection = inspect_backup(request.partial_path, request.limits,request.cancelled,[&](auto event){
        event.total_bytes=total_bytes;event.total_files=sources.size();if(request.backup_progress)request.backup_progress(event);});
    if (!inspection.result.ok()) {
        std::filesystem::remove(request.partial_path, error);
        return inspection.result;
    }
    if(request.backup_progress)request.backup_progress({BackupStage::publication});
    if(is_cancelled(request.cancelled)){std::filesystem::remove(request.partial_path,error);return failure(Error::cancelled,"cancelled");}
    if (!publish_create_new(request.partial_path, request.destination_path)) {
        std::filesystem::remove(request.partial_path, error);
        return failure(Error::publication_failed,
                       "cannot publish backup without replacement");
    }
    if (!flush_file(request.destination_path)) {
        return failure(Error::publication_failed, "cannot flush published backup");
    }
    return {};
}

Result create_zip(const CreateZipRequest& request) {
    if (request.partial_path.empty() || request.destination_path.empty() ||
        request.partial_path == request.destination_path ||
        request.partial_path.parent_path() != request.destination_path.parent_path() ||
        request.partial_path.extension() != ".partial" ||
        request.compression_level < 0 || request.compression_level > 9) {
        return failure(Error::invalid_request,
                       "invalid ZIP output or compression level");
    }
    std::error_code error;
    if (std::filesystem::exists(request.partial_path, error) || error ||
        std::filesystem::exists(request.destination_path, error) || error) {
        return failure(Error::destination_exists, "ZIP output already exists");
    }
    if (!std::filesystem::is_directory(request.destination_path.parent_path(), error) ||
        error || is_reparse_or_symlink(request.destination_path.parent_path())) {
        return failure(Error::invalid_request, "ZIP output directory is not safe");
    }

    std::vector<SourceFile> sources;
    std::uint64_t total_bytes = 0;
    auto result = collect_zip_sources(request, sources, total_bytes);
    if (!result.ok()) return result;

    ArchivePtr writer(archive_write_new());
    const auto level = std::to_string(request.compression_level);
    const char* compression = request.compression_level == 0 ? "store" : "deflate";
    if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(), "zip", "compression",
                                        compression) != ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(), "zip", "hdrcharset",
                                        "UTF-8") != ARCHIVE_OK ||
        (request.compression_level != 0 &&
         archive_write_set_format_option(writer.get(), "zip", "compression-level",
                                         level.c_str()) != ARCHIVE_OK) ||
        archive_write_set_format_option(writer.get(), "zip", "zip64", "1") !=
            ARCHIVE_OK ||
        archive_write_set_format_option(writer.get(), "zip", "encryption", nullptr) !=
            ARCHIVE_OK ||
        open_archive_for_write(writer.get(), request.partial_path) != ARCHIVE_OK) {
        std::filesystem::remove(request.partial_path, error);
        return failure(Error::archive_io, archive_error(writer.get()));
    }
    std::uint64_t completed = 0;
    for (const auto& source : sources) {
        result = write_source_entry(writer.get(), source, request.cancelled);
        if (!result.ok()) break;
        completed += source.record.size;
        if (request.progress) {
            request.progress(completed, total_bytes, source.record.path);
        }
    }
    if (result.ok() && archive_write_close(writer.get()) != ARCHIVE_OK) {
        result = failure(Error::archive_io, archive_error(writer.get()));
    }
    writer.reset();
    if (!result.ok()) {
        std::filesystem::remove(request.partial_path, error);
        return result;
    }
    if (!flush_file(request.partial_path)) {
        std::filesystem::remove(request.partial_path, error);
        return failure(Error::archive_io, "cannot flush partial ZIP");
    }
    const auto inspection = inspect_zip(request.partial_path, request.limits);
    if (!inspection.result.ok()) {
        std::filesystem::remove(request.partial_path, error);
        return inspection.result;
    }
    if (inspection.files.size() != sources.size()) {
        std::filesystem::remove(request.partial_path, error);
        return failure(Error::archive_invalid, "created ZIP inventory mismatch");
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& expected = sources[index].record;
        const auto& actual = inspection.files[index];
        if (expected.path != actual.path || expected.size != actual.size ||
            expected.sha256 != actual.sha256) {
            std::filesystem::remove(request.partial_path, error);
            return failure(Error::archive_invalid,
                           "created ZIP content verification failed");
        }
    }
    if (!publish_create_new(request.partial_path, request.destination_path)) {
        std::filesystem::remove(request.partial_path, error);
        return failure(Error::publication_failed,
                       "cannot publish ZIP without replacement");
    }
    if (!flush_file(request.destination_path)) {
        return failure(Error::publication_failed, "cannot flush published ZIP");
    }
    return {};
}

ZipInspection inspect_zip(const std::filesystem::path& archive_path,
                          const Limits& limits) {
    return scan_zip(archive_path, limits).inspection;
}

BackupInspection inspect_backup(const std::filesystem::path& archive_path,
                                const Limits& limits,const CancelCheck& cancelled,const BackupProgress& progress) {
    BackupInspection result;
    auto scanned = scan_zip(archive_path, limits,cancelled,progress);
    if (!scanned.inspection.result.ok()) {
        result.result = scanned.inspection.result;
        return result;
    }
    if (scanned.info.empty() || scanned.manifest.empty()) {
        result.result =
            failure(Error::metadata_invalid, "backup control files are missing");
        return result;
    }
    result.result = parse_metadata(scanned.info, result.metadata);
    if (!result.result.ok()) return result;

    std::vector<FileRecord> expected;
    result.result = parse_manifest(scanned.manifest, limits, expected);
    if (!result.result.ok()) return result;
    std::vector<FileRecord> actual;
    for (const auto& file : scanned.inspection.files) {
        if (file.path != backup_manifest_name) actual.push_back(file);
    }
    auto by_path = [](const FileRecord& left, const FileRecord& right) {
        return collision_key(left.path) < collision_key(right.path);
    };
    std::sort(expected.begin(), expected.end(), by_path);
    std::sort(actual.begin(), actual.end(), by_path);
    if (expected.size() != actual.size()) {
        result.result = failure(Error::manifest_invalid,
                                "backup manifest inventory does not match ZIP");
        return result;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index].path != actual[index].path ||
            expected[index].size != actual[index].size ||
            expected[index].sha256 != actual[index].sha256 ||
            expected[index].owner_executable != actual[index].owner_executable) {
            result.result = failure(Error::manifest_invalid,
                                    "backup manifest hash or size mismatch");
            return result;
        }
        if (actual[index].path != backup_info_name) {
            result.files.push_back(actual[index]);
            if (!checked_add(result.total_bytes, actual[index].size,
                             limits.maximum_total_bytes)) {
                result.result =
                    failure(Error::limit_exceeded, "backup size limit exceeded");
                return result;
            }
        }
    }
    result.result = {};
    return result;
}

ZipMember read_zip_member(const std::filesystem::path& path,const std::string& member,std::size_t maximum) {
    ZipMember result;result.result=failure(Error::archive_invalid,"ZIP member missing or invalid");
    if(member.empty()||member.size()>2048||!maximum||maximum>64U*1024U*1024U)return result;
    ArchivePtr reader(archive_read_new());
    if(!reader||archive_read_support_filter_none(reader.get())!=ARCHIVE_OK||
       archive_read_support_format_zip(reader.get())!=ARCHIVE_OK||open_archive_for_read(reader.get(),path)!=ARCHIVE_OK)return result;
    bool found{};std::size_t entries{};struct archive_entry* entry=nullptr;
    for(;;){
        const auto status=archive_read_next_header(reader.get(),&entry);
        if(status==ARCHIVE_EOF)break;
        if(status!=ARCHIVE_OK||++entries>100000)return result;
        const char* name=archive_entry_pathname_utf8(entry);if(!name)return result;
        if(member!=name){if(archive_read_data_skip(reader.get())!=ARCHIVE_OK)return result;continue;}
        if(found||archive_entry_filetype(entry)!=AE_IFREG||archive_entry_hardlink(entry)||archive_entry_symlink(entry)||
           archive_entry_is_encrypted(entry)||archive_entry_size(entry)<0||static_cast<std::uint64_t>(archive_entry_size(entry))>maximum)return result;
        found=true;std::array<std::uint8_t,65536> buffer{};
        for(;;){const auto count=archive_read_data(reader.get(),buffer.data(),buffer.size());if(!count)break;
            if(count<0||static_cast<std::size_t>(count)>maximum-result.bytes.size())return result;
            result.bytes.insert(result.bytes.end(),buffer.begin(),buffer.begin()+count);}
        if(result.bytes.size()!=static_cast<std::uint64_t>(archive_entry_size(entry)))return result;
    }
    if(found&&archive_read_close(reader.get())==ARCHIVE_OK)result.result={};
    return result;
}

Result stream_zip_member(const std::filesystem::path& path,const std::string& member,std::uint64_t expected,
    const std::function<bool(const std::uint8_t*,std::size_t)>& sink,const CancelCheck& cancelled){
    const auto invalid=[](){return failure(Error::archive_invalid,"exact ZIP member missing, changed or invalid");};
    if(member.empty()||member.size()>2048||expected>64ULL*1024*1024*1024||!sink)return invalid();
    ArchivePtr reader(archive_read_new());
    if(!reader||archive_read_support_filter_none(reader.get())!=ARCHIVE_OK||archive_read_support_format_zip(reader.get())!=ARCHIVE_OK||open_archive_for_read(reader.get(),path)!=ARCHIVE_OK)return invalid();
    bool found{};std::size_t entries{};struct archive_entry* entry=nullptr;
    for(;;){
        if(cancelled&&cancelled())return failure(Error::cancelled,"file content restore cancelled");
        const auto status=archive_read_next_header(reader.get(),&entry);if(status==ARCHIVE_EOF)break;
        if(status!=ARCHIVE_OK||++entries>100000)return invalid();
        const char* name=archive_entry_pathname_utf8(entry);if(!name)return invalid();
        if(member!=name){if(archive_read_data_skip(reader.get())!=ARCHIVE_OK)return invalid();continue;}
        if(found||archive_entry_filetype(entry)!=AE_IFREG||archive_entry_hardlink(entry)||archive_entry_symlink(entry)||archive_entry_is_encrypted(entry)||archive_entry_size(entry)<0||static_cast<std::uint64_t>(archive_entry_size(entry))!=expected)return invalid();
        found=true;std::uint64_t total{};std::array<std::uint8_t,65536> buffer{};
        for(;;){if(cancelled&&cancelled())return failure(Error::cancelled,"file content restore cancelled");const auto n=archive_read_data(reader.get(),buffer.data(),buffer.size());if(!n)break;
            if(n<0||static_cast<std::uint64_t>(n)>expected-total)return invalid();
            total+=static_cast<std::uint64_t>(n);
            if(!sink(buffer.data(),static_cast<std::size_t>(n)))return failure(Error::destination_exists,"file content staging refused");}
        if(total!=expected)return invalid();
    }
    return found&&archive_read_close(reader.get())==ARCHIVE_OK?Result{}:invalid();
}

Result extract_zip(const ExtractRequest& request) {
    if(!valid_physical_layout(request.physical_layout))return failure(Error::invalid_request,"invalid extraction physical layout");
    const auto inspection = inspect_zip(request.archive_path, request.limits);
    if (!inspection.result.ok()) return inspection.result;
    const auto selected_count = static_cast<std::size_t>(std::count_if(
        inspection.files.begin(), inspection.files.end(), [&](const FileRecord& file) {
            return selected_path(file.path, request.included_patterns,
                                 request.excluded_patterns) &&
                   (!request.skip_backup_control_files ||
                    (file.path != backup_info_name &&
                     file.path != backup_manifest_name));
        }));
    if (selected_count == 0) {
        return failure(Error::invalid_request, "no ZIP entries selected");
    }
    // Validate mapped names and collisions before creating the extraction
    // root. Selection and archive hashes remain in the canonical namespace.
    std::set<std::string> native_keys;
    for (const auto& file : inspection.files) {
        if (!selected_path(file.path, request.included_patterns, request.excluded_patterns) ||
            (request.skip_backup_control_files &&
                (file.path == backup_info_name || file.path == backup_manifest_name))) continue;
        const auto native = physical_relative(file.path, request.physical_layout);
        const auto valid = validate_archive_path(native, request.limits);
        if (!valid.ok()) return valid;
        if (!native_keys.insert(collision_key(native)).second)
            return failure(Error::unsafe_path, "archive physical layout collision");
    }
    std::error_code error;
    if (std::filesystem::exists(request.destination_root, error)) {
        if (error || !std::filesystem::is_directory(request.destination_root, error) ||
            error || is_reparse_or_symlink(request.destination_root)) {
            return failure(Error::invalid_request, "extraction root is not safe");
        }
    } else if (error || !std::filesystem::create_directories(
                           request.destination_root, error) ||
               error) {
        return failure(Error::archive_io, "cannot create extraction root");
    }

    ArchivePtr reader(archive_read_new());
    if (!reader || archive_read_support_filter_none(reader.get()) != ARCHIVE_OK ||
        archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
        open_archive_for_read(reader.get(), request.archive_path) != ARCHIVE_OK) {
        return failure(Error::archive_invalid, archive_error(reader.get()));
    }
    std::uint64_t completed = 0;
    struct archive_entry* entry = nullptr;
    for (;;) {
        if (is_cancelled(request.cancelled)) return failure(Error::cancelled, "cancelled");
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK) {
            return failure(Error::archive_invalid, archive_error(reader.get()));
        }
        const char* name = archive_entry_pathname_utf8(entry);
        if (name == nullptr) return failure(Error::unsafe_path, "invalid UTF-8 path");
        std::string entry_path(name);
        while (!entry_path.empty() && entry_path.back() == '/') entry_path.pop_back();
        if (request.skip_backup_control_files &&
            (entry_path == backup_info_name || entry_path == backup_manifest_name)) {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
                return failure(Error::archive_invalid, archive_error(reader.get()));
            }
            continue;
        }
        if (!selected_path(entry_path, request.included_patterns,
                           request.excluded_patterns)) {
            if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
                return failure(Error::archive_invalid, archive_error(reader.get()));
            }
            continue;
        }
        const auto native_path = physical_relative(entry_path, request.physical_layout);
        const auto valid_path = validate_archive_path(native_path, request.limits);
        if (!valid_path.ok()) return valid_path;
        const auto destination = request.destination_root /
                                 std::filesystem::u8path(native_path);
        if (!safe_destination_ancestors(request.destination_root,
                                        destination.parent_path())) {
            return failure(Error::unsafe_path, "unsafe extraction destination");
        }
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            if (std::filesystem::exists(destination, error)) {
                if (error || !std::filesystem::is_directory(destination, error) ||
                    error || is_reparse_or_symlink(destination)) {
                    return failure(Error::unsafe_path,
                                   "unsafe existing extraction directory");
                }
            } else if (error || !std::filesystem::create_directory(destination, error) ||
                       error) {
                return failure(Error::archive_io,
                               "cannot create extraction directory");
            }
            continue;
        }

        CreateNewFile output(destination);
        if (!output.valid()) {
            return failure(Error::destination_exists,
                           "extraction never replaces an existing file");
        }
        std::array<unsigned char, io_buffer_bytes> buffer{};
        for (;;) {
            const auto count = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (count == 0) break;
            if (count < 0 || !output.write(buffer.data(), static_cast<std::size_t>(count))) {
                std::filesystem::remove(destination, error);
                return failure(Error::archive_io, "cannot extract ZIP entry");
            }
            completed += static_cast<std::uint64_t>(count);
        }
        if (!output.finish()) {
            std::filesystem::remove(destination, error);
            return failure(Error::archive_io, "cannot flush extracted file");
        }
        if (request.progress) request.progress(completed, inspection.total_bytes, entry_path);
    }
    if (archive_read_close(reader.get()) != ARCHIVE_OK) {
        return failure(Error::archive_invalid, archive_error(reader.get()));
    }
    return {};
}

const char* error_name(const Error error) noexcept {
    switch (error) {
        case Error::none: return "none";
        case Error::cancelled: return "cancelled";
        case Error::invalid_request: return "invalid_request";
        case Error::unsafe_path: return "unsafe_path";
        case Error::limit_exceeded: return "limit_exceeded";
        case Error::source_changed: return "source_changed";
        case Error::source_io: return "source_io";
        case Error::archive_io: return "archive_io";
        case Error::archive_invalid: return "archive_invalid";
        case Error::metadata_invalid: return "metadata_invalid";
        case Error::manifest_invalid: return "manifest_invalid";
        case Error::destination_exists: return "destination_exists";
        case Error::publication_failed: return "publication_failed";
    }
    return "unknown";
}

} // namespace sfx::archive
