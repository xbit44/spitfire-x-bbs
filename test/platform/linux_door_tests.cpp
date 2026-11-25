#include "sfx/platform/linux/local_door.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <future>
#include <atomic>
namespace app=sfx::application;namespace host=sfx::platform::linux_host;namespace fs=std::filesystem;
unsigned checks{};void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"unprivileged Linux fixture required");const fs::path root=argv[1];check(!fs::exists(root),"fresh Linux fixture required");fs::create_directories(root/"games");fs::create_directory(root/"bin");fs::copy_file(argv[2],root/"games/fixture");fs::permissions(root/"games/fixture",fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
    host::ensure_door_nodes(root,3);std::ofstream(root/"node/3/KEEP")<<"keep";host::ensure_door_nodes(root,2);check(fs::exists(root/"node/3/KEEP"),"node reduction preserves data");
    app::DoorDefinition door;door.id=1;door.code="NATIVE";door.name="Linux Fixture";door.enabled=true;door.kind=app::DoorExecutionKind::local;door.program.type=app::DoorProgramType::native;door.program.io=app::DoorIo::standard;door.program.startup_directory=(root/"games").string();door.program.command_line="fixture stdio";door.program.drop_file=app::DoorDropFile::none;
    app::DoorDropContext caller;caller.account=42;caller.node=1;caller.nodes=3;caller.alias="Test Caller";caller.user_key="42";caller.board="Test BBS";caller.security=10;caller.remaining_seconds=600;caller.ansi=true;caller.date="10/25/26";caller.clock="12:30";caller.logon_date=caller.date;caller.logon_clock=caller.clock;caller.deadline_utc="2026-10-25T12:40:00Z";
    host::DoorOptions options;std::string input="hello q";app::DoorEnd forced=app::DoorEnd::returned;bool lose{};
    const auto run=[&](app::DoorEnd expected){std::string output;bool sent{};const auto began=std::chrono::steady_clock::now();app::DoorChannel channel;
        channel.read=[&](std::size_t maximum){if(lose&&output.find("READY")!=output.npos)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};if(sent||output.find("READY")==output.npos)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};check(input.size()<=maximum,"bounded input");sent=true;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{input.begin(),input.end()}};};
        channel.write=[&](std::string_view b){const auto n=std::min<std::size_t>(101,b.size());output.append(b.data(),n);return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,n};};
        channel.stop=[&]()->std::optional<app::DoorEnd>{const auto elapsed=std::chrono::steady_clock::now()-began;if(forced!=app::DoorEnd::returned&&elapsed>std::chrono::milliseconds(80))return forced;if(elapsed>std::chrono::seconds(12))return app::DoorEnd::calling_time;return {};};
        const auto result=host::run_local_door(root,door,caller,channel,options);std::cout<<door.program.command_line<<" "<<app::door_result_code(result.end)<<" "<<result.diagnostic<<" exit="<<result.exit_code<<std::endl;check(result.end==expected&&result.cleanup_complete,"expected result and owned cleanup");return output;};
    // An intentionally inheritable high descriptor must never enter the child.
    const int source=open("/dev/null",O_RDONLY);check(source>=0,"sentinel open");const int high=fcntl(source,F_DUPFD,100);close(source);check(high>=100,"high descriptor sentinel");
    for(const bool pty:{false,true}){options.pseudo_terminal=pty;door.program.cleanup_command="fixture cleanup \"%N/CLEANED\"";check(run(app::DoorEnd::returned)=="LINUX READY\r\nhello LINUX RETURN\r\n","stdio and raw PTY exact partial IO, stderr isolated");check(fs::exists(root/"node/1/CLEANED"),"cleanup after actual launch");}close(high);door.program.cleanup_command.clear();
    door.program.io=app::DoorIo::socket;door.program.command_line="fixture socket";door.program.drop_file=app::DoorDropFile::door32_sys;check(run(app::DoorEnd::returned)=="LINUX READY\r\nhello LINUX RETURN\r\n","private socket fd3 exact bytes");check(!fs::exists(root/"node/1/DOOR32.SYS"),"socket drop removed");
    door.program.io=app::DoorIo::standard;for(const auto placement:{app::DoorDropLocation::node,app::DoorDropLocation::startup,app::DoorDropLocation::temporary}){door.program.drop_location=placement;door.program.drop_file=app::DoorDropFile::bbsdev_drp;door.program.command_line="fixture bbsdev";run(app::DoorEnd::returned);}
    door.program.drop_location=app::DoorDropLocation::node;door.program.drop_file=app::DoorDropFile::none;door.program.command_line="fixture bulk";input="q";check(run(app::DoorEnd::returned)=="LINUX READY\r\n"+std::string(131072,'X')+"LINUX RETURN\r\n","128 KiB output survives bounded buffers and partial writes");
    door.program.command_line="fixture stdio";input=std::string(1,29)+"D";run(app::DoorEnd::escaped);input=std::string(2,29)+"q";check(run(app::DoorEnd::returned)=="LINUX READY\r\n"+std::string(1,29)+"LINUX RETURN\r\n","literal escape");
    input="q";door.program.command_line="fixture fail";run(app::DoorEnd::launch_failed);door.program.command_line="missing";run(app::DoorEnd::launch_failed);
    door.program.command_line="fixture wait";for(const auto end:{app::DoorEnd::operator_terminated,app::DoorEnd::calling_time,app::DoorEnd::inactivity,app::DoorEnd::shutdown}){forced=end;run(end);}forced=app::DoorEnd::returned;
    door.program.command_line="fixture stdio";lose=true;run(app::DoorEnd::caller_disconnected);lose=false;
    door.program.io=app::DoorIo::socket;door.program.command_line="fixture close";run(app::DoorEnd::game_disconnect);std::ofstream(root/"games/sfxexec.ini")<<"CanDisconnect=false\n";run(app::DoorEnd::returned);
    door.program.io=app::DoorIo::standard;door.program.type=app::DoorProgramType::dos;door.program.command_line="game.exe";run(app::DoorEnd::launch_failed);
    door.program.type=app::DoorProgramType::native;door.program.command_line="fixture stdio";
    fs::rename(root/"games/sfxexec.ini",root/"games/ini-before-fifo");
    check(mkfifo((root/"games/sfxexec.ini").c_str(),0600)==0,"FIFO Door settings without writer");
    run(app::DoorEnd::launch_failed);
    check(fs::is_fifo(root/"games/sfxexec.ini"),"FIFO settings preserved after refusal");
    check(unlink((root/"games/sfxexec.ini").c_str())==0,"remove only added settings FIFO");
    fs::rename(root/"games/ini-before-fifo",root/"games/sfxexec.ini");
    check(mkfifo((root/"games/fifo-program").c_str(),0700)==0,"FIFO executable without writer");
    door.program.command_line="fifo-program";run(app::DoorEnd::launch_failed);
    check(fs::is_fifo(root/"games/fifo-program"),"FIFO executable preserved after refusal");
    door.program.command_line="fixture stdio";input="hello q";
    check(run(app::DoorEnd::returned)=="LINUX READY\r\nhello LINUX RETURN\r\n","valid Door still runs after special-file refusals");
    std::ofstream(root/"games/sfxexec.ini")<<"CanDisconnect=false\nDosInputIntervalMs=100\n";
    check(run(app::DoorEnd::returned)=="LINUX READY\r\nhello LINUX RETURN\r\n","Linux native Door ignores serial DOS pacing setting");
    check(!fs::exists(root/"node/1/SFXDOOR.LCK"),"all launch reservations released");std::cout<<"PASS "<<checks<<" actual unprivileged Linux adapter checks; DOS execution is exercised separately\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
