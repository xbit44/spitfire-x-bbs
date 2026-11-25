#include "linux_rlogin.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
namespace fs=std::filesystem;namespace rt=sfx::apps::runtime;namespace host=sfx::platform::linux_host;
int main(int argc,char** argv){unsigned checks{};const auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};try{
    check(argc==4&&geteuid()!=0,"normal-user fresh root and two helper images required");const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"fresh private fixture");
    check(fs::create_directory(root)&&chmod(root.c_str(),0700)==0&&fs::create_directory(root/"bin")&&chmod((root/"bin").c_str(),0700)==0,"private helper directory");
    const auto helper=root/"bin/sfxhostlookup";fs::copy_file(argv[2],helper);check(chmod(helper.c_str(),0700)==0,"private real helper");
    auto result=rt::resolve_linux_gateway_ipv4(root,"localhost",[]{return true;});
    check(!result.empty()&&result.front()==std::array<std::uint8_t,4>{127,0,0,1},"actual system localhost forward lookup");
    check(rt::resolve_linux_gateway_ipv4(root,"localhost",[]{return false;}).empty(),"already cancelled call starts no helper");
    check(rt::resolve_linux_gateway_ipv4(root,"bad\nname",[]{return true;}).empty(),"control-bearing hostname refused");
    fs::copy_file(argv[3],helper,fs::copy_options::overwrite_existing);check(chmod(helper.c_str(),0700)==0,"private controlled helper");
    auto protected_directory=host::open_owned_directory(root);
    result=rt::resolve_linux_gateway_ipv4(root,"good.example",[]{return true;});
    check(result.size()==2&&result[1]==std::array<std::uint8_t,4>{192,0,2,1},"bounded multiple addresses and helper no leaked fd/capabilities");
    check(rt::resolve_linux_gateway_ipv4(root,"bad.example",[]{return true;}).empty(),"whole malformed result refused");
    check(rt::resolve_linux_gateway_ipv4(root,"oversized.example",[]{return true;}).empty(),"oversized helper output refused");
    check(rt::resolve_linux_gateway_ipv4(root,"failed.example",[]{return true;}).empty(),"failed helper output refused");
    auto begin=std::chrono::steady_clock::now();check(rt::resolve_linux_gateway_ipv4(root,"slow.example",[]{return true;}).empty(),"blocked resolver cancelled");
    check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(3),"hard DNS deadline includes child retirement");
    begin=std::chrono::steady_clock::now();check(rt::resolve_linux_gateway_ipv4(root,"slow.example",[&]{return std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(60);}).empty(),"caller stop cancels DNS");
    check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"caller stop has bounded latency");
    int status{};check(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD,"every started resolver child joined");
    check(chmod(helper.c_str(),0755)==0&&rt::resolve_linux_gateway_ipv4(root,"good.example",[]{return true;}).empty(),"nonprivate helper refused");
    fs::remove(helper);check(mkfifo(helper.c_str(),0600)==0,"owned test FIFO created");
    begin=std::chrono::steady_clock::now();check(rt::resolve_linux_gateway_ipv4(root,"good.example",[]{return true;}).empty()&&std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"FIFO refused without blocking");
    std::cout<<"PASS "<<checks<<" native forward DNS/helper cancellation and cleanup checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
