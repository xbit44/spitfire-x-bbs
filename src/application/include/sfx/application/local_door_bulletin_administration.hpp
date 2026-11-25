#pragma once
#include "sfx/application/local_door_administration.hpp"
#include "sfx/application/door_bulletin_preview.hpp"
#include <functional>
namespace sfx::application {
enum class LocalDoorBulletinQueryKind : std::uint8_t {catalog,validate,preview};
struct LocalDoorBulletinQuery {
    LocalDoorBulletinQueryKind kind{LocalDoorBulletinQueryKind::catalog};
    std::int64_t revision{};
    std::uint32_t offset{};
    DoorBulletinId id;
    bool ansi{true};
};
using LocalDoorBulletinPreview=std::function<DoorBulletinPreview(DoorBulletinId,bool,std::int64_t)>;
class LocalDoorBulletinSnapshotCache final {
public:
    LocalDoorSnapshot query(IDoorStorage&,LocalAccountAuthority,const LocalDoorBulletinQuery&,const LocalDoorBulletinPreview& preview={});
    void clear()noexcept;
private:
    LocalDoorBulletinQuery query_;
    std::int64_t revision_{};
    std::vector<std::uint8_t> bytes_;
};
LocalDoorResult administer_door_bulletin(IDoorStorage&,LocalAccountAuthority,const DoorBulletinChange&);
}
