#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/file_receive_budget.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
namespace native=sfx::platform::win32;
namespace ins=native::installation;
namespace {
unsigned checks{},processes{};
constexpr std::uint64_t room=1024*1024,maximum=700*1024;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void refused(F action,const char* why){bool result{};try{action();}catch(const std::exception&){result=true;}check(result,why);}
native::FilePayloadDirectory directory(const std::filesystem::path& root){return native::FilePayloadDirectory::private_store(root,ins::operator_sid(),[]{return native::kFilePayloadFreeReserve+room;});}
bool space_refused(native::FilePayloadDirectory& selected,const char* name){
    try{auto second=selected.create(name,maximum);return false;}
    catch(const std::exception& e){const std::string code=e.what();if(code!="file_free_space_reserve"&&code!="file_budget_insufficient_space")throw;return true;}
}
std::wstring text(HANDLE h){return std::to_wstring(reinterpret_cast<std::uintptr_t>(h));}
HANDLE handle(const wchar_t* value){return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(value)));}
ins::Handle event(){SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};ins::Handle value(CreateEventW(&attributes,TRUE,FALSE,nullptr));check(static_cast<bool>(value),"owned test event");return value;}
struct Child {
    ins::Handle process,thread;HANDLE stop{};bool resumed{};
    Child(std::wstring args,const std::vector<HANDLE>& allowed,HANDLE stop_event):stop(stop_event){
        std::array<wchar_t,32768> exe{};const auto n=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));check(n&&n<exe.size(),"own test image");
        std::wstring command=L"\""+std::wstring(exe.data())+L"\" "+args;SIZE_T bytes{};InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> buffer(bytes);auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        check(InitializeProcThreadAttributeList(list,1,0,&bytes)!=FALSE,"test attribute list");struct Delete{LPPROC_THREAD_ATTRIBUTE_LIST p;~Delete(){DeleteProcThreadAttributeList(p);}} close{list};
        check(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE*>(allowed.data()),allowed.size()*sizeof(HANDLE),nullptr,nullptr)!=FALSE,"test exact inheritance");
        STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.lpAttributeList=list;PROCESS_INFORMATION child{};
        check(CreateProcessW(exe.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&start.StartupInfo,&child)!=FALSE,"owned native child");process.reset(child.hProcess);thread.reset(child.hThread);++processes;
    }
    void resume(){check(ResumeThread(thread.get())!=static_cast<DWORD>(-1),"child resume");resumed=true;}
    void success(){check(WaitForSingleObject(process.get(),10000)==WAIT_OBJECT_0,"child normal exit");DWORD code{};check(GetExitCodeProcess(process.get(),&code)&&code==0,"child successful result");}
    ~Child(){SetEvent(stop);if(!resumed&&thread)ResumeThread(thread.get());if(process)WaitForSingleObject(process.get(),10000);}
};
int keeper(int argc,wchar_t** argv){
    if(argc!=6)return 20;const auto ready=handle(argv[2]),stop=handle(argv[3]),mapping=handle(argv[4]),file=handle(argv[5]);
    auto* read=MapViewOfFile(mapping,FILE_MAP_READ,0,0,4096);if(!read)return 21;UnmapViewOfFile(read);
    auto* write=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,4096);if(write){UnmapViewOfFile(write);return 22;}
    DWORD done{},pid=GetCurrentProcessId();if(!WriteFile(file,&pid,sizeof(pid),&done,nullptr)||done!=sizeof(pid))return 23;
    if(!SetEvent(ready)||WaitForSingleObject(stop,30000)!=WAIT_OBJECT_0)return 24;return 0;
}
int crash_owner(int argc,wchar_t** argv){
    if(argc!=5)return 30;auto selected=directory(argv[2]);auto payload=selected.create("crash-child.tmp",maximum);auto writer=payload.duplicate_transfer_handle(true);
    auto budget=native::FileReceiveBudget::for_transfer(writer.get(),ins::operator_sid(),maximum);auto mapping=budget->inherit_keeper();const auto ready=handle(argv[3]),stop=handle(argv[4]);
    Child child(L"--keeper "+text(ready)+L" "+text(stop)+L" "+text(mapping.get())+L" "+text(writer.get()),{ready,stop,mapping.get(),writer.get()},stop);
    budget->bind_child(child.process.get());child.resume();if(WaitForSingleObject(ready,10000)!=WAIT_OBJECT_0)return 31;
    // Abrupt exit of THIS diagnostic only; no product/Runtime/other-process kill.
    ExitProcess(0);
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc>=2&&std::wstring_view(argv[1])==L"--keeper")return keeper(argc,argv);
    if(argc>=2&&std::wstring_view(argv[1])==L"--crash-owner")return crash_owner(argc,argv);
    if(argc==3&&std::wstring_view(argv[1])==L"--refuse"){auto selected=directory(argv[2]);return space_refused(selected,"other-process.tmp")?0:40;}
    check(argc==2,"fresh fixture required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"retain old fixtures");const auto owner=ins::operator_sid();const auto security=ins::mutable_descriptor(owner);ins::create_directory(root,security.get());
    auto selected=directory(root);check(selected.available_bytes()==native::kFilePayloadFreeReserve+room,"fault ceiling only reduces physical free bytes");
    {
        auto first=selected.create("first.tmp",maximum);auto inherited=first.duplicate_transfer_handle(true);
        check(space_refused(selected,"second.tmp")&&selected.absent("second.tmp"),"aggregate refusal creates no file");
        check(selected.available_bytes()==native::kFilePayloadFreeReserve+room-maximum,"free room subtracts whole maximum");refused([&]{selected.require_space(maximum);},"preflight includes pending reservations");
        auto duplicate=native::FileReceiveBudget::for_transfer(inherited.get(),owner,maximum);check(selected.available_bytes()==native::kFilePayloadFreeReserve+room-maximum,"helper reuses allowance without double charge");
        refused([&]{native::FileReceiveBudget::for_transfer(inherited.get(),owner,maximum-1);},"changed helper maximum refused");
        auto stop=event();Child other(L"--refuse \""+root.native()+L"\"",{stop.get()},stop.get());other.resume();other.success();check(selected.absent("other-process.tmp"),"independent process observes shared allowance");
        DWORD n{};check(WriteFile(inherited.get(),"tiny",4,&n,nullptr)&&n==4,"tiny actual raw write");inherited.reset();first.identity();check(selected.available_bytes()==native::kFilePayloadFreeReserve+room,"seal releases allowance with original lease retained");
        auto second=selected.create("after-seal.tmp",maximum);second.identity();
    }
    check(selected.available_bytes()==native::kFilePayloadFreeReserve+room,"normal destruction has no budget leak");
    {
        std::uint64_t ceiling=native::kFilePayloadFreeReserve+room;auto bounded=native::FilePayloadDirectory::private_store(root,owner,[&]{return ceiling;});auto first=bounded.create("launch-room.tmp",maximum);
        ceiling=native::kFilePayloadFreeReserve+maximum-1;refused([&]{first.duplicate_transfer_handle(true);},"lost free room is checked again before exporting write capability");ceiling=native::kFilePayloadFreeReserve+room;first.identity();
        ins::create_file(root/L"trusted-raw.tmp",{},security.get());ins::Handle raw(CreateFileW((root/L"trusted-raw.tmp").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));check(raw&&raw.get()!=INVALID_HANDLE_VALUE,"owned raw-host empty file");
        auto budget=native::FileReceiveBudget::for_transfer(raw.get(),owner,maximum);check(selected.available_bytes()==native::kFilePayloadFreeReserve+room-maximum,"raw trusted host also reserves whole allowance");budget->seal();check(selected.available_bytes()==native::kFilePayloadFreeReserve+room,"raw-host seal returns allowance");
    }
    {
        auto first=std::make_unique<native::FilePayloadLease>(selected.create("child.tmp",maximum));auto writer=first->duplicate_transfer_handle(true);auto budget=native::FileReceiveBudget::for_transfer(writer.get(),owner,maximum);auto mapping=budget->inherit_keeper();auto ready=event(),stop=event();
        Child child(L"--keeper "+text(ready.get())+L" "+text(stop.get())+L" "+text(mapping.get())+L" "+text(writer.get()),{ready.get(),stop.get(),mapping.get(),writer.get()},stop.get());
        budget->bind_child(child.process.get());refused([&]{first->identity();},"live suspended child blocks premature seal");child.resume();check(WaitForSingleObject(ready.get(),10000)==WAIT_OBJECT_0,"child verifies read-only keeper and tiny write");
        writer.reset();first.reset();budget.reset();mapping.reset();check(space_refused(selected,"child-still-live.tmp"),"released lease retains live child cap");check(SetEvent(stop.get())!=FALSE,"normal child stop");child.success();
        auto after=selected.create("child-reaped.tmp",maximum);after.identity();check(selected.absent("child-still-live.tmp"),"positive child exit reaps allowance");
    }
    {
        auto ready=event(),stop=event();Child owner_process(L"--crash-owner \""+root.native()+L"\" "+text(ready.get())+L" "+text(stop.get()),{ready.get(),stop.get()},stop.get());owner_process.resume();check(WaitForSingleObject(ready.get(),10000)==WAIT_OBJECT_0,"crash-test writer ready");owner_process.success();
        ins::Handle input(CreateFileW((root/L"crash-child.tmp").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));DWORD pid{},n{};check(input&&ReadFile(input.get(),&pid,sizeof(pid),&n,nullptr)&&n==sizeof(pid)&&pid!=GetCurrentProcessId(),"owned child PID read from tiny fixture");input.reset();ins::Handle writer_process(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        check(writer_process&&WaitForSingleObject(writer_process.get(),0)==WAIT_TIMEOUT,"child alive after abrupt owner exit");check(space_refused(selected,"crashed-owner-still-live.tmp"),"read-only keeper preserves cap after parent death");
        check(SetEvent(stop.get())!=FALSE,"orphan diagnostic normal stop");check(WaitForSingleObject(writer_process.get(),10000)==WAIT_OBJECT_0,"orphan child exit confirmed");DWORD code{};check(GetExitCodeProcess(writer_process.get(),&code)&&code==0,"orphan child keeper/write checks");
        auto after=selected.create("after-crash-reap.tmp",maximum);after.identity();check(selected.absent("crashed-owner-still-live.tmp"),"dead parent and child reaped on admission");
    }
    check(selected.available_bytes()==native::kFilePayloadFreeReserve+room,"all allowances returned");std::cout<<"PASS receive budget lifetime "<<checks<<" checks; "<<processes+1<<" owned subprocesses incl grandchild; tiny files/fault ceiling, NOT real disk-full/Runtime/client tests\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL receive budget lifetime after "<<checks<<": "<<e.what()<<" win32="<<GetLastError()<<'\n';return 1;}}
