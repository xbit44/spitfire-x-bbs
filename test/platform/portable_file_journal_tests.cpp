// Real SQLite/NTFS adapter qualification; actors and scanner are synthetic.
// No installed authentication, live peer or replacement-PC claim.
#define main retained_file_intake_tests
#include "fixture_paths.hpp"
#include "../integration/file_intake_tests.cpp"
#undef main
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <fstream>

namespace fs=std::filesystem;
std::vector<char> bytes(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
bool ready(const native::PortableFileBindingResult& result){if(!result.published)for(const auto& d:result.diagnostics)std::cerr<<d<<'\n';return result.published&&result.state==native::PortableFileBindingState::ready;}
bool same(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id&&a.bytes==b.bytes&&a.sha256==b.sha256;}

int main(int argc,char** argv)try{
    check(argc==2,"new contained journal fixture required");auto root=fs::absolute(argv[1]).lexically_normal();root.make_preferred();
    auto prefix=(sfx::test::fixture_checkout() / L"build/section16-portable-r1/journal-fixtures");prefix.make_preferred();
    check(root.native().rfind(prefix.native()+L"\\",0)==0&&!fs::exists(root),"new owned fixture only");fs::create_directories(root);
    const auto sid=native::current_process_user_sid_string();const auto descriptor=ins::mutable_descriptor(sid);
    const auto original=root/L"original",copied=root/L"copied";
    ins::create_directory(original,descriptor.get());
    for(const auto* leaf:{L"Config",L"Files",L"Files/General",L"Files/Uploads",L"Files/UploadToSysop",L"Data",L"Data/FileIntake",L"Data/FileRemoved",L"FTN",L"FTN/Outbound",L"Work"}){auto directory=original/leaf;directory.make_preferred();ins::create_directory(directory,descriptor.get());}
    check(static_cast<bool>(native::create_portable_board_key(original,sid)),"synthetic portable board key");
    const auto settings=[&](const fs::path& board){storage::SqliteStorageSettings out;out.data_directory=board/L"Data";out.utc_now=[]{return std::int64_t{1792248000};};return out;};
    sfx::test::FakeStoragePathPolicy paths;const app::FileConfigurationActor owner{true,{}};const app::FileActor caller{id(2),2,1,1};
    app::FileId ordinary,pending;app::Identifier128 pending_operation=id(140);app::FilePayloadIdentity retained;
    const auto request=[&](storage::SqliteStorage& db,app::FileId file,unsigned operation,std::string name){
        app::FileOperationRequest r;r.operation=id(operation);r.action=app::FileOperationAction::move;r.file=file;r.filename=std::move(name);r.destination={1};
        const auto current=db.managed_file(owner,file);check(current&&current.value,"actual current file revision");r.expected_revision=current.value->file.revision;
        const auto config=db.file_configuration(owner);r.expected_destination_revision=config.value->areas[0].area.revision;return r;
    };
    {
        storage::SqliteStorage db(settings(original),paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"current real schema initialized");seed(original/L"Data/SFX.DB");check(db.start().succeeded(),"synthetic actors loaded");
        // An absolute internal area remains byte-exact in SQLite across copying.
        for(const auto index:{0U,1U}){const auto config=db.file_configuration(owner);app::FileConfigurationChange change;change.operation=id(90+index);change.action=app::FileConfigurationAction::area;change.expected_catalog_revision=config.value->policy.catalog_revision;change.area=config.value->areas[index];change.area.storage_path=(original/(index==0?L"Files/General":L"Files/Uploads")).u8string();check(db.change_file_configuration(owner,change).succeeded(),"absolute SFX-owned area fixture");}
        auto staging=native::FilePayloadDirectory::private_store(original/L"Data/FileIntake",sid);
        auto uploads=native::FilePayloadDirectory::area(original,original/L"Files/Uploads",{original/L"Files/General",L"Files/UploadToSysop"},{},sid);
        const auto upload=[&](unsigned operation,const char* name){native::WindowsFileIntake in(db,owner,caller,{id(operation),{2},false,"2026-11-06"},staging);append(in,"retained journal payload");check(in.finish(name,true).state==app::FileIntakeState::ready,"real intake ready");const auto out=in.accept(original,uploads,{"Portable test",""},false);if(!out.accepted)std::cerr<<out.detail<<'\n';check(out.accepted.has_value()&&out.journal_confirmed,"real intake accepted");return out.accepted->id;};
        ordinary=upload(100,"ordinary.txt");pending=upload(101,"pending.txt");retained=db.managed_file(owner,ordinary).value->payload;
        native::WindowsFileManager manager(db,owner,original,sid,[](auto point){if(point==native::FileManagerBoundary::after_rename)throw std::runtime_error("owned interruption after native rename");});
        auto move=request(db,pending,140,"pending-moved.txt");check(!manager.execute(move).completed&&fs::exists(original/L"Files/General/pending-moved.txt")&&!fs::exists(original/L"Files/Uploads/pending.txt"),"interrupted rename remains uncommitted and durable");
        check(db.checkpoint().succeeded()&&db.shutdown().succeeded(),"source storage stopped before complete copy");
    }
    check(ready(native::refresh_portable_file_snapshot(original,sid,true)),"automatic-update namespace snapshot");
    const auto source_db=bytes(original/L"Data/SFX.DB"),source_key=bytes(original/L"Config/board.key");
    fs::create_directory(copied);fs::copy(original,copied,fs::copy_options::recursive);
    // Native copying inherits destination access. This adapter test applies the
    // current private policy explicitly; the real preparation UI/worker is a
    // separate installed qualification, not simulated as a PASS here.
    native::WindowsStorageDaclPolicy current_access(sid);
    check(current_access.apply(copied,native::StorageDaclObjectKind::directory).succeeded(),"synthetic copied-host private root preparation");
    for(const auto& entry:fs::recursive_directory_iterator(copied))check(current_access.apply(entry.path(),entry.is_directory()?native::StorageDaclObjectKind::directory:native::StorageDaclObjectKind::file).succeeded(),"synthetic copied-host contained private access");
    check(ready(native::prepare_copied_file_bindings(copied,sid)),"copied bytes explicitly prepared");
    check(bytes(copied/L"Data/SFX.DB")==source_db&&bytes(original/L"Data/SFX.DB")==source_db&&bytes(copied/L"Config/board.key")==source_key,"preparation leaves both database bytes and original key exact");
    {
        storage::SqliteStorage db(settings(copied),paths);check(db.start().succeeded(),"copied database opens normally");
        auto uploads=native::FilePayloadDirectory::area(copied,original/L"Files/Uploads",{original/L"Files/General",L"Files/UploadToSysop"},{},sid);
        check(uploads.path()==copied/L"Files/Uploads","unchanged old internal configuration resolves only to current copy");
        {auto held=uploads.read("ordinary.txt");const auto logical=held.identity();const auto raw=held.native_object_identity();check(same(logical,retained)&&raw.file_id!=retained.file_id,"actual copied lease retains journal proof and exposes distinct native identity");auto duplicate=held.duplicate_transfer_handle(false);check(std::to_string(ins::file_id(duplicate.get()))==raw.file_id,"native transfer duplicate uses actual current handle identity");}
        native::WindowsFileManager manager(db,owner,copied,sid);
        const auto completed=manager.reconcile(pending_operation);check(completed.completed&&completed.journal_confirmed,"post-copy interrupted rename reconciles actual destination without replay");
        check(manager.reconcile(pending_operation).completed&&!fs::exists(copied/L"Files/Uploads/pending.txt"),"completed journal replay creates no source or duplicate");
        const auto moved=manager.execute(request(db,ordinary,141,"ordinary-moved.txt"));check(moved.completed&&moved.journal_confirmed&&!fs::exists(copied/L"Files/Uploads/ordinary.txt"),"post-copy real file-manager move commits");
        const auto current=db.managed_file(owner,ordinary);check(current&&current.value&&same(current.value->payload,retained),"same-volume rename preserves original logical payload identity");
        {auto staging=native::FilePayloadDirectory::private_store(copied/L"Data/FileIntake",sid);native::WindowsFileIntake in(db,owner,caller,{id(102),{2},false,"2026-11-06"},staging);append(in,"new copied-board upload");check(in.finish("new.txt",true).state==app::FileIntakeState::ready,"new post-copy intake ready");const auto out=in.accept(copied,uploads,{"New upload",""},false);check(out.accepted&&out.journal_confirmed,"post-copy real intake places and commits under resolved area");const auto record=db.file_intake(owner,id(102));check(record.value->staged_object&&record.value->placed_object&&record.value->staged_object->file_id==record.value->placed_object->file_id,"post-copy intake chooses native same-volume rename");}
        // Namespace proof never authorizes an unrelated replacement, even when
        // its bytes and basename happen to equal an old accepted payload.
        const auto path=copied/L"Files/General/ordinary-moved.txt";fs::rename(path,root/L"held-original-payload");fs::copy_file(root/L"held-original-payload",path);
        app::FileOperationRequest edit;edit.operation=id(142);edit.action=app::FileOperationAction::metadata;edit.file=ordinary;edit.expected_revision=current.value->file.revision;edit.descriptions={"Unrelated replacement",""};
        check(!manager.execute(edit).completed&&bytes(root/L"held-original-payload")==bytes(path),"identical foreign replacement refused and both objects preserved");
        check(db.checkpoint().succeeded()&&db.shutdown().succeeded(),"copied database stopped");
    }
    check(bytes(original/L"Data/SFX.DB")==source_db&&bytes(original/L"Config/board.key")==source_key,"all copy operations leave source data and board key exact");
    std::cout<<"PASS "<<checks<<" real copied SQLite/NTFS journal, interrupted rename, explicit reconciliation, intake, native transfer and foreign-replacement checks; synthetic actors, no installed/physical claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}
