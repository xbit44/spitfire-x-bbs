std::string read_linux_ftn_nodelist(const std::filesystem::path& path,const std::function<bool()>& running){
    auto parent=native::open_owned_directory(path.parent_path(),false);struct stat directory{},before{},after{},named{};
    require(fstat(parent.get(),&directory)==0,"ftn_nodelist_directory_unavailable");
    native::FileDescriptor input(openat(parent.get(),path.filename().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    require(input&&fstat(input.get(),&before)==0&&S_ISREG(before.st_mode)&&before.st_uid==geteuid()&&
        before.st_nlink==1&&(before.st_mode&022)==0&&before.st_size>=0&&before.st_size<=32*1024*1024,"ftn_nodelist_unavailable");
    std::string text(static_cast<std::size_t>(before.st_size),'\0');std::size_t offset{};
    while(offset<text.size()){
        require(running(),"ftn_session_cancelled");const auto count=pread(input.get(),text.data()+offset,std::min<std::size_t>(65536,text.size()-offset),static_cast<off_t>(offset));
        if(count<0&&errno==EINTR)continue;
        require(count>0,"ftn_nodelist_read_failed");offset+=static_cast<std::size_t>(count);
    }
    const auto current=native::open_owned_directory(path.parent_path(),false);struct stat current_directory{};
    require(fstat(current.get(),&current_directory)==0&&directory.st_dev==current_directory.st_dev&&directory.st_ino==current_directory.st_ino&&
        fstatat(parent.get(),path.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&fstat(input.get(),&after)==0&&
        named.st_dev==before.st_dev&&named.st_ino==before.st_ino&&after.st_dev==before.st_dev&&after.st_ino==before.st_ino&&
        after.st_size==before.st_size&&after.st_mtim.tv_sec==before.st_mtim.tv_sec&&after.st_mtim.tv_nsec==before.st_mtim.tv_nsec&&
        after.st_ctim.tv_sec==before.st_ctim.tv_sec&&after.st_ctim.tv_nsec==before.st_ctim.tv_nsec,"ftn_nodelist_changed");
    return text;
}
