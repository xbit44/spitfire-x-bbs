#include "sfx/platform/linux/file_backup.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sqlite3.h>
#include <sys/stat.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;using Path=std::filesystem::path;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string unique_id(){LinuxSecureIdentifierGenerator generator;const auto id=generator.generate();std::string text;for(auto b:id.bytes){text+="0123456789abcdef"[b>>4];text+="0123456789abcdef"[b&15];}return text;}
std::string digest(const app::FilePayloadIdentity& identity){std::string out;for(const auto b:identity.sha256){out+="0123456789ABCDEF"[b>>4];out+="0123456789ABCDEF"[b&15];}return out;}
bool cancelled(const archive::CancelCheck& check){return check&&check();}
// A confirmed owner-selected restore previews physical destination bytes, not
// catalog authorization. A copied/recovered conflict may deliberately have a
// deferred portable identity. Inspect it without adopting or rewriting that
// identity; all ordinary Runtime reads keep their original binding guard.
class ContentInspectionBindings final:public IFilePayloadBindings {
public:
    explicit ContentInspectionBindings(std::shared_ptr<IFilePayloadBindings> paths):paths_(std::move(paths)){}
    app::FileObjectIdentity existing(const Path&,int fd)override{
        struct stat info{};require(fstat(fd,&info)==0&&S_ISREG(info.st_mode)&&info.st_nlink==1,"contents_inspection_identity_unavailable");
        return {std::to_string(info.st_dev),std::to_string(info.st_ino)};
    }
    app::FileObjectIdentity created(const Path&,int)override{throw std::runtime_error("contents_inspection_is_read_only");}
    void moved(const Path&,const Path&,int,const app::FileObjectIdentity&)override{throw std::runtime_error("contents_inspection_is_read_only");}
    void removed(const Path&,const app::FileObjectIdentity&)override{throw std::runtime_error("contents_inspection_is_read_only");}
    Path resolve_path(const Path& path)const override{return paths_->resolve_path(path);}
private:
    std::shared_ptr<IFilePayloadBindings> paths_;
};
FilePayloadDirectory directory(const Path& root,std::shared_ptr<IFilePayloadBindings> bindings,const app::FileConfiguration& config,std::int64_t id,const Path& collection){
    const auto found=std::find_if(config.areas.begin(),config.areas.end(),[id](const auto& a){return a.area.id.value==id;});require(found!=config.areas.end(),"file_backup_area_not_configured");
    std::vector<Path> others;for(const auto& a:config.areas)if(a.area.id.value!=id)others.push_back(std::filesystem::u8path(a.storage_path));
    return FilePayloadDirectory::area(root,std::filesystem::u8path(found->storage_path),others,
        {"bin","config","data","display","backup","logs","work","doors","botgate",collection},bindings);
}
std::vector<std::string> complete_names(const FilePayloadDirectory& directory,const archive::CancelCheck& cancel){
    require(!cancelled(cancel),"file_backup_cancelled");
    auto names=directory.strict_names();std::set<std::string> keys;
    for(const auto& name:names){const auto key=app::file_case_key(name);require(key&&keys.insert(*key).second,"file_backup_area_name_ambiguous");}
    require(!cancelled(cancel),"file_backup_cancelled");return names;
}
bool same_configuration(const app::FileConfiguration& a,const app::FileConfiguration& b){
    if(a.policy.catalog_revision!=b.policy.catalog_revision||a.areas.size()!=b.areas.size())return false;
    for(const auto& area:a.areas){const auto found=std::find_if(b.areas.begin(),b.areas.end(),[&](const auto& x){return x.area.id==area.area.id;});if(found==b.areas.end()||found->area.revision!=area.area.revision||found->storage_path!=area.storage_path)return false;}return true;
}
}
FileBackupSnapshot::FileBackupSnapshot(const FilePayloadDirectory& directory,std::string name)
    :directory_(directory),name_(std::move(name)),pin_(directory_.read(name_)),identity_(pin_->object_identity()){}
