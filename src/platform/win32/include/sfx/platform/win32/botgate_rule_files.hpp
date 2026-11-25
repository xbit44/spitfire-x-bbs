#pragma once
#include "sfx/application/botgate_rules.hpp"
#include <filesystem>
namespace sfx::platform::win32 {
void initialize_botgate_files(const std::filesystem::path& root,const std::wstring& owner,
    const application::BotGateConfiguration& legacy);
application::BotGateRuleSnapshot load_botgate_files(const std::filesystem::path& root,const std::wstring& owner);
std::string read_botgate_rule(const std::filesystem::path& root,const std::wstring& owner,unsigned index);
void save_botgate_rule(const std::filesystem::path& root,const std::wstring& owner,unsigned index,std::string_view text);
void save_botgate_temporary(const std::filesystem::path& root,const std::wstring& owner,std::string_view text);
}
