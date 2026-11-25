#pragma once
#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/file_receive_budget.hpp"

namespace sfx::platform::win32 {
enum class ClamAvResourceKind { scan, definition_update };
// These native-only fault ceilings can REDUCE observed resources, never raise
// them. There is no corresponding caller/IPC/configuration success override.
struct ClamAvResourceCeilings {
    std::optional<std::uint64_t> free_memory,free_commit,free_disk;
};
// One scan OR update per private Work/ClamAV owner. An explicitly inherited
// read-only lock handle keeps admission closed until the actual child dies,
// even if its supervising parent exits. Disk allowances use the same per-volume
// process-identity ledger as incoming transfers. Unknown liveness stays charged.
class ClamAvResourceLease final {
public:
    static std::unique_ptr<ClamAvResourceLease> acquire(
        const FilePayloadDirectory& scratch,const std::wstring& owner,
        std::uint64_t maximum_scratch_bytes,const std::function<bool()>& cancelled={},
        const std::function<void(std::uint64_t)>& activity={},
        std::uint32_t wait_milliseconds=180000,const ClamAvResourceCeilings& ceilings={},
        ClamAvResourceKind kind=ClamAvResourceKind::scan);
    // Updates wait on the existing Work/ClamAV gate BEFORE opening vendor logs
    // or creating scratch. The same per-volume budget is reserved on this parent.
    static std::unique_ptr<ClamAvResourceLease> acquire_update(
        const FilePayloadDirectory& work_parent,const std::wstring& owner,
        std::uint64_t maximum_scratch_bytes,const std::function<bool()>& cancelled={},
        const std::function<void(std::uint64_t)>& activity={},
        std::uint32_t wait_milliseconds=180000,const ClamAvResourceCeilings& ceilings={});
    ~ClamAvResourceLease();
    installation::Handle inherit_lock()const;
    installation::Handle inherit_budget()const;
    void bind_child(HANDLE process);
    // Recheck current headroom; never raise the initial allowance or the old
    // 2GiB-minus64KiB cap. Keep256MiB commit outside this child's Job allowance.
    std::uint64_t child_memory_limit()const;
private:
    friend class ClamAvScratch;
    struct Impl;
    static std::unique_ptr<ClamAvResourceLease> acquire_in_directory(
        const FilePayloadDirectory& parent,const FilePayloadDirectory& budget_directory,
        const std::wstring& owner,std::uint64_t maximum,const std::function<bool()>& cancelled,
        const std::function<void(std::uint64_t)>& activity,std::uint32_t wait,
        const ClamAvResourceCeilings& ceilings,ClamAvResourceKind kind);
    ClamAvResourceLease();
    std::unique_ptr<Impl> impl_;
};
// Only a NEW exact private scratch directory can be cleaned by this capability.
// Cleanup requires admission: reacquire it to exclude inherited child keepers,
// or retain the exact owning lease with pinned actual-child exit proof.
// Inventory is bounded and entirely verified BEFORE deletion; no reparse points,
// hardlinks, library roots, preexisting job folders or arbitrary recursive erase.
// A failed cleanup retains attention state, never claims the directory is gone.
class ClamAvScratch final {
public:
    static std::unique_ptr<ClamAvScratch> create(const std::filesystem::path& work_parent,
        const std::wstring& owner,bool update=false);
    ~ClamAvScratch();
    const std::filesystem::path& path()const noexcept;
    bool cleanup()noexcept;
    // Keep the caller's exact operation gate until cleanup finishes. Actual
    // bound-child exit is checked via a pinned process handle; no success flag.
    bool cleanup(ClamAvResourceLease&)noexcept;
private:
    struct Impl;
    ClamAvScratch();
    bool cleanup_held()noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::win32
