#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sfx::platform::win32 {
struct SoundFileResult {
    std::vector<unsigned char> bytes;
    std::wstring detail;
    std::uint64_t duration_ms{};
    explicit operator bool() const noexcept { return !bytes.empty(); }
};
// Local fixed drive, no reparse/ADS/UNC/device paths; bounded PCM RIFF/WAVE.
SoundFileResult read_event_sound(const std::wstring& path);
class EventSoundPlayer final {
public:
    ~EventSoundPlayer();
    EventSoundPlayer() = default;
    EventSoundPlayer(const EventSoundPlayer&) = delete;
    EventSoundPlayer& operator=(const EventSoundPlayer&) = delete;
    bool preview(const std::wstring& path, std::wstring& detail);
    void stop() noexcept;
    bool owns_buffer() const noexcept { return !bytes_.empty(); }
    bool playing() const noexcept;
private:
    std::vector<unsigned char> bytes_;
    std::uint64_t end_ms_{};
};
}
