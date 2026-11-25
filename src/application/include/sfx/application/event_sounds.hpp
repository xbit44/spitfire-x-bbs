#pragma once
#include "sfx/application/account_storage.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace sfx::application {
// Stable assignment order, independent of log message text and UI row order.
enum class SoundEvent : unsigned char {
    telnet_login, telnet_logout, ssh_login, ssh_logout,
    rlogin_entry, rlogin_exit, page_sysop, security_alert,
    operational_alert, inbound_binkp, count
};
inline constexpr std::size_t kSoundEventCount = static_cast<std::size_t>(SoundEvent::count);
inline constexpr std::size_t kSoundPathMaximumBytes = 768;
inline constexpr std::array<std::string_view,kSoundEventCount> kSoundEventNames{{
    "Telnet Login", "Telnet Logout", "SSH Login", "SSH Logout",
    "Outbound RLogin Entry", "Outbound RLogin Exit", "Page Sysop",
    "Security Alert", "Operational/Error Alert", "Inbound BinkP Connection"
}};
struct EventSoundConfiguration {
    std::array<std::string,kSoundEventCount> paths{}; // UTF-8; empty is silent.
    bool operator==(const EventSoundConfiguration& other) const noexcept { return paths == other.paths; }
};
// Portable lexical validation only. The local adapter checks actual files.
bool valid_event_sound_path(std::string_view path) noexcept;
struct OperationalEvent;
std::optional<SoundEvent> operational_sound_event(const OperationalEvent&) noexcept;
struct SoundEventItem {
    std::uint64_t sequence{},at_ms{};
    SoundEvent event{SoundEvent::operational_alert};
};
struct SoundPollRequest {
    Identifier128 owner,epoch;
    std::uint64_t after{};
    bool fresh_desktop_start{};
};
struct SoundPollSnapshot {
    bool owner{};
    Identifier128 epoch;
    std::uint64_t through{};
    EventSoundConfiguration configuration;
    std::vector<SoundEventItem> events;
};
// Transient events only: bounded memory, one leased consumer, no file/history
// replay. Callers publish typed facts; no WAV/path, decoder or UI lives here.
class EventSoundStream final {
public:
    explicit EventSoundStream(Identifier128 epoch):epoch_(epoch){}
    void publish(SoundEvent,std::uint64_t now_ms,bool startup_failure=false) noexcept;
    void observe(const OperationalEvent&,std::uint64_t now_ms) noexcept;
    SoundPollSnapshot poll(const SoundPollRequest&,const EventSoundConfiguration&,std::uint64_t now_ms) noexcept;
private:
    std::mutex mutex_;
    Identifier128 epoch_,owner_;
    std::uint64_t sequence_{},lease_ms_{},startup_sequence_{},delivered_{};
    bool startup_delivered_{};
    std::array<std::optional<std::uint64_t>,kSoundEventCount> last_;
    std::deque<SoundEventItem> events_;
};
}
