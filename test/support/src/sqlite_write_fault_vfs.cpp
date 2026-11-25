#include "sfx/test/sqlite_write_fault_vfs.hpp"

#include "sqlite3.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

namespace sfx::test {
namespace {

std::atomic<int> write_fault{SQLITE_OK};
std::atomic_bool wal_only{false};
std::string target_marker;

struct FaultInjectingFile {
    sqlite3_file base{};
    sqlite3_file* delegate{nullptr};
    bool target{false};
    bool wal{false};
};

FaultInjectingFile* fault_file(sqlite3_file* file) noexcept {
    return reinterpret_cast<FaultInjectingFile*>(file);
}

sqlite3_vfs fault_vfs{};
sqlite3_vfs* delegate_vfs{nullptr};

sqlite3_vfs* vfs_delegate() noexcept {
    return static_cast<sqlite3_vfs*>(fault_vfs.pAppData);
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

int fault_write(sqlite3_file* file, const void* buffer,
    int amount, sqlite3_int64 offset) {
    auto* wrapper = fault_file(file);
    if (wrapper->target && (!wal_only.load() || wrapper->wal)) {
        const int injected = write_fault.exchange(SQLITE_OK);
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

int fault_shm_map(sqlite3_file* file, int page, int page_size,
    int extend, void volatile** output) {
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

int fault_fetch(sqlite3_file* file, sqlite3_int64 offset,
    int amount, void** output) {
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

int fault_open(sqlite3_vfs*, const char* name, sqlite3_file* file,
    int flags, int* output_flags) {
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
    wrapper->target = name != nullptr && !target_marker.empty() &&
        std::strstr(name, target_marker.c_str()) != nullptr;
    wrapper->wal=(flags & SQLITE_OPEN_WAL)!=0;
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

int fault_full_path(sqlite3_vfs*, const char* name, int output_size, char* output) {
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

}  // namespace

bool install_sqlite_write_fault_vfs(const std::string_view target_path_marker) {
    if (target_path_marker.empty() || target_path_marker.find('\0') !=
            std::string_view::npos || delegate_vfs != nullptr) return false;
    delegate_vfs = sqlite3_vfs_find(nullptr);
    if (delegate_vfs == nullptr) return false;
    target_marker.assign(target_path_marker);
    constexpr std::size_t alignment = alignof(std::max_align_t);
    constexpr std::size_t delegate_offset =
        (sizeof(FaultInjectingFile) + alignment - 1U) & ~(alignment - 1U);
    fault_vfs.iVersion = delegate_vfs->iVersion;
    fault_vfs.szOsFile = static_cast<int>(
        delegate_offset + static_cast<std::size_t>(delegate_vfs->szOsFile));
    fault_vfs.mxPathname = delegate_vfs->mxPathname;
    fault_vfs.zName = "sfx-window6c-fault-vfs";
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
    if (sqlite3_vfs_register(&fault_vfs, 1) != SQLITE_OK) {
        target_marker.clear();
        delegate_vfs = nullptr;
        fault_vfs = {};
        return false;
    }
    return true;
}

void inject_next_sqlite_write_fault(const int sqlite_result) noexcept {
    wal_only.store(false);
    write_fault.store(sqlite_result);
}

void inject_next_sqlite_wal_write_fault(const int sqlite_result) noexcept {
    wal_only.store(true);write_fault.store(sqlite_result);
}

int pending_sqlite_write_fault() noexcept {
    return write_fault.load();
}

void uninstall_sqlite_write_fault_vfs() noexcept {
    wal_only.store(false);
    write_fault.store(SQLITE_OK);
    if (delegate_vfs != nullptr) {
        (void)sqlite3_vfs_register(delegate_vfs, 1);
        (void)sqlite3_vfs_unregister(&fault_vfs);
    }
    target_marker.clear();
    delegate_vfs = nullptr;
    fault_vfs = {};
}

}  // namespace sfx::test
