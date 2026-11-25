#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include "sqlite_sfx_baseline.hpp"
#include <future>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace storage=sfx::adapters::storage;
namespace internal=storage::internal;
namespace {
unsigned checks{};
void check(bool c,const char* why){++checks;if(!c)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 v;v.bytes[0]=9;v.bytes[12]=static_cast<std::uint8_t>(n>>24);v.bytes[13]=static_cast<std::uint8_t>(n>>16);v.bytes[14]=static_cast<std::uint8_t>(n>>8);v.bytes[15]=static_cast<std::uint8_t>(n);return v;}
void seed(const std::filesystem::path& file){
    internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
    c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000),(3,'Bob','bob',10,'active',1,1,1700000000,1700000000)","owned synthetic file actors");
    c.exec("INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","complete synthetic fixture");
    for(unsigned n=1;n<=3;++n){internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1700000000,0,1700000000,NULL,NULL,?1)","trusted test sessions; not authentication evidence");q.bind(1,id(n));q.bind(2,static_cast<std::int64_t>(n));q.step();}
}
std::int64_t scalar(const std::filesystem::path& file,const char* sql){internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return c.scalar_int64(sql,"owned file fixture invariant");}
storage::SqliteStorageSettings settings(const std::filesystem::path& root){std::filesystem::create_directories(root);storage::SqliteStorageSettings s;s.data_directory=root;s.utc_now=[]{return std::int64_t{1792248000};};return s;}
#include "file_first_run_defaults.inc"
void migration_faults(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(const char* point:{"before-file-migration","before-file-migration-commit","after-file-migration-commit"}){
        auto s=settings(root/point);auto predecessor=storage::sfx_storage_migrations();predecessor.resize(14);
        {storage::SqliteStorage old(s,paths,predecessor);check(old.start().succeeded() && old.health().schema_version==14,"exact production v14 predecessor");check(old.shutdown().succeeded(),"v14 stopped before synthetic accounts");}
        seed(s.data_directory/"SFX.DB");
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected file migration failure"}:app::StorageStatus::success();};
        {storage::SqliteStorage failed(s,paths);check(!failed.start(),"file migration injected failure reported");}
        {internal::SqliteConnection c(s.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"failed migration leaves exact old/new schema");check(c.scalar_int64("PRAGMA user_version","atomic file migration")== (std::string_view(point)=="after-file-migration-commit"?15:14),"file migration commit boundary atomic");}
        s.bootstrap_boundary_hook={};auto file_migrations=storage::sfx_storage_migrations();file_migrations.resize(15);storage::SqliteStorage retry(s,paths,file_migrations);const auto started=retry.start();if(!started)std::cerr<<"migration retry "<<started.operation<<" code="<<static_cast<unsigned>(started.code)<<'\n';
        check(started.succeeded() && retry.health().schema_version==15,"file migration retries once");check(retry.shutdown().succeeded(),"upgraded fixture stopped");
        check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3 && scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_schema_migrations")==15,"accounts retained and fifteen exact migrations");
    }
}
}
int main(int argc,char** argv){try{
    check(argc==2,"new isolated fixture root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"never reuse or overwrite fixture");
    sfx::test::FakeStoragePathPolicy paths;
    first_run_defaults(root/"first-run-defaults",paths);
    auto s=settings(root/"primary");storage::SqliteStorage db(s,paths);
    const auto started=db.start();if(!started)std::cerr<<"startup "<<started.operation<<" code="<<static_cast<unsigned>(started.code)<<'\n';
    check(started.succeeded() && db.health().schema_version==27,"fresh current schema27 includes Section14 file storage");check(db.shutdown().succeeded(),"stop for synthetic actors");
    const auto file=s.data_directory/"SFX.DB";seed(file);check(db.start().succeeded(),"populated fixture starts");
    const app::FileActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};const app::FileConfigurationActor owner{true,{}};
    auto cfg=db.file_configuration(owner);check(cfg && cfg.value->groups.size()==1 && cfg.value->areas.size()==3,"exact three starter areas");
    check(cfg.value->groups[0].name=="Main" && cfg.value->areas[0].area.name=="General Files" && cfg.value->areas[1].area.name=="Uploads" && cfg.value->areas[2].area.name=="Upload to Sysop","approved starter names");
    check(cfg.value->policy.maximum_upload_bytes==104857600 && !cfg.value->policy.counts.ratio && !cfg.value->policy.counts.daily_downloads,"approved global defaults without ratio");
    for(const auto& a:cfg.value->areas)check(!a.area.backup_contents && !a.area.own_description_edit,"all payload backups and own editing Off");
    check(!cfg.value->areas[0].area.permissions.uploads_enabled && cfg.value->areas[1].area.permissions.downloads_sysop_only && !cfg.value->areas[2].area.ratio_uploads,"ordinary upload Off; review protected; private ratio excluded");
    const auto catalog=db.file_catalog(alice),privileged=db.file_catalog(sysop);check(catalog && catalog.value->areas.size()==2 && privileged && privileged.value->areas.size()==3,"caller catalog omits private area");
    check(!db.file_configuration({false,alice}) && !db.file_configuration({true,sysop}),"ordinary/mixed native authority refused");
    auto forged=bob;forged.session=alice.session;check(!db.file_catalog(forged) && !db.file_caller_statistics(forged,"2026-10-17"),"cross-account session refused");
    auto state=db.file_caller_state(alice);check(state && state.value->since_utc==1700000000 && state.value->last_seen_publication==0,"new scan starts at join date, not first-use date");
    auto desired=*state.value;desired.current_area={3};check(!db.save_file_caller_state(alice,desired),"private area cannot become ordinary current area");
    desired.current_area={1};auto saved=db.save_file_caller_state(alice,desired);check(saved && saved.value->revision==2 && !db.save_file_caller_state(alice,desired),"saved current area with revision CAS");
    unsigned op=100;
    const auto import=[&](std::int64_t area,std::string name,unsigned content,std::uint64_t bytes=50){
        const auto configuration=db.file_configuration(owner);const auto configured=std::find_if(configuration.value->areas.begin(),configuration.value->areas.end(),[area](const auto& value){return value.area.id.value==area;});if(configured==configuration.value->areas.end())throw std::runtime_error("import fixture area missing");
        app::FileRegistration r;r.operation=id(op++);r.area={area};r.expected_area_revision=configured->area.revision;r.filename=std::move(name);r.descriptions={"Short description","Long description with MAGIC marker"};r.public_uploader="Sysop";r.payload={bytes,{},"synthetic-volume","synthetic-"+std::to_string(content)};r.payload.sha256[0]=static_cast<std::uint8_t>(content);r.original_upload_utc=1600000000;return r;
    };
    auto public_request=import(1,"Example.ZIP",1,104857601);auto public_file=db.register_file(owner,public_request);
    if(!public_file)std::cerr<<"registration "<<public_file.status.operation<<" code="<<static_cast<unsigned>(public_file.status.code)<<'\n';
    check(public_file && public_file.value->bytes==104857601,"Sysop import not caller-capped; synthetic payload identity only");
    const auto replay=db.register_file(owner,public_request);check(replay && replay.value->id==public_file.value->id && scalar(file,"SELECT count(*) FROM sfx_file_catalog")==1,"register idempotent with same operation");
    auto altered=public_request;altered.filename="changed.zip";check(!db.register_file(owner,altered),"changed operation collision refused");
    auto dupe=import(1,"duplicate.zip",1);dupe.payload.bytes=public_request.payload.bytes;check(!db.register_file(owner,dupe),"duplicate content warns before registration");dupe.allow_duplicate_content=true;
    check(!db.register_file(owner,dupe),"duplicate override cannot alias an already registered native object");
    dupe.payload.file_id="different-native-file-same-content";
    auto duplicate=db.register_file(owner,dupe);check(duplicate && !(duplicate.value->id==public_file.value->id),"explicit Sysop duplicate override retains both independent entries");
    auto collision=import(1,"EXAMPLE.zip",2);collision.allow_duplicate_content=true;check(!db.register_file(owner,collision),"duplicate override never bypasses filename collision");
    check(!db.register_file({false,alice},import(1,"forged.zip",3)),"caller cannot invoke native import");
    auto private_file=db.register_file(owner,import(3,"private-secret.zip",4));auto review_file=db.register_file(owner,import(2,"review.zip",5));
    check(private_file && review_file && !db.file_information(alice,private_file.value->id) && db.file_information(sysop,private_file.value->id).succeeded(),"private filename/metadata only Sysop");
    check(!db.reserve_file_download(alice,id(op++),review_file.value->id,"2026-10-17"),"review files cannot be downloaded by ordinary caller");
    check(scalar(file,"SELECT count(*) FROM sfx_file_statistics")==0 && scalar(file,"SELECT count(*) FROM sfx_file_transfers")==0,"imports fabricate no caller upload/download or ratio counters");
    app::FileQuery query;query.scope=app::FileSearchScope::all;
    auto page=db.file_page(alice,query,{},2);check(page && page.value->entries.size()==2 && page.value->next,"bounded first visible page");
    query.publication_fence=page.value->publication_fence;auto page2=db.file_page(alice,query,*page.value->next,2);check(page2 && page2.value->entries.size()==1 && !page2.value->next,"keyset continuation has only visible files");
    query.search=app::FileSearchKind::filename;query.query="private*";check(db.file_page(alice,query,{},100).value->entries.empty(),"global filename search hides private");
    query.search=app::FileSearchKind::descriptions;query.query="MAGIC;nonexistent";check(db.file_page(alice,query,{},100).value->entries.size()==3,"OR description search does not expose private");
    query.search.reset();query.downloadable_only=true;check(db.file_page(alice,query,{},100).value->entries.size()==2,"download search excludes review area");
    query.downloadable_only=false;query.new_only=true;query.since_utc=1700000000;check(db.file_page(alice,query,{},100).value->entries.size()==3,"new scan uses publication not older upload date");
    query.after_publication=query.publication_fence;check(db.file_page(alice,query,{},100).value->entries.empty(),"seen publication fence excludes already delivered scan results");
    query={};query.scope=app::FileSearchScope::area;query.area={3};check(!db.file_page(alice,query,{},100),"guessed private area ID refused");
    check(!db.edit_own_file_description(alice,id(op++),public_file.value->id,1,{"changed",""}),"ordinary caller cannot edit imported files");
    app::FileConfigurationChange change;change.operation=id(op++);change.expected_catalog_revision=cfg.value->policy.catalog_revision;change.policy=cfg.value->policy;change.policy.counts.daily_downloads=1;change.policy.counts.ratio=app::FileRatio{};
    check(!db.change_file_configuration(owner,change),"enabling empty ratio invalid");change.policy.counts.ratio=app::FileRatio{10,1};
    auto changed=db.change_file_configuration(owner,change);
    if(!changed)std::cerr<<"policy change "<<changed.status.operation<<" code="<<static_cast<unsigned>(changed.status.code)<<'\n';
    check(changed.succeeded(),"explicit count policy commits");
    const auto policy_replay=db.change_file_configuration(owner,change);
    check(policy_replay && policy_replay.value->replayed,"explicit count policy idempotent receipt");
    check(scalar(file,"SELECT count(*) FROM sfx_file_audit WHERE event_kind='configuration.changed' AND typeof(detail)='text' AND length(detail)=0")==1,"omitted audit detail is empty TEXT, not SQL NULL");
    check(!db.change_file_configuration({false,alice},change),"caller cannot change limits");
    const auto first=id(op++),second=id(op++);
    auto one=std::async(std::launch::async,[&]{return db.reserve_file_download(alice,first,public_file.value->id,"2026-10-17");});
    auto two=std::async(std::launch::async,[&]{return db.reserve_file_download(alice,second,public_file.value->id,"2026-10-17");});
    auto r1=one.get(),r2=two.get();check(r1 && r2 && static_cast<bool>(r1.value->reservation)!=static_cast<bool>(r2.value->reservation),"serialized daily allowance permits only one concurrent reservation");
    const auto reservation=r1.value->reservation?*r1.value->reservation:*r2.value->reservation;
    auto stats=db.file_caller_statistics(alice,"2026-10-17");check(stats && stats.value->counts.successful_downloads==0 && stats.value->counts.reserved_downloads==1,"reservation does not report transfer success");
    auto failed=db.finish_file_download(alice,reservation.operation,{},false);check(failed && !failed.value->completed,"failed transfer releases allowance");
    stats=db.file_caller_statistics(alice,"2026-10-17");check(stats && stats.value->counts.successful_downloads==0 && stats.value->counts.reserved_downloads==0,"failure changes no counters");
    auto download=db.reserve_file_download(alice,id(op++),public_file.value->id,"2026-10-17");check(download && download.value->reservation,"slot available after failure");
    auto wrong=download.value->reservation->payload;wrong.bytes++;
    check(!db.finish_file_download(alice,download.value->reservation->operation,wrong,true),"wrong completion payload cannot charge");
    const auto completed_download=db.finish_file_download(alice,download.value->reservation->operation,download.value->reservation->payload,true);
    check(completed_download && completed_download.value->completed && !completed_download.value->replayed,"exact successful transfer committed once");
    const auto repeated=db.finish_file_download(alice,download.value->reservation->operation,download.value->reservation->payload,true);check(repeated && repeated.value->replayed,"completion replay no double count");
    check(!db.finish_file_download(bob,download.value->reservation->operation,download.value->reservation->payload,true),"cross-caller receipt denied");
    stats=db.file_caller_statistics(alice,"2026-10-17");check(stats && stats.value->counts.successful_downloads==1 && stats.value->counts.eligible_downloads==1 && stats.value->counts.today_downloads==1 && stats.value->download_bytes==104857601 && stats.value->counts.reserved_downloads==0,"exact count/bytes/day accounting");
    check(db.file_information(alice,public_file.value->id).value->downloads==1,"file popularity counted once");
    auto denied=db.reserve_file_download(alice,id(op++),public_file.value->id,"2026-10-17");check(denied && denied.value->allowance==app::FileDownloadAllowance::daily_limit && !denied.value->reservation,"clear daily refusal only");
    check(scalar(file,"SELECT security_level FROM sfx_accounts WHERE account_id=2")==10,"allowance refusal does not mutate security");
    check(db.shutdown().succeeded() && db.start().succeeded(),"file state restart");stats=db.file_caller_statistics(alice,"2026-10-18");check(stats && stats.value->counts.today_downloads==0 && stats.value->counts.eligible_downloads==1 && stats.value->counts.successful_downloads==1,"daily boundary does not renew starter or lifetime counts");
    check(db.finish_file_download(alice,download.value->reservation->operation,download.value->reservation->payload,true).value->replayed,"receipt survives restart");
    cfg=db.file_configuration(owner);change={};change.operation=id(op++);change.action=app::FileConfigurationAction::area;change.expected_catalog_revision=cfg.value->policy.catalog_revision;change.area=cfg.value->areas[0];change.area.storage_path="Files/Moved";
    check(!db.change_file_configuration(owner,change),"nonempty path change never silently relocates catalog");
    change.area.storage_path=cfg.value->areas[0].storage_path;change.area.area.permissions.download=20;check(db.change_file_configuration(owner,change).succeeded(),"independent live download threshold");
    check(!db.reserve_file_download(alice,id(op++),public_file.value->id,"2026-10-18") && db.file_information(alice,public_file.value->id).succeeded(),"revoked download denied while independent list/info remains");
    const auto receive=[&](const app::FileActor& who,app::FileAreaId area,bool private_upload,std::string name,app::FileScanVerdict verdict=app::FileScanVerdict::not_scanned,std::uint64_t bytes=12){
        app::FileIntakeStart start{id(op++),area,private_upload,"2026-10-17"};const auto begun=db.begin_file_intake(who,start);
        check(begun && begun.value->state==app::FileIntakeState::receiving && begun.value->stage_key.size()==32,"journal before native receive storage");
        check(db.begin_file_intake(who,start).value->stage_key==begun.value->stage_key,"start replay retains same private stage");
        app::FileIntakeReceived r;r.operation=start.operation;r.filename=std::move(name);r.payload={bytes,{},"synthetic-stage-volume","stage-"+std::to_string(op)};r.payload.sha256[0]=77;r.payload.sha256[1]=static_cast<std::uint8_t>(op);r.scan.verdict=verdict;r.transfer_complete=true;
        if(verdict==app::FileScanVerdict::detected)r.scan.detections={"Synthetic.Positive.Signature"};
        check(!db.record_file_intake(who,r),"receive receipt cannot invent an unbound staging object");
        check(db.bind_file_intake_object(who,r.operation,false,{r.payload.volume,r.payload.file_id}).succeeded(),"trusted staging identity checkpoint");
        check(!db.bind_file_intake_object(who,r.operation,false,{r.payload.volume,"replacement"}),"staging checkpoint immutable");
        const auto recorded=db.record_file_intake(who,r);if(!recorded)std::cerr<<"record upload "<<recorded.status.operation<<" code="<<static_cast<unsigned>(recorded.status.code)<<'\n';
        check(recorded.succeeded(),"retain scan outcome before any catalog acceptance");return r;
    };
    check(!db.begin_file_intake(alice,{id(op++),{1},false,"2026-10-17"}),"ordinary starter area upload disabled");
    check(!db.begin_file_intake(alice,{id(op++),{3},false,"2026-10-17"}),"private requires dedicated submission route");
    check(!db.begin_file_intake(alice,{id(op++),{2},true,"2026-10-17"}),"private route cannot silently retarget ordinary queue");
    auto incoming=receive(alice,{2},false,"caller-upload.zip");
    check(!db.pending_file_intake({false,alice},256) && db.pending_file_intake(owner,256).value->size()==1,"pending intake Sysop-only");
    check(!db.record_file_intake(bob,incoming),"cross-caller incoming payload refused");
    check(!db.commit_file_intake(alice,incoming.operation,incoming.payload),"scan/receive success alone cannot publish");
    auto prepared=db.prepare_file_intake(alice,incoming.operation,{"Caller description","Extended text"},false);
    check(prepared && prepared.value->state==app::FileIntakeState::prepared,"metadata review reserves filename");
    check(db.prepare_file_intake(alice,incoming.operation,{"Caller description","Extended text"},false).succeeded() && !db.prepare_file_intake(alice,incoming.operation,{"Changed",""},false),"placement metadata exact replay");
    check(!db.register_file(owner,import(2,"caller-upload.zip",66)),"Sysop import cannot race pending placement");
    auto wrong_upload=incoming.payload;wrong_upload.bytes++;
    check(!db.commit_file_intake(alice,incoming.operation,wrong_upload),"wrong placed content cannot commit");
    auto placed=incoming.payload;placed.volume="synthetic-destination-volume";placed.file_id="accepted-upload-1";
    check(!db.commit_file_intake(alice,incoming.operation,placed),"content equality alone cannot bind an uncheckpointed destination");
    check(!db.bind_file_intake_object(bob,incoming.operation,true,{placed.volume,placed.file_id})&&db.bind_file_intake_object(alice,incoming.operation,true,{placed.volume,placed.file_id}).succeeded(),"only same live caller host binds prepared destination");
    auto substituted=placed;substituted.file_id="unrelated-same-content";
    check(!db.commit_file_intake(alice,incoming.operation,substituted),"same bytes at different native destination cannot commit");
    auto accepted=db.commit_file_intake(alice,incoming.operation,placed);
    if(!accepted)std::cerr<<"commit upload "<<accepted.status.operation<<" code="<<static_cast<unsigned>(accepted.status.code)<<'\n';
    check(accepted && accepted.value->uploader==2 && accepted.value->public_uploader=="Alice" && accepted.value->origin==app::FileOrigin::caller_upload,"accepted upload retains genuine public attribution");
    check(db.commit_file_intake(alice,incoming.operation,placed).value->id==accepted.value->id,"accepted upload replay no double accounting");
    stats=db.file_caller_statistics(alice,"2026-10-17");check(stats && stats.value->counts.successful_uploads==1 && stats.value->counts.eligible_uploads==1 && stats.value->upload_bytes==12,"only committed eligible upload credited");
    check(!db.reserve_file_download(alice,id(op++),accepted.value->id,"2026-10-18"),"new Uploads file remains Sysop-download only");
    check(!db.resolve_file_intake(owner,incoming.operation,placed,true,"not allowed"),"accepted library file excluded from detection deletion");
    auto private_incoming=receive(alice,{3},true,"private-caller.zip");
    const auto private_prepared=db.prepare_file_intake(alice,private_incoming.operation,{"Private note",""},false);
    check(private_prepared.succeeded()&&private_prepared.value,"private submission metadata without private area browsing");
    check(private_prepared.value->placement_name.empty()&&app::file_intake_payload_name(*private_prepared.value)=="private-caller.zip","new private final placements use original filenames, not operation IDs");
    check(db.bind_file_intake_object(alice,private_incoming.operation,true,{private_incoming.payload.volume,private_incoming.payload.file_id}).succeeded(),"same-volume placement binds original object before rename");
    auto private_accepted=db.commit_file_intake(alice,private_incoming.operation,private_incoming.payload);
    check(private_accepted && !db.file_information(alice,private_accepted.value->id) && db.file_information(sysop,private_accepted.value->id).succeeded(),"accepted private upload stays invisible to ordinary uploader");
    check(!db.file_intake({false,alice},private_incoming.operation)&&db.file_intake(owner,private_incoming.operation).value->placed_object.has_value(),"native binding details Sysop-only");
    check(!db.finish_file_intake_cleanup({false,alice},private_incoming.operation,{private_incoming.payload.volume,private_incoming.payload.file_id})&&!db.finish_file_intake_cleanup(owner,private_incoming.operation,{"wrong","wrong"}),"cleanup authority and native identity rechecked");
    check(db.finish_file_intake_cleanup(owner,private_incoming.operation,{private_incoming.payload.volume,private_incoming.payload.file_id}).succeeded()&&db.finish_file_intake_cleanup(owner,private_incoming.operation,{private_incoming.payload.volume,private_incoming.payload.file_id}).succeeded(),"trusted absent-staging receipt replay does not change acceptance");
    stats=db.file_caller_statistics(alice,"2026-10-17");check(stats.value->counts.successful_uploads==2 && stats.value->counts.eligible_uploads==1 && stats.value->upload_bytes==24,"private upload statistics yes ratio excluded");
    auto oversize=receive(alice,{2},false,"oversize.zip",app::FileScanVerdict::not_scanned,104857601);
    check(db.record_file_intake(alice,oversize).value->state==app::FileIntakeState::held && !db.prepare_file_intake(alice,oversize.operation,{"Too large",""},false),"actual byte overflow held without indexing");
    check(!db.resolve_file_intake(owner,oversize.operation,oversize.payload,true,"cannot delete"),"oversize is not positive malware");
    cfg=db.file_configuration(owner);change={};change.operation=id(op++);change.expected_catalog_revision=cfg.value->policy.catalog_revision;change.policy=cfg.value->policy;change.policy.scan_uploads=true;
    check(db.change_file_configuration(owner,change).succeeded(),"enable independent caller scanning for outcome tests");
    for(auto verdict:{app::FileScanVerdict::error,app::FileScanVerdict::timeout,app::FileScanVerdict::incomplete,app::FileScanVerdict::not_scanned}){
        auto held=receive(alice,{2},false,"held-"+std::to_string(op)+".zip",verdict);
        check(db.record_file_intake(alice,held).value->state==app::FileIntakeState::held && !db.prepare_file_intake(alice,held.operation,{"Held",""},false),"nonclean required scan is durable hold");
        check(!db.resolve_file_intake(owner,held.operation,held.payload,true,"false deleted") && db.resolve_file_intake(owner,held.operation,held.payload,false,"Retained for review").succeeded(),"scan failure never authorizes deletion");
    }
    auto malware=receive(alice,{2},false,"synthetic-detection.txt",app::FileScanVerdict::detected);
    check(!db.prepare_file_intake(alice,malware.operation,{"Blocked",""},false),"synthetic detection cannot be accepted");
    auto not_exact=malware.payload;not_exact.file_id="unrelated";
    check(!db.resolve_file_intake(owner,malware.operation,not_exact,true,"wrong file"),"positive detection binds exact staged object");
    check(db.resolve_file_intake(owner,malware.operation,malware.payload,false,"Deletion failed; retained privately").succeeded(),"failed deletion retains positive detection hold");
    check(db.resolve_file_intake(owner,malware.operation,malware.payload,true,"Native exact deletion confirmed").succeeded() && db.resolve_file_intake(owner,malware.operation,malware.payload,true,"Replay").succeeded(),"injected native deletion receipt idempotent; no actual filesystem claim");
    auto clean_incoming=receive(bob,{2},false,"clean-review.zip",app::FileScanVerdict::clean);
    check(db.prepare_file_intake(bob,clean_incoming.operation,{"Clean",""},false).succeeded(),"clean ready for validated placement");
    const auto uploads_before=scalar(file,"SELECT sum(upload_count) FROM sfx_file_statistics");
    check(db.shutdown().succeeded(),"stop to simulate ended interrupted session");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec("UPDATE sfx_account_sessions SET completed_utc_seconds=1792248001,outcome_code='disconnected' WHERE account_id=3","synthetic ended session for recovery");}
    check(db.start().succeeded() && db.recover_file_intake(owner).succeeded(),"restart holds interrupted prepared upload");
    auto held_rows=db.pending_file_intake(owner,256);check(held_rows.succeeded(),"owner recovery inspection");
    const auto recovered=std::find_if(held_rows.value->begin(),held_rows.value->end(),[&](const auto& row){return row.operation==clean_incoming.operation;});
    check(recovered!=held_rows.value->end() && recovered->state==app::FileIntakeState::held && !db.commit_file_intake(bob,clean_incoming.operation,clean_incoming.payload),"dead actor cannot publish after recovery");
    check(scalar(file,"SELECT sum(upload_count) FROM sfx_file_statistics")==uploads_before && scalar(file,"SELECT security_level FROM sfx_accounts WHERE account_id=2")==10,"holds/recovery do not credit or alter security");
    check(!db.register_file(owner,import(2,"clean-review.zip",201)),"Find Unregistered cannot bypass an interrupted private intake hold");
    auto managed_source=db.register_file(owner,import(2,"manager-source.txt",202));check(managed_source.succeeded(),"independent File Manager source fixture");
    const auto manager_id=managed_source.value->id;
    const auto request=[&](app::FileId target,app::FileOperationAction action,app::FileAreaId destination=app::FileAreaId{},std::string filename=""){
        app::FileOperationRequest r;r.operation=id(op++);r.action=action;r.file=target;r.expected_revision=db.managed_file(owner,target).value->file.revision;r.destination=destination;r.filename=std::move(filename);
        if(destination.value){const auto current=db.file_configuration(owner);const auto found=std::find_if(current.value->areas.begin(),current.value->areas.end(),[&](const auto& area){return area.area.id==destination;});r.expected_destination_revision=found->area.revision;}return r;
    };
    auto metadata=request(manager_id,app::FileOperationAction::metadata);metadata.descriptions={"Corrected text","Long corrected description"};metadata.public_uploader="Public Archivist";
    check(!db.managed_file({false,alice},manager_id)&&!db.managed_file_page({false,alice},{},{},10)&&!db.prepare_file_operation({false,alice},metadata),"all File Manager interfaces require current Sysop/native authority");
    const auto before_manager_stats=scalar(file,"SELECT sum(upload_count+download_count) FROM sfx_file_statistics");
    auto planned=db.prepare_file_operation(owner,metadata);if(!planned)std::cerr<<"prepare metadata "<<planned.status.operation<<" code="<<static_cast<unsigned>(planned.status.code)<<'\n';
    check(planned&&planned.value->state==app::FileOperationState::prepared&&!db.file_information(alice,manager_id),"pending native mutation hidden from callers and blocks readers");
    check(db.prepare_file_operation(owner,metadata).succeeded(),"exact prepare replay does not duplicate intent");
    auto competing=metadata;competing.operation=id(op++);check(!db.prepare_file_operation(owner,competing),"second concurrent operation cannot reserve same file");
    app::FileOperationCompletion completed{planned.value->source.payload,planned.value->source.payload,false,true};auto forged_completion=completed;forged_completion.source.file_id="wrong";
    check(!db.commit_file_operation(owner,metadata.operation,forged_completion),"native operation requires exact held source identity");
    check(db.commit_file_operation(owner,metadata.operation,completed).succeeded()&&db.commit_file_operation(owner,metadata.operation,completed).succeeded(),"metadata commit and exact terminal replay");
    check(!db.commit_file_operation(owner,metadata.operation,forged_completion),"completed receipt refuses different native evidence");
    auto managed=db.managed_file(owner,manager_id);check(managed&&managed.value->file.public_uploader=="Public Archivist"&&managed.value->file.descriptions.short_text=="Corrected text"&&!managed.value->operation_pending,"public attribution correction logged without changing payload");
    app::FileManagementQuery manager_query;manager_query.field=app::FileManagementSearch::uploader;manager_query.search="Archivist";
    auto managed_page=db.managed_file_page(owner,manager_query,{},1);check(managed_page&&managed_page.value->files.size()==1&&managed_page.value->files[0].file.id==manager_id,"bounded public-uploader search");
    auto copy=request(manager_id,app::FileOperationAction::copy,{1},"manager-copy.txt");check(!db.prepare_file_operation(owner,copy),"copy requires explicit duplicate content warning override");copy.allow_duplicate_content=true;
    planned=db.prepare_file_operation(owner,copy);check(planned.succeeded(),"explicit independent copy intent");completed={planned.value->source.payload,planned.value->source.payload,false,true};
    check(!db.commit_file_operation(owner,copy.operation,completed),"copy cannot share same native object");completed.destination.file_id="independent-copy-object";
    check(!db.commit_file_operation(owner,copy.operation,completed),"unbound different object cannot be claimed as operation destination");
    check(db.bind_file_operation_destination(owner,copy.operation,{completed.destination.volume,completed.destination.file_id}).succeeded(),"bind independent copy before its native receipt");
    check(!db.bind_file_operation_destination(owner,copy.operation,{completed.destination.volume,"replacement"}),"destination object binding immutable");
    const auto copied=db.commit_file_operation(owner,copy.operation,completed);check(copied&&!(copied.value->result==manager_id),"copy creates new stable catalog identity without caller credits");
    auto rename=request(copied.value->result,app::FileOperationAction::rename,{1},"renamed-copy.txt");planned=db.prepare_file_operation(owner,rename);check(planned.succeeded(),"reserve safe rename");completed={planned.value->source.payload,planned.value->source.payload,true,true};
    check(db.commit_file_operation(owner,rename.operation,completed).succeeded()&&db.managed_file(owner,copied.value->result).value->file.name=="renamed-copy.txt","rename preserves stable ID");
    const auto before_move=db.managed_file(owner,manager_id).value->file;
    auto move=request(manager_id,app::FileOperationAction::move,{1},"reviewed.txt");planned=db.prepare_file_operation(owner,move);check(planned.succeeded(),"reserve review-area publication move");completed={planned.value->source.payload,planned.value->source.payload,true,true};
    check(db.commit_file_operation(owner,move.operation,completed).succeeded(),"commit final-area move");const auto moved=db.managed_file(owner,manager_id).value->file;
    check(moved.id==before_move.id&&moved.uploaded_utc==before_move.uploaded_utc&&moved.public_uploader==before_move.public_uploader&&moved.publication_sequence>before_move.publication_sequence,"final-area availability has new scan sequence and retained upload provenance");
    auto remove=request(manager_id,app::FileOperationAction::remove);planned=db.prepare_file_operation(owner,remove);check(planned.succeeded(),"prepare recoverable removal");completed={planned.value->source.payload,planned.value->source.payload,true,true};
    check(db.commit_file_operation(owner,remove.operation,completed).succeeded(),"record retained private removal, no purge");managed=db.managed_file(owner,manager_id);check(managed.value->file.state==app::FileCatalogState::removed&&!managed.value->purged&&managed.value->removed_key.size()==32&&!db.file_information(alice,manager_id),"recoverable removed payload has exact durable location key");
    auto restore=request(manager_id,app::FileOperationAction::restore,{1},"restored.txt");planned=db.prepare_file_operation(owner,restore);check(planned.succeeded(),"prepare explicit restore without overwrite");completed={planned.value->source.payload,planned.value->source.payload,true,true};
    check(db.commit_file_operation(owner,restore.operation,completed).succeeded()&&db.managed_file(owner,manager_id).value->removed_key.empty(),"restore retains file ID and clears only location index");
    remove=request(manager_id,app::FileOperationAction::remove);planned=db.prepare_file_operation(owner,remove);completed={planned.value->source.payload,planned.value->source.payload,true,true};check(db.commit_file_operation(owner,remove.operation,completed).succeeded(),"second explicit recoverable removal");
    auto purge=request(manager_id,app::FileOperationAction::purge);planned=db.prepare_file_operation(owner,purge);check(planned.succeeded(),"explicit purge requires removed payload state");completed={planned.value->source.payload,{},true,false};
    check(db.commit_file_operation(owner,purge.operation,completed).succeeded()&&db.managed_file(owner,manager_id).value->purged,"synthetic exact purge receipt retained; no physical deletion claim");
    restore=request(manager_id,app::FileOperationAction::restore,{1},"cannot-restore.txt");check(!db.prepare_file_operation(owner,restore),"purged entry cannot pretend payload is recoverable");
    auto missing=request(copied.value->result,app::FileOperationAction::mark_missing);planned=db.prepare_file_operation(owner,missing);completed={planned.value->source.payload,{},true,false};
    check(db.commit_file_operation(owner,missing.operation,completed).succeeded()&&!db.file_information(alice,copied.value->result),"verified missing record hidden from callers");
    restore=request(copied.value->result,app::FileOperationAction::restore,{1},"renamed-copy.txt");planned=db.prepare_file_operation(owner,restore);check(planned.succeeded(),"same-path missing file recovery preview");completed={planned.value->source.payload,planned.value->source.payload,false,true};
    check(db.commit_file_operation(owner,restore.operation,completed).succeeded(),"exact reappeared object restores visibility without moving it");
    auto held=request(copied.value->result,app::FileOperationAction::rename,{1},"held-rename.txt");planned=db.prepare_file_operation({false,sysop},held);check(planned.succeeded(),"caller Sysop native operation context");
    check(db.hold_file_operation(owner,held.operation,"Injected native failure; retain both paths for reconciliation").succeeded(),"installation owner may reconcile another authorized operator's held intent");
    const auto unresolved=db.pending_file_operations(owner,{},100);check(unresolved&&unresolved.value->size()==1&&unresolved.value->front().request.operation==held.operation,"bounded unresolved operations visible to authorized recovery host");
    check(!db.file_information(alice,copied.value->result)&&!db.register_file(owner,import(1,"held-rename.txt",203)),"held mutation protects source and destination against caller/import races");
    app::FileOperationCompletion cancellation{planned.value->source.payload,{},false,false};auto changed_cancel=cancellation;changed_cancel.source_absent=true;
    check(!db.cancel_file_operation(owner,held.operation,changed_cancel),"missing original cannot be cancelled as unchanged");
    check(db.cancel_file_operation(owner,held.operation,cancellation).succeeded()&&db.cancel_file_operation(owner,held.operation,cancellation).succeeded(),"verified unchanged cancellation has exact idempotent receipt");
    check(db.file_information(alice,copied.value->result).succeeded()&&!db.commit_file_operation(owner,held.operation,completed),"cancelled intent releases visibility and cannot later commit");
    check(db.register_file(owner,import(1,"held-rename.txt",203)).succeeded(),"same fresh-revision destination registers only after held reservation is released");
    std::vector<app::Identifier128> paged_intake;app::Identifier128 cursor{};
    for(;;){const auto intake_page=db.pending_file_intake(owner,2,cursor);check(intake_page.succeeded(),"bounded private intake cursor valid");if(intake_page.value->empty())break;for(const auto& row:*intake_page.value)paged_intake.push_back(row.operation);cursor=intake_page.value->back().operation;}
    check(paged_intake.size()==db.pending_file_intake(owner,256).value->size(),"pending intake pagination loses no holds");
    check(db.file_importer_name(owner).value==std::optional<std::string>{"Sysop"}&&!db.file_importer_name({false,alice}),"importer attribution exposes public Sysop name only to authorized operator");
    app::FileImportRequest import_intent{import(1,"reserved-import.txt",211),app::FileImportMode::copy,"C:/owned-fixture/external.txt"};
    check(!db.prepare_file_import({false,alice},import_intent)&&db.prepare_file_import(owner,import_intent).succeeded(),"private import preparation requires current authority");
    check(db.prepare_file_import(owner,import_intent).succeeded(),"exact native import preparation replay");auto altered_import=import_intent;altered_import.source_path="C:/different/source.txt";check(!db.prepare_file_import(owner,altered_import),"import operation cannot be reused for different source");
    auto pending_imports=db.pending_file_imports(owner,{},1);check(pending_imports&&pending_imports.value->size()==1&&!db.pending_file_imports({false,alice},{},1),"bounded pending import journal is not caller-visible");
    check(!db.register_file(owner,import_intent.registration)&&!db.register_file(owner,import(1,"reserved-import.txt",212)),"direct registration cannot bypass its own or another pending import");
    app::FileOperationCompletion import_proof{import_intent.registration.payload,import_intent.registration.payload,false,true};
    check(!db.commit_file_import(owner,import_intent.registration.operation,import_proof),"copy completion requires independent destination");import_proof.destination.file_id="new-import-object";
    check(!db.commit_file_import(owner,import_intent.registration.operation,import_proof),"unbound import destination rejected");
    check(db.bind_file_import_destination(owner,import_intent.registration.operation,{import_proof.destination.volume,import_proof.destination.file_id}).succeeded()&&!db.bind_file_import_destination(owner,import_intent.registration.operation,{"wrong-volume","wrong-object"}),"new import destination binding immutable");
    auto current_configuration=db.file_configuration(owner);app::FileConfigurationChange renamed_area;renamed_area.operation=id(op++);renamed_area.action=app::FileConfigurationAction::area;renamed_area.expected_catalog_revision=current_configuration.value->policy.catalog_revision;renamed_area.area=current_configuration.value->areas[0];renamed_area.area.area.name="Renamed General";
    check(db.change_file_configuration(owner,renamed_area).succeeded(),"area display metadata may change during prepared import");
    auto moved_path=renamed_area;current_configuration=db.file_configuration(owner);moved_path.operation=id(op++);moved_path.expected_catalog_revision=current_configuration.value->policy.catalog_revision;moved_path.area=current_configuration.value->areas[0];moved_path.area.storage_path="Files/DifferentPhysicalPath";
    check(!db.change_file_configuration(owner,moved_path),"pending import and catalog protect physical path binding");
    check(db.commit_file_import(owner,import_intent.registration.operation,import_proof).succeeded()&&db.commit_file_import(owner,import_intent.registration.operation,import_proof).succeeded(),"native import commits after harmless label revision and replays exactly");
    auto forged_import=import_proof;forged_import.destination.file_id="other-object";check(!db.commit_file_import(owner,import_intent.registration.operation,forged_import),"terminal import rejects altered native proof");
    check(!db.cancel_file_import(owner,import_intent.registration.operation,{import_proof.source,{},false,false}),"accepted import cannot be cancelled");
    app::FileImportRequest cancelled_import{import(1,"cancelled-import.txt",213),app::FileImportMode::move,"C:/owned-fixture/move.txt"};cancelled_import.registration.expected_area_revision=current_configuration.value->areas[0].area.revision;
    check(db.prepare_file_import(owner,cancelled_import).succeeded(),"prepare separate cancellable import");
    app::FileOperationCompletion cancellation_proof{cancelled_import.registration.payload,{},false,false};auto absent_cancel=cancellation_proof;absent_cancel.source_absent=true;
    check(!db.cancel_file_import(owner,cancelled_import.registration.operation,absent_cancel)&&db.cancel_file_import(owner,cancelled_import.registration.operation,cancellation_proof).succeeded()&&db.cancel_file_import(owner,cancelled_import.registration.operation,cancellation_proof).succeeded(),"import cancellation requires unchanged present source with exact replay");
    auto native_move=request(copied.value->result,app::FileOperationAction::move,{1},"revised-area-move.txt");const auto native_plan=db.prepare_file_operation(owner,native_move);check(native_plan.succeeded(),"prepare maintenance intent before another area label revision");
    current_configuration=db.file_configuration(owner);renamed_area.operation=id(op++);renamed_area.expected_catalog_revision=current_configuration.value->policy.catalog_revision;renamed_area.area=current_configuration.value->areas[0];renamed_area.area.area.name="General Final";check(db.change_file_configuration(owner,renamed_area).succeeded(),"edit area metadata while native move prepared");
    check(db.commit_file_operation(owner,native_move.operation,{native_plan.value->source.payload,native_plan.value->source.payload,true,true}).succeeded(),"performed move not stranded by unrelated area revision");
    check(scalar(file,"SELECT sum(upload_count+download_count) FROM sfx_file_statistics")==before_manager_stats,"all File Manager mutations fabricate no caller transfer accounting");
#include "file_scan_state_checks.inc"
    auto unicode_import=import(1,u8"\u00c9COLE-\u03a3-\U00010400.zip",245);unicode_import.descriptions={u8"\u00c9cole fran\u00e7aise",u8"\u03a3ample extended"};unicode_import.public_uploader=u8"Ren\u00e9";
    unicode_import.expected_area_revision=db.file_configuration(owner).value->areas[0].area.revision;
    const auto unicode_file=db.register_file(owner,unicode_import);check(unicode_file.succeeded(),"UTF-8 catalog retains display filename");
    auto unicode_collision=unicode_import;unicode_collision.operation=id(op++);unicode_collision.filename=u8"\u00e9cole-\u03c2-\U00010428.ZIP";unicode_collision.payload.file_id="different-unicode-native";unicode_collision.allow_duplicate_content=true;
    check(!db.register_file(owner,unicode_collision),"Unicode case-equivalent catalog name cannot overwrite even with duplicate-content override");
    app::FileQuery unicode_query;unicode_query.area={1};unicode_query.search=app::FileSearchKind::filename;unicode_query.query=u8"\u00e9cole-?-?.ZIP";
    const auto unicode_page=db.file_page(alice,unicode_query,{},100);
    check(unicode_page&&unicode_page.value->entries.size()==1&&unicode_page.value->entries[0].id==unicode_file.value->id&&unicode_page.value->entries[0].name==unicode_import.filename,"SQLite filename fold and scalar wildcard preserve original display");
    unicode_query.search=app::FileSearchKind::descriptions;unicode_query.query=u8"fran\u00e7aise;\u00e9COLE";
    check(db.file_page(alice,unicode_query,{},100).value->entries.size()==1,"SQLite Unicode description terms match portable model");
    app::FileManagementQuery unicode_manager;unicode_manager.area={1};unicode_manager.field=app::FileManagementSearch::uploader;unicode_manager.search=u8"REN\u00c9";
    check(db.managed_file_page(owner,unicode_manager,{},100).value->files.size()==1,"Sysop uploader Unicode search");
    unicode_query.search=app::FileSearchKind::filename;unicode_query.query="\xc0\xaf";
    check(!db.file_page(alice,unicode_query,{},100),"invalid UTF-8 query rejected without executing native data");
    check(db.shutdown().succeeded(),"final file writer stopped");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"final exact manifest/integrity/foreign keys");}
    migration_faults(root/"migration",paths);
    std::cout<<"PASS file storage "<<checks<<" checks; real SQLite with synthetic actors/payload identities, not native filesystem/client acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL file storage after "<<checks<<": "<<e.what()<<'\n';return 1;}}
