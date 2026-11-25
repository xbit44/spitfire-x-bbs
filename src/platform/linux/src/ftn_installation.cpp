#include "sfx/platform/linux/ftn_installation.hpp"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace sfx::platform::linux_host {
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
}
void verify_ftn_directory(const FtnInstallation& installation){
    require(installation.bindings&&installation.root.is_absolute()&&
        installation.root.lexically_normal()==installation.root&&
        installation.root!=installation.root.root_path(),"ftn_installation_context_invalid");
    require(installation.bindings->resolve_path(installation.root/"ftn")==installation.root/"ftn","ftn_installation_binding_changed");
    auto root=open_owned_directory(installation.root);auto ftn=open_owned_directory(installation.root/"ftn");
    (void)root;(void)ftn;
}
FilePayloadDirectory create_ftn_directory(const FtnInstallation& installation,const std::filesystem::path& path){
    verify_ftn_directory(installation);
    const auto relative=path.lexically_relative(installation.root/"ftn");
    require(path.is_absolute()&&path.lexically_normal()==path&&!relative.empty()&&relative!=".","ftn_directory_path_invalid");
    for(const auto& component:relative)require(component!=".."&&component!="."&&!component.empty(),"ftn_directory_path_invalid");
    auto parent=open_owned_directory(path.parent_path());
    if(mkdirat(parent.get(),path.filename().c_str(),0700)!=0)require(errno==EEXIST,"ftn_directory_create_failed");
    require(fsync(parent.get())==0,"ftn_directory_flush_failed");
    auto current=open_owned_directory(path.parent_path());struct stat before{},after{};
    require(fstat(parent.get(),&before)==0&&fstat(current.get(),&after)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino,"ftn_directory_parent_changed");
    return FilePayloadDirectory::private_store(path,installation.bindings);
}
}
