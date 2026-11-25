#pragma once
#include "alpha_operational_journal.hpp"
#include "sfx/platform/linux/clamav_update.hpp"
#include <condition_variable>
#include <thread>

namespace sfx::apps::runtime {
struct LinuxClamAvWorkerSnapshot {
    platform::linux_host::LinuxClamAvStatus status;
    platform::linux_host::LinuxClamAvUpdateResult last_update;
    bool active{},queued{},paused{};
    std::uint64_t operation{},completed_operation{},elapsed_milliseconds{};
};
// One management Runtime owns the optional daily updater, even with BBS stopped.
// Browser lifetime never owns it. Explicit manual requests share the scanner's
// native operation gate, cancellation and persistent automatic-day admission.
class LinuxClamAvWorker final {
public:
    LinuxClamAvWorker(std::filesystem::path,application::IClock&,
        std::shared_ptr<AlphaOperationalJournal>);
    ~LinuxClamAvWorker();
    LinuxClamAvWorkerSnapshot snapshot() const;
    std::uint64_t request_update();
    bool cancel(std::uint64_t operation);
    void pause_automatic(bool);
    void stop() noexcept;
private:
    std::filesystem::path root_;
    application::IClock& clock_;
    std::shared_ptr<AlphaOperationalJournal> journal_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopped_{false},cancelled_{false},paused_{false};
    LinuxClamAvWorkerSnapshot current_;
    std::uint64_t sequence_{};
    std::thread thread_;
    void run() noexcept;
};
}
