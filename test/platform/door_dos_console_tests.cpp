// Actual Win32 process/console/pipe tests. NTVDM and SF Guess speed are NOT
// simulated claims: those still require the owner's native x86 machine.
#include "../../src/apps/runtime/alpha_local_door.cpp"
#include "../../components/sfxexec/host/console_session.hpp"
#include <fstream>
#include <iostream>
#include <future>
#include <thread>
namespace rt=sfx::apps::runtime;
namespace ai=sfx::application;
namespace wi=sfx::platform::win32::installation;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
HANDLE number(const char* text){return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(text)));}
std::string numeric(HANDLE h){return std::to_string(reinterpret_cast<std::uintptr_t>(h));}
void send(HANDLE h,std::string_view text){DWORD count{};check(WriteFile(h,text.data(),static_cast<DWORD>(text.size()),&count,nullptr)&&count==text.size(),"explicit pipe write");}
std::string collect(rt::Pipe& pipe){std::string text;const auto until=GetTickCount64()+3000;while(GetTickCount64()<until){auto r=pipe.read();if(r.outcome==ai::InteractiveStreamOutcome::completed)text.append(r.bytes.begin(),r.bytes.end());else if(r.outcome==ai::InteractiveStreamOutcome::disconnected)break;else Sleep(2);}return text;}
int game(int argc,char** argv){
    check(argc==9,"game arguments");const auto window=GetConsoleWindow();
    check(window&&numeric(window)!=argv[3],"game owns a separate console");
    auto top=GetAncestor(window,GA_ROOTOWNER);if(!top)top=window;
    const auto until=GetTickCount64()+2000;while(!IsIconic(top)&&GetTickCount64()<until)Sleep(10);
    check(IsWindowVisible(top)&&IsIconic(top)&&!GetWindow(top,GW_OWNER)&&!(GetWindowLongPtrW(top,GWL_EXSTYLE)&WS_EX_TOOLWINDOW),"game console minimized and taskbar eligible");
    ShowWindow(top,SW_RESTORE);check(!IsIconic(top),"sysop can restore game console");ShowWindow(top,SW_SHOWMINNOACTIVE);check(IsIconic(top),"game console can minimize again");
    DWORD mode{};CONSOLE_SCREEN_BUFFER_INFO screen{};
    check(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),&mode),"game has real console input");
    check(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&screen),"game has real console output");
    check(GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE),&screen),"game has real console error");
    BOOL in_job{};check(IsProcessInJob(GetCurrentProcess(),nullptr,&in_job)&&in_job,"game remains in Runtime-owned job");
    for(int i=5;i<9;++i){ULONG server{};check(!(GetFileType(number(argv[i]))==FILE_TYPE_PIPE&&GetNamedPipeServerProcessId(number(argv[i]),&server)&&server==std::stoul(argv[4])),"no private Runtime pipe leaked to game");}
    // Isolated local-console keyboard roundtrip; caller keyboard stays on the
    // separate driver path. No owner console or desktop receives input.
    check(FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)),"clear own console fixture input");
    INPUT_RECORD input{};input.EventType=KEY_EVENT;input.Event.KeyEvent.bKeyDown=TRUE;input.Event.KeyEvent.wRepeatCount=1;input.Event.KeyEvent.uChar.AsciiChar='K';DWORD n{};
    check(WriteConsoleInputA(GetStdHandle(STD_INPUT_HANDLE),&input,1,&n)&&n==1,"own console input injection");
    INPUT_RECORD read{};check(ReadConsoleInputA(GetStdHandle(STD_INPUT_HANDLE),&read,1,&n)&&n==1&&read.Event.KeyEvent.uChar.AsciiChar=='K',"own local keyboard roundtrip");
    send(GetStdHandle(STD_OUTPUT_HANDLE),"LOCAL CONSOLE DIAGNOSTIC\r\n");
    std::ofstream report(argv[2]);report<<"PASS game checks="<<checks<<" pid="<<GetCurrentProcessId()<<"\n";
    return 0;
}
int host(int argc,char** argv){
    check(argc==9,"host arguments");auto input=number(argv[3]),output=number(argv[4]),error=number(argv[5]),status=number(argv[6]);const bool fixed=std::string(argv[2])=="fixed";
    if(!fixed){
        check(GetStdHandle(STD_INPUT_HANDLE)==input&&GetStdHandle(STD_OUTPUT_HANDLE)==output&&GetStdHandle(STD_ERROR_HANDLE)==error,"baseline exposes private pipes as standard devices");
        send(GetStdHandle(STD_OUTPUT_HANDLE),"LOCAL CONSOLE DIAGNOSTIC\r\n");
        bool rejected=false;try{sfx_dos_console_session({input,output,error,status});}catch(const std::exception&){rejected=true;}
        check(rejected,"corrected contract rejects r14 redirected host");send(status,"PASS baseline rejected; local diagnostic leaked to caller pipe\n");return 0;
    }
    sfx_dos_console_session({input,output,error,status});
    for(const auto h:{input,output,error,status}){DWORD flags{};check(GetHandleInformation(h,&flags)&&!(flags&HANDLE_FLAG_INHERIT),"private pipe inheritance cleared before child");}
    const auto window=GetConsoleWindow();check(!window,"background host has no taskbar window");
    std::string line="\""+std::string(argv[0])+"\" --game \""+std::string(argv[8])+"\" "+numeric(window)+" "+argv[7];for(const auto h:{input,output,error,status})line+=' '+numeric(h);
    auto startup=sfx_dos_child_startup();PROCESS_INFORMATION child{};
    check(CreateProcessA(nullptr,line.data(),nullptr,nullptr,FALSE,sfx_dos_child_flags,nullptr,nullptr,&startup,&child),"same production separate child launch contract");
    wi::Handle process(child.hProcess),thread(child.hThread);check(ResumeThread(thread.get())!=DWORD(-1),"resume owned child");
    check(WaitForSingleObject(process.get(),5000)==WAIT_OBJECT_0,"game child exits promptly");DWORD code{};check(GetExitCodeProcess(process.get(),&code)&&code==0,"game console contract passes");
    char text[6]{};DWORD got{};check(ReadFile(input,text,sizeof(text),&got,nullptr)&&got==sizeof(text)&&std::string(text,sizeof(text))=="CALLER","explicit caller input remains untouched by local console");
    send(output,"DRIVER:CALLER");send(status,"PASS fixed host; console separated; caller bytes exact\n");return 0;
}
void competing_reader(){
    // Reproduce the underlying synchronous-pipe hazard without pretending a
    // Win32 fixture is NTVDM. A second reader must never share caller stdin.
    HANDLE read{},write{};check(CreatePipe(&read,&write,nullptr,0),"blocking hazard pipe");wi::Handle r(read),w(write);
    std::promise<void> reading;auto ready=reading.get_future();
    std::thread reader([&]{reading.set_value();char c;DWORD n{};ReadFile(read,&c,1,&n,nullptr);});ready.wait();Sleep(50);
    auto peek=std::async(std::launch::async,[&]{DWORD n{};return PeekNamedPipe(read,nullptr,0,nullptr,&n,nullptr)!=FALSE;});
    const bool blocked=peek.wait_for(std::chrono::milliseconds(200))==std::future_status::timeout;
    DWORD n{};WriteFile(write,"x",1,&n,nullptr);reader.join();check(peek.get(),"peek resumes when competing read completes");
    check(blocked,"synchronous pipe read stalls host peek");
    std::cout<<"PASS negative control: competing read stalled PeekNamedPipe for at least 200ms\n";
}
}
int main(int argc,char** argv){try{
    if(argc>1&&std::string(argv[1])=="--game")return game(argc,argv);
    if(argc>1&&std::string(argv[1])=="--host")return host(argc,argv);
    check(argc==2,"fresh fixture directory required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"retain earlier fixtures");std::filesystem::create_directories(root);
    std::array<wchar_t,32768> self{};check(GetModuleFileNameW(nullptr,self.data(),static_cast<DWORD>(self.size())),"test image path");const auto owner=wi::operator_sid();
    {
        auto missing=rt::command("\""+std::filesystem::path(self.data()).u8string()+"\"",root,false);missing.executable=root/L"does-not-exist.exe";
        rt::Child refused;rt::DosHostDevices devices;DWORD captured{};bool failed=false;
        try{rt::start(refused,missing,root,rt::environment(root),devices.input.get(),devices.output.get(),devices.error.get(),{devices.input.get(),devices.output.get(),devices.error.get()},{},&captured);}catch(const std::exception&){failed=true;}
        SetLastError(ERROR_ACCESS_DENIED);check(failed&&captured==ERROR_FILE_NOT_FOUND,"exact CreateProcess error retained before cleanup");
    }
    competing_reader();
    SetLastError(ERROR_ACCESS_DENIED);
    try{sfx_dos_api(false,"create_process");check(false,"API exception required");}
    catch(const SfxDosLaunchFailure& e){SetLastError(ERROR_INVALID_HANDLE);check(e.win32==ERROR_ACCESS_DENIED,"Windows failure survives later API/cleanup");}
    const auto failure_code=sfx_dos_failure_code("create_process","NTVDM launch");
    check(failure_code=="door_dos_create_process_ntvdm_launch","stable bounded failure reason");
    check(sfx_dos_failure_code("request",std::string(300,'x')).size()==72,"failure reason bounded with error suffix space");
    rt::Pipe failure;failure.create(owner,true);
    const auto frame=sfx::core::dos_host::encode_frame({sfx::core::dos_host::Message::failed,ERROR_BAD_EXE_FORMAT,failure_code});
    send(failure.child.get(),{reinterpret_cast<const char*>(frame->data()),frame->size()});failure.child.reset();
    rt::HostStatus received;const auto deadline=GetTickCount64()+2000;while(!received.finished&&GetTickCount64()<deadline){received.poll(failure);Sleep(2);}
    check(received.finished&&!received.started&&received.error==failure_code+"_win32_193","early exact failure survives host exit through production status parser");
    for(const bool fixed:{false,true}){
        rt::Pipe input,output,error,status;input.create(owner,false);output.create(owner,true);error.create(owner,true);status.create(owner,true);
        const auto report=root/(fixed?"fixed-game.txt":"baseline-game.txt");
        std::string line="\""+std::filesystem::path(self.data()).u8string()+"\" --host "+(fixed?"fixed":"baseline");for(auto h:{input.child.get(),output.child.get(),error.child.get(),status.child.get()})line+=' '+numeric(h);line+=' '+std::to_string(GetCurrentProcessId())+" \""+report.u8string()+"\"";
        auto command=rt::command(line,root,false);rt::Child child;
        rt::DosHostDevices devices;
        rt::start(child,command,root,rt::environment(root),fixed?devices.input.get():input.child.get(),fixed?devices.output.get():output.child.get(),fixed?devices.error.get():error.child.get(),{input.child.get(),output.child.get(),error.child.get(),status.child.get(),devices.input.get(),devices.output.get(),devices.error.get()});
        input.child.reset();output.child.reset();error.child.reset();status.child.reset();check(input.write("CALLER").outcome==ai::InteractiveStreamOutcome::completed,"caller fixture input queued");
        check(WaitForSingleObject(child.process.get(),8000)==WAIT_OBJECT_0,"host exits promptly");DWORD code{};check(GetExitCodeProcess(child.process.get(),&code)&&code==0,"host contract passes");
        const auto bytes=collect(output),diagnostic=collect(error),result=collect(status);
        std::ofstream(root/(fixed?"fixed-result.txt":"baseline-result.txt"))<<result<<"caller_output="<<bytes<<"\nstderr="<<diagnostic;
        check(result.rfind("PASS",0)==0,"host reports PASS");check(diagnostic.empty(),"no host errors");
        check(bytes==(fixed?"DRIVER:CALLER":"LOCAL CONSOLE DIAGNOSTIC\r\n"),fixed?"only private driver bytes reach caller":"baseline console text contaminates caller");
        if(fixed){std::ifstream file(report);std::string contents((std::istreambuf_iterator<char>(file)),{});check(contents.rfind("PASS game",0)==0,"independent game result");}
        check(child.stop(),"entire owned process tree reaped");std::cout<<result;
    }
    std::cout<<"PASS DOS console tests: "<<checks<<" parent checks plus separate host/game checks; real NTVDM speed remains owner pending\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" error="<<GetLastError()<<'\n';return 1;}}
