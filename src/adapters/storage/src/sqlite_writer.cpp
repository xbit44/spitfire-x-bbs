#include "sqlite_internal.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace sfx::adapters::storage::internal {
namespace {

enum class QueueItemKind : std::uint8_t { write, checkpoint, backup, ftn_retention_expiry };

struct QueueItem final {
    QueueItemKind kind{QueueItemKind::write};
    WriteRequest write;
    std::shared_ptr<application::FtnRetentionRemoval> expiry;
    bool truncate_checkpoint{false};
    std::filesystem::path backup_file;
    std::promise<application::StorageStatus> status_promise;
    std::promise<InternalBackupResult> backup_promise;
    std::size_t estimated_bytes{1};
};

std::future<application::StorageStatus> ready_status_future(application::StorageStatus status) {
    std::promise<application::StorageStatus> promise;
    std::future<application::StorageStatus> future = promise.get_future();
    promise.set_value(std::move(status));
    return future;
}

int exec_code(sqlite3* const database, const char* const sql) {
    char* error = nullptr;
    const int code = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    sqlite3_free(error);
    return code;
}

application::StorageStatus status_for_sqlite(
    sqlite3* const database,
    const int code,
    const std::string& operation) {
    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) {
        return application::StorageStatus::success();
    }
    (void)database;
    return {map_sqlite_error(code), operation};
}

}  // namespace

class SqliteWriterCoordinator::Impl final {
public:
    application::StorageStatus start(
        std::filesystem::path database_path,
        SqliteStorageSettings settings,
        std::vector<SqliteMigration> migrations,
        SqlitePagerTelemetry& telemetry) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (worker_.joinable() || health_.state != application::StorageRuntimeState::stopped) {
            return {application::StorageErrorCode::internal, "start SQLite writer twice"};
        }
        if (settings.queue.maximum_operations == 0 || settings.queue.maximum_bytes == 0 ||
            settings.queue.dequeue_batch_limit == 0 || settings.busy.maximum_attempts == 0 ||
            !settings.utc_now) {
            return {application::StorageErrorCode::internal, "validate SQLite writer settings"};
        }

        database_path_ = std::move(database_path);
        settings_ = std::move(settings);
        migrations_ = std::move(migrations);
        telemetry_ = &telemetry;
        accepting_ = true;
        stopping_ = false;
        health_ = {};
        health_.state = application::StorageRuntimeState::starting;

        std::promise<application::StorageStatus> startup_promise;
        std::future<application::StorageStatus> startup_future = startup_promise.get_future();
        worker_ = std::thread([this, promise = std::move(startup_promise)]() mutable {
            run(std::move(promise));
        });
        lock.unlock();
        return startup_future.get();
    }

    std::future<application::StorageStatus> submit(WriteRequest request) {
        if (!request.apply || request.operation.empty() || request.estimated_bytes == 0) {
            return ready_status_future({application::StorageErrorCode::internal, "validate SQLite write request"});
        }
        auto item = std::make_shared<QueueItem>();
        item->kind = QueueItemKind::write;
        item->estimated_bytes = request.estimated_bytes;
        item->write = std::move(request);
        std::future<application::StorageStatus> future = item->status_promise.get_future();
        const application::StorageStatus enqueue_status = enqueue(item);
        if (!enqueue_status) {
            item->status_promise.set_value(enqueue_status);
        }
        return future;
    }

    application::StorageStatus checkpoint(const bool truncate) {
        auto item = std::make_shared<QueueItem>();
        item->kind = QueueItemKind::checkpoint;
        item->truncate_checkpoint = truncate;
        std::future<application::StorageStatus> future = item->status_promise.get_future();
        const application::StorageStatus enqueue_status = enqueue(item);
        if (!enqueue_status) {
            return enqueue_status;
        }
        return future.get();
    }

    std::future<application::StorageStatus> submit_ftn_retention_expiry(std::shared_ptr<application::FtnRetentionRemoval> removal){
        if(!removal||!removal->intent().operation.valid()||!removal->intent().dispatch.valid())return ready_status_future({application::StorageErrorCode::invalid_argument,"validate sealed retention dispatch"});
        auto item=std::make_shared<QueueItem>();item->kind=QueueItemKind::ftn_retention_expiry;item->estimated_bytes=8192;item->expiry=std::move(removal);auto future=item->status_promise.get_future();const auto accepted=enqueue(item);if(!accepted)item->status_promise.set_value(accepted);return future;
    }

    InternalBackupResult backup_to(const std::filesystem::path& destination_file) {
        auto item = std::make_shared<QueueItem>();
        item->kind = QueueItemKind::backup;
        item->backup_file = destination_file;
        std::future<InternalBackupResult> future = item->backup_promise.get_future();
        const application::StorageStatus enqueue_status = enqueue(item);
        if (!enqueue_status) {
            return {enqueue_status, 0, 0};
        }
        return future.get();
    }

    application::StorageStatus shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_.joinable()) {
                return shutdown_status_;
            }
            accepting_ = false;
            stopping_ = true;
            if (health_.state != application::StorageRuntimeState::recovery_required) {
                health_.state = application::StorageRuntimeState::stopping;
            }
        }
        queue_ready_.notify_all();
        capacity_ready_.notify_all();
        worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdown_status_;
    }

    application::StorageStatus reset_after_offline_recovery() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_.joinable() || accepting_ || !queue_.empty() || unresolved_expiry_) {
            return {
                application::StorageErrorCode::internal,
                "reset active SQLite writer after offline recovery"};
        }
        stopping_ = false;
        queued_bytes_ = 0;
        health_ = {};
        shutdown_status_ = application::StorageStatus::success();
        return application::StorageStatus::success();
    }

    application::StorageHealth health() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return health_;
    }

