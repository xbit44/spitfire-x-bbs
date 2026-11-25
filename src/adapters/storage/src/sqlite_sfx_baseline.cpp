#include "sqlite_sfx_baseline.hpp"
#include "sqlite_internal.hpp"
#include "sfx/application/classic_information_storage.hpp"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <memory>
#include <sstream>
#include <tuple>
#include <stdexcept>

namespace sfx::adapters::storage::internal {
namespace {

const std::string proposed_migration_sql =
#include "sqlite_sfx_baseline_v1.inc"
;

const std::array<SfxBaselineSchemaObject, 47> proposed_manifest{{
    {SfxBaselineSchemaObjectKind::index, "sfx_account_audit_kind_time",
        "39f88fac31ae8b41ba39f51f5dc0d0d1e8d581b907871113b98ddc69dddcb619"},
    {SfxBaselineSchemaObjectKind::index, "sfx_account_audit_subject_time",
        "2e59f7b241351dd4f005819e0c19b073badb539500fa8e9d7bb9de2ba3dbba8c"},
    {SfxBaselineSchemaObjectKind::index, "sfx_account_audit_time",
        "534bed2ce701fc621025de635af8a820888672a22d8e004b351ab0ca1586791d"},
    {SfxBaselineSchemaObjectKind::index, "sfx_account_sessions_account_time",
        "d8b75a7d635d45cb999410cf36ab8cc3cb6271197a59acad9089516c50176197"},
    {SfxBaselineSchemaObjectKind::index, "sfx_account_statistics_last_login",
        "90b000efeec83b06585f17dfe576bf7ce4c0d96d31b398d0a425b4f9f207cda1"},
    {SfxBaselineSchemaObjectKind::index, "sfx_accounts_name_key_unique",
        "9c2190fe291d4f68d2959a4c955e75ad34b74158582257bd08d388b04d2af37a"},
    {SfxBaselineSchemaObjectKind::index, "sfx_accounts_security_name",
        "8c0afa45a9d94d6adf6d78efa1bec9b1cc9db22f3aeda7d676ab62c2a539bcbf"},
    {SfxBaselineSchemaObjectKind::index, "sfx_accounts_state_name",
        "e73c8fb754dd5967bee6025284f5b6c6f9e30f4d454bee7c8f1df973f4ce0cc7"},
    {SfxBaselineSchemaObjectKind::index, "sfx_authentication_audit_kind_time",
        "c3ef340e49c5fe2fab73b4c8a156edda95665877a6e2a8551c991461408d5ca7"},
    {SfxBaselineSchemaObjectKind::index, "sfx_authentication_audit_subject_time",
        "e50f7d9e46c1335188bfc4eb020985387cc2585d34077f91649639b89047b983"},
    {SfxBaselineSchemaObjectKind::index, "sfx_authentication_rate_expiry",
        "4df38b83ce986a7808616e3902f7d07572291344158c8fca59b75c51203eb86c"},
    {SfxBaselineSchemaObjectKind::index, "sfx_failed_login_notice_claims_expiry",
        "deabc7199f7ec799ab46c94ebe68f78b9006c72d5bca402176c17f498975477a"},
    {SfxBaselineSchemaObjectKind::index, "sfx_failed_login_notice_claims_one_active",
        "87ab198d81e2a25d5497f2be709e316c4b5a9e15d239033b9fdfdaf849fd8481"},
    {SfxBaselineSchemaObjectKind::index, "sfx_failed_login_notice_enqueues_account",
        "7ce46ac62f6bf354644c50c74ec7c90a32f53d567106534d4e5773c3f0f10ec2"},
    {SfxBaselineSchemaObjectKind::index, "sfx_failed_login_notices_claim",
        "174e09f28a1c67e86c29c4637cb634be6040a0235686bb09aa2cc1e40f8d5282"},
    {SfxBaselineSchemaObjectKind::index, "sfx_failed_login_notices_unread",
        "6d03c998448ba19c10d24330d0d38e51e5d1ad3382d4f1fe3f89f2b942e1faa5"},
    {SfxBaselineSchemaObjectKind::index, "sfx_name_reservations_account",
        "bc81088e5a7a041b4c389e9004fe7ecca637a57120e80a92e3810e8ba2bf0874"},
    {SfxBaselineSchemaObjectKind::index, "sfx_successful_login_ip_account_time",
        "28ef9229a4adac942f7b2a8e93c904a657e6668b8bf9cfc17510a2c46eda36b8"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_audit",
        "9f72f7a83ab3f77c0e92b03a9cec72221b7c279950aa4f8862af18e4ebf99029"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_credentials",
        "d41d2993eb66cdcd778a1806b8279a87ed609a1874fb07df8aed121726ec3d41"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_name_reservations",
        "a3fb5db22410bea7b9d9082a46e9506cf02bd6a9041fe65d0f713fbe65fbf684"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_password_resets",
        "42a5692a3584dfb847c3f2bfa1023a133448bc585cc41f35ea787c9756f5f0bb"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_profiles",
        "0d91d2557329d4657d84d51f68a764d2467c2bdf65c32e94aa5e2bbc514537ac"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_retention_policy",
        "c388e7aafe96707f9708e6147e44bb7ca37d8b39b6ed3b7b685f14a7fb3fcbbc"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_sessions",
        "28c0726d562f2d91844252593cd93158ae4968efc3dff208b560e867a6c8914d"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_statistics",
        "28ff1b632addcc6c5c780a2381f827e3c57064f0bf7cc4b55045358268368680"},
    {SfxBaselineSchemaObjectKind::table, "sfx_account_transfer_preferences",
        "12c21375f14f09a171b97a8f7d7dc9a76a1f16ce6e0473f19daa385bf1af27dc"},
    {SfxBaselineSchemaObjectKind::table, "sfx_accounts",
        "a3ca3ab9a192fff94a162e66b60cd6ecf0a0bbb9cc305c1ca0bebd56cb57fd2a"},
    {SfxBaselineSchemaObjectKind::table, "sfx_authentication_audit",
        "1045a594f0bd11dda4d01a163a804c17ee4361d01f802d1d84c77a9ca590e293"},
    {SfxBaselineSchemaObjectKind::table, "sfx_authentication_rate_state",
        "76d3c7315001337fad88b1fd325b0924e7ea35bbfb0deb8a75eaff45fb2bb31f"},
    {SfxBaselineSchemaObjectKind::table, "sfx_failed_login_notice_claims",
        "b9b0353ebf725b4ac0e47c0386360fd4f42259e96bd1fa47090d583821156802"},
    {SfxBaselineSchemaObjectKind::table, "sfx_failed_login_notice_enqueues",
        "fb5c2892df44aeb76ed3218add0a8041114cd11d9a4dac6a98b27c016212698c"},
    {SfxBaselineSchemaObjectKind::table, "sfx_failed_login_notices",
        "6f9e98094cf150c621c55eeb6ddbb5c6354a162321b55f54c9e4da8b58dc5927"},
    {SfxBaselineSchemaObjectKind::table, "sfx_installation_state",
        "c00a4f88d19362c088b722ca1757165cb96d5f3b5f41f79eccadbc0e938cba92"},
    {SfxBaselineSchemaObjectKind::table, "sfx_registration_commit_receipts",
        "103a6e5f09647edb33ef18bae05ddab2872c43a2737bd784d14bb7dcd54223b9"},
    {SfxBaselineSchemaObjectKind::table, "sfx_schema_migrations",
        "990fdd7721f7f666f9f4aa8fd291339450bac582aa267d5e49c8ae45a2d056b9"},
    {SfxBaselineSchemaObjectKind::table, "sfx_security_levels",
        "1722826920a060c2bd169253a40037af2aa1670cbfe148c6d1fd47298f716f3f"},
    {SfxBaselineSchemaObjectKind::table, "sfx_security_policy",
        "4b2551f562bbce6ba8f13499854a639a2db59cc10addb81b2474254ad28d579a"},
    {SfxBaselineSchemaObjectKind::table, "sfx_successful_login_ip_history",
        "d39ac4bf21a637da9c6105ef249eba5193da22f05e8eaba3425259c4a91b5c15"},
    {SfxBaselineSchemaObjectKind::table, "sfx_system_credentials",
        "4c29ba12246a759ba972601984cd53b739482f46aba40f2c3d3d05f21b31de8f"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_account_sessions_release_failed_login_claim",
        "670d88ef679a7cf9720fc50f6ed853306a7cc5776f6d13ad91bcf2f23861d409"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_accounts_default_transfer_preference",
        "2a856e7f0de80722e05f9e314014c0a60fc6ce919252b5ddfe111d67012674c2"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_failed_login_notice_claims_retain_terminal",
        "68366de8ea0a8109fa33e7c20324af54c32e2f079cb863b4cc5c8124d74710bf"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_failed_login_notice_enqueues_retain_recent",
        "e7e77ae80a652702eda5136e5b9bddb08eb6aeb769b6dad7bcd9005a51aa16cf"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_failed_login_notices_retain_five",
        "d867d0bb5a2eeccbdb20110539b9698442abd66759a050e8fc88a1f883e65488"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_registration_commit_receipts_immutable_delete",
        "1f89cbff2a43bb11dfabcad8aa46752252b08eddfd041fb73f4a5f6fc73c9bec"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_registration_commit_receipts_immutable_update",
        "e12f12c515000c88030397a048d722bbe5d97158be47561dbc7b6b2db6ffe5a3"},
}};

bool sidecar_valid(const SfxBaselineSidecarFact& sidecar) {
    return sidecar.present || sidecar.bytes == 0;
}

bool nonempty(const SfxBaselineSidecarFact& sidecar) {
    return sidecar.present && sidecar.bytes != 0;
}

bool any_sidecar(const SfxBaselineFacts& facts) {
    return facts.wal.present || facts.shared_memory.present ||
        facts.rollback_journal.present;
}

bool recovery_material(const SfxBaselineFacts& facts) {
    return nonempty(facts.wal) || nonempty(facts.rollback_journal);
}

bool identity_valid(const SfxBaselineFacts& facts) {
    return facts.file_identity_available &&
        (facts.file_volume_identity != 0 || facts.file_identity != 0);
}

auto object_key(const SfxBaselineSchemaObject& value) {
    return std::tie(value.kind, value.name, value.definition_sha256);
}

bool manifest_matches(std::vector<SfxBaselineSchemaObject> actual) {
    auto expected = sfx_baseline_schema_manifest();
    const auto less = [](const SfxBaselineSchemaObject& left,
                          const SfxBaselineSchemaObject& right) {
        return object_key(left) < object_key(right);
    };
    std::sort(actual.begin(), actual.end(), less);
    std::sort(expected.begin(), expected.end(), less);
    if (actual.size() != expected.size()) return false;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (object_key(actual[index]) != object_key(expected[index])) return false;
    }
    return true;
}

bool ledger_matches(const std::vector<SfxBaselineLedgerEntry>& ledger) {
    return ledger.size() == 1 &&
        ledger.front().version == sfx_baseline_schema_version &&
        ledger.front().name == sfx_baseline_migration_name &&
        ledger.front().content_sha256 == sfx_baseline_migration_sha256 &&
        ledger.front().applied_utc_seconds >= 0;
}

SfxBaselineClassification invalid(const SfxBaselineIssue issue) {
    return {SfxBaselineState::invalid, issue, false};
}

bool read_int64(sqlite3* database, const char* sql, std::int64_t* value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return false;
    const int step = sqlite3_step(statement);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return false;
    }
    *value = sqlite3_column_int64(statement, 0);
    return sqlite3_step(statement) == SQLITE_DONE && sqlite3_finalize(statement) == SQLITE_OK;
}

