#include <winsock2.h>
#include <windows.h>
#include "alpha_local_door.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <future>
#include <atomic>
namespace app=sfx::application;
namespace runtime=sfx::apps::runtime;
namespace inst=sfx::platform::win32::installation;
namespace {
unsigned checks{};
void check(bool value,const char* what){++checks;if(!value)throw std::runtime_error(what);}
}
int main(int argc,char** argv){try{
    check(argc==3,"fresh evidence directory and exact native fixture required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve previous native fixtures");std::filesystem::create_directories(root/"Door With Spaces");
    const auto program=root/"Door With Spaces"/"native-fixture.exe";std::filesystem::copy_file(argv[2],program);const auto owner=inst::operator_sid();runtime::ensure_door_node_directories(root,owner,3);
    {std::ofstream keep(root/"Node"/"3"/"KEEP.DAT");keep<<"preserve on reduction";}runtime::ensure_door_node_directories(root,owner,2);check(std::filesystem::exists(root/"Node"/"3"/"KEEP.DAT"),"node count reduction preserves data");
    WSADATA data{};check(WSAStartup(MAKEWORD(2,2),&data)==0,"fixture Winsock");
    app::DoorDefinition door;door.id=1;door.name="Native Fixture";door.code="NATIVE";door.enabled=true;door.kind=app::DoorExecutionKind::local;door.security_level=door.execution.level=10;door.program.type=app::DoorProgramType::native;door.program.io=app::DoorIo::standard;door.program.startup_directory=(root/"Door With Spaces").u8string();
    runtime::LocalDoorCaller caller{42,2,10,"Fixture Caller","Fixture BBS",600,true};
    for(const bool socket:{false,true}){
        door.program.command_line=socket?"native-fixture.exe --socket-contract \"%D/DOOR32.SYS\"":"native-fixture.exe --stdio";door.program.io=socket?app::DoorIo::socket:app::DoorIo::standard;door.program.drop_file=socket?app::DoorDropFile::door32_sys:app::DoorDropFile::none;
        door.program.cleanup_command="native-fixture.exe --cleanup \"%N/CLEANED.TXT\"";
        std::string output;bool sent{};unsigned keyboard{};const auto started=GetTickCount64();app::DoorChannel channel;
        channel.read=[&](std::size_t maximum){if(sent||output.find("SFX LOCAL READY")==std::string::npos)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};const std::string input="hello q";check(maximum>=input.size(),"bounded caller read");sent=true;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{input.begin(),input.end()}};};
        channel.write=[&](std::string_view bytes){const auto count=std::min<std::size_t>(3,bytes.size());output.append(bytes.data(),count);return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,count};};
        channel.keyboard=[&]{++keyboard;};channel.stop=[&]()->std::optional<app::DoorEnd>{if(GetTickCount64()-started>10000)return app::DoorEnd::calling_time;return {};};
        const auto result=runtime::run_alpha_local_door(root,owner,door,caller,channel);
        std::cout<<(socket?"socket":"stdio")<<" end="<<app::door_result_code(result.end)<<" diagnostic="<<result.diagnostic<<" exit="<<result.exit_code<<"\n";
        check(result.end==app::DoorEnd::returned&&result.exit_code==0&&result.cleanup_complete&&!result.cleanup_command_failed,"owned native Door and cleanup return normally");
        check(output=="SFX LOCAL READY\r\nhello SFX LOCAL RETURN\r\n"&&keyboard==1,"partial caller writes preserve exact bytes and actual input resets AFK");check(std::filesystem::exists(root/"Node"/"2"/"CLEANED.TXT"),"bounded cleanup command ran in captured context");check(!std::filesystem::exists(root/"Node"/"2"/"DOOR32.SYS"),"owned drop file removed after cleanup");
    }
    door.program.io=app::DoorIo::standard;door.program.drop_file=app::DoorDropFile::none;door.program.cleanup_command.clear();
    for(const auto ending:{app::DoorEnd::operator_terminated,app::DoorEnd::calling_time,app::DoorEnd::inactivity,app::DoorEnd::shutdown}){
        door.program.command_line="native-fixture.exe --wait";const auto began=GetTickCount64();app::DoorChannel channel;
        channel.read=[](std::size_t){return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};channel.write=[](std::string_view bytes){return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,bytes.size()};};channel.stop=[&]()->std::optional<app::DoorEnd>{if(GetTickCount64()-began>=100)return ending;return {};};
        const auto result=runtime::run_alpha_local_door(root,owner,door,caller,channel);check(result.end==ending&&result.cleanup_complete,"bounded operator/time/AFK/shutdown cleanup preserves cause");check(GetTickCount64()-began<3000,"cancellation returns promptly");
    }
    std::filesystem::create_directory(root/"Bin");
    const auto run=[&](std::string input,app::DoorEnd expected){
        std::string output;bool sent=false;const auto began=GetTickCount64();app::DoorChannel channel;
        channel.read=[&](std::size_t){if(sent||output.find("SFX LOCAL READY")==std::string::npos)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};sent=true;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{input.begin(),input.end()}};};
        channel.write=[&](std::string_view text){output+=text;return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,text.size()};};channel.stop=[&]()->std::optional<app::DoorEnd>{if(GetTickCount64()-began>=10000)return app::DoorEnd::calling_time;return {};};
        const auto result=runtime::run_alpha_local_door(root,owner,door,caller,channel);std::cout<<door.program.command_line<<" end="<<app::door_result_code(result.end)<<" diagnostic="<<result.diagnostic<<"\n";
        check(result.end==expected&&result.cleanup_complete,"expanded adapter case returns expected cause with owned cleanup");return output;
    };
    for(const auto placement:{app::DoorDropLocation::node,app::DoorDropLocation::startup,app::DoorDropLocation::temporary}){
        door.program.drop_location=placement;door.program.drop_file=app::DoorDropFile::bbsdev_drp;door.program.command_line="native-fixture.exe --bbsdev";
        check(run("q",app::DoorEnd::returned)=="SFX LOCAL READY\r\nSFX LOCAL RETURN\r\n","real BBSDEV consumer sees context and unquoted environment in each placement");
        check(!std::filesystem::exists(root/"Node"/"2"/"BBSDEV.DRP")&&!std::filesystem::exists(root/"Door With Spaces"/"BBSDEV.DRP"),"owned fixed drop files removed");
    }
    for(const auto& entry:std::filesystem::directory_iterator(root/"Node"/"2"))check(entry.path().filename().wstring().rfind(L"Door-",0)!=0,"empty owned temporary directory removed");
    caller.drop_context.file_time="2026-11-04T06:45:32-08:00";
    caller.drop_context.software_version="1.0.0-alpha.2";
    caller.drop_context.user_key="sfx-fixture-key";
    door.program.drop_file=app::DoorDropFile::doordrop_ini;
    door.program.drop_location=app::DoorDropLocation::temporary;
    for(const bool socket:{false,true}){
        door.program.io=socket?app::DoorIo::socket:app::DoorIo::standard;
        door.program.command_line=socket?"native-fixture.exe --doordrop-socket \"%D\\DOORDROP.INI\"":
            "native-fixture.exe --doordrop-stdio \"%D\\DOORDROP.INI\"";
        check(run("hello q",app::DoorEnd::returned)=="SFX LOCAL READY\r\nhello SFX LOCAL RETURN\r\n",
              "real child independently parses the INI at the argument/environment path and exchanges caller bytes");
    }
    for(const auto& entry:std::filesystem::directory_iterator(root/"Node"/"2"))
        check(entry.path().filename().wstring().rfind(L"Door-",0)!=0,"DOORDROP.INI removed with owned temporary directory");
    door.program.drop_location=app::DoorDropLocation::node;door.program.drop_file=app::DoorDropFile::none;door.program.command_line="native-fixture.exe --stderr";
    door.program.io=app::DoorIo::standard;
    check(run("q",app::DoorEnd::returned).find("DIAGNOSTIC")==std::string::npos,"standard error is not caller output");
    door.program.command_line="native-fixture.exe --stdio";check(run(std::string(1,char(29))+"D",app::DoorEnd::escaped)=="SFX LOCAL READY\r\n","local escape returns without forwarding command");
    check(run(std::string(2,char(29))+"q",app::DoorEnd::returned)=="SFX LOCAL READY\r\n"+std::string(1,char(29))+"SFX LOCAL RETURN\r\n","doubled escape sends one literal byte");
    for(const bool socket:{false,true}){
        door.program.io=socket?app::DoorIo::socket:app::DoorIo::standard;
        door.program.drop_file=socket?app::DoorDropFile::door32_sys:app::DoorDropFile::none;
        door.program.command_line=socket?"native-fixture.exe --socket-contract \"%D/DOOR32.SYS\"":"native-fixture.exe --stdio";
        std::string output;unsigned sent{};std::uint64_t prefix_at{};const auto began=GetTickCount64();app::DoorChannel delayed;
        delayed.read=[&](std::size_t){
            if(output.find("SFX LOCAL READY")!=std::string::npos){
                if(!sent){sent=1;prefix_at=GetTickCount64();return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{29}};}
                if(sent==1&&GetTickCount64()-prefix_at>=900){sent=2;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{'D'}};}
            }return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};
        delayed.write=[&](std::string_view bytes){output+=bytes;return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,bytes.size()};};
        delayed.stop=[&]()->std::optional<app::DoorEnd>{if(GetTickCount64()-began>5000)return app::DoorEnd::operator_terminated;return {};};
        const auto result=runtime::run_alpha_local_door(root,owner,door,caller,delayed);
        check(result.end==app::DoorEnd::escaped&&result.cleanup_complete&&sent==2,"human-paced split escape returns and reaps stdio/socket Door");
        check(output=="SFX LOCAL READY\r\n","escape bytes never reach game or caller");
    }
    door.program.io=app::DoorIo::socket;door.program.drop_file=app::DoorDropFile::door32_sys;door.program.command_line="native-fixture.exe --socket-close \"%D/DOOR32.SYS\"";
    {std::ofstream ini(root/"Bin"/"sfxexec.ini");ini<<"; retain custom shared INI\nYieldInterval=0.25\nCanDisconnect=true\n";}
    run("",app::DoorEnd::game_disconnect);
    {std::ofstream ini(root/"Door With Spaces"/"sfxexec.ini");ini<<"CanDisconnect=false\n";}
    run("",app::DoorEnd::returned);
    {std::ifstream ini(root/"Bin"/"sfxexec.ini");std::string first;std::getline(ini,first);check(first=="; retain custom shared INI","custom shared INI preserved");}
    door.program.io=app::DoorIo::standard;door.program.command_line="native-fixture.exe --remove-drop \"%D/DOOR32.SYS\"";run("",app::DoorEnd::returned);
    door.program.drop_location=app::DoorDropLocation::startup;door.program.command_line="native-fixture.exe --wait";
    std::atomic<bool> stop{};app::DoorChannel held;held.read=[](std::size_t){return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};held.write=[](std::string_view value){return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,value.size()};};held.stop=[&]()->std::optional<app::DoorEnd>{if(stop)return app::DoorEnd::operator_terminated;return {};};
    const auto first=door;auto running=std::async(std::launch::async,[&]{return runtime::run_alpha_local_door(root,owner,first,caller,held);});
    const auto until=GetTickCount64()+3000;while(!std::filesystem::exists(root/"Door With Spaces"/"SFXDOOR.LCK")&&GetTickCount64()<until)Sleep(5);
    const auto second=runtime::run_alpha_local_door(root,owner,door,caller,held);stop=true;const auto completed=running.get();
    check(second.end==app::DoorEnd::busy&&second.cleanup_complete,"fixed directory reservation prevents a concurrent writer");check(completed.cleanup_complete,"first owner releases files after concurrent refusal");
    // Failed owned fixed drops are preserved by exact native identity in a
    // private review folder only after reaping. A new caller on the SAME node
    // can then use fresh fixed drops; no failed-job cleanup command is run.
    caller.node=1;door.program.drop_location=app::DoorDropLocation::node;door.program.io=app::DoorIo::standard;
    door.program.drop_file=app::DoorDropFile::door32_sys;
    const auto release=root/"release-failed-door",node=root/"Node"/"1";
    door.program.command_line="native-fixture.exe --wait-fail \""+release.u8string()+"\"";
    door.program.cleanup_command="native-fixture.exe --cleanup \"%N/FAILED-CLEANUP.TXT\"";
    {std::ofstream unknown(node/"scs-owner-unknown.tmp",std::ios::binary);unknown<<"unknown node work stays exact";}
    app::DoorChannel failing;failing.read=[](std::size_t){return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};
    failing.write=[](std::string_view value){return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,value.size()};};
    failing.stop=[]()->std::optional<app::DoorEnd>{return {};};
    auto failed_future=std::async(std::launch::async,[&]{return runtime::run_alpha_local_door(root,owner,door,caller,failing);});
    const auto drop_path=node/"DOOR32.SYS";const auto deadline=GetTickCount64()+3000;
    std::vector<std::uint8_t> before;
    while(GetTickCount64()<deadline){try{before=inst::read_file(drop_path,8192);if(!before.empty())break;}catch(...){ }Sleep(5);}
    check(!before.empty(),"failed-job real drop sealed before releasing fixture");
    auto prior=inst::open_object(drop_path,false,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);const auto prior_id=inst::file_id(prior.get());
    {std::ofstream released(release);released<<"controlled failure now";}
    const auto failed=failed_future.get();
    check(failed.end==app::DoorEnd::launch_failed&&failed.exit_code==7&&failed.cleanup_complete&&failed.work_preserved&&!failed.retained_location.empty(),"failed child reaped and review disposition confirmed");
    check(!std::filesystem::exists(drop_path)&&!std::filesystem::exists(node/"SFXDOOR.LCK")&&!std::filesystem::exists(node/"FAILED-CLEANUP.TXT"),"failed fixed names released; cleanup command not executed");
    const auto review=node/std::filesystem::u8path(failed.retained_location.substr(failed.retained_location.find('/')+1));
    check(inst::read_file(review/"DOOR32.SYS",8192)==before&&inst::file_id(inst::open_object(review/"DOOR32.SYS",false,FILE_READ_ATTRIBUTES).get())==prior_id,"retained failed drop keeps exact bytes and native identity");prior.reset();
    door.program.command_line="native-fixture.exe --remove-drop \"%D/DOOR32.SYS\"";door.program.cleanup_command.clear();
    const auto next=runtime::run_alpha_local_door(root,owner,door,caller,failing);
    check(next.end==app::DoorEnd::returned&&next.cleanup_complete&&!next.work_preserved,"next caller on same node runs with fresh drops after failure");
    const auto unknown=inst::read_file(node/"scs-owner-unknown.tmp",8192);
    check(std::string(unknown.begin(),unknown.end())=="unknown node work stays exact"&&inst::read_file(review/"DOOR32.SYS",8192)==before,"later success preserves failed review and unknown Node tmp bytes");
    WSACleanup();std::cout<<"PASS "<<checks<<" native Door checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
