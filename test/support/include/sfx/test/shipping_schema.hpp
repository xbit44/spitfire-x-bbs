#pragma once

#include <cstddef>
#include <cstdint>

namespace sfx::test {

// Keep the expected shipping version independent of the production migration plan.
inline constexpr std::int32_t shipping_schema_version = 31;
inline constexpr std::size_t shipping_migration_count = shipping_schema_version;

}  // namespace sfx::test
