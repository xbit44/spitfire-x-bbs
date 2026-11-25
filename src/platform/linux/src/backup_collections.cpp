#include "sfx/platform/linux/backup_collections.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/application/scheduler.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace fs=std::filesystem;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void running(const archive::CancelCheck& cancel){require(!cancel||!cancel(),"backup_retention_cancelled_preserved");}
std::string hex(const std::uint8_t* bytes,std::size_t size){
    std::string result;for(std::size_t i=0;i<size;++i){result+="0123456789ABCDEF"[bytes[i]>>4];result+="0123456789ABCDEF"[bytes[i]&15];}return result;
}
std::string digest(const application::FilePayloadIdentity& value){return hex(value.sha256.data(),value.sha256.size());}
std::string board_id(const fs::path& root){
    const auto key=load_portable_board_key(root);require(bool(key),"backup_installation_identity_unavailable");
    return hex(key.value->board.data(),key.value->board.size());
}
const char* pool_type(archive::BackupKind kind){
    if(kind==archive::BackupKind::setup_upgrade)return "setup-upgrade";
    require(kind==archive::BackupKind::manual||kind==archive::BackupKind::scheduled,"backup_retention_kind_invalid");return "ordinary";
}
bool type_matches(const std::string& type,archive::BackupKind kind){
    return kind==archive::BackupKind::setup_upgrade?type=="setup-upgrade":type=="manual"||type=="scheduled";
}
bool beneath(const fs::path& child,const fs::path& parent){
    const auto relative=child.lexically_relative(parent);return !relative.empty()&&*relative.begin()!="..";
}
std::vector<fs::path> candidates(const BackupCollection& collection,const archive::CancelCheck& cancel){
    require(collection.custody.has_value(),"backup_collection_unverified");collection.custody->verify();
    std::vector<fs::path> result;std::size_t visited{};
    for(const auto& entry:fs::directory_iterator(collection.directory)){
        running(cancel);require(++visited<=4096,"backup_inventory_limit_preserved");
        if(entry.path().extension()==".zip")result.push_back(entry.path());
    }
    collection.custody->verify();return result;
}
struct Member {
    VerifiedBackupMember verified;
    application::FilePayloadIdentity archive_identity,marker_identity;
};
std::optional<Member> managed_member(const fs::path& path,const BackupCollection& collection,
    const std::string& identity,archive::BackupKind kind,const archive::CancelCheck& cancel){
    try{
        require(path.parent_path()==collection.directory&&collection.custody.has_value(),"backup_member_wrong_collection");
        const auto& directory=*collection.custody;directory.verify();
        auto marker=directory.read(path.filename().string()+".sfx-owned");
        require(marker.byte_count()<=4096,"backup_marker_bound");const auto raw=marker.read(0,4096);
        std::istringstream input(std::string(raw.begin(),raw.end()));std::array<std::string,7> fields{};
        for(auto& field:fields)require(bool(std::getline(input,field)),"backup_marker_incomplete");
        require(input.peek()==std::char_traits<char>::eof()&&fields[0]=="SFX-BACKUP-OWNER-1"&&fields[1]==identity&&fields[2]==pool_type(kind)&&fields[6]==path.filename().string(),"backup_marker_not_owned");
        std::uint64_t sequence{};const auto number=std::from_chars(fields[3].data(),fields[3].data()+fields[3].size(),sequence);
        require(number.ec==std::errc{}&&number.ptr==fields[3].data()+fields[3].size()&&sequence!=0,"backup_sequence_invalid");
        auto file=directory.read(path.filename().string());const auto progress=[&](auto,auto){return !cancel||!cancel();};
        const auto payload=file.identity(progress);require(digest(payload)==fields[5],"backup_archive_changed_preserved");
        auto inspection=archive::inspect_backup(path,{},cancel);
        require(inspection.result.ok()&&type_matches(inspection.metadata.backup_type,kind)&&inspection.metadata.transaction_id==fields[4],"backup_archive_unverified");
        auto stamp=inspection.metadata.created;const auto& civil=stamp.local;
        require(civil.size()==19&&civil[4]=='-'&&civil[7]=='-'&&civil[10]=='T'&&civil[13]==':'&&civil[16]==':',"backup_timestamp_invalid");
        for(std::size_t i=0;i<civil.size();++i)if(i!=4&&i!=7&&i!=10&&i!=13&&i!=16)require(civil[i]>='0'&&civil[i]<='9',"backup_timestamp_invalid");
        stamp.filename_component=civil.substr(0,4)+civil.substr(5,2)+civil.substr(8,2)+"-"+civil.substr(11,2)+civil.substr(14,2)+civil.substr(17,2);
        const auto actual_kind=inspection.metadata.backup_type=="setup-upgrade"?archive::BackupKind::setup_upgrade:inspection.metadata.backup_type=="manual"?archive::BackupKind::manual:archive::BackupKind::scheduled;
        require(archive::managed_backup_filename(actual_kind,stamp,archive::PhysicalLayout::linux_lowercase_v1)==path.filename(),"backup_filename_binding_invalid");
        auto contents=archive::inspect_content_index(path,inspection);
        const auto current=file.identity(progress);require(current.bytes==payload.bytes&&current.sha256==payload.sha256,"backup_changed_during_inspection");
        Member member;member.archive_identity=payload;member.marker_identity=marker.identity(progress);
        auto& verified=member.verified;verified.path=path;verified.sequence=sequence;verified.sha256=fields[5];verified.inspection=std::move(inspection);verified.contents=std::move(contents);
        verified.pins.push_back(std::move(file));verified.pins.push_back(std::move(marker));directory.verify();return member;
    }catch(const std::exception&){return {};}
}
void delete_exact(const BackupCollection& collection,const std::string& name,const application::FilePayloadIdentity& expected,
    const archive::CancelCheck& cancel){
    running(cancel);auto file=collection.custody->read(name,true);const auto current=file.identity([&](auto,auto){return !cancel||!cancel();});
    require(current.volume==expected.volume&&current.file_id==expected.file_id&&current.bytes==expected.bytes&&current.sha256==expected.sha256,"backup_retention_member_changed_preserved");
    running(cancel);file.remove_exact();
}
}
BackupCollection open_backup_collection(const fs::path& root,std::shared_ptr<IFilePayloadBindings> bindings,
    archive::BackupKind kind,const std::string& destination,bool create){
    (void)pool_type(kind);require(bindings&&application::valid_backup_destination_text(destination),"backup_destination_syntax_invalid");
    BackupCollection result;result.kind=kind;result.directory=kind==archive::BackupKind::setup_upgrade?root/"backup/upgrades":destination.empty()?root/"backup/scheduled":fs::u8path(destination);
    // Foreign Windows settings remain readable but cannot become a relative
    // Linux folder or silently switch to the default destination.
    require(result.directory.is_absolute()&&application::valid_backup_destination_text(result.directory.string())&&result.directory.string().size()<=2048,"backup_destination_native_absolute_required");
    require(!beneath(root,result.directory)&&(!beneath(result.directory,root)||(beneath(result.directory,root/"backup")&&result.directory!=root/"backup")),"backup_destination_live_role_overlap");
    struct stat info{};
    if(lstat(result.directory.c_str(),&info)!=0){
        require(errno==ENOENT&&create,"backup_destination_missing_no_fallback");
        auto parent=open_owned_directory(result.directory.parent_path(),false);
        require(mkdirat(parent.get(),result.directory.filename().c_str(),0700)==0,"backup_destination_create_failed");
        require(fsync(parent.get())==0,"backup_destination_creation_unconfirmed_preserved");
    }
    result.custody.emplace(FilePayloadDirectory::private_store(result.directory,std::move(bindings)));result.custody->verify();return result;
}
namespace {
void secure_new_output(const BackupCollection& collection,const fs::path& path,bool archive){
    require(collection.custody&&path.parent_path()==collection.directory&&(!archive||path.extension()==".zip"),"backup_new_publication_wrong_collection");
    collection.custody->verify();auto directory=open_owned_directory(collection.directory);
    if(archive)require(collection.custody->absent(path.filename().string()+".sfx-owned"),"backup_existing_owned_archive_not_repaired");
    FileDescriptor file(openat(directory.get(),path.filename().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));struct stat before{},named{},after{};
    require(file&&fstat(file.get(),&before)==0&&S_ISREG(before.st_mode)&&before.st_uid==geteuid()&&before.st_nlink==1&&before.st_size>0,"backup_new_publication_object_invalid");
    require(fstatat(directory.get(),path.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&named.st_dev==before.st_dev&&named.st_ino==before.st_ino,"backup_new_publication_name_changed");
    // libarchive's generic filename writer uses the process umask. The held
    // destination is already0700, so bytes are private throughout; tighten the
    // newly generated leaf before ownership recording, restore use or pruning.
    require(fchmod(file.get(),0600)==0&&fsync(file.get())==0&&fsync(directory.get())==0,"backup_new_publication_protection_unconfirmed");
    require(fstat(file.get(),&after)==0&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&private_regular_file(file.get(),UINT64_MAX),"backup_new_publication_changed");
    require(fstatat(directory.get(),path.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&named.st_dev==before.st_dev&&named.st_ino==before.st_ino,"backup_new_publication_name_changed");
    collection.custody->verify();
}
}
void secure_new_backup_archive(const BackupCollection& collection,const fs::path& path){secure_new_output(collection,path,true);}
void secure_new_backup_snapshot(const FilePayloadDirectory& directory,std::string_view name){
    require(name.rfind("scheduler-database-",0)==0&&valid_private_name(name),"backup_generated_snapshot_name_invalid");
    BackupCollection collection;collection.directory=directory.path();collection.custody=directory;
    secure_new_output(collection,directory.path()/std::string(name),false);
}
std::vector<VerifiedBackupMember> verified_backup_members(const fs::path& root,std::shared_ptr<IFilePayloadBindings> bindings,
    const BackupCollection& collection,archive::BackupKind kind,const archive::CancelCheck& cancel){
    require(bindings&&std::string(pool_type(kind))==pool_type(collection.kind),"backup_collection_pool_mismatch");const auto identity=board_id(root);std::vector<VerifiedBackupMember> members;
    for(const auto& path:candidates(collection,cancel))if(auto value=managed_member(path,collection,identity,kind,cancel))members.push_back(std::move(value->verified));
    running(cancel);std::sort(members.begin(),members.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});
    for(std::size_t i=1;i<members.size();++i)require(members[i-1].sequence!=members[i].sequence,"file_backup_sequence_ambiguous");
    return members;
}
std::vector<std::size_t> content_dependency_closure(const std::vector<VerifiedBackupMember>& members,const std::vector<std::size_t>& selected){
    std::set<std::size_t> retained;std::vector<std::size_t> pending=selected;
    while(!pending.empty()){
        const auto n=pending.back();pending.pop_back();require(n<members.size(),"file_backup_restore_point_invalid");if(!retained.insert(n).second)continue;
        const auto& item=members[n];
        for(const auto& content:item.contents.files){if(content.archive.empty())continue;
            const auto found=std::find_if(members.begin(),members.end(),[&](const auto& m){return m.path.filename().u8string()==content.archive;});
            require(found!=members.end()&&found->sequence<item.sequence&&found->sha256==content.archive_sha256,"file_backup_dependency_missing_or_changed");
            const auto payload=std::find_if(found->inspection.files.begin(),found->inspection.files.end(),[&](const auto& f){return f.path==content.member;});
            require(payload!=found->inspection.files.end()&&payload->size==content.bytes&&payload->sha256==content.sha256,"file_backup_dependency_payload_mismatch");
            pending.push_back(static_cast<std::size_t>(found-members.begin()));
        }
    }
    return {retained.begin(),retained.end()};
}
void verify_content_dependencies(const std::vector<VerifiedBackupMember>& members,const std::vector<std::size_t>& selected){
    auto points=selected;if(points.empty())for(std::size_t n=0;n<members.size();++n)points.push_back(n);
    (void)content_dependency_closure(members,points);
}
std::size_t finish_backup_collection(const fs::path& root,std::shared_ptr<IFilePayloadBindings> bindings,
    const BackupCollection& collection,const fs::path& published,archive::BackupKind kind,std::uint32_t retained,
    archive::CancelCheck cancel,std::function<void(std::size_t,std::size_t)> progress){
    require(bindings&&collection.custody&&retained>=1&&retained<=365&&published.parent_path()==collection.directory,"backup_retention_request_invalid");
    require(std::string(pool_type(kind))==pool_type(collection.kind),"backup_collection_pool_mismatch");
    const auto identity=board_id(root);std::vector<Member> members;std::uint64_t sequence{};auto paths=candidates(collection,cancel);
    paths.erase(std::remove(paths.begin(),paths.end(),published),paths.end());std::size_t completed{};if(progress)progress(0,paths.size());
    for(const auto& path:paths){
        if(auto member=managed_member(path,collection,identity,kind,cancel)){sequence=std::max(sequence,member->verified.sequence);members.push_back(std::move(*member));}
        running(cancel);if(progress)progress(++completed,paths.size());
    }
    require(sequence!=UINT64_MAX,"backup_retention_sequence_exhausted");
    const auto& directory=*collection.custody;auto file=directory.read(published.filename().string());
    const auto first=file.identity([&](auto,auto){return !cancel||!cancel();});const auto checked=archive::inspect_backup(published,{},cancel);
    require(checked.result.ok()&&type_matches(checked.metadata.backup_type,kind),"backup_publication_unverified");
    (void)archive::inspect_content_index(published,checked);
    const auto last=file.identity([&](auto,auto){return !cancel||!cancel();});require(first.bytes==last.bytes&&first.sha256==last.sha256,"backup_publication_changed");
    const std::string marker="SFX-BACKUP-OWNER-1\n"+identity+"\n"+pool_type(kind)+"\n"+std::to_string(sequence+1)+"\n"+checked.metadata.transaction_id+"\n"+digest(last)+"\n"+published.filename().string()+"\n";
    {auto output=directory.create(published.filename().string()+".sfx-owned",marker.size());output.append(reinterpret_cast<const std::uint8_t*>(marker.data()),marker.size());output.flush();}
    auto current=managed_member(published,collection,identity,kind,cancel);require(current.has_value(),"backup_owner_marker_verification_failed");
    members.push_back(std::move(*current));std::sort(members.begin(),members.end(),[](const auto& a,const auto& b){return a.verified.sequence<b.verified.sequence;});
    for(std::size_t i=1;i<members.size();++i)require(members[i-1].verified.sequence!=members[i].verified.sequence,"backup_duplicate_sequence_preserved");
    std::vector<VerifiedBackupMember> verified;for(auto& member:members)verified.push_back(std::move(member.verified));
    std::vector<std::size_t> points;for(std::size_t i=verified.size()>retained?verified.size()-retained:0;i<verified.size();++i)points.push_back(i);
    const auto closure=content_dependency_closure(verified,points);const std::set<std::size_t> keep(closure.begin(),closure.end());
    for(std::size_t i=0;i<verified.size();++i)if(!keep.count(i))verified[i].pins.clear();
    std::size_t removed{};
    for(std::size_t i=0;i<verified.size();++i){if(keep.count(i))continue;
        delete_exact(collection,verified[i].path.filename().string(),members[i].archive_identity,cancel);
        delete_exact(collection,verified[i].path.filename().string()+".sfx-owned",members[i].marker_identity,cancel);++removed;
    }
    return removed;
}
} // namespace sfx::platform::linux_host
