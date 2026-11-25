#pragma once
#include "sfx/application/ftn_configuration_transport.hpp"
#include "sfx/application/ftn_retention.hpp"
#include <functional>

namespace sfx::apps::runtime::web {
using FtnExchange=std::function<application::FtnConfigurationResponse(const application::FtnConfigurationRequest&)>;
struct WebFtnSnapshot {
    application::FtnConfigurationCode code{application::FtnConfigurationCode::unavailable};
    std::optional<application::FtnConfiguration> configuration;
};
// Internal host-side transfer context, never decoded from browser JSON. A page
// retains only an opaque handle; the host keeps this exact prepared request.
struct WebFtnPrepared {
    application::FtnConfigurationCode code{application::FtnConfigurationCode::unavailable};
    application::FtnConfigurationRequest commit;
    std::optional<application::FtnRetentionPreview> review;
};
WebFtnSnapshot web_ftn_read(const FtnExchange&,application::FtnTransferToken);
WebFtnPrepared web_ftn_prepare(const FtnExchange&,application::FtnTransferToken,const application::FtnConfiguration&);
application::FtnConfigurationResponse web_ftn_commit(const FtnExchange&,WebFtnPrepared&);
void web_ftn_cancel(const FtnExchange&,WebFtnPrepared&) noexcept;
application::FtnConfigurationResponse web_ftn_reconcile(const FtnExchange&,application::FtnTransferToken);
} // namespace sfx::apps::runtime::web
