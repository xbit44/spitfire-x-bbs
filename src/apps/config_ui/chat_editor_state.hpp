#pragma once
#include "sfx/application/chat.hpp"
enum ChatControlId {
    IDC_CHAT_LIST=9000,IDC_CHAT_ADD,IDC_CHAT_EDIT,IDC_CHAT_DELETE,IDC_CHAT_RELOAD,
    IDC_CHAT_NAME,IDC_CHAT_ENABLED,IDC_CHAT_MODE,IDC_CHAT_LEVEL,IDC_CHAT_SYSOP,
    IDC_CHAT_SAVE,IDC_CHAT_CANCEL,IDC_CHAT_STATS,IDC_CHAT_STATS_SAVE,
    IDC_CHAT_COMMANDS,IDC_CHAT_COMMAND_LIST,IDC_CHAT_COMMAND_ADD,IDC_CHAT_COMMAND_EDIT,
    IDC_CHAT_COMMAND_DELETE,IDC_CHAT_COMMAND_MENU,IDC_CHAT_COMMAND_DOOR,IDC_CHAT_COMMAND_KEY,
    IDC_CHAT_COMMAND_NAME,IDC_CHAT_COMMAND_ENABLED,IDC_CHAT_COMMAND_MODE,IDC_CHAT_COMMAND_LEVEL,
    IDC_CHAT_COMMAND_SYSOP,IDC_CHAT_COMMAND_SAVE,IDC_CHAT_COMMAND_BACK
};
struct ChatEditorState {
    std::optional<sfx::application::ChatConfiguration> config;
    HWND list{},name{},enabled{},mode{},level{},sysop{},detail{},stats{};
    sfx::application::ChatRoomId room{};bool editing{};
    HWND commands{},command_menu{},command_door{},command_key{},command_name{},command_enabled{},command_mode{},command_level{},command_sysop{},command_detail{};
    int command{-1};bool command_editing{};
};
