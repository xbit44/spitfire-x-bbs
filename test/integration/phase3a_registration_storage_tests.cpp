#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/phase3a.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_registration_fingerprint.hpp"
#include "sqlite_statement.hpp"
#include "sqlite3.h"

#include <sodium.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace app = sfx::application;
namespace storage = sfx::adapters::storage;
namespace internal = sfx::adapters::storage::internal;
namespace test = sfx::test;
using namespace std::chrono_literals;

constexpr std::int64_t kBaseUtc = 1'788'200'000;
std::size_t assertion_count = 0;

std::atomic<int> sqlite_write_fault{SQLITE_OK};

struct FaultInjectingFile {
    sqlite3_file base{};
    sqlite3_file* delegate{nullptr};
    bool target{false};
};

FaultInjectingFile* fault_file(sqlite3_file* file) {
    return reinterpret_cast<FaultInjectingFile*>(file);
}

int fault_close(sqlite3_file* file) {
    auto* wrapper = fault_file(file);
    const int result = wrapper->delegate->pMethods->xClose(wrapper->delegate);
    wrapper->base.pMethods = nullptr;
    return result;
}

int fault_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xRead(
        wrapper->delegate, buffer, amount, offset);
}

int fault_write(
    sqlite3_file* file,
    const void* buffer,
    int amount,
    sqlite3_int64 offset) {
    auto* wrapper = fault_file(file);
    if (wrapper->target) {
        const int injected = sqlite_write_fault.exchange(SQLITE_OK);
        if (injected != SQLITE_OK) return injected;
    }
    return wrapper->delegate->pMethods->xWrite(
        wrapper->delegate, buffer, amount, offset);
}

int fault_truncate(sqlite3_file* file, sqlite3_int64 size) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xTruncate(wrapper->delegate, size);
}

int fault_sync(sqlite3_file* file, int flags) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xSync(wrapper->delegate, flags);
}

int fault_file_size(sqlite3_file* file, sqlite3_int64* size) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xFileSize(wrapper->delegate, size);
}

int fault_lock(sqlite3_file* file, int level) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xLock(wrapper->delegate, level);
}

int fault_unlock(sqlite3_file* file, int level) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xUnlock(wrapper->delegate, level);
}

int fault_reserved(sqlite3_file* file, int* result) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xCheckReservedLock(
        wrapper->delegate, result);
}

int fault_control(sqlite3_file* file, int operation, void* argument) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xFileControl(
        wrapper->delegate, operation, argument);
}

int fault_sector_size(sqlite3_file* file) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xSectorSize(wrapper->delegate);
}

int fault_characteristics(sqlite3_file* file) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xDeviceCharacteristics(wrapper->delegate);
}

int fault_shm_map(
    sqlite3_file* file,
    int page,
    int page_size,
    int extend,
    void volatile** output) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xShmMap(
        wrapper->delegate, page, page_size, extend, output);
}

int fault_shm_lock(sqlite3_file* file, int offset, int count, int flags) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xShmLock(
        wrapper->delegate, offset, count, flags);
}

void fault_shm_barrier(sqlite3_file* file) {
    auto* wrapper = fault_file(file);
    wrapper->delegate->pMethods->xShmBarrier(wrapper->delegate);
}

int fault_shm_unmap(sqlite3_file* file, int delete_flag) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xShmUnmap(
        wrapper->delegate, delete_flag);
}

int fault_fetch(
    sqlite3_file* file,
    sqlite3_int64 offset,
    int amount,
    void** output) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xFetch(
        wrapper->delegate, offset, amount, output);
}

int fault_unfetch(sqlite3_file* file, sqlite3_int64 offset, void* pointer) {
    auto* wrapper = fault_file(file);
    return wrapper->delegate->pMethods->xUnfetch(
        wrapper->delegate, offset, pointer);
}

const sqlite3_io_methods fault_methods_v1{
    1, fault_close, fault_read, fault_write, fault_truncate, fault_sync,
    fault_file_size, fault_lock, fault_unlock, fault_reserved, fault_control,
    fault_sector_size, fault_characteristics, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr};

const sqlite3_io_methods fault_methods_v2{
    2, fault_close, fault_read, fault_write, fault_truncate, fault_sync,
    fault_file_size, fault_lock, fault_unlock, fault_reserved, fault_control,
    fault_sector_size, fault_characteristics, fault_shm_map, fault_shm_lock,
    fault_shm_barrier, fault_shm_unmap, nullptr, nullptr};

const sqlite3_io_methods fault_methods_v3{
    3, fault_close, fault_read, fault_write, fault_truncate, fault_sync,
    fault_file_size, fault_lock, fault_unlock, fault_reserved, fault_control,
    fault_sector_size, fault_characteristics, fault_shm_map, fault_shm_lock,
    fault_shm_barrier, fault_shm_unmap, fault_fetch, fault_unfetch};

sqlite3_vfs fault_vfs{};
sqlite3_vfs* delegate_vfs{nullptr};

sqlite3_vfs* vfs_delegate() {
    return static_cast<sqlite3_vfs*>(fault_vfs.pAppData);
}

int fault_open(
    sqlite3_vfs*,
    const char* name,
    sqlite3_file* file,
    int flags,
    int* output_flags) {
    constexpr std::size_t alignment = alignof(std::max_align_t);
    constexpr std::size_t delegate_offset =
        (sizeof(FaultInjectingFile) + alignment - 1U) & ~(alignment - 1U);
    std::memset(file, 0, static_cast<std::size_t>(fault_vfs.szOsFile));
    auto* wrapper = fault_file(file);
    wrapper->delegate = reinterpret_cast<sqlite3_file*>(
        reinterpret_cast<unsigned char*>(file) + delegate_offset);
    const int result = vfs_delegate()->xOpen(
        vfs_delegate(), name, wrapper->delegate, flags, output_flags);
    if (result != SQLITE_OK) return result;
    wrapper->target = name != nullptr &&
        std::strstr(name, "window5-fault") != nullptr;
    switch (wrapper->delegate->pMethods->iVersion) {
        case 1: wrapper->base.pMethods = &fault_methods_v1; break;
        case 2: wrapper->base.pMethods = &fault_methods_v2; break;
        default: wrapper->base.pMethods = &fault_methods_v3; break;
    }
    return SQLITE_OK;
}

int fault_delete(sqlite3_vfs*, const char* name, int sync_directory) {
    return vfs_delegate()->xDelete(vfs_delegate(), name, sync_directory);
}

int fault_access(sqlite3_vfs*, const char* name, int flags, int* result) {
    return vfs_delegate()->xAccess(vfs_delegate(), name, flags, result);
}

