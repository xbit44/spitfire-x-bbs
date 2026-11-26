#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <algorithm>
#include <unistd.h>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <fstream>
#include <sys/stat.h>

namespace app=sfx::application;
namespace host=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Clock final:app::IClock {
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
std::vector<std::uint8_t> exchange(rt::LinuxRuntimeHost& runtime,app::LocalControlMessageType type,
    std::vector<std::uint8_t> bytes,app::LocalControlMessageType expected){
    app::LocalControlFrame request;request.type=type;request.request_id=73;request.payload=std::move(bytes);
    const auto response=runtime.control(request);
    check(response&&response->type==expected&&response->request_id==73,"typed response retains correlation and operation");
    check(std::all_of(request.payload.begin(),request.payload.end(),[](auto value){return value==0;}),"request payload erased on exit");
    return response->payload;
}
app::AlphaRuntimeStatus wire_status(rt::LinuxRuntimeHost& runtime,std::uint16_t screen=0){
    const auto value=app::read_status_snapshot(screen,[&](const app::AlphaStatusRequest& request)->std::optional<std::vector<std::uint8_t>>{
        return exchange(runtime,app::LocalControlMessageType::status_request,app::encode_status_request(request),app::LocalControlMessageType::status_response);
    });
    check(value.has_value(),"actual typed status passes shared chunk and snapshot validation");return *value;
}
struct Client {
    int fd{-1};
    explicit Client(std::uint16_t port){
        fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);check(fd>=0,"test caller socket");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
        if(::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){::close(fd);fd=-1;throw std::runtime_error("test caller connection");}
    }
    ~Client(){if(fd>=0)::close(fd);}
};
app::AlphaRuntimeStatus await_callers(rt::LinuxRuntimeHost& runtime,unsigned count){
    for(unsigned attempt=0;attempt<300;++attempt){
        const auto status=runtime.status();
        if(status.lifecycle.active_work.caller_sessions==count&&
            std::count_if(status.nodes.begin(),status.nodes.end(),[](const auto& node){return node.caller_connected;})==count)return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("actual caller count did not settle");
}
void check_assignment_lifetime(rt::LinuxRuntimeHost& runtime){
    const auto idle=wire_status(runtime);
    check(idle.nodes.size()==idle.lifecycle.configured_nodes&&idle.lifecycle.active_work.caller_sessions==0,"running idle status contains every configured node");
    Client first(idle.telnet_port);const auto occupied=await_callers(runtime,1);
    const auto found=std::find_if(occupied.nodes.begin(),occupied.nodes.end(),[](const auto& node){return node.caller_connected;});
    check(found!=occupied.nodes.end(),"real prelogin caller has a node");const auto old=found->screen_owner;
    const auto live=wire_status(runtime,old.node);
    check(live.runtime_epoch==old.runtime_epoch&&live.nodes.size()==idle.nodes.size(),"caller screen belongs to management epoch and complete node list");
    const auto pending=runtime.stop_bbs();check(pending.callers==1&&pending.confirmation!=0,"stop requires actual caller confirmation");
    check(runtime.stop_bbs(pending.confirmation).response.code==app::LocalControlResponseCode::ok,"confirmed stop retires actual caller");
    check(wire_status(runtime).runtime_epoch==idle.runtime_epoch,"BBS stop preserves management epoch");
    check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"restart within same management lifetime");
    Client second(runtime.status().telnet_port);const auto replacement=await_callers(runtime,1);
    const auto current=std::find_if(replacement.nodes.begin(),replacement.nodes.end(),[](const auto& node){return node.caller_connected;})->screen_owner;
    check(current.node==old.node&&current.runtime_epoch==old.runtime_epoch&&current.generation>old.generation,"reused node advances assignment generation across Stop/Start");
    check(!runtime.disconnect(old),"retired identity cannot disconnect replacement caller");
    check(wire_status(runtime,current.node).lifecycle.active_work.caller_sessions==1,"replacement remains connected after stale disconnect");
    check(runtime.disconnect(current),"exact current identity disconnects caller");
    (void)await_callers(runtime,0);check(wire_status(runtime).nodes.size()==idle.nodes.size(),"retired worker leaves complete idle node list");
}
#include "linux_management_file_checks.inc"
#include "linux_management_ftn_checks.inc"
}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"prepared private root/timezone and normal user required");
    check(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"private timezone initialized");
    Clock clock;host::LinuxSecureIdentifierGenerator identifiers;
    rt::LinuxRuntimeHost runtime(argv[1],clock,[]{return false;});
    check(runtime.open().code==app::LocalControlResponseCode::ok,"prepared board opens stopped");
    check(!runtime.bbs_owned(),"catalog opening never starts BBS");
    (void)wire_status(runtime);
    const auto initial_log=app::decode_log_page(exchange(runtime,app::LocalControlMessageType::log_page_request,app::encode_log_cursor({runtime.status().runtime_epoch,0}),app::LocalControlMessageType::log_page_response));
    check(initial_log&&initial_log->cursor.epoch.valid()&&!initial_log->entries.empty(),"management journal exists and records startup before any caller listener");
    for(bool online:{false,true}){
        if(online)check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"start actual native BBS");
        file_checks(runtime,argv[1],online,identifiers);
        ftn_checks(runtime,online,identifiers);
        const auto policy=app::decode_qwk_policy_result(exchange(runtime,app::LocalControlMessageType::qwk_policy_query_request,{},app::LocalControlMessageType::qwk_policy_query_response));
        check(policy&&policy->code==app::LocalAccountResultCode::ok,"read authoritative QWK policy");
        app::QwkPolicyChange qwk{identifiers.generate(),policy->policy.revision,online?"LINLIVE":"LINSTOP",200};
        const auto saved=app::decode_qwk_policy_result(exchange(runtime,app::LocalControlMessageType::qwk_policy_change_request,app::encode_qwk_policy_change(qwk),app::LocalControlMessageType::qwk_policy_change_response));
        check(saved&&saved->code==app::LocalAccountResultCode::ok&&saved->policy.board_id==qwk.board_id,"QWK policy save uses shared validation and durable revision");
        qwk.operation=identifiers.generate();qwk.board_id="STALE";
        check(runtime.change_qwk_policy(qwk).code==app::LocalAccountResultCode::conflict,"stale QWK revision cannot overwrite accepted save");
        const auto catalog=app::decode_message_configuration_snapshot(exchange(runtime,app::LocalControlMessageType::message_configuration_query_request,app::encode_message_configuration_query({}),app::LocalControlMessageType::message_configuration_query_response));
        check(catalog&&catalog->code==app::LocalAccountResultCode::ok,"message catalog snapshot");
        app::MessageConfigurationChange message;message.operation=identifiers.generate();message.expected_configuration_revision=catalog->catalog.revision;
        message.group.name=online?"Live native messages":"Stopped native messages";message.group.order=online?92:91;
        const auto group=app::decode_message_configuration_result(exchange(runtime,app::LocalControlMessageType::message_configuration_change_request,app::encode_message_configuration_change(message),app::LocalControlMessageType::message_configuration_change_response));
        check(group&&group->code==app::LocalAccountResultCode::ok&&group->result.entity_id>0,"native message group committed through shared service");
        message.operation=identifiers.generate();message.group.name="Stale group";
        check(runtime.change_message_configuration(message).code==app::LocalAccountResultCode::conflict,"stale message revision refused");
        message.action=app::MessageConfigurationAction::delete_group;
        check(runtime.change_message_configuration(message).code==app::LocalAccountResultCode::unauthorized,"held offline deletion cannot bypass Runtime boundary");
        const auto chunk=app::decode_door_configuration_snapshot(exchange(runtime,app::LocalControlMessageType::door_configuration_query_request,app::encode_door_configuration_query({}),app::LocalControlMessageType::door_configuration_query_response));
        check(chunk&&chunk->code==app::LocalAccountResultCode::ok&&chunk->bytes.size()==chunk->total,"actual small Door catalog chunk complete");
        const auto doors=app::decode_door_catalog(chunk->bytes);check(doors&&!doors->doors.empty(),"fixture Door catalog decodes");
        app::DoorConfigurationChange door;door.operation=identifiers.generate();door.expected_revision=doors->revision;
        door.action=app::DoorConfigurationAction::save_door;door.door=doors->doors.front();door.door.name=online?"Live native Door":"Stopped native Door";
        const auto changed=app::decode_door_configuration_result(exchange(runtime,app::LocalControlMessageType::door_configuration_change_request,app::encode_door_configuration_change(door),app::LocalControlMessageType::door_configuration_change_response));
        check(changed&&changed->code==app::LocalAccountResultCode::ok,"Door update commits without changing running state");
        door.operation=identifiers.generate();door.door.name="Stale Door";
        check(runtime.change_door_configuration(door).code==app::LocalAccountResultCode::conflict,"stale Door revision refused");
        check(runtime.bbs_owned()==online,"catalog transactions preserve explicit BBS state");
        const auto classic=app::decode_classic_information_snapshot(exchange(runtime,app::LocalControlMessageType::classic_information_query_request,app::encode_classic_information_query(0),app::LocalControlMessageType::classic_information_query_response));
        check(classic&&classic->code==app::LocalAccountResultCode::ok,"Other BBS text catalog attached to actual Runtime storage");
        app::ClassicInformationChange other;other.values={online?"Live native board":"Stopped native board","example.invalid:2323","example.invalid:2222"};
        const auto added=app::decode_account_result(exchange(runtime,app::LocalControlMessageType::classic_information_change_request,app::encode_classic_information_change(other),app::LocalControlMessageType::classic_information_change_response));
        check(added&&added->code==app::LocalAccountResultCode::ok,"actual typed Other BBS addition and durable journal publication");
        const auto page=runtime.classic_information_snapshot(0);check(page.code==app::LocalAccountResultCode::ok&&!page.boards.entries.empty(),"published Other BBS text reads back");
        const auto row=page.boards.entries.back();other.action=app::ClassicInformationAction::edit_board;other.id=row.id;other.expected_revision=row.revision;other.values.name+=" edited";
        check(runtime.change_classic_information(other).code==app::LocalAccountResultCode::ok,"owner edits current text revision");
        check(runtime.change_classic_information(other).code==app::LocalAccountResultCode::conflict,"stale text revision cannot overwrite owner edit");
    }
    check_assignment_lifetime(runtime);
    check(runtime.stop_bbs().response.code==app::LocalControlResponseCode::ok,"stop actual BBS");
    check(!ftn_status(runtime).classic_listening&&ftn_status(runtime).diagnostic=="ftn_bbs_stopped","BBS Stop retires the actual FTN engine before storage close");
    const auto stopped_log=runtime.log_page({initial_log->cursor.epoch,0});
    check(stopped_log&&stopped_log->entries.size()>=initial_log->entries.size(),"journal history and cursor survive BBS start/stop");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"retire services and close store");
    check(runtime.qwk_policy_snapshot().code==app::LocalAccountResultCode::unavailable,"closed host cannot use released storage");
    check(runtime.open().code==app::LocalControlResponseCode::ok,"reopen same durable board");
    check(read_ftn(runtime,identifiers).networks.front().name=="Live native FTN"&&!ftn_status(runtime).classic_listening,"FTN saved configuration survives reopen without automatic BBS start");
    check(!runtime.log_page({initial_log->cursor.epoch,0}),"a retired management epoch cannot select a replacement journal");
    check(runtime.qwk_policy_snapshot().policy.board_id=="LINLIVE","policy survives complete store reopen");
    const auto classic=runtime.classic_information_snapshot(0);
    check(classic.code==app::LocalAccountResultCode::ok&&classic.boards.entries.size()==2&&classic.boards.entries.back().values.name=="Live native board edited","Other BBS text survives complete Runtime reopen");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"close reopened board");
    std::cout<<"PASS "<<checks<<" actual native typed management catalog transactions\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
