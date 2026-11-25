// Only constructed after a parsed unsent Local/FileAttach request matches an
// enabled owner-configured per-network source and an owned origin. Transport
// spool directories are excluded before reaching this code.
class StoredSourceBindings final:public IFilePayloadBindings {
    std::shared_ptr<LinuxFilePayloadBindings> map_;std::filesystem::path path_;
    std::function<bool()> running_;bool managed_{};
public:
    StoredSourceBindings(std::shared_ptr<LinuxFilePayloadBindings> map,const std::filesystem::path& root,
        std::filesystem::path path,std::function<bool()> running):map_(std::move(map)),path_(std::move(path)),running_(std::move(running)){
        const auto relative=path_.lexically_relative(root).generic_u8string();
        managed_=relative.rfind("ftn/",0)==0||relative.rfind("files/",0)==0||relative.rfind("doors/",0)==0||relative.rfind("games/",0)==0;
    }
    app::FileObjectIdentity existing(const std::filesystem::path& path,int fd)override{
        require(path==path_&&running_(),"ftn_msg_source_scope_changed");return map_->preview_unregistered(path,fd);
    }
    bool protects_import_permissions()const noexcept override{return managed_;}
    std::filesystem::path resolve_path(const std::filesystem::path& path)const override{return map_->resolve_path(path);}
    app::FileObjectIdentity created(const std::filesystem::path&,int)override{throw std::runtime_error("ftn_msg_source_cannot_create");}
    void moved(const std::filesystem::path&,const std::filesystem::path&,int,const app::FileObjectIdentity&)override{throw std::runtime_error("ftn_msg_source_cannot_move");}
    void removed(const std::filesystem::path&,const app::FileObjectIdentity&)override{throw std::runtime_error("ftn_msg_preview_cannot_remove");}
    void checkpoint(const std::filesystem::path& path,int fd,const app::FilePayloadIdentity& value)override{
        require(path==path_&&running_(),"ftn_msg_source_scope_changed");
        // The durable source configuration authorizes local game input. This
        // records custody only, never queue/delivery/cleanup success. External
        // read-only attachments keep their original permissions and native ID.
        if(managed_)map_->adopt_unregistered(path,fd,value,[&]{return !running_();});
    }
};
bool stored_same_object(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool stored_same_stamp(const struct stat& a,const struct stat& b){
    return stored_same_object(a,b)&&a.st_size==b.st_size&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&
        a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
}
struct StoredFile {
    FilePayloadDirectory directory;FileDescriptor parent,handle;
    std::string name,raw,identity;std::filesystem::path path;app::FileObjectIdentity logical;
    struct stat parent_stamp{},stamp{};std::shared_ptr<LinuxFilePayloadBindings> map;
    StoredFile(const FilePayloadDirectory& source,std::string leaf):directory(source),parent(open_owned_directory(source.path(),false)),
        name(std::move(leaf)),path(source.path()/name){
        require(app::valid_ftn_leaf_name(name)&&fstat(parent.get(),&parent_stamp)==0,"ftn_msg_name_invalid");
        handle=FileDescriptor(openat(parent.get(),name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
        require(handle&&flock(handle.get(),LOCK_EX|LOCK_NB)==0,"ftn_msg_open_failed");
        require(fstat(handle.get(),&stamp)==0&&S_ISREG(stamp.st_mode)&&stamp.st_uid==geteuid()&&stamp.st_nlink==1&&
            (stamp.st_mode&022)==0&&stamp.st_size>=191&&stamp.st_size<=1024*1024,"ftn_msg_invalid_file");
        raw=read_bytes();live();
    }
    std::string read_bytes()const{
        std::string bytes(static_cast<std::size_t>(stamp.st_size),'\0');std::size_t offset{};
        while(offset<bytes.size()){
            const auto got=pread(handle.get(),bytes.data()+offset,bytes.size()-offset,static_cast<off_t>(offset));
            if(got<0&&errno==EINTR)continue;
            require(got>0,"ftn_msg_read_failed");offset+=static_cast<std::size_t>(got);
        }
        return bytes;
    }
    void live()const{
        const auto current=open_owned_directory(directory.path(),false);struct stat parent_now{},named{},opened{};
        require(fstat(current.get(),&parent_now)==0&&stored_same_object(parent_stamp,parent_now)&&
            fstat(handle.get(),&opened)==0&&stored_same_stamp(stamp,opened)&&opened.st_uid==geteuid()&&opened.st_nlink==1&&
            (opened.st_mode&022)==0&&fstatat(parent.get(),name.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&stored_same_stamp(stamp,named),"ftn_msg_path_or_content_changed");
    }
    void bind(std::shared_ptr<LinuxFilePayloadBindings> bindings,const std::filesystem::path& root,const std::function<bool()>& running){
        live();map=std::move(bindings);StoredSourceBindings authorized(map,root,path,running);logical=authorized.existing(path,handle.get());
        app::FilePayloadIdentity payload{raw.size(),hash(raw),logical.volume,logical.file_id};
        authorized.checkpoint(path,handle.get(),payload);struct stat after{};
        require(fstat(handle.get(),&after)==0&&stored_same_object(stamp,after)&&stamp.st_size==after.st_size&&
            stamp.st_mtim.tv_sec==after.st_mtim.tv_sec&&stamp.st_mtim.tv_nsec==after.st_mtim.tv_nsec,"ftn_msg_binding_changed_content");
        stamp=after;live();identity=logical.volume+"/"+logical.file_id;
    }
    void finish(bool kill){
        require(bool(map),"ftn_msg_unbound_request");live();require(read_bytes()==raw,"ftn_msg_content_changed");
        if(kill){
            const auto expected=hash(raw);const auto bytes=raw.size();handle=FileDescriptor();
            auto exact=directory.read(name,true);const auto actual=exact.identity();
            require(actual.volume==logical.volume&&actual.file_id==logical.file_id&&actual.bytes==bytes&&actual.sha256==expected,"ftn_msg_cleanup_source_changed");
            exact.remove_exact();
        }else{
            // Raw preview stays read-only. Obtain write access only for this
            // delivered request, while the original exclusive lease remains
            // held, and prove the second descriptor names that same object.
            FileDescriptor writer(openat(parent.get(),name.c_str(),O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));struct stat writable{};
            require(writer&&fstat(writer.get(),&writable)==0&&stored_same_stamp(stamp,writable),"ftn_msg_sent_writer_changed");live();
            const auto attributes=static_cast<std::uint16_t>(static_cast<unsigned char>(raw[186])|(static_cast<unsigned char>(raw[187])<<8)|8);
            const unsigned char sent[]{static_cast<unsigned char>(attributes),static_cast<unsigned char>(attributes>>8)};
            ssize_t count;do{count=pwrite(writer.get(),sent,2,186);}while(count<0&&errno==EINTR);
            require(count==2&&fsync(writer.get())==0,"ftn_msg_sent_mark_failed");
            struct stat after{};require(fstat(handle.get(),&after)==0&&stored_same_object(stamp,after)&&stamp.st_size==after.st_size,"ftn_msg_sent_mark_changed_object");
            stamp=after;raw[186]=static_cast<char>(sent[0]);raw[187]=static_cast<char>(sent[1]);live();require(read_bytes()==raw,"ftn_msg_sent_mark_changed_content");
            map->checkpoint(path,handle.get(),{raw.size(),hash(raw),logical.volume,logical.file_id});
        }
    }
};
