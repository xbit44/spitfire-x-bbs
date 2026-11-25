#pragma once
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/chat_notice.hpp"
#include "sfx/application/terminal.hpp"

namespace sfx::apps::runtime {
namespace control_dispatch_detail {
inline void erase_payload(std::vector<std::uint8_t>& bytes) noexcept {
    application::detail::explicitly_erase_bytes(bytes.data(),bytes.size());bytes.clear();
}
template<class Result,class Code> Result rejected(Code code) {
    Result result{};result.code=code;return result;
}
inline application::LocalControlFrame response_frame(application::LocalControlMessageType type,
    std::uint32_t request_id,std::vector<std::uint8_t> payload) {
    application::LocalControlFrame response;response.type=type;
    response.request_id=request_id;response.payload=std::move(payload);return response;
}
}

// Shared typed business dispatch. The platform authenticates the connection,
// owns replay handling/status cache and controls process shutdown. In particular
// Linux management lifetime must not inherit Windows UI/process ownership.
// A host must implement the entire operation surface to instantiate this code;
// there are no success stubs for missing Linux functions.
template<class Host>
std::optional<application::LocalControlFrame> dispatch_runtime_control(Host& host,
    application::LocalControlFrame& request,application::AlphaStatusSnapshotCache& status_cache) {
    namespace app=application;
    struct Erase {std::vector<std::uint8_t>& bytes;~Erase(){control_dispatch_detail::erase_payload(bytes);}} erase{request.payload};
    app::LocalControlFrame response;
        if (request.type == app::LocalControlMessageType::status_request) {
            const auto status_request = app::decode_status_request(request.payload);
            if (!status_request || (status_request->snapshot_sequence == 0 &&
                !status_cache.capture(host.status(), status_request->screen_node))) return std::nullopt;
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::status_response,
                request.request_id, status_cache.chunk(*status_request));
        } else if (request.type == app::LocalControlMessageType::metrics_request &&
            request.payload.empty()) {
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::metrics_response,
                request.request_id, app::encode_runtime_metrics(host.metrics()));
        } else if (request.type == app::LocalControlMessageType::log_page_request) {
            const auto cursor = app::decode_log_cursor(request.payload);
            const auto page = cursor ? host.log_page(*cursor) : std::nullopt;
            if (!page) return std::nullopt; // malformed/stale/future cursor: fail closed, no partial history
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::log_page_response,
                request.request_id, app::encode_log_page(*page));
        } else if (request.type == app::LocalControlMessageType::bootstrap_request) {
            auto bootstrap = app::decode_bootstrap_request(request.payload);
            struct EraseBootstrap {
                std::optional<app::AlphaBootstrapRequest>& value;
                ~EraseBootstrap(){if(value)app::erase_bootstrap_secrets(*value);}
            } erase_bootstrap{bootstrap};
            control_dispatch_detail::erase_payload(request.payload);
            const app::AlphaControlResponse result = bootstrap
                ? host.bootstrap(*bootstrap)
                : app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,
                    "bootstrap_frame_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::bootstrap_response,
                request.request_id, app::encode_control_response(result));
        } else if (request.type == app::LocalControlMessageType::sound_poll_request) {
            const auto poll=app::decode_sound_poll_request(request.payload);
            if(!poll)return std::nullopt;
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::sound_poll_response,
                request.request_id,app::encode_sound_poll_snapshot(host.sound_poll(*poll)));
        } else if (request.type == app::LocalControlMessageType::node_command_request) {
            const auto command = app::decode_node_command_request(request.payload);
            const app::AlphaControlResponse result = command
                ? host.command_node(*command)
                : app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,
                    "node_command_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::node_command_response,
                request.request_id, app::encode_control_response(result));
        } else if (request.type == app::LocalControlMessageType::local_session_start_request) {
            const auto local = app::decode_local_session_start_request(request.payload);
            const auto result = local ? host.start_local_session(*local) :
                app::AlphaLocalSessionStartResult{{app::LocalControlResponseCode::invalid_request,
                    "local_session_request_invalid"}, {}, 0, {}, 0};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::local_session_start_response,
                request.request_id, app::encode_local_session_start_result(result));
        } else if (request.type == app::LocalControlMessageType::local_session_cancel_request) {
            const auto local = app::decode_local_session_cancel_request(request.payload);
            const auto result = local ? host.cancel_local_session(*local) :
                app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,
                    "local_session_cancel_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::local_session_cancel_response,
                request.request_id, app::encode_control_response(result));
        } else if (request.type == app::LocalControlMessageType::settings_request) {
            const auto settings = app::decode_settings_request(request.payload);
            const app::AlphaControlResponse result = settings
                ? host.stage_settings(*settings)
                : app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,
                    "settings_request_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::settings_response,
                request.request_id, app::encode_control_response(result));
        } else if (request.type == app::LocalControlMessageType::throttle_clear_request) {
            const auto command = app::decode_throttle_clear_request(request.payload);
            const auto result = command ? host.clear_throttle(*command) :
                app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"throttle_clear_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::throttle_clear_response,
                request.request_id,app::encode_control_response(result));
        } else if(request.type==app::LocalControlMessageType::message_maintenance_query_request){
            const auto kind=app::decode_message_maintenance_kind(request.payload);
            const auto result=kind?host.message_maintenance_status(*kind):app::MessageMaintenanceStatus{};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::message_maintenance_query_response,request.request_id,app::encode_message_maintenance_status(result));
        } else if(request.type==app::LocalControlMessageType::message_maintenance_schedule_request){
            const auto settings=app::decode_message_maintenance_schedule(request.payload);
            const auto result=settings?host.configure_message_schedule(*settings):app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"message_schedule_invalid"};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::message_maintenance_schedule_response,request.request_id,app::encode_control_response(result));
        } else if(request.type==app::LocalControlMessageType::message_maintenance_run_request){
            const auto kind=app::decode_message_maintenance_kind(request.payload);
            const auto result=kind?host.run_message_maintenance_now(*kind):app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"message_job_unknown"};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::message_maintenance_run_response,request.request_id,app::encode_control_response(result));
        } else if(request.type==app::LocalControlMessageType::file_management_query_request){
            const auto query=app::decode_file_management_read_request(request.payload);const auto result=query?host.query_file_management(*query):app::FileManagementReadSnapshot{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::file_management_query_response,request.request_id,app::encode_file_management_read_snapshot(result));
        } else if(request.type==app::LocalControlMessageType::file_management_start_request){
            const auto task=app::decode_file_management_task(request.payload);const auto result=task?host.start_file_management(*task):control_dispatch_detail::rejected<app::FileManagementJobStatus>(app::LocalAccountResultCode::invalid_request);
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::file_management_start_response,request.request_id,app::encode_file_management_status(result));
        } else if(request.type==app::LocalControlMessageType::file_management_status_request||request.type==app::LocalControlMessageType::file_management_cancel_request){
            const auto id=app::decode_file_management_job(request.payload);const bool cancel=request.type==app::LocalControlMessageType::file_management_cancel_request;
            const auto result=id?(cancel?host.cancel_file_management(*id):host.file_management_status(*id)):control_dispatch_detail::rejected<app::FileManagementJobStatus>(app::LocalAccountResultCode::invalid_request);
            response=control_dispatch_detail::response_frame(cancel?app::LocalControlMessageType::file_management_cancel_response:app::LocalControlMessageType::file_management_status_response,request.request_id,app::encode_file_management_status(result));
        } else if(request.type==app::LocalControlMessageType::clamav_manual_notice_request){
            const auto notice=app::decode_manual_clamav_notice(request.payload);
            const auto result=notice?host.manual_clamav_notice(*notice):app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"clamav_notice_invalid"};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::clamav_manual_notice_response,request.request_id,app::encode_control_response(result));
        } else if(request.type==app::LocalControlMessageType::chat_operator_request){
            const auto command=app::decode_chat_operator_request(request.payload);
            const auto result=command?host.chat_operator(*command):app::ChatOperatorSnapshot{};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::chat_operator_response,request.request_id,app::encode_chat_operator_snapshot(result));
        } else if(request.type==app::LocalControlMessageType::chat_notice_request){
            const auto notice=app::decode_chat_notice(request.payload);
            const auto result=notice?host.send_chat_notice(*notice):app::ChatResult::invalid;
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::chat_notice_response,request.request_id,{static_cast<std::uint8_t>(result)});
        } else if(request.type==app::LocalControlMessageType::ftn_held_request){
            const auto query=app::decode_ftn_held_request(request.payload);auto result=query?host.ftn_held(*query):app::FtnHeldControlResponse{};if(!query)result.code=app::FtnConfigurationCode::invalid;
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::ftn_held_response,request.request_id,app::encode_ftn_held_response(result));
        } else if(request.type==app::LocalControlMessageType::ftn_runtime_request){
            const auto query=app::decode_ftn_runtime_request(request.payload);auto result=query?host.ftn_runtime(*query):app::FtnRuntimeResponse{};if(!query)result.code=app::FtnConfigurationCode::invalid;
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::ftn_runtime_response,request.request_id,app::encode_ftn_runtime_response(result));
        } else if(request.type==app::LocalControlMessageType::ftn_hatch_catalog_request){
            const auto query=app::decode_ftn_configuration_request(request.payload);const auto result=query?host.ftn_hatch_catalog(*query):control_dispatch_detail::rejected<app::FtnConfigurationResponse>(app::FtnConfigurationCode::invalid);response=control_dispatch_detail::response_frame(app::LocalControlMessageType::ftn_hatch_catalog_response,request.request_id,app::encode_ftn_configuration_response(result));
        } else if(request.type==app::LocalControlMessageType::ftn_configuration_request){
            const auto query=app::decode_ftn_configuration_request(request.payload);
            const auto result=query?host.ftn_configuration(*query):control_dispatch_detail::rejected<app::FtnConfigurationResponse>(app::FtnConfigurationCode::invalid);
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::ftn_configuration_response,request.request_id,app::encode_ftn_configuration_response(result));
        } else if(request.type==app::LocalControlMessageType::chat_configuration_query_request){
            const auto result=request.payload.empty()?host.chat_configuration():app::ChatConfigurationResult{app::ChatResult::invalid};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::chat_configuration_query_response,request.request_id,app::encode_chat_result(result));
        } else if(request.type==app::LocalControlMessageType::chat_configuration_change_request){
            const auto c=app::decode_chat_configuration(request.payload);
            const auto result=c?host.change_chat_configuration(*c):app::ChatConfigurationResult{app::ChatResult::invalid};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::chat_configuration_change_response,request.request_id,app::encode_chat_result(result));
        } else if(request.type==app::LocalControlMessageType::door_configuration_query_request){
            const auto query=app::decode_door_configuration_query(request.payload);
            const auto result=query?host.door_configuration_snapshot(*query):app::LocalDoorSnapshot{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::door_configuration_query_response,request.request_id,app::encode_door_configuration_snapshot(result));
        } else if(request.type==app::LocalControlMessageType::door_configuration_change_request){
            const auto change=app::decode_door_configuration_change(request.payload);
            const auto result=change?host.change_door_configuration(*change):app::LocalDoorResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::door_configuration_change_response,request.request_id,app::encode_door_configuration_result(result));
        } else if(request.type==app::LocalControlMessageType::door_bulletin_query_request){
            const auto query=app::decode_door_bulletin_query(request.payload);
            const auto result=query?host.door_bulletin_snapshot(*query):app::LocalDoorSnapshot{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::door_bulletin_query_response,request.request_id,app::encode_door_configuration_snapshot(result));
        } else if(request.type==app::LocalControlMessageType::door_bulletin_change_request){
            const auto change=app::decode_door_bulletin_change(request.payload);
            const auto result=change?host.change_door_bulletin(*change):app::LocalDoorResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::door_bulletin_change_response,request.request_id,app::encode_door_configuration_result(result));
        } else if(request.type==app::LocalControlMessageType::file_configuration_query_request){
            const auto query=app::decode_file_configuration_query(request.payload);
            const auto result=query?host.file_configuration_snapshot(*query):app::LocalFileConfigurationSnapshot{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::file_configuration_query_response,request.request_id,app::encode_file_configuration_snapshot(result));
        } else if(request.type==app::LocalControlMessageType::file_configuration_change_request){
            const auto change=app::decode_file_configuration_change(request.payload);
            const auto result=change?host.change_file_configuration(*change):app::LocalFileConfigurationResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::file_configuration_change_response,request.request_id,app::encode_file_configuration_result(result));
        } else if (request.type == app::LocalControlMessageType::message_configuration_query_request) {
            const auto query=app::decode_message_configuration_query(request.payload);
            const auto result=query?host.message_configuration_snapshot(*query):app::LocalMessageConfigurationSnapshot{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::message_configuration_query_response,request.request_id,app::encode_message_configuration_snapshot(result));
        } else if (request.type == app::LocalControlMessageType::message_configuration_change_request) {
            const auto change=app::decode_message_configuration_change(request.payload);
            const auto result=change?host.change_message_configuration(*change):app::LocalMessageConfigurationResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::message_configuration_change_response,request.request_id,app::encode_message_configuration_result(result));
        } else if(request.type==app::LocalControlMessageType::qwk_policy_query_request){
            const auto result=request.payload.empty()?host.qwk_policy_snapshot():app::LocalQwkPolicyResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::qwk_policy_query_response,request.request_id,app::encode_qwk_policy_result(result));
        } else if(request.type==app::LocalControlMessageType::qwk_policy_change_request){
            const auto change=app::decode_qwk_policy_change(request.payload);const auto result=change?host.change_qwk_policy(*change):app::LocalQwkPolicyResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::qwk_policy_change_response,request.request_id,app::encode_qwk_policy_result(result));
        } else if (request.type == app::LocalControlMessageType::classic_information_query_request) {
            const auto query=app::decode_classic_information_query(request.payload);
            const auto result=query?host.classic_information_snapshot(*query):app::LocalClassicInformationSnapshot{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::classic_information_query_response,request.request_id,app::encode_classic_information_snapshot(result));
        } else if (request.type == app::LocalControlMessageType::classic_information_change_request) {
            const auto change=app::decode_classic_information_change(request.payload);
            const auto result=change?host.change_classic_information(*change):app::LocalAccountResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::classic_information_change_response,request.request_id,app::encode_account_result(result));
        } else if (request.type == app::LocalControlMessageType::security_query_request) {
            const auto query=app::decode_account_query(request.payload);
            const auto result=query && !query->account_id && query->name_prefix.empty() ? host.security_snapshot(query->offset) : app::LocalSecuritySnapshot{};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::security_query_response,request.request_id,app::encode_security_snapshot(result));
        } else if (request.type == app::LocalControlMessageType::security_change_request) {
            const auto change=app::decode_security_change(request.payload);
            const auto result=change ? host.change_security(*change) : app::LocalAccountResult{app::LocalAccountResultCode::invalid_request};
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::security_change_response,request.request_id,app::encode_account_result(result));
        } else if (request.type == app::LocalControlMessageType::account_query_request) {
            const auto query = app::decode_account_query(request.payload);
            const auto result = query ? host.account_snapshot(*query) :
                app::LocalAccountSnapshot{app::LocalAccountResultCode::invalid_request};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::account_query_response,
                request.request_id, app::encode_account_snapshot(result));
        } else if (request.type == app::LocalControlMessageType::account_command_request) {
            auto command = app::decode_account_request(request.payload);
            struct EraseAccount {
                std::optional<app::LocalAccountRequest>& value;
                ~EraseAccount(){if(value)app::erase_local_account_secrets(*value);}
            } erase_account{command};
            control_dispatch_detail::erase_payload(request.payload);
            const auto result = command ? host.administer_account(*command) :
                app::LocalAccountResult{app::LocalAccountResultCode::invalid_request};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::account_command_response,
                request.request_id, app::encode_account_result(result));
        } else if (request.type == app::LocalControlMessageType::botgate_feed_status_request && request.payload.empty()) {
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::botgate_feed_status_response,
                request.request_id,app::encode_botgate_feed_status(host.botgate_feed_status()));
        } else if (request.type == app::LocalControlMessageType::botgate_feed_update_request && request.payload.empty()) {
            response=control_dispatch_detail::response_frame(app::LocalControlMessageType::botgate_feed_update_response,
                request.request_id,app::encode_control_response(host.update_botgate_feed()));
        } else if (request.type == app::LocalControlMessageType::configuration_snapshot_request &&
            request.payload.empty()) {
            const auto snapshot = host.configuration_snapshot();
            if (!snapshot) return std::nullopt;
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::configuration_snapshot_response,
                request.request_id, app::encode_configuration_snapshot(*snapshot));
        } else if (request.type == app::LocalControlMessageType::configuration_update_request) {
            const auto update = app::decode_configuration_update_request(request.payload);
            const auto result = update ? host.update_configuration(*update) :
                app::AlphaConfigurationUpdateResult{
                    app::LocalControlResponseCode::invalid_request,
                    "configuration_update_invalid",
                    app::AlphaConfigurationActivation::refused, 1, 1,
                    "Refused - invalid configuration request"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::configuration_update_response,
                request.request_id, app::encode_configuration_update_result(result));
        } else if (request.type == app::LocalControlMessageType::scheduler_settings_request) {
            const auto settings = app::decode_scheduler_settings_request(request.payload);
            const app::AlphaControlResponse result = settings
                ? host.configure_scheduler(*settings)
                : app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,
                    "scheduler_settings_request_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::scheduler_settings_response,
                request.request_id, app::encode_control_response(result));
        } else if (request.type == app::LocalControlMessageType::scheduler_run_request) {
            const auto run = app::decode_scheduler_run_request(request.payload);
            const app::AlphaControlResponse result = run
                ? host.run_scheduler_now(*run)
                : app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,
                    "scheduler_run_request_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::scheduler_run_response,
                request.request_id, app::encode_control_response(result));
        } else if (request.type == app::LocalControlMessageType::lifecycle_command_request) {
            const auto command = app::decode_lifecycle_command_request(request.payload);
            const auto result = command ? host.command_lifecycle(*command) :
                app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,
                    "lifecycle_command_invalid"};
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::lifecycle_command_response,
                request.request_id, app::encode_control_response(result));
        } else {
            response = control_dispatch_detail::response_frame(app::LocalControlMessageType::bootstrap_response,
                request.request_id, app::encode_control_response(
                    {app::LocalControlResponseCode::invalid_request, "message_not_allowed"}));
        }
    return response;
}
} // namespace sfx::apps::runtime
