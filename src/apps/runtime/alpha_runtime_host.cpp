#include "alpha_runtime_host.hpp"
#include "binkp_activity.hpp"
#include "../product_version.h"
#include "sfx/application/door_bulletin_preview_context.hpp"
#include "sfx/platform/win32/door_bulletin_sources.hpp"
#include "runtime_bootstrap.hpp"
#include "runtime_configuration_update.hpp"
#include "caller_runtime_helpers.hpp"
#include "sfx/application/ftn_hatch_catalog.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sfx/platform/win32/starter_doors.hpp"
#include "sfx/platform/win32/service_secret_bridge.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/file_backup.hpp"
#include "sfx/platform/win32/file_configuration.hpp"
#include "sfx/platform/win32/botgate_rule_files.hpp"
#include "sfx/platform/win32/other_board_text_file.hpp"
#include "alpha_hostname_lookup.hpp"
#include "alpha_ssh_stream.hpp"
#include "alpha_durable_journal.hpp"
#include "alpha_local_door.hpp"

#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/adapters/storage/control_panel_statistics.hpp"
#include "sfx/archive/backup_service.hpp"
#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/clamav_update.hpp"

#include <chrono>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace sfx::apps::runtime {
namespace {

using application::AlphaControlResponse;
using application::AlphaRuntimePresentationState;
using application::LocalControlResponseCode;

bool storage_not_found(const application::StorageStatus& status) {
    return status.code == application::StorageErrorCode::not_found;
}

namespace install = platform::win32::installation;
constexpr wchar_t first_run_ticket_name[] = L"first-run.v1";

std::string first_run_ticket(const std::filesystem::path& root, const std::wstring& owner, HANDLE database) {
    const auto receipt = install::read_receipt(root, owner);
    install::require(receipt.committed && receipt.volume == install::volume_serial(database),
        "first_run_installation_unverified");
    return "SFX-WINDOWS-FIRST-RUN-1\n" + std::filesystem::path(receipt.id).u8string() + "\n" +
        std::to_string(install::file_id(database)) + "\n";
}

bool first_run_ticket_matches(const std::wstring& owner, HANDLE marker, std::string_view expected) {
    BY_HANDLE_FILE_INFORMATION info{};
    const auto security = install::mutable_descriptor(owner);
    if (!GetFileInformationByHandle(marker, &info) || info.nNumberOfLinks != 1 || info.nFileSizeHigh ||
        info.nFileSizeLow != expected.size() ||
        !install::exact_security(marker, security.get())) return false;
    std::array<char, 128> bytes{};
    DWORD count{};
    return ReadFile(marker, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
        std::string_view(bytes.data(), count) == expected;
}

bool retire_first_run_ticket(const std::filesystem::path& root, const std::wstring& owner) noexcept {
    try {
        const auto path = root / L"Config" / first_run_ticket_name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return GetLastError() == ERROR_FILE_NOT_FOUND;
        const auto marker = install::open_object(path, false, GENERIC_READ | DELETE | READ_CONTROL, FILE_SHARE_READ);
        const auto database = install::open_object(root / L"Data" / L"SFX.DB", false,
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE);
        FILE_DISPOSITION_INFO disposition{TRUE};
        return first_run_ticket_matches(owner, marker.get(), first_run_ticket(root, owner, database.get())) &&
            SetFileInformationByHandle(marker.get(), FileDispositionInfo, &disposition, sizeof(disposition));
    } catch (...) { return false; }
}

bool configuration_matches(
    const application::OperationalConfiguration& left,
    const application::OperationalConfiguration& right) {
    return application::equivalent_operational_configuration(left, right);
}

std::string_view presentation_diagnostic(const AlphaRuntimePresentationState state) {
    switch (state) {
        case AlphaRuntimePresentationState::starting: return "starting";
        case AlphaRuntimePresentationState::setup_required: return "setup_required";
        case AlphaRuntimePresentationState::ready: return "ready";
        case AlphaRuntimePresentationState::recovery_required: return "recovery_required";
        case AlphaRuntimePresentationState::stopping: return "stopping";
        case AlphaRuntimePresentationState::faulted: return "faulted";
        case AlphaRuntimePresentationState::draining: return "draining";
    }
    return "faulted";
}

class FixedClock final : public application::IClock {
public:
    explicit FixedClock(const std::int64_t now) : now_(now) {}
    std::int64_t unix_seconds() const noexcept override { return now_; }
    std::uint64_t monotonic_millis() const noexcept override { return 0; }
private:
    std::int64_t now_{0};
};

class RuntimeSecurityPublication final : public application::ISecurityPolicyPublication {
public:
    std::function<application::StorageStatus(const application::LocalSecuritySnapshot&)> apply;
    std::string fingerprint(std::string_view text) override {
        auto hash=platform::win32::installation::sha256({text.begin(),text.end()});
        std::transform(hash.begin(),hash.end(),hash.begin(),[](char c){return c>='A'&&c<='F'?char(c-'A'+'a'):c;});
        return hash;
    }
    application::StorageStatus publish(const application::LocalSecuritySnapshot& value) override {
        return apply ? apply(value) : application::StorageStatus{application::StorageErrorCode::internal,"publication unavailable"};
    }
};

std::optional<std::array<std::int64_t, 2>> bbs_today_range(
    const application::OperationalConfiguration& configuration,
    const std::int64_t now) {
    auto rules = platform::win32::WindowsRegistryBbsTimeZoneRuleProvider::create(
        configuration.bbs_time_zone);
    if (!rules) return std::nullopt;
    FixedClock clock(now);
    application::BbsLocalTimeAdapters time(rules->configuration(), *rules, clock);
    return sfx::apps::runtime::bbs_today_range(time, now);
}

std::string identifier_text(const application::Identifier128& identifier) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(32);
    for (const auto byte : identifier.bytes) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0fU]);
    }
    return result;
}

std::string bounded_scheduler_detail(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > 2048) value.resize(2048);
    return value;
}

}  // namespace

#include "alpha_message_maintenance.inc"
#include "alpha_clamav.inc"

bool AlphaRuntimeHost::RuntimeRevocationSink::revoke(
    const application::SessionRevocationCommand& command) {
    std::shared_ptr<AlphaTelnetListener> listener;
    {
        std::lock_guard<std::mutex> lock(mutex);
        listener = target.lock();
        if (!listener) return bootstrap_only;
    }
    // No Host or bridge lock is held while caller workers finish cleanup.
    return listener->revoke_sessions(command);
}

void AlphaRuntimeHost::RuntimeRevocationSink::attach(
    const std::shared_ptr<AlphaTelnetListener>& listener) {
    std::lock_guard<std::mutex> lock(mutex);
    target = listener;
    bootstrap_only = false;
}

bool AlphaRuntimeHost::RuntimeRevocationSink::refresh_account_access(application::AccountId account_id) {
    std::shared_ptr<AlphaTelnetListener> listener;
    { std::lock_guard<std::mutex> lock(mutex); listener=target.lock(); if(!listener) return bootstrap_only; }
    return listener->refresh_account_access(account_id);
}

AlphaRuntimeHost::AlphaRuntimeHost(std::filesystem::path installation_root, AlphaCallerInterlock interlock,std::wstring service_owner_sid)
    : installation_root_(std::move(installation_root)), service_owner_sid_(std::move(service_owner_sid)), caller_interlock_(std::move(interlock)),
      kdf_(2, 16), runtime_epoch_(identifiers_.generate()),
      sounds_(std::make_shared<application::EventSoundStream>(runtime_epoch_)) {
    lifecycle_thread_ = std::thread([this] { lifecycle_loop(); });
}

AlphaRuntimeHost::~AlphaRuntimeHost() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lifecycle_worker_stop_ = true;
    }
    lifecycle_wake_.notify_all();
    if (lifecycle_thread_.joinable()) lifecycle_thread_.join();
    if (external_events_) external_events_->stop();
    stop_scheduler();
    if(ftn_engine_)ftn_engine_->stop();
    stop_clamav();
    if(file_administration_)file_administration_->stop();
    if (telnet_listener_) telnet_listener_->stop();
    if (botgate_feed_) botgate_feed_->stop();
    if (journal_) (void)journal_->flush();
    if (durable_journal_) durable_journal_->stop();
    if (storage_) (void)storage_->shutdown();
    kdf_.shutdown();
}

bool AlphaRuntimeHost::initialize_scheduler_locked() {
    if (scheduler_) return true;
    if (presentation_state_ != AlphaRuntimePresentationState::ready || !storage_) {
        scheduler_diagnostic_ = "scheduler_runtime_not_ready";
        return false;
    }
    auto configuration_file = platform::win32::read_scheduler_configuration_file(
        installation_root_, owner_sid_);
    application::SchedulerConfiguration configuration;
    if (configuration_file.state == platform::win32::ConfigurationFileState::missing) {
        const auto saved = platform::win32::publish_scheduler_configuration_file(
            installation_root_, owner_sid_, configuration);
        if (!saved) {
            scheduler_diagnostic_ = saved.operation;
            return false;
        }
    } else if (configuration_file.state == platform::win32::ConfigurationFileState::loaded &&
               configuration_file.value) {
        configuration = *configuration_file.value;
    } else {
        scheduler_diagnostic_ = configuration_file.diagnostic_code;
        return false;
    }

    application::SchedulerState state;
    state.configuration_revision = configuration.revision;
    const auto state_file = platform::win32::read_scheduler_state_file(
        installation_root_, owner_sid_);
    if (state_file.state == platform::win32::ConfigurationFileState::loaded &&
        state_file.value) {
        state = *state_file.value;
    } else if (state_file.state != platform::win32::ConfigurationFileState::missing) {
        scheduler_diagnostic_ = state_file.diagnostic_code;
        return false;
    }

    const std::string scheduler_zone = configuration_->bbs_time_zone;
    auto candidate = std::make_unique<application::RuntimeScheduler>(
        configuration, state,
        [scheduler_zone](const std::int64_t after, const std::uint8_t hour,
                         const std::uint8_t minute) {
            return platform::win32::next_bbs_daily_run(
                scheduler_zone, after, hour, minute);
        });
    if (!candidate->recover_on_start(now_utc_seconds())) {
        scheduler_diagnostic_ = "scheduler_next_run_unavailable";
        return false;
    }
    const auto state_saved = platform::win32::publish_scheduler_state_file(
        installation_root_, owner_sid_, candidate->state());
    if (!state_saved) {
        scheduler_diagnostic_ = state_saved.operation;
        return false;
    }
    scheduler_source_generation_ = configuration_ ?
        std::to_string(configuration_->security_policy_revision) : "unknown";
    scheduler_ = std::move(candidate);
    initialize_message_schedulers_locked();
    scheduler_diagnostic_ = "scheduler_ready";
    scheduler_stop_ = false;
    scheduler_cancel_.store(false);
    try {
        scheduler_thread_ = std::thread([this] { scheduler_loop(); });
    } catch (...) {
        scheduler_.reset();
        scheduler_diagnostic_ = "scheduler_worker_start_failed";
        return false;
    }
    return true;
}

application::AlphaSchedulerStatus AlphaRuntimeHost::scheduler_status() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    application::AlphaSchedulerStatus result;
    result.diagnostic_code = scheduler_diagnostic_;
    if (!scheduler_) return result;
    const auto& configuration = scheduler_->configuration();
    const auto& state = scheduler_->state();
    result.configuration_loaded = true;
    result.job_id = configuration.job_id;
    result.enabled = configuration.enabled;
    result.configuration_revision = configuration.revision;
    result.local_hour = configuration.local_hour;
    result.local_minute = configuration.local_minute;
    result.next_run_utc = state.next_run_utc;
    result.active = state.active;
    result.active_scheduled = state.active_scheduled;
    result.queued = pending_scheduler_run_.has_value();
    result.active_run_id = state.active_run_id;
    result.active_started_utc = state.active_started_utc;
    result.last_run_utc = state.last_run_utc;
    result.last_result = state.last_result;
    result.last_detail = state.last_detail;
    result.history = state.history;
    for (auto& item : result.history)
        item.detail = application::bounded_scheduler_status_history_detail(item.detail);
    return result;
}

