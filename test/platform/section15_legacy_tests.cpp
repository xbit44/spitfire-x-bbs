#include "ftn_current_installed_client.inc"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include <fstream>
namespace {
ins::Path root;
std::string bindings(){
    const auto owner=ins::operator_sid();const auto ftn=win::offline_ftn_configuration(root,owner);
    const auto doors=win::offline_door_configuration(root,owner);
    check(ftn&&ftn.value&&doors&&doors.value,"legacy catalogs readable offline");
    check(ftn.value->links.size()==1&&doors.value->doors.size()==2,"only synthetic legacy definitions present");
    const auto& link=ftn.value->links.front();const auto& creds=link.credentials;
    std::string result;
    for(const auto& item:{std::pair{win::FtnCredentialKind::session,&creds.session},std::pair{win::FtnCredentialKind::packet,&creds.packet},
        std::pair{win::FtnCredentialKind::areafix,&creds.areafix},std::pair{win::FtnCredentialKind::tic,&creds.tic}}){
        check(item.second->size()>5&&item.second->at(4)==1,"old FTN blob remains exact user-bound v1");
        const auto clear=win::unprotect_ftn_secret(root.u8string(),link.network,link.code,item.first,*item.second);
        check(clear&&clear->view()==(item.first==win::FtnCredentialKind::packet?"oldpass":"fixture-legacy-secret"),"old FTN original still decrypts as owner");result+=ins::sha256(*item.second)+"\n";
    }
    unsigned door_blobs{};
    for(const auto& door:doors.value->doors)for(const auto* blob:{&door.rlogin.protected_credential,&door.rlogin.protected_pepper}){
        if(blob->empty())continue;++door_blobs;
        check(blob->size()>5&&blob->at(4)==1,"old Door blob remains user-bound v1");
        check(win::unprotect_rlogin_secret(*blob).has_value(),"old Door original still readable as owner");result+=ins::sha256(*blob)+"\n";
    }
    check(door_blobs==2,"two valid independent Door credential purposes retained");
    result+=ins::file_sha256(root/L"Config"/L"ssh_host_ed25519.pem")+"\n";
    return result;
}
}
int main(int argc,char** argv)try{
    check(argc==4&&!ins::elevated(),"ordinary closed legacy fixture action");
    const std::string action=argv[1];
    root=ins::canonical_root(ins::Path(argv[2]));
    check(root==ins::canonical_root(L"C:/Program Files/SFX-Existing-Desktop-Test-20261105-r2")||
        (action=="prepare-active"&&root==ins::canonical_root(L"C:/Program Files/SFX-Service-Fresh-Test-20261105-r2")),"only named closed fixture action");
    const auto out=std::filesystem::absolute(argv[3]);
    check(!std::filesystem::exists(out),"preserve prior legacy evidence");std::filesystem::create_directories(out);
    if(action=="prepare-active"){
        const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);
        win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"stopped fixture exclusive configuration guard");
        const auto before=win::read_operational_configuration_file(root);check(before.configuration.has_value(),"actual saved fixture configuration");
        auto changed=*before.configuration;changed.maximum_sessions_per_account=2;++changed.configuration_revision;
        check(win::publish_operational_configuration_file(root,owner,changed).succeeded(),"synthetic two-node session allowance saved through native protected publication");
        const auto reopened=win::read_operational_configuration_file(root);
        check(reopened.configuration&&reopened.configuration->maximum_sessions_per_account==2,"synthetic session allowance reopens exactly");
        std::cout<<"PASS "<<checks<<" active fixture preparation checks\n";return 0;
    }
    check(!ins::read_service_mode(root),"legacy action requires stopped Desktop mode");
    if(action=="bootstrap"||action=="seed"){
        check(!std::filesystem::exists(root/L"Bin"/L"starter-doors.manifest"),"old incomplete upgrade has no starter eligibility");
        if(action=="bootstrap"){
        RuntimeSession runtime(root,true);app::AlphaBootstrapRequest q;
        q.configuration.bbs_name="Section15 Legacy Fixture";q.configuration.bbs_time_zone="Pacific Standard Time";q.configuration.logical_nodes=2;
        q.configuration.telnet.bind_address="127.0.0.1";q.configuration.telnet.port=18717;q.configuration.telnet.bot_gate_enabled=false;
        q.configuration.botgate.enabled=false;q.configuration.hostname_lookup=false;q.configuration.ssh.enabled=true;
        q.configuration.ssh.bind_address="127.0.0.1";q.configuration.ssh.port=18718;
        q.sysop_name="Sysop Fixture";q.sysop_password="Synthetic-owner-only-123";q.system_password="Synthetic-system-only-456";
        const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& s)->std::optional<std::vector<std::uint8_t>>{
            return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(s));});
        check(status.has_value(),"Desktop first-run security snapshot");q.configuration.security_policy_revision=status->security_policy_revision;q.configuration.security_policy_hash=status->security_policy_hash;
        const auto response=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::bootstrap_request,app::LocalControlMessageType::bootstrap_response,app::encode_bootstrap_request(q)));
        app::erase_bootstrap_secrets(q);check(response&&response->code==app::LocalControlResponseCode::ok,"real shipping Desktop first-run completes");runtime.stop();
        }
        const auto owner=ins::operator_sid();auto ftn=win::offline_ftn_configuration(root,owner);auto doors=win::offline_door_configuration(root,owner);
        check(ftn&&ftn.value&&doors&&doors.value&&doors.value->doors.size()<=2,"only bounded synthetic catalog preparation");
        for(const auto& door:doors.value->doors)check(door.id==10||door.id==11,"no existing operator door can be overwritten by test seeding");
        if(ftn.value->links.empty()){
        ftn.value->settings.mode=app::FtnMailerMode::disabled;ftn.value->settings.listener_enabled=ftn.value->settings.tls_listener_enabled=false;
        app::FtnNetwork net;net.code="S15TEST";net.name="Synthetic disabled network";net.address="1:1/1";net.origin_line="Synthetic isolated fixture";
        net.enabled=false;net.new_group_name="Synthetic Test";ftn.value->networks.push_back(net);
        app::FtnLink link;link.code="LOCAL";link.network=net.code;link.name="Synthetic disabled peer";link.address="1:1/2";link.host="127.0.0.1";link.enabled=false;
        link.credentials.session=*win::protect_ftn_secret(root.u8string(),net.code,link.code,win::FtnCredentialKind::session,"fixture-legacy-secret");
        link.credentials.packet=win::protect_ftn_secret(root.u8string(),net.code,link.code,win::FtnCredentialKind::packet,"oldpass").value();
        link.credentials.areafix=*win::protect_ftn_secret(root.u8string(),net.code,link.code,win::FtnCredentialKind::areafix,"fixture-legacy-secret");
        link.credentials.tic=*win::protect_ftn_secret(root.u8string(),net.code,link.code,win::FtnCredentialKind::tic,"fixture-legacy-secret");ftn.value->links.push_back(link);
        check(win::offline_change_ftn_configuration(root,owner,token(),*ftn.value).status.succeeded(),"synthetic old FTN four-purpose credentials saved offline; no network work");
        }else check(ftn.value->links.size()==1&&ftn.value->links.front().network=="S15TEST"&&ftn.value->links.front().code=="LOCAL"&&!ftn.value->links.front().enabled,"retry preserves exact previously seeded FTN originals");
        if(!ftn.value->links.empty()&&ftn.value->links.front().credentials.packet.empty()){
            auto& saved=ftn.value->links.front();saved.credentials.packet=win::protect_ftn_secret(root.u8string(),saved.network,saved.code,win::FtnCredentialKind::packet,"oldpass").value();
            check(win::offline_change_ftn_configuration(root,owner,token(),*ftn.value).status.succeeded(),"test fixture missing packet credential filled within its eight-character contract; other original ciphertexts retained");
        }
        for(const auto id:{app::DoorId{10},app::DoorId{11}}){
            doors=win::offline_door_configuration(root,owner);check(doors&&doors.value,"current synthetic Door revision");
            if(std::any_of(doors.value->doors.begin(),doors.value->doors.end(),[&](const auto& d){return d.id==id;}))continue;
            app::DoorConfigurationChange change;change.operation.bytes=token();change.expected_revision=doors.value->revision;change.create_only=true;
            change.door.id=id;change.door.code=id==10?"S15CRED":"S15PEPPER";change.door.name=id==10?"Synthetic disabled credential fixture":"Synthetic disabled pepper fixture";change.door.enabled=false;
            change.door.rlogin.host="127.0.0.1";
            if(id==10)change.door.rlogin.protected_credential=*win::protect_rlogin_secret("fixture-door-secret");
            else{change.door.rlogin.authentication=app::RloginAuthentication::synchronet_caller_hash;change.door.rlogin.prompt_credential=true;change.door.rlogin.protected_pepper=*win::protect_rlogin_secret("fixture-door-pepper");}
            check(win::offline_change_door_configuration(root,owner,change).code==app::LocalAccountResultCode::ok,"valid synthetic old Door credential purpose saved offline");
        }
    }else if(action=="desktop"){
        RuntimeSession runtime(root);const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& s)->std::optional<std::vector<std::uint8_t>>{
            return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(s));});
        check(status&&status->state==app::AlphaRuntimePresentationState::ready&&status->lifecycle.owner_mode==app::RuntimeOwnershipMode::desktop,"Desktop fallback reaches ready with retained database and SSH identity");runtime.stop();
    }else check(false,"closed legacy action");
    std::ofstream report(out/L"original-bindings.txt");report<<bindings();check(bool(report),"six ciphertext hashes and original SSH hash retained without plaintext");
    std::cout<<"PASS "<<checks<<" legacy credential/Desktop checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
