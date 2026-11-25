#pragma once
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/ftn_native_session.hpp"
#include "sfx/application/event_sounds.hpp"
#include "sfx/application/file_storage.hpp"
#include "sfx/application/ftn_runtime.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>

namespace sfx::apps::runtime {
struct LinuxFtnSessionView {
    std::uint64_t serial{};
    bool active{};
    platform::linux_host::FtnNativeSessionResult transfer;
};
struct LinuxFtnSnapshot {
    application::FtnMailerMode mode{application::FtnMailerMode::disabled};
    std::int64_t saved_revision{},active_revision{};
    bool capable{},paused{},classic_listening{},tls_listening{},serial_recovery_required{};
    std::uint16_t classic_port{},tls_port{};
    std::string diagnostic{"ftn_not_started"},classic_error,tls_error;
    std::vector<LinuxFtnSessionView> sessions;
    std::vector<application::FtnActivity> activity;
    bool processing{};
    std::uint64_t processed_receipts{},held_receipts{},imported_messages{},replayed_messages{},scanned_messages{};
    std::string processing_error;
};
// Sole host-owned engine. All native sockets/workers are joined before a new
// configuration generation starts, backup takes custody, or storage shuts down.
// No detached workers and no callbacks into the Runtime host mutex.
class LinuxFtnEngine final {
public:
    LinuxFtnEngine(platform::linux_host::FtnInstallation,application::IFtnMailStorage&,application::IFileStorage&,
        application::Identifier128,application::IClock&,std::shared_ptr<application::EventSoundStream>);
    ~LinuxFtnEngine();
    void configure(application::FtnConfiguration);
    void set_bbs_time_zone(std::string_view);
    LinuxFtnSnapshot snapshot() const;
    bool poll(std::string_view network,std::string_view link,bool include_hold=false,std::int64_t expected_revision=0);
    bool scan(std::int64_t expected_revision=0);
    // A successful hold stays in force across configuration saves. Caller must
    // release after its DB snapshot AND spool archive complete, including errors.
    bool pause_for_backup();
    void resume_after_backup() noexcept;
    void stop() noexcept;
private:
    void run() noexcept;
    void activity_locked(std::uint64_t,std::string_view,std::string_view,std::string);
    platform::linux_host::FtnInstallation receipt_;
    application::IFtnMailStorage& storage_;
    application::IFileStorage& files_;
    application::Identifier128 generation_;
    application::IClock& clock_;
    bool capable_{};
    std::shared_ptr<application::EventSoundStream> sounds_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<application::FtnConfiguration> wanted_;
    struct TimeZone;
    std::shared_ptr<const TimeZone> timezone_;
    std::uint64_t change_{};
    struct Poll {std::string network,link;bool hold{};};
    std::vector<Poll> polls_;
    LinuxFtnSnapshot view_;
    std::atomic_bool stopping_{false},cancel_{false};
    std::shared_ptr<std::atomic_bool> retention_cancel_{std::make_shared<std::atomic_bool>(false)};
    std::atomic_bool scan_requested_{false};
    bool backup_hold_{},quiescent_{true};
    std::thread thread_;
};
}
