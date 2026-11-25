#pragma once
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/local_qwk_administration.hpp"
#include "sfx/application/local_classic_information.hpp"
#include "sfx/application/chat.hpp"
#include "sfx/application/display.hpp"
#include "sfx/application/local_file_administration.hpp"
#include "sfx/application/external_events.hpp"
#include "linux_runtime_jobs.hpp"
#include <nlohmann/json.hpp>
namespace sfx::apps::runtime::web {
bool operational_route(std::string_view);
nlohmann::json configuration_routes();
nlohmann::json operational_page(std::string_view,const application::AlphaConfigurationSnapshot&);
application::AlphaConfigurationUpdateRequest operational_update(std::string_view,const nlohmann::json&,const application::AlphaConfigurationSnapshot&);
nlohmann::json menu_preview(const nlohmann::json&,const application::AlphaConfigurationSnapshot&,application::ITrustedDisplayAssetReader*);
application::QwkPolicyChange qwk_update(const nlohmann::json&);
nlohmann::json opening_date_page(const application::LocalClassicInformationSnapshot&,std::string_view,const application::IClock&);
application::ClassicInformationChange opening_date_update(const nlohmann::json&);
nlohmann::json statistics_page(const application::ChatConfiguration&);
application::ChatConfiguration statistics_update(const nlohmann::json&,const application::ChatConfiguration&);
nlohmann::json chat_room_page(const application::ChatConfiguration&,std::optional<std::uint16_t>);
application::ChatConfiguration chat_room_update(const nlohmann::json&,const application::ChatConfiguration&,bool remove);
nlohmann::json door_command_page(const application::ChatConfiguration&,const application::DoorCatalog&,std::optional<unsigned>);
application::ChatConfiguration door_command_update(const nlohmann::json&,const application::ChatConfiguration&,const application::DoorCatalog&,bool remove);
unsigned door_command_identity(const application::ChatDoorCommand&);
nlohmann::json transfer_policy_page(const application::FilePolicy&,std::string_view operation);
application::FileConfigurationChange transfer_policy_update(const nlohmann::json&,const application::FilePolicy&,bool scanning=false);
nlohmann::json clamav_policy_page(const application::FilePolicy&,std::string_view operation);
nlohmann::json file_count_rule_page(const application::FilePolicy&,const std::vector<application::FileSecurityRule>&,const std::vector<application::SecurityLevelRecord>&,std::optional<std::uint16_t>,std::string_view operation);
application::FileConfigurationChange file_count_rule_update(const nlohmann::json&);
nlohmann::json caller_exemption_page(const application::FilePolicy&,const std::optional<application::FileCallerExemption>&,std::optional<application::AccountId>,std::string_view,bool,std::string_view operation);
application::FileConfigurationChange caller_exemption_update(const nlohmann::json&);
nlohmann::json security_level_page(const application::LocalSecuritySnapshot&,std::optional<std::uint16_t>);
application::LocalSecurityChange security_level_update(const nlohmann::json&,const application::LocalSecuritySnapshot&);
application::LocalSecurityChange security_level_removal(const nlohmann::json&,const application::LocalSecuritySnapshot&);
application::MessageConfigurationChange message_utility_update(std::string_view,const nlohmann::json&);
nlohmann::json account_page(const application::LocalAccountSnapshot&,std::optional<application::AccountId>);
application::LocalAccountRequest account_update(const nlohmann::json&);
application::LocalAccountRequest account_action(std::string_view,const nlohmann::json&);
nlohmann::json system_password_page(const application::LocalAccountSnapshot&);
application::LocalAccountRequest system_password_update(const nlohmann::json&);
nlohmann::json throttle_page();
application::LocalThrottleClearRequest throttle_update(const nlohmann::json&);
std::string_view scheduled_job_id(unsigned);
nlohmann::json scheduled_job_page(const LinuxScheduledJobSnapshot&,unsigned,const application::MessageMaintenanceStatus* = nullptr);
struct BackupScheduleUpdate { application::AlphaSchedulerSettingsRequest request;std::uint32_t retained{}; };
BackupScheduleUpdate backup_schedule_update(const nlohmann::json&);
application::MessageMaintenanceScheduleRequest message_schedule_update(const nlohmann::json&);
nlohmann::json external_events_page(const application::ExternalEventCatalog&, std::optional<std::uint64_t>,
    const application::BbsLocalTimeAdapters&, std::int64_t);
application::ExternalEvent external_event_update(const nlohmann::json&);
}
