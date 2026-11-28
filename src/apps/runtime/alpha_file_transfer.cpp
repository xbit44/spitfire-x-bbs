#include <winsock2.h>
#include "alpha_file_transfer.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "transfer_ini_policy.hpp"
#include "qwk_packet_completion.hpp"
#include "qwk_private_file_descriptor.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/storage_access.hpp"
#include "sfx/platform/win32/file_receive_budget.hpp"
#include "sfx/application/qwk.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace inst=platform::win32::installation;
using inst::Handle;using inst::Path;using inst::require;
using file_transfer_detail::Direction;
constexpr char file_helper_sha[]="8B87D172E85FE23AA792A984E69DC305292A74D125F7414B7607218B487479A4";
struct Socket {SOCKET value{INVALID_SOCKET};~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}};
struct Attributes {std::vector<unsigned char> bytes;LPPROC_THREAD_ATTRIBUTE_LIST list{};~Attributes(){if(list)DeleteProcThreadAttributeList(list);}};
using Child=OwnedTransferChild;
// Sleep(1) can lose an entire scheduler tick at each bounded ZMODEM window.
// Use an unnamed, non-inheritable one-shot timer only while this file pump is
// idle. No process/global timer-resolution request, spin loop or larger credit.
// Older Windows and timer failures retain the original bounded Sleep fallback.
struct TransferIdleWait {
    Handle timer{CreateWaitableTimerExW(nullptr,nullptr,0x00000002U,
        TIMER_MODIFY_STATE|SYNCHRONIZE)}; // HIGH_RESOLUTION, Windows10 1803+
    ~TransferIdleWait(){if(timer)CancelWaitableTimer(timer.get());}
    void pause()noexcept{
        if(timer){
            LARGE_INTEGER due{};due.QuadPart=-1000; // 100 microseconds, relative
            if(SetWaitableTimer(timer.get(),&due,0,nullptr,nullptr,FALSE)){
                const auto result=WaitForSingleObject(timer.get(),2);
                // A bounded timeout is ordinary scheduler pressure, not timer
                // failure. Recheck all guards/I/O before the next idle wait.
                if(result==WAIT_OBJECT_0||result==WAIT_TIMEOUT)return;
            }
            // Do not retry a failing timer on every turn or leave it armed.
            CancelWaitableTimer(timer.get());timer.reset();
        }
        Sleep(1);
    }
};
// Queue and counters remain bounded independently of file/batch sizes.
struct Queue {
    std::string data;std::size_t first{};bool paused{};
    std::size_t size()const{return data.size()-first;}
    std::size_t capacity(){if(size()>=app::kMessageReceiveHighWater)paused=true;else if(size()<app::kMessageReceiveLowWater)paused=false;return paused?0:std::min<std::size_t>(4096,app::kMessageReceiveQueueBytes-size());}
    void append(const std::uint8_t* p,std::size_t n){require(n<=app::kMessageReceiveQueueBytes-size(),"file_transfer_queue_bound");if(first){data.erase(0,first);first=0;}data.append(reinterpret_cast<const char*>(p),n);}
    std::string_view front()const{return std::string_view(data).substr(first,std::min<std::size_t>(4096,size()));}
    void consume(std::size_t n){require(n<=size(),"file_transfer_queue_consume");first+=n;if(first==data.size()){data.clear();first=0;}}
};
std::string snapshot(HANDLE h,std::size_t maximum){
    LARGE_INTEGER n{},zero{};BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(h,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileSizeEx(h,&n)&&n.QuadPart>0&&static_cast<std::uint64_t>(n.QuadPart)<=maximum&&SetFilePointerEx(h,zero,nullptr,FILE_BEGIN),"file_transfer_control_object");
    std::string out(static_cast<std::size_t>(n.QuadPart),'\0');for(std::size_t at=0;at<out.size();){DWORD got{};require(ReadFile(h,out.data()+at,static_cast<DWORD>(std::min<std::size_t>(4096,out.size()-at)),&got,nullptr)&&got,"file_transfer_control_read");at+=got;}return out;
}
std::uint64_t exact_object(HANDLE h,const app::FileObjectIdentity& expected){
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    require(GetFileType(h)==FILE_TYPE_DISK&&GetFileInformationByHandle(h,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileSizeEx(h,&size)&&size.QuadPart>=0,"file_transfer_payload_regular");
    require(std::to_string(inst::file_id(h))==expected.file_id&&std::to_string(inst::volume_serial(h))==expected.volume,"file_transfer_payload_identity");return static_cast<std::uint64_t>(size.QuadPart);
}
std::string encoded_name(std::string_view name){
    // Legacy ZMODEM metadata is CP437; it never becomes a path or command.
    // Reuse the application basename policy (including DOS device aliases),
    // independently of the separate helper's defensive manifest checks.
    require(!name.empty()&&name.size()<=128&&app::valid_file_name(app::qwk_cp437_to_utf8(name)),"file_transfer_name_bound");
    std::string encoded;for(unsigned char c:name){require(c>=32&&c!=127&&std::string_view("/\\:<>\"|?*").find(static_cast<char>(c))==std::string_view::npos,"file_transfer_name_structure");encoded+="0123456789ABCDEF"[c>>4];encoded+="0123456789ABCDEF"[c&15];}return encoded;
}
bool no_ini(const Path& path){WIN32_FIND_DATAW d{};const auto h=FindFirstFileW((path/L"*.ini").c_str(),&d);if(h!=INVALID_HANDLE_VALUE){FindClose(h);return false;}return GetLastError()==ERROR_FILE_NOT_FOUND;}
using PrivateJob=OwnedTransferWork;
}
struct AlphaFileTransfer::Impl {
    Path root;std::wstring owner;DecodedFileTransferChannel channel;MessageReceiveObserver observer;WorkProblemObserver work_problem;
    Impl(Path r,std::wstring o,DecodedFileTransferChannel c,MessageReceiveObserver observe,WorkProblemObserver problem):root(std::move(r)),owner(std::move(o)),channel(std::move(c)),observer(std::move(observe)),work_problem(std::move(problem)){}
    struct Pins {Handle root,bin,tools,work,helper;};
    Pins pin(){
        require(channel.acquire&&channel.release&&channel.read&&channel.write&&channel.now&&channel.absolute_deadline&&channel.authorized&&root.is_absolute()&&inst::canonical_root(root)==root,"file_transfer_channel");Pins p;
        p.root=inst::open_object(root,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        p.bin=inst::open_object(root/L"Bin",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        p.tools=inst::open_object(root/L"Bin"/L"Tools",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        p.work=inst::open_object(root/L"Work",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        for(const auto& path:{root,root/L"Bin",root/L"Bin"/L"Tools"})require(platform::win32::assess_current_program_access(path,platform::win32::StorageDaclObjectKind::directory).status.succeeded(),"file_transfer_structural_access");
        const auto work=platform::win32::WindowsStorageDaclPolicy(owner).assess(root/L"Work",platform::win32::StorageDaclObjectKind::directory);require(work.status&&work.private_access&&work.effective_access,"file_transfer_work_security");
        p.helper=inst::open_object(root/L"Bin"/L"Tools"/L"SFXFileTransfer.exe",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);const auto bytes=snapshot(p.helper.get(),4*1024*1024);
        require(platform::win32::assess_current_program_access(root/L"Bin"/L"Tools"/L"SFXFileTransfer.exe",platform::win32::StorageDaclObjectKind::file).status.succeeded()&&inst::sha256({bytes.begin(),bytes.end()})==file_helper_sha&&transfer_ini_directory_clear(root/L"Bin",true)&&no_ini(root/L"Bin"/L"Tools"),"file_transfer_helper_identity");return p;
    }
    void observed(std::string_view phase,DWORD error,DWORD exit,std::size_t pending)noexcept{if(observer)try{observer(phase,error,exit,pending);}catch(...){}}
    bool recover(std::uint64_t began,std::uint64_t deadline,std::size_t buffered){
        // An unproven receipt never becomes success. Explicit ZMODEM cancel,
        // bounded raw draining and a quiet boundary only restore text mode.
        // This is NOT used for forged completion/tampered control evidence.
        if(!channel.authorized()||(channel.begin_recovery&&!channel.begin_recovery()))return false;
        const auto start=channel.now();const auto end=std::min(deadline,start+5000);
        if(start<began||start>=end||buffered>262144)return false;
        constexpr std::string_view cancel="\x18\x18\x18\x18\x18\x18\x18\x18\b\b\b\b\b\b\b\b";
        const auto valid=[&]{const auto live=channel.absolute_deadline();const auto now=channel.now();return channel.authorized()&&now>=start&&now<end&&(!live||now<live);};
        std::size_t sent{};
        while(sent<cancel.size()){
            if(!valid())return false;const auto bytes=cancel.substr(sent);const auto written=channel.write(bytes);
            if(!app::valid_interactive_stream_write_result(bytes.size(),written))return false;
            if(written.outcome==app::InteractiveStreamOutcome::completed)sent+=written.bytes_written;
            else if(written.outcome!=app::InteractiveStreamOutcome::timed_out)return false;
            Sleep(1);
        }
        auto quiet=channel.now();std::size_t drained=buffered;
        while(valid()){
            const auto read=channel.read(4096);if(!app::valid_interactive_stream_read_result(4096,read))return false;
            if(read.outcome==app::InteractiveStreamOutcome::completed){
                if(read.bytes.size()>262144-drained)return false;drained+=read.bytes.size();quiet=channel.now();
            }else if(read.outcome==app::InteractiveStreamOutcome::timed_out){
                const auto now=channel.now();if(now>=quiet&&now-quiet>=1000)return valid();
            }else return false;
            Sleep(1);
        }
        return false;
    }
    NativeFileTransferResult run(Direction direction,const std::vector<NativeFileTransferItem>& items,app::TransferProtocolPreference protocol){
        bool leased{},synchronized{};struct Release{Impl& self;bool& leased;bool& synchronized;~Release(){if(leased)self.channel.release(synchronized);}} release{*this,leased,synchronized};
        try{
            const auto protocol_code=file_transfer_detail::protocol_code(protocol);
            require(protocol_code&&(protocol_code!='X'||items.size()==1),"file_transfer_protocol");
            auto pins=pin();require(channel.authorized()&&!items.empty()&&items.size()<=app::kFileQueueMaximum,"file_transfer_authority_manifest");const bool sending=direction==Direction::send;
            auto work=std::make_shared<PrivateJob>();auto& control=*work;control.notice=work_problem;control.owner=owner;control.path=root/L"Work"/(L"file-"+inst::unique_id());const auto sd=inst::mutable_descriptor(owner);inst::create_directory(control.path,sd.get());
            control.directory=inst::open_object(control.path,true,DELETE|FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);control.identity=inst::file_id(control.directory.get());control.volume=inst::volume_serial(control.directory.get());require(inst::exact_security(control.directory.get(),sd.get())&&no_ini(control.path),"file_transfer_job_security");
            std::vector<Handle> payloads,space_keepers;std::vector<std::shared_ptr<platform::win32::FileReceiveBudget>> space_budgets;std::vector<file_transfer_detail::ExpectedFile> expected;std::set<std::string> names;std::set<HANDLE> unique_handles;std::set<std::pair<std::string,std::string>> unique_objects;
            std::string configuration=sending?"SFX-FILE-SEND 1\n":"SFX-FILE-RECEIVE 1\n";configuration+=std::to_string(items.size())+'\n';
            for(const auto& item:items){
                require(item.duplicate&&item.maximum>0&&item.maximum<=app::kMaximumCallerFileBytes,"file_transfer_item_bound");const auto name=encoded_name(item.wire_name);auto folded=item.wire_name;for(auto& c:folded)if(c>='a'&&c<='z')c-=32;require(names.insert(folded).second,"file_transfer_duplicate_name");
                require(unique_objects.insert({item.object.volume,item.object.file_id}).second,"file_transfer_duplicate_object");
                auto h=item.duplicate();require(h&&h.get()!=INVALID_HANDLE_VALUE&&unique_handles.insert(h.get()).second,"file_transfer_duplicate_capability");const auto bytes=exact_object(h.get(),item.object);require(bytes<=item.maximum&&(sending||bytes==0),"file_transfer_initial_length");DWORD flags{};require(GetHandleInformation(h.get(),&flags)&&(flags&HANDLE_FLAG_INHERIT),"file_transfer_inherit_capability");
                if(!sending){auto budget=platform::win32::FileReceiveBudget::for_transfer(h.get(),owner,item.maximum);space_keepers.push_back(budget->inherit_keeper());space_budgets.push_back(std::move(budget));}
                expected.push_back({item.maximum,bytes});configuration+=std::to_string(reinterpret_cast<std::uintptr_t>(h.get()))+' '+std::to_string(item.maximum)+' '+name+'\n';payloads.push_back(std::move(h));
            }
            const auto file_sd=qwk_native_detail::private_file_descriptor(owner);inst::create_file(control.path/L"job.cfg",{configuration.begin(),configuration.end()},file_sd.get());auto config=inst::open_object(control.path/L"job.cfg",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);require(snapshot(config.get(),131072)==configuration,"file_transfer_fixed_manifest");
            Socket listener,parent,endpoint;listener.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(listener.value!=INVALID_SOCKET&&SetHandleInformation(reinterpret_cast<HANDLE>(listener.value),HANDLE_FLAG_INHERIT,0),"file_transfer_listener");
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);require(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener.value,1)==0,"file_transfer_private_bind");int length=sizeof(address);require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"file_transfer_private_address");
            endpoint.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(endpoint.value!=INVALID_SOCKET&&connect(endpoint.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"file_transfer_private_connect");parent.value=accept(listener.value,nullptr,nullptr);require(parent.value!=INVALID_SOCKET,"file_transfer_private_accept");closesocket(listener.value);listener.value=INVALID_SOCKET;
            require(SetHandleInformation(reinterpret_cast<HANDLE>(parent.value),HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(reinterpret_cast<HANDLE>(endpoint.value),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"file_transfer_socket_allowlist");u_long nonblocking=1;require(ioctlsocket(parent.value,FIONBIO,&nonblocking)==0,"file_transfer_nonblocking");
            // The helper now batches private IPC at protocol boundaries. Do
            // not let Nagle/delayed-ACK coupling stall a short control header
            // while the caller waits for that very header. PRIVATE sockets
            // only: no global TCP settings or message/QWK transport changes.
            const int immediate=1;
            require(setsockopt(parent.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))==0&&
                setsockopt(endpoint.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))==0,"file_transfer_private_latency");
            SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};Handle null_io(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherited,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));require(null_io.get()!=INVALID_HANDLE_VALUE,"file_transfer_null_io");
            std::vector<HANDLE> allowed{reinterpret_cast<HANDLE>(endpoint.value),null_io.get()};for(const auto& h:payloads)allowed.push_back(h.get());for(const auto& h:space_keepers)allowed.push_back(h.get());Attributes attributes;SIZE_T needed{};InitializeProcThreadAttributeList(nullptr,1,0,&needed);require(needed&&needed<65536,"file_transfer_attributes_bound");attributes.bytes.resize(needed);attributes.list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());require(InitializeProcThreadAttributeList(attributes.list,1,0,&needed)&&UpdateProcThreadAttribute(attributes.list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowed.data(),allowed.size()*sizeof(HANDLE),nullptr,nullptr),"file_transfer_explicit_handles");
            // Child is destroyed/reaped BEFORE inherited parent duplicates and
            // original leases are used for verification or control cleanup.
            Child child;child.job.reset(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.BasicLimitInformation.ActiveProcessLimit=1;require(child.job&&SetInformationJobObject(child.job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"file_transfer_job_limits");
            std::array<wchar_t,32768> windows{};const auto n=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));require(n&&n<windows.size(),"file_transfer_system_root");std::wstring environment=L"SystemRoot="+std::wstring(windows.data())+L'\0'+L"TEMP="+control.path.native()+L'\0'+L"TMP="+control.path.native()+L'\0'+L'\0';
            const auto exe=root/L"Bin"/L"Tools"/L"SFXFileTransfer.exe";std::wstring command=L"\""+exe.native()+L"\" "+(sending?L"--sfx-file-send-v1 ":L"--sfx-file-receive-v1 ")+std::to_wstring(static_cast<unsigned long long>(endpoint.value));
            if(protocol_code!='Z')command=L"\""+exe.native()+L"\" "+(sending?L"--sfx-file-send-v2 ":L"--sfx-file-receive-v2 ")+std::to_wstring(static_cast<unsigned long long>(endpoint.value))+L" "+static_cast<wchar_t>(protocol_code);
            STARTUPINFOEXW start_info{};start_info.StartupInfo.cb=sizeof(start_info);start_info.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;start_info.StartupInfo.wShowWindow=SW_HIDE;start_info.StartupInfo.hStdInput=start_info.StartupInfo.hStdOutput=start_info.StartupInfo.hStdError=null_io.get();start_info.lpAttributeList=attributes.list;PROCESS_INFORMATION process{};
            require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),control.path.c_str(),&start_info.StartupInfo,&process),"file_transfer_create_suspended");child.process.reset(process.hProcess);child.thread.reset(process.hThread);
            if(!AssignProcessToJobObject(child.job.get(),child.process.get())){TerminateProcess(child.process.get(),1);WaitForSingleObject(child.process.get(),2000);throw std::runtime_error("file_transfer_job_assignment");}
            child.assigned=true;
            for(const auto& budget:space_budgets)budget->bind_child(child.process.get());
            require(channel.authorized()&&channel.acquire(),"file_transfer_acquire");leased=true;const auto began=channel.now();auto deadline=began+app::kMessageReceiveTotalMillis;const auto initial=channel.absolute_deadline();if(initial)deadline=std::min(deadline,initial);require(deadline>began,"file_transfer_initial_deadline");require(ResumeThread(child.thread.get())!=static_cast<DWORD>(-1),"file_transfer_resume");child.thread.reset();
            Queue inbound,outbound;TransferIdleWait idle_wait;message_receive_detail::ForwardedInput input;file_transfer_detail::OutputLedger output;std::uint64_t last=began;bool reaped{},eof{},closed{},complete{};DWORD exit=STILL_ACTIVE;std::string_view stop_phase="file_transfer_incomplete";
            for(;;){
                const auto now=channel.now(),live=channel.absolute_deadline();if(live)deadline=std::min(deadline,live);
                bool progressed{};
                if(now<began||now>=deadline||now<last){stop_phase="file_transfer_hard_deadline";break;}
                if(now-last>=app::kMessageReceiveIdleMillis){stop_phase="file_transfer_idle";break;}
                if(!channel.authorized()){stop_phase="file_transfer_authority";break;}
                if(!reaped&&WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0){require(GetExitCodeProcess(child.process.get(),&exit),"file_transfer_exit");reaped=closed=true;require(shutdown(endpoint.value,SD_SEND)==0,"file_transfer_half_close");}
                if(!closed&&!eof&&inbound.size()){const auto data=inbound.front();const auto sent=::send(parent.value,data.data(),static_cast<int>(data.size()),0);if(sent>0){require(input.record(data.substr(0,sent)),"file_transfer_input_count");inbound.consume(sent);progressed=true;}else if(sent==0)closed=true;else if(WSAGetLastError()!=WSAEWOULDBLOCK)break;}
                if(!eof&&outbound.capacity()){std::array<std::uint8_t,4096> data{};const auto got=recv(parent.value,reinterpret_cast<char*>(data.data()),static_cast<int>(outbound.capacity()),0);if(got>0){outbound.append(data.data(),got);progressed=true;}else if(!got)eof=true;else if(WSAGetLastError()!=WSAEWOULDBLOCK)break;}
                if(outbound.size()){const auto data=outbound.front();const auto sent=channel.write(data);if(!app::valid_interactive_stream_write_result(data.size(),sent)){stop_phase="file_transfer_caller_write";break;}if(sent.outcome==app::InteractiveStreamOutcome::completed){require(output.record(data.substr(0,sent.bytes_written)),"file_transfer_output_count");outbound.consume(sent.bytes_written);progressed=true;if(sending)last=channel.now();}else if(sent.outcome!=app::InteractiveStreamOutcome::timed_out){stop_phase="file_transfer_caller_write";break;}}
                if(!eof&&!closed&&inbound.capacity()){const auto maximum=inbound.capacity();const auto read=channel.read(maximum);if(!app::valid_interactive_stream_read_result(maximum,read)){stop_phase="file_transfer_caller_read";break;}if(read.outcome==app::InteractiveStreamOutcome::completed){inbound.append(read.bytes.data(),read.bytes.size());last=channel.now();progressed=true;}else if(read.outcome!=app::InteractiveStreamOutcome::timed_out){stop_phase="file_transfer_caller_read";break;}}
                if(reaped&&eof&&!outbound.size()){complete=exit==0;stop_phase="file_transfer_helper_exit";break;}
                // Keep the original bounds/guards on EVERY turn. Yield when
                // busy; sleep only when neither direction made progress.
                if(progressed)SwitchToThread();else idle_wait.pause();
            }
            if(!complete){observed(stop_phase,GetLastError(),exit,inbound.size());shutdown(parent.value,SD_BOTH);
                if(child.reap()&&recover(began,deadline,inbound.size())){synchronized=true;observed("file_transfer_recovered",0,exit,0);return {NativeFileTransferCode::failed,{},0};}
                return {NativeFileTransferCode::disconnected,{},0};}
            require(child.reap(),"file_transfer_children_not_stopped");control.children_stopped=true;
            const auto valid=[&]{const auto live=channel.absolute_deadline();if(live)deadline=std::min(deadline,live);const auto now=channel.now();return now>=began&&now<deadline&&now>=last&&now-last<app::kMessageReceiveIdleMillis&&channel.authorized();};require(valid(),"file_transfer_final_authority");
            for(std::size_t i=0;i<items.size();++i){const auto actual=exact_object(payloads[i].get(),items[i].object);require(actual<=items[i].maximum&&(!sending||actual==expected[i].actual),"file_transfer_final_payload");expected[i].actual=actual;}
            std::size_t count{};for(const auto& entry:std::filesystem::directory_iterator(control.path)){const auto name=entry.path().filename().native();require(name==L"job.cfg"||name==L"result.txt","file_transfer_extra_control_object");++count;}require(count==2&&snapshot(config.get(),131072)==configuration,"file_transfer_control_manifest_unchanged");
            auto receipt=inst::open_object(control.path/L"result.txt",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
            platform::win32::WindowsStorageDaclPolicy policy(owner);
            if(platform::win32::service_file_owner_enabled(owner))require(policy.apply(control.path/L"result.txt",platform::win32::StorageDaclObjectKind::file).succeeded(),"file_transfer_receipt_service_owner");
            const auto assessment=policy.assess(control.path/L"result.txt",platform::win32::StorageDaclObjectKind::file,true);require(assessment.status&&assessment.effective_access,"file_transfer_receipt_security");
            const auto proof=file_transfer_detail::completion(direction,snapshot(receipt.get(),file_transfer_detail::maximum_receipt),expected,protocol);
            require(proof&&output.finished(direction,protocol,proof->produced)&&input.completion_tail(proof->consumed,std::string_view(inbound.data).substr(inbound.first))&&valid(),"file_transfer_complete_proof");
            control.capture(L"job.cfg",config.get());control.capture(L"result.txt",receipt.get());
            if(!sending&&!proof->attempted&&!proof->refused)control.confirm_completed();
            synchronized=true;return {NativeFileTransferCode::completed,proof->files,proof->refused,[work]{work->confirm_completed();}};
        }catch(const std::exception& e){const std::string_view phase=e.what();observed(phase.size()<=64&&phase.substr(0,14)=="file_transfer_"?phase:"file_transfer_local_failure",GetLastError(),0,0);return {leased&&!synchronized?NativeFileTransferCode::disconnected:NativeFileTransferCode::unavailable,{},0};}
        catch(...){return {leased?NativeFileTransferCode::disconnected:NativeFileTransferCode::unavailable,{},0};}
    }
};
AlphaFileTransfer::AlphaFileTransfer(Path root,std::wstring owner,DecodedFileTransferChannel channel,MessageReceiveObserver observe,WorkProblemObserver problem):impl_(std::make_unique<Impl>(std::move(root),std::move(owner),std::move(channel),std::move(observe),std::move(problem))){}
AlphaFileTransfer::~AlphaFileTransfer()=default;
bool AlphaFileTransfer::available(){try{auto pins=impl_->pin();return impl_->channel.authorized();}catch(...){return false;}}
NativeFileTransferResult AlphaFileTransfer::run(Direction d,const std::vector<NativeFileTransferItem>& items,app::TransferProtocolPreference protocol){return impl_->run(d,items,protocol);}
} // namespace sfx::apps::runtime
