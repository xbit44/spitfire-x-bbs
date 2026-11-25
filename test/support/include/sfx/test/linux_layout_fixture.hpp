#pragma once
#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>

namespace sfx::test {
// Component tests construct synthetic private roles directly. Represent the
// installer/provisioner's schema marker explicitly before initializing keys
// and portable file maps; never retrofit a native installed test board.
inline void mark_provisioned_linux_fixture(const std::filesystem::path& root) {
    namespace host=platform::linux_host;
    auto config=host::open_owned_directory(root/"config");struct stat current{};
    if(fstatat(config.get(),"linux-layout.v1",&current,AT_SYMLINK_NOFOLLOW)!=0){
        if(errno!=ENOENT)throw std::runtime_error("fixture_layout_marker_unavailable");
        const auto text=host::layout::marker_text;
        host::publish_private_file(config.get(),"linux-layout.v1",{text.begin(),text.end()},false);
    }
    host::layout::require_current(root);
}
}
