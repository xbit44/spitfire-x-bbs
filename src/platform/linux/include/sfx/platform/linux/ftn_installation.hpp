#pragma once
#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"
namespace sfx::platform::linux_host {
// Host composition capability. Peer/caller input cannot construct this context.
struct FtnInstallation {
    std::filesystem::path root;
    std::shared_ptr<IFilePayloadBindings> bindings;
};
void verify_ftn_directory(const FtnInstallation&);
FilePayloadDirectory create_ftn_directory(const FtnInstallation&,const std::filesystem::path&);
}