std::pair<application::ScheduledJobResult, std::string>
AlphaRuntimeHost::execute_scheduler_backup(
    const application::ScheduledRun& run) noexcept {
    std::unique_ptr<platform::win32::FileBackupSnapshot> snapshot_cleanup;
    std::unique_ptr<platform::win32::FileBackupSnapshot> identity_cleanup;
    struct BackupOutcome {
        std::shared_ptr<AlphaOperationalJournal> journal;std::string operation;bool completed{};
        ~BackupOutcome(){if(!completed&&journal)journal->problem(application::AttentionKind::work_preserved,"Backup "+operation,"backup_job_incomplete_work_preserved");}
    } backup_outcome{journal_,run.run_id};
    struct FtnBackupHold {
        std::shared_ptr<AlphaFtnEngine> engine;
        ~FtnBackupHold(){if(engine)engine->resume_after_backup();}
    } ftn_hold;
    try {
        const auto receipt=platform::win32::installation::read_receipt(installation_root_,owner_sid_);
        if(receipt.ftn_layout) {
            platform::win32::installation::verify_ftn_directory(receipt);
            std::shared_ptr<AlphaFtnEngine> engine;
            {std::lock_guard<std::mutex> lock(mutex_);engine=ftn_engine_;}
            if(!engine||!engine->pause_for_backup())return {application::ScheduledJobResult::failed,"ftn_backup_could_not_quiesce"};
            ftn_hold.engine=std::move(engine);
        }
        const auto kind = run.scheduled ? archive::BackupKind::scheduled : archive::BackupKind::manual;
        auto collection = platform::win32::open_backup_collection(
            installation_root_, owner_sid_, kind, run.backup_destination);
        const auto file_config=storage_->file_configuration({true,{}});
        platform::win32::installation::require(file_config&&file_config.value,"file_backup_configuration_unavailable");
        auto contents=platform::win32::FileBackupCapture::begin(installation_root_,owner_sid_,*file_config.value,collection,kind,[this]{return scheduler_cancel_.load();});
        const auto snapshot = installation_root_ / L"Work" /
            std::filesystem::u8path("scheduler-database-" + run.run_id + ".sqlite");
        if (GetFileAttributesW(snapshot.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return {application::ScheduledJobResult::failed,
                    "scheduler_snapshot_destination_exists"};
        }
        const auto database_backup = storage_->create_backup(snapshot);
        if (!database_backup.status) {
            return {application::ScheduledJobResult::failed,
                    bounded_scheduler_detail(database_backup.status.operation)};
        }
        snapshot_cleanup=std::make_unique<platform::win32::FileBackupSnapshot>(
            platform::win32::FilePayloadDirectory::private_store(installation_root_/L"Work",owner_sid_),
            snapshot.filename().u8string());
        const auto database_valid = storage_->validate_backup(snapshot);
        if (!database_valid) {
            return {application::ScheduledJobResult::failed,
                    bounded_scheduler_detail(database_valid.operation)};
        }
        archive::ManagedBackupRequest request;
        request.installation_root = installation_root_;
        request.ftn_layout=receipt.ftn_layout;
        request.destination_directory = collection.directory;
        request.kind = run.scheduled ? archive::BackupKind::scheduled
                                     : archive::BackupKind::manual;
        request.transaction_id = run.run_id;
        request.sfx_version = SFX_PRODUCT_VERSION_SHORT_STRING;
        request.source_generation = scheduler_source_generation_;
        request.created = archive::current_timestamp();
        request.database_snapshot = snapshot;
        request.cancelled = [this] { return scheduler_cancel_.load(); };
        const auto current_files=storage_->file_configuration({true,{}});
        platform::win32::installation::require(current_files&&current_files.value,"file_backup_configuration_unavailable");
        contents->bind(request,*current_files.value);
        const auto identity_path=installation_root_/L"Work"/std::filesystem::u8path("file-identities-"+run.run_id+".snapshot");
        const auto identities=platform::win32::refresh_portable_file_snapshot(installation_root_,owner_sid_,false,identity_path,request.cancelled);
        platform::win32::installation::require(identities.published,"backup_file_identity_snapshot_incomplete_preserved");
        identity_cleanup=std::make_unique<platform::win32::FileBackupSnapshot>(platform::win32::FilePayloadDirectory::private_store(installation_root_/L"Work",owner_sid_),identity_path.filename().u8string());
        request.portable_identity_snapshot=identity_path;
        const auto backup = archive::create_managed_backup(request);
        if (!backup.result.ok()) {
            return {application::ScheduledJobResult::failed,
                    bounded_scheduler_detail(std::string(archive::error_name(
                        backup.result.error)) + ": " + backup.result.detail)};
        }
        platform::win32::WindowsStorageDaclPolicy policy(owner_sid_);
        const auto secured = policy.apply(backup.archive_path,
            platform::win32::StorageDaclObjectKind::file);
        if (!secured) {
            return {application::ScheduledJobResult::failed,
                    bounded_scheduler_detail(secured.operation)};
        }
        const bool content_cleanup=contents->cleanup();
        const bool removed=snapshot_cleanup->cleanup();
        const bool identities_removed=identity_cleanup->cleanup();
        if (!removed || !content_cleanup || !identities_removed) {
            return {application::ScheduledJobResult::failed,
                    "scheduler_snapshot_cleanup_failed"};
        }
        backup_outcome.completed=true;
        std::size_t pruned{};
        try {
            pruned = platform::win32::finish_backup_collection(installation_root_, owner_sid_, collection,
                backup.archive_path, kind, run.retained_backups, [this] { return scheduler_cancel_.load(); });
        } catch (const std::exception& error) {
            return {application::ScheduledJobResult::failed, bounded_scheduler_detail(
                "Verified archive retained at " + backup.archive_path.u8string() + "; retention incomplete: " + error.what())};
        }
        return {application::ScheduledJobResult::success,
                bounded_scheduler_detail(backup.archive_path.u8string() + "; retained limit=" +
                    std::to_string(run.retained_backups) + "; pruned=" + std::to_string(pruned))};
    } catch (const std::exception& error) {
        return {application::ScheduledJobResult::failed,
                bounded_scheduler_detail(error.what())};
    } catch (...) {
        return {application::ScheduledJobResult::failed,
                "scheduler_backup_unknown_failure"};
    }
}

void AlphaRuntimeHost::scheduler_loop() noexcept {
    std::unique_lock<std::mutex> lock(scheduler_mutex_);
    while (!scheduler_stop_) {
        if (!scheduler_has_work_locked() && scheduler_ && !scheduler_paused_.load()) {
            const auto prior = scheduler_->state();
            auto due = scheduler_->begin_due(
                now_utc_seconds(), identifier_text(identifiers_.generate()));
            if (due) {
                const auto saved = platform::win32::publish_scheduler_state_file(
                    installation_root_, owner_sid_, scheduler_->state());
                if (saved) pending_scheduler_run_ = std::move(due);
                else {
                    scheduler_->replace_state(prior);
                    scheduler_diagnostic_ = saved.operation;
                }
            }
        }
        if(!pending_scheduler_run_ && !scheduler_has_work_locked() && !scheduler_paused_.load())for(auto& job:message_schedulers_){
            if(!job)continue;
            auto message_due=job->begin_due(now_utc_seconds(),identifier_text(identifiers_.generate()));
            if(!message_due)continue;
            const auto saved=platform::win32::publish_scheduler_state_file(installation_root_,owner_sid_,job->state());
            if(saved)pending_scheduler_run_=std::move(message_due);
            else{
                // No database work starts. Consume failed admission and wait
                // for the next cycle, not an immediate tick-by-tick retry.
                const auto index=job->configuration().job_id==application::kMessageRetentionJobId?0U:1U;
                const bool completed_admission=job->complete(*message_due,now_utc_seconds(),application::ScheduledJobResult::failed,"State publication failed; no message work started");
                if(!completed_admission)job.reset();
                message_scheduler_diagnostics_[index]="message_job_state_publication_failed";
            }
            break;
        }
        if (!pending_scheduler_run_) {
            scheduler_wake_.wait_for(lock, std::chrono::seconds(1), [this] {
                return scheduler_stop_ || pending_scheduler_run_.has_value();
            });
            continue;
        }
        const auto run = *pending_scheduler_run_;
        pending_scheduler_run_.reset();
        const auto message_started=GetTickCount64();
        lock.unlock();
        const bool backup=run.job_id==application::kBackupJobId;
        if (backup && journal_) journal_->emit({application::OperationalKind::backup_started,{}, {}});
        std::pair<application::ScheduledJobResult,std::string> outcome;
        std::optional<bool> verification_required;
        if(backup)outcome=execute_scheduler_backup(run);
        else{const auto message=execute_scheduler_message(run);outcome={message.result,message.detail};verification_required=message.requires_fresh_check;}
        lock.lock();
        bool completed = false;
        auto* completed_job=scheduler_for_job(run.job_id);
        if (completed_job && completed_job->complete(run, now_utc_seconds(),
                                               outcome.first, outcome.second)) {
            if(verification_required){auto state=completed_job->state();state.requires_fresh_check=*verification_required;completed_job->replace_state(std::move(state));}
            const auto saved = platform::win32::publish_scheduler_state_file(
                installation_root_, owner_sid_, completed_job->state());
            if(backup)scheduler_diagnostic_ = saved ? "scheduler_ready" : saved.operation;
            else{
                const auto index=run.job_id==application::kMessageRetentionJobId?0U:1U;
                message_scheduler_diagnostics_[index]=saved?"message_maintenance_ready":"message_completion_publication_failed";
                if(!saved && index==1){auto state=completed_job->state();state.requires_fresh_check=true;completed_job->replace_state(std::move(state));}
            }
            completed = saved && outcome.first == application::ScheduledJobResult::success;
        } else {
            if(backup)scheduler_diagnostic_ = "scheduler_completion_state_failed";
            else{
                const auto index=run.job_id==application::kMessageRetentionJobId?0U:1U;
                message_scheduler_diagnostics_[index]="message_completion_state_failed";
                message_schedulers_[index].reset(); // unknown next slot never retries each tick
            }
        }
        if (backup && journal_) journal_->emit({completed ? application::OperationalKind::backup_completed :
            application::OperationalKind::backup_failed,{}, {}});
        if(!backup && journal_){
            const auto id=maintenance_identifier(run.run_id);
            const auto actual=id?storage_->message_maintenance_run({true,{}},*id):application::StorageResult<application::MessageMaintenanceRun>{};
            bool overage=false;if(actual && actual.value)for(const auto& e:actual.value->echoes)overage=overage || e.protected_overage>0;
            application::OperationalContext context;context.resource_id=1;
            context.duration_seconds=(GetTickCount64()-message_started)/1000;
            context.resource_name=(run.job_id==application::kMessageRetentionJobId?"Retention: ":"Integrity: ")+outcome.second;
            if(context.resource_name.size()>128)context.resource_name.resize(128);
            context.diagnostic_code=outcome.first==application::ScheduledJobResult::success?"complete":
                outcome.first==application::ScheduledJobResult::partial?"partial":outcome.first==application::ScheduledJobResult::skipped?"skipped":"failed";
            journal_->emit({completed && !overage?application::OperationalKind::message_maintenance:application::OperationalKind::message_maintenance_problem,{},context});
            if(actual && actual.value)for(const auto& e:actual.value->echoes)if(e.removed || e.protected_overage){
                application::OperationalContext area;area.resource_id=static_cast<std::uint64_t>(e.echo.value);
                area.resource_name="Echo #"+std::to_string(e.echo.value)+": committed removals="+std::to_string(e.removed)+"; protected overage="+std::to_string(e.protected_overage);
                journal_->emit({e.protected_overage?application::OperationalKind::message_maintenance_problem:application::OperationalKind::message_maintenance,{},std::move(area)});
            }
        }
    }
}

void AlphaRuntimeHost::stop_scheduler() noexcept {
    {
        std::lock_guard<std::mutex> lock(scheduler_mutex_);
        scheduler_stop_ = true;
        scheduler_cancel_.store(true);
    }
    scheduler_wake_.notify_all();
    if (scheduler_thread_.joinable()) scheduler_thread_.join();
}

std::int64_t AlphaRuntimeHost::now_utc_seconds() const noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

application::AuditContext AlphaRuntimeHost::account_audit_locked(
    const application::AuditReasonCode reason,
    const std::optional<application::AccountId> actor) {
    return {identifiers_.generate(), identifiers_.generate(), actor, now_utc_seconds(), reason};
}

application::AuthenticationAuditContext AlphaRuntimeHost::authentication_audit_locked(
    const std::optional<application::AccountId> actor) {
    return {identifiers_.generate(), identifiers_.generate(), actor, now_utc_seconds()};
}

AlphaControlResponse AlphaRuntimeHost::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    starting_sounds_=true;
    struct EndStart { bool& active; ~EndStart(){active=false;} } end_start{starting_sounds_};
    if (!lifecycle_.request_start()) {
        return {LocalControlResponseCode::internal_failure, "runtime_start_state_invalid"};
    }
    owner_sid_ = service_owner_sid_.empty()?platform::win32::current_process_user_sid_string():service_owner_sid_;
    if (owner_sid_.empty()) return fail_recovery_locked("installation_owner_unavailable");
    const auto provisioned = platform::win32::provision_alpha_installation_tree(
        installation_root_, owner_sid_);
    if (!provisioned) return fail_recovery_locked(provisioned.operation);
    const auto guarded = instance_guard_.acquire(installation_root_, owner_sid_);
    if (!guarded) return fail_recovery_locked(guarded.operation);

    const auto prior_state = platform::win32::alpha_installation_has_persistent_state(
        installation_root_, owner_sid_,[this](const auto& path){
            if(path==installation_root_/L"Config"/L"service-secrets.v1") {
                try{return platform::win32::load_service_secret_bridge(installation_root_,owner_sid_).empty();}
                catch(...){return false;}
            }
            return platform::win32::stock_docs_bootstrap_metadata(installation_root_,path,owner_sid_)||
                platform::win32::clamav_bootstrap_metadata(installation_root_,path,owner_sid_);
        });
    if (!prior_state || !prior_state.value) return fail_recovery_locked(prior_state.status.operation);
    fresh_setup_allowed_ = !*prior_state.value;
    if(fresh_setup_allowed_&&platform::win32::clamav_available()&&platform::win32::clamav_installed(installation_root_)){
        // First setup must not adopt component metadata without trusted media.
        try{const auto receipt=platform::win32::installation::read_receipt(installation_root_,owner_sid_);
            platform::win32::verify_clamav_installation(installation_root_,owner_sid_,receipt.id);}
        catch(...){return fail_recovery_locked("clamav_initial_component_unverified");}
    }
    const auto close_file = [](void* handle) { if (handle) CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_file)> existing_database(nullptr, close_file);
    const auto initial_config = platform::win32::read_operational_configuration_file(installation_root_);
    const auto ticket_path = installation_root_ / L"Config" / first_run_ticket_name;
    const bool resume_first_run = initial_config.state == platform::win32::ConfigurationFileState::missing &&
        !fresh_setup_allowed_ && GetFileAttributesW(ticket_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (initial_config.state == platform::win32::ConfigurationFileState::missing) {
        if (!fresh_setup_allowed_ && !resume_first_run) return fail_recovery_locked("configuration_missing");
    } else if (initial_config.state != platform::win32::ConfigurationFileState::loaded) {
        return fail_recovery_locked(initial_config.diagnostic_code);
    } else {
        platform::win32::WindowsStorageDaclPolicy policy(owner_sid_);
        if (!policy.assess(installation_root_ / L"Config" / L"SFX.conf",
                platform::win32::StorageDaclObjectKind::file).status)
            return fail_recovery_locked("configuration_security_invalid");
        const auto& config = *initial_config.configuration;
        if (config.logical_nodes < 1 || config.logical_nodes > application::kAlphaNodePoolCapacity)
            return fail_recovery_locked("alpha_node_count_requires_1_to_128");
        auto zone = platform::win32::WindowsRegistryBbsTimeZoneRuleProvider::create(config.bbs_time_zone);
        if (!zone || zone->configuration().canonical_windows_registry_key != config.bbs_time_zone)
            return fail_recovery_locked("configuration_time_zone_unavailable");
    }
    if (initial_config.state == platform::win32::ConfigurationFileState::loaded || resume_first_run) {
        // Deny deletion/replacement until SQLite has acquired its own handles.
        const HANDLE file = CreateFileW((installation_root_ / L"Data" / L"SFX.DB").c_str(),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return fail_recovery_locked(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? "installation_database_missing" : "installation_database_inspection_failed");
        }
        existing_database.reset(file);
        BY_HANDLE_FILE_INFORMATION data{};
        if (!GetFileInformationByHandle(file, &data))
            return fail_recovery_locked("installation_database_inspection_failed");
        if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            (!data.nFileSizeHigh && !data.nFileSizeLow))
            return fail_recovery_locked("installation_database_invalid");
        if (resume_first_run) {
            try {
                const auto marker = install::open_object(ticket_path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
                if (data.nNumberOfLinks != 1 || !first_run_ticket_matches(owner_sid_, marker.get(),
                        first_run_ticket(installation_root_, owner_sid_, file)))
                    return fail_recovery_locked("configuration_missing");
            } catch (...) { return fail_recovery_locked("configuration_missing"); }
        }
    }

    path_policy_ = std::make_unique<platform::win32::WindowsLocalStoragePathPolicy>(owner_sid_);
    adapters::storage::SqliteStorageSettings settings;
    settings.data_directory = installation_root_ / L"Data";
    platform::win32::initialize_other_board_text(installation_root_,owner_sid_);
    settings.other_board_text=std::make_shared<platform::win32::WindowsOtherBoardTextStorage>(installation_root_,owner_sid_,
        [this](const application::OtherBoardLogRecord& record){return durable_journal_ && durable_journal_->other_board(record);});
    settings.utc_now = [this] { return now_utc_seconds(); };
    storage_ = std::make_unique<adapters::storage::SqliteStorage>(
        std::move(settings), *path_policy_, adapters::storage::sfx_storage_migrations());
    const auto started = storage_->start();
    if (!started) return fail_recovery_locked(started.operation);
    if (resume_first_run) {
        const auto first = storage_->first_run_state();
        const auto system = storage_->system_password();
        if (!first || !first.value || first.value->complete || first.value->staged_sysop_account_id ||
            !storage_not_found(system.status)) return fail_recovery_locked("configuration_missing");
        fresh_setup_allowed_ = true;
    } else if (fresh_setup_allowed_ && initial_config.state == platform::win32::ConfigurationFileState::missing &&
        GetFileAttributesW((installation_root_ / L"Bin" / L"installation.id").c_str()) != INVALID_FILE_ATTRIBUTES) {
        try {
            // Only a pristine, protected installation creates a ticket. Bind it
            // to the exact newly created database before preparing more state.
            const auto database = install::open_object(installation_root_ / L"Data" / L"SFX.DB", false,
                FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE);
            const auto ticket = first_run_ticket(installation_root_, owner_sid_, database.get());
            const auto security = install::mutable_descriptor(owner_sid_);
            install::create_file(ticket_path, {ticket.begin(), ticket.end()}, security.get());
        } catch (...) { return fail_recovery_locked("first_run_intent_unavailable"); }
    }
    std::vector<platform::win32::PortableCredentialIssue> credential_issues;
    {
        const application::MessageConfigurationActor actor{true,{}};
        const auto ftn=storage_->ftn_configuration(actor);const auto doors=storage_->door_configuration(actor);
        if(!ftn||!ftn.value||!doors||!doors.value)return fail_recovery_locked("portable_credential_catalog_unavailable");
        try{credential_issues=platform::win32::prepare_portable_credentials(installation_root_,owner_sid_,*ftn.value,*doors.value).issues;}
        catch(const std::exception&){credential_issues.push_back({"Board connection credentials","portable_credential_metadata_unavailable_preserved"});}
    }
    auto file_bindings=platform::win32::configure_portable_file_bindings(installation_root_);
    if(file_bindings.state==platform::win32::PortableFileBindingState::missing&&platform::win32::portable_file_initialization_pending(installation_root_))
        file_bindings=platform::win32::refresh_portable_file_snapshot(installation_root_,owner_sid_);
    if(!service_owner_sid_.empty()) {
        // Opt-in must prove retained credentials are readable by the actual
        // System Runtime before callers or background network work can start.
        const application::MessageConfigurationActor actor{true,{}};
        const auto ftn=storage_->ftn_configuration(actor);
        const auto doors=storage_->door_configuration(actor);
        if(!ftn||!ftn.value||!doors||!doors.value||
            !platform::win32::valid_ftn_protected_credentials(installation_root_.u8string(),*ftn.value))
            return fail_recovery_locked("service_stored_credentials_unavailable");
        for(const auto& door:doors.value->doors)
            for(const auto* encrypted:{&door.rlogin.protected_credential,&door.rlogin.protected_pepper})
                if(!encrypted->empty()&&!platform::win32::unprotect_rlogin_secret(installation_root_,*encrypted))
                    return fail_recovery_locked("service_stored_credentials_unavailable");
    }
    authentication_ = std::make_unique<application::AuthenticationSecurityService>(
        *storage_, *storage_, kdf_, &password_blocklist_, revocations_);
    if (!lifecycle_.report_started()) return fail_recovery_locked("runtime_lifecycle_start_failed");
    refresh_state_locked();
    if (presentation_state_ == AlphaRuntimePresentationState::recovery_required)
        return {LocalControlResponseCode::recovery_required, diagnostic_code_};
    if (!ensure_ssh_backend_locked()) return fail_recovery_locked(diagnostic_code_);
    journal_ = std::make_shared<AlphaOperationalJournal>(
        installation_root_ / L"Logs" / L"SFXRuntime.log", owner_sid_, runtime_epoch_,sounds_);
    // Apply saved retention before any diagnostic can open/prune the files.
    if (configuration_) (void)journal_->configure(configuration_->logging);
    for(std::size_t i=0;i<std::min<std::size_t>(8,credential_issues.size());++i){const auto& issue=credential_issues[i];journal_->problem(application::AttentionKind::saved_connection_unavailable,issue.connection,issue.diagnostic);}
    if(credential_issues.size()>8)journal_->problem(application::AttentionKind::saved_connection_unavailable,"Additional saved connections: "+std::to_string(credential_issues.size()-8),"additional_saved_connections_unavailable");
    if(file_bindings.state!=platform::win32::PortableFileBindingState::ready||file_bindings.deferred)
        journal_->problem(application::AttentionKind::work_preserved,"Portable file identity snapshot","file_identity_snapshot_unavailable_preserved");
    retained_logs_ = journal_->snapshot();
    if(!initialize_clamav_locked())return fail_recovery_locked("clamav_initial_scan_policy_failed");
    const auto controls = platform::win32::load_alpha_node_controls(installation_root_, owner_sid_);
    if (!controls || !controls.value) {
        if (presentation_state_ == AlphaRuntimePresentationState::recovery_required)
            return {LocalControlResponseCode::recovery_required, diagnostic_code_};
        return fail_recovery_locked(controls.status.operation);
    }
    nodes_enabled_ = *controls.value;
    if (presentation_state_ == AlphaRuntimePresentationState::ready) {
        const auto pending = platform::win32::read_pending_operational_configuration_file(
            installation_root_);
        if (pending.state == platform::win32::ConfigurationFileState::loaded &&
            pending.configuration && configuration_) {
            const auto active = *configuration_;
            configuration_ = *pending.configuration;
            const bool endpoint_ready = start_listener_locked(false);
            if (endpoint_ready &&
                platform::win32::publish_operational_configuration_file(
                    installation_root_, owner_sid_, *configuration_)) {
                platform::win32::remove_pending_operational_configuration_file(installation_root_);
                diagnostic_code_ = "ready";
            } else {
                if (!retire_listener_locked()) return fail_recovery_locked("caller_cleanup_failed");
                configuration_ = active;
                presentation_state_ = AlphaRuntimePresentationState::ready;
                platform::win32::remove_pending_operational_configuration_file(installation_root_);
                if (!start_listener_locked()) {
                    return {LocalControlResponseCode::recovery_required, diagnostic_code_};
                }
                diagnostic_code_ = "settings_activation_failed_restored";
            }
        } else if (pending.state != platform::win32::ConfigurationFileState::missing) {
            platform::win32::remove_pending_operational_configuration_file(installation_root_);
            diagnostic_code_ = "invalid_pending_settings_ignored";
            if (!start_listener_locked()) {
                return {LocalControlResponseCode::recovery_required, diagnostic_code_};
            }
        } else if (!start_listener_locked()) {
            return {LocalControlResponseCode::recovery_required, diagnostic_code_};
        }
    }
    if (presentation_state_ != AlphaRuntimePresentationState::recovery_required && !started_tick_)
        started_tick_ = GetTickCount64();
    if (presentation_state_ == AlphaRuntimePresentationState::ready) {
        initialize_ftn_locked();
        (void)initialize_scheduler_locked();
    }
    if (presentation_state_ != AlphaRuntimePresentationState::recovery_required)
        journal_->emit({application::OperationalKind::runtime_started, {}, {}});
    if(presentation_state_!=AlphaRuntimePresentationState::recovery_required&&
       platform::win32::clamav_available()&&platform::win32::clamav_installed(installation_root_)&&!clamav_thread_.joinable()){
        clamav_stop_.store(false);clamav_thread_=std::thread([this]{clamav_loop();});
    }
    return presentation_state_ == AlphaRuntimePresentationState::recovery_required
        ? AlphaControlResponse{LocalControlResponseCode::recovery_required, diagnostic_code_}
        : AlphaControlResponse{presentation_state_ == AlphaRuntimePresentationState::setup_required
              ? LocalControlResponseCode::setup_required : LocalControlResponseCode::ok,
              diagnostic_code_};
}

bool AlphaRuntimeHost::ensure_ssh_backend_locked() {
    // Never mask a pre-existing configuration/provisioning recovery diagnostic.
    if (presentation_state_ != AlphaRuntimePresentationState::ready ||
        !configuration_ || !configuration_->ssh.enabled || ssh_backend_) return true;
    std::string diagnostic;
    ssh_backend_ = AlphaSshBackend::acquire(installation_root_, owner_sid_, diagnostic);
    if (!ssh_backend_) diagnostic_code_ = std::move(diagnostic);
    return static_cast<bool>(ssh_backend_);
}

bool AlphaRuntimeHost::start_listener_locked(const bool fault_on_failure) {
    const auto reject = [this, fault_on_failure](std::string diagnostic) {
        diagnostic_code_ = std::move(diagnostic);
        if (fault_on_failure) (void)fail_recovery_locked(diagnostic_code_);
        return false;
    };
    if (telnet_listener_) return reject(telnet_listener_->status().diagnostic_code == "caller_cleanup_failed"
        ? "caller_cleanup_failed" : "caller_service_already_owned");
    if (presentation_state_ != AlphaRuntimePresentationState::ready ||
        !configuration_ || !storage_ || !authentication_) {
        return reject("telnet_listener_prerequisites_unavailable");
    }
    if (configuration_->logical_nodes < 1 || configuration_->logical_nodes > application::kAlphaNodePoolCapacity)
        return reject("alpha_node_count_requires_1_to_128");
    try{ensure_door_node_directories(installation_root_,owner_sid_,configuration_->logical_nodes);}
    catch(...){return reject("door_node_directory_unavailable");}
    if (!ensure_ssh_backend_locked()) return reject(diagnostic_code_);
    (void)journal_->configure(configuration_->logging);
    if (!durable_journal_) {
        auto candidate = std::make_shared<AlphaDurableJournal>(*storage_, identifiers_, installation_root_,
            owner_sid_, configuration_->bbs_time_zone, journal_);
        if (!candidate->configure_logging(configuration_->logging)) return reject("caller_journal_logging_configuration_invalid");
        if (!candidate->start()) return reject("caller_journal_authoritative_recovery_failed");
        durable_journal_ = std::move(candidate);
        journal_->attention_sink([weak=std::weak_ptr<AlphaDurableJournal>(durable_journal_)](application::AttentionRecord record) {
            if (const auto durable=weak.lock()) durable->attention(std::move(record));
        });
    }
    if (!botgate_feed_) {
        try { platform::win32::initialize_botgate_files(installation_root_,owner_sid_,configuration_->botgate); }
        catch(...) { return reject("botgate_file_initialization_failed"); }
        botgate_feed_=std::make_shared<AlphaBotGateFeed>(installation_root_,owner_sid_,journal_);
        (void)botgate_feed_->start();
    }
    telnet_listener_ = std::make_shared<AlphaTelnetListener>(
        *storage_, *authentication_, kdf_, identifiers_, *configuration_, statistics_,
        installation_root_, owner_sid_, ssh_backend_, caller_interlock_, nodes_enabled_, runtime_epoch_,
        previous_generations_, journal_, durable_journal_, botgate_feed_);
    revocations_.attach(telnet_listener_);telnet_listener_->attach_sysop_ftn(ftn_engine_);
    telnet_listener_->set_external_event_gate(true, 0);
    if (!telnet_listener_->start()) {
        const std::string listener_diagnostic = telnet_listener_->status().diagnostic_code;
        if (!retire_listener_locked()) return reject("caller_cleanup_failed");
        return reject("telnet_listener_" + listener_diagnostic);
    }
    const auto listening = telnet_listener_->status();
    if ((configuration_->telnet.enabled && !listening.listening) ||
        (configuration_->ssh.enabled && !listening.ssh_listening)) {
        if (!retire_listener_locked()) return reject("caller_cleanup_failed");
        return reject("configured_caller_listener_not_listening");
    }
    class EventClock final : public application::IClock {
    public:
        std::int64_t unix_seconds() const noexcept override {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        std::uint64_t monotonic_millis() const noexcept override { return GetTickCount64(); }
    };
    static const EventClock event_clock;
    const auto listener = telnet_listener_;
    external_events_ = std::make_unique<ExternalEventRunner>(installation_root_, owner_sid_,
        configuration_->bbs_time_zone, event_clock, ExternalEventCallerGate{
            [listener](bool held, std::int64_t cutoff) { listener->set_external_event_gate(held, cutoff); },
            [listener] { return listener->external_event_callers_empty(); },
            [listener] { listener->end_calls_for_external_event(); }}, journal_, durable_journal_, service_owner_sid_.empty());
    if (external_events_->start(!external_events_started_)) external_events_started_ = true;
    diagnostic_code_ = "ready";
    return true;
}

bool AlphaRuntimeHost::retire_listener_locked() {
    if (!telnet_listener_) return true;
    if (external_events_) {
        (void)telnet_listener_->set_draining(true);
        external_events_.reset();
    }
    telnet_listener_->stop();
    const auto final = telnet_listener_->status();
    // A failed authoritative cleanup keeps the actual quarantined service alive;
    // fallback activation must not create a new reusable pool over that failure.
    if (final.diagnostic_code == "caller_cleanup_failed") return false;
    for (const auto& node : final.nodes)
        previous_generations_[node.number - 1] = std::max(
            previous_generations_[node.number - 1], node.assignment_sequence);
    telnet_listener_.reset();
    return true;
}

bool AlphaRuntimeHost::activate_saved_configuration_locked(
    const application::OperationalConfiguration& candidate,
    std::string& diagnostic) {
    if (!configuration_ || presentation_state_ != AlphaRuntimePresentationState::ready) {
        diagnostic = "configuration_runtime_not_ready";
        return false;
    }
    const auto previous = *configuration_;
    const bool timezone_changed =
        previous.bbs_time_zone != candidate.bbs_time_zone;
    std::unique_lock<std::mutex> scheduler_lock(
        scheduler_mutex_, std::defer_lock);
    std::unique_ptr<application::RuntimeScheduler> rezoned_scheduler;
    if (timezone_changed) {
        scheduler_lock.lock();
        if (!scheduler_) {
            diagnostic = scheduler_diagnostic_;
            return false;
        }
        try {
            const auto schedule = scheduler_->configuration();
            const auto scheduler_state = scheduler_->state();
            const std::string selected_zone = candidate.bbs_time_zone;
            rezoned_scheduler = std::make_unique<application::RuntimeScheduler>(
                schedule, scheduler_state,
                [selected_zone](const std::int64_t after,
                                const std::uint8_t hour,
                                const std::uint8_t minute) {
                    return platform::win32::next_bbs_daily_run(
                        selected_zone, after, hour, minute);
                });
            if (!rezoned_scheduler->recompute_next(now_utc_seconds())) {
                diagnostic = "scheduler_time_zone_recompute_failed";
                return false;
            }
        } catch (...) {
            diagnostic = "scheduler_time_zone_recompute_failed";
            return false;
        }
    }
    if (!retire_listener_locked()) {
        diagnostic = "caller_cleanup_failed";
        return false;
    }
    if (timezone_changed && durable_journal_) {
        durable_journal_->stop();
        durable_journal_.reset();
    }
    configuration_ = candidate;
    bool candidate_active = ensure_ssh_backend_locked() &&
        start_listener_locked(false);
    if (candidate_active && timezone_changed) {
        const auto saved = platform::win32::publish_scheduler_state_file(
            installation_root_, owner_sid_, rezoned_scheduler->state());
        if (!saved) {
            diagnostic_code_ = saved.operation;
            candidate_active = false;
        }
    }
    if (candidate_active) {
        if (timezone_changed) {
            if (ftn_engine_) ftn_engine_->set_bbs_time_zone(candidate.bbs_time_zone);
            scheduler_ = std::move(rezoned_scheduler);
            scheduler_diagnostic_ = "scheduler_ready";
            scheduler_wake_.notify_all();
        }
        pending_configuration_.reset();
        pending_settings_.reset();
        diagnostic_code_ = "ready";
        diagnostic = "configuration_applied";
        return true;
    }

    const std::string candidate_failure = diagnostic_code_;
    (void)retire_listener_locked();
    if (durable_journal_) {
        durable_journal_->stop();
        durable_journal_.reset();
    }
    configuration_ = previous;
    presentation_state_ = AlphaRuntimePresentationState::ready;
    const auto restored = platform::win32::publish_operational_configuration_file(
        installation_root_, owner_sid_, previous);
    if (!restored || !start_listener_locked(false)) {
        (void)fail_recovery_locked("configuration_rollback_failed");
        diagnostic = diagnostic_code_;
        return false;
    }
    pending_configuration_.reset();
    pending_settings_.reset();
    diagnostic_code_ = "ready";
    diagnostic = "configuration_activation_failed_restored_" + candidate_failure;
    return false;
}

void AlphaRuntimeHost::activate_pending_configuration_if_safe_locked() {
    if (presentation_state_ != AlphaRuntimePresentationState::ready) return;
    if (!pending_configuration_ || !telnet_listener_) return;
    if (telnet_listener_->status().active_connections != 0) return;
    const auto candidate = *pending_configuration_;
    std::string ignored;
    (void)activate_saved_configuration_locked(candidate, ignored);
}

AlphaControlResponse AlphaRuntimeHost::fail_recovery_locked(std::string diagnostic) {
    sounds_->publish(application::SoundEvent::operational_alert,GetTickCount64(),starting_sounds_);
    lifecycle_.report_failure();
    presentation_state_ = AlphaRuntimePresentationState::recovery_required;
    diagnostic_code_ = std::move(diagnostic);
    return {LocalControlResponseCode::recovery_required, diagnostic_code_};
}

application::SoundPollSnapshot AlphaRuntimeHost::sound_poll(const application::SoundPollRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only the protected, successfully decoded configuration supplies paths.
    application::EventSoundConfiguration mappings;
    if(configuration_)mappings=configuration_->event_sounds;
    else if(!owner_sid_.empty()){
        platform::win32::WindowsStorageDaclPolicy policy(owner_sid_);
        if(policy.assess(installation_root_/L"Config"/L"SFX.conf",platform::win32::StorageDaclObjectKind::file).status){
            const auto saved=platform::win32::read_operational_configuration_file(installation_root_);
            if(saved.configuration)mappings=saved.configuration->event_sounds;
        }
    }
    return sounds_->poll(request,mappings,GetTickCount64());
}

void AlphaRuntimeHost::refresh_state_locked() {
    if (!storage_) {
        presentation_state_ = AlphaRuntimePresentationState::faulted;
        diagnostic_code_ = "storage_unavailable";
        return;
    }
    const auto config_file = platform::win32::read_operational_configuration_file(installation_root_);
    configuration_.reset();
    if (config_file.state == platform::win32::ConfigurationFileState::loaded) {
        configuration_ = config_file.configuration;
        if (configuration_->logical_nodes < 1 || configuration_->logical_nodes > application::kAlphaNodePoolCapacity) {
            (void)fail_recovery_locked("alpha_node_count_requires_1_to_128");
            return;
        }
        auto zone = platform::win32::WindowsRegistryBbsTimeZoneRuleProvider::create(
            configuration_->bbs_time_zone);
        if (!zone || zone->configuration().canonical_windows_registry_key !=
                configuration_->bbs_time_zone) {
            (void)fail_recovery_locked("configuration_time_zone_unavailable");
            return;
        }
    } else if (config_file.state != platform::win32::ConfigurationFileState::missing) {
        (void)fail_recovery_locked(config_file.diagnostic_code);
        return;
    } else if (!fresh_setup_allowed_) {
        (void)fail_recovery_locked("configuration_missing");
        return;
    }
    const auto first_run = storage_->first_run_state();
    const auto system = storage_->system_password();
    const auto policy = storage_->security_policy();
    if (!first_run || !policy || (!system && !storage_not_found(system.status))) {
        (void)fail_recovery_locked("bootstrap_state_unavailable");
        return;
    }
    security_policy_ = *policy.value;

    const bool system_present = system.succeeded() && system.value.has_value();
    const bool staged = first_run.value->staged_sysop_account_id.has_value();
    if (first_run.value->complete) {
        const auto levels = storage_->security_levels();
        if (configuration_ && levels && levels.value &&
            !platform::win32::reconcile_security_snapshot(installation_root_,owner_sid_,
                {application::LocalAccountResultCode::ok,*policy.value,*levels.value},*configuration_)) {
            (void)fail_recovery_locked("security_snapshot_mismatch_or_publication_failed");
            return;
        }
        if (!levels || !levels.value) { (void)fail_recovery_locked("security_catalog_unavailable"); return; }
        if (!configuration_ || !system_present || !staged ||
            !policy.value->published_revision || !policy.value->published_policy_hash ||
            *policy.value->published_revision != configuration_->security_policy_revision ||
            *policy.value->published_policy_hash != configuration_->security_policy_hash ||
            policy.value->revision != configuration_->security_policy_revision ||
            policy.value->policy_hash != configuration_->security_policy_hash) {
            (void)fail_recovery_locked("completed_bootstrap_mismatch");
            return;
        }
        presentation_state_ = AlphaRuntimePresentationState::ready;
        diagnostic_code_ = "ready";
    } else if (!staged && !system_present && fresh_setup_allowed_ && !configuration_) {
        presentation_state_ = AlphaRuntimePresentationState::setup_required;
        diagnostic_code_ = "setup_required";
    } else {
        presentation_state_ = AlphaRuntimePresentationState::recovery_required;
        diagnostic_code_ = "bootstrap_interrupted_recovery_required";
    }

    statistics_ = {};
    if (configuration_) {
        const auto range = bbs_today_range(*configuration_, now_utc_seconds());
        if (range) statistics_ = adapters::storage::ControlPanelStatisticsReader::read(
            installation_root_ / L"Data" / L"SFX.DB", (*range)[0], (*range)[1]);
    }
}

application::AlphaRuntimeStatus AlphaRuntimeHost::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    activate_pending_configuration_if_safe_locked();
    AlphaTelnetListenerStatus listener_status;
    if (telnet_listener_) listener_status = telnet_listener_->status();
    if (listener_status.diagnostic_code == "caller_cleanup_failed") {
        (void)lifecycle_.report_failure();
        presentation_state_ = AlphaRuntimePresentationState::faulted;
        diagnostic_code_ = "caller_cleanup_failed";
    }
    if (presentation_state_ == AlphaRuntimePresentationState::ready &&
        configuration_ && ((configuration_->telnet.enabled && !listener_status.listening) ||
            (configuration_->ssh.enabled && !listener_status.ssh_listening))) {
        (void)lifecycle_.report_failure();
        presentation_state_ = AlphaRuntimePresentationState::faulted;
        diagnostic_code_ = "caller_listener_" + listener_status.diagnostic_code;
    }
    application::AlphaRuntimeStatus result;
    if (external_events_) result.external_event_activity = external_events_->activity();
    result.runtime_epoch = runtime_epoch_;
    // Saturate rather than wrap; the client then rejects repeated sequence IDs.
    if (status_sequence_ != UINT64_MAX) ++status_sequence_;
    result.status_sequence = status_sequence_;
    result.configuration_revision = configuration_ ? configuration_->configuration_revision : 1;
    result.state = presentation_state_;
    result.lifecycle.state = lifecycle_.state();
    result.lifecycle.owner_mode = service_owner_sid_.empty()?application::RuntimeOwnershipMode::desktop:application::RuntimeOwnershipMode::service;
    result.lifecycle.configured_nodes = configuration_ ? configuration_->logical_nodes : 4;
    result.lifecycle.active_work.caller_sessions = listener_status.active_connections;
    result.configuration_loaded = configuration_.has_value();
    result.bootstrap_complete = presentation_state_ == AlphaRuntimePresentationState::ready ||
        presentation_state_ == AlphaRuntimePresentationState::draining;
    if (security_policy_) {
        result.security_policy_revision = security_policy_->revision;
        result.security_policy_hash = security_policy_->policy_hash;
    }
    if (configuration_) {
        result.bbs_name = configuration_->bbs_name;
        result.bbs_time_zone = configuration_->bbs_time_zone;
        result.telnet_configured = configuration_->telnet.enabled;
        result.telnet_bind = configuration_->telnet.bind_address;
        result.telnet_port = configuration_->telnet.port;
        result.telnet_bot_gate_enabled = configuration_->telnet.bot_gate_enabled;
        result.ssh_configured = configuration_->ssh.enabled;
        result.ssh_bind = configuration_->ssh.bind_address;
        result.ssh_port = configuration_->ssh.port;
        result.allow_new_users = configuration_->allow_new_users;
        result.hostname_lookup = configuration_->hostname_lookup;
    }
    // Caller service is the only live node authority. No synthetic ready rows.
    result.nodes = std::move(listener_status.nodes);
    if (storage_) result.storage = storage_->health();
    result.telnet_active = listener_status.listening;
    result.ssh_active = listener_status.ssh_listening;
    result.listener_restart_required = pending_settings_.has_value();
    result.pending_settings = pending_settings_;
    result.logs = journal_ ? journal_->snapshot() : retained_logs_;
    if (journal_) result.reporting = journal_->reporting();
    if (storage_) {
        result.administration_history = administration_reporting_.refresh(result.storage.committed_operations,
            [this]() -> std::optional<std::vector<application::AdministrationAuditSummary>> {
                const auto accounts = storage_->audit_history({{},{},application::kMaximumUtcSeconds,
                    application::kAdministrationHistoryLimit});
                const auto authentication = storage_->authentication_audit_history({{},{},application::kMaximumUtcSeconds,
                    application::kAdministrationHistoryLimit});
                if (!accounts || !accounts.value || !authentication || !authentication.value) return std::nullopt;
                std::vector<application::AdministrationAuditSummary> rows;
                rows.reserve(accounts.value->size() + authentication.value->size());
                for (const auto& entry : *accounts.value) rows.push_back({false,static_cast<std::uint8_t>(entry.event_kind),
                    entry.subject_account_id.value_or(0),entry.actor_account_id.value_or(0),
                    entry.occurred_utc_seconds,entry.event_id});
                for (const auto& entry : *authentication.value) rows.push_back({true,static_cast<std::uint8_t>(entry.event_kind),
                    entry.subject_account_id.value_or(0),entry.actor_account_id.value_or(0),
                    entry.occurred_utc_seconds,entry.event_id});
                return rows;
            });
    }
    result.log_health = journal_ ? journal_->health() : 0;
    result.hostname_exhausted = alpha_hostname_exhausted();
    result.statistics = telnet_listener_ ? listener_status.statistics : statistics_;
    result.scheduler = scheduler_status();
    result.diagnostic_code = diagnostic_code_;
    return result;
}

application::AlphaRuntimeMetricSnapshot AlphaRuntimeHost::metrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    application::ControlPanelSqlitePagerCounters counters;
    if (storage_) {
        const auto pager = storage_->pager_counters();
        counters.available = pager.available;
        counters.connection_generation = pager.connection_generation;
        counters.cache_hits = pager.cache_hits;
        counters.cache_misses = pager.cache_misses;
        counters.cache_writes = pager.cache_writes;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    application::AlphaRuntimeMetricSnapshot result;
    result.sqlite = sqlite_rate_tracker_.sample(counters,
        now < 0 ? 0U : static_cast<std::uint64_t>(now));
    result.uptime_available = started_tick_.has_value();
    const auto tick = GetTickCount64();
    result.uptime_milliseconds = started_tick_ && tick >= *started_tick_ ? tick - *started_tick_ : 0;
    return result;
}

std::optional<application::OperationalLogPage> AlphaRuntimeHost::log_page(application::OperationalLogCursor cursor) {
    std::shared_ptr<AlphaOperationalJournal> journal;
    { std::lock_guard<std::mutex> lock(mutex_); journal = journal_; }
    if (journal) return journal->page(cursor);
    return application::OperationalBacklog(runtime_epoch_).page(cursor);
}

AlphaControlResponse AlphaRuntimeHost::command_node(
    const application::AlphaNodeCommandRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configuration_ || !telnet_listener_ || request.node_number < 1 ||
        request.node_number > configuration_->logical_nodes ||
        application::encode_node_command_request(request).empty() ||
        (presentation_state_ != AlphaRuntimePresentationState::ready &&
            !(presentation_state_ == AlphaRuntimePresentationState::draining &&
                (request.command == application::AlphaNodeCommandKind::disconnect||request.command == application::AlphaNodeCommandKind::terminate_door))))
        return {LocalControlResponseCode::invalid_request, "node_not_controllable"};
    if(request.command==application::AlphaNodeCommandKind::terminate_door)return telnet_listener_->terminate_door(
        {request.expected_epoch,request.node_number,request.expected_generation},request.expected_door_launch);
    if (request.command == application::AlphaNodeCommandKind::disconnect) {
        const auto result = telnet_listener_->disconnect_caller({request.expected_epoch,
            request.node_number, request.expected_generation});
        if (result.code == LocalControlResponseCode::storage_failure)
            return fail_recovery_locked(result.diagnostic_code);
        return result;
    }
    const bool enable = request.command == application::AlphaNodeCommandKind::enable;
    const bool guarded = request.command == application::AlphaNodeCommandKind::disable_observed;
    if (guarded && !(request.expected_epoch == runtime_epoch_))
        return {LocalControlResponseCode::invalid_request, "node_caller_changed"};
    const auto index = request.node_number - 1;
    if (enable == nodes_enabled_[index]) return {LocalControlResponseCode::ok,
        enable ? "node_already_enabled" : "node_already_disabled"};
    auto candidate = nodes_enabled_;
    candidate[index] = enable;
    const auto persist = [&] {
        const auto result = platform::win32::publish_alpha_node_controls(
            installation_root_, owner_sid_, candidate);
        if (result) nodes_enabled_ = candidate;
        return result;
    };
    const auto persistence_failed = [&](const std::string& diagnostic) {
        // Disk publication might have succeeded before verification failed.
        telnet_listener_->stop();
        return fail_recovery_locked(diagnostic);
    };
    bool applied;
    if (guarded) {
        const auto result = telnet_listener_->disable_observed_node(
            request.node_number, request.expected_generation, persist);
        if (result.code == LocalControlResponseCode::invalid_request) return result;
        applied = result.code == LocalControlResponseCode::ok;
        if (!applied && nodes_enabled_[index] != enable) return persistence_failed(result.diagnostic_code);
    } else {
        const auto stored = persist();
        if (!stored) return persistence_failed(stored.operation);
        applied = telnet_listener_->set_node_enabled(request.node_number, enable);
    }
    if (!applied) {
        (void)lifecycle_.report_failure();
        presentation_state_ = AlphaRuntimePresentationState::faulted;
        diagnostic_code_ = "caller_cleanup_failed";
        return {LocalControlResponseCode::storage_failure, diagnostic_code_};
    }
    application::OperationalContext log_context;
    log_context.node = request.node_number;
    telnet_listener_->record_operational_log(enable ? application::OperationalKind::node_enabled :
        application::OperationalKind::node_disabled, std::move(log_context));
    return {LocalControlResponseCode::ok, enable ? "node_enabled" : "node_disabled"};
}

