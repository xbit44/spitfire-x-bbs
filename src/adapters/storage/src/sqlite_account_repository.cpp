#include "sqlite_account_repository.hpp"
#include "sfx/application/local_file_administration.hpp"
#include "sfx/application/ftn_external.hpp"

#include "sqlite_registration_fingerprint.hpp"
#include "sqlite_statement.hpp"
#include "sqlite_ftn_serial_state.hpp"
#include "sqlite_sfx_baseline.hpp"
#include "sfx/application/caller_journal.hpp"
#include "sfx/application/hostname_lookup.hpp"
#include "sfx/application/message_source.hpp"
#include "sfx/application/qwk.hpp"
#include "sfx/application/portable_credential.hpp"
#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace sfx::adapters::storage::internal {
namespace {

using application::AccountId;
using application::AccountRecord;
using application::AccountState;
using application::AuditContext;
using application::AuditEventKind;
using application::AuditReasonCode;
using application::CredentialEnvelope;
using application::Identifier128;
using application::MutationResult;
using application::SecurityLevelRecord;
using application::SecurityLevelState;
using application::SessionOutcomeCode;
using application::StorageErrorCode;
using application::StorageStatus;

StorageStatus invalid(const char* operation) {
    return {StorageErrorCode::invalid_argument, operation};
}
template<class Audit> void require_runtime_sysop(sqlite3* db,const Audit& audit){
    if(!audit.sysop_session)return; // existing protected local services
    if(!audit.actor_account_id||!audit.sysop_session->valid())
        throw StorageFailure(StorageErrorCode::permission_denied,"runtime Sysop proof unavailable",SQLITE_AUTH);
    SqliteStatement q(db,"SELECT 1 FROM sfx_accounts a JOIN sfx_account_sessions s ON s.account_id=a.account_id JOIN sfx_security_policy p ON p.singleton_id=1 JOIN sfx_security_levels l ON l.security_level=a.security_level WHERE a.account_id=?1 AND s.session_id=?2 AND s.completed_utc_seconds IS NULL AND a.account_state='active' AND l.level_state='active' AND a.security_level>=p.sysop_threshold AND p.published_revision=p.revision AND p.published_policy_hash=p.policy_hash","revalidate exact Runtime Sysop in account transaction");
    q.bind(1,*audit.actor_account_id);q.bind(2,*audit.sysop_session);
    if(!q.row())throw StorageFailure(StorageErrorCode::permission_denied,"runtime Sysop access changed",SQLITE_AUTH);
}
bool chat_statistics_supported(sqlite3* database){
    SqliteStatement version(database,"PRAGMA user_version","read Chat statistics schema");return version.row()&&version.int64(0)>=19;
}
bool private_chat_limits_supported(sqlite3* database){
    SqliteStatement version(database,"PRAGMA user_version","read private Chat limits schema");return version.row()&&version.int64(0)>=20;
}
void read_private_chat_limits(sqlite3* database,SecurityLevelRecord& record){
    if(!private_chat_limits_supported(database))return;
    SqliteStatement limits(database,"SELECT minutes_per_chat,minutes_per_day FROM sfx_security_chat_limits WHERE security_level=?1","read private Chat limits");
    limits.bind(1,record.level);if(limits.row()){
        if(!limits.is_null(0))record.private_chat_minutes=static_cast<std::uint32_t>(limits.int64(0));
        if(!limits.is_null(1))record.private_chat_minutes_per_day=static_cast<std::uint32_t>(limits.int64(1));
    }
}
void record_chat_usage_policy(sqlite3* database,const application::StartSessionRequest& request){
    if(!chat_statistics_supported(database))return;
    SqliteStatement policy(database,"SELECT c.definition,a.security_level,p.sysop_threshold FROM sfx_chat_configuration c JOIN sfx_accounts a ON a.account_id=?1 JOIN sfx_security_policy p WHERE c.singleton_id=1","read usage exclusion at session start");policy.bind(1,request.account_id);
    if(!policy.row())throw StorageFailure(StorageErrorCode::corrupt,"usage policy unavailable",SQLITE_CORRUPT);
    const auto bytes=policy.blob(0);const auto config=bytes.empty()?std::optional<application::ChatConfiguration>(application::ChatConfiguration{}):application::decode_chat_configuration(bytes);
    if(!config)throw StorageFailure(StorageErrorCode::corrupt,"usage policy invalid",SQLITE_CORRUPT);
    if(config->exclude_sysop_statistics&&policy.int64(1)>=policy.int64(2)){
        SqliteStatement save(database,"INSERT INTO sfx_chat_usage_exclusions VALUES(?1)","retain session aggregate exclusion");save.bind(1,request.session_id);save.step();
    }
}
bool calling_time_supported(sqlite3* database) {
    SqliteStatement query(database,"SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sfx_security_calling_limits'","check calling time schema");
    return query.row() && query.int64(0)==1;
}
bool valid_daily_seconds(const std::vector<std::pair<std::string,std::int64_t>>& segments,std::int64_t total) {
    if(total<0 || segments.size()>4096) return false;
    std::int64_t accumulated{};std::set<std::string> days;
    for(const auto& day:segments) {
        if(!application::valid_iso_civil_date(day.first) || day.second<0 || !days.insert(day.first).second ||
            day.second>total-accumulated) return false;
        accumulated+=day.second;
    }
    return segments.empty() || accumulated==total;
}
int persist_daily_seconds(sqlite3* database,Identifier128 session,const std::vector<std::pair<std::string,std::int64_t>>& segments) {
    if(segments.empty()) return SQLITE_OK;
    SqliteStatement previous(database,"SELECT local_date,seconds FROM sfx_caller_daily_seconds WHERE session_id=?1","check committed day segments");
    previous.bind(1,session);
    while(previous.row()) {
        const auto found=std::find_if(segments.begin(),segments.end(),[&](const auto& segment){return segment.first==previous.text(0);});
        if(found==segments.end() || found->second<previous.int64(1)) return SQLITE_ABORT;
    }
    for(const auto& day:segments) {
        SqliteStatement save(database,"INSERT INTO sfx_caller_daily_seconds VALUES(?1,?2,?3) ON CONFLICT(session_id,local_date) DO UPDATE SET seconds=excluded.seconds","persist cumulative daily usage");
        save.bind(1,session);save.bind(2,day.first);save.bind(3,day.second);save.step();
    }
    return SQLITE_OK;
}

bool has_usable_sysop(sqlite3* database, std::int64_t now) {
    SqliteStatement query(database,
        "SELECT count(*) FROM sfx_accounts a JOIN sfx_security_policy p ON p.singleton_id=1 "
        "JOIN sfx_account_credentials c ON c.account_id=a.account_id "
        "JOIN sfx_security_levels l ON l.security_level=a.security_level "
        "WHERE a.account_state='active' AND a.security_level>=p.sysop_threshold "
        "AND l.level_state='active' AND (c.password_change_required=0 OR EXISTS ("
        "SELECT 1 FROM sfx_account_password_resets r WHERE r.account_id=a.account_id "
        "AND r.reset_state='active' AND r.expires_utc_seconds>?1))", "protect last usable Sysop");
    query.bind(1, now);
    return query.row() && query.int64(0) > 0;
}

bool valid_journal_start(const application::StartSessionRequest& request) {
    const auto source = application::canonicalize_source_ip(request.journal_source_ip);
    return (request.journal_transport == "unknown" || request.journal_transport == "Telnet" || request.journal_transport == "SSH") &&
        (request.journal_source_ip.empty() || (source && source->text == request.journal_source_ip)) &&
        request.journal_ansi >= -1 && request.journal_ansi <= 1 &&
        (request.journal_new_registration ? (application::valid_utc_seconds(request.journal_registration_utc) &&
            request.journal_registration_utc <= request.started_utc_seconds) : request.journal_registration_utc == 0);
}

void allocate_caller_number(sqlite3* database, const application::StartSessionRequest& request) {
    // Called only inside the existing successful-session writer transaction.
    SqliteStatement advance(database,
        "UPDATE sfx_caller_number_state SET last_number=last_number+1 "
        "WHERE singleton_id=1 AND last_number<9223372036854775807", "allocate durable caller number");
    advance.step();
    if (sqlite3_changes(database) != 1) throw StorageFailure(StorageErrorCode::full,
        "caller number space exhausted", SQLITE_FULL);
    std::vector<application::CallerActivity> activities{
        {application::CallerActivityKind::successful_logon, request.started_utc_seconds, {}}};
    if (request.journal_new_registration) activities.insert(activities.begin(),
        {application::CallerActivityKind::registration_committed, request.journal_registration_utc, {}});
    const auto encoded = application::encode_caller_activities(activities);
    SqliteStatement journal(database,
        "INSERT INTO sfx_caller_journals(caller_number,session_id,display_name,transport,source_ip,ansi,activities) "
        "SELECT n.last_number,?1,a.display_name,?2,?3,?4,?5 FROM sfx_caller_number_state n "
        "JOIN sfx_accounts a ON a.account_id=?6 WHERE n.singleton_id=1", "begin durable caller journal");
    journal.bind(1, request.session_id); journal.bind(2, request.journal_transport);
    journal.bind(3, request.journal_source_ip); journal.bind(4, static_cast<std::int64_t>(request.journal_ansi));
    journal.bind(5, *encoded); journal.bind(6, request.account_id); journal.step();
    if (sqlite3_changes(database) != 1) throw StorageFailure(StorageErrorCode::corrupt,
        "require caller journal allocation", SQLITE_CORRUPT);
}

bool exact_journal_start(sqlite3* database, const application::StartSessionRequest& request) {
    SqliteStatement row(database, "SELECT transport,source_ip,ansi,activities FROM sfx_caller_journals WHERE session_id=?1",
        "check immutable caller journal replay");
    row.bind(1, request.session_id);
    // Accepted pre-R1 history has no journal row; never manufacture one on replay.
    if (!row.row()) return true;
    if (row.text(0) != request.journal_transport || row.text(1) != request.journal_source_ip ||
        row.int64(2) != request.journal_ansi) return false;
    const auto activities = application::decode_caller_activities(row.text(3));
    if (!activities) return false;
    const auto registration = std::find_if(activities->begin(), activities->end(), [](const auto& activity) {
        return activity.kind == application::CallerActivityKind::registration_committed;
    });
    return request.journal_new_registration
        ? registration != activities->end() && registration->utc_seconds == request.journal_registration_utc
        : registration == activities->end();
}

template <typename T>
application::StorageResult<T> read_failure(StorageStatus status) {
    return {std::move(status), std::nullopt};
}

std::optional<AccountState> account_state(const std::string_view value) {
    if (value == "active") return AccountState::active;
    if (value == "locked") return AccountState::locked;
    if (value == "deletion_scheduled") return AccountState::deletion_scheduled;
    if (value == "tombstoned") return AccountState::tombstoned;
    return std::nullopt;
}

std::optional<SecurityLevelState> security_level_state(const std::string_view value) {
    if (value == "active") return SecurityLevelState::active;
    if (value == "retired") return SecurityLevelState::retired;
    return std::nullopt;
}

std::optional<AuditEventKind> audit_event_kind(const std::string_view value) {
    for (const AuditEventKind kind : {
             AuditEventKind::account_created,
             AuditEventKind::account_renamed,
             AuditEventKind::account_state_changed,
             AuditEventKind::account_profile_changed,
             AuditEventKind::account_credential_replaced,
             AuditEventKind::account_tombstoned,
             AuditEventKind::security_policy_changed,
             AuditEventKind::retention_policy_changed,
             AuditEventKind::first_run_completed,
             AuditEventKind::session_completed}) {
        if (application::storage_code(kind) == value) return kind;
    }
    return std::nullopt;
}

std::optional<AuditReasonCode> audit_reason(const std::string_view value) {
    for (const AuditReasonCode reason : {
             AuditReasonCode::initial_setup,
             AuditReasonCode::registration,
             AuditReasonCode::sysop_action,
             AuditReasonCode::inactivity_policy,
             AuditReasonCode::security_policy,
             AuditReasonCode::retention_policy,
             AuditReasonCode::session_finalization,AuditReasonCode::caller_action}) {
        if (application::storage_code(reason) == value) return reason;
    }
    return std::nullopt;
}

std::optional<SessionOutcomeCode> session_outcome(const std::string_view value) {
    for (const SessionOutcomeCode outcome : {
             SessionOutcomeCode::normal,
             SessionOutcomeCode::disconnected,
             SessionOutcomeCode::timed_out,
             SessionOutcomeCode::shutdown,
             SessionOutcomeCode::runtime_error}) {
        if (application::storage_code(outcome) == value) return outcome;
    }
    return std::nullopt;
}

int guarded_write(const std::function<int()>& operation, bool* assignment_refused=nullptr) noexcept {
    try {
        return operation();
    } catch (const StorageFailure& failure) {
        // Classify only our fixed database-trigger refusal. Never forward raw
        // SQLite diagnostics or caller data through the local protocol.
        if(assignment_refused && (failure.sqlite_code()&0xff)==SQLITE_CONSTRAINT &&
            failure.detail()=="reassign System Sysop first")*assignment_refused=true;
        return failure.sqlite_code();
    } catch (...) {
        return SQLITE_ERROR;
    }
}

int insert_audit(
    sqlite3* database,
    const AuditContext& audit,
    const AuditEventKind kind,
    const std::optional<AccountId> subject) {
    SqliteStatement statement(
        database,
        "INSERT INTO sfx_account_audit(event_id,event_kind,result_code,subject_account_id,"
        "actor_account_id,occurred_utc_seconds,correlation_id,reason_code)"
        " VALUES(?1,?2,'succeeded',?3,?4,?5,?6,?7)",
        "insert typed account audit");
    statement.bind(1, audit.event_id);
    statement.bind(2, application::storage_code(kind));
    bind_optional_account_id(statement, 3, subject);
    bind_optional_account_id(statement, 4, audit.actor_account_id);
    statement.bind(5, audit.occurred_utc_seconds);
    statement.bind(6, audit.correlation_id);
    statement.bind(7, application::storage_code(audit.reason));
    return statement.step() == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
}

int insert_profile(sqlite3* database, const AccountId account_id, const application::AccountProfile& profile) {
    SqliteStatement statement(
        database,
        "INSERT INTO sfx_account_profiles(account_id,real_name,location,phone,email,birth_date,sex)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7)",
        "insert private account profile");
    statement.bind(1, account_id);
    bind_optional_text(statement, 2, profile.real_name);
    bind_optional_text(statement, 3, profile.location);
    bind_optional_text(statement, 4, profile.phone);
    bind_optional_text(statement, 5, profile.email);
    bind_optional_text(statement, 6, profile.birth_date);
    if (profile.sex.has_value()) {
        statement.bind(7, std::string(1U, *profile.sex));
    } else {
        statement.bind_null(7);
    }
    return statement.step() == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
}

int insert_credential(sqlite3* database, const AccountId account_id, const CredentialEnvelope& credential) {
    SqliteStatement statement(
        database,
        "INSERT INTO sfx_account_credentials(account_id,envelope_format_version,scheme_identifier,"
        "parameter_encoding_version,canonical_parameters,salt,verifier,pepper_key_identifier,"
        "created_utc_seconds,password_change_required) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
        "insert opaque credential envelope");
    statement.bind(1, account_id);
    statement.bind(2, static_cast<std::int64_t>(credential.envelope_format_version));
    statement.bind(3, credential.scheme_identifier);
    statement.bind(4, static_cast<std::int64_t>(credential.parameter_encoding_version));
    statement.bind(5, credential.canonical_parameters);
    statement.bind(6, credential.salt);
    statement.bind(7, credential.verifier);
    bind_optional_text(statement, 8, credential.pepper_key_identifier);
    statement.bind(9, credential.created_utc_seconds);
    statement.bind(10, credential.password_change_required);
    return statement.step() == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
}

int update_profile_row(sqlite3* database, const AccountId account_id, const application::AccountProfile& profile) {
    SqliteStatement statement(
        database,
        "INSERT INTO sfx_account_profiles(account_id,real_name,location,phone,email,birth_date,sex)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(account_id) DO UPDATE SET"
        " real_name=excluded.real_name,location=excluded.location,phone=excluded.phone,"
        " email=excluded.email,birth_date=excluded.birth_date,sex=excluded.sex",
        "replace private account profile");
    statement.bind(1, account_id);
    bind_optional_text(statement, 2, profile.real_name);
    bind_optional_text(statement, 3, profile.location);
    bind_optional_text(statement, 4, profile.phone);
    bind_optional_text(statement, 5, profile.email);
    bind_optional_text(statement, 6, profile.birth_date);
    if (profile.sex.has_value()) statement.bind(7, std::string(1U, *profile.sex)); else statement.bind_null(7);
    return statement.step() == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
}

constexpr const char* account_select =
    "SELECT a.account_id,a.display_name,a.name_key,a.security_level,a.account_state,a.revision,"
    "a.purge_allowed,a.created_utc_seconds,a.modified_utc_seconds,a.deletion_scheduled_utc_seconds,"
    "a.tombstoned_utc_seconds,a.tombstone_reason_code,p.real_name,p.location,p.phone,p.email,"
    "p.birth_date,p.sex,s.successful_login_count,s.authenticated_session_seconds,"
    "s.last_successful_login_utc_seconds,s.last_completed_session_utc_seconds,s.last_session_outcome_code "
    "FROM sfx_accounts a LEFT JOIN sfx_account_profiles p ON p.account_id=a.account_id "
    "JOIN sfx_account_statistics s ON s.account_id=a.account_id ";

AccountRecord read_account(SqliteStatement& statement) {
    AccountRecord record;
    record.account_id = statement.int64(0);
    if (!statement.is_null(1)) record.display_name = statement.text(1);
    if (!statement.is_null(2)) record.name_key = statement.text(2);
    if (!statement.is_null(3)) record.security_level = static_cast<std::uint16_t>(statement.int64(3));
    const std::optional<AccountState> state = account_state(statement.text(4));
    if (!state.has_value()) throw StorageFailure(StorageErrorCode::corrupt, "read account state", SQLITE_CORRUPT);
    record.state = *state;
    record.revision = statement.int64(5);
    if (!statement.is_null(6)) record.purge_allowed = statement.boolean(6);
    record.created_utc_seconds = statement.int64(7);
    record.modified_utc_seconds = statement.int64(8);
    if (!statement.is_null(9)) record.deletion_scheduled_utc_seconds = statement.int64(9);
    if (!statement.is_null(10)) record.tombstoned_utc_seconds = statement.int64(10);
    if (!statement.is_null(11)) record.tombstone_reason_code = statement.text(11);
    if (!statement.is_null(12)) record.profile.real_name = statement.text(12);
    if (!statement.is_null(13)) record.profile.location = statement.text(13);
    if (!statement.is_null(14)) record.profile.phone = statement.text(14);
    if (!statement.is_null(15)) record.profile.email = statement.text(15);
    if (!statement.is_null(16)) record.profile.birth_date = statement.text(16);
    if (!statement.is_null(17)) record.profile.sex = statement.text(17).front();
    record.statistics.successful_login_count = statement.int64(18);
    record.statistics.authenticated_session_seconds = statement.int64(19);
    if (!statement.is_null(20)) record.statistics.last_successful_login_utc_seconds = statement.int64(20);
    if (!statement.is_null(21)) record.statistics.last_completed_session_utc_seconds = statement.int64(21);
    if (!statement.is_null(22)) {
        const auto outcome = session_outcome(statement.text(22));
        if (!outcome.has_value()) throw StorageFailure(StorageErrorCode::corrupt, "read session outcome", SQLITE_CORRUPT);
        record.statistics.last_session_outcome = *outcome;
    }
    return record;
}

bool valid_security_level_record(const SecurityLevelRecord& level) {
    if((level.private_chat_minutes&&!*level.private_chat_minutes)||(level.private_chat_minutes_per_day&&!*level.private_chat_minutes_per_day))return false;
    if((level.minutes_per_call && !*level.minutes_per_call) || (level.minutes_per_day && !*level.minutes_per_day) ||
        (level.level==999 && (level.minutes_per_call || level.minutes_per_day))) return false;
    if (level.level < 1 || level.level > 999 || level.label.empty() ||
        level.label.size() > application::kSecurityLabelMaximum ||
        application::normalize_optional_profile_text(level.label, application::kSecurityLabelMaximum) != level.label) {
        return false;
    }
    return !level.description.has_value() ||
        (level.description->size() <= application::kSecurityDescriptionMaximum &&
         application::normalize_optional_profile_text(*level.description, application::kSecurityDescriptionMaximum) ==
             level.description);
}

class ReaderPool final {
public:
    ~ReaderPool() { (void)shutdown(); }

    StorageStatus start(
        const std::filesystem::path& path,
        const SqliteReaderSettings& settings,
        const std::chrono::milliseconds busy_timeout,
        SqlitePagerTelemetry& telemetry,
        std::function<void()> dequeued_hook) {
        if (settings.pool_size == 0 || settings.maximum_pending_operations == 0) {
            return invalid("validate SQLite reader-pool settings");
        }
        path_ = path;
        settings_ = settings;
        busy_timeout_ = busy_timeout;
        telemetry_ = &telemetry;
        dequeued_hook_ = std::move(dequeued_hook);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            health_ = {};
            stopping_ = false;
            accepting_ = true;
        }
        try {
            for (std::size_t index = 0; index < settings_.pool_size; ++index) {
                std::promise<StorageStatus> ready;
                auto future = ready.get_future();
                workers_.emplace_back([this, index, ready = std::move(ready)]() mutable {
                    run(index, std::move(ready));
                });
                const StorageStatus status = future.get();
                if (!status) {
                    (void)shutdown();
                    return status;
                }
            }
        } catch (...) {
            const auto failure = status_from_current_exception("start SQLite account reader workers");
            (void)shutdown(); // joins every successfully constructed partial-start worker
            return failure;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        health_.pool_size = workers_.size();
        return StorageStatus::success();
    }

