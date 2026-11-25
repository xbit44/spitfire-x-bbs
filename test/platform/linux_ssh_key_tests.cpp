#include "alpha_ssh_stream.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace runtime=sfx::apps::runtime;
namespace host=sfx::platform::linux_host;
namespace {
unsigned checks{};
void check(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
void create(const fs::path& path){check(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"fresh private directory");}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal user and explicit fixture required");
    const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"fresh root required");
    create(root);create(root/"config");create(root/"other");create(root/"other/Config");
    const auto owner=std::to_wstring(geteuid());std::string diagnostic;
    auto first=runtime::AlphaSshBackend::acquire(root,owner,diagnostic);
    check(static_cast<bool>(first)&&diagnostic=="ssh_host_key_ready","fresh private host identity generated");
    auto directory=host::open_owned_directory(root/"config");
    const auto original=host::read_private_file(directory.get(),"ssh_host_ed25519.pem",16384);
    check(!original.empty()&&!fs::exists(root/"config/ssh_host_ed25519.pending"),"complete key published and pending retired");
    auto second=runtime::AlphaSshBackend::acquire(root,owner,diagnostic);
    check(second==first,"same board shares immutable identity capability");
    check(!runtime::AlphaSshBackend::acquire(root/"other",owner,diagnostic)&&diagnostic=="ssh_backend_identity_in_use","different live board identity refused");
    auto counts=runtime::AlphaSshBackend::statistics();
    check(counts.registration_attempts==1&&counts.key_generation_attempts==1&&counts.key_load_attempts==1,"one registration/generation/load while capability retained");
    first.reset();second.reset();
    auto again=runtime::AlphaSshBackend::acquire(root,owner,diagnostic);
    check(static_cast<bool>(again)&&host::read_private_file(directory.get(),"ssh_host_ed25519.pem",16384)==original,"restart reuses exact host key bytes");
    again.reset();
    const auto key=root/"config/ssh_host_ed25519.pem";
    check(chmod(key.c_str(),0644)==0,"create shared-key fixture");
    check(!runtime::AlphaSshBackend::acquire(root,owner,diagnostic),"shared key refused without regeneration");
    check(chmod(key.c_str(),0600)==0,"restore key protection");
    check(link(key.c_str(),(root/"config/key-link").c_str())==0,"create multiply-linked key fixture");
    check(!runtime::AlphaSshBackend::acquire(root,owner,diagnostic),"multiply-linked key refused");
    check(unlink((root/"config/key-link").c_str())==0,"remove only added key hardlink");
    check(chmod((root/"config").c_str(),0755)==0,"create shared config fixture");
    check(!runtime::AlphaSshBackend::acquire(root,owner,diagnostic),"shared configuration directory refused");
    check(chmod((root/"config").c_str(),0700)==0,"restore private config");
    fs::rename(key,root/"config/original.pem");
    check(symlink("original.pem",key.c_str())==0,"create symlink key fixture");
    check(!runtime::AlphaSshBackend::acquire(root,owner,diagnostic),"symlink key refused");
    check(unlink(key.c_str())==0,"remove only added key symlink");
    check(mkfifo(key.c_str(),0600)==0,"create FIFO host-key fixture without writer");
    check(!runtime::AlphaSshBackend::acquire(root,owner,diagnostic)&&diagnostic=="ssh_host_key_insecure_or_unreadable","FIFO key refused without blocking or regeneration");
    check(fs::is_fifo(key),"refused FIFO identity preserved");
    check(unlink(key.c_str())==0,"remove only added key FIFO");
    fs::rename(root/"config/original.pem",key);
    check(host::read_private_file(directory.get(),"ssh_host_ed25519.pem",16384)==original,"all refusals preserved original identity");
    check(!runtime::AlphaSshBackend::acquire(root,L"wrong-owner",diagnostic),"wrong caller owner refused");
    auto restored=runtime::AlphaSshBackend::acquire(root,owner,diagnostic);
    check(static_cast<bool>(restored),"valid board works after fixture restoration");restored.reset();
    const auto pending=root/"other/Config/ssh_host_ed25519.pending";
    {std::ofstream out(pending);out<<"preserve incomplete generation";}check(chmod(pending.c_str(),0600)==0,"prepare prior incomplete generation");
    check(!runtime::AlphaSshBackend::acquire(root/"other",owner,diagnostic)&&fs::exists(pending)&&!fs::exists(root/"other/Config/ssh_host_ed25519.pem"),"incomplete first-start key kept and startup refused");
    counts=runtime::AlphaSshBackend::statistics();check(counts.key_generation_attempts==1,"invalid cases never replaced board identity");
    std::cout<<"PASS "<<checks<<" real Linux SSH host-key custody checks; no key bytes logged\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
