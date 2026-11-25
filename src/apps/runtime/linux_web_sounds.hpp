#pragma once
#include "sfx/application/event_sounds.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

namespace sfx::apps::runtime::web {
struct WebSoundFile { std::vector<std::uint8_t> bytes; std::uint64_t duration_ms{}; };
bool linux_sound_path(std::string_view) noexcept;
bool pcm_sound(const std::vector<std::uint8_t>&,std::uint64_t&) noexcept;
WebSoundFile read_web_sound(std::string_view);
application::Identifier128 web_sound_owner(std::string_view session,std::string_view tab);
}
