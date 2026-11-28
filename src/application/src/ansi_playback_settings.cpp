#include "sfx/application/ansi_playback_settings.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace sfx::application {
namespace {

using Path = std::filesystem::path;

struct PathLess {
    AnsiPlaybackSettings::PathRules rules;

    Path normalize(Path path) const {
        if (rules.normalize) path = rules.normalize(std::move(path));
        return path.lexically_normal().make_preferred();
    }

    bool operator()(const Path& left, const Path& right) const noexcept {
        return rules.less ? rules.less(left, right) : left.native() < right.native();
    }
};

using Rates = std::map<Path, std::uint32_t, PathLess>;
std::shared_ptr<const Rates> active_rates;
std::atomic<bool> worker_active{false};

std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view whitespace = " \t\r";
    const auto first = text.find_first_not_of(whitespace);
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

bool is_ansi_file(const Path& path) {
    auto suffix = path.extension().u8string();
    for (auto& ch : suffix)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return suffix == ".ans";
}

std::optional<Rates> parse(std::string_view text, PathLess path_less) {
    constexpr std::string_view bom = "\xef\xbb\xbf";
    if (text.substr(0, bom.size()) == bom) text.remove_prefix(bom.size());
    if (text.find('\0') != text.npos) return {};
    Rates rates(path_less);
    bool in_section = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = trim(text.substr(0, end));
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line == "[PlaybackBps]" && !in_section) {
            in_section = true;
            continue;
        }
        const auto separator = line.rfind('=');
        if (!in_section || separator == line.npos) return {};
        const auto filename = trim(line.substr(0, separator));
        if (std::any_of(filename.begin(), filename.end(), [](unsigned char byte) {
                return byte < 32 || byte == 127;
            })) return {};
        auto path = path_less.normalize(std::filesystem::u8path(filename));
        if (!path.is_absolute() || !is_ansi_file(path)) return {};

        const auto value = trim(line.substr(separator + 1));
        if (value.empty()) return {};
        std::uint32_t rate{};
        const auto number = std::from_chars(value.data(), value.data() + value.size(), rate);
        if (number.ec != std::errc{} || number.ptr != value.data() + value.size() ||
            (rate != 0 && (rate < kAnsiPlaybackMinimumBps || rate > kAnsiPlaybackMaximumBps)))
            return {};
        if (!rates.emplace(std::move(path), rate).second) return {};
    }
    return rates;
}

}  // namespace

struct AnsiPlaybackSettings::Worker {
    Worker(ReadConfiguration read, PathRules rules) : read_(std::move(read)), path_less_{rules} {
        refresh(true);
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(wait_);
            while (!wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; })) {
                lock.unlock();
                refresh(false);
                lock.lock();
            }
        });
    }

    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(wait_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

private:
    void refresh(bool initial) noexcept {
        try {
            auto text = read_();
            if (!text || text->size() > kAnsiPlaybackConfigMaximumBytes || text == accepted_) {
                pending_.reset();
                return;
            }
            // Confirm changed text on a second bounded read. Editors may
            // truncate or temporarily remove the old file while saving.
            if (!initial && text != pending_) {
                pending_ = std::move(text);
                return;
            }
            pending_.reset();
            auto rates = parse(*text, path_less_);
            if (!rates) return;
            auto snapshot = std::make_shared<const Rates>(std::move(*rates));
            accepted_ = std::move(text);
            std::atomic_store(&active_rates, std::move(snapshot));
        } catch (...) {
            pending_.reset();
        }
    }

    ReadConfiguration read_;
    PathLess path_less_;
    std::optional<std::string> accepted_, pending_;
    std::mutex wait_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::thread thread_;
};

AnsiPlaybackSettings::AnsiPlaybackSettings(ReadConfiguration read, PathRules rules) noexcept {
    if (worker_active.exchange(true)) return;
    try {
        worker_ = std::make_unique<Worker>(std::move(read), rules);
    } catch (...) {
        std::atomic_store(&active_rates, std::shared_ptr<const Rates>{});
        worker_active.store(false);
    }
}

AnsiPlaybackSettings::~AnsiPlaybackSettings() {
    if (!worker_) return;
    worker_.reset();
    std::atomic_store(&active_rates, std::shared_ptr<const Rates>{});
    worker_active.store(false);
}

std::uint32_t ansi_playback_bps(const std::filesystem::path& asset) noexcept {
    try {
        const auto snapshot = std::atomic_load(&active_rates);
        if (snapshot && !snapshot->empty()) {
            const auto found = snapshot->find(snapshot->key_comp().normalize(asset));
            if (found != snapshot->end()) return found->second;
        }
    } catch (...) {}
    return 0;
}

}  // namespace sfx::application
