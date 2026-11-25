#include "sfx/platform/win32/file_receive_budget.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>

namespace sfx::platform::win32 {
namespace {
namespace ins=installation;
using ins::require;
constexpr DWORD slot_count=32768;
constexpr std::uint64_t signature=0x3154474442465853ULL;
// No pointers, native HANDLEs or architecture-sized fields in shared memory.
struct Slot {
    DWORD active{},parent_pid{};
    std::uint64_t parent_birth{};
    DWORD child_pid{},released{};
    std::uint64_t child_birth{},object_id{},maximum{},token_a{},token_b{};
};
static_assert(sizeof(Slot)==64);
struct Ledger {
    std::uint64_t magic{},total{};
    DWORD version{},slots{},dirty{},reserved{};
    Slot entries[slot_count];
};
std::uint64_t birth(HANDLE process){
    FILETIME created{},exited{},kernel{},user{};
    require(GetProcessTimes(process,&created,&exited,&kernel,&user)!=FALSE,"file_budget_process_identity");
    return (static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
}
// Only a positive exit/nonexistence or a different creation time proves death.
// Access denied/unknown must never free a possibly live helper's allowance.
bool dead(DWORD pid,std::uint64_t expected){
    if(!pid)return true;
    ins::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));
    if(!process)return GetLastError()==ERROR_INVALID_PARAMETER;
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(process.get(),&created,&exited,&kernel,&user))return false;
    if(((static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime)!=expected)return true;
    return WaitForSingleObject(process.get(),0)==WAIT_OBJECT_0;
}
ins::Descriptor security(const std::wstring& owner){
    require(service_file_owner_enabled(owner)||owner==ins::operator_sid(),"file_budget_owner_mismatch");
    const auto text=L"O:"+owner+L"D:P(A;;GA;;;"+owner+L")(A;;GA;;;SY)(A;;GA;;;BA)";
    PSECURITY_DESCRIPTOR raw{};
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),SDDL_REVISION_1,&raw,nullptr)!=FALSE,"file_budget_security_create");
    return ins::Descriptor(raw);
}
void verify_security(HANDLE object,const std::wstring& expected,DWORD full){
    PSID owner{};PACL dacl{};PSECURITY_DESCRIPTOR raw{};
    require(GetSecurityInfo(object,SE_KERNEL_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,&raw)==ERROR_SUCCESS,"file_budget_security_query");
    ins::Descriptor descriptor(raw);PSID own_raw{};
    require(ConvertStringSidToSidW(expected.c_str(),&own_raw)!=FALSE,"file_budget_owner_sid");ins::Descriptor own(own_raw);
    std::array<unsigned char,SECURITY_MAX_SID_SIZE> system{},admin{};DWORD system_size=static_cast<DWORD>(system.size()),admin_size=static_cast<DWORD>(admin.size());
    require(CreateWellKnownSid(WinLocalSystemSid,nullptr,system.data(),&system_size)&&CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admin.data(),&admin_size),"file_budget_trusted_sids");
    require(owner&&EqualSid(owner,own.get())&&dacl&&dacl->AceCount==3,"file_budget_owner_or_dacl");
    std::array<bool,3> found{};
    for(DWORD n=0;n<dacl->AceCount;++n){void* raw_ace{};require(GetAce(dacl,n,&raw_ace)!=FALSE,"file_budget_ace_query");const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
        require(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE&&ace->Header.AceFlags==0&&(ace->Mask==GENERIC_ALL||ace->Mask==full),"file_budget_ace_refused");
        const auto sid=const_cast<DWORD*>(&ace->SidStart);std::size_t which=3;
        if(EqualSid(sid,own.get()))which=0;else if(EqualSid(sid,system.data()))which=1;else if(EqualSid(sid,admin.data()))which=2;
        require(which<found.size()&&!found[which],"file_budget_unknown_or_duplicate_principal");found[which]=true;
    }
}
std::wstring volume_key(HANDLE object,const std::wstring& owner){
    std::array<wchar_t,32768> text{};
    const auto n=GetFinalPathNameByHandleW(object,text.data(),static_cast<DWORD>(text.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_GUID);
    require(n>0&&n<text.size(),"file_budget_volume_identity");std::wstring path(text.data(),n);
    require(path.rfind(L"\\\\?\\Volume{",0)==0,"file_budget_local_volume_required");const auto end=path.find(L'}');
    require(end!=std::wstring::npos&&end+1<path.size()&&path[end+1]==L'\\',"file_budget_volume_guid");
    path.resize(end+1);std::transform(path.begin(),path.end(),path.begin(),[](wchar_t c){return static_cast<wchar_t>(towupper(c));});
    path+=L":"+owner;const auto* bytes=reinterpret_cast<const std::uint8_t*>(path.data());
    const auto hash=ins::sha256({bytes,bytes+path.size()*sizeof(wchar_t)});
    return L"Local\\SFX-FileReceiveBudget-v1-"+std::wstring(hash.begin(),hash.end());
}
struct Budget {
    ins::Handle mutex,mapping;std::wstring volume_identity,owner;Ledger* ledger{};
    ~Budget(){if(ledger)UnmapViewOfFile(ledger);}
    void validate()const{require(ledger&&ledger->magic==signature&&ledger->version==1&&ledger->slots==slot_count&&!ledger->reserved,"file_budget_header_refused");}
    void total(){
        std::uint64_t sum{};
        for(const auto& entry:ledger->entries){require(entry.active<=1,"file_budget_slot_corrupt");if(!entry.active)continue;
            require(entry.parent_pid&&entry.parent_birth&&entry.maximum&&entry.maximum<=INT64_MAX&&(entry.token_a||entry.token_b)&&entry.released<=1&&(!entry.child_pid||entry.child_birth),"file_budget_entry_corrupt");
            require(entry.maximum<=UINT64_MAX-sum,"file_budget_sum_overflow");sum+=entry.maximum;
        }
        ledger->total=sum;ledger->dirty=0;
    }
    void reap(){
        std::map<std::pair<DWORD,std::uint64_t>,bool> cache;
        const auto gone=[&](DWORD pid,std::uint64_t time){const auto key=std::make_pair(pid,time);const auto old=cache.find(key);if(old!=cache.end())return old->second;const bool value=dead(pid,time);cache.emplace(key,value);return value;};
        ledger->dirty=1;
        for(auto& entry:ledger->entries)if(entry.active&&(entry.released||gone(entry.parent_pid,entry.parent_birth))&&(!entry.child_pid||gone(entry.child_pid,entry.child_birth)))entry.active=0;
        total();
    }
};
struct Lock {
    Budget& budget;
    explicit Lock(Budget& value):budget(value){
        const auto wait=WaitForSingleObject(budget.mutex.get(),5000);
        require(wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED,"file_budget_mutex_unavailable");
        try{budget.validate();if(wait==WAIT_ABANDONED||budget.ledger->dirty)budget.total();}catch(...){ReleaseMutex(budget.mutex.get());throw;}
    }
    ~Lock(){ReleaseMutex(budget.mutex.get());}
};
std::mutex cache_mutex;
std::map<std::wstring,std::weak_ptr<Budget>> budgets;
std::map<std::pair<std::wstring,std::uint64_t>,std::weak_ptr<FileReceiveBudget>> files;
std::shared_ptr<Budget> open_budget(HANDLE object,const std::wstring& owner){
    const auto name=volume_key(object,owner);
    {std::lock_guard<std::mutex> cache(cache_mutex);const auto old=budgets.find(name);if(old!=budgets.end())if(auto live=old->second.lock())return live;}
    auto result=std::make_shared<Budget>();result->volume_identity=name;result->owner=owner;const auto sd=security(owner);SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),sd.get(),FALSE};
    ServiceFileOwnerScope scope(owner);require(scope.valid(),"file_budget_owner_scope_failed");
    result->mutex.reset(CreateMutexW(&attributes,FALSE,(name+L"-lock").c_str()));require(static_cast<bool>(result->mutex),"file_budget_mutex_create");verify_security(result->mutex.get(),owner,MUTEX_ALL_ACCESS);
    const auto wait=WaitForSingleObject(result->mutex.get(),5000);require(wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED,"file_budget_initial_lock");
    struct Release{HANDLE h;~Release(){ReleaseMutex(h);}} release{result->mutex.get()};
    SetLastError(ERROR_SUCCESS);result->mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE,&attributes,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(Ledger)),(name+L"-map").c_str()));const auto error=GetLastError();require(static_cast<bool>(result->mapping),"file_budget_mapping_create");
    verify_security(result->mapping.get(),owner,FILE_MAP_ALL_ACCESS);
    result->ledger=static_cast<Ledger*>(MapViewOfFile(result->mapping.get(),FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Ledger)));require(result->ledger,"file_budget_mapping_view");
    if(error!=ERROR_ALREADY_EXISTS){std::memset(result->ledger,0,sizeof(Ledger));result->ledger->version=1;result->ledger->slots=slot_count;result->ledger->magic=signature;}
    result->validate();if(wait==WAIT_ABANDONED||result->ledger->dirty)result->total();
    {std::lock_guard<std::mutex> cache(cache_mutex);for(auto it=budgets.begin();it!=budgets.end();)if(it->second.expired())it=budgets.erase(it);else ++it;budgets[name]=result;}return result;
}
std::uint64_t usable(std::uint64_t physical,std::uint64_t reserved){return physical>=reserved?physical-reserved:0;}
void require_room(std::uint64_t physical,std::uint64_t reserved,std::uint64_t incoming){
    const auto free=usable(physical,reserved);require(free>=kFilePayloadFreeReserve&&incoming<=free-kFilePayloadFreeReserve,"file_budget_insufficient_space");
}
}
struct FileReceiveBudget::Impl {
    std::shared_ptr<Budget> budget;DWORD index=slot_count;std::uint64_t a{},b{},maximum{};
    Slot& slot()const{require(index<slot_count,"file_budget_missing_slot");auto& value=budget->ledger->entries[index];require(value.active&&value.token_a==a&&value.token_b==b&&value.parent_pid==GetCurrentProcessId(),"file_budget_slot_identity");return value;}
};
FileReceiveBudget::FileReceiveBudget():impl_(std::make_unique<Impl>()){}
std::shared_ptr<FileReceiveBudget> FileReceiveBudget::reserve(HANDLE directory,const std::wstring& owner,std::uint64_t physical,std::uint64_t maximum){
    require(maximum>0&&maximum<=INT64_MAX,"file_budget_limit_invalid");auto result=std::shared_ptr<FileReceiveBudget>(new FileReceiveBudget);result->impl_->budget=open_budget(directory,owner);
    auto& budget=*result->impl_->budget;Lock lock(budget);budget.reap();require_room(physical,budget.ledger->total,maximum);
    DWORD index{};while(index<slot_count&&budget.ledger->entries[index].active)++index;require(index<slot_count,"file_budget_slots_exhausted");
    Slot entry{};entry.parent_pid=GetCurrentProcessId();entry.parent_birth=birth(GetCurrentProcess());entry.maximum=maximum;
    do{randombytes_buf(&entry.token_a,sizeof(entry.token_a));randombytes_buf(&entry.token_b,sizeof(entry.token_b));}while(!entry.token_a&&!entry.token_b);
    budget.ledger->dirty=1;budget.ledger->entries[index]=entry;budget.ledger->entries[index].active=1;
    result->impl_->index=index;result->impl_->a=entry.token_a;result->impl_->b=entry.token_b;result->impl_->maximum=maximum;
    budget.total();return result;
}
void FileReceiveBudget::bind_file(HANDLE file){
    const auto key=std::make_pair(volume_key(file,impl_->budget->owner),ins::file_id(file));
    {auto& budget=*impl_->budget;Lock lock(budget);auto& value=impl_->slot();require(key.first==budget.volume_identity&&(!value.object_id||value.object_id==key.second),"file_budget_file_changed");value.object_id=key.second;}
    std::lock_guard<std::mutex> cache(cache_mutex);for(auto it=files.begin();it!=files.end();)if(it->second.expired())it=files.erase(it);else ++it;files[key]=shared_from_this();
}
std::shared_ptr<FileReceiveBudget> FileReceiveBudget::for_transfer(HANDLE file,const std::wstring& owner,std::uint64_t maximum){
    require(service_file_owner_enabled(owner)||owner==ins::operator_sid(),"file_budget_owner_mismatch");const auto key=std::make_pair(volume_key(file,owner),ins::file_id(file));std::shared_ptr<FileReceiveBudget> live;
    {std::lock_guard<std::mutex> cache(cache_mutex);const auto found=files.find(key);if(found!=files.end())live=found->second.lock();}
    // Also cover raw, trusted native-host receive handles which do not originate
    // in FilePayloadLease (e.g. the actual helper host qualification fixture).
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER bytes{};
    require(GetFileType(file)==FILE_TYPE_DISK&&GetFileInformationByHandle(file,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&info.nNumberOfLinks==1&&GetFileSizeEx(file,&bytes)&&bytes.QuadPart==0,"file_budget_transfer_not_empty_regular");
    std::array<wchar_t,32768> text{};const auto n=GetFinalPathNameByHandleW(file,text.data(),static_cast<DWORD>(text.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    require(n>0&&n<text.size(),"file_budget_transfer_path");const auto parent=std::filesystem::path(text.data()).parent_path();ULARGE_INTEGER free{};
    require(GetDiskFreeSpaceExW(parent.c_str(),&free,nullptr,nullptr)!=FALSE,"file_budget_transfer_space");
    if(live){auto& budget=*live->impl_->budget;Lock lock(budget);const auto& entry=live->impl_->slot();require(!entry.released&&live->impl_->maximum==maximum&&budget.ledger->total>=entry.maximum,"file_budget_transfer_cap_mismatch");require_room(free.QuadPart,budget.ledger->total-entry.maximum,maximum);return live;}
    auto result=reserve(file,owner,free.QuadPart,maximum);result->bind_file(file);return result;
}
std::uint64_t FileReceiveBudget::available(HANDLE directory,const std::wstring& owner,std::uint64_t physical){
    auto budget=open_budget(directory,owner);Lock lock(*budget);budget->reap();return usable(physical,budget->ledger->total);
}
void FileReceiveBudget::require_write(std::uint64_t physical,std::uint64_t bytes)const{
    auto& budget=*impl_->budget;Lock lock(budget);const auto& value=impl_->slot();require(!value.released,"file_budget_already_released");
    require(budget.ledger->total>=value.maximum,"file_budget_total_corrupt");require_room(physical,budget.ledger->total-value.maximum,bytes);
}
ins::Handle FileReceiveBudget::inherit_keeper()const{
    HANDLE duplicate{};require(DuplicateHandle(GetCurrentProcess(),impl_->budget->mapping.get(),GetCurrentProcess(),&duplicate,FILE_MAP_READ,TRUE,0)!=FALSE,"file_budget_keeper_duplicate");return ins::Handle(duplicate);
}
void FileReceiveBudget::bind_child(HANDLE process){
    const auto pid=GetProcessId(process);const auto time=birth(process);require(pid&&WaitForSingleObject(process,0)==WAIT_TIMEOUT,"file_budget_child_not_live");
    auto& budget=*impl_->budget;Lock lock(budget);auto& value=impl_->slot();require(!value.released&&(!value.child_pid||dead(value.child_pid,value.child_birth)),"file_budget_child_already_bound");
    // Publish PID last. An interruption before this returns leaves the child
    // suspended; the host never resumes before all slots have been bound.
    value.child_birth=time;value.child_pid=pid;
}
void FileReceiveBudget::seal(){
    if(impl_->index==slot_count)return;auto& budget=*impl_->budget;Lock lock(budget);auto& value=impl_->slot();
    require(!value.child_pid||dead(value.child_pid,value.child_birth),"file_budget_child_still_live");budget.ledger->dirty=1;value.active=0;impl_->index=slot_count;budget.total();
}
FileReceiveBudget::~FileReceiveBudget(){
    if(!impl_||impl_->index==slot_count)return;
    try{auto& budget=*impl_->budget;Lock lock(budget);auto& value=impl_->slot();value.released=1;
        if(!value.child_pid||dead(value.child_pid,value.child_birth)){budget.ledger->dirty=1;value.active=0;budget.total();}
    }catch(...){/* Retain uncertain allowances; never turn unknown into free. */}
}
} // namespace sfx::platform::win32