application::AlphaLocalSessionStartResult AlphaRuntimeHost::start_local_session(
    const application::AlphaLocalSessionStartRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configuration_ || !telnet_listener_ ||
        presentation_state_ != AlphaRuntimePresentationState::ready ||
        request.node_number == 0 || request.node_number > configuration_->logical_nodes) {
        return {{LocalControlResponseCode::invalid_request, "node_not_ready"}};
    }
    return telnet_listener_->start_local_session(request.node_number);
}

application::AlphaControlResponse AlphaRuntimeHost::cancel_local_session(
    const application::AlphaLocalSessionCancelRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!telnet_listener_) {
        return {LocalControlResponseCode::invalid_request, "local_reservation_stale"};
    }
    return telnet_listener_->cancel_local_session(request);
}

std::optional<application::AlphaConfigurationSnapshot>
AlphaRuntimeHost::configuration_snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configuration_ || presentation_state_ != AlphaRuntimePresentationState::ready)
        return std::nullopt;
    activate_pending_configuration_if_safe_locked();
    const auto saved = platform::win32::read_operational_configuration_file(
        installation_root_);
    if (saved.state != platform::win32::ConfigurationFileState::loaded ||
        !saved.configuration) return std::nullopt;
    application::AlphaConfigurationSnapshot result;
    result.active = *configuration_;
    result.saved = *saved.configuration;
    if (pending_configuration_) {
        result.activation = application::AlphaConfigurationActivation::pending_safe_boundary;
        result.detail = "Pending - waiting for active caller hostname lookups to end";
    } else if (result.active.configuration_revision !=
               result.saved.configuration_revision) {
        result.activation = application::AlphaConfigurationActivation::restart_required;
        result.detail = "Restart required - saved settings are not active yet";
    } else {
        result.activation = application::AlphaConfigurationActivation::applied;
        result.detail = "Applied";
    }
    return result;
}

