#pragma once
#include "sfx/platform/linux/file_payload_store.hpp"
#include "sfx/platform/linux/ftn_installation.hpp"
#include "sfx/application/ftn_work.hpp"

namespace sfx::platform::linux_host {
// Installation-owner engine boundary. The context comes from the session's
// actual authentication decision, never from a peer command or caller IPC.
// A native ACK may be emitted only after finish() succeeds.
class FtnNativeReceive final {
public:
    static FtnNativeReceive open(const FilePayloadDirectory&,application::IFtnMailStorage&,
        application::FtnIngressReceipt,const std::function<bool()>& running,
        const FilePayloadDirectory* completed=nullptr);
    ~FtnNativeReceive();
    FtnNativeReceive(FtnNativeReceive&&) noexcept;
    FtnNativeReceive& operator=(FtnNativeReceive&&) noexcept;
    FtnNativeReceive(const FtnNativeReceive&)=delete;
    std::uint64_t offset() const noexcept;
    const application::FtnIngressReceipt& receipt() const noexcept;
    void append(std::string_view); // bounded frame; never beyond declared size
    void checkpoint(); // flush native bytes before one offset/hash transaction
    application::FilePayloadIdentity finish(); // independent full native hash
private:
    struct Impl;
    explicit FtnNativeReceive(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
FilePayloadLease open_ftn_received(application::IFtnMailStorage&,const FtnInstallation&,
    const application::FtnIngressReceipt&,const std::function<bool()>&,bool mutable_file=true,
    std::string_view resolved_network={});
std::string ftn_bad_original_name(const application::FtnIngressReceipt&);
void migrate_ftn_inbound(application::IFtnMailStorage&,const FtnInstallation&,
    const std::function<bool()>&);
}
