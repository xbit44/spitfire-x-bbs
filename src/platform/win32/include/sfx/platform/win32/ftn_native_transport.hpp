#pragma once
#include "sfx/application/ftn.hpp"
#include <array>
#include <chrono>
#include <functional>
#include <memory>

namespace sfx::platform::win32 {
enum class FtnReadOutcome { data,idle,closed };
struct FtnReadResult {FtnReadOutcome outcome{FtnReadOutcome::idle};std::string bytes;};
// Host-only nonblocking socket/TLS transport. DNS is supplied by the Runtime's
// existing bounded asynchronous resolver; this boundary never calls blocking
// getaddrinfo. Peer command text cannot select an endpoint or TLS policy.
class FtnNativeChannel final {
public:
    static std::unique_ptr<FtnNativeChannel> connect_ipv4(const std::array<std::uint8_t,4>&,
        std::uint16_t,const application::FtnLink&,const std::wstring& owner,
        const std::function<bool()>& running);
    ~FtnNativeChannel();
    std::string peer_ip() const;
    FtnNativeChannel(const FtnNativeChannel&)=delete;
    void handshake(const std::function<bool()>& running);
    FtnReadResult read(std::chrono::milliseconds,const std::function<bool()>& running);
    void write(std::string_view,const std::function<bool()>& running);
    bool verified_server_tls() const noexcept;
    bool encrypted() const noexcept;
    bool matches_outbound_link(const application::FtnLink&) const noexcept;
private:
    struct Impl;
    explicit FtnNativeChannel(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
    friend class FtnNativeListener;
};
class FtnNativeListener final {
public:
    // TLS is explicit and separate from classic TCP; a bad certificate/key
    // fails this activation without opening a substitute plaintext listener.
    static std::unique_ptr<FtnNativeListener> open(std::string_view bind,std::uint16_t port,
        bool tls,const application::FtnSettings&,const std::wstring& owner);
    ~FtnNativeListener();
    FtnNativeListener(const FtnNativeListener&)=delete;
    std::uint16_t port() const noexcept;
    // No peer handshake on the accept thread. Each bounded session worker
    // calls channel.handshake with its cancellation/deadline ownership.
    std::unique_ptr<FtnNativeChannel> accept(std::chrono::milliseconds,
        const std::function<bool()>& running);
private:
    struct Impl;
    explicit FtnNativeListener(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