application::LocalAccountSnapshot AlphaRuntimeHost::account_snapshot(
    const application::LocalAccountQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configuration_ || !storage_ || !authentication_ ||
        presentation_state_ != AlphaRuntimePresentationState::ready) return {};
    FixedClock clock(now_utc_seconds());
    application::LocalAccountAdministration administration(*storage_, *storage_,
        *authentication_, revocations_, identifiers_, clock, *configuration_,
        application::LocalAccountAuthority::installation_owner);
    auto result = administration.snapshot(query);
    result.online = query.account_id && telnet_listener_ && telnet_listener_->account_online(query.account_id);
    return result;
}

application::LocalAccountResult AlphaRuntimeHost::administer_account(
    application::LocalAccountRequest& request) {
    struct Erase { application::LocalAccountRequest& value;
        ~Erase() { application::erase_local_account_secrets(value); } } erase{request};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configuration_ || !storage_ || !authentication_ ||
        presentation_state_ != AlphaRuntimePresentationState::ready) return {};
    // Runtime's sole pipe admits the verified owning Control Panel. The relay
    // verifies SFXConfig; clients never supply or elevate this authority value.
    FixedClock clock(now_utc_seconds());
    application::LocalAccountAdministration administration(*storage_, *storage_,
        *authentication_, revocations_, identifiers_, clock, *configuration_,
        application::LocalAccountAuthority::installation_owner);
    application::CancellationSource cancellation;
    const auto result = administration.execute(request, cancellation.token());
    if (result.committed && journal_)
        journal_->emit({application::OperationalKind::account_administration,{}, {}});
    if (result.code == application::LocalAccountResultCode::revocation_failed) {
        if(telnet_listener_) telnet_listener_->stop();
        presentation_state_ = AlphaRuntimePresentationState::faulted;
        diagnostic_code_ = "account_administration_revocation_failed";
    }
    return result;
}