int fault_full_path(
    sqlite3_vfs*, const char* name, int output_size, char* output) {
    return vfs_delegate()->xFullPathname(
        vfs_delegate(), name, output_size, output);
}

void* fault_dl_open(sqlite3_vfs*, const char* name) {
    return vfs_delegate()->xDlOpen(vfs_delegate(), name);
}

void fault_dl_error(sqlite3_vfs*, int size, char* message) {
    vfs_delegate()->xDlError(vfs_delegate(), size, message);
}

void (*fault_dl_symbol(sqlite3_vfs*, void* handle, const char* symbol))(void) {
    return vfs_delegate()->xDlSym(vfs_delegate(), handle, symbol);
}

void fault_dl_close(sqlite3_vfs*, void* handle) {
    vfs_delegate()->xDlClose(vfs_delegate(), handle);
}

int fault_random(sqlite3_vfs*, int count, char* output) {
    return vfs_delegate()->xRandomness(vfs_delegate(), count, output);
}

int fault_sleep(sqlite3_vfs*, int microseconds) {
    return vfs_delegate()->xSleep(vfs_delegate(), microseconds);
}

int fault_current_time(sqlite3_vfs*, double* value) {
    return vfs_delegate()->xCurrentTime(vfs_delegate(), value);
}

int fault_last_error(sqlite3_vfs*, int size, char* message) {
    return vfs_delegate()->xGetLastError(vfs_delegate(), size, message);
}

int fault_current_time64(sqlite3_vfs*, sqlite3_int64* value) {
    return vfs_delegate()->xCurrentTimeInt64(vfs_delegate(), value);
}

int fault_set_system_call(
    sqlite3_vfs*, const char* name, sqlite3_syscall_ptr function) {
    return vfs_delegate()->xSetSystemCall(vfs_delegate(), name, function);
}

sqlite3_syscall_ptr fault_get_system_call(sqlite3_vfs*, const char* name) {
    return vfs_delegate()->xGetSystemCall(vfs_delegate(), name);
}

const char* fault_next_system_call(sqlite3_vfs*, const char* name) {
    return vfs_delegate()->xNextSystemCall(vfs_delegate(), name);
}

bool install_fault_vfs() {
    delegate_vfs = sqlite3_vfs_find(nullptr);
    if (delegate_vfs == nullptr) return false;
    constexpr std::size_t alignment = alignof(std::max_align_t);
    constexpr std::size_t delegate_offset =
        (sizeof(FaultInjectingFile) + alignment - 1U) & ~(alignment - 1U);
    fault_vfs.iVersion = delegate_vfs->iVersion;
    fault_vfs.szOsFile = static_cast<int>(
        delegate_offset + static_cast<std::size_t>(delegate_vfs->szOsFile));
    fault_vfs.mxPathname = delegate_vfs->mxPathname;
    fault_vfs.zName = "sfx-window5-fault-vfs";
    fault_vfs.pAppData = delegate_vfs;
    fault_vfs.xOpen = fault_open;
    fault_vfs.xDelete = fault_delete;
    fault_vfs.xAccess = fault_access;
    fault_vfs.xFullPathname = fault_full_path;
    fault_vfs.xDlOpen = fault_dl_open;
    fault_vfs.xDlError = fault_dl_error;
    fault_vfs.xDlSym = fault_dl_symbol;
    fault_vfs.xDlClose = fault_dl_close;
    fault_vfs.xRandomness = fault_random;
    fault_vfs.xSleep = fault_sleep;
    fault_vfs.xCurrentTime = fault_current_time;
    fault_vfs.xGetLastError = fault_last_error;
    if (delegate_vfs->iVersion >= 2) {
        fault_vfs.xCurrentTimeInt64 = fault_current_time64;
    }
    if (delegate_vfs->iVersion >= 3) {
        fault_vfs.xSetSystemCall = fault_set_system_call;
        fault_vfs.xGetSystemCall = fault_get_system_call;
        fault_vfs.xNextSystemCall = fault_next_system_call;
    }
    return sqlite3_vfs_register(&fault_vfs, 1) == SQLITE_OK;
}

void uninstall_fault_vfs() {
    sqlite_write_fault.store(SQLITE_OK);
    if (delegate_vfs != nullptr) {
        (void)sqlite3_vfs_register(delegate_vfs, 1);
        (void)sqlite3_vfs_unregister(&fault_vfs);
    }
}

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++assertion_count;
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "remove Window 5 test tree");
}

app::Identifier128 identifier(const std::uint32_t sequence) {
    app::Identifier128 value;
    value.bytes[0] = 0x53;
    value.bytes[1] = 0x46;
    value.bytes[2] = 0x58;
    value.bytes[3] = 0x42;
    value.bytes[12] = static_cast<std::uint8_t>((sequence >> 24U) & 0xffU);
    value.bytes[13] = static_cast<std::uint8_t>((sequence >> 16U) & 0xffU);
    value.bytes[14] = static_cast<std::uint8_t>((sequence >> 8U) & 0xffU);
    value.bytes[15] = static_cast<std::uint8_t>(sequence & 0xffU);
    return value;
}

app::AuditContext audit(
    const std::uint32_t sequence,
    const std::int64_t instant,
    const app::AuditReasonCode reason) {
    return {identifier(sequence), identifier(100'000U + sequence), std::nullopt,
        instant, reason};
}

app::CredentialEnvelope credential(
    const std::int64_t instant,
    const std::uint8_t marker = 1U) {
    app::CredentialEnvelope value;
    value.envelope_format_version = 1;
    value.scheme_identifier = "argon2id13-test-envelope";
    value.parameter_encoding_version = 1;
    value.canonical_parameters = {0x01U, 0x02U, marker};
    value.salt = {0x10U, 0x11U, 0x12U, marker};
    value.verifier = {0x20U, 0x21U, 0x22U, marker};
    value.created_utc_seconds = instant;
    return value;
}

storage::SqliteStorageSettings settings_for(
    const std::filesystem::path& directory) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return kBaseUtc; };
    settings.queue.maximum_operations = 1024;
    settings.queue.maximum_bytes = 4U * 1024U * 1024U;
    settings.queue.dequeue_batch_limit = 64;
    settings.queue.dequeue_batch_window = 2ms;
    settings.queue.enqueue_wait = 2000ms;
    settings.busy.sqlite_timeout = 50ms;
    settings.busy.transaction_deadline = 1000ms;
    settings.busy.maximum_attempts = 3;
    settings.checkpoint.passive_interval = std::chrono::hours(1);
    settings.readers.pool_size = 1;
    settings.readers.maximum_pending_operations = 1024;
    settings.readers.enqueue_wait = 2000ms;
    return settings;
}

