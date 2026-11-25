#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite_internal.hpp"
#include "sfx/application/account_storage.hpp"

namespace sfx::adapters::storage::internal {

class SqliteStatement final {
public:
    SqliteStatement(sqlite3* database, const char* sql, std::string operation)
        : database_(database), operation_(std::move(operation)) {
        require(sqlite3_prepare_v3(
            database_, sql, -1, SQLITE_PREPARE_PERSISTENT, &statement_, nullptr));
    }

    ~SqliteStatement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    void bind(const int index, const std::int64_t value) { require(sqlite3_bind_int64(statement_, index, value)); }
    void bind(const int index, const std::uint16_t value) { require(sqlite3_bind_int(statement_, index, value)); }
    void bind(const int index, const bool value) { require(sqlite3_bind_int(statement_, index, value ? 1 : 0)); }
    void bind(const int index, const std::string_view value) {
        require(sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    void bind(const int index, const std::string& value) { bind(index, std::string_view(value)); }
    void bind(const int index, const application::Identifier128& value) {
        require(sqlite3_bind_blob(statement_, index, value.bytes.data(),
            static_cast<int>(value.bytes.size()), SQLITE_TRANSIENT));
    }
    void bind(const int index, const std::vector<std::uint8_t>& value) {
        const void* const data = value.empty() ? static_cast<const void*>("") : value.data();
        require(sqlite3_bind_blob(statement_, index, data, static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    template <std::size_t Size>
    void bind(const int index, const std::array<std::uint8_t, Size>& value) {
        require(sqlite3_bind_blob(statement_, index, value.data(),
            static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    void bind_null(const int index) { require(sqlite3_bind_null(statement_, index)); }

    int step() {
        const int code = sqlite3_step(statement_);
        if (code != SQLITE_ROW && code != SQLITE_DONE) {
            require(code);
        }
        return code;
    }

    bool row() { return step() == SQLITE_ROW; }
    std::int64_t int64(const int column) const { return sqlite3_column_int64(statement_, column); }
    bool boolean(const int column) const { return sqlite3_column_int(statement_, column) != 0; }
    bool is_null(const int column) const { return sqlite3_column_type(statement_, column) == SQLITE_NULL; }
    std::string text(const int column) const {
        const unsigned char* const value = sqlite3_column_text(statement_, column);
        const int bytes = sqlite3_column_bytes(statement_, column);
        return value == nullptr ? std::string{} :
            std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes));
    }
    std::vector<std::uint8_t> blob(const int column) const {
        const void* const value = sqlite3_column_blob(statement_, column);
        const int bytes = sqlite3_column_bytes(statement_, column);
        if (value == nullptr || bytes <= 0) {
            return {};
        }
        const auto* const begin = static_cast<const std::uint8_t*>(value);
        return {begin, begin + bytes};
    }
    application::Identifier128 identifier(const int column) const {
        application::Identifier128 value;
        const void* const bytes = sqlite3_column_blob(statement_, column);
        if (bytes != nullptr && sqlite3_column_bytes(statement_, column) ==
            static_cast<int>(value.bytes.size())) {
            std::memcpy(value.bytes.data(), bytes, value.bytes.size());
        }
        return value;
    }

    sqlite3_stmt* get() noexcept { return statement_; }

private:
    void require(const int code) const {
        if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) {
            return;
        }
        throw StorageFailure(
            map_sqlite_error(code),
            operation_,
            code,
            sqlite3_errmsg(database_));
    }

    sqlite3* database_{nullptr};
    sqlite3_stmt* statement_{nullptr};
    std::string operation_;
};

inline void bind_optional_text(
    SqliteStatement& statement,
    const int index,
    const std::optional<std::string>& value) {
    if (value.has_value()) {
        statement.bind(index, *value);
    } else {
        statement.bind_null(index);
    }
}

inline void bind_optional_account_id(
    SqliteStatement& statement,
    const int index,
    const std::optional<application::AccountId>& value) {
    if (value.has_value()) {
        statement.bind(index, *value);
    } else {
        statement.bind_null(index);
    }
}

}  // namespace sfx::adapters::storage::internal
