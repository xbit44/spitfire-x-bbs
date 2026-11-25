#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#define SFX_FTN_TIC_ENTRY retained_stored_tic_fixture_main
#include "linux_ftn_native_tic_tests.cpp"
#undef SFX_FTN_TIC_ENTRY
#include "sfx/platform/linux/ftn_stored_scan.hpp"
#include <fcntl.h>
#include <unistd.h>
namespace {
std::string stored_message(std::string_view subject,std::uint16_t attributes=0x191,std::string_view flags="KFS"){
    std::string bytes(190,'\0');bytes.replace(0,10,"BRE System");bytes.replace(36,10,"BRE System");bytes.replace(72,subject.size(),subject);
    const auto word=[&](std::size_t offset,unsigned value){bytes[offset]=static_cast<char>(value);bytes[offset+1]=static_cast<char>(value>>8);};
    word(166,1);word(168,2);word(172,100);word(174,100);word(186,attributes);
    return bytes+"\1INTL 2:100/1 2:100/2\r\1FLAGS "+std::string(flags)+"\r\n"+'\0';
}
void local_game_file(const std::filesystem::path& path,std::string_view bytes){
    native::FileDescriptor file(open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644));
    check(file&&write(file.get(),bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size())&&fsync(file.get())==0&&fchmod(file.get(),0644)==0,"new local game file with normal readonly-public mode");
}
std::string local_bytes(const std::filesystem::path& path){std::ifstream input(path,std::ios::binary);check(bool(input),"read isolated local source");return {std::istreambuf_iterator<char>(input),{}};}
void change_local_bytes(const std::filesystem::path& path,std::string_view bytes){
    native::FileDescriptor file(open(path.c_str(),O_WRONLY|O_TRUNC|O_NOFOLLOW|O_CLOEXEC));
    check(file&&write(file.get(),bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size())&&fsync(file.get())==0,"change only isolated local source bytes");
}
#include "linux_ftn_stored_portability_checks.inc"
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh native stored-message fixture required");const auto root=std::filesystem::absolute(argv[1]);
    check(!std::filesystem::exists(root),"preserve previous stored-message evidence");std::filesystem::create_directories(root.parent_path());
    check(mkdir(root.c_str(),0700)==0,"private stored-message fixture");
    for(const auto* role:{"config","data","ftn","ftn/NetMail","ftn/inbound","ftn/inbound/test","ftn/outbound","ftn/outbound/test","ftn/bad","ftn/processed"})check(mkdir((root/role).c_str(),0700)==0,"private stored-message role");
    {auto config=native::open_owned_directory(root/"config");const std::string first="SFX_LINUX_FIRST_RUN=1\n";native::publish_private_file(config.get(),"first-run.v1",{first.begin(),first.end()},false);}
    check(bool((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"local game fixture portable board key");
    auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);native::FtnInstallation installed{root,bindings};
    sfx::test::FakeStoragePathPolicy paths;auto options=settings(root/"data");storage::SqliteStorage db(options,paths);
    check(bool(db.start())&&bool(db.shutdown()),"initialize current stored-message schema");const auto database=root/"data/sfx.db";seed(database);check(bool(db.start()),"start isolated managed store");
    const app::MessageConfigurationActor owner{true,{}};auto config=*db.ftn_configuration(owner).value;
    app::FtnNetwork network;network.code="TEST";network.name="Local games";network.address="2:100/2@test";network.enabled=true;network.msg_netmail_directory=(root/"ftn/NetMail").string();
    config.networks.push_back(network);app::FtnLink peer;peer.network="TEST";peer.code="PEER";peer.name="Synthetic delivery";peer.address="2:100/1@test";peer.host="127.0.0.1";peer.enabled=true;config.links.push_back(peer);
    unsigned sequence=500;const auto save=[&]{check(bool(db.change_ftn_configuration(owner,id(sequence++).bytes,config)),"save current per-network local game setup");config=*db.ftn_configuration(owner).value;};save();
    const auto mail=root/"ftn/NetMail";local_game_file(mail/"attach.bin","local game attachment");const auto request=stored_message("attach.bin");local_game_file(mail/"1.MSG",request);
    std::string cursor;bool running=true;const auto scan=[&]{cursor.clear();return native::scan_ftn_stored_messages(db,config,installed,cursor,[&]{return running;});};
    auto result=scan();check(result.queued==0&&result.failed==0&&scalar(database,"SELECT count(*) FROM sfx_ftn_outbound")==0,"network Off does not import or queue a local request");
    struct stat mode{};check(stat((mail/"1.MSG").c_str(),&mode)==0&&(mode.st_mode&0777)==0644,"disabled scan leaves source permissions unchanged");
    config.networks.front().msg_netmail_enabled=true;save();result=scan();if(result.failed)std::cerr<<result.reason<<'\n';
    check(result.queued==1&&result.failed==0&&std::filesystem::exists(mail/"1.MSG")&&std::filesystem::exists(mail/"attach.bin"),"new local request queues real independent spool but retains both originals before acknowledgement");
    check(stat((mail/"1.MSG").c_str(),&mode)==0&&(mode.st_mode&0777)==0600,"managed local request enters private board custody");
    result=scan();check(!result.failed&&!result.queued&&scalar(database,"SELECT count(*) FROM sfx_ftn_outbound")==1,"pending repeated scan does not create duplicate jobs");
    auto read_request=request;read_request[164]=1;read_request[186]=static_cast<char>(static_cast<unsigned char>(read_request[186])|4U);change_local_bytes(mail/"1.MSG",read_request);
    result=scan();check(!result.failed&&!result.queued&&scalar(database,"SELECT count(*) FROM sfx_ftn_outbound")==1,"local Received and timesRead edits preserve original request operation");
    bindings->refresh();check(bool(db.checkpoint())&&bool(db.shutdown()),"stop original board for whole-folder copy");
    const auto copied=root.parent_path()/(root.filename().string()+"-copied");check(!std::filesystem::exists(copied),"fresh copied stored-message board");std::filesystem::copy(root,copied,std::filesystem::copy_options::recursive);
    {
        auto copied_bindings=native::LinuxFilePayloadBindings::prepare(copied);native::FtnInstallation copy{copied,copied_bindings};
        storage::SqliteStorage copied_db(settings(copied/"data"),paths);check(bool(copied_db.start()),"open copied managed queue");
        const auto copied_config=copied_db.ftn_configuration(owner);std::string after;
        const auto copied_scan=native::scan_ftn_stored_messages(copied_db,*copied_config.value,copy,after,[]{return true;});
        check(!copied_scan.failed&&!copied_scan.queued&&scalar(copied/"data/sfx.db","SELECT count(*) FROM sfx_ftn_outbound")==1,"copy and Prepare keep exact pending request and portable identity without duplicate queue");
        check(std::filesystem::exists(copied/"ftn/NetMail/1.MSG")&&bool(copied_db.shutdown()),"copied unacknowledged original remains intact");
    }
    check(bool(db.start()),"reopen original owner store");
    const auto acknowledge=[&]{const auto claim=id(sequence++);auto jobs=db.claim_ftn_jobs(owner,"TEST","PEER",id(sequence++),claim,64,false);check(jobs&&jobs.value->size()==1,"claim exact independent outgoing attachment");check(bool(db.finish_ftn_job(owner,jobs.value->front().id,claim,app::FtnOutboundState::delivered,{})),"synthetic trusted transport records exact delivered job");};
    acknowledge();result=scan();if(result.failed)std::cerr<<result.reason<<'\n';check(!result.failed&&result.completed==1&&!std::filesystem::exists(mail/"1.MSG")&&!std::filesystem::exists(mail/"attach.bin"),"delivered exact job alone permits KillSent and KFS cleanup");
    local_game_file(mail/"keep.bin","retained attachment");local_game_file(mail/"2.MSG",stored_message("keep.bin",0x111,""));
    check(scan().queued==1,"queue non-KillSent request");acknowledge();result=scan();const auto sent=local_bytes(mail/"2.MSG");
    check(!result.failed&&result.completed==1&&(static_cast<unsigned char>(sent[186])&8)&&std::filesystem::exists(mail/"keep.bin"),"non-KillSent request marks Sent only after delivery and retains attachment");
    local_game_file(mail/"changed.bin","original attachment");local_game_file(mail/"3.MSG",stored_message("changed.bin"));check(scan().queued==1,"queue source replacement fixture");
    change_local_bytes(mail/"changed.bin","different attachment");acknowledge();result=scan();
    check(result.failed&&std::filesystem::exists(mail/"3.MSG")&&local_bytes(mail/"changed.bin")=="different attachment","changed attachment is preserved instead of KFS deletion");
    local_game_file(mail/"4.MSG",stored_message("missing.bin"));const auto jobs_before=scalar(database,"SELECT count(*) FROM sfx_ftn_outbound");result=scan();
    check(result.failed&&scalar(database,"SELECT count(*) FROM sfx_ftn_outbound")==jobs_before&&std::filesystem::exists(mail/"4.MSG"),"missing attachment never becomes queued/delivered");
    running=false;result=scan();check(result.failed&&scalar(database,"SELECT count(*) FROM sfx_ftn_outbound")==jobs_before,"cancelled scanner leaves queue unchanged");running=true;
    check(symlink("2.MSG",(mail/"5.MSG").c_str())==0&&mkfifo((mail/"6.MSG").c_str(),0600)==0,"create bounded link/FIFO negative fixtures");
    result=scan();check(result.failed&&std::filesystem::is_symlink(mail/"5.MSG")&&std::filesystem::exists(mail/"6.MSG"),"unsafe local request kinds are rejected without following or blocking");
    auto unsafe=config;unsafe.networks.front().msg_netmail_directory=(root/"ftn/inbound/test").string();cursor.clear();result=native::scan_ftn_stored_messages(db,unsafe,installed,cursor,[]{return true;});
    check(result.failed&&!result.queued,"peer transport spool cannot become trusted local game request source");
    const auto policy_mail=root/"ftn/PolicyMail";check(mkdir(policy_mail.c_str(),0700)==0,"isolated current-policy source");
    config.networks.front().msg_netmail_directory=policy_mail.string();save();
    local_game_file(policy_mail/"policy.bin","policy-bound attachment");local_game_file(policy_mail/"1.MSG",stored_message("policy.bin"));
    const auto stale=config;config.networks.front().msg_netmail_enabled=false;save();cursor.clear();
    result=native::scan_ftn_stored_messages(db,stale,installed,cursor,[]{return true;});
    check(result.failed&&!result.queued&&result.reason=="ftn_msg_configuration_changed"&&stat((policy_mail/"1.MSG").c_str(),&mode)==0&&(mode.st_mode&0777)==0644,"revoked source policy refuses fresh adoption and leaves raw preview unchanged");
    config.networks.front().msg_netmail_enabled=true;save();check(scan().queued==1,"current enabled policy queues exact new request");
    const auto delivered_policy=config;config.networks.front().msg_netmail_enabled=false;save();acknowledge();cursor.clear();
    result=native::scan_ftn_stored_messages(db,delivered_policy,installed,cursor,[]{return true;});
    check(result.failed&&!result.completed&&std::filesystem::exists(policy_mail/"1.MSG")&&std::filesystem::exists(policy_mail/"policy.bin"),"stale source policy cannot perform delivered KFS or Sent cleanup");
    config.networks.front().msg_netmail_enabled=true;save();result=scan();
    check(!result.failed&&result.completed==1&&!std::filesystem::exists(policy_mail/"1.MSG"),"current policy later completes exact delivered request");
    local_game_file(policy_mail/"2.MSG",stored_message("C:\\DOOR\\ATTACH.BIN"));const auto before_dos=scalar(database,"SELECT count(*) FROM sfx_ftn_outbound");result=scan();
    check(result.failed&&!result.queued&&std::filesystem::exists(policy_mail/"2.MSG")&&scalar(database,"SELECT count(*) FROM sfx_ftn_outbound")==before_dos,"DOS drive path is retained for review without invented host path conversion");
    check(bool(db.shutdown()),"stored-message fixture stops");
    stored_portability_checks(root.parent_path()/(root.filename().string()+"-portable"));
    std::cout<<"PASS "<<checks<<" native local MSG source/queue/delivery cleanup/Sent/restart/copy/case-sensitive cursor and Door portability checks; synthetic ACK, no engine/peer claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
