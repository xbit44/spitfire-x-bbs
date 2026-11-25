#pragma once
#include "sfx/application/file_storage.hpp"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace sfx::platform::win32 {
enum class PortableFileBindingState {ready,missing,different_host,unavailable};
struct PortableFileBindingResult {
    PortableFileBindingState state{PortableFileBindingState::unavailable};
    bool published{};std::size_t recorded{},deferred{};
    std::filesystem::path snapshot;
    std::vector<std::string> diagnostics;
};
// Read-only process context. A namespace binding never grants mutation, replay,
// successful completion or cleanup. Actual operations retain current native pins.
PortableFileBindingResult configure_portable_file_bindings(const std::filesystem::path&)noexcept;
// Retained before the first key is published, bound to the exact original
// local root. Missing map on an established/copied root never grants this.
bool portable_file_initialization_pending(const std::filesystem::path&)noexcept;
application::FileObjectIdentity portable_logical_file_identity(const std::filesystem::path&,HANDLE);
// Namespace evidence only: called on an actual exclusive SFX CREATE_NEW handle.
// It never claims a business commit, successful outcome or cleanup authority.
void record_portable_new_file(const std::filesystem::path&,HANDLE);
// Called only after the owned pinned deletion and final absence are confirmed.
void record_portable_removed_file(const std::filesystem::path&,const application::FileObjectIdentity&);
std::filesystem::path resolve_portable_owned_path(const std::filesystem::path& current_root,const std::filesystem::path& configured);
// Stable source spelling for SFX-owned path-derived durable FTN identities.
// This is never a path to open; current access uses resolve_portable_owned_path.
std::filesystem::path portable_logical_owned_path(const std::filesystem::path& current_root,const std::filesystem::path&);
// Stopped update/clean shutdown, or the existing quiescent backup capture.
// Only a verified first update/fresh bootstrap may initialize a missing snapshot.
// Optional output is a new host-owned Work snapshot for a managed backup override.
PortableFileBindingResult refresh_portable_file_snapshot(const std::filesystem::path&,const std::wstring& owner,bool allow_initialize=false,
    const std::filesystem::path& output={},const std::function<bool()>& cancelled={});
// Explicit copied-board preparation: verify bytes using held current handles,
// rebind only proven entries, preserve/flag affected unknown or changed work.
PortableFileBindingResult prepare_copied_file_bindings(const std::filesystem::path&,const std::wstring& owner,
    const std::function<bool()>& cancelled={});
}
