#include "sfx/platform/linux/file_manager.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/archive/file_zip.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace sfx::platform::linux_host {
namespace app=application;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::filesystem::path checked_root(const std::filesystem::path& root){require(root.is_absolute()&&root==root.lexically_normal(),"file_manager_root_invalid");(void)open_owned_directory(root);return root;}
std::uint64_t monotonic_millis(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
using Action=app::FileOperationAction;
bool same(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.bytes==b.bytes&&a.sha256==b.sha256&&a.volume==b.volume&&a.file_id==b.file_id;}
bool object(const app::FileObjectIdentity& a,const app::FileObjectIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id;}
std::string key(app::Identifier128 id){std::string r;for(auto b:id.bytes){r.push_back("0123456789abcdef"[b>>4]);r.push_back("0123456789abcdef"[b&15]);}return r;}
}
LinuxFileManager::LinuxFileManager(app::IFileStorage& db,app::FileConfigurationActor actor,
    std::filesystem::path root,std::shared_ptr<IFilePayloadBindings> bindings,Boundary boundary):storage_(db),actor_(std::move(actor)),
    installation_(checked_root(root)),bindings_(std::move(bindings)),
    removed_(FilePayloadDirectory::private_store(installation_/"data"/"fileremoved",bindings_)),boundary_(std::move(boundary)){
    require(storage_.file_configuration(actor_).succeeded(),"file_manager_authority_required");
}
FilePayloadDirectory LinuxFileManager::area(const app::FileConfiguration& config,app::FileAreaId id)const{
    const auto found=std::find_if(config.areas.begin(),config.areas.end(),[id](const auto& a){return a.area.id==id;});
    require(found!=config.areas.end(),"file_manager_area_unavailable");
    std::vector<std::filesystem::path> others;for(const auto& a:config.areas)if(!(a.area.id==id))others.push_back(std::filesystem::u8path(a.storage_path));
    return FilePayloadDirectory::area(installation_,std::filesystem::u8path(found->storage_path),others,
        {"bin","config","data","display","backup","logs","work","doors","botgate"},bindings_);
}
struct FileUnregisteredScan::Impl {
    app::IFileStorage& storage;
    app::FileConfigurationActor actor;
    app::FileAreaConfiguration area;
    FilePayloadEnumeration cursor;
    bool failed{};
    Impl(app::IFileStorage& db,app::FileConfigurationActor a,app::FileAreaConfiguration ar,FilePayloadEnumeration c)
        :storage(db),actor(std::move(a)),area(std::move(ar)),cursor(std::move(c)){}
    void current() const {
        const auto config=storage.file_configuration(actor);require(config&&config.value,"file_discovery_authority_changed");
        const auto found=std::find_if(config.value->areas.begin(),config.value->areas.end(),[this](const auto& candidate){return candidate.area.id==area.area.id;});
        require(found!=config.value->areas.end()&&found->area.revision==area.area.revision&&found->storage_path==area.storage_path,"file_discovery_configuration_changed");
    }
};
FileUnregisteredScan::FileUnregisteredScan(std::unique_ptr<Impl> p):impl_(std::move(p)){}
FileUnregisteredScan::~FileUnregisteredScan()=default;
FileUnregisteredScan::FileUnregisteredScan(FileUnregisteredScan&&) noexcept=default;
FileUnregisteredScan& FileUnregisteredScan::operator=(FileUnregisteredScan&&) noexcept=default;
FilePayloadNamePage FileUnregisteredScan::next(std::size_t limit,const std::function<bool()>& keep_going){
    require(impl_&&!impl_->failed&&limit>=1&&limit<=100,"file_discovery_invalid_cursor_or_limit");
    try{
        impl_->current();auto page=impl_->cursor.next(limit,keep_going);
        const auto status=impl_->storage.file_name_status(impl_->actor,impl_->area.area.id,page.names);
        require(status&&status.value&&status.value->size()==page.names.size(),"file_discovery_catalog_unavailable");
        page.names.clear();for(const auto& item:*status.value)if(!item.registered.value&&!item.pending)page.names.push_back(item.filename);
        impl_->current();return page;
    }catch(...){impl_->failed=true;throw;}
}
FileUnregisteredScan LinuxFileManager::find_unregistered(app::FileAreaId id){
    const auto config=storage_.file_configuration(actor_);require(config&&config.value,"file_discovery_authority_required");
    const auto found=std::find_if(config.value->areas.begin(),config.value->areas.end(),[id](const auto& candidate){return candidate.area.id==id;});
    require(found!=config.value->areas.end(),"file_discovery_area_unavailable");
    auto directory=area(*config.value,id);
    return FileUnregisteredScan(std::make_unique<FileUnregisteredScan::Impl>(storage_,actor_,*found,directory.enumerate()));
}
FileReconciliationPage LinuxFileManager::inspect_files(const app::FileManagementQuery& query,app::FileId after,std::size_t limit,const Progress& progress){
    const auto config=storage_.file_configuration(actor_);require(config&&config.value,"file_reconciliation_authority_required");
    const auto records=storage_.managed_file_page(actor_,query,after,limit);require(records&&records.value,"file_reconciliation_catalog_unavailable");
    FileReconciliationPage result;result.next=records.value->next;
    for(const auto& record:records.value->files){
        FileReconciliationEntry item;item.record=record;
        if(record.operation_pending)item.presence=FilePayloadPresence::pending;
        else if(record.file.state==app::FileCatalogState::removed)item.presence=FilePayloadPresence::removed;
        else{
            bool cancelled{};
            try{
                const auto directory=area(*config.value,record.file.area);
                if(directory.absent(app::file_managed_payload_name(record)))item.presence=FilePayloadPresence::missing;
                else{auto payload=directory.read(app::file_managed_payload_name(record));const auto identity=payload.identity([&](auto done,auto total){if(progress&&!progress(done,total)){cancelled=true;return false;}return true;});item.presence=same(identity,record.payload)?FilePayloadPresence::present:FilePayloadPresence::changed;}
            }catch(...){if(cancelled)throw;item.presence=FilePayloadPresence::unavailable;}
        }
        require(!progress||progress(0,0),"file_reconciliation_cancelled");
        result.entries.push_back(std::move(item));
    }
    // Do not publish a private preview after live authority/configuration changed.
    const auto current=storage_.file_configuration(actor_);require(current&&current.value&&current.value->policy.catalog_revision==config.value->policy.catalog_revision,"file_reconciliation_configuration_changed");
    return result;
}
NativeFileOperationOutcome LinuxFileManager::hold(app::Identifier128 id){
    const std::string text="Operation not committed; source and destination retained for explicit reconciliation.";
    return {false,false,storage_.hold_file_operation(actor_,id,text).succeeded(),{},text};
}
NativeFileOperationOutcome LinuxFileManager::execute(const app::FileOperationRequest& request,const Progress& progress){
    // Ordinary collisions and same-path aliases are rejected before reserving
    // a mutation. CREATE_NEW/rename still close races after this read-only check.
    if(!storage_.file_operation(actor_,request.operation)){
        try{
            const auto config=storage_.file_configuration(actor_);const auto source=storage_.managed_file(actor_,request.file);
            require(config&&config.value&&source&&source.value,"file_manager_preview_unavailable");
            if(request.destination.value&&!(request.action==Action::restore&&source.value->file.state==app::FileCatalogState::missing)){
                const auto destination=area(*config.value,request.destination);
                require(destination.absent(request.filename),"file_manager_destination_exists");
                const auto source_dir=source.value->removed_key.empty()?area(*config.value,source.value->file.area):removed_;
                const auto source_name=source.value->removed_key.empty()?app::file_managed_payload_name(*source.value):source.value->removed_key;
                require((source_dir.path()/std::filesystem::u8path(source_name))!=(destination.path()/std::filesystem::u8path(request.filename)),"file_manager_same_path_refused");
            }
        }catch(...){return {false,false,false,{},"Operation refused; destination exists or paths could not be safely verified."};}
    }
    const auto prepared=storage_.prepare_file_operation(actor_,request);
    if(!prepared||!prepared.value)return {false,false,false,{},"Operation refused; review current metadata, duplicate warning and destination."};
    return run(*prepared.value,false,progress);
}
NativeFileOperationOutcome LinuxFileManager::reconcile(app::Identifier128 id,const Progress& progress){
    const auto op=storage_.file_operation(actor_,id);
    if(!op||!op.value)return {false,false,false,{},"Operation unavailable."};
    return run(*op.value,true,progress);
}
void LinuxFileManager::reconcile_removal(const std::filesystem::path& path,const app::FilePayloadIdentity& payload){
    const auto map=std::dynamic_pointer_cast<LinuxFilePayloadBindings>(bindings_);
    // Outside sources/areas use native custody; only managed roles have map rows.
    if(map&&map->can_move(path,removed_.path()/"receipt"))map->reconcile_recorded_removal(path,payload);
}
NativeFileOperationOutcome LinuxFileManager::run(const app::FileOperation& op,bool recovery,const Progress& progress){
    if(op.state==app::FileOperationState::completed)return {true,false,true,op.result,"Operation already committed."};
    if(op.state==app::FileOperationState::cancelled)return {false,true,true,{},"Operation already cancelled."};
    const auto& r=op.request;const auto action=r.action;
    const auto at=[this](FileManagerBoundary b){if(boundary_)boundary_(b);};
    try{
        const auto config=storage_.file_configuration(actor_);require(config&&config.value,"file_manager_configuration_unavailable");
        auto source_dir=op.source.removed_key.empty()?area(*config.value,op.source.file.area):removed_;
        const auto source_name=op.source.removed_key.empty()?app::file_managed_payload_name(op.source):op.source.removed_key;
        std::optional<FilePayloadDirectory> dest_dir;std::string dest_name;
        if(r.destination.value){dest_dir=area(*config.value,r.destination);dest_name=r.filename;}
        else if(action==Action::remove){dest_dir=removed_;dest_name=key(r.operation);}
        app::FileOperationCompletion completion;completion.source=op.source.payload;
        const bool missing=op.source.file.state==app::FileCatalogState::missing;
        if(action==Action::mark_missing||(action==Action::metadata&&missing)||(action==Action::purge&&missing)){
            require(source_dir.absent(source_name),"file_manager_source_not_absent");completion.source_absent=true;
        }else if(action==Action::restore&&missing){
            auto source=source_dir.read(source_name);const auto identity=source.identity(progress);require(same(identity,op.source.payload),"file_manager_reappeared_identity_changed");
            completion.destination=identity;completion.destination_present=true;at(FileManagerBoundary::before_commit);
            const auto committed=storage_.commit_file_operation(actor_,r.operation,completion);
            if(!committed||!committed.value)return hold(r.operation);
            return {true,false,true,committed.value->result,"Restored exact missing file."};
        }else{
            const bool source_absent=source_dir.absent(source_name);
            std::optional<FilePayloadLease> source,dest;
            if(!source_absent){
                source.emplace(source_dir.read(source_name,action!=Action::metadata&&action!=Action::copy));
                require(same(source->identity(progress),op.source.payload),"file_manager_source_identity_changed");
            }else require((recovery||action==Action::purge)&&action!=Action::metadata&&action!=Action::copy,"file_manager_source_missing");
            at(FileManagerBoundary::before_change);
            if(action==Action::metadata){completion.destination=op.source.payload;completion.destination_present=true;}
            else if(action==Action::purge){if(source){source->remove_exact();source.reset();}else reconcile_removal(source_dir.path()/source_name,op.source.payload);require(source_dir.absent(source_name),"file_manager_purge_absence_unconfirmed");completion.source_absent=true;}
            else{
                require(dest_dir.has_value(),"file_manager_destination_missing");
                require((source_dir.path()/std::filesystem::u8path(source_name))!=(dest_dir->path()/std::filesystem::u8path(dest_name)),"file_manager_same_path_refused");
                if(!dest_dir->absent(dest_name)){
                    require(recovery,"file_manager_existing_destination_refused");
                    if(!source&&action!=Action::copy&&(!op.placed_object||object(*op.placed_object,{op.source.payload.volume,op.source.payload.file_id}))){
                        const auto map=std::dynamic_pointer_cast<LinuxFilePayloadBindings>(bindings_);
                        if(map&&map->can_move(source_dir.path()/source_name,removed_.path()/"receipt")&&map->can_move(dest_dir->path()/dest_name,removed_.path()/"receipt"))
                            map->reconcile_recorded_move(source_dir.path()/source_name,dest_dir->path()/dest_name,op.source.payload);
                    }
                    dest.emplace(dest_dir->read(dest_name));
                    const auto native=dest->object_identity();
                    require((op.placed_object&&object(native,*op.placed_object))||(!source&&native.volume==op.source.payload.volume&&native.file_id==op.source.payload.file_id),"file_manager_destination_not_operation_owned");
                }else{
                    require(source.has_value(),"file_manager_both_paths_absent");
                    if(action!=Action::copy&&source->can_rename_to(*dest_dir)){
                        source->rename_to(*dest_dir,dest_name);dest.emplace(std::move(*source));source.reset();at(FileManagerBoundary::after_rename);
                    }else{
                        dest.emplace(dest_dir->create(dest_name,(std::max)(std::uint64_t{1},op.source.payload.bytes)));
                        require(storage_.bind_file_operation_destination(actor_,r.operation,dest->object_identity()).succeeded(),"file_manager_destination_journal_failed");
                        at(FileManagerBoundary::destination_created);
                        for(std::uint64_t offset=0;offset<op.source.payload.bytes;){
                            require(!progress||progress(offset,op.source.payload.bytes),"file_manager_copy_cancelled");
                            const auto bytes=source->read(offset);require(!bytes.empty(),"file_manager_copy_short_read");dest->append(bytes.data(),bytes.size());offset+=bytes.size();
                        }
                    }
                }
                completion.destination=dest->identity(progress);completion.destination_present=true;
                require(completion.destination.bytes==op.source.payload.bytes&&completion.destination.sha256==op.source.payload.sha256,"file_manager_destination_content_mismatch");
                require(storage_.bind_file_operation_destination(actor_,r.operation,dest->object_identity()).succeeded(),"file_manager_placement_journal_failed");
                at(FileManagerBoundary::after_copy);
                if(action!=Action::copy&&source){source->remove_exact();source.reset();}
                else if(action!=Action::copy&&source_absent&&op.placed_object&&!object(*op.placed_object,{op.source.payload.volume,op.source.payload.file_id}))
                    reconcile_removal(source_dir.path()/source_name,op.source.payload);
                completion.source_absent=action!=Action::copy;
                require(!completion.source_absent||source_dir.absent(source_name),"file_manager_old_path_not_absent");
            }
            // Keep verified source/destination handles alive across SQL commit.
            at(FileManagerBoundary::after_change);at(FileManagerBoundary::before_commit);
            const auto committed=storage_.commit_file_operation(actor_,r.operation,completion);
            if(!committed||!committed.value)return hold(r.operation);
            return {true,false,true,committed.value->result,"Operation committed."};
        }
        at(FileManagerBoundary::before_commit);const auto committed=storage_.commit_file_operation(actor_,r.operation,completion);
        if(!committed||!committed.value)return hold(r.operation);
        return {true,false,true,committed.value->result,"Verified absence committed."};
    }catch(...){return hold(r.operation);}
}
NativeFileOperationOutcome LinuxFileManager::cancel_unchanged(app::Identifier128 id,const Progress& progress){
    const auto found=storage_.file_operation(actor_,id);if(!found||!found.value)return {false,false,false,{},"Operation unavailable."};
    const auto& op=*found.value;if(op.state==app::FileOperationState::cancelled)return {false,true,true,{},"Operation already cancelled."};
    if(op.state==app::FileOperationState::completed)return {true,false,true,op.result,"Completed operation cannot be cancelled."};
    try{
        const auto config=storage_.file_configuration(actor_);require(config&&config.value,"file_manager_configuration_unavailable");
        auto source_dir=op.source.removed_key.empty()?area(*config.value,op.source.file.area):removed_;
        const auto name=op.source.removed_key.empty()?app::file_managed_payload_name(op.source):op.source.removed_key;
        app::FileOperationCompletion proof;proof.source=op.source.payload;std::optional<FilePayloadLease> source;
        if(op.source.file.state==app::FileCatalogState::missing){require(source_dir.absent(name),"file_manager_missing_source_changed");proof.source_absent=true;}
        else{source.emplace(source_dir.read(name));require(same(source->identity(progress),op.source.payload),"file_manager_source_not_unchanged");}
        if(op.request.destination.value)require(area(*config.value,op.request.destination).absent(op.request.filename),"file_manager_destination_requires_reconciliation");
        else if(op.request.action==Action::remove)require(removed_.absent(key(id)),"file_manager_removed_payload_requires_reconciliation");
        require(storage_.cancel_file_operation(actor_,id,proof).succeeded(),"file_manager_cancellation_unconfirmed");
        return {false,true,true,{},"Cancelled; original file unchanged."};
    }catch(...){return hold(id);}
}
#include "file_import_manager.inc"
#include "file_intake_purge_manager.inc"
#include "file_bulk_manager.inc"
#include "file_manager_prepare.inc"
} // namespace sfx::platform::linux_host