application::AlphaControlResponse AlphaRuntimeHost::clear_throttle(
    const application::LocalThrottleClearRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!application::valid_local_throttle_clear(request))
        return {LocalControlResponseCode::invalid_request,"throttle_clear_invalid"};
    if (!storage_ || !telnet_listener_ || presentation_state_ != AlphaRuntimePresentationState::ready)
        return {LocalControlResponseCode::configuration_failure,"throttle_clear_runtime_not_ready"};
    // Same authenticated owner-only pipe/Config relay as account administration.
    const auto result = telnet_listener_->clear_throttle(request,authentication_audit_locked());
    if (result && journal_) journal_->emit({application::OperationalKind::throttle_cleared,{}, {}});
    return result ? application::AlphaControlResponse{LocalControlResponseCode::ok,"throttle_cleared"} :
        application::AlphaControlResponse{LocalControlResponseCode::storage_failure,"throttle_clear_failed"};
}

application::LocalSecuritySnapshot AlphaRuntimeHost::security_snapshot(std::uint32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_ || !configuration_ || presentation_state_!=AlphaRuntimePresentationState::ready || offset>999) return {};
    FixedClock clock(now_utc_seconds()); RuntimeSecurityPublication publication;
    auto result=application::LocalSecurityAdministration(*storage_,publication,identifiers_,clock,
        application::LocalAccountAuthority::installation_owner).snapshot();
    if(result.code!=application::LocalAccountResultCode::ok) return result;
    if(offset>result.levels.size()) return {};
    result.levels.erase(result.levels.begin(),result.levels.begin()+offset);
    result.has_more=result.levels.size()>32;
    if(result.has_more) result.levels.resize(32);
    return result;
}
application::LocalClassicInformationSnapshot AlphaRuntimeHost::classic_information_snapshot(std::int64_t after) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_ || !configuration_ || presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    FixedClock clock(now_utc_seconds());
    return application::LocalClassicInformationAdministration(*storage_,identifiers_,clock,
        application::LocalAccountAuthority::installation_owner).snapshot(after);
}
application::LocalAccountResult AlphaRuntimeHost::change_classic_information(const application::ClassicInformationChange& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_ || !configuration_ || presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    FixedClock clock(now_utc_seconds());
    return application::LocalClassicInformationAdministration(*storage_,identifiers_,clock,
        application::LocalAccountAuthority::installation_owner).execute(change);
}