private:
    application::StorageStatus enqueue(const std::shared_ptr<QueueItem>& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto has_capacity = [this, &item] {
            return !accepting_ ||
                   (queue_.size() < settings_.queue.maximum_operations &&
                    queued_bytes_ <= settings_.queue.maximum_bytes - std::min(
                        item->estimated_bytes,
                        settings_.queue.maximum_bytes));
        };
        if (item->estimated_bytes > settings_.queue.maximum_bytes) {
            ++health_.rejected_operations;
            return {application::StorageErrorCode::backpressure, "reject oversized SQLite write"};
        }
        if (!capacity_ready_.wait_for(lock, settings_.queue.enqueue_wait, has_capacity)) {
            ++health_.rejected_operations;
            return {application::StorageErrorCode::backpressure, "wait for SQLite write capacity"};
        }
        if (!accepting_) {
            ++health_.rejected_operations;
            return {application::StorageErrorCode::shutting_down, "enqueue SQLite write during shutdown"};
        }
        queue_.push_back(item);
        queued_bytes_ += item->estimated_bytes;
        health_.queued_operations = queue_.size();
        health_.queued_bytes = queued_bytes_;
        health_.queue_high_water_operations = std::max(health_.queue_high_water_operations, queue_.size());
        health_.queue_high_water_bytes = std::max(health_.queue_high_water_bytes, queued_bytes_);
        queue_ready_.notify_one();
        return application::StorageStatus::success();
    }

    void run(std::promise<application::StorageStatus> startup_promise) noexcept {
        bool startup_reported = false;
        bool telemetry_opened = false;
        try {
            SqliteConnection connection(
                database_path_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                settings_.busy.sqlite_timeout,
                true);
            if (settings_.bootstrap_boundary_hook) {
                const application::StorageStatus status =
                    settings_.bootstrap_boundary_hook("after-sqlite-initialization");
                if (!status) {
                    throw StorageFailure(status.code, status.operation, SQLITE_ABORT);
                }
            }
            bootstrap_and_migrate(
                connection,
                migrations_,
                settings_.utc_now,
                settings_.bootstrap_boundary_hook,
                settings_.layout_profile);
            telemetry_->connection_opened(0, connection.get());
            telemetry_opened = true;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                health_.state = application::StorageRuntimeState::ready;
                health_.engine_version = sqlite3_libversion();
                health_.engine_source_id = sqlite3_sourceid();
                health_.schema_version = static_cast<std::int32_t>(
                    connection.scalar_int64("PRAGMA user_version", "read active schema version"));
                shutdown_status_ = application::StorageStatus::success();
            }
            startup_promise.set_value(application::StorageStatus::success());
            startup_reported = true;
            auto last_checkpoint = std::chrono::steady_clock::now();

            for (;;) {
                std::vector<std::shared_ptr<QueueItem>> batch;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    queue_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                    if (queue_.empty() && stopping_) {
                        break;
                    }
                    if (!stopping_ && settings_.queue.dequeue_batch_window.count() > 0) {
                        const auto deadline = std::chrono::steady_clock::now() + settings_.queue.dequeue_batch_window;
                        queue_ready_.wait_until(lock, deadline, [this] {
                            return stopping_ || queue_.size() >= settings_.queue.dequeue_batch_limit;
                        });
                    }
                    while (!queue_.empty() && batch.size() < settings_.queue.dequeue_batch_limit) {
                        std::shared_ptr<QueueItem> item = std::move(queue_.front());
                        queue_.pop_front();
                        queued_bytes_ -= item->estimated_bytes;
                        batch.push_back(std::move(item));
                    }
                    health_.queued_operations = queue_.size();
                    health_.queued_bytes = queued_bytes_;
                }
                capacity_ready_.notify_all();

                bool fail_closed = false;
                for (std::size_t index = 0; index < batch.size();) {
                    if (batch[index]->kind == QueueItemKind::write) {
                        std::size_t end = index + 1U;
                        while (end < batch.size() && batch[end]->kind == QueueItemKind::write) {
                            ++end;
                        }
                        const std::vector<application::StorageStatus> statuses =
                            perform_write_batch(connection.get(), batch, index, end);
                        telemetry_->sample(0, connection.get());
                        for (std::size_t completed = index; completed < end; ++completed) {
                            batch[completed]->status_promise.set_value(statuses[completed - index]);
                        }
                        index = end;
                    } else if(batch[index]->kind==QueueItemKind::ftn_retention_expiry){
                        // A distinct queue kind cannot enter grouped SAVEPOINT
                        // execution or whole-group callback replay.
                        batch[index]->status_promise.set_value(perform_ftn_expiry(connection.get(),batch[index]->expiry));
                        telemetry_->sample(0,connection.get());++index;
                    } else {
                        process_maintenance_item(connection, batch[index]);
                        telemetry_->sample(0, connection.get());
                        ++index;
                    }

                    application::StorageStatus fatal_status;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (health_.state == application::StorageRuntimeState::recovery_required) {
                            fatal_status = {health_.last_error, health_.last_operation};
                            shutdown_status_ = fatal_status;
                            accepting_ = false;
                            stopping_ = true;
                            fail_closed = true;
                        }
                    }
                    if (fail_closed) {
                        for (; index < batch.size(); ++index) {
                            complete_failed_item(batch[index], fatal_status);
                        }
                        fail_pending(fatal_status);
                        capacity_ready_.notify_all();
                        break;
                    }
                }
                if (fail_closed) {
                    break;
                }

                const auto now = std::chrono::steady_clock::now();
                std::error_code size_error;
                const std::uintmax_t wal_bytes = std::filesystem::file_size(
                    std::filesystem::path(database_path_.string() + "-wal"),
                    size_error);
                const bool interval_due = now - last_checkpoint >= settings_.checkpoint.passive_interval;
                const bool frame_due = !size_error && wal_bytes >=
                    static_cast<std::uintmax_t>(settings_.checkpoint.frame_target) * 4096U;
                if (interval_due || frame_due) {
                    const application::StorageStatus status = perform_checkpoint(connection.get(), false);
                    telemetry_->sample(0, connection.get());
                    if (status) {
                        last_checkpoint = now;
                    }
                }
                if (!size_error && wal_bytes > settings_.checkpoint.wal_warning_bytes) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    health_.state = application::StorageRuntimeState::degraded;
                    health_.last_operation = "SQLite WAL exceeded measured warning threshold";
                }
            }

            if(unresolved_expiry_&&unresolved_expiry_->native_->close_at_shutdown())unresolved_expiry_.reset();
            const application::StorageStatus checkpoint_status = health().state==application::StorageRuntimeState::recovery_required?shutdown_status_:perform_checkpoint(connection.get(), true);
            if (!checkpoint_status) {
                std::lock_guard<std::mutex> lock(mutex_);
                shutdown_status_ = checkpoint_status;
            }
            if (settings_.bootstrap_boundary_hook) {
                const application::StorageStatus status =
                    settings_.bootstrap_boundary_hook("before-ordinary-connection-shutdown");
                if (!status) {
                    throw StorageFailure(status.code, status.operation, SQLITE_ABORT);
                }
            }
            telemetry_->connection_closed(0);
            telemetry_opened = false;
            connection.close();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (shutdown_status_) {
                    health_.state = application::StorageRuntimeState::stopped;
                    health_.last_error = application::StorageErrorCode::none;
                    health_.last_operation.clear();
                } else {
                    health_.state = application::StorageRuntimeState::recovery_required;
                }
            }
        } catch (...) {
            if (telemetry_opened && telemetry_ != nullptr) telemetry_->connection_closed(0);
            const application::StorageStatus failure = status_from_current_exception("run SQLite writer");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepting_ = false;
                stopping_ = true;
                health_.state = application::StorageRuntimeState::recovery_required;
                health_.last_error = failure.code;
                health_.last_operation = failure.operation;
                shutdown_status_ = failure;
            }
            if (!startup_reported) {
                startup_promise.set_value(failure);
            }
            fail_pending(failure);
        }
        capacity_ready_.notify_all();
    }

    void process_maintenance_item(
        SqliteConnection& connection,
        const std::shared_ptr<QueueItem>& item) noexcept {
        if (item->kind == QueueItemKind::checkpoint) {
            item->status_promise.set_value(perform_checkpoint(connection.get(), item->truncate_checkpoint));
            return;
        }
        item->backup_promise.set_value(
            backup_database(connection.get(), item->backup_file, settings_.busy.sqlite_timeout, migrations_));
    }

    void quarantine_expiry(const application::StorageStatus& status){
        std::lock_guard<std::mutex> lock(mutex_);health_.state=application::StorageRuntimeState::recovery_required;health_.last_error=status.code;health_.last_operation=status.operation;shutdown_status_=status;
    }
    application::StorageStatus perform_ftn_expiry(sqlite3* database,const std::shared_ptr<application::FtnRetentionRemoval>& removal) noexcept {
        using Code=application::StorageErrorCode;using Native=application::FtnExpiryNativeState;using Outcome=application::FtnExpiryOutcome;
        // Earlier ordinary transactions must have conclusively settled before
        // this barrier. Never BEGIN within a leftover failed group transaction.
        if(!sqlite3_get_autocommit(database)){const application::StorageStatus failure{Code::io,"ftn_retention_prior_transaction_unresolved"};quarantine_expiry(failure);return failure;}
        if(removal->result().state!=Native::not_invoked)return {Code::conflict,"ftn_retention_dispatch_already_consumed"};
        if(removal->cancelled())return {Code::not_due,"ftn_retention_cancelled_before_dispatch"};
        bool begun=false,invoked=false;
        const auto settle=[&](application::StorageStatus failure){
            bool quarantine=false;
            if(begun&&!sqlite3_get_autocommit(database)){
                const int rolled=exec_code(database,"ROLLBACK");
                if(rolled!=SQLITE_OK||!sqlite3_get_autocommit(database)){failure={Code::io,"ftn_retention_rollback_unresolved"};quarantine=true;}
            }
            if(removal->result().state==Native::close_unconfirmed||removal->result().state==Native::invoking){unresolved_expiry_=removal;failure={Code::io,"ftn_retention_native_close_unconfirmed"};quarantine=true;}
            record_write_results({failure});if(quarantine)quarantine_expiry(failure);return failure;
        };
        try{
            int code=SQLITE_BUSY;const auto deadline=std::chrono::steady_clock::now()+settings_.busy.transaction_deadline;
            for(std::uint32_t attempt=0;attempt<settings_.busy.maximum_attempts;++attempt){
                if(removal->cancelled())return {Code::not_due,"ftn_retention_cancelled_before_begin"};
                code=exec_code(database,"BEGIN IMMEDIATE");if(code==SQLITE_OK){begun=true;break;}
                if(!sqlite3_get_autocommit(database)){begun=true;return settle({Code::io,"ftn_retention_begin_state_unresolved"});}
                if(map_sqlite_error(code)!=Code::busy||attempt+1>=settings_.busy.maximum_attempts||std::chrono::steady_clock::now()>=deadline)return settle(status_for_sqlite(database,code,"ftn_retention_begin"));
                {std::lock_guard<std::mutex> lock(mutex_);++health_.busy_retries;}sqlite3_sleep(static_cast<int>(10U*(attempt+1)));
            }
            if(!begun)return settle({Code::busy,"ftn_retention_begin"});
            const auto now=settings_.utc_now();const auto& intent=removal->intent();
            if(removal->cancelled())return settle({Code::not_due,"ftn_retention_cancelled_before_native"});
            if(!validate_ftn_retention_expiry(database,intent,now))return settle({Code::conflict,"ftn_retention_policy_or_reference_changed"});
            // invoke sets its one-shot latch before the first native API. The
            // queue owns the same exclusive verified handle until completion.
            invoked=removal->invoke();if(!invoked)return settle({Code::conflict,"ftn_retention_dispatch_already_consumed"});
            const auto native=removal->result();
            if(native.state!=Native::closed&&native.state!=Native::disposition_failed)return settle({Code::io,"ftn_retention_native_close_unconfirmed"});
            finish_ftn_retention_expiry(database,intent,native.state==Native::closed?Outcome::removed_confirmed:Outcome::native_failed,native.error,now);
            code=exec_code(database,"COMMIT");if(code!=SQLITE_OK||!sqlite3_get_autocommit(database))return settle({Code::io,"ftn_retention_native_outcome_uncertain"});
            begun=false;const auto status=native.state==Native::closed?application::StorageStatus::success():application::StorageStatus{Code::io,"ftn_retention_native_disposition_failed"};record_write_results({status});return status;
        }catch(...){return settle(invoked?application::StorageStatus{Code::io,"ftn_retention_native_outcome_uncertain"}:status_from_current_exception("ftn_retention_authoritative_check"));}
    }

    std::vector<application::StorageStatus> perform_write_batch(
        sqlite3* const database,
        const std::vector<std::shared_ptr<QueueItem>>& batch,
        const std::size_t begin,
        const std::size_t end) noexcept {
        const std::size_t operation_count = end - begin;
        const auto deadline = std::chrono::steady_clock::now() + settings_.busy.transaction_deadline;
        for (std::uint32_t attempt = 0; attempt < settings_.busy.maximum_attempts; ++attempt) {
            std::vector<application::StorageStatus> results(
                operation_count,
                application::StorageStatus::success());
            std::string operation = batch[begin]->write.operation;
            int code = exec_code(database, "BEGIN IMMEDIATE");
            bool completed_all = code == SQLITE_OK;
            if (completed_all) {
                for (std::size_t index = begin; index < end; ++index) {
                    operation = batch[index]->write.operation;
                    code = exec_code(database, "SAVEPOINT sfx_request");
                    if (code != SQLITE_OK) {
                        operation = "begin atomic SQLite write request";
                        completed_all = false;
                        break;
                    }
                    try {
                        code = batch[index]->write.apply(database);
                    } catch (...) {
                        (void)exec_code(database, "ROLLBACK");
                        const application::StorageStatus failure =
                            status_from_current_exception(operation);
                        std::fill(results.begin(), results.end(), failure);
                        record_write_results(results);
                        return results;
                    }

                    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) {
                        code = exec_code(database, "RELEASE sfx_request");
                        if (code != SQLITE_OK) {
                            operation = "release atomic SQLite write request";
                            completed_all = false;
                            break;
                        }
                        continue;
                    }

                    const application::StorageStatus request_failure =
                        status_for_sqlite(database, code, operation);
                    const bool request_local_failure =
                        request_failure.code == application::StorageErrorCode::constraint ||
                        request_failure.code == application::StorageErrorCode::conflict ||
                        request_failure.code == application::StorageErrorCode::not_found ||
                        request_failure.code == application::StorageErrorCode::not_due;
                    if (request_local_failure) {
                        const int rollback_code =
                            exec_code(database, "ROLLBACK TO sfx_request");
                        const int release_code =
                            rollback_code == SQLITE_OK
                                ? exec_code(database, "RELEASE sfx_request")
                                : rollback_code;
                        if (rollback_code == SQLITE_OK && release_code == SQLITE_OK) {
                            results[index - begin] = request_failure;
                            continue;
                        }
                        code = rollback_code != SQLITE_OK ? rollback_code : release_code;
                        operation = "roll back atomic SQLite write request";
                    }
                    completed_all = false;
                    break;
                }
            }

            if (completed_all) {
                operation = "commit SQLite write group";
                code = exec_code(database, "COMMIT");
                if (code == SQLITE_OK) {
                    record_write_results(results);
                    return results;
                }
            }
            (void)exec_code(database, "ROLLBACK");

            const application::StorageStatus failure = status_for_sqlite(database, code, operation);
            if (failure.code == application::StorageErrorCode::busy &&
                attempt + 1U < settings_.busy.maximum_attempts &&
                std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++health_.busy_retries;
                }
                sqlite3_sleep(static_cast<int>(10U * (attempt + 1U)));
                continue;
            }
            std::fill(results.begin(), results.end(), failure);
            record_write_results(results);
            return results;
        }

        std::vector<application::StorageStatus> results(
            operation_count,
            {application::StorageErrorCode::busy, batch[begin]->write.operation});
        record_write_results(results);
        return results;
    }

    application::StorageStatus perform_checkpoint(sqlite3* const database, const bool truncate) noexcept {
        int log_frames = 0;
        int checkpointed_frames = 0;
        const int code = sqlite3_wal_checkpoint_v2(
            database,
            "main",
            truncate ? SQLITE_CHECKPOINT_TRUNCATE : SQLITE_CHECKPOINT_PASSIVE,
            &log_frames,
            &checkpointed_frames);
        application::StorageStatus status =
            status_for_sqlite(database, code, "checkpoint SQLite WAL");
        const bool incomplete_passive =
            !truncate && status && log_frames >= 0 && checkpointed_frames >= 0 &&
            checkpointed_frames < log_frames;
        if (incomplete_passive) {
            status = {
                application::StorageErrorCode::busy,
                "checkpoint SQLite WAL incomplete: " +
                    std::to_string(checkpointed_frames) + " of " +
                    std::to_string(log_frames) + " frames checkpointed"};
        }

        std::lock_guard<std::mutex> lock(mutex_);
        health_.last_checkpoint_log_frames = log_frames;
        health_.last_checkpointed_frames = checkpointed_frames;
        if (status) {
            ++health_.checkpoints;
            if (health_.state == application::StorageRuntimeState::degraded &&
                (health_.last_operation == "SQLite WAL exceeded measured warning threshold" ||
                 health_.last_operation.rfind("checkpoint SQLite WAL incomplete:", 0) == 0)) {
                health_.state = application::StorageRuntimeState::ready;
                health_.last_error = application::StorageErrorCode::none;
                health_.last_operation.clear();
            }
        } else {
            if (incomplete_passive) {
                ++health_.incomplete_checkpoints;
                health_.state = application::StorageRuntimeState::degraded;
            }
            health_.last_error = status.code;
            health_.last_operation = status.operation;
        }
        return status;
    }

    void record_write_results(
        const std::vector<application::StorageStatus>& results) {
        const std::size_t committed_operations = static_cast<std::size_t>(std::count_if(
            results.begin(),
            results.end(),
            [](const application::StorageStatus& status) { return status.succeeded(); }));

        std::lock_guard<std::mutex> lock(mutex_);
        if (committed_operations > 0) {
            health_.committed_operations += committed_operations;
            ++health_.committed_batches;
            health_.maximum_committed_batch_operations =
                std::max(health_.maximum_committed_batch_operations, committed_operations);
        }
        for (const application::StorageStatus& status : results) {
            if (status) {
                continue;
            }
            if (status.code == application::StorageErrorCode::constraint ||
                status.code == application::StorageErrorCode::conflict ||
                status.code == application::StorageErrorCode::not_found ||
                status.code == application::StorageErrorCode::not_due) {
                continue;
            }
            health_.last_error = status.code;
            health_.last_operation = status.operation;
            if (status.code == application::StorageErrorCode::corrupt ||
                status.code == application::StorageErrorCode::incompatible_schema ||
                status.code == application::StorageErrorCode::migration_failed) {
                health_.state = application::StorageRuntimeState::recovery_required;
            } else if (status.code == application::StorageErrorCode::full ||
                       status.code == application::StorageErrorCode::io ||
                       status.code == application::StorageErrorCode::permission_denied) {
                health_.state = application::StorageRuntimeState::degraded;
            }
        }
    }
    static void complete_failed_item(
        const std::shared_ptr<QueueItem>& item,
        const application::StorageStatus& failure) noexcept {
        if (item->kind == QueueItemKind::backup) {
            item->backup_promise.set_value({failure, 0, 0});
        } else {
            item->status_promise.set_value(failure);
        }
    }

    void fail_pending(const application::StorageStatus& failure) noexcept {
        std::deque<std::shared_ptr<QueueItem>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(queue_);
            queued_bytes_ = 0;
            health_.queued_operations = 0;
            health_.queued_bytes = 0;
        }
        for (const std::shared_ptr<QueueItem>& item : pending) {
            complete_failed_item(item, failure);
        }
    }

    std::filesystem::path database_path_;
    SqliteStorageSettings settings_;
    std::vector<SqliteMigration> migrations_;
    mutable std::mutex mutex_;
    std::condition_variable queue_ready_;
    std::condition_variable capacity_ready_;
    std::deque<std::shared_ptr<QueueItem>> queue_;
    std::size_t queued_bytes_{0};
    std::thread worker_;
    bool accepting_{false};
    bool stopping_{false};
    application::StorageHealth health_;
    application::StorageStatus shutdown_status_;
    SqlitePagerTelemetry* telemetry_{nullptr};
    std::shared_ptr<application::FtnRetentionRemoval> unresolved_expiry_;
};

