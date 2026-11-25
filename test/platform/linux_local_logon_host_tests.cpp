#define main retained_management_catalog_main
#include "linux_management_catalog_tests.cpp"
#undef main
#include "sfx/platform/linux/botgate_rule_files.hpp"
#include <signal.h>

int main(int argc,char** argv){try{
    check(argc==4&&geteuid()!=0,"fresh normal-user board, timezone and small resolver fixture required");
    check(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"private timezone initialized");
    const std::filesystem::path root(argv[1]);Clock clock;
    rt::LinuxRuntimeHost runtime(root,clock,[]{return false;});
    check(runtime.open().code==app::LocalControlResponseCode::ok,"actual Host opens");
    check(!runtime.open_browser_node(1).client&&runtime.reserve_local_node(1).response.code!=app::LocalControlResponseCode::ok,"Host refuses local logon while stopped");
    check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"actual Host starts");
    auto browser=runtime.open_browser_node(2);
    check(browser.client&&browser.identity.node==2,"Host assigns exact browser node");
    (void)await_callers(runtime,1);
    const auto pending=runtime.stop_bbs();
    check(pending.callers==1&&pending.confirmation!=0&&runtime.bbs_owned(),"browser caller participates in exact stop confirmation");
    check(runtime.stop_bbs(pending.confirmation).response.code==app::LocalControlResponseCode::ok,"confirmed stop retires actual browser caller");
    browser.client=host::FileDescriptor{};
    check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"restart after browser stop");
    auto replacement=runtime.open_browser_node(2);check(replacement.client&&replacement.identity.generation>browser.identity.generation,"Host retains generation across restart");
    check(!runtime.disconnect(browser.identity)&&runtime.disconnect(replacement.identity),"old identity cannot disconnect replacement browser");
    replacement.client=host::FileDescriptor{};(void)await_callers(runtime,0);
    const auto reserved=runtime.reserve_local_node(1);
    check(reserved.response.code==app::LocalControlResponseCode::ok,"Host reserves desktop node");
    check(runtime.stop_bbs().response.code==app::LocalControlResponseCode::ok&&!runtime.cancel_local_node(reserved.reservation),"stop releases unused desktop reservation without claiming a live caller");

    auto settings=runtime.configuration();check(settings.has_value(),"stopped configuration available");
    auto candidate=settings->saved;candidate.configuration_revision++;candidate.botgate.enabled=true;candidate.telnet.bot_gate_enabled=true;candidate.hostname_lookup=false;
    check(runtime.save_configuration({settings->saved.configuration_revision,candidate}).code==app::LocalControlResponseCode::ok,"enable real domain admission for cancellation test");
    host::save_botgate_rule(root,std::to_wstring(geteuid()),0,"");
    host::save_botgate_rule(root,std::to_wstring(geteuid()),3,"blocked.example\n");
    const auto helper=root/"bin/sfxhostlookup",marker=root/"bin/admission-entered";
    check(std::filesystem::file_size(argv[3])<16*1024*1024&&!std::filesystem::exists(helper)&&std::filesystem::copy_file(argv[3],helper)&&chmod(helper.c_str(),0700)==0,"owned bounded ELF resolver fixture installed");
    for(const bool browser_mode:{false,true}){
        check(!std::filesystem::exists(marker),"new admission marker absent");
        check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"Host starts blocked-admission case");
        std::atomic<bool> returned{},refused{};
        std::thread request([&]{if(browser_mode){auto result=runtime.open_browser_node(1);refused=!result.client;}else refused=runtime.reserve_local_node(1).response.code!=app::LocalControlResponseCode::ok;returned=true;});
        bool entered{};for(unsigned i=0;i<150;++i){if(std::filesystem::exists(marker)){entered=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        const auto started=std::chrono::steady_clock::now();const auto stopped=runtime.stop_bbs();
        const auto elapsed=std::chrono::steady_clock::now()-started;request.join();
        std::cout<<"admission_entered="<<entered<<" stop_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()<<" stop_code="<<static_cast<unsigned>(stopped.response.code)<<'\n';
        check(entered&&stopped.response.code==app::LocalControlResponseCode::ok&&elapsed<std::chrono::milliseconds(1000),"Host stop interrupts real supervised admission before its two-second timeout");
        check(returned&&refused&&!runtime.bbs_owned()&&runtime.status().lifecycle.active_work.caller_sessions==0,"request returns refused with no post-stop node allocation");
        pid_t child{};{std::ifstream file(marker);file>>child;}
        check(child>0&&kill(child,0)==-1&&errno==ESRCH,"owned resolver child was killed and joined before stop returned");
        check(std::filesystem::remove(marker),"only completed owned marker retired");
    }
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"Host closes cleanly after cancellation cases");
    std::cout<<"PASS "<<checks<<" actual Host exact-node and blocked-admission checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
