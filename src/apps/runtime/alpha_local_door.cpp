#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include "alpha_local_door.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/core/dos_host_protocol.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <memory>
#include <ctime>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace inst=platform::win32::installation;
using Path=std::filesystem::path;
using Handle=inst::Handle;
using Outcome=app::InteractiveStreamOutcome;
void require(bool value,const char* diagnostic){if(!value)throw std::runtime_error(diagnostic);}
std::wstring wide(std::string_view text){
    if(text.empty())return {};const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);require(n>0,"door_text_encoding");
    std::wstring out(n,L'\0');require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n)==n,"door_text_encoding");return out;
}
std::string utf8(const Path& path){return path.u8string();}
Path safe_directory(Path path){
    path=std::filesystem::absolute(path).lexically_normal();require(path.has_root_name()&&path.native().rfind(L"\\\\",0)!=0,"door_local_directory_required");
    Path at;for(const auto& part:path){at/=part;const auto flags=GetFileAttributesW(at.c_str());require(flags!=INVALID_FILE_ATTRIBUTES&&(flags&FILE_ATTRIBUTE_DIRECTORY)&&!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"door_directory_unavailable_or_redirected");}
    return path;
}
void ensure_directory(const Path& path,void* descriptor){
    const auto flags=GetFileAttributesW(path.c_str());
    if(flags==INVALID_FILE_ATTRIBUTES){require(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND,"door_directory_unavailable");inst::create_directory(path,descriptor);}
    (void)safe_directory(path);
}
struct Attributes {
    std::vector<unsigned char> bytes;LPPROC_THREAD_ATTRIBUTE_LIST list{};
    ~Attributes(){if(list)DeleteProcThreadAttributeList(list);}
    void initialize(const std::vector<HANDLE>& handles){SIZE_T n{};InitializeProcThreadAttributeList(nullptr,1,0,&n);require(n&&n<65536,"door_handle_list_size");bytes.resize(n);auto* value=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());require(InitializeProcThreadAttributeList(value,1,0,&n)!=FALSE,"door_handle_list");list=value;require(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE*>(handles.data()),handles.size()*sizeof(HANDLE),nullptr,nullptr)!=FALSE,"door_handle_allowlist");}
};
struct Child {
    Handle job,process,thread;bool assigned{},reaped{};
    bool stop(){
        if(!process){reaped=true;return true;}
        if(assigned){if(!TerminateJobObject(job.get(),0x53465844)&&GetLastError()!=ERROR_ACCESS_DENIED)return false;}
        else if(WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT&&!TerminateProcess(process.get(),0x53465844))return false;
        reaped=WaitForSingleObject(process.get(),5000)==WAIT_OBJECT_0;
        if(reaped&&assigned){const auto until=GetTickCount64()+5000;for(;;){JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            if(!QueryInformationJobObject(job.get(),JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr)){reaped=false;break;}
            if(!info.ActiveProcesses)break;if(GetTickCount64()>=until){reaped=false;break;}Sleep(10);
        }}return reaped;
    }
    ~Child(){if(!reaped)(void)stop();}
};
// Parent ends are overlapped and bounded; the game gets ordinary synchronous
// stdin/stdout handles. No worker can remain blocked in a game pipe write.
struct Pipe {
    Handle parent,child,event;OVERLAPPED operation{};std::array<char,4096> data{};
    bool pending{},reading{},closed{};DWORD transferred{};ULONGLONG pending_since{};
    ~Pipe(){if(pending){CancelIoEx(parent.get(),&operation);DWORD done{};GetOverlappedResult(parent.get(),&operation,&done,TRUE);}}
    void close(){if(pending){CancelIoEx(parent.get(),&operation);DWORD done{};GetOverlappedResult(parent.get(),&operation,&done,TRUE);pending=false;}parent.reset();closed=true;}
    void create(const std::wstring& owner,bool read){
        reading=read;auto sd=inst::mutable_descriptor(owner);SECURITY_ATTRIBUTES sa{sizeof(sa),sd.get(),FALSE};
        const auto name=L"\\\\.\\pipe\\SFX.Door."+inst::unique_id();
        {sfx::platform::win32::ServiceFileOwnerScope scope(owner);require(scope.valid(),"door_pipe_owner_scope_failed");
            parent.reset(CreateNamedPipeW(name.c_str(),(read?PIPE_ACCESS_INBOUND:PIPE_ACCESS_OUTBOUND)|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&sa));}
        require(parent&&parent.get()!=INVALID_HANDLE_VALUE,"door_private_pipe");
        sa.bInheritHandle=TRUE;child.reset(CreateFileW(name.c_str(),read?GENERIC_WRITE:GENERIC_READ,0,&sa,OPEN_EXISTING,0,nullptr));
        if(!child||child.get()==INVALID_HANDLE_VALUE)throw std::runtime_error("door_private_pipe_child_"+std::to_string(GetLastError()));
        event.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr));require(event!=nullptr,"door_pipe_event");operation.hEvent=event.get();
    }
    bool ready(){
        if(!pending)return true;DWORD n{};
        if(!GetOverlappedResult(parent.get(),&operation,&n,FALSE)){const auto error=GetLastError();if(error==ERROR_IO_INCOMPLETE)return false;pending=false;closed=true;return true;}
        pending=false;transferred=n;return true;
    }
    app::InteractiveStreamReadResult read(){
        if(!ready())return {Outcome::timed_out,{}};
        if(transferred){std::vector<std::uint8_t> bytes(data.begin(),data.begin()+transferred);transferred=0;return {Outcome::completed,std::move(bytes)};}
        if(closed)return {Outcome::disconnected,{}};
        ResetEvent(event.get());DWORD n{};if(ReadFile(parent.get(),data.data(),static_cast<DWORD>(data.size()),&n,&operation)){transferred=n;if(!n){closed=true;return {Outcome::disconnected,{}};}return read();}
        if(GetLastError()==ERROR_IO_PENDING){pending=true;return {Outcome::timed_out,{}};}closed=true;return {Outcome::disconnected,{}};
    }
    app::InteractiveStreamWriteResult write(std::string_view bytes){
        if(!ready())return {Outcome::timed_out,0};transferred=0;if(closed)return {Outcome::disconnected,0};
        const auto n=std::min(bytes.size(),data.size());std::copy_n(bytes.begin(),n,data.begin());ResetEvent(event.get());DWORD sent{};
        if(WriteFile(parent.get(),data.data(),static_cast<DWORD>(n),&sent,&operation))return {Outcome::completed,sent};
        if(GetLastError()==ERROR_IO_PENDING){pending=true;pending_since=GetTickCount64();return {Outcome::completed,n};}closed=true;return {Outcome::disconnected,0};
    }
};
struct Socket {
    SOCKET value{INVALID_SOCKET};~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
    void close(){if(value!=INVALID_SOCKET)closesocket(value);value=INVALID_SOCKET;}
};
void sockets(Socket& parent,Socket& child){
    Socket listener;listener.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(listener.value!=INVALID_SOCKET,"door_socket_listener");
    SetHandleInformation(reinterpret_cast<HANDLE>(listener.value),HANDLE_FLAG_INHERIT,0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener.value,1)==0,"door_socket_bind");int size=sizeof(address);require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&size)==0,"door_socket_address");
    child.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(child.value!=INVALID_SOCKET&&connect(child.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"door_socket_child");parent.value=accept(listener.value,nullptr,nullptr);require(parent.value!=INVALID_SOCKET,"door_socket_parent");
    sockaddr_in child_address{},parent_peer{};int child_size=sizeof(child_address),peer_size=sizeof(parent_peer);
    require(getsockname(child.value,reinterpret_cast<sockaddr*>(&child_address),&child_size)==0&&getpeername(parent.value,reinterpret_cast<sockaddr*>(&parent_peer),&peer_size)==0&&child_address.sin_family==parent_peer.sin_family&&child_address.sin_addr.s_addr==parent_peer.sin_addr.s_addr&&child_address.sin_port==parent_peer.sin_port,"door_socket_owned_peer");
    require(SetHandleInformation(reinterpret_cast<HANDLE>(parent.value),HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(reinterpret_cast<HANDLE>(child.value),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"door_socket_allowlist");
    u_long nonblocking=1;require(ioctlsocket(parent.value,FIONBIO,&nonblocking)==0,"door_socket_nonblocking");const int enabled=1;
    require(setsockopt(parent.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&enabled),sizeof(enabled))==0&&setsockopt(child.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&enabled),sizeof(enabled))==0,"door_socket_latency");
}
struct DropFile {
    Handle file;Path path;bool owned{};
    bool finish(){
        if(!owned)return true;if(!file||file.get()==INVALID_HANDLE_VALUE)return false;
        Handle removal(CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!removal||removal.get()==INVALID_HANDLE_VALUE){
            if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_ACCESS_DENIED)return false;
            FILE_STANDARD_INFO info{};if(!GetFileInformationByHandleEx(file.get(),FileStandardInfo,&info,sizeof(info))||!info.DeletePending)return false;
            owned=false;file.reset();return true;
        }
        if(inst::file_id(removal.get())!=inst::file_id(file.get())||inst::volume_serial(removal.get())!=inst::volume_serial(file.get()))return false;
        FILE_DISPOSITION_INFO remove{TRUE};if(!SetFileInformationByHandle(removal.get(),FileDispositionInfo,&remove,sizeof(remove)))return false;
        owned=false;file.reset();return true;
    }
    ~DropFile()=default; // uncertain outcomes retain the original owned object
    bool preserve_as(const Path& destination){
        if(!owned)return true;if(!file||file.get()==INVALID_HANDLE_VALUE)return false;
        auto moving=inst::open_object(path,false,DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(moving.get(),&info)||info.nNumberOfLinks!=1||
           inst::file_id(moving.get())!=inst::file_id(file.get())||inst::volume_serial(moving.get())!=inst::volume_serial(file.get()))return false;
        const auto absolute=L"\\\\?\\"+destination.native();
        std::vector<unsigned char> bytes(sizeof(FILE_RENAME_INFO)+(absolute.size()+1)*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(bytes.data());rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;
        rename->FileNameLength=static_cast<DWORD>(absolute.size()*sizeof(wchar_t));std::copy(absolute.begin(),absolute.end(),rename->FileName);
        if(!SetFileInformationByHandle(moving.get(),FileRenameInfo,rename,static_cast<DWORD>(bytes.size())))return false;
        auto check=inst::open_object(destination,false,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
        if(inst::file_id(check.get())!=inst::file_id(file.get())||inst::volume_serial(check.get())!=inst::volume_serial(file.get()))return false;
        path=destination;owned=false;return true;
    }
    void create(const Path& target,const std::string& bytes,const std::wstring& owner){
        auto sd=inst::mutable_descriptor(owner);SECURITY_ATTRIBUTES sa{sizeof(sa),sd.get(),FALSE};path=target;
        {sfx::platform::win32::ServiceFileOwnerScope scope(owner);require(scope.valid(),"door_drop_owner_scope_failed");
            file.reset(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));}
        require(file&&file.get()!=INVALID_HANDLE_VALUE,"door_drop_file_in_use_or_existing");owned=true;DWORD written{};require(WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file.get()),"door_drop_file_write");
        // Legacy readers may deny write-sharing while reading the drop file.
        // Retain identity without data access. A duplicated handle shares the
        // original file object's write access, so open an independent metadata
        // handle and verify it before releasing the writer.
        Handle identity(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        require(identity&&identity.get()!=INVALID_HANDLE_VALUE&&inst::file_id(identity.get())==inst::file_id(file.get())&&inst::volume_serial(identity.get())==inst::volume_serial(file.get()),"door_drop_file_identity");
        file=std::move(identity);
    }
};
// Keep a handle to every owned file through the child's lifetime. Deletion
// verifies file identity; an intervening replacement is never removed.
struct DropFiles {
    std::vector<std::unique_ptr<DropFile>> owned;
    Handle directory_pin;Path directory;bool temporary{};
    std::string retained_location;
    void capture_directory(const Path& value,bool transient){directory=value;temporary=transient;directory_pin=inst::open_object(value,true,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);}
    void create(const Path& path,std::string_view bytes,const std::wstring& owner){auto file=std::make_unique<DropFile>();owned.push_back(std::move(file));owned.back()->create(path,std::string(bytes),owner);}
    bool finish(){
        if(!directory_pin)return true;
        auto guard=inst::open_object(directory,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
        if(inst::file_id(guard.get())!=inst::file_id(directory_pin.get())||inst::volume_serial(guard.get())!=inst::volume_serial(directory_pin.get()))return false;
        bool done=true;for(auto i=owned.rbegin();i!=owned.rend();++i)try{done=(*i)->finish()&&done;}catch(...){done=false;}
        guard.reset();
        if(temporary&&directory_pin){
            Handle removal(CreateFileW(directory.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(!removal||removal.get()==INVALID_HANDLE_VALUE)return false;
            if(inst::file_id(removal.get())!=inst::file_id(directory_pin.get())||inst::volume_serial(removal.get())!=inst::volume_serial(directory_pin.get()))return false;
            FILE_DISPOSITION_INFO dispose{TRUE};
            // A game may create persistent data here. Only an empty directory
            // is removable; no recursive cleanup or wildcard deletion.
            if(!SetFileInformationByHandle(removal.get(),FileDispositionInfo,&dispose,sizeof(dispose))&&GetLastError()!=ERROR_DIR_NOT_EMPTY)done=false;
            directory_pin.reset();temporary=false;
        }return done;
    }
    bool has_owned()const{return std::any_of(owned.begin(),owned.end(),[](const auto& file){return file->owned;});}
    bool preserve(const Path& root,const std::wstring& owner){
        (void)root;
        if(!has_owned())return true;if(!directory_pin)return false;
        auto guard=inst::open_object(directory,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
        if(inst::file_id(guard.get())!=inst::file_id(directory_pin.get())||inst::volume_serial(guard.get())!=inst::volume_serial(directory_pin.get()))return false;
        // A unique temporary drop directory already avoids the next caller's
        // fixed names. Preserve it intact, including unknown game-created data.
        if(temporary){retained_location="temporary drop folder/"+directory.filename().u8string();return true;}
        // Keep the private review folder on the same volume, including Doors
        // whose Sysop-selected startup/drop directory is outside SFX.
        const auto review=directory/(L"SFXReview-"+inst::unique_id());const auto descriptor=inst::mutable_descriptor(owner);inst::create_directory(review,descriptor.get());
        retained_location="drop folder/"+review.filename().u8string();
        auto review_pin=inst::open_object(review,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);
        bool done=true;for(auto& file:owned)if(file->owned)try{done=file->preserve_as(review/file->path.filename())&&done;}catch(...){done=false;}
        return done;
    }
    ~DropFiles()=default;
};
bool disposable_door_result(const app::DoorProgramResult& result){
    return result.cleanup_complete&&!result.cleanup_command_failed&&
        ((result.end==app::DoorEnd::returned&&result.exit_code==0)||result.end==app::DoorEnd::escaped);
}
std::string optional_ini(const Path& path){
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES){require(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND,"door_ini_unavailable");return {};}
    auto file=inst::open_object(path,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);LARGE_INTEGER size{};require(GetFileSizeEx(file.get(),&size)&&size.QuadPart>=0&&size.QuadPart<=16384,"door_ini_too_large");
    std::string data(static_cast<std::size_t>(size.QuadPart),'\0');DWORD n{};require(ReadFile(file.get(),data.data(),static_cast<DWORD>(data.size()),&n,nullptr)&&n==data.size(),"door_ini_read");return data;
}
app::DoorDropContext drop_context(const LocalDoorCaller& caller,const app::DoorDefinition& door,const Path& node,const Path& drop,const Path& startup,std::uint64_t socket){
    auto context=caller.drop_context;context.account=caller.account;context.node=caller.node;context.nodes=std::max(context.nodes,caller.node);context.security=caller.security_level;context.alias=caller.alias;context.board=caller.board_name;context.ansi=caller.ansi;context.remaining_seconds=caller.remaining_seconds;context.dos=door.program.type==app::DoorProgramType::dos;context.io=door.program.io;context.socket=socket;
    context.node_path=utf8(node);context.drop_path=utf8(drop);context.startup_path=utf8(startup);
    if (door.program.drop_file==app::DoorDropFile::doordrop_ini) {
        context.doordrop_utf8=door.program.doordrop_utf8;
        context.door_code=app::effective_door_code(door);
        context.door_name=door.name;
        return context;
    }
    if(context.user_key.empty())context.user_key="sfx-account-"+std::to_string(caller.account);
    if(caller.remaining_seconds&&context.deadline_utc.empty()){
        const auto now=caller.captured_utc_seconds>0?caller.captured_utc_seconds:static_cast<std::int64_t>(std::time(nullptr));
        require(*caller.remaining_seconds<=3155760000ull,"door_deadline_range");const __time64_t deadline=now+static_cast<__time64_t>(*caller.remaining_seconds);std::tm utc{};require(_gmtime64_s(&utc,&deadline)==0,"door_deadline_invalid");char text[32]{};require(std::strftime(text,sizeof(text),"%Y-%m-%dT%H:%M:%SZ",&utc)==20,"door_deadline_format");context.deadline_utc=text;
    }
    // Standalone harness callers may omit calendar context. Real Runtime
    // callers supply captured BBS-local dates and a UTC deadline below.
    if(context.date.empty()){SYSTEMTIME now{};GetLocalTime(&now);char date[16],clock[16];sprintf_s(date,"%02u/%02u/%02u",now.wMonth,now.wDay,now.wYear%100);sprintf_s(clock,"%02u:%02u",now.wHour,now.wMinute);context.date=context.logon_date=date;context.clock=context.logon_clock=clock;context.seconds_today=context.logon_seconds_today=now.wHour*3600+now.wMinute*60+now.wSecond;}
    return context;
}
struct LaunchCommand {Path executable;std::wstring line;Handle pin;};
LaunchCommand command(const std::string& input,const Path& startup,bool shell){
    LaunchCommand result;auto value=wide(input);require(!value.empty(),"door_command_empty");
    if(shell){std::array<wchar_t,32768> system{};const auto n=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));require(n&&n<system.size(),"door_system_directory");result.executable=Path(system.data())/L"cmd.exe";result.line=L"\""+result.executable.native()+L"\" /D /S /C \""+value+L"\"";}
    else{int count{};auto argv=CommandLineToArgvW(value.c_str(),&count);require(argv&&count>0,"door_command_quotes");const Path program(argv[0]);LocalFree(argv);result.executable=program.is_absolute()?program:startup/program;if(result.executable.extension().empty())result.executable+=L".exe";result.line=std::move(value);}
    result.executable=std::filesystem::absolute(result.executable).lexically_normal();(void)safe_directory(result.executable.parent_path());
    result.pin=inst::open_object(result.executable,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);DWORD type{};require(GetBinaryTypeW(result.executable.c_str(),&type)&&(type==SCS_32BIT_BINARY||type==SCS_64BIT_BINARY),"door_native_program_required");return result;
}
std::wstring environment(const Path& node,const std::map<std::string,std::string>& extra={}){
    std::array<wchar_t,32768> windows{},system{};auto n=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));require(n&&n<windows.size(),"door_windows_directory");n=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));require(n&&n<system.size(),"door_system_directory");
    std::map<std::wstring,std::wstring> values{{L"PATH",system.data()},{L"SystemRoot",windows.data()},{L"TEMP",node.native()},{L"TMP",node.native()},{L"WINDIR",windows.data()}};
    for(const auto& item:extra){require((item.first=="BBSDEV_DRP"||item.first=="DOORDROP_INI")&&item.second.find('\0')==item.second.npos,"door_environment_invalid");values.emplace(wide(item.first),wide(item.second));}
    std::wstring result;for(const auto& item:values)result+=item.first+L'='+item.second+L'\0';result+=L'\0';return result;
}
void start(Child& child,const LaunchCommand& command,const Path& startup,std::wstring env,HANDLE input,HANDLE output,HANDLE error,const std::vector<HANDLE>& inherited,std::wstring console_title={},DWORD* launch_error=nullptr){
    Attributes attributes;attributes.initialize(inherited);child.job.reset(CreateJobObjectW(nullptr,nullptr));require(child.job!=nullptr,"door_job_create");JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;require(SetInformationJobObject(child.job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE,"door_job_limits");
    STARTUPINFOEXW info{};info.StartupInfo.cb=sizeof(info);info.lpAttributeList=attributes.list;
    const bool console=!console_title.empty();
    if(console){
        // Socket Doors still use a local console for CRT initialization, sysop
        // keys and diagnostics. Pipes are not console handles. Give the child
        // its own taskbar console while inheriting only its private socket.
        // Start minimized without stealing focus; the sysop can restore it.
        info.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;info.StartupInfo.wShowWindow=SW_SHOWMINNOACTIVE;
        info.StartupInfo.lpTitle=console_title.data();
    }else{
        info.StartupInfo.dwFlags=STARTF_USESTDHANDLES;info.StartupInfo.hStdInput=input;info.StartupInfo.hStdOutput=output;info.StartupInfo.hStdError=error;
    }
    PROCESS_INFORMATION process{};auto line=command.line;
    const auto launched=CreateProcessW(command.executable.c_str(),line.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|(console?CREATE_NEW_CONSOLE:CREATE_NO_WINDOW)|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,env.data(),startup.c_str(),&info.StartupInfo,&process);if(!launched&&launch_error)*launch_error=GetLastError();require(launched!=FALSE,"door_native_launch_failed");
    child.process.reset(process.hProcess);child.thread.reset(process.hThread);require(AssignProcessToJobObject(child.job.get(),child.process.get())!=FALSE,"door_job_assignment");child.assigned=true;require(ResumeThread(child.thread.get())!=DWORD(-1),"door_resume_failed");
}
#include "alpha_local_dos.inc"
} // namespace
void ensure_door_node_directories(const Path& root,const std::wstring& owner,unsigned count){
    require(count>=1&&count<=128,"door_node_count");(void)safe_directory(root);auto sd=inst::mutable_descriptor(owner);ensure_directory(root/L"Node",sd.get());for(unsigned n=1;n<=count;++n)ensure_directory(root/L"Node"/std::to_wstring(n),sd.get());
}
app::DoorProgramResult run_alpha_local_door(const Path& root,const std::wstring& owner,const app::DoorDefinition& door,const LocalDoorCaller& caller,const app::DoorChannel& channel,std::vector<std::uint8_t> prefix,bool discard_lf){
    app::DoorProgramResult result;Child child;DropFiles files;Path node,startup,drop;bool launched{};app::DoorDriverSettings settings;
    try{
        require(app::valid_door_definition(door)&&door.kind==app::DoorExecutionKind::local&&app::door_has_command(door)&&caller.node>=1&&caller.node<=128&&prefix.size()<=4096&&channel.read&&channel.write&&channel.stop,"door_launch_context");
        if(auto stop=channel.stop()){result.end=*stop;return result;}
        // Observe a closed decoded transport before creating any game process.
        // Preserve queued keystrokes for the child instead of consuming them.
        if(prefix.size()<4096){const auto ready=channel.read(4096-prefix.size());
            require(app::valid_interactive_stream_read_result(4096-prefix.size(),ready),"door_caller_read_invalid");
            if(ready.outcome!=app::InteractiveStreamOutcome::completed&&ready.outcome!=app::InteractiveStreamOutcome::timed_out){result.end=app::DoorEnd::caller_disconnected;return result;}
            prefix.insert(prefix.end(),ready.bytes.begin(),ready.bytes.end());
        }
        node=safe_directory(root/L"Node"/std::to_wstring(caller.node));
        startup=door.program.startup_directory.empty()?root:Path(wide(door.program.startup_directory));if(startup.is_relative())startup=root/startup;startup=safe_directory(startup);
        std::string ini_error;const auto parsed=app::parse_door_driver_settings(optional_ini(root/L"Bin"/L"sfxexec.ini"),optional_ini(startup/L"sfxexec.ini"),&ini_error);require(parsed.has_value(),ini_error.c_str());settings=*parsed;
        drop=node;if(door.program.drop_location==app::DoorDropLocation::startup)drop=startup;
        if(door.program.drop_location==app::DoorDropLocation::temporary){
            auto sd=inst::mutable_descriptor(owner);
            const auto identity=inst::unique_id();
            const bool dos_ini=door.program.type==app::DoorProgramType::dos && door.program.drop_file==app::DoorDropFile::doordrop_ini;
            drop=node/(dos_ini?L"I"+identity.substr(0,7):L"Door-"+identity);
            inst::create_directory(drop,sd.get());drop=safe_directory(drop);
        }
        files.capture_directory(drop,door.program.drop_location==app::DoorDropLocation::temporary);
        if(door.program.type==app::DoorProgramType::dos){result=run_dos_sequence(root,owner,door,caller,startup,node,drop,files,settings,channel,std::move(prefix),discard_lf);}
        else {
        const auto expanded=app::expand_door_command(door.program.command_line,{utf8(node),utf8(drop),utf8(startup),app::effective_door_code(door),caller.node});require(expanded.has_value(),"door_command_substitution");
        auto executable=command(*expanded,startup,door.program.command_shell);Pipe input,output,error;Socket parent,socket_child;const bool socket_io=door.program.io==app::DoorIo::socket;
        if(socket_io)sockets(parent,socket_child);
        else{input.create(owner,false);output.create(owner,true);error.create(owner,true);}
        const auto drops=app::make_door_drop_files(door.program.drop_file,drop_context(caller,door,node,drop,startup,socket_io?socket_child.value:0));require(drops.valid(),drops.diagnostic.c_str());
        if(!drops.files.empty())files.create(drop/L"SFXDOOR.LCK","SFX owned drop files\r\n",owner);
        for(const auto& image:drops.files)files.create(drop/wide(image.name),std::string_view(reinterpret_cast<const char*>(image.bytes.data()),image.bytes.size()),owner);
        std::vector<HANDLE> handles;
        if(socket_io)handles.push_back(reinterpret_cast<HANDLE>(socket_child.value));
        else handles={input.child.get(),output.child.get(),error.child.get()};
        const auto title=socket_io?wide(door.name)+L" - SFX Node "+std::to_wstring(caller.node):std::wstring{};
        start(child,executable,startup,environment(node,drops.environment),input.child.get(),output.child.get(),error.child.get(),handles,title);launched=true;input.child.reset();output.child.reset();error.child.reset();socket_child.close();
        std::string to_game,to_caller;DoorKeys keys{discard_lf};keys.append(prefix,to_game);bool output_closed=false;std::uint64_t drain{},blocked_input{},blocked_output{},socket_closed_at{};
        result.end=app::DoorEnd::returned;
        for(;;){
            const auto now=GetTickCount64();if(auto stop=channel.stop()){result.end=*stop;break;}
            keys.expire(to_game);if(keys.escaped){result.end=app::DoorEnd::escaped;break;}
            if(!socket_io)(void)error.read(); // Socket diagnostics stay in the local console.
            if(!socket_io&&input.pending&&now-input.pending_since>=5000&&!input.ready()){result.end=app::DoorEnd::backpressure_timeout;break;}
            const bool exited=WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0;if(exited&&!drain)drain=now+1000;
            if(!exited&&to_game.size()<4095){auto read=channel.read(4095-to_game.size());require(app::valid_interactive_stream_read_result(4095-to_game.size(),read),"door_caller_read");if(read.outcome==Outcome::completed){if(channel.keyboard)channel.keyboard();keys.append(read.bytes,to_game);if(keys.escaped){result.end=app::DoorEnd::escaped;break;}}else if(read.outcome!=Outcome::timed_out){result.end=app::DoorEnd::caller_disconnected;break;}}
            if(!to_game.empty()&&!exited){app::InteractiveStreamWriteResult written;
                if(socket_io){const auto n=send(parent.value,to_game.data(),static_cast<int>(to_game.size()),0);written=n>0?app::InteractiveStreamWriteResult{Outcome::completed,static_cast<std::size_t>(n)}:app::InteractiveStreamWriteResult{WSAGetLastError()==WSAEWOULDBLOCK?Outcome::timed_out:Outcome::disconnected,0};}
                else written=input.write(to_game);
                if(written.outcome==Outcome::completed){to_game.erase(0,written.bytes_written);blocked_input=0;}else if(written.outcome==Outcome::timed_out){if(!blocked_input)blocked_input=now;}else{result.end=app::DoorEnd::transport_failed;break;}
            }
            if(!output_closed&&to_caller.empty()){
                app::InteractiveStreamReadResult read;
                if(socket_io){std::array<char,4096> bytes{};const auto n=recv(parent.value,bytes.data(),static_cast<int>(bytes.size()),0);if(n>0)read={Outcome::completed,{bytes.begin(),bytes.begin()+n}};else read={n==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK?Outcome::timed_out:Outcome::disconnected,{}};}
                else read=output.read();
                if(read.outcome==Outcome::completed)to_caller.assign(read.bytes.begin(),read.bytes.end());else if(read.outcome!=Outcome::timed_out)output_closed=true;
            }
            if(!to_caller.empty()){const auto written=channel.write(to_caller);require(app::valid_interactive_stream_write_result(to_caller.size(),written),"door_caller_write");if(written.outcome==Outcome::completed){to_caller.erase(0,written.bytes_written);blocked_output=0;}else if(written.outcome==Outcome::timed_out){if(!blocked_output)blocked_output=now;}else{result.end=app::DoorEnd::caller_disconnected;break;}}
            if((blocked_input&&now-blocked_input>=5000)||(blocked_output&&now-blocked_output>=5000)){result.end=app::DoorEnd::backpressure_timeout;break;}
            if(exited&&to_caller.empty()&&(output_closed||now>=drain))break;
            if(socket_io&&output_closed&&!exited){if(!socket_closed_at)socket_closed_at=now;if(now-socket_closed_at>=100){result.end=settings.can_disconnect?app::DoorEnd::game_disconnect:app::DoorEnd::returned;break;}}
            Sleep(2);
        }
        DWORD exit_code{};if(GetExitCodeProcess(child.process.get(),&exit_code))result.exit_code=exit_code==STILL_ACTIVE&&socket_closed_at&&!settings.can_disconnect?0:exit_code;
        if(result.end==app::DoorEnd::caller_disconnected){
            if(socket_io)shutdown(parent.value,SD_SEND);else input.close();
            const auto until=GetTickCount64()+5000;while(WaitForSingleObject(child.process.get(),20)==WAIT_TIMEOUT&&GetTickCount64()<until){
                if(const auto stop=channel.stop())if(*stop==app::DoorEnd::shutdown||*stop==app::DoorEnd::operator_terminated)break;
            }
        }
        result.cleanup_complete=child.stop();
        if(result.end==app::DoorEnd::returned&&result.exit_code!=0)result.end=app::DoorEnd::launch_failed;
        result.diagnostic=app::door_result_code(result.end);
        }
    }catch(const std::exception& e){result.diagnostic=e.what();result.end=channel.stop?channel.stop().value_or(app::DoorEnd::launch_failed):app::DoorEnd::launch_failed;if(result.diagnostic=="door_drop_file_in_use_or_existing")result.end=app::DoorEnd::busy;result.cleanup_complete=child.stop();}
    if(launched&&disposable_door_result(result)&&!door.program.cleanup_command.empty()){
        Child cleanup;
        try{
            const auto expanded=app::expand_door_command(door.program.cleanup_command,{utf8(node),utf8(drop),utf8(startup),app::effective_door_code(door),caller.node});require(expanded.has_value(),"door_cleanup_substitution");
            const auto executable=command(*expanded,startup,door.program.command_shell);Pipe input,output;input.create(owner,false);output.create(owner,true);
            start(cleanup,executable,startup,environment(node),input.child.get(),output.child.get(),output.child.get(),{input.child.get(),output.child.get()});input.child.reset();output.child.reset();input.close();
            const auto until=GetTickCount64()+30000;
            while(WaitForSingleObject(cleanup.process.get(),0)==WAIT_TIMEOUT&&GetTickCount64()<until){(void)output.read();if(const auto stop=channel.stop())if(*stop==app::DoorEnd::shutdown||*stop==app::DoorEnd::operator_terminated||*stop==app::DoorEnd::calling_time||*stop==app::DoorEnd::inactivity)break;Sleep(2);}
            DWORD exit_code{};result.cleanup_command_failed=!GetExitCodeProcess(cleanup.process.get(),&exit_code)||exit_code!=0;
            result.cleanup_complete=cleanup.stop();
        }catch(...){result.cleanup_command_failed=true;result.cleanup_complete=cleanup.stop();}
    }
    try{
        if(disposable_door_result(result)){if(!files.finish()){result.cleanup_complete=false;result.work_preserved=true;}}
        else if(files.has_owned()||launched||result.end==app::DoorEnd::busy){
            result.work_preserved=true;
            // Only positively reaped children permit moving owned fixed drops.
            // An active/unconfirmed job retains its exact original paths.
            if(result.cleanup_complete&&!files.preserve(root,owner))result.cleanup_complete=false;
            if(!files.retained_location.empty())result.retained_location=files.retained_location+(result.retained_location.empty()?"":"; "+result.retained_location);
        }
    }catch(...){result.cleanup_complete=false;result.work_preserved=true;}
    return result;
}
} // namespace sfx::apps::runtime
