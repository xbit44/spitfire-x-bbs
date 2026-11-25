#pragma once
#include "sfx/application/ftn.hpp"
#include "sfx/application/ftn_configuration_transport.hpp"
#include "sfx/application/ftn_retention.hpp"
#include "sfx/application/message_storage.hpp"
namespace sfx::application {
struct FtnConfigurationChangeResult {
    FtnConfiguration configuration;
    std::int64_t applied_revision{};
    bool replayed{};
};
class IFtnStorage {
public:
    virtual ~IFtnStorage()=default;
    virtual StorageResult<FtnConfiguration> ftn_configuration(const MessageConfigurationActor&)=0;
    virtual StorageResult<std::int64_t> ftn_configuration_receipt(const MessageConfigurationActor&,const FtnTransferToken&)=0;
    virtual StorageResult<FtnRetentionPreview> preview_ftn_retention(const MessageConfigurationActor&,const FtnTransferToken&,const FtnConfiguration&)=0;
    virtual StorageResult<FtnConfigurationChangeResult> change_ftn_configuration(
        const MessageConfigurationActor&,const FtnTransferToken&,const FtnConfiguration&)=0;
};
}
