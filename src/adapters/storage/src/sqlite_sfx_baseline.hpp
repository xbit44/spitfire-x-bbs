#pragma once

#include "sfx/adapters/storage/sqlite_storage.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

namespace sfx::adapters::storage::internal {

inline constexpr std::int32_t sfx_baseline_application_id = 1397119042;
inline constexpr std::int32_t sfx_baseline_schema_version = 1;
inline constexpr const char* sfx_baseline_database_filename = "SFX.DB";
inline constexpr const char* sfx_baseline_migration_name =
    "sfx-v1-initial-persistence-baseline";
inline constexpr const char* sfx_baseline_migration_sha256 =
    "c9ed2791ec70d18cb69696f837d27cc74f931d7c3253247768b7245bf0864806";
inline constexpr std::size_t sfx_baseline_migration_utf8_bytes = 25849;
inline constexpr const char* sfx_baseline_request_savepoint = "sfx_request";

enum class SfxBaselineSchemaObjectKind : std::uint8_t {
    table,
    index,
    trigger,
    view,
};

struct SfxBaselineSchemaObject {
    SfxBaselineSchemaObjectKind kind{SfxBaselineSchemaObjectKind::table};
    std::string name;
    std::string definition_sha256;
};

struct SfxBaselineLedgerEntry {
    std::int32_t version{0};
    std::string name;
    std::string content_sha256;
    std::int64_t applied_utc_seconds{0};
};

struct SfxBaselineSidecarFact {
    bool present{false};
    std::uintmax_t bytes{0};
};

struct SfxBaselineFacts {
    bool canonical_protected_data_directory{false};
    std::string database_filename{sfx_baseline_database_filename};
    bool database_present{false};
    std::uintmax_t database_bytes{0};
    bool file_identity_available{false};
    std::uint64_t file_volume_identity{0};
    std::uint64_t file_identity{0};
    bool sqlite_header_valid{false};
    std::int32_t application_id{0};
    std::int32_t user_version{0};
    bool integrity_check_ok{false};
    bool foreign_key_check_ok{false};
    std::int64_t column_count{0};
    std::int64_t foreign_key_count{0};
    std::vector<SfxBaselineSchemaObject> schema_objects;
    std::vector<SfxBaselineLedgerEntry> ledger;
    std::vector<std::string> sqlite_sequence_table_names;
    SfxBaselineSidecarFact wal;
    SfxBaselineSidecarFact shared_memory;
    SfxBaselineSidecarFact rollback_journal;
};

enum class SfxBaselineState : std::uint8_t {
    absent_fresh,
    prepublication,
    completed,
    invalid,
};

enum class SfxBaselineIssue : std::uint8_t {
    none,
    malformed_facts,
    unbound_path_or_identity,
    orphaned_sidecar,
    conflicting_sidecars,
    unknown_database_identity,
    migration_ledger_mismatch,
    schema_manifest_mismatch,
    unrecognized_state,
};

struct SfxBaselineClassification {
    SfxBaselineState state{SfxBaselineState::invalid};
    SfxBaselineIssue issue{SfxBaselineIssue::unrecognized_state};
    bool requires_sqlite_recovery{false};

    bool valid() const noexcept { return state != SfxBaselineState::invalid; }
};

const SqliteMigration& sfx_initial_baseline_migration();
const SqliteMigration& sfx_caller_journal_migration();
const SqliteMigration& sfx_menu_preferences_migration();
const SqliteMigration& sfx_caller_utilities_migration();
const SqliteMigration& sfx_classic_information_migration();
const SqliteMigration& sfx_other_board_text_migration();
const SqliteMigration& sfx_classic_delete_migration();
const SqliteMigration& sfx_local_message_migration();
const SqliteMigration& sfx_message_admin_migration();
const SqliteMigration& sfx_message_group_migration();
const SqliteMigration& sfx_message_delete_migration();
const SqliteMigration& sfx_message_source_migration();
const SqliteMigration& sfx_message_maintenance_migration();
const SqliteMigration& sfx_qwk_migration();
const SqliteMigration& sfx_file_migration();
const SqliteMigration& sfx_file_group_deletion_migration();
const SqliteMigration& sfx_ftn_wire_body_migration();
const SqliteMigration& sfx_ftn_subject_migration();
const SqliteMigration& sfx_message_location_migration();
const SqliteMigration& sfx_ftn_external_migration();
const SqliteMigration& sfx_ftn_recovery_migration();
const SqliteMigration& sfx_door_bulletin_migration();
const SqliteMigration& sfx_message_list_delete_migration();
const SqliteMigration& sfx_door_migration();
const SqliteMigration& sfx_door_security_migration();
const SqliteMigration& sfx_door_program_migration();
const SqliteMigration& sfx_chat_migration();
const SqliteMigration& sfx_private_chat_migration();
const SqliteMigration& sfx_ftn_migration();
const SqliteMigration& sfx_ftn_mail_migration();
const SqliteMigration& sfx_optional_subject_migration();
std::vector<SfxBaselineSchemaObject> sfx_baseline_schema_manifest();
SfxBaselineClassification classify_sfx_baseline_state(
    const SfxBaselineFacts& facts);
bool validate_sfx_baseline_database(sqlite3* database, bool full_integrity) noexcept;
// Recognizes exact shipping v1/v2/v3; does not relax the frozen v1 validator.
bool validate_sfx_shipping_database(sqlite3* database, bool full_integrity) noexcept;
bool validate_sfx_prepublication_database(sqlite3* database) noexcept;

}  // namespace sfx::adapters::storage::internal
