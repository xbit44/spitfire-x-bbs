#pragma once

#include "sfx/application/message_storage.hpp"
#include <array>
#include <functional>

namespace sfx::application {

enum class MessageDeletionKind : std::uint8_t { echo, group };
struct MessageDeletionTarget {
    MessageDeletionKind kind{MessageDeletionKind::echo};
    std::int64_t id{},expected_revision{};
};
struct MessageDeletionPreview {
    MessageDeletionTarget target;
    std::int64_t configuration_revision{};
    std::string name;
    std::int64_t echo_count{},message_count{},permanent_count{};
    std::array<std::uint8_t,32> fingerprint{};
};
struct MessageDeletionResult {
    std::int64_t configuration_revision{},echo_count{},message_count{},permanent_count{};
    bool replayed{};
};
// Private host-created offline authority only. Never serialized to callers or
// used through Runtime's ordinary configuration route. The Windows owner must
// hold the installation guard from preview/optional backup through final commit.
class IMessageDeletionStorage {
public:
    virtual ~IMessageDeletionStorage()=default;
    virtual StorageResult<MessageDeletionPreview> preview_message_deletion(
        const MessageConfigurationActor&,const MessageDeletionTarget&,std::function<bool()> cancelled={})=0;
    virtual StorageResult<MessageDeletionResult> commit_message_deletion(
        const MessageConfigurationActor&,Identifier128,const MessageDeletionPreview&,std::function<bool()> cancelled={})=0;
};

} // namespace sfx::application