bool read_text(sqlite3* database, const char* sql, std::string* value) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return false;
    const int step = sqlite3_step(statement);
    const unsigned char* text = step == SQLITE_ROW ? sqlite3_column_text(statement, 0) : nullptr;
    if (text == nullptr) {
        sqlite3_finalize(statement);
        return false;
    }
    *value = reinterpret_cast<const char*>(text);
    return sqlite3_step(statement) == SQLITE_DONE && sqlite3_finalize(statement) == SQLITE_OK;
}

std::string stored_definition_sha256(const std::string& definition) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    if (sodium_init() < 0 || crypto_hash_sha256(
            digest.data(), reinterpret_cast<const unsigned char*>(definition.data()),
            static_cast<unsigned long long>(definition.size())) != 0) return {};
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

bool collect_schema_manifest(sqlite3* database, std::vector<SfxBaselineSchemaObject>* objects) {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "SELECT type,name,sql FROM sqlite_schema WHERE sql IS NOT NULL "
        "AND name NOT GLOB 'sqlite_*' ORDER BY type,name";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return false;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char* type_text = sqlite3_column_text(statement, 0);
        const unsigned char* name_text = sqlite3_column_text(statement, 1);
        const unsigned char* sql_text = sqlite3_column_text(statement, 2);
        if (type_text == nullptr || name_text == nullptr || sql_text == nullptr) {
            sqlite3_finalize(statement);
            return false;
        }
        const std::string type(reinterpret_cast<const char*>(type_text));
        SfxBaselineSchemaObjectKind kind = SfxBaselineSchemaObjectKind::table;
        if (type == "index") kind = SfxBaselineSchemaObjectKind::index;
        else if (type == "trigger") kind = SfxBaselineSchemaObjectKind::trigger;
        else if (type == "view") kind = SfxBaselineSchemaObjectKind::view;
        else if (type != "table") {
            sqlite3_finalize(statement);
            return false;
        }
        const std::string hash = stored_definition_sha256(
            reinterpret_cast<const char*>(sql_text));
        if (hash.empty()) {
            sqlite3_finalize(statement);
            return false;
        }
        objects->push_back({kind, reinterpret_cast<const char*>(name_text), hash});
    }
    return step == SQLITE_DONE && sqlite3_finalize(statement) == SQLITE_OK;
}

