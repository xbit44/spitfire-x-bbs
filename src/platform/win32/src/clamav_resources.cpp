#include "sfx/platform/win32/clamav_resources.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/clamav_status_gate.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <algorithm>
#include <array>

namespace sfx::platform::win32 {
namespace {
namespace ins=installation;using Path=std::filesystem::path;using ins::require;
constexpr std::uint64_t maximum_scratch=2ULL*1024*1024*1024;
constexpr std::uint64_t mib=1024ULL*1024;
constexpr std::uint64_t commit_reserve=256*mib;
constexpr std::uint64_t maximum_child_memory=2147418112ULL;
constexpr wchar_t lock_name[]=L"SFX-ClamAV.operation.lock";
ins::Handle operation_lock(const Path& parent,const std::wstring& owner,
    std::uint32_t wait,const std::function<bool()>& cancelled,
    const std::function<void(std::uint64_t)>& activity){
    require((service_file_owner_enabled(owner)||owner==ins::operator_sid())&&wait<=180000,"scan_admission_policy");
    const auto descriptor=ins::mutable_descriptor(owner);SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor.get(),FALSE};
    const auto started=GetTickCount64();std::uint64_t last=UINT64_MAX;
    for(;;){
        require(!cancelled||!cancelled(),"scan_admission_cancelled");
        ins::Handle file;
        {ServiceFileOwnerScope scope(owner);require(scope.valid(),"scan_admission_owner_scope");
            file.reset(CreateFileW((parent/lock_name).c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,&attributes,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));}
        if(file.get()!=INVALID_HANDLE_VALUE){
            BY_HANDLE_FILE_INFORMATION identity{};
            require(GetFileType(file.get())==FILE_TYPE_DISK&&GetFileInformationByHandle(file.get(),&identity)&&identity.nNumberOfLinks==1&&
                !(identity.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&!identity.nFileSizeHigh&&!identity.nFileSizeLow,"scan_admission_object");
            require(ins::exact_security(file.get(),descriptor.get()),"scan_admission_security");return file;
        }
        const auto error=GetLastError();require(error==ERROR_SHARING_VIOLATION||error==ERROR_LOCK_VIOLATION,"scan_admission_open");
        const auto elapsed=GetTickCount64()-started;require(elapsed<wait,"scan_admission_busy");
        if(activity&&last!=elapsed/500){last=elapsed/500;activity(elapsed);}
        Sleep(static_cast<DWORD>(std::min<std::uint64_t>(25,wait-elapsed)));
    }
}
std::uint64_t limited(std::uint64_t real,const std::optional<std::uint64_t>& ceiling){return ceiling?std::min(real,*ceiling):real;}
std::uint64_t memory_allowance(ClamAvResourceKind kind,const ClamAvResourceCeilings& ceilings){
    require(kind==ClamAvResourceKind::scan||kind==ClamAvResourceKind::definition_update,"scan_admission_policy");
    // Pinned x86 measurements: FreshClam validated all members at~439MiB
    // peak commit; full-set ClamScan~715MiB. Admission includes margins and
    // the separate commit reserve; real vendor failures still fail closed.
    const bool update=kind==ClamAvResourceKind::definition_update;
    const auto minimum_physical=(update?768ULL:1024ULL)*mib;
    const auto minimum_commit=(update?1024ULL:1536ULL)*mib;
    MEMORYSTATUSEX memory{sizeof(memory)};
    require(GlobalMemoryStatusEx(&memory)!=FALSE,"scan_admission_memory_status");
    require(limited(memory.ullAvailPhys,ceilings.free_memory)>=minimum_physical,"scan_admission_physical_headroom");
    const auto commit=limited(memory.ullAvailPageFile,ceilings.free_commit);
    require(commit>=minimum_commit,"scan_admission_commit_headroom");
    return std::min(maximum_child_memory,commit-commit_reserve);
}
ins::Handle inherited_read(HANDLE original){HANDLE copy{};require(DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&copy,GENERIC_READ,TRUE,0)!=FALSE,"scan_admission_keeper");return ins::Handle(copy);}
struct Entry {ins::Handle handle;Path path;bool directory{};};
void inventory(const Path& path,const std::wstring& owner,DWORD volume,std::vector<Entry>& entries,std::uint64_t& bytes,unsigned depth,std::uint64_t expected_root=0){
    require(depth<=16&&entries.size()<8192,"scan_scratch_inventory_limit");
    auto handle=ins::open_object(path,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE);
    require(!expected_root||ins::file_id(handle.get())==expected_root,"scan_scratch_root_replaced");
    require(ins::volume_serial(handle.get())==volume&&WindowsStorageDaclPolicy(owner).assess(path,StorageDaclObjectKind::directory,depth>0).status.succeeded(),"scan_scratch_directory_security");
    // Keep every actual ancestor pinned against rename while children are read.
    entries.push_back({std::move(handle),path,true});
    WIN32_FIND_DATAW data{};const auto find=FindFirstFileW((path/L"*").c_str(),&data);require(find!=INVALID_HANDLE_VALUE,"scan_scratch_enumeration");
    struct FindCloseGuard {HANDLE value;~FindCloseGuard(){FindClose(value);}} close{find};
    for(;;){
        const std::wstring name=data.cFileName;
        if(name!=L"."&&name!=L".."){
            require(!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&entries.size()<8192,"scan_scratch_redirect_or_limit");
            const auto child=path/name;
            if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)inventory(child,owner,volume,entries,bytes,depth+1);
            else{
                auto file=ins::open_object(child,false,FILE_READ_ATTRIBUTES|READ_CONTROL|DELETE,FILE_SHARE_READ);
                BY_HANDLE_FILE_INFORMATION identity{};require(GetFileInformationByHandle(file.get(),&identity)&&identity.nNumberOfLinks==1&&identity.dwVolumeSerialNumber==volume,"scan_scratch_file_identity");
                const auto size=(static_cast<std::uint64_t>(identity.nFileSizeHigh)<<32)|identity.nFileSizeLow;
                require(size<=maximum_scratch-bytes&&WindowsStorageDaclPolicy(owner).assess(child,StorageDaclObjectKind::file,true).status.succeeded(),"scan_scratch_file_security_or_limit");bytes+=size;
                entries.push_back({std::move(file),child,false});
            }
        }
        if(!FindNextFileW(find,&data)){require(GetLastError()==ERROR_NO_MORE_FILES,"scan_scratch_enumeration");break;}
    }
}
}
ins::Handle clamav_status_gate(const FilePayloadDirectory& work,const std::wstring& owner,const std::function<bool()>& cancelled,std::uint32_t wait){
    require(wait<=5000&&work.path().filename()==L"ClamAV","scan_status_gate_policy");return operation_lock(work.path(),owner,wait,cancelled,{});
}
struct ClamAvResourceLease::Impl {
    // Destruction releases the budget before the gate. Inherited child handles
    // remain alive independently; the native budget retains any unknown child.
    std::optional<FilePayloadDirectory> parent;
    ins::Handle lock;
    std::shared_ptr<FileReceiveBudget> budget;
    ClamAvResourceKind kind{ClamAvResourceKind::scan};
    ClamAvResourceCeilings ceilings;
    std::uint64_t memory_limit{};
    ins::Handle child;
    bool child_bound{},keepers_issued{};
};
ClamAvResourceLease::ClamAvResourceLease():impl_(std::make_unique<Impl>()){}
ClamAvResourceLease::~ClamAvResourceLease()=default;
std::unique_ptr<ClamAvResourceLease> ClamAvResourceLease::acquire(const FilePayloadDirectory& scratch,const std::wstring& owner,
    std::uint64_t maximum,const std::function<bool()>& cancelled,const std::function<void(std::uint64_t)>& activity,
    std::uint32_t wait,const ClamAvResourceCeilings& ceilings,ClamAvResourceKind kind){
    const auto parent=FilePayloadDirectory::private_store(scratch.path().parent_path(),owner);
    return acquire_in_directory(parent,scratch,owner,maximum,cancelled,activity,wait,ceilings,kind);
}
std::unique_ptr<ClamAvResourceLease> ClamAvResourceLease::acquire_update(const FilePayloadDirectory& work,const std::wstring& owner,
    std::uint64_t maximum,const std::function<bool()>& cancelled,const std::function<void(std::uint64_t)>& activity,
    std::uint32_t wait,const ClamAvResourceCeilings& ceilings){
    require(work.path().filename()==L"ClamAV"&&work.path().parent_path().filename()==L"Work","scan_admission_parent_policy");
    return acquire_in_directory(work,work,owner,maximum,cancelled,activity,wait,ceilings,ClamAvResourceKind::definition_update);
}
std::unique_ptr<ClamAvResourceLease> ClamAvResourceLease::acquire_in_directory(const FilePayloadDirectory& parent,const FilePayloadDirectory& budget_directory,
    const std::wstring& owner,std::uint64_t maximum,const std::function<bool()>& cancelled,const std::function<void(std::uint64_t)>& activity,
    std::uint32_t wait,const ClamAvResourceCeilings& ceilings,ClamAvResourceKind kind){
    require(maximum>0&&maximum<=maximum_scratch,"scan_admission_scratch_policy");auto result=std::unique_ptr<ClamAvResourceLease>(new ClamAvResourceLease);
    result->impl_->parent.emplace(FilePayloadDirectory::private_store(parent.path(),owner));
    result->impl_->lock=operation_lock(result->impl_->parent->path(),owner,wait,cancelled,activity);
    result->impl_->kind=kind;result->impl_->ceilings=ceilings;result->impl_->memory_limit=memory_allowance(kind,ceilings);
    ULARGE_INTEGER available{};require(GetDiskFreeSpaceExW(budget_directory.path().c_str(),&available,nullptr,nullptr)!=FALSE,"scan_admission_disk_space");
    auto directory=ins::open_object(budget_directory.path(),true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    result->impl_->budget=FileReceiveBudget::reserve(directory.get(),owner,limited(available.QuadPart,ceilings.free_disk),maximum);
    return result;
}
ins::Handle ClamAvResourceLease::inherit_lock()const{impl_->keepers_issued=true;return inherited_read(impl_->lock.get());}
ins::Handle ClamAvResourceLease::inherit_budget()const{impl_->keepers_issued=true;return impl_->budget->inherit_keeper();}
void ClamAvResourceLease::bind_child(HANDLE child){
    require(!impl_->child_bound,"scan_admission_child_already_bound");impl_->child_bound=true;
    HANDLE pinned{};require(DuplicateHandle(GetCurrentProcess(),child,GetCurrentProcess(),&pinned,SYNCHRONIZE,FALSE,0)!=FALSE,"scan_admission_child_pin");
    impl_->child.reset(pinned);impl_->budget->bind_child(child);
}
std::uint64_t ClamAvResourceLease::child_memory_limit()const{return std::min(impl_->memory_limit,memory_allowance(impl_->kind,impl_->ceilings));}

struct ClamAvScratch::Impl {
    std::optional<FilePayloadDirectory> parent;
    Path path;std::wstring owner;std::uint64_t id{};DWORD volume{};bool cleaned{};
};
ClamAvScratch::ClamAvScratch():impl_(std::make_unique<Impl>()){}
ClamAvScratch::~ClamAvScratch()=default; // No silent recursive destructor cleanup.
std::unique_ptr<ClamAvScratch> ClamAvScratch::create(const Path& parent,const std::wstring& owner,bool update){
    auto result=std::unique_ptr<ClamAvScratch>(new ClamAvScratch);result->impl_->parent.emplace(FilePayloadDirectory::private_store(parent,owner));result->impl_->owner=owner;
    result->impl_->path=result->impl_->parent->path()/((update?L"update-":L"scan-")+ins::unique_id());const auto descriptor=ins::mutable_descriptor(owner);ins::create_directory(result->impl_->path,descriptor.get());
    const auto file=ins::open_object(result->impl_->path,true);result->impl_->id=ins::file_id(file.get());result->impl_->volume=ins::volume_serial(file.get());return result;
}
const Path& ClamAvScratch::path()const noexcept{return impl_->path;}
bool ClamAvScratch::cleanup()noexcept{
    if(impl_->cleaned)return true;
    try{
        // A child that inherited admission MUST have exited; otherwise no byte
        // is deleted. Also excludes a new scan/update during this inventory.
        const auto gate=operation_lock(impl_->parent->path(),impl_->owner,0,{},{});
        return cleanup_held();
    }catch(...){return false;}
}
bool ClamAvScratch::cleanup(ClamAvResourceLease& resource)noexcept{
    if(impl_->cleaned)return true;
    try{
        const auto& admitted=*resource.impl_;
        require(admitted.parent&&admitted.lock&&admitted.parent->path()==impl_->parent->path(),"scan_scratch_admission_mismatch");
        require(!admitted.keepers_issued||admitted.child_bound,"scan_scratch_child_unknown");
        require(!admitted.child_bound||(admitted.child&&WaitForSingleObject(admitted.child.get(),0)==WAIT_OBJECT_0),"scan_scratch_child_not_reaped");
        return cleanup_held();
    }catch(...){return false;}
}
bool ClamAvScratch::cleanup_held()noexcept{
    try{
        std::vector<Entry> entries;std::uint64_t bytes{};inventory(impl_->path,impl_->owner,impl_->volume,entries,bytes,0,impl_->id);
        require(!entries.empty()&&ins::file_id(entries.front().handle.get())==impl_->id,"scan_scratch_root_replaced");
        // First inventory every object without deleting anything. Then remove
        // exact held children before parents. A failure retains the remainder.
        for(auto entry=entries.rbegin();entry!=entries.rend();++entry){FILE_DISPOSITION_INFO disposition{TRUE};require(SetFileInformationByHandle(entry->handle.get(),FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE,"scan_scratch_cleanup_failed");entry->handle.reset();}
        require(GetFileAttributesW(impl_->path.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"scan_scratch_cleanup_unconfirmed");impl_->cleaned=true;return true;
    }catch(...){return false;}
}
} // namespace sfx::platform::win32
