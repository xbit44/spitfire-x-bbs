// Genuine shipping Runtime, fixed new normal-Setup fixture, ordinary owner IPC.
#include "ftn_current_installed_client.inc"
#include "network_setup_fixture.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/application/ftn_wire.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include <fstream>
int main(int argc,char** argv)try{
    check(argc==3,"new output directory and closed bootstrap/poll action required");
    const std::string action=argv[2];check(action=="--bootstrap-predecessor"||action=="--verify-upgrade"||action=="--poll","closed action");
    const auto out=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(out),"retain every earlier fixture");std::filesystem::create_directories(out);
    const auto root=ins::canonical_root(network_setup_qualification_root());const auto owner=ins::operator_sid();check(!ins::elevated(),"ordinary owner only");ins::verify_runtime_installation(root,owner);
    if(action=="--bootstrap-predecessor"){
        RuntimeSession runtime(root,true);app::AlphaBootstrapRequest request;
        request.configuration.bbs_name="Network Setup Qualification";request.configuration.bbs_time_zone="Pacific Standard Time";request.configuration.logical_nodes=4;
        request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=26431;request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
        request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=26432;
        request.sysop_name="Setup Fixture";request.sysop_password="Synthetic-owner-only-123";request.system_password="Synthetic-system-only-456";
        const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& q)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(q));});check(status&&runtime.tracker.accept(*status),"actual prerequisite security policy snapshot");
        request.configuration.security_policy_revision=status->security_policy_revision;request.configuration.security_policy_hash=status->security_policy_hash;
        const auto bootstrap_bytes=app::encode_bootstrap_request(request);check(!bootstrap_bytes.empty(),"valid nonempty first-run request");
        const auto response=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::bootstrap_request,app::LocalControlMessageType::bootstrap_response,bootstrap_bytes));app::erase_bootstrap_secrets(request);
        if(!response||response->code!=app::LocalControlResponseCode::ok)std::cerr<<"Bootstrap diagnostic: "<<(response?response->diagnostic_code:"invalid_response")<<'\n';
        check(response&&response->code==app::LocalControlResponseCode::ok,"normal shipping first-run bootstrap");
        const auto request_ftn=[&](const app::FtnConfigurationRequest& q){return app::decode_ftn_configuration_response(runtime.exchange(app::LocalControlMessageType::ftn_configuration_request,app::LocalControlMessageType::ftn_configuration_response,app::encode_ftn_configuration_request(q))).value_or(app::FtnConfigurationResponse{});};
        app::FtnConfigurationRequest read_request;read_request.token=token();std::vector<std::uint8_t> raw;
        for(;;){const auto page=request_ftn(read_request);check(page.code==app::FtnConfigurationCode::ok&&page.offset==raw.size(),"actual predecessor configuration read");raw.insert(raw.end(),page.bytes.begin(),page.bytes.end());if(raw.size()==page.total)break;read_request.offset=static_cast<std::uint32_t>(raw.size());read_request.revision=page.revision;}
        read_request.operation=app::FtnConfigurationOperation::cancel;read_request.offset=0;check(request_ftn(read_request).code==app::FtnConfigurationCode::ok,"release predecessor read ownership");
        const auto prior=app::decode_ftn_configuration(raw);check(prior&&raw.front()==6&&prior->networks.empty(),"exact RC12 codec6 predecessor");auto next=*prior;next.settings.listener_enabled=false;next.settings.tls_listener_enabled=false;next.settings.bind_address="127.0.0.1";
        app::FtnNetwork n;n.code="legacy-fixture";n.name="Legacy setup network";n.address="64202:100/49";n.message_group=1;n.enabled=false;next.networks.push_back(n);
        app::FtnLink l;l.network=n.code;l.code="legacy-peer";l.name="Legacy disabled peer";l.address="64202:100/1";l.host="127.0.0.1";l.port=9;l.poll_minutes=0;l.enabled=false;const auto secret=win::protect_ftn_secret(root.u8string(),n.code,l.code,win::FtnCredentialKind::session,"legacy-synthetic-key");check(bool(secret),"predecessor installation-bound synthetic credential");l.credentials.session=*secret;next.links.push_back(l);
        app::FtnMessageEcho echo;echo.network=n.code;echo.tag="LEGACY.ECHO";echo.local_name="Legacy Echo";next.message_echoes.push_back(echo);
        const auto encoded=app::encode_ftn_configuration_legacy(next,6);check(!encoded.empty(),"exact legacy6 write, no current storage open");app::FtnConfigurationRequest write;write.operation=app::FtnConfigurationOperation::begin;write.token=token();write.revision=next.revision;write.total=static_cast<std::uint32_t>(encoded.size());check(request_ftn(write).code==app::FtnConfigurationCode::ok,"predecessor write begin");write.operation=app::FtnConfigurationOperation::append;write.bytes=encoded;check(request_ftn(write).code==app::FtnConfigurationCode::ok,"predecessor staged bytes");write.operation=app::FtnConfigurationOperation::commit;write.bytes.clear();check(request_ftn(write).code==app::FtnConfigurationCode::ok,"predecessor saves populated settings and legacy echo");runtime.stop();
    }else if(action=="--verify-upgrade"){
        RuntimeSession runtime(root);runtime.stop();const auto c=win::offline_ftn_configuration(root,owner);check(c&&c.value&&c.value->networks.size()==1&&c.value->links.size()==1&&c.value->message_echoes.size()==1,"populated predecessor identities survive actual shipping upgrade");
        check(c.value->networks[0].name=="Legacy setup network"&&!c.value->networks[0].msg_netmail_enabled.has_value()&&!c.value->links[0].credentials.session.empty(),"legacy MSG inheritance and protected credential retained");check(c.value->message_echoes[0].local_echo>0&&c.value->message_echoes[0].maximum_messages==1000U&&!c.value->message_echoes[0].ansi_colors.has_value(),"existing retention and legacy ANSI presentation stay intact");
    }else{
        const auto c=win::offline_ftn_configuration(root,owner);check(c&&c.value,"saved shipping setup readable");
        const auto n=std::find_if(c.value->networks.begin(),c.value->networks.end(),[](const auto& v){return v.name=="Setup online test";});check(n!=c.value->networks.end(),"exact online test network");
        const auto l=std::find_if(c.value->links.begin(),c.value->links.end(),[&](const auto& v){return v.network==n->code&&v.code==n->default_link;});check(l!=c.value->links.end()&&l->host=="127.0.0.1"&&!l->contact_on_mail&&!l->poll_minutes,"only reviewed loopback peer");
        RuntimeSession runtime(root);const auto call=[&](app::FtnRuntimeRequest q){return app::decode_ftn_runtime_response(runtime.exchange(app::LocalControlMessageType::ftn_runtime_request,app::LocalControlMessageType::ftn_runtime_response,app::encode_ftn_runtime_request(q))).value_or(app::FtnRuntimeResponse{});};
        check(call({app::FtnRuntimeAction::scan}).code==app::FtnConfigurationCode::ok,"shipping outbound Scan accepted");
        const auto until=GetTickCount64()+30000;bool ready{};while(GetTickCount64()<until){auto status=call({});const auto queues=call({app::FtnRuntimeAction::queues});if(status.active_revision==c.value->revision&&!status.processing&&queues.queues&&queues.queues->outbound[0]>0){ready=true;break;}Sleep(20);}check(ready,"shipping worker publishes the queued control packet before Poll");
        check(call({app::FtnRuntimeAction::poll,n->code,l->code}).code==app::FtnConfigurationCode::ok,"explicit shipping Poll accepted");
        bool delivered{};const auto deadline=GetTickCount64()+45000;while(GetTickCount64()<deadline){const auto status=call({});if(std::any_of(status.sessions.begin(),status.sessions.end(),[](const auto& s){return s.completed&&s.sent_files>0;})){delivered=true;break;}Sleep(25);}check(delivered,"actual shipping session receives peer GOT for queued AreaFix packet");runtime.stop();
    }
    std::cout<<"PASS "<<checks<<" fixed new installed bootstrap/poll checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