    StorageStatus shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            stopping_ = true;
        }
        ready_.notify_all();
        capacity_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        return health_.failed ? StorageStatus{StorageErrorCode::internal,
            "SQLite account reader worker failed"} : StorageStatus::success();
    }

    template <typename T, typename Function>
    application::StorageResult<T> read(const char* operation, Function function) {
        auto promise = std::make_shared<std::promise<application::StorageResult<T>>>();
        auto future = promise->get_future();
        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto job = std::make_shared<Job>();
        job->queued_at = std::chrono::steady_clock::now();
        job->fail = [promise, completed, operation] {
            if (!completed->exchange(true))
                promise->set_value(read_failure<T>({StorageErrorCode::internal, operation}));
        };
        job->run = [promise, completed, operation, function = std::move(function)](sqlite3* database) mutable {
            try {
                auto value = function(database);
                if (!completed->exchange(true))
                    promise->set_value({StorageStatus::success(), std::move(value)});
            } catch (const StorageFailure& failure) {
                if (!completed->exchange(true))
                    promise->set_value(read_failure<T>(status_from_exception(failure)));
            } catch (...) {
                if (!completed->exchange(true))
                    promise->set_value(read_failure<T>({StorageErrorCode::internal, operation}));
            }
        };
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto has_capacity = [this] {
                return !accepting_ || queue_.size() < settings_.maximum_pending_operations;
            };
            if (!capacity_.wait_for(lock, settings_.enqueue_wait, has_capacity)) {
                ++health_.rejected_operations;
                return read_failure<T>({StorageErrorCode::backpressure, operation});
            }
            if (!accepting_) {
                ++health_.rejected_operations;
                return read_failure<T>({health_.failed ? StorageErrorCode::internal :
                    StorageErrorCode::shutting_down, operation});
            }
            queue_.push_back(job);
            health_.queued_operations = queue_.size();
            health_.queue_high_water_operations =
                std::max(health_.queue_high_water_operations, queue_.size());
        }
        ready_.notify_one();
        return future.get();
    }

    SqliteReaderPoolHealth health() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return health_;
    }

private:
    struct Job {
        std::chrono::steady_clock::time_point queued_at;
        std::function<void(sqlite3*)> run;
        std::function<void()> fail;
    };

    void run(const std::size_t worker_index, std::promise<StorageStatus> ready) noexcept {
        bool connected = false;
        bool ready_reported = false;
        std::shared_ptr<Job> job;
        try {
            SqliteConnection connection(path_, SQLITE_OPEN_READONLY, busy_timeout_, false);
            telemetry_->connection_opened(worker_index + 1U, connection.get());
            connected = true;
            ready.set_value(StorageStatus::success());
            ready_reported = true;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                    if (queue_.empty() && stopping_) break;
                    job = std::move(queue_.front());
                    queue_.pop_front();
                    health_.queued_operations = queue_.size();
                }
                capacity_.notify_all();
                if (dequeued_hook_) dequeued_hook_();
                const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - job->queued_at);
                job->run(connection.get());
                job.reset();
                telemetry_->sample(worker_index + 1U, connection.get());
                std::lock_guard<std::mutex> lock(mutex_);
                ++health_.completed_operations;
                health_.maximum_queue_wait_microseconds = std::max<std::uint64_t>(
                    health_.maximum_queue_wait_microseconds,
                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, wait.count())));
            }
            telemetry_->connection_closed(worker_index + 1U);
            connected = false;
        } catch (...) {
            std::deque<std::shared_ptr<Job>> rejected;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                health_.failed = true;
                accepting_ = false;
                stopping_ = true;
                rejected.swap(queue_);
                health_.queued_operations = 0;
                health_.rejected_operations += rejected.size() + (job ? 1U : 0U);
            }
            ready_.notify_all();
            capacity_.notify_all();
            if (job) { try { job->fail(); } catch (...) {} }
            for (const auto& pending : rejected) { try { pending->fail(); } catch (...) {} }
            if (connected && telemetry_ != nullptr)
                telemetry_->connection_closed(worker_index + 1U);
            if (!ready_reported)
                ready.set_value(status_from_current_exception("start SQLite account reader"));
        }
    }

    std::filesystem::path path_;
    SqliteReaderSettings settings_;
    std::chrono::milliseconds busy_timeout_{0};
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable capacity_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::vector<std::thread> workers_;
    SqliteReaderPoolHealth health_;
    SqlitePagerTelemetry* telemetry_{nullptr};
    std::function<void()> dequeued_hook_;
    bool accepting_{false};
    bool stopping_{false};
};

}  // namespace

class SqliteAccountRepository::Impl final {
public:
    StorageStatus start(
        const std::filesystem::path& database_path,
        const SqliteStorageSettings& settings,
        SqliteWriterCoordinator& writer,
        SqlitePagerTelemetry& telemetry) {
        writer_ = &writer;
        other_board_text_=settings.other_board_text;
        message_utc_now_=settings.utc_now;
        {
            std::lock_guard<std::mutex> lock(maintenance_mutex_);
            maintenance_path_=database_path;
            maintenance_busy_timeout_=settings.busy.sqlite_timeout;
        }
        return readers_.start(database_path, settings.readers,
            settings.busy.sqlite_timeout, telemetry, settings.reader_dequeued_hook);
    }

    StorageStatus shutdown() {
        const auto status=readers_.shutdown();
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        maintenance_path_.clear();
        return status;
    }
    SqliteReaderPoolHealth reader_health() const { return readers_.health(); }

    // One dedicated bounded integrity connection. Never occupy the ordinary
    // caller reader queue for a full-store scan, or move SQL onto a UI thread.
    // Shutdown waits for this snapshot to close before storage ownership ends.
    template<typename T,typename Function>
    application::StorageResult<T> maintenance_read(const char* operation,Function function){
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        if(maintenance_path_.empty())return read_failure<T>({StorageErrorCode::shutting_down,operation});
        try{
            SqliteConnection connection(maintenance_path_,SQLITE_OPEN_READONLY,maintenance_busy_timeout_,false);
            return {StorageStatus::success(),function(connection.get())};
        }catch(...){return read_failure<T>(status_from_current_exception(operation));}
    }

    template <typename Function>
    MutationResult mutate(const char* operation, const std::size_t estimated_bytes, Function function) {
        struct Output { std::optional<AccountId> account_id; std::optional<std::int64_t> revision; bool replayed{false}; bool last_sysop_protected{false}; bool assignment_refused{false}; };
        auto output = std::make_shared<Output>();
        auto future = writer_->submit({
            operation,
            estimated_bytes,
            WriteDurability::durable,
            [output, function = std::move(function)](sqlite3* database) mutable noexcept {
                return guarded_write([&] { return function(database, *output); },&output->assignment_refused);
            }});
        auto status = future.get();
        if(!status && output->assignment_refused)status={StorageErrorCode::system_sysop_protected,operation};
        return {status, status ? output->account_id : std::nullopt,
            status ? output->revision : std::nullopt, status && output->replayed,
            !status && output->last_sysop_protected};
    }

    template <typename Function>
    StorageStatus write(const char* operation, const std::size_t estimated_bytes, Function function) {
        const auto assignment_refused=std::make_shared<bool>(false);
        auto future = writer_->submit({
            operation,
            estimated_bytes,
            WriteDurability::durable,
            [function = std::move(function),assignment_refused](sqlite3* database) mutable noexcept {
                return guarded_write([&] { return function(database); },assignment_refused.get());
            }});
        auto status=future.get();
        if(!status && *assignment_refused)status={StorageErrorCode::system_sysop_protected,operation};
        return status;
    }

    template <typename T, typename Function>
    application::StorageResult<T> write_result(
        const char* operation,
        const std::size_t estimated_bytes,
        Function function) {
        auto output = std::make_shared<std::optional<T>>();
        auto future = writer_->submit({
            operation,
            estimated_bytes,
            WriteDurability::durable,
            [output, function = std::move(function)](sqlite3* database) mutable noexcept {
                return guarded_write([&] { return function(database, *output); });
            }});
        const StorageStatus status = future.get();
        if (!status) return {status, std::nullopt};
        if (!output->has_value()) {
            return {{StorageErrorCode::internal, operation}, std::nullopt};
        }
        return {status, std::move(**output)};
    }

    ReaderPool readers_;
    SqliteWriterCoordinator* writer_{nullptr};
    std::shared_ptr<application::IOtherBoardTextStorage> other_board_text_;
    std::function<std::int64_t()> message_utc_now_;
    std::mutex maintenance_mutex_;
    std::filesystem::path maintenance_path_;
    std::chrono::milliseconds maintenance_busy_timeout_{250};
};

SqliteAccountRepository::SqliteAccountRepository() : impl_(std::make_unique<Impl>()) {}
SqliteAccountRepository::~SqliteAccountRepository() = default;

StorageStatus SqliteAccountRepository::start(
    const std::filesystem::path& path,
    const SqliteStorageSettings& settings,
    SqliteWriterCoordinator& writer,
    SqlitePagerTelemetry& telemetry) {
    return impl_->start(path, settings, writer, telemetry);
}

StorageStatus SqliteAccountRepository::shutdown() { return impl_->shutdown(); }
SqliteReaderPoolHealth SqliteAccountRepository::reader_health() const { return impl_->reader_health(); }

application::StorageResult<application::FirstRunState> SqliteAccountRepository::first_run_state() {
    return impl_->readers_.read<application::FirstRunState>("read first-run state", [](sqlite3* database) {
        SqliteStatement statement(database,
            "SELECT setup_state,staged_sysop_account_id,completed_utc_seconds FROM sfx_installation_state WHERE singleton_id=1",
            "read first-run state");
        if (!statement.row()) throw StorageFailure(StorageErrorCode::corrupt, "read first-run state", SQLITE_CORRUPT);
        application::FirstRunState state;
        state.complete = statement.text(0) == "complete";
        if (!statement.is_null(1)) state.staged_sysop_account_id = statement.int64(1);
        if (!statement.is_null(2)) state.completed_utc_seconds = statement.int64(2);
        return state;
    });
}

namespace {

bool valid_create_request(const application::CreateAccountRequest& request, const AuditReasonCode reason) {
    const auto normalized = application::normalize_account_name(request.display_name);
    return normalized.has_value() && *normalized == request.display_name &&
        application::valid_profile(request.profile) &&
        application::valid_credential_envelope(request.credential) &&
        application::valid_audit_context(request.audit) && request.audit.reason == reason;
}

template <typename Output>
int create_account_rows(
    sqlite3* database,
    const application::CreateAccountRequest& request,
    const std::uint16_t security_level,
    Output& output) {
    const std::string name_key = *application::account_name_comparison_key(request.display_name);
    SqliteStatement account(database,
        "INSERT INTO sfx_accounts(display_name,name_key,security_level,account_state,revision,purge_allowed,"
        "created_utc_seconds,modified_utc_seconds) VALUES(?1,?2,?3,'active',1,1,?4,?4)",
        "create permanent account");
    account.bind(1, request.display_name);
    account.bind(2, name_key);
    account.bind(3, security_level);
    account.bind(4, request.audit.occurred_utc_seconds);
    account.step();
    const AccountId account_id = sqlite3_last_insert_rowid(database);

    // Migration 3 and its trigger remain immutable. Only a newly created
    // account's trigger-created preference row is changed; old saved Off/On
    // choices and migration backfills are never touched.
    SqliteStatement menu_schema(database,
        "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sfx_account_menu_preferences'",
        "inspect new account preference support");
    if (!menu_schema.row()) return SQLITE_CORRUPT;
    if (menu_schema.int64(0) == 1) {
        SqliteStatement preferences(database,
            "UPDATE sfx_account_menu_preferences SET hotkey=1 WHERE account_id=?1",
            "initialize new account hotkeys");
        preferences.bind(1, account_id);
        preferences.step();
        if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;
    }

    SqliteStatement reservation(database,
        "INSERT INTO sfx_account_name_reservations(name_key,account_id,is_current,display_name,reserved_utc_seconds)"
        " VALUES(?1,?2,1,?3,?4)", "reserve permanent account name");
    reservation.bind(1, name_key);
    reservation.bind(2, account_id);
    reservation.bind(3, request.display_name);
    reservation.bind(4, request.audit.occurred_utc_seconds);
    reservation.step();
    insert_profile(database, account_id, request.profile);
    insert_credential(database, account_id, request.credential);
    SqliteStatement statistics(database,
        "INSERT INTO sfx_account_statistics(account_id) VALUES(?1)", "initialize account statistics");
    statistics.bind(1, account_id);
    statistics.step();
    insert_audit(database, request.audit, AuditEventKind::account_created, account_id);
    output.account_id = account_id;
    output.revision = 1;
    return SQLITE_OK;
}

}  // namespace

