#include "linux_verified_domain.hpp"
#include "linux_hostname_lookup.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
namespace fs=std::filesystem;namespace rt=sfx::apps::runtime;namespace host=sfx::platform::linux_host;
int main(int argc,char** argv){unsigned checks{};const auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};try{
    check(argc==4&&geteuid()!=0,"fresh root and exact real/controlled helpers required");const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"fresh fixture");
    check(fs::create_directory(root)&&chmod(root.c_str(),0700)==0&&fs::create_directory(root/"bin")&&chmod((root/"bin").c_str(),0700)==0,"private helper directory");
    const auto helper=root/"bin/sfxhostlookup";fs::copy_file(argv[2],helper);check(chmod(helper.c_str(),0700)==0,"private real helper");std::atomic<bool> stop{};
    auto result=rt::verify_linux_peer_domain(root,0x7f000001,stop);check(result.completed,"real localhost reverse and forward query completes");
    const auto real_ptr=rt::lookup_linux_hostname(root,0x7f000001,[&]{return stop.load();});check(real_ptr.has_value(),"real display-only localhost PTR query completes");
    if(!result.name.empty())check(result.name=="localhost"||result.name=="localhost.localdomain","only verified localhost name accepted");
    fs::copy_file(argv[3],helper,fs::copy_options::overwrite_existing);check(chmod(helper.c_str(),0700)==0,"private controlled helper");
    auto protected_directory=host::open_owned_directory(root);
    result=rt::verify_linux_peer_domain(root,0xc0000201,stop);check(result.completed&&result.name=="verified.example","normalized result; helper has no capabilities or leaked descriptors");
    check(!rt::verify_linux_peer_domain(root,0xc0000202,stop).completed,"terminal control byte refused");
    check(!rt::verify_linux_peer_domain(root,0xc0000203,stop).completed,"oversized output refused");
    check(!rt::verify_linux_peer_domain(root,0xc0000205,stop).completed,"failed helper cannot produce verified name");
    auto begin=std::chrono::steady_clock::now();check(!rt::verify_linux_peer_domain(root,0xc0000204,stop).completed,"blocked resolver times out");
    check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(3),"blocked resolver retired within bounded window");
    std::thread cancel([&]{std::this_thread::sleep_for(std::chrono::milliseconds(50));stop=true;});
    begin=std::chrono::steady_clock::now();const auto cancelled=rt::verify_linux_peer_domain(root,0xc0000204,stop);cancel.join();
    check(!cancelled.completed&&std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"shutdown cancels and joins helper");
    int status{};check(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD,"no unresolved child remains");stop=false;
    const auto display=rt::lookup_linux_hostname(root,0xc0000201,[&]{return stop.load();});check(display&&display->view()=="display.example","separate typed display PTR result is normalized");
    check(!rt::lookup_linux_hostname(root,0xc0000202,{}),"display PTR control bytes refused");check(!rt::lookup_linux_hostname(root,0xc0000203,{}),"oversized display PTR refused");
    struct Clock final:sfx::application::IClock{std::int64_t unix_seconds()const noexcept override{return 1;}std::uint64_t monotonic_millis()const noexcept override{return 1;}}clock;
    sfx::application::Identifier128 epoch{};epoch.bytes[0]=3;sfx::application::FourNodePool pool(epoch,4,4);std::vector<std::shared_ptr<sfx::application::CallerAssignment>> callers;
    rt::LinuxHostnameLookup asynchronous(root,true);
    begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<4;++i){auto lease=pool.allocate("192.0.2.4",false);check(lease.lease.has_value(),"independent slow hostname caller assignment");auto caller=std::make_shared<sfx::application::CallerAssignment>(std::move(*lease.lease),pool,clock,false,"192.0.2.4");asynchronous.subscribe(caller);callers.push_back(std::move(caller));}
    check(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds(200),"caller subscription never waits for blocked DNS");std::this_thread::sleep_for(std::chrono::milliseconds(100));begin=std::chrono::steady_clock::now();asynchronous.stop();
    check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"four blocked resolver workers cancelled and joined within bound");
    check(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD,"asynchronous display lookup leaves no child processes");
    for(const auto& caller:callers){check(!caller->stopping.load()&&caller->snapshot().hostname.empty(),"lookup shutdown neither disconnects callers nor publishes failed hints");caller->cancel();caller->transport_closed();check(caller->retire(true),"synthetic caller cleanup complete");}
    check(chmod(helper.c_str(),0755)==0&&!rt::verify_linux_peer_domain(root,0xc0000201,stop).completed,"shared helper permissions refused");
    std::cout<<"PASS "<<checks<<" Linux helper identity, DNS, limits, cancellation and child cleanup checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
