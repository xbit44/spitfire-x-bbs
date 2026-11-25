#pragma once
#include "sfx/application/door_storage.hpp"
#include "sfx/application/local_account_administration.hpp"

namespace sfx::application {
inline constexpr std::size_t kLocalDoorChunkBytes=60000;
inline constexpr std::size_t kLocalDoorCatalogMaximumBytes=48U*1024U*1024U;
struct LocalDoorQuery {std::int64_t revision{};std::uint32_t offset{};};
struct LocalDoorSnapshot {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    std::int64_t revision{};std::uint32_t offset{},total{};
    std::vector<std::uint8_t> bytes{};
};
struct LocalDoorResult {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};DoorChangeOutcome result{};
};
// One owner-host cache, externally serialized by the existing local host mutex.
// A bounded snapshot is encoded once, then released on the final chunk. It is
// not a worker, persistent setting, caller authority or decrypted secret store.
class LocalDoorSnapshotCache final {
public:
    LocalDoorSnapshot query(IDoorStorage&,LocalAccountAuthority,const LocalDoorQuery&);
    void clear() noexcept;
private:
    std::int64_t revision_{};
    std::vector<std::uint8_t> bytes_;
};
LocalDoorResult administer_door(IDoorStorage&,LocalAccountAuthority,const DoorConfigurationChange&);
bool valid_local_door_snapshot(const LocalDoorSnapshot&) noexcept;
} // namespace sfx::application
