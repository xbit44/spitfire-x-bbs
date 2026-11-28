#include "sfx/application/ansi_playback_settings.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
namespace app = sfx::application;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Configuration {
    std::optional<std::string> read() {
        std::lock_guard<std::mutex> lock(mutex);
        ++reads;
        changed.notify_all();
        return text;
    }

    unsigned set(std::optional<std::string> next) {
        std::lock_guard<std::mutex> lock(mutex);
        text = std::move(next);
        return reads;
    }

    void wait_for_reads(unsigned target) {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, 6s, [&] { return reads >= target; }),
            "Configuration worker did not poll");
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::optional<std::string> text;
    unsigned reads{};
};

void wait_for_rate(const fs::path& path, const std::uint32_t rate) {
    const auto until = std::chrono::steady_clock::now() + 6s;
    while (app::ansi_playback_bps(path) != rate) {
        check(std::chrono::steady_clock::now() < until, "ANSI speed did not reload");
        std::this_thread::sleep_for(10ms);
    }
}

void verify_settings() {
#ifdef _WIN32
    const fs::path root = L"C:/SFX test";
#else
    const fs::path root = "/SFX test";
#endif
    const auto first = root / "Display" / "slowansi.ans";
    const auto second = root / "Alternate" / "slowansi.ans";
    const auto unlisted = root / "Unlisted" / "slowansi.ans";
    const auto equals_in_name = root / "Display" / "art = intro.ANS";
    const auto relative = fs::path("Display") / "slowansi.ans";
    const auto paths_with_dots = root / "Display" / "." / "backup" / ".." / "slowansi.ans";
    const auto different_case = root / "display" / "SLOWANSI.ANS";
    const auto section = "[PlaybackBps]\n";
    const auto with_rate = [](const fs::path& path, unsigned speed) {
        return path.u8string() + " = " + std::to_string(speed) + "\r\n";
    };

    check(app::ansi_playback_bps(first) == 0, "Full speed with no settings worker");
    Configuration config;
    config.set(std::string("\xef\xbb\xbf; ANSI settings\r\n\r\n") + section +
        with_rate(first, 19200) + with_rate(second, 0) + with_rate(equals_in_name, 300));
    {
        app::AnsiPlaybackSettings settings([&] { return config.read(); });
        check(app::ansi_playback_bps(first) == 19200, "Startup reads BOM, comments, CRLF and spaces");
        check(app::ansi_playback_bps(second) == 0 && app::ansi_playback_bps(unlisted) == 0,
            "Identical leaf names use distinct full paths");
        check(app::ansi_playback_bps(equals_in_name) == 300, "Equals sign is allowed inside the filename");
        check(app::ansi_playback_bps(relative) == 0, "Relative lookup does not identify an asset");
        check(app::ansi_playback_bps(paths_with_dots) == 19200, "Lookup uses normalized path");
#ifdef _WIN32
        check(app::ansi_playback_bps(different_case) == 19200, "Windows path comparison ignores case");
#else
        check(app::ansi_playback_bps(different_case) == 0, "Linux path comparison preserves case");
#endif

        // A partially written document must not publish just the valid prefix.
        auto before = config.set(section + with_rate(first, 9600) + second.u8string() + "=");
        config.wait_for_reads(before + 3);
        check(app::ansi_playback_bps(first) == 19200, "Truncated edit retained the last valid snapshot");

        before = config.set(section + with_rate(first, 9600) + with_rate(paths_with_dots, 4800));
        config.wait_for_reads(before + 3);
        check(app::ansi_playback_bps(first) == 19200, "Duplicate normalized path rejected the document");

        config.set(section + with_rate(first, 38400) + with_rate(second, 115200));
        wait_for_rate(first, 38400);
        check(app::ansi_playback_bps(second) == 115200, "Complete edit publishes every rate together");
        check(app::ansi_playback_bps(equals_in_name) == 0, "Removed entry returns to full speed");

        before = config.set(std::nullopt);
        config.wait_for_reads(before + 3);
        check(app::ansi_playback_bps(first) == 38400, "Uncertain read retained the last valid snapshot");

        config.set(std::string{});
        wait_for_rate(first, 0);
        check(app::ansi_playback_bps(second) == 0, "Removing the file clears all entries");

        config.set(section + with_rate(first, 2400));
        wait_for_rate(first, 2400);
    }
    check(app::ansi_playback_bps(first) == 0, "Closing the owner clears its snapshot");
}

}  // namespace

int main() {
    try {
        verify_settings();
        std::cout << "ANSI speed parsing, path matching and live reload passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
