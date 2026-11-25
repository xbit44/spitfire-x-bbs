#include "../support/include/sfx/test/linux_layout_fixture.hpp"
// Reuse the real storage fixture and assertions from the native manager suite.
#define main file_manager_baseline_main
#include "linux_file_manager_tests.cpp"
#undef main
#include <unistd.h>

bool import_without_birth{};
extern "C" int __real_statx(int,const char*,int,unsigned,struct statx*);
extern "C" int __wrap_statx(int fd,const char* path,int flags,unsigned mask,struct statx* output){
    const auto result=__real_statx(fd,path,flags,mask,output);
    if(result==0&&import_without_birth)output->stx_mask&=~STATX_BTIME;
    return result;
}
namespace {
namespace fs=std::filesystem;
unsigned permissions(const fs::path& path){struct stat value{};check(stat(path.c_str(),&value)==0,"observe fixture permissions");return value.st_mode&07777;}
std::string contents(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
template<class F>void rejects(F fn,const char* reason){bool failed{};try{fn();}catch(...){failed=true;}check(failed,reason);}
void setup_import(const fs::path& root){
    for(const auto& p:{root,root/"config",root/"data",root/"data/fileintake",root/"data/fileremoved",root/"files",root/"files/general",root/"files/uploads",root/"files/uploadtosysop"})
        check(fs::create_directory(p)&&chmod(p.c_str(),0700)==0,"new private import fixture");
    const auto config=native::open_owned_directory(root/"config");const std::string first="SFX_LINUX_FIRST_RUN=1\n";
    native::publish_private_file(config.get(),"first-run.v1",{first.begin(),first.end()},false);
    check(bool((sfx::test::mark_provisioned_linux_fixture(root),native::initialize_fresh_portable_board_key(root))),"import fixture portable key");
}
storage::SqliteStorageSettings settings(const fs::path& root){storage::SqliteStorageSettings result;result.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;result.data_directory=root/"data";result.utc_now=[]{return std::int64_t{1792248000};};return result;}
void raw(const fs::path& path,const std::string& value){
    check(!fs::exists(path),"new external copy path");const auto prior=umask(0022);{std::ofstream out(path,std::ios::binary);out<<value;check(bool(out),"normal external copied bytes");}umask(prior);
    check(permissions(path)==0644,"ordinary Linux copy fixture really has mode 0644");
}
void exercise(const fs::path& root,int fault){
    setup_import(root);native::LinuxLocalStoragePathPolicy paths;storage::SqliteStorage db(settings(root),paths);
    check(db.start().succeeded()&&db.shutdown().succeeded(),"fresh import schema");seed(root/"data/sfx.db");check(db.start().succeeded(),"import actors");
    const app::FileConfigurationActor owner{true,{}};
    const auto file=root/"files/uploads/copied.txt",mapfile=root/"config/file-identities-linux.v1",injector=root/"config/import-map-link";
    const std::string bytes="ordinary Sysop copied file\n";raw(file,bytes);app::FileImportRequest request;
    {
        auto map=native::LinuxFilePayloadBindings::initialize_fresh(root);native::LinuxFileManager manager(db,owner,root,map);
        const auto map_before=contents(mapfile);native::NativeFileImportChoice choice;choice.operation=id(950);choice.area={2};choice.source=file;choice.mode=app::FileImportMode::in_place;
        const auto preview=manager.preview_import(choice);check(preview.has_value(),"ordinary 0644 file preview succeeds");request=*preview;
        check(contents(mapfile)==map_before&&permissions(file)==0644&&contents(file)==bytes&&!db.file_import(owner,choice.operation),"preview changes neither map, permissions, bytes nor intent");
        auto area=native::FilePayloadDirectory::area(root,"files/uploads",{}, {"config","data"},map);
        rejects([&]{area.read("copied.txt");},"ordinary managed reads still reject public permissions");
        if(fault<0){
            const auto cancelled=manager.import_file(request,[](auto,auto){return false;});
            check(!cancelled.completed&&!db.file_import(owner,choice.operation)&&contents(mapfile)==map_before&&permissions(file)==0644,"preflight cancel leaves copy untouched");
            const auto result=manager.import_file(request);check(result.completed&&permissions(file)==0600&&contents(file)==bytes,"explicit import establishes private custody and commits");
            check(area.read("copied.txt").identity().file_id==request.registration.payload.file_id,"catalog and private native object share reviewed identity");
            check(chmod(file.c_str(),0644)==0,"simulate unexpected permissions change on known object");choice.operation=id(951);
            check(!manager.preview_import(choice),"known private object with changed permissions is never silently adopted");
            check(chmod(file.c_str(),0600)==0,"restore only test object's permissions");
            raw(root/"files/uploads/unsafe.txt","unsafe permissions");check(chmod((root/"files/uploads/unsafe.txt").c_str(),0664)==0,"unsafe group-writable fixture");choice.source=root/"files/uploads/unsafe.txt";
            check(!manager.preview_import(choice),"group-writable source rejected");
            check(chmod(choice.source.c_str(),0644)==0,"restore safe copied mode");fs::create_hard_link(choice.source,root/"files/uploads/linked.txt");check(!manager.preview_import(choice),"hardlinked source rejected");
            check(fs::remove(root/"files/uploads/linked.txt"),"remove exact synthetic hardlink");fs::create_symlink(choice.source,root/"files/uploads/symlink.txt");choice.source=root/"files/uploads/symlink.txt";check(!manager.preview_import(choice),"symlink source rejected");
        }else{
            if(fault==0)fs::create_hard_link(mapfile,injector);
            native::LinuxFileManager interrupted(db,owner,root,map,[&](auto point){
                if(fault==1&&point==native::FileManagerBoundary::import_identity_recorded)throw std::runtime_error("synthetic stop after durable identity");
                if(fault==2&&point==native::FileManagerBoundary::import_permissions_protected)fs::create_hard_link(mapfile,injector);
            });
            check(!interrupted.import_file(request).completed&&db.file_import(owner,choice.operation).value->state==app::FileOperationState::held,"interrupted adoption has durable held import, no false success");
            check(permissions(file)==(fault==2?0600:0644)&&contents(file)==bytes,"failure preserves exact bytes and truthful permission stage");
            check(scalar(root/"data/sfx.db","SELECT count(*) FROM sfx_file_catalog")==0,"failed custody publication creates no catalog entry");
            if(fault!=1)check(fs::remove(injector),"remove exact publication fault link");
        }
    }
    check(db.shutdown().succeeded(),"close import fixture for copy");
    if(fault<0)return;
    const fs::path copied(root.u8string()+"-copy");fs::copy(root,copied,fs::copy_options::recursive);
    {
        storage::SqliteStorage copied_db(settings(copied),paths);check(copied_db.start().succeeded(),"copied pending import database");
        const auto copyfile=copied/"files/uploads/copied.txt";const auto mode=permissions(copyfile);
        auto prepared=native::prepare_file_intake_bindings(copied_db,owner,copied);
        check(prepared->deferred_records()==0&&permissions(copyfile)==mode&&contents(copyfile)==bytes,"stopped copied Prepare reconstructs exact pending identity without chmod or content writes");
        native::LinuxFileManager recovered(copied_db,owner,copied,prepared);
        check(recovered.reconcile_import(request.registration.operation).completed&&permissions(copyfile)==0600&&contents(copyfile)==bytes,"copied pending import explicitly reconciles through catalog transaction");
        check(copied_db.shutdown().succeeded(),"close copied import database");
    }
    check(db.start().succeeded(),"reopen original pending import database");
    if(import_without_birth&&fault==2){
        auto reopened=native::LinuxFilePayloadBindings::open(root);native::LinuxFileManager held(db,owner,root,reopened);
        check(!held.reconcile_import(request.registration.operation).completed,"no-birth changed ctime stays held until explicit Prepare");
    }
    {
        auto prepared=native::prepare_file_intake_bindings(db,owner,root);native::LinuxFileManager recovered(db,owner,root,prepared);
        check(recovered.reconcile_import(request.registration.operation).completed&&permissions(file)==0600&&contents(file)==bytes,"original explicit Prepare and reconcile preserve exact import");
        check(scalar(root/"data/sfx.db","SELECT coalesce(sum(upload_count+download_count),0) FROM sfx_file_statistics")==0,"Sysop import fabricates no caller credit");
    }
    check(db.shutdown().succeeded(),"final import storage shutdown");
}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"one fixture path under normal account");const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root)&&fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"new custody suite root");
    for(unsigned mode=0;mode<2;++mode){import_without_birth=mode!=0;for(int fault=-1;fault<3;++fault)exercise(root/(std::to_string(mode)+"-"+std::to_string(fault)),fault);}
    std::cout<<"PASS native import custody "<<checks<<" checks; normal 0644 copies, read-only preview, confirmed 0600 custody, publication faults and original/copied recovery with and without birth stamps\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL native import custody after "<<checks<<": "<<error.what()<<" errno="<<errno<<'\n';return 1;}}
