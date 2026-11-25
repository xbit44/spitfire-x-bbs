#include "sfx/platform/linux/trusted_display_asset_reader.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace host=sfx::platform::linux_host;
using Code=sfx::application::TrustedDisplayAssetReadCode;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal user and explicit fixture");
    const fs::path root=argv[1];
    check(root.is_absolute()&&!fs::exists(root),"fresh absolute fixture");
    check(fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"owned display directory");
    const std::string expected="Owner artwork\r\n";
    {std::ofstream file(root/"Main.ASC",std::ios::binary);file<<expected;}
    check(chmod((root/"Main.ASC").c_str(),0600)==0,"owned artwork");
    auto reader=host::LinuxTrustedDisplayAssetReader::open(root);
    check(static_cast<bool>(reader),"trusted reader opened");
    check(reader->read_asset("main.asc",32768).code==Code::available,"case-insensitive regular artwork available");
    check(mkfifo((root/"welcome.asc").c_str(),0600)==0,"FIFO display without writer");
    check(reader->read_asset("WELCOME.ASC",32768).code==Code::unavailable,"FIFO display refused without waiting");
    check(fs::is_fifo(root/"welcome.asc"),"refused FIFO preserved");
    check(symlink("Main.ASC",(root/"alias.asc").c_str())==0,"symlink fixture");
    check(reader->read_asset("alias.asc",32768).code==Code::unavailable,"symlink display refused");
    check(link((root/"Main.ASC").c_str(),(root/"link.asc").c_str())==0,"hardlink fixture");
    check(reader->read_asset("main.asc",32768).code==Code::unavailable,"multiply-linked display refused");
    check(unlink((root/"link.asc").c_str())==0,"remove only added hardlink");
    check(reader->read_asset("main.asc",2).code==Code::invalid,"oversized display refused");
    check(reader->read_asset("../main.asc",32768).code==Code::invalid,"directory escape refused");
    check(reader->read_asset("missing.asc",32768).code==Code::not_found,"missing optional artwork distinct from unsafe asset");
    check(reader->read_asset("main.asc",32768).code==Code::available,"regular artwork usable after refusals");
    std::cout<<"PASS "<<checks<<" Linux display file checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