bool exact_ledger(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "SELECT version,name,content_sha256,applied_utc FROM sfx_schema_migrations ORDER BY version";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return false;
    const int first = sqlite3_step(statement);
    const unsigned char* name = first == SQLITE_ROW ? sqlite3_column_text(statement, 1) : nullptr;
    const unsigned char* hash = first == SQLITE_ROW ? sqlite3_column_text(statement, 2) : nullptr;
    const bool matches = first == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1 &&
        name != nullptr && hash != nullptr &&
        std::string(reinterpret_cast<const char*>(name)) == sfx_baseline_migration_name &&
        std::string(reinterpret_cast<const char*>(hash)) == sfx_baseline_migration_sha256 &&
        sqlite3_column_int64(statement, 3) >= 0 && sqlite3_step(statement) == SQLITE_DONE;
    return sqlite3_finalize(statement) == SQLITE_OK && matches;
}

bool exact_sequence(sqlite3* database) {
    std::int64_t exists = 0;
    if (!read_int64(database,
            "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sqlite_sequence'",
            &exists)) return false;
    if (exists == 0) return true;
    if (exists != 1) return false;
    std::int64_t unexpected = 0;
    return read_int64(database,
        "SELECT count(*) FROM sqlite_sequence WHERE name NOT IN ('sfx_accounts','sfx_message_groups','sfx_local_echoes','sfx_local_messages') "
        "AND NOT ((SELECT user_version FROM pragma_user_version)>=15 AND name IN "
        "('sfx_file_groups','sfx_file_areas','sfx_file_catalog','sfx_file_publications','sfx_file_audit'))", &unexpected) &&
        unexpected == 0;
}