SqliteWriterCoordinator::SqliteWriterCoordinator() : impl_(std::make_unique<Impl>()) {}
SqliteWriterCoordinator::~SqliteWriterCoordinator() { (void)impl_->shutdown(); }

application::StorageStatus SqliteWriterCoordinator::start(
    std::filesystem::path database_path,
    SqliteStorageSettings settings,
    std::vector<SqliteMigration> migrations,
    SqlitePagerTelemetry& telemetry) {
    return impl_->start(std::move(database_path), std::move(settings),
        std::move(migrations), telemetry);
}

std::future<application::StorageStatus> SqliteWriterCoordinator::submit(WriteRequest request) {
    return impl_->submit(std::move(request));
}
std::future<application::StorageStatus> SqliteWriterCoordinator::submit_ftn_retention_expiry(std::shared_ptr<application::FtnRetentionRemoval> removal){return impl_->submit_ftn_retention_expiry(std::move(removal));}

application::StorageStatus SqliteWriterCoordinator::checkpoint(const bool truncate) {
    return impl_->checkpoint(truncate);
}

InternalBackupResult SqliteWriterCoordinator::backup_to(const std::filesystem::path& destination_file) {
    return impl_->backup_to(destination_file);
}

application::StorageStatus SqliteWriterCoordinator::shutdown() { return impl_->shutdown(); }
application::StorageStatus SqliteWriterCoordinator::reset_after_offline_recovery() {
    return impl_->reset_after_offline_recovery();
}
application::StorageHealth SqliteWriterCoordinator::health() const { return impl_->health(); }

}  // namespace sfx::adapters::storage::internal
