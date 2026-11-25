#include "sfx/application/door_execution.hpp"
#include "sfx/application/rlogin.hpp"

namespace sfx::application {
DoorEnd door_end(RloginEnd value) noexcept {
    switch(value){
    case RloginEnd::remote_closed:return DoorEnd::returned;
    case RloginEnd::gateway_escape:return DoorEnd::escaped;
    case RloginEnd::operator_terminated:return DoorEnd::operator_terminated;
    case RloginEnd::caller_disconnected:return DoorEnd::caller_disconnected;
    case RloginEnd::shutdown:return DoorEnd::shutdown;
    case RloginEnd::authority_lost:return DoorEnd::authority_lost;
    case RloginEnd::calling_time:return DoorEnd::calling_time;
    case RloginEnd::inactivity:return DoorEnd::inactivity;
    case RloginEnd::connection_failed:return DoorEnd::launch_failed;
    case RloginEnd::handshake_failed:return DoorEnd::handshake_failed;
    case RloginEnd::transport_failed:return DoorEnd::transport_failed;
    case RloginEnd::backpressure_timeout:return DoorEnd::backpressure_timeout;
    }
    return DoorEnd::transport_failed;
}
const char* door_result_code(DoorEnd value) noexcept {
    switch(value){
    case DoorEnd::returned:return "returned";
    case DoorEnd::escaped:return "escaped";
    case DoorEnd::operator_terminated:return "operator_terminated";
    case DoorEnd::caller_disconnected:return "caller_disconnected";
    case DoorEnd::shutdown:return "shutdown";
    case DoorEnd::authority_lost:return "authority_lost";
    case DoorEnd::calling_time:return "calling_time";
    case DoorEnd::inactivity:return "inactivity";
    case DoorEnd::launch_failed:return "launch_failed";
    case DoorEnd::handshake_failed:return "handshake_failed";
    case DoorEnd::transport_failed:return "transport_failed";
    case DoorEnd::backpressure_timeout:return "backpressure_timeout";
    case DoorEnd::busy:return "busy";
    case DoorEnd::unavailable:return "unavailable";
    case DoorEnd::game_disconnect:return "game_disconnect";
    }
    return "unknown";
}
DoorReservations::Lease::~Lease(){
    if(!released_){std::lock_guard<std::mutex> lock(shared_->mutex);shared_->slots[id_].quarantined=true;}
}
void DoorReservations::Lease::release_after_cleanup() noexcept {
    try{std::lock_guard<std::mutex> lock(shared_->mutex);if(released_)return;
        auto& slot=shared_->slots[id_];if(slot.count)--slot.count;
        if(!slot.count)slot.exclusive=false;
        released_=true;
    }catch(...){/* Keep the reservation if cleanup publication fails. */}
}
std::unique_ptr<DoorReservations::Lease> DoorReservations::acquire(DoorId id,bool multiple){
    if(id<1||id>999)return {};
    // Allocate before publishing ownership; allocation failure cannot leak count.
    auto lease=std::unique_ptr<Lease>(new Lease(shared_,id));lease->released_=true;
    std::lock_guard<std::mutex> lock(shared_->mutex);auto& slot=shared_->slots[id];
    if(slot.quarantined||slot.exclusive||(!multiple&&slot.count)||slot.count>=128)return {};
    ++slot.count;slot.exclusive=!multiple;lease->released_=false;return lease;
}
std::optional<std::string> expand_door_command(std::string_view command,const DoorCommandPaths& paths){
    if(!valid_door_text(command,4096,true)||paths.node_number<1||paths.node_number>128||!valid_door_code(paths.code))return {};
    std::string out;
    for(std::size_t i=0;i<command.size();++i){
        if(command[i]!='%')out+=command[i];
        else{if(++i==command.size())return {};
            switch(command[i]){
            case '%':out+='%';break;
            case '#':out+=std::to_string(paths.node_number);break;
            case 'N':out+=paths.node;break;
            case 'D':out+=paths.drop;break;
            case 'S':out+=paths.startup;break;
            case 'C':out+=paths.code;break;
            default:return {};
            }
        }
        if(out.size()>16384)return {};
    }
    return out;
}
} // namespace sfx::application