#include "sqlite_caller_journal_schema.inc"
#include "sqlite_menu_schema.inc"
#include "sqlite_caller_utilities_schema.inc"
#include "sqlite_classic_information_schema.inc"
#include "sqlite_classic_delete_schema.inc"
#include "sqlite_local_message_schema.inc"
#include "sqlite_message_admin_schema.inc"
#include "sqlite_message_group_schema.inc"
#include "sqlite_message_delete_schema.inc"
#include "sqlite_message_source_schema.inc"
#include "sqlite_message_maintenance_schema.inc"
#include "sqlite_qwk_schema.inc"
#include "sqlite_qwk_manifest.inc"
#include "sqlite_file_schema.inc"
#include "sqlite_file_manifest.inc"
#include "sqlite_door_schema.inc"
#include "sqlite_chat_schema.inc"
#include "sqlite_ftn_schema.inc"
#include "sqlite_ftn_mail_schema.inc"
#include "sqlite_optional_subject_schema.inc"
#include "sqlite_file_group_deletion_schema.inc"
#include "sqlite_ftn_wire_body_schema.inc"
#include "sqlite_ftn_subject_schema.inc"
#include "sqlite_message_location_schema.inc"
#include "sqlite_ftn_external_schema.inc"
#include "sqlite_ftn_recovery_schema.inc"
#include "sqlite_door_bulletin_schema.inc"
#include "sqlite_message_list_delete_schema.inc"

}  // namespace

const SqliteMigration& sfx_initial_baseline_migration() {
    static const SqliteMigration migration{
        sfx_baseline_schema_version,
        sfx_baseline_migration_name,
        sfx_baseline_migration_sha256,
        proposed_migration_sql};
    return migration;
}

std::vector<SfxBaselineSchemaObject> sfx_baseline_schema_manifest() {
    return {proposed_manifest.begin(), proposed_manifest.end()};
}

const SqliteMigration& sfx_caller_journal_migration() {
    static const SqliteMigration migration{2, "sfx-caller-journal-foundation",
        stored_definition_sha256(journal_migration_sql), journal_migration_sql};
    return migration;
}

const SqliteMigration& sfx_menu_preferences_migration() {
    static const SqliteMigration migration{3, "sfx-menu-preferences",
        stored_definition_sha256(menu_migration_sql), menu_migration_sql};
    return migration;
}
const SqliteMigration& sfx_caller_utilities_migration() {
    static const SqliteMigration migration{4,"sfx-caller-utilities",stored_definition_sha256(caller_utilities_migration_sql),caller_utilities_migration_sql};
    return migration;
}
const SqliteMigration& sfx_classic_information_migration() {
    static const SqliteMigration migration{5,"sfx-classic-information",stored_definition_sha256(classic_information_migration_sql),classic_information_migration_sql};
    return migration;
}
const SqliteMigration& sfx_classic_delete_migration() {
    static const SqliteMigration migration{6,"sfx-classic-caller-delete",stored_definition_sha256(classic_delete_migration_sql),classic_delete_migration_sql};
    return migration;
}
const SqliteMigration& sfx_other_board_text_migration() {
    // Owner explicitly declines import of the test catalog/history. Leave the
    // frozen schema objects alone but retire only Other BBS data, once. Opening
    // date audit (action 3) and every account/session ledger are untouched.
    static const std::string sql="DELETE FROM sfx_other_boards;\nDELETE FROM sfx_classic_information_audit WHERE action IN (0,1,2);\n";
    static const SqliteMigration migration{7,"sfx-other-bbs-text",stored_definition_sha256(sql),sql};return migration;
}

const SqliteMigration& sfx_local_message_migration() {
    static const SqliteMigration migration{8,"sfx-core-local-messaging",
        stored_definition_sha256(local_message_migration_sql),local_message_migration_sql};
    return migration;
}
const SqliteMigration& sfx_message_admin_migration() {
    static const SqliteMigration migration{9,"sfx-echo-administration",stored_definition_sha256(message_admin_migration_sql),message_admin_migration_sql};return migration;
}

