#pragma once
#include "sfx/application/chat.hpp"
namespace sfx::application {
struct PrivateChatAllowance {
    std::optional<std::uint32_t> minutes_per_chat;
    std::optional<std::int64_t> seconds_remaining_today;
};
class IChatStorage {
public:
    virtual ~IChatStorage()=default;
    virtual StorageResult<ChatConfiguration> chat_configuration(const MessageConfigurationActor&)=0;
    virtual StorageResult<ChatConfiguration> change_chat_configuration(const MessageConfigurationActor&,const ChatConfiguration&)=0;
    virtual StorageResult<ChatPreferences> chat_preferences(const MessageActor&)=0;
    virtual StorageResult<ChatPreferences> save_chat_preferences(const MessageActor&,const ChatPreferences&)=0;
    virtual StorageResult<MenuDoorCommands> menu_door_commands(const MessageActor&,MenuId)=0;
    virtual StorageResult<PrivateChatAllowance> private_chat_allowance(const MessageActor&,std::string_view date)=0;
    virtual StorageStatus persist_private_chat_usage(Identifier128 session,std::int64_t seconds,
        const std::vector<std::pair<std::string,std::int64_t>>& days)=0;
    virtual StorageResult<DoorLaunch> menu_door_launch(const MessageActor&,const MenuDoorSelection&)=0;
};
}
