#pragma once
#include "sfx/platform/win32/installation_security.hpp"
#include <memory>

namespace sfx::platform::win32 {
// Native capabilities only, never an IPC/caller supplied reservation. Whole
// maxima are conservatively charged until seal/reap, even while bytes exist on
// disk. Shared only by this operator, Windows session and actual volume GUID.
// External programs/other sessions can still consume space; IO errors remain
// failures, not accepted transfers. Unknown child liveness retains the charge.
class FileReceiveBudget final : public std::enable_shared_from_this<FileReceiveBudget> {
public:
    static std::shared_ptr<FileReceiveBudget> reserve(HANDLE directory,
        const std::wstring& owner,std::uint64_t available,std::uint64_t maximum);
    static std::shared_ptr<FileReceiveBudget> for_transfer(HANDLE empty_file,
        const std::wstring& owner,std::uint64_t maximum);
    static std::uint64_t available(HANDLE directory,const std::wstring& owner,
        std::uint64_t physical_available);
    ~FileReceiveBudget();
    FileReceiveBudget(const FileReceiveBudget&)=delete;
    void bind_file(HANDLE);
    // Called while the child is still suspended and before ResumeThread.
    void bind_child(HANDLE process);
    // A READ-ONLY mapping handle, explicitly allowed into that child, keeps
    // charges discoverable if the parent dies before its job kills the child.
    installation::Handle inherit_keeper() const;
    void require_write(std::uint64_t physical_available,std::uint64_t bytes) const;
    void seal();
private:
    struct Impl;
    FileReceiveBudget();
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::win32
