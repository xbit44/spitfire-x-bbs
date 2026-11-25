#define main retained_management_catalog_main
#include "linux_management_catalog_tests.cpp"
#undef main
#include "linux_node_controls.hpp"

namespace {
struct ControlledClock final:app::IClock {
    Clock actual;std::atomic<std::uint64_t> offset{};
    std::int64_t unix_seconds()const noexcept override{return actual.unix_seconds();}
    std::uint64_t monotonic_millis()const noexcept override{return actual.monotonic_millis()+offset.load();}
};
void check_control_files(const std::filesystem::path& root){
    const auto base=root/"node-control-test-files";check(std::filesystem::create_directory(base)&&chmod(base.c_str(),0700)==0,"owned controls fixture root");
    const auto make=[&](const char* name){const auto path=base/name;check(std::filesystem::create_directory(path)&&chmod(path.c_str(),0700)==0&&std::filesystem::create_directory(path/"config")&&chmod((path/"config").c_str(),0700)==0,"new private Config fixture");return path;};
    const auto write=[](const std::filesystem::path& path,const char* name,const std::string& value){const auto dir=host::open_owned_directory(path/"config");host::publish_private_file(dir.get(),name,{value.begin(),value.end()},false);};
    const auto defaults=make("absent");check(rt::read_linux_node_controls(defaults)==app::default_node_enablement(),"absent controls default enabled");
    auto flags=app::default_node_enablement();flags[0]=false;flags.back()=false;rt::publish_linux_node_controls(defaults,flags);check(rt::read_linux_node_controls(defaults)==flags,"all current node flags round trip exactly");
    const auto old=make("v2");write(old,"SFXNodes.state","SFX_NODE_CONTROL=2\nnode1_enabled=0\nnode2_enabled=1\nnode3_enabled=0\nnode4_enabled=1\n");const auto legacy=rt::read_linux_node_controls(old);check(!legacy[0]&&legacy[1]&&!legacy[2]&&legacy[3]&&legacy[4],"four-node v2 imports remaining defaults");
    const auto one=make("v1");write(one,"SFXNode1.state","SFX_NODE_CONTROL=1\nnode1_enabled=0\n");const auto legacy_one=rt::read_linux_node_controls(one);check(std::none_of(legacy_one.begin(),legacy_one.end(),[](bool value){return value;}),"v1 disabled control retains Windows migration meaning");
    unsigned index{};for(const std::string text:{std::string{},std::string("SFX_NODE_CONTROL=3\n"),std::string("SFX_NODE_CONTROL=2\nnode1_enabled=2\n"),std::string("SFX_NODE_CONTROL=2\nnode1_enabled=0\nnode2_enabled=1\nnode3_enabled=0\nnode4_enabled=1\nextra\n")}){
        const auto path=make(("invalid"+std::to_string(++index)).c_str());write(path,"SFXNodes.state",text);bool refused{};try{(void)rt::read_linux_node_controls(path);}catch(const std::exception&){refused=true;}check(refused,"malformed controls refuse instead of enabling nodes");
    }
    const auto unsafe=make("symlink");check(symlink((defaults/"config/sfxnodes.state").c_str(),(unsafe/"config/sfxnodes.state").c_str())==0,"owned symlink fixture");bool refused{};try{(void)rt::read_linux_node_controls(unsafe);}catch(const std::exception&){refused=true;}check(refused,"symlink controls refuse");
    refused=false;try{rt::publish_linux_node_controls(unsafe,app::default_node_enablement());}catch(const std::exception&){refused=true;}check(refused&&rt::read_linux_node_controls(defaults)==flags,"unsafe publication preserves external target and refuses");
    const auto shared=make("shared");write(shared,"SFXNodes.state","SFX_NODE_CONTROL=3\n");check(chmod((shared/"config/sfxnodes.state").c_str(),0644)==0,"shared file fixture");refused=false;try{(void)rt::read_linux_node_controls(shared);}catch(const std::exception&){refused=true;}check(refused,"shared controls refuse without permission repair");
}
}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"fresh normal-user board and timezone required");
    check(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"private timezone initialized");
    const std::filesystem::path root(argv[1]);ControlledClock clock;
    check_control_files(root);
    rt::LinuxRuntimeHost runtime(root,clock,[]{return false;});using Code=app::LocalControlResponseCode;
    check(runtime.open().code==Code::ok,"actual Host opens");
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain,60}).code!=Code::ok,"stopped BBS cannot drain");
    check(runtime.start_bbs().code==Code::ok,"actual BBS starts");
    const auto run=runtime.lifecycle_controls().run_sequence;
    check(run!=0,"BBS lifetime identity published");
    auto caller=runtime.open_browser_node(1);check(bool(caller.client),"actual caller attached");(void)await_callers(runtime,1);
    check(runtime.view_node(caller.identity).has_value(),"passive view observes exact caller without reserving a node");
    check(runtime.status().lifecycle.active_work.caller_sessions==1,"view did not add a caller");
    auto stale=caller.identity;++stale.generation;
    check(!runtime.view_node(stale),"passive view refuses another generation");
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain,0}).code!=Code::ok,"zero drain deadline refused");
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain,3601}).code!=Code::ok,"unbounded drain refused");
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain,60},run+1).code!=Code::ok,"stale lifecycle refuses before changing admission");
    const auto drain_bytes=exchange(runtime,app::LocalControlMessageType::lifecycle_command_request,
        app::encode_lifecycle_command_request({app::AlphaLifecycleCommandKind::begin_drain,60}),app::LocalControlMessageType::lifecycle_command_response);
    const auto drain=app::decode_control_response(drain_bytes);check(drain&&drain->code==Code::ok,"typed graceful drain accepted");
    check(runtime.status().lifecycle.state==app::RuntimeState::draining,"running drain truth published");
    check(!runtime.open_browser_node(2).client,"drain refuses another caller");
    check(runtime.run_scheduler_now({app::kBackupJobId}).code==Code::shutting_down,"drain refuses new manual backup work");
    check(runtime.view_node(caller.identity).has_value(),"drain preserves existing caller");
    const auto controls=runtime.lifecycle_controls();check(controls.draining&&controls.drain_remaining_milliseconds>50000&&controls.drain_remaining_milliseconds<=60000,"finite deadline published");
    const auto pending=runtime.stop_bbs();check(pending.confirmation&&runtime.lifecycle_controls().draining,"unconfirmed Stop preserves existing graceful shutdown");
    runtime.cancel_stop();check(runtime.lifecycle_controls().draining,"cancel Stop review does not cancel graceful shutdown");
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::cancel_drain,0},run).code==Code::ok,"explicit Cancel ends drain");
    auto second=runtime.open_browser_node(2);check(bool(second.client),"Cancel makes BBS usable again");(void)await_callers(runtime,2);
    const auto stop=runtime.stop_bbs();check(stop.callers==2&&stop.confirmation,"Stop reviews both exact callers");
    const auto restart=runtime.restart_bbs(run,stop.confirmation);check(restart.confirmation&&restart.confirmation!=stop.confirmation,"Stop ticket cannot authorize Restart");
    const auto crossed=runtime.stop_bbs(restart.confirmation);check(crossed.confirmation&&crossed.confirmation!=restart.confirmation,"Restart ticket cannot authorize Stop");
    check(runtime.disconnect(second.identity),"second caller deliberately disconnected");second.client=host::FileDescriptor{};(void)await_callers(runtime,1);
    const auto changed=runtime.stop_bbs(crossed.confirmation);check(changed.confirmation&&changed.callers==1&&changed.confirmation!=crossed.confirmation,"changed caller set invalidates confirmation");
    const auto review=runtime.restart_bbs(run);check(review.confirmation,"Restart reviews current caller");
    check(runtime.restart_bbs(run,review.confirmation).response.code==Code::ok,"confirmed Restart completes");caller.client=host::FileDescriptor{};
    check(runtime.lifecycle_controls().run_sequence==run+1&&runtime.bbs_owned()&&runtime.management_owned(),"restart advances only BBS lifetime, management stays alive");
    auto replacement=runtime.open_browser_node(1);check(bool(replacement.client),"successor caller admitted");(void)await_callers(runtime,1);
    check(runtime.restart_bbs(run,review.confirmation).response.code!=Code::ok&&runtime.view_node(replacement.identity).has_value(),"lost Restart reply cannot replay against successor");
    check(!runtime.view_node(caller.identity),"passive view never follows successor");
    app::AlphaNodeCommandRequest command;command.node_number=1;command.command=app::AlphaNodeCommandKind::disable;
    check(runtime.command_node(command,caller.identity).code!=Code::ok&&runtime.view_node(replacement.identity).has_value(),"stale Disable does not persist or disconnect successor");
    command.command=app::AlphaNodeCommandKind::disconnect;command.expected_epoch=caller.identity.runtime_epoch;command.expected_generation=caller.identity.generation;
    check(runtime.command_node(command).code!=Code::ok,"typed stale Disconnect refuses");
    command.expected_generation=replacement.identity.generation;command.command=app::AlphaNodeCommandKind::terminate_door;command.expected_door_launch=1;
    check(runtime.command_node(command).code!=Code::ok&&runtime.view_node(replacement.identity).has_value(),"Terminate Door refuses caller with no matching Door");
    command={};command.node_number=1;command.command=app::AlphaNodeCommandKind::disable;
    check(runtime.command_node(command,replacement.identity).code==Code::ok,"reviewed Disable persists and disconnects exact caller");replacement.client=host::FileDescriptor{};(void)await_callers(runtime,0);
    check(!runtime.open_browser_node(1).client&&!rt::read_linux_node_controls(root)[0],"disabled node is persisted and unavailable");
    check(runtime.stop_bbs().response.code==Code::ok&&runtime.start_bbs().code==Code::ok,"BBS restarts with persisted controls");
    check(!runtime.status().nodes.front().enabled&&!runtime.open_browser_node(1).client,"disabled node remains disabled after restart");
    command.command=app::AlphaNodeCommandKind::enable;
    const auto enable_bytes=exchange(runtime,app::LocalControlMessageType::node_command_request,app::encode_node_command_request(command),app::LocalControlMessageType::node_command_response);
    const auto enable=app::decode_control_response(enable_bytes);check(enable&&enable->code==Code::ok,"typed Enable accepted");
    auto deadline_caller=runtime.open_browser_node(1);check(bool(deadline_caller.client),"enabled node usable");(void)await_callers(runtime,1);
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain,60}).code==Code::ok,"deadline drain begins");
    clock.offset+=60001;
    // Host tick is the management loop, with no browser or status polling.
    runtime.tick_controls();
    check(!runtime.bbs_owned()&&runtime.management_owned(),"deadline ends caller/BBS without browser polling, management remains");deadline_caller.client=host::FileDescriptor{};
    check(runtime.start_bbs().code==Code::ok,"BBS starts after completed graceful stop");
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain,900}).code==Code::ok,"idle graceful shutdown accepted");runtime.tick_controls();
    check(!runtime.bbs_owned(),"empty BBS stops before deadline");
    check(runtime.command_lifecycle({app::AlphaLifecycleCommandKind::cancel_drain,0}).code!=Code::ok,"completed drain cannot be cancelled");
    check(runtime.shutdown().code==Code::ok,"management shuts down cleanly");
    rt::LinuxRuntimeHost reopened(root,clock,[]{return false;});check(reopened.open().code==Code::ok&&reopened.start_bbs().code==Code::ok,"new management process state opens persisted controls");
    check(reopened.status().nodes.front().enabled,"final enabled setting retained");check(reopened.shutdown().code==Code::ok,"reopened management closes");
    std::cout<<"PASS "<<checks<<" actual lifecycle, restart, node identity and persisted-control checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
