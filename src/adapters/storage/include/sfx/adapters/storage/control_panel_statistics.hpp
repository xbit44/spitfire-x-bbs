#pragma once

#include "sfx/application/local_control_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>

namespace sfx::adapters::storage {

class ControlPanelStatisticsReader final {
public:
    static application::StorageResult<application::AlphaRuntimeStatistics> read_checked(
        const std::filesystem::path& database_path,
        std::int64_t today_begin_utc_seconds,
        std::int64_t tomorrow_begin_utc_seconds,
        const std::function<void()>& first_category_read = {}) noexcept;
    static application::AlphaRuntimeStatistics read(
        const std::filesystem::path& database_path,
        std::int64_t today_begin_utc_seconds,
        std::int64_t tomorrow_begin_utc_seconds) noexcept;
};

}  // namespace sfx::adapters::storage
