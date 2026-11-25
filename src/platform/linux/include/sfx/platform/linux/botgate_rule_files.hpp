#pragma once
#include "sfx/application/botgate_rules.hpp"
#include <filesystem>
namespace sfx::platform::linux_host {
void initialize_botgate_files(const std::filesystem::path&,const std::wstring&,const application::BotGateConfiguration&);
std::string read_botgate_rule(const std::filesystem::path&,const std::wstring&,unsigned);
void save_botgate_rule(const std::filesystem::path&,const std::wstring&,unsigned,std::string_view);
void save_botgate_temporary(const std::filesystem::path&,const std::wstring&,std::string_view);
application::BotGateRuleSnapshot load_botgate_files(const std::filesystem::path&,const std::wstring&);
}