const SqliteMigration& sfx_message_group_migration(){
    static const SqliteMigration migration{10,"sfx-empty-group-deletion",stored_definition_sha256(message_group_migration_sql),message_group_migration_sql};return migration;
}
const SqliteMigration& sfx_message_delete_migration(){
    static const SqliteMigration migration{11,"sfx-individual-message-deletion",stored_definition_sha256(message_delete_migration_sql),message_delete_migration_sql};return migration;
}
const SqliteMigration& sfx_message_source_migration(){
    static const SqliteMigration migration{12,"section8b_message_sources_and_pipe_policy",stored_definition_sha256(message_source_migration_sql),message_source_migration_sql};return migration;
}
const SqliteMigration& sfx_message_maintenance_migration(){
    static const SqliteMigration migration{13,"section8c_message_retention_and_maintenance",stored_definition_sha256(message_maintenance_migration_sql),message_maintenance_migration_sql};return migration;
}
const SqliteMigration& sfx_qwk_migration(){
    static const SqliteMigration migration{14,"section8d_qwk_settings_continuation_and_atomic_receipts",stored_definition_sha256(qwk_migration_sql),qwk_migration_sql};return migration;
}
const SqliteMigration& sfx_file_migration() {
    static const SqliteMigration migration{15,"section9_native_file_catalog_and_transfer_receipts",
        stored_definition_sha256(file_migration_sql),file_migration_sql};
    return migration;
}
const SqliteMigration& sfx_door_migration() {
    static const SqliteMigration migration{16,"section11_outbound_door_catalog",
        stored_definition_sha256(door_migration_sql),door_migration_sql};
    return migration;
}
const SqliteMigration& sfx_door_security_migration(){
    static const SqliteMigration migration{17,"section11_door_requirements",stored_definition_sha256(door_security_migration_sql),door_security_migration_sql};return migration;
}
const SqliteMigration& sfx_door_program_migration(){
    static const SqliteMigration migration{18,"section11_local_door_programs",stored_definition_sha256(door_program_migration_sql),door_program_migration_sql};return migration;
}
const SqliteMigration& sfx_chat_migration(){
    static const SqliteMigration migration{19,"section12_chat_settings",stored_definition_sha256(chat_migration_sql),chat_migration_sql};return migration;
}
const SqliteMigration& sfx_private_chat_migration(){
    static const SqliteMigration migration{20,"section12_private_chat_limits",stored_definition_sha256(private_chat_migration_sql),private_chat_migration_sql};return migration;
}
const SqliteMigration& sfx_ftn_migration(){
    static const SqliteMigration migration{21,"section14_ftn_configuration",stored_definition_sha256(ftn_migration_sql),ftn_migration_sql};return migration;
}
const SqliteMigration& sfx_ftn_mail_migration(){
    static const SqliteMigration migration{22,"section14_ftn_mail",stored_definition_sha256(ftn_mail_migration_sql),ftn_mail_migration_sql};return migration;
}
const SqliteMigration& sfx_optional_subject_migration(){
    static const SqliteMigration migration{23,"optional_message_subject",stored_definition_sha256(optional_subject_migration_sql),optional_subject_migration_sql};return migration;
}
const SqliteMigration& sfx_file_group_deletion_migration(){
    static const SqliteMigration migration{24,"file_group_deletion",stored_definition_sha256(file_group_deletion_migration_sql),file_group_deletion_migration_sql};return migration;
}
const SqliteMigration& sfx_ftn_wire_body_migration(){
    static const SqliteMigration migration{25,"ftn_wire_body",stored_definition_sha256(ftn_wire_body_migration_sql),ftn_wire_body_migration_sql};return migration;
}
const SqliteMigration& sfx_ftn_subject_migration(){
    static const SqliteMigration migration{26,"ftn_complete_subject",stored_definition_sha256(ftn_subject_migration_sql),ftn_subject_migration_sql};return migration;
}
const SqliteMigration& sfx_message_location_migration(){
    static const SqliteMigration migration{27,"caller_message_location",stored_definition_sha256(message_location_migration_sql),message_location_migration_sql};return migration;
}

const SqliteMigration& sfx_ftn_external_migration(){
    static const SqliteMigration migration{28,"external_binkd_handoff",stored_definition_sha256(ftn_external_migration_sql),ftn_external_migration_sql};return migration;
}

const SqliteMigration& sfx_ftn_recovery_migration(){
    static const SqliteMigration migration{29,"ftn_history_recovery",stored_definition_sha256(ftn_recovery_migration_sql),ftn_recovery_migration_sql};return migration;
}

const SqliteMigration& sfx_door_bulletin_migration(){
    static const SqliteMigration migration{30,"door_bulletins",stored_definition_sha256(door_bulletin_migration_sql),door_bulletin_migration_sql};return migration;
}

const SqliteMigration& sfx_message_list_delete_migration() {
    static const SqliteMigration migration{31,"message_list_deletion",
        stored_definition_sha256(message_list_delete_migration_sql),message_list_delete_migration_sql};
    return migration;
}

