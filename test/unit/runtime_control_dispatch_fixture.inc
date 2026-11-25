// Test-only host: every unexpected operation throws instead of faking success.
struct DispatchFixture {
 unsigned calls{}; bool fail_bootstrap{};
 template<class... Args> application::LocalDoorSnapshot door_bulletin_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected door_bulletin_snapshot");}
 template<class... Args> application::LocalDoorResult change_door_bulletin(Args&&...) {++calls;throw std::runtime_error("unexpected change_door_bulletin");}
 template<class... Args> application::LocalAccountResult administer_account(Args&&...) {++calls;throw std::runtime_error("unexpected administer_account");}
 template<class... Args> application::BotGateFeedStatus botgate_feed_status(Args&&...) {++calls;throw std::runtime_error("unexpected botgate_feed_status");}
 template<class... Args> application::FileManagementJobStatus cancel_file_management(Args&&...) {++calls;throw std::runtime_error("unexpected cancel_file_management");}
 template<class... Args> application::AlphaControlResponse cancel_local_session(Args&&...) {++calls;throw std::runtime_error("unexpected cancel_local_session");}
 template<class... Args> application::ChatConfigurationResult change_chat_configuration(Args&&...) {++calls;throw std::runtime_error("unexpected change_chat_configuration");}
 template<class... Args> application::LocalAccountResult change_classic_information(Args&&...) {++calls;throw std::runtime_error("unexpected change_classic_information");}
 template<class... Args> application::LocalDoorResult change_door_configuration(Args&&...) {++calls;throw std::runtime_error("unexpected change_door_configuration");}
 template<class... Args> application::LocalFileConfigurationResult change_file_configuration(Args&&...) {++calls;throw std::runtime_error("unexpected change_file_configuration");}
 template<class... Args> application::LocalMessageConfigurationResult change_message_configuration(Args&&...) {++calls;throw std::runtime_error("unexpected change_message_configuration");}
 template<class... Args> application::LocalQwkPolicyResult change_qwk_policy(Args&&...) {++calls;throw std::runtime_error("unexpected change_qwk_policy");}
 template<class... Args> application::LocalAccountResult change_security(Args&&...) {++calls;throw std::runtime_error("unexpected change_security");}
 template<class... Args> application::ChatConfigurationResult chat_configuration(Args&&...) {++calls;throw std::runtime_error("unexpected chat_configuration");}
 template<class... Args> application::ChatOperatorSnapshot chat_operator(Args&&...) {++calls;throw std::runtime_error("unexpected chat_operator");}
 template<class... Args> application::LocalClassicInformationSnapshot classic_information_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected classic_information_snapshot");}
 template<class... Args> application::AlphaControlResponse clear_throttle(Args&&...) {++calls;throw std::runtime_error("unexpected clear_throttle");}
 template<class... Args> application::AlphaControlResponse command_node(Args&&...) {++calls;throw std::runtime_error("unexpected command_node");}
 template<class... Args> std::optional<application::AlphaConfigurationSnapshot> configuration_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected configuration_snapshot");}
 template<class... Args> application::AlphaControlResponse configure_message_schedule(Args&&...) {++calls;throw std::runtime_error("unexpected configure_message_schedule");}
 template<class... Args> application::AlphaControlResponse configure_scheduler(Args&&...) {++calls;throw std::runtime_error("unexpected configure_scheduler");}
 template<class... Args> application::LocalDoorSnapshot door_configuration_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected door_configuration_snapshot");}
 template<class... Args> application::LocalFileConfigurationSnapshot file_configuration_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected file_configuration_snapshot");}
 template<class... Args> application::FileManagementJobStatus file_management_status(Args&&...) {++calls;throw std::runtime_error("unexpected file_management_status");}
 template<class... Args> application::FtnConfigurationResponse ftn_configuration(Args&&...) {++calls;throw std::runtime_error("unexpected ftn_configuration");}
 template<class... Args> application::FtnConfigurationResponse ftn_hatch_catalog(Args&&...) {++calls;throw std::runtime_error("unexpected ftn_hatch_catalog");}
 template<class... Args> application::FtnHeldControlResponse ftn_held(Args&&...) {++calls;throw std::runtime_error("unexpected ftn_held");}
 template<class... Args> application::FtnRuntimeResponse ftn_runtime(Args&&...) {++calls;throw std::runtime_error("unexpected ftn_runtime");}
 template<class... Args> std::optional<application::OperationalLogPage> log_page(Args&&...) {++calls;throw std::runtime_error("unexpected log_page");}
 template<class... Args> application::AlphaControlResponse manual_clamav_notice(Args&&...) {++calls;throw std::runtime_error("unexpected manual_clamav_notice");}
 template<class... Args> application::LocalMessageConfigurationSnapshot message_configuration_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected message_configuration_snapshot");}
 template<class... Args> application::MessageMaintenanceStatus message_maintenance_status(Args&&...) {++calls;throw std::runtime_error("unexpected message_maintenance_status");}
 template<class... Args> application::AlphaRuntimeMetricSnapshot metrics(Args&&...) {++calls;throw std::runtime_error("unexpected metrics");}
 template<class... Args> application::FileManagementReadSnapshot query_file_management(Args&&...) {++calls;throw std::runtime_error("unexpected query_file_management");}
 template<class... Args> application::LocalQwkPolicyResult qwk_policy_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected qwk_policy_snapshot");}
 template<class... Args> application::AlphaControlResponse run_message_maintenance_now(Args&&...) {++calls;throw std::runtime_error("unexpected run_message_maintenance_now");}
 template<class... Args> application::AlphaControlResponse run_scheduler_now(Args&&...) {++calls;throw std::runtime_error("unexpected run_scheduler_now");}
 template<class... Args> application::LocalSecuritySnapshot security_snapshot(Args&&...) {++calls;throw std::runtime_error("unexpected security_snapshot");}
 template<class... Args> application::ChatResult send_chat_notice(Args&&...) {++calls;throw std::runtime_error("unexpected send_chat_notice");}
 template<class... Args> application::SoundPollSnapshot sound_poll(Args&&...) {++calls;throw std::runtime_error("unexpected sound_poll");}
 template<class... Args> application::AlphaControlResponse stage_settings(Args&&...) {++calls;throw std::runtime_error("unexpected stage_settings");}
 template<class... Args> application::FileManagementJobStatus start_file_management(Args&&...) {++calls;throw std::runtime_error("unexpected start_file_management");}
 template<class... Args> application::AlphaLocalSessionStartResult start_local_session(Args&&...) {++calls;throw std::runtime_error("unexpected start_local_session");}
 template<class... Args> application::AlphaRuntimeStatus status(Args&&...) {++calls;throw std::runtime_error("unexpected status");}
 template<class... Args> application::AlphaControlResponse update_botgate_feed(Args&&...) {++calls;throw std::runtime_error("unexpected update_botgate_feed");}
 template<class... Args> application::AlphaConfigurationUpdateResult update_configuration(Args&&...) {++calls;throw std::runtime_error("unexpected update_configuration");}
 app::AlphaControlResponse bootstrap(app::AlphaBootstrapRequest& value) {
   ++calls;check(value.sysop_password=="Fixture account password"&&value.system_password=="Fixture System Password","decoded secrets delivered only to typed host");
   if(fail_bootstrap)throw std::runtime_error("fixture bootstrap failure");
   return {app::LocalControlResponseCode::setup_required,"fixture_setup_required"};
 }
 app::LocalAccountSnapshot account_snapshot(const app::LocalAccountQuery& query) {
   ++calls;check(query.account_id==42,"typed account query retains requested identity");
   return {app::LocalAccountResultCode::unavailable};
 }
 app::AlphaControlResponse command_lifecycle(const app::AlphaLifecycleCommandRequest& value) {
   ++calls;check(value.command==app::AlphaLifecycleCommandKind::cancel_drain&&value.timeout_seconds==0,"typed cancellation preserves command");
   return {app::LocalControlResponseCode::ok,"fixture_cancelled"};
 }
};
const app::LocalControlMessageType malformed_types[]{
 app::LocalControlMessageType::account_command_request,
 app::LocalControlMessageType::account_query_request,
 app::LocalControlMessageType::bootstrap_request,
 app::LocalControlMessageType::botgate_feed_status_request,
 app::LocalControlMessageType::botgate_feed_update_request,
 app::LocalControlMessageType::chat_configuration_change_request,
 app::LocalControlMessageType::chat_configuration_query_request,
 app::LocalControlMessageType::chat_notice_request,
 app::LocalControlMessageType::chat_operator_request,
 app::LocalControlMessageType::clamav_manual_notice_request,
 app::LocalControlMessageType::classic_information_change_request,
 app::LocalControlMessageType::classic_information_query_request,
 app::LocalControlMessageType::configuration_snapshot_request,
 app::LocalControlMessageType::configuration_update_request,
 app::LocalControlMessageType::door_configuration_change_request,
 app::LocalControlMessageType::door_configuration_query_request,
 app::LocalControlMessageType::door_bulletin_query_request,
 app::LocalControlMessageType::door_bulletin_change_request,
 app::LocalControlMessageType::file_configuration_change_request,
 app::LocalControlMessageType::file_configuration_query_request,
 app::LocalControlMessageType::file_management_cancel_request,
 app::LocalControlMessageType::file_management_query_request,
 app::LocalControlMessageType::file_management_start_request,
 app::LocalControlMessageType::file_management_status_request,
 app::LocalControlMessageType::ftn_configuration_request,
 app::LocalControlMessageType::ftn_hatch_catalog_request,
 app::LocalControlMessageType::ftn_held_request,
 app::LocalControlMessageType::ftn_runtime_request,
 app::LocalControlMessageType::lifecycle_command_request,
 app::LocalControlMessageType::local_session_cancel_request,
 app::LocalControlMessageType::local_session_start_request,
 app::LocalControlMessageType::log_page_request,
 app::LocalControlMessageType::message_configuration_change_request,
 app::LocalControlMessageType::message_configuration_query_request,
 app::LocalControlMessageType::message_maintenance_query_request,
 app::LocalControlMessageType::message_maintenance_run_request,
 app::LocalControlMessageType::message_maintenance_schedule_request,
 app::LocalControlMessageType::metrics_request,
 app::LocalControlMessageType::node_command_request,
 app::LocalControlMessageType::qwk_policy_change_request,
 app::LocalControlMessageType::qwk_policy_query_request,
 app::LocalControlMessageType::scheduler_run_request,
 app::LocalControlMessageType::scheduler_settings_request,
 app::LocalControlMessageType::security_change_request,
 app::LocalControlMessageType::security_query_request,
 app::LocalControlMessageType::settings_request,
 app::LocalControlMessageType::sound_poll_request,
 app::LocalControlMessageType::status_request,
 app::LocalControlMessageType::throttle_clear_request
};
