#pragma once
#include <charconv>
#include <string>
#include <string_view>

namespace sfx::apps::runtime {
inline std::string linux_startup_message(std::string_view diagnostic){
    for(const auto protocol:{"telnet","ssh"})for(const auto reason:{"port_in_use","port_permission_denied","endpoint_failed"}){
        const auto prefix=std::string(protocol)+"_"+reason+":";
        if(diagnostic.substr(0,prefix.size())!=prefix)continue;
        const auto endpoint=diagnostic.substr(prefix.size());
        const auto split=endpoint.find(':');
        if(split==endpoint.npos)break;
        const auto address=endpoint.substr(0,split),port_text=endpoint.substr(split+1);
        unsigned port{};
        const auto parsed=std::from_chars(port_text.data(),port_text.data()+port_text.size(),port);
        if(address.empty()||address.size()>15||address.find_first_not_of("0123456789.")!=address.npos||
            parsed.ec!=std::errc{}||parsed.ptr!=port_text.data()+port_text.size()||port>65535)break;
        const std::string label=std::string_view(protocol)=="ssh"?"SSH":"Telnet";
        const auto start=label+" could not start on "+std::string(address)+":"+std::to_string(port)+". ";
        if(std::string_view(reason)=="port_in_use")return start+"This address and port are already in use. Choose a free port in Telnet / SSH settings, or stop the other program if intended.";
        if(std::string_view(reason)=="port_permission_denied")return start+"The SFX account does not have permission to use this port. Choose an allowed port or review the service permissions.";
        return start+"The listener could not open; no more specific cause was available. Check the connection settings and available system resources.";
    }
    if(diagnostic=="listeners_disabled")return "The BBS could not start because both Telnet and SSH are disabled. Enable a connection in Telnet / SSH settings.";
    if(diagnostic=="maintenance_active")return "The BBS cannot start while maintenance is active. Wait for it to finish, then try Start again.";
    if(diagnostic=="caller_engine_already_owned")return "The BBS is already running or is still stopping. Refresh its status before trying Start again.";
    if(diagnostic=="scheduler_work_already_active")return "The BBS cannot start while a scheduled job is active. Wait for it to finish, then try Start again.";
    for(const auto code:{"listener_address_invalid","listener_socket_failed","listener_address_unavailable","listener_start_failed",
        "caller_time_zone_unavailable","caller_rate_control_unavailable","sysop_identity_unavailable","sysop_name_unavailable",
        "caller_policy_unpublished","chat_configuration_unavailable","management_journals_unavailable",
        "botgate_rules_unavailable","botgate_feed_worker_unavailable","ssh_backend_identity_in_use",
        "ssh_algorithm_initialization_failed","ssh_initialization_failed","ssh_host_key_owner_missing",
        "ssh_host_key_config_root_insecure","ssh_host_key_unreadable","ssh_host_key_pending_exists_or_unwritable",
        "ssh_host_key_generation_or_protection_failed","ssh_host_key_pending_changed","ssh_host_key_publication_failed",
        "ssh_host_key_publication_flush_failed","ssh_host_key_insecure_or_unreadable","ssh_host_key_invalid"})
        if(diagnostic==code)return "The BBS could not start. Reason: "+std::string(code)+". Check the related configuration or report this reason to the Sysop.";
    return "The BBS could not start. No specific cause was available. Check its status and saved settings before trying Start again.";
}
}
