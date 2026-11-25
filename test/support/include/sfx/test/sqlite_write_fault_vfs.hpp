#pragma once

#include <string_view>

namespace sfx::test {

bool install_sqlite_write_fault_vfs(std::string_view target_path_marker);
void inject_next_sqlite_write_fault(int sqlite_result) noexcept;
// Route to a genuine WAL write, not an asynchronous passive-checkpoint DB write.
void inject_next_sqlite_wal_write_fault(int sqlite_result) noexcept;
int pending_sqlite_write_fault() noexcept;
void uninstall_sqlite_write_fault_vfs() noexcept;

}  // namespace sfx::test
