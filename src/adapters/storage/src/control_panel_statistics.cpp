#include "sfx/adapters/storage/control_panel_statistics.hpp"

#include <sqlite3.h>

#include <array>
#include <limits>
#include <memory>
#include <string>

namespace sfx::adapters::storage {
namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

bool read_pair(sqlite3* database, const char* sql, const std::int64_t begin,
    const std::int64_t end, std::uint64_t& today, std::uint64_t& total) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) return false;
    Statement statement(raw, &sqlite3_finalize);
    if (sqlite3_bind_int64(raw, 1, begin) != SQLITE_OK ||
        sqlite3_bind_int64(raw, 2, end) != SQLITE_OK || sqlite3_step(raw) != SQLITE_ROW) return false;
    const sqlite3_int64 today_value = sqlite3_column_int64(raw, 0);
    const sqlite3_int64 total_value = sqlite3_column_int64(raw, 1);
    if (today_value < 0 || total_value < 0 || sqlite3_step(raw) != SQLITE_DONE) return false;
    today = static_cast<std::uint64_t>(today_value);
    total = static_cast<std::uint64_t>(total_value);
    return true;
}

}  // namespace

application::StorageResult<application::AlphaRuntimeStatistics> ControlPanelStatisticsReader::read_checked(
    const std::filesystem::path& database_path,
    const std::int64_t today_begin_utc_seconds,
    const std::int64_t tomorrow_begin_utc_seconds,
    const std::function<void()>& first_category_read) noexcept {
    application::AlphaRuntimeStatistics result;
    const auto failure = [](const char* stage="snapshot",int code=0) { return application::StorageResult<application::AlphaRuntimeStatistics>{
        {application::StorageErrorCode::io, "read coherent runtime statistics: "+std::string(stage)+"/"+std::to_string(code)}, {}}; };
    if (today_begin_utc_seconds >= tomorrow_begin_utc_seconds) return failure();
    try {
        sqlite3* raw = nullptr;
        const std::string path = database_path.u8string();
        if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW,
                nullptr) != SQLITE_OK) {
            const auto code=raw?sqlite3_extended_errcode(raw):SQLITE_CANTOPEN;
            if (raw != nullptr) sqlite3_close_v2(raw);
            return failure("open",code);
        }
        const auto close = [](sqlite3* value) { if (value != nullptr) sqlite3_close_v2(value); };
        std::unique_ptr<sqlite3, decltype(close)> database(raw, close);
        (void)sqlite3_busy_timeout(raw, 1000);
        int enabled = 0;
        if (sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, &enabled) != SQLITE_OK ||
            sqlite3_exec(raw, "PRAGMA query_only=ON; BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return failure("begin",sqlite3_extended_errcode(raw));
        }
        sqlite3_stmt* version_raw=nullptr;
        if(sqlite3_prepare_v2(raw,"PRAGMA user_version",-1,&version_raw,nullptr)!=SQLITE_OK)return failure();
        Statement version(version_raw,&sqlite3_finalize);if(sqlite3_step(version_raw)!=SQLITE_ROW)return failure();
        const std::string filter=sqlite3_column_int64(version_raw,0)>=19?" WHERE NOT EXISTS(SELECT 1 FROM sfx_chat_usage_exclusions x WHERE x.session_id=sfx_account_sessions.session_id)":"";
        const std::string logons_sql=
            "SELECT count(CASE WHEN started_utc_seconds>=?1 AND started_utc_seconds<?2 THEN 1 END),count(*) FROM sfx_account_sessions"+filter;
        result.successful_logons.available = read_pair(raw,
            logons_sql.c_str(),
            today_begin_utc_seconds, tomorrow_begin_utc_seconds,
            result.successful_logons.today, result.successful_logons.total);
        if(!result.successful_logons.available)return failure("logons",sqlite3_extended_errcode(raw));
        // Private deterministic snapshot interlock; production leaves empty.
        if (first_category_read) first_category_read();
        const std::string seconds_sql=
            "SELECT coalesce(sum(CASE WHEN persisted_seconds>0 AND started_utc_seconds<?2 "
            "AND started_utc_seconds+persisted_seconds>?1 THEN "
            "min(started_utc_seconds+persisted_seconds,?2)-max(started_utc_seconds,?1) "
            "ELSE 0 END),0),coalesce(sum(persisted_seconds),0) "
            "FROM sfx_account_sessions"+filter;
        result.caller_seconds.available = read_pair(raw,seconds_sql.c_str(),
            today_begin_utc_seconds, tomorrow_begin_utc_seconds,
            result.caller_seconds.today, result.caller_seconds.total);
        if(!result.caller_seconds.available)return failure("seconds",sqlite3_extended_errcode(raw));
        result.new_users.available = read_pair(raw,
            "SELECT count(CASE WHEN created_utc_seconds>=?1 AND created_utc_seconds<?2 THEN 1 END),"
            "count(*) FROM sfx_accounts",
            today_begin_utc_seconds, tomorrow_begin_utc_seconds,
            result.new_users.today, result.new_users.total);
        if(!result.new_users.available)return failure("new_users",sqlite3_extended_errcode(raw));
        if (!result.successful_logons.available || !result.caller_seconds.available ||
            !result.new_users.available ||
            sqlite3_exec(raw, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) return failure("commit",sqlite3_extended_errcode(raw));
    } catch (...) {
        return failure();
    }
    return {application::StorageStatus::success(), result};
}

application::AlphaRuntimeStatistics ControlPanelStatisticsReader::read(
    const std::filesystem::path& path, std::int64_t begin, std::int64_t end) noexcept {
    const auto result = read_checked(path, begin, end);
    return result && result.value ? *result.value : application::AlphaRuntimeStatistics{};
}

}  // namespace sfx::adapters::storage
