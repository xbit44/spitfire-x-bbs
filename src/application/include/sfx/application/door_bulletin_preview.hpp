#pragma once
#include "sfx/application/door_bulletin.hpp"
#include <vector>
namespace sfx::application {
struct DoorBulletinPreviewCell {std::uint8_t glyph{32},attribute{7};};
// Bounded inert terminal snapshot for owner-only native/web preview.
struct DoorBulletinPreview {
    DoorBulletinId id;
    std::string title,diagnostic;
    bool available{};
    std::uint16_t columns{80},rows{};
    std::vector<DoorBulletinPreviewCell> cells;
};
bool valid_door_bulletin_preview(const DoorBulletinPreview&) noexcept;
std::vector<std::uint8_t> encode_door_bulletin_preview(const DoorBulletinPreview&);
std::optional<DoorBulletinPreview> decode_door_bulletin_preview(const std::vector<std::uint8_t>&);
}
