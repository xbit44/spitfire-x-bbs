#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#define SFX_FTN_TIC_ENTRY retained_native_tic_admission_main
#include "linux_ftn_native_tic_tests.cpp"
#undef SFX_FTN_TIC_ENTRY
#include "sfx/platform/linux/ftn_native_toss.hpp"
#include "sfx/platform/linux/file_manager.hpp"
#include "sfx/platform/linux/ftn_processed_expiry.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
extern char** environ;
namespace {
int retry_hatch_child(const std::filesystem::path& root,std::string_view token){
    app::Identifier128 operation;check(token.size()==32,"exact child retry operation");for(std::size_t n=0;n<16;++n){const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("child operation encoding");};operation.bytes[n]=static_cast<std::uint8_t>((digit(token[2*n])<<4)|digit(token[2*n+1]));}
    sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(settings(root/"data"),paths);
    check(bool(db.start()),"second process opens actual original store");const auto original=db.ftn_hatch({true,{}},operation);
    check(original&&original.value,"second process reads exact durable request");
    if(original.value->state==app::FtnHatchState::queued){check(bool(db.shutdown()),"second process closes queued receipt read");return 0;}
    // Linux has one installation map owner, stronger than individual payload
    // flock. A separate native host must not create a stale competing map.
    try{auto bindings=native::LinuxFilePayloadBindings::open(root);(void)bindings;}
    catch(const std::exception& error){if(std::string_view(error.what())=="linux_file_map_already_owned"){check(bool(db.shutdown()),"second process closes refused ownership attempt");return 77;}throw;}
    check(bool(db.shutdown()),"unexpected second map owner closes store");return 78;
}
unsigned run_hatch_child(const std::filesystem::path& root,const app::Identifier128& operation){
    std::array<char,4097> executable{};const auto count=readlink("/proc/self/exe",executable.data(),executable.size()-1);
    check(count>0&&static_cast<std::size_t>(count)<executable.size()-1,"current native test executable path");
    const std::string path=root.string(),token=native::ftn_native_artifact_name(operation).substr(0,32);
    char* args[]={executable.data(),const_cast<char*>("--retry-hatch"),const_cast<char*>(path.c_str()),const_cast<char*>(token.c_str()),nullptr};
    pid_t child{};check(posix_spawn(&child,executable.data(),nullptr,nullptr,args,environ)==0,"launch isolated native hatch process");
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(30);int result{};
    for(;;){const auto waited=waitpid(child,&result,WNOHANG);if(waited==child)break;
        if(waited<0&&errno!=EINTR)throw std::runtime_error("native hatch wait failed");
        if(std::chrono::steady_clock::now()>=until){kill(child,SIGKILL);while(waitpid(child,&result,0)<0&&errno==EINTR){}throw std::runtime_error("second native hatch deadline");}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    check(WIFEXITED(result),"native hatch child exits normally");return static_cast<unsigned>(WEXITSTATUS(result));
}
}
#ifndef SFX_FTN_PUBLICATION_ENTRY
#define SFX_FTN_PUBLICATION_ENTRY main
#endif
int SFX_FTN_PUBLICATION_ENTRY(int argc,char** argv){try{
    if(argc==4&&std::string_view(argv[1])=="--retry-hatch")return retry_hatch_child(std::filesystem::absolute(argv[2]),argv[3]);
    check(argc==2,"fresh native publication fixture required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve native publication history");std::filesystem::create_directories(root.parent_path());check(::mkdir(root.c_str(),0700)==0,"private native publication fixture");
    for(const auto* role:{"config","data","data/fileremoved","data/fileintake","files","files/general","files/uploads","files/uploadtosysop","ftn","ftn/inbound","ftn/inbound/test","ftn/outbound","ftn/outbound/test","ftn/bad","ftn/bad/test","ftn/processed","ftn/processed/test","ftn/nonsecure"})check(::mkdir((root/role).c_str(),0700)==0,"private publication role");
    {auto config=native::open_owned_directory(root/"config");const std::string intent="SFX_LINUX_FIRST_RUN=1\n";native::publish_private_file(config.get(),"first-run.v1",{intent.begin(),intent.end()},false);}
    check(bool((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"portable publication board key");
    const auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);native::FtnInstallation installed{root,bindings};
    sfx::test::FakeStoragePathPolicy paths;auto s=settings(root/L"data");s.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;s.utc_now=[]{return static_cast<std::int64_t>(std::time(nullptr));};storage::SqliteStorage db(s,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"current publication schema initialized");const auto database=s.data_directory/L"sfx.db";seed(database);check(db.start().succeeded(),"native publication actors ready");const app::MessageConfigurationActor owner{true,{}};
    auto c=*db.ftn_configuration(owner).value;app::FtnNetwork net;net.code="TEST";net.name="Native publication";net.address="2:100/1@test";net.enabled=true;c.networks.push_back(net);app::FtnLink peer;peer.network=net.code;peer.code="PEER";peer.name="Synthetic transport peer";peer.address="2:100/2@test";peer.host="127.0.0.1";peer.enabled=true;peer.credentials.session=*native::protect_ftn_secret(root.u8string(),net.code,peer.code,native::FtnCredentialKind::session,"session-key");peer.credentials.tic=*native::protect_ftn_secret(root.u8string(),net.code,peer.code,native::FtnCredentialKind::tic,"tic-key");c.links.push_back(peer);c.file_echoes.push_back({net.code,"TEST.FILES","Native publication",1});c.permissions.push_back({net.code,peer.code,"TEST.FILES",true,true,false,false});check(db.change_ftn_configuration(owner,id(20).bytes,c).succeeded(),"separate protected TIC credential and Receive grant saved");c=*db.ftn_configuration(owner).value;
    const auto incoming=native::FilePayloadDirectory::private_store(root/L"ftn"/L"Inbound"/L"TEST",bindings),processed=native::FilePayloadDirectory::private_store(root/L"ftn"/L"Processed"/L"TEST",bindings),removed=native::FilePayloadDirectory::private_store(root/L"data"/L"fileremoved",bindings);
    const auto general=native::FilePayloadDirectory::area(root,L"files/general",{L"files/uploads",L"files/uploadtosysop"},{L"ftn"},bindings);const auto live=[]{return true;};unsigned next=100;
    const auto receive=[&](std::string_view bytes,std::string name){app::FtnIngressReceipt r;r.id=id(next++);r.session=id(next+10000);r.identity=tic_hash("publication-"+std::to_string(next));r.network=net.code;r.link=peer.code;r.proof=app::FtnIngressProof::inbound_cram;r.configuration_revision=c.revision;r.file={std::move(name),bytes.size(),1700000000+next};auto sink=native::FtnNativeReceive::open(incoming,db,r,live);for(std::size_t at=0;at<bytes.size();at+=16384)sink.append(bytes.substr(at,16384));sink.finish();return sink.receipt();};
    struct Input{app::FtnIngressReceipt tic,payload;std::string bytes;};
    const auto input=[&](std::string filename,char fill,bool replacement){Input r;r.bytes=std::string(150003,fill);r.bytes.back()='!';r.payload=receive(r.bytes,filename);app::FtnTic tic;tic.area="TEST.FILES";tic.file=filename;tic.origin=*app::parse_ftn_address("2:100/9@test");tic.from=*app::parse_ftn_address(peer.address);tic.to=*app::parse_ftn_address(net.address);tic.size=r.bytes.size();tic.crc=app::ftn_crc32(r.bytes);tic.password="tic-key";tic.descriptions={"Actual native received file"};tic.path.push_back({tic.from,std::uint64_t{1700000000},{}});if(replacement)tic.replaces="*.zip";const auto text=app::encode_ftn_tic(tic);check(text.has_value(),"TIC fixture encodes");r.tic=receive(*text,"control.tic");return r;};
    const auto publish=[&](const Input& input,const std::function<void(std::string_view)>& boundary={},bool force_copy=false){return native::publish_ftn_native_tic(db,db,c,installed,input.tic,live,boundary,force_copy);};
    std::cout<<"STAGE ftn_tic_arrival_order_checks.inc\n"<<std::flush;
    #include "linux_ftn_tic_arrival_order_checks.inc"
    const auto retained_name=[&](const app::FtnIngressReceipt& receipt){auto file=native::open_ftn_received(db,installed,receipt,live,false);check(file.path().parent_path()==processed.path(),"native journal proof resolves retained Processed original");return file.path().filename().u8string();};
    const auto initial=input("first.zip",'A',false);auto result=publish(initial);if(!result.processed)std::cerr<<result.reason<<'\n';check(result.processed,"actual native initial TIC publication completes");check(general.read("first.zip").identity().sha256==tic_hash(initial.bytes)&&incoming.absent(native::ftn_native_artifact_name(initial.payload.id))&&!processed.absent(retained_name(initial.payload)),"published content and both original custody roots verified");
    std::cout<<"STAGE ftn_forward_plan_native_checks.inc\n"<<std::flush;
    #include "linux_ftn_forward_plan_native_checks.inc"
    const auto first_id=app::FileId{scalar(database,"SELECT file_id FROM sfx_file_catalog WHERE filename='first.zip'")};check(publish(initial).processed&&scalar(database,"SELECT count(*) FROM sfx_file_catalog WHERE filename='first.zip'")==1,"completed native retry never duplicates its catalog identity");
    const auto replacement=input("first.zip",'B',true);{auto busy=general.read("first.zip");result=publish(replacement);check(!result.processed&&busy.identity().sha256==tic_hash(initial.bytes),"busy target retains original and prevents displacement");}
    result=publish(replacement);if(!result.processed)std::cerr<<result.reason<<'\n';check(result.processed&&general.read("first.zip").identity().sha256==tic_hash(replacement.bytes),"busy replacement recovers after reader closes");const auto displaced=db.managed_file(owner,first_id);check(displaced&&displaced.value->file.origin==app::FileOrigin::ftn&&removed.read(displaced.value->removed_key).identity().sha256==tic_hash(initial.bytes),"actual displaced original stays recoverable and retains provenance");
    native::LinuxFileManager manager(db,owner,root,bindings);app::FileOperationRequest restore;restore.operation=id(next++);restore.action=app::FileOperationAction::restore;restore.file=first_id;restore.expected_revision=displaced.value->file.revision;restore.destination={1};restore.expected_destination_revision=1;restore.filename="recovered.zip";check(manager.execute(restore).completed&&general.read("recovered.zip").identity().sha256==tic_hash(initial.bytes),"existing native owner Restore recovers actual TIC displacement");
    for(const auto* stop:{"stage_bound","stage_copy_chunk","stage_verified_before_checkpoint","stage_verified","recovery_bound","original_displaced_before_checkpoint","displacement_verified","stage_renamed_before_checkpoint","placement_verified","before_catalog_commit","after_catalog_commit"}){
        const auto name="case-"+std::to_string(next)+".zip";auto old=input(name,'C',false);check(publish(old).processed,"fault case original published");auto incoming_file=input(name,'D',true);bool fired{};const auto failed=publish(incoming_file,[&](std::string_view at){if(!fired&&at==stop){fired=true;throw std::runtime_error("ftn_injected_stop");}});check(fired&&!failed.processed,"injected boundary stops without a completion claim");check(db.shutdown().succeeded()&&db.start().succeeded(),"actual database restart between interrupted native phases");const auto resumed=publish(incoming_file);if(!resumed.processed)std::cerr<<stop<<": "<<resumed.reason<<'\n';check(resumed.processed&&general.read(name).identity().sha256==tic_hash(incoming_file.bytes),"exact journal-bound native recovery publishes correct new bytes");
    }
    for(const auto* stop:{"recovery_bound","recovery_copy_chunk","recovery_verified_before_remove","original_displaced_before_checkpoint"}){
        const auto name="copy-"+std::to_string(next)+".zip";auto old=input(name,'F',false);check(publish(old).processed,"copy-path original published");auto newer=input(name,'G',true);bool fired{};const auto failed=publish(newer,[&](std::string_view at){if(!fired&&at==stop){fired=true;throw std::runtime_error("ftn_injected_copy_stop");}},true);check(fired&&!failed.processed,"forced native copy path interrupted without fabricating a volume");check(db.shutdown().succeeded()&&db.start().succeeded(),"copy-path actual managed restart");const auto resumed=publish(newer);if(!resumed.processed)std::cerr<<stop<<": "<<resumed.reason<<'\n';check(resumed.processed&&general.read(name).identity().sha256==tic_hash(newer.bytes),"journal resumes partial copy or post-delete custody without requiring diagnostic mode");
    }
    const auto purge_id=app::FileId{scalar(database,"SELECT file_id FROM sfx_ftn_file_displacements WHERE state=0 ORDER BY rowid DESC LIMIT 1")};const auto purge_source=db.managed_file(owner,purge_id);check(bool(purge_source),"exact retained displacement selected for explicit owner purge");app::FileOperationRequest purge;purge.operation=id(next++);purge.action=app::FileOperationAction::purge;purge.file=purge_id;purge.expected_revision=purge_source.value->file.revision;check(manager.execute(purge).completed&&removed.absent(purge_source.value->removed_key)&&db.managed_file(owner,purge_id).value->purged,"existing owner native Purge removes only selected FTN recovery bytes");
    auto unbound=input("unbound.zip",'E',false);bool fired{};result=publish(unbound,[&](std::string_view at){if(at=="stage_created_before_binding"){fired=true;throw std::runtime_error("ftn_injected_stop");}});check(fired&&!result.processed,"create-before-bind interruption is retained");result=publish(unbound);check(!result.processed&&result.reason=="ftn_tic_unbound_stage"&&general.absent("unbound.zip"),"unbound staging name is held, never adopted or deleted");
    auto old_unbound=input("unbound-recovery.zip",'H',false);check(publish(old_unbound).processed,"recovery-binding-gap original published");auto new_unbound=input("unbound-recovery.zip",'I',true);fired=false;result=publish(new_unbound,[&](std::string_view at){if(at=="recovery_created_before_binding"){fired=true;throw std::runtime_error("ftn_injected_stop");}},true);check(fired&&!result.processed,"recovery create-before-binding gap retained");result=publish(new_unbound);check(!result.processed&&result.reason=="ftn_tic_unbound_recovery"&&general.read("unbound-recovery.zip").identity().sha256==tic_hash(old_unbound.bytes),"unbound recovery object never adopted and original stays intact");
    auto old_revoked=input("revoked.zip",'J',false);check(publish(old_revoked).processed,"late-revocation original published");auto new_revoked=input("revoked.zip",'K',true);fired=false;result=publish(new_revoked,[&](std::string_view at){if(!fired&&at=="displacement_verified"){fired=true;auto changed=c;changed.permissions.front().receive=false;check(db.change_ftn_configuration(owner,id(next++).bytes,changed).succeeded(),"owner revokes Receive after native displacement");}});check(fired&&!result.processed&&result.reason=="ftn_tic_configuration_changed"&&general.absent("revoked.zip"),"late revocation prevents placement/catalog publication");const auto old_id=app::FileId{scalar(database,"SELECT file_id FROM sfx_file_catalog WHERE filename='revoked.zip'")};check(db.managed_file(owner,old_id).value->operation_pending&&scalar(database,"SELECT count(*) FROM sfx_ftn_file_publications WHERE filename='revoked.zip' AND phase=4")==1,"both displaced original and staged new payload remain journal-owned under hold");
    std::cout<<"STAGE ftn_tic_rejection_publication_checks.inc\n"<<std::flush;
    #include "linux_ftn_tic_rejection_publication_checks.inc"
    std::cout<<"STAGE ftn_tic_continuation_native_checks.inc\n"<<std::flush;
    #include "linux_ftn_tic_continuation_native_checks.inc"
    std::cout<<"STAGE ftn_tic_continuation_negative_checks.inc\n"<<std::flush;
    #include "linux_ftn_tic_continuation_negative_checks.inc"
    std::cout<<"STAGE ftn_native_forward_checks.inc\n"<<std::flush;
    #include "linux_ftn_native_forward_checks.inc"
    std::cout<<"STAGE ftn_native_hatch_checks.inc\n"<<std::flush;
    #include "linux_ftn_native_hatch_checks.inc"
    std::cout<<"STAGE ftn_file_retention_native_checks.inc\n"<<std::flush;
    #include "linux_ftn_file_retention_native_checks.inc"
    check(scalar(database,"SELECT count(*) FROM sfx_file_statistics")==0&&scalar(database,"SELECT count(*) FROM pragma_foreign_key_check")==0,"all native publication/recovery paths avoid caller credit and dangling provenance");check(db.shutdown().succeeded(),"native publication fixture closes");std::cout<<"PASS "<<checks<<" actual Linux/portable credentials/SQLite TIC publication, outgoing spool/restart and credential separation, same-volume rename and forced-copy recovery, owner Restore/Purge and late revocation; synthetic transport proof and GOT, no real cross-volume/installed/hub acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
