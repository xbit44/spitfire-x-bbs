// Controlled host ingress and normal FileMan import for installed UI tests.
// Synthetic session provenance, not a socket/peer authentication qualification.
#define main retained_resume_main
#include "ftn_retention_resume_tests.cpp"
#undef main
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/offline_file_administration.hpp"
#include "sfx/platform/win32/ftn_native_toss.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include <sodium.h>
int main(int argc,char** argv)try{
    const bool resume=argc==2&&std::string(argv[1])=="--resume-after-config";
    check(argc==1||resume,"closed installed positive-fixture invocation");RuntimeClient runtime;std::string why;
    const auto root=runtime.root;const auto owner=ins::operator_sid();
    auto config=read(runtime);
    check(std::count_if(config.message_echoes.begin(),config.message_echoes.end(),[](const auto& e){return e.tag=="TEST.RETENTION";})==(resume?1:0),"exact fresh or saved configuration checkpoint");
    check(!std::filesystem::exists(root/L"Files/General/fixture-details.txt"),"preserve any prior detail fixture");
    const auto network=std::find_if(config.networks.begin(),config.networks.end(),[](const auto& n){return n.code=="TEST";});
    const auto link=std::find_if(config.links.begin(),config.links.end(),[](const auto& l){return l.network=="TEST"&&l.code=="DEST";});
    check(network!=config.networks.end()&&link!=config.links.end()&&!config.settings.listener_enabled&&!config.settings.tls_listener_enabled&&link->poll_minutes==0&&link->host=="127.0.0.1","existing isolated test network, no listener or scheduled remote poll");
    if(!resume){network->keep_processed=true;network->retention_days=0;
    link->credentials.packet=*win::protect_ftn_secret(root.u8string(),"TEST","DEST",win::FtnCredentialKind::packet,"packkey");
    app::FtnMessageEcho echo;echo.network="TEST";echo.tag="TEST.RETENTION";echo.description="Controlled installed retention fixture";echo.store_locally=true;config.message_echoes.push_back(echo);
    config.permissions.push_back({"TEST","DEST","TEST.RETENTION",false,true,false,false});
    check(upload(runtime,config,token()).code==app::FtnConfigurationCode::ok,"normal owner IPC saves isolated Forever policy");}
    else check(network->keep_processed&&network->retention_days==0&&!link->credentials.packet.empty(),"saved Forever and protected packet configuration retained");
    config=read(runtime);check(runtime.shutdown(why),"Runtime stopped before controlled host fixture");
    {
        win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive ordinary installed fixture guard");
        const auto receipt=ins::read_receipt(root,owner);ins::verify_ftn_directory(receipt);
        win::WindowsLocalStoragePathPolicy paths(owner);sfx::adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
        sfx::adapters::storage::SqliteStorage storage(settings,paths);check(storage.start().succeeded(),"actual installed native-path writer");
        auto reviewed=config;std::find_if(reviewed.networks.begin(),reviewed.networks.end(),[](const auto& n){return n.code=="TEST";})->retention_days=1;
        const auto prior=storage.preview_ftn_retention({true,{}},token(),reviewed);check(prior&&prior.value->changed==0,"no existing TEST originals changed by proposed one-day policy");
        const auto security=ins::mutable_descriptor(owner);
        for(const auto* role:{L"FTN/Inbound/TEST",L"FTN/Processed/TEST",L"FTN/Bad/TEST"})if(!std::filesystem::exists(root/role))ins::create_directory((root/role).make_preferred(),security.get());
        auto incoming=win::FilePayloadDirectory::private_store(root/L"FTN/Inbound/TEST",owner);
        for(unsigned i=1;i<=2;++i){
            app::FtnPacket packet;packet.header.origin=*app::parse_ftn_address("2:100/2@test");packet.header.destination=*app::parse_ftn_address("2:100/1@test");packet.header.year=2026;packet.header.month=10;packet.header.day=30;std::copy_n("packkey",7,packet.header.password.begin());
            app::FtnPackedMessage message;message.origin_net=message.destination_net=100;message.origin_node=2;message.destination_node=1;message.date="30 Oct 26  12:00:00";message.from="Controlled Host";message.to="All";message.subject="Installed retained original "+std::to_string(i);message.text="AREA:TEST.RETENTION\r\1MSGID: 2:100/2 installed-retention-"+std::to_string(i)+"\r\1CHRS: CP437 2\rSynthetic preserved content.\r";packet.messages.push_back(message);
            const auto bytes=app::encode_ftn_packet(packet);check(bytes.has_value(),"valid synthetic packet");
            app::FtnIngressReceipt ingress;ingress.id=win::WindowsSecureIdentifierGenerator{}.generate();ingress.session=win::WindowsSecureIdentifierGenerator{}.generate();check(sodium_init()>=0&&crypto_hash_sha256(ingress.identity.data(),reinterpret_cast<const unsigned char*>(bytes->data()),bytes->size())==0,"actual packet hash");
            ingress.network="TEST";ingress.link="DEST";ingress.proof=app::FtnIngressProof::inbound_cram;ingress.configuration_revision=config.revision;ingress.file={"retained"+std::to_string(i)+".pkt",bytes->size(),1793361600+i};
            {auto receiver=win::FtnNativeReceive::open(incoming,storage,ingress,[]{return true;});receiver.append(*bytes);receiver.finish();ingress=receiver.receipt();}
            const auto result=win::toss_ftn_native_receipt(storage,config,receipt,ingress,[]{return true;});if(!result.processed)std::cerr<<result.reason<<'\n';check(result.processed&&result.imported==1,"controlled host packet actually imported and retained natively");
        }
        const auto retained=storage.preview_ftn_retention({true,{}},token(),reviewed);check(retained&&retained.value->changed==2&&retained.value->shortened==2&&retained.value->networks.size()==1&&retained.value->networks[0].from_forever==2&&retained.value->networks[0].expire_later==2,"two actual Forever originals for installed UI review");
        check(storage.shutdown().succeeded(),"controlled installed writer stops normally");
    }
    auto offline=win::OfflineFileAdministration::begin(root,owner,why);check(bool(offline),"normal exclusive FileMan owner client");
    auto files=win::FilePayloadDirectory::area(root,L"Files/General",{L"Files/Uploads",L"Files/UploadToSysop"},{L"FTN"},owner);
    const std::string body="Synthetic detail walkthrough file.\r\n";
    {auto file=files.create("fixture-details.txt",body.size());file.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());}
    std::string description;for(unsigned i=1;i<=60;++i)description+="Detail line "+std::to_string(i)+": preserve the full original description while paging.\n";
    const auto run=[&](const app::FileManagementTask& task){auto result=offline->start(task);const auto deadline=GetTickCount64()+30000;while(result.code==app::LocalAccountResultCode::ok&&result.state==app::FileManagementJobState::running&&GetTickCount64()<deadline){Sleep(5);result=offline->status(task.job);}check(result.code==app::LocalAccountResultCode::ok&&result.state==app::FileManagementJobState::completed,"normal FileMan import job completes");return result;};
    app::FileManagementTask preview;preview.job=win::WindowsSecureIdentifierGenerator{}.generate();preview.kind=app::FileManagementTaskKind::preview_import;preview.choice.operation=win::WindowsSecureIdentifierGenerator{}.generate();preview.choice.area={1};preview.choice.source_path=(root/L"Files/General/fixture-details.txt").u8string();preview.choice.descriptions=app::FileDescriptions{"Short description must not duplicate full DIZ",description};preview.choice.public_uploader="Synthetic Sysop";
    const auto proposed=run(preview);check(proposed.preview.has_value(),"current exact import preview");app::FileManagementTask imported;imported.job=win::WindowsSecureIdentifierGenerator{}.generate();imported.kind=app::FileManagementTaskKind::import_file;imported.import=*proposed.preview;check(run(imported).journal_confirmed,"actual file import journaled");offline.reset();
    std::cout<<"PASS "<<checks<<" controlled installed host-ingress originals and normal FileMan detail fixture; no socket/peer claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