application::LocalMessageConfigurationSnapshot AlphaRuntimeHost::message_configuration_snapshot(const application::LocalMessageConfigurationQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_ || !configuration_ || presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    return application::LocalMessageAdministration(*storage_,application::LocalAccountAuthority::installation_owner).snapshot(query);
}
application::LocalMessageConfigurationResult AlphaRuntimeHost::change_message_configuration(const application::MessageConfigurationChange& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_ || !configuration_ || presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    if(change.action==application::MessageConfigurationAction::delete_echo || change.action==application::MessageConfigurationAction::delete_group)
        return {application::LocalAccountResultCode::unauthorized}; // held offline transaction only
    // Message catalog DB revisions are authoritative at every caller operation.
    // No second SFX.conf permission cache is published or trusted.
    return application::LocalMessageAdministration(*storage_,application::LocalAccountAuthority::installation_owner).execute(change);
}
application::LocalQwkPolicyResult AlphaRuntimeHost::qwk_policy_snapshot(){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    return application::LocalQwkAdministration(*storage_,application::LocalAccountAuthority::installation_owner).snapshot();
}
application::LocalFileConfigurationSnapshot AlphaRuntimeHost::file_configuration_snapshot(const application::LocalFileConfigurationQuery& query){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    return application::LocalFileAdministration(*storage_,application::LocalAccountAuthority::installation_owner).snapshot(query);
}
application::ChatResult AlphaRuntimeHost::send_chat_notice(const application::ChatNoticeRequest& r){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return application::ChatResult::unavailable;
    return telnet_listener_?telnet_listener_->send_chat_notice(r):application::ChatResult::unavailable;
}
application::ChatOperatorSnapshot AlphaRuntimeHost::chat_operator(const application::ChatOperatorRequest& r){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready||!telnet_listener_)return {};
    return telnet_listener_->chat_operator(r);
}
application::ChatConfigurationResult AlphaRuntimeHost::chat_configuration(){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    const auto result=storage_->chat_configuration({true,{}});return result&&result.value?application::ChatConfigurationResult{application::ChatResult::ok,*result.value}:application::ChatConfigurationResult{};
}
void AlphaRuntimeHost::initialize_ftn_locked() {
    if(ftn_engine_||!storage_||presentation_state_!=AlphaRuntimePresentationState::ready)return;
    try {
        const auto receipt=platform::win32::installation::read_receipt(installation_root_,owner_sid_);
        const auto configuration=storage_->ftn_configuration({true,{}});
        if(!configuration||!configuration.value){ftn_initialization_error_="ftn_configuration_unavailable";return;}
        auto engine=std::make_shared<AlphaFtnEngine>(receipt,*storage_,*storage_,runtime_epoch_,sounds_);
        engine->set_bbs_time_zone(configuration_->bbs_time_zone);
        engine->configure(*configuration.value);ftn_snapshot_=*configuration.value;ftn_engine_=std::move(engine);if(telnet_listener_)telnet_listener_->attach_sysop_ftn(ftn_engine_);
    }catch(const std::exception&){ftn_initialization_error_="ftn_engine_initialization_failed";}
}
AlphaFtnSnapshot AlphaRuntimeHost::ftn_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(ftn_engine_)return ftn_engine_->snapshot();
    AlphaFtnSnapshot result;result.diagnostic=ftn_initialization_error_;return result;
}
bool AlphaRuntimeHost::ftn_poll(std::string_view network,std::string_view link,bool include_hold) {
    std::lock_guard<std::mutex> lock(mutex_);
    return presentation_state_==AlphaRuntimePresentationState::ready&&ftn_engine_&&ftn_engine_->poll(network,link,include_hold);
}
application::FtnHeldControlResponse AlphaRuntimeHost::ftn_held(const application::FtnHeldControlRequest& request){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    return application::execute_ftn_held_control(*storage_,{true,{}},request);
}
application::FtnRuntimeResponse AlphaRuntimeHost::ftn_runtime(const application::FtnRuntimeRequest& request){
    namespace app=application;std::lock_guard<std::mutex> lock(mutex_);app::FtnRuntimeResponse out;
    if(app::encode_ftn_runtime_request(request).empty()){out.code=app::FtnConfigurationCode::invalid;return out;}
    if(!storage_||presentation_state_!=AlphaRuntimePresentationState::ready)return out;
    out.code=app::FtnConfigurationCode::ok;
    if(request.action==app::FtnRuntimeAction::poll&&(!ftn_engine_||!ftn_engine_->poll(request.network,request.link,request.include_hold)))out.code=app::FtnConfigurationCode::unavailable;
    if(request.action==app::FtnRuntimeAction::scan&&(!ftn_engine_||!ftn_engine_->scan()))out.code=app::FtnConfigurationCode::unavailable;
    if(ftn_engine_){const auto state=ftn_engine_->snapshot();out.mode=state.mode;out.saved_revision=state.saved_revision;out.active_revision=state.active_revision;out.capable=state.capable;out.paused=state.paused;out.serial_recovery_required=state.serial_recovery_required;out.classic_listening=state.classic_listening;out.tls_listening=state.tls_listening;out.processing=state.processing;out.classic_port=state.classic_port;out.tls_port=state.tls_port;out.diagnostic=state.diagnostic;out.classic_error=state.classic_error;out.tls_error=state.tls_error;out.processing_error=state.processing_error;out.processed_receipts=state.processed_receipts;out.held_receipts=state.held_receipts;out.imported_messages=state.imported_messages;out.replayed_messages=state.replayed_messages;out.scanned_messages=state.scanned_messages;out.activity=state.activity;
        for(const auto& s:state.sessions){const auto& t=s.transfer;out.sessions.push_back({s.serial,s.active,t.outbound,t.completed,t.peer_verified,std::string(binkp_session_network(t)),t.link,t.reason,t.received_bytes,t.sent_bytes,t.received_files,t.sent_files,t.send,t.receive});}
    }else out.diagnostic=ftn_initialization_error_;
    if(request.action==app::FtnRuntimeAction::queues){const auto page=storage_->ftn_queue_page({true,{}},request.offset);if(page&&page.value)out.queues=*page.value;else out.code=app::FtnConfigurationCode::unavailable;}
    if(request.action==app::FtnRuntimeAction::connections){const auto c=storage_->ftn_configuration({true,{}});if(!c||!c.value)out.code=app::FtnConfigurationCode::unavailable;else{const auto& links=c.value->links;const auto end=std::min<std::size_t>(links.size(),static_cast<std::size_t>(request.offset)+64);for(auto i=static_cast<std::size_t>(request.offset);i<end;++i){const auto& link=links[i];const auto n=std::find_if(c.value->networks.begin(),c.value->networks.end(),[&](const auto& network){return network.code==link.network;});out.connections.push_back({link.network,link.code,(n!=c.value->networks.end()?n->name:std::string("Unavailable network"))+" / "+link.name,link.enabled&&n!=c.value->networks.end()&&n->enabled,app::ftn_outbound(link)});}out.more_connections=end<links.size();}}
    // These are observation copies, never storage/routing objects. Keep private
    // routing keys only in connection choices used by Poll requests.
    const auto catalog=storage_->ftn_configuration({true,{}});
    const auto names=[&](std::string& network,std::string& link){
        const auto network_code=network,link_code=link;network="Unavailable network";link="Unconfigured peer";
        if(catalog&&catalog.value){for(const auto& n:catalog.value->networks)if(n.code==network_code){network=n.name;break;}
            for(const auto& l:catalog.value->links)if(l.network==network_code&&l.code==link_code){link=l.name;break;}}
    };
    for(auto& s:out.sessions){
        if(catalog&&catalog.value)for(const auto& l:catalog.value->links)if(l.network==s.network&&l.code==s.link){s.peer_address=l.address;s.endpoint=l.host.empty()?std::string{}:l.host+":"+std::to_string(l.port);break;}
        auto display=binkp_display_names(catalog&&catalog.value?&*catalog.value:nullptr,s.network,s.link);
        s.network=std::move(display.first);
        s.link=std::move(display.second);
    }
    for(auto& a:out.activity){
        auto display=binkp_display_names(catalog&&catalog.value?&*catalog.value:nullptr,a.network,a.link);
        a.network=std::move(display.first);
        a.link=std::move(display.second);
    }
    if(out.queues)for(auto& row:out.queues->rows)names(row.network,row.link);
    return out;
}
application::FtnConfigurationResponse AlphaRuntimeHost::ftn_hatch_catalog(const application::FtnConfigurationRequest& request){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    if(!ftn_hatch_publisher_)ftn_hatch_publisher_=std::make_unique<application::FtnReadOnlyPublisher>([this]()->std::optional<application::FtnReadOnlyPublisher::Snapshot>{const auto c=storage_->ftn_configuration({true,{}});if(!c||!c.value)return {};auto bytes=application::encode_ftn_hatch_catalog(application::ftn_hatch_catalog(*c.value));if(bytes.empty())return {};return application::FtnReadOnlyPublisher::Snapshot{c.value->revision,std::move(bytes)};});return ftn_hatch_publisher_->handle(request,GetTickCount64());
}
application::FtnConfigurationResponse AlphaRuntimeHost::ftn_configuration(const application::FtnConfigurationRequest& request){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    if(!ftn_publisher_)ftn_publisher_=std::make_unique<application::FtnConfigurationPublisher>(
        [this]()->std::optional<application::FtnConfiguration>{
            const auto c=storage_->ftn_configuration({true,{}});return c&&c.value?c.value:std::nullopt;
        },[this](const application::FtnTransferToken& token,const application::FtnConfiguration& c){
            application::FtnConfigurationResponse response;
            const auto current=storage_->ftn_configuration({true,{}});
            if(!current||!current.value||!platform::win32::valid_changed_ftn_credentials(installation_root_.u8string(),*current.value,c)){response.code=application::FtnConfigurationCode::invalid;return response;}
            const auto changed=storage_->change_ftn_configuration({true,{}},token,c);
            if(changed&&changed.value){ftn_snapshot_=changed.value->configuration;if(ftn_engine_)ftn_engine_->configure(*ftn_snapshot_);response.code=application::FtnConfigurationCode::ok;response.revision=changed.value->applied_revision;}
            else response.code=changed.status.code==application::StorageErrorCode::backpressure?application::FtnConfigurationCode::busy:changed.status.code==application::StorageErrorCode::conflict?application::FtnConfigurationCode::conflict:application::FtnConfigurationCode::unavailable;
            return response;
        },[this](const application::FtnTransferToken& token){
            application::FtnConfigurationResponse response;const auto receipt=storage_->ftn_configuration_receipt({true,{}},token);
            if(receipt&&receipt.value){response.revision=*receipt.value;response.code=response.revision?application::FtnConfigurationCode::ok:application::FtnConfigurationCode::conflict;}
            return response;
        },[this](const application::FtnTransferToken& token,const application::FtnConfiguration& c){
            application::FtnConfigurationResponse response;
            const auto current=storage_->ftn_configuration({true,{}});
            if(!current||!current.value||!platform::win32::valid_changed_ftn_credentials(installation_root_.u8string(),*current.value,c)){response.code=application::FtnConfigurationCode::invalid;return response;}
            const auto review=storage_->preview_ftn_retention({true,{}},token,c);
            if(review&&review.value){response.bytes=application::encode_ftn_retention_preview(*review.value);if(response.bytes.empty())return response;response.total=static_cast<std::uint32_t>(response.bytes.size());response.revision=c.revision;response.code=application::FtnConfigurationCode::ok;}
            else response.code=(review.status.code==application::StorageErrorCode::backpressure||review.status.code==application::StorageErrorCode::busy)?application::FtnConfigurationCode::busy:review.status.code==application::StorageErrorCode::conflict?application::FtnConfigurationCode::conflict:application::FtnConfigurationCode::unavailable;
            return response;
        });
    return ftn_publisher_->handle(request,GetTickCount64());
}
application::ChatConfigurationResult AlphaRuntimeHost::change_chat_configuration(const application::ChatConfiguration& c){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    if(telnet_listener_)return telnet_listener_->change_chat_configuration(c);
    const auto result=storage_->change_chat_configuration({true,{}},c);return result&&result.value?application::ChatConfigurationResult{application::ChatResult::ok,*result.value}:application::ChatConfigurationResult{application::ChatResult::conflict};
}
application::LocalDoorSnapshot AlphaRuntimeHost::door_configuration_snapshot(const application::LocalDoorQuery& query){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    return door_snapshot_cache_.query(*storage_,application::LocalAccountAuthority::installation_owner,query);
}
application::LocalDoorResult AlphaRuntimeHost::change_door_configuration(const application::DoorConfigurationChange& change){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    const auto result=application::administer_door(*storage_,application::LocalAccountAuthority::installation_owner,change);
    if(result.code==application::LocalAccountResultCode::ok)door_snapshot_cache_.clear();door_bulletin_snapshot_cache_.clear();return result;
}
application::LocalDoorSnapshot AlphaRuntimeHost::door_bulletin_snapshot(const application::LocalDoorBulletinQuery& query){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    const auto source=platform::win32::door_bulletin_sources(installation_root_,owner_sid_,[weak=std::weak_ptr<AlphaOperationalJournal>(journal_)](application::DoorBulletinId id,std::string_view diagnostic){if(const auto journal=weak.lock())journal->problem(application::AttentionKind::door_bulletin_unavailable,"Door Bulletin "+std::to_string(id.value),std::string(diagnostic));});
    return door_bulletin_snapshot_cache_.query(*storage_,application::LocalAccountAuthority::installation_owner,query,[&](application::DoorBulletinId id,bool ansi,std::int64_t revision){auto request=application::door_bulletin_preview_request(id,ansi?application::GraphicsMode::ansi:application::GraphicsMode::ascii,configuration_->bbs_name,SFX_PRODUCT_DISPLAY_VERSION,configuration_->logical_nodes);return application::preview_door_bulletin(*storage_,id,std::move(request),source,revision);});
}
application::LocalDoorResult AlphaRuntimeHost::change_door_bulletin(const application::DoorBulletinChange& change){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    const auto result=application::administer_door_bulletin(*storage_,application::LocalAccountAuthority::installation_owner,change);if(result.code==application::LocalAccountResultCode::ok)door_bulletin_snapshot_cache_.clear();return result;
}
application::LocalFileConfigurationResult AlphaRuntimeHost::change_file_configuration(const application::FileConfigurationChange& change){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    platform::win32::WindowsFileConfigurationPaths paths(installation_root_,owner_sid_);
    return application::LocalFileAdministration(*storage_,application::LocalAccountAuthority::installation_owner,&paths).execute(change);
}
application::FileManagementJobStatus AlphaRuntimeHost::start_file_management(const application::FileManagementTask& task){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    try{if(!file_administration_)file_administration_=std::make_shared<platform::win32::WindowsFileAdministration>(*storage_,installation_root_,owner_sid_,application::LocalAccountAuthority::installation_owner,platform::win32::WindowsFileManager::Boundary{},platform::win32::WindowsFileAdministration::IdleClock{},storage_.get());return file_administration_->start(task);}catch(...){return {};}
}
application::FileManagementReadSnapshot AlphaRuntimeHost::query_file_management(const application::FileManagementReadRequest& query){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    try{return application::LocalFileManagementQueries(*storage_,application::LocalAccountAuthority::installation_owner,storage_.get()).snapshot(query);}catch(...){return {};}
}
application::FileManagementJobStatus AlphaRuntimeHost::file_management_status(application::Identifier128 id){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    try{return file_administration_?file_administration_->status(id):application::FileManagementJobStatus{application::LocalAccountResultCode::policy_rejected};}catch(...){return {};}
}
application::FileManagementJobStatus AlphaRuntimeHost::cancel_file_management(application::Identifier128 id){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    try{return file_administration_?file_administration_->cancel(id):application::FileManagementJobStatus{application::LocalAccountResultCode::policy_rejected};}catch(...){return {};}
}
application::LocalQwkPolicyResult AlphaRuntimeHost::change_qwk_policy(const application::QwkPolicyChange& change){
    std::lock_guard<std::mutex> lock(mutex_);if(!storage_||!configuration_||presentation_state_!=AlphaRuntimePresentationState::ready)return {};
    return application::LocalQwkAdministration(*storage_,application::LocalAccountAuthority::installation_owner).execute(change);
}
application::LocalAccountResult AlphaRuntimeHost::change_security(const application::LocalSecurityChange& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!storage_ || !configuration_ || !telnet_listener_ || presentation_state_!=AlphaRuntimePresentationState::ready) return {};
    auto saved=platform::win32::read_operational_configuration_file(installation_root_);
    if(!saved.configuration) return {};
    FixedClock clock(now_utc_seconds()); RuntimeSecurityPublication publication;
    publication.apply=[&](const application::LocalSecuritySnapshot& truth) {
        const bool fully_active=configuration_->configuration_revision==saved.configuration->configuration_revision;
        auto result=platform::win32::reconcile_security_snapshot(installation_root_,owner_sid_,truth,*saved.configuration);
        if(!result) return result;
        configuration_->security_policy_revision=truth.policy.revision;
        configuration_->security_policy_hash=truth.policy.policy_hash;
        if(fully_active) configuration_->configuration_revision=saved.configuration->configuration_revision;
        if(pending_configuration_) {
            pending_configuration_->security_policy_revision=truth.policy.revision;
            pending_configuration_->security_policy_hash=truth.policy.policy_hash;
            pending_configuration_->configuration_revision=saved.configuration->configuration_revision;
        }
        security_policy_=truth.policy;
        if(!telnet_listener_->refresh_security_policy(*configuration_))
            return application::StorageStatus{application::StorageErrorCode::internal,"live security enforcement failed"};
        return application::StorageStatus::success();
    };
    auto result=application::LocalSecurityAdministration(*storage_,publication,identifiers_,clock,
        application::LocalAccountAuthority::installation_owner).execute(request);
    if(result.committed && journal_) journal_->emit({application::OperationalKind::account_administration,{}, {}});
    if(result.committed && result.code!=application::LocalAccountResultCode::ok) {
        telnet_listener_->stop();
        (void)fail_recovery_locked("security_policy_publication_failed");
    }
    return result;
}

