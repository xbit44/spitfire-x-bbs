#include "linux_runtime_host.hpp"
#include "linux_system_monitor.hpp"
#include "caller_runtime_helpers.hpp"
#include "sfx/adapters/storage/control_panel_statistics.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/botgate_rule_files.hpp"
#include <sodium.h>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace app=application;
using Code=app::LocalControlResponseCode;
app::AlphaRuntimeMetricSnapshot LinuxRuntimeHost::monitor_runtime(){
    std::lock_guard<std::recursive_mutex> lock(operations_);app::AlphaRuntimeMetricSnapshot out;
    const auto now=clock_.monotonic_millis();
    if(!store_.storage()){sqlite_rates_.reset();sqlite_sample_={};sqlite_sample_ms_.reset();}
    else {
        const auto values=store_.storage()->pager_counters();
        if(!values.available||!sqlite_sample_ms_||values.connection_generation!=sqlite_sample_generation_||now<*sqlite_sample_ms_||now-*sqlite_sample_ms_>=1000){
            sqlite_sample_=sqlite_rates_.sample({values.available,values.connection_generation,values.cache_hits,values.cache_misses,values.cache_writes},now);sqlite_sample_ms_=now;sqlite_sample_generation_=values.connection_generation;
        }
    }
    out.sqlite=sqlite_sample_;const auto boot=linux_boot_milliseconds();
    if(bbs_owned()&&bbs_started_boot_ms_&&boot&&*boot>=*bbs_started_boot_ms_){out.uptime_available=true;out.uptime_milliseconds=*boot-*bbs_started_boot_ms_;}
    return out;
}
app::AlphaRuntimeStatistics LinuxRuntimeHost::persisted_statistics(){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!store_.storage()||store_.state()!=app::AlphaRuntimePresentationState::ready)return {};
    const auto config=active_configuration_?active_configuration_:store_.configuration();if(!config)return {};
    const auto zone=platform::linux_host::LinuxBbsTimeZoneRuleProvider::create(config->bbs_time_zone);if(!zone)return {};
    app::BbsLocalTimeAdapters local(zone->configuration(),*zone,clock_);
    const auto range=bbs_today_range(local,clock_.unix_seconds());if(!range)return {};
    return adapters::storage::ControlPanelStatisticsReader::read(root_/"data/sfx.db",(*range)[0],(*range)[1]);
}
app::BotGateFeedStatus LinuxRuntimeHost::botgate_status(){
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto engine=std::atomic_load(&engine_);
    auto result=engine?engine->botgate_status():app::BotGateFeedStatus{};
    const auto config=active_configuration_?active_configuration_:store_.configuration();
    if(config){result.enabled=config->botgate.enabled;result.telnet=config->telnet.bot_gate_enabled;result.ssh=config->botgate.ssh_enabled;}
    if(journal_)journal_->gate_status(result);
    return result;
}
namespace {
constexpr std::size_t botgate_editor_maximum=65535;
std::string rule_revision(std::string_view text){
    std::array<unsigned char,crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(text.data()),text.size());
    std::array<char,crypto_hash_sha256_BYTES*2+1> hex{};
    sodium_bin2hex(hex.data(),hex.size(),digest.data(),digest.size());return hex.data();
}
}
LinuxBotGateRuleFile LinuxRuntimeHost::botgate_rule_file(unsigned index){
    LinuxBotGateRuleFile result;result.file=index;
    std::lock_guard<std::mutex> maintenance(compaction_lifecycle_);
    if(compaction_status().active||offline_status().active){result.detail="Maintenance is active. Read the rule file after it finishes.";return result;}
    std::lock_guard<std::recursive_mutex> lock(operations_);
    const auto config=store_.configuration();
    if(index>=app::kBotGateRuleNames.size()||!store_.owns_installation()||!config){result.detail="Rule file unavailable. Finish BBS setup and reopen the controls.";return result;}
    try{
        // Match the desktop editor's normal private-file initialization.
        // Never adopt foreign files, linked paths or conflicting legacy rules.
        const auto owner=std::to_wstring(geteuid());
        platform::linux_host::initialize_botgate_files(root_,owner,config->botgate);
        const auto raw=platform::linux_host::read_botgate_rule(root_,owner,index);
        result.available=true;result.revision=rule_revision(raw);
        result.editable=index!=4&&raw.size()<=botgate_editor_maximum;
        if(raw.size()<=botgate_editor_maximum)result.text=raw;
        result.detail=raw.size()>botgate_editor_maximum?"This rule file exceeds the 65,535-byte editor limit. Edit it on Linux as the BBS owner; it is read-only here.":
            index==4?"Temporary bans are managed by the BBS and are read-only here.":"Rule-file saves are separate from protection settings. Valid rules reload within about two seconds while the BBS runs.";
    }catch(const std::exception&){result.detail="Rule file could not be read safely. Existing rules were kept. Check ownership, permissions and rule files on Linux.";}
    return result;
}
app::AlphaControlResponse LinuxRuntimeHost::save_botgate_rule_file(unsigned index,std::string_view expected,std::string_view text){
    std::lock_guard<std::mutex> maintenance(compaction_lifecycle_);
    if(compaction_status().active||offline_status().active)return {Code::invalid_request,"maintenance_active"};
    std::lock_guard<std::recursive_mutex> lock(operations_);
    const auto config=store_.configuration();
    if(!store_.owns_installation()||!config)return {Code::invalid_request,"botgate_rule_unavailable"};
    if(index>=app::kBotGateRuleNames.size()||index==4||text.size()>botgate_editor_maximum||
       expected.size()!=64||expected.find_first_not_of("0123456789abcdef")!=expected.npos||
       !app::parse_botgate_patterns(text,index==3))return {Code::invalid_request,"botgate_rule_invalid"};
    try{
        const auto owner=std::to_wstring(geteuid());
        platform::linux_host::initialize_botgate_files(root_,owner,config->botgate);
        const auto current=platform::linux_host::read_botgate_rule(root_,owner,index);
        if(current.size()>botgate_editor_maximum)return {Code::invalid_request,"botgate_rule_invalid"};
        if(rule_revision(current)!=expected)return {Code::invalid_request,"botgate_rule_changed"};
        platform::linux_host::save_botgate_rule(root_,owner,index,text);
        return {Code::ok,"botgate_rule_saved"};
    }catch(const std::exception&){return {Code::invalid_request,"botgate_rule_save_unconfirmed"};}
}
app::AlphaControlResponse LinuxRuntimeHost::update_botgate_feed(){
    std::lock_guard<std::mutex> maintenance(compaction_lifecycle_);
    if(compaction_status().active||offline_status().active)return {Code::invalid_request,"maintenance_active"};
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto engine=std::atomic_load(&engine_);
    if(!engine)return {Code::invalid_request,"feed_requires_running_bbs"};
    return engine->update_botgate_feed()?app::AlphaControlResponse{Code::ok,"feed_update_queued"}:
        app::AlphaControlResponse{Code::invalid_request,"feed_update_already_active_or_unavailable"};
}
}
