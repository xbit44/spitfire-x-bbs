#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#include "sfx/platform/linux/file_intake.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <fstream>

bool without_birth{};
extern "C" int __real_statx(int,const char*,int,unsigned,struct statx*);
extern "C" int __wrap_statx(int fd,const char* path,int flags,unsigned mask,struct statx* output){const auto result=__real_statx(fd,path,flags,mask,output);if(result==0&&without_birth)output->stx_mask&=~STATX_BTIME;return result;}

namespace app=sfx::application;
namespace native=sfx::platform::linux_host;
namespace storage=sfx::adapters::storage;
namespace sql=storage::internal;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 r;r.bytes[0]=19;r.bytes[14]=static_cast<std::uint8_t>(n>>8);r.bytes[15]=static_cast<std::uint8_t>(n);return r;}
void seed(const std::filesystem::path& file){
    sql::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
    c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000)","owned synthetic native-intake actors");
    c.exec("INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","complete synthetic fixture");
    for(unsigned n=1;n<=2;++n){sql::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1700000000,0,1700000000,NULL,NULL,?1)","synthetic live sessions, not authentication proof");q.bind(1,id(n));q.bind(2,static_cast<std::int64_t>(n));q.step();}
}
std::int64_t scalar(const std::filesystem::path& file,const char* query){sql::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return c.scalar_int64(query,"native intake fixture check");}
void append(native::LinuxFileIntake& receive,std::string_view bytes){receive.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());}
void child_write(native::LinuxFileIntake& receive,std::string_view bytes){
    auto descriptor=receive.begin_receive_transfer();const auto child=fork();check(child>=0,"spawn isolated intake writer");
    if(child==0){const auto written=write(descriptor.get(),bytes.data(),bytes.size());_exit(written==static_cast<ssize_t>(bytes.size())&&fsync(descriptor.get())==0?0:1);}
    descriptor=native::FileDescriptor();int status{};
    check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"intake writer reaped before custody return");
    receive.finish_receive_transfer_after_reap();
}
void mutation_publication_failure(const std::filesystem::path& root,bool moved){
    namespace fs=std::filesystem;
    const auto directory=[](const fs::path& p){check(fs::create_directory(p)&&chmod(p.c_str(),0700)==0,"fresh mutation fixture directory");};
    const auto map_bytes=[](const fs::path& board){const auto config=native::open_owned_directory(board/"config");return native::read_private_file(config.get(),"file-identities-linux.v1",1024*1024);};
    for(const auto& p:{root,root/"config",root/"files",root/"files/general",root/"files/uploads",root/"files/uploadtosysop",root/"data",root/"data/fileintake",root/"data/fileremoved"})directory(p);
    const auto config=native::open_owned_directory(root/"config");const std::string intent="SFX_LINUX_FIRST_RUN=1\n";
    native::publish_private_file(config.get(),"first-run.v1",{intent.begin(),intent.end()},false);
    check(static_cast<bool>((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"mutation fixture portable key");
    native::LinuxLocalStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/"data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"mutation fixture schema");seed(root/"data/sfx.db");check(db.start().succeeded(),"mutation fixture actors");
    const app::FileConfigurationActor owner{true,{}};const app::FileActor caller{id(2),2,1,1};
    const auto target=moved?root/"files/uploads":root.parent_path()/"external-mutation-copy";
    if(!moved){directory(target);const auto current=db.file_configuration(owner);app::FileConfigurationChange edit;edit.operation=id(90);edit.action=app::FileConfigurationAction::area;edit.expected_catalog_revision=current.value->policy.catalog_revision;edit.area=current.value->areas[0];edit.area.storage_path=target.u8string();edit.area.area.permissions.uploads_enabled=true;edit.area.area.sysop_review=false;check(db.change_file_configuration(owner,edit).succeeded(),"mutation external area");}
    const auto operation=id(100);std::string stage;app::FilePayloadIdentity expected;
    {
        auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);
        auto staging=native::FilePayloadDirectory::private_store(root/"data/fileintake",bindings);
        auto destination=native::FilePayloadDirectory::area(root,target,{}, {"data","config"},bindings);
        native::LinuxFileIntake receive(db,owner,caller,{operation,{moved?2:1},false,"2026-10-17"},staging,[&](auto at){
            if(at==(moved?native::FileIntakeBoundary::before_placement:native::FileIntakeBoundary::before_staging_cleanup))fs::create_hard_link(root/"config/file-identities-linux.v1",root/"config/publication-fault");
        });
        append(receive,"exact retained bytes");check(receive.finish("fault.txt",true).state==app::FileIntakeState::ready,"mutation fixture ready upload");expected=receive.record().payload;stage=receive.record().stage_key;
        const auto result=receive.accept(root,destination,{"Fault test",""},false);
        check((moved?result.state==app::FileIntakeState::held:result.state==app::FileIntakeState::accepted)&&staging.absent(stage)&&fs::exists(target/"fault.txt"),"actual namespace mutation precedes refused map publication");
        check(fs::exists(root/"config/publication-fault"),"real map publication fault was reached");fs::remove(root/"config/publication-fault");
        bool refused=false;try{bindings->refresh();}catch(const std::runtime_error&){refused=true;}check(refused,"mutation failure remains latched after hardlink removed");
    }
    check(db.shutdown().succeeded(),"unresolved original closes before any recovery or copy");
    const auto original_map=map_bytes(root);const auto copied=fs::path(root.u8string()+"-unresolved-copy");fs::copy(root,copied,fs::copy_options::recursive);
    storage::SqliteStorageSettings copy_settings=settings;copy_settings.data_directory=copied/"data";storage::SqliteStorage copied_db(copy_settings,paths);check(copied_db.start().succeeded(),"copied unresolved database opens");
    bool cancelled=false;try{native::prepare_file_intake_bindings(copied_db,owner,copied,[]{return true;});}catch(const std::runtime_error&){cancelled=true;}
    check(cancelled&&map_bytes(copied)==original_map,"cancelled copied pending Prepare preserves original authenticated map");
    if(moved){
        const auto file=copied/"files/uploads/fault.txt";fs::rename(file,copied/"files/uploads/retained-original");
        {std::ofstream changed(file,std::ios::binary);changed<<"different copied content";}check(chmod(file.c_str(),0600)==0,"private changed copied fixture");
        {auto attention=native::prepare_file_intake_bindings(copied_db,owner,copied);check(attention->deferred_records()==1&&fs::exists(file)&&copied_db.file_intake(owner,operation).value->state==app::FileIntakeState::held,"changed pending destination stays held and preserved with Needs attention");}
        fs::remove(file);fs::rename(copied/"files/uploads/retained-original",file);
    }
    {
        auto prepared=native::prepare_file_intake_bindings(copied_db,owner,copied);prepared->refresh();
        check(map_bytes(root)==original_map,"copied unresolved Prepare does not repair or mutate original board");
        if(moved){
            auto directory=native::FilePayloadDirectory::area(copied,"files/uploads",{}, {"data","config"},prepared);auto file=directory.read("fault.txt");const auto actual=file.identity();
            check(actual.volume==expected.volume&&actual.file_id==expected.file_id&&actual.sha256==expected.sha256,"copied pending rename rebinds only exact saved identity and bytes");
            check(copied_db.file_intake(owner,operation).value->state==app::FileIntakeState::held&&scalar(copied/"data/sfx.db","SELECT count(*) FROM sfx_file_catalog")==0,"copied Prepare leaves held upload unpublished and uncredited");
        }else{
            auto staging=native::FilePayloadDirectory::private_store(copied/"data/fileintake",prepared);
            check(!copied_db.file_intake(owner,operation).value->staging_clean,"Prepare updates custody without inventing DB cleanup receipt");
            check(native::cleanup_accepted_file_intake(copied_db,owner,operation,staging,copied,prepared),"accepted copied unlink completes exact verified cleanup after Prepare");
            prepared->refresh();
        }
    }
    {auto reopened=native::LinuxFilePayloadBindings::open(copied);reopened->refresh();check(true,"copied pending recovery map reopens cleanly");}
    check(copied_db.shutdown().succeeded()&&db.start().succeeded(),"copied board closes; unresolved original reopens independently");
    if(moved){
        {
            auto bindings=native::LinuxFilePayloadBindings::open(root);
            fs::rename(target/"fault.txt",target/"original.retained");fs::copy_file(target/"original.retained",target/"fault.txt");
            check(!native::reconcile_file_intake_placement(db,owner,operation,root,bindings),"byte-identical replacement cannot automatically reconcile old rename intent");
            fs::remove(target/"fault.txt");fs::rename(target/"original.retained",target/"fault.txt");
            if(without_birth)check(!native::reconcile_file_intake_placement(db,owner,operation,root,bindings),"changed fallback ctime remains unproved automatically");
            else check(native::reconcile_file_intake_placement(db,owner,operation,root,bindings),"reopen reconciles exact recorded native moved object");
        }
        auto bindings=native::prepare_file_intake_bindings(db,owner,root);bindings->refresh();
        auto destination=native::FilePayloadDirectory::area(root,target,{}, {"data","config"},bindings);auto file=destination.read("fault.txt");const auto actual=file.identity();
        check(actual.volume==expected.volume&&actual.file_id==expected.file_id&&actual.sha256==expected.sha256,"explicit Prepare verifies same-host pending fallback without ctime inference");
        check(db.file_intake(owner,operation).value->state==app::FileIntakeState::held&&scalar(root/"data/sfx.db","SELECT count(*) FROM sfx_file_catalog")==0,"original recovery never publishes or credits held upload");
    }else{
        auto bindings=native::LinuxFilePayloadBindings::open(root);auto staging=native::FilePayloadDirectory::private_store(root/"data/fileintake",bindings);
        check(native::cleanup_accepted_file_intake(db,owner,operation,staging,root,bindings),"accepted original unlink failure reconciles map after reopen");bindings->refresh();
        check(db.file_intake(owner,operation).value->staging_clean&&staging.absent(stage),"original accepted cleanup receipt matches actual absence");
    }
    check(db.shutdown().succeeded(),"mutation recovery fixture closes");
}
}
int main(int argc,char** argv){try{
    check(argc==2||(argc==3&&std::string_view(argv[2])=="--no-birth"),"new isolated root and optional forced fallback required");without_birth=argc==3;const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"never reuse or overwrite fixture");
    check(geteuid()!=0,"normal test account");
    const auto directory=[](const std::filesystem::path& p){check(std::filesystem::create_directory(p)&&chmod(p.c_str(),0700)==0,"fresh private directory");};
    for(const auto& p:{root,root/"config",root/"files",root/"files/general",root/"files/uploads",root/"files/uploadtosysop",root/"data",root/"data/fileintake",root/"data/fileremoved"})directory(p);
    const auto config_dir=native::open_owned_directory(root/"config");const std::string intent="SFX_LINUX_FIRST_RUN=1\n";
    native::publish_private_file(config_dir.get(),"first-run.v1",{intent.begin(),intent.end()},false);
    check(static_cast<bool>((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"real portable board identity");
    auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);
    native::LinuxLocalStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/"data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"fresh schema15 fixture");const auto file=settings.data_directory/"sfx.db";seed(file);check(db.start().succeeded(),"synthetic actors start");
    const app::FileConfigurationActor owner{true,{}};const app::FileActor caller{id(2),2,1,1},sysop{id(1),1,1,1};
    {const auto cfg=db.file_configuration(owner);app::FileConfigurationChange edit;edit.operation=id(90);edit.action=app::FileConfigurationAction::area;
        edit.expected_catalog_revision=cfg.value->policy.catalog_revision;edit.area=cfg.value->areas[2];
        edit.area.area.permissions.access=999;edit.area.area.permissions.upload=10;edit.area.area.permissions.download=999;
        check(db.change_file_configuration(owner,edit).succeeded(),"private999/10/999 permits real intake without browse access");}
    auto staging=native::FilePayloadDirectory::private_store(root/"data"/"fileintake",bindings);
    auto uploads=native::FilePayloadDirectory::area(root,"files/uploads",{"files/general","files/uploadtosysop"},{},bindings);
    auto private_area=native::FilePayloadDirectory::area(root,"files/uploadtosysop",{"files/general","files/uploads"},{},bindings);
    unsigned operation=100;unsigned scan_calls{};std::string private_name;
    const auto scanner=[&](native::FilePayloadLease& input,std::uint64_t maximum){++scan_calls;check(input.identity().bytes<=maximum,"scanner receives locked bounded actual input");return app::FileScanResult{app::FileScanVerdict::clean,{},"Synthetic clean result."};};
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);
        check(scalar(file,"SELECT count(*) FROM sfx_file_intake WHERE state=0")==1&&staging.names().size()==1,"durable journal precedes new private payload");
        check(receive.record().staged_object&&db.file_intake(owner,receive.record().operation).value->staged_object.has_value(),"fresh native staging bound before first caller byte");
        child_write(receive,"first upload"); // Real descriptor child, not protocol-helper qualification.
        const auto ready=receive.finish("first.txt",true,scanner);
        check(ready.state==app::FileIntakeState::ready&&scan_calls==0&&ready.scan.verdict==app::FileScanVerdict::not_scanned,"Off invokes no scanner or fake clean report");
        check(scalar(file,"SELECT count(*) FROM sfx_file_catalog")==0,"receive/validation alone not indexed");
        const auto accepted=receive.accept(root,uploads,{"First","Long description"},false);
        check(accepted.accepted&&accepted.state==app::FileIntakeState::accepted&&accepted.journal_confirmed,"actual atomic native move then SQLite acceptance");
        check(staging.names().empty()&&uploads.names()==std::vector<std::string>{"first.txt"},"exact source and final membership");
        const auto saved=db.file_intake(owner,receive.record().operation);
        check(saved.value->staging_clean&&saved.value->placed_object&&saved.value->placed_object->file_id==saved.value->staged_object->file_id,"same-volume rename and absent staging durably accounted");
        check(db.file_information(caller,accepted.accepted->id).succeeded()&&!db.reserve_file_download(caller,id(operation++),accepted.accepted->id,"2026-10-17"),"review upload visible but ordinary download denied");
        check(receive.accept(root,uploads,{"ignored after success",""},false).accepted->id==accepted.accepted->id,"same native completion reports stable receipt");
    }
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{3},true,"2026-10-17"},staging);append(receive,"private");receive.finish("private.txt",true);
        const auto accepted=receive.accept(root,private_area,{"Private",""},false);
        private_name=app::file_intake_payload_name(receive.record());check(private_name=="private.txt"&&private_area.absent(receive.record().stage_key)&&receive.record().payload.bytes==7,"final private area retains original basename; operation ID stays in staging/journal only");
        check(accepted.accepted&&!db.file_information(caller,accepted.accepted->id)&&db.file_information(sysop,accepted.accepted->id).succeeded(),"actual private placement omitted from caller DTOs");
        const auto stats=db.file_caller_statistics(caller,"2026-10-17");check(stats&&stats.value->counts.successful_uploads==2&&stats.value->counts.eligible_uploads==1,"private upload counted once but excluded from ratio");
    }
    check(private_area.read(private_name).identity().bytes==7,"private placement reopens after the original protected receive handle closes");
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"do not overwrite");receive.finish("first.txt",true);
        check(receive.accept(root,uploads,{"Collision",""},false).state==app::FileIntakeState::held,"catalog collision holds original without overwrite");
        check(uploads.read("first.txt").identity().bytes==12&&std::filesystem::exists(staging.path()/receive.record().stage_key),"both existing and rejected payload retained");
    }
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"wrong area");receive.finish("wrong.txt",true);
        check(receive.accept(root,private_area,{"Wrong",""},false).state==app::FileIntakeState::held&&!std::filesystem::exists(private_area.path()/"wrong.txt"),"host cannot bind a different configured destination");
    }
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);child_write(receive,"partial");
        check(receive.finish("partial.txt",false).state==app::FileIntakeState::held&&std::filesystem::exists(staging.path()/receive.record().stage_key),"failed transfer remains durable private hold");
    }
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"invalid zip bytes");
        check(receive.finish("unsafe.zip",true).state==app::FileIntakeState::held,"malformed archive cannot be accepted via manual description");
    }
    auto config=db.file_configuration(owner);app::FileConfigurationChange policy;policy.operation=id(operation++);policy.expected_catalog_revision=config.value->policy.catalog_revision;policy.policy=config.value->policy;policy.policy.scan_uploads=true;policy.policy.maximum_upload_bytes=16;
    check(db.change_file_configuration(owner,policy).succeeded(),"enable scan with bounded test cap");
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"clean");
        check(receive.finish("clean.txt",true,scanner).state==app::FileIntakeState::ready&&scan_calls==1,"actual held payload delivered to synthetic scanner");
        check(receive.accept(root,uploads,{"Clean",""},false).accepted.has_value(),"clean result plus actual placement commits");
    }
    const auto detected=[](native::FilePayloadLease&,std::uint64_t){return app::FileScanResult{app::FileScanVerdict::detected,{"Synthetic.Test.Signature"},"Injected detection; not actual malware."};};
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{3},true,"2026-10-17"},staging);append(receive,"harmless test");const auto key=receive.record().stage_key;
        const auto result=receive.finish("detected.txt",true,detected);
        check(result.state==app::FileIntakeState::deleted&&result.deletion_confirmed&&result.journal_confirmed&&!std::filesystem::exists(staging.path()/key),"synthetic positive causes actual exact new-stage deletion and receipt");
        check(std::filesystem::exists(uploads.path()/"first.txt")&&std::filesystem::exists(private_area.path()/std::filesystem::u8path(private_name)),"positive deletion leaves all established library content untouched");
    }
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging,[](auto at){if(at==native::FileIntakeBoundary::before_delete)throw std::runtime_error("injected deletion fault");});append(receive,"harmless fault");
        const auto result=receive.finish("held.txt",true,detected);
        check(result.state==app::FileIntakeState::held&&!result.deletion_confirmed&&std::filesystem::exists(staging.path()/receive.record().stage_key),"injected delete failure retains payload and detection privately");
    }
    for(const auto verdict:{app::FileScanVerdict::error,app::FileScanVerdict::timeout,app::FileScanVerdict::incomplete}){
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"hold");
        const auto result=receive.finish("held.txt",true,[verdict](auto&,auto){return app::FileScanResult{verdict,{},"Injected scan failure."};});
        check(result.state==app::FileIntakeState::held&&!result.deletion_confirmed&&std::filesystem::exists(staging.path()/receive.record().stage_key),"nonpositive scanner failure never deletes");
    }
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"1234567890123456");bool refused{};
        try{append(receive,"x");}catch(...){refused=true;}const auto result=receive.finish("large.txt",true,scanner);
        check(refused&&result.state==app::FileIntakeState::held&&receive.record().payload.bytes==16&&scan_calls==1,"actual limit blocks extra write and acceptance without scanning partial payload");
    }
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging,[](auto at){if(at==native::FileIntakeBoundary::after_placement)throw std::runtime_error("injected crash boundary");});append(receive,"placement");receive.finish("placed.txt",true,scanner);
        const auto result=receive.accept(root,uploads,{"Placed",""},false);
        check(result.state==app::FileIntakeState::held&&!result.accepted&&std::filesystem::exists(uploads.path()/"placed.txt"),"failure between move and DB commit retained without false accepted receipt");
        check(scalar(file,"SELECT count(*) FROM sfx_file_catalog WHERE filename='placed.txt'")==0&&scalar(file,"SELECT count(*) FROM sfx_file_intake WHERE filename='placed.txt' AND state=5")==1,"exact intended destination retained for reconciliation, not caller listed");
    }
    check(scalar(file,"SELECT count(*) FROM sfx_file_catalog")==3&&scalar(file,"SELECT upload_count FROM sfx_file_statistics WHERE account_id=2")==3,"only three successful native acceptances credited");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"restart durable intake and exact current schema");
    const auto pending=db.pending_file_intake(owner,256);check(pending&&pending.value->size()==10,"all ten unresolved attempts survive restart");
    check(!db.pending_file_intake({false,caller},256),"private hold journal not exposed to ordinary caller");

    const auto external=root.parent_path()/"external-library";directory(external);
    auto current=db.file_configuration(owner);app::FileConfigurationChange edit;edit.operation=id(operation++);edit.action=app::FileConfigurationAction::area;
    edit.expected_catalog_revision=current.value->policy.catalog_revision;edit.area=current.value->areas[0];edit.area.storage_path=external.u8string();
    edit.area.area.permissions.uploads_enabled=true;edit.area.area.sysop_review=false;
    check(db.change_file_configuration(owner,edit).succeeded(),"existing empty area configured outside board");
    auto outside=native::FilePayloadDirectory::area(root,external,{"files/uploads","files/uploadtosysop"},{"data","config"},bindings);
    app::Identifier128 cleanup_operation;std::string cleanup_stage;
    {
        native::LinuxFileIntake receive(db,owner,caller,{id(operation++),{1},false,"2026-10-17"},staging,[](auto at){if(at==native::FileIntakeBoundary::before_staging_cleanup)throw std::runtime_error("injected cleanup failure");});
        append(receive,"external copy");check(receive.finish("external.txt",true,scanner).state==app::FileIntakeState::ready,"external upload checked ready");
        const auto result=receive.accept(root,outside,{"External",""},false);check(result.accepted.has_value(),"same-filesystem external placement uses checked copy");
        const auto stored=db.file_intake(owner,receive.record().operation);check(stored&&stored.value&&stored.value->staged_object&&stored.value->placed_object&&stored.value->staged_object->volume!=stored.value->placed_object->volume,"portable staging and external native IDs remain distinct");
        check(!stored.value->staging_clean&&!staging.absent(receive.record().stage_key)&&outside.read("external.txt").byte_count()==13,"cleanup fault preserves original and committed external copy");
        cleanup_operation=receive.record().operation;cleanup_stage=receive.record().stage_key;
    }
    // A catalog receipt alone never authorizes deletion of the redundant stage
    // when the final object has since been replaced with byte-identical content.
    std::filesystem::rename(external/"external.txt",external/"original.retained");
    {auto replacement=outside.create("external.txt",256);const std::string text="external copy";replacement.append(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());}
    check(!native::cleanup_accepted_file_intake(db,owner,cleanup_operation,staging,root,bindings)&&!staging.absent(cleanup_stage),"replaced final refuses cleanup and preserves stage");
    {auto replacement=outside.read("external.txt",true);replacement.remove_exact();}
    std::filesystem::rename(external/"original.retained",external/"external.txt");
    check(native::cleanup_accepted_file_intake(db,owner,cleanup_operation,staging,root,bindings)&&staging.absent(cleanup_stage),"explicit exact final restoration permits redundant staging cleanup");
    check(outside.read("external.txt").byte_count()==13,"cleanup never removes final library payload");
    check(db.shutdown().succeeded(),"native intake storage closes");
    mutation_publication_failure(root.parent_path()/"rename-publication",true);
    mutation_publication_failure(root.parent_path()/"unlink-publication",false);
    std::cout<<"PASS "<<checks<<" Linux intake and external placement checks; no-birth="<<without_birth<<"; scanner results injected, no transfer helper claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
