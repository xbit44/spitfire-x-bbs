#pragma once

#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <atomic>
#include <functional>

namespace sfx::platform::linux_host {

// Same bounded typed frames as Windows, over an owner-only Linux connection.
// The private endpoint record lives in Management; the abstract socket has no
// installation path-length limit and disappears automatically on process exit.
class LinuxLocalControlServer final {
public:
    using Handler=std::function<std::optional<application::LocalControlFrame>(application::LocalControlFrame&)>;
    explicit LinuxLocalControlServer(std::filesystem::path management_directory);
    ~LinuxLocalControlServer();
    LinuxLocalControlServer(const LinuxLocalControlServer&)=delete;
    LinuxLocalControlServer& operator=(const LinuxLocalControlServer&)=delete;
    bool open();
    // Called by the owning control thread. Slow/incomplete peers have one total
    // five-second frame deadline; stop interrupts transport waits. A dispatched
    // typed transaction finishes normally before ownership can be released.
    bool serve_one(const std::atomic<bool>& stop, const Handler&);
    void close() noexcept;
private:
    std::filesystem::path directory_;
    InstallationLock ownership_;
    FileDescriptor listener_, directory_pin_;
};

std::optional<application::LocalControlFrame> exchange_local_control(
    const std::filesystem::path& management_directory,
    const application::LocalControlFrame& request,
    application::LocalControlMessageType expected_response,
    const std::atomic<bool>& stop, std::uint32_t timeout_millis=5000);

} // namespace sfx::platform::linux_host
