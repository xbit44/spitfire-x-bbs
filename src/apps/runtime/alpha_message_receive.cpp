#include <winsock2.h>
#include "alpha_message_receive.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "transfer_ini_policy.hpp"
#include "message_receive_completion.hpp"
#include "message_receive_policy.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/storage_access.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace sfx::apps::runtime {
namespace {
namespace app=sfx::application;
namespace inst=sfx::platform::win32::installation;
using inst::Handle;using inst::Path;using inst::require;
constexpr char helper_sha[]="F6C7E6079AA68F3C10A902A8CC90257C8F0BA158FCEEC76EDA0C9E2CBB7DFB91";
struct Socket {
    SOCKET value{INVALID_SOCKET};
    ~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
};
std::string snapshot(HANDLE file,std::size_t maximum){
    LARGE_INTEGER length{},zero{};BY_HANDLE_FILE_INFORMATION before{},after{};
    require(GetFileInformationByHandle(file,&before) && !(before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) && before.nNumberOfLinks==1,"receive_file_not_regular");
    require(GetFileSizeEx(file,&length) && length.QuadPart>0 && static_cast<unsigned long long>(length.QuadPart)<=maximum && SetFilePointerEx(file,zero,nullptr,FILE_BEGIN),"receive_file_size");
    std::string bytes(static_cast<std::size_t>(length.QuadPart),'\0');std::size_t offset{};
    while(offset<bytes.size()){
        DWORD read{};require(ReadFile(file,bytes.data()+offset,static_cast<DWORD>(std::min<std::size_t>(4096,bytes.size()-offset)),&read,nullptr) && read && read<=bytes.size()-offset,"receive_file_read");offset+=read;
    }
    char extra;DWORD read{};require(ReadFile(file,&extra,1,&read,nullptr) && read==0 && GetFileInformationByHandle(file,&after),"receive_file_eof");
    require(before.dwVolumeSerialNumber==after.dwVolumeSerialNumber && before.nFileIndexHigh==after.nFileIndexHigh && before.nFileIndexLow==after.nFileIndexLow && before.nFileSizeHigh==after.nFileSizeHigh && before.nFileSizeLow==after.nFileSizeLow && before.nNumberOfLinks==after.nNumberOfLinks &&
        before.ftLastWriteTime.dwHighDateTime==after.ftLastWriteTime.dwHighDateTime && before.ftLastWriteTime.dwLowDateTime==after.ftLastWriteTime.dwLowDateTime,"receive_file_changed");
    return bytes;
}
bool no_ini(const Path& dir){
    WIN32_FIND_DATAW data{};const auto found=FindFirstFileW((dir/L"*.ini").c_str(),&data);
    if(found!=INVALID_HANDLE_VALUE){FindClose(found);return false;}
    return GetLastError()==ERROR_FILE_NOT_FOUND;
}
struct Queue {
    std::string bytes;std::size_t first{};bool paused{};
    std::size_t size()const{return bytes.size()-first;}
    std::size_t capacity(){
        if(size()>=app::kMessageReceiveHighWater)paused=true;
        else if(size()<app::kMessageReceiveLowWater)paused=false;
        return paused?0:std::min<std::size_t>(4096,app::kMessageReceiveQueueBytes-size());
    }
    void append(const std::uint8_t* data,std::size_t count){
        require(count<=app::kMessageReceiveQueueBytes-size(),"receive_queue_overflow");
        if(first){bytes.erase(0,first);first=0;}bytes.append(reinterpret_cast<const char*>(data),count);
    }
    std::string_view front()const{return std::string_view(bytes).substr(first,std::min<std::size_t>(4096,size()));}
    void consume(std::size_t count){require(count<=size(),"receive_queue_consume");first+=count;if(first==bytes.size()){bytes.clear();first=0;}}
};
// Retain failures with unknown objects for recovery. Known-object deletion is
// through validated handles; never recursively remove a caller-controlled path.
using PrivateJob=OwnedTransferWork;
using Child=OwnedTransferChild;
struct AttributeList {
    std::vector<std::uint8_t> bytes;LPPROC_THREAD_ATTRIBUTE_LIST value{};
    ~AttributeList(){if(value)DeleteProcThreadAttributeList(value);}
};
} // namespace
struct AlphaMessageSourceReceiver::Impl {
    Path root;std::wstring owner;app::DecodedMessageReceiveChannel channel;MessageReceiveObserver observer;WorkProblemObserver work_problem;
    Impl(Path r,std::wstring o,app::DecodedMessageReceiveChannel c,MessageReceiveObserver observe,WorkProblemObserver problem):root(std::move(r)),owner(std::move(o)),channel(std::move(c)),observer(std::move(observe)),work_problem(std::move(problem)){}
    void observed(std::string_view phase,std::uint32_t error,std::uint32_t exit,std::size_t pending)noexcept{
        if(observer)try{observer(phase,error,exit,pending);}catch(...){/* Diagnostics never alter security/outcome. */}
    }
    struct Pinned {Handle root,bin,tools,work,helper;};
    Pinned pin(){
        require(app::valid_message_receive_channel(channel) && root.is_absolute() && inst::canonical_root(root)==root,"receive_root");
        Pinned p;
        p.root=inst::open_object(root,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        p.bin=inst::open_object(root/L"Bin",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        p.tools=inst::open_object(root/L"Bin"/L"Tools",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        p.work=inst::open_object(root/L"Work",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        for(const auto& path:{root,root/L"Bin",root/L"Bin"/L"Tools"})require(sfx::platform::win32::assess_current_program_access(path,sfx::platform::win32::StorageDaclObjectKind::directory).status.succeeded(),"receive_structural_access");
        const auto policy=sfx::platform::win32::WindowsStorageDaclPolicy(owner);
        const auto secure=policy.assess(root/L"Work",sfx::platform::win32::StorageDaclObjectKind::directory);
        require(secure.status && secure.private_access && secure.effective_access,"receive_work_security");
        const auto helper=root/L"Bin"/L"Tools"/L"sexyz.exe";
        p.helper=inst::open_object(helper,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        require(sfx::platform::win32::assess_current_program_access(helper,sfx::platform::win32::StorageDaclObjectKind::file).status.succeeded() && inst::sha256(std::vector<std::uint8_t>(snapshot_bytes(p.helper.get())))==helper_sha,"receive_helper_identity");
        require(no_ini(root/L"Bin"/L"Tools") && transfer_ini_directory_clear(root/L"Bin",true),"receive_adjacent_ini");
        return p;
    }
    static std::vector<std::uint8_t> snapshot_bytes(HANDLE file){auto bytes=snapshot(file,4U*1024U*1024U);return {bytes.begin(),bytes.end()};}
    app::MessageReceiveResult run(const app::MessageReceiveRequest& request){
        bool leased=false,synchronized=false;
        struct Lease {Impl& self;bool& leased;bool& synchronized;~Lease(){if(leased)self.channel.release(synchronized);}} lease{*this,leased,synchronized};
        try{
            auto pinned=pin();require(channel.authorized(request),"receive_authority_preflight");
            PrivateJob job;job.notice=work_problem;job.owner=owner;job.path=root/L"Work"/(L"message-"+inst::unique_id());
            const auto descriptor=inst::mutable_descriptor(owner);inst::create_directory(job.path,descriptor.get());
            job.directory=inst::open_object(job.path,true,DELETE|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
            job.identity=inst::file_id(job.directory.get());job.volume=inst::volume_serial(job.directory.get());require(no_ini(job.path),"receive_job_ini");
            Socket listener,parent,child_socket;
            listener.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(listener.value!=INVALID_SOCKET,"receive_socket");
            require(SetHandleInformation(reinterpret_cast<HANDLE>(listener.value),HANDLE_FLAG_INHERIT,0),"receive_listener_private");
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            require(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener.value,1)==0,"receive_loopback_bind");
            int length=sizeof(address);require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"receive_loopback_address");
            child_socket.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(child_socket.value!=INVALID_SOCKET && connect(child_socket.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"receive_loopback_connect");
            parent.value=accept(listener.value,nullptr,nullptr);require(parent.value!=INVALID_SOCKET,"receive_loopback_accept");closesocket(listener.value);listener.value=INVALID_SOCKET;
            require(SetHandleInformation(reinterpret_cast<HANDLE>(parent.value),HANDLE_FLAG_INHERIT,0) && SetHandleInformation(reinterpret_cast<HANDLE>(child_socket.value),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"receive_socket_allowlist");
            u_long nonblocking=1;require(ioctlsocket(parent.value,FIONBIO,&nonblocking)==0,"receive_private_nonblocking");
            SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
            Handle null_io(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherited,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));require(null_io.get()!=INVALID_HANDLE_VALUE,"receive_null_io");
            HANDLE handles[]{reinterpret_cast<HANDLE>(child_socket.value),null_io.get()};AttributeList attributes;SIZE_T size{};
            InitializeProcThreadAttributeList(nullptr,1,0,&size);require(size>0 && size<65536,"receive_attributes_size");attributes.bytes.resize(size);attributes.value=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());
            require(InitializeProcThreadAttributeList(attributes.value,1,0,&size) && UpdateProcThreadAttribute(attributes.value,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr),"receive_handle_allowlist");
            Child child;child.job.reset(CreateJobObjectW(nullptr,nullptr));require(bool(child.job),"receive_job_object");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.BasicLimitInformation.ActiveProcessLimit=1;
            require(SetInformationJobObject(child.job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"receive_job_limits");
            std::array<wchar_t,32768> windows{};const auto chars=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));require(chars && chars<windows.size(),"receive_windows_directory");
            std::wstring environment=L"SystemRoot="+std::wstring(windows.data())+L'\0'+L"TEMP="+job.path.native()+L'\0'+L"TMP="+job.path.native()+L'\0'+L'\0';
            const auto exe=root/L"Bin"/L"Tools"/L"sexyz.exe";
            std::wstring command=L"\""+exe.native()+L"\" --sfx-message-v1 "+std::to_wstring(static_cast<unsigned long long>(child_socket.value));
            STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;
            startup.StartupInfo.hStdInput=startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=null_io.get();startup.lpAttributeList=attributes.value;PROCESS_INFORMATION process{};
            require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),job.path.c_str(),&startup.StartupInfo,&process),"receive_create_suspended");
            child.process.reset(process.hProcess);child.thread.reset(process.hThread);
            if(!AssignProcessToJobObject(child.job.get(),child.process.get())){TerminateProcess(child.process.get(),1);WaitForSingleObject(child.process.get(),2000);throw std::runtime_error("receive_job_assignment");}
            child.assigned=true;
            require(channel.authorized(request) && channel.acquire(),"receive_channel_acquire");leased=true;
            const auto start=channel.now();auto deadline=start+app::kMessageReceiveTotalMillis;const auto existing=channel.absolute_deadline();if(existing)deadline=std::min(deadline,existing);
            require(deadline>start && channel.authorized(request),"receive_deadline_start");
            require(ResumeThread(child.thread.get())!=static_cast<DWORD>(-1),"receive_resume");child.thread.reset();
            // Keep our private duplicate of the inherited endpoint until the
            // helper is reaped. Otherwise closing its last socket reference
            // with an unread early Enter can reset the relay and discard the
            // helper's final reply. This endpoint is never read or drained.
            Queue inbound,outbound;message_receive_detail::ForwardedInput input_accounting;
            message_receive_detail::ForwardedOutput output_accounting;
            std::uint64_t last_inbound=start;bool eof=false,send_closed=false,helper_reaped=false;DWORD helper_exit=STILL_ACTIVE;app::MessageReceiveCode failure=app::MessageReceiveCode::failed;
            bool success=false;const char* phase="receive_helper_completion";DWORD socket_error=0;
            for(;;){
                const auto now=channel.now();const auto live_deadline=channel.absolute_deadline();if(live_deadline)deadline=std::min(deadline,live_deadline);
                if(now<start || now>=deadline || now-last_inbound>=app::kMessageReceiveIdleMillis){failure=app::MessageReceiveCode::timed_out;break;}
                if(!channel.authorized(request)){failure=app::MessageReceiveCode::cancelled;break;}
                if(!helper_reaped && WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0){
                    require(GetExitCodeProcess(child.process.get(),&helper_exit),"receive_helper_exit");
                    helper_reaped=true;send_closed=true;
                    // A reaped helper cannot produce more output. Half-close
                    // only its private send direction, allowing every buffered
                    // final byte to reach the caller before receipt validation.
                    // No caller input is consumed by this retained endpoint.
                    require(shutdown(child_socket.value,SD_SEND)==0,"receive_helper_half_close");
                }
                if(!send_closed && !eof && inbound.size()){
                    const auto bytes=inbound.front();const auto sent=send(parent.value,bytes.data(),static_cast<int>(bytes.size()),0);
                    if(sent>0){require(input_accounting.record(bytes.substr(0,sent)),"receive_input_count_overflow");inbound.consume(sent);}
                    else if(sent==0)send_closed=true;
                    else if((socket_error=WSAGetLastError())!=WSAEWOULDBLOCK){
                        phase="receive_helper_send";
                        if(socket_error==WSAECONNRESET || socket_error==WSAECONNABORTED)send_closed=true;
                        else break;
                    }
                }
                if(!eof && outbound.capacity()){
                    std::array<std::uint8_t,4096> bytes{};const auto got=recv(parent.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(outbound.capacity()),0);
                    if(got>0)outbound.append(bytes.data(),got);else if(got==0)eof=true;
                    else if((socket_error=WSAGetLastError())!=WSAEWOULDBLOCK){
                        phase="receive_helper_read";
                        if(socket_error==WSAECONNRESET || socket_error==WSAECONNABORTED)eof=true;
                        else break;
                    }
                }
                if(outbound.size()){
                    const auto bytes=outbound.front();const auto sent=channel.write(bytes);
                    if(!app::valid_interactive_stream_write_result(bytes.size(),sent))break;
                    if(sent.outcome==app::InteractiveStreamOutcome::completed){output_accounting.record(bytes.substr(0,sent.bytes_written));outbound.consume(sent.bytes_written);}
                    else if(sent.outcome!=app::InteractiveStreamOutcome::timed_out){failure=app::MessageReceiveCode::disconnected;break;}
                }
                // Stop reading caller traffic after helper EOF, so menu keys
                // arriving after a completed batch remain in the caller stream.
                if(!eof && !send_closed && inbound.capacity()){
                    const auto maximum=inbound.capacity();const auto read=channel.read(maximum);
                    if(!app::valid_interactive_stream_read_result(maximum,read))break;
                    if(read.outcome==app::InteractiveStreamOutcome::completed){inbound.append(read.bytes.data(),read.bytes.size());last_inbound=channel.now();}
                    else if(read.outcome!=app::InteractiveStreamOutcome::timed_out){failure=app::MessageReceiveCode::disconnected;break;}
                }
                if(helper_reaped && eof && !outbound.size()){
                    success=helper_exit==0 && output_accounting.finished();break;
                }
                // Bounded cancellation checks even when both channels stall.
                Sleep(1);
            }
            if(!success){
                (void)GetExitCodeProcess(child.process.get(),&helper_exit);
                observed(phase,socket_error,helper_exit,inbound.size());
                // Never turn incomplete protocol bytes into editor commands.
                shutdown(parent.value,SD_BOTH);TerminateJobObject(child.job.get(),1);WaitForSingleObject(child.process.get(),2000);
                (void)failure;return {app::MessageReceiveCode::disconnected,{}};
            }
            require(child.reap(),"receive_children_not_stopped");job.children_stopped=true;
            require(channel.authorized(request) && channel.now()<deadline,"receive_authority_completion");
            // Refusal has exactly result/DSZ; success additionally has payload.
            // Receipt state must match this actual file set, not an assumed one.
            std::size_t count{};bool payload_present=false;
            for(const auto& item:std::filesystem::directory_iterator(job.path)){
                const auto name=item.path().filename().native();require(name==L"payload.bin" || name==L"result.txt" || name==L"dsz.log","receive_extra_object");
                if(name==L"payload.bin")payload_present=true;++count;
            }require(count==(payload_present?3U:2U),"receive_exact_files");
            Handle payload;
            if(payload_present)payload=inst::open_object(job.path/L"payload.bin",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
            auto result=inst::open_object(job.path/L"result.txt",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
            auto dsz=inst::open_object(job.path/L"dsz.log",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
            const auto policy=sfx::platform::win32::WindowsStorageDaclPolicy(owner);
            for(const auto* name:{L"payload.bin",L"result.txt",L"dsz.log"}){
                if(!payload_present && std::wstring_view(name)==L"payload.bin")continue;
                if(sfx::platform::win32::service_file_owner_enabled(owner))require(policy.apply(job.path/name,sfx::platform::win32::StorageDaclObjectKind::file).succeeded(),"receive_result_service_owner");
                const auto secure=policy.assess(job.path/name,sfx::platform::win32::StorageDaclObjectKind::file,true);require(secure.status && secure.effective_access,"receive_file_security");
            }
            auto bytes=payload_present?snapshot(payload.get(),app::kMessageSourceBytes):std::string{};
            const auto completion=message_receive_detail::policy_completion(snapshot(result.get(),1024),snapshot(dsz.get(),1024),payload_present,bytes.size());
            require(completion && input_accounting.completion_tail(completion->consumed,std::string_view(inbound.bytes).substr(inbound.first)),"receive_completion_evidence");
            if(payload_present)job.capture(L"payload.bin",payload.get());job.capture(L"result.txt",result.get());job.capture(L"dsz.log",dsz.get());
            // Only an already-read single logical Enter is discarded. Never
            // drain later caller input or feed unaccounted protocol to editor.
            std::optional<app::MessageSource> source;
            bool valid=false;
            if(payload_present){
                source=app::MessageSource{bytes.find('\x1b')==std::string::npos?app::MessageSourceFormat::plain:app::MessageSourceFormat::ansi_canvas,1,std::move(bytes)};
                valid=bool(app::validate_message_source(*source,request.context.maximum_lines));
            }
            // File snapshot/parsing is bounded but still consumes time. Never
            // let this work outlive a shortened call/drain/AFK deadline or a
            // concurrent authority change before handing the source back.
            const auto final_deadline=channel.absolute_deadline();if(final_deadline)deadline=std::min(deadline,final_deadline);
            const auto final_now=channel.now();
            require(final_now>=start && final_now>=last_inbound && final_now-last_inbound<app::kMessageReceiveIdleMillis && final_now<deadline && channel.authorized(request),"receive_authority_after_validation");
            synchronized=true; // complete accounted protocol and fresh authority
            if(completion->refusal==message_receive_detail::PolicyRefusal::empty_batch){
                observed("receive_empty_cancelled",0,0,0);
                job.confirm_completed();
                return {app::MessageReceiveCode::cancelled,{}};
            }
            if(completion->refusal!=message_receive_detail::PolicyRefusal::none){
                const auto reason=completion->refusal==message_receive_detail::PolicyRefusal::extension?app::MessageReceiveRefusal::extension:app::MessageReceiveRefusal::too_large;
                observed("receive_policy_refused",0,0,0);
                return {app::MessageReceiveCode::refused,{},reason};
            }
            observed(valid?"receive_validated":"receive_content_refused",0,0,0);
            if(!valid)return {app::MessageReceiveCode::refused,{}};
            job.confirm_completed();
            return {app::MessageReceiveCode::completed,std::move(*source)};
        }catch(...){return {leased?app::MessageReceiveCode::disconnected:app::MessageReceiveCode::unavailable,{}};}
    }
};
AlphaMessageSourceReceiver::AlphaMessageSourceReceiver(Path root,std::wstring owner,app::DecodedMessageReceiveChannel channel,MessageReceiveObserver observer,WorkProblemObserver problem):impl_(std::make_unique<Impl>(std::move(root),std::move(owner),std::move(channel),std::move(observer),std::move(problem))){}
AlphaMessageSourceReceiver::~AlphaMessageSourceReceiver()=default;
bool AlphaMessageSourceReceiver::available(const app::MessageReceiveRequest& request){try{auto pinned=impl_->pin();return impl_->channel.authorized(request);}catch(...){return false;}}
app::MessageReceiveResult AlphaMessageSourceReceiver::receive(const app::MessageReceiveRequest& request){return impl_->run(request);}
} // namespace sfx::apps::runtime
