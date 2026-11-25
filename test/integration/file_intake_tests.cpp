#include "sfx/platform/win32/file_intake.hpp"
#include "sfx/platform/win32/file_manager.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace native=sfx::platform::win32;
namespace ins=native::installation;
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
void append(native::WindowsFileIntake& receive,std::string_view bytes){receive.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());}
}
int main(int argc,char** argv){try{
    check(argc==2,"new isolated root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"never reuse or overwrite fixture");
    std::filesystem::create_directories(root.parent_path());const auto sid=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(sid);
    for(const auto& p:{root,root/L"Files",root/L"Files"/L"General",root/L"Files"/L"Uploads",root/L"Files"/L"UploadToSysop",root/L"Data",root/L"Data"/L"FileIntake",root/L"Data"/L"FileRemoved"})ins::create_directory(p,descriptor.get());
    sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"fresh schema15 fixture");const auto file=settings.data_directory/L"SFX.DB";seed(file);check(db.start().succeeded(),"synthetic actors start");
    const app::FileConfigurationActor owner{true,{}};const app::FileActor caller{id(2),2,1,1},sysop{id(1),1,1,1};
    {const auto cfg=db.file_configuration(owner);app::FileConfigurationChange edit;edit.operation=id(90);edit.action=app::FileConfigurationAction::area;
        edit.expected_catalog_revision=cfg.value->policy.catalog_revision;edit.area=cfg.value->areas[2];
        edit.area.area.permissions.access=999;edit.area.area.permissions.upload=10;edit.area.area.permissions.download=999;
        check(db.change_file_configuration(owner,edit).succeeded(),"private999/10/999 permits real intake without browse access");}
    auto staging=native::FilePayloadDirectory::private_store(root/L"Data"/L"FileIntake",sid);
    auto uploads=native::FilePayloadDirectory::area(root,L"Files/Uploads",{L"Files/General",L"Files/UploadToSysop"},{},sid);
    auto private_area=native::FilePayloadDirectory::area(root,L"Files/UploadToSysop",{L"Files/General",L"Files/Uploads"},{},sid);
    unsigned operation=100;unsigned scan_calls{};std::string private_name;
    const auto scanner=[&](native::FilePayloadLease& input,std::uint64_t maximum){++scan_calls;check(input.identity().bytes<=maximum,"scanner receives locked bounded actual input");return app::FileScanResult{app::FileScanVerdict::clean,{},"Synthetic clean result."};};
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);
        check(scalar(file,"SELECT count(*) FROM sfx_file_intake WHERE state=0")==1&&staging.names().size()==1,"durable journal precedes new private payload");
        check(receive.record().staged_object&&db.file_intake(owner,receive.record().operation).value->staged_object.has_value(),"fresh native staging bound before first caller byte");
        {auto receive_handle=receive.receive_handle();DWORD count{};check(WriteFile(receive_handle.get(),"first upload",12,&count,nullptr)&&count==12,"native transfer duplicate writes only already journaled staging");}
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
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{3},true,"2026-10-17"},staging);append(receive,"private");receive.finish("private.txt",true);
        const auto accepted=receive.accept(root,private_area,{"Private",""},false);
        private_name=app::file_intake_payload_name(receive.record());check(private_name=="private.txt"&&private_area.absent(receive.record().stage_key)&&receive.record().payload.bytes==7,"final private area retains original basename; operation ID stays in staging/journal only");
        check(accepted.accepted&&!db.file_information(caller,accepted.accepted->id)&&db.file_information(sysop,accepted.accepted->id).succeeded(),"actual private placement omitted from caller DTOs");
        const auto stats=db.file_caller_statistics(caller,"2026-10-17");check(stats&&stats.value->counts.successful_uploads==2&&stats.value->counts.eligible_uploads==1,"private upload counted once but excluded from ratio");
    }
    check(private_area.read(private_name).identity().bytes==7,"private placement reopens after the original protected receive handle closes");
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"do not overwrite");receive.finish("first.txt",true);
        check(receive.accept(root,uploads,{"Collision",""},false).state==app::FileIntakeState::held,"catalog collision holds original without overwrite");
        check(uploads.read("first.txt").identity().bytes==12&&std::filesystem::exists(staging.path()/receive.record().stage_key),"both existing and rejected payload retained");
    }
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"wrong area");receive.finish("wrong.txt",true);
        check(receive.accept(root,private_area,{"Wrong",""},false).state==app::FileIntakeState::held&&!std::filesystem::exists(private_area.path()/L"wrong.txt"),"host cannot bind a different configured destination");
    }
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"partial");
        check(receive.finish("partial.txt",false).state==app::FileIntakeState::held&&std::filesystem::exists(staging.path()/receive.record().stage_key),"failed transfer remains durable private hold");
    }
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"invalid zip bytes");
        check(receive.finish("unsafe.zip",true).state==app::FileIntakeState::held,"malformed archive cannot be accepted via manual description");
    }
    auto config=db.file_configuration(owner);app::FileConfigurationChange policy;policy.operation=id(operation++);policy.expected_catalog_revision=config.value->policy.catalog_revision;policy.policy=config.value->policy;policy.policy.scan_uploads=true;policy.policy.maximum_upload_bytes=16;
    check(db.change_file_configuration(owner,policy).succeeded(),"enable scan with bounded test cap");
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"clean");
        check(receive.finish("clean.txt",true,scanner).state==app::FileIntakeState::ready&&scan_calls==1,"actual held payload delivered to synthetic scanner");
        check(receive.accept(root,uploads,{"Clean",""},false).accepted.has_value(),"clean result plus actual placement commits");
    }
    const auto detected=[](native::FilePayloadLease&,std::uint64_t){return app::FileScanResult{app::FileScanVerdict::detected,{"Synthetic.Test.Signature"},"Injected detection; not actual malware."};};
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{3},true,"2026-10-17"},staging);append(receive,"harmless test");const auto key=receive.record().stage_key;
        const auto result=receive.finish("detected.txt",true,detected);
        check(result.state==app::FileIntakeState::deleted&&result.deletion_confirmed&&result.journal_confirmed&&!std::filesystem::exists(staging.path()/key),"synthetic positive causes actual exact new-stage deletion and receipt");
        check(std::filesystem::exists(uploads.path()/L"first.txt")&&std::filesystem::exists(private_area.path()/std::filesystem::u8path(private_name)),"positive deletion leaves all established library content untouched");
    }
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging,[](auto at){if(at==native::FileIntakeBoundary::before_delete)throw std::runtime_error("injected deletion fault");});append(receive,"harmless fault");
        const auto result=receive.finish("held.txt",true,detected);
        check(result.state==app::FileIntakeState::held&&!result.deletion_confirmed&&std::filesystem::exists(staging.path()/receive.record().stage_key),"injected delete failure retains payload and detection privately");
    }
    for(const auto verdict:{app::FileScanVerdict::error,app::FileScanVerdict::timeout,app::FileScanVerdict::incomplete}){
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"hold");
        const auto result=receive.finish("held.txt",true,[verdict](auto&,auto){return app::FileScanResult{verdict,{},"Injected scan failure."};});
        check(result.state==app::FileIntakeState::held&&!result.deletion_confirmed&&std::filesystem::exists(staging.path()/receive.record().stage_key),"nonpositive scanner failure never deletes");
    }
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging);append(receive,"1234567890123456");bool refused{};
        try{append(receive,"x");}catch(...){refused=true;}const auto result=receive.finish("large.txt",true,scanner);
        check(refused&&result.state==app::FileIntakeState::held&&receive.record().payload.bytes==16&&scan_calls==1,"actual limit blocks extra write and acceptance without scanning partial payload");
    }
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{2},false,"2026-10-17"},staging,[](auto at){if(at==native::FileIntakeBoundary::after_placement)throw std::runtime_error("injected crash boundary");});append(receive,"placement");receive.finish("placed.txt",true,scanner);
        const auto result=receive.accept(root,uploads,{"Placed",""},false);
        check(result.state==app::FileIntakeState::held&&!result.accepted&&std::filesystem::exists(uploads.path()/L"placed.txt"),"failure between move and DB commit retained without false accepted receipt");
        check(scalar(file,"SELECT count(*) FROM sfx_file_catalog WHERE filename='placed.txt'")==0&&scalar(file,"SELECT count(*) FROM sfx_file_intake WHERE filename='placed.txt' AND state=5")==1,"exact intended destination retained for reconciliation, not caller listed");
    }
    check(scalar(file,"SELECT count(*) FROM sfx_file_catalog")==3&&scalar(file,"SELECT upload_count FROM sfx_file_statistics WHERE account_id=2")==3,"only three successful native acceptances credited");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"restart durable intake and exact current schema");
    const auto pending=db.pending_file_intake(owner,256);check(pending&&pending.value->size()==10,"all ten unresolved attempts survive restart");
    check(!db.pending_file_intake({false,caller},256),"private hold journal not exposed to ordinary caller");
    const auto change_area=[&](std::int64_t area_id,const auto& edit){
        const auto current=db.file_configuration(owner);app::FileConfigurationChange change;change.operation=id(operation++);change.action=app::FileConfigurationAction::area;change.expected_catalog_revision=current.value->policy.catalog_revision;
        change.area=*std::find_if(current.value->areas.begin(),current.value->areas.end(),[area_id](const auto& ar){return ar.area.id.value==area_id;});edit(change.area);return db.change_file_configuration(owner,change);
    };
    check(change_area(1,[](auto& ar){ar.area.sysop_review=true;ar.area.permissions.uploads_enabled=true;ar.area.maximum_upload_bytes=32;ar.area.ratio_uploads=false;}).succeeded(),"enable explicit per-area review and selected-area ratio exclusion");
    check(change_area(2,[](auto& ar){ar.area.maximum_upload_bytes=24;}).succeeded(),"review placement has independent override");
    auto general=native::FilePayloadDirectory::area(root,L"Files/General",{L"Files/Uploads",L"Files/UploadToSysop"},{},sid);
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{1},false,"2026-10-17"},staging);
        check(receive.record().area.value==1&&receive.record().placement_area.value==2&&receive.record().maximum_bytes==24,"journal keeps original intent and stricter effective placement override");
        append(receive,"review required");check(receive.finish("reviewed.txt",true,scanner).state==app::FileIntakeState::ready,"review upload eligible for metadata");
        const auto result=receive.accept(root,uploads,{"Review",""},false);
        check(result.accepted&&result.accepted->area.value==2&&result.accepted->bytes==15&&!uploads.absent("reviewed.txt")&&general.absent("reviewed.txt"),"actual placement in Uploads not intended final library");
        check(db.managed_file(owner,result.accepted->id).value->intended_area.value==1&&!db.managed_file({false,caller},result.accepted->id),"intended final area retained only in authorized management context");
        check(db.file_information(caller,result.accepted->id).succeeded()&&!db.reserve_file_download(caller,id(operation++),result.accepted->id,"2026-10-17"),"review acceptance listable but not caller-downloadable");
        check(db.file_caller_statistics(caller,"2026-10-17").value->counts.eligible_uploads==2,"review routing does not override intended area ratio exclusion");
    }
    check(uploads.read("reviewed.txt").identity().bytes==15,"reopen final content only after protective receive lease is released");
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{1},false,"2026-10-17"},staging);append(receive,"wrong target");receive.finish("bypass.txt",true,scanner);
        check(receive.accept(root,general,{"Bypass",""},false).state==app::FileIntakeState::held&&general.absent("bypass.txt"),"caller host cannot bypass review by selecting final-area capability");
    }
    check(change_area(2,[](auto& ar){ar.area.enabled=false;}).succeeded(),"disable review destination");
    check(!db.begin_file_intake(caller,{id(operation++),{1},false,"2026-10-17"}),"disabled review area refuses start, never silently publishes elsewhere");
    check(change_area(2,[](auto& ar){ar.area.enabled=true;}).succeeded(),"restore review destination");
    check(change_area(1,[](auto& ar){ar.area.permissions.upload=20;}).succeeded(),"raise selected area upload threshold");
    check(!db.begin_file_intake(caller,{id(operation++),{1},false,"2026-10-17"}),"permissive Uploads does not bypass selected area permissions");
    check(change_area(1,[](auto& ar){ar.area.permissions.upload=1;}).succeeded(),"restore selected permission");
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{1},false,"2026-10-17"},staging);append(receive,"revoke");receive.finish("revoked.txt",true,scanner);
        check(change_area(1,[](auto& ar){ar.area.sysop_review=false;}).succeeded(),"review policy can change during receive");
        check(receive.accept(root,uploads,{"Revoked",""},false).state==app::FileIntakeState::held&&uploads.absent("revoked.txt"),"changed routing holds without publishing or reinterpreting original intent");
    }
    {
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{1},false,"2026-10-17"},staging);append(receive,"direct");receive.finish("direct.txt",true,scanner);
        check(receive.record().placement_area.value==1&&receive.accept(root,general,{"Direct",""},false).accepted.has_value(),"review Off keeps authorized direct destination");
    }
    check(!change_area(3,[](auto& ar){ar.area.sysop_review=true;}),"private submissions can never be redirected into public review area");
    check(scalar(file,"SELECT count(*) FROM sfx_file_catalog")==5&&scalar(file,"SELECT upload_count FROM sfx_file_statistics WHERE account_id=2")==5,"only committed direct and review accepts added to earlier exact counters");
    {
        std::uint64_t ceiling=native::kFilePayloadFreeReserve+32;
        auto limited=native::FilePayloadDirectory::private_store(staging.path(),sid,[&]{return ceiling;});
        native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{1},false,"2026-10-17"},limited);append(receive,"four");ceiling=native::kFilePayloadFreeReserve;
        bool refused{};try{append(receive,"x");}catch(...){refused=true;}
        const auto before_scan_calls=scan_calls;const auto result=receive.finish("low-space.txt",true,scanner);
        check(refused&&result.state==app::FileIntakeState::held&&!result.accepted&&receive.record().payload.bytes==4&&scan_calls==before_scan_calls,"injected mid-transfer low space preserves partial HOLD without scanning or credit");
    }
    {
        const auto before=staging.names();const auto journals=scalar(file,"SELECT count(*) FROM sfx_file_intake");bool refused{};
        auto full=native::FilePayloadDirectory::private_store(staging.path(),sid,[]{return std::uint64_t{0};});
        try{native::WindowsFileIntake receive(db,owner,caller,{id(operation++),{1},false,"2026-10-17"},full);}catch(...){refused=true;}
        check(refused&&staging.names()==before&&scalar(file,"SELECT count(*) FROM sfx_file_intake")==journals+1,"pre-create low-space refusal retains journal but writes no payload");
        check(scalar(file,"SELECT upload_count FROM sfx_file_statistics WHERE account_id=2")==5,"space failures never grant upload credit");
    }
    {
        const auto interrupted=id(operation++);bool failed{};
        try{native::WindowsFileIntake receive(db,owner,caller,{interrupted,{1},false,"2026-10-17"},staging,[](auto at){if(at==native::FileIntakeBoundary::staging_bound)throw std::runtime_error("synthetic after-create interruption");});}catch(...){failed=true;}
        const auto journal=db.file_intake(owner,interrupted);auto exact=staging.read(journal.value->stage_key);
        check(failed&&journal.value->state==app::FileIntakeState::held&&journal.value->staged_object&&exact.object_identity().file_id==journal.value->staged_object->file_id&&exact.identity().bytes==0,"after-create interruption keeps durable exact empty staging binding");
        check(!native::cleanup_accepted_file_intake(db,owner,interrupted,staging,root,sid),"accepted-only cleanup cannot delete held or incomplete uploads");
    }
    {
        const auto interrupted=id(operation++);std::string key;
        {
            native::WindowsFileIntake receive(db,owner,caller,{interrupted,{1},false,"2026-10-17"},staging,[](auto at){if(at==native::FileIntakeBoundary::before_staging_cleanup)throw std::runtime_error("synthetic after-commit interruption");});
            append(receive,"cleanup proof");receive.finish("cleanup.txt",true,scanner);key=receive.record().stage_key;
            check(receive.accept(root,general,{"Cleanup",""},false).accepted.has_value(),"acceptance stays committed when cleanup journal interrupted");
        }
        check(!db.file_intake(owner,interrupted).value->staging_clean,"accepted operation remains eligible for explicit cleanup reconciliation");
        const auto cleanup_pending=db.pending_file_intake(owner,256);
        check(std::any_of(cleanup_pending.value->begin(),cleanup_pending.value->end(),[&](const auto& r){return r.operation==interrupted;}),"pending intake includes accepted but unreconciled staging");
        {
            auto unrelated=staging.create(key,8);unrelated.append(reinterpret_cast<const std::uint8_t*>("unknown"),7);
            check(!native::cleanup_accepted_file_intake(db,owner,interrupted,staging,root,sid)&&unrelated.identity().bytes==7,"replacement staging object retained, never guessed redundant");
            unrelated.remove_exact(); // only this exact test-created fixture
        }
        check(db.shutdown().succeeded()&&db.start().succeeded(),"reopen durable interrupted-cleanup journal");
        check(native::cleanup_accepted_file_intake(db,owner,interrupted,staging,root,sid)&&native::cleanup_accepted_file_intake(db,owner,interrupted,staging,root,sid),"explicit absent staging reconciliation idempotent after restart");
        check(general.read("cleanup.txt").identity().bytes==13&&db.file_intake(owner,interrupted).value->staging_clean&&scalar(file,"SELECT upload_count FROM sfx_file_statistics WHERE account_id=2")==6,"cleanup leaves final payload and success-only accounting exact");
    }
    {
        auto current=db.file_configuration(owner);app::FileConfigurationChange recovery_policy;recovery_policy.operation=id(operation++);recovery_policy.expected_catalog_revision=current.value->policy.catalog_revision;recovery_policy.policy=current.value->policy;recovery_policy.policy.maximum_upload_bytes=32;recovery_policy.policy.scan_uploads=false;check(db.change_file_configuration(owner,recovery_policy).succeeded(),"configure tiny accepted-copy recovery fixture");
        const auto recovery=id(operation++);const auto begun=db.begin_file_intake(caller,{recovery,{1},false,"2026-10-17"});check(begun&&begun.value,"native copied-acceptance intent starts");const auto key=begun.value->stage_key;const std::string body="recovery";
        {
            auto original=staging.create(key,32);check(db.bind_file_intake_object(caller,recovery,false,original.object_identity()).succeeded(),"actual original recovery object bound before write");original.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());app::FileIntakeReceived received;received.operation=recovery;received.filename="recovery-copy.txt";received.payload=original.identity();received.scan.verdict=app::FileScanVerdict::not_scanned;received.transfer_complete=true;check(db.record_file_intake(caller,received).succeeded()&&db.prepare_file_intake(caller,recovery,{"Recovery copy",""},false).succeeded(),"actual receive identity/metadata prepared for copied placement");
            // Two genuine independent C: objects exercise copied-placement
            // recovery without mounting/faking an alternate volume.
            auto final=general.create(received.filename,32);check(db.bind_file_intake_object(caller,recovery,true,final.object_identity()).succeeded(),"actual distinct destination object durably bound");const auto bytes=original.read(0);final.append(bytes.data(),bytes.size());check(final.object_identity().file_id!=original.object_identity().file_id&&db.commit_file_intake(caller,recovery,final.identity()).succeeded(),"independent actual copy accepted; redundant original deliberately retained");
        }
        check(db.shutdown().succeeded()&&db.start().succeeded(),"copied acceptance persists through genuine DB restart");
        {auto final=general.read("recovery-copy.txt",true);final.remove_exact();}check(general.absent("recovery-copy.txt")&&!staging.absent(key),"only published test copy removed; recovery original still exists");
        const auto cleaned=native::cleanup_accepted_file_intake(db,owner,recovery,staging,root,sid);const bool retained=!staging.absent(key);std::cout<<"OBS accepted-copy cleanup="<<cleaned<<" final_absent="<<general.absent("recovery-copy.txt")<<" recovery_retained="<<retained<<std::endl;
        check(!cleaned&&retained&&!db.file_intake(owner,recovery).value->staging_clean,"cleanup MUST retain the sole recovery copy when final publication is missing");
    }
    {
        struct Copy {app::Identifier128 operation;app::FileId file;std::string key,name;};
        const auto make_copy=[&](std::string name){
            Copy copy{id(operation++),{},{},std::move(name)};const auto begun=db.begin_file_intake(caller,{copy.operation,{1},false,"2026-10-17"});check(begun&&begun.value,"copied recovery starts from supported durable intent");copy.key=begun.value->stage_key;
            auto original=staging.create(copy.key,32);check(db.bind_file_intake_object(caller,copy.operation,false,original.object_identity()).succeeded(),"copied recovery binds actual staging object");original.append(reinterpret_cast<const std::uint8_t*>("recovery"),8);
            app::FileIntakeReceived received;received.operation=copy.operation;received.filename=copy.name;received.payload=original.identity();received.scan.verdict=app::FileScanVerdict::not_scanned;received.transfer_complete=true;
            check(db.record_file_intake(caller,received).succeeded()&&db.prepare_file_intake(caller,copy.operation,{copy.name,""},false).succeeded(),"copied recovery records actual receive identity");
            auto final=general.create(copy.name,32);check(db.bind_file_intake_object(caller,copy.operation,true,final.object_identity()).succeeded(),"copied recovery binds independent final object");const auto bytes=original.read(0);final.append(bytes.data(),bytes.size());const auto accepted=db.commit_file_intake(caller,copy.operation,final.identity());check(accepted&&accepted.value,"copied recovery commits real copied object");copy.file=accepted.value->id;return copy;
        };
        const auto cleanup=[&](const Copy& copy,const native::WindowsFileManager::Progress& progress=native::WindowsFileManager::Progress{}){return native::cleanup_accepted_file_intake(db,owner,copy.operation,staging,root,sid,progress);};
        const auto counts_before=scalar(file,"SELECT upload_count FROM sfx_file_statistics WHERE account_id=2");
        const auto replaced=make_copy("replaced-copy.txt");
        {auto original_final=general.read(replaced.name,true);original_final.rename_to(general,"replaced-copy-original.txt");auto replacement=general.create(replaced.name,32);replacement.append(reinterpret_cast<const std::uint8_t*>("recovery"),8);check(replacement.identity().file_id!=original_final.identity().file_id,"replacement has same bytes but distinct actual NTFS identity");}
        check(!cleanup(replaced)&&!staging.absent(replaced.key)&&!db.file_intake(owner,replaced.operation).value->staging_clean,"same-content replacement final cannot authorize recovery deletion");
        const auto changed=make_copy("changed-copy.txt");const auto old_identity=db.managed_file(owner,changed.file).value->payload;
        {ins::Handle writable(CreateFileW((general.path()/std::filesystem::u8path(changed.name)).c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD written{};check(writable.get()!=INVALID_HANDLE_VALUE&&WriteFile(writable.get(),"modified",8,&written,nullptr)&&written==8&&FlushFileBuffers(writable.get()),"change only exact test-owned final bytes with native write handle");}
        const auto changed_identity=general.read(changed.name).identity();check(changed_identity.file_id==old_identity.file_id&&changed_identity.sha256!=old_identity.sha256,"changed final retains native ID but content differs");
        check(!cleanup(changed)&&!staging.absent(changed.key),"changed current final preserves original recovery object");
        const auto valid=make_copy("valid-copy.txt");unsigned callbacks{};
        check(!cleanup(valid,[&](auto,auto){++callbacks;return false;})&&callbacks>0&&!staging.absent(valid.key),"cancelled verification keeps both copies and pending receipt");
        bool final_locked{};check(cleanup(valid,[&](auto,auto){ins::Handle mutation(CreateFileW((general.path()/std::filesystem::u8path(valid.name)).c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));if(mutation.get()==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION)final_locked=true;return true;})&&final_locked,"current final stays immutable throughout recovery hashing and stage deletion");
        check(staging.absent(valid.key)&&general.read(valid.name).identity().bytes==8&&cleanup(valid)&&db.file_intake(owner,valid.operation).value->staging_clean,"valid final permits exact once cleanup and stable replay");
        native::WindowsFileManager manager(db,owner,root,sid);
        const auto request=[&](app::FileId selected,app::FileOperationAction action,app::FileAreaId destination=app::FileAreaId{},std::string name=""){
            app::FileOperationRequest r;r.operation=id(operation++);r.file=selected;r.expected_revision=db.managed_file(owner,selected).value->file.revision;r.action=action;r.destination=destination;r.filename=std::move(name);if(destination.value){const auto cfg=db.file_configuration(owner);r.expected_destination_revision=std::find_if(cfg.value->areas.begin(),cfg.value->areas.end(),[&](const auto& a){return a.area.id==destination;})->area.revision;}return r;
        };
        const auto moved=make_copy("moved-copy.txt");check(manager.execute(request(moved.file,app::FileOperationAction::move,{2},"moved-final.txt")).completed,"supported manager moves accepted copy while recovery remains pending");
        check(cleanup(moved)&&staging.absent(moved.key)&&uploads.read("moved-final.txt").identity().bytes==8,"cleanup resolves current moved friendly name instead of stale original destination");
        const auto removed=make_copy("removed-copy.txt");check(manager.execute(request(removed.file,app::FileOperationAction::remove)).completed,"supported recoverable removal with pending accepted recovery");
        check(!manager.execute(request(removed.file,app::FileOperationAction::purge)).completed&&!staging.absent(removed.key),"purge must wait for explicit accepted staging reconciliation");
        const auto current_removed=db.managed_file(owner,removed.file);auto removed_store=native::FilePayloadDirectory::private_store(root/L"Data"/L"FileRemoved",sid);
        check(cleanup(removed)&&staging.absent(removed.key)&&removed_store.read(current_removed.value->removed_key).identity().bytes==8,"cleanup verifies current recoverable removed copy before deleting redundant stage");
        check(manager.execute(request(removed.file,app::FileOperationAction::purge)).completed,"explicit library purge becomes available only after staging receipt is complete");
        check(scalar(file,"SELECT upload_count FROM sfx_file_statistics WHERE account_id=2")==counts_before+5,"recovery/move/removal/purge never fabricate transfer credits");
    }
    check(db.shutdown().succeeded(),"normal storage shutdown");
    std::cout<<"PASS native file intake "<<checks<<" checks; actual NTFS/SQLite, synthetic actors and scanner/fault outcomes; no installed transport/real malware claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native file intake after "<<checks<<": "<<e.what()<<" win32="<<GetLastError()<<'\n';return 1;}}
