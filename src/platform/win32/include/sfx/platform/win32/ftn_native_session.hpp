#pragma once
#include "sfx/platform/win32/ftn_native_transport.hpp"
#include "sfx/platform/win32/ftn_native_custody.hpp"

namespace sfx::platform::win32 {
struct FtnNativeSessionResult {
    bool completed{},peer_verified{},outbound{};
    application::FtnIngressProof proof{application::FtnIngressProof::nonsecure};
    std::string network,link,reason;
    application::Identifier128 last_ingress; // host audit binding, not caller IPC
    std::uint64_t received_bytes{},sent_bytes{};
    std::uint32_t received_files{},sent_files{},completed_batches{};
    application::FtnFileProgress send,receive;
    bool handshake_complete{}; // Protocol acceptance, distinct from verified peer identity.
};
struct FtnNativeSessionPorts {
    application::IFtnMailStorage& storage;
    application::FtnConfiguration configuration;
    FilePayloadDirectory inbound,outbound;
    std::string installation;
    application::Identifier128 generation;
    std::optional<application::FtnPeerMatch> dialled_link;
    bool include_hold{};
    std::function<bool()> running;
    std::function<void(const FtnNativeSessionResult&)> observe;
    // Host-created per-network custody capabilities selected only AFTER the
    // session's identity/proof decision. Empty network means nonsecure ingress.
    std::function<FilePayloadDirectory(std::string_view,bool outbound)> directory;
    std::optional<FilePayloadDirectory> completed_inbound;
};
// Runs one host-owned, cancellable BinkP session over the already selected
// socket. Queue/session concurrency is owned by the enclosing Runtime engine.
// Receipts distinguish received custody, local processing, and remote ACK.
FtnNativeSessionResult run_ftn_native_session(FtnNativeChannel&,FtnNativeSessionPorts);
std::string ftn_native_artifact_name(const application::Identifier128&);
std::string ftn_native_spool_name(const application::FtnOutboundJob&);
std::string ftn_existing_spool_name(const FilePayloadDirectory&,const application::FtnOutboundJob&);
}
