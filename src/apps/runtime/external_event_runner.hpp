#pragma once

#include "sfx/application/external_events.hpp"
#include <atomic>
#include <filesystem>
#include <memory>

namespace sfx::apps::runtime {
class AlphaOperationalJournal;
class AlphaDurableJournal;

struct ExternalEventCallerGate {
    std::function<void(bool paused, std::int64_t cutoff_utc)> update;
    std::function<bool()> empty;
    std::function<void()> disconnect;
};

class ExternalEventRunner final {
public:
    ExternalEventRunner(std::filesystem::path root, std::wstring owner, std::string zone,
        const application::IClock&, ExternalEventCallerGate,
        std::shared_ptr<AlphaOperationalJournal>, std::shared_ptr<AlphaDurableJournal>,
        bool desktop_console = false);
    ~ExternalEventRunner();
    bool start(bool board_start);
    void stop() noexcept;
    bool busy() const noexcept;
    std::string activity() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::runtime