bool validate_sfx_shipping_database(sqlite3* database, const bool full_integrity) noexcept {
    try {
        std::int64_t version{}, application_id{}, foreign_keys{};
        if (!database || !read_int64(database, "PRAGMA user_version", &version)) return false;
        if (version == 1) return validate_sfx_baseline_database(database, full_integrity);
        std::string integrity;
        if ((version < 2 || version > 31) || !read_int64(database, "PRAGMA application_id", &application_id) ||
            application_id != sfx_baseline_application_id ||
            !read_int64(database, "SELECT count(*) FROM pragma_foreign_key_check", &foreign_keys) ||
            foreign_keys || !exact_sequence(database) ||
            (full_integrity && (!read_text(database, "PRAGMA integrity_check", &integrity) || integrity != "ok"))) return false;
        std::vector<SfxBaselineSchemaObject> actual;
        if (!collect_schema_manifest(database, &actual)) return false;
        if (version >= 8) for (const auto& required : version>=31?message_list_delete_manifest():version>=30?door_bulletin_manifest():version>=29?ftn_recovery_manifest():version>=28?ftn_external_manifest():version>=27?message_location_manifest():version>=26?ftn_subject_manifest():version>=25?ftn_wire_body_manifest():version>=24?file_group_deletion_manifest():version>=23?optional_subject_manifest():version>=22?ftn_mail_manifest():version>=21?ftn_manifest():version>=20?private_chat_manifest():version>=19?chat_manifest():version>=18?door_program_manifest():version>=17?door_security_manifest():version>=16?door_manifest():version>=15?file_manifest():version>=14?qwk_manifest():version>=13?message_maintenance_manifest():version>=12?message_source_manifest():version>=11?message_delete_manifest():version>=10?message_group_manifest():version>=9?message_admin_manifest():local_message_manifest()) {
            const auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& object) {
                return object_key(object) == object_key(required);
            });
            if (found == actual.end()) return false;
            actual.erase(found);
        }
        if(version>=5) for(auto required:classic_information_manifest()) {
            if(version>=6 && required.name=="sfx_classic_information_audit")required.definition_sha256=stored_definition_sha256(classic_delete_audit_ddl);
            const auto found=std::find_if(actual.begin(),actual.end(),[&](const auto& object){return object_key(object)==object_key(required);});
            if(found==actual.end()) return false;
            actual.erase(found);
        }
        if(version>=4) {
            for(const auto& required:caller_utilities_manifest()) {
                const auto found=std::find_if(actual.begin(),actual.end(),[&](const auto& object){return object_key(object)==object_key(required);});
                if(found==actual.end()) return false;
                actual.erase(found);
            }
            const auto audit=std::find_if(actual.begin(),actual.end(),[](const auto& object){return object.name=="sfx_account_audit";});
            if(audit==actual.end() || audit->definition_sha256!=stored_definition_sha256(caller_audit_ddl)) return false;
            const auto prior=std::find_if(proposed_manifest.begin(),proposed_manifest.end(),[](const auto& object){return object.name=="sfx_account_audit";});
            *audit=*prior;
        }
        if (version >= 3) for (const auto& required : menu_manifest()) {
            const auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& object) {
                return object_key(object) == object_key(required);
            });
            if (found == actual.end()) return false;
            actual.erase(found);
        }
        for (const auto& required : journal_manifest()) {
            const auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& object) {
                return object_key(object) == object_key(required);
            });
            if (found == actual.end()) return false;
            actual.erase(found);
        }
        if (!manifest_matches(std::move(actual))) return false;
        sqlite3_stmt* raw{};
        if (sqlite3_prepare_v2(database,
            "SELECT version,name,content_sha256,applied_utc FROM sfx_schema_migrations ORDER BY version",
            -1, &raw, nullptr) != SQLITE_OK) return false;
        const auto finalize = [](sqlite3_stmt* p) { sqlite3_finalize(p); };
        std::unique_ptr<sqlite3_stmt, decltype(finalize)> statement(raw, finalize);
        std::vector<const SqliteMigration*> lineage{&sfx_initial_baseline_migration(), &sfx_caller_journal_migration()};
        if (version >= 3) lineage.push_back(&sfx_menu_preferences_migration());
        if (version >= 4) lineage.push_back(&sfx_caller_utilities_migration());
        if (version >= 5) lineage.push_back(&sfx_classic_information_migration());
        if (version >= 6) lineage.push_back(&sfx_classic_delete_migration());
        if (version >= 7) lineage.push_back(&sfx_other_board_text_migration());
        if (version >= 8) lineage.push_back(&sfx_local_message_migration());
        if (version >= 9) {
            lineage.push_back(&sfx_message_admin_migration());
            std::int64_t pending{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_message_purge_scope",&pending) || pending)return false;
        }
        if(version>=10)lineage.push_back(&sfx_message_group_migration());
        if(version>=11){
            lineage.push_back(&sfx_message_delete_migration());
            std::int64_t pending{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_message_delete_scope",&pending) || pending)return false;
        }
        if(version>=12){
            lineage.push_back(&sfx_message_source_migration());
            std::int64_t pending{},policies{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_message_source_write_scope",&pending) || pending ||
                !read_int64(database,"SELECT count(*) FROM sfx_message_presentation_policy WHERE singleton_id=1",&policies) || policies!=1)return false;
        }
        if(version>=13){
            lineage.push_back(&sfx_message_maintenance_migration());
            std::int64_t policies{},missing{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_unread_mail_retention WHERE singleton_id=1",&policies) || policies!=1 ||
                !read_int64(database,"SELECT count(*) FROM sfx_local_echoes e LEFT JOIN sfx_echo_retention p USING(echo_id) WHERE p.echo_id IS NULL",&missing) || missing)return false;
        }
        if(version>=14){
            lineage.push_back(&sfx_qwk_migration());std::int64_t policies{},missing{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_qwk_policy WHERE singleton_id=1",&policies) || policies!=1 ||
                !read_int64(database,"SELECT count(*) FROM sfx_local_echoes e LEFT JOIN sfx_qwk_conference_mapping q USING(echo_id) WHERE q.echo_id IS NULL",&missing) || missing)return false;
        }
        if (version>=15) {
            lineage.push_back(&sfx_file_migration());
            std::int64_t policies{}, missing_paths{}, special{};
            if (!read_int64(database,"SELECT count(*) FROM sfx_file_policy WHERE singleton_id=1",&policies) || policies!=1 ||
                !read_int64(database,"SELECT count(*) FROM sfx_file_areas a LEFT JOIN sfx_file_area_paths p USING(area_id) WHERE p.area_id IS NULL",&missing_paths) || missing_paths ||
                !read_int64(database,"SELECT count(*) FROM sfx_file_areas WHERE kind IN(1,2)",&special) || special!=2) return false;
        }
        if(version>=16){
            lineage.push_back(&sfx_door_migration());
            std::int64_t policy{},root{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_door_policy WHERE singleton_id=1",&policy) || policy!=1 ||
                !read_int64(database,"SELECT count(*) FROM sfx_door_menus WHERE menu_id=0",&root) || root!=1)return false;
        }
        if(version>=17){
            lineage.push_back(&sfx_door_security_migration());std::int64_t missing{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_door_menus m LEFT JOIN sfx_door_menu_requirements r USING(menu_id) WHERE r.menu_id IS NULL",&missing)||missing)return false;
        }
        if(version>=18)lineage.push_back(&sfx_door_program_migration());
        if(version>=19)lineage.push_back(&sfx_chat_migration());
        if(version>=20)lineage.push_back(&sfx_private_chat_migration());
        if(version>=21)lineage.push_back(&sfx_ftn_migration());
        if(version>=22)lineage.push_back(&sfx_ftn_mail_migration());
        if(version>=23)lineage.push_back(&sfx_optional_subject_migration());
        if(version>=24)lineage.push_back(&sfx_file_group_deletion_migration());
        if(version>=25)lineage.push_back(&sfx_ftn_wire_body_migration());
        if(version>=26)lineage.push_back(&sfx_ftn_subject_migration());
        if(version>=27)lineage.push_back(&sfx_message_location_migration());
        if(version>=28)lineage.push_back(&sfx_ftn_external_migration());
        if(version>=29)lineage.push_back(&sfx_ftn_recovery_migration());
        if(version>=30)lineage.push_back(&sfx_door_bulletin_migration());
        if(version>=31)lineage.push_back(&sfx_message_list_delete_migration());
        for (const auto* expected : lineage) {
            if (sqlite3_step(raw) != SQLITE_ROW || sqlite3_column_int(raw, 0) != expected->version ||
                !sqlite3_column_text(raw, 1) || !sqlite3_column_text(raw, 2) ||
                expected->name != reinterpret_cast<const char*>(sqlite3_column_text(raw, 1)) ||
                expected->content_sha256 != reinterpret_cast<const char*>(sqlite3_column_text(raw, 2)) ||
                sqlite3_column_int64(raw, 3) < 0) return false;
        }
        if(version>=5) {
            std::int64_t opening_count{},retained_count{};
            if(!read_int64(database,"SELECT count(*) FROM sfx_bbs_opening_date",&opening_count) || opening_count!=1 ||
                !read_int64(database,"SELECT count(*) FROM sfx_bbs_opening_date d JOIN sfx_schema_migrations m ON m.version=1 AND d.initialization_utc=m.applied_utc",&retained_count) || retained_count!=1)return false;
            std::string date;
            if(!read_text(database,"SELECT coalesce(configured_date,'') FROM sfx_bbs_opening_date WHERE singleton_id=1",&date) ||
                (!date.empty() && !application::valid_iso_civil_date(date)))return false;
        }
        std::int64_t counter_count{}, counter_inconsistent{};
        return sqlite3_step(raw) == SQLITE_DONE &&
            read_int64(database, "SELECT count(*) FROM sfx_caller_number_state WHERE singleton_id=1", &counter_count) &&
            counter_count == 1 && read_int64(database,
                "SELECT count(*) FROM sfx_caller_journals WHERE caller_number > "
                "(SELECT last_number FROM sfx_caller_number_state WHERE singleton_id=1)", &counter_inconsistent) &&
            counter_inconsistent == 0;
    } catch (...) { return false; }
}

SfxBaselineClassification classify_sfx_baseline_state(
    const SfxBaselineFacts& facts) {
    if (facts.database_filename != sfx_baseline_database_filename ||
        !sidecar_valid(facts.wal) || !sidecar_valid(facts.shared_memory) ||
        !sidecar_valid(facts.rollback_journal) || facts.column_count < 0 ||
        facts.foreign_key_count < 0) {
        return invalid(SfxBaselineIssue::malformed_facts);
    }
    if (!facts.canonical_protected_data_directory) {
        return invalid(SfxBaselineIssue::unbound_path_or_identity);
    }
    if (!facts.database_present) {
        if (facts.database_bytes != 0 || facts.file_identity_available ||
            facts.file_volume_identity != 0 || facts.file_identity != 0 ||
            facts.sqlite_header_valid || facts.application_id != 0 ||
            facts.user_version != 0 || facts.integrity_check_ok ||
            facts.foreign_key_check_ok || facts.column_count != 0 ||
            facts.foreign_key_count != 0 || !facts.schema_objects.empty() ||
            !facts.ledger.empty() || !facts.sqlite_sequence_table_names.empty()) {
            return invalid(SfxBaselineIssue::malformed_facts);
        }
        if (any_sidecar(facts)) {
            return invalid(SfxBaselineIssue::orphaned_sidecar);
        }
        return {SfxBaselineState::absent_fresh, SfxBaselineIssue::none, false};
    }
    if (!identity_valid(facts)) {
        return invalid(SfxBaselineIssue::unbound_path_or_identity);
    }
    if (nonempty(facts.wal) && nonempty(facts.rollback_journal)) {
        return invalid(SfxBaselineIssue::conflicting_sidecars);
    }
    if (nonempty(facts.shared_memory) && !nonempty(facts.wal)) {
        return invalid(SfxBaselineIssue::conflicting_sidecars);
    }
    const bool needs_recovery = recovery_material(facts);
    const bool no_published_state = facts.application_id == 0 &&
        facts.user_version == 0 && facts.column_count == 0 &&
        facts.foreign_key_count == 0 && facts.schema_objects.empty() &&
        facts.ledger.empty() && facts.sqlite_sequence_table_names.empty();
    if (facts.database_bytes == 0) {
        if (!facts.sqlite_header_valid && no_published_state && !any_sidecar(facts) &&
            !facts.integrity_check_ok && !facts.foreign_key_check_ok) {
            return {SfxBaselineState::prepublication, SfxBaselineIssue::none, false};
        }
        return invalid(SfxBaselineIssue::unrecognized_state);
    }
    if (facts.sqlite_header_valid && no_published_state &&
        facts.integrity_check_ok && facts.foreign_key_check_ok) {
        return {SfxBaselineState::prepublication, SfxBaselineIssue::none, needs_recovery};
    }
    if (facts.application_id != sfx_baseline_application_id ||
        facts.user_version != sfx_baseline_schema_version) {
        return invalid(SfxBaselineIssue::unknown_database_identity);
    }
    if (!ledger_matches(facts.ledger)) {
        return invalid(SfxBaselineIssue::migration_ledger_mismatch);
    }
    const bool sequence_matches = facts.sqlite_sequence_table_names.empty() ||
        facts.sqlite_sequence_table_names == std::vector<std::string>{"sfx_accounts"};
    if (!facts.sqlite_header_valid || !facts.integrity_check_ok ||
        !facts.foreign_key_check_ok || facts.column_count != 156 ||
        facts.foreign_key_count != 22 ||
        !sequence_matches || !manifest_matches(facts.schema_objects)) {
        return invalid(SfxBaselineIssue::schema_manifest_mismatch);
    }
    return {SfxBaselineState::completed, SfxBaselineIssue::none, needs_recovery};
}

bool validate_sfx_baseline_database(sqlite3* database, const bool full_integrity) noexcept {
    try {
        if (database == nullptr) return false;
        std::int64_t application_id = 0;
        std::int64_t user_version = 0;
        std::int64_t foreign_key_violations = 0;
        std::string integrity;
        if (!read_int64(database, "PRAGMA application_id", &application_id) ||
            !read_int64(database, "PRAGMA user_version", &user_version) ||
            application_id != sfx_baseline_application_id ||
            user_version != sfx_baseline_schema_version ||
            !read_int64(database,
                "SELECT count(*) FROM pragma_foreign_key_check", &foreign_key_violations) ||
            foreign_key_violations != 0 ||
            (full_integrity &&
             (!read_text(database, "PRAGMA integrity_check", &integrity) || integrity != "ok"))) {
            return false;
        }
        std::vector<SfxBaselineSchemaObject> objects;
        return collect_schema_manifest(database, &objects) && manifest_matches(std::move(objects)) &&
            exact_ledger(database) && exact_sequence(database);
    } catch (...) {
        return false;
    }
}

bool validate_sfx_prepublication_database(sqlite3* database) noexcept {
    try {
        if (database == nullptr) return false;
        std::int64_t application_id = 0;
        std::int64_t user_version = 0;
        std::int64_t object_count = 0;
        return read_int64(database, "PRAGMA application_id", &application_id) &&
            read_int64(database, "PRAGMA user_version", &user_version) &&
            read_int64(database,
                "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'", &object_count) &&
            application_id == 0 && user_version == 0 && object_count == 0;
    } catch (...) {
        return false;
    }
}

}  // namespace sfx::adapters::storage::internal

