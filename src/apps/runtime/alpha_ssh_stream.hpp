#pragma once

#include "sfx/application/phase3a_workflow.hpp"
#include "sfx/application/terminal_stream.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace sfx::apps::runtime {

struct SshTerminalDimensions {
    std::uint16_t columns{80};
    std::uint16_t rows{25};
};

struct AlphaSshBackendStatistics {
    std::uint64_t registration_attempts{0};
    std::uint64_t key_generation_attempts{0};
    std::uint64_t key_load_attempts{0};
    unsigned live_sessions{0};
};

// DeuceSSH stores keys in its process-global algorithm registry. One immutable
// identity capability is owned by Runtime and retained by every listener/stream.
// Reacquisition shares it, including while a stream has not yet initialized its
// session. A different root/owner cannot replace it while ANY capability lives.
// No public reload/reset API. The last stream releases it only AFTER demux join.
class AlphaSshBackend final {
public:
    static std::shared_ptr<const AlphaSshBackend> acquire(
        const std::filesystem::path& installation_root,
        const std::wstring& installation_owner_sid, std::string& diagnostic) noexcept;
    static AlphaSshBackendStatistics statistics(); // Counts only; no key/credential data.
    AlphaSshBackend(const AlphaSshBackend&) = delete;
    AlphaSshBackend& operator=(const AlphaSshBackend&) = delete;
private:
    AlphaSshBackend(std::filesystem::path root, std::wstring owner);
    const std::filesystem::path root_;
    const std::wstring owner_;
};

class AlphaSshStream final : public application::IInteractiveSessionByteStream {
public:
    // Apply before bind/listen: shrinking an accepted socket is too late to
    // bound the TCP window already negotiated during the handshake.
    static bool prepare_listener_socket(std::uintptr_t socket) noexcept;
    AlphaSshStream(std::shared_ptr<const AlphaSshBackend> backend,
        std::uintptr_t socket, const std::atomic<bool>& stopping,
        std::function<void(std::string_view)> output_observer,
        std::function<void(SshTerminalDimensions)> resize_observer,
        std::function<void(bool)> payload_activity = {},
        std::function<void()> encrypted_observer = {});
    ~AlphaSshStream() override;
    AlphaSshStream(const AlphaSshStream&) = delete;
    AlphaSshStream& operator=(const AlphaSshStream&) = delete;
    bool establish(application::Phase3aTransportPasswordLogin& login,
        std::string& diagnostic, const std::string& authentication_banner = {}) noexcept;
    // Bounded transport-only refusal: no authentication callback, demux, channel,
    // shell or BBS/account authority. Uses the same retained backend capability.
    bool deny_capacity(std::string_view notice) noexcept;
    void finish_shell(bool normal_logoff) noexcept;
    void enable_menu_polling() noexcept;
    bool message_binary_mode(bool enabled) noexcept;
    void message_binary_deadline(std::uint64_t deadline) noexcept;
    bool ansi_terminal() const noexcept;
    application::InteractiveStreamReadResult read_some(std::size_t maximum_bytes) override;
    // File pump owns both directions: never wait for caller input while a
    // helper response/output may need forwarding on that same pump thread.
    application::InteractiveStreamReadResult read_ready(std::size_t maximum_bytes);
    application::InteractiveStreamWriteResult write_some(std::string_view bytes) override;
    application::InteractiveStreamWriteResult write_ready(std::string_view bytes);

private:
    application::InteractiveStreamReadResult read_polled(std::size_t maximum_bytes, int wait_milliseconds);
    application::InteractiveStreamWriteResult write_polled(std::string_view bytes, bool ready_only);
    bool message_binary_mode_{};
    friend class AlphaSshBackend;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool menu_polling_{};
};

} // namespace sfx::apps::runtime
