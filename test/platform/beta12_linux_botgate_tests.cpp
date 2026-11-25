#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/botgate_rule_files.hpp"
#include <chrono>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace host=sfx::platform::linux_host;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Clock final:app::IClock {
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
};
}
int main(int argc,char** argv){try{
    using Code=app::LocalControlResponseCode;
    check(argc==3&&geteuid()!=0,"normal-user fixture and timezone required");
    check(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"timezone initialized");
    const fs::path root(argv[1]);check(root.is_absolute()&&!fs::exists(root),"new explicit fixture path");
    check(fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"private fixture created");
    Clock clock;rt::LinuxRuntimeHost runtime(root,clock,[]{return false;});
    check(runtime.open().code==Code::setup_required,"ordinary fresh host requires setup");
    check(!runtime.botgate_rule_file(0).available,"no rule editor before BBS setup");
    app::AlphaBootstrapRequest setup;
    setup.configuration.bbs_name="BotGate editor fixture";setup.configuration.bbs_time_zone="Etc/UTC";
    setup.configuration.telnet.bind_address=setup.configuration.ssh.bind_address="127.0.0.1";
    setup.sysop_name="Fixture Sysop";setup.sysop_password="Native harness first setup 83";setup.system_password="Native harness system key 71";
    check(runtime.bootstrap(setup).code==Code::ok,"ordinary bootstrap succeeds without starting services");
    check(!runtime.bbs_owned(),"BBS remains stopped");
    const auto configuration=runtime.configuration();check(configuration.has_value(),"configuration available");
    for(unsigned i=0;i<6;++i){
        const auto file=runtime.botgate_rule_file(i);
        check(file.available&&file.file==i&&file.revision.size()==64&&file.editable==(i!=4),"six fixed rule files and temporary-ban readonly contract");
    }
    const auto original=runtime.botgate_rule_file(1);const std::string candidate="# test block\n192.0.2.0/24\n!192.0.2.10\n";
    check(runtime.save_botgate_rule_file(1,original.revision,candidate).code==Code::ok,"valid rule file save accepted");
    const auto saved=runtime.botgate_rule_file(1);
    check(saved.text==candidate&&saved.revision!=original.revision,"saved bytes and new content revision read back");
    check(runtime.save_botgate_rule_file(1,original.revision,"203.0.113.1\n").diagnostic_code=="botgate_rule_changed","stale revision refused");
    check(runtime.botgate_rule_file(1).text==candidate,"stale save preserves current file");
    check(runtime.save_botgate_rule_file(1,saved.revision,"invalid rule !\n").diagnostic_code=="botgate_rule_invalid","invalid rules refused");
    check(runtime.botgate_rule_file(1).revision==saved.revision,"invalid candidate preserves exact current bytes");
    const auto temporary=runtime.botgate_rule_file(4);
    check(runtime.save_botgate_rule_file(4,temporary.revision,"192.0.2.1\n").diagnostic_code=="botgate_rule_invalid","crafted temporary-ban write refused");
    check(runtime.botgate_rule_file(4).revision==temporary.revision,"temporary bans unchanged");
    check(!runtime.botgate_rule_file(6).available&&runtime.save_botgate_rule_file(6,saved.revision,"").code!=Code::ok,"out-of-range selectors refused");
    check(runtime.save_botgate_rule_file(1,"../escape",candidate).code!=Code::ok,"malformed revision refused");
    const auto owner=std::to_wstring(geteuid());
    host::save_botgate_rule(root,owner,1,"198.51.100.1\n");
    check(runtime.save_botgate_rule_file(1,saved.revision,candidate).diagnostic_code=="botgate_rule_changed","external edit refuses stale browser save");
    const auto reloaded=runtime.botgate_rule_file(1);
    check(reloaded.text=="198.51.100.1\n"&&reloaded.revision!=saved.revision,"Reload Rules reads external file without changing settings");
    const auto directory=host::open_owned_directory(root/"botgate/can");
    const std::string large(65536,'\n');host::publish_private_file(directory.get(),"ip.can",{large.begin(),large.end()},true);
    const auto oversized=runtime.botgate_rule_file(1);
    check(oversized.available&&!oversized.editable&&oversized.text.empty()&&oversized.revision.size()==64,"large file remains intact and readonly in web editor");
    check(runtime.save_botgate_rule_file(1,oversized.revision,large).diagnostic_code=="botgate_rule_invalid","oversized write refused");
    check(runtime.save_botgate_rule_file(1,oversized.revision,"192.0.2.1\n").diagnostic_code=="botgate_rule_invalid","oversized readonly source cannot be replaced by a short crafted POST");
    const auto large_bytes=host::read_private_file(directory.get(),"ip.can",1024*1024);
    check(large_bytes==std::vector<std::uint8_t>(large.begin(),large.end()),"refused short replacement preserves the whole oversized file");
    host::save_botgate_rule(root,owner,1,"198.51.100.1\n");
    const auto safe=runtime.botgate_rule_file(1);
    const auto path=root/"botgate/can/ip.can",kept=root/"botgate/can/kept-rule.cfg";
    fs::rename(path,kept);check(symlink("kept-rule.cfg",path.c_str())==0,"owned adversarial link created");
    check(!runtime.botgate_rule_file(1).available,"linked rule read refused");
    check(runtime.save_botgate_rule_file(1,safe.revision,candidate).code!=Code::ok,"linked rule save refused");
    check(host::read_private_file(directory.get(),"kept-rule.cfg",1024)==std::vector<std::uint8_t>({'1','9','8','.','5','1','.','1','0','0','.','1','\n'}),"linked target preserved");
    check(unlink(path.c_str())==0,"remove only new test link");fs::rename(kept,path);
    check(chmod(path.c_str(),0644)==0,"shared permission fixture set");
    check(!runtime.botgate_rule_file(1).available,"shared rule refused without permission repair");
    check((fs::status(path).permissions()&fs::perms::others_read)!=fs::perms::none,"unsafe permissions were not silently changed");
    check(chmod(path.c_str(),0600)==0,"restore only test fixture permissions");
    check(runtime.update_botgate_feed().diagnostic_code=="feed_requires_running_bbs","manual feed request needs running worker");
    check(!runtime.botgate_status().updating,"stopped request starts no fetch");
    const auto after=runtime.configuration();
    check(after&&after->saved.configuration_revision==configuration->saved.configuration_revision,"rule-file operations do not save protection settings");
    check(runtime.shutdown().code==Code::ok,"ordinary host shuts down cleanly");
    check(!runtime.botgate_rule_file(1).available,"closed management cannot read or initialize rules");
    std::cout<<"PASS "<<checks<<" Linux BotGate editor CAS, validation, readonly, ownership, reload and worker-admission checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
