#include <winsock2.h>
#include "alpha_qwk_transfer.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "transfer_ini_policy.hpp"
#include "alpha_qwk_reply_spool.hpp"
#include "qwk_packet_completion.hpp"
#include "qwk_private_file_descriptor.hpp"
#include "sfx/application/message_receive.hpp"
#include "sfx/archive/qwk_archive.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/storage_access.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace sfx::apps::runtime {
namespace {
namespace app=sfx::application;
namespace inst=sfx::platform::win32::installation;
using inst::Handle;using inst::Path;using inst::require;
using qwk_packet_detail::Direction;
constexpr char helper_sha[]="981A34ADBBEE712E04FE39E4A087C938763D78A25C288749A64FAB05E8D049EB";
struct Socket {SOCKET value{INVALID_SOCKET};~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}};
// Match the ordinary-file pump's bounded idle wait; no global timer changes.
struct PacketIdleWait {
    Handle timer{CreateWaitableTimerExW(nullptr,nullptr,0x00000002U,TIMER_MODIFY_STATE|SYNCHRONIZE)};
    ~PacketIdleWait(){if(timer)CancelWaitableTimer(timer.get());}
    void pause()noexcept{
        if(timer){LARGE_INTEGER due{};due.QuadPart=-1000;
            if(SetWaitableTimer(timer.get(),&due,0,nullptr,nullptr,FALSE)){
                const auto result=WaitForSingleObject(timer.get(),2);
                if(result==WAIT_OBJECT_0||result==WAIT_TIMEOUT)return;
            }
            CancelWaitableTimer(timer.get());timer.reset();
        }
        Sleep(1);
    }
};
std::string snapshot(HANDLE file,std::size_t maximum,const std::function<bool()>& cancelled={}){
    LARGE_INTEGER length{},zero{};BY_HANDLE_FILE_INFORMATION before{},after{};
    require(GetFileInformationByHandle(file,&before)&&!(before.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&before.nNumberOfLinks==1,"packet_file_regular");
    require(GetFileSizeEx(file,&length)&&length.QuadPart>0&&static_cast<unsigned long long>(length.QuadPart)<=maximum&&SetFilePointerEx(file,zero,nullptr,FILE_BEGIN),"packet_file_size");
    std::string bytes(static_cast<std::size_t>(length.QuadPart),'\0');
    for(std::size_t at=0;at<bytes.size();){require(!cancelled||!cancelled(),"packet_snapshot_cancelled");DWORD read{};const auto n=static_cast<DWORD>(std::min<std::size_t>(4096,bytes.size()-at));require(ReadFile(file,bytes.data()+at,n,&read,nullptr)&&read>0&&read<=n,"packet_file_read");at+=read;}
    char extra;DWORD read{};require(ReadFile(file,&extra,1,&read,nullptr)&&read==0&&GetFileInformationByHandle(file,&after),"packet_file_eof");
    require(before.dwVolumeSerialNumber==after.dwVolumeSerialNumber&&before.nFileIndexHigh==after.nFileIndexHigh&&before.nFileIndexLow==after.nFileIndexLow&&before.nFileSizeHigh==after.nFileSizeHigh&&before.nFileSizeLow==after.nFileSizeLow&&before.nNumberOfLinks==after.nNumberOfLinks&&before.ftLastWriteTime.dwHighDateTime==after.ftLastWriteTime.dwHighDateTime&&before.ftLastWriteTime.dwLowDateTime==after.ftLastWriteTime.dwLowDateTime,"packet_file_changed");return bytes;
}
bool no_ini(const Path& directory){WIN32_FIND_DATAW data{};const auto find=FindFirstFileW((directory/L"*.ini").c_str(),&data);if(find!=INVALID_HANDLE_VALUE){FindClose(find);return false;}return GetLastError()==ERROR_FILE_NOT_FOUND;}
struct Queue {
    std::string bytes;std::size_t first{};bool paused{};
    std::size_t size()const{return bytes.size()-first;}
    std::size_t capacity(){if(size()>=app::kMessageReceiveHighWater)paused=true;else if(size()<app::kMessageReceiveLowWater)paused=false;return paused?0:std::min<std::size_t>(4096,app::kMessageReceiveQueueBytes-size());}
    void append(const std::uint8_t* data,std::size_t n){require(n<=app::kMessageReceiveQueueBytes-size(),"packet_queue_overflow");if(first){bytes.erase(0,first);first=0;}bytes.append(reinterpret_cast<const char*>(data),n);}
    std::string_view front()const{return std::string_view(bytes).substr(first,std::min<std::size_t>(4096,size()));}
    void consume(std::size_t n){require(n<=size(),"packet_queue_consume");first+=n;if(first==bytes.size()){bytes.clear();first=0;}}
};
using PrivateJob=OwnedTransferWork;
using Child=OwnedTransferChild;
struct Attributes {std::vector<std::uint8_t> bytes;LPPROC_THREAD_ATTRIBUTE_LIST value{};~Attributes(){if(value)DeleteProcThreadAttributeList(value);}};
struct TransferResult {app::QwkTransferCode code{app::QwkTransferCode::failed};app::QwkTransferRefusal refusal{};std::shared_ptr<app::IQwkReplySource> source;std::shared_ptr<std::atomic<bool>> cancellation;std::function<void()> confirm_completed;};
}
struct AlphaQwkPacketTransfer::Impl {
    Path root;std::wstring owner;app::DecodedQwkTransferChannel channel;MessageReceiveObserver observer;std::string unavailable;WorkProblemObserver work_problem;
    Impl(Path r,std::wstring o,app::DecodedQwkTransferChannel c,MessageReceiveObserver observe,WorkProblemObserver problem):root(std::move(r)),owner(std::move(o)),channel(std::move(c)),observer(std::move(observe)),work_problem(std::move(problem)){}
    void observed(std::string_view phase,DWORD error,DWORD exit,std::size_t pending)noexcept{if(observer)try{observer(phase,error,exit,pending);}catch(...){}}
    struct Pinned {Handle root,bin,tools,work,helper;};
    Pinned pin(const app::QwkTransferRequest& request){
        require(channel.acquire&&channel.release&&channel.read&&channel.write&&channel.now&&channel.absolute_deadline&&channel.authorized&&app::valid_qwk_board_id(request.board_id)&&app::canonical_qwk_board_id(request.board_id)==request.board_id&&request.board_revision>0&&root.is_absolute()&&inst::canonical_root(root)==root,"packet_request_channel");
        Pinned p;
        p.root=inst::open_object(root,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        p.bin=inst::open_object(root/L"Bin",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        p.tools=inst::open_object(root/L"Bin"/L"Tools",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        p.work=inst::open_object(root/L"Work",true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        for(const auto& path:{root,root/L"Bin",root/L"Bin"/L"Tools"})require(sfx::platform::win32::assess_current_program_access(path,sfx::platform::win32::StorageDaclObjectKind::directory).status.succeeded(),"packet_structural_access");
        const auto assessment=sfx::platform::win32::WindowsStorageDaclPolicy(owner).assess(root/L"Work",sfx::platform::win32::StorageDaclObjectKind::directory);require(assessment.status&&assessment.private_access&&assessment.effective_access,"packet_work_security");
        p.helper=inst::open_object(root/L"Bin"/L"Tools"/L"SFXQWKTransfer.exe",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        const auto bytes=snapshot(p.helper.get(),4U*1024U*1024U);require(sfx::platform::win32::assess_current_program_access(root/L"Bin"/L"Tools"/L"SFXQWKTransfer.exe",sfx::platform::win32::StorageDaclObjectKind::file).status.succeeded()&&inst::sha256({bytes.begin(),bytes.end()})==helper_sha,"packet_helper_identity");
        require(transfer_ini_directory_clear(root/L"Bin",true)&&no_ini(root/L"Bin"/L"Tools"),"packet_adjacent_ini");return p;
    }
    TransferResult run(const app::QwkTransferRequest& request,Direction direction,std::string_view zip){
        bool leased=false,synchronized=false;
        struct Lease {Impl& self;bool& leased;bool& synchronized;~Lease(){if(leased)self.channel.release(synchronized);}} lease{*this,leased,synchronized};
        try{
            auto pins=pin(request);require(channel.authorized(request),"packet_authority_preflight");
            const bool sending=direction==Direction::send;
            require(sending?(!zip.empty()&&zip.size()<=app::kQwkCompressedBytes):zip.empty(),"packet_send_size");
            auto job=std::make_shared<PrivateJob>();job->notice=work_problem;job->owner=owner;job->path=root/L"Work"/(L"qwk-"+inst::unique_id());const auto security=inst::mutable_descriptor(owner);inst::create_directory(job->path,security.get());
            job->directory=inst::open_object(job->path,true,DELETE|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);job->identity=inst::file_id(job->directory.get());job->volume=inst::volume_serial(job->directory.get());require(inst::exact_security(job->directory.get(),security.get())&&no_ini(job->path),"packet_job_security");
            Handle send_file;
            if(sending){const auto file_security=qwk_native_detail::private_file_descriptor(owner);inst::create_file(job->path/L"payload.qwk",{zip.begin(),zip.end()},file_security.get());send_file=inst::open_object(job->path/L"payload.qwk",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);require(inst::exact_security(send_file.get(),file_security.get())&&snapshot(send_file.get(),app::kQwkCompressedBytes)==zip,"packet_send_file_exact");}
            Socket listener,parent,child_socket;listener.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(listener.value!=INVALID_SOCKET&&SetHandleInformation(reinterpret_cast<HANDLE>(listener.value),HANDLE_FLAG_INHERIT,0),"packet_private_listener");
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);require(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener.value,1)==0,"packet_private_bind");
            int length=sizeof(address);require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"packet_private_address");child_socket.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(child_socket.value!=INVALID_SOCKET&&connect(child_socket.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"packet_private_connect");
            parent.value=accept(listener.value,nullptr,nullptr);require(parent.value!=INVALID_SOCKET,"packet_private_accept");closesocket(listener.value);listener.value=INVALID_SOCKET;
            require(SetHandleInformation(reinterpret_cast<HANDLE>(parent.value),HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(reinterpret_cast<HANDLE>(child_socket.value),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"packet_socket_allowlist");u_long nonblocking=1;require(ioctlsocket(parent.value,FIONBIO,&nonblocking)==0,"packet_private_nonblocking");
            const int immediate=1;
            require(setsockopt(parent.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))==0&&
                setsockopt(child_socket.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))==0,"packet_private_latency");
            SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};Handle null_io(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherited,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));require(null_io.get()!=INVALID_HANDLE_VALUE,"packet_null_io");
            HANDLE handles[]{reinterpret_cast<HANDLE>(child_socket.value),null_io.get()};Attributes attributes;SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);require(size>0&&size<65536,"packet_attribute_size");attributes.bytes.resize(size);attributes.value=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());
            require(InitializeProcThreadAttributeList(attributes.value,1,0,&size)&&UpdateProcThreadAttribute(attributes.value,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr),"packet_handle_allowlist");
            Child child;child.job.reset(CreateJobObjectW(nullptr,nullptr));require(bool(child.job),"packet_job_object");JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.BasicLimitInformation.ActiveProcessLimit=1;require(SetInformationJobObject(child.job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"packet_job_limits");
            std::array<wchar_t,32768> windows{};const auto chars=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));require(chars&&chars<windows.size(),"packet_system_root");std::wstring environment=L"SystemRoot="+std::wstring(windows.data())+L'\0'+L"TEMP="+job->path.native()+L'\0'+L"TMP="+job->path.native()+L'\0'+L'\0';
            const auto exe=root/L"Bin"/L"Tools"/L"SFXQWKTransfer.exe";
            std::wstring command=L"\""+exe.native()+L"\" "+(sending?std::wstring(L"--sfx-packet-send-v1 "):std::wstring(L"--sfx-packet-receive-v1 "))+std::to_wstring(static_cast<unsigned long long>(child_socket.value));
            if(sending)command+=L" "+std::wstring(request.board_id.begin(),request.board_id.end());
            STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.StartupInfo.hStdInput=startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=null_io.get();startup.lpAttributeList=attributes.value;PROCESS_INFORMATION process{};
            require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),job->path.c_str(),&startup.StartupInfo,&process),"packet_create_suspended");child.process.reset(process.hProcess);child.thread.reset(process.hThread);
            if(!AssignProcessToJobObject(child.job.get(),child.process.get())){TerminateProcess(child.process.get(),1);WaitForSingleObject(child.process.get(),2000);throw std::runtime_error("packet_job_assignment");}
            child.assigned=true;
            require(channel.authorized(request)&&channel.acquire(),"packet_channel_acquire");leased=true;
            const auto start=channel.now();auto deadline=start+app::kMessageReceiveTotalMillis;const auto current_deadline=channel.absolute_deadline();if(current_deadline)deadline=std::min(deadline,current_deadline);require(deadline>start&&channel.authorized(request),"packet_deadline_start");
            require(ResumeThread(child.thread.get())!=static_cast<DWORD>(-1),"packet_resume");child.thread.reset();
            // Retain the duplicate until helper reap: an unread early Enter must
            // not reset away the final helper response. Never read this endpoint.
            Queue inbound,outbound;PacketIdleWait idle_wait;message_receive_detail::ForwardedInput input;qwk_packet_detail::OutputLedger output;
            std::uint64_t last_inbound=start;bool eof=false,send_closed=false,reaped=false,complete=false;DWORD exit=STILL_ACTIVE,error=0;const char* phase="packet_relay_completion";
            for(;;){
                bool progressed=false;
                const auto now=channel.now(),live=channel.absolute_deadline();if(live)deadline=std::min(deadline,live);
                if(now<start||now>=deadline||now<last_inbound||now-last_inbound>=app::kMessageReceiveIdleMillis||!channel.authorized(request))break;
                if(!reaped&&WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0){require(GetExitCodeProcess(child.process.get(),&exit),"packet_helper_exit");reaped=true;send_closed=true;require(shutdown(child_socket.value,SD_SEND)==0,"packet_helper_half_close");}
                if(!send_closed&&!eof&&inbound.size()){
                    const auto bytes=inbound.front();const auto sent=::send(parent.value,bytes.data(),static_cast<int>(bytes.size()),0);
                    if(sent>0){require(input.record(bytes.substr(0,sent)),"packet_input_overflow");inbound.consume(sent);progressed=true;}else if(sent==0)send_closed=true;else if((error=WSAGetLastError())!=WSAEWOULDBLOCK){phase="packet_helper_send";if(error==WSAECONNRESET||error==WSAECONNABORTED)send_closed=true;else break;}
                }
                if(!eof&&outbound.capacity()){
                    std::array<std::uint8_t,4096> bytes{};const auto got=recv(parent.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(outbound.capacity()),0);
                    if(got>0){outbound.append(bytes.data(),got);progressed=true;}else if(got==0)eof=true;else if((error=WSAGetLastError())!=WSAEWOULDBLOCK){phase="packet_helper_read";if(error==WSAECONNRESET||error==WSAECONNABORTED)eof=true;else break;}
                }
                if(outbound.size()){
                    const auto bytes=outbound.front();const auto sent=channel.write(bytes);if(!app::valid_interactive_stream_write_result(bytes.size(),sent))break;
                    if(sent.outcome==app::InteractiveStreamOutcome::completed){require(output.record(bytes.substr(0,sent.bytes_written)),"packet_output_overflow");outbound.consume(sent.bytes_written);progressed=true;if(sending)last_inbound=channel.now();}else if(sent.outcome!=app::InteractiveStreamOutcome::timed_out)break;
                }
                if(!eof&&!send_closed&&inbound.capacity()){
                    const auto maximum=inbound.capacity();const auto read=channel.read(maximum);if(!app::valid_interactive_stream_read_result(maximum,read))break;
                    if(read.outcome==app::InteractiveStreamOutcome::completed){inbound.append(read.bytes.data(),read.bytes.size());last_inbound=channel.now();progressed=true;}else if(read.outcome!=app::InteractiveStreamOutcome::timed_out)break;
                }
                if(reaped&&eof&&!outbound.size()){complete=exit==0;break;}
                if(progressed)SwitchToThread();else idle_wait.pause();
            }
            if(!complete){(void)GetExitCodeProcess(child.process.get(),&exit);observed(phase,error,exit,inbound.size());shutdown(parent.value,SD_BOTH);TerminateJobObject(child.job.get(),1);WaitForSingleObject(child.process.get(),2000);return {app::QwkTransferCode::disconnected};}
            require(child.reap(),"packet_children_not_stopped");job->children_stopped=true;
            const auto cancelled=[&]{const auto live=channel.absolute_deadline();if(live)deadline=std::min(deadline,live);const auto now=channel.now();return now<start||now>=deadline||now<last_inbound||now-last_inbound>=app::kMessageReceiveIdleMillis||!channel.authorized(request);};
            require(!cancelled(),"packet_authority_completion");
            std::size_t count{};bool payload_present=false;const auto payload_name=sending?L"payload.qwk":L"payload.rep";
            for(const auto& object:std::filesystem::directory_iterator(job->path)){const auto name=object.path().filename().native();require(name==payload_name||name==L"result.txt"||name==L"dsz.log","packet_extra_object");if(name==payload_name)payload_present=true;++count;}require(count==(payload_present?3U:2U),"packet_exact_file_set");
            Handle received;if(!sending&&payload_present)received=inst::open_object(job->path/payload_name,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
            auto receipt=inst::open_object(job->path/L"result.txt",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);auto dsz=inst::open_object(job->path/L"dsz.log",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
            // The reaped System helper creates its own output files. Seal only
            // this exact regular-file set in our pinned new private job before
            // accepting bytes/receipts. Desktop keeps its original strict check.
            for(const auto* name:{payload_name,L"result.txt",L"dsz.log"}){if(name==payload_name&&!payload_present)continue;
                sfx::platform::win32::WindowsStorageDaclPolicy policy(owner);
                if(sfx::platform::win32::service_file_owner_enabled(owner))require(policy.apply(job->path/name,sfx::platform::win32::StorageDaclObjectKind::file).succeeded(),"packet_result_service_owner");
                const auto assessed=policy.assess(job->path/name,sfx::platform::win32::StorageDaclObjectKind::file,true);require(assessed.status&&assessed.effective_access,"packet_result_security");}
            auto bytes=payload_present?snapshot(sending?send_file.get():received.get(),app::kQwkCompressedBytes,cancelled):std::string{};if(sending)require(bytes==zip,"packet_send_final_identity");
            const auto proof=qwk_packet_detail::completion(direction,snapshot(receipt.get(),1024),snapshot(dsz.get(),1024),payload_present,bytes.size());
            require(proof&&output.finished(direction,proof->produced)&&input.completion_tail(proof->consumed,std::string_view(inbound.bytes).substr(inbound.first)),"packet_actual_completion");
            require(!cancelled(),"packet_authority_after_proof");synchronized=true;
            if(payload_present)job->capture(payload_name,sending?send_file.get():received.get());job->capture(L"result.txt",receipt.get());job->capture(L"dsz.log",dsz.get());
            if(proof->refusal){if(proof->refusal==3)job->confirm_completed();observed("packet_policy_refused",0,0,0);return {proof->refusal==3?app::QwkTransferCode::cancelled:app::QwkTransferCode::refused,proof->refusal==1?app::QwkTransferRefusal::extension:proof->refusal==2?app::QwkTransferRefusal::too_large:proof->refusal==4?app::QwkTransferRefusal::skipped:app::QwkTransferRefusal::none};}
            if(sending){observed("packet_send_completed",0,0,0);return {app::QwkTransferCode::completed,{},{},{},[job]{job->confirm_completed();}};}
            std::shared_ptr<app::IQwkReplySource> sealed;
            auto stop=std::make_shared<std::atomic<bool>>(false);
            {
                auto archive=sfx::archive::read_qwk_reply_zip(bytes,request.board_id,cancelled);if(!archive.result.ok()){observed("packet_reply_archive_refused",0,0,0);return {app::QwkTransferCode::refused,app::QwkTransferRefusal::content};}
                std::uint64_t expanded{};std::vector<app::QwkFile> files;for(auto& f:archive.files){expanded+=f.bytes.size();files.push_back({std::move(f.name),std::move(f.bytes)});}archive={};
                auto packet=app::decode_qwk_reply(request.board_id,files);files={};if(!packet){observed("packet_reply_codec_refused",0,0,0);return {app::QwkTransferCode::refused,app::QwkTransferRefusal::content};}
                require(!cancelled(),"packet_authority_before_seal");
                // No callback captures this host, caller stack or decoded data.
                // The caller-command RAII token plus fixed deadline outlive a
                // queued writer reference; writer rechecks all current gates.
                const auto source_cancel=[stop,deadline]{return stop->load()||GetTickCount64()>=deadline;};
                auto source=seal_qwk_reply_spool(job->path,owner,packet,bytes.size(),expanded,source_cancel,job,job->directory.get(),[job]{job->confirm_completed();});
                if(!source){const auto spool_phase=std::string_view(source.status.operation);observed(spool_phase.size()<=64&&spool_phase.substr(0,12)=="reply_spool_"?spool_phase:"packet_reply_spool_refused",0,0,0);return {app::QwkTransferCode::refused,app::QwkTransferRefusal::content};}sealed=*source.value;
            } // release all envelope/decoded/logical vectors BEFORE enqueue
            require(!cancelled(),"packet_authority_after_seal");observed("packet_reply_sealed",0,0,0);return {app::QwkTransferCode::completed,{},std::move(sealed),std::move(stop)};
        }catch(const std::exception& e){const std::string_view phase=e.what();observed(phase.size()<=64&&phase.substr(0,7)=="packet_"?phase:"packet_local_failure",GetLastError(),0,0);return {leased&&!synchronized?app::QwkTransferCode::disconnected:leased?app::QwkTransferCode::failed:app::QwkTransferCode::unavailable};}
        catch(...){return {leased&&!synchronized?app::QwkTransferCode::disconnected:leased?app::QwkTransferCode::failed:app::QwkTransferCode::unavailable};}
    }
};
AlphaQwkPacketTransfer::AlphaQwkPacketTransfer(Path root,std::wstring owner,app::DecodedQwkTransferChannel channel,MessageReceiveObserver observer,WorkProblemObserver problem):impl_(std::make_unique<Impl>(std::move(root),std::move(owner),std::move(channel),std::move(observer),std::move(problem))){}
AlphaQwkPacketTransfer::~AlphaQwkPacketTransfer()=default;
bool AlphaQwkPacketTransfer::available(const app::QwkTransferRequest& request){impl_->unavailable.clear();try{auto pins=impl_->pin(request);if(impl_->channel.authorized(request))return true;impl_->unavailable="Packet transfer authority changed. Return to the menu and try again.";}catch(const std::exception& e){const std::string_view why=e.what();impl_->observed(why.size()<=64&&why.substr(0,7)=="packet_"?why:"packet_preflight_failed",GetLastError(),0,0);impl_->unavailable=why=="packet_adjacent_ini"?"Packet transfer blocked by an unexpected helper INI file. Ask the Sysop to check Runtime logs.":why=="packet_helper_identity"?"Packet transfer helper failed verification. Ask the Sysop to run normal Setup.":"Packet transfer preflight failed. Ask the Sysop to check Runtime logs and installation access.";}catch(...){impl_->unavailable="Packet transfer preflight failed. Ask the Sysop to check Runtime logs.";}return false;}
std::string AlphaQwkPacketTransfer::unavailable_reason()const{return impl_->unavailable;}
app::QwkReceiveResult AlphaQwkPacketTransfer::receive(const app::QwkTransferRequest& request){auto result=impl_->run(request,Direction::receive,{});return {result.code,result.refusal,std::move(result.source),std::move(result.cancellation)};}
app::QwkSendResult AlphaQwkPacketTransfer::send(const app::QwkTransferRequest& request,std::string_view zip){auto result=impl_->run(request,Direction::send,zip);return {result.code,result.refusal,std::move(result.confirm_completed)};}
} // namespace sfx::apps::runtime
