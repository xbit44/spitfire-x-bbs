// SFX-owned normal Runtime adapter. Included only in SFXDOSHost's normal build;
// the former profile/RLogin main is not an entry point in that binary.
#include "sfx/core/dos_host_protocol.hpp"
#include <charconv>
#include <cstring>
#include "output_batch.hpp"
#include "console_session.hpp"
namespace protocol=sfx::core::dos_host;
static HANDLE inherited(std::uint64_t value){return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));}
static fs::path normal_path(const std::string& value){
    require(!value.empty(),"empty path");const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);require(size>0,"invalid path encoding");
    std::wstring out(size,L'\0');require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),size)==size,"invalid path encoding");return safe_path(fs::path(out));
}
static std::uint64_t birth(HANDLE process){FILETIME born{},exit{},kernel{},user{};require(GetProcessTimes(process,&born,&exit,&kernel,&user),"parent creation identity unavailable");return (std::uint64_t(born.dwHighDateTime)<<32)|born.dwLowDateTime;}
static void pipe_owner(HANDLE pipe,DWORD pid){ULONG owner{};require(GetFileType(pipe)==FILE_TYPE_PIPE&&GetNamedPipeServerProcessId(pipe,&owner)&&owner==pid,"inherited pipe parent mismatch");}
static void normal_status(HANDLE pipe,protocol::Message message,DWORD value,const std::string& diagnostic={}){
    const auto frame=protocol::encode_frame({message,value,diagnostic});require(frame.has_value(),"invalid host status");DWORD sent{};require(WriteFile(pipe,frame->data(),static_cast<DWORD>(frame->size()),&sent,nullptr)&&sent==frame->size(),"Runtime status pipe closed");
}
// Read one request and require EOF. No profile, listener, BBS log, account
// lookup or environment variable can supply this launch authority.
static protocol::Request normal_request(HANDLE pipe,HANDLE status,const char*& stage){
    require(GetFileType(pipe)==FILE_TYPE_PIPE,"request must be an inherited pipe");ULONG parent_pid{};require(GetNamedPipeServerProcessId(pipe,&parent_pid)&&parent_pid,"request parent unavailable");
    Handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,parent_pid));require(parent.h&&parent.h!=INVALID_HANDLE_VALUE,"request parent unavailable");
    std::vector<std::uint8_t> bytes;const auto until=GetTickCount64()+5000;bool eof=false;
    while(GetTickCount64()<until&&WaitForSingleObject(parent.h,0)==WAIT_TIMEOUT){DWORD count{};
        if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&count,nullptr)){require(GetLastError()==ERROR_BROKEN_PIPE,"request channel failed");eof=true;break;}
        require(count<=protocol::maximum_request&&bytes.size()+count<=protocol::maximum_request,"request bound exceeded");
        if(count){const auto old=bytes.size();bytes.resize(old+count);DWORD got{};require(ReadFile(pipe,bytes.data()+old,count,&got,nullptr)&&got==count,"request truncated");}else Sleep(2);
    }
    require(eof,"request EOF missing");const auto decoded=protocol::decode(bytes);require(decoded.has_value(),"invalid private launch request");const auto& r=*decoded;require(inherited(r.status)==status,"request status handle mismatch");stage="parent_identity";
    require(r.parent_pid==parent_pid&&GetProcessId(inherited(r.parent_process))==parent_pid&&birth(parent.h)==r.parent_birth&&birth(inherited(r.parent_process))==r.parent_birth,"parent identity mismatch");
    for(auto handle:{r.input,r.output,r.error,r.control,r.status})pipe_owner(inherited(handle),parent_pid);
    Acl acl;require(same_owner(parent.h,acl.sid),"parent owner mismatch");std::array<wchar_t,32768> image{};DWORD length=static_cast<DWORD>(image.size());require(QueryFullProcessImageNameW(parent.h,0,image.data(),&length),"parent image unavailable");
    const auto root=normal_path(r.root),expected=root/L"Bin"/L"SFXRuntime.exe";require(_wcsicmp(safe_path(fs::path(image.data())).c_str(),safe_path(expected).c_str())==0&&digest(expected)==SFX_RUNTIME_SHA256,"installed Runtime image mismatch");
    BOOL in_job{};require(IsProcessInJob(GetCurrentProcess(),nullptr,&in_job)&&in_job,"Runtime-owned job required");
    stage="local_devices";sfx_dos_console_session({pipe,inherited(r.parent_process),inherited(r.input),inherited(r.output),inherited(r.error),inherited(r.control),inherited(r.status)});
    return r;
}
static bool normal_control(const protocol::Request& request,std::vector<std::uint8_t>& pending,DWORD& reason){
    if(WaitForSingleObject(inherited(request.parent_process),0)!=WAIT_TIMEOUT){reason=3;return true;}
    DWORD count{};if(!PeekNamedPipe(inherited(request.control),nullptr,0,nullptr,&count,nullptr)){reason=3;return true;}
    require(count<=protocol::maximum_frame&&pending.size()+count<=protocol::maximum_frame,"control frame bound exceeded");
    if(count){const auto old=pending.size();pending.resize(old+count);DWORD got{};require(ReadFile(inherited(request.control),pending.data()+old,count,&got,nullptr)&&got==count,"control read failed");}
    if(pending.size()<4)return false;protocol::detail::Reader reader{pending};const auto size=reader.u32();require(size>=16&&size<=protocol::maximum_frame,"control frame invalid");if(pending.size()<size)return false;
    require(pending.size()==size,"duplicate control command refused");const auto frame=protocol::decode_frame(pending);require(frame&&frame->message==protocol::Message::stop&&frame->diagnostic.empty()&&frame->value<=3,"invalid stop command");reason=frame->value;pending.clear();return true;
}
static void normal_output(HANDLE pipe,const std::string& bytes){DWORD sent{};require(WriteFile(pipe,bytes.data(),static_cast<DWORD>(bytes.size()),&sent,nullptr)&&sent==bytes.size(),"caller output pipe closed");}
static DWORD normal_run(const protocol::Request& request,const char*& stage){
    stage="platform";
    BOOL wow{};require(IsWow64Process(GetCurrentProcess(),&wow),"host platform unavailable");
    require(!wow,request.allow_x64?"optional x64 DOS runtime is not qualified for this matched component pair":"DOS runtime unavailable on x64; optional runtime is disabled");
    stage="paths";const auto root=normal_path(request.root),work=normal_path(request.startup),node=normal_path(request.node_directory),private_dir=normal_path(request.private_directory);
    require(node==safe_path(root/L"Node"/std::to_wstring(request.node))&&private_dir.parent_path()==node&&private_dir.filename().wstring().size()==8,"private node directory mismatch");
    for(const auto& path:{root,work,node,private_dir,normal_path(request.drop_directory)}){const auto attributes=GetFileAttributesW(path.c_str());require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY),"captured launch directory unavailable");}
    std::array<wchar_t,32768> self{};require(GetModuleFileNameW(nullptr,self.data(),static_cast<DWORD>(self.size())),"host identity unavailable");require(safe_path(fs::path(self.data()))==safe_path(root/L"Bin"/L"SFXDOSHost.exe"),"host outside captured installation");
    stage="pair";PairPins pins(root,system_driver());
    require(GetFileAttributesW((work/L"SFXEXEC.DLL").c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"startup-directory driver conflicts with System32 identity");
    stage="files";const auto env=private_dir/L"SFXRUN.ENV",ini=private_dir/L"SFXRUN.INI";
    const auto environment=protocol::environment_file(request);
    require(read_file(env,8192)==environment&&read_file(ini,16384)==request.driver_ini,"captured launch files changed");
    require(read_file(private_dir/L"SFXRUN.RET",32).empty()&&read_file(private_dir/L"SFXRUN.ERR",512).empty(),"stale launcher results refused");
    require(request.command.size()<=120&&std::all_of(request.command.begin(),request.command.end(),[](unsigned char c){return c>=32&&c<127;}),"DOS command exceeds 120 ASCII bytes; use DOS short paths");
    stage="private_ipc";Acl acl;const auto& session=request.session;const auto actual_node=request.node;
    Handle output(CreateMailslotA(name(session,actual_node,"from_dos").c_str(),4000,0,&acl.attributes));require(output.h!=INVALID_HANDLE_VALUE,"private DOS output unavailable");
    auto hungup=named_event(acl,session,actual_node,"hungup",true),hangup=named_event(acl,session,actual_node,"hangup",true),carrier=named_event(acl,session,actual_node,"carrier",false);
    Handle from_credit(CreateSemaphoreA(&acl.attributes,64,64,name(session,actual_node,"from_credit").c_str()));require(from_credit.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"private output credits conflict");
    Handle to_credit(CreateSemaphoreA(&acl.attributes,64,64,name(session,actual_node,"to_credit").c_str()));require(to_credit.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"private input credits conflict");
    stage="command";const auto launcher=root/L"Bin"/L"SFXDOOR.EXE";const auto tail=dospath(env)+" NT "+std::to_string(actual_node)+" "+std::to_string(request.mode)+" "+dospath(ini)+" "+session;require(tail.size()<=125,"DOS launcher tail exceeds 125 bytes; shorten installation path");
    auto line="\""+dospath(launcher)+"\" "+tail;std::vector<char> command(line.begin(),line.end());command.push_back(0);Child child;FILETIME earliest{};GetSystemTimeAsFileTime(&earliest);auto startup=sfx_dos_child_startup();PROCESS_INFORMATION process{};
    const auto work_short=dospath(work);stage="create_process";
    // A 16-bit executable must be supplied as the quoted first command token;
    // lpApplicationName is NULL per the Windows CreateProcess contract.
    sfx_dos_api(CreateProcessA(nullptr,command.data(),nullptr,nullptr,FALSE,sfx_dos_child_flags,nullptr,work_short.c_str(),&startup,&process)!=FALSE,"ntvdm_launch");
    stage="process_identity";
    child.process.h=process.hProcess;child.thread.h=process.hThread;FILETIME created{},exit{},kernel{},user{};
    require(GetProcessTimes(child.process.h,&created,&exit,&kernel,&user)&&CompareFileTime(&created,&earliest)>=0,"pre-existing VDM refused");child.owned=true;
    BOOL in_job{};require(IsProcessInJob(child.process.h,nullptr,&in_job)&&in_job,"VDM must inherit Runtime job ownership");require(same_owner(child.process.h,acl.sid),"VDM owner mismatch");sfx_dos_api(ResumeThread(child.thread.h)!=DWORD(-1),"vdm_resume");
    stage="startup_hello";std::string hello;std::vector<std::uint8_t> control;DWORD stop_reason{};const auto until=GetTickCount64()+8000;
    while(GetTickCount64()<until&&WaitForSingleObject(child.process.h,0)==WAIT_TIMEOUT){if(normal_control(request,control,stop_reason))throw std::runtime_error("launch cancelled");DWORD next{},count{};require(GetMailslotInfo(output.h,nullptr,&next,&count,nullptr)&&count<=64&&(next==MAILSLOT_NO_MESSAGE||next<=4000),"startup output bound exceeded");if(count){hello=message(output.h);require(ReleaseSemaphore(from_credit.h,1,nullptr),"startup credit failed");break;}Sleep(2);}
    if(hello.empty()){
        if(WaitForSingleObject(child.process.h,0)==WAIT_OBJECT_0){DWORD code{};sfx_dos_api(GetExitCodeProcess(child.process.h,&code)!=FALSE,"launcher_exit_code");throw std::runtime_error("launcher_exit_"+std::to_string(code));}
        throw std::runtime_error("launcher_startup_timeout");
    }
    stage="startup_identity";const auto approval=startup_approval(hello,session,actual_node,process.dwProcessId,pins.driver.h);
    Handle input(CreateFileA(name(session,actual_node,"to_dos").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));require(input.h!=INVALID_HANDLE_VALUE,"private DOS input unavailable");DWORD sent{};require(WaitForSingleObject(to_credit.h,0)==WAIT_OBJECT_0&&WriteFile(input.h,approval.data(),static_cast<DWORD>(approval.size()),&sent,nullptr)&&sent==approval.size(),"private startup approval failed");
    stage="startup_ack";startup_acknowledged(output.h,from_credit.h,child.process.h,"SFXSTARTED2|"+session+"|"+std::to_string(actual_node));normal_status(inherited(request.status),protocol::Message::started,0);
    stage="running";bool cancelled=false,game_hangup=false;
    for(;;){
        if(normal_control(request,control,stop_reason)){cancelled=true;break;}
        const bool ended=WaitForSingleObject(child.process.h,0)==WAIT_OBJECT_0;
        // UART writes are one-byte messages. Drain a bounded burst before the
        // pipe write: one message per sleeping loop throttles the producer and
        // turns every character into a separate Runtime/SSH write. Keep the
        // former component bridge's 64-message/32KiB limit so input and stop
        // handling still run between bursts. The owned job bounds pipe stalls.
        const auto pending=sfx_dos_output_batch(output.h,from_credit.h);
        if(!pending.empty())normal_output(inherited(request.output),pending);
        else if(ended)break;
        if(!ended){DWORD available{};if(!PeekNamedPipe(inherited(request.input),nullptr,0,nullptr,&available,nullptr)){cancelled=true;stop_reason=1;break;}
            if(available&&WaitForSingleObject(to_credit.h,0)==WAIT_OBJECT_0){std::array<char,256> bytes{};DWORD got{};require(ReadFile(inherited(request.input),bytes.data(),std::min<DWORD>(available,static_cast<DWORD>(bytes.size())),&got,nullptr)&&got,"caller input failed");require(WriteFile(input.h,bytes.data(),got,&sent,nullptr)&&sent==got,"DOS input write failed");}
            if(WaitForSingleObject(hangup->h,0)==WAIT_OBJECT_0&&request.can_disconnect){game_hangup=true;normal_status(inherited(request.status),protocol::Message::hangup,0);break;}
        }Sleep(2);
    }
    stage="exit";SetEvent(hungup->h);SetEvent(carrier->h);
    const DWORD grace=cancelled&&stop_reason==1?5000:0;
    if(WaitForSingleObject(child.process.h,grace)!=WAIT_OBJECT_0)require(TerminateProcess(child.process.h,0x53465844),"owned VDM termination failed");
    require(WaitForSingleObject(child.process.h,3000)==WAIT_OBJECT_0,"owned VDM did not exit");child.owned=false;
    DWORD result{};if(!cancelled&&!game_hangup){auto returned=read_file(private_dir/L"SFXRUN.RET",32);int code{};const auto parsed=std::from_chars(returned.data(),returned.data()+returned.size(),code);require(parsed.ec==std::errc{}&&parsed.ptr==returned.data()+returned.size(),"launcher result unavailable");result=static_cast<DWORD>(code);}
    return result;
}
int main(int argc,char** argv){HANDLE status=INVALID_HANDLE_VALUE;const char* stage="bootstrap";try{
    require(argc==5&&std::string_view(argv[1])=="--runtime"&&std::string_view(argv[3])=="--status","private Runtime request required");
    const auto parse=[](const char* text){std::uint64_t value{};const auto length=std::strlen(text);const auto result=std::from_chars(text,text+length,value);require(result.ec==std::errc{}&&result.ptr==text+length&&value&&value<UINT32_MAX,"invalid bootstrap handle");return inherited(value);};
    Handle pipe(parse(argv[2]));const auto candidate=parse(argv[4]);require(candidate!=pipe.h,"bootstrap handles must differ");
    ULONG parent{};sfx_dos_api(GetNamedPipeServerProcessId(pipe.h,&parent)!=FALSE,"request_parent");require(parent!=0,"request parent missing");
    pipe_owner(candidate,parent);status=candidate;
    // Reporting is allowed only to another pipe owned by the request parent.
    // Launch still requires the full process, owner, image hash and job checks.
    stage="request";const auto request=normal_request(pipe.h,status,stage);CloseHandle(pipe.h);pipe.h=INVALID_HANDLE_VALUE;
    const auto result=normal_run(request,stage);normal_status(status,protocol::Message::finished,result);return 0;
}catch(const SfxDosLaunchFailure& error){
    if(status!=INVALID_HANDLE_VALUE)try{normal_status(status,protocol::Message::failed,error.win32,sfx_dos_failure_code(stage,error.what()));}catch(...){}return 1;
}catch(const fs::filesystem_error&){
    if(status!=INVALID_HANDLE_VALUE)try{normal_status(status,protocol::Message::failed,0,sfx_dos_failure_code(stage,"filesystem_failure"));}catch(...){}return 1;
}catch(const std::exception& error){
    if(status!=INVALID_HANDLE_VALUE)try{normal_status(status,protocol::Message::failed,0,sfx_dos_failure_code(stage,error.what()));}catch(...){}return 1;}}
