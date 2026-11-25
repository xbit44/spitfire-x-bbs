#include "sfx/platform/linux/local_control.hpp"
#include <iostream>
#include <unistd.h>
int main(int argc,char** argv){
    namespace app=sfx::application;namespace host=sfx::platform::linux_host;
    if(argc!=2||geteuid()==0)return 2;
    std::atomic<bool> stop{false};std::uint32_t id=1;
    const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>>{
        app::LocalControlFrame request;request.request_id=id++;request.type=app::LocalControlMessageType::status_request;request.payload=app::encode_status_request(query);
        const auto response=host::exchange_local_control(std::filesystem::path(argv[1])/"management",request,app::LocalControlMessageType::status_response,stop);
        return response?std::optional<std::vector<std::uint8_t>>{response->payload}:std::nullopt;
    });
    if(!status){std::cerr<<"typed status snapshot unavailable\n";return 4;}
    unsigned packet{},passed{},failed{};
    for(const auto& row:status->logs)if(row.event){
        if(row.event->context.diagnostic_code=="packet_actual_completion"||row.event->context.diagnostic_code=="packet_relay_incomplete")++packet;
        if(row.event->kind==app::OperationalKind::botgate_passed)++passed;
        if(row.event->kind==app::OperationalKind::botgate_failed)++failed;
    }
    std::cout<<"{\"health\":"<<static_cast<unsigned>(status->log_health)<<",\"packet_phase\":"<<packet<<",\"botgate_passed\":"<<passed<<",\"botgate_failed\":"<<failed<<"}\n";
    return status->log_health?1:0;
}