struct ReadyStorage {
    std::unique_ptr<storage::SqliteStorage> database;
    app::AccountId sysop_id{0};
};

ReadyStorage create_ready_storage(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy,
    storage::SqliteStorageSettings settings) {
    std::filesystem::create_directories(directory);
    auto database = std::make_unique<storage::SqliteStorage>(
        std::move(settings), policy, storage::sfx_storage_migrations());
    require(database->start().succeeded(), "Window 5 storage starts");
require(database->health().schema_version == 27,
        "Window 5 operations run against the active clean SFX schema");

    app::StageInitialSysopRequest stage;
    stage.display_name = "Window Five Sysop";
    stage.credential = credential(kBaseUtc);
    stage.audit = audit(1, kBaseUtc, app::AuditReasonCode::initial_setup);
    const app::MutationResult staged = database->stage_initial_sysop(stage);
    require(staged.succeeded() && staged.account_id.has_value(),
        "Window 5 Sysop stages");
    const auto policy_record = database->security_policy();
    require(policy_record.succeeded() && policy_record.value.has_value(),
        "initial policy is readable");
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = policy_record.value->revision;
    complete.expected_policy_hash = policy_record.value->policy_hash;
    complete.published_policy_hash = policy_record.value->policy_hash;
    complete.audit = audit(2, kBaseUtc + 1, app::AuditReasonCode::initial_setup);
    require(database->complete_first_run(complete).succeeded(),
        "Window 5 bootstrap completes");
    return {std::move(database), *staged.account_id};
}

ReadyStorage create_ready_storage(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy) {
    return create_ready_storage(directory, policy, settings_for(directory));
}

app::RegistrationCommitRequest registration_request(
    storage::SqliteStorage& database,
    std::string display_name,
    const std::uint32_t sequence,
    const app::TransferProtocolPreference preference =
        app::TransferProtocolPreference::select_at_transfer) {
    const auto policy = database.security_policy();
    require(policy.succeeded() && policy.value.has_value(),
        "registration fixture reads published policy");
    app::RegistrationCommitRequest request;
    request.account.display_name = std::move(display_name);
    request.account.credential = credential(
        kBaseUtc + static_cast<std::int64_t>(sequence),
        static_cast<std::uint8_t>((sequence % 250U) + 1U));
    request.account.audit = audit(
        sequence, kBaseUtc + static_cast<std::int64_t>(sequence),
        app::AuditReasonCode::registration);
    request.security_level = policy.value->new_user_level;
    request.transfer_preference = preference;
    request.expected_security_policy_revision = policy.value->revision;
    request.expected_security_policy_hash = policy.value->policy_hash;
    return request;
}