application::AlphaConfigurationUpdateResult AlphaRuntimeHost::update_configuration(
    const application::AlphaConfigurationUpdateRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    application::AlphaConfigurationUpdateResult result;
    const auto reject = [&](const LocalControlResponseCode code,
                            const std::string& diagnostic,
                            const std::string& detail) {
        result.code = code;
        result.diagnostic_code = diagnostic;
        result.activation = application::AlphaConfigurationActivation::refused;
        result.detail = detail;
        result.active_revision = configuration_ ? configuration_->configuration_revision : 1;
        const auto saved = platform::win32::read_operational_configuration_file(
            installation_root_);
        result.saved_revision = saved.configuration
            ? saved.configuration->configuration_revision : result.active_revision;
        return result;
    };
    if (!configuration_ || presentation_state_ != AlphaRuntimePresentationState::ready) {
        return reject(LocalControlResponseCode::configuration_failure,
            "configuration_runtime_not_ready", "Refused - Runtime is not ready");
    }
    const auto saved = platform::win32::read_operational_configuration_file(
        installation_root_);
    if (saved.state != platform::win32::ConfigurationFileState::loaded ||
        !saved.configuration) {
        return reject(LocalControlResponseCode::configuration_failure,
            saved.diagnostic_code, "Refused - saved configuration is unavailable");
    }
    const auto& candidate = request.configuration;
    if (const auto invalid=validate_runtime_configuration_update(request,*saved.configuration))
        return reject(LocalControlResponseCode::invalid_request,invalid->diagnostic,invalid->detail);
    const auto zone = platform::win32::WindowsRegistryBbsTimeZoneRuleProvider::create(
        candidate.bbs_time_zone);
    if (!zone || zone->configuration().canonical_windows_registry_key !=
            candidate.bbs_time_zone) {
        return reject(LocalControlResponseCode::invalid_request,
            "configuration_time_zone_invalid",
            "Refused - select an installed Windows time zone");
    }
    const auto current_status = telnet_listener_ ? telnet_listener_->status()
                                                  : AlphaTelnetListenerStatus{};
    const bool telnet_endpoint_changed =
        candidate.telnet.enabled != configuration_->telnet.enabled ||
        candidate.telnet.bind_address != configuration_->telnet.bind_address ||
        candidate.telnet.port != configuration_->telnet.port;
    const bool ssh_endpoint_changed =
        candidate.ssh.enabled != configuration_->ssh.enabled ||
        candidate.ssh.bind_address != configuration_->ssh.bind_address ||
        candidate.ssh.port != configuration_->ssh.port;
    if (candidate.password_mode == application::PasswordMode::strong && !password_blocklist_.ready())
        return reject(LocalControlResponseCode::configuration_failure,
            "strong_password_blocklist_unavailable", "Refused - packaged Strong-password data is unavailable");
    if (candidate.logical_nodes != configuration_->logical_nodes || candidate.password_mode != configuration_->password_mode ||
        candidate.maximum_connections_per_source != configuration_->maximum_connections_per_source ||
        candidate.maximum_sessions_per_account != configuration_->maximum_sessions_per_account ||
        candidate.source_connection_exceptions != configuration_->source_connection_exceptions) {
        const auto published = platform::win32::publish_operational_configuration_file(
            installation_root_, owner_sid_, candidate);
        if (!published) {
            return reject(LocalControlResponseCode::configuration_failure,
                published.operation, "Refused - configuration could not be saved");
        }
        result.code = LocalControlResponseCode::ok;
        result.saved_revision = candidate.configuration_revision;
        pending_configuration_.reset();
        pending_settings_.reset();
        result.activation = application::AlphaConfigurationActivation::restart_required;
        result.active_revision = configuration_->configuration_revision;
        result.diagnostic_code = "configuration_saved_restart_required";
        result.detail = "Restart required - node count, password or connection policy changed";
        if (journal_) journal_->emit({application::OperationalKind::configuration_saved,{}, {}});
        if (telnet_listener_)
            telnet_listener_->record_operational_log(
                application::OperationalKind::settings_pending);
        return result;
    }

    const bool hostname_changed =
        candidate.hostname_lookup != configuration_->hostname_lookup;
    const bool endpoint_changed = telnet_endpoint_changed || ssh_endpoint_changed;
    const bool multiple_endpoints = telnet_endpoint_changed && ssh_endpoint_changed;
    const bool timezone_changed =
        candidate.bbs_time_zone != configuration_->bbs_time_zone;
    if (current_status.active_connections != 0 &&
        (multiple_endpoints || (hostname_changed &&
            (telnet_endpoint_changed || ssh_endpoint_changed)))) {
        return reject(LocalControlResponseCode::configuration_failure,
            multiple_endpoints
                ? "multiple_listener_swap_requires_zero_active_callers"
                : "hostname_and_listener_change_requires_zero_active_callers",
            "Refused - this combined listener boundary requires zero active callers; the prior working settings remain authoritative");
    }

    // A running hostname-lookup lifetime is process-wide. Existing callers keep
    // their bounded subscription coherent, then the complete saved candidate is
    // activated at the named zero-caller boundary.
    if (current_status.active_connections != 0 && hostname_changed) {
        const auto published = platform::win32::publish_operational_configuration_file(
            installation_root_, owner_sid_, candidate);
        if (!published) {
            return reject(LocalControlResponseCode::configuration_failure,
                published.operation, "Refused - configuration could not be saved");
        }
        pending_configuration_ = candidate;
        result.code = LocalControlResponseCode::ok;
        result.activation = application::AlphaConfigurationActivation::pending_safe_boundary;
        result.active_revision = configuration_->configuration_revision;
        result.saved_revision = candidate.configuration_revision;
        result.diagnostic_code = "configuration_pending_active_callers";
        result.detail = "Pending - waiting for active caller hostname lookups to end";
        if (journal_) journal_->emit({application::OperationalKind::configuration_saved,{}, {}});
        if (telnet_listener_)
            telnet_listener_->record_operational_log(
                application::OperationalKind::settings_pending);
        return result;
    }

    std::unique_lock<std::mutex> scheduler_lock(
        scheduler_mutex_, std::defer_lock);
    std::unique_ptr<application::RuntimeScheduler> rezoned_scheduler;
    if (timezone_changed) {
        scheduler_lock.lock();
        if (!scheduler_) {
            return reject(LocalControlResponseCode::configuration_failure,
                scheduler_diagnostic_,
                "Refused - the scheduler is unavailable, so future eligibility could not be recomputed");
        }
        try {
            const auto schedule = scheduler_->configuration();
            const auto scheduler_state = scheduler_->state();
            const std::string selected_zone = candidate.bbs_time_zone;
            rezoned_scheduler = std::make_unique<application::RuntimeScheduler>(
                schedule, scheduler_state,
                [selected_zone](const std::int64_t after,
                                const std::uint8_t hour,
                                const std::uint8_t minute) {
                    return platform::win32::next_bbs_daily_run(
                        selected_zone, after, hour, minute);
                });
            if (!rezoned_scheduler->recompute_next(now_utc_seconds())) {
                return reject(LocalControlResponseCode::configuration_failure,
                    "scheduler_time_zone_recompute_failed",
                    "Refused - future Backup eligibility could not be calculated in the selected time zone");
            }
        } catch (...) {
            return reject(LocalControlResponseCode::configuration_failure,
                "scheduler_time_zone_recompute_failed",
                "Refused - future Backup eligibility could not be calculated in the selected time zone");
        }
    }

    // One endpoint can be prepared and exchanged in place. New work captures
    // the matching immutable non-listener settings; established callers keep
    // their prior configuration/time context.
    const bool activating_from_fully_disabled = endpoint_changed &&
        !configuration_->telnet.enabled && !configuration_->ssh.enabled;
    if (!multiple_endpoints && !activating_from_fully_disabled) {
        const auto previous = *configuration_;
        std::string endpoint_diagnostic = "listener_endpoints_unchanged";
        if (endpoint_changed && (!telnet_listener_ ||
                !telnet_listener_->reconfigure_listener_endpoints(
                    candidate, endpoint_diagnostic))) {
            return reject(LocalControlResponseCode::configuration_failure,
                endpoint_diagnostic,
                "Refused - the affected listener could not be swapped; the prior working endpoint remains active");
        }
        std::string settings_diagnostic;
        if (!telnet_listener_ || !telnet_listener_->reconfigure_live_settings(
                candidate, settings_diagnostic)) {
            if (endpoint_changed) {
                std::string rollback_diagnostic;
                if (!telnet_listener_->reconfigure_listener_endpoints(
                        previous, rollback_diagnostic)) {
                    presentation_state_ = AlphaRuntimePresentationState::recovery_required;
                    diagnostic_code_ = "configuration_listener_rollback_failed";
                    return reject(LocalControlResponseCode::recovery_required,
                        diagnostic_code_,
                        "Refused - listener recovery requires review");
                }
            }
            return reject(LocalControlResponseCode::configuration_failure,
                settings_diagnostic,
                "Refused - live settings could not be activated; the prior working settings remain authoritative");
        }
        const auto published = platform::win32::publish_operational_configuration_file(
            installation_root_, owner_sid_, candidate);
        if (!published) {
            std::string rollback_diagnostic;
            bool restored = true;
            if (endpoint_changed)
                restored = telnet_listener_->reconfigure_listener_endpoints(
                    previous, rollback_diagnostic);
            if (restored)
                restored = telnet_listener_->reconfigure_live_settings(
                    previous, rollback_diagnostic);
            if (!restored) {
                presentation_state_ = AlphaRuntimePresentationState::recovery_required;
                diagnostic_code_ = "configuration_live_rollback_failed";
                return reject(LocalControlResponseCode::recovery_required,
                    diagnostic_code_,
                    "Refused - configuration publication failed and live recovery requires review");
            }
            return reject(LocalControlResponseCode::configuration_failure,
                published.operation,
                "Refused - configuration could not be saved; the prior working settings were restored");
        }
        if (timezone_changed) {
            const auto state_saved =
                platform::win32::publish_scheduler_state_file(
                    installation_root_, owner_sid_, rezoned_scheduler->state());
            if (!state_saved) {
                const auto durable_restored =
                    platform::win32::publish_operational_configuration_file(
                        installation_root_, owner_sid_, *saved.configuration);
                std::string rollback_diagnostic;
                bool live_restored = true;
                if (endpoint_changed)
                    live_restored = telnet_listener_->reconfigure_listener_endpoints(
                        previous, rollback_diagnostic);
                if (live_restored)
                    live_restored = telnet_listener_->reconfigure_live_settings(
                        previous, rollback_diagnostic);
                if (!durable_restored || !live_restored) {
                    presentation_state_ = AlphaRuntimePresentationState::recovery_required;
                    diagnostic_code_ = "configuration_time_zone_rollback_failed";
                    return reject(LocalControlResponseCode::recovery_required,
                        diagnostic_code_,
                        "Refused - scheduler publication failed and configuration recovery requires review");
                }
                return reject(LocalControlResponseCode::configuration_failure,
                    state_saved.operation,
                    "Refused - the scheduler state could not be published; the prior time zone and listener settings were restored");
            }
        }
        configuration_ = candidate;
        if (timezone_changed) {
            if (ftn_engine_) ftn_engine_->set_bbs_time_zone(candidate.bbs_time_zone);
            scheduler_ = std::move(rezoned_scheduler);
            scheduler_diagnostic_ = "scheduler_ready";
            scheduler_wake_.notify_all();
        }
        pending_configuration_.reset();
        pending_settings_.reset();
        result.code = LocalControlResponseCode::ok;
        result.activation = application::AlphaConfigurationActivation::applied;
        result.active_revision = candidate.configuration_revision;
        result.saved_revision = candidate.configuration_revision;
        result.diagnostic_code = endpoint_changed
            ? endpoint_diagnostic : settings_diagnostic;
        result.detail = "Applied";
        if (journal_) journal_->emit({application::OperationalKind::configuration_saved,{}, {}});
        return result;
    }

    const auto published = platform::win32::publish_operational_configuration_file(
        installation_root_, owner_sid_, candidate);
    if (!published) {
        return reject(LocalControlResponseCode::configuration_failure,
            published.operation, "Refused - configuration could not be saved");
    }
    result.code = LocalControlResponseCode::ok;
    result.saved_revision = candidate.configuration_revision;

    std::string diagnostic;
    if (!activate_saved_configuration_locked(candidate, diagnostic)) {
        return reject(LocalControlResponseCode::configuration_failure,
            diagnostic, "Refused - activation failed and the prior settings were restored");
    }
    result.activation = application::AlphaConfigurationActivation::applied;
    result.active_revision = candidate.configuration_revision;
    result.diagnostic_code = diagnostic;
    result.detail = "Applied";
    if (journal_) journal_->emit({application::OperationalKind::configuration_saved,{}, {}});
    return result;
}

application::BotGateFeedStatus AlphaRuntimeHost::botgate_feed_status() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result=botgate_feed_ ? botgate_feed_->status() : application::BotGateFeedStatus{};
    if(configuration_) {result.enabled=configuration_->botgate.enabled;result.telnet=configuration_->telnet.bot_gate_enabled;result.ssh=configuration_->botgate.ssh_enabled;}
    if(journal_) journal_->gate_status(result);
    if(telnet_listener_) telnet_listener_->botgate_status(result);
    return result;
}
application::AlphaControlResponse AlphaRuntimeHost::update_botgate_feed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!botgate_feed_ || presentation_state_!=AlphaRuntimePresentationState::ready)
        return {LocalControlResponseCode::configuration_failure,"feed_runtime_not_ready"};
    return botgate_feed_->update_now() ? application::AlphaControlResponse{LocalControlResponseCode::ok,"feed_update_queued"} :
        application::AlphaControlResponse{LocalControlResponseCode::invalid_request,"feed_update_already_active_or_unavailable"};
}

AlphaControlResponse AlphaRuntimeHost::stage_settings(
    const application::AlphaSettingsRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configuration_ || presentation_state_ != AlphaRuntimePresentationState::ready) {
        return {LocalControlResponseCode::configuration_failure, "settings_runtime_not_ready"};
    }
    auto candidate = *configuration_;
    candidate.bbs_name = request.bbs_name;
    candidate.telnet.bind_address = request.telnet_bind;
    candidate.telnet.port = request.telnet_port;
    candidate.ssh.enabled = request.ssh_enabled;
    candidate.ssh.bind_address = request.ssh_bind;
    candidate.ssh.port = request.ssh_port;
    candidate.allow_new_users = request.allow_new_users;
    candidate.hostname_lookup = request.hostname_lookup;
    if (!application::validate_operational_configuration(candidate).valid()) {
        return {LocalControlResponseCode::invalid_request, "settings_invalid"};
    }
    if (application::equivalent_operational_configuration(candidate, *configuration_)) {
        const auto removed = platform::win32::remove_pending_operational_configuration_file(installation_root_);
        if (!removed) return {LocalControlResponseCode::configuration_failure, removed.operation};
        pending_settings_.reset();
        return {LocalControlResponseCode::ok, "settings_unchanged"};
    }
    auto pending = request; // Allocate the status snapshot before durable publication.
    const auto saved = platform::win32::publish_pending_operational_configuration_file(
        installation_root_, owner_sid_, candidate);
    if (!saved) return {LocalControlResponseCode::configuration_failure, saved.operation};
    pending_settings_ = std::move(pending);
    if (journal_) journal_->emit({application::OperationalKind::configuration_saved,{}, {}});
    if (telnet_listener_) telnet_listener_->record_operational_log(application::OperationalKind::settings_pending);
    return {LocalControlResponseCode::ok, "settings_saved_restart_required"};
}

