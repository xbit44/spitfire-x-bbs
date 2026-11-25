// Included with the payload lease implementation: the writer callback uses
// only preallocated descriptor/name capabilities and native system calls.
namespace {
std::string expiry_hex(const unsigned char* bytes,std::size_t count){
    std::string out(count*2,'0');
    for(std::size_t i=0;i<count;++i){out[i*2]="0123456789abcdef"[bytes[i]>>4];out[i*2+1]="0123456789abcdef"[bytes[i]&15];}
    return out;
}
bool expiry_same(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){
    return a.bytes==b.bytes&&a.sha256==b.sha256&&a.volume==b.volume&&a.file_id==b.file_id;
}
std::string expiry_record(const app::FtnRetainedOriginal& r,std::string_view name){
    return "SFX-LINUX-EXPIRY-1\n"+std::string(name)+"\n"+r.network+"\n"+
        expiry_hex(r.ingress.bytes.data(),r.ingress.bytes.size())+"\n"+r.native.volume+"\n"+
        r.native.file_id+"\n"+std::to_string(r.native.bytes)+"\n"+expiry_hex(r.native.sha256.data(),r.native.sha256.size())+"\n";
}
std::string expiry_container(const app::FtnRetainedOriginal& r){return ".sfx-expiry-"+expiry_hex(r.ingress.bytes.data(),r.ingress.bytes.size());}
bool expiry_absent(int parent,const char* name){
    struct stat info{};if(fstatat(parent,name,&info,AT_SYMLINK_NOFOLLOW)==0)return false;
    require(errno==ENOENT,"ftn_expiry_presence_failed");return true;
}
}
struct FtnProcessedExpiry::Impl {
    app::FtnRetainedOriginal original;std::shared_ptr<std::atomic_bool> cancelled;
    Observation observation{Observation::missing};std::unique_ptr<FilePayloadLease> lease;
    std::weak_ptr<app::FtnRetentionRemoval> sealed;
    struct Removal final:app::FtnRetentionRemoval::Native {
        std::unique_ptr<FilePayloadLease::Impl> held;
        std::shared_ptr<FilePayloadDirectory::Impl> parent,capsule;
        std::string original_name,container;
        Path original_path;app::FilePayloadIdentity expected;
        struct stat captured{};
        bool captured_name{},mapped{},unlinked{};
        Removal(std::unique_ptr<FilePayloadLease::Impl> value,const app::FtnRetainedOriginal& r):
            held(std::move(value)),parent(held->directory),original_name(held->name),
            container(expiry_container(r)),original_path(held->path),expected(r.native){}
        bool native_exact()const noexcept{
            struct stat directory{},named{},opened{};
            return capsule&&held->file&&fstatat(parent->directory.get(),container.c_str(),&directory,AT_SYMLINK_NOFOLLOW)==0&&
                same_object(directory,capsule->bound)&&S_ISDIR(directory.st_mode)&&directory.st_uid==geteuid()&&(directory.st_mode&077)==0&&
                fstatat(capsule->directory.get(),"payload",&named,AT_SYMLINK_NOFOLLOW)==0&&fstat(held->file.get(),&opened)==0&&
                same_stamp(captured,named)&&same_stamp(captured,opened)&&S_ISREG(named.st_mode)&&named.st_uid==geteuid()&&
                named.st_nlink==1&&(named.st_mode&077)==0;
        }
        void clean_empty(){
            parent->live();capsule->live();require(expiry_absent(capsule->directory.get(),"payload"),"ftn_expiry_capsule_not_empty");
            require(unlinkat(capsule->directory.get(),"intent",0)==0,"ftn_expiry_metadata_cleanup_failed");
            require(fsync(capsule->directory.get())==0,"ftn_expiry_metadata_flush_failed");
            require(unlinkat(parent->directory.get(),container.c_str(),AT_REMOVEDIR)==0&&fsync(parent->directory.get())==0,"ftn_expiry_capsule_cleanup_failed");
        }
        bool restore()noexcept{
            try{
                std::lock_guard<std::recursive_mutex> guard(operations);
                if(!captured_name||unlinked)return !captured_name;
                parent->live();capsule->live();DirectoryMutation lock(parent->directory.get());
                require(native_exact(),"ftn_expiry_restore_capture_changed");
                require(syscall(SYS_renameat2,capsule->directory.get(),"payload",parent->directory.get(),original_name.c_str(),1U)==0,"ftn_expiry_restore_name_occupied");
                captured_name=false;
                require(fsync(capsule->directory.get())==0&&fsync(parent->directory.get())==0,"ftn_expiry_restore_flush_failed");
                if(mapped)parent->bindings->moved(capsule->path/"payload",original_path,held->file.get(),held->logical);
                else parent->bindings->checkpoint(original_path,held->file.get(),expected);
                held->directory=parent;held->path=original_path;held->name=original_name;held->stamp=status(held->file.get());
                clean_empty();return true;
            }catch(...){return false;}
        }
        ~Removal()override{if(!unlinked)static_cast<void>(restore());}
        bool close_at_shutdown()noexcept override{
            if(!held->file)return true;
            // On Linux even EINTR releases the fd. Never retry a numeric fd
            // that another thread may already have reused.
            return close(held->file.release())==0;
        }
        app::FtnExpiryNativeResult disposition_and_close()noexcept override{
            using State=app::FtnExpiryNativeState;
            // No allocation, hashing, C++ lock or storage access in this path.
            if(!native_exact())return {State::disposition_failed,ESTALE};
            if(unlinkat(capsule->directory.get(),"payload",0)!=0)return {State::disposition_failed,static_cast<std::uint32_t>(errno)};
            unlinked=true;held->removed=true;captured_name=false;
            const int sync_error=fsync(capsule->directory.get())==0?0:errno;
            const int close_error=close(held->file.release())==0?0:errno;
            if(sync_error||close_error)return {State::close_unconfirmed,static_cast<std::uint32_t>(sync_error?sync_error:close_error)};
            return {State::closed,0};
        }
        void capture(const app::FtnExpiryIntent& intent){
            std::lock_guard<std::recursive_mutex> guard(operations);held->live();DirectoryMutation lock(parent->directory.get());
            require(mkdirat(parent->directory.get(),container.c_str(),0700)==0,"ftn_expiry_capsule_exists_or_create_failed");
            capsule=std::make_shared<FilePayloadDirectory::Impl>(parent->path/container,parent->bindings,false,false,true);
            const auto record=expiry_record(intent.original,original_name)+expiry_hex(intent.operation.bytes.data(),16)+"\n"+expiry_hex(intent.dispatch.bytes.data(),16)+"\n";
            publish_private_file(capsule->directory.get(),"intent",{record.begin(),record.end()},false);
            require(fsync(parent->directory.get())==0,"ftn_expiry_capsule_parent_flush_failed");
            // All destination names, paths and directory capabilities already
            // exist before capture. A crash leaves this deterministic journal.
            const auto destination=capsule->path/"payload";const std::string destination_name="payload";
            held->live();
            require(syscall(SYS_renameat2,parent->directory.get(),original_name.c_str(),capsule->directory.get(),"payload",1U)==0,"ftn_expiry_capture_failed");
            captured_name=true;captured=status(held->file.get());
            require(native_exact()&&same_object(captured,held->stamp)&&captured.st_size==held->stamp.st_size&&
                captured.st_mtim.tv_sec==held->stamp.st_mtim.tv_sec&&captured.st_mtim.tv_nsec==held->stamp.st_mtim.tv_nsec,"ftn_expiry_capture_substituted");
            require(fsync(parent->directory.get())==0&&fsync(capsule->directory.get())==0,"ftn_expiry_capture_flush_failed");
            parent->bindings->moved(original_path,destination,held->file.get(),held->logical);mapped=true;
            held->directory=capsule;held->path=destination;held->name=destination_name;held->stamp=captured;held->live();
        }
    };
};
FtnProcessedExpiry::FtnProcessedExpiry(std::unique_ptr<Impl> value):impl_(std::move(value)){}
FtnProcessedExpiry::~FtnProcessedExpiry()=default;
FtnProcessedExpiry::Observation FtnProcessedExpiry::observation()const noexcept{return impl_->observation;}
std::unique_ptr<FtnProcessedExpiry> FtnProcessedExpiry::inspect(const FtnInstallation& installation,
    const app::FtnRetainedOriginal& original,std::shared_ptr<std::atomic_bool> cancelled){
    require(cancelled&&!cancelled->load()&&original.ingress.valid()&&app::valid_ftn_code(original.network)&&installation.bindings,"ftn_expiry_context_invalid");
    static_cast<void>(open_owned_directory(installation.root));
    const auto directory=FilePayloadDirectory::private_store(installation.root/"ftn"/"processed"/original.network,installation.bindings);
    auto result=std::make_unique<Impl>();result->original=original;result->cancelled=std::move(cancelled);
    const auto container=expiry_container(original);
    if(!expiry_absent(directory.impl_->directory.get(),container.c_str())){
        auto capsule=FilePayloadDirectory::private_store(directory.path()/container,installation.bindings);
        const auto record_bytes=read_private_file(capsule.impl_->directory.get(),"intent",2048);
        const std::string record(record_bytes.begin(),record_bytes.end());const auto first=record.find('\n'),last=record.find('\n',first+1);
        require(first!=std::string::npos&&last!=std::string::npos,"ftn_expiry_recovery_record_invalid");
        const auto name=record.substr(first+1,last-first-1);require(app::valid_ftn_leaf_name(name),"ftn_expiry_recovery_name_invalid");
        const auto prefix=expiry_record(original,name);
        require(record.size()==prefix.size()+66&&record.compare(0,prefix.size(),prefix)==0,"ftn_expiry_recovery_binding_changed");
        if(!capsule.absent("payload")){
            // Existing authenticated map identity plus full content proof. If
            // capture crashed before the map publication, only the recorded
            // native move proof may reconcile it. No-birth ambiguity stays held.
            try{auto probe=capsule.read("payload");require(expiry_same(probe.identity(),original.native),"ftn_expiry_recovery_payload_changed");}
            catch(...){
                auto bindings=std::dynamic_pointer_cast<LinuxFilePayloadBindings>(installation.bindings);
                require(bool(bindings),"ftn_expiry_recovery_bindings_required");
                bindings->reconcile_recorded_move(directory.path()/name,capsule.path()/"payload",original.native);
            }
            auto lease=capsule.read("payload",true);require(expiry_same(lease.identity(),original.native),"ftn_expiry_recovery_payload_changed");
            Impl::Removal recovery(std::move(lease.impl_),original);recovery.parent=directory.impl_;recovery.capsule=capsule.impl_;
            recovery.original_name=name;recovery.original_path=directory.path()/name;recovery.captured=recovery.held->stamp;
            recovery.captured_name=true;recovery.mapped=true;
            require(recovery.restore(),"ftn_expiry_recovery_preserved_for_review");
        }else{
            if(!directory.absent(name)){
                try{auto probe=directory.read(name);require(expiry_same(probe.identity(),original.native),"ftn_expiry_recovery_payload_changed");}
                catch(...){
                    // Restoration may have renamed back before its fsync/map
                    // commit. Reconcile only that exact recorded reverse move;
                    // fallback ctime ambiguity remains held, never adopted.
                    auto bindings=std::dynamic_pointer_cast<LinuxFilePayloadBindings>(installation.bindings);
                    require(bool(bindings),"ftn_expiry_recovery_bindings_required");
                    bindings->reconcile_recorded_move(capsule.path()/"payload",directory.path()/name,original.native);
                }
                auto original_lease=directory.read(name,true);
                if(expiry_same(original_lease.identity(),original.native)){
                    // Crash before capture (or after exact restoration): only
                    // retire this empty journal after proving the original.
                    Impl::Removal empty(std::move(original_lease.impl_),original);
                    empty.parent=directory.impl_;empty.capsule=capsule.impl_;empty.clean_empty();
                }else{
                    result->observation=Observation::changed;
                    return std::unique_ptr<FtnProcessedExpiry>(new FtnProcessedExpiry(std::move(result)));
                }
            }else{
                // Missing after an interrupted unlink is never confirmed deletion.
                // Keep the journal and map for explicit uncertain reconciliation.
                result->observation=Observation::missing;
                return std::unique_ptr<FtnProcessedExpiry>(new FtnProcessedExpiry(std::move(result)));
            }
        }
    }
    const auto legacy=expiry_hex(original.ingress.bytes.data(),16)+".dat";std::vector<std::string> names;
    if(app::valid_ftn_leaf_name(original.wire_name)){
        names.push_back(original.wire_name);const auto dot=original.wire_name.rfind('.');
        const auto extension=dot==std::string::npos?std::string{}:original.wire_name.substr(dot);
        if(extension.size()<=16)names.push_back(original.wire_name.substr(0,dot).substr(0,128-33-extension.size())+"-"+legacy.substr(0,32)+extension);
    }
    names.push_back(legacy);
    for(const auto& name:names){
        if(directory.absent(name))continue;
        auto lease=directory.read(name,true);const auto actual=lease.identity([&](auto,auto){return !result->cancelled->load();});
        if(expiry_same(actual,original.native)){
            result->lease=std::make_unique<FilePayloadLease>(std::move(lease));result->observation=Observation::exact;break;
        }
        result->observation=Observation::changed;
    }
    return std::unique_ptr<FtnProcessedExpiry>(new FtnProcessedExpiry(std::move(result)));
}
std::shared_ptr<app::FtnRetentionRemoval> FtnProcessedExpiry::seal(const app::FtnExpiryIntent& intent){
    const auto& a=impl_->original;const auto& b=intent.original;
    require(impl_->observation==Observation::exact&&impl_->lease&&!impl_->cancelled->load()&&intent.operation.valid()&&intent.dispatch.valid()&&
        intent.prepared_utc>0&&intent.outcome==app::FtnExpiryOutcome::prepared&&a.ingress==b.ingress&&a.network==b.network&&
        a.successful_utc==b.successful_utc&&a.configuration_revision==b.configuration_revision&&a.protection_revision==b.protection_revision&&
        a.deadline==b.deadline&&expiry_same(a.native,b.native),"ftn_expiry_seal_binding_invalid");
    auto native=std::make_unique<Impl::Removal>(std::move(impl_->lease->impl_),a);impl_->lease.reset();native->capture(intent);
    auto result=std::shared_ptr<app::FtnRetentionRemoval>(new app::FtnRetentionRemoval(intent,std::move(native),impl_->cancelled));impl_->sealed=result;return result;
}
bool FtnProcessedExpiry::record_confirmed_removal(const std::shared_ptr<app::FtnRetentionRemoval>& proof)noexcept{
    try{
        const auto exact=impl_->sealed.lock();if(!proof||proof!=exact||proof->result().state!=app::FtnExpiryNativeState::closed)return false;
        auto* native=static_cast<Impl::Removal*>(proof->native_.get());
        if(!native||!native->unlinked||native->held->file)return false;
        native->parent->bindings->removed(native->capsule->path/"payload",native->held->logical);native->clean_empty();return true;
    }catch(...){return false;}
}
bool FtnProcessedExpiry::restore_after_dispatch(const std::shared_ptr<app::FtnRetentionRemoval>& proof)noexcept{
    if(!proof)return false;
    const auto state=proof->result().state;
    if(state!=app::FtnExpiryNativeState::not_invoked&&state!=app::FtnExpiryNativeState::disposition_failed)return false;
    auto* native=dynamic_cast<Impl::Removal*>(proof->native_.get());return native&&native->restore();
}
