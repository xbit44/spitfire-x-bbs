// Private per-operation VFS, registered NON-default. Do not change SQLite's
// process-global temp-store directory or let private rows reach public OS temp.
class PrivateCompactionVfs final {
public:
    PrivateCompactionVfs(const std::filesystem::path& folder,const IStoragePathPolicy& policy):folder_(folder),policy_(policy){
        delegate_=sqlite3_vfs_find(nullptr);if(!delegate_)throw internal::StorageFailure(application::StorageErrorCode::internal,"compaction VFS missing",SQLITE_ERROR);
        std::array<unsigned char,16> token{};if(sodium_init()<0)throw internal::StorageFailure(application::StorageErrorCode::internal,"compaction entropy",SQLITE_ERROR);
        randombytes_buf(token.data(),token.size());constexpr char hex[]="0123456789ABCDEF";name_="sfx-private-compaction-";
        for(const auto byte:token){name_.push_back(hex[byte>>4]);name_.push_back(hex[byte&15]);}
        vfs_=*delegate_;vfs_.pNext=nullptr;vfs_.zName=name_.c_str();vfs_.pAppData=this;
        vfs_.xOpen=open;vfs_.xDelete=remove;vfs_.xAccess=access;vfs_.xFullPathname=full_path;
        vfs_.xDlOpen=dl_open;vfs_.xDlError=dl_error;vfs_.xDlSym=dl_sym;vfs_.xDlClose=dl_close;
        vfs_.xRandomness=randomness;vfs_.xSleep=sleep;vfs_.xCurrentTime=current_time;vfs_.xGetLastError=last_error;
        if(vfs_.iVersion>=2 && delegate_->xCurrentTimeInt64)vfs_.xCurrentTimeInt64=current_time64;
        if(vfs_.iVersion>=3){vfs_.xSetSystemCall=delegate_->xSetSystemCall?set_system:nullptr;vfs_.xGetSystemCall=delegate_->xGetSystemCall?get_system:nullptr;vfs_.xNextSystemCall=delegate_->xNextSystemCall?next_system:nullptr;}
        if(sqlite3_vfs_register(&vfs_,0)!=SQLITE_OK)throw internal::StorageFailure(application::StorageErrorCode::internal,"register private compaction VFS",SQLITE_ERROR);
        registered_=true;
    }
    ~PrivateCompactionVfs(){
        if(registered_)sqlite3_vfs_unregister(&vfs_);
        for(const auto name:names_)sqlite3_free_filename(name);
        // Only exact files successfully created exclusively by this operation.
        // Successful delete-on-close leaves this empty; failures retain no public
        // temp leak. Never recurse, erase an existing collision or change ACLs.
        for(const auto& path:owned_){std::error_code ignored;std::filesystem::remove(path,ignored);}
    }
    const char* name()const noexcept{return name_.c_str();}
    std::size_t temporary_files()const noexcept{return owned_.size();}
private:
    static PrivateCompactionVfs& self(sqlite3_vfs* vfs){return *static_cast<PrivateCompactionVfs*>(vfs->pAppData);}
    static int open(sqlite3_vfs* vfs,const char* file,sqlite3_file* result,int flags,int* output){
        auto& s=self(vfs);
        if(file)return s.delegate_->xOpen(s.delegate_,file,result,flags,output);
        result->pMethods=nullptr;
        if(!(flags&SQLITE_OPEN_DELETEONCLOSE))return SQLITE_CANTOPEN;
        try{
            const auto path=s.folder_/std::filesystem::u8path(s.name_+"-"+std::to_string(++s.sequence_)+".sqlite");
            if(path.parent_path()!=s.folder_)return SQLITE_CANTOPEN;
            const auto created=s.policy_.create_database_file_exclusive(path);if(!created)return SQLITE_CANTOPEN;
            s.owned_.push_back(path);
            const auto protected_file=s.policy_.protect_and_verify_database_file(path);if(!protected_file)return SQLITE_CANTOPEN;
            const auto filename=path.generic_u8string();const auto persisted=sqlite3_create_filename(filename.c_str(),"","",0,nullptr);
            if(!persisted)return SQLITE_NOMEM;
            s.names_.push_back(persisted);
            // The private policy has already created the regular protected file
            // exclusively. Preserve DELETEONCLOSE; do not change DB durability.
            return s.delegate_->xOpen(s.delegate_,persisted,result,(flags&~SQLITE_OPEN_EXCLUSIVE)|SQLITE_OPEN_NOFOLLOW,output);
        }catch(...){return SQLITE_CANTOPEN;}
    }
    static int remove(sqlite3_vfs* v,const char* n,int sync){auto& s=self(v);return s.delegate_->xDelete(s.delegate_,n,sync);}
    static int access(sqlite3_vfs* v,const char* n,int flags,int* out){auto& s=self(v);return s.delegate_->xAccess(s.delegate_,n,flags,out);}
    static int full_path(sqlite3_vfs* v,const char* n,int count,char* out){auto& s=self(v);return s.delegate_->xFullPathname(s.delegate_,n,count,out);}
    static void* dl_open(sqlite3_vfs* v,const char* n){auto& s=self(v);return s.delegate_->xDlOpen(s.delegate_,n);}
    static void dl_error(sqlite3_vfs* v,int count,char* out){auto& s=self(v);s.delegate_->xDlError(s.delegate_,count,out);}
    static void(*dl_sym(sqlite3_vfs* v,void* lib,const char* symbol))(void){auto& s=self(v);return s.delegate_->xDlSym(s.delegate_,lib,symbol);}
    static void dl_close(sqlite3_vfs* v,void* lib){auto& s=self(v);s.delegate_->xDlClose(s.delegate_,lib);}
    static int randomness(sqlite3_vfs* v,int count,char* out){auto& s=self(v);return s.delegate_->xRandomness(s.delegate_,count,out);}
    static int sleep(sqlite3_vfs* v,int millis){auto& s=self(v);return s.delegate_->xSleep(s.delegate_,millis);}
    static int current_time(sqlite3_vfs* v,double* out){auto& s=self(v);return s.delegate_->xCurrentTime(s.delegate_,out);}
    static int last_error(sqlite3_vfs* v,int count,char* out){auto& s=self(v);return s.delegate_->xGetLastError(s.delegate_,count,out);}
    static int current_time64(sqlite3_vfs* v,sqlite3_int64* out){auto& s=self(v);return s.delegate_->xCurrentTimeInt64(s.delegate_,out);}
    static int set_system(sqlite3_vfs* v,const char* n,sqlite3_syscall_ptr f){auto& s=self(v);return s.delegate_->xSetSystemCall(s.delegate_,n,f);}
    static sqlite3_syscall_ptr get_system(sqlite3_vfs* v,const char* n){auto& s=self(v);return s.delegate_->xGetSystemCall(s.delegate_,n);}
    static const char* next_system(sqlite3_vfs* v,const char* n){auto& s=self(v);return s.delegate_->xNextSystemCall(s.delegate_,n);}
    sqlite3_vfs vfs_{};sqlite3_vfs* delegate_{};
    std::filesystem::path folder_;const IStoragePathPolicy& policy_;std::string name_;
    std::vector<sqlite3_filename> names_;std::vector<std::filesystem::path> owned_;
    std::uint64_t sequence_{};bool registered_{};
};
