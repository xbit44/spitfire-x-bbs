#pragma once
#include "sfx/application/door.hpp"
#include "sfx/application/door_bulletin.hpp"
#include "sfx/application/message_storage.hpp"

namespace sfx::application {
enum class DoorConfigurationAction : std::uint8_t { save_door, save_menu, save_entry, remove_door, remove_menu, remove_entry, duplicate_menu };
struct DoorConfigurationChange {
    Identifier128 operation;
    std::int64_t expected_revision{};
    DoorConfigurationAction action{DoorConfigurationAction::save_door};
    DoorDefinition door;
    DoorMenuDefinition menu;
    DoorMenuEntry entry;
    std::uint8_t previous_number{}; // Explicit move within the same menu only.
    DoorMenuId source_menu{}; // Duplicate Menu: new ID is menu.id; no recursion.
    bool create_only{}; // Create rejects an existing permanent ID/placement;
                       // edit requires its existing captured identity.
    bool move_entry{}; // Atomic move from source_menu/previous_number to entry.
    // Explicit caller-number permutation, bounded to one destination menu.
    // Numbers are the pre-permutation identities, not automatically compacted.
    std::vector<std::uint8_t> order;
};
struct DoorChangeOutcome { std::int64_t revision{}; bool replayed{}; };
struct DoorCallerMenu {
    std::int64_t revision{};
    DoorMenuDefinition menu;
    std::vector<DoorMenuEntry> entries;
    bool has_hidden_entries{};
};
struct DoorLaunchRequest { DoorMenuId menu{};std::uint8_t number{};std::int64_t expected_revision{}; };
struct DoorLaunch {
    std::int64_t catalog_revision{};
    DoorDefinition door;
    DoorMenuDefinition menu;
    DoorMenuEntry entry;
};
struct DoorEventList {std::int64_t revision{};std::vector<DoorId> doors;};
struct DoorEventLaunchRequest {DoorId door{};DoorAutomaticEvent event{DoorAutomaticEvent::none};std::int64_t expected_revision{};};
bool valid_door_change(const DoorConfigurationChange&) noexcept;
// Pure, bounded model transformation; grants no storage or caller authority.
// Failure leaves the source catalog unchanged when passed by value.
std::optional<DoorCatalog> changed_door_catalog(DoorCatalog,const DoorConfigurationChange&);
// Delete a category and every reachable category/Door, including placements
// elsewhere. Root is immutable. Program files are outside this catalog.
std::optional<DoorCatalog> without_door_category(DoorCatalog,DoorMenuId);
class IDoorStorage {
public:
    virtual ~IDoorStorage() = default;
    virtual StorageResult<DoorCatalog> door_configuration(const MessageConfigurationActor&) = 0;
    virtual StorageResult<DoorChangeOutcome> change_door_configuration(const MessageConfigurationActor&,const DoorConfigurationChange&) = 0;
    virtual StorageResult<DoorCallerMenu> door_menu(const MessageActor&,DoorMenuId) = 0;
    virtual StorageResult<DoorLaunch> door_launch(const MessageActor&,const DoorLaunchRequest&) = 0;
    virtual StorageResult<DoorEventList> door_events(const MessageActor&,DoorAutomaticEvent) = 0;
    virtual StorageResult<DoorLaunch> door_event_launch(const MessageActor&,const DoorEventLaunchRequest&) = 0;
    virtual StorageResult<DoorBulletinCatalog> door_bulletin_configuration(const MessageConfigurationActor&) {return {{StorageErrorCode::not_found,"Door Bulletins unavailable"},std::nullopt};}
    virtual StorageResult<DoorChangeOutcome> change_door_bulletin(const MessageConfigurationActor&,const DoorBulletinChange&) {return {{StorageErrorCode::not_found,"Door Bulletins unavailable"},std::nullopt};}
    virtual StorageResult<DoorBulletinListing> door_bulletins(const MessageActor&) {return {{StorageErrorCode::not_found,"Door Bulletins unavailable"},std::nullopt};}
    virtual StorageResult<std::vector<DoorBulletinNotice>> door_bulletin_changes(const MessageActor&,std::int64_t) {return {{StorageErrorCode::not_found,"Door Bulletins unavailable"},std::nullopt};}
    virtual StorageResult<DoorBulletinSource> door_bulletin_source(const MessageConfigurationActor&,DoorBulletinId) {return {{StorageErrorCode::not_found,"Door Bulletins unavailable"},std::nullopt};}
    virtual StorageResult<DoorBulletinPublicationResult> publish_door_bulletin(const MessageConfigurationActor&,const DoorBulletinPublication&) {return {{StorageErrorCode::not_found,"Door Bulletins unavailable"},std::nullopt};}
};
} // namespace sfx::application
