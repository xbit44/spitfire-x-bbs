#include "alpha_local_door.hpp"
#include "sfx/platform/linux/local_door.hpp"
#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace app=application;
void ensure_door_node_directories(const std::filesystem::path& root,const std::wstring& owner,unsigned count){
    if(geteuid()==0||owner!=std::to_wstring(geteuid()))throw std::runtime_error("door_owner_identity_mismatch");
    platform::linux_host::ensure_door_nodes(root,count);
}
app::DoorProgramResult run_alpha_local_door(const std::filesystem::path& root,const std::wstring& owner,
    const app::DoorDefinition& door,const LocalDoorCaller& caller,const app::DoorChannel& channel,
    std::vector<std::uint8_t> prefix,bool discard_lf){
    app::DoorProgramResult failure;failure.end=app::DoorEnd::launch_failed;
    if(geteuid()==0||owner!=std::to_wstring(geteuid())){failure.diagnostic="door_owner_identity_mismatch";return failure;}
    auto context=caller.drop_context;
    context.account=caller.account;context.node=caller.node;context.nodes=std::max(context.nodes,caller.node);
    context.security=caller.security_level;context.alias=caller.alias;context.board=caller.board_name;
    context.ansi=caller.ansi;context.remaining_seconds=caller.remaining_seconds;
    if(door.program.drop_file==app::DoorDropFile::doordrop_ini){
        context.doordrop_utf8=door.program.doordrop_utf8;
        context.door_code=app::effective_door_code(door);context.door_name=door.name;
    }
    if(context.user_key.empty())context.user_key="sfx-account-"+std::to_string(caller.account);
    if(caller.remaining_seconds&&context.deadline_utc.empty()){
        const auto now=caller.captured_utc_seconds>0?caller.captured_utc_seconds:static_cast<std::int64_t>(std::time(nullptr));
        if(*caller.remaining_seconds>3155760000ULL||now>INT64_MAX-static_cast<std::int64_t>(*caller.remaining_seconds)){
            failure.diagnostic="door_deadline_range";return failure;}
        const time_t deadline=static_cast<time_t>(now+static_cast<std::int64_t>(*caller.remaining_seconds));std::tm utc{};char text[32]{};
        if(!gmtime_r(&deadline,&utc)||std::strftime(text,sizeof(text),"%Y-%m-%dT%H:%M:%SZ",&utc)!=20){failure.diagnostic="door_deadline_invalid";return failure;}
        context.deadline_utc=text;
    }
    return platform::linux_host::run_local_door(root,door,std::move(context),channel,{},std::move(prefix),discard_lf);
}
} // namespace sfx::apps::runtime
