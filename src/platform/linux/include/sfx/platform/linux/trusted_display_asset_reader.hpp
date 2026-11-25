#pragma once
#include "sfx/application/display.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <atomic>
#include <memory>

namespace sfx::platform::linux_host {
class LinuxTrustedDisplayAssetReader final:public application::ITrustedDisplayAssetReader {
public:
    static std::unique_ptr<LinuxTrustedDisplayAssetReader> open(std::filesystem::path);
    application::TrustedDisplayAssetReadResult read_asset(std::string_view,std::size_t) override;
private:
    LinuxTrustedDisplayAssetReader(std::filesystem::path root,FileDescriptor directory)
        :root_(std::move(root)),directory_(std::move(directory)){}
    std::filesystem::path root_;
    FileDescriptor directory_;
    std::atomic<bool> invalidated_{false};
};
} // namespace sfx::platform::linux_host
