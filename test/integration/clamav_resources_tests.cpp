#include "sfx/platform/win32/clamav_resources.hpp"
#include "sfx/platform/win32/clamav_status_gate.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <algorithm>
#include <array>
#include <iostream>
namespace native=sfx::platform::win32;namespace ins=native::installation;
namespace {
unsigned checks{},processes{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void refused(F action,const char* why){bool failed{};try{action();}catch(...){failed=true;}check(failed,why);}
std::wstring text(HANDLE h){return std::to_wstring(reinterpret_cast<std::uintptr_t>(h));}
HANDLE handle(const wchar_t* value){return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(value)));}
ins::Handle event(){SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE};ins::Handle result(CreateEventW(&attributes,TRUE,FALSE,nullptr));check(bool(result),"owned event");return result;}
void write(const std::filesystem::path& path,const std::wstring& owner,const char* name,const std::string& value){auto directory=native::FilePayloadDirectory::private_store(path,owner);auto file=directory.create(name,std::max<std::uint64_t>(1,value.size()));if(!value.empty())file.append(reinterpret_cast<const std::uint8_t*>(value.data()),value.size());file.identity();}
struct Child {
    ins::Handle process,thread;HANDLE stop{};bool resumed{};
    Child(const std::wstring& args,const std::vector<HANDLE>& allowed,HANDLE stop_event):stop(stop_event){
        std::array<wchar_t,32768> exe{};const auto n=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));check(n&&n<exe.size(),"own diagnostic image");
        std::wstring command=L"\""+std::wstring(exe.data())+L"\" "+args;SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<std::uint8_t> buffer(size);auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        check(InitializeProcThreadAttributeList(list,1,0,&size)!=FALSE,"native child attribute list");struct Delete {LPPROC_THREAD_ATTRIBUTE_LIST value;~Delete(){DeleteProcThreadAttributeList(value);}} cleanup{list};
        check(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE*>(allowed.data()),allowed.size()*sizeof(HANDLE),nullptr,nullptr)!=FALSE,"exact four-handle inheritance");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;PROCESS_INFORMATION child{};
        check(CreateProcessW(exe.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&child)!=FALSE,"owned suspended diagnostic child");process.reset(child.hProcess);thread.reset(child.hThread);++processes;
    }
    void resume(){check(ResumeThread(thread.get())!=static_cast<DWORD>(-1),"diagnostic resume");resumed=true;}
    void finish(){check(SetEvent(stop)!=FALSE&&WaitForSingleObject(process.get(),10000)==WAIT_OBJECT_0,"child normally exits after own stop event");DWORD code{};check(GetExitCodeProcess(process.get(),&code)&&code==0,"actual read-only keeper assertions pass");}
    ~Child(){SetEvent(stop);if(!resumed&&thread)ResumeThread(thread.get());if(process)WaitForSingleObject(process.get(),10000);}
};
int keeper(int argc,wchar_t** argv){
    if(argc!=6)return 20;const auto ready=handle(argv[2]),stop=handle(argv[3]),lock=handle(argv[4]),mapping=handle(argv[5]);
    char byte{};DWORD bytes{};if(!ReadFile(lock,&byte,1,&bytes,nullptr)||bytes!=0)return 21;
    if(WriteFile(lock,"x",1,&bytes,nullptr))return 22;
    auto* read=MapViewOfFile(mapping,FILE_MAP_READ,0,0,4096);if(!read)return 23;UnmapViewOfFile(read);
    auto* write_view=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,4096);if(write_view){UnmapViewOfFile(write_view);return 24;}
    if(!SetEvent(ready)||WaitForSingleObject(stop,30000)!=WAIT_OBJECT_0)return 25;return 0;
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc>=2&&std::wstring_view(argv[1])==L"--keeper")return keeper(argc,argv);
    check(argc==2,"fresh owned fixture required");const auto root=ins::canonical_root(std::filesystem::absolute(argv[1]));
    check(root.parent_path()==std::filesystem::path(L"C:\\sfxtst\\runs")&&root.filename().native().rfind(L"section9-clamav-resources-r",0)==0&&!std::filesystem::exists(root),"bounded new root only");
    const auto owner=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(owner);const auto parent=root/L"Work"/L"ClamAV";
    for(const auto& path:{root,root/L"Work",parent,root/L"external"})ins::create_directory(path,descriptor.get());
    auto job=native::ClamAvScratch::create(parent,owner);write(job->path(),owner,"tiny.tmp","scratch only");
    {
        auto scratch=native::FilePayloadDirectory::private_store(job->path(),owner);native::ClamAvResourceCeilings ceilings;ceilings.free_memory=0;
        constexpr std::uint64_t mib=1024ULL*1024;
        for(const auto kind:{native::ClamAvResourceKind::scan,native::ClamAvResourceKind::definition_update}){
            const auto physical=(kind==native::ClamAvResourceKind::scan?1024ULL:768ULL)*mib;
            const auto commit=(kind==native::ClamAvResourceKind::scan?1536ULL:1024ULL)*mib;
            ceilings.free_memory=physical;ceilings.free_commit=commit;
            {auto admitted=native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceilings,kind);
                check(admitted->child_memory_limit()==commit-256*mib,"minimum measured profile leaves256MiB commit outside child allowance");
                check(admitted->child_memory_limit()<=2147418112ULL,"measured profile never raises old process cap");}
            ceilings.free_memory=physical-1;
            refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceilings,kind);},"one byte below physical floor refused for either work kind");
            ceilings.free_memory=physical;ceilings.free_commit=commit-1;
            refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceilings,kind);},"one byte below commit floor refused for either work kind");
            ceilings.free_memory=1331*mib;ceilings.free_commit=1740*mib;
            {auto admitted=native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceilings,kind);
                check(admitted->child_memory_limit()==1484*mib,"Win10-like reduced snapshot admits but child cap leaves reserve");}
        }
        refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,{},static_cast<native::ClamAvResourceKind>(99));},"unknown resource work kind refused");
        ceilings={};ceilings.free_memory=0;
        refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceilings);},"memory ceiling cannot bypass actual headroom");ceilings.free_memory.reset();ceilings.free_commit=0;
        refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceilings);},"commit ceiling cannot bypass headroom");ceilings.free_commit.reset();ceilings.free_disk=0;
        refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceilings);},"insufficient reserved disk refuses without allocation");
        auto first=native::ClamAvResourceLease::acquire(scratch,owner,1024);
        const auto work=native::FilePayloadDirectory::private_store(parent,owner);
        refused([&]{native::clamav_status_gate(work,owner,{},25);},"status read cannot overlap active scan/update publication");
        refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25);},"only one scan or update admitted per owner");
        refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,[]{return true;});},"waiting caller cancellation refuses promptly");
        check(!job->cleanup()&&std::filesystem::exists(job->path()/L"tiny.tmp"),"active admission prevents ANY scratch deletion");first.reset();
        auto status=native::clamav_status_gate(work,owner);check(bool(status),"short status read acquires the same exact operation gate without resource reservation");
        refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25);},"scan/update waits until exact status hashes release their pins");status.reset();
        refused([&]{native::clamav_status_gate(work,owner,[]{return true;});},"status wait cancellation is prompt and does not create a vendor child");
        check(!job->cleanup(),"live directory capability also prevents deletion before host releases it");
    }
    check(job->cleanup()&&!std::filesystem::exists(job->path())&&job->cleanup(),"exact owned scratch removal succeeds after release and is idempotent");
    {
        auto work=native::FilePayloadDirectory::private_store(parent,owner);const auto before=work.names();
        auto gate=native::clamav_status_gate(work,owner);
        refused([&]{native::ClamAvResourceLease::acquire_update(work,owner,1024,[]{return true;},{},25);},"queued update cancellation creates no scratch");
        refused([&]{native::ClamAvResourceLease::acquire_update(work,owner,1024,{}, {},25);},"queued update wait is bounded before scratch");
        check(work.names()==before,"blocked update leaves parent contents exact");gate.reset();
        auto admitted=native::ClamAvResourceLease::acquire_update(work,owner,1024);
        auto owned=native::ClamAvScratch::create(parent,owner,true);write(owned->path(),owner,"tiny.tmp","exact own bytes");
        check(owned->cleanup(*admitted),"exact scratch cleanup under same live admission");
        refused([&]{native::ClamAvResourceLease::acquire_update(work,owner,1024,{}, {},25);},"cleanup keeps admission closed until caller releases it");
        auto foreign=native::ClamAvScratch::create(root/L"external",owner,true);write(foreign->path(),owner,"foreign.tmp","keep foreign scratch");
        check(!foreign->cleanup(*admitted)&&std::filesystem::exists(foreign->path()/L"foreign.tmp"),"wrong-parent admission cannot authorize scratch deletion");
        check(foreign->cleanup(),"own foreign scratch retains ordinary verified cleanup route");
        auto unknown=native::ClamAvScratch::create(parent,owner,true);auto keeper=admitted->inherit_lock();
        check(!unknown->cleanup(*admitted),"issued child keeper without bound process is unknown, never cleanup proof");
        keeper.reset();admitted.reset();check(unknown->cleanup(),"normal keeper and parent close permit exclusive cleanup");
        refused([&]{const auto other=native::FilePayloadDirectory::private_store(root/L"external",owner);native::ClamAvResourceLease::acquire_update(other,owner,1024);},"update parent must be exact private Work/ClamAV");
    }
    {
        auto nested=native::ClamAvScratch::create(parent,owner);ins::create_directory(nested->path()/L"nested",descriptor.get());write(nested->path(),owner,"outer.tmp","outer");write(nested->path()/L"nested",owner,"inner.tmp","inner");
        check(nested->cleanup()&&!std::filesystem::exists(nested->path()),"all verified regular children removed before exact directories");
    }
    {
        auto vendor=native::ClamAvScratch::create(parent,owner,true);const auto nested=vendor->path()/L"tmp.vendor";
        check(CreateDirectoryW(nested.c_str(),nullptr)!=FALSE,"vendor-style inherited scratch directory created");
        native::WindowsStorageDaclPolicy policy(owner);
        check(!policy.assess(nested,native::StorageDaclObjectKind::directory).status&&policy.assess(nested,native::StorageDaclObjectKind::directory,true).status.succeeded(),"inherited private directory requires explicit host-only assessment");
        {ins::Handle file(CreateFileW((nested/L"partial.cvd").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD bytes{};check(file.get()!=INVALID_HANDLE_VALUE&&WriteFile(file.get(),"partial",7,&bytes,nullptr)&&bytes==7,"vendor-style inherited scratch file created");}
        check(vendor->cleanup()&&!std::filesystem::exists(vendor->path()),"exact job cleanup accepts only matching private inherited vendor children");
    }
    {
        auto linked=native::ClamAvScratch::create(parent,owner);write(linked->path(),owner,"aaa.tmp","keep if inventory fails");write(root/L"external",owner,"original.tmp","never delete external object");
        check(CreateHardLinkW((linked->path()/L"linked.tmp").c_str(),(root/L"external"/L"original.tmp").c_str(),nullptr)!=FALSE,"deliberate fixture hardlink");
        check(!linked->cleanup()&&std::filesystem::exists(linked->path()/L"aaa.tmp")&&std::filesystem::exists(linked->path()/L"linked.tmp")&&std::filesystem::exists(root/L"external"/L"original.tmp"),"hardlink fails whole pre-delete inventory and preserves ALL bytes");
    }
    {
        auto replaced=native::ClamAvScratch::create(parent,owner);write(replaced->path(),owner,"original.tmp","original job remains");const auto old=parent/L"retained-original-job";std::filesystem::rename(replaced->path(),old);ins::create_directory(replaced->path(),descriptor.get());write(replaced->path(),owner,"foreign.tmp","foreign job remains");
        check(!replaced->cleanup()&&std::filesystem::exists(old/L"original.tmp")&&std::filesystem::exists(replaced->path()/L"foreign.tmp"),"replaced job native ID refused before inventory or deletion");
    }
    {
        auto locked=native::ClamAvScratch::create(parent,owner);write(locked->path(),owner,"aaa.tmp","all retained on lock");write(locked->path(),owner,"zzz.tmp","locked remainder");
        ins::Handle pin(CreateFileW((locked->path()/L"zzz.tmp").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));check(pin.get()!=INVALID_HANDLE_VALUE,"lock test-only scratch file");
        check(!locked->cleanup()&&std::filesystem::exists(locked->path()/L"aaa.tmp"),"locked late child fails preflight before any earlier deletion");pin.reset();check(locked->cleanup(),"normal close permits later exact cleanup without force/security changes");
    }
    {
        auto inherited=native::ClamAvScratch::create(parent,owner);auto ready=event(),stop=event();
        ins::Handle lock_keeper,budget_keeper;
        {
            auto scratch=native::FilePayloadDirectory::private_store(inherited->path(),owner);auto lease=native::ClamAvResourceLease::acquire(scratch,owner,1024);lock_keeper=lease->inherit_lock();budget_keeper=lease->inherit_budget();
            Child child(L"--keeper "+text(ready.get())+L" "+text(stop.get())+L" "+text(lock_keeper.get())+L" "+text(budget_keeper.get()),{ready.get(),stop.get(),lock_keeper.get(),budget_keeper.get()},stop.get());
            lease->bind_child(child.process.get());child.resume();check(WaitForSingleObject(ready.get(),10000)==WAIT_OBJECT_0,"actual child holds reduced lock and ledger capabilities");lock_keeper.reset();budget_keeper.reset();lease.reset();
            refused([&]{native::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25);},"parent lease release NEVER admits while keeper child remains alive");
            check(!inherited->cleanup(),"parent lease release NEVER authorizes cleanup while child remains alive");child.finish();
        }
        check(inherited->cleanup(),"actual child exit permits cleanup, not an asserted success bit");
    }
    {
        const auto work=native::FilePayloadDirectory::private_store(parent,owner);auto lease=native::ClamAvResourceLease::acquire_update(work,owner,1024);
        auto owned=native::ClamAvScratch::create(parent,owner,true);write(owned->path(),owner,"tiny.tmp","keep until child exits");
        auto ready=event(),stop=event();auto lock_keeper=lease->inherit_lock(),budget_keeper=lease->inherit_budget();
        Child child(L"--keeper "+text(ready.get())+L" "+text(stop.get())+L" "+text(lock_keeper.get())+L" "+text(budget_keeper.get()),{ready.get(),stop.get(),lock_keeper.get(),budget_keeper.get()},stop.get());
        lease->bind_child(child.process.get());child.resume();check(WaitForSingleObject(ready.get(),10000)==WAIT_OBJECT_0,"actual update keeper child active");lock_keeper.reset();budget_keeper.reset();
        check(!owned->cleanup(*lease)&&std::filesystem::exists(owned->path()/L"tiny.tmp"),"live bound process prevents cleanup even with owning admission");
        child.finish();check(owned->cleanup(*lease),"pinned actual child exit allows exact cleanup while gate stays held");
        refused([&]{native::ClamAvResourceLease::acquire_update(work,owner,1024,{}, {},25);},"queued update cannot take gate between publication and cleanup");
    }
    std::cout<<"PASS native ClamAV admission/scratch "<<checks<<" checks; "<<processes<<" diagnostic child normally reaped; no actual scanner/update, Setup, installed or GUI claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native ClamAV admission after "<<checks<<": "<<e.what()<<" win32="<<GetLastError()<<'\n';return 1;}}
