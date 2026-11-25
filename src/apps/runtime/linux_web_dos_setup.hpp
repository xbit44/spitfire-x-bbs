#pragma once
#include "sfx/platform/linux/dos_setup.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>
#include <thread>
namespace sfx::apps::runtime::web {
class LinuxDosSetup final {
public:
    explicit LinuxDosSetup(std::filesystem::path);
    ~LinuxDosSetup();
    bool active() const;
    nlohmann::json status() const;
    nlohmann::json start(const nlohmann::json&);
    nlohmann::json cancel();
    nlohmann::json save(const nlohmann::json&);
    nlohmann::json inspect_game(const nlohmann::json&) const;
    nlohmann::json prepare_game(const nlohmann::json&);
    void reset();
private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::mutex action_mutex_;
    std::thread worker_;
    std::atomic<bool> cancel_{};
    bool active_{},ok_{};
    std::uint64_t expected_revision_{};
    std::string job_,detail_{"Choose Check DOS Support to inspect this computer."};
    std::optional<platform::linux_host::DosRuntimeCheck> checked_;
    std::filesystem::path prepare_tools();
    void progress(std::string_view);
};
}
