#pragma once

#include "sfx/archive/archive_engine.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sfx::apps::archive_tools {

inline int exit_code(const archive::Error error) noexcept {
    switch (error) {
        case archive::Error::none: return 0;
        case archive::Error::invalid_request: return 2;
        case archive::Error::source_io:
        case archive::Error::source_changed: return 3;
        case archive::Error::unsafe_path: return 4;
        case archive::Error::archive_invalid:
        case archive::Error::metadata_invalid:
        case archive::Error::manifest_invalid: return 5;
        case archive::Error::archive_io:
        case archive::Error::destination_exists:
        case archive::Error::publication_failed: return 6;
        case archive::Error::limit_exceeded: return 7;
        case archive::Error::cancelled: return 8;
    }
    return 9;
}

inline int report(const archive::Result& result, const bool quiet) {
    (void)quiet;
    if (result.ok()) return 0;
    std::cerr << "SFX archive tool error: " << archive::error_name(result.error);
    if (!result.detail.empty()) std::cerr << ": " << result.detail;
    std::cerr << '\n';
    return exit_code(result.error);
}

inline std::string pattern(const std::filesystem::path& value) {
    return value.generic_u8string();
}

inline bool wildcard_match(const std::string& expression, const std::string& value) {
    std::size_t expression_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::string::npos;
    std::size_t retry = 0;
    while (value_index < value.size()) {
        if (expression_index < expression.size() &&
            (expression[expression_index] == '?' ||
             std::tolower(static_cast<unsigned char>(expression[expression_index])) ==
                 std::tolower(static_cast<unsigned char>(value[value_index])))) {
            ++expression_index;
            ++value_index;
        } else if (expression_index < expression.size() &&
                   expression[expression_index] == '*') {
            star = expression_index++;
            retry = value_index;
        } else if (star != std::string::npos) {
            expression_index = star + 1;
            value_index = ++retry;
        } else {
            return false;
        }
    }
    while (expression_index < expression.size() && expression[expression_index] == '*') {
        ++expression_index;
    }
    return expression_index == expression.size();
}

inline bool selected(const std::string& path,
                     const std::vector<std::string>& included,
                     const std::vector<std::string>& excluded) {
    const bool accepted = included.empty() ||
        std::any_of(included.begin(), included.end(), [&](const std::string& value) {
            return wildcard_match(value, path);
        });
    return accepted && !std::any_of(excluded.begin(), excluded.end(),
        [&](const std::string& value) { return wildcard_match(value, path); });
}

inline void prepare_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

inline bool dangerous_switch(const std::wstring& value) {
    return value == L"-P" || value == L"-e" || value == L"--encrypt" ||
           value == L"-m" || value == L"--move" || value == L"-F" ||
           value == L"-FF" || value == L"--fix" || value == L"-o" ||
           value == L"--overwrite" || value == L"-U" ||
           value == L"--backup-control" || value == L"--volume-label" ||
           value == L"--privileges";
}

} // namespace sfx::apps::archive_tools
