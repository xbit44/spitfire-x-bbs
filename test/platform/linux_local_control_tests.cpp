#include "linux_runtime_store.hpp"
#include "sfx/platform/linux/local_control.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace app=sfx::application;
namespace host=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace fs=std::filesystem;
using Type=app::LocalControlMessageType;
unsigned checks{};
void require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
struct Clock final:app::IClock {
    std::int64_t unix_seconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::uint64_t monotonic_millis() const noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};
struct Revocations final:app::ISessionRevocationSink {
    bool revoke(const app::SessionRevocationCommand& command) override {
        return command.kind==app::SessionRevocationKind::sysop_level_all;
    }
};
struct Child {
    pid_t pid{-1};
    ~Child() { if (pid>0) { kill(pid,SIGKILL);int status{};while(waitpid(pid,&status,0)<0 && errno==EINTR){} } }
    int join() {int status{};const auto result=waitpid(pid,&status,0);pid=-1;return result>0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;}
};
void directory(const fs::path& path) {
    require(fs::create_directory(path) && chmod(path.c_str(),0700)==0,"private exclusive fixture directory");
}
int server(const fs::path& root,int signal_fd) {
    Clock clock;Revocations revocations;
    rt::LinuxRuntimeStore store(root,revocations,clock,[]{return false;});
    if (store.open().code!=app::LocalControlResponseCode::setup_required) return 2;
    host::LinuxLocalControlServer control(root/"management");
    if (!control.open()) return 3;
    const auto epoch=store.identifiers().generate();std::uint64_t sequence{};
    std::atomic<bool> stop{false};bool done=false;
    if (write(signal_fd,"R",1)!=1) return 4;
    close(signal_fd);
    const auto deadline=clock.monotonic_millis()+45000;
    while (!done && clock.monotonic_millis()<deadline) {
        (void)control.serve_one(stop,[&](app::LocalControlFrame& frame)->std::optional<app::LocalControlFrame> {
            app::LocalControlFrame response;response.request_id=frame.request_id;
            if (frame.type==Type::status_request && frame.payload.empty()) {
                response.type=Type::status_response;
                response.payload=app::encode_runtime_status(store.stopped_snapshot(epoch,++sequence));
            } else if (frame.type==Type::bootstrap_request) {
                auto request=app::decode_bootstrap_request(frame.payload);
                if (!request) return std::nullopt;
                const auto result=store.bootstrap(*request);
                app::erase_bootstrap_secrets(*request);
                response.type=Type::bootstrap_response;response.payload=app::encode_control_response(result);
            } else if (frame.type==Type::configuration_snapshot_request && frame.payload.empty() && store.configuration()) {
                response.type=Type::configuration_snapshot_response;
                response.payload=app::encode_configuration_snapshot({*store.configuration(),*store.configuration(),
                    app::AlphaConfigurationActivation::saved_next_start,"BBS is stopped"});
            } else if (frame.type==Type::configuration_update_request) {
                const auto candidate=app::decode_configuration_update_request(frame.payload);
                if (!candidate) return std::nullopt;
                response.type=Type::configuration_update_response;
                response.payload=app::encode_configuration_update_result(store.save_stopped_configuration(*candidate));
            } else if (frame.type==Type::shutdown_request && frame.payload.empty()) {
                response.type=Type::shutdown_response;
                response.payload=app::encode_control_response(store.close_for_maintenance());done=true;
            } else return std::nullopt;
            if (response.payload.empty()) return std::nullopt;
            return response;
        });
    }
    return done ? 0 : 5;
}
void run(const fs::path& root,const fs::path& tzdata) {
    require(geteuid()!=0,"normal account");
    require(host::LinuxBbsTimeZoneRuleProvider::initialize(tzdata),"private bundled timezone");
    directory(root);directory(root/"management");
    const auto management=root/"management";
    std::atomic<bool> stop{false};app::LocalControlFrame request;
    request.request_id=1;request.type=Type::status_request;
    require(!host::exchange_local_control(management,request,Type::status_response,stop),"missing control endpoint fails closed");
    int signal_pipe[2]{};require(pipe(signal_pipe)==0,"child readiness pipe");
    Child child;child.pid=fork();require(child.pid>=0,"separate Runtime/control process");
    if (child.pid==0) {
        close(signal_pipe[0]);int result=6;
        try {result=server(root,signal_pipe[1]);} catch (...) {}
        _exit(result);
    }
    close(signal_pipe[1]);char ready{};
    require(read(signal_pipe[0],&ready,1)==1 && ready=='R',"actual owner process is ready");close(signal_pipe[0]);
    host::LinuxLocalControlServer competitor(management);
    require(!competitor.open(),"second control server cannot replace endpoint");
    const auto clone=root/"copied-management";directory(clone);
    require(fs::copy_file(management/"control.endpoint",clone/"control.endpoint") &&
        chmod((clone/"control.endpoint").c_str(),0600)==0,"copy endpoint as a whole-folder backup would");
    require(!host::exchange_local_control(clone,request,Type::status_response,stop),
        "copied folder cannot control original live board");
    host::LinuxLocalControlServer clone_server(clone);
    require(clone_server.open(),"copied management starts its own fresh control identity");
    std::thread clone_worker([&] {
        (void)clone_server.serve_one(stop,[](app::LocalControlFrame& frame)->std::optional<app::LocalControlFrame> {
            app::LocalControlFrame reply;reply.type=Type::status_response;reply.request_id=frame.request_id;
            reply.payload=app::encode_control_response({app::LocalControlResponseCode::ok,"copy_only"});
            return reply;
        });
    });
    const auto clone_reply=host::exchange_local_control(clone,request,Type::status_response,stop);
    clone_worker.join();
    require(clone_reply && app::decode_control_response(clone_reply->payload) &&
        app::decode_control_response(clone_reply->payload)->diagnostic_code=="copy_only",
        "copy talks to its own live server while original remains running");
    clone_server.close();
    auto response=host::exchange_local_control(management,request,Type::status_response,stop);
    require(response.has_value(),"real process typed status exchange");
    auto status=app::decode_runtime_status(response->payload);
    require(status && status->state==app::AlphaRuntimePresentationState::setup_required &&
        status->lifecycle.state==app::RuntimeState::stopped && !status->telnet_active && !status->ssh_active,
        "opening control host leaves BBS stopped");
    require(status->security_policy_revision>0 && status->security_policy_hash.size()==64,"authoritative bootstrap policy over typed status");
    ++request.request_id;request.version.major=0;
    require(!host::exchange_local_control(management,request,Type::status_response,stop),"wrong protocol version refused");
    request.version=app::kAlphaLocalControlVersion;
    ++request.request_id;request.payload.resize(app::kLocalControlMaximumPayloadBytes+1);
    require(!host::exchange_local_control(management,request,Type::status_response,stop,200),"oversized outgoing frame refused");
    request.payload.clear();++request.request_id;
    require(!host::exchange_local_control(management,request,Type::bootstrap_response,stop),"wrong expected reply type refused");
    stop.store(true);
    require(!host::exchange_local_control(management,request,Type::status_response,stop),"cancelled operation never dispatched");stop.store(false);
    app::AlphaBootstrapRequest setup;
    setup.configuration.bbs_name="Native IPC fixture";setup.configuration.bbs_time_zone="Etc/UTC";
    setup.configuration.telnet.bind_address="127.0.0.1";setup.configuration.ssh.bind_address="127.0.0.1";
    setup.configuration.security_policy_revision=status->security_policy_revision;
    setup.configuration.security_policy_hash=status->security_policy_hash;
    setup.sysop_name="IPC Sysop";setup.sysop_password="Native IPC secret 83";setup.system_password="Native IPC system 71";
    request.type=Type::bootstrap_request;request.payload=app::encode_bootstrap_request(setup);++request.request_id;
    require(!request.payload.empty(),"shared typed bootstrap request encoding");
    app::erase_bootstrap_secrets(setup);
    response=host::exchange_local_control(management,request,Type::bootstrap_response,stop,30000);
    std::fill(request.payload.begin(),request.payload.end(),0);request.payload.clear();
    require(response.has_value(),"actual cross-process bootstrap transaction returned");
    const auto completed=app::decode_control_response(response->payload);
    require(completed && completed->code==app::LocalControlResponseCode::ok,"typed bootstrap completed real database setup");
    request.type=Type::configuration_snapshot_request;++request.request_id;
    response=host::exchange_local_control(management,request,Type::configuration_snapshot_response,stop);
    require(response.has_value(),"typed configuration snapshot from owning process");
    const auto snapshot=app::decode_configuration_snapshot(response->payload);
    require(snapshot.has_value(),"shared configuration snapshot codec");
    app::AlphaConfigurationUpdateRequest change;
    change.expected_revision=snapshot->saved.configuration_revision;change.configuration=snapshot->saved;
    ++change.configuration.configuration_revision;change.configuration.bbs_name="IPC saved settings fixture";
    request.type=Type::configuration_update_request;request.payload=app::encode_configuration_update_request(change);++request.request_id;
    response=host::exchange_local_control(management,request,Type::configuration_update_response,stop);
    require(response.has_value(),"cross-process typed settings save");
    const auto saved=app::decode_configuration_update_result(response->payload);
    require(saved && saved->code==app::LocalControlResponseCode::ok &&
        saved->activation==app::AlphaConfigurationActivation::saved_next_start,"settings remain saved for next BBS Start");
    ++request.request_id;
    response=host::exchange_local_control(management,request,Type::configuration_update_response,stop);
    require(response && app::decode_configuration_update_result(response->payload) &&
        app::decode_configuration_update_result(response->payload)->diagnostic_code=="configuration_revision_conflict",
        "stale cross-process settings save refused");
    request.payload.clear();
    request.type=Type::status_request;++request.request_id;
    response=host::exchange_local_control(management,request,Type::status_response,stop);
    require(response.has_value(),"status after bootstrap");status=app::decode_runtime_status(response->payload);
    require(status && status->bootstrap_complete && status->configuration_loaded &&
        status->state==app::AlphaRuntimePresentationState::stopped && status->nodes.empty() &&
        !status->telnet_active && !status->ssh_active,"completed setup still requires explicit BBS Start");
    require(status->bbs_name==change.configuration.bbs_name && status->configuration_revision==change.configuration.configuration_revision,
        "status reports saved settings without starting caller listeners");
    request.type=Type::shutdown_request;++request.request_id;
    response=host::exchange_local_control(management,request,Type::shutdown_response,stop);
    require(response.has_value() && app::decode_control_response(response->payload)->code==app::LocalControlResponseCode::ok,
        "typed maintenance closes actual storage");
    require(child.join()==0,"owner process exits cleanly");
    require(!host::exchange_local_control(management,request,Type::shutdown_response,stop),"stale endpoint cannot attach to an unrelated process");
    require(competitor.open(),"new server publishes fresh endpoint after real owner exits");competitor.close();
    Clock clock;Revocations revocations;rt::LinuxRuntimeStore reopened(root,revocations,clock,[]{return false;});
    require(reopened.open().code==app::LocalControlResponseCode::ok,"new owner reacquires completed database after IPC maintenance");
}
}
int main(int argc,char** argv) {
    try {require(argc==3,"fresh fixture root and timezone data required");run(argv[1],argv[2]);
        std::cout<<"PASS "<<checks<<" native cross-process control/bootstrap checks\n";return 0;}
    catch (const std::exception& error) {std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
}