namespace sfx::adapters::storage {

std::vector<SqliteMigration> sfx_storage_migrations() {
    return {internal::sfx_initial_baseline_migration(), internal::sfx_caller_journal_migration(),
        internal::sfx_menu_preferences_migration(),internal::sfx_caller_utilities_migration(),internal::sfx_classic_information_migration(),internal::sfx_classic_delete_migration(),internal::sfx_other_board_text_migration(),internal::sfx_local_message_migration(),internal::sfx_message_admin_migration(),internal::sfx_message_group_migration(),internal::sfx_message_delete_migration(),internal::sfx_message_source_migration(),internal::sfx_message_maintenance_migration(),internal::sfx_qwk_migration(),internal::sfx_file_migration(),internal::sfx_door_migration(),internal::sfx_door_security_migration(),internal::sfx_door_program_migration(),internal::sfx_chat_migration(),internal::sfx_private_chat_migration(),internal::sfx_ftn_migration(),internal::sfx_ftn_mail_migration(),internal::sfx_optional_subject_migration(),internal::sfx_file_group_deletion_migration(),internal::sfx_ftn_wire_body_migration(),internal::sfx_ftn_subject_migration(),internal::sfx_message_location_migration(),internal::sfx_ftn_external_migration(),internal::sfx_ftn_recovery_migration(),internal::sfx_door_bulletin_migration(),internal::sfx_message_list_delete_migration()};
}

}  // namespace sfx::adapters::storage