MutationResult SqliteAccountRepository::stage_initial_sysop(
    const application::StageInitialSysopRequest& request) {
    if (!valid_create_request(request, AuditReasonCode::initial_setup)) {
        return {invalid("validate staged initial Sysop"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("stage initial Sysop", 2048U + request.credential.verifier.size(),
        [request](sqlite3* database, auto& output) {
            SqliteStatement state(database,
                "SELECT setup_state,staged_sysop_account_id FROM sfx_installation_state WHERE singleton_id=1",
                "inspect initial Sysop staging");
            if (!state.row() || state.text(0) != "incomplete") return SQLITE_ABORT;
            if (!state.is_null(1)) return SQLITE_ABORT;
            SqliteStatement level(database,
                "SELECT count(*) FROM sfx_security_levels WHERE security_level=999 AND level_state='active'",
                "verify initial Sysop level");
            if (!level.row() || level.int64(0) != 1) return SQLITE_ABORT;
            const int code = create_account_rows(database, request, 999, output);
            if (code != SQLITE_OK) return code;
            SqliteStatement update(database,
                "UPDATE sfx_installation_state SET staged_sysop_account_id=?1 WHERE singleton_id=1 AND setup_state='incomplete' AND staged_sysop_account_id IS NULL",
                "record staged initial Sysop");
            update.bind(1, *output.account_id);
            update.step();
            return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_ABORT;
        });
}

MutationResult SqliteAccountRepository::complete_first_run(
    const application::CompleteFirstRunRequest& request) {
    if (request.staged_sysop_account_id <= 0 || request.expected_policy_revision < 1 ||
        !application::valid_sha256_text(request.expected_policy_hash) ||
        request.expected_policy_hash != request.published_policy_hash ||
        !application::valid_audit_context(request.audit) ||
        request.audit.reason != AuditReasonCode::initial_setup) {
        return {invalid("validate first-run completion"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("complete first-run setup", 1024,
        [request](sqlite3* database, auto& output) {
            SqliteStatement staged(database,
                "SELECT count(*) FROM sfx_accounts a JOIN sfx_account_credentials c ON c.account_id=a.account_id "
                "WHERE a.account_id=?1 AND a.account_state='active' AND a.security_level=999",
                "verify staged initial Sysop before completion");
            staged.bind(1, request.staged_sysop_account_id);
            if (!staged.row() || staged.int64(0) != 1) return SQLITE_ABORT;
            SqliteStatement policy(database,
                "UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash,"
                "modified_utc_seconds=?1 WHERE singleton_id=1 AND revision=?2 AND policy_hash=?3",
                "publish matching initial security policy");
            policy.bind(1, request.audit.occurred_utc_seconds);
            policy.bind(2, request.expected_policy_revision);
            policy.bind(3, request.expected_policy_hash);
            policy.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement state(database,
                "UPDATE sfx_installation_state SET setup_state='complete',completed_utc_seconds=?1 "
                "WHERE singleton_id=1 AND setup_state='incomplete' AND staged_sysop_account_id=?2",
                "complete fail-closed first run");
            state.bind(1, request.audit.occurred_utc_seconds);
            state.bind(2, request.staged_sysop_account_id);
            state.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            // New-install defaults belong to first-run completion, not a rewrite
            // of migration15 or a reset on every upgrade/start. Only untouched
            // bootstrap areas qualify; any prior configuration is preserved.
            SqliteStatement presentation_schema(database,
                "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sfx_message_presentation_policy'",
                "inspect optional first-run private presentation");
            if(presentation_schema.row()&&presentation_schema.int64(0)==1){
                SqliteStatement defaults(database,
                    "UPDATE sfx_message_presentation_policy SET private_pipe_colors=1,revision=revision+1 "
                    "WHERE singleton_id=1 AND revision=1 AND private_pipe_colors=0 AND "
                    "NOT EXISTS(SELECT 1 FROM sfx_message_configuration_audit WHERE action=10)",
                    "apply untouched first-run private pipe color default");
                defaults.step();
            }
            SqliteStatement file_schema(database,
                "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sfx_file_areas'",
                "inspect optional first-run file configuration");
            if(file_schema.row()&&file_schema.int64(0)==1){
                SqliteStatement untouched(database,
                    "SELECT count(*) FROM sfx_file_areas WHERE revision=1 AND group_id=1 AND "
                    "access_minimum=1 AND upload_minimum=1 AND download_minimum=1 AND enabled=1 AND "
                    "((area_id=1 AND kind=0 AND uploads_enabled=0) OR "
                    "(area_id=2 AND kind=1 AND uploads_enabled=1) OR (area_id=3 AND kind=2 AND uploads_enabled=1)) "
                    "AND EXISTS(SELECT 1 FROM sfx_file_policy WHERE catalog_revision=1)",
                    "preserve customized first-run areas");
                if(untouched.row()&&untouched.int64(0)==3){
                    SqliteStatement defaults(database,
                        "UPDATE sfx_file_areas SET access_minimum=CASE area_id WHEN 3 THEN 999 ELSE 10 END,"
                        "upload_minimum=CASE area_id WHEN 1 THEN 999 ELSE 10 END,"
                        "download_minimum=CASE area_id WHEN 1 THEN 10 ELSE 999 END,uploads_enabled=1,revision=revision+1 "
                        "WHERE area_id IN(1,2,3)","apply initial file-area permission defaults");
                    defaults.step();if(sqlite3_changes(database)!=3)return SQLITE_ABORT;
                    SqliteStatement file_revision(database,
                        "UPDATE sfx_file_policy SET catalog_revision=catalog_revision+1,revision=revision+1 WHERE singleton_id=1",
                        "publish initial file-area defaults");file_revision.step();
                }
            }
            // Starter categories share the first-run transaction and audit.
            // Never reset an existing board or pre-setup Door customization.
            SqliteStatement door_schema(database,
                "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sfx_door_menu_requirements'",
                "inspect optional first-run Door configuration");
            if (door_schema.row() && door_schema.int64(0) == 1) {
                SqliteStatement untouched(database,
                    "SELECT count(*) FROM sfx_door_policy WHERE singleton_id=1 AND revision=1 "
                    "AND NOT EXISTS(SELECT 1 FROM sfx_door_definitions) "
                    "AND NOT EXISTS(SELECT 1 FROM sfx_door_entries) "
                    "AND (SELECT count(*) FROM sfx_door_menus)=1 "
                    "AND EXISTS(SELECT 1 FROM sfx_door_menus WHERE menu_id=0 AND name='Door Menu') "
                    "AND (SELECT count(*) FROM sfx_door_menu_requirements)=1 "
                    "AND EXISTS(SELECT 1 FROM sfx_door_menu_requirements WHERE menu_id=0 "
                    "AND access_mode=0 AND access_level=1 AND access_sysop=0 "
                    "AND execution_mode=0 AND execution_level=1 AND execution_sysop=0)",
                    "preserve customized first-run Door menus");
                if (untouched.row() && untouched.int64(0) == 1) {
                    SqliteStatement menus(database,
                        "INSERT INTO sfx_door_menus VALUES"
                        "(1,'Main'),(2,'BBS Transporter'),(3,'Utilities & Tools')",
                        "create initial Door categories");
                    menus.step();
                    SqliteStatement requirements(database,
                        "INSERT INTO sfx_door_menu_requirements VALUES"
                        "(1,0,10,0,0,10,0),(2,0,10,0,0,10,0),(3,0,10,0,0,10,0)",
                        "apply initial Door category security levels");
                    requirements.step();
                    SqliteStatement entries(database,
                        "INSERT INTO sfx_door_entries VALUES"
                        "(0,1,1,1,'Main',''),(0,2,1,2,'BBS Transporter',''),"
                        "(0,3,1,3,'Utilities & Tools','')",
                        "place initial Door categories on the root menu");
                    entries.step();
                    if (request.starter_doors_eligible) {
#ifdef __linux__
                        constexpr auto platform=application::StarterDoorPlatform::linux_native;
#else
                        constexpr auto platform=application::StarterDoorPlatform::windows;
#endif
                        const auto starters=application::fresh_install_starter_doors(platform);
                        for (const auto& door:starters) {
                            const auto bytes=application::encode_door_definition(door);
                            if(bytes.empty()) return SQLITE_ABORT;
                            SqliteStatement definition(database,
                                "INSERT INTO sfx_door_definitions VALUES(?1,?2)",
                                "create fresh installation starter Door");
                            definition.bind(1,door.id); definition.bind(2,bytes); definition.step();
                            SqliteStatement placement(database,
                                "INSERT INTO sfx_door_entries VALUES(?1,1,0,?1,?2,'')",
                                "place fresh installation starter Door");
                            placement.bind(1,door.id); placement.bind(2,door.name); placement.step();
                        }
                    }
                    SqliteStatement revision(database,
                        "UPDATE sfx_door_policy SET revision=revision+1 WHERE singleton_id=1 AND revision=1",
                        "publish initial Door category defaults");
                    revision.step();
                    if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
                }
            }
            insert_audit(database, request.audit, AuditEventKind::first_run_completed,
                request.staged_sysop_account_id);
            output.account_id = request.staged_sysop_account_id;
            SqliteStatement revision(database,
                "SELECT revision FROM sfx_accounts WHERE account_id=?1", "read staged Sysop revision");
            revision.bind(1, request.staged_sysop_account_id);
            if (!revision.row()) return SQLITE_NOTFOUND;
            output.revision = revision.int64(0);
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::create_account(const application::CreateAccountRequest& request) {
    if (!valid_create_request(request, AuditReasonCode::registration) &&
        !valid_create_request(request, AuditReasonCode::sysop_action)) {
        return {invalid("validate account creation"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("create account", 2048U + request.credential.verifier.size(),
        [request](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            SqliteStatement setup(database,
                "SELECT count(*) FROM sfx_installation_state WHERE singleton_id=1 AND setup_state='complete'",
                "require completed first run");
            if (!setup.row() || setup.int64(0) != 1) return SQLITE_ABORT;
            SqliteStatement policy(database,
                "SELECT p.new_user_level FROM sfx_security_policy p JOIN sfx_security_levels l "
                "ON l.security_level=p.new_user_level WHERE p.singleton_id=1 AND l.level_state='active' "
                "AND p.published_revision=p.revision AND p.published_policy_hash=p.policy_hash",
                "load published New User level");
            if (!policy.row()) return SQLITE_ABORT;
            return create_account_rows(database, request,
                static_cast<std::uint16_t>(policy.int64(0)), output);
        });
}

MutationResult SqliteAccountRepository::commit_registration(
    const application::RegistrationCommitRequest& request) {
    const auto fingerprint = registration_request_fingerprint_v1(request);
    if (!fingerprint.has_value()) {
        return {invalid("validate registration commit"), std::nullopt, std::nullopt, false};
    }
    const std::size_t estimated_bytes =
        4096U + request.account.display_name.size() +
        request.account.credential.scheme_identifier.size() +
        request.account.credential.canonical_parameters.size() +
        request.account.credential.salt.size() +
        request.account.credential.verifier.size() +
        request.expected_security_policy_hash.size();
    return impl_->mutate("commit registration", estimated_bytes,
        [request, fingerprint = *fingerprint](sqlite3* database, auto& output) {
            SqliteStatement receipt(database,
                "SELECT fingerprint_format_version,request_fingerprint,account_id,"
                "committed_account_revision,committed_utc_seconds "
                "FROM sfx_registration_commit_receipts WHERE registration_event_id=?1",
                "read registration commit receipt");
            receipt.bind(1, request.account.audit.event_id);
            if (receipt.row()) {
                const std::vector<std::uint8_t> stored_bytes = receipt.blob(1);
                if (receipt.is_null(0) || receipt.int64(0) !=
                        kRegistrationFingerprintFormatVersion ||
                    stored_bytes.size() != fingerprint.size() ||
                    receipt.is_null(2) || receipt.int64(2) <= 0 ||
                    receipt.is_null(3) || receipt.int64(3) != 1 ||
                    receipt.is_null(4) ||
                    receipt.int64(4) != request.account.audit.occurred_utc_seconds) {
                    return SQLITE_CORRUPT;
                }
                RegistrationRequestFingerprint stored{};
                std::copy(stored_bytes.begin(), stored_bytes.end(), stored.begin());
                if (!registration_request_fingerprints_equal(stored, fingerprint)) {
                    return SQLITE_ABORT;
                }
                output.account_id = receipt.int64(2);
                output.revision = receipt.int64(3);
                output.replayed = true;
                return SQLITE_OK;
            }

            SqliteStatement older_event(database,
                "SELECT count(*) FROM sfx_account_audit WHERE event_id=?1",
                "reject registration event without immutable receipt");
            older_event.bind(1, request.account.audit.event_id);
            if (!older_event.row() || older_event.int64(0) != 0) {
                return SQLITE_ABORT;
            }

            SqliteStatement setup(database,
                "SELECT count(*) FROM sfx_installation_state "
                "WHERE singleton_id=1 AND setup_state='complete'",
                "require completed first run for registration");
            if (!setup.row() || setup.int64(0) != 1) return SQLITE_ABORT;

            SqliteStatement policy(database,
                "SELECT count(*) FROM sfx_security_policy p "
                "JOIN sfx_security_levels l ON l.security_level=p.new_user_level "
                "WHERE p.singleton_id=1 AND p.revision=?1 AND p.policy_hash=?2 "
                "AND p.published_revision=?1 AND p.published_policy_hash=?2 "
                "AND p.new_user_level=?3 AND l.level_state='active'",
                "validate authoritative registration security policy");
            policy.bind(1, request.expected_security_policy_revision);
            policy.bind(2, request.expected_security_policy_hash);
            policy.bind(3, request.security_level);
            if (!policy.row() || policy.int64(0) != 1) return SQLITE_ABORT;

            const std::string name_key =
                *application::account_name_comparison_key(request.account.display_name);
            SqliteStatement availability(database,
                "SELECT "
                "(SELECT count(*) FROM sfx_account_name_reservations WHERE name_key=?1)+"
                "(SELECT count(*) FROM sfx_accounts WHERE name_key=?1)",
                "validate permanent registration name availability");
            availability.bind(1, name_key);
            if (!availability.row() || availability.int64(0) != 0) {
                return SQLITE_ABORT;
            }

            const int created = create_account_rows(
                database, request.account, request.security_level, output);
            if (created != SQLITE_OK || !output.account_id.has_value() ||
                !output.revision.has_value() || *output.revision != 1) {
                return created == SQLITE_OK ? SQLITE_CORRUPT : created;
            }

            SqliteStatement preference(database,
                "UPDATE sfx_account_transfer_preferences "
                "SET preference_code=?1,revision=1,modified_utc_seconds=?2 "
                "WHERE account_id=?3 AND revision=1",
                "set registration transfer preference");
            preference.bind(1, application::storage_code(request.transfer_preference));
            preference.bind(2, request.account.audit.occurred_utc_seconds);
            preference.bind(3, *output.account_id);
            preference.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;

            SqliteStatement insert_receipt(database,
                "INSERT INTO sfx_registration_commit_receipts("
                "registration_event_id,fingerprint_format_version,request_fingerprint,"
                "account_id,committed_account_revision,committed_utc_seconds)"
                " VALUES(?1,?2,?3,?4,?5,?6)",
                "insert immutable registration commit receipt");
            insert_receipt.bind(1, request.account.audit.event_id);
            insert_receipt.bind(2, kRegistrationFingerprintFormatVersion);
            insert_receipt.bind(3, fingerprint);
            insert_receipt.bind(4, *output.account_id);
            insert_receipt.bind(5, *output.revision);
            insert_receipt.bind(6, request.account.audit.occurred_utc_seconds);
            insert_receipt.step();
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::rename_account(const application::RenameAccountRequest& request) {
    const auto normalized = application::normalize_account_name(request.new_display_name);
    if (request.account_id <= 0 || request.expected_revision < 1 || !normalized.has_value() ||
        *normalized != request.new_display_name || !application::valid_audit_context(request.audit) ||
        request.audit.reason != AuditReasonCode::sysop_action) {
        return {invalid("validate account rename"), std::nullopt, std::nullopt, false};
    }
    const std::string key = *application::account_name_comparison_key(request.new_display_name);
    return impl_->mutate("rename account", 1024,
        [request, key](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            SqliteStatement current(database,
                "SELECT name_key FROM sfx_accounts WHERE account_id=?1 AND revision=?2 AND account_state<>'tombstoned'",
                "load account for rename");
            current.bind(1, request.account_id);
            current.bind(2, request.expected_revision);
            if (!current.row()) return SQLITE_ABORT;
            const std::string old_key = current.text(0);
            if (old_key == key) return SQLITE_ABORT;
            SqliteStatement retire(database,
                "UPDATE sfx_account_name_reservations SET is_current=0,display_name=NULL,retired_utc_seconds=?1 "
                "WHERE name_key=?2 AND account_id=?3 AND is_current=1",
                "retire previous account name");
            retire.bind(1, request.audit.occurred_utc_seconds);
            retire.bind(2, old_key);
            retire.bind(3, request.account_id);
            retire.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;
            SqliteStatement reserve(database,
                "INSERT INTO sfx_account_name_reservations(name_key,account_id,is_current,display_name,reserved_utc_seconds)"
                " VALUES(?1,?2,1,?3,?4)", "reserve renamed account name");
            reserve.bind(1, key);
            reserve.bind(2, request.account_id);
            reserve.bind(3, request.new_display_name);
            reserve.bind(4, request.audit.occurred_utc_seconds);
            reserve.step();
            SqliteStatement update(database,
                "UPDATE sfx_accounts SET display_name=?1,name_key=?2,revision=revision+1,modified_utc_seconds=?3 "
                "WHERE account_id=?4 AND revision=?5 AND account_state<>'tombstoned'",
                "update renamed account");
            update.bind(1, request.new_display_name);
            update.bind(2, key);
            update.bind(3, request.audit.occurred_utc_seconds);
            update.bind(4, request.account_id);
            update.bind(5, request.expected_revision);
            update.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            insert_audit(database, request.audit, AuditEventKind::account_renamed, request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::update_profile(const application::UpdateProfileRequest& request) {
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        !application::valid_profile(request.profile) || !application::valid_audit_context(request.audit) ||
        (request.audit.reason != AuditReasonCode::sysop_action && request.audit.reason != AuditReasonCode::caller_action) ||
        (request.audit.reason==AuditReasonCode::caller_action && request.audit.actor_account_id!=request.account_id)) {
        return {invalid("validate account profile update"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("update account profile", 1024,
        [request](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            if(request.audit.reason==AuditReasonCode::caller_action) {
                SqliteStatement previous(database,"SELECT a.account_state,p.birth_date FROM sfx_accounts a "
                    "JOIN sfx_account_profiles p ON p.account_id=a.account_id WHERE a.account_id=?1 AND a.revision=?2","protect self profile identity and DOB");
                previous.bind(1,request.account_id);previous.bind(2,request.expected_revision);
                if(!previous.row() || previous.text(0)!="active" ||
                    (previous.is_null(1)?request.profile.birth_date.has_value():request.profile.birth_date!=previous.text(1))) return SQLITE_ABORT;
            }
            SqliteStatement account(database,
                "UPDATE sfx_accounts SET revision=revision+1,modified_utc_seconds=?1 "
                "WHERE account_id=?2 AND revision=?3 AND account_state<>'tombstoned'",
                "revise account profile");
            account.bind(1, request.audit.occurred_utc_seconds);
            account.bind(2, request.account_id);
            account.bind(3, request.expected_revision);
            account.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            update_profile_row(database, request.account_id, request.profile);
            insert_audit(database, request.audit, AuditEventKind::account_profile_changed, request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::set_purge_allowed(
    const application::SetPurgeAllowedRequest& request) {
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        !application::valid_audit_context(request.audit) || request.audit.reason != AuditReasonCode::sysop_action) {
        return {invalid("validate Purge Allowed update"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("set Purge Allowed", 512,
        [request](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            SqliteStatement update(database,
                "UPDATE sfx_accounts SET purge_allowed=?1,revision=revision+1,modified_utc_seconds=?2 "
                "WHERE account_id=?3 AND revision=?4 AND account_state<>'tombstoned'",
                "set Purge Allowed");
            update.bind(1, request.purge_allowed);
            update.bind(2, request.audit.occurred_utc_seconds);
            update.bind(3, request.account_id);
            update.bind(4, request.expected_revision);
            update.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            insert_audit(database, request.audit, AuditEventKind::account_profile_changed, request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::assign_security_level(
    const application::AssignAccountSecurityLevelRequest& request) {
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        request.expected_revision == std::numeric_limits<std::int64_t>::max() ||
        request.expected_policy_revision < 1 || request.security_level < 1 ||
        request.security_level > 999 || !application::valid_audit_context(request.audit) ||
        request.audit.reason != AuditReasonCode::sysop_action) {
        return {invalid("validate account security assignment"), {}, {}, false};
    }
    return impl_->mutate("assign account security level", 768,
        [request](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            const bool had_sysop = has_usable_sysop(database,request.audit.occurred_utc_seconds);
            SqliteStatement update(database,
                "UPDATE sfx_accounts SET security_level=?1,revision=revision+1,modified_utc_seconds=?2 "
                "WHERE account_id=?3 AND revision=?4 AND account_state<>'tombstoned' "
                "AND security_level<>?1 AND EXISTS(SELECT 1 FROM sfx_security_policy "
                "WHERE singleton_id=1 AND revision=?5) AND EXISTS(SELECT 1 FROM sfx_security_levels "
                "WHERE security_level=?1 AND level_state='active')",
                "assign exact account configured security level");
            update.bind(1, request.security_level);
            update.bind(2, request.audit.occurred_utc_seconds);
            update.bind(3, request.account_id);
            update.bind(4, request.expected_revision);
            update.bind(5, request.expected_policy_revision);
            update.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            // Existing typed security audit, with an account subject to
            // distinguish assignment from global policy/catalog publication.
            if (had_sysop && !has_usable_sysop(database,request.audit.occurred_utc_seconds)) {
                output.last_sysop_protected=true; return SQLITE_ABORT;
            }
            insert_audit(database, request.audit, AuditEventKind::security_policy_changed,
                request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::save_account(const application::SaveAccountRequest& request) {
    const auto key = application::account_name_comparison_key(request.display_name);
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        request.expected_revision == std::numeric_limits<std::int64_t>::max() ||
        request.expected_policy_revision < 1 || !key ||
        application::normalize_account_name(request.display_name) != request.display_name ||
        !application::valid_profile(request.profile) || request.security_level < 1 || request.security_level > 999 ||
        !application::valid_audit_context(request.audit) || request.audit.reason != AuditReasonCode::sysop_action)
        return {invalid("validate account editor save"), {}, {}, false};
    return impl_->mutate("save user record", 2048, [request, key](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
        const bool had_sysop = has_usable_sysop(database,request.audit.occurred_utc_seconds);
        SqliteStatement current(database,
            "SELECT name_key,display_name,security_level FROM sfx_accounts WHERE account_id=?1 AND revision=?2 "
            "AND account_state<>'tombstoned'", "check editor revision");
        current.bind(1,request.account_id); current.bind(2,request.expected_revision);
        if (!current.row()) return SQLITE_ABORT;
        const bool renamed = current.text(1) != request.display_name;
        const bool access_changed=current.int64(2)!=request.security_level;
        if (current.text(0) != *key) {
            SqliteStatement retire(database,
                "UPDATE sfx_account_name_reservations SET is_current=0,display_name=NULL,retired_utc_seconds=?1 "
                "WHERE account_id=?2 AND is_current=1", "reserve prior user name");
            retire.bind(1,request.audit.occurred_utc_seconds); retire.bind(2,request.account_id); retire.step();
            SqliteStatement reserve(database,
                "INSERT INTO sfx_account_name_reservations(name_key,account_id,is_current,display_name,reserved_utc_seconds) "
                "VALUES(?1,?2,1,?3,?4)", "reserve editor user name");
            reserve.bind(1,*key); reserve.bind(2,request.account_id); reserve.bind(3,request.display_name);
            reserve.bind(4,request.audit.occurred_utc_seconds); reserve.step();
        } else if (renamed) {
            SqliteStatement display(database,
                "UPDATE sfx_account_name_reservations SET display_name=?1 WHERE account_id=?2 AND is_current=1",
                "update user name spelling");
            display.bind(1,request.display_name); display.bind(2,request.account_id); display.step();
        }
        SqliteStatement save(database,
            "UPDATE sfx_accounts SET display_name=?1,name_key=?2,security_level=?3,purge_allowed=?4,"
            "revision=revision+1,modified_utc_seconds=?5 WHERE account_id=?6 AND revision=?7 "
            "AND account_state<>'tombstoned' AND EXISTS(SELECT 1 FROM sfx_security_policy WHERE singleton_id=1 AND revision=?8) "
            "AND EXISTS(SELECT 1 FROM sfx_security_levels WHERE security_level=?3 AND level_state='active')",
            "commit complete user record");
        save.bind(1,request.display_name); save.bind(2,*key); save.bind(3,request.security_level);
        save.bind(4,request.purge_allowed); save.bind(5,request.audit.occurred_utc_seconds);
        save.bind(6,request.account_id); save.bind(7,request.expected_revision); save.bind(8,request.expected_policy_revision);
        save.step();
        if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
        if (had_sysop && !has_usable_sysop(database,request.audit.occurred_utc_seconds)) {
            output.last_sysop_protected=true; return SQLITE_ABORT;
        }
        update_profile_row(database,request.account_id,request.profile);
        insert_audit(database,request.audit,access_changed ? AuditEventKind::security_policy_changed :
            renamed ? AuditEventKind::account_renamed : AuditEventKind::account_profile_changed,request.account_id);
        output.account_id=request.account_id; output.revision=request.expected_revision+1;
        return SQLITE_OK;
    });
}

MutationResult SqliteAccountRepository::replace_credential(
    const application::ReplaceCredentialRequest& request) {
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        !application::valid_credential_envelope(request.credential) ||
        !application::valid_audit_context(request.audit) || request.audit.reason != AuditReasonCode::sysop_action) {
        return {invalid("validate opaque credential replacement"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("replace opaque credential", 1024U + request.credential.verifier.size(),
        [request](sqlite3* database, auto& output) {
            SqliteStatement account(database,
                "UPDATE sfx_accounts SET revision=revision+1,modified_utc_seconds=?1 "
                "WHERE account_id=?2 AND revision=?3 AND account_state<>'tombstoned'",
                "revise credential metadata");
            account.bind(1, request.audit.occurred_utc_seconds);
            account.bind(2, request.account_id);
            account.bind(3, request.expected_revision);
            account.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement remove(database,
                "DELETE FROM sfx_account_credentials WHERE account_id=?1", "remove previous credential envelope");
            remove.bind(1, request.account_id);
            remove.step();
            insert_credential(database, request.account_id, request.credential);
            insert_audit(database, request.audit, AuditEventKind::account_credential_replaced, request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::change_account_state(
    const application::ChangeAccountStateRequest& request) {
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        request.new_state == AccountState::tombstoned || !application::valid_audit_context(request.audit) ||
        (request.audit.reason != AuditReasonCode::sysop_action &&
         request.audit.reason != AuditReasonCode::inactivity_policy) ||
        (request.new_state == AccountState::deletion_scheduled) != request.deletion_scheduled_utc_seconds.has_value() ||
        (request.deletion_scheduled_utc_seconds.has_value() &&
         !application::valid_utc_seconds(*request.deletion_scheduled_utc_seconds))) {
        return {invalid("validate account-state change"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("change account state", 768,
        [request](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            const bool had_sysop = has_usable_sysop(database,request.audit.occurred_utc_seconds);
            if (request.audit.reason == AuditReasonCode::inactivity_policy) {
                SqliteStatement eligible(database,
                    "SELECT count(*) FROM sfx_accounts a JOIN sfx_security_policy p ON p.singleton_id=1 "
                    "WHERE a.account_id=?1 AND a.revision=?2 AND a.account_state='active' AND a.purge_allowed=1 "
                    "AND a.security_level<p.sysop_threshold",
                    "recheck inactivity scheduling exemption");
                eligible.bind(1, request.account_id);
                eligible.bind(2, request.expected_revision);
                if (!eligible.row() || eligible.int64(0) != 1 ||
                    request.new_state != AccountState::deletion_scheduled) return SQLITE_ABORT;
            }
            SqliteStatement update(database,
                "UPDATE sfx_accounts SET account_state=?1,deletion_scheduled_utc_seconds=?2,"
                "revision=revision+1,modified_utc_seconds=?3 WHERE account_id=?4 AND revision=?5 "
                "AND account_state<>'tombstoned' AND account_state<>?1",
                "change account state");
            update.bind(1, application::storage_code(request.new_state));
            if (request.deletion_scheduled_utc_seconds.has_value())
                update.bind(2, *request.deletion_scheduled_utc_seconds);
            else update.bind_null(2);
            update.bind(3, request.audit.occurred_utc_seconds);
            update.bind(4, request.account_id);
            update.bind(5, request.expected_revision);
            update.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            if (had_sysop && !has_usable_sysop(database,request.audit.occurred_utc_seconds)) {
                output.last_sysop_protected=true; return SQLITE_ABORT;
            }
            insert_audit(database, request.audit, AuditEventKind::account_state_changed, request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::tombstone_account(
    const application::TombstoneAccountRequest& request) {
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        (request.reason != AuditReasonCode::sysop_action &&
         request.reason != AuditReasonCode::inactivity_policy) ||
        !application::valid_audit_context(request.audit) || request.audit.reason != request.reason) {
        return {invalid("validate account tombstone"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("tombstone account", 1024,
        [request](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            const bool had_sysop = has_usable_sysop(database,request.audit.occurred_utc_seconds);
            SqliteStatement current(database,
                "SELECT a.account_state,a.purge_allowed,a.security_level<p.sysop_threshold "
                "FROM sfx_accounts a JOIN sfx_security_policy p ON p.singleton_id=1 "
                "WHERE a.account_id=?1 AND a.revision=?2 AND a.account_state<>'tombstoned'",
                "load account for tombstone");
            current.bind(1, request.account_id);
            current.bind(2, request.expected_revision);
            if (!current.row()) return SQLITE_ABORT;
            if (request.reason == AuditReasonCode::inactivity_policy &&
                (current.text(0) != application::storage_code(AccountState::deletion_scheduled) ||
                 !current.boolean(1) || !current.boolean(2))) return SQLITE_ABORT;
            SqliteStatement active_sessions(database,
                "SELECT count(*) FROM sfx_account_sessions WHERE account_id=?1 AND completed_utc_seconds IS NULL",
                "reject tombstone with active authenticated session");
            active_sessions.bind(1, request.account_id);
            if (!active_sessions.row() || active_sessions.int64(0) != 0) return SQLITE_ABORT;
            SqliteStatement profile(database, "DELETE FROM sfx_account_profiles WHERE account_id=?1",
                "remove tombstoned profile");
            profile.bind(1, request.account_id);
            profile.step();
            SqliteStatement credential(database, "DELETE FROM sfx_account_credentials WHERE account_id=?1",
                "remove tombstoned credential");
            credential.bind(1, request.account_id);
            credential.step();
            SqliteStatement stage5c_tables(database,
                "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                "AND name='sfx_successful_login_ip_history'",
                "detect Stage 5C account history");
            if (!stage5c_tables.row()) return SQLITE_CORRUPT;
            if (stage5c_tables.int64(0) == 1) {
                // Deletion blocks access, not historical retention. Login IP
                // history keeps its account reference and normal retention.
                SqliteStatement reset_history(database,
                    "DELETE FROM sfx_account_password_resets WHERE account_id=?1",
                    "remove tombstoned temporary-password state");
                reset_history.bind(1, request.account_id);
                reset_history.step();
            }
            SqliteStatement phase3a_preference_table(database,
                "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                "AND name='sfx_account_transfer_preferences'",
                "detect Phase 3A transfer preference");
            if (!phase3a_preference_table.row()) return SQLITE_CORRUPT;
            if (phase3a_preference_table.int64(0) == 1) {
                SqliteStatement preference(database,
                    "DELETE FROM sfx_account_transfer_preferences WHERE account_id=?1",
                    "remove tombstoned transfer preference");
                preference.bind(1, request.account_id);
                preference.step();
            }
            SqliteStatement phase3a_notice_table(database,
                "SELECT count(*) FROM sqlite_schema WHERE type='table' "
                "AND name='sfx_failed_login_notices'",
                "detect Phase 3A failed-login notices");
            if (!phase3a_notice_table.row()) return SQLITE_CORRUPT;
            if (phase3a_notice_table.int64(0) == 1) {
                SqliteStatement notices(database,
                    "DELETE FROM sfx_failed_login_notices WHERE account_id=?1",
                    "remove tombstoned failed-login notices");
                notices.bind(1, request.account_id);
                notices.step();
                SqliteStatement claims(database,
                    "DELETE FROM sfx_failed_login_notice_claims WHERE account_id=?1",
                    "remove tombstoned failed-login claim receipts");
                claims.bind(1, request.account_id);
                claims.step();
                SqliteStatement enqueues(database,
                    "DELETE FROM sfx_failed_login_notice_enqueues WHERE account_id=?1",
                    "remove tombstoned failed-login enqueue receipts");
                enqueues.bind(1, request.account_id);
                enqueues.step();
            }
            SqliteStatement reservations(database,
                "UPDATE sfx_account_name_reservations SET is_current=0,display_name=NULL,retired_utc_seconds=?1 "
                "WHERE account_id=?2 AND is_current=1", "hide tombstoned name reservation");
            reservations.bind(1, request.audit.occurred_utc_seconds);
            reservations.bind(2, request.account_id);
            reservations.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;
            // Preserve historical statistics/session dates along with the
            // durable account ID, session journal and audit references.
            SqliteStatement account(database,
                "UPDATE sfx_accounts SET display_name=NULL,name_key=NULL,security_level=NULL,account_state='tombstoned',"
                "revision=revision+1,purge_allowed=NULL,deletion_scheduled_utc_seconds=NULL,modified_utc_seconds=?1,tombstoned_utc_seconds=?1,"
                "tombstone_reason_code=?2 WHERE account_id=?3 AND revision=?4 AND account_state<>'tombstoned'",
                "minimize permanent account tombstone");
            account.bind(1, request.audit.occurred_utc_seconds);
            account.bind(2, application::storage_code(request.reason));
            account.bind(3, request.account_id);
            account.bind(4, request.expected_revision);
            account.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            if (had_sysop && !has_usable_sysop(database,request.audit.occurred_utc_seconds)) {
                output.last_sysop_protected=true; return SQLITE_ABORT;
            }
            insert_audit(database, request.audit, AuditEventKind::account_tombstoned, request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

StorageStatus SqliteAccountRepository::publish_security_policy(
    const application::PublishSecurityPolicyRequest& request) {
    if (request.expected_revision < 1 || request.levels.empty() ||
        !application::valid_sha256_text(request.policy_hash) ||
        request.policy_hash != request.published_policy_hash ||
        !application::valid_audit_context(request.audit) ||
        request.audit.reason != AuditReasonCode::security_policy) {
        return invalid("validate security-policy publication");
    }
    std::map<std::uint16_t, SecurityLevelRecord> levels;
    for (const auto& level : request.levels) {
        if (!valid_security_level_record(level) || !levels.emplace(level.level, level).second) {
            return invalid("validate security-level catalog");
        }
    }
    if (levels.count(request.new_user_level) == 0 || levels.count(request.sysop_threshold) == 0 ||
        levels.at(request.new_user_level).state != SecurityLevelState::active ||
        levels.at(request.sysop_threshold).state != SecurityLevelState::active) {
        return invalid("validate active security-policy references");
    }
    std::map<std::uint16_t, std::uint16_t> reassignments;
    for (const auto& reassignment : request.reassignments) {
        if (!reassignments.emplace(reassignment.retired_level, reassignment.replacement_level).second ||
            levels.count(reassignment.replacement_level) == 0 ||
            levels.at(reassignment.replacement_level).state != SecurityLevelState::active) {
            return invalid("validate security-level reassignments");
        }
    }
    return impl_->write("publish security policy", 4096U + request.levels.size() * 256U,
        [request, levels, reassignments](sqlite3* database) {
            const bool had_sysop = has_usable_sysop(database,request.audit.occurred_utc_seconds);
            SqliteStatement revision(database,
                "SELECT revision FROM sfx_security_policy WHERE singleton_id=1", "read security-policy revision");
            if (!revision.row()) return SQLITE_CORRUPT;
            if (revision.int64(0) != request.expected_revision) return SQLITE_ABORT;

            std::map<std::uint16_t, SecurityLevelState> existing;
            if(request.expected_reassigned_accounts) {
                std::vector<std::pair<AccountId,std::int64_t>> affected;
                for(const auto& mapping:reassignments) {
                    SqliteStatement users(database,"SELECT account_id,revision FROM sfx_accounts WHERE security_level=?1 AND account_state<>'tombstoned' ORDER BY account_id","verify reviewed level removal");
                    users.bind(1,mapping.first);
                    while(users.row()) affected.emplace_back(users.int64(0),users.int64(1));
                }
                std::sort(affected.begin(),affected.end());
                if(affected!=*request.expected_reassigned_accounts) return SQLITE_ABORT;
            }
            SqliteStatement rows(database,
                "SELECT security_level,level_state FROM sfx_security_levels ORDER BY security_level",
                "read existing security-level catalog");
            while (rows.row()) {
                const auto state = security_level_state(rows.text(1));
                if (!state.has_value()) return SQLITE_CORRUPT;
                existing.emplace(static_cast<std::uint16_t>(rows.int64(0)), *state);
            }
            for (const auto& proposed : levels) {
                if (existing.count(proposed.first) == 0) {
                    SqliteStatement insert(database,
                        "INSERT INTO sfx_security_levels(security_level,label,description,level_state,created_utc_seconds,modified_utc_seconds)"
                        " VALUES(?1,?2,?3,'active',?4,?4)",
                        "add active security level before reassignment");
                    insert.bind(1, proposed.first);
                    insert.bind(2, proposed.second.label);
                    bind_optional_text(insert, 3, proposed.second.description);
                    insert.bind(4, request.audit.occurred_utc_seconds);
                    insert.step();
                }
            }
            for (const auto& current : existing) {
                const auto proposed = levels.find(current.first);
                if (proposed == levels.end() ||
                    (current.second == SecurityLevelState::retired &&
                     proposed->second.state != SecurityLevelState::retired)) return SQLITE_ABORT;
                if (current.second == SecurityLevelState::active &&
                    proposed->second.state == SecurityLevelState::retired) {
                    const auto mapping = reassignments.find(current.first);
                    if (mapping == reassignments.end()) return SQLITE_ABORT;
                    SqliteStatement accounts(database,
                        "UPDATE sfx_accounts SET security_level=?1,revision=revision+1,modified_utc_seconds=?2 "
                        "WHERE security_level=?3 AND account_state<>'tombstoned'",
                        "reassign accounts from retired security level");
                    accounts.bind(1, mapping->second);
                    accounts.bind(2, request.audit.occurred_utc_seconds);
                    accounts.bind(3, current.first);
                    accounts.step();
                }
            }
            for (const auto& proposed : levels) {
                const auto prior = existing.find(proposed.first);
                if (prior == existing.end() && proposed.second.state != SecurityLevelState::active)
                    return SQLITE_ABORT;
                SqliteStatement upsert(database,
                    "INSERT INTO sfx_security_levels(security_level,label,description,level_state,created_utc_seconds,modified_utc_seconds,retired_utc_seconds)"
                    " VALUES(?1,?2,?3,?4,?5,?5,?6) ON CONFLICT(security_level) DO UPDATE SET label=excluded.label,"
                    "description=excluded.description,level_state=excluded.level_state,modified_utc_seconds=excluded.modified_utc_seconds,"
                    "retired_utc_seconds=CASE WHEN sfx_security_levels.level_state='active' AND excluded.level_state='retired'"
                    " THEN excluded.modified_utc_seconds ELSE sfx_security_levels.retired_utc_seconds END",
                    "publish security-level definition");
                upsert.bind(1, proposed.first);
                upsert.bind(2, proposed.second.label);
                bind_optional_text(upsert, 3, proposed.second.description);
                upsert.bind(4, application::storage_code(proposed.second.state));
                upsert.bind(5, request.audit.occurred_utc_seconds);
                if (proposed.second.state == SecurityLevelState::retired)
                    upsert.bind(6, request.audit.occurred_utc_seconds);
                else upsert.bind_null(6);
                upsert.step();
                if(calling_time_supported(database)) {
                    SqliteStatement allowance(database,"INSERT INTO sfx_security_calling_limits VALUES(?1,?2,?3) "
                        "ON CONFLICT(security_level) DO UPDATE SET minutes_per_call=excluded.minutes_per_call,minutes_per_day=excluded.minutes_per_day",
                        "publish independent calling limits");
                    allowance.bind(1,proposed.first);
                    if(proposed.second.minutes_per_call) allowance.bind(2,static_cast<std::int64_t>(*proposed.second.minutes_per_call));else allowance.bind_null(2);
                    if(proposed.second.minutes_per_day) allowance.bind(3,static_cast<std::int64_t>(*proposed.second.minutes_per_day));else allowance.bind_null(3);
                    allowance.step();
                } else if(proposed.second.minutes_per_call || proposed.second.minutes_per_day) return SQLITE_ABORT;
                if(private_chat_limits_supported(database)){
                    SqliteStatement limits(database,"INSERT INTO sfx_security_chat_limits VALUES(?1,?2,?3) ON CONFLICT(security_level) DO UPDATE SET minutes_per_chat=excluded.minutes_per_chat,minutes_per_day=excluded.minutes_per_day","publish private Chat limits");
                    limits.bind(1,proposed.first);
                    if(proposed.second.private_chat_minutes)limits.bind(2,static_cast<std::int64_t>(*proposed.second.private_chat_minutes));else limits.bind_null(2);
                    if(proposed.second.private_chat_minutes_per_day)limits.bind(3,static_cast<std::int64_t>(*proposed.second.private_chat_minutes_per_day));else limits.bind_null(3);
                    limits.step();
                }else if(proposed.second.private_chat_minutes||proposed.second.private_chat_minutes_per_day)return SQLITE_ABORT;
            }
            SqliteStatement policy(database,
                "UPDATE sfx_security_policy SET revision=revision+1,new_user_level=?1,sysop_threshold=?2,"
                "policy_hash=?3,published_revision=revision+1,published_policy_hash=?4,modified_utc_seconds=?5 "
                "WHERE singleton_id=1 AND revision=?6",
                "publish database-authoritative security policy");
            policy.bind(1, request.new_user_level);
            policy.bind(2, request.sysop_threshold);
            policy.bind(3, request.policy_hash);
            policy.bind(4, request.published_policy_hash);
            policy.bind(5, request.audit.occurred_utc_seconds);
            policy.bind(6, request.expected_revision);
            policy.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            if (had_sysop && !has_usable_sysop(database,request.audit.occurred_utc_seconds)) return SQLITE_ABORT;
            return insert_audit(database, request.audit, AuditEventKind::security_policy_changed, std::nullopt);
        });
}

StorageStatus SqliteAccountRepository::publish_retention_policy(
    const application::PublishRetentionPolicyRequest& request) {
    application::RetentionPolicyRecord proposed;
    proposed.revision = request.expected_revision + 1;
    proposed.audit_online_months = request.audit_online_months;
    proposed.audit_total_months = request.audit_total_months;
    proposed.session_online_months = request.session_online_months;
    proposed.session_total_months = request.session_total_months;
    proposed.modified_utc_seconds = request.audit.occurred_utc_seconds;
    if (request.expected_revision < 1 || !application::valid_retention_policy(proposed) ||
        !application::valid_audit_context(request.audit) ||
        request.audit.reason != AuditReasonCode::retention_policy) {
        return invalid("validate retention-policy publication");
    }
    return impl_->write("publish retention policy", 1024U, [request](sqlite3* database) {
        SqliteStatement current(database,
            "SELECT revision,audit_online_months,audit_total_months,session_online_months,session_total_months "
            "FROM sfx_account_retention_policy WHERE singleton_id=1", "read retention-policy revision");
        if (!current.row()) return SQLITE_CORRUPT;
        if (current.int64(0) != request.expected_revision) return SQLITE_ABORT;
        const bool shortening = request.audit_online_months < current.int64(1) ||
            request.audit_total_months < current.int64(2) ||
            request.session_online_months < current.int64(3) ||
            request.session_total_months < current.int64(4);
        if (shortening && !request.shortening_effect_preview_confirmed) return SQLITE_ABORT;
        SqliteStatement update(database,
            "UPDATE sfx_account_retention_policy SET revision=revision+1,audit_online_months=?1,"
            "audit_total_months=?2,session_online_months=?3,session_total_months=?4,modified_utc_seconds=?5 "
            "WHERE singleton_id=1 AND revision=?6", "publish versioned retention policy");
        update.bind(1, request.audit_online_months);
        update.bind(2, request.audit_total_months);
        update.bind(3, request.session_online_months);
        update.bind(4, request.session_total_months);
        update.bind(5, request.audit.occurred_utc_seconds);
        update.bind(6, request.expected_revision);
        update.step();
        if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
        return insert_audit(database, request.audit, AuditEventKind::retention_policy_changed, std::nullopt);
    });
}

MutationResult SqliteAccountRepository::record_session_started(
    const application::StartSessionRequest& request) {
    if (!request.session_id.valid() || !request.correlation_id.valid() || request.account_id <= 0 ||
        request.node_id < 1 || request.node_id > 128 ||
        !application::valid_utc_seconds(request.started_utc_seconds) || !valid_journal_start(request)) {
        return {invalid("validate authenticated session start"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("record authenticated session start", 768,
        [request](sqlite3* database, auto& output) {
            SqliteStatement existing(database,
                "SELECT account_id,node_id,started_utc_seconds,correlation_id FROM sfx_account_sessions WHERE session_id=?1",
                "check session-start idempotency");
            existing.bind(1, request.session_id);
            if (existing.row()) {
                if (existing.int64(0) == request.account_id && existing.int64(1) == request.node_id &&
                    existing.int64(2) == request.started_utc_seconds &&
                    existing.identifier(3) == request.correlation_id && exact_journal_start(database, request)) {
                    output.account_id = request.account_id;
                    output.replayed = true;
                    return SQLITE_OK;
                }
                return SQLITE_ABORT;
            }
            SqliteStatement active(database,
                "SELECT count(*) FROM sfx_accounts WHERE account_id=?1 AND account_state='active'",
                "require active account for session start");
            active.bind(1, request.account_id);
            if (!active.row() || active.int64(0) != 1) return SQLITE_ABORT;
            SqliteStatement session(database,
                "INSERT INTO sfx_account_sessions(session_id,account_id,node_id,started_utc_seconds,persisted_seconds,"
                "last_persisted_utc_seconds,correlation_id) VALUES(?1,?2,?3,?4,0,?4,?5)",
                "insert authenticated session start");
            session.bind(1, request.session_id);
            session.bind(2, request.account_id);
            session.bind(3, request.node_id);
            session.bind(4, request.started_utc_seconds);
            session.bind(5, request.correlation_id);
            session.step();
            record_chat_usage_policy(database, request);
            allocate_caller_number(database, request);
            SqliteStatement statistics(database,
                "UPDATE sfx_account_statistics SET successful_login_count=successful_login_count+1,"
                "last_successful_login_utc_seconds=?1 WHERE account_id=?2 AND successful_login_count<9223372036854775807",
                "record successful authenticated login");
            statistics.bind(1, request.started_utc_seconds);
            statistics.bind(2, request.account_id);
            statistics.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CONSTRAINT;
            output.account_id = request.account_id;
            return SQLITE_OK;
        });
}

StorageStatus SqliteAccountRepository::persist_session_progress(
    const application::PersistSessionProgressRequest& request) {
    if(!valid_daily_seconds(request.daily_seconds,request.total_authenticated_seconds))
        return invalid("validate cumulative daily session total");
    if (!request.session_id.valid() || request.total_authenticated_seconds < 0 ||
        !application::valid_utc_seconds(request.persisted_utc_seconds)) {
        return invalid("validate session-progress persistence");
    }
    return impl_->write("persist bounded session progress", 384+request.daily_seconds.size()*64,
        [request](sqlite3* database) {
            SqliteStatement row(database,
                "SELECT account_id,persisted_seconds,last_persisted_utc_seconds,completed_utc_seconds "
                "FROM sfx_account_sessions WHERE session_id=?1", "load session progress");
            row.bind(1, request.session_id);
            if (!row.row()) return SQLITE_NOTFOUND;
            const AccountId account_id = row.int64(0);
            const std::int64_t persisted = row.int64(1);
            const std::int64_t last_utc = row.int64(2);
            const auto daily_saved=persist_daily_seconds(database,request.session_id,request.daily_seconds);
            if(daily_saved!=SQLITE_OK) return daily_saved;
            if (!row.is_null(3)) return request.total_authenticated_seconds == persisted ? SQLITE_OK : SQLITE_ABORT;
            if (request.total_authenticated_seconds < persisted) return SQLITE_ABORT;
            if (request.total_authenticated_seconds == persisted) return SQLITE_OK;
            if (request.daily_seconds.empty() && request.persisted_utc_seconds < last_utc + 60) return SQLITE_NOTICE;
            const std::int64_t delta = request.total_authenticated_seconds - persisted;
            SqliteStatement session(database,
                "UPDATE sfx_account_sessions SET persisted_seconds=?1,last_persisted_utc_seconds=?2 "
                "WHERE session_id=?3 AND completed_utc_seconds IS NULL AND persisted_seconds=?4",
                "persist session delta");
            session.bind(1, request.total_authenticated_seconds);
            session.bind(2, std::max(last_utc,request.persisted_utc_seconds));
            session.bind(3, request.session_id);
            session.bind(4, persisted);
            session.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement statistics(database,
                "UPDATE sfx_account_statistics SET authenticated_session_seconds=authenticated_session_seconds+?1 "
                "WHERE account_id=?2 AND authenticated_session_seconds<=9223372036854775807-?1",
                "accumulate persisted authenticated seconds");
            statistics.bind(1, delta);
            statistics.bind(2, account_id);
            statistics.step();
            return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
        });
}

MutationResult SqliteAccountRepository::finalize_session(
    const application::FinalizeSessionRequest& request) {
    if (!valid_daily_seconds(request.daily_seconds,request.total_authenticated_seconds) || !request.session_id.valid() || !request.audit_event_id.valid() ||
        request.total_authenticated_seconds < 0 || !application::valid_utc_seconds(request.completed_utc_seconds)) {
        return {invalid("validate session finalization"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("finalize authenticated session", 768+request.daily_seconds.size()*64,
        [request](sqlite3* database, auto& output) {
            SqliteStatement row(database,
                "SELECT account_id,persisted_seconds,completed_utc_seconds,outcome_code,correlation_id,started_utc_seconds "
                "FROM sfx_account_sessions WHERE session_id=?1", "load session for finalization");
            row.bind(1, request.session_id);
            if (!row.row()) return SQLITE_NOTFOUND;
            const AccountId account_id = row.int64(0);
            const std::int64_t persisted = row.int64(1);
            const Identifier128 correlation = row.identifier(4);
            const auto daily_saved=persist_daily_seconds(database,request.session_id,request.daily_seconds);
            if(daily_saved!=SQLITE_OK) return daily_saved;
            if (!row.is_null(2)) {
                if (row.int64(2) == request.completed_utc_seconds &&
                    row.text(3) == application::storage_code(request.outcome) &&
                    persisted == request.total_authenticated_seconds) {
                    output.account_id = account_id;
                    output.replayed = true;
                    return SQLITE_OK;
                }
                return SQLITE_ABORT;
            }
            if (request.total_authenticated_seconds < persisted || request.completed_utc_seconds < row.int64(5))
                return SQLITE_ABORT;
            const std::int64_t delta = request.total_authenticated_seconds - persisted;
            SqliteStatement session(database,
                "UPDATE sfx_account_sessions SET persisted_seconds=?1,last_persisted_utc_seconds=?2,"
                "completed_utc_seconds=?2,outcome_code=?3 WHERE session_id=?4 AND completed_utc_seconds IS NULL",
                "complete authenticated session");
            session.bind(1, request.total_authenticated_seconds);
            session.bind(2, request.completed_utc_seconds);
            session.bind(3, application::storage_code(request.outcome));
            session.bind(4, request.session_id);
            session.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement statistics(database,
                "UPDATE sfx_account_statistics SET authenticated_session_seconds=authenticated_session_seconds+?1,"
                "last_completed_session_utc_seconds=?2,last_session_outcome_code=?3 "
                "WHERE account_id=?4 AND authenticated_session_seconds<=9223372036854775807-?1",
                "finalize account session statistics");
            statistics.bind(1, delta);
            statistics.bind(2, request.completed_utc_seconds);
            statistics.bind(3, application::storage_code(request.outcome));
            statistics.bind(4, account_id);
            statistics.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CONSTRAINT;
            AuditContext audit;
            audit.event_id = request.audit_event_id;
            audit.correlation_id = correlation;
            audit.occurred_utc_seconds = request.completed_utc_seconds;
            audit.reason = AuditReasonCode::session_finalization;
            insert_audit(database, audit, AuditEventKind::session_completed, account_id);
            output.account_id = account_id;
            return SQLITE_OK;
        });
}

application::StorageResult<AccountRecord> SqliteAccountRepository::find_account_by_name(
    const std::string_view candidate) {
    const auto normalized = application::normalize_account_name(candidate);
    if (!normalized.has_value()) return read_failure<AccountRecord>(invalid("validate account-name lookup"));
    const std::string key = *application::account_name_comparison_key(*normalized);
    return impl_->readers_.read<AccountRecord>("find account by normalized identity",
        [key](sqlite3* database) {
            const std::string sql = std::string(account_select) + "WHERE a.name_key=?1";
            SqliteStatement statement(database, sql.c_str(), "find account by normalized identity");
            statement.bind(1, key);
            if (!statement.row()) throw StorageFailure(StorageErrorCode::not_found,
                "find account by normalized identity", SQLITE_NOTFOUND);
            return read_account(statement);
        });
}

application::StorageResult<std::vector<application::CallerJournalRecord>>
SqliteAccountRepository::pending_caller_journals(const bool unfinished, const std::size_t limit) {
    using Records = std::vector<application::CallerJournalRecord>;
    if (!limit || limit > 16) return read_failure<Records>(invalid("bound caller journal page"));
    return impl_->readers_.read<Records>("read pending caller journals", [unfinished, limit](sqlite3* database) {
        SqliteStatement row(database,
            "SELECT j.caller_number,s.session_id,s.account_id,s.node_id,s.started_utc_seconds,s.persisted_seconds,"
            "s.last_persisted_utc_seconds,s.completed_utc_seconds,s.outcome_code,s.correlation_id,"
            "j.display_name,j.transport,j.source_ip,j.hostname_hint,j.ansi,j.activities,j.recovered "
            "FROM sfx_caller_journals j JOIN sfx_account_sessions s USING(session_id) "
            "WHERE j.published=0 AND (s.completed_utc_seconds IS NULL)=?1 ORDER BY j.caller_number LIMIT ?2",
            "page pending caller journals");
        row.bind(1, static_cast<std::int64_t>(unfinished)); row.bind(2, static_cast<std::int64_t>(limit));
        Records result;
        while (row.row()) {
            application::CallerJournalRecord record;
            record.caller_number = row.int64(0); auto& s = record.session;
            s.session_id = row.identifier(1); s.account_id = row.int64(2);
            s.node_id = static_cast<std::uint16_t>(row.int64(3)); s.started_utc_seconds = row.int64(4);
            s.persisted_seconds = row.int64(5); s.last_persisted_utc_seconds = row.int64(6);
            if (!row.is_null(7)) s.completed_utc_seconds = row.int64(7);
            if (!row.is_null(8)) s.outcome = session_outcome(row.text(8));
            s.correlation_id = row.identifier(9); record.display_name = row.text(10);
            record.transport = row.text(11); record.source_ip = row.text(12); record.hostname_hint = row.text(13);
            record.ansi = static_cast<int>(row.int64(14)); record.recovered = row.int64(16) != 0;
            const auto activities = application::decode_caller_activities(row.text(15));
            if (!activities) throw StorageFailure(StorageErrorCode::corrupt, "decode bounded caller activities", SQLITE_CORRUPT);
            record.activities = *activities;
            result.push_back(std::move(record));
        }
        return result;
    });
}

StorageStatus SqliteAccountRepository::append_caller_activity(const Identifier128 session,
    const application::CallerActivity& activity, const std::string_view hostname_hint) {
    const auto hostname = application::normalize_hostname_hint(hostname_hint);
    if (!session.valid() || !application::valid_caller_activity(activity) ||
        (!hostname_hint.empty() && (!hostname || hostname->view() != hostname_hint)))
        return invalid("validate bounded caller activity");
    return impl_->write("retain typed caller activity", 8192,
        [session, activity, hint = std::string(hostname_hint)](sqlite3* database) {
            SqliteStatement row(database,
                "SELECT j.activities,s.started_utc_seconds,s.completed_utc_seconds FROM sfx_caller_journals j "
                "JOIN sfx_account_sessions s USING(session_id) WHERE j.session_id=?1 AND j.published=0",
                "load exact session activity state");
            row.bind(1, session);
            if (!row.row()) return SQLITE_NOTFOUND;
            if (!row.is_null(2) || activity.utc_seconds < row.int64(1)) return SQLITE_ABORT;
            auto activities = application::decode_caller_activities(row.text(0));
            if (!activities) return SQLITE_CORRUPT;
            // Duplicate delivery of an identical typed transition is idempotent.
            if (std::any_of(activities->begin(), activities->end(), [&](const auto& a) {
                return a.kind == activity.kind && a.utc_seconds == activity.utc_seconds &&
                    a.configured_label == activity.configured_label;
            })) return SQLITE_OK;
            if (activities->size() == application::kCallerJournalActivityLimit) return SQLITE_FULL;
            activities->push_back(activity);
            const auto encoded = application::encode_caller_activities(*activities);
            if (!encoded) return SQLITE_MISMATCH;
            SqliteStatement update(database,
                "UPDATE sfx_caller_journals SET activities=?1,hostname_hint=CASE WHEN ?2='' THEN hostname_hint ELSE ?2 END "
                "WHERE session_id=?3 AND published=0", "retain bounded typed transition");
            update.bind(1, *encoded); update.bind(2, hint); update.bind(3, session); update.step();
            return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_ABORT;
        });
}

StorageStatus SqliteAccountRepository::mark_caller_recovered(const Identifier128 session) {
    if (!session.valid()) return invalid("validate recovered caller identity");
    return impl_->write("mark interrupted caller journal", 256, [session](sqlite3* database) {
        SqliteStatement update(database,
            "UPDATE sfx_caller_journals SET recovered=1 WHERE session_id=?1 AND published=0 "
            "AND EXISTS(SELECT 1 FROM sfx_account_sessions s WHERE s.session_id=?1 AND s.completed_utc_seconds IS NULL)",
            "mark exact interrupted caller");
        update.bind(1, session); update.step();
        return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_ABORT;
    });
}

application::StorageResult<std::optional<application::CallerJournalPublication>>
SqliteAccountRepository::caller_publication() {
    return impl_->readers_.read<std::optional<application::CallerJournalPublication>>(
        "read caller publication intent", [](sqlite3* database) -> std::optional<application::CallerJournalPublication> {
            SqliteStatement row(database,
                "SELECT caller_number,file_identity,byte_offset,block FROM sfx_caller_journal_publication WHERE singleton_id=1",
                "read serialized publication intent");
            if (!row.row()) return {};
            return application::CallerJournalPublication{row.int64(0), row.text(1),
                static_cast<std::uint64_t>(row.int64(2)), row.text(3)};
        });
}

StorageStatus SqliteAccountRepository::prepare_caller_publication(const application::CallerJournalPublication& value) {
    if (value.caller_number < 1 || value.file_identity.empty() || value.file_identity.size() > 128 ||
        value.byte_offset > application::kDurableJournalMaximumFileBytes || value.block.empty() ||
        value.block.size() > application::kCallerJournalRecordBytes ||
        value.block.size() > application::kDurableJournalMaximumFileBytes - value.byte_offset)
        return invalid("bound caller publication intent");
    for (const unsigned char c : value.file_identity) if (!((c >= '0' && c <= '9') || c == ':'))
        return invalid("validate journal file identity token");
    for (std::size_t i = 0; i < value.block.size(); ++i) {
        const unsigned char c = value.block[i];
        if ((c == '\r' && (i + 1 == value.block.size() || value.block[i + 1] != '\n')) ||
            (c == '\n' && (!i || value.block[i - 1] != '\r')) ||
            (c != '\r' && c != '\n' && (c < 32 || c > 126))) return invalid("validate caller publication text");
    }
    return impl_->write("prepare recoverable caller append", value.block.size() + 512,
        [value](sqlite3* database) {
            SqliteStatement existing(database,
                "SELECT caller_number,file_identity,byte_offset,block FROM sfx_caller_journal_publication WHERE singleton_id=1",
                "check publication intent replay");
            if (existing.row()) return existing.int64(0) == value.caller_number && existing.text(1) == value.file_identity &&
                existing.int64(2) == static_cast<std::int64_t>(value.byte_offset) && existing.text(3) == value.block ? SQLITE_OK : SQLITE_ABORT;
            SqliteStatement insert(database,
                "INSERT INTO sfx_caller_journal_publication(singleton_id,caller_number,file_identity,byte_offset,block) "
                "SELECT 1,?1,?2,?3,?4 FROM sfx_caller_journals j JOIN sfx_account_sessions s USING(session_id) "
                "WHERE j.caller_number=?1 AND j.published=0 AND s.completed_utc_seconds IS NOT NULL",
                "prepare exact finalized caller append");
            insert.bind(1, value.caller_number); insert.bind(2, value.file_identity);
            insert.bind(3, static_cast<std::int64_t>(value.byte_offset)); insert.bind(4, value.block); insert.step();
            return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_ABORT;
        });
}

StorageStatus SqliteAccountRepository::acknowledge_caller_publication(const std::int64_t number) {
    if (number < 1) return invalid("validate caller publication acknowledgement");
    return impl_->write("acknowledge complete caller block", 512, [number](sqlite3* database) {
        SqliteStatement update(database,
            "UPDATE sfx_caller_journals SET published=1 WHERE caller_number=?1 AND "
            "EXISTS(SELECT 1 FROM sfx_caller_journal_publication WHERE singleton_id=1 AND caller_number=?1)",
            "acknowledge exact caller publication");
        update.bind(1, number); update.step();
        if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
        SqliteStatement retire(database,
            "DELETE FROM sfx_caller_journal_publication WHERE singleton_id=1 AND caller_number=?1",
            "retire acknowledged caller intent");
        retire.bind(1, number); retire.step();
        return sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_ABORT;
    });
}

application::StorageResult<application::CallerNameResolution>
SqliteAccountRepository::classify_caller_name(const std::string_view submitted_name) {
    const auto normalized = application::normalize_account_name(submitted_name);
    if (!normalized.has_value()) {
        return read_failure<application::CallerNameResolution>(
            invalid("validate caller-name classification"));
    }
    const auto key = application::account_name_comparison_key(*normalized);
    if (!key.has_value()) {
        return read_failure<application::CallerNameResolution>(
            invalid("derive caller-name comparison key"));
    }
    return impl_->readers_.read<application::CallerNameResolution>(
        "classify authoritative caller name",
        [normalized = *normalized, key = *key](sqlite3* database) {
            SqliteStatement resolution(database,
                "SELECT a.account_id,a.display_name,a.name_key,a.security_level,"
                "(SELECT count(*) FROM sfx_account_name_reservations r "
                " WHERE r.name_key=?1),"
                "(SELECT count(*) FROM sfx_account_name_reservations r "
                " WHERE r.name_key=?1 AND r.account_id=a.account_id "
                " AND r.is_current=1) "
                "FROM (SELECT 1) seed LEFT JOIN sfx_accounts a ON a.name_key=?1",
                "resolve authoritative caller identity and reservation");
            resolution.bind(1, key);
            if (!resolution.row() || resolution.int64(4) < 0 ||
                resolution.int64(4) > 1 || resolution.int64(5) < 0 ||
                resolution.int64(5) > 1) {
                throw StorageFailure(StorageErrorCode::corrupt,
                    "validate permanent caller-name reservation", SQLITE_CORRUPT);
            }
            if (!resolution.is_null(0)) {
                if (resolution.is_null(1) || resolution.is_null(2) ||
                    resolution.is_null(3) || resolution.text(2) != key ||
                    resolution.int64(0) <= 0 ||
                    resolution.int64(3) < core::kMinimumSecurityLevel ||
                    resolution.int64(3) > core::kMaximumSecurityLevel ||
                    resolution.int64(4) != 1 || resolution.int64(5) != 1) {
                    throw StorageFailure(StorageErrorCode::corrupt,
                        "validate classified account identity", SQLITE_CORRUPT);
                }
                application::CallerNameResolution existing;
                existing.classification = application::CallerNameClassification::existing;
                existing.normalized_name = normalized;
                existing.account_id = resolution.int64(0);
                existing.saved_display_name = resolution.text(1);
                existing.security_level =
                    static_cast<std::uint16_t>(resolution.int64(3));
                if (!existing.valid()) {
                    throw StorageFailure(StorageErrorCode::corrupt,
                        "validate caller-name resolution", SQLITE_CORRUPT);
                }
                return existing;
            }
            application::CallerNameResolution result;
            result.classification = resolution.int64(4) == 0
                ? application::CallerNameClassification::registerable
                : application::CallerNameClassification::unavailable;
            result.normalized_name = normalized;
            return result;
        });
}

MutationResult SqliteAccountRepository::commit_authenticated_session(
    const application::AuthenticatedSessionCommitRequest& request) {
    if (!application::valid_authenticated_session_commit_request(request) || !valid_journal_start(request.session)) {
        return {invalid("validate authenticated-session commit"),
            std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("commit authenticated session", 2048U,
        [request](sqlite3* database, auto& output) {
            SqliteStatement existing_session(database,
                "SELECT account_id,node_id,started_utc_seconds,correlation_id "
                "FROM sfx_account_sessions WHERE session_id=?1",
                "check authenticated-session replay");
            existing_session.bind(1, request.session.session_id);
            const bool session_exists = existing_session.row();

            SqliteStatement existing_source(database,
                "SELECT account_id,source_ip,protocol_code,succeeded_utc_seconds "
                "FROM sfx_successful_login_ip_history WHERE event_id=?1",
                "check authenticated-source replay");
            existing_source.bind(1, request.successful_source.event_id);
            const bool source_exists = existing_source.row();

            if (session_exists != source_exists) return SQLITE_CORRUPT;
            if (session_exists) {
                const bool exact_session =
                    existing_session.int64(0) == request.session.account_id &&
                    existing_session.int64(1) == request.session.node_id &&
                    existing_session.int64(2) == request.session.started_utc_seconds &&
                    existing_session.identifier(3) == request.session.correlation_id;
                const bool exact_source =
                    existing_source.int64(0) == request.successful_source.account_id &&
                    existing_source.text(1) == request.successful_source.source_ip &&
                    existing_source.text(2) ==
                        application::storage_code(request.successful_source.protocol) &&
                    existing_source.int64(3) ==
                        request.successful_source.succeeded_utc_seconds;
                if (!exact_session || !exact_source || !exact_journal_start(database, request.session)) return SQLITE_ABORT;
            }

            SqliteStatement account(database,
                "SELECT count(*) FROM sfx_accounts WHERE account_id=?1 "
                "AND revision=?2 AND name_key=?3 AND security_level=?4 "
                "AND account_state='active'",
                "revalidate authenticated account snapshot");
            account.bind(1, request.session.account_id);
            account.bind(2, request.expected_account_revision);
            account.bind(3, request.expected_account_name_key);
            account.bind(4, request.expected_security_level);
            if (!account.row() || account.int64(0) != 1) return SQLITE_ABORT;

            SqliteStatement policy(database,
                "SELECT count(*) FROM sfx_security_policy WHERE singleton_id=1 "
                "AND revision=?1 AND policy_hash=?2 AND published_revision=?1 "
                "AND published_policy_hash=?2",
                "revalidate authenticated security policy");
            policy.bind(1, request.expected_security_policy_revision);
            policy.bind(2, request.expected_security_policy_hash);
            if (!policy.row() || policy.int64(0) != 1) return SQLITE_ABORT;

            if (session_exists) {
                output.account_id = request.session.account_id;
                output.revision = request.expected_account_revision;
                output.replayed = true;
                return SQLITE_OK;
            }

            SqliteStatement session(database,
                "INSERT INTO sfx_account_sessions(session_id,account_id,node_id,"
                "started_utc_seconds,persisted_seconds,last_persisted_utc_seconds,"
                "correlation_id) VALUES(?1,?2,?3,?4,0,?4,?5)",
                "insert atomic authenticated session");
            session.bind(1, request.session.session_id);
            session.bind(2, request.session.account_id);
            session.bind(3, request.session.node_id);
            session.bind(4, request.session.started_utc_seconds);
            session.bind(5, request.session.correlation_id);
            session.step();
            record_chat_usage_policy(database, request.session);
            allocate_caller_number(database, request.session);

            SqliteStatement statistics(database,
                "UPDATE sfx_account_statistics SET "
                "successful_login_count=successful_login_count+1,"
                "last_successful_login_utc_seconds=?1 WHERE account_id=?2 "
                "AND successful_login_count<9223372036854775807",
                "record atomic successful authenticated login");
            statistics.bind(1, request.session.started_utc_seconds);
            statistics.bind(2, request.session.account_id);
            statistics.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CONSTRAINT;

            SqliteStatement source(database,
                "INSERT INTO sfx_successful_login_ip_history(event_id,account_id,"
                "source_ip,protocol_code,succeeded_utc_seconds) "
                "VALUES(?1,?2,?3,?4,?5)",
                "insert atomic successful-source history");
            source.bind(1, request.successful_source.event_id);
            source.bind(2, request.successful_source.account_id);
            source.bind(3, request.successful_source.source_ip);
            source.bind(4, application::storage_code(request.successful_source.protocol));
            source.bind(5, request.successful_source.succeeded_utc_seconds);
            source.step();

            SqliteStatement prune(database,
                "DELETE FROM sfx_successful_login_ip_history WHERE account_id=?1 "
                "AND event_id IN (SELECT event_id FROM "
                "sfx_successful_login_ip_history WHERE account_id=?1 "
                "ORDER BY succeeded_utc_seconds DESC,event_id DESC LIMIT -1 OFFSET 5)",
                "retain newest five atomic successful-source records");
            prune.bind(1, request.successful_source.account_id);
            prune.step();
            output.account_id = request.session.account_id;
            output.revision = request.expected_account_revision;
            return SQLITE_OK;
        });
}

application::StorageResult<std::string> SqliteAccountRepository::door_user_key(AccountId account_id) {
    return impl_->readers_.read<std::string>("read stable Door caller identity",[account_id](sqlite3* db) {
        // The account creation event is committed with the account. Its unique
        // random ID survives renames, tombstones and restoration of this account.
        SqliteStatement query(db,"SELECT lower(hex(event_id)) FROM sfx_account_audit "
            "JOIN sfx_accounts ON account_id=subject_account_id "
            "WHERE subject_account_id=?1 AND event_kind='account.created' AND account_state='active' LIMIT 2",
            "read account creation identity");
        query.bind(1,account_id);
        if (!query.row()) throw StorageFailure(StorageErrorCode::not_found,"door_account_identity_unavailable");
        auto key=query.text(0);
        if (key.size()!=32 || key==std::string(32,'0') || query.row())
            throw StorageFailure(StorageErrorCode::corrupt,"door_account_identity_invalid");
        return "sfx-"+key;
    });
}

application::StorageResult<AccountRecord> SqliteAccountRepository::find_account_by_id(
    const AccountId account_id) {
    if (account_id <= 0) return read_failure<AccountRecord>(invalid("validate account-ID lookup"));
    return impl_->readers_.read<AccountRecord>("find account by permanent ID",
        [account_id](sqlite3* database) {
            const std::string sql = std::string(account_select) + "WHERE a.account_id=?1";
            SqliteStatement statement(database, sql.c_str(), "find account by permanent ID");
            statement.bind(1, account_id);
            if (!statement.row()) throw StorageFailure(StorageErrorCode::not_found,
                "find account by permanent ID", SQLITE_NOTFOUND);
            return read_account(statement);
        });
}

application::StorageResult<CredentialEnvelope> SqliteAccountRepository::load_credential(
    const AccountId account_id) {
    if (account_id <= 0) return read_failure<CredentialEnvelope>(invalid("validate credential lookup"));
    return impl_->readers_.read<CredentialEnvelope>("load opaque credential envelope",
        [account_id](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT envelope_format_version,scheme_identifier,parameter_encoding_version,canonical_parameters,"
                "salt,verifier,pepper_key_identifier,created_utc_seconds,password_change_required "
                "FROM sfx_account_credentials WHERE account_id=?1",
                "load opaque credential envelope");
            statement.bind(1, account_id);
            if (!statement.row()) throw StorageFailure(StorageErrorCode::not_found,
                "load opaque credential envelope", SQLITE_NOTFOUND);
            CredentialEnvelope credential;
            credential.envelope_format_version = static_cast<std::uint16_t>(statement.int64(0));
            credential.scheme_identifier = statement.text(1);
            credential.parameter_encoding_version = static_cast<std::uint16_t>(statement.int64(2));
            credential.canonical_parameters = statement.blob(3);
            credential.salt = statement.blob(4);
            credential.verifier = statement.blob(5);
            if (!statement.is_null(6)) credential.pepper_key_identifier = statement.text(6);
            credential.created_utc_seconds = statement.int64(7);
            credential.password_change_required = statement.boolean(8);
            return credential;
        });
}

application::StorageResult<std::vector<AccountRecord>> SqliteAccountRepository::list_accounts(
    const application::AccountListQuery& query) {
    if (query.limit == 0 || query.limit > 256 || query.offset > 1'000'000 ||
        (query.security_level.has_value() && (*query.security_level < 1 || *query.security_level > 999))) {
        return read_failure<std::vector<AccountRecord>>(invalid("validate bounded account listing"));
    }
    std::optional<std::string> prefix;
    if (query.name_prefix.has_value()) {
        const auto normalized = application::normalize_account_name(*query.name_prefix);
        if (!normalized.has_value()) return read_failure<std::vector<AccountRecord>>(
            invalid("validate account-list name prefix"));
        prefix = *application::account_name_comparison_key(*normalized);
    }
    return impl_->readers_.read<std::vector<AccountRecord>>("list bounded accounts",
        [query, prefix](sqlite3* database) {
            std::string sql = account_select;
            std::vector<std::string> clauses;
            if (query.exclude_tombstoned) clauses.emplace_back("a.account_state<>'tombstoned'");
            if (prefix.has_value()) clauses.emplace_back("a.name_key>=?1 AND a.name_key<?2");
            if (query.state.has_value()) clauses.emplace_back(
                std::string("a.account_state=?") + std::to_string(prefix.has_value() ? 3 : 1));
            if (query.security_level.has_value()) clauses.emplace_back(
                std::string("a.security_level=?") + std::to_string((prefix.has_value() ? 2 : 0) +
                    (query.state.has_value() ? 2 : 1)));
            if (!clauses.empty()) {
                sql += "WHERE ";
                for (std::size_t index = 0; index < clauses.size(); ++index) {
                    if (index != 0) sql += " AND ";
                    sql += clauses[index];
                }
                sql += ' ';
            }
            sql += "ORDER BY a.name_key,a.account_id LIMIT ?";
            const int limit_index = (prefix.has_value() ? 2 : 0) +
                (query.state.has_value() ? 1 : 0) + (query.security_level.has_value() ? 1 : 0) + 1;
            sql += std::to_string(limit_index) + " OFFSET ?" + std::to_string(limit_index + 1);
            SqliteStatement statement(database, sql.c_str(), "list bounded accounts");
            int bind = 1;
            if (prefix.has_value()) {
                statement.bind(bind++, *prefix);
                statement.bind(bind++, *prefix + std::string(1U, static_cast<char>(0x7f)));
            }
            if (query.state.has_value()) statement.bind(bind++, application::storage_code(*query.state));
            if (query.security_level.has_value()) statement.bind(bind++, *query.security_level);
            statement.bind(bind++, static_cast<std::int64_t>(query.limit));
            statement.bind(bind, static_cast<std::int64_t>(query.offset));
            std::vector<AccountRecord> records;
            while (statement.row()) records.push_back(read_account(statement));
            return records;
        });
}

application::StorageResult<std::vector<AccountRecord>> SqliteAccountRepository::inactivity_candidates(
    const application::InactivityCandidateQuery& query) {
    if (!application::valid_utc_seconds(query.last_login_at_or_before_utc_seconds) ||
        query.limit == 0 || query.limit > 256) {
        return read_failure<std::vector<AccountRecord>>(invalid("validate inactivity-candidate query"));
    }
    return impl_->readers_.read<std::vector<AccountRecord>>("select inactivity-deletion candidates",
        [query](sqlite3* database) {
            std::string sql = std::string(account_select) +
                "JOIN sfx_security_policy policy ON policy.singleton_id=1 ";
            if (query.kind == application::InactivitySelectionKind::schedule_eligible) {
                sql += "WHERE a.account_state='active' AND a.purge_allowed=1 AND a.security_level<policy.sysop_threshold "
                    "AND s.last_successful_login_utc_seconds IS NOT NULL AND s.last_successful_login_utc_seconds<=?1 "
                    "ORDER BY s.last_successful_login_utc_seconds,a.account_id LIMIT ?2";
            } else {
                sql += "WHERE a.account_state='deletion_scheduled' AND a.purge_allowed=1 "
                    "AND a.security_level<policy.sysop_threshold AND a.deletion_scheduled_utc_seconds<=?1 "
                    "ORDER BY a.deletion_scheduled_utc_seconds,a.account_id LIMIT ?2";
            }
            SqliteStatement statement(database, sql.c_str(), "select inactivity-deletion candidates");
            statement.bind(1, query.last_login_at_or_before_utc_seconds);
            statement.bind(2, static_cast<std::int64_t>(query.limit));
            std::vector<AccountRecord> records;
            while (statement.row()) records.push_back(read_account(statement));
            return records;
        });
}

application::StorageResult<std::vector<application::SessionRecord>>
SqliteAccountRepository::session_history(const application::HistoryQuery& query) {
    if (query.account_id <= 0 || query.offset > 1'000'000 || !application::valid_utc_seconds(query.at_or_before_utc_seconds) ||
        query.limit == 0 || query.limit > 256 || (query.after_session &&
            (!application::valid_utc_seconds(query.after_session->first) || !query.after_session->second.valid() || query.offset))) {
        return read_failure<std::vector<application::SessionRecord>>(invalid("validate session-history query"));
    }
    return impl_->readers_.read<std::vector<application::SessionRecord>>("read account session history",
        [query](sqlite3* database) {
            std::string sql=
                "SELECT session_id,account_id,node_id,started_utc_seconds,persisted_seconds,last_persisted_utc_seconds,"
                "completed_utc_seconds,outcome_code,correlation_id FROM sfx_account_sessions "
                "WHERE account_id=?1 AND started_utc_seconds<=?2 ";
            if(query.after_session) sql+="AND (started_utc_seconds<?5 OR (started_utc_seconds=?5 AND session_id>?6)) ";
            sql+="ORDER BY started_utc_seconds DESC,session_id LIMIT ?3 OFFSET ?4";
            SqliteStatement statement(database,sql.c_str(),"read account session history");
            statement.bind(1, query.account_id);
            statement.bind(2, query.at_or_before_utc_seconds);
            statement.bind(3, static_cast<std::int64_t>(query.limit));
            statement.bind(4, static_cast<std::int64_t>(query.offset));
            if(query.after_session) {statement.bind(5,query.after_session->first);statement.bind(6,query.after_session->second);}
            std::vector<application::SessionRecord> records;
            while (statement.row()) {
                application::SessionRecord record;
                record.session_id = statement.identifier(0);
                record.account_id = statement.int64(1);
                record.node_id = static_cast<std::uint16_t>(statement.int64(2));
                record.started_utc_seconds = statement.int64(3);
                record.persisted_seconds = statement.int64(4);
                record.last_persisted_utc_seconds = statement.int64(5);
                if (!statement.is_null(6)) record.completed_utc_seconds = statement.int64(6);
                if (!statement.is_null(7)) {
                    const auto outcome = session_outcome(statement.text(7));
                    if (!outcome.has_value()) throw StorageFailure(StorageErrorCode::corrupt,
                        "read session outcome", SQLITE_CORRUPT);
                    record.outcome = *outcome;
                }
                record.correlation_id = statement.identifier(8);
                records.push_back(std::move(record));
            }
            return records;
        });
}

application::StorageResult<std::vector<application::CallerDirectoryEntry>>
SqliteAccountRepository::caller_directory(const application::CallerDirectoryQuery& query) {
    using Entry=application::CallerDirectoryEntry;
    if(!query.limit || query.limit>256 || query.offset>1'000'000 ||
        (query.logon_utc_range && (!application::valid_utc_seconds(query.logon_utc_range->first) ||
            !application::valid_utc_seconds(query.logon_utc_range->second) ||
            query.logon_utc_range->first>=query.logon_utc_range->second)))
        return read_failure<std::vector<Entry>>(invalid("validate public caller directory"));
    std::string fragment;
    if(query.name_fragment) {
        const auto normalized=application::normalize_account_name(*query.name_fragment);
        if(!normalized) return read_failure<std::vector<Entry>>(invalid("validate caller name search"));
        fragment=*application::account_name_comparison_key(*normalized);
    }
    return impl_->readers_.read<std::vector<Entry>>("read public caller directory",[query,fragment](sqlite3* database) {
        const bool day=query.logon_utc_range.has_value();
        const std::string sql=std::string("SELECT a.display_name,p.location,")+
            (day?"max(c.started_utc_seconds)":"s.last_successful_login_utc_seconds")+
            " FROM sfx_accounts a LEFT JOIN sfx_account_profiles p ON p.account_id=a.account_id "
            "LEFT JOIN sfx_account_statistics s ON s.account_id=a.account_id "+
            (day?"JOIN sfx_account_sessions c ON c.account_id=a.account_id AND c.started_utc_seconds>=?4 AND c.started_utc_seconds<?5 ":"")+
            "WHERE a.account_state='active' AND instr(a.name_key,?1)>0 "+
            (day?"GROUP BY a.account_id ":"")+"ORDER BY a.name_key,a.account_id LIMIT ?2 OFFSET ?3";
        SqliteStatement statement(database,sql.c_str(),"read public caller directory");
        statement.bind(1,fragment);statement.bind(2,static_cast<std::int64_t>(query.limit));
        statement.bind(3,static_cast<std::int64_t>(query.offset));
        if(day) {statement.bind(4,query.logon_utc_range->first);statement.bind(5,query.logon_utc_range->second);}
        std::vector<Entry> rows;
        while(statement.row()) {
            Entry row;row.name=statement.text(0);
            if(!statement.is_null(1)) row.location=statement.text(1);
            if(!statement.is_null(2)) row.last_logon_utc=statement.int64(2);
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

application::StorageResult<std::vector<application::AuditRecord>>
SqliteAccountRepository::audit_history(const application::AuditHistoryQuery& query) {
    if ((query.subject_account_id.has_value() && *query.subject_account_id <= 0) ||
        !application::valid_utc_seconds(query.at_or_before_utc_seconds) ||
        query.limit == 0 || query.limit > 256) {
        return read_failure<std::vector<application::AuditRecord>>(invalid("validate audit-history query"));
    }
    return impl_->readers_.read<std::vector<application::AuditRecord>>("read bounded audit history",
        [query](sqlite3* database) {
            std::string sql =
                "SELECT event_id,event_kind,result_code,subject_account_id,actor_account_id,occurred_utc_seconds,"
                "correlation_id,reason_code FROM sfx_account_audit WHERE ";
            int next_bind = 1;
            if (query.subject_account_id.has_value()) {
                sql += "subject_account_id=?" + std::to_string(next_bind++) + " AND ";
            }
            if (query.event_kind.has_value()) {
                sql += "event_kind=?" + std::to_string(next_bind++) + " AND ";
            }
            const int cutoff_bind = next_bind++;
            const int limit_bind = next_bind++;
            sql += "occurred_utc_seconds<=?" + std::to_string(cutoff_bind) +
                " ORDER BY occurred_utc_seconds DESC,event_id LIMIT ?" + std::to_string(limit_bind);
            SqliteStatement statement(database, sql.c_str(), "read bounded audit history");
            if (sqlite3_bind_parameter_count(statement.get()) != limit_bind) {
                throw StorageFailure(StorageErrorCode::internal, "validate bounded audit bind plan", SQLITE_RANGE);
            }
            int bind = 1;
            if (query.subject_account_id.has_value()) statement.bind(bind++, *query.subject_account_id);
            if (query.event_kind.has_value()) statement.bind(bind++, application::storage_code(*query.event_kind));
            statement.bind(bind++, query.at_or_before_utc_seconds);
            statement.bind(bind, static_cast<std::int64_t>(query.limit));
            std::vector<application::AuditRecord> records;
            while (statement.row()) {
                application::AuditRecord record;
                record.event_id = statement.identifier(0);
                const auto kind = audit_event_kind(statement.text(1));
                const auto reason = audit_reason(statement.text(7));
                if (!kind.has_value() || !reason.has_value()) throw StorageFailure(StorageErrorCode::corrupt,
                    "read typed audit codes", SQLITE_CORRUPT);
                record.event_kind = *kind;
                if (!statement.is_null(3)) record.subject_account_id = statement.int64(3);
                if (!statement.is_null(4)) record.actor_account_id = statement.int64(4);
                record.occurred_utc_seconds = statement.int64(5);
                record.correlation_id = statement.identifier(6);
                record.reason = *reason;
                records.push_back(std::move(record));
            }
            return records;
        });
}

application::StorageResult<std::int64_t> SqliteAccountRepository::caller_day_seconds(AccountId account,std::string_view date) {
    if(account<=0 || !application::valid_iso_civil_date(date)) return read_failure<std::int64_t>(invalid("validate caller daily usage query"));
    return impl_->readers_.read<std::int64_t>("read durable caller daily usage",[account,date=std::string(date)](sqlite3* database) {
        SqliteStatement total(database,"SELECT coalesce(sum(d.seconds),0) FROM sfx_caller_daily_seconds d "
            "JOIN sfx_account_sessions s ON s.session_id=d.session_id WHERE s.account_id=?1 AND d.local_date=?2","read durable caller daily usage");
        total.bind(1,account);total.bind(2,date);if(!total.row()) throw StorageFailure(StorageErrorCode::corrupt,"read daily usage",SQLITE_CORRUPT);
        return total.int64(0);
    });
}

application::StorageResult<std::vector<SecurityLevelRecord>> SqliteAccountRepository::security_levels() {
    return impl_->readers_.read<std::vector<SecurityLevelRecord>>("list security levels",
        [](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT security_level,label,description,level_state FROM sfx_security_levels ORDER BY security_level",
                "list security levels");
            std::vector<SecurityLevelRecord> records;
            while (statement.row()) {
                SecurityLevelRecord record;
                record.level = static_cast<std::uint16_t>(statement.int64(0));
                record.label = statement.text(1);
                if (!statement.is_null(2)) record.description = statement.text(2);
                const auto state = security_level_state(statement.text(3));
                if (!state.has_value()) throw StorageFailure(StorageErrorCode::corrupt,
                    "read security-level state", SQLITE_CORRUPT);
                record.state = *state;
                if(calling_time_supported(database)) {
                    SqliteStatement limits(database,"SELECT minutes_per_call,minutes_per_day FROM sfx_security_calling_limits WHERE security_level=?1","read independent calling limits");
                    limits.bind(1,record.level);
                    if(limits.row()) {if(!limits.is_null(0)) record.minutes_per_call=static_cast<std::uint32_t>(limits.int64(0));if(!limits.is_null(1)) record.minutes_per_day=static_cast<std::uint32_t>(limits.int64(1));}
                }
                read_private_chat_limits(database,record);
                records.push_back(std::move(record));
            }
            return records;
        });
}

application::StorageResult<SecurityLevelRecord> SqliteAccountRepository::security_level(
    const std::uint16_t level) {
    if (level < 1 || level > 999) {
        return read_failure<SecurityLevelRecord>(invalid("validate security-level lookup"));
    }
    return impl_->readers_.read<SecurityLevelRecord>("look up security level", [level](sqlite3* database) {
        SqliteStatement statement(database,
            "SELECT security_level,label,description,level_state FROM sfx_security_levels WHERE security_level=?1",
            "look up security level");
        statement.bind(1, level);
        if (!statement.row()) throw StorageFailure(StorageErrorCode::not_found,
            "look up security level", SQLITE_NOTFOUND);
        SecurityLevelRecord record;
        record.level = static_cast<std::uint16_t>(statement.int64(0));
        record.label = statement.text(1);
        if (!statement.is_null(2)) record.description = statement.text(2);
        const auto state = security_level_state(statement.text(3));
        if (!state.has_value()) throw StorageFailure(StorageErrorCode::corrupt,
            "read security-level state", SQLITE_CORRUPT);
        record.state = *state;
        if(calling_time_supported(database)) {
            SqliteStatement limits(database,"SELECT minutes_per_call,minutes_per_day FROM sfx_security_calling_limits WHERE security_level=?1","read independent calling limits");
            limits.bind(1,record.level);
            if(limits.row()) {if(!limits.is_null(0)) record.minutes_per_call=static_cast<std::uint32_t>(limits.int64(0));if(!limits.is_null(1)) record.minutes_per_day=static_cast<std::uint32_t>(limits.int64(1));}
        }
        read_private_chat_limits(database,record);
        return record;
    });
}

application::StorageResult<application::RetentionPolicyRecord> SqliteAccountRepository::retention_policy() {
    return impl_->readers_.read<application::RetentionPolicyRecord>("read retention policy",
        [](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT revision,audit_online_months,audit_total_months,session_online_months,session_total_months,modified_utc_seconds "
                "FROM sfx_account_retention_policy WHERE singleton_id=1", "read retention policy");
            if (!statement.row()) throw StorageFailure(StorageErrorCode::corrupt,
                "read retention policy", SQLITE_CORRUPT);
            application::RetentionPolicyRecord record;
            record.revision = statement.int64(0);
            record.audit_online_months = static_cast<std::uint16_t>(statement.int64(1));
            record.audit_total_months = static_cast<std::uint16_t>(statement.int64(2));
            record.session_online_months = static_cast<std::uint16_t>(statement.int64(3));
            record.session_total_months = static_cast<std::uint16_t>(statement.int64(4));
            record.modified_utc_seconds = statement.int64(5);
            if (!application::valid_retention_policy(record)) throw StorageFailure(
                StorageErrorCode::corrupt, "validate retention policy", SQLITE_CORRUPT);
            return record;
        });
}

application::StorageResult<application::SecurityPolicyRecord> SqliteAccountRepository::security_policy() {
    return impl_->readers_.read<application::SecurityPolicyRecord>("read security policy",
        [](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT revision,new_user_level,sysop_threshold,policy_hash,published_revision,published_policy_hash "
                "FROM sfx_security_policy WHERE singleton_id=1", "read security policy");
            if (!statement.row()) throw StorageFailure(StorageErrorCode::corrupt,
                "read security policy", SQLITE_CORRUPT);
            application::SecurityPolicyRecord record;
            record.revision = statement.int64(0);
            record.new_user_level = static_cast<std::uint16_t>(statement.int64(1));
            record.sysop_threshold = static_cast<std::uint16_t>(statement.int64(2));
            record.policy_hash = statement.text(3);
            if (!statement.is_null(4)) record.published_revision = statement.int64(4);
            if (!statement.is_null(5)) record.published_policy_hash = statement.text(5);
            return record;
        });
}

namespace {

void insert_authentication_audit(
    sqlite3* database,
    const application::AuthenticationAuditContext& audit,
    const application::AuthenticationAuditEventKind kind,
    const std::optional<AccountId> subject_account_id) {
    SqliteStatement statement(database,
        "INSERT INTO sfx_authentication_audit(event_id,event_kind,subject_account_id,actor_account_id,"
        "occurred_utc_seconds,correlation_id) VALUES(?1,?2,?3,?4,?5,?6)",
        "insert typed authentication audit");
    statement.bind(1, audit.event_id);
    statement.bind(2, application::storage_code(kind));
    bind_optional_account_id(statement, 3, subject_account_id);
    bind_optional_account_id(statement, 4, audit.actor_account_id);
    statement.bind(5, audit.occurred_utc_seconds);
    statement.bind(6, audit.correlation_id);
    statement.step();
}

CredentialEnvelope read_credential_columns(SqliteStatement& statement, const int offset) {
    CredentialEnvelope credential;
    credential.envelope_format_version = static_cast<std::uint16_t>(statement.int64(offset));
    credential.scheme_identifier = statement.text(offset + 1);
    credential.parameter_encoding_version = static_cast<std::uint16_t>(statement.int64(offset + 2));
    credential.canonical_parameters = statement.blob(offset + 3);
    credential.salt = statement.blob(offset + 4);
    credential.verifier = statement.blob(offset + 5);
    if (!statement.is_null(offset + 6)) credential.pepper_key_identifier = statement.text(offset + 6);
    credential.created_utc_seconds = statement.int64(offset + 7);
    credential.password_change_required = statement.boolean(offset + 8);
    return credential;
}

void bind_credential_columns(
    SqliteStatement& statement,
    const int offset,
    const CredentialEnvelope& credential) {
    statement.bind(offset, static_cast<std::int64_t>(credential.envelope_format_version));
    statement.bind(offset + 1, credential.scheme_identifier);
    statement.bind(offset + 2, static_cast<std::int64_t>(credential.parameter_encoding_version));
    statement.bind(offset + 3, credential.canonical_parameters);
    statement.bind(offset + 4, credential.salt);
    statement.bind(offset + 5, credential.verifier);
    bind_optional_text(statement, offset + 6, credential.pepper_key_identifier);
    statement.bind(offset + 7, credential.created_utc_seconds);
    statement.bind(offset + 8, credential.password_change_required);
}

std::optional<application::TemporaryPasswordState> temporary_password_state(const std::string_view value) {
    for (const auto state : {
             application::TemporaryPasswordState::active,
             application::TemporaryPasswordState::consumed,
             application::TemporaryPasswordState::cancelled,
             application::TemporaryPasswordState::expired}) {
        if (application::storage_code(state) == value) return state;
    }
    return std::nullopt;
}

std::optional<application::AuthenticationProtocol> authentication_protocol(const std::string_view value) {
    for (const auto protocol : {
             application::AuthenticationProtocol::telnet,
             application::AuthenticationProtocol::ssh}) {
        if (application::storage_code(protocol) == value) return protocol;
    }
    return std::nullopt;
}

std::optional<application::AuthenticationAuditEventKind> authentication_audit_kind(
    const std::string_view value) {
    for (const auto kind : {
             application::AuthenticationAuditEventKind::account_password_changed,
             application::AuthenticationAuditEventKind::account_credential_rehashed,
             application::AuthenticationAuditEventKind::temporary_password_issued,
             application::AuthenticationAuditEventKind::temporary_password_consumed,
             application::AuthenticationAuditEventKind::temporary_password_cancelled,
             application::AuthenticationAuditEventKind::temporary_password_expired,
             application::AuthenticationAuditEventKind::system_password_replaced,
             application::AuthenticationAuditEventKind::system_credential_rehashed,
             application::AuthenticationAuditEventKind::throttling_cleared}) {
        if (application::storage_code(kind) == value) return kind;
    }
    return std::nullopt;
}

std::optional<application::RateLimitScope> rate_limit_scope(const std::string_view value) {
    for (const auto scope : {
             application::RateLimitScope::account,
             application::RateLimitScope::source,
             application::RateLimitScope::system_password,
             application::RateLimitScope::global}) {
        if (application::storage_code(scope) == value) return scope;
    }
    return std::nullopt;
}

}  // namespace

MutationResult SqliteAccountRepository::replace_account_password(
    const application::ReplaceAccountPasswordRequest& request) {
    const auto decoded = application::decode_argon2id_credential_envelope(request.credential);
    if (request.account_id <= 0 || request.expected_account_revision < 1 || !decoded.valid() ||
        request.credential.password_change_required ||
        !application::valid_authentication_audit_context(request.audit)) {
        return {invalid("validate account-password replacement"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("replace account password", 2048U + request.credential.verifier.size(),
        [request](sqlite3* database, auto& output) {
            SqliteStatement account(database,
                "UPDATE sfx_accounts SET revision=revision+1,modified_utc_seconds=?1 "
                "WHERE account_id=?2 AND revision=?3 AND account_state='active'",
                "revise account for password replacement");
            account.bind(1, request.audit.occurred_utc_seconds);
            account.bind(2, request.account_id);
            account.bind(3, request.expected_account_revision);
            account.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement remove(database,
                "DELETE FROM sfx_account_credentials WHERE account_id=?1",
                "remove previous account password");
            remove.bind(1, request.account_id);
            remove.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;
            insert_credential(database, request.account_id, request.credential);
            SqliteStatement reset(database,
                "DELETE FROM sfx_account_password_resets WHERE account_id=?1",
                "clear completed temporary-password state");
            reset.bind(1, request.account_id);
            reset.step();
            insert_authentication_audit(database, request.audit,
                application::AuthenticationAuditEventKind::account_password_changed,
                request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_account_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::rehash_account_credential(
    const application::RehashAccountCredentialRequest& request) {
    const auto decoded = application::decode_argon2id_credential_envelope(request.credential);
    if (request.account_id <= 0 || request.expected_account_revision < 1 || !decoded.valid() ||
        !application::valid_authentication_audit_context(request.audit)) {
        return {invalid("validate account-credential rehash"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("rehash account credential", 2048U + request.credential.verifier.size(),
        [request](sqlite3* database, auto& output) {
            SqliteStatement account(database,
                "UPDATE sfx_accounts SET revision=revision+1,modified_utc_seconds=?1 "
                "WHERE account_id=?2 AND revision=?3 AND account_state='active'",
                "revise account for credential rehash");
            account.bind(1, request.audit.occurred_utc_seconds);
            account.bind(2, request.account_id);
            account.bind(3, request.expected_account_revision);
            account.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement current(database,
                "SELECT password_change_required FROM sfx_account_credentials WHERE account_id=?1",
                "read account credential rehash state");
            current.bind(1, request.account_id);
            if (!current.row() || current.boolean(0) != request.credential.password_change_required) {
                return SQLITE_CORRUPT;
            }
            SqliteStatement remove(database,
                "DELETE FROM sfx_account_credentials WHERE account_id=?1",
                "remove prior account credential for rehash");
            remove.bind(1, request.account_id);
            remove.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;
            insert_credential(database, request.account_id, request.credential);
            insert_authentication_audit(database, request.audit,
                application::AuthenticationAuditEventKind::account_credential_rehashed,
                request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_account_revision + 1;
            return SQLITE_OK;
        });
}

MutationResult SqliteAccountRepository::issue_temporary_password(
    const application::IssueTemporaryPasswordRequest& request) {
    const auto decoded = application::decode_argon2id_credential_envelope(request.credential);
    if (request.account_id <= 0 || request.expected_account_revision < 1 || !decoded.valid() ||
        !request.credential.password_change_required ||
        !application::valid_authentication_audit_context(request.audit) ||
        request.expires_utc_seconds !=
            request.audit.occurred_utc_seconds + application::kTemporaryPasswordLifetimeSeconds) {
        return {invalid("validate temporary-password issuance"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("issue temporary account password", 2048U + request.credential.verifier.size(),
        [request](sqlite3* database, auto& output) {
            require_runtime_sysop(database,request.audit);
            SqliteStatement account(database,
                "UPDATE sfx_accounts SET revision=revision+1,modified_utc_seconds=?1 "
                "WHERE account_id=?2 AND revision=?3 AND account_state<>'tombstoned'",
                "revise account for temporary password");
            account.bind(1, request.audit.occurred_utc_seconds);
            account.bind(2, request.account_id);
            account.bind(3, request.expected_account_revision);
            account.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement remove(database,
                "DELETE FROM sfx_account_credentials WHERE account_id=?1",
                "remove password before temporary replacement");
            remove.bind(1, request.account_id);
            remove.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;
            insert_credential(database, request.account_id, request.credential);
            SqliteStatement reset(database,
                "INSERT INTO sfx_account_password_resets(account_id,revision,reset_state,issued_utc_seconds,"
                "expires_utc_seconds,ended_utc_seconds) VALUES(?1,1,'active',?2,?3,NULL) "
                "ON CONFLICT(account_id) DO UPDATE SET revision=sfx_account_password_resets.revision+1,"
                "reset_state='active',issued_utc_seconds=excluded.issued_utc_seconds,"
                "expires_utc_seconds=excluded.expires_utc_seconds,ended_utc_seconds=NULL",
                "publish temporary-password state");
            reset.bind(1, request.account_id);
            reset.bind(2, request.audit.occurred_utc_seconds);
            reset.bind(3, request.expires_utc_seconds);
            reset.step();
            insert_authentication_audit(database, request.audit,
                application::AuthenticationAuditEventKind::temporary_password_issued,
                request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_account_revision + 1;
            return SQLITE_OK;
        });
}

StorageStatus SqliteAccountRepository::transition_temporary_password(
    const application::TransitionTemporaryPasswordRequest& request) {
    if (request.account_id <= 0 || request.expected_reset_revision < 1 ||
        request.new_state == application::TemporaryPasswordState::active ||
        !application::valid_authentication_audit_context(request.audit)) {
        return invalid("validate temporary-password transition");
    }
    return impl_->write("transition temporary account password", 1024U, [request](sqlite3* database) {
        require_runtime_sysop(database,request.audit);
        SqliteStatement update(database,
            "UPDATE sfx_account_password_resets SET revision=revision+1,reset_state=?1,ended_utc_seconds=?2 "
            "WHERE account_id=?3 AND revision=?4 AND reset_state='active'",
            "transition temporary-password state");
        update.bind(1, application::storage_code(request.new_state));
        update.bind(2, request.audit.occurred_utc_seconds);
        update.bind(3, request.account_id);
        update.bind(4, request.expected_reset_revision);
        update.step();
        if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
        application::AuthenticationAuditEventKind kind =
            application::AuthenticationAuditEventKind::temporary_password_consumed;
        if (request.new_state == application::TemporaryPasswordState::cancelled) {
            kind = application::AuthenticationAuditEventKind::temporary_password_cancelled;
        } else if (request.new_state == application::TemporaryPasswordState::expired) {
            kind = application::AuthenticationAuditEventKind::temporary_password_expired;
        }
        insert_authentication_audit(database, request.audit, kind, request.account_id);
        return SQLITE_OK;
    });
}

MutationResult SqliteAccountRepository::complete_temporary_password(
    const application::CompleteTemporaryPasswordRequest& request) {
    const auto decoded = application::decode_argon2id_credential_envelope(request.credential);
    if (request.account_id <= 0 || request.expected_account_revision < 1 ||
        request.expected_reset_revision < 1 || !decoded.valid() ||
        request.credential.password_change_required ||
        !application::valid_authentication_audit_context(request.consumed_audit) ||
        !application::valid_authentication_audit_context(request.changed_audit) ||
        request.changed_audit.occurred_utc_seconds < request.consumed_audit.occurred_utc_seconds ||
        request.consumed_audit.event_id == request.changed_audit.event_id) {
        return {invalid("validate temporary-password completion"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("complete temporary account password", 3072U + request.credential.verifier.size(),
        [request](sqlite3* database, auto& output) {
            SqliteStatement reset(database,
                "UPDATE sfx_account_password_resets SET revision=revision+1,reset_state='consumed',"
                "ended_utc_seconds=?1 WHERE account_id=?2 AND revision=?3 AND reset_state='active' "
                "AND expires_utc_seconds>?1",
                "consume temporary-password state");
            reset.bind(1, request.consumed_audit.occurred_utc_seconds);
            reset.bind(2, request.account_id);
            reset.bind(3, request.expected_reset_revision);
            reset.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement account(database,
                "UPDATE sfx_accounts SET revision=revision+1,modified_utc_seconds=?1 "
                "WHERE account_id=?2 AND revision=?3 AND account_state='active'",
                "revise account after temporary password");
            account.bind(1, request.changed_audit.occurred_utc_seconds);
            account.bind(2, request.account_id);
            account.bind(3, request.expected_account_revision);
            account.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            SqliteStatement current(database,
                "SELECT password_change_required FROM sfx_account_credentials WHERE account_id=?1",
                "require temporary credential before completion");
            current.bind(1, request.account_id);
            if (!current.row() || !current.boolean(0)) return SQLITE_CORRUPT;
            SqliteStatement remove(database,
                "DELETE FROM sfx_account_credentials WHERE account_id=?1",
                "remove consumed temporary credential");
            remove.bind(1, request.account_id);
            remove.step();
            if (sqlite3_changes(database) != 1) return SQLITE_CORRUPT;
            insert_credential(database, request.account_id, request.credential);
            insert_authentication_audit(database, request.consumed_audit,
                application::AuthenticationAuditEventKind::temporary_password_consumed,
                request.account_id);
            insert_authentication_audit(database, request.changed_audit,
                application::AuthenticationAuditEventKind::account_password_changed,
                request.account_id);
            output.account_id = request.account_id;
            output.revision = request.expected_account_revision + 1;
            return SQLITE_OK;
        });
}

application::StorageResult<application::TemporaryPasswordRecord>
SqliteAccountRepository::temporary_password(const AccountId account_id) {
    if (account_id <= 0) return read_failure<application::TemporaryPasswordRecord>(
        invalid("validate temporary-password lookup"));
    return impl_->readers_.read<application::TemporaryPasswordRecord>("load temporary-password state",
        [account_id](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT account_id,revision,reset_state,issued_utc_seconds,expires_utc_seconds,ended_utc_seconds "
                "FROM sfx_account_password_resets WHERE account_id=?1",
                "load temporary-password state");
            statement.bind(1, account_id);
            if (!statement.row()) throw StorageFailure(StorageErrorCode::not_found,
                "load temporary-password state", SQLITE_NOTFOUND);
            const auto state = temporary_password_state(statement.text(2));
            if (!state.has_value()) throw StorageFailure(StorageErrorCode::corrupt,
                "decode temporary-password state", SQLITE_CORRUPT);
            application::TemporaryPasswordRecord record;
            record.account_id = statement.int64(0);
            record.revision = statement.int64(1);
            record.state = *state;
            record.issued_utc_seconds = statement.int64(3);
            record.expires_utc_seconds = statement.int64(4);
            if (!statement.is_null(5)) record.ended_utc_seconds = statement.int64(5);
            return record;
        });
}

StorageStatus SqliteAccountRepository::replace_system_password(
    const application::ReplaceSystemPasswordRequest& request) {
    const auto decoded = application::decode_argon2id_credential_envelope(request.credential);
    if (request.expected_revision < 0 || !decoded.valid() || request.credential.password_change_required ||
        !application::valid_authentication_audit_context(request.audit)) {
        return invalid("validate System Password replacement");
    }
    return impl_->write("replace global System Password", 2048U + request.credential.verifier.size(),
        [request](sqlite3* database) {
            if (request.expected_revision == 0) {
                SqliteStatement insert(database,
                    "INSERT INTO sfx_system_credentials(singleton_id,revision,envelope_format_version,"
                    "scheme_identifier,parameter_encoding_version,canonical_parameters,salt,verifier,"
                    "pepper_key_identifier,created_utc_seconds,password_change_required,modified_utc_seconds) "
                    "VALUES(1,1,?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
                    "create global System Password");
                bind_credential_columns(insert, 1, request.credential);
                insert.bind(10, request.audit.occurred_utc_seconds);
                insert.step();
            } else {
                SqliteStatement update(database,
                    "UPDATE sfx_system_credentials SET revision=revision+1,envelope_format_version=?1,"
                    "scheme_identifier=?2,parameter_encoding_version=?3,canonical_parameters=?4,salt=?5,"
                    "verifier=?6,pepper_key_identifier=?7,created_utc_seconds=?8,password_change_required=?9,"
                    "modified_utc_seconds=?10 WHERE singleton_id=1 AND revision=?11",
                    "replace global System Password");
                bind_credential_columns(update, 1, request.credential);
                update.bind(10, request.audit.occurred_utc_seconds);
                update.bind(11, request.expected_revision);
                update.step();
                if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            }
            insert_authentication_audit(database, request.audit,
                application::AuthenticationAuditEventKind::system_password_replaced,
                std::nullopt);
            return SQLITE_OK;
        });
}

StorageStatus SqliteAccountRepository::rehash_system_credential(
    const application::RehashSystemCredentialRequest& request) {
    const auto decoded = application::decode_argon2id_credential_envelope(request.credential);
    if (request.expected_revision < 1 || !decoded.valid() ||
        request.credential.password_change_required ||
        !application::valid_authentication_audit_context(request.audit)) {
        return invalid("validate System Password rehash");
    }
    return impl_->write("rehash global System Password", 2048U + request.credential.verifier.size(),
        [request](sqlite3* database) {
            SqliteStatement update(database,
                "UPDATE sfx_system_credentials SET revision=revision+1,envelope_format_version=?1,"
                "scheme_identifier=?2,parameter_encoding_version=?3,canonical_parameters=?4,salt=?5,"
                "verifier=?6,pepper_key_identifier=?7,created_utc_seconds=?8,password_change_required=?9,"
                "modified_utc_seconds=?10 WHERE singleton_id=1 AND revision=?11",
                "rehash global System Password");
            bind_credential_columns(update, 1, request.credential);
            update.bind(10, request.audit.occurred_utc_seconds);
            update.bind(11, request.expected_revision);
            update.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            insert_authentication_audit(database, request.audit,
                application::AuthenticationAuditEventKind::system_credential_rehashed,
                std::nullopt);
            return SQLITE_OK;
        });
}

application::StorageResult<application::SystemCredentialRecord>
SqliteAccountRepository::system_password() {
    return impl_->readers_.read<application::SystemCredentialRecord>("load global System Password",
        [](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT revision,envelope_format_version,scheme_identifier,parameter_encoding_version,"
                "canonical_parameters,salt,verifier,pepper_key_identifier,created_utc_seconds,"
                "password_change_required,modified_utc_seconds FROM sfx_system_credentials WHERE singleton_id=1",
                "load global System Password");
            if (!statement.row()) throw StorageFailure(StorageErrorCode::not_found,
                "load global System Password", SQLITE_NOTFOUND);
            application::SystemCredentialRecord record;
            record.revision = statement.int64(0);
            record.credential = read_credential_columns(statement, 1);
            record.modified_utc_seconds = statement.int64(10);
            if (!application::decode_argon2id_credential_envelope(record.credential).valid() ||
                record.credential.password_change_required) {
                throw StorageFailure(StorageErrorCode::corrupt,
                    "validate global System Password", SQLITE_CORRUPT);
            }
            return record;
        });
}

StorageStatus SqliteAccountRepository::record_successful_login_ip(
    const application::RecordSuccessfulLoginIpRequest& request) {
    if (!request.event_id.valid() || request.account_id <= 0 ||
        !application::valid_source_ip_text(request.source_ip) ||
        !application::valid_utc_seconds(request.succeeded_utc_seconds)) {
        return invalid("validate successful-login IP history");
    }
    return impl_->write("record successful-login IP history", 1024U, [request](sqlite3* database) {
        SqliteStatement account(database,
            "SELECT count(*) FROM sfx_accounts WHERE account_id=?1 AND account_state='active'",
            "validate account for successful-login IP history");
        account.bind(1, request.account_id);
        if (!account.row() || account.int64(0) != 1) return SQLITE_ABORT;
        SqliteStatement insert(database,
            "INSERT INTO sfx_successful_login_ip_history(event_id,account_id,source_ip,protocol_code,"
            "succeeded_utc_seconds) VALUES(?1,?2,?3,?4,?5)",
            "insert successful-login IP history");
        insert.bind(1, request.event_id);
        insert.bind(2, request.account_id);
        insert.bind(3, request.source_ip);
        insert.bind(4, application::storage_code(request.protocol));
        insert.bind(5, request.succeeded_utc_seconds);
        insert.step();
        SqliteStatement prune(database,
            "DELETE FROM sfx_successful_login_ip_history WHERE account_id=?1 AND event_id IN ("
            "SELECT event_id FROM sfx_successful_login_ip_history WHERE account_id=?1 "
            "ORDER BY succeeded_utc_seconds DESC,event_id DESC LIMIT -1 OFFSET 5)",
            "retain last five successful-login IP records");
        prune.bind(1, request.account_id);
        prune.step();
        return SQLITE_OK;
    });
}

application::StorageResult<std::vector<application::SuccessfulLoginIpRecord>>
SqliteAccountRepository::successful_login_ips(const AccountId account_id) {
    if (account_id <= 0) return read_failure<std::vector<application::SuccessfulLoginIpRecord>>(
        invalid("validate successful-login IP lookup"));
    return impl_->readers_.read<std::vector<application::SuccessfulLoginIpRecord>>(
        "load successful-login IP history", [account_id](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT event_id,account_id,source_ip,protocol_code,succeeded_utc_seconds "
                "FROM sfx_successful_login_ip_history WHERE account_id=?1 "
                "ORDER BY succeeded_utc_seconds DESC,event_id DESC LIMIT 5",
                "load successful-login IP history");
            statement.bind(1, account_id);
            std::vector<application::SuccessfulLoginIpRecord> records;
            while (statement.row()) {
                const auto protocol = authentication_protocol(statement.text(3));
                if (!protocol.has_value()) throw StorageFailure(StorageErrorCode::corrupt,
                    "decode successful-login protocol", SQLITE_CORRUPT);
                application::SuccessfulLoginIpRecord record;
                record.event_id = statement.identifier(0);
                record.account_id = statement.int64(1);
                record.source_ip = statement.text(2);
                record.protocol = *protocol;
                record.succeeded_utc_seconds = statement.int64(4);
                records.push_back(std::move(record));
            }
            return records;
        });
}

StorageStatus SqliteAccountRepository::store_rate_limit_snapshot(
    const application::RateLimitSnapshot& snapshot) {
    if (!application::valid_rate_limit_snapshot(snapshot)) {
        return invalid("validate rate-limit snapshot");
    }
    return impl_->write("store rate-limit snapshot", 512U, [snapshot](sqlite3* database) {
        SqliteStatement statement(database,
            "INSERT INTO sfx_authentication_rate_state(scope_code,subject_hash,failure_count,"
            "window_started_utc_seconds,last_failure_utc_seconds,blocked_until_utc_seconds,expires_utc_seconds) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(scope_code,subject_hash) DO UPDATE SET "
            "failure_count=excluded.failure_count,window_started_utc_seconds=excluded.window_started_utc_seconds,"
            "last_failure_utc_seconds=excluded.last_failure_utc_seconds,"
            "blocked_until_utc_seconds=excluded.blocked_until_utc_seconds,expires_utc_seconds=excluded.expires_utc_seconds",
            "store rate-limit snapshot");
        statement.bind(1, application::storage_code(snapshot.key.scope));
        statement.bind(2, snapshot.key.subject_hash);
        statement.bind(3, static_cast<std::int64_t>(snapshot.failure_count));
        statement.bind(4, snapshot.window_started_utc_seconds);
        statement.bind(5, snapshot.last_failure_utc_seconds);
        statement.bind(6, snapshot.blocked_until_utc_seconds);
        statement.bind(7, snapshot.expires_utc_seconds);
        statement.step();
        return SQLITE_OK;
    });
}

StorageStatus SqliteAccountRepository::remove_rate_limit_snapshot(
    const application::RateLimitKey& key) {
    return impl_->write("remove rate-limit snapshot", 256U, [key](sqlite3* database) {
        SqliteStatement statement(database,
            "DELETE FROM sfx_authentication_rate_state WHERE scope_code=?1 AND subject_hash=?2",
            "remove rate-limit snapshot");
        statement.bind(1, application::storage_code(key.scope));
        statement.bind(2, key.subject_hash);
        statement.step();
        return SQLITE_OK;
    });
}

StorageStatus SqliteAccountRepository::clear_rate_limit(
    const application::ClearRateLimitRequest& request) {
    if (application::storage_code(request.key.scope).empty() ||
        !application::valid_authentication_audit_context(request.audit)) {
        return invalid("validate audited rate-limit clear");
    }
    return impl_->write("clear rate-limit state", 512U, [request](sqlite3* database) {
        SqliteStatement statement(database,
            "DELETE FROM sfx_authentication_rate_state WHERE scope_code=?1 AND subject_hash=?2",
            "clear rate-limit state");
        statement.bind(1, application::storage_code(request.key.scope));
        statement.bind(2, request.key.subject_hash);
        statement.step();
        insert_authentication_audit(database, request.audit,
            application::AuthenticationAuditEventKind::throttling_cleared, std::nullopt);
        return SQLITE_OK;
    });
}

application::StorageResult<std::vector<application::RateLimitSnapshot>>
SqliteAccountRepository::active_rate_limit_snapshots(
    const std::int64_t now_utc_seconds,
    const std::size_t limit) {
    if (!application::valid_utc_seconds(now_utc_seconds) || limit == 0 || limit > 4096) {
        return read_failure<std::vector<application::RateLimitSnapshot>>(
            invalid("validate active rate-limit snapshot query"));
    }
    return impl_->readers_.read<std::vector<application::RateLimitSnapshot>>(
        "load active rate-limit snapshots", [now_utc_seconds, limit](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT scope_code,subject_hash,failure_count,window_started_utc_seconds,"
                "last_failure_utc_seconds,blocked_until_utc_seconds,expires_utc_seconds "
                "FROM sfx_authentication_rate_state WHERE expires_utc_seconds>?1 "
                "ORDER BY expires_utc_seconds,scope_code,subject_hash LIMIT ?2",
                "load active rate-limit snapshots");
            statement.bind(1, now_utc_seconds);
            statement.bind(2, static_cast<std::int64_t>(limit));
            std::vector<application::RateLimitSnapshot> snapshots;
            while (statement.row()) {
                const auto scope = rate_limit_scope(statement.text(0));
                const std::vector<std::uint8_t> hash = statement.blob(1);
                if (!scope.has_value() || hash.size() != 32) throw StorageFailure(
                    StorageErrorCode::corrupt, "decode rate-limit snapshot", SQLITE_CORRUPT);
                application::RateLimitSnapshot snapshot;
                snapshot.key.scope = *scope;
                std::copy(hash.begin(), hash.end(), snapshot.key.subject_hash.begin());
                snapshot.failure_count = static_cast<std::uint32_t>(statement.int64(2));
                snapshot.window_started_utc_seconds = statement.int64(3);
                snapshot.last_failure_utc_seconds = statement.int64(4);
                snapshot.blocked_until_utc_seconds = statement.int64(5);
                snapshot.expires_utc_seconds = statement.int64(6);
                if (!application::valid_rate_limit_snapshot(snapshot)) throw StorageFailure(
                    StorageErrorCode::corrupt, "validate stored rate-limit snapshot", SQLITE_CORRUPT);
                snapshots.push_back(std::move(snapshot));
            }
            return snapshots;
        });
}

application::StorageResult<std::vector<application::AuthenticationAuditRecord>>
SqliteAccountRepository::authentication_audit_history(
    const application::AuthenticationAuditQuery& query) {
    if ((query.subject_account_id.has_value() && *query.subject_account_id <= 0) ||
        !application::valid_utc_seconds(query.at_or_before_utc_seconds) ||
        query.limit == 0 || query.limit > 256) {
        return read_failure<std::vector<application::AuthenticationAuditRecord>>(
            invalid("validate authentication-audit query"));
    }
    return impl_->readers_.read<std::vector<application::AuthenticationAuditRecord>>(
        "load authentication audit", [query](sqlite3* database) {
            std::string sql = "SELECT event_id,event_kind,subject_account_id,actor_account_id,"
                "occurred_utc_seconds,correlation_id FROM sfx_authentication_audit WHERE ";
            int bind_index = 1;
            if (query.subject_account_id.has_value()) {
                sql += "subject_account_id=?" + std::to_string(bind_index++) + " AND ";
            }
            if (query.event_kind.has_value()) {
                sql += "event_kind=?" + std::to_string(bind_index++) + " AND ";
            }
            const int cutoff_index = bind_index++;
            const int limit_index = bind_index++;
            sql += "occurred_utc_seconds<=?" + std::to_string(cutoff_index) +
                " ORDER BY occurred_utc_seconds DESC,event_id DESC LIMIT ?" + std::to_string(limit_index);
            SqliteStatement statement(database, sql.c_str(), "load authentication audit");
            int bind = 1;
            if (query.subject_account_id.has_value()) statement.bind(bind++, *query.subject_account_id);
            if (query.event_kind.has_value()) statement.bind(bind++, application::storage_code(*query.event_kind));
            statement.bind(bind++, query.at_or_before_utc_seconds);
            statement.bind(bind, static_cast<std::int64_t>(query.limit));
            std::vector<application::AuthenticationAuditRecord> records;
            while (statement.row()) {
                const auto kind = authentication_audit_kind(statement.text(1));
                if (!kind.has_value()) throw StorageFailure(StorageErrorCode::corrupt,
                    "decode authentication audit", SQLITE_CORRUPT);
                application::AuthenticationAuditRecord record;
                record.event_id = statement.identifier(0);
                record.event_kind = *kind;
                if (!statement.is_null(2)) record.subject_account_id = statement.int64(2);
                if (!statement.is_null(3)) record.actor_account_id = statement.int64(3);
                record.occurred_utc_seconds = statement.int64(4);
                record.correlation_id = statement.identifier(5);
                records.push_back(std::move(record));
            }
            return records;
        });
}

application::StorageResult<application::TransferPreferenceRecord>
SqliteAccountRepository::transfer_preference(const AccountId account_id) {
    if (account_id <= 0) return read_failure<application::TransferPreferenceRecord>(
        invalid("validate transfer-preference lookup"));
    return impl_->readers_.read<application::TransferPreferenceRecord>(
        "load transfer preference", [account_id](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT account_id,preference_code,revision,modified_utc_seconds "
                "FROM sfx_account_transfer_preferences WHERE account_id=?1",
                "load transfer preference");
            statement.bind(1, account_id);
            if (!statement.row()) throw StorageFailure(
                StorageErrorCode::not_found, "load transfer preference", SQLITE_NOTFOUND);
            const auto preference = application::transfer_preference_from_storage_code(statement.text(1));
            if (!preference.has_value()) throw StorageFailure(
                StorageErrorCode::corrupt, "decode transfer preference", SQLITE_CORRUPT);
            application::TransferPreferenceRecord record;
            record.account_id = statement.int64(0);
            record.preference = *preference;
            record.revision = statement.int64(2);
            record.modified_utc_seconds = statement.int64(3);
            return record;
        });
}

application::StorageResult<application::MenuPreferenceRecord>
SqliteAccountRepository::menu_preferences(const AccountId account_id) {
    if (account_id <= 0) return read_failure<application::MenuPreferenceRecord>(
        invalid("validate menu-preference lookup"));
    return impl_->readers_.read<application::MenuPreferenceRecord>(
        "load menu preferences", [account_id](sqlite3* database) {
            SqliteStatement statement(database,
                "SELECT p.color,p.hotkey,p.lightbar,p.revision,p.modified_utc_seconds "
                "FROM sfx_account_menu_preferences p JOIN sfx_accounts a ON a.account_id=p.account_id "
                "WHERE p.account_id=?1 AND a.account_state<>'tombstoned'", "load menu preferences");
            statement.bind(1,account_id);
            if (!statement.row()) throw StorageFailure(StorageErrorCode::not_found,
                "load menu preferences", SQLITE_NOTFOUND);
            const auto color=statement.int64(0), hotkey=statement.int64(1), lightbar=statement.int64(2);
            if (color<0 || color>15 || hotkey<0 || hotkey>1 || lightbar<0 || lightbar>1 ||
                statement.int64(3)<1 || !application::valid_utc_seconds(statement.int64(4)))
                throw StorageFailure(StorageErrorCode::corrupt,"decode menu preferences",SQLITE_CORRUPT);
            return application::MenuPreferenceRecord{account_id,
                {static_cast<std::uint8_t>(color),hotkey!=0,lightbar!=0}, statement.int64(3),statement.int64(4)};
        });
}

MutationResult SqliteAccountRepository::update_menu_preferences(
    const application::UpdateMenuPreferenceRequest& request) {
    if (request.account_id<=0 || request.expected_revision<1 ||
        request.expected_revision==std::numeric_limits<std::int64_t>::max() ||
        !application::valid_menu_preferences(request.preferences) ||
        !application::valid_utc_seconds(request.modified_utc_seconds))
        return {invalid("validate menu-preference update"),std::nullopt,std::nullopt,false};
    return impl_->mutate("update menu preferences",512U,[request](sqlite3* database,auto& output) {
        SqliteStatement update(database,
            "UPDATE sfx_account_menu_preferences SET color=?1,hotkey=?2,lightbar=?3,revision=revision+1,"
            "modified_utc_seconds=?4 WHERE account_id=?5 AND revision=?6 AND EXISTS "
            "(SELECT 1 FROM sfx_accounts a WHERE a.account_id=?5 AND a.account_state='active')",
            "save caller menu preferences");
        update.bind(1,static_cast<std::int64_t>(request.preferences.color));
        update.bind(2,static_cast<std::int64_t>(request.preferences.hotkey));
        update.bind(3,static_cast<std::int64_t>(request.preferences.lightbar));
        update.bind(4,request.modified_utc_seconds); update.bind(5,request.account_id);
        update.bind(6,request.expected_revision); update.step();
        if (sqlite3_changes(database)!=1) return SQLITE_ABORT;
        output.account_id=request.account_id; output.revision=request.expected_revision+1;
        return SQLITE_OK;
    });
}

MutationResult SqliteAccountRepository::update_transfer_preference(
    const application::UpdateTransferPreferenceRequest& request) {
    if (request.account_id <= 0 || request.expected_revision < 1 ||
        application::storage_code(request.preference).empty() ||
        !application::valid_utc_seconds(request.modified_utc_seconds)) {
        return {invalid("validate transfer-preference update"), std::nullopt, std::nullopt, false};
    }
    return impl_->mutate("update transfer preference", 512U,
        [request](sqlite3* database, auto& output) {
            SqliteStatement account(database,
                "SELECT count(*) FROM sfx_accounts WHERE account_id=?1 AND account_state<>'tombstoned'",
                "validate transfer-preference account");
            account.bind(1, request.account_id);
            if (!account.row() || account.int64(0) != 1) return SQLITE_ABORT;
            SqliteStatement update(database,
                "UPDATE sfx_account_transfer_preferences SET preference_code=?1,revision=revision+1,"
                "modified_utc_seconds=?2 WHERE account_id=?3 AND revision=?4",
                "update transfer preference");
            update.bind(1, application::storage_code(request.preference));
            update.bind(2, request.modified_utc_seconds);
            update.bind(3, request.account_id);
            update.bind(4, request.expected_revision);
            update.step();
            if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
            output.account_id = request.account_id;
            output.revision = request.expected_revision + 1;
            return SQLITE_OK;
        });
}

namespace {

application::FailedLoginNoticeRecord read_failed_login_notice(SqliteStatement& statement) {
    application::FailedLoginNoticeRecord record;
    record.notice_id = statement.identifier(0);
    record.account_id = statement.int64(1);
    const auto source_ip = application::canonical_source_ip(
        static_cast<std::uint8_t>(statement.int64(2)), statement.blob(3));
    if (!record.notice_id.valid() || !source_ip.has_value()) throw StorageFailure(
        StorageErrorCode::corrupt, "decode failed-login notice", SQLITE_CORRUPT);
    record.source_ip = *source_ip;
    record.event_utc_seconds = statement.int64(4);
    return record;
}

std::vector<application::FailedLoginNoticeRecord> read_claimed_failed_login_notices(
    sqlite3* database,
    const AccountId account_id,
    const Identifier128& claim_id,
    const Identifier128& session_id) {
    SqliteStatement statement(database,
        "SELECT notice_id,account_id,source_address_family,source_address,event_utc_seconds "
        "FROM sfx_failed_login_notices WHERE account_id=?1 AND claim_id=?2 AND claimed_session_id=?3 "
        "ORDER BY event_utc_seconds DESC,notice_id DESC",
        "load claimed failed-login notices");
    statement.bind(1, account_id);
    statement.bind(2, claim_id);
    statement.bind(3, session_id);
    std::vector<application::FailedLoginNoticeRecord> records;
    while (statement.row()) records.push_back(read_failed_login_notice(statement));
    return records;
}

}  // namespace

StorageStatus SqliteAccountRepository::enqueue_failed_login_notice(
    const application::EnqueueFailedLoginNoticeRequest& request) {
    const auto address = application::canonicalize_source_ip(request.source_ip);
    if (!request.notice_id.valid() || request.account_id <= 0 || !address.has_value() ||
        !application::valid_utc_seconds(request.event_utc_seconds)) {
        return invalid("validate failed-login notice enqueue");
    }
    return impl_->write("enqueue failed-login notice", 768U, [request, address](sqlite3* database) {
        SqliteStatement account(database,
            "SELECT count(*) FROM sfx_accounts WHERE account_id=?1 AND account_state<>'tombstoned'",
            "validate failed-login notice account");
        account.bind(1, request.account_id);
        if (!account.row() || account.int64(0) != 1) return SQLITE_ABORT;

        const std::vector<std::uint8_t> address_bytes(
            address->bytes.begin(), address->bytes.begin() + address->size);
        SqliteStatement existing(database,
            "SELECT account_id,source_address_family,source_address,event_utc_seconds "
            "FROM sfx_failed_login_notice_enqueues WHERE notice_id=?1",
            "check failed-login notice retry");
        existing.bind(1, request.notice_id);
        if (existing.row()) {
            const bool identical = existing.int64(0) == request.account_id &&
                existing.int64(1) == address->family && existing.blob(2) == address_bytes &&
                existing.int64(3) == request.event_utc_seconds;
            return identical ? SQLITE_OK : SQLITE_CONSTRAINT;
        }
        SqliteStatement receipt(database,
            "INSERT INTO sfx_failed_login_notice_enqueues(notice_id,account_id,"
            "source_address_family,source_address,event_utc_seconds) VALUES(?1,?2,?3,?4,?5)",
            "insert failed-login enqueue receipt");
        receipt.bind(1, request.notice_id);
        receipt.bind(2, request.account_id);
        receipt.bind(3, static_cast<std::int64_t>(address->family));
        receipt.bind(4, address_bytes);
        receipt.bind(5, request.event_utc_seconds);
        receipt.step();
        SqliteStatement insert(database,
            "INSERT INTO sfx_failed_login_notices(notice_id,account_id,source_address_family,"
            "source_address,event_utc_seconds) VALUES(?1,?2,?3,?4,?5)",
            "insert failed-login notice");
        insert.bind(1, request.notice_id);
        insert.bind(2, request.account_id);
        insert.bind(3, static_cast<std::int64_t>(address->family));
        insert.bind(4, address_bytes);
        insert.bind(5, request.event_utc_seconds);
        insert.step();
        return SQLITE_OK;
    });
}

application::StorageResult<application::FailedLoginNoticeBatch>
SqliteAccountRepository::claim_failed_login_notices(
    const application::ClaimFailedLoginNoticesRequest& request) {
    if (request.account_id <= 0 || !request.claim_id.valid() || !request.session_id.valid() ||
        request.limit == 0 || request.limit > application::kFailedLoginNoticeMaximum ||
        !application::valid_utc_seconds(request.claimed_utc_seconds) ||
        request.claimed_utc_seconds > application::kMaximumUtcSeconds -
            application::kFailedLoginClaimLeaseSeconds) {
        return read_failure<application::FailedLoginNoticeBatch>(
            invalid("validate failed-login notice claim"));
    }
    return impl_->write_result<application::FailedLoginNoticeBatch>(
        "claim failed-login notices", 1024U,
        [request](sqlite3* database, auto& output) {
            application::FailedLoginNoticeBatch batch;
            batch.claim_id = request.claim_id;
            batch.session_id = request.session_id;

            SqliteStatement existing(database,
                "SELECT account_id,session_id,claim_state FROM sfx_failed_login_notice_claims "
                "WHERE claim_id=?1",
                "check failed-login claim retry");
            existing.bind(1, request.claim_id);
            if (existing.row()) {
                if (existing.int64(0) != request.account_id ||
                    existing.identifier(1) != request.session_id) return SQLITE_ABORT;
                if (existing.text(2) == "active") {
                    batch.notices = read_claimed_failed_login_notices(
                        database, request.account_id, request.claim_id, request.session_id);
                } else if (existing.text(2) != "acknowledged" && existing.text(2) != "recovered") {
                    return SQLITE_CORRUPT;
                }
                output = std::move(batch);
                return SQLITE_OK;
            }

            SqliteStatement account(database,
                "SELECT count(*) FROM sfx_accounts WHERE account_id=?1 AND account_state<>'tombstoned'",
                "validate failed-login claim account");
            account.bind(1, request.account_id);
            if (!account.row() || account.int64(0) != 1) return SQLITE_ABORT;
            SqliteStatement session(database,
                "SELECT count(*) FROM sfx_account_sessions WHERE session_id=?1 AND account_id=?2 "
                "AND completed_utc_seconds IS NULL",
                "validate failed-login claim session");
            session.bind(1, request.session_id);
            session.bind(2, request.account_id);
            if (!session.row() || session.int64(0) != 1) return SQLITE_ABORT;
            SqliteStatement active_claim(database,
                "SELECT count(*) FROM sfx_failed_login_notice_claims "
                "WHERE account_id=?1 AND claim_state='active'",
                "enforce one active failed-login claim per account");
            active_claim.bind(1, request.account_id);
            if (!active_claim.row()) return SQLITE_CORRUPT;
            if (active_claim.int64(0) != 0) return SQLITE_ABORT;

            SqliteStatement receipt(database,
                "INSERT INTO sfx_failed_login_notice_claims(claim_id,account_id,session_id,claim_state,"
                "claimed_utc_seconds,claim_expires_utc_seconds) VALUES(?1,?2,?3,'active',?4,?5)",
                "insert failed-login claim receipt");
            receipt.bind(1, request.claim_id);
            receipt.bind(2, request.account_id);
            receipt.bind(3, request.session_id);
            receipt.bind(4, request.claimed_utc_seconds);
            receipt.bind(5, request.claimed_utc_seconds + application::kFailedLoginClaimLeaseSeconds);
            receipt.step();

            SqliteStatement claim(database,
                "UPDATE sfx_failed_login_notices SET claim_id=?1,claimed_session_id=?2,"
                "claimed_utc_seconds=?3,claim_expires_utc_seconds=?4 WHERE notice_id IN ("
                "SELECT notice_id FROM sfx_failed_login_notices WHERE account_id=?5 AND claim_id IS NULL "
                "ORDER BY event_utc_seconds DESC,notice_id DESC LIMIT ?6)",
                "claim failed-login notice batch");
            claim.bind(1, request.claim_id);
            claim.bind(2, request.session_id);
            claim.bind(3, request.claimed_utc_seconds);
            claim.bind(4, request.claimed_utc_seconds + application::kFailedLoginClaimLeaseSeconds);
            claim.bind(5, request.account_id);
            claim.bind(6, static_cast<std::int64_t>(request.limit));
            claim.step();
            batch.notices = read_claimed_failed_login_notices(
                database, request.account_id, request.claim_id, request.session_id);
            if (batch.notices.empty()) {
                SqliteStatement empty_receipt(database,
                    "UPDATE sfx_failed_login_notice_claims SET claim_state='acknowledged' WHERE claim_id=?1",
                    "complete empty failed-login claim");
                empty_receipt.bind(1, request.claim_id);
                empty_receipt.step();
            }
            output = std::move(batch);
            return SQLITE_OK;
        });
}

StorageStatus SqliteAccountRepository::acknowledge_failed_login_notices(
    const application::AcknowledgeFailedLoginNoticesRequest& request) {
    if (request.account_id <= 0 || !request.claim_id.valid() || !request.session_id.valid()) {
        return invalid("validate failed-login notice acknowledgement");
    }
    return impl_->write("acknowledge failed-login notices", 512U, [request](sqlite3* database) {
        SqliteStatement existing(database,
            "SELECT account_id,session_id,claim_state FROM sfx_failed_login_notice_claims "
            "WHERE claim_id=?1",
            "check failed-login acknowledgement ownership");
        existing.bind(1, request.claim_id);
        if (!existing.row()) return SQLITE_ABORT;
        if (existing.int64(0) != request.account_id ||
            existing.identifier(1) != request.session_id) return SQLITE_ABORT;
        const std::string state = existing.text(2);
        if (state == "acknowledged") return SQLITE_OK;
        if (state != "active") return SQLITE_ABORT;
        SqliteStatement remove(database,
            "DELETE FROM sfx_failed_login_notices WHERE account_id=?1 AND claim_id=?2 "
            "AND claimed_session_id=?3",
            "acknowledge failed-login notice batch");
        remove.bind(1, request.account_id);
        remove.bind(2, request.claim_id);
        remove.bind(3, request.session_id);
        remove.step();
        SqliteStatement receipt(database,
            "UPDATE sfx_failed_login_notice_claims SET claim_state='acknowledged' "
            "WHERE claim_id=?1 AND claim_state='active'",
            "acknowledge failed-login claim receipt");
        receipt.bind(1, request.claim_id);
        receipt.step();
        if (sqlite3_changes(database) != 1) return SQLITE_ABORT;
        return SQLITE_OK;
    });
}

application::StorageResult<std::size_t>
SqliteAccountRepository::recover_abandoned_failed_login_claims(
    const application::RecoverFailedLoginClaimsRequest& request) {
    if (!application::valid_utc_seconds(request.now_utc_seconds) ||
        request.limit == 0 || request.limit > 4096) {
        return read_failure<std::size_t>(invalid("validate abandoned failed-login claim recovery"));
    }
    return impl_->write_result<std::size_t>("recover abandoned failed-login claims", 1024U,
        [request](sqlite3* database, auto& output) {
            SqliteStatement recover(database,
                "UPDATE sfx_failed_login_notices SET claim_id=NULL,claimed_session_id=NULL,"
                "claimed_utc_seconds=NULL,claim_expires_utc_seconds=NULL WHERE notice_id IN ("
                "SELECT n.notice_id FROM sfx_failed_login_notices n "
                "JOIN sfx_failed_login_notice_claims c ON c.claim_id=n.claim_id "
                "WHERE c.claim_state='active' AND c.claim_expires_utc_seconds<=?1 "
                "AND c.claim_id IN (SELECT claim_id FROM sfx_failed_login_notice_claims "
                "WHERE claim_state='active' AND claim_expires_utc_seconds<=?1 "
                "ORDER BY claim_expires_utc_seconds,account_id,claim_id LIMIT ?2))",
                "recover abandoned failed-login claims");
            recover.bind(1, request.now_utc_seconds);
            recover.bind(2, static_cast<std::int64_t>(request.limit));
            recover.step();
            const std::size_t recovered_notices = static_cast<std::size_t>(sqlite3_changes(database));
            SqliteStatement retain_newest(database,
                "DELETE FROM sfx_failed_login_notices WHERE claim_id IS NULL AND notice_id IN ("
                "SELECT candidate.notice_id FROM sfx_failed_login_notices candidate "
                "WHERE candidate.account_id=sfx_failed_login_notices.account_id "
                "AND candidate.claim_id IS NULL ORDER BY candidate.event_utc_seconds DESC,"
                "candidate.notice_id DESC LIMIT -1 OFFSET 5)",
                "bound recovered failed-login unread notices");
            retain_newest.step();
            SqliteStatement receipts(database,
                "UPDATE sfx_failed_login_notice_claims SET claim_state='recovered' WHERE claim_id IN ("
                "SELECT claim_id FROM sfx_failed_login_notice_claims WHERE claim_state='active' "
                "AND claim_expires_utc_seconds<=?1 ORDER BY claim_expires_utc_seconds,account_id,claim_id LIMIT ?2)",
                "complete abandoned failed-login claim recovery");
            receipts.bind(1, request.now_utc_seconds);
            receipts.bind(2, static_cast<std::int64_t>(request.limit));
            receipts.step();
            output = recovered_notices;
            return SQLITE_OK;
        });
}

#include "sqlite_classic_information_repository.inc"
#include "sqlite_message_repository.inc"
#include "sqlite_message_queries.inc"
#include "sqlite_message_scan_settings.inc"
#include "sqlite_message_session_context.inc"
#include "sqlite_message_configuration.inc"
#include "sqlite_message_delete.inc"
#include "sqlite_message_maintenance.inc"
#include "sqlite_message_area_deletion.inc"
#include "sqlite_qwk_repository.inc"
#include "sqlite_qwk_download.inc"
#include "sqlite_qwk_progress.inc"
#include "sqlite_qwk_reply.inc"
#include "sqlite_file_repository.inc"
#include "sqlite_door_repository.inc"
#include "sqlite_door_bulletin_repository.inc"
#include "sqlite_chat_repository.inc"
#include "sqlite_file_configuration.inc"
#include "sqlite_file_imports.inc"
#include "sqlite_file_queries.inc"
#include "sqlite_file_transfers.inc"
#include "sqlite_file_intake.inc"
#include "sqlite_file_operations.inc"
#include "sqlite_file_scan_state.inc"
#include "sqlite_ftn_repository.inc"
#include "sqlite_ftn_work_repository.inc"
#include "sqlite_ftn_external_repository.inc"
#include "sqlite_ftn_dispatch_repository.inc"
#include "sqlite_ftn_held_repository.inc"
#include "sqlite_ftn_reprocess_repository.inc"
#include "sqlite_ftn_import_repository.inc"
#include "sqlite_ftn_areafix_repository.inc"
#include "sqlite_ftn_provider_response.inc"
#include "sqlite_ftn_rescan_repository.inc"
#include "sqlite_ftn_file_repository.inc"
#include "sqlite_ftn_tic_review_repository.inc"
#include "sqlite_ftn_held_review_repository.inc"
#include "sqlite_ftn_retention_repository.inc"
#include "sqlite_ftn_expiry_repository.inc"
}  // namespace sfx::adapters::storage::internal
