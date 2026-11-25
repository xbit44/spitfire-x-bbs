// Isolated real-game probe. Never runs against installed or owner game data.
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include "alpha_local_door.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
namespace runtime=sfx::apps::runtime;
namespace inst=sfx::platform::win32::installation;
void capture_child_console(const char* prefix){
    inst::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    PROCESSENTRY32W item{};item.dwSize=sizeof(item);DWORD pid{};
    if(Process32FirstW(snapshot.get(),&item))do{if(item.th32ParentProcessID==GetCurrentProcessId()&&(_wcsicmp(item.szExeFile,L"dblockswin32.exe")==0||_wcsicmp(item.szExeFile,L"usurper.exe")==0)){pid=item.th32ProcessID;break;}}while(Process32NextW(snapshot.get(),&item));
    std::ofstream file(std::string(prefix)+".console");file<<"child_pid="<<pid<<"\n";
    file<<"probe_console="<<GetConsoleWindow()<<"\n";
    FreeConsole(); // Detach only this diagnostic process before inspecting its child.
    if(!pid||!AttachConsole(pid)){file<<"attach_error="<<GetLastError()<<"\n";return;}
    {inst::Handle console(CreateFileW(L"CONOUT$",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if(GetConsoleScreenBufferInfo(console.get(),&info)){
        file<<"size="<<info.dwSize.X<<","<<info.dwSize.Y<<" visible="<<IsWindowVisible(GetConsoleWindow())<<"\n";
        std::string text(static_cast<std::size_t>(info.dwSize.X)*std::min<int>(info.dwSize.Y,100),' ');DWORD count{};
        if(ReadConsoleOutputCharacterA(console.get(),text.data(),static_cast<DWORD>(text.size()),{0,0},&count))file.write(text.data(),count);
    }else file<<"screen_error="<<GetLastError()<<"\n";}
    FreeConsole();
}
int main(int argc,char** argv){try{
    if(argc!=5)throw std::runtime_error("root, game directory, command and result prefix required");
    const auto root=std::filesystem::absolute(argv[1]);
    if(std::filesystem::exists(root))throw std::runtime_error("fresh isolated root required");
    std::filesystem::create_directories(root);
    const auto owner=inst::operator_sid();runtime::ensure_door_node_directories(root,owner,2);
    WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))throw std::runtime_error("Winsock");
    app::DoorDefinition door;door.id=1;door.name="Isolated native game";door.code="NATIVE_PROBE";door.enabled=true;
    door.kind=app::DoorExecutionKind::local;door.security_level=door.execution.level=10;
    door.program.type=app::DoorProgramType::native;door.program.io=app::DoorIo::socket;
    door.program.drop_file=app::DoorDropFile::door32_sys;door.program.startup_directory=argv[2];door.program.command_line=argv[3];
    runtime::LocalDoorCaller caller{42,1,10,"FixtureCaller","Fixture BBS",600,true};
    std::ofstream transcript(std::string(argv[4])+".terminal",std::ios::binary);
    std::ofstream events(std::string(argv[4])+".events");
    const auto began=GetTickCount64();unsigned reads{},writes{},keys{};bool space{},escape{},captured{};
    app::DoorChannel channel;
    channel.read=[&](std::size_t){++reads;const auto elapsed=GetTickCount64()-began;
        if(!space&&elapsed>=1500){space=true;events<<"input space at "<<elapsed<<std::endl;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{' '}};}
        if(!escape&&elapsed>=4500){escape=true;events<<"input escape at "<<elapsed<<std::endl;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{29,'D'}};}
        return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};
    channel.write=[&](std::string_view bytes){++writes;transcript.write(bytes.data(),bytes.size());transcript.flush();events<<"output "<<bytes.size()<<" at "<<(GetTickCount64()-began)<<std::endl;return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,bytes.size()};};
    channel.keyboard=[&]{++keys;};
    channel.stop=[&]()->std::optional<app::DoorEnd>{const auto elapsed=GetTickCount64()-began;if(!captured&&elapsed>=2500){captured=true;capture_child_console(argv[4]);}if(elapsed>=8000)return app::DoorEnd::operator_terminated;return {};};
    const auto result=runtime::run_alpha_local_door(root,owner,door,caller,channel);
    events<<"end="<<app::door_result_code(result.end)<<" diagnostic="<<result.diagnostic<<" exit="<<result.exit_code<<" cleanup="<<result.cleanup_complete<<" reads="<<reads<<" writes="<<writes<<" keys="<<keys<<std::endl;
    std::cout<<"end="<<app::door_result_code(result.end)<<" diagnostic="<<result.diagnostic<<" cleanup="<<result.cleanup_complete<<" writes="<<writes<<" keys="<<keys<<std::endl;
    WSACleanup();return result.cleanup_complete?0:2;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
