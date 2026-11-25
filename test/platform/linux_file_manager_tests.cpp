#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#include "sfx/platform/linux/file_manager.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/file_intake.hpp"
#include <sys/stat.h>
#include <cerrno>
#include <fstream>
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include "sqlite_sfx_baseline.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>

namespace app=sfx::application;
namespace native=sfx::platform::linux_host;
namespace storage=sfx::adapters::storage;
namespace sql=storage::internal;
namespace {
unsigned checks{},operations=100;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 r;r.bytes[0]=29;r.bytes[14]=static_cast<std::uint8_t>(n>>8);r.bytes[15]=static_cast<std::uint8_t>(n);return r;}
void seed(const std::filesystem::path& file){
    sql::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
    c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000)","synthetic File Manager actors");
    c.exec("INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","complete isolated synthetic fixture");
    sql::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,2,2,1700000000,0,1700000000,NULL,NULL,?1)","synthetic live session");q.bind(1,id(2));q.step();
}
std::int64_t scalar(const std::filesystem::path& file,const char* query){sql::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return c.scalar_int64(query,"native file management assertion");}
la_ssize_t zip_write(struct archive*,void* context,const void* bytes,std::size_t count){static_cast<std::string*>(context)->append(static_cast<const char*>(bytes),count);return static_cast<la_ssize_t>(count);}
std::string diz_zip(){
    struct Free{void operator()(struct archive* a)const{archive_write_free(a);}};std::unique_ptr<struct archive,Free> writer(archive_write_new());std::string bytes;
    if(archive_write_set_format_zip(writer.get())!=ARCHIVE_OK||archive_write_set_bytes_per_block(writer.get(),0)!=ARCHIVE_OK||archive_write_open(writer.get(),&bytes,nullptr,zip_write,nullptr)!=ARCHIVE_OK)throw std::runtime_error("DIZ fixture init");
    const std::string body="\r\n\x1b[31mUseful DIZ title\x1b[0m\r\nExtended DIZ text.\r\n";auto* entry=archive_entry_new();archive_entry_set_pathname(entry,"FILE_ID.DIZ");archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_size(entry,static_cast<la_int64_t>(body.size()));
    const auto header=archive_write_header(writer.get(),entry);archive_entry_free(entry);if(header!=ARCHIVE_OK||archive_write_data(writer.get(),body.data(),body.size())!=static_cast<la_ssize_t>(body.size())||archive_write_close(writer.get())!=ARCHIVE_OK)throw std::runtime_error("DIZ fixture write");return bytes;
}
void publication_failure(const std::filesystem::path& root,unsigned mode){
    namespace fs=std::filesystem;
    const auto directory=[](const fs::path& path){check(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"fresh publication fixture directory");};
    for(const auto& p:{root,root/"config",root/"data",root/"data/fileintake",root/"data/fileremoved",root/"files",root/"files/general",root/"files/uploads",root/"files/uploadtosysop"})directory(p);
    const auto config=native::open_owned_directory(root/"config");const std::string first="SFX_LINUX_FIRST_RUN=1\n";
    native::publish_private_file(config.get(),"first-run.v1",{first.begin(),first.end()},false);
    check(bool((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"publication fixture key");
    native::LinuxLocalStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/"data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"publication fixture schema");seed(root/"data/sfx.db");check(db.start().succeeded(),"publication fixture actors");
    const app::FileConfigurationActor owner{true,{}};const auto destination=mode==2?fs::path(root.u8string()+"-external"):root/"files/general";
    std::int64_t destination_revision=1;
    if(mode==2){directory(destination);const auto current=db.file_configuration(owner);app::FileConfigurationChange edit;edit.operation=id(900);edit.action=app::FileConfigurationAction::area;edit.expected_catalog_revision=current.value->policy.catalog_revision;edit.area=current.value->areas[0];edit.area.storage_path=destination.u8string();check(db.change_file_configuration(owner,edit).succeeded(),"external area requires journaled copy move");destination_revision=2;}
    app::FileOperationRequest request;app::FilePayloadIdentity expected;
    {
        auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);auto source=native::FilePayloadDirectory::area(root,"files/uploads",{}, {"config","data"},bindings);
        app::FileRegistration registration;registration.operation=id(901);registration.area={2};registration.expected_area_revision=1;registration.filename="original.txt";registration.descriptions={"Recovery",""};registration.public_uploader="Sysop";
        {auto file=source.create("original.txt",100);const std::string bytes="retained journal bytes";file.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());registration.payload=expected=file.identity();}
        const auto registered=db.register_file(owner,registration);check(registered.succeeded(),"publication fixture registered");
        request.operation=id(902);request.file=registered.value->id;request.expected_revision=registered.value->revision;request.action=mode==1?app::FileOperationAction::purge:app::FileOperationAction::move;
        if(mode!=1){request.destination={1};request.filename="final.txt";request.expected_destination_revision=destination_revision;}
        native::LinuxFileManager fault(db,owner,root,bindings,[&](auto point){if(point==(mode==2?native::FileManagerBoundary::after_copy:native::FileManagerBoundary::before_change))fs::create_hard_link(root/"config/file-identities-linux.v1",root/"config/publication-fault");});
        check(!fault.execute(request).completed&&!fs::exists(root/"files/uploads/original.txt")&&(mode==1||fs::exists(destination/"final.txt")),"real namespace mutation before refused map publication stays held");
        check(fs::remove(root/"config/publication-fault"),"remove only owned fault injector");
        bool latched=false;try{bindings->refresh();}catch(...){latched=true;}check(latched,"failed publication map cannot be reused after injector removed");
    }
    check(db.shutdown().succeeded(),"close pending fixture before copy");
    const auto original_map=native::read_private_file(config.get(),"file-identities-linux.v1",1024*1024);
    const fs::path copied(root.u8string()+"-copy");fs::copy(root,copied,fs::copy_options::recursive);
    auto copied_settings=settings;copied_settings.data_directory=copied/"data";storage::SqliteStorage copy_db(copied_settings,paths);check(copy_db.start().succeeded(),"copied pending database opens");
    bool cancelled=false;try{native::prepare_file_intake_bindings(copy_db,owner,copied,[]{return true;});}catch(...){cancelled=true;}
    const auto copied_config=native::open_owned_directory(copied/"config");
    check(cancelled&&native::read_private_file(copied_config.get(),"file-identities-linux.v1",1024*1024)==original_map,"cancelled Prepare retains exact old authenticated map");
    if(mode==0){
        fs::rename(copied/"files/general/final.txt",copied/"files/general/retained-original");
        {std::ofstream replacement(copied/"files/general/final.txt");replacement<<"unrelated replacement";}check(chmod((copied/"files/general/final.txt").c_str(),0600)==0,"private replacement");
        {auto rejected=native::prepare_file_intake_bindings(copy_db,owner,copied);check(rejected->deferred_records()==1&&copy_db.file_operation(owner,request.operation).value->state==app::FileOperationState::held,"changed copied destination preserved and deferred");}
        fs::remove(copied/"files/general/final.txt");fs::rename(copied/"files/general/retained-original",copied/"files/general/final.txt");
    }
    {
        auto prepared=native::prepare_file_intake_bindings(copy_db,owner,copied);check(prepared->deferred_records()==0,"exact pending receipt resolves copied custody");
        native::LinuxFileManager manager(copy_db,owner,copied,prepared);check(manager.reconcile(request.operation).completed,"copied pending operation reconciles through shared transaction");prepared->refresh();
        check(native::read_private_file(config.get(),"file-identities-linux.v1",1024*1024)==original_map,"copied Prepare does not modify original board map");
    }
    check(copy_db.shutdown().succeeded()&&db.start().succeeded(),"copy closed and original reopened independently");
    {
        auto reopened=native::LinuxFilePayloadBindings::open(root);native::LinuxFileManager manager(db,owner,root,reopened);
        check(manager.reconcile(request.operation).completed&&manager.reconcile(request.operation).completed,"same-host map publication recovery and idempotent replay");reopened->refresh();
        check(db.managed_file(owner,request.file).value->purged==(mode==1),"correct terminal catalog state");
    }
    check(db.shutdown().succeeded(),"publication fixture clean shutdown");
}
}
int main(int argc,char** argv){try{
    check(argc==2,"new isolated root required");const auto fixture=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(fixture),"never overwrite or reuse fixture");const auto root=fixture/"Install";

    for(const auto& path:{fixture,root,fixture/"Input",root/"files",root/"files"/"general",root/"files"/"uploads",root/"files"/"uploadtosysop",root/"config",root/"data",root/"data"/"fileremoved",root/"data"/"fileintake"})check(std::filesystem::create_directory(path)&&chmod(path.c_str(),0700)==0,"new private fixture directory");
    const auto config_directory=native::open_owned_directory(root/"config");const std::string intent="SFX_LINUX_FIRST_RUN=1\n";
    native::publish_private_file(config_directory.get(),"first-run.v1",{intent.begin(),intent.end()},false);
    check(bool((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"fresh portable board key");
    const auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);
    native::LinuxLocalStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/"data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"fresh amended unshipped schema15");const auto database=root/"data"/"sfx.db";seed(database);check(db.start().succeeded(),"synthetic authority fixture starts");
    app::FileConfigurationActor owner{true,{}};app::FileActor caller{id(2),2,1,1};
    const auto general=native::FilePayloadDirectory::area(root,"files/general",{"files/uploads","files/uploadtosysop"},{},bindings);
    const auto uploads=native::FilePayloadDirectory::area(root,"files/uploads",{"files/general","files/uploadtosysop"},{},bindings);
    const auto removed=native::FilePayloadDirectory::private_store(root/"data"/"fileremoved",bindings);
    native::LinuxFileManager manager(db,owner,root,bindings);
    const auto make=[&](std::string name,std::string body){auto payload=uploads.create(name,1000000);for(std::size_t at=0;at<body.size();){const auto count=(std::min)(std::size_t{65536},body.size()-at);payload.append(reinterpret_cast<const std::uint8_t*>(body.data()+at),count);at+=count;}app::FileRegistration registration;registration.operation=id(operations++);registration.area={2};registration.expected_area_revision=1;registration.filename=name;registration.descriptions={name,"Original long text"};registration.public_uploader="Sysop";registration.payload=payload.identity();registration.allow_duplicate_content=true;const auto result=db.register_file(owner,registration);check(result.succeeded(),"native preexisting-file fixture registration");return result.value->id;};
    const auto request=[&](app::FileId file,app::FileOperationAction action,app::FileAreaId destination=app::FileAreaId{},std::string name=""){
        app::FileOperationRequest r;r.operation=id(operations++);r.file=file;r.action=action;r.expected_revision=db.managed_file(owner,file).value->file.revision;r.destination=destination;r.filename=std::move(name);
        if(destination.value){const auto config=db.file_configuration(owner);const auto a=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& x){return x.area.id==destination;});r.expected_destination_revision=a->area.revision;}return r;
    };
    bool refused{};try{native::LinuxFileManager ordinary(db,{false,caller},root,bindings);}catch(...){refused=true;}check(refused,"ordinary caller cannot acquire native manager authority");
    const auto original=make("source.txt","owned original");const auto old=db.managed_file(owner,original).value->file;
    auto meta=request(original,app::FileOperationAction::metadata);meta.descriptions={"Edited","Long edited text"};meta.public_uploader="Archivist";
    check(manager.execute(meta).completed&&manager.execute(meta).completed,"metadata edit and terminal replay without native content mutation");
    check(uploads.read("source.txt").identity().bytes==14&&db.managed_file(owner,original).value->file.public_uploader=="Archivist","actual payload unchanged by metadata attribution edit");
    auto copy=request(original,app::FileOperationAction::copy,{1},"copy.txt");check(!manager.execute(copy).completed&&general.absent("copy.txt"),"duplicate warning refusal makes no destination");copy.allow_duplicate_content=true;
    const auto copied=manager.execute(copy);check(copied.completed&&!(copied.result==original),"native independent copy and catalog commit");
    const auto original_identity=uploads.read("source.txt").identity(),copy_identity=general.read("copy.txt").identity();
    check(original_identity.sha256==copy_identity.sha256&&original_identity.file_id!=copy_identity.file_id,"copy content exact and native objects independent");
    auto move=request(original,app::FileOperationAction::move,{1},"reviewed.txt");check(manager.execute(move).completed&&uploads.absent("source.txt")&&!general.absent("reviewed.txt"),"atomic actual review move to final area");
    check(db.managed_file(owner,original).value->file.uploaded_utc==old.uploaded_utc&&db.managed_file(owner,original).value->file.publication_sequence>old.publication_sequence,"original upload retained and final availability newly published");
    auto rename=request(original,app::FileOperationAction::rename,{1},"renamed.txt");check(manager.execute(rename).completed&&general.absent("reviewed.txt")&&!general.absent("renamed.txt"),"actual rename preserves catalog identity");
    auto remove=request(original,app::FileOperationAction::remove);check(manager.execute(remove).completed&&general.absent("renamed.txt"),"recoverable removal takes file out of visible area");
    auto record=db.managed_file(owner,original);check(record.value->file.state==app::FileCatalogState::removed&&!record.value->purged&&!removed.absent(record.value->removed_key),"removed exact payload remains in durable private store");
    auto restore=request(original,app::FileOperationAction::restore,{1},"restored.txt");check(manager.execute(restore).completed&&!general.absent("restored.txt")&&removed.names().empty(),"explicit restore recovers original file ID and bytes");
    remove=request(original,app::FileOperationAction::remove);check(manager.execute(remove).completed,"second recoverable removal");const auto removed_key=db.managed_file(owner,original).value->removed_key;
    auto purge=request(original,app::FileOperationAction::purge);check(manager.execute(purge).completed&&removed.absent(removed_key)&&db.managed_file(owner,original).value->purged,"explicit exact purge confirmed absent and durable receipt");
    check(!general.absent("copy.txt"),"purge leaves independent copy untouched");
    auto wrong_missing=request(copied.result,app::FileOperationAction::mark_missing);check(!manager.execute(wrong_missing).completed,"existing file cannot be marked missing");
    check(manager.cancel_unchanged(wrong_missing.operation).cancelled&&db.file_information(caller,copied.result).succeeded(),"unchanged source permits cancelling failed intent and restores visibility");
    const auto interrupted=make("interrupted.txt","recoverable move");auto interrupted_move=request(interrupted,app::FileOperationAction::move,{1},"recovered.txt");
    native::LinuxFileManager fail_commit(db,owner,root,bindings,[](auto point){if(point==native::FileManagerBoundary::before_commit)throw std::runtime_error("injected before commit");});
    check(!fail_commit.execute(interrupted_move).completed&&uploads.absent("interrupted.txt")&&!general.absent("recovered.txt"),"actual move before injected DB boundary retained safely");
    check(!db.file_information(caller,interrupted)&&!manager.cancel_unchanged(interrupted_move.operation).cancelled,"moved source cannot be cancelled as unchanged or listed");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"restart retains operation journal");
    check(manager.reconcile(interrupted_move.operation).completed&&db.file_information(caller,interrupted).succeeded(),"exact same native destination reconciles interrupted move after restart");
    auto interrupted_copy=request(interrupted,app::FileOperationAction::copy,{1},"recovered-copy.txt");interrupted_copy.allow_duplicate_content=true;
    check(!fail_commit.execute(interrupted_copy).completed&&!general.absent("recovered.txt")&&!general.absent("recovered-copy.txt"),"copy fault retains both exact independent objects");
    check(manager.reconcile(interrupted_copy.operation).completed,"bound copy identity reconciles completed bytes without repeating copy");
    auto collision=request(interrupted,app::FileOperationAction::rename,{1},"foreign.txt");{auto file=general.create("foreign.txt",100);const std::string text="foreign";file.append(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());file.identity();}
    check(!manager.execute(collision).completed&&!db.file_operation(owner,collision.operation)&&db.file_information(caller,interrupted).succeeded(),"preexisting collision refused before reservation without hiding source");
    check(general.read("foreign.txt").identity().bytes==7&&general.read("recovered.txt").identity().bytes==16,"collision preserves both unrelated file and source");
    auto alias=request(interrupted,app::FileOperationAction::rename,{1},"RECOVERED.TXT");check(!manager.execute(alias).completed&&!db.file_operation(owner,alias.operation),"same native path case alias refused before reservation");
    const auto partial_source=make("partial-source.txt","must remain intact");auto partial=request(partial_source,app::FileOperationAction::copy,{1},"partial-copy.txt");partial.allow_duplicate_content=true;
    native::LinuxFileManager fail_created(db,owner,root,bindings,[](auto point){if(point==native::FileManagerBoundary::destination_created)throw std::runtime_error("injected before first byte");});
    check(!fail_created.execute(partial).completed&&!general.absent("partial-copy.txt")&&general.read("partial-copy.txt").identity().bytes==0,"empty new destination bound before injected copy fault");
    check(!manager.reconcile(partial.operation).completed&&!manager.cancel_unchanged(partial.operation).cancelled&&!uploads.absent("partial-source.txt"),"partial copy not falsely committed, deleted or hidden from recovery");
    const auto cancelled_source=make("cancel-source.txt",std::string(150000,'c'));auto cancelled=request(cancelled_source,app::FileOperationAction::rename,{2},"cancelled.txt");unsigned callbacks{};
    check(!manager.execute(cancelled,[&](auto done,auto){++callbacks;return done<65536;}).completed&&callbacks>=2&&uploads.absent("cancelled.txt"),"bounded hash cancellation before mutation keeps original");
    check(manager.cancel_unchanged(cancelled.operation).cancelled&&!uploads.absent("cancel-source.txt"),"verified unchanged cancellation releases interrupted hash intent");
    const auto missing_file=make("missing.txt","temporarily relocated");{auto file=uploads.read("missing.txt",true);file.rename_to(removed,"missing-fixture");}
    auto missing=request(missing_file,app::FileOperationAction::mark_missing);check(manager.execute(missing).completed&&!db.file_information(caller,missing_file),"actual native absence marks missing without deleting metadata");
    {auto file=removed.read("missing-fixture",true);file.rename_to(uploads,"missing.txt");}
    auto found=request(missing_file,app::FileOperationAction::restore,{2},"missing.txt");check(manager.execute(found).completed&&db.file_information(caller,missing_file).succeeded(),"same original native file reappearing is restored without move");
    const auto atomic_source=make("atomic.txt","atomic move boundary");auto atomic=request(atomic_source,app::FileOperationAction::move,{1},"atomic-final.txt");
    native::LinuxFileManager fail_atomic(db,owner,root,bindings,[](auto point){if(point==native::FileManagerBoundary::after_rename)throw std::runtime_error("injected immediately after atomic rename");});
    check(!fail_atomic.execute(atomic).completed&&!db.file_operation(owner,atomic.operation).value->placed_object,"atomic rename fault before checkpoint remains held with original native identity");
    check(manager.reconcile(atomic.operation).completed&&uploads.absent("atomic.txt")&&!general.absent("atomic-final.txt"),"atomic original file identity recovers rename before destination checkpoint");
    const auto input=native::FilePayloadDirectory::private_store(fixture/"Input",bindings);
    const auto write_owned=[](const native::FilePayloadDirectory& directory,const std::string& name,const std::string& bytes){auto file=directory.create(name,(std::max)(std::size_t{1},bytes.size()));for(std::size_t at=0;at<bytes.size();){const auto count=(std::min)(std::size_t{65536},bytes.size()-at);file.append(reinterpret_cast<const std::uint8_t*>(bytes.data()+at),count);at+=count;}return file.identity();};
    auto config=db.file_configuration(owner);app::FileConfigurationChange scan_policy;scan_policy.operation=id(operations++);scan_policy.expected_catalog_revision=config.value->policy.catalog_revision;scan_policy.policy=config.value->policy;scan_policy.policy.scan_uploads=true;
    check(db.change_file_configuration(owner,scan_policy).succeeded(),"turn caller scanning On; Sysop imports remain separate");
    const auto in_place_identity=write_owned(uploads,"dropped.bin","Dropped directly into selected area");
    native::NativeFileImportChoice choice;choice.operation=id(operations++);choice.area={2};choice.source=uploads.path()/"dropped.bin";choice.mode=app::FileImportMode::in_place;
    auto preview=manager.preview_import(choice);check(preview&&preview->registration.public_uploader=="Sysop"&&preview->registration.descriptions.short_text=="dropped.bin","no-DIZ in-place preview uses filename and public Sysop name");
    const auto in_place=manager.import_file(*preview);check(in_place.completed&&uploads.read("dropped.bin").identity().file_id==in_place_identity.file_id,"actual in-place registration does not copy or move");
    check(manager.import_file(*preview).result==in_place.result,"exact import replay stable without duplicate record");
    choice.operation=id(operations++);preview=manager.preview_import(choice);check(preview&&!manager.import_file(*preview).completed&&!db.file_import(owner,choice.operation),"already registered drop skipped without creating import intent");
    const auto external_identity=write_owned(input,"external.txt","External copy payload");choice={};choice.operation=id(operations++);choice.area={1};choice.source=input.path()/"external.txt";
    preview=manager.preview_import(choice);check(preview&&preview->mode==app::FileImportMode::copy,"outside-folder import defaults to copy");
    const auto imported_copy=manager.import_file(*preview);check(imported_copy.completed&&!input.absent("external.txt")&&general.read("external.txt").identity().sha256==external_identity.sha256&&general.read("external.txt").identity().file_id!=external_identity.file_id,"copy retains outside source and creates independent protected object");
    check(db.managed_file(owner,imported_copy.result).value->file.origin==app::FileOrigin::sysop_import,"import retains Sysop origin, not caller upload");
    write_owned(input,"duplicate.txt","External copy payload");choice.operation=id(operations++);choice.source=input.path()/"duplicate.txt";
    preview=manager.preview_import(choice);check(preview&&!manager.import_file(*preview).completed&&general.absent("duplicate.txt")&&!db.file_import(owner,choice.operation),"duplicate-content warning precedes any copy or journal");
    choice.allow_duplicate_content=true;choice.public_uploader="Museum Archivist";preview=manager.preview_import(choice);const auto override_import=manager.import_file(*preview);
    check(override_import.completed&&db.managed_file(owner,override_import.result).value->file.public_uploader=="Museum Archivist","explicit batch attribution and duplicate override accepted");
    const auto move_identity=write_owned(input,"external-move.txt","Explicit outside move");choice={};choice.operation=id(operations++);choice.area={1};choice.source=input.path()/"external-move.txt";choice.mode=app::FileImportMode::move;
    preview=manager.preview_import(choice);check(preview.has_value(),"explicit outside move preview");const auto imported_move=manager.import_file(*preview);
    check(imported_move.completed&&input.absent("external-move.txt")&&general.read("external-move.txt").identity().sha256==move_identity.sha256&&general.read("external-move.txt").identity().file_id!=move_identity.file_id,"outside move verifies private copy before exact original deletion");
    write_owned(input,"stale.txt","Before preview");choice={};choice.operation=id(operations++);choice.area={1};choice.source=input.path()/"stale.txt";preview=manager.preview_import(choice);
    {auto owned=input.read("stale.txt",true);owned.remove_exact();}write_owned(input,"stale.txt","Changed after preview");
    check(preview&&!manager.import_file(*preview).completed&&general.absent("stale.txt")&&!db.file_import(owner,choice.operation),"changed preview source refused without mutation");
    write_owned(input,"recover-import.txt","Import copy recovery");choice.operation=id(operations++);choice.source=input.path()/"recover-import.txt";preview=manager.preview_import(choice);
    check(preview&&!fail_commit.import_file(*preview).completed&&!general.absent("recover-import.txt"),"actual copied import retained after injected before-commit failure");
    auto bypass=preview->registration;bypass.operation=id(operations++);check(!db.register_file(owner,bypass)&&!db.pending_file_imports({false,caller},{},100),"held import reservation cannot be bypassed or disclosed to callers");
    check(db.shutdown().succeeded()&&db.start().succeeded()&&manager.reconcile_import(choice.operation).completed,"bound completed copy reconciles after restart without repeated source removal");
    write_owned(input,"partial-import.txt","Partial import recovery");choice.operation=id(operations++);choice.source=input.path()/"partial-import.txt";preview=manager.preview_import(choice);
    check(preview&&!fail_created.import_file(*preview).completed&&!manager.reconcile_import(choice.operation).completed&&!manager.cancel_import_unchanged(choice.operation).cancelled&&!input.absent("partial-import.txt"),"partial import privately held without source deletion or false success");
    write_owned(uploads,"cancel-import.txt","Unchanged in-place cancellation");choice={};choice.operation=id(operations++);choice.area={2};choice.source=uploads.path()/"cancel-import.txt";choice.mode=app::FileImportMode::in_place;preview=manager.preview_import(choice);
    check(preview&&!fail_commit.import_file(*preview).completed&&manager.cancel_import_unchanged(choice.operation).cancelled&&!uploads.absent("cancel-import.txt"),"in-place failed commit can cancel after exact unchanged-file verification");
    const auto zip=diz_zip();const auto zip_identity=write_owned(input,"diz.ZIP",zip);choice={};choice.operation=id(operations++);choice.area={1};choice.source=input.path()/"diz.ZIP";preview=manager.preview_import(choice);
    check(preview&&preview->registration.descriptions.short_text=="Useful DIZ title"&&preview->registration.descriptions.long_text.find('\x1b')==std::string::npos,"actual ZIP DIZ sanitized in import preview");
    check(manager.import_file(*preview).completed&&general.read("diz.ZIP").identity().sha256==zip_identity.sha256&&input.read("diz.ZIP").identity().sha256==zip_identity.sha256,"DIZ import never rewrites archive bytes");
    choice.operation=id(operations++);choice.filename="diz-override.zip";choice.allow_duplicate_content=true;choice.descriptions=app::FileDescriptions{"Manual short","Manual long"};preview=manager.preview_import(choice);
    check(preview&&preview->registration.descriptions.short_text=="Manual short"&&manager.import_file(*preview).completed,"individual manual metadata override leaves source DIZ intact");
    write_owned(input,"invalid.zip","Not a ZIP; Sysop-owned import");choice={};choice.operation=id(operations++);choice.area={1};choice.source=input.path()/"invalid.zip";preview=manager.preview_import(choice);
    check(preview&&preview->registration.descriptions.short_text=="invalid.zip"&&manager.import_file(*preview).completed,"unusable DIZ uses filename fallback, not caller scan policy");
    choice.operation=id(operations++);choice.source=database;check(!manager.preview_import(choice),"protected database cannot be imported as an outside source");
    const auto input_only=native::FilePayloadDirectory::import_source(input.path(),{root},bindings);bool create_refused{},move_refused{};
    try{input_only.create("not-output.txt",10);}catch(...){create_refused=true;}try{input_only.read("external.txt",true);}catch(...){move_refused=true;}
    check(create_refused&&move_refused&&input.absent("not-output.txt"),"ordinary source capability cannot create or acquire deletion rights");
    const auto names=db.file_name_status(owner,{1},{"COPY.TXT","not-copy.txt","partial-copy.txt","partial-import.txt"});
    check(names&&names.value->size()==4&&(*names.value)[0].registered==copied.result&&!(*names.value)[1].registered.value&&(*names.value)[2].pending&&(*names.value)[3].pending,"exact lookup folds names and excludes held mutations/imports");
    check(!db.file_name_status({false,caller},{3},{"secret.txt"})&&!db.file_name_status({false,caller},{3},{}),"ordinary caller cannot use filename reconciliation, even empty requests");
    check(!db.file_name_status(owner,{999},{"name.txt"})&&!db.file_name_status(owner,{1},{"../bad"})&&!db.file_name_status(owner,{1},std::vector<std::string>(101,"valid.txt")),"lookup rejects unknown area, unsafe names and oversized batches");
    write_owned(general,u8"\u00c9preuve.txt","Unicode existing import");
    choice={};choice.operation=id(operations++);choice.area={1};choice.source=general.path()/std::filesystem::u8path(u8"\u00c9preuve.txt");choice.mode=app::FileImportMode::in_place;
    preview=manager.preview_import(choice);check(preview&&manager.import_file(*preview).completed,"Unicode in-place catalog fixture");
    const auto folded=db.file_name_status(owner,{1},{u8"\u00e9PREUVE.TXT"});
    check(folded&&folded.value->front().registered.value>0,"exact discovery lookup uses shared Unicode fold");
    const auto before_discovery=scalar(database,"SELECT count(*) FROM sfx_file_catalog");
    for(unsigned n=0;n<231;++n)write_owned(general,"discover-"+std::to_string(n)+".txt","unregistered "+std::to_string(n));
    write_owned(general,u8"\U00010400-unregistered.txt","supplementary unregistered");
    std::vector<std::string> discovered;auto discovery=manager.find_unregistered({1});unsigned discovery_pages{};bool finished{};
    while(!finished&&discovery_pages<1000){const auto page=discovery.next(7);++discovery_pages;check(page.names.size()<=7,"discovery page memory stays bounded");finished=page.finished;discovered.insert(discovered.end(),page.names.begin(),page.names.end());}
    check(finished&&discovery_pages>30&&discovered.size()==233,"incremental actual Linux filesystem discovery finds every supported unregistered candidate");
    check(std::find(discovered.begin(),discovered.end(),"foreign.txt")!=discovered.end()&&std::find(discovered.begin(),discovered.end(),u8"\U00010400-unregistered.txt")!=discovered.end(),"discovery preserves native names including supplementary scalars");
    check(std::find(discovered.begin(),discovered.end(),"partial-copy.txt")==discovered.end()&&std::find(discovered.begin(),discovered.end(),"partial-import.txt")==discovered.end()&&std::find(discovered.begin(),discovered.end(),"copy.txt")==discovered.end(),"registered and reserved native entries never appear as unregistered");
    check(scalar(database,"SELECT count(*) FROM sfx_file_catalog")==before_discovery&&!general.absent("discover-0.txt"),"discovery alone never registers, moves or deletes payloads");
    auto cancelled_scan=manager.find_unregistered({1});bool scan_cancelled{},scan_poisoned{};
    try{cancelled_scan.next(3,[]{return false;});}catch(...){scan_cancelled=true;}
    try{cancelled_scan.next(3);}catch(...){scan_poisoned=true;}
    check(scan_cancelled&&scan_poisoned,"cancelled discovery cannot silently resume past skipped entries");
    auto stale_scan=manager.find_unregistered({1});config=db.file_configuration(owner);app::FileConfigurationChange area_change;
    area_change.operation=id(operations++);area_change.action=app::FileConfigurationAction::area;area_change.expected_catalog_revision=config.value->policy.catalog_revision;area_change.area=config.value->areas[0];area_change.area.area.description="Changed during discovery";
    check(db.change_file_configuration(owner,area_change).succeeded(),"authorized area edit while read-only discovery exists");
    bool stale_refused{};try{stale_scan.next(5);}catch(...){stale_refused=true;}check(stale_refused,"changed area invalidates discovery instead of continuing stale configuration");
    choice={};choice.operation=id(operations++);choice.area={1};choice.source=general.path()/"discover-0.txt";choice.mode=app::FileImportMode::in_place;
    preview=manager.preview_import(choice);check(preview&&manager.import_file(*preview).completed,"explicit selected discovery candidate registers in place after fresh preview");
    check(db.file_name_status(owner,{1},{"discover-0.txt"}).value->front().registered.value>0,"new registration immediately disappears from exact unregistered lookup");
    app::FileManagementQuery inspect;inspect.area={2};inspect.search="missing.txt";inspect.field=app::FileManagementSearch::filename;
    auto presence=manager.inspect_files(inspect,{},10);check(presence.entries.size()==1&&presence.entries[0].presence==native::FilePayloadPresence::present,"reconciliation verifies exact present payload without mutation");
    {auto file=uploads.read("missing.txt",true);file.rename_to(removed,"discovery-missing-fixture");}
    presence=manager.inspect_files(inspect,{},10);check(presence.entries.size()==1&&presence.entries[0].presence==native::FilePayloadPresence::missing&&db.managed_file(owner,missing_file).value->file.state==app::FileCatalogState::available,"read-only missing detection leaves catalog unchanged until confirmation");
    write_owned(uploads,"missing.txt","replacement, not original");presence=manager.inspect_files(inspect,{},10);
    check(presence.entries.size()==1&&presence.entries[0].presence==native::FilePayloadPresence::changed,"different native object at old name is changed, never restored as original");
    {auto replacement=uploads.read("missing.txt",true);replacement.rename_to(removed,"discovery-replacement-fixture");auto original_file=removed.read("discovery-missing-fixture",true);original_file.rename_to(uploads,"missing.txt");}
    {auto locked=uploads.read("missing.txt",true);presence=manager.inspect_files(inspect,{},10);check(presence.entries.size()==1&&presence.entries[0].presence==native::FilePayloadPresence::unavailable,"native sharing conflict is unavailable, never incorrectly missing");}
    bool inspect_cancelled{};try{manager.inspect_files(inspect,{},10,[](auto,auto){return false;});}catch(...){inspect_cancelled=true;}check(inspect_cancelled,"cancelled reconciliation aborts preview without catalog writes");
    const auto staging=native::FilePayloadDirectory::private_store(root/"data"/"fileintake",bindings);
    const auto held_upload=[&](const std::string& name,bool place){
        const auto operation=id(operations++);auto begun=db.begin_file_intake(caller,{operation,{2},false,"2026-10-17"});check(begun.succeeded(),"new synthetic held-upload journal");
        auto payload=staging.create(begun.value->stage_key,128);check(db.bind_file_intake_object(caller,operation,false,payload.object_identity()).succeeded(),"bound native staging before test bytes");
        const std::string text="held private payload";payload.append(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());
        app::FileIntakeReceived received;received.operation=operation;received.filename=name;received.payload=payload.identity();received.transfer_complete=true;
        received.scan=place?app::FileScanResult{app::FileScanVerdict::clean,{},"Synthetic clean"}:app::FileScanResult{app::FileScanVerdict::error,{},"Synthetic scan error"};
        check(db.record_file_intake(caller,received).succeeded(),"record synthetic native receive/scan outcome");
        if(place){check(db.prepare_file_intake(caller,operation,{"Held placement",""},false).succeeded()&&db.bind_file_intake_object(caller,operation,true,payload.object_identity()).succeeded(),"prepare and bind interrupted placement");payload.rename_to(uploads,name);}
        check(db.resolve_file_intake(owner,operation,received.payload,false,"Explicit test HOLD").succeeded(),"retain unaccepted private HOLD");return *db.file_intake(owner,operation).value;
    };
    const auto error_hold=held_upload("error-purge.txt",false);const auto error_preview=manager.preview_intake_purge(error_hold.operation);
    check(error_preview&&error_preview->staging&&!error_preview->placed&&!db.file_intake_purge(owner,error_hold.operation),"read-only Sysop preview does not imply purge authority or create intent");
    check(!db.file_intake_purge({false,caller},error_hold.operation)&&!db.prepare_file_intake_purge({false,caller},*error_preview)&&!db.finish_file_intake_purge({false,caller},*error_preview),"ordinary caller cannot inspect, prepare or complete manual purge");
    check(!db.resolve_file_intake(owner,error_hold.operation,error_hold.payload,true,"not malware"),"scan error remains ineligible for automatic detection deletion");
    check(manager.purge_held_intake(*error_preview).completed&&staging.absent(error_hold.stage_key)&&db.file_intake_purge(owner,error_hold.operation).value->completed,"explicit confirmed Sysop purge safely removes exact scan-error staging");
    check(db.file_intake(owner,error_hold.operation).value->scan.verdict==app::FileScanVerdict::error&&manager.purge_held_intake(*error_preview).completed,"manual purge preserves truthful original verdict and idempotent completion");
    const auto placed_hold=held_upload("placed-purge.txt",true);const auto placed_preview=manager.preview_intake_purge(placed_hold.operation);
    check(placed_preview&&!placed_preview->staging&&placed_preview->placed&&manager.purge_held_intake(*placed_preview).completed&&uploads.absent("placed-purge.txt"),"explicit purge reconciles interrupted same-volume placement without publishing it");
    const auto crash_hold=held_upload("crash-purge.txt",false);const auto crash_preview=manager.preview_intake_purge(crash_hold.operation);
    native::LinuxFileManager purge_fault(db,owner,root,bindings,[](auto at){if(at==native::FileManagerBoundary::after_staging_purge)throw std::runtime_error("synthetic purge interruption");});
    check(crash_preview&&!purge_fault.purge_held_intake(*crash_preview).completed&&staging.absent(crash_hold.stage_key)&&!db.file_intake_purge(owner,crash_hold.operation).value->completed,"actual removal before injected failure retains incomplete durable purge intent");
    const auto replacement_identity=write_owned(staging,crash_hold.stage_key,"unrelated replacement");
    check(!manager.purge_held_intake(*crash_preview).completed&&staging.read(crash_hold.stage_key).identity().file_id==replacement_identity.file_id,"retry retains replacement at former staging name");
    {auto replacement=staging.read(crash_hold.stage_key,true);replacement.rename_to(removed,"purge-test-replacement-preserved");}
    check(db.shutdown().succeeded()&&db.start().succeeded(),"restart preserves interrupted explicit purge");
    const auto recovered_preview=manager.preview_intake_purge(crash_hold.operation);
    check(recovered_preview&&recovered_preview->staging&&manager.purge_held_intake(*recovered_preview).completed,"restart reads original confirmed intent and reconciles exact absent payload");
    const auto empty_operation=id(operations++);const auto empty_start=db.begin_file_intake(caller,{empty_operation,{2},false,"2026-10-17"});check(empty_start&&db.resolve_file_intake(owner,empty_operation,{},false,"No payload was created").succeeded(),"allocation-failure HOLD without native object");
    const auto empty_preview=manager.preview_intake_purge(empty_operation);check(empty_preview&&!empty_preview->staging&&!empty_preview->placed&&manager.purge_held_intake(*empty_preview).completed,"explicit verified empty-HOLD closure releases capacity without touching files");
    check(!general.absent("copy.txt")&&!removed.absent("purge-test-replacement-preserved"),"held-upload purge leaves existing library and replacement fixture untouched");
    // Current owner workflow: one permanent delete, old retained compatibility,
    // all-area bulk registration and native absence-only catalog cleanup.
    const auto direct=make("direct-delete.txt","permanent delete test");
    auto direct_request=request(direct,app::FileOperationAction::purge);
    check(manager.execute(direct_request).completed&&uploads.absent("direct-delete.txt")&&db.managed_file(owner,direct).value->purged,"current Remove deletes directly without a retained payload or second purge");
    check(removed.absent(db.managed_file(owner,direct).value->removed_key)&&manager.execute(direct_request).completed,"permanent deletion receipt replays without another payload operation");
    const auto fault_delete=make("fault-delete.txt","restart exact delete");auto fault_request=request(fault_delete,app::FileOperationAction::purge);
    check(!fail_commit.execute(fault_request).completed&&uploads.absent("fault-delete.txt"),"delete-before-commit fault is not reported successful");
    check(db.shutdown().succeeded()&&db.start().succeeded()&&manager.reconcile(fault_request.operation).completed&&db.managed_file(owner,fault_delete).value->purged,"same recorded delete reconciles verified absence after restart");
    for(unsigned n=0;n<125;++n)write_owned(general,"bulk-"+std::to_string(n)+".txt","bulk duplicate content");
    write_owned(uploads,"bulk-diz.zip",diz_zip());write_owned(uploads,"bulk-no-diz.zip","not an archive");
    const auto registered_before=scalar(database,"SELECT count(*) FROM sfx_file_catalog");
    std::vector<std::string> added_report;const auto bulk=manager.scan_files(true,{},&added_report);check(bulk.completed&&scalar(database,"SELECT count(*) FROM sfx_file_catalog")>=registered_before+127,"bulk Pending spans areas/pages and registers duplicate content under distinct names without prompts");
    std::string listed;for(const auto& part:added_report){check(part.size()<=14000,"scan report pages stay within bounded private frames");listed+=part;}check(added_report.size()>1,"bulk additions span report pages rather than silently truncate");for(unsigned n=0;n<125;++n)check(listed.find("Added: bulk-"+std::to_string(n)+".txt [")!=listed.npos,"every confirmed bulk addition listed by filename and area");check(listed.find("bulk-diz.zip [")!=listed.npos&&listed.find("bulk-no-diz.zip [")!=listed.npos,"successful ZIP entries have result lines");
    const auto bulk_names=db.file_name_status(owner,{2},{"bulk-diz.zip","bulk-no-diz.zip"});check(bulk_names&&bulk_names.value->size()==2,"bulk results visible through exact native-name lookup");
    const auto diz_record=db.managed_file(owner,(*bulk_names.value)[0].registered),plain_record=db.managed_file(owner,(*bulk_names.value)[1].registered);
    check(diz_record&&diz_record.value->file.descriptions.short_text=="Useful DIZ title"&&plain_record&&plain_record.value->file.descriptions.short_text=="No Description","bulk DIZ extraction and exact No Description fallback");
    const auto registered_after=scalar(database,"SELECT count(*) FROM sfx_file_catalog");check(manager.scan_files(true).completed&&scalar(database,"SELECT count(*) FROM sfx_file_catalog")==registered_after,"second Pending scan does not duplicate existing catalog entries");
    const auto offline=make("offline-delete.txt","removed outside File Manager");{auto payload=uploads.read("offline-delete.txt",true);payload.remove_exact();}
    const auto gone=manager.scan_files(false);check(gone.completed&&db.managed_file(owner,offline).value->purged,"Offline bulk removes confirmed-missing listing without per-file prompt");
    const auto reappeared=make("reappeared.txt","original reappearing file");{auto payload=uploads.read("reappeared.txt",true);payload.rename_to(removed,"reappear-original");}
    check(manager.execute(request(reappeared,app::FileOperationAction::mark_missing)).completed,"native confirmed absence fixture");
    write_owned(uploads,"reappeared.txt","replacement must survive");const auto before_reappeared=uploads.read("reappeared.txt").identity();
    const auto erase_reappeared=request(reappeared,app::FileOperationAction::purge);check(!manager.execute(erase_reappeared).completed&&uploads.read("reappeared.txt").identity().file_id==before_reappeared.file_id&&!db.managed_file(owner,reappeared).value->purged,"missing-only deletion refuses a reappearing replacement and preserves its bytes");
    app::FileId offline_area_file;
    {const auto private_area=native::FilePayloadDirectory::area(root,"files/uploadtosysop",{"files/general","files/uploads"},{},bindings);write_owned(private_area,"offline-area.txt","retain when directory unavailable");
        native::NativeFileImportChoice offline_choice;offline_choice.operation=id(operations++);offline_choice.area={3};offline_choice.source=private_area.path()/"offline-area.txt";offline_choice.mode=app::FileImportMode::in_place;const auto p=manager.preview_import(offline_choice);check(p.has_value(),"actual third-area source preview");const auto imported=manager.import_file(*p);check(imported.completed,"actual third-area fixture registered");offline_area_file=imported.result;}
    const auto area_path=root/"files"/"uploadtosysop",held_area=root/"files"/"OfflineFixture";
    check(!std::filesystem::exists(held_area),"temporary native fixture destination absent");std::filesystem::rename(area_path,held_area);
    native::NativeFileOperationOutcome skipped_area;
    try{skipped_area=manager.scan_files(false);}catch(...){std::filesystem::rename(held_area,area_path);throw;}
    std::filesystem::rename(held_area,area_path);
    check(skipped_area.completed&&skipped_area.detail.find("locations skipped: 1")!=std::string::npos&&!db.managed_file(owner,offline_area_file).value->purged,"inaccessible area is reported and its catalog retained");
    const auto count_before_cancel=scalar(database,"SELECT count(*) FROM sfx_file_catalog");check(manager.scan_files(true,[](auto,auto){return false;}).cancelled&&scalar(database,"SELECT count(*) FROM sfx_file_catalog")==count_before_cancel,"bulk cancellation before work leaves catalog unchanged");
    check(scalar(database,"SELECT coalesce(sum(upload_count+download_count),0) FROM sfx_file_statistics")==0,"manager operations never fabricate caller transfer statistics");
    const auto map_bytes=[&]{return native::read_private_file(config_directory.get(),"file-identities-linux.v1",1024*1024);};
    const auto raw=[&](const std::string& name,const std::string& bytes){const auto path=uploads.path()/name;check(!std::filesystem::exists(path),"raw Sysop drop fixture is new");{std::ofstream out(path,std::ios::binary);out<<bytes;}check(chmod(path.c_str(),0600)==0,"raw Sysop drop is private");};
    raw("raw-drop.txt","copied by the Sysop outside SFX");const auto before_preview=map_bytes();
    choice={};choice.operation=id(operations++);choice.area={2};choice.source=uploads.path()/"raw-drop.txt";choice.mode=app::FileImportMode::in_place;
    preview=manager.preview_import(choice);check(preview&&map_bytes()==before_preview,"unregistered native drop preview publishes no custody map or catalog change");
    const auto raw_import=manager.import_file(*preview);check(raw_import.completed&&uploads.read("raw-drop.txt").identity().file_id==preview->registration.payload.file_id,"explicit confirmed in-place import publishes portable identity then catalog");
    raw("raw-replaced.txt","before review");choice.operation=id(operations++);choice.source=uploads.path()/"raw-replaced.txt";preview=manager.preview_import(choice);check(preview.has_value(),"raw replacement preview");
    std::filesystem::rename(uploads.path()/"raw-replaced.txt",uploads.path()/"raw-original-retained.txt");raw("raw-replaced.txt","replacement after review");const auto before_refusal=map_bytes();
    check(!manager.import_file(*preview).completed&&!db.file_import(owner,choice.operation)&&map_bytes()==before_refusal,"replaced raw preview refused before intent or adoption");
    raw("raw-bulk.txt","new bulk import outside SFX");const auto raw_bulk=manager.scan_files(true);check(raw_bulk.completed&&db.file_name_status(owner,{2},{"raw-bulk.txt"}).value->front().registered.value>0,"bulk Pending registers actual externally dropped files through confirmed import");
    check(db.shutdown().succeeded(),"normal storage shutdown");
    {sql::SqliteConnection c(database,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(sql::validate_sfx_shipping_database(c.get(),true),"exact schema integrity and foreign keys after actual mutations");}
    publication_failure(fixture/"map-move",0);publication_failure(fixture/"map-purge",1);publication_failure(fixture/"map-copy-move",2);
    std::cout<<"PASS native file manager "<<checks<<" checks; actual Linux filesystem/SQLite, map publication faults and copied-folder recovery; no installed GUI/cross-volume claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native file manager after "<<checks<<": "<<e.what()<<" errno="<<errno<<'\n';return 1;}}
