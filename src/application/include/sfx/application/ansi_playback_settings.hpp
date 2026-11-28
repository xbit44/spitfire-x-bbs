#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace sfx::application {

inline constexpr std::size_t kAnsiPlaybackConfigMaximumBytes = 64U * 1024U;
inline constexpr std::uint32_t kAnsiPlaybackMinimumBps = 300;
inline constexpr std::uint32_t kAnsiPlaybackMaximumBps = 115200;

// Runtime passes the supported reader for this installation. The callback
// returns empty text for an absent file, and no value for an uncertain read.
class AnsiPlaybackSettings final {
public:
    using ReadConfiguration = std::function<std::optional<std::string>()>;

    explicit AnsiPlaybackSettings(ReadConfiguration) noexcept;
    ~AnsiPlaybackSettings();
    AnsiPlaybackSettings(const AnsiPlaybackSettings&) = delete;
    AnsiPlaybackSettings& operator=(const AnsiPlaybackSettings&) = delete;

private:
    struct Worker;
    std::unique_ptr<Worker> worker_;
};

// Call only after the display has been selected through its usual file
// boundary. A lookup never accesses the filesystem or opens artwork.
std::uint32_t ansi_playback_bps(const std::filesystem::path& asset) noexcept;

}  // namespace sfx::application