std::string fingerprint_hex(
    const internal::RegistrationRequestFingerprint& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest) {
        output.push_back(digits[(byte >> 4U) & 0x0fU]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

std::int64_t scalar(
    const std::filesystem::path& database_path,
    const std::string& sql) {
    internal::SqliteConnection database(
        database_path, SQLITE_OPEN_READONLY, 100ms, false);
    return database.scalar_int64(sql, "Window 5 scalar inspection");
}

std::string scalar_text(
    const std::filesystem::path& database_path,
    const std::string& sql) {
    internal::SqliteConnection database(
        database_path, SQLITE_OPEN_READONLY, 100ms, false);
    return database.scalar_text(sql, "Window 5 text inspection");
}

void exec_raw(
    const std::filesystem::path& database_path,
    const std::string& sql) {
    internal::SqliteConnection database(
        database_path, SQLITE_OPEN_READWRITE, 100ms, false);
    const int result = sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) {
        throw std::runtime_error("Window 5 raw fixture mutation failed");
    }
}

bool raw_fails(
    const std::filesystem::path& database_path,
    const std::string& sql) {
    try {
        exec_raw(database_path, sql);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

std::string sha256_hex(const std::string& value) {
    internal::RegistrationRequestFingerprint digest{};
    require(crypto_hash_sha256(
        digest.data(), reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<unsigned long long>(value.size())) == 0,
        "libsodium computes migration hash");
    return fingerprint_hex(digest);
}

app::RegistrationCommitRequest fingerprint_vector_request(const bool full) {
    app::RegistrationCommitRequest request;
    request.account.display_name = full ? "Caller Vector" : "A";
    if (full) {
        request.account.profile.real_name = "Real Vector";
        request.account.profile.location = "Somewhere";
        request.account.profile.phone = "+1 555 0100";
        request.account.profile.email = "vector@example.test";
        request.account.profile.birth_date = "1980-12-31";
        request.account.profile.sex = 'X';
    }
    request.account.credential = credential(full ? 1'700'000'001 : 1'700'000'000,
        full ? 0x7fU : 0x01U);
    if (full) {
        request.account.credential.pepper_key_identifier = "key-A";
        request.account.credential.password_change_required = true;
    }
    request.account.audit = audit(full ? 0x10203040U : 3U,
        full ? 1'700'000'001 : 1'700'000'000,
        app::AuditReasonCode::registration);
    request.security_level = full ? 321U : 10U;
    request.transfer_preference = full
        ? app::TransferProtocolPreference::ymodem_g
        : app::TransferProtocolPreference::select_at_transfer;
    request.expected_security_policy_revision = full ? 42 : 1;
    request.expected_security_policy_hash = full
        ? "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        : "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    return request;
}

void test_fingerprint_golden_vectors() {
    require(sodium_init() >= 0, "libsodium initializes for Window 5 vectors");
    const auto minimal = internal::registration_request_fingerprint_v1(
        fingerprint_vector_request(false));
    const auto full = internal::registration_request_fingerprint_v1(
        fingerprint_vector_request(true));
    require(minimal.has_value() && full.has_value(),
        "valid requests produce v1 fingerprints");
    const std::string minimal_hex = fingerprint_hex(*minimal);
    const std::string full_hex = fingerprint_hex(*full);
    std::cout << "registration_fingerprint_v1_minimal=" << minimal_hex << '\n';
    std::cout << "registration_fingerprint_v1_full=" << full_hex << '\n';
    require(minimal_hex ==
        "e3b0cbf44cb89a99dad29c89ab2a5e2c240b7fde862cd736b0a723e0bce30112",
        "minimal registration fingerprint matches golden vector");
    require(full_hex ==
        "fca23003193b54a79c88a30d40a2a63da5285b101a67cd21d10a5b3ddbe41a44",
        "full registration fingerprint matches golden vector");

    auto changed = fingerprint_vector_request(true);
    changed.account.profile.location = "Elsewhere";
    const auto changed_digest = internal::registration_request_fingerprint_v1(changed);
    require(changed_digest.has_value() &&
        !internal::registration_request_fingerprints_equal(*full, *changed_digest),
        "a validated request-field change changes the digest");
    changed = fingerprint_vector_request(true);
    changed.account.audit.event_id = identifier(0x10203041U);
    const auto changed_event = internal::registration_request_fingerprint_v1(changed);
    require(changed_event.has_value() &&
        !internal::registration_request_fingerprints_equal(*full, *changed_event),
        "event identity participates in the digest");

    const auto expect_changed = [&full](
        app::RegistrationCommitRequest candidate,
        const char* message) {
        const auto digest = internal::registration_request_fingerprint_v1(candidate);
        require(digest.has_value() &&
            !internal::registration_request_fingerprints_equal(*full, *digest), message);
    };
    changed = fingerprint_vector_request(true);
    changed.account.display_name = "Other Vector";
    expect_changed(changed, "display name participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.profile.real_name = "Other Real";
    expect_changed(changed, "real name participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.profile.phone = "555 0199";
    expect_changed(changed, "phone participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.profile.email = "other@example.test";
    expect_changed(changed, "email participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.profile.birth_date = "1981-01-01";
    expect_changed(changed, "birth date participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.profile.sex = 'F';
    expect_changed(changed, "sex participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.envelope_format_version = 2;
    expect_changed(changed, "credential envelope version participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.scheme_identifier = "argon2id14-test-envelope";
    expect_changed(changed, "credential scheme participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.parameter_encoding_version = 2;
    expect_changed(changed, "parameter version participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.canonical_parameters.push_back(0x44U);
    expect_changed(changed, "canonical parameters participate in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.salt.push_back(0x45U);
    expect_changed(changed, "credential salt participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.verifier.push_back(0x46U);
    expect_changed(changed, "credential verifier participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.pepper_key_identifier = "key-B";
    expect_changed(changed, "optional pepper identifier participates in the digest");
    changed = fingerprint_vector_request(true);
    ++changed.account.credential.created_utc_seconds;
    expect_changed(changed, "credential creation time participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.credential.password_change_required = false;
    expect_changed(changed, "password-change state participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.account.audit.correlation_id = identifier(0x10203042U);
    expect_changed(changed, "correlation identity participates in the digest");
    changed = fingerprint_vector_request(true);
    ++changed.account.audit.occurred_utc_seconds;
    expect_changed(changed, "audit time participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.security_level = 322U;
    expect_changed(changed, "security level participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.transfer_preference = app::TransferProtocolPreference::zmodem;
    expect_changed(changed, "transfer preference participates in the digest");
    changed = fingerprint_vector_request(true);
    ++changed.expected_security_policy_revision;
    expect_changed(changed, "expected policy revision participates in the digest");
    changed = fingerprint_vector_request(true);
    changed.expected_security_policy_hash.assign(64U, 'b');
    expect_changed(changed, "expected policy hash participates in the digest");

    changed = fingerprint_vector_request(true);
    changed.initial_statistics.successful_login_count = 1;
    require(!internal::registration_request_fingerprint_v1(changed).has_value(),
        "invalid non-initial statistics are never fingerprinted");
    changed = fingerprint_vector_request(true);
    changed.account.audit.actor_account_id = 1;
    require(!internal::registration_request_fingerprint_v1(changed).has_value(),
        "invalid registration actor state is never fingerprinted");
}

void test_clean_sfx_receipt_schema(const std::filesystem::path& root) {
    const auto migrations = storage::sfx_storage_migrations();
    require(migrations.size() == 27 && migrations.back().version == 27 && migrations[2].version == 3 &&
        migrations[2].name == "sfx-menu-preferences" &&
        sha256_hex(migrations[2].sql) == migrations[2].content_sha256 && migrations[1].version == 2 &&
        migrations[1].name == "sfx-caller-journal-foundation" &&
        sha256_hex(migrations[1].sql) == migrations[1].content_sha256 &&
        migrations[0].version == 1 &&
        migrations[0].name == "sfx-v1-initial-persistence-baseline" &&
        migrations[0].sql.size() == 25'849U &&
        migrations[0].content_sha256 ==
            "c9ed2791ec70d18cb69696f837d27cc74f931d7c3253247768b7245bf0864806" &&
        sha256_hex(migrations[0].sql) == migrations[0].content_sha256,
        "clean SFX migration identity, byte count, and SQL hash are exact");

    const auto directory = root / "clean-sfx-receipt-schema";
    std::filesystem::create_directories(directory);
    test::FakeStoragePathPolicy policy;
    storage::SqliteStorage database(settings_for(directory), policy, migrations);
require(database.start().succeeded() && database.health().schema_version == 27,
        "clean SFX retains exact baseline and journal, then adds menu preferences");
    require(scalar(database.database_path(),
        "SELECT count(*) FROM sfx_registration_commit_receipts") == 0,
        "clean SFX baseline performs no receipt backfill");
    require(scalar(database.database_path(),
        "SELECT strict FROM pragma_table_list "
        "WHERE name='sfx_registration_commit_receipts'") == 1,
        "registration receipts use a STRICT table");
    require(scalar(database.database_path(),
        "SELECT count(*) FROM pragma_foreign_key_list("
        "'sfx_registration_commit_receipts') WHERE \"table\"='sfx_accounts' "
        "AND on_delete='NO ACTION'") == 1,
        "receipt account foreign key has no cascade");
    require(scalar(database.database_path(),
        "SELECT count(*) FROM pragma_foreign_key_list("
        "'sfx_registration_commit_receipts') WHERE \"table\"='sfx_account_audit'") == 0,
        "receipt has no audit foreign key");
    require(database.shutdown().succeeded(), "clean SFX receipt fixture closes");
}

void require_committed_readback(
    storage::SqliteStorage& database,
    const app::RegistrationCommitRequest& request,
    const app::MutationResult& committed) {
    require(committed.succeeded() && committed.account_id.has_value() &&
        committed.revision == std::optional<std::int64_t>{1} && !committed.replayed,
        "registration commit returns stable account identity and revision one");
    const auto account = database.find_account_by_id(*committed.account_id);
    require(account.succeeded() && account.value.has_value() &&
        account.value->display_name == std::optional<std::string>{request.account.display_name} &&
        account.value->security_level == std::optional<std::uint16_t>{request.security_level} &&
        account.value->statistics.successful_login_count == 0 &&
        account.value->statistics.authenticated_session_seconds == 0,
        "authoritative account/profile/statistics readback succeeds");
    const auto stored_credential = database.load_credential(*committed.account_id);
    require(stored_credential.succeeded() && stored_credential.value.has_value() &&
        stored_credential.value->scheme_identifier ==
            request.account.credential.scheme_identifier &&
        stored_credential.value->verifier.size() ==
            request.account.credential.verifier.size(),
        "opaque credential readback preserves its envelope without diagnostics");
    const auto preference = database.transfer_preference(*committed.account_id);
    require(preference.succeeded() && preference.value.has_value() &&
        preference.value->preference == request.transfer_preference &&
        preference.value->revision == 1,
        "registration preference readback matches the committed request");
    const auto database_path = database.database_path();
    require(scalar(database_path,
        "SELECT count(*) FROM sfx_accounts WHERE account_id=" +
            std::to_string(*committed.account_id)) == 1 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_name_reservations WHERE account_id=" +
            std::to_string(*committed.account_id)) == 1 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_profiles WHERE account_id=" +
            std::to_string(*committed.account_id)) == 1 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_credentials WHERE account_id=" +
            std::to_string(*committed.account_id)) == 1 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_statistics WHERE account_id=" +
            std::to_string(*committed.account_id)) == 1 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_transfer_preferences WHERE account_id=" +
            std::to_string(*committed.account_id)) == 1 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_audit WHERE subject_account_id=" +
            std::to_string(*committed.account_id) +
            " AND event_kind='account.created'") == 1 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_registration_commit_receipts WHERE account_id=" +
            std::to_string(*committed.account_id)) == 1,
        "one transaction creates exactly one of every registration row");
}

void test_success_and_transfer_values(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "success", policy);
    auto& database = *ready.database;
    std::uint32_t sequence = 100U;
    for (const auto preference : {
             app::TransferProtocolPreference::xmodem,
             app::TransferProtocolPreference::ymodem,
             app::TransferProtocolPreference::ymodem_g,
             app::TransferProtocolPreference::zmodem,
             app::TransferProtocolPreference::select_at_transfer}) {
        auto request = registration_request(database,
            "Registration " + std::to_string(sequence), sequence, preference);
        const auto result = database.commit_registration(request);
        require_committed_readback(database, request, result);
        ++sequence;
    }
    require(database.shutdown().succeeded(), "successful registration fixture closes");
}

void test_preconditions_and_legacy_collision(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "preconditions", policy);
    auto& database = *ready.database;

    auto invalid_statistics = registration_request(database,
        "Invalid Statistics", 200U);
    invalid_statistics.initial_statistics.successful_login_count = 1;
    require(database.commit_registration(invalid_statistics).status.code ==
        app::StorageErrorCode::invalid_argument,
        "non-initial registration statistics fail before the writer");
    require(!database.find_account_by_name("Invalid Statistics").succeeded(),
        "invalid statistics create no account");

    auto original = registration_request(database, "Reserved Caller", 201U);
    const auto committed = database.commit_registration(original);
    require(committed.succeeded(), "precondition fixture registration commits");
    auto duplicate_name = registration_request(database, "Reserved Caller", 202U);
    require(database.commit_registration(duplicate_name).status.code ==
        app::StorageErrorCode::conflict,
        "reserved caller name is rejected");
    auto policy_revision_drift = registration_request(database, "Revision Drift", 203U);
    ++policy_revision_drift.expected_security_policy_revision;
    require(database.commit_registration(policy_revision_drift).status.code ==
        app::StorageErrorCode::conflict,
        "security-policy revision drift fails closed");
    auto policy_hash_drift = registration_request(database, "Hash Drift", 204U);
    policy_hash_drift.expected_security_policy_hash.assign(64U, 'f');
    require(database.commit_registration(policy_hash_drift).status.code ==
        app::StorageErrorCode::conflict,
        "security-policy hash drift fails closed");
    auto level_drift = registration_request(database, "Level Drift", 205U);
    level_drift.security_level = 11U;
    require(database.commit_registration(level_drift).status.code ==
        app::StorageErrorCode::conflict,
        "New User security-level drift fails closed");

    app::CreateAccountRequest legacy;
    legacy.display_name = "Legacy Event";
    legacy.credential = credential(kBaseUtc + 206, 0x55U);
    legacy.audit = audit(206U, kBaseUtc + 206, app::AuditReasonCode::registration);
    require(database.create_account(legacy).succeeded(),
        "accepted create_account remains usable");
    auto legacy_event = registration_request(database, "Different Name", 206U);
    require(database.commit_registration(legacy_event).status.code ==
        app::StorageErrorCode::conflict,
        "old audit event without a receipt is not treated as replay");
    require(database.shutdown().succeeded(), "precondition fixture closes");

    const auto incomplete_directory = root / "incomplete-first-run";
    std::filesystem::create_directories(incomplete_directory);
    storage::SqliteStorage incomplete(
        settings_for(incomplete_directory), policy, storage::sfx_storage_migrations());
    require(incomplete.start().succeeded(), "incomplete first-run storage starts");
    auto before_setup = original;
    before_setup.account.display_name = "Before Setup";
    before_setup.account.audit = audit(207U, kBaseUtc + 207,
        app::AuditReasonCode::registration);
    require(incomplete.commit_registration(before_setup).status.code ==
        app::StorageErrorCode::conflict,
        "registration cannot commit before bootstrap completion");
    require(incomplete.shutdown().succeeded(), "incomplete first-run fixture closes");
}

void require_replay(
    storage::SqliteStorage& database,
    const app::RegistrationCommitRequest& request,
    const app::AccountId account_id,
    const char* message) {
    const auto replay = database.commit_registration(request);
    require(replay.succeeded() && replay.replayed &&
        replay.account_id == std::optional<app::AccountId>{account_id} &&
        replay.revision == std::optional<std::int64_t>{1}, message);
}

void test_replay_lifetime(const std::filesystem::path& root) {
    const auto directory = root / "replay-lifetime";
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(directory, policy);
    auto request = registration_request(*ready.database, "Replay Caller", 300U,
        app::TransferProtocolPreference::xmodem);
    request.account.profile.location = "Original Location";
    const auto committed = ready.database->commit_registration(request);
    require(committed.succeeded() && committed.account_id.has_value(),
        "uncertain-outcome fixture commits once");
    const app::AccountId account_id = *committed.account_id;
    require_replay(*ready.database, request, account_id,
        "uncertain immediate retry returns the original receipt");
    require(ready.database->shutdown().succeeded(),
        "uncertain-outcome fixture closes before restart");

    auto restart_settings = settings_for(directory);
    storage::SqliteStorage restarted(
        restart_settings, policy, storage::sfx_storage_migrations());
    require(restarted.start().succeeded(), "receipt fixture restarts");
    require_replay(restarted, request, account_id,
        "replay survives process restart");

    app::RenameAccountRequest rename;
    rename.account_id = account_id;
    rename.expected_revision = 1;
    rename.new_display_name = "Renamed Caller";
    rename.audit = audit(301U, kBaseUtc + 301,
        app::AuditReasonCode::sysop_action);
    const auto renamed = restarted.rename_account(rename);
    require(renamed.succeeded() && renamed.revision == std::optional<std::int64_t>{2},
        "account rename succeeds");
    require_replay(restarted, request, account_id,
        "replay survives account rename");

    app::UpdateProfileRequest profile;
    profile.account_id = account_id;
    profile.expected_revision = 2;
    profile.profile.location = "Changed Location";
    profile.profile.email = "changed@example.test";
    profile.audit = audit(302U, kBaseUtc + 302,
        app::AuditReasonCode::sysop_action);
    const auto profiled = restarted.update_profile(profile);
    require(profiled.succeeded() && profiled.revision == std::optional<std::int64_t>{3},
        "profile mutation succeeds");
    require_replay(restarted, request, account_id,
        "replay survives profile mutation");

    app::ReplaceCredentialRequest replacement;
    replacement.account_id = account_id;
    replacement.expected_revision = 3;
    replacement.credential = credential(kBaseUtc + 303, 0xa5U);
    replacement.audit = audit(303U, kBaseUtc + 303,
        app::AuditReasonCode::sysop_action);
    const auto replaced = restarted.replace_credential(replacement);
    require(replaced.succeeded() && replaced.revision == std::optional<std::int64_t>{4},
        "credential mutation succeeds");
    require_replay(restarted, request, account_id,
        "replay survives credential mutation");

    app::UpdateTransferPreferenceRequest preference;
    preference.account_id = account_id;
    preference.expected_revision = 1;
    preference.preference = app::TransferProtocolPreference::zmodem;
    preference.modified_utc_seconds = kBaseUtc + 304;
    require(restarted.update_transfer_preference(preference).succeeded(),
        "transfer preference mutation succeeds");
    require_replay(restarted, request, account_id,
        "replay survives transfer preference mutation");

    app::StartSessionRequest start_session{
        identifier(304U), identifier(100'304U), account_id, 1U, kBaseUtc + 305};
    require(restarted.record_session_started(start_session).succeeded(),
        "statistics fixture starts a session");
    app::FinalizeSessionRequest finish_session{
        start_session.session_id, 25, kBaseUtc + 330,
        app::SessionOutcomeCode::normal, identifier(305U)};
    require(restarted.finalize_session(finish_session).succeeded(),
        "statistics fixture finalizes a session");
    require_replay(restarted, request, account_id,
        "replay survives statistics/session mutation");

    const auto levels = restarted.security_levels();
    const auto policy_record = restarted.security_policy();
    require(levels.succeeded() && levels.value.has_value() &&
        policy_record.succeeded() && policy_record.value.has_value(),
        "policy mutation fixture reads authoritative policy");
    app::PublishSecurityPolicyRequest publish;
    publish.expected_revision = policy_record.value->revision;
    publish.levels = *levels.value;
    publish.new_user_level = policy_record.value->new_user_level;
    publish.sysop_threshold = policy_record.value->sysop_threshold;
    publish.policy_hash.assign(64U, 'b');
    publish.published_policy_hash = publish.policy_hash;
    publish.audit = audit(306U, kBaseUtc + 331,
        app::AuditReasonCode::security_policy);
    require(restarted.publish_security_policy(publish).succeeded(),
        "security-policy mutation succeeds");
    require_replay(restarted, request, account_id,
        "replay bypasses later security-policy state");

    const auto retention_record = restarted.retention_policy();
    require(retention_record.succeeded() && retention_record.value.has_value(),
        "retention fixture reads current policy");
    app::PublishRetentionPolicyRequest retention;
    retention.expected_revision = retention_record.value->revision;
    retention.audit_online_months = retention_record.value->audit_online_months;
    retention.audit_total_months = retention_record.value->audit_total_months;
    retention.session_online_months = retention_record.value->session_online_months;
    retention.session_total_months = retention_record.value->session_total_months;
    retention.audit = audit(307U, kBaseUtc + 332,
        app::AuditReasonCode::retention_policy);
    require(restarted.publish_retention_policy(retention).succeeded(),
        "retention-policy mutation succeeds");
    require_replay(restarted, request, account_id,
        "replay survives retention-policy mutation");

    const auto database_path = restarted.database_path();
    require(restarted.shutdown().succeeded(),
        "receipt fixture closes before audit pruning");
    exec_raw(database_path,
        "DELETE FROM sfx_account_audit WHERE event_kind='account.created' "
        "AND subject_account_id=" + std::to_string(account_id));
    storage::SqliteStorage after_prune(
        restart_settings, policy, storage::sfx_storage_migrations());
    require(after_prune.start().succeeded(), "audit-pruned fixture restarts");
    require_replay(after_prune, request, account_id,
        "replay survives registration-audit pruning");

    const auto current = after_prune.find_account_by_id(account_id);
    require(current.succeeded() && current.value.has_value(),
        "tombstone fixture reads current revision");
    app::TombstoneAccountRequest tombstone;
    tombstone.account_id = account_id;
    tombstone.expected_revision = current.value->revision;
    tombstone.reason = app::AuditReasonCode::sysop_action;
    tombstone.audit = audit(308U, kBaseUtc + 333,
        app::AuditReasonCode::sysop_action);
    require(after_prune.tombstone_account(tombstone).succeeded(),
        "registered account tombstones normally");
    require_replay(after_prune, request, account_id,
        "replay survives account tombstoning");
    require(scalar(database_path,
        "SELECT count(*) FROM sfx_registration_commit_receipts WHERE account_id=" +
            std::to_string(account_id)) == 1,
        "tombstone retains the immutable receipt");
    require(after_prune.shutdown().succeeded(), "replay lifetime fixture closes");
}

void test_concurrent_races(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "races", policy);
    auto& database = *ready.database;

    const auto identical = registration_request(database, "Identical Race", 400U);
    auto same_a = std::async(std::launch::async, [&] {
        return database.commit_registration(identical);
    });
    auto same_b = std::async(std::launch::async, [&] {
        return database.commit_registration(identical);
    });
    const auto same_first = same_a.get();
    const auto same_second = same_b.get();
    require(same_first.succeeded() && same_second.succeeded() &&
        same_first.account_id == same_second.account_id &&
        same_first.revision == same_second.revision &&
        same_first.replayed != same_second.replayed,
        "concurrent identical event retries converge on one receipt");
    require(scalar(database.database_path(),
        "SELECT count(*) FROM sfx_accounts WHERE name_key='identical race'") == 1,
        "concurrent identical event creates one account");

    const auto name_a = registration_request(database, "Same Name Race", 401U);
    const auto name_b = registration_request(database, "Same Name Race", 402U);
    auto race_a = std::async(std::launch::async, [&] {
        return database.commit_registration(name_a);
    });
    auto race_b = std::async(std::launch::async, [&] {
        return database.commit_registration(name_b);
    });
    const auto result_a = race_a.get();
    const auto result_b = race_b.get();
    const bool a_won = result_a.succeeded() &&
        result_b.status.code == app::StorageErrorCode::conflict;
    const bool b_won = result_b.succeeded() &&
        result_a.status.code == app::StorageErrorCode::conflict;
    require(a_won != b_won,
        "concurrent same-name registrations produce one winner and one conflict");
    require(scalar(database.database_path(),
        "SELECT count(*) FROM sfx_accounts WHERE name_key='same name race'") == 1 &&
        scalar(database.database_path(),
        "SELECT count(*) FROM sfx_registration_commit_receipts r "
        "JOIN sfx_accounts a ON a.account_id=r.account_id "
        "WHERE a.name_key='same name race'") == 1,
        "same-name race leaves one complete registration and receipt");

    auto reused = identical;
    reused.account.profile.location = "Different Content";
    const auto mismatch = database.commit_registration(reused);
    require(mismatch.status.code == app::StorageErrorCode::conflict &&
        !mismatch.account_id.has_value() && !mismatch.revision.has_value() &&
        !mismatch.replayed,
        "same event ID with different canonical content fails closed");
    require(mismatch.status.operation.find("Identical Race") == std::string::npos &&
        mismatch.status.operation.find("Different Content") == std::string::npos &&
        mismatch.status.operation.find("argon2id13-test-envelope") == std::string::npos,
        "failure status exposes no registration private data");
    require(database.shutdown().succeeded(), "race fixture closes");
}

void test_receipt_guards_and_corruption(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    const auto guarded_directory = root / "receipt-guards";
    ReadyStorage guarded = create_ready_storage(guarded_directory, policy);
    const auto guarded_request = registration_request(
        *guarded.database, "Guarded Receipt", 500U);
    const auto guarded_commit = guarded.database->commit_registration(guarded_request);
    require(guarded_commit.succeeded(), "guarded receipt fixture commits");
    const auto guarded_path = guarded.database->database_path();
    require(guarded.database->shutdown().succeeded(), "guarded receipt fixture closes");
    require(raw_fails(guarded_path,
        "UPDATE sfx_registration_commit_receipts "
        "SET committed_account_revision=2") &&
        raw_fails(guarded_path,
        "DELETE FROM sfx_registration_commit_receipts"),
        "normal receipt updates and deletes fail through immutable guards");

    const auto missing_directory = root / "missing-receipt";
    ReadyStorage missing = create_ready_storage(missing_directory, policy);
    const auto missing_request = registration_request(
        *missing.database, "Missing Receipt", 501U);
    require(missing.database->commit_registration(missing_request).succeeded(),
        "missing receipt fixture commits");
    const auto missing_path = missing.database->database_path();
    require(missing.database->shutdown().succeeded(), "missing receipt fixture closes");
    exec_raw(missing_path,
        "DROP TRIGGER sfx_registration_commit_receipts_immutable_delete;"
        "DELETE FROM sfx_registration_commit_receipts;");
    storage::SqliteStorage missing_restart(
        settings_for(missing_directory), policy, storage::sfx_storage_migrations());
    require(!missing_restart.start().succeeded(),
        "clean SFX startup rejects the missing immutable-receipt guard");
    require(scalar(missing_path,
        "SELECT count(*) FROM sfx_accounts WHERE name_key='missing receipt'") == 1,
        "startup rejection does not duplicate or remove the account");
    (void)missing_restart.shutdown();

    const auto corrupt_directory = root / "corrupt-receipt";
    ReadyStorage corrupt = create_ready_storage(corrupt_directory, policy);
    const auto corrupt_request = registration_request(
        *corrupt.database, "Corrupt Receipt", 502U);
    require(corrupt.database->commit_registration(corrupt_request).succeeded(),
        "corrupt receipt fixture commits");
    const auto corrupt_path = corrupt.database->database_path();
    require(corrupt.database->shutdown().succeeded(), "corrupt receipt fixture closes");
    exec_raw(corrupt_path,
        "DROP TRIGGER sfx_registration_commit_receipts_immutable_update;"
        "UPDATE sfx_registration_commit_receipts "
        "SET committed_account_revision=2;");
    storage::SqliteStorage corrupt_restart(
        settings_for(corrupt_directory), policy, storage::sfx_storage_migrations());
    require(!corrupt_restart.start().succeeded(),
        "clean SFX startup rejects the altered immutable-receipt definition");
    (void)corrupt_restart.shutdown();
}

void test_busy_backpressure_constraint_and_rollback(
    const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    const auto busy_directory = root / "busy";
    auto busy_settings = settings_for(busy_directory);
    busy_settings.busy.sqlite_timeout = 10ms;
    busy_settings.busy.transaction_deadline = 100ms;
    busy_settings.busy.maximum_attempts = 2;
    ReadyStorage busy = create_ready_storage(busy_directory, policy, busy_settings);
    const auto busy_request = registration_request(
        *busy.database, "Busy Registration", 600U);
    internal::SqliteConnection lock(
        busy.database->database_path(), SQLITE_OPEN_READWRITE, 10ms, true);
    lock.exec("BEGIN IMMEDIATE", "hold Window 5 busy lock");
    require(busy.database->commit_registration(busy_request).status.code ==
        app::StorageErrorCode::busy,
        "bounded registration writer reports busy");
    lock.exec("ROLLBACK", "release Window 5 busy lock");
    require(!busy.database->find_account_by_name("Busy Registration").succeeded() &&
        scalar(busy.database->database_path(),
        "SELECT count(*) FROM sfx_registration_commit_receipts") == 0,
        "busy transaction leaves no partial registration state");
    require(busy.database->commit_registration(busy_request).succeeded(),
        "identical busy retry commits cleanly after lock release");
    require(busy.database->shutdown().succeeded(), "busy fixture closes");

    const auto pressure_directory = root / "backpressure";
    ReadyStorage pressure_base = create_ready_storage(pressure_directory, policy);
    auto pressure_request = registration_request(
        *pressure_base.database, "Backpressure Registration", 601U);
    require(pressure_base.database->shutdown().succeeded(),
        "backpressure fixture base closes");
    auto pressure_settings = settings_for(pressure_directory);
    pressure_settings.queue.maximum_bytes = 64U;
    storage::SqliteStorage pressured(
        pressure_settings, policy, storage::sfx_storage_migrations());
    require(pressured.start().succeeded(), "bounded backpressure fixture starts");
    require(pressured.commit_registration(pressure_request).status.code ==
        app::StorageErrorCode::backpressure,
        "oversized bounded registration request reports backpressure");
    require(!pressured.find_account_by_name("Backpressure Registration").succeeded() &&
        scalar(pressured.database_path(),
        "SELECT count(*) FROM sfx_registration_commit_receipts") == 0,
        "backpressure leaves no partial registration state");
    require(pressured.shutdown().succeeded(), "backpressure fixture closes");
    storage::SqliteStorage pressure_retry(
        settings_for(pressure_directory), policy, storage::sfx_storage_migrations());
    require(pressure_retry.start().succeeded() &&
        pressure_retry.commit_registration(pressure_request).succeeded(),
        "backpressured request retries under restored capacity");
    require(pressure_retry.shutdown().succeeded(), "backpressure retry closes");

    const auto constraint_directory = root / "constraint";
    ReadyStorage constrained = create_ready_storage(constraint_directory, policy);
    const auto constraint_request = registration_request(
        *constrained.database, "Constraint Registration", 602U);
    exec_raw(constrained.database->database_path(),
        "CREATE TRIGGER window5_force_receipt_constraint "
        "BEFORE INSERT ON sfx_registration_commit_receipts "
        "BEGIN SELECT RAISE(ABORT,'window5 forced receipt constraint'); END;");
    require(constrained.database->commit_registration(constraint_request).status.code ==
        app::StorageErrorCode::constraint,
        "final receipt constraint reports the exact storage class");
    require(!constrained.database->find_account_by_name("Constraint Registration").succeeded() &&
        scalar(constrained.database->database_path(),
        "SELECT count(*) FROM sfx_account_audit WHERE event_kind='account.created' "
        "AND occurred_utc_seconds=" + std::to_string(kBaseUtc + 602)) == 0 &&
        scalar(constrained.database->database_path(),
        "SELECT count(*) FROM sfx_registration_commit_receipts") == 0,
        "late constraint rolls back account, audit, and receipt together");
    exec_raw(constrained.database->database_path(),
        "DROP TRIGGER window5_force_receipt_constraint");
    require(constrained.database->commit_registration(constraint_request).succeeded(),
        "constraint-failed event retries cleanly after fault removal");
    require(constrained.database->shutdown().succeeded(), "constraint fixture closes");
}

void test_integrity_and_error_mapping(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    const auto directory = root / "window5-fault-io-full";
    ReadyStorage ready = create_ready_storage(directory, policy);
    const auto full_request = registration_request(
        *ready.database, "Full Registration", 700U);
    sqlite_write_fault.store(SQLITE_FULL);
    const auto full = ready.database->commit_registration(full_request);
    require(full.status.code == app::StorageErrorCode::full &&
        sqlite_write_fault.load() == SQLITE_OK,
        "test VFS injects and adapter reports SQLITE_FULL");
    const auto database_path = ready.database->database_path();
    require(!ready.database->find_account_by_name("Full Registration").succeeded() &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_audit WHERE event_kind='account.created' "
        "AND occurred_utc_seconds=" + std::to_string(kBaseUtc + 700)) == 0 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_registration_commit_receipts") == 0,
        "disk-full failure rolls back every registration artifact");
    require(ready.database->commit_registration(full_request).succeeded(),
        "disk-full event retries cleanly after capacity returns");

    const auto io_request = registration_request(
        *ready.database, "IO Registration", 701U);
    sqlite_write_fault.store(SQLITE_IOERR_WRITE);
    const auto io = ready.database->commit_registration(io_request);
    require(io.status.code == app::StorageErrorCode::io &&
        sqlite_write_fault.load() == SQLITE_OK,
        "test VFS injects and adapter reports SQLITE_IOERR_WRITE");
    require(!ready.database->find_account_by_name("IO Registration").succeeded() &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_account_audit WHERE event_kind='account.created' "
        "AND occurred_utc_seconds=" + std::to_string(kBaseUtc + 701)) == 0 &&
        scalar(database_path,
        "SELECT count(*) FROM sfx_registration_commit_receipts") == 1,
        "I/O failure rolls back the second registration without harming the first");
    require(ready.database->commit_registration(io_request).succeeded(),
        "I/O-failed event retries cleanly after the fault clears");
    require(ready.database->checkpoint().succeeded(),
        "post-failure checkpoint succeeds");
    require(scalar_text(database_path, "PRAGMA integrity_check") == "ok" &&
        scalar(database_path,
        "SELECT count(*) FROM pragma_foreign_key_check") == 0,
        "full integrity and foreign-key checks pass after write-fault recovery");
    require(internal::map_sqlite_error(SQLITE_IOERR_WRITE) ==
        app::StorageErrorCode::io &&
        internal::map_sqlite_error(SQLITE_FULL) == app::StorageErrorCode::full,
        "SQLite I/O and disk-full results retain accepted error mapping");
    require(ready.database->shutdown().succeeded(), "write-fault fixture closes");

    storage::SqliteStorage restarted(
        settings_for(directory), policy, storage::sfx_storage_migrations());
    require(restarted.start().succeeded() &&
        restarted.find_account_by_name("Full Registration").succeeded() &&
        restarted.find_account_by_name("IO Registration").succeeded(),
        "startup recovery sees only both complete retried registrations");
    require(restarted.shutdown().succeeded(), "write-fault recovery fixture closes");
}

void test_shutdown_failure(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "shutdown", policy);
    const auto request = registration_request(
        *ready.database, "Shutdown Registration", 800U);
    require(ready.database->shutdown().succeeded(), "shutdown fixture closes");
    require(ready.database->commit_registration(request).status.code ==
        app::StorageErrorCode::shutting_down,
        "registration commit after shutdown fails closed");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Window 5 tests require an output directory");
        require(install_fault_vfs(), "test-only fault-injecting VFS installs");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        remove_tree(root);
        std::filesystem::create_directories(root);
        test_fingerprint_golden_vectors();
        test_clean_sfx_receipt_schema(root);
        test_success_and_transfer_values(root);
        test_preconditions_and_legacy_collision(root);
        test_replay_lifetime(root);
        test_concurrent_races(root);
        test_receipt_guards_and_corruption(root);
        test_busy_backpressure_constraint_and_rollback(root);
        test_integrity_and_error_mapping(root);
        test_shutdown_failure(root);
        uninstall_fault_vfs();
        std::cout << "all SFX Phase 3A Window 5 registration storage tests passed ("
                  << assertion_count << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        uninstall_fault_vfs();
        std::cerr << "Window 5 registration storage test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
