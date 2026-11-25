#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <iostream>
#include <thread>
#include <fstream>
namespace app=sfx::application;
namespace host=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Clock final:app::IClock{
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
struct Caller{
    int fd{-1};std::string received;std::size_t consumed{};
    explicit Caller(std::uint16_t port){
        fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);check(fd>=0,"real caller socket");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){close(fd);fd=-1;throw std::runtime_error("real caller connect");}
    }
    ~Caller(){if(fd>=0)close(fd);}
    void send(std::string_view text){while(!text.empty()){const auto n=::send(fd,text.data(),text.size(),MSG_NOSIGNAL);check(n>0,"caller input reaches Runtime");text.remove_prefix(static_cast<std::size_t>(n));}}
    void prompt(std::string_view needle,std::string_view answer={}){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
        for(;;){
            const auto at=received.find(needle,consumed);if(at!=std::string::npos){consumed=at+needle.size();++checks;send(answer);return;}
            if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("caller prompt unavailable: "+std::string(needle));
            pollfd descriptor{fd,POLLIN,0};const auto ready=poll(&descriptor,1,100);
            if(ready<0)throw std::runtime_error("caller poll failed");
            if(ready){char bytes[4096];const auto count=recv(fd,bytes,sizeof(bytes),0);if(count<=0)throw std::runtime_error("caller ended before prompt: "+std::string(needle));received.append(bytes,static_cast<std::size_t>(count));}
        }
    }
    void login(){
        prompt("[Y/n] ","n");prompt("Enter your first name: ","Native\r");prompt("Enter your last name: ","Caller\r");
        prompt("Enter your password: ","Harness night caller 83\r");prompt("Press <ENTER> to continue.","\r");prompt("<Enter> to continue ","\r");prompt("Command: ");
        check(received.find("Harness night caller 83")==std::string::npos,"caller password never echoed");
    }
};
std::vector<std::uint8_t> exchange(rt::LinuxRuntimeHost& runtime,app::LocalControlMessageType type,std::vector<std::uint8_t> payload,app::LocalControlMessageType expected){
    app::LocalControlFrame request;request.type=type;request.request_id=91;request.payload=std::move(payload);const auto reply=runtime.control(request);
    check(reply&&reply->type==expected&&reply->request_id==91,"typed account response matches request");
    check(std::all_of(request.payload.begin(),request.payload.end(),[](auto byte){return byte==0;}),"account request bytes wiped");return reply->payload;
}
app::LocalAccountSnapshot snapshot(rt::LinuxRuntimeHost& runtime,app::AccountId account){
    const auto result=app::decode_account_snapshot(exchange(runtime,app::LocalControlMessageType::account_query_request,app::encode_account_query({account}),app::LocalControlMessageType::account_query_response));
    check(result&&result->code==app::LocalAccountResultCode::ok,"actual account snapshot");return *result;
}
app::LocalAccountResult change(rt::LinuxRuntimeHost& runtime,app::LocalAccountRequest request){
    const auto result=app::decode_account_result(exchange(runtime,app::LocalControlMessageType::account_command_request,app::encode_account_request(request),app::LocalControlMessageType::account_command_response));
    check(result.has_value(),"actual account mutation decodes");return *result;
}
}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"prepared private board and normal user");check(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"private timezone");
    Clock clock;rt::LinuxRuntimeHost runtime(argv[1],clock,[]{return false;});check(runtime.open().code==app::LocalControlResponseCode::ok,"management opens stopped");
    const auto config_directory=host::open_owned_directory(std::filesystem::path(argv[1])/"config");
    const auto before_policy=host::read_private_file(config_directory.get(),"SFX.conf",1024U*1024U);
    const auto accounts=snapshot(runtime,0);const auto found=std::find_if(accounts.accounts.begin(),accounts.accounts.end(),[](const auto& value){return value.display_name=="Native Caller";});
    check(found!=accounts.accounts.end(),"synthetic ordinary caller exists");const auto account=found->account_id;
    check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"start actual caller service");
    Caller first(runtime.status().telnet_port);first.login();auto current=snapshot(runtime,account);check(current.online,"published account marked online");
    const auto live_status=runtime.status();const auto live_node=std::find_if(live_status.nodes.begin(),live_status.nodes.end(),[&](const auto& node){return node.account_id==account;});
    check(live_node!=live_status.nodes.end(),"online Chat target comes from actual Runtime node status");
    const app::NodeAssignmentIdentity chat_target{live_status.runtime_epoch,live_node->number,live_node->assignment_sequence};
    app::ChatNoticeRequest notice;notice.target=chat_target;notice.text="Native operator notice proof";
    const auto notice_result=exchange(runtime,app::LocalControlMessageType::chat_notice_request,app::encode_chat_notice(notice),app::LocalControlMessageType::chat_notice_response);
    check(notice_result==std::vector<std::uint8_t>{static_cast<std::uint8_t>(app::ChatResult::ok)},"typed notice routes to exact live caller");
    first.prompt("Native operator notice proof");first.prompt("Command: ");
    const auto chat_config=app::decode_chat_result(exchange(runtime,app::LocalControlMessageType::chat_configuration_query_request,{},app::LocalControlMessageType::chat_configuration_query_response));
    check(chat_config&&chat_config->code==app::ChatResult::ok&&chat_config->configuration,"typed Chat configuration returns current catalog");
    auto chat_change=*chat_config->configuration;chat_change.rooms.front().name="Native Lobby";
    const auto chat_saved=app::decode_chat_result(exchange(runtime,app::LocalControlMessageType::chat_configuration_change_request,app::encode_chat_configuration(chat_change),app::LocalControlMessageType::chat_configuration_change_response));
    check(chat_saved&&chat_saved->code==app::ChatResult::ok&&chat_saved->configuration->revision==chat_change.revision+1,"live Chat catalog transaction publishes new revision");
    check(runtime.change_chat_configuration(chat_change).code==app::ChatResult::conflict,"stale Chat catalog edit refused");
    app::ChatOperatorRequest operator_request;operator_request.action=app::ChatOperatorAction::availability;operator_request.available=true;
    const auto operator_result=app::decode_chat_operator_snapshot(exchange(runtime,app::LocalControlMessageType::chat_operator_request,app::encode_chat_operator_request(operator_request),app::LocalControlMessageType::chat_operator_response));
    check(operator_result&&operator_result->available,"typed operator availability reaches live hub");
    app::LocalAccountRequest request;request.action=app::LocalAccountAction::assign_security_level;request.account_id=account;request.expected_revision=current.accounts.front().revision;
    request.expected_policy_revision=current.policy.revision;request.security_level=*current.accounts.front().security_level;
    const auto same=change(runtime,request);check(same.code==app::LocalAccountResultCode::conflict&&!same.committed,"shared writer refuses an unchanged security assignment");
    check(runtime.refresh_account_access(account)&&snapshot(runtime,account).online,"same live entitlement refresh retains actual caller");
    app::LocalSecurityChange security;security.expected_revision=current.policy.revision;security.action=app::LocalSecurityAction::upsert_level;security.level=20;security.label="Registered Caller";
    security.new_user_level=current.policy.new_user_level;security.sysop_threshold=current.policy.sysop_threshold;
    const auto security_result=app::decode_account_result(exchange(runtime,app::LocalControlMessageType::security_change_request,app::encode_security_change(security),app::LocalControlMessageType::security_change_response));
    check(security_result&&security_result->code==app::LocalAccountResultCode::ok&&security_result->committed,"live security catalog commits and publishes matching public configuration");
    check(runtime.change_security(security).code==app::LocalAccountResultCode::conflict,"stale security publication rejected");
    const auto security_snapshot=app::decode_security_snapshot(exchange(runtime,app::LocalControlMessageType::security_query_request,app::encode_account_query({}),app::LocalControlMessageType::security_query_response));
    check(security_snapshot&&security_snapshot->code==app::LocalAccountResultCode::ok&&security_snapshot->levels.size()==3&&runtime.configuration()->saved.security_policy_revision==security_snapshot->policy.revision,"typed security catalog and saved configuration share committed revision");
    current=snapshot(runtime,account);request.expected_revision=current.accounts.front().revision;request.expected_policy_revision=current.policy.revision;request.security_level=20;
    const auto reassigned=change(runtime,request);check(reassigned.code==app::LocalAccountResultCode::ok&&reassigned.committed&&snapshot(runtime,account).online,"live non-Sysop level change refreshes authority without disconnecting caller");
    current=snapshot(runtime,account);request={};request.action=app::LocalAccountAction::lock;request.account_id=account;request.expected_revision=current.accounts.front().revision;
    const auto locked=change(runtime,request);check(locked.code==app::LocalAccountResultCode::ok&&locked.committed&&locked.sessions_revoked,"account lock confirms real worker retirement through host revocation");
    check(change(runtime,request).code==app::LocalAccountResultCode::conflict,"stale account edit refused");
    current=snapshot(runtime,account);check(!current.online&&current.accounts.front().state==app::AccountState::locked&&runtime.status().lifecycle.active_work.caller_sessions==0,"locked account is offline with no worker");
    request.action=app::LocalAccountAction::unlock;request.expected_revision=current.accounts.front().revision;
    check(change(runtime,request).code==app::LocalAccountResultCode::ok,"unlock same account through shared writer");
    Caller fresh(runtime.status().telnet_port);fresh.login();check(snapshot(runtime,account).online,"new login after actual ledger revocation gets a fresh checkpoint");
    check(runtime.send_chat_notice(notice)==app::ChatResult::unavailable,"stale exact-node notice cannot reach replacement login");
    const auto stopping=runtime.stop_bbs();check(stopping.callers==1&&stopping.confirmation!=0,"one real replacement caller requires confirmation");
    check(runtime.stop_bbs(stopping.confirmation).response.code==app::LocalControlResponseCode::ok,"confirmed stop joins fresh caller");
    check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"restart before publication failure");
    Caller endangered(runtime.status().telnet_port);endangered.login();
    check(snapshot(runtime,account).online,"real caller online before failed publication");
    check(linkat(config_directory.get(),"SFXSecurity.conf",config_directory.get(),"security-hold",0)==0,"inject unsafe publication using fixture-only hardlink");
    security.expected_revision=security_snapshot->policy.revision;security.level=30;security.label="Established Caller";
    const auto failed_publication=runtime.change_security(security);
    check(failed_publication.committed&&failed_publication.code!=app::LocalAccountResultCode::ok,"database commit cannot disguise failed file publication");
    const auto failed_status=runtime.status();
    check(failed_status.state==app::AlphaRuntimePresentationState::recovery_required&&failed_status.lifecycle.active_work.caller_sessions==0&&failed_status.diagnostic_code=="security_policy_publication_failed","publication failure retires actual caller and requires recovery");
    check(runtime.start_bbs().code!=app::LocalControlResponseCode::ok,"recovery prevents caller service restart");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"management closes after actual worker retirement");
    check(faccessat(config_directory.get(),"security-hold",F_OK,0)==0,"unsafe publication evidence survives shutdown");
    check(unlinkat(config_directory.get(),"security-hold",0)==0,"remove only injected fixture hardlink before recovery");
    // Simulate interruption after DB/public policy commit but before SFX.conf
    // publication, only in this synthetic board after every caller has joined.
    host::publish_private_file(config_directory.get(),"SFX.conf",before_policy,true);
    check(runtime.open().code==app::LocalControlResponseCode::ok,"reopen reconciles interrupted policy publication from authoritative DB");
    check(runtime.configuration()->saved.security_policy_revision==security_snapshot->policy.revision+1&&runtime.security_snapshot(0).levels.size()==4,"recovered configuration retains committed catalog including failed publication");
    check(!runtime.bbs_owned(),"offline publication failure begins without any caller engine");
    check(linkat(config_directory.get(),"SFXSecurity.conf",config_directory.get(),"offline-security-hold",0)==0,"inject offline fixture publication failure");
    security.expected_revision=runtime.security_snapshot(0).policy.revision;security.level=40;security.label="Longtime Caller";
    const auto failed_offline=runtime.change_security(security);
    check(failed_offline.committed&&failed_offline.code!=app::LocalAccountResultCode::ok,"offline committed publication failure retained");
    check(runtime.start_bbs().code==app::LocalControlResponseCode::recovery_required&&!runtime.bbs_owned(),"engine-absent start cannot clear failed policy publication");
    app::AlphaBootstrapRequest redundant_bootstrap;
    check(runtime.bootstrap(redundant_bootstrap).code==app::LocalControlResponseCode::recovery_required,"bootstrap cannot bypass publication recovery latch");
    app::AlphaConfigurationUpdateRequest redundant_save;
    check(runtime.save_configuration(redundant_save).code==app::LocalControlResponseCode::recovery_required,"configuration save cannot clear publication recovery latch");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"close recovered store");
    check(unlinkat(config_directory.get(),"offline-security-hold",0)==0,"remove only offline injected fixture hardlink");
    check(runtime.open().code==app::LocalControlResponseCode::ok&&runtime.security_snapshot(0).levels.size()==5,"reopen explicitly reconciles offline committed policy");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"close offline recovered store");
    auto tampered=host::read_private_file(config_directory.get(),"SFXSecurity.conf",1024U*1024U);tampered.push_back('x');
    host::publish_private_file(config_directory.get(),"SFXSecurity.conf",tampered,true);
    check(runtime.open().code==app::LocalControlResponseCode::recovery_required,"same-revision edited public policy refuses startup");
    check(host::read_private_file(config_directory.get(),"SFXSecurity.conf",1024U*1024U)==tampered,"refused policy evidence preserved exactly");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"close refused fixture without repairing owner file");
    std::cout<<"PASS "<<checks<<" native account host/typed mutation/real revocation and fresh login checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
