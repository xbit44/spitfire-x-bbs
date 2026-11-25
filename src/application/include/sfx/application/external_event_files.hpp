#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace sfx::application {

enum class ExternalEventFile { catalog, schedule_progress };
// An empty result document means the file has not been created. A missing
// optional means the installation or file failed verification.
std::optional<std::string> read_external_event_file(const std::filesystem::path& root,
    const std::wstring& owner, ExternalEventFile file);
bool publish_external_event_file(const std::filesystem::path& root, const std::wstring& owner,
    ExternalEventFile file, std::string_view document);

} // namespace sfx::application
