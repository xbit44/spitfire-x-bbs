#pragma once
namespace sfx::platform::linux_host {
// In a newly forked Door/helper child only, before exec. Linux kernel calls
// clear ambient/permitted/effective/inheritable rights and forbid later gain.
bool drop_child_capabilities() noexcept;
// AMD64 vendor updater only: allow resolver threads in this same process, while
// denying fork/vfork/process clones and namespace changes. Must run after
// drop_child_capabilities in the newly forked child, before its pinned exec.
bool restrict_child_to_threads() noexcept;
}