AlphaControlResponse AlphaRuntimeHost::configure_scheduler(
    const application::AlphaSchedulerSettingsRequest& request) {
    std::lock_guard<std::mutex> host_lock(mutex_);
    if (presentation_state_ != AlphaRuntimePresentationState::ready) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_runtime_not_ready"};
    }
    std::lock_guard<std::mutex> scheduler_lock(scheduler_mutex_);
    if (!scheduler_) {
        return {LocalControlResponseCode::configuration_failure,
                scheduler_diagnostic_};
    }
    const auto& current = scheduler_->configuration();
    if (request.job_id != application::kBackupJobId ||
        request.expected_revision != current.revision ||
        request.local_hour >= 24 || request.local_minute >= 60 ||
        (request.backup_destination && !application::valid_backup_destination_text(*request.backup_destination))) {
        return {LocalControlResponseCode::invalid_request,
                request.expected_revision != current.revision
                    ? "scheduler_revision_conflict" : "scheduler_settings_invalid"};
    }
    if (current.revision == std::numeric_limits<std::uint64_t>::max()) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_revision_exhausted"};
    }
    auto replacement = current;
    replacement.format_version = 2;
    ++replacement.revision;
    replacement.enabled = request.enabled;
    replacement.local_hour = request.local_hour;
    replacement.local_minute = request.local_minute;
    // Hold the validated collection through publication, without changing any
    // active/queued run's captured destination or starting retention work.
    std::optional<platform::win32::BackupCollection> destination;
    if (request.backup_destination) {
        try {
            destination = platform::win32::open_backup_collection(
                installation_root_, owner_sid_, archive::BackupKind::manual, *request.backup_destination);
        } catch (const std::exception& error) {
            return {LocalControlResponseCode::configuration_failure, bounded_scheduler_detail(error.what())};
        }
        replacement.backup_destination = *request.backup_destination;
    }
    const std::string scheduler_zone = configuration_->bbs_time_zone;
    auto candidate = std::make_unique<application::RuntimeScheduler>(
        current, scheduler_->state(),
        [scheduler_zone](const std::int64_t after, const std::uint8_t hour,
                         const std::uint8_t minute) {
            return platform::win32::next_bbs_daily_run(
                scheduler_zone, after, hour, minute);
        });
    if (!candidate->reconfigure(replacement, now_utc_seconds())) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_next_run_unavailable"};
    }
    const auto config_saved = platform::win32::publish_scheduler_configuration_file(
        installation_root_, owner_sid_, replacement);
    if (!config_saved) {
        return {LocalControlResponseCode::configuration_failure,
                config_saved.operation};
    }
    const auto state_saved = platform::win32::publish_scheduler_state_file(
        installation_root_, owner_sid_, candidate->state());
    if (!state_saved) {
        const auto rolled_back =
            platform::win32::publish_scheduler_configuration_file(
                installation_root_, owner_sid_, current);
        if (!rolled_back) {
            presentation_state_ = AlphaRuntimePresentationState::recovery_required;
            diagnostic_code_ = "scheduler_configuration_rollback_failed";
            scheduler_diagnostic_ = diagnostic_code_;
            return {LocalControlResponseCode::recovery_required,
                    diagnostic_code_};
        }
        scheduler_diagnostic_ = state_saved.operation;
        return {LocalControlResponseCode::configuration_failure,
                scheduler_diagnostic_};
    }
    scheduler_ = std::move(candidate);
    scheduler_diagnostic_ = "scheduler_ready";
    scheduler_wake_.notify_all();
    if (journal_) journal_->emit({application::OperationalKind::configuration_saved,{}, {}});
    return {LocalControlResponseCode::ok, "scheduler_settings_saved"};
}

AlphaControlResponse AlphaRuntimeHost::run_scheduler_now(
    const application::AlphaSchedulerRunRequest& request) {
    std::lock_guard<std::mutex> host_lock(mutex_);
    if (presentation_state_ != AlphaRuntimePresentationState::ready) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_runtime_not_ready"};
    }
    std::lock_guard<std::mutex> scheduler_lock(scheduler_mutex_);
    if (!scheduler_ || request.job_id != application::kBackupJobId) {
        return {LocalControlResponseCode::invalid_request,
                scheduler_ ? "scheduler_job_unknown" : scheduler_diagnostic_};
    }
    if(scheduler_has_work_locked())return {LocalControlResponseCode::invalid_request,"scheduler_work_already_active"};
    const auto prior = scheduler_->state();
    auto run = scheduler_->begin_manual(
        now_utc_seconds(), identifier_text(identifiers_.generate()));
    if (!run) {
        return {LocalControlResponseCode::invalid_request,
                "scheduler_job_already_running"};
    }
    const auto saved = platform::win32::publish_scheduler_state_file(
        installation_root_, owner_sid_, scheduler_->state());
    if (!saved) {
        scheduler_->replace_state(prior);
        scheduler_diagnostic_ = saved.operation;
        return {LocalControlResponseCode::configuration_failure,
                scheduler_diagnostic_};
    }
    pending_scheduler_run_ = std::move(run);
    scheduler_diagnostic_ = "scheduler_ready";
    scheduler_wake_.notify_all();
    return {LocalControlResponseCode::ok, "scheduler_run_started"};
}

AlphaControlResponse AlphaRuntimeHost::bootstrap(application::AlphaBootstrapRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (presentation_state_ == AlphaRuntimePresentationState::recovery_required)
        return {LocalControlResponseCode::recovery_required, diagnostic_code_};
    if (!storage_ || !authentication_) {
        return {LocalControlResponseCode::storage_failure,
            diagnostic_code_.empty() ? "storage_unavailable" : diagnostic_code_};
    }
    if (presentation_state_ == AlphaRuntimePresentationState::ready) {
        return {LocalControlResponseCode::ok, "bootstrap_already_complete"};
    }
    if (presentation_state_ != AlphaRuntimePresentationState::setup_required) {
        return {LocalControlResponseCode::shutting_down, "runtime_not_accepting_bootstrap"};
    }
    const auto controls = platform::win32::load_alpha_node_controls(installation_root_, owner_sid_);
    if (!controls || !controls.value)
        return {LocalControlResponseCode::configuration_failure, controls.status.operation};
    nodes_enabled_ = *controls.value;
    RuntimeBootstrapPorts ports;
    ports.strong_passwords_ready = [this] { return password_blocklist_.ready(); };
    ports.canonical_time_zone = [](std::string_view name) -> std::optional<std::string> {
        auto zone = platform::win32::WindowsRegistryBbsTimeZoneRuleProvider::create(std::string(name));
        if (!zone) return std::nullopt;
        return zone->configuration().canonical_windows_registry_key;
    };
    ports.now_utc_seconds = [this] { return now_utc_seconds(); };
    ports.account_audit = [this](auto reason, auto actor) { return account_audit_locked(reason, actor); };
    ports.authentication_audit = [this](auto actor) { return authentication_audit_locked(actor); };
    ports.publish_configuration = [this](const auto& candidate) {
        return platform::win32::publish_operational_configuration_file(installation_root_, owner_sid_, candidate);
    };
    ports.verified_starter_doors = [this] {
        return platform::win32::installation::verified_fresh_starter_doors(installation_root_, owner_sid_);
    };
    const auto transaction = run_runtime_bootstrap_transaction(*storage_, *authentication_, request,
        configuration_, presentation_state_, diagnostic_code_, ports);
    if (transaction.code != LocalControlResponseCode::ok) return transaction;
    if (!retire_first_run_ticket(installation_root_, owner_sid_) && journal_)
        journal_->problem(application::AttentionKind::work_preserved, "First-run setup", "first_run_intent_retained");
    refresh_state_locked();
    if (presentation_state_ == AlphaRuntimePresentationState::ready &&
        !start_listener_locked()) {
        return {LocalControlResponseCode::recovery_required, diagnostic_code_};
    }
    if (presentation_state_ == AlphaRuntimePresentationState::ready) {
        (void)initialize_scheduler_locked();
    }
    return presentation_state_ == AlphaRuntimePresentationState::ready
        ? AlphaControlResponse{LocalControlResponseCode::ok, "bootstrap_complete"}
        : AlphaControlResponse{LocalControlResponseCode::recovery_required, diagnostic_code_};
}

AlphaControlResponse AlphaRuntimeHost::command_lifecycle(
    const application::AlphaLifecycleCommandRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (application::encode_lifecycle_command_request(request).empty() || !telnet_listener_)
        return {LocalControlResponseCode::invalid_request, "lifecycle_command_invalid"};
    if (request.command == application::AlphaLifecycleCommandKind::cancel_drain) {
        if (lifecycle_.state() != application::RuntimeState::draining)
            return {LocalControlResponseCode::invalid_request, "drain_no_longer_cancellable"};
        if (!telnet_listener_->set_draining(false)) return fail_recovery_locked("caller_cleanup_failed");
        (void)lifecycle_.cancel_shutdown();
        scheduler_paused_.store(false);
        scheduler_wake_.notify_all();
        presentation_state_ = AlphaRuntimePresentationState::ready;
        diagnostic_code_ = "drain_cancelled";
        if (journal_) journal_->emit({application::OperationalKind::drain_cancelled,{}, {}});
        return {LocalControlResponseCode::ok, "drain_cancelled"};
    }
    if (presentation_state_ != AlphaRuntimePresentationState::ready ||
        lifecycle_.state() != application::RuntimeState::online)
        return {LocalControlResponseCode::invalid_request, "runtime_not_ready_to_drain"};
    // Freeze admission before counting work, under the listener's assignment
    // lock. The scheduler freeze is serialized with its own due/start boundary.
    const auto deadline = GetTickCount64() + static_cast<std::uint64_t>(request.timeout_seconds) * 1000;
    if (!telnet_listener_->set_draining(true, deadline)) return fail_recovery_locked("caller_cleanup_failed");
    {
        std::lock_guard<std::mutex> scheduler_lock(scheduler_mutex_);
        scheduler_paused_.store(true);
    }
    application::ShutdownRequest shutdown;
    shutdown.disposition = application::ShutdownDisposition::graceful_drain;
    shutdown.active_work.caller_sessions = telnet_listener_->status().active_connections;
    shutdown.active_work.supervised_processes = scheduler_has_work() ? 1 : 0;
    shutdown.drain_deadline = {deadline};
    if (!lifecycle_.request_shutdown(shutdown, GetTickCount64()))
        return fail_recovery_locked("drain_transition_failed");
    presentation_state_ = AlphaRuntimePresentationState::draining;
    diagnostic_code_ = "draining_new_callers_refused";
    if (journal_) journal_->emit({application::OperationalKind::drain_started,{}, {}});
    lifecycle_wake_.notify_all();
    return {LocalControlResponseCode::ok, "drain_started"};
}

void AlphaRuntimeHost::lifecycle_loop() noexcept {
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!lifecycle_worker_stop_) {
            lifecycle_wake_.wait_for(lock, std::chrono::seconds(1), [this] { return lifecycle_worker_stop_ ||
                presentation_state_ == AlphaRuntimePresentationState::draining; });
            if (lifecycle_worker_stop_) break;
            initialize_ftn_locked();
            if(file_administration_)file_administration_->expire_idle_views();
            if(presentation_state_!=AlphaRuntimePresentationState::draining)continue;
            const auto callers = telnet_listener_ ? telnet_listener_->status().active_connections : 0;
            const bool jobs = scheduler_has_work();
            if (lifecycle_.state() == application::RuntimeState::draining)
                (void)lifecycle_.observe_active_work({static_cast<std::uint16_t>(callers),
                    static_cast<std::uint16_t>(jobs ? 1 : 0)}, GetTickCount64());
            if (lifecycle_.state() == application::RuntimeState::stopping) {
                lock.unlock();
                (void)shutdown();
                lock.lock();
            } else {
                lifecycle_wake_.wait_for(lock, std::chrono::milliseconds(100));
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        lifecycle_.report_failure();
        presentation_state_ = AlphaRuntimePresentationState::faulted;
        diagnostic_code_ = "lifecycle_worker_failed";
    }
}

AlphaControlResponse AlphaRuntimeHost::shutdown() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (lifecycle_.state() == application::RuntimeState::stopped) {
        return {LocalControlResponseCode::ok, "shutdown_already_complete"};
    }
    if (presentation_state_ == AlphaRuntimePresentationState::stopping) {
        return {LocalControlResponseCode::shutting_down, "shutdown_in_progress"};
    }
    application::ShutdownRequest request;
    request.disposition = application::ShutdownDisposition::now;
    request.active_work.caller_sessions = telnet_listener_
        ? telnet_listener_->status().active_connections : 0;
    if ((lifecycle_.state() == application::RuntimeState::online ||
            lifecycle_.state() == application::RuntimeState::draining) &&
        !lifecycle_.request_shutdown(request, GetTickCount64())) {
        return {LocalControlResponseCode::internal_failure, "shutdown_transition_rejected"};
    }
    presentation_state_ = AlphaRuntimePresentationState::stopping;
    diagnostic_code_ = "stopping";
    if (journal_) journal_->emit({application::OperationalKind::runtime_stopping, {}, {}});
    // Retain the same authoritative service for read-only status while the
    // shutdown owner cancels and joins outside the host lock. Never fabricate
    // zero callers or replacement node rows before their cleanup has finished.
    const auto listener = telnet_listener_;
    const auto file_administration=file_administration_;
    auto external_events = std::move(external_events_);
    application::AlphaRuntimeStatistics stopped_statistics = statistics_;
    lock.unlock();
    if (external_events) {
        if (listener) (void)listener->set_draining(true);
        external_events->stop();
    }
    stop_scheduler();
    if(ftn_engine_)ftn_engine_->stop();
    stop_clamav();
    if(file_administration)file_administration->stop();
    std::vector<application::AlphaOperationalLogEntry> stopped_logs;
    bool caller_cleanup_failed = false;
    if (listener) {
        listener->stop();
        const auto stopped = listener->status();
        stopped_logs = stopped.logs;
        stopped_statistics = stopped.statistics;
        caller_cleanup_failed = stopped.diagnostic_code == "caller_cleanup_failed";
    }
    if (botgate_feed_) botgate_feed_->stop();
    if (journal_) (void)journal_->flush();
    lock.lock();
    if (!stopped_logs.empty()) retained_logs_ = std::move(stopped_logs);
    statistics_ = stopped_statistics;
    bool storage_cleanup_failed = false;
    if (storage_) {
        const auto checkpointed = storage_->checkpoint();
        const auto stopped = storage_->shutdown();
        storage_cleanup_failed = !checkpointed || !stopped;
    }
    // Every producer has stopped before capturing the copy-recovery namespace.
    // An incomplete capture retains its prior usable generation and warns; it
    // never manufactures job completion or changes a durable SQL receipt.
    if(!caller_cleanup_failed&&!storage_cleanup_failed){
        const auto snapshot=platform::win32::refresh_portable_file_snapshot(installation_root_,owner_sid_);
        if((!snapshot.published||snapshot.deferred)&&journal_)
            journal_->problem(application::AttentionKind::work_preserved,"Portable file identity snapshot","stopped_file_snapshot_incomplete_preserved");
    }else if(journal_)journal_->problem(application::AttentionKind::work_preserved,"Stopped Runtime work","stopped_work_not_fully_confirmed_preserved");
    if(journal_)(void)journal_->flush();
    if(durable_journal_)durable_journal_->stop();
    file_administration_.reset();
    // A storage failure must not skip draining the remaining shared worker service.
    kdf_.shutdown();
    if (caller_cleanup_failed || storage_cleanup_failed) {
        (void)lifecycle_.report_failure();
        presentation_state_ = AlphaRuntimePresentationState::faulted;
        diagnostic_code_ = caller_cleanup_failed ? "caller_cleanup_failed" : "storage_shutdown_failed";
        return {LocalControlResponseCode::storage_failure, diagnostic_code_};
    }
    if (lifecycle_.state() == application::RuntimeState::stopping) (void)lifecycle_.report_stopped();
    if (journal_) {
        journal_->emit({application::OperationalKind::runtime_stopped, {}, {}});
        (void)journal_->flush();
    }
    return {LocalControlResponseCode::ok, "shutdown_complete"};
}

}  // namespace sfx::apps::runtime
