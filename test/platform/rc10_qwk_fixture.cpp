// NG34 fixed-root synthetic fixture preparation. Never discovers real boards.
#define main retained_qwk_storage_fixture_main
#include "../integration/qwk_storage_tests.cpp"
#undef main
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/application/ftn_work.hpp"
#include <fstream>
namespace win=sfx::platform::win32;
namespace ins=win::installation;
int main(int argc,char** argv)try{
    check(argc==3,"closed fixture action and new output required");const std::string action=argv[1];
    const auto root=ins::canonical_root(L"C:/Program Files/SFX-RC10-Test-20261110-r2");const auto owner=ins::operator_sid();
    const auto out=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(out),"preserve previous evidence");
    if(action=="retire"){check(ins::elevated(),"retire needs normal administrator approval");const auto receipt=ins::read_receipt(root,owner);check(receipt.committed,"committed exact fixture receipt");ins::verify_structure(receipt);}
    else ins::verify_runtime_installation(root,owner);
    const auto registration=ins::read_service_registration(root);check(registration&&!registration->enabled,"only isolated Desktop fixture");
    const auto state=ins::query_owned_service(*registration);check(state.state==SERVICE_STOPPED&&state.start_type==SERVICE_DISABLED,"stopped Disabled isolated service");
    if(action=="retire"){
        win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive stopped fixture retirement guard");
        auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);check(manager!=nullptr,"exact fixture SCM connection");
        struct Close {SC_HANDLE value;~Close(){if(value)CloseServiceHandle(value);}} close_manager{manager};
        auto service=OpenServiceW(manager,registration->name.c_str(),DELETE|SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS);check(service!=nullptr,"delete authority only for validated new stopped fixture service");Close close_service{service};
        check(ins::query_owned_service(*registration).state==SERVICE_STOPPED&&DeleteService(service)!=FALSE,"retire only exact new stopped registration; no uninstall journal or file changes");
        std::ofstream(out)<<"PASS retired only NG34 fixture registration; files retained\n";return 0;
    }
    if(action=="reset-first-use"){
        check(!ins::elevated(),"ordinary owner fixture-only preference reset");win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive fixture preference reset guard");const auto file=root/L"Data/SFX.DB";
        check(scalar(file,"SELECT count(*) FROM sfx_accounts")==2&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==2210&&scalar(file,"SELECT count(*) FROM sfx_qwk_caller_preferences WHERE account_id=1")==1,"exact synthetic baseline checkpoint; only one caller preference row");
        internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(1000),false);c.exec("DELETE FROM sfx_qwk_caller_preferences WHERE account_id=1","reset only this disposable caller preference row to exercise first Download");
        std::ofstream(out)<<"{\"result\":\"PASS\",\"deleted_synthetic_preference_rows\":1,\"messages_credentials_and_pointers_changed\":false}\n";std::cout<<"PASS exact fixture-only first-use reset\n";return 0;
    }
    const bool resume=action=="resume-ingress";check((action=="seed"||resume)&&!ins::elevated(),"ordinary owner synthetic seed or exact checkpoint only");win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive stopped fixture guard");
    const auto file=root/L"Data/SFX.DB";check(scalar(file,"SELECT count(*) FROM sfx_accounts")== (resume?2:1)&&scalar(file,"SELECT count(*) FROM sfx_local_messages")== (resume?2209:0),"exact fresh bootstrap or preserved synthetic seed checkpoint");
    if(!resume){
        internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(1000),false);
        c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(2,'NG34 Author','ng34 author',10,'active',1,1,1793970000,1793970000);INSERT INTO sfx_account_statistics(account_id) VALUES(2);INSERT INTO sfx_account_profiles(account_id) VALUES(2);INSERT INTO sfx_account_name_reservations VALUES('ng34 author',2,1,'NG34 Author',1793970000,NULL)","closed synthetic message author, no login credential");
        for(unsigned account=1;account<=2;++account){internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1793970000,0,1793970000,NULL,NULL,?1)","trusted offline seed sessions only");q.bind(1,id(50000+account));q.bind(2,static_cast<std::int64_t>(account));q.step();}
    }
    win::WindowsLocalStoragePathPolicy paths(owner);storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";storage::SqliteStorage db(settings,paths);check(db.start().succeeded(),"installed native writer starts under exclusive guard");
    app::MessageActor author{id(50002),2,1,1},sysop{id(50001),1,scalar(file,"SELECT revision FROM sfx_accounts WHERE account_id=1"),scalar(file,"SELECT revision FROM sfx_security_policy WHERE singleton_id=1")};unsigned operation=resume?55000:51000;
    const auto write_metadata=[&](app::MessageId message){
        check(bool(db.qwk_preferences(sysop)),"ordinary QWK entry initializes caller defaults before planning");
        const auto reference=db.qwk_reference(sysop,message);const auto plan=db.begin_qwk_packet(sysop,id(55999));if(!reference)std::cerr<<"reference: "<<reference.status.operation<<'\n';if(!plan)std::cerr<<"plan: "<<plan.status.operation<<'\n';check(reference&&plan&&plan.value->netmail_conference,"owned canonical original reply metadata");
        std::ofstream report(out);report<<"{\"result\":\"PASS\",\"board\":\""<<plan.value->policy.board_id<<"\",\"netmail_conference\":"<<*plan.value->netmail_conference<<",\"reference\":"<<*reference.value<<",\"public_messages\":2208,\"remote_name\":\"Remote Person Long Full Name\"}\n";check(bool(report),"synthetic fixture manifest");
    };
    if(!resume){for(unsigned n=0;n<2208;++n){const auto context=db.begin_message(author,{app::MessageKind::public_echo,{2},{},{}});check(bool(context),"synthetic public context");
        std::string body;std::uint32_t random=n+1;
        for(unsigned i=0;i<768;++i){random^=random<<13;random^=random>>17;random^=random<<5;body.push_back(static_cast<char>('!'+random%90));if(i%64==63)body.push_back('\n');}
        check(bool(db.commit_message({author,id(++operation),*context.value,"NG34 Public "+std::to_string(n),body})),"synthetic public commit");}
    const auto local=db.begin_message(author,{app::MessageKind::private_local,{},1,{}});check(bool(local)&&bool(db.commit_message({author,id(++operation),*local.value,"NG34 Local Inbox","Synthetic unread private content\n"})),"synthetic private local commit");}
    auto config=*db.ftn_configuration({true,{}}).value;check(resume?(config.networks.size()==1&&config.links.size()==1&&config.links.front().network=="FIDO"&&config.links.front().code=="LOCAL"&&config.links.front().host=="127.0.0.1"&&!config.links.front().contact_on_mail&&config.links.front().poll_minutes==0):(config.networks.empty()&&config.links.empty()),"only exact synthetic network checkpoint");config.settings.mode=app::FtnMailerMode::native;config.settings.listener_enabled=config.settings.tls_listener_enabled=false;
    if(!resume){
    app::FtnNetwork network;network.code="FIDO";network.name="NG34 Synthetic FidoNet";network.address="1:105/49@fidonet";network.enabled=true;config.networks.push_back(network);
    app::FtnLink link;link.network="FIDO";link.code="LOCAL";link.name="NG34 synthetic loopback only";link.address="1:105/44@fidonet";link.host="127.0.0.1";link.enabled=true;link.poll_minutes=0;link.contact_on_mail=false;
    link.credentials.session=*win::protect_ftn_secret(root.u8string(),link.network,link.code,win::FtnCredentialKind::session,"synthetic-only");config.links.push_back(link);}
    const auto saved=db.change_ftn_configuration({true,{}},id(++operation).bytes,config);check(bool(saved),"native custody with no listeners, scheduled/contact-on-mail polling or remote host");config=*db.ftn_configuration({true,{}}).value;
    const auto digest=[](std::string_view bytes){app::FtnDigest d{};check(crypto_hash_sha256(d.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())==0,"synthetic ingress digest");return d;};
    app::FtnImportRequest q;q.operation=id(++operation);q.ingress=id(++operation);q.network="FIDO";q.expected_configuration_revision=config.revision;q.header.origin=*app::parse_ftn_address("1:105/44@fidonet");q.header.destination=*app::parse_ftn_address("1:105/49@fidonet");q.header.year=2026;q.header.month=11;q.header.day=6;
    auto& m=q.message;m.origin_net=m.destination_net=105;m.origin_node=44;m.destination_node=49;m.date="06 Nov 26  09:00:00";m.to="Sysop Fixture";m.from="Remote Person Long Full Name";m.subject="NG34 Incoming NetMail";m.text="\1MSGID: 1:105/44 ng34-installed-1\r\1CHRS: CP437 2\rSynthetic received NetMail\r";
    const auto inspected=app::inspect_ftn_message(q.header,m);check(bool(inspected),"received private metadata");const auto material=app::ftn_message_fingerprint_material(m,*inspected);q.identity=digest(material.identity);q.content=digest(material.content);const auto packet=app::encode_ftn_packet({q.header,{m}});check(bool(packet),"complete synthetic host packet");
    app::FtnIngressReceipt ingress;ingress.id=q.ingress;ingress.session=id(++operation);ingress.identity=digest("NG34 installed custody");ingress.network="FIDO";ingress.link="LOCAL";ingress.proof=app::FtnIngressProof::inbound_cram;ingress.configuration_revision=config.revision;ingress.file={"ng34-installed.pkt",packet->size(),1793970000};
    check(bool(db.begin_ftn_ingress({true,{}},ingress))&&db.advance_ftn_ingress({true,{}},ingress.id,0,packet->size(),digest(*packet),app::FtnIngressState::received,{}).succeeded(),"trusted host fixture receipt, no peer authentication claim");const auto received=db.import_ftn_message({true,{}},q);check(received&&received.value->message,"production private FTN import");
    write_metadata(received.value->message->id);
    check(db.shutdown().succeeded(),"installed synthetic writer stops normally");std::cout<<"PASS NG34 fixture "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL NG34 fixture: "<<e.what()<<'\n';return 1;}
