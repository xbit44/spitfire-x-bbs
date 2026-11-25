#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/local_control.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <charconv>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace host=sfx::platform::linux_host;
using Code=app::LocalControlResponseCode;
volatile std::sig_atomic_t interrupted{};
void interrupt(int){interrupted=1;}
class Clock final:public app::IClock {
public:
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
// Console EOF never stops the BBS. The browser host uses these same explicit
// management operations; browser and caller lifetime remain independent.
int run(const std::filesystem::path& root,const std::filesystem::path& tzdata){
    if(geteuid()==0||!host::LinuxBbsTimeZoneRuleProvider::initialize(tzdata))throw std::runtime_error("normal_account_and_private_time_zone_required");
    Clock clock;rt::LinuxRuntimeHost runtime(root,clock,[]{return false;});
    const auto opened=runtime.open();const bool copied_recovery=opened.code==Code::recovery_required&&opened.diagnostic_code=="linux_file_map_prepare_required";
    if(opened.code!=Code::ok&&opened.code!=Code::setup_required&&!copied_recovery)throw std::runtime_error(opened.diagnostic_code);
    const auto root_pin=host::open_owned_directory(root);
    if(mkdirat(root_pin.get(),"management",0700)!=0&&errno!=EEXIST)throw std::runtime_error("management_directory_create_failed");
    host::LinuxLocalControlServer control(root/"management");if(!control.open())throw std::runtime_error("management_control_unavailable");
    std::atomic<bool> stop{false};std::string console;bool console_open=true;
    const auto stop_bbs=[&](std::optional<std::uint64_t> ticket){const auto result=runtime.stop_bbs(ticket);
        if(result.confirmation)std::cout<<"STOP_CONFIRM "<<result.confirmation<<' '<<result.callers<<'\n';
        else if(result.response.code==Code::ok)std::cout<<"STOPPED\n";
        else std::cout<<"STOP_FAILED "<<result.response.diagnostic_code<<'\n';};
    std::cout<<(copied_recovery?"MANAGEMENT_RECOVERY linux_file_map_prepare_required\n":"MANAGEMENT_READY STOPPED DEVELOPMENT\n")<<std::flush;
    while(!stop.load()&&!interrupted){
        runtime.tick_controls();
        if(console_open){pollfd input{STDIN_FILENO,POLLIN,0};const auto available=poll(&input,1,0);
            if(available>0&&(input.revents&(POLLIN|POLLHUP))){char bytes[256];const auto count=read(STDIN_FILENO,bytes,sizeof(bytes));
                if(count==0)console_open=false;else if(count>0)console.append(bytes,static_cast<std::size_t>(count));}
            if(console.size()>1024){console.clear();std::cout<<"COMMAND_REFUSED too_long\n";}
            for(auto end=console.find('\n');end!=console.npos;end=console.find('\n')){
                auto command=console.substr(0,end);console.erase(0,end+1);if(!command.empty()&&command.back()=='\r')command.pop_back();
                if(command=="start"){
                    const auto result=runtime.start_bbs();
                    if(result.code!=Code::ok)std::cout<<"START_REFUSED "<<result.diagnostic_code<<'\n';
                    else{const auto current=runtime.status();std::cout<<"STARTED "<<(current.telnet_active?current.telnet_port:0)<<' '<<(current.ssh_active?current.ssh_port:0)<<'\n';}
                }else if(command=="prepare-files"){
                    const auto result=runtime.prepare_copied_file_bindings([]{return interrupted!=0;});
                    std::cout<<(result.code==Code::ok?"PREPARED ":"PREPARE_REFUSED ")<<result.diagnostic_code<<'\n';
                }else if(command=="stop")stop_bbs({});
                else if(command.rfind("confirm ",0)==0){std::uint64_t ticket{};const auto text=std::string_view(command).substr(8);const auto parsed=std::from_chars(text.data(),text.data()+text.size(),ticket);
                    if(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&ticket)stop_bbs(ticket);else std::cout<<"COMMAND_REFUSED confirmation_invalid\n";}
                else if(command=="cancel"){runtime.cancel_stop();std::cout<<"CANCELLED\n";}
                else if(command=="status"){const auto value=runtime.status();std::cout<<"STATUS "<<static_cast<unsigned>(value.state)<<' '<<value.lifecycle.active_work.caller_sessions<<' '<<(value.telnet_active?value.telnet_port:0)<<' '<<(value.ssh_active?value.ssh_port:0)<<'\n';}
                else if(command=="quit"){
                    if(runtime.bbs_owned())std::cout<<"QUIT_REFUSED stop_bbs_first\n";else stop=true;
                }else std::cout<<"COMMAND_REFUSED unknown_command\n";
            }
            std::cout<<std::flush;
        }
        if(stop.load())break;
        (void)control.serve_one(stop,[&](app::LocalControlFrame& frame){return runtime.control(frame);});
    }
    control.close();const auto result=runtime.shutdown();const bool clean=result.code==Code::ok;
    std::cout<<(clean?"MANAGEMENT_STOPPED\n":"RECOVERY_REQUIRED\n")<<std::flush;return clean?0:2;
}
}
int main(int argc,char** argv){
    try{
        if(argc!=6||std::string_view(argv[1])!="--root"||std::string_view(argv[3])!="--tzdata"||std::string_view(argv[5])!="--development-console"){
            std::cerr<<"Usage: sfxruntime --root /absolute/board --tzdata /absolute/tzdata --development-console\n";return 64;}
        std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
        return run(argv[2],argv[4]);
    }catch(const std::exception& e){std::cerr<<"Runtime unavailable: "<<e.what()<<'\n';return 1;}
}
