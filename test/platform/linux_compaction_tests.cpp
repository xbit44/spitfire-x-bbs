#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <iostream>
#include <unistd.h>
namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace native=sfx::platform::linux_host;
namespace {
unsigned checks{};
void check(bool value,const char* detail){++checks;if(!value)throw std::runtime_error(detail);}
class Clock final:public app::IClock{
public:std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::uint64_t monotonic_millis()const noexcept override{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
};
}
int main(int argc,char** argv){try{
    check(argc==4&&geteuid()!=0,"prepared private board, timezone and mode required");
    check(native::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"private time rules initialize");Clock clock;rt::LinuxRuntimeHost runtime(argv[1],clock,[]{return false;});
    const auto opened=runtime.open();
    if(std::string_view(argv[3])=="guard"){
        check(opened.code==app::LocalControlResponseCode::recovery_required&&opened.diagnostic_code=="interrupted_compaction_requires_verified_recovery","actual pending-marker recovery reason is exact");
        check(!runtime.bbs_owned()&&runtime.request_compaction()==0,"guard cannot start BBS or repeat compaction");
        check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"guarded owner closes without discarding evidence");
    }else{
        check(std::string_view(argv[3])=="shutdown"&&opened.code==app::LocalControlResponseCode::ok,"fresh management opened stopped");
        const auto operation=runtime.request_compaction();check(operation!=0,"owned maintenance accepted");
        check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"shutdown cancels and joins before taking operations lock");
        const auto result=runtime.compaction_status();check(result.operation==operation&&result.completed&&!result.active,"joined maintenance exposes truthful final operation state");
        check(!result.result.physical_compaction_committed||result.result.logical_content_verified||std::filesystem::exists(std::filesystem::path(argv[1])/"config/compaction-pending.v1"),"uncertain physical outcome preserves recovery guard");
        check(runtime.request_compaction()==0,"closed owner does not start another worker");
    }
    std::cout<<"PASS "<<checks<<" compaction host lifecycle checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