FileBackupSnapshot::~FileBackupSnapshot()=default;
bool FileBackupSnapshot::cleanup() noexcept{
    if(attempted_)return removed_;
    attempted_=true;pin_.reset();
    try{
        if(directory_.absent(name_))return removed_=true;
        auto exact=directory_.read(name_,true);const auto current=exact.object_identity();
        if(current.volume!=identity_.volume||current.file_id!=identity_.file_id)return false;
        exact.remove_exact();removed_=true;
    }catch(...){return false;}
    return removed_;
}
app::FileConfiguration read_backup_file_configuration(const Path& root,std::shared_ptr<IFilePayloadBindings> bindings){
    auto data=FilePayloadDirectory::private_store(root/"data",bindings);auto pin=data.read("sfx.db");
    sqlite3* db{};struct Close{sqlite3*& db;~Close(){if(db)sqlite3_close(db);}} close{db};
    require(sqlite3_open_v2(pin.path().u8string().c_str(),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,nullptr)==SQLITE_OK,"file_backup_database_open");
    require(sqlite3_exec(db,"PRAGMA query_only=ON; BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"file_backup_database_snapshot");
    struct Statement{sqlite3_stmt* p{};~Statement(){if(p)sqlite3_finalize(p);}} exists,policy,areas;
    require(sqlite3_prepare_v2(db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN('sfx_file_policy','sfx_file_areas','sfx_file_area_paths')",-1,&exists.p,nullptr)==SQLITE_OK&&sqlite3_step(exists.p)==SQLITE_ROW,"file_backup_schema_inspection");
    const auto tables=sqlite3_column_int(exists.p,0);app::FileConfiguration result;if(!tables)return result;require(tables==3,"file_backup_schema_incomplete");
    require(sqlite3_prepare_v2(db,"SELECT catalog_revision FROM sfx_file_policy WHERE singleton_id=1",-1,&policy.p,nullptr)==SQLITE_OK&&sqlite3_step(policy.p)==SQLITE_ROW,"file_backup_policy_read");result.policy.catalog_revision=sqlite3_column_int64(policy.p,0);require(result.policy.catalog_revision>0&&sqlite3_step(policy.p)==SQLITE_DONE,"file_backup_policy_invalid");
    require(sqlite3_prepare_v2(db,"SELECT a.area_id,a.revision,a.name,a.backup_contents,p.storage_path FROM sfx_file_areas a JOIN sfx_file_area_paths p ON p.area_id=a.area_id ORDER BY a.area_id",-1,&areas.p,nullptr)==SQLITE_OK,"file_backup_area_read");
    int status{};while((status=sqlite3_step(areas.p))==SQLITE_ROW){
        app::FileAreaConfiguration area;area.area.id.value=sqlite3_column_int64(areas.p,0);area.area.revision=sqlite3_column_int64(areas.p,1);
        const auto text=[&](int column){const auto n=sqlite3_column_bytes(areas.p,column);const auto* p=sqlite3_column_text(areas.p,column);require(p&&n>0&&n<=2048,"file_backup_database_text");return std::string(reinterpret_cast<const char*>(p),static_cast<std::size_t>(n));};
        area.area.name=text(2);const auto enabled=sqlite3_column_int(areas.p,3);area.area.backup_contents=enabled!=0;area.storage_path=text(4);
        require(result.areas.size()<10000&&area.area.id.value>0&&area.area.revision>0&&(enabled==0||enabled==1),"file_backup_database_area_invalid");result.areas.push_back(std::move(area));
    }
    require(status==SQLITE_DONE,"file_backup_database_interrupted");return result;
}
struct FileBackupCapture::Impl {
    Path root;std::shared_ptr<IFilePayloadBindings> bindings;app::FileConfiguration configuration;archive::CancelCheck cancel;
    std::vector<VerifiedBackupMember> predecessors;
    std::vector<FilePayloadDirectory> directories;
    std::vector<std::vector<std::string>> names;
    std::vector<FilePayloadLease> payloads;
    std::vector<archive::CreateBackupRequest::SourceOverride> sources;
    archive::ContentIndex index;
    std::optional<FilePayloadLease> index_pin;std::optional<app::FileObjectIdentity> index_identity;std::string index_name;
    bool bound{},cleaned{};
};
FileBackupCapture::FileBackupCapture(std::unique_ptr<Impl> p):impl_(std::move(p)){}
FileBackupCapture::~FileBackupCapture()=default;
std::unique_ptr<FileBackupCapture> FileBackupCapture::begin(const Path& root,std::shared_ptr<IFilePayloadBindings> bindings,
    const app::FileConfiguration& configuration,const BackupCollection& collection,archive::BackupKind kind,const archive::CancelCheck& cancel){
    auto p=std::make_unique<Impl>();p->root=root;p->bindings=bindings;p->configuration=configuration;p->cancel=cancel;
    const bool enabled=std::any_of(configuration.areas.begin(),configuration.areas.end(),[](const auto& a){return a.area.backup_contents;});
    if(enabled){p->predecessors=verified_backup_members(root,bindings,collection,kind,cancel);if(!p->predecessors.empty())verify_content_dependencies(p->predecessors,{p->predecessors.size()-1});}
    const VerifiedBackupMember* previous=p->predecessors.empty()?nullptr:&p->predecessors.back();std::set<std::string> included;
    for(const auto& a:configuration.areas){if(!a.area.backup_contents)continue;
        p->directories.push_back(directory(root,bindings,configuration,a.area.id.value,collection.directory));auto& dir=p->directories.back();
        p->names.push_back(complete_names(dir,cancel));p->index.areas.push_back({a.area.id.value,a.area.revision,a.area.name,a.storage_path});
        bool same_area=false;if(previous)for(const auto& old:previous->contents.areas)if(old.id==a.area.id.value&&old.configured_path==a.storage_path)same_area=true;
        for(const auto& name:p->names.back()){
            require(p->index.files.size()<100000&&!cancelled(cancel),"file_backup_cancelled_or_total_limit");
            p->payloads.push_back(dir.read(name));auto& lease=p->payloads.back();const auto identity=lease.identity([&](auto,auto){return !cancelled(cancel);});
            archive::ContentFile file{a.area.id.value,name,identity.bytes,digest(identity),{}, {},{}};file.member="FileContents/Objects/"+file.sha256;
            if(same_area){const auto old=std::find_if(previous->contents.files.begin(),previous->contents.files.end(),[&](const auto& f){return f.area==file.area&&f.name==file.name&&f.bytes==file.bytes&&f.sha256==file.sha256;});
                if(old!=previous->contents.files.end()){file.archive=old->archive.empty()?previous->path.filename().u8string():old->archive;file.archive_sha256=old->archive.empty()?previous->sha256:old->archive_sha256;}}
            if(file.archive.empty()&&included.insert(file.member).second)p->sources.push_back({file.member,lease.path()});
            p->index.files.push_back(std::move(file));
        }
    }
    return std::unique_ptr<FileBackupCapture>(new FileBackupCapture(std::move(p)));
}
void FileBackupCapture::bind(archive::ManagedBackupRequest& request,const app::FileConfiguration& current){
    auto& p=*impl_;require(!p.bound&&!p.cleaned&&same_configuration(p.configuration,current)&&request.installation_root==p.root&&request.file_contents.empty(),"file_backup_configuration_changed_or_reused");
    for(std::size_t n=0;n<p.directories.size();++n)require(complete_names(p.directories[n],p.cancel)==p.names[n],"file_backup_membership_changed_since_snapshot");
    // Include an empty index when all areas are Off. A later On therefore starts
    // a new complete baseline instead of inheriting an older disabled capture.
    const auto bytes=archive::encode_content_index(p.index);const auto work=FilePayloadDirectory::private_store(p.root/"work",p.bindings);
    p.index_name="file-backup-"+Path(unique_id()).u8string()+".index";
    {auto file=work.create(p.index_name,std::max<std::size_t>(1,bytes.size()));p.index_identity=file.object_identity();for(std::size_t at=0;at<bytes.size();at+=65536){const auto n=std::min<std::size_t>(65536,bytes.size()-at);file.append(bytes.data()+at,n);}(void)file.identity();}
    p.index_pin.emplace(work.read(p.index_name));request.file_contents=p.sources;request.file_contents.push_back({archive::file_contents_index,p.index_pin->path()});p.bound=true;
}
bool FileBackupCapture::cleanup()noexcept{
    if(!impl_||impl_->cleaned)return true;
    auto& p=*impl_;p.payloads.clear();p.directories.clear();p.predecessors.clear();p.index_pin.reset();
    try{if(p.index_identity){auto work=FilePayloadDirectory::private_store(p.root/"work",p.bindings);auto file=work.read(p.index_name,true);const auto identity=file.object_identity();require(identity.volume==p.index_identity->volume&&identity.file_id==p.index_identity->file_id,"file_backup_index_cleanup_identity_changed");file.remove_exact();}p.cleaned=true;return true;}catch(...){return false;}
}

struct FileContentRestore::Impl {
    Path root;std::shared_ptr<IFilePayloadBindings> bindings;app::FileConfiguration configuration;
    std::vector<VerifiedBackupMember> members;std::size_t point{};
    std::map<std::int64_t,FilePayloadDirectory> directories;
    std::vector<std::pair<std::int64_t,Path>> destinations;
    std::vector<FileContentRestoreItem> items;std::vector<archive::ContentFile> files;
    std::vector<FilePayloadLease> existing;
    bool consumed{};
};
FileContentRestore::FileContentRestore(std::unique_ptr<Impl> p):impl_(std::move(p)){}
FileContentRestore::~FileContentRestore()=default;
const std::vector<FileContentRestoreItem>& FileContentRestore::items()const noexcept{return impl_->items;}
const std::vector<std::pair<std::int64_t,Path>>& FileContentRestore::destinations()const noexcept{return impl_->destinations;}
std::unique_ptr<FileContentRestore> FileContentRestore::preview(const Path& root,std::shared_ptr<IFilePayloadBindings> bindings,
    const app::FileConfiguration& config,const BackupCollection& collection,const Path& point,
    const std::vector<std::int64_t>& selected,const archive::CancelCheck& cancel){
    require(!selected.empty()&&selected.size()<=10000,"file_restore_select_areas");std::set<std::int64_t> chosen(selected.begin(),selected.end());require(chosen.size()==selected.size(),"file_restore_duplicate_area");
    auto p=std::make_unique<Impl>();p->root=root;p->bindings=bindings;p->configuration=config;
    p->members=verified_backup_members(root,bindings,collection,collection.kind,cancel);
    const auto found=std::find_if(p->members.begin(),p->members.end(),[&](const auto& m){return m.path==point;});require(found!=p->members.end(),"file_restore_point_not_owned_or_verified");p->point=static_cast<std::size_t>(found-p->members.begin());
    verify_content_dependencies(p->members,{p->point});
    for(const auto id:selected){const auto a=std::find_if(found->contents.areas.begin(),found->contents.areas.end(),[&](const auto& v){return v.id==id;});require(a!=found->contents.areas.end(),"file_restore_area_not_in_point");
        auto destination=directory(root,bindings,config,id,collection.directory);p->destinations.emplace_back(id,destination.path());p->directories.emplace(id,std::move(destination));}
    for(const auto& f:found->contents.files)if(chosen.count(f.area)){
        require(!cancelled(cancel),"file_restore_preview_cancelled");auto& destination=p->directories.at(f.area);FileContentRestoreItem item{f.area,f.name,FileContentRestoreItem::State::missing};
        if(!destination.absent(f.name)){auto inspection=directory(root,std::make_shared<ContentInspectionBindings>(bindings),config,f.area,collection.directory);
            p->existing.push_back(inspection.read(f.name));const auto identity=p->existing.back().identity([&](auto,auto){return !cancelled(cancel);});item.state=identity.bytes==f.bytes&&digest(identity)==f.sha256?FileContentRestoreItem::State::identical:FileContentRestoreItem::State::conflict;}
        p->items.push_back(item);p->files.push_back(f);
    }
    return std::unique_ptr<FileContentRestore>(new FileContentRestore(std::move(p)));
}
FileContentRestoreResult FileContentRestore::execute(const app::FileConfiguration& config,const archive::CancelCheck& cancel){
    FileContentRestoreResult result;auto& p=*impl_;
    try{
        require(!p.consumed&&same_configuration(p.configuration,config),"file_restore_configuration_changed_or_reused");p.consumed=true;
        // Revalidate held existing objects before publishing any missing file.
        // A changed destination needs a new explicit preview, never overwrite.
        for(const auto& existing:p.existing)(void)existing.identity([&](auto,auto){return !cancelled(cancel);});
        for(std::size_t i=0;i<p.items.size();++i){const auto& item=p.items[i];const auto& f=p.files[i];require(!cancelled(cancel),"file_restore_cancelled");
            if(item.state==FileContentRestoreItem::State::identical){++result.identical;continue;}if(item.state==FileContentRestoreItem::State::conflict){++result.conflicts;continue;}
            auto& destination=p.directories.at(f.area);require(destination.absent(f.name),"file_restore_new_conflict_no_overwrite");
            auto source=p.members[p.point].path;if(!f.archive.empty())source=source.parent_path()/std::filesystem::u8path(f.archive);
            const auto temporary="sfx-restore-"+Path(unique_id()).u8string()+".partial";auto output=destination.create(temporary,std::max<std::uint64_t>(1,f.bytes));
            bool confirmed_cancel_before_publication{};
            try{
                const auto streamed=archive::stream_zip_member(source,f.member,f.bytes,[&](const auto* data,std::size_t count){output.append(data,count);return true;},cancel);
                confirmed_cancel_before_publication=streamed.error==archive::Error::cancelled;
                require(streamed.ok(),"file_restore_member_read_failed");const auto identity=output.identity([&](auto,auto){return !cancelled(cancel);});
                require(identity.bytes==f.bytes&&digest(identity)==f.sha256,"file_restore_member_hash_failed");output.rename_to(destination,f.name);++result.restored;
            }catch(...){
                // No async child remains here. Only a positively requested,
                // completed cancellation permits exact owned scratch cleanup.
                // Stream/hash/I/O uncertainty retains the partial for review.
                if(confirmed_cancel_before_publication){
                    try{output.remove_exact();result.diagnostic="file_restore_cancelled_owned_partial_removed";}
                    catch(...){result.diagnostic="file_restore_cancelled_partial_retained_review_destination";}
                }else result.diagnostic="file_restore_failed_partial_retained_review_destination";
                throw;
            }
        }
        result.completed=true;result.diagnostic="Contents restored without overwrite/delete. Catalog identities are unchanged; use File Manager Inspect/Reconcile before serving restored files.";
    }catch(const std::exception& e){if(result.diagnostic.empty())result.diagnostic=e.what();}catch(...){result.diagnostic="file_restore_incomplete_review_destination";}
    return result;
}
} // namespace sfx::platform::linux_host
