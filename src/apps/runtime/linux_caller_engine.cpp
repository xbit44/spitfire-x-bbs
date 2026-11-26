#include "linux_caller_engine.hpp"
#include "linux_hostname_lookup.hpp"
#include "../product_version.h"
#include "linux_caller_registry.hpp"
#include "linux_caller_session.hpp"
#include "caller_configuration.hpp"
#include "sfx/application/password_change_prompt.hpp"
#include "sfx/platform/linux/door_bulletin_sources.hpp"
#include "caller_runtime_accounting.hpp"
#include "sfx/application/external_events.hpp"
#include "caller_runtime_helpers.hpp"
#include "alpha_trusted_displays.hpp"
#include "alpha_local_door.hpp"
#include "alpha_telnet_stream.hpp"
#include "alpha_durable_journal.hpp"
#include "alpha_botgate_rules.hpp"
#include "alpha_botgate_feed.hpp"
#include "linux_verified_domain.hpp"
#include "linux_rlogin.hpp"
#include "linux_caller_files.hpp"
#include "linux_file_viewer.hpp"
#include "linux_sysop_log_reader.hpp"
#include "linux_sysop_ftn_reader.hpp"
#include "linux_ftn_engine.hpp"
#include "sysop_work.hpp"
#include "newsletter_file.hpp"
#include "sfx/platform/linux/file_administration.hpp"
#include "sfx/platform/network.hpp"
#include "sfx/application/sysop_session.hpp"
#include "qwk_local_time.hpp"
#include "linux_qwk_transfer.hpp"
#include "qwk_packet_notices.hpp"
#include "qwk_runtime_authority.hpp"
#include "qwk_preparation_activity.hpp"
#include "qwk_download_preparation.hpp"
#include "sfx/archive/qwk_packet_builder.hpp"
#include "sfx/application/qwk_session.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/file_scanner.hpp"
#include "sfx/application/caller_login_secret.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include "sfx/adapters/storage/control_panel_statistics.hpp"
#include "sfx/application/runtime_statistics.hpp"
#include "sfx/application/caller_inactivity.hpp"
#include "sfx/application/menu_session.hpp"
#include "sfx/application/message_session.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/application/message_arrival.hpp"
#include "sfx/application/classic_information.hpp"
#include "sfx/application/caller_utilities.hpp"
#include "sfx/application/chat_session.hpp"
#include "sfx/application/door_session.hpp"
#include <climits>
#include <cstdio>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace host=platform::linux_host;
namespace auth=adapters::authentication;
using Code=app::LocalControlResponseCode;
bool write_all(app::IInteractiveSessionByteStream& stream,std::string_view text){
    while(!text.empty()){const auto r=stream.write_some(text);
        if(!app::valid_interactive_stream_write_result(text.size(),r)||r.outcome!=app::InteractiveStreamOutcome::completed||!r.bytes_written)return false;
        text.remove_prefix(r.bytes_written);}return true;
}
}
class LinuxCallerEngine::Impl:public app::ISessionRevocationSink {
public:
    struct LiveSettings {
        app::OperationalConfiguration configuration;
        std::unique_ptr<host::LinuxBbsTimeZoneRuleProvider> zone;
        std::unique_ptr<app::BbsLocalTimeAdapters> local;
        std::shared_ptr<const AlphaSshBackend> ssh;
        LiveSettings(const app::OperationalConfiguration& value,app::IClock& clock,std::shared_ptr<const AlphaSshBackend> backend):configuration(value),ssh(std::move(backend)){
            zone=host::LinuxBbsTimeZoneRuleProvider::create(value.bbs_time_zone);if(!zone)throw std::runtime_error("caller_time_zone_unavailable");
            local=std::make_unique<app::BbsLocalTimeAdapters>(zone->configuration(),*zone,clock);
        }
    };
    struct Statistics final:app::ICallerWelcomeStatisticsPort {
        Impl& owner;app::BbsLocalTimeAdapters& local;app::RuntimeStatisticsPublisher publisher;
        Statistics(Impl& value,app::BbsLocalTimeAdapters& time):owner(value),local(time){}
        app::StorageResult<std::uint64_t> record_successful_call_and_count_today()override{
            const auto range=bbs_today_range(local,owner.clock.unix_seconds());
            if(!range)return {{app::StorageErrorCode::internal,"caller_local_day_unavailable"},{}};
            const auto result=publisher.refresh([&]{
                const auto value=adapters::storage::ControlPanelStatisticsReader::read_checked(owner.root/"data/sfx.db",(*range)[0],(*range)[1]);
                if(!value){auto diagnostic=value.status.operation;
                    for(auto& ch:diagnostic)if(!((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')))ch='_';
                    owner.journal->problem(app::AttentionKind::work_preserved,"caller",diagnostic);}
                return value;
            });
            if(!result)owner.journal->problem(app::AttentionKind::work_preserved,"caller","linux_welcome_statistics_unavailable");
            return result;
        }
    };
    struct Caller final:app::ICallerStageObserver {
        Impl& owner;std::shared_ptr<app::CallerAssignment> assignment;
        std::shared_ptr<const LiveSettings> settings;
        std::unique_ptr<CallerRuntimeAccounting> accounting;
        std::unique_ptr<app::CallerInactivity> inactivity;
        std::optional<app::AuthenticatedCallerReady> ready;
        app::AccountId account{};app::Identifier128 session;
        std::uint64_t last_checkpoint{UINT64_MAX};std::optional<std::int64_t> remaining;
        std::uint64_t authenticated_millis{};
        std::int64_t calling_snapshot_utc{};
        bool timed_out{},idle_warning_pending{};std::uint8_t caller_color{7};
        bool authenticated_as_sysop{},published{},live_sysop{},ended_recorded{},registration_started{};
        app::OperationalContext log_context;
        std::optional<app::CallerStage> last_log_stage;
        std::uint64_t authorization_checkpoint{};
        std::string diagnostic;
        app::CallerLoginSecret login_secret;
        Caller(Impl& o,std::shared_ptr<app::CallerAssignment> a):owner(o),assignment(std::move(a)),settings(std::atomic_load(&o.live_settings)){
            const auto id=assignment->identity();const auto node=assignment->snapshot();log_context.epoch=id.runtime_epoch;log_context.node=id.node;log_context.generation=log_context.attempt=id.generation;
            log_context.source_ip=node.source_ip;log_context.transport=node.connection=="SSH"?app::OperationalTransport::ssh:app::OperationalTransport::telnet;log_context.correlation_id=owner.store.identifiers().generate();
        }
        void log(app::OperationalKind kind,app::OperationalOutcome outcome={})noexcept{
            try{const auto node=assignment->snapshot();log_context.hostname=node.hostname;log_context.duration_seconds=node.connected_seconds;log_context.columns=node.terminal_columns;log_context.rows=node.terminal_rows;
                owner.journal->emit({kind,outcome,log_context});if(kind==app::OperationalKind::connection_ended||kind==app::OperationalKind::authentication_denied||kind==app::OperationalKind::session_failed)ended_recorded=true;
            }catch(...){owner.journal->mark_event_failed();}
        }
        void stage(app::CallerStage value)noexcept override{
            assignment->stage(value);if(last_log_stage==value)return;last_log_stage=value;log_context.stage=value;
            if(value==app::CallerStage::registration&&!registration_started){registration_started=true;log(app::OperationalKind::registration_started);}
            else if(value!=app::CallerStage::cleanup)log(app::OperationalKind::stage_changed);
        }
        void observation(app::CallerWorkflowObservation what,app::AccountId id,app::Identifier128 durable={})noexcept override{
            if(what==app::CallerWorkflowObservation::registration_committed){log_context.committed_account_id=id;log(app::OperationalKind::registration_committed);return;}
            if(what==app::CallerWorkflowObservation::duplicate_denied){log_context.verified_account_id=id;log(app::OperationalKind::duplicate_denied);return;}
            log_context.verified_account_id=id;log_context.durable_session_id=durable;log(app::OperationalKind::session_persisted);
            account=id;session=durable;
            try{
                const auto policy=owner.database().security_policy();
                const auto caller=owner.database().find_account_by_id(account);
                if(!policy||!policy.value||!caller||!caller.value||!caller.value->security_level)throw std::runtime_error("caller_account_policy_unavailable");
                const auto now=owner.clock.monotonic_millis();
                authenticated_millis=now;
                inactivity=std::make_unique<app::CallerInactivity>(now,settings->configuration.caller_afk_minutes,*caller.value->security_level>=policy.value->sysop_threshold);
                accounting=std::make_unique<CallerRuntimeAccounting>(now,[this](std::int64_t utc){return settings->local->format_utc_seconds(utc);},
                    owner.clock,owner.chat,[this](auto sid,auto seconds,const auto& days){return owner.database().persist_private_chat_usage(sid,seconds,days);});
                const auto preference=owner.database().menu_preferences(account);if(preference&&preference.value)caller_color=preference.value->preferences.color;
            }catch(...){assignment->cleanup_health->failed=true;assignment->cancel();}
        }
        void attention(app::AttentionKind kind,app::AccountId id,std::string_view name={},std::uint16_t existing=0)noexcept override{
            try{app::AttentionRecord event;event.kind=kind;event.utc_seconds=owner.clock.unix_seconds();event.node=assignment->identity().node;
                event.existing_node=existing;const auto node=assignment->snapshot();event.transport=node.connection;event.source_ip=node.source_ip;
                event.identity=kind==app::AttentionKind::duplicate_login?app::AttentionIdentity::verified:app::AttentionIdentity::claimed;
                if(!name.empty())event.account_name=name;
                else if(id){const auto value=owner.database().find_account_by_id(id);if(!value||!value.value||!value.value->display_name)throw std::runtime_error("journal_account_unavailable");event.account_name=*value.value->display_name;}
                owner.durable->attention(std::move(event));
            }catch(...){owner.journal->mark_durable_journal_failed();}
        }
        app::SessionAccountingSnapshot accounting_snapshot()override{
            return accounting?accounting->snapshot({session,account,assignment->identity().node},timed_out):app::SessionAccountingSnapshot{};
        }
        bool refresh_access(){
            if(!published||assignment->stopping.load())return false;
            const auto current=owner.database().find_account_by_id(account);const auto policy=owner.database().security_policy();
            if(!current||!current.value||current.value->state!=app::AccountState::active||!current.value->security_level||!policy||!policy.value){assignment->cancel();return false;}
            const auto level=*current.value->security_level;const bool sysop=level>=policy.value->sysop_threshold;
            if(sysop&&!authenticated_as_sysop){assignment->cancel();return false;}
            if(!sysop)authenticated_as_sysop=false;
            if(level!=999&&!owner.registry->enforce_single_session(account))return false;
            if(!owner.registry->refresh_access(assignment->identity(),account,session,sysop,authorization_checkpoint))return false;
            live_sysop=sysop;if(inactivity)inactivity->set_sysop_exemption(sysop);return true;
        }
        bool tick(){
            if(assignment->stopping||assignment->cleanup_health->failed||timed_out)return false;
            if(!accounting)return true;
            const auto second=owner.clock.monotonic_millis()/1000;
            if(second!=last_checkpoint){
                if(!refresh_access())return false;
                const auto usage=accounting_snapshot();const auto caller=owner.database().find_account_by_id(account);
                const auto policy=owner.database().security_policy();
                if(!caller||!caller.value||caller.value->state!=app::AccountState::active||!caller.value->security_level||!policy||!policy.value)return false;
                const auto level=owner.database().security_level(*caller.value->security_level);
                const auto now=owner.clock.unix_seconds();const auto label=settings->local->format_utc_seconds(now);
                if(!level||!level.value||!label||label->size()<10)throw std::runtime_error("caller_limits_unavailable");
                const auto progress=owner.database().persist_session_progress({session,usage.total_seconds,now,usage.daily_seconds});
                const auto used=owner.database().caller_day_seconds(account,label->substr(0,10));
                if(!progress||!used||!used.value)throw std::runtime_error("caller_progress_unavailable");
                remaining=app::caller_remaining_seconds(*level.value,usage.total_seconds,*used.value);last_checkpoint=second;calling_snapshot_utc=now;
                inactivity->set_sysop_exemption(*caller.value->security_level>=policy.value->sysop_threshold);
                if(remaining&&*remaining==0){timed_out=true;return false;}
            }
            if(inactivity){
                const auto action=inactivity->poll(owner.clock.monotonic_millis());
                if(action==app::CallerInactivity::Action::disconnect){timed_out=true;return false;}
                if(action==app::CallerInactivity::Action::warn)idle_warning_pending=true;
            }
            return true;
        }
        void keyboard(){
            if(inactivity)inactivity->keyboard(owner.clock.monotonic_millis());
            idle_warning_pending=false;
        }
    };
    std::filesystem::path root;LinuxRuntimeStore& store;app::IClock& clock;app::Identifier128 epoch;
    app::OperationalConfiguration config;std::string sysop_name;
    std::shared_ptr<const LiveSettings> live_settings;
    std::shared_ptr<LinuxHostnameLookup> hostname_lookup;
    std::shared_ptr<const app::Phase3aConfigurationSnapshot> caller_config;
    std::atomic<std::uint64_t> menu_configuration_revision{1};
    std::shared_ptr<const AlphaSshBackend> ssh_backend;
    AlphaTrustedDisplays displays;app::HostIndependentAccountSessionAdmissionCoordinator admission;
    app::ChatHub chat;app::DoorReservations reservations;
    SysopWork sysop_work;
    std::shared_ptr<LinuxFtnEngine> ftn_engine;
    std::shared_ptr<AlphaOperationalJournal> journal;std::shared_ptr<AlphaDurableJournal> durable;
    std::unique_ptr<AlphaBotGateRules> rules;std::shared_ptr<AlphaBotGateFeed> feed;
    std::unique_ptr<LinuxCallerRegistry> registry;std::unique_ptr<LinuxCallerListener> listener;
    std::unique_ptr<auth::Phase3aRateControlCoordinator> rates;
    std::mutex failed_mutex;std::vector<std::shared_ptr<Caller>> failed_callers;
    std::string diagnostic{"stopped"};bool retired{};
    const std::array<std::uint64_t,app::kAlphaNodePoolCapacity> previous_generations;
    const app::NodeEnablement enabled_nodes;
    Impl(std::filesystem::path r,LinuxRuntimeStore& s,app::IClock& c,app::Identifier128 e,
        std::shared_ptr<AlphaOperationalJournal> j,std::shared_ptr<AlphaDurableJournal> d,std::array<std::uint64_t,app::kAlphaNodePoolCapacity> generations,app::NodeEnablement enabled):
        root(std::move(r)),store(s),clock(c),epoch(e),displays(root),journal(std::move(j)),durable(std::move(d)),previous_generations(generations),enabled_nodes(enabled){}
    adapters::storage::SqliteStorage& database(){if(!store.storage())throw std::runtime_error("caller_store_closed");return *store.storage();}
    bool revoke(const app::SessionRevocationCommand& command)override{return !registry||registry->revoke(command);}
    bool refresh_account_access(app::AccountId account)override{
        if(!registry)return true;
        const auto current=database().find_account_by_id(account);const auto policy=database().security_policy();
        if(!current||!current.value||!current.value->security_level||!policy||!policy.value)return false;
        return registry->refresh_account_authority(account,current.value->state==app::AccountState::active,*current.value->security_level>=policy.value->sysop_threshold,*current.value->security_level==999);
    }
    std::optional<app::MessageActor> sysop_actor(const app::ChatIdentity& identity){
        if(!registry||!registry->sysop_authorized(identity))return {};
        const auto account=database().find_account_by_id(identity.account);const auto policy=database().security_policy();
        if(!account||!account.value||account.value->state!=app::AccountState::active||!account.value->security_level||!policy||!policy.value||*account.value->security_level<policy.value->sysop_threshold||!registry->sysop_authorized(identity))return {};
        return app::MessageActor{identity.session,identity.account,account.value->revision,policy.value->revision};
    }
    bool initial_event_hold{};
    std::atomic<std::int64_t> event_cutoff_utc{};
    app::AlphaControlResponse start(){
        if(listener||retired)return {Code::invalid_request,"caller_engine_already_used"};
        if(store.state()!=app::AlphaRuntimePresentationState::ready||!store.configuration())return {Code::setup_required,"caller_setup_required"};
        try{
            config=*store.configuration();menu_configuration_revision=config.configuration_revision;
            rates=std::make_unique<auth::Phase3aRateControlCoordinator>(database(),clock.unix_seconds(),4095);
            if(!rates->ready())throw std::runtime_error("caller_rate_control_unavailable");
            const auto first=database().first_run_state();if(!first||!first.value||!first.value->staged_sysop_account_id)throw std::runtime_error("sysop_identity_unavailable");
            const auto sysop=database().find_account_by_id(*first.value->staged_sysop_account_id);
            if(!sysop||!sysop.value||!sysop.value->display_name)throw std::runtime_error("sysop_name_unavailable");
            sysop_name=*sysop.value->display_name;
            caller_config=make_caller_configuration(database(),config,sysop_name,SFX_PRODUCT_RUNTIME_VERSION);if(!caller_config)throw std::runtime_error("caller_policy_unpublished");
            const auto chat_configuration=database().chat_configuration({true,{}});
            if(!chat_configuration||!chat_configuration.value||!chat.configure(*chat_configuration.value))throw std::runtime_error("chat_configuration_unavailable");
            const auto owner=std::to_wstring(geteuid());
            ensure_door_node_directories(root,owner,config.logical_nodes);
            if(!journal||!durable)throw std::runtime_error("management_journals_unavailable");
            rules=std::make_unique<AlphaBotGateRules>(root,owner,config.botgate);
            if(config.botgate.enabled&&!rules->snapshot()->ready)throw std::runtime_error("botgate_rules_unavailable");
            feed=std::make_shared<AlphaBotGateFeed>(root,owner,journal);if(!feed->start())throw std::runtime_error("botgate_feed_worker_unavailable");
            if(config.ssh.enabled){ssh_backend=AlphaSshBackend::acquire(root,owner,diagnostic);if(!ssh_backend)throw std::runtime_error(diagnostic);}
            live_settings=std::make_shared<const LiveSettings>(config,clock,ssh_backend);
            hostname_lookup=std::make_shared<LinuxHostnameLookup>(root,config.hostname_lookup);
            registry=std::make_unique<LinuxCallerRegistry>(epoch,LinuxCallerRegistry::Ports{
                [this](auto id,auto wait){return listener&&listener->wait_for_retirement(id,wait);},
                [this]{if(listener)(void)listener->set_draining(true);}});
            LinuxCallerListenerPorts ports{clock,{}, {}};
            ports.admit=[this](std::string_view source,bool ssh,const std::atomic<bool>& stopping){return admit(source,ssh,stopping);};
            ports.serve=[this](int socket,bool ssh,const auto& assignment){return serve(socket,ssh,assignment);};
            ports.observe=[this](app::OperationalEvent event){event.context.epoch=epoch;
                if(!event.context.attempt&&(event.kind==app::OperationalKind::capacity_refused||event.kind==app::OperationalKind::source_refused||event.kind==app::OperationalKind::runtime_refused))event.context.attempt=journal->next_attempt();
                journal->emit(std::move(event));};
            listener=std::make_unique<LinuxCallerListener>(epoch,config,std::move(ports),enabled_nodes,previous_generations);
            listener->set_external_event_hold(initial_event_hold);
            if(!listener->start())throw std::runtime_error(listener->status().diagnostic);
            diagnostic="linux_runtime_development_online";return {Code::ok,diagnostic};
        }catch(const std::exception& error){diagnostic=error.what();(void)stop();return {Code::recovery_required,diagnostic};}
    }
    bool stop()noexcept{
        bool clean=true;retired=true;
        if(listener){listener->stop();const auto state=listener->status();clean=state.pool.active==0&&state.diagnostic!="caller_cleanup_failed";}
        if(const auto lookup=std::atomic_exchange(&hostname_lookup,std::shared_ptr<LinuxHostnameLookup>{}))lookup->stop();
        try{sysop_work.stop();}catch(...){clean=false;}
        if(feed)feed->stop();
        rules.reset();
        if(durable)durable->wake();
        if(journal)clean=journal->flush()&&clean;
        return clean&&(!registry||registry->healthy())&&failed_callers.empty();
    }
    LinuxConnectionAdmission admit(std::string_view source,bool ssh,const std::atomic<bool>& stopping){
        const auto settings=std::atomic_load(&live_settings);const auto& config=settings->configuration;
        if(stopping.load())return {};
        if(!app::botgate_filters_enabled(config.botgate,config.telnet.bot_gate_enabled,ssh))return {true,false};
        journal->gate_count(0);const auto snapshot=rules->snapshot();const auto now=clock.unix_seconds();
        const auto ip=app::parse_botgate_ipv4(source);const bool exempt=snapshot->exempt(source,now);
        auto reason=snapshot->refusal(source,"",now);if(!snapshot->ready)reason="rules_unavailable";
        if(!ip)reason="invalid_source";
        if(!exempt&&reason.empty()&&!rules->arrive(source,clock.monotonic_millis()/1000,now))reason="temporary_rate_ban";
        if(!exempt&&reason.empty()&&!snapshot->lists[3].empty()&&ip){const auto domain=verify_linux_peer_domain(root,*ip,stopping);
            if(domain.completed)reason=snapshot->refusal(source,domain.name,now);}
        const auto downloaded=feed->snapshot();
        if(!exempt&&reason.empty()&&ip&&!app::botgate_patterns_match(snapshot->lists[5],source,now)&&std::binary_search(downloaded->begin(),downloaded->end(),*ip))reason="downloaded_feed";
        app::OperationalContext context;context.epoch=epoch;context.source_ip=source;context.attempt=journal->next_attempt();context.transport=ssh?app::OperationalTransport::ssh:app::OperationalTransport::telnet;
        if(!reason.empty()){
            context.diagnostic_code=reason=="ip.can"?"manual_ip_rule":reason=="host.can"?"verified_domain_rule":reason=="temp_ip.can"?"temporary_ban":reason.rfind("geo/",0)==0?"geo_country_rule":reason;
            if(reason=="ip-silent.can")journal->gate_count(4);else journal->emit({app::OperationalKind::botgate_blocked,app::OperationalOutcome::denied,context});
            return {};
        }
        // Admission precedes a node assignment. The shared bypass record needs
        // an assignment, so count the real exemption without inventing one.
        if(exempt)journal->gate_count(5);
        return {!stopping.load(),exempt};
    }
    bool challenge(app::IInteractiveSessionByteStream& stream,const std::shared_ptr<app::CallerAssignment>& assignment,bool ssh,const app::OperationalConfiguration& config){
        if(!config.botgate.enabled||!(ssh?config.botgate.ssh_enabled:config.telnet.bot_gate_enabled))return true;
        struct Active{AlphaOperationalJournal& journal;explicit Active(AlphaOperationalJournal& j):journal(j){journal.gate_active(true);}~Active(){journal.gate_active(false);}}active(*journal);
        std::string artwork;if(const auto reader=open_native_display_reader(root/"display")){const auto asset=reader->read_asset("bg-prelog.ans",16384);if(asset.code==app::TrustedDisplayAssetReadCode::available)artwork=asset.bytes;}
        app::BotGateChallenge gate(clock.monotonic_millis(),config.botgate.timeout_seconds);unsigned last=UINT_MAX;bool last_ansi{};
        const auto log=[&](app::OperationalKind kind,app::OperationalOutcome outcome){app::OperationalContext context;context.epoch=epoch;context.node=assignment->identity().node;
            context.generation=assignment->identity().generation;context.attempt=assignment->identity().generation;context.source_ip=assignment->snapshot().source_ip;context.transport=ssh?app::OperationalTransport::ssh:app::OperationalTransport::telnet;journal->emit({kind,outcome,context});};
        while(!assignment->stopping.load()){
            const auto now=clock.monotonic_millis();const auto state=gate.poll(now);
            if(state==app::BotGateChallenge::Result::passed){log(app::OperationalKind::botgate_passed,{});return write_all(stream,last_ansi?"\x1b[0m\x1b[2J\x1b[H":"\r\n\r\n");}
            if(state==app::BotGateChallenge::Result::timed_out){log(app::OperationalKind::botgate_timed_out,app::OperationalOutcome::timed_out);return false;}
            const bool ansi=ssh?static_cast<AlphaSshStream&>(stream).ansi_terminal():static_cast<AlphaTelnetStream&>(stream).ansi_terminal();
            const auto remaining=gate.remaining(now);
            if(last!=remaining||last_ansi!=ansi){const auto text=!ansi&&last!=UINT_MAX?"\rYou have "+std::to_string(remaining)+" seconds.    ":app::render_botgate_art(artwork,remaining,ansi,config.botgate.timeout_seconds>=100?3:2);
                if(!write_all(stream,text))return false;
                last=remaining;last_ansi=ansi;}
            const auto input=stream.read_some(app::kInteractiveStreamMaximumReadBytes);if(input.outcome==app::InteractiveStreamOutcome::timed_out)continue;
            if(input.outcome!=app::InteractiveStreamOutcome::completed)return false;
            if(gate.input({reinterpret_cast<const char*>(input.bytes.data()),input.bytes.size()},clock.monotonic_millis())==app::BotGateChallenge::Result::invalid){log(app::OperationalKind::botgate_failed,app::OperationalOutcome::denied);return false;}
        }
        return false;
    }
    bool serve(int socket,bool ssh,const std::shared_ptr<app::CallerAssignment>& assignment){
        if(!registry->begin(assignment)){
            // Stop may win before this worker publishes its prelogin entry.
            // No account/session resources exist at this point.
            return registry->healthy()&&assignment->stopping.load()&&!assignment->cleanup_health->failed.load();
        }
        auto caller=std::make_shared<Caller>(*this,assignment);
        // Capture the accepted socket endpoints, including kernel-selected ports.
        // Local browser socket pairs have no TCP ports and retain zero.
        sockaddr_storage endpoint{};socklen_t endpoint_size=sizeof(endpoint);
        if(getsockname(socket,reinterpret_cast<sockaddr*>(&endpoint),&endpoint_size)==0&&endpoint.ss_family==AF_INET)
            caller->log_context.local_port=ntohs(reinterpret_cast<const sockaddr_in*>(&endpoint)->sin_port);
        endpoint={};endpoint_size=sizeof(endpoint);
        if(getpeername(socket,reinterpret_cast<sockaddr*>(&endpoint),&endpoint_size)==0&&endpoint.ss_family==AF_INET)
            caller->log_context.peer_port=ntohs(reinterpret_cast<const sockaddr_in*>(&endpoint)->sin_port);
        caller->log(app::OperationalKind::connection_assigned);
        if(const auto lookup=std::atomic_load(&hostname_lookup))lookup->subscribe(assignment);
        const auto& config=caller->settings->configuration;const auto& local=caller->settings->local;Statistics statistics(*this,*local);
        caller->authorization_checkpoint=registry->authorization_checkpoint();
        try{
            auto credentials=auth::Phase3aCredentialOperationsBridge::create(*store.authentication(),store.password_kdf(),clock.unix_seconds(),assignment->cancellation);
            // The dummy credential KDF observes cancellation. A cancelled
            // prelogin setup has no durable session to recover.
            if(!credentials&&assignment->cancellation.is_cancelled()){caller->log(app::OperationalKind::connection_ended,app::OperationalOutcome::cancelled);return registry->forget(assignment->identity(),!assignment->cleanup_health->failed.load());}
            if(!credentials||!rates||!rates->ready())throw std::runtime_error("caller_authentication_unavailable");
            const auto source=assignment->snapshot().source_ip;
            core::Session session("linux-"+std::to_string(assignment->identity().generation),*core::NodeNumber::create(assignment->identity().node),
                {ssh?core::TransportKind::ssh:core::TransportKind::telnet,source,{},""});
            app::Phase3aPrivateCompositionDependencies dependencies{database(),*credentials,database(),database(),database(),database(),admission,database(),
                displays,*rates,*local,statistics,*local,clock,store.identifiers(),caller.get(),assignment->cleanup_health,&durable->new_names(),
                [&](app::AccountId id,const app::CredentialEnvelope& proof,std::string_view password){caller->login_secret.capture(id,proof,password);}};
            const auto policy_snapshot=std::atomic_load(&caller_config);if(!policy_snapshot)throw std::runtime_error("caller_policy_unavailable");
            LinuxCallerSessionPorts ports{dependencies,*policy_snapshot,session,{source,caller->log_context.correlation_id,{}},assignment->cancellation,
                assignment->stopping,caller->settings->ssh,{},config.login_afk_seconds,{},{},{},{},{},{},{}};
            ports.authorize_ready=[&](const auto& ready){
                const bool sysop=ready.security_level>=policy_snapshot->security_policy().sysop_security_threshold;
                if(!registry->publish(assignment->identity(),ready.account_id,ready.durable_session_id,sysop))return false;
                caller->authenticated_as_sysop=sysop;caller->published=true;
                return caller->refresh_access();
            };
            ports.admission_gate=[&](auto& stream){return challenge(stream,assignment,ssh,config);};
            ports.output=[assignment](auto bytes){assignment->output(bytes);};
            ports.payload_activity=[assignment](bool input){assignment->payload_activity(input);};
            ports.resize=[assignment](auto size){assignment->resize(size.columns,size.rows);};
            ports.encrypted=[assignment]{assignment->encrypted_transport_established();};
            ports.menus=[&](auto& ready,auto& stream){
                try{return menus(*caller,ready,stream,ssh);}
                catch(const std::exception&){caller->diagnostic="linux_caller_menu_exception";throw;}
            };
            auto result=run_linux_caller_session(socket,ssh,std::move(ports));caller->ready=std::move(result.ready);caller->login_secret.clear();
            if(result.login==app::Phase3aWorkflowCode::authentication_denied)caller->log(app::OperationalKind::authentication_denied,app::OperationalOutcome::denied);
            else caller->log(app::OperationalKind::connection_ended,caller->timed_out||result.login==app::Phase3aWorkflowCode::timed_out?app::OperationalOutcome::timed_out:result.normal_logoff?app::OperationalOutcome::normal:assignment->stopping.load()?app::OperationalOutcome::cancelled:app::OperationalOutcome::disconnected);
            if(result.login==app::Phase3aWorkflowCode::storage_failure||result.login==app::Phase3aWorkflowCode::internal_failure)
                journal->problem(app::AttentionKind::work_preserved,"caller","linux_login_workflow_failure_"+std::to_string(static_cast<unsigned>(result.login)));
            if(!caller->diagnostic.empty())journal->problem(app::AttentionKind::work_preserved,"caller",caller->diagnostic);
            if(caller->ready){const auto outcome=caller->timed_out?app::SessionOutcomeCode::timed_out:result.normal_logoff?app::SessionOutcomeCode::normal:
                    assignment->stopping.load()?app::SessionOutcomeCode::shutdown:app::SessionOutcomeCode::disconnected;
                const bool finalized=caller->ready->durable_cleanup->finalize(0,outcome);
                const bool released=caller->ready->admission_lease->release();if(!finalized||!released)assignment->cleanup_health->failed=true;
                if(!assignment->cleanup_health->failed)caller->ready.reset();
                durable->wake();}
        }catch(...){caller->log(app::OperationalKind::session_failed,app::OperationalOutcome::internal_failure);assignment->cleanup_health->failed=true;assignment->cancel();}
        caller->login_secret.clear();
        if(!caller->ended_recorded)caller->log(app::OperationalKind::connection_ended,app::OperationalOutcome::cancelled);
        if(caller->log_context.account_id)journal->sound(ssh?app::SoundEvent::ssh_logout:app::SoundEvent::telnet_logout);
        const bool clean=!assignment->cleanup_health->failed.load();
        if(!clean){std::lock_guard<std::mutex> lock(failed_mutex);failed_callers.push_back(std::move(caller));}
        return registry->forget(assignment->identity(),clean)&&clean;
    }
    bool menus(Caller& caller,app::AuthenticatedCallerReady& ready,app::IInteractiveSessionByteStream& stream,bool ssh){
        auto config=caller.settings->configuration;const auto& local=caller.settings->local;const auto& zone=caller.settings->zone;
        auto assignment=caller.assignment;assignment->authenticated(ready.display_name,ready.account_id);
        caller.log_context.account_id=ready.account_id;caller.log_context.caller=ready.display_name;caller.log_context.durable_session_id=ready.durable_session_id;
        caller.log(app::OperationalKind::authenticated); // The journal emits the typed login sound once.
        MenuModalStream guarded(stream,[&]{return caller.tick();},[&]{caller.keyboard();});
        app::StreamCallerTerminal terminal(guarded,{app::SecretInputEchoMode::masked,config.menus.text_color,config.menus.input_color,&caller.caller_color});
        (void)terminal.set_text_style(ready.graphics==app::GraphicsMode::ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        const auto message_actor=[&]() -> std::optional<app::MessageActor>{
            if(!caller.tick()||!caller.refresh_access())return {};
            const auto account=database().find_account_by_id(ready.account_id);const auto policy=database().security_policy();
            if(!account||!account.value||account.value->state!=app::AccountState::active||!policy||!policy.value)return {};
            return app::MessageActor{ready.durable_session_id,ready.account_id,account.value->revision,policy.value->revision};
        };
        const auto own_name=[&]{const auto account=database().find_account_by_id(ready.account_id);
            return account&&account.value?account.value->display_name.value_or("Removed caller"):std::string("Removed caller");};
#include "linux_chat_runtime_session.inc"
        const auto message_date=[&](std::int64_t utc){return local->format_utc_seconds(utc).value_or("Unavailable");};
        app::MessageQuestionTerminal message_terminal(terminal,[&]{return ready.graphics==app::GraphicsMode::ansi;},
            [&](std::string_view text){return write_all(guarded,text);},
            [&]{return assignment->snapshot().terminal_columns;},
            [&]{return assignment->snapshot().terminal_rows;});
        app::MessageComposePorts compose{database(),message_terminal,message_actor,[&]{return store.identifiers().generate();},
            [&]{const auto node=assignment->snapshot();return app::MessageEditorDimensions{ready.graphics==app::GraphicsMode::ansi,node.terminal_columns,node.terminal_rows};},
            [&]{struct Restore{MenuModalStream& stream;~Restore(){stream.preserve_poll_timeouts(false);}}restore{guarded};
                guarded.preserve_poll_timeouts(true);return terminal.read_editor_byte();},
            [&](std::string_view text){return write_all(guarded,text);},
            [&]() -> std::optional<std::pair<app::MessageTarget,std::string>>{
                const auto actor=message_actor();if(!actor)return {};
                const auto target=database().system_sysop_target(*actor);if(!target||!target.value)return {};
                const auto recipient=database().resolve_message_recipient(*actor,std::to_string(*target.value->recipient));
                if(!recipient||!recipient.value||recipient.value->entries.size()!=1)return {};
                return std::make_pair(*target.value,recipient.value->entries.front().name);
            }};
        compose.accepted_key=[&](bool carriage_return){terminal.finish_editor_key(carriage_return);};
        compose.text_color=[&]{return caller.caller_color>0&&caller.caller_color<=15?caller.caller_color:config.menus.text_color;};
        std::vector<app::MenuTextRow> notifications;
        compose.unavailable_notice=[&](std::string_view text){
            const auto width=std::clamp<std::size_t>(assignment->snapshot().terminal_columns,2,4096)-1;
            for(const auto& row:app::wrap_message_line(text,width,false)){
                const auto first=row.text.find_first_not_of(' ');if(first==std::string::npos)continue;
                const auto last=row.text.find_last_not_of(' ');
                notifications.push_back({app::MenuTextRole::notification,row.text.substr(first,last-first+1)});
            }return true;
        };
        compose.real_name_editable=!std::atomic_load(&caller_config)->real_names_required();
        const auto initial_actor=message_actor();if(!initial_actor)return false;
        const auto initial_counts=database().message_caller_statistics(*initial_actor,{});
        if(!initial_counts||!initial_counts.value)return false;
        std::int64_t dispatch_fence=initial_counts.value->last_arrival;
        app::MessageArrivalQueue arrivals(ready.account_id,dispatch_fence);
        std::uint64_t arrival_presentations{};
        const auto message_boundary=[&](bool menu){
            const auto actor=message_actor();if(!actor)return false;
            for(unsigned page=0;page<4;++page){
                const auto work=database().message_arrival_page(*actor,dispatch_fence,100);
                if(!work||!work.value)return true; // Retry later; no false empty witness.
                for(const auto& item:work.value->arrivals)if(!arrivals.enqueue(item))return false;
                dispatch_fence=work.value->next_after.value_or(work.value->fence);
                if(!work.value->next_after)break;
            }
            // Collect during modal commands, but present only on the menu's own screen.
            if(!menu)return true;
            bool storage_failed=false;std::string sender;
            const auto accepted=arrivals.present_checked(true,[&](app::MessageId id)->std::optional<bool>{
                const auto live=message_actor();if(!live){storage_failed=true;return {};}
                const auto result=database().message_arrival_sender(*live,id);
                if(!result||!result.value){storage_failed=true;return {};}
                sender.clear();if(!result.value->has_value())return false;
                sender=**result.value;return true;
            },[&]()->std::optional<bool>{
                const auto live=message_actor();if(!live){storage_failed=true;return {};}
                sender.clear();const auto result=database().has_addressed_unread(*live,false);
                if(!result||!result.value){storage_failed=true;return {};}
                return *result.value;
            },[&](std::string_view text){
                const auto rendered=sender.empty()?std::string(text):app::message_arrival_notice(sender);
                notifications.push_back({app::MenuTextRole::notification,rendered});
                ++arrival_presentations;
                return true;
            });
            return accepted||storage_failed;
        };
        app::MessageCallerSession messages({std::move(compose),own_name,message_date,[&]{return message_boundary(false);},
            []{return app::TerminalDisplayPagingPolicy{true,std::uint16_t{24}};},
            [&]{return config.menus.lightbars_enabled;},
            [&]{return std::atomic_load(&live_settings)->configuration.menus.legacy_message_pipe_colors;}});
        if(!messages.initialize())return false;
#include "linux_file_runtime_session.inc"
#include "linux_qwk_runtime_session.inc"
        app::MenuSessionPorts ports;
        app::CallerTimeWarnings calling_time_warnings;
        const auto effective_seconds = [&] {
            return app::event_limited_calling_seconds(caller.remaining, event_cutoff_utc.load(), clock.unix_seconds());
        };
        ports.calling_time_warning = [&] { return calling_time_warnings.take(effective_seconds()); };
        ports.snapshot=[&]() -> std::optional<app::MenuSessionSnapshot>{
            if(!caller.tick()||!caller.refresh_access()||!chat_refresh())return {};
            config=std::atomic_load(&live_settings)->configuration;
            const auto account=database().find_account_by_id(ready.account_id);const auto preference=database().menu_preferences(ready.account_id);
            const auto label=local->format_utc_seconds(clock.unix_seconds());
            if(!account||!account.value||!account.value->security_level||!preference||!preference.value||!label||label->size()<19)return {};
            app::MenuSessionSnapshot result;const auto node=assignment->snapshot();
            result.access={*account.value->security_level,caller.live_sysop,static_cast<std::uint64_t>(account.value->revision)};
            result.configuration_revision=menu_configuration_revision.load();result.preferences=*preference.value;result.graphics=ready.graphics;
            result.columns=node.terminal_columns;result.rows=node.terminal_rows;result.compact_footer=config.menus.compact_footer;
            result.lightbars_enabled=config.menus.lightbars_enabled;result.default_color=config.menus.text_color;result.input_color=config.menus.input_color;
            result.selected_foreground=config.menus.selected_foreground;result.selected_background=config.menus.selected_background;
            auto& context=result.context;context.bbs_name=config.bbs_name;context.sysop=sysop_name;context.version=SFX_PRODUCT_RUNTIME_VERSION;
            context.alias=ready.display_name;context.name=ready.display_name;context.node=std::to_string(ready.node_id);context.nodes=std::to_string(config.logical_nodes);
            context.transport=ssh?"SSH":"Telnet";context.date=label->substr(0,10);context.time=label->substr(11,8);context.datetime=*label;context.security_level=std::to_string(*account.value->security_level);
            context.time_on=std::to_string((clock.monotonic_millis()-caller.authenticated_millis)/1000U);
            const auto remaining = effective_seconds();
            context.time_left=remaining?std::to_string(*remaining):"Unlimited";
            context.logon_minutes=remaining?std::to_string(*remaining/60):"Unlimited";
            if(!messages.refresh_selection())return {};
            const auto actor=message_actor();if(!actor)return {};
            const auto counts=database().message_caller_statistics(*actor,messages.selection()?std::optional<app::LocalEchoId>{messages.selection()->echo.id}:std::nullopt);
            if(!counts||!counts.value)return {};
            context.posts=std::to_string(counts.value->messages_entered);context.mail_total=std::to_string(counts.value->mail_total);context.mail_unread=std::to_string(counts.value->mail_unread);
            if(messages.selection()){
                result.message_group=messages.selection()->group.name;result.message_echo=messages.selection()->echo.name;
                context.message_conference=result.message_echo;context.message_conference_number=std::to_string(messages.selection()->echo.conference_number);
                if(counts.value->echo_count)context.message_count=std::to_string(*counts.value->echo_count);
                if(counts.value->new_messages)context.new_messages=std::to_string(*counts.value->new_messages);
            }
            const auto room=chat_session.selected_room();
            const auto file_context=files.context();if(!file_context)return {};
            result.file_group=app::file_terminal_text(file_context->group,64);result.file_area=app::file_terminal_text(file_context->area,64);result.file_area_number=file_context->number;
            if(file_context->id.value){context.file_area=result.file_area;context.file_area_number=std::to_string(result.file_area_number);}
            const auto file_state=database().file_caller_state(*actor);
            const auto file_stats=database().file_caller_statistics(*actor,file_local_date());if(!file_state||!file_state.value||!file_stats||!file_stats.value)return {};
            context.uploads=std::to_string(file_stats.value->counts.successful_uploads);context.downloads=std::to_string(file_stats.value->counts.successful_downloads);
            context.uploaded_kb=std::to_string(file_stats.value->upload_bytes/1024);context.downloaded_kb=std::to_string(file_stats.value->download_bytes/1024);
            if(file_context->id.value){app::FileQuery query;query.area=file_context->id;const auto page=database().file_page(*actor,query,{},1);if(page&&page.value)context.file_count=std::to_string(page.value->total_files);
                query.new_only=query.unseen_only=true;query.since_utc=file_state.value->since_utc;const auto unread=database().file_page(*actor,query,{},1);if(unread&&unread.value)context.new_files=std::to_string(unread.value->total_files);}
            context.chat_room=room?room->name:std::string("None");
            return result;
        };
        ports.artwork=[&](const auto& request){return displays.select(request);};
        ports.display=[&](const auto& program){
            const auto rows=assignment->snapshot().terminal_rows;
            const auto height=static_cast<std::uint16_t>(std::clamp<unsigned>(rows?rows-1U:0U,10,24));
            const bool ok=terminal.present_display({"menu.artwork",&program,{true,height}})==app::TerminalOutcome::completed;
            if(!ok)caller.diagnostic="linux_menu_display_failed";
            return ok;
        };
        ports.write=[&](auto text){return write_all(guarded,text);};
        bool menu_notice_surface=false;
        ports.safe_boundary=[&](std::string_view){menu_notice_surface=true;return caller.tick();};
        ports.read=[&](app::MenuInputDecoder& decoder,const std::function<bool()>& redraw)->std::optional<app::MenuInput>{
            MenuDelimiterStream menu_stream(guarded,terminal,decoder);
            chat.safe(chat_identity,true);
            struct Restore{MenuModalStream& stream;bool& surface;app::ChatHub& hub;app::ChatIdentity id;
                ~Restore(){stream.preserve_poll_timeouts(false);surface=false;hub.safe(id,false);}}restore{guarded,menu_notice_surface,chat,chat_identity};
            guarded.preserve_poll_timeouts(true);
            while(caller.tick()){
                if(menu_notice_surface&&decoder.pending_text().empty()){
                    if(calling_time_warnings.pending(effective_seconds())) return app::MenuInput{app::MenuInputKind::redraw};
                    const auto before=arrival_presentations;
                    const auto chat_before=chat_session.presentations();const auto notices_before=notifications.size();
                    if(!chat_refresh()||!chat_session.safe_events([&](std::string text){notifications.push_back({app::MenuTextRole::feedback,std::move(text)});})||!message_boundary(true))return std::nullopt;
                    if((before!=arrival_presentations||chat_before!=chat_session.presentations()||notices_before!=notifications.size())&&!redraw())return std::nullopt;
                }
                const auto input=menu_stream.read_some(1);
                if(input.outcome==app::InteractiveStreamOutcome::timed_out){
                    const auto pending=decoder.idle();
                    if(pending.kind!=app::MenuInputKind::pending)return pending;
                    continue;
                }
                if(input.outcome!=app::InteractiveStreamOutcome::completed)return std::nullopt;
                if(!input.bytes.empty())notifications.clear();
                for(const auto byte:input.bytes){
                    const auto before=std::string(decoder.pending_text());
                    const auto decoded=decoder.feed(byte);
                    const auto after=std::string(decoder.pending_text());
                    const auto echo=app::menu_input_echo(byte,before,decoded,after);
                    if(!echo.empty()&&!write_all(guarded,echo))return std::nullopt;
                    if(decoded.kind!=app::MenuInputKind::pending){
                        return decoded;
                    }
                }
            }
            return std::nullopt;
        };
        ports.save_preferences=[&](const auto& request){return request.account_id==ready.account_id&&database().update_menu_preferences(request).succeeded();};
        ports.transfer_preference=[&]() -> std::optional<app::TransferPreferenceRecord>{
            if(!caller.tick())return {};
            const auto value=database().transfer_preference(ready.account_id);return value&&value.value?value.value:std::nullopt;
        };
        ports.save_transfer_preference=[&](const app::UpdateTransferPreferenceRequest& request){return caller.tick()&&request.account_id==ready.account_id&&database().update_transfer_preference(request).succeeded();};
#include "linux_sysop_runtime_session.inc"
        ports.action=[&](app::MenuAction action)->std::optional<std::string>{
            notifications.clear();if(!caller.tick())return {};
            if(action==app::MenuAction::sysop_utilities)return sysop_menu();
            if(action>=app::MenuAction::qwk_download&&action<=app::MenuAction::qwk_pointers){message_terminal.menu_boundary();return offline.action(action);}
            if(action>=app::MenuAction::message_change&&action<=app::MenuAction::comment_sysop){message_terminal.menu_boundary();return messages.action(action);}
            if(action==app::MenuAction::graphics){
                if(ready.graphics==app::GraphicsMode::ansi&&!write_all(guarded,"\x1b[0m"))return {};
                ready.graphics=ready.graphics==app::GraphicsMode::ansi?app::GraphicsMode::ascii:app::GraphicsMode::ansi;
                return ready.graphics==app::GraphicsMode::ansi?"Graphics: ANSI.":"Graphics: ASCII.";
            }
            if(action==app::MenuAction::other_boards){
                return app::run_other_board_session(ports,terminal,[&](std::int64_t after){
                    if(!caller.tick())return app::StorageResult<app::OtherBoardPage>{{app::StorageErrorCode::internal,"caller_unavailable"},{}};
                    return database().other_boards(after);
                },[&](const app::OtherBoardValues& values){
                    if(!caller.tick())return false;
                    const auto account=database().find_account_by_id(ready.account_id);
                    if(!account||!account.value||account.value->state!=app::AccountState::active)return false;
                    app::ClassicInformationChange change;change.values=values;
                    return database().change_classic_information(change,{false,ready.account_id,account.value->revision,clock.unix_seconds(),store.identifiers().generate()}).succeeded();
                },[&](const app::OtherBoardRecord& shown){
                    if(!caller.tick()||!caller.refresh_access()||!caller.live_sysop)return false;
                    const auto account=database().find_account_by_id(ready.account_id);
                    if(!account||!account.value||account.value->state!=app::AccountState::active||account.value->security_level!=999)return false;
                    // The shared storage also rechecks level999 in its writer.
                    app::ClassicInformationChange change;change.action=app::ClassicInformationAction::remove_board;change.id=shown.id;change.expected_revision=shown.revision;
                    return database().change_classic_information(change,{false,ready.account_id,account.value->revision,clock.unix_seconds(),store.identifiers().generate()}).succeeded();
                });
            }
            if(action==app::MenuAction::system_information){
                const auto range=bbs_today_range(*local,clock.unix_seconds());
                if(!range)return "System information is unavailable. Please try again later.";
                const auto counts=database().system_information_counters((*range)[0],(*range)[1]);const auto opening=database().opening_date();
                if(!counts||!counts.value||!opening||!opening.value)return "System information is unavailable. Please try again later.";
                auto date=opening.value->configured_date;
                if(!date){const auto initialized=local->format_utc_seconds(opening.value->initialization_utc);if(initialized&&initialized->size()>=10)date=initialized->substr(0,10);}
                return app::show_system_information(ports,*counts.value,date.value_or("Unavailable"))?std::optional<std::string>{""}:std::nullopt;
            }
            if(action==app::MenuAction::caller_profile||action==app::MenuAction::caller_directory||action==app::MenuAction::locate_caller){
                const auto policy=std::atomic_load(&caller_config);
                app::CallerUtilityPorts utilities(terminal,ready.account_id);
                utilities.rules={!policy->real_names_required(),policy->location_mode()==app::RegistrationFieldMode::required,
                    policy->email_mode()==app::EmailRegistrationMode::required,policy->sex_mode()==app::RegistrationFieldMode::required};
                utilities.page_rows=std::clamp<unsigned>(assignment->snapshot().terminal_rows,12,25)-5;
                utilities.account=[&]() -> std::optional<app::AccountRecord>{
                    if(!caller.tick()||!caller.refresh_access())return {};
                    const auto account=database().find_account_by_id(ready.account_id);
                    return account&&account.value?account.value:std::nullopt;
                };
                utilities.save_profile=[&](const app::AccountRecord& shown,const app::AccountProfile& replacement){
                    if(!caller.tick()||!caller.refresh_access()||shown.account_id!=ready.account_id||
                        !app::valid_caller_profile_change(shown.profile,replacement,utilities.rules))return false;
                    const auto authoritative=database().find_account_by_id(ready.account_id);
                    if(!authoritative||!authoritative.value||authoritative.value->state!=app::AccountState::active||
                        authoritative.value->revision!=shown.revision||!app::valid_caller_profile_change(authoritative.value->profile,replacement,utilities.rules))return false;
                    return database().update_profile({ready.account_id,shown.revision,replacement,
                        {store.identifiers().generate(),ready.correlation_id,ready.account_id,clock.unix_seconds(),app::AuditReasonCode::caller_action}}).succeeded();
                };
                utilities.history=[&](const app::HistoryQuery& query){
                    if(!caller.tick()||query.account_id!=ready.account_id)
                        return app::StorageResult<std::vector<app::SessionRecord>>{{app::StorageErrorCode::invalid_argument,"caller_identity_mismatch"},{}};
                    return database().session_history(query);
                };
                utilities.directory=[&](const app::CallerDirectoryQuery& query){
                    if(!caller.tick())return app::StorageResult<std::vector<app::CallerDirectoryEntry>>{{app::StorageErrorCode::internal,"caller_unavailable"},{}};
                    return database().caller_directory(query);
                };
                utilities.local_time=[&](std::int64_t utc){return local->format_utc_seconds(utc);};
                utilities.utc_now=[&]{return clock.unix_seconds();};
                utilities.ghost=[&]() -> std::optional<bool>{if(!caller.live_sysop||!chat_refresh())return {};const auto me=chat.participant(chat_identity);return me?std::optional<bool>{me->preferences.ghost}:std::nullopt;};
                utilities.toggle_ghost=[&]{
                    const auto actor=message_actor();if(!actor||!caller.live_sysop)return false;
                    const auto old=database().chat_preferences(*actor);if(!old||!old.value)return false;
                    auto next=*old.value;next.ghost=!next.ghost;return database().save_chat_preferences(*actor,next).succeeded()&&chat_refresh();
                };
                return app::run_caller_utility(action,utilities);
            }
            if(action==app::MenuAction::chat_dnd||action==app::MenuAction::chat_notices)return chat_toggle(action);
            if(action==app::MenuAction::online){
                chat.safe(chat_identity,true);const auto ok=chat_session.online();chat.safe(chat_identity,false);
                return ok?std::optional<std::string>{""}:std::nullopt;
            }
            if(action==app::MenuAction::page_sysop){
                const auto comment=chat_session.page_sysop();chat.safe(chat_identity,false);if(!comment)return {};
                if(*comment){message_terminal.menu_boundary();return messages.action(app::MenuAction::comment_sysop);}
                return std::string{};
            }
            if(action==app::MenuAction::password){
                struct Erase{std::string& value;~Erase(){app::explicitly_erase_terminal_secret(value);}};
                const auto cancelled=[](const auto& input)->std::optional<std::string>{
                    return input.outcome==app::TerminalOutcome::cancelled?std::optional<std::string>{"Password change cancelled."}:std::nullopt;
                };
                auto old=app::read_password_change_current(terminal,app::kPasswordMaximum);Erase erase_old{old.value};
                if(!old.has_value())return cancelled(old);
                auto replacement=terminal.read_secret({"menu.password.new","New password: ",app::kPasswordMaximum});Erase erase_new{replacement.value};
                if(!replacement.has_value())return cancelled(replacement);
                auto confirmation=terminal.read_secret({"menu.password.confirm","Confirm new password: ",app::kPasswordMaximum});Erase erase_confirmation{confirmation.value};
                if(!confirmation.has_value())return cancelled(confirmation);
                if(replacement.value!=confirmation.value)return "Passwords did not match. Nothing changed.";
                if(!caller.tick()||!caller.refresh_access())return {};
                const auto account=database().find_account_by_id(ready.account_id);
                if(!account||!account.value||!account.value->display_name)return {};
                const auto changed=store.authentication()->change_account_password({ready.account_id,account.value->revision,old.value,replacement.value,
                    {config.password_mode,*account.value->display_name,config.bbs_name},ready.durable_session_id,
                    {store.identifiers().generate(),ready.correlation_id,ready.account_id,clock.unix_seconds()}},assignment->cancellation);
                if(changed.credential_committed&&changed.committed_credential)caller.login_secret.capture(ready.account_id,*changed.committed_credential,replacement.value);
                else if(changed.credential_committed)caller.login_secret.clear();
                return changed.succeeded()?"Password changed.":"Password not changed. Check your current password and password rules.";
            }
            return "This feature is not available in this Linux owner-test build.";
        };
        ports.login_message_scan=[&]{return messages.login_scan();};
        ports.file_action=[&](app::MenuAction action,app::MenuId menu)->std::optional<app::FileSessionEffect>{if(!file_live()||!write_all(guarded,"\r\n"))return {};return files.action(action,menu);};
        ports.file_navigation=[&](app::MenuId from,app::MenuId to){return files.leave(from,to);};
        ports.login_file_scan=[&]{return files.login_scan();};
        ports.chat=[&](app::MenuAction action){chat.safe(chat_identity,false);return chat_session.menu_action(action);};
        ports.previous_login_utc=ready.previous_login_utc;
        ports.door_bulletin_updates=[&](std::int64_t previous)->std::optional<std::vector<app::DoorBulletinNotice>>{const auto actor=message_actor();if(!actor)return {};const auto result=database().door_bulletin_changes(*actor,previous);return result&&result.value?result.value:std::nullopt;};
        ports.notifications=[&]{return notifications;};
        ports.utc_now=[&]{return clock.unix_seconds();};
        ports.menu_entered=[&](app::MenuId menu){assignment->menu_entered(menu);(void)chat_refresh();if(menu==app::MenuId::main)durable->activity(ready.durable_session_id,{app::CallerActivityKind::main_menu,clock.unix_seconds(),{}},{});};
        app::DoorSessionPorts doors{database(),ports,{},{},{}};
        const auto bulletin_sources=host::door_bulletin_sources(root,store.file_bindings(),[weak=std::weak_ptr<AlphaOperationalJournal>(journal)](app::DoorBulletinId id,std::string_view diagnostic){if(const auto log=weak.lock())log->problem(app::AttentionKind::door_bulletin_unavailable,"Door Bulletin "+std::to_string(id.value),std::string(diagnostic));});
        doors.bulletin_refresh=[&](app::DoorBulletinId id,app::TrustedDisplaySelectionRequest request,std::int64_t revision){const auto actor=message_actor();if(!actor)return app::DoorBulletinRefreshResult{{app::StorageErrorCode::permission_denied,"Caller session ended"},{},{},{}};return app::refresh_door_bulletin(database(),{false,*actor},id,std::move(request),bulletin_sources,revision);};
        doors.actor=message_actor;
        app::DoorAutomaticEvent active_door_event{app::DoorAutomaticEvent::none};
        std::optional<app::MenuDoorSelection> active_menu_door;
        const auto authorize_door=[&](const app::MessageActor& actor,const app::DoorLaunch& selected){
            if(active_menu_door)return database().menu_door_launch(actor,*active_menu_door);
            return active_door_event==app::DoorAutomaticEvent::none?
                database().door_launch(actor,{selected.menu.id,selected.entry.number,selected.catalog_revision}):
                database().door_event_launch(actor,{selected.door.id,active_door_event,selected.catalog_revision});
        };
        const auto record_door=[&](app::DoorId id,std::string name,std::string diagnostic,app::DoorEnd end){
            const auto node=assignment->snapshot();const auto identity=assignment->identity();app::OperationalContext context;
            context.epoch=epoch;context.node=identity.node;context.generation=identity.generation;context.attempt=identity.generation;
            context.transport=ssh?app::OperationalTransport::ssh:app::OperationalTransport::telnet;context.source_ip=node.source_ip;
            context.account_id=ready.account_id;context.caller=own_name();context.durable_session_id=ready.durable_session_id;context.correlation_id=ready.correlation_id;
            context.resource_id=id;if(id)context.resource_name=std::move(name);context.diagnostic_code=std::move(diagnostic);
            context.columns=node.terminal_columns;context.rows=node.terminal_rows;
            journal->emit({app::OperationalKind::stage_changed,end==app::DoorEnd::returned?app::OperationalOutcome::normal:app::OperationalOutcome::cancelled,context});
        };
        doors.launch=[&](const app::DoorLaunch& selected)->std::optional<app::DoorEnd>{
            chat.safe(chat_identity,false);
            if(!caller.tick())return {};
            const auto actor=message_actor();if(!actor)return {};
            const auto authorized=authorize_door(*actor,selected);if(!authorized||!authorized.value)return app::DoorEnd::authority_lost;
            const auto launch=*authorized.value;
#include "linux_caller_remote_door.inc"
            const auto account=database().find_account_by_id(ready.account_id);
            if(!account||!account.value||!account.value->security_level||ready.account_id<1||static_cast<std::uint64_t>(ready.account_id)>UINT32_MAX)return {};
            auto reservation=reservations.acquire(launch.door.id,launch.door.allow_multiple);if(!reservation)return app::DoorEnd::busy;
            bool clean=true;
            struct ReservationRestore{app::DoorReservations::Lease& lease;bool& clean;~ReservationRestore(){if(clean)lease.release_after_cleanup();}}reservation_restore{*reservation,clean};
            const auto sequence=assignment->begin_door(launch.door.id,launch.door.name,app::DoorExecutionKind::local);if(!sequence)return {};
            auto* telnet=ssh?nullptr:&static_cast<AlphaTelnetStream&>(stream);
            if(telnet&&!telnet->begin_gateway()){assignment->end_door(sequence);return {};}
            struct Restore{AlphaTelnetStream* stream;std::shared_ptr<app::CallerAssignment> state;std::uint64_t sequence;
                ~Restore(){if(stream&&!stream->end_gateway())state->cleanup_health->failed=true;state->end_door(sequence);}}restore{telnet,assignment,sequence};
            app::DoorChannel channel;channel.read=[&](auto maximum){return ssh?static_cast<AlphaSshStream&>(stream).read_ready(maximum):telnet->read_ready(maximum);};
            channel.write=[&](auto bytes){return ssh?static_cast<AlphaSshStream&>(stream).write_ready(bytes):telnet->write_ready(bytes);};
            channel.keyboard=[&]{caller.keyboard();};
            channel.stop=[&]() -> std::optional<app::DoorEnd>{if(assignment->stopping)return app::DoorEnd::shutdown;if(assignment->door_termination_requested(sequence))return app::DoorEnd::operator_terminated;
                return caller.tick()?std::nullopt:std::optional<app::DoorEnd>{caller.timed_out?app::DoorEnd::calling_time:app::DoorEnd::transport_failed};};
            LocalDoorCaller user;user.account=static_cast<std::uint32_t>(ready.account_id);user.alias=account.value->display_name.value_or("Caller");user.node=ready.node_id;user.security_level=*account.value->security_level;
            const auto current_screen=ports.snapshot();if(!current_screen)return {};
            user.board_name=config.bbs_name;user.ansi=current_screen->graphics==app::GraphicsMode::ansi;
            if(const auto remaining=effective_seconds())user.remaining_seconds=static_cast<std::uint64_t>(std::max<std::int64_t>(0,*remaining));
            user.captured_utc_seconds=caller.calling_snapshot_utc?caller.calling_snapshot_utc:clock.unix_seconds();
            user.generation=assignment->identity().generation;user.sequence=sequence;
            auto& drop=user.drop_context;drop.nodes=config.logical_nodes;drop.sysop=sysop_name;drop.sysop_access=caller.live_sysop;
            const auto now_millis=clock.monotonic_millis();if(now_millis<caller.authenticated_millis)return app::DoorEnd::launch_failed;
            drop.elapsed_seconds=static_cast<std::uint32_t>(std::min<std::uint64_t>((now_millis-caller.authenticated_millis)/1000,UINT32_MAX));
            drop.user_key="sfx-account-"+std::to_string(user.account)+"-"+std::to_string(account.value->created_utc_seconds);
            const auto label=local->format_utc_seconds(user.captured_utc_seconds),logon=local->format_utc_seconds(user.captured_utc_seconds-drop.elapsed_seconds);
            if(!label||!logon||label->size()<19||logon->size()<19)return app::DoorEnd::launch_failed;
            const auto date=[](const std::string& iso){return iso.substr(5,2)+"/"+iso.substr(8,2)+"/"+iso.substr(2,2);};
            const auto seconds=[](const std::string& iso){return static_cast<std::uint32_t>(std::stoi(iso.substr(11,2))*3600+std::stoi(iso.substr(14,2))*60+std::stoi(iso.substr(17,2)));};
            drop.date=date(*label);drop.clock=label->substr(11,5);drop.logon_date=date(*logon);drop.logon_clock=logon->substr(11,5);
            drop.seconds_today=seconds(*label);drop.logon_seconds_today=seconds(*logon);
            const auto node=assignment->snapshot();drop.columns=node.terminal_columns;drop.rows=node.terminal_rows;
            const auto qwk=database().qwk_policy({true,{}});if(!qwk||!qwk.value)return app::DoorEnd::launch_failed;drop.board_code=qwk.value->board_id;
            if(launch.door.program.drop_file==app::DoorDropFile::doordrop_ini){
                const auto key=database().door_user_key(ready.account_id);const auto projected=local->project_utc_seconds(user.captured_utc_seconds);
                if(!key||!key.value||!projected)return app::DoorEnd::launch_failed;
                drop.user_key=*key.value;drop.software_version=SFX_PRODUCT_VERSION_SHORT_STRING;
                const auto magnitude=std::abs(projected->offset_minutes);char offset[16]{};
                std::snprintf(offset,sizeof(offset),"%c%02d:%02d",projected->offset_minutes<0?'-':'+',magnitude/60,magnitude%60);
                drop.file_time=*label;drop.file_time[10]='T';drop.file_time+=offset;
            }
            record_door(launch.door.id,launch.door.name,"door_entered",app::DoorEnd::returned);
            durable->activity(ready.durable_session_id,{app::CallerActivityKind::door_entered,clock.unix_seconds(),launch.door.name},{});
            auto prefix=terminal.take_binary_input_prefix();clean=false;
            const auto result=run_alpha_local_door(root,std::to_wstring(geteuid()),launch.door,user,channel,std::move(prefix.bytes),prefix.discard_delimiter_lf);
            clean=result.cleanup_complete;if(!clean)assignment->cleanup_health->failed=true;
            if(result.work_preserved||!result.cleanup_complete||result.cleanup_command_failed)journal->problem(app::AttentionKind::work_preserved,"Door "+std::to_string(launch.door.id)+" / Node "+std::to_string(user.node)+(result.retained_location.empty()?" / Node or startup files":" / "+result.retained_location),"door_work_incomplete_preserved");
            if(result.diagnostic.rfind("door_dos_",0)==0&&result.diagnostic.size()<=96&&
               std::all_of(result.diagnostic.begin(),result.diagnostic.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}))
                record_door(launch.door.id,launch.door.name,result.diagnostic,app::DoorEnd::unavailable);
            if(!clean){record_door(launch.door.id,launch.door.name,"door_cleanup_failed",app::DoorEnd::launch_failed);return {};}
            record_door(launch.door.id,launch.door.name,std::string("door_")+app::door_result_code(result.end),result.end);
            if(result.cleanup_command_failed)record_door(launch.door.id,launch.door.name,"door_cleanup_command_failed",app::DoorEnd::launch_failed);
            durable->activity(ready.durable_session_id,{app::CallerActivityKind::door_returned,clock.unix_seconds(),launch.door.name},{});
            if(result.end==app::DoorEnd::game_disconnect)assignment->stopping=true;
            return result.end;
        };
        ports.doors=[&]{return app::run_door_session(doors);};
        primary_door_commands=[&](app::MenuId menu)->std::optional<app::MenuDoorCommands>{
            const auto actor=message_actor();if(!actor)return {};
            const auto result=database().menu_door_commands(*actor,menu);return result&&result.value?result.value:std::nullopt;
        };
        primary_door_launch=[&](const app::MenuDoorSelection& selection)->std::optional<std::string>{
            const auto actor=message_actor();if(!actor)return {};
            const auto authorized=database().menu_door_launch(*actor,selection);
            if(!authorized||!authorized.value)return "The Door or its access changed. Please choose again.";
            active_menu_door=selection;
            struct Restore{std::optional<app::MenuDoorSelection>& selection;~Restore(){selection.reset();}}restore{active_menu_door};
            const auto screen=ports.snapshot();if(!screen)return {};
            app::TrustedDisplaySelectionRequest pre;pre.identity=app::TrustedDisplayIdentity::door_prelaunch;pre.graphics=screen->graphics;
            pre.authenticated_security_level=screen->access.security_level;pre.context=screen->context;pre.door_asset_number=authorized.value->door.id;
            pre.context.door_name=authorized.value->door.name;pre.context.door_number=std::to_string(authorized.value->door.id);pre.context.door_menu=authorized.value->menu.name;
            const auto artwork=ports.artwork(pre);if(artwork.available()){
                auto program=artwork.program->terminal_program;
                for(auto& op:program.operations)if(op.kind==app::TerminalDisplayOperationKind::paging_newline)op.kind=app::TerminalDisplayOperationKind::logical_newline;
                if(!ports.display(program))return {};
            }
            const auto result=doors.launch(*authorized.value);if(!result)return {};
            switch(*result){case app::DoorEnd::caller_disconnected:case app::DoorEnd::game_disconnect:case app::DoorEnd::shutdown:case app::DoorEnd::authority_lost:case app::DoorEnd::calling_time:case app::DoorEnd::inactivity:return {};default:break;}
            if(*result==app::DoorEnd::busy)return "This Door is busy. Try again later.";
            if(*result==app::DoorEnd::launch_failed||*result==app::DoorEnd::unavailable)return "The Door could not be started.";
            return "Returned from "+authorized.value->door.name+".";
        };
        ports.door_commands=primary_door_commands;ports.door_command=primary_door_launch;
        doors.event_outcome=[&](app::DoorId id,app::DoorAutomaticEvent event,app::DoorEnd end){
            record_door(id,"Door "+std::to_string(id),std::string(event==app::DoorAutomaticEvent::logon?"door_logon_":"door_logoff_")+app::door_result_code(end),end);
        };
        ports.door_event=[&](app::DoorAutomaticEvent event){
            if(!caller.tick())return false;
            active_door_event=event;
            struct Restore{app::DoorAutomaticEvent& event;~Restore(){event=app::DoorAutomaticEvent::none;}}restore{active_door_event};
            return app::run_door_events(doors,event);
        };
        return app::run_menu_session(ports,app::MenuId::main,true);
    }
};
LinuxCallerEngine::LinuxCallerEngine(std::filesystem::path root,LinuxRuntimeStore& store,app::IClock& clock,app::Identifier128 epoch,
    std::shared_ptr<AlphaOperationalJournal> journal,std::shared_ptr<AlphaDurableJournal> durable,std::array<std::uint64_t,app::kAlphaNodePoolCapacity> generations,app::NodeEnablement enabled):
    impl_(std::make_unique<Impl>(std::move(root),store,clock,epoch,std::move(journal),std::move(durable),generations,enabled)){}
LinuxCallerEngine::~LinuxCallerEngine(){(void)impl_->stop();}
void LinuxCallerEngine::attach_sysop_ftn(std::shared_ptr<LinuxFtnEngine> value){std::atomic_store(&impl_->ftn_engine,std::move(value));}
app::AlphaControlResponse LinuxCallerEngine::start(){return impl_->start();}
void LinuxCallerEngine::set_external_event_gate(bool held, std::int64_t cutoff_utc) {
    impl_->event_cutoff_utc.store(cutoff_utc);
    if (impl_->listener) impl_->listener->set_external_event_hold(held);
    else impl_->initial_event_hold = held;
}
void LinuxCallerEngine::end_calls_for_external_event() {
    if (impl_->listener) {
        for (const auto& slot : impl_->listener->status().pool.slots)
            if (slot.occupied) (void)impl_->listener->disconnect(slot.identity);
    }
}
bool LinuxCallerEngine::stop()noexcept{return impl_->stop();}
bool LinuxCallerEngine::set_draining(bool value){return impl_->listener&&impl_->listener->set_draining(value);}
bool LinuxCallerEngine::revoke(const app::SessionRevocationCommand& value){return !impl_->registry||impl_->registry->revoke(value);}
app::Phase3aRateGateResult LinuxCallerEngine::web_authentication_admission(const app::Phase3aRateScopeSet& scopes,std::int64_t now){
    return impl_->rates?impl_->rates->try_admission(scopes,now):app::Phase3aRateGateResult{};
}
app::StorageStatus LinuxCallerEngine::web_authentication_result(const app::Phase3aRateScopeSet& scopes,std::int64_t now,bool success){
    if(!impl_->rates)return {app::StorageErrorCode::internal,"caller_rate_control_unavailable"};
    return success?impl_->rates->record_success(scopes,now):impl_->rates->record_failure(scopes,now);
}
app::StorageStatus LinuxCallerEngine::clear_throttle(const app::LocalThrottleClearRequest& request,const app::AuthenticationAuditContext& audit){
    return impl_->rates?impl_->rates->clear_locally(request,audit):app::StorageStatus{app::StorageErrorCode::internal,"caller_rate_control_unavailable"};
}
bool LinuxCallerEngine::disconnect(app::NodeAssignmentIdentity identity){return impl_->listener&&impl_->listener->disconnect(identity);}
bool LinuxCallerEngine::set_enabled(std::uint16_t node,bool enabled){return impl_->listener&&impl_->listener->set_enabled(node,enabled);}
bool LinuxCallerEngine::set_enabled(std::uint16_t node,bool enabled,std::optional<app::NodeAssignmentIdentity> observed,const std::function<void()>& publish){return impl_->listener&&impl_->listener->set_enabled(node,enabled,observed,publish);}
bool LinuxCallerEngine::terminate_door(app::NodeAssignmentIdentity identity,std::uint64_t launch){return impl_->listener&&impl_->listener->terminate_door(identity,launch);}
app::AlphaLocalSessionStartResult LinuxCallerEngine::reserve_local_node(std::uint16_t node){
    if(!impl_->listener)return {{Code::invalid_request,"bbs_not_ready"},{},0,{},0};
    return impl_->listener->reserve_local_node(node);
}
bool LinuxCallerEngine::cancel_local_node(app::NodeAssignmentIdentity identity){return impl_->listener&&impl_->listener->cancel_local_node(identity);}
LinuxBrowserNodeStart LinuxCallerEngine::open_browser_node(std::uint16_t node){
    if(!impl_->listener)return {{Code::invalid_request,"bbs_not_ready"},{},host::FileDescriptor{}};
    return impl_->listener->open_browser_node(node);
}
bool LinuxCallerEngine::account_online(app::AccountId account)const{return impl_->registry&&impl_->registry->account_online(account);}
bool LinuxCallerEngine::update_botgate_feed(){return impl_->feed&&impl_->feed->update_now();}
app::BotGateFeedStatus LinuxCallerEngine::botgate_status()const{
    auto value=impl_->feed?impl_->feed->status():app::BotGateFeedStatus{};
    if(!impl_->rules)return value;
    const auto rules=impl_->rules->snapshot();value.rules_ready=rules->ready;value.rule_status=impl_->rules->status();
    if(value.rule_status.size()>128||std::any_of(value.rule_status.begin(),value.rule_status.end(),[](unsigned char c){return c<32||c>126;}))value.rule_status="rules_reload_failed";
    for(std::size_t i=0;i<rules->lists.size();++i)value.rule_counts[i]=rules->lists[i].size();
    for(const auto& geo:rules->geo){if(!value.geo_summary.empty())value.geo_summary+=", ";value.geo_summary+=geo.name;}
    if(value.geo_summary.size()>4096)value.geo_summary.resize(4096);
    return value;
}
bool LinuxCallerEngine::refresh_account_access(app::AccountId account){
    return impl_->refresh_account_access(account);
}
bool LinuxCallerEngine::refresh_security_policy(const app::OperationalConfiguration& candidate){
    const auto snapshot=make_caller_configuration(impl_->database(),candidate,impl_->sysop_name,SFX_PRODUCT_RUNTIME_VERSION);if(!snapshot)return false;
    std::atomic_store(&impl_->caller_config,snapshot);impl_->menu_configuration_revision=candidate.configuration_revision;
    if(impl_->registry)for(const auto account:impl_->registry->published_accounts())if(!refresh_account_access(account))return false;
    return true;
}
bool LinuxCallerEngine::reconfigure(const app::OperationalConfiguration& candidate,const std::function<bool()>& publish){
    try{
        if(!impl_->listener||impl_->retired||!publish)return false;
        const auto current=std::atomic_load(&impl_->live_settings);
        auto backend=current->ssh;
        if(candidate.ssh.enabled&&!backend){std::string diagnostic;backend=AlphaSshBackend::acquire(impl_->root,std::to_wstring(geteuid()),diagnostic);if(!backend)return false;}
        if(candidate.botgate.enabled&&!impl_->rules->snapshot()->ready)return false;
        const auto policy=make_caller_configuration(impl_->database(),candidate,impl_->sysop_name,SFX_PRODUCT_RUNTIME_VERSION);if(!policy)return false;
        auto prepared=std::make_shared<const Impl::LiveSettings>(candidate,impl_->clock,std::move(backend));
        auto hostname=std::atomic_load(&impl_->hostname_lookup);
        if(candidate.hostname_lookup!=current->configuration.hostname_lookup)hostname=std::make_shared<LinuxHostnameLookup>(impl_->root,candidate.hostname_lookup);
        return impl_->listener->reconfigure_endpoints(candidate,[&]{
            if(!publish())return false;
            std::atomic_store(&impl_->live_settings,std::move(prepared));std::atomic_store(&impl_->caller_config,policy);
            std::atomic_store(&impl_->hostname_lookup,std::move(hostname));
            impl_->menu_configuration_revision=candidate.configuration_revision;return true;
        });
    }catch(const std::exception&){return false;}
}
LinuxCallerListenerStatus LinuxCallerEngine::status() const {
    if (impl_->listener) return impl_->listener->status();
    LinuxCallerListenerStatus result;
    result.diagnostic = impl_->diagnostic;
    return result;
}
app::ChatConfigurationResult LinuxCallerEngine::change_chat_configuration(const app::ChatConfiguration& value){
    if(impl_->retired)return {};
    const auto result=impl_->chat.change_configuration(value,[&]{return impl_->database().change_chat_configuration({true,{}},value).succeeded();});
    return {result,impl_->chat.configuration()};
}
app::ChatOperatorSnapshot LinuxCallerEngine::chat_operator(const app::ChatOperatorRequest& value){
    if(impl_->retired||app::encode_chat_operator_request(value).empty())return {};
    const auto now=impl_->clock.monotonic_millis();auto& hub=impl_->chat;hub.tick(now);
    using Action=app::ChatOperatorAction;
    switch(value.action){
        case Action::status:return hub.operator_status();
        case Action::availability:hub.operator_available(value.available);return hub.operator_status();
        case Action::poll:return hub.operator_poll(value.token,value.view,value.sequence,now);
        case Action::end:hub.operator_end(value.token,value.view,now);return hub.operator_poll(value.token,value.view,0,now);
        case Action::send:{app::ChatOperatorSnapshot result;result.code=hub.operator_send(value.token,value.view,value.sequence,value.text,now);return result;}
        case Action::start:case Action::answer:break;
    }
    const auto target=value.target&&impl_->registry?impl_->registry->chat_identity(*value.target):std::nullopt;
    return target?hub.operator_start(*target,value.view,value.page,value.override_dnd,now):app::ChatOperatorSnapshot{};
}
app::ChatResult LinuxCallerEngine::send_chat_notice(const app::ChatNoticeRequest& value){
    if(impl_->retired||app::encode_chat_notice(value).empty())return app::ChatResult::invalid;
    std::optional<app::ChatIdentity> target;
    if(value.target){target=impl_->registry?impl_->registry->chat_identity(*value.target):std::nullopt;if(!target)return app::ChatResult::unavailable;}
    if(value.room){const auto configuration=impl_->chat.configuration();
        if(std::none_of(configuration.rooms.begin(),configuration.rooms.end(),[&](const auto& room){return room.id==value.room&&room.enabled;}))return app::ChatResult::unavailable;}
    return impl_->chat.notice(target,value.room,value.text,value.override_dnd);
}
} // namespace sfx::apps::runtime
