// Qualification driver: real supplied DOS game, SFX drop files and DoorChannel.
// Game bytes remain external; this is not a Linux BBS Runtime.
#include "sfx/platform/linux/local_door.hpp"
#include <unistd.h>
#include <poll.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <cerrno>

namespace app=sfx::application;namespace host=sfx::platform::linux_host;namespace fs=std::filesystem;
int main(int argc,char** argv){try{
    if((argc!=4&&argc!=5)||geteuid()==0)throw std::runtime_error("fresh root, runtime, owner-supplied SFGUESS.EXE and optional input interval required as non-root");
    const fs::path root=argv[1];if(fs::exists(root))throw std::runtime_error("fresh game root required");
    fs::create_directories(root/"games");fs::create_directory(root/"bin");
    fs::copy_file(argv[3],root/"games/SFGUESS.EXE");host::ensure_door_nodes(root,1);
    std::ofstream(root/"bin/sfxexec.ini")<<"CanDisconnect=false\n";
    app::DoorDefinition door;door.id=1;door.code="OGSFGUESS";door.name="Original DOS Guess";door.enabled=true;
    door.kind=app::DoorExecutionKind::local;door.program.type=app::DoorProgramType::dos;door.program.io=app::DoorIo::uart_only;
    door.program.startup_directory="games";door.program.command_line="SFGUESS.EXE";
    door.program.drop_file=app::DoorDropFile::sfdoors_dat;door.program.drop_location=app::DoorDropLocation::startup;
    app::DoorDropContext caller;caller.account=42;caller.node=1;caller.nodes=1;caller.alias="Linux Guess Tester";
    caller.user_key="42";caller.board="SFX DOS Qualification";caller.security=10;caller.remaining_seconds=120;
    caller.ansi=true;caller.date="11/08/26";caller.clock="12:30";caller.logon_date=caller.date;
    caller.logon_clock=caller.clock;caller.deadline_utc="2026-11-08T12:32:00Z";
    host::DoorOptions options;options.dosemu_root=argv[2];bool captured{};
    if(argc==5){const auto value=std::stoul(argv[4]);if(value>100)throw std::runtime_error("input interval must be 0..100 ms");options.dos_serial_input_interval_ms=static_cast<unsigned>(value);}
    const auto started=std::chrono::steady_clock::now();app::DoorChannel channel;
    channel.read=[&](std::size_t maximum){
        pollfd fd{STDIN_FILENO,POLLIN,0};const auto ready=poll(&fd,1,0);
        if(ready<0&&errno!=EINTR)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};
        if(ready<=0)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};
        std::vector<std::uint8_t> bytes(std::min<std::size_t>(maximum,4096));const auto count=read(STDIN_FILENO,bytes.data(),bytes.size());
        if(count<=0)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};
        bytes.resize(static_cast<std::size_t>(count));return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(bytes)};
    };
    channel.write=[&](std::string_view bytes){
        if(!captured&&fs::exists(root/"games/SFDOORS.DAT")){fs::copy_file(root/"games/SFDOORS.DAT",root/"observed-SFDOORS.DAT");captured=true;}
        const auto count=write(STDOUT_FILENO,bytes.data(),bytes.size());
        return app::InteractiveStreamWriteResult{count<0?app::InteractiveStreamOutcome::disconnected:app::InteractiveStreamOutcome::completed,count>0?static_cast<std::size_t>(count):0};
    };
    channel.stop=[&]()->std::optional<app::DoorEnd>{if(std::chrono::steady_clock::now()-started>std::chrono::seconds(65))return app::DoorEnd::calling_time;return {};};
    const auto result=host::run_local_door(root,door,caller,channel,options);
    std::cerr<<"SFX_RESULT end="<<app::door_result_code(result.end)<<" exit="<<result.exit_code<<" clean="<<result.cleanup_complete<<" captured_drop="<<captured<<" diagnostic="<<result.diagnostic<<'\n';
    return result.end==app::DoorEnd::returned&&result.exit_code==0&&result.cleanup_complete&&captured&&
        !fs::exists(root/"games/SFDOORS.DAT")&&!fs::exists(root/"games/sfxdoor.lck")?0:1;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
