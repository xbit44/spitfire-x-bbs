#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/file_receive_budget.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/ftn_processed_expiry.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/application/ftn_wire.hpp"
#include <sodium.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace sfx::platform::win32 {
namespace {
namespace app=application;
namespace ins=installation;
using Path=std::filesystem::path;
std::mutex payload_write_budget;
bool equal(const Path& a,const Path& b){return _wcsicmp(a.c_str(),b.c_str())==0;}
bool within(const Path& child,const Path& parent){
    const auto c=child.native(),p=parent.native();
    return c.size()>p.size() && c[p.size()]==L'\\' && !_wcsnicmp(c.c_str(),p.c_str(),p.size());
}
bool overlap(const Path& a,const Path& b){return equal(a,b)||within(a,b)||within(b,a);}
ins::Descriptor payload_security(const std::wstring& owner){
    // File policy differs from the containing directory: no execute,
    // delete-child or inheritable ACE flags on a newly received payload.
    const auto sddl=L"O:"+owner+L"D:P(A;;0x13019f;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)";
    PSECURITY_DESCRIPTOR raw{};ins::require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr)!=FALSE,"file_security_descriptor_failed");
    return ins::Descriptor(raw);
}
std::wstring wide(std::string_view text){
    ins::require(app::valid_file_name(text),"file_name_invalid");
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    ins::require(count>0,"file_name_utf8_invalid");std::wstring result(static_cast<std::size_t>(count),L'\0');
    ins::require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count)==count,"file_name_decode_failed");
    return result;
}
std::string utf8(std::wstring_view text){
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    ins::require(count>0,"file_name_encode_failed");std::string result(static_cast<std::size_t>(count),'\0');
    ins::require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr)==count,"file_name_encode_failed");return result;
}
Path native_path(const Path& path){return L"\\\\?\\"+path.native();}
Path canonical_directory(const Path& input,bool require_online_volume=true){
    // Installation roots retain Setup's 160-character contract. Descendant
    // payload directories need their own bounded path check, not that root cap.
    auto text=input.native();std::replace(text.begin(),text.end(),L'/',L'\\');
    ins::require(text.size()>=4&&text.size()<=2048&&text[1]==L':'&&text[2]==L'\\'&&
        ((text[0]>=L'A'&&text[0]<=L'Z')||(text[0]>=L'a'&&text[0]<=L'z')),"file_directory_requires_local_absolute_path");
    text[0]=static_cast<wchar_t>(towupper(text[0]));
    for(std::size_t at=3;at<text.size();){
        const auto end=text.find(L'\\',at);const auto part=text.substr(at,end==std::wstring::npos?end:end-at);
        ins::require(part.find(L'~')==std::wstring::npos&&app::valid_file_name(utf8(part)),"file_directory_component_invalid");
        if(end==std::wstring::npos)break;at=end+1;ins::require(at<text.size(),"file_directory_trailing_separator");
    }
    std::array<wchar_t,4096> full{};const auto length=GetFullPathNameW(text.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
    ins::require(length>0&&length<full.size()&&!_wcsicmp(full.data(),text.c_str()),"file_directory_alias_refused");
    const auto drive=text.substr(0,3);wchar_t filesystem[32]{};DWORD flags{};
    ins::require(!require_online_volume||(GetDriveTypeW(drive.c_str())==DRIVE_FIXED&&GetVolumeInformationW(drive.c_str(),nullptr,0,nullptr,nullptr,&flags,filesystem,32)&&
        !_wcsicmp(filesystem,L"NTFS")&&(flags&FILE_PERSISTENT_ACLS)),"file_directory_requires_fixed_ntfs");
    std::array<wchar_t,MAX_PATH> windows{};ins::require(GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()))!=0,"file_windows_directory_unknown");
    ins::require(!overlap(Path(text),Path(windows.data())),"file_system_directory_refused");return text;
}
ins::Handle open_payload_object(const Path& path,bool directory,DWORD access,DWORD sharing){
    ins::Handle handle(CreateFileW(native_path(path).c_str(),access,sharing,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr));
    ins::require(handle.get()!=INVALID_HANDLE_VALUE,"file_object_open_failed");BY_HANDLE_FILE_INFORMATION info{};
    ins::require(GetFileInformationByHandle(handle.get(),&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
        ((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory&&(directory||info.nNumberOfLinks==1),"file_object_link_or_kind_refused");
    std::array<wchar_t,32768> final{};const auto n=GetFinalPathNameByHandleW(handle.get(),final.data(),static_cast<DWORD>(final.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    ins::require(n>0&&n<final.size()&&!_wcsicmp(final.data(),native_path(path).c_str()),"file_object_final_path_mismatch");return handle;
}
std::uint64_t size(HANDLE file){LARGE_INTEGER n{};ins::require(GetFileSizeEx(file,&n)&&n.QuadPart>=0,"file_size_failed");return static_cast<std::uint64_t>(n.QuadPart);}
void seek(HANDLE file,std::uint64_t offset){ins::require(offset<=INT64_MAX,"file_offset_invalid");LARGE_INTEGER n{};n.QuadPart=static_cast<LONGLONG>(offset);ins::require(SetFilePointerEx(file,n,nullptr,FILE_BEGIN)!=FALSE,"file_seek_failed");}
void verify_regular(HANDLE handle,const Path& path){
    BY_HANDLE_FILE_INFORMATION i{};ins::require(GetFileInformationByHandle(handle,&i)!=FALSE,"file_identity_failed");
    ins::require(!(i.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&i.nNumberOfLinks==1,"file_link_or_directory_refused");
    std::array<wchar_t,32768> text{};const auto n=GetFinalPathNameByHandleW(handle,text.data(),static_cast<DWORD>(text.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    ins::require(n>0&&n<text.size()&&!_wcsicmp(text.data(),(L"\\\\?\\"+path.native()).c_str()),"file_final_identity_mismatch");
}
} // namespace
struct FilePayloadDirectory::Impl {
    Path path;std::wstring owner;std::vector<ins::Handle> ancestors;bool import_only{},permit_move{},private_custody{},inherited_area{};std::function<std::uint64_t()> available_ceiling;
    Impl(const Path& directory,std::wstring sid,bool input=false,bool move=false,std::function<std::uint64_t()> ceiling={},bool custody=false,bool inherited=false):path(canonical_directory(directory)),owner(std::move(sid)),import_only(input),permit_move(move),private_custody(custody),inherited_area(inherited),available_ceiling(std::move(ceiling)){
        std::vector<Path> chain;
        for(auto p=path;!p.empty();){chain.push_back(p);const auto next=p.parent_path();if(next==p)break;p=next;}
        std::reverse(chain.begin(),chain.end());
        // Attribute-only opens do not establish the read-sharing constraint
        // needed to block DELETE opens. Keep actual directory read access.
        for(const auto& p:chain)ancestors.push_back(open_payload_object(p,true,READ_CONTROL|FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE));
        if(!import_only){const auto checked=WindowsStorageDaclPolicy(owner).assess(native_path(path),StorageDaclObjectKind::directory,inherited_area);if(!checked.status)throw std::runtime_error(checked.status.operation);}
    }
    std::uint64_t available()const{
        ULARGE_INTEGER bytes{};ins::require(GetDiskFreeSpaceExW(native_path(path).c_str(),&bytes,nullptr,nullptr)!=FALSE,"file_free_space_unavailable");
        return available_ceiling?std::min<std::uint64_t>(bytes.QuadPart,available_ceiling()):bytes.QuadPart;
    }
    void space(std::uint64_t incoming)const{
        const auto free=FileReceiveBudget::available(ancestors.back().get(),owner,available());ins::require(free>=kFilePayloadFreeReserve&&incoming<=free-kFilePayloadFreeReserve,"file_free_space_reserve");
    }
};
struct FilePayloadLease::Impl {
    std::shared_ptr<FilePayloadDirectory::Impl> directory;
    Path path;std::string name;std::shared_ptr<FileReceiveBudget> receive_budget;ins::Handle file;
    app::FileObjectIdentity logical_object;
    std::uint64_t limit{};bool created{},mutation{};mutable bool sealed{};bool removed{};
    Impl(std::shared_ptr<FilePayloadDirectory::Impl> d,std::string n,bool fresh,bool mutate,std::uint64_t maximum,const app::FilePayloadIdentity* prefix=nullptr,const std::function<bool(std::uint64_t,std::uint64_t)>& progress={},const app::FileObjectIdentity* copied_object=nullptr,const FilePayloadLease* copy_source=nullptr,const std::string_view* generated_source=nullptr,DWORD sharing=FILE_SHARE_READ):
        directory(std::move(d)),path(directory->path/wide(n)),name(std::move(n)),limit(maximum),created(fresh),mutation(mutate||fresh){
        ins::require(!fresh || (maximum>0&&maximum<=INT64_MAX),"file_create_limit_invalid");
        ins::require(!directory->import_only||(!fresh&&(!mutate||directory->permit_move)),"file_import_source_not_output");
        ins::require(!prefix||(!fresh&&mutate&&directory->private_custody&&maximum>0&&maximum<=INT64_MAX&&prefix->bytes<=maximum),"file_resume_requires_private_receipt_capability");
        ins::require((copied_object==nullptr)==(!copy_source&&!generated_source)&&!(copy_source&&generated_source),"file_copy_resume_context_invalid");
        ins::require((!copy_source&&!generated_source)||(!fresh&&mutate&&!prefix&&!directory->import_only&&maximum>0&&maximum<=INT64_MAX),"file_copy_resume_requires_owned_new_object"); ins::require(!generated_source||(directory->private_custody&&generated_source->size()<=65536),"file_generated_resume_requires_bounded_private_metadata");
        if(fresh){
            std::lock_guard<std::mutex> budget(payload_write_budget);
            receive_budget=FileReceiveBudget::reserve(directory->ancestors.back().get(),directory->owner,directory->available(),maximum);
            const auto descriptor=payload_security(directory->owner);
            PACL base{};BOOL present{},defaulted{};
            ins::require(GetSecurityDescriptorDacl(descriptor.get(),&present,&base,&defaulted)&&present&&base,"file_payload_security_missing");
            const auto readers=capture_backup_read_grants(directory->ancestors.back().get(),directory->owner,false,true);
            auto merged=merge_backup_read_grants(base,readers);
            SECURITY_DESCRIPTOR secured{};PSID selected_owner{};
            ins::require(GetSecurityDescriptorOwner(descriptor.get(),&selected_owner,&defaulted)&&
                InitializeSecurityDescriptor(&secured,SECURITY_DESCRIPTOR_REVISION)&&
                SetSecurityDescriptorOwner(&secured,selected_owner,FALSE)&&
                SetSecurityDescriptorDacl(&secured,TRUE,reinterpret_cast<PACL>(merged.data()),FALSE)&&
                SetSecurityDescriptorControl(&secured,SE_DACL_PROTECTED,SE_DACL_PROTECTED),"file_payload_reader_security_failed");
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),&secured,FALSE};
            {
                ServiceFileOwnerScope scope(directory->owner);ins::require(scope.valid(),"file_create_owner_scope_failed");
                file.reset(CreateFileW(native_path(path).c_str(),GENERIC_READ|GENERIC_WRITE|DELETE|READ_CONTROL,FILE_SHARE_READ,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            }
            ins::require(file.get()!=INVALID_HANDLE_VALUE,"file_create_new_failed");
        }else file=open_payload_object(path,false,GENERIC_READ|READ_CONTROL|(mutate?DELETE:0)|((prefix||copy_source||generated_source)?GENERIC_WRITE:0),sharing);
        verify_regular(file.get(),path);
        if(fresh){
            record_portable_new_file(path,file.get());
            logical_object={std::to_string(ins::volume_serial(file.get())),std::to_string(ins::file_id(file.get()))};
        }else logical_object=portable_logical_file_identity(path,file.get());
        if(!directory->import_only)ins::require(WindowsStorageDaclPolicy(directory->owner).assess(native_path(path),StorageDaclObjectKind::file,true).status.succeeded(),"file_payload_not_private");
        if(receive_budget)receive_budget->bind_file(file.get());
        if(prefix){
            ins::require(prefix->volume==logical_object.volume&&prefix->file_id==logical_object.file_id,"file_resume_object_identity_changed");
            const auto actual=size(file.get());ins::require(actual>=prefix->bytes&&actual<=maximum,"file_resume_size_mismatch");
            crypto_hash_sha256_state hash{};crypto_hash_sha256_init(&hash);std::array<unsigned char,65536> buffer{};std::uint64_t total{};seek(file.get(),0);
            while(total<prefix->bytes){ins::require(!progress||progress(total,prefix->bytes),"file_resume_hash_cancelled");DWORD count{};const auto amount=static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(),prefix->bytes-total));ins::require(ReadFile(file.get(),buffer.data(),amount,&count,nullptr)&&count,"file_resume_prefix_read_failed");crypto_hash_sha256_update(&hash,buffer.data(),count);total+=count;}
            std::array<unsigned char,32> digest{};crypto_hash_sha256_final(&hash,digest.data());ins::require(sodium_memcmp(digest.data(),prefix->sha256.data(),digest.size())==0,"file_resume_prefix_changed");
            ins::require(!progress||progress(total,prefix->bytes),"file_resume_hash_cancelled");
            {std::lock_guard<std::mutex> budget(payload_write_budget);receive_budget=FileReceiveBudget::reserve(directory->ancestors.back().get(),directory->owner,directory->available(),std::max<std::uint64_t>(1,maximum-prefix->bytes));receive_budget->bind_file(file.get());}
            if(actual!=prefix->bytes){seek(file.get(),prefix->bytes);ins::require(SetEndOfFile(file.get())&&FlushFileBuffers(file.get()),"file_resume_uncommitted_tail_recovery_failed");}
            created=true;mutation=true;
        }
        if(copy_source||generated_source){
            const app::FileObjectIdentity actual_object{std::to_string(ins::volume_serial(file.get())),std::to_string(ins::file_id(file.get()))};
            ins::require(logical_object.volume==copied_object->volume&&logical_object.file_id==copied_object->file_id,"file_copy_resume_object_changed");
            app::FilePayloadIdentity source;if(copy_source){source=copy_source->identity(progress);const auto source_object=copy_source->native_object_identity();ins::require(actual_object.volume!=source_object.volume||actual_object.file_id!=source_object.file_id,"file_copy_resume_source_alias");}else source.bytes=generated_source->size();
            ins::require(maximum==std::max<std::uint64_t>(1,source.bytes),"file_copy_resume_source_size_changed");const auto actual=size(file.get());ins::require(actual<=source.bytes,"file_copy_resume_excess_bytes");
            std::array<std::uint8_t,65536> buffer{};
            struct WipeCopyBuffer{decltype(buffer)& bytes;~WipeCopyBuffer(){sodium_memzero(bytes.data(),bytes.size());}} wipe_copy_buffer{buffer};
            std::uint64_t total{};seek(file.get(),0);
            while(total<actual){ins::require(!progress||progress(total,source.bytes),"file_copy_resume_cancelled");const auto amount=static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(),actual-total));DWORD count{};ins::require(ReadFile(file.get(),buffer.data(),amount,&count,nullptr)&&count==amount,"file_copy_resume_read_failed");if(copy_source){const auto expected=copy_source->read(total,amount);ins::require(expected.size()==count&&sodium_memcmp(buffer.data(),expected.data(),count)==0,"file_copy_resume_prefix_changed");}else ins::require(sodium_memcmp(buffer.data(),generated_source->data()+total,count)==0,"file_copy_resume_prefix_changed");total+=count;}
            ins::require(!progress||progress(total,source.bytes),"file_copy_resume_cancelled");
            {std::lock_guard<std::mutex> budget(payload_write_budget);receive_budget=FileReceiveBudget::reserve(directory->ancestors.back().get(),directory->owner,directory->available(),std::max<std::uint64_t>(1,source.bytes-actual));receive_budget->bind_file(file.get());}
            // No SetEndOfFile: exact bound matching prefixes are append-only.
            created=true;mutation=true;
        }
    }
    void live()const{ins::require(!removed&&file&&file.get()!=INVALID_HANDLE_VALUE,"file_lease_closed");verify_regular(file.get(),path);}
};
FilePayloadDirectory::FilePayloadDirectory(std::shared_ptr<Impl> value):impl_(std::move(value)){}
FilePayloadDirectory FilePayloadDirectory::area(const Path& root,const Path& configured,const std::vector<Path>& others,const std::vector<Path>& protected_paths,const std::wstring& owner){
    const auto install=ins::canonical_root(root);
    // Reject dot/ADS/device/short-path aliases before normalization, not after.
    const auto selected=canonical_directory(resolve_portable_owned_path(install,configured));
    ins::require(!equal(selected,install)&&!within(install,selected),"file_area_cannot_contain_installation");
    if(within(selected,install))ins::require(within(selected,install/L"Files"),"file_area_requires_files_role");
    // These are lexical exclusions, not sources to open/adopt. An unrelated
    // offline drive must not disable a healthy selected C: library. The actual
    // selected directory/ancestors are still pinned and checked below.
    for(const auto& path:others){const auto other=canonical_directory(resolve_portable_owned_path(install,path),false);ins::require(!overlap(selected,other),"file_area_overlap_refused");}
    for(const auto& path:protected_paths){const auto protect=canonical_directory(resolve_portable_owned_path(install,path),false);ins::require(!overlap(selected,protect),"file_area_protected_overlap");}
    const auto files=install/L"Files";const bool inherited=within(selected,files);
    if(inherited){
        // Only the installation's Files role grants inherited Area admission.
        // Every intervening directory needs appropriate private current access;
        // this assessment never changes a security descriptor.
        for(auto p=selected;within(p,files)||equal(p,files);p=p.parent_path()){
            const auto checked=WindowsStorageDaclPolicy(owner).assess(native_path(p),StorageDaclObjectKind::directory,!equal(p,files));
            if(!checked.status)throw std::runtime_error(checked.status.operation);
        }
    }
    return FilePayloadDirectory(std::make_shared<Impl>(selected,owner,false,false,std::function<std::uint64_t()>{},false,inherited));
}
FilePayloadDirectory FilePayloadDirectory::private_store(const Path& path,const std::wstring& owner,std::function<std::uint64_t()> ceiling){return FilePayloadDirectory(std::make_shared<Impl>(path,owner,false,false,std::move(ceiling),true));}
FilePayloadDirectory FilePayloadDirectory::import_source(const Path& path,const std::vector<Path>& protected_paths,const std::wstring& owner,bool move){
    const auto selected=canonical_directory(path);for(const auto& protected_path:protected_paths)ins::require(!overlap(selected,canonical_directory(protected_path,false)),"file_import_protected_source_refused");
    return FilePayloadDirectory(std::make_shared<Impl>(selected,owner,true,move));
}
const Path& FilePayloadDirectory::path()const noexcept{return impl_->path;}
std::uint64_t FilePayloadDirectory::available_bytes()const{return FileReceiveBudget::available(impl_->ancestors.back().get(),impl_->owner,impl_->available());}
void FilePayloadDirectory::require_space(std::uint64_t incoming)const{impl_->space(incoming);}
bool FilePayloadDirectory::absent(std::string_view name)const{
    const auto attributes=GetFileAttributesW(native_path(impl_->path/wide(name)).c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES)return false;
    ins::require(GetLastError()==ERROR_FILE_NOT_FOUND,"file_absence_not_verified");return true;
}
std::string FilePayloadDirectory::volume()const{return std::to_string(ins::volume_serial(impl_->ancestors.back().get()));}
struct FilePayloadEnumeration::Impl {
    std::shared_ptr<FilePayloadDirectory::Impl> directory;HANDLE search{INVALID_HANDLE_VALUE};WIN32_FIND_DATAW data{};bool started{},finished{};
    explicit Impl(std::shared_ptr<FilePayloadDirectory::Impl> d):directory(std::move(d)){}
    ~Impl(){if(search!=INVALID_HANDLE_VALUE)FindClose(search);}
    void first(){
        if(started)return;started=true;search=FindFirstFileW(native_path(directory->path/L"*").c_str(),&data);
        if(search==INVALID_HANDLE_VALUE){ins::require(GetLastError()==ERROR_FILE_NOT_FOUND,"file_directory_list_failed");finished=true;}
    }
    void advance(){if(!FindNextFileW(search,&data)){ins::require(GetLastError()==ERROR_NO_MORE_FILES,"file_directory_list_interrupted");finished=true;}}
};
FilePayloadEnumeration::FilePayloadEnumeration(std::shared_ptr<FilePayloadDirectory::Impl> d):impl_(std::make_unique<Impl>(std::move(d))){}
FilePayloadEnumeration::~FilePayloadEnumeration()=default;
FilePayloadEnumeration::FilePayloadEnumeration(FilePayloadEnumeration&&)noexcept=default;
FilePayloadEnumeration& FilePayloadEnumeration::operator=(FilePayloadEnumeration&&)noexcept=default;
FilePayloadEnumeration FilePayloadDirectory::enumerate()const{return FilePayloadEnumeration(impl_);}
FilePayloadNamePage FilePayloadEnumeration::next(std::size_t limit,const std::function<bool()>& keep_going){
    ins::require(limit>0&&limit<=10000,"file_directory_page_bound");impl_->first();FilePayloadNamePage result;
    // A directory containing only unsupported entries must still yield bounded
    // work to the native UI. Empty nonterminal pages are permitted.
    std::size_t visited{};
    while(!impl_->finished&&result.names.size()<limit&&visited++<10000){
        ins::require(!keep_going||keep_going(),"file_directory_listing_cancelled");
        if(!(impl_->data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){const auto name=utf8(impl_->data.cFileName);if(app::valid_file_name(name))result.names.push_back(name);}
        impl_->advance();
    }
    result.finished=impl_->finished;return result;
}
std::vector<std::string> FilePayloadDirectory::names(std::size_t maximum)const{
    ins::require(maximum>0&&maximum<=100000,"file_directory_listing_bound");
    WIN32_FIND_DATAW data{};const auto handle=FindFirstFileW(native_path(impl_->path/L"*").c_str(),&data);
    if(handle==INVALID_HANDLE_VALUE){ins::require(GetLastError()==ERROR_FILE_NOT_FOUND,"file_directory_list_failed");return {};}
    struct Close {HANDLE h;~Close(){FindClose(h);}} closer{handle};std::vector<std::string> result;
    do{
        if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
        const auto name=utf8(data.cFileName);if(!app::valid_file_name(name))continue;
        ins::require(result.size()<maximum,"file_directory_listing_limit");result.push_back(name);
    }while(FindNextFileW(handle,&data));
    ins::require(GetLastError()==ERROR_NO_MORE_FILES,"file_directory_list_interrupted");
    std::sort(result.begin(),result.end());return result;
}
FilePayloadLease FilePayloadDirectory::read(std::string_view name,bool mutate)const{return FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,mutate,0));}
FilePayloadLease FilePayloadDirectory::create(std::string_view name,std::uint64_t maximum)const{return FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),true,true,maximum));}
FilePayloadLease FilePayloadDirectory::resume_private(std::string_view name,std::uint64_t maximum,const app::FilePayloadIdentity& prefix,const std::function<bool(std::uint64_t,std::uint64_t)>& progress)const{return FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,true,maximum,&prefix,progress));}
FilePayloadLease FilePayloadDirectory::resume_owned_copy(std::string_view name,const app::FileObjectIdentity& object,const FilePayloadLease& source,const std::function<bool(std::uint64_t,std::uint64_t)>& progress)const{const auto identity=source.identity(progress);return FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,true,std::max<std::uint64_t>(1,identity.bytes),nullptr,progress,&object,&source));}
FilePayloadLease FilePayloadDirectory::resume_owned_bytes(std::string_view name,const app::FilePayloadIdentity& expected,std::string_view regenerated,const std::function<bool(std::uint64_t,std::uint64_t)>& progress)const{
    ins::require(regenerated.size()<=65536&&expected.bytes==regenerated.size(),"file_generated_intent_size_changed");
    ins::require(!progress||progress(0,expected.bytes),"file_generated_resume_cancelled");
    std::array<unsigned char,32> hash{};crypto_hash_sha256(hash.data(),reinterpret_cast<const unsigned char*>(regenerated.data()),regenerated.size());
    ins::require(sodium_memcmp(hash.data(),expected.sha256.data(),hash.size())==0,"file_generated_intent_digest_changed");
    const app::FileObjectIdentity object{expected.volume,expected.file_id};
    return FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,true,std::max<std::uint64_t>(1,expected.bytes),nullptr,progress,&object,nullptr,&regenerated));
}
FilePayloadLease::FilePayloadLease(std::unique_ptr<Impl> value):impl_(std::move(value)){}
FilePayloadLease::~FilePayloadLease()=default;
FilePayloadLease::FilePayloadLease(FilePayloadLease&&)noexcept=default;
FilePayloadLease& FilePayloadLease::operator=(FilePayloadLease&&)noexcept=default;
std::string FilePayloadLease::name()const{return impl_->name;}
const Path& FilePayloadLease::path()const noexcept{return impl_->path;}
std::vector<std::uint8_t> FilePayloadLease::read(std::uint64_t offset,std::size_t maximum)const{
    impl_->live();ins::require(maximum<=65536&&offset<=size(impl_->file.get()),"file_read_bound");seek(impl_->file.get(),offset);
    std::vector<std::uint8_t> result(maximum);DWORD n{};ins::require(ReadFile(impl_->file.get(),result.data(),static_cast<DWORD>(maximum),&n,nullptr)!=FALSE,"file_read_failed");result.resize(n);return result;
}
void FilePayloadLease::append(const std::uint8_t* bytes,std::size_t count){
    impl_->live();ins::require(impl_->created&&!impl_->sealed&&count<=65536&&(bytes||!count),"file_write_not_permitted");
    const auto n=size(impl_->file.get());ins::require(n<=impl_->limit&&count<=impl_->limit-n,"file_actual_size_limit");
    // Serialize this host's bounded writes and recheck real usable free space
    // on every chunk. External programs can still consume disk space: ordinary
    // WriteFile failures remain truthful private HOLDs, never success receipts.
    std::lock_guard<std::mutex> budget(payload_write_budget);impl_->receive_budget->require_write(impl_->directory->available(),count);seek(impl_->file.get(),n);
    while(count){DWORD done{};ins::require(WriteFile(impl_->file.get(),bytes,static_cast<DWORD>(count),&done,nullptr)&&done,"file_write_failed");bytes+=done;count-=done;}
}
std::uint64_t FilePayloadLease::byte_count()const{impl_->live();return size(impl_->file.get());}
void FilePayloadLease::flush(){impl_->live();ins::require(impl_->created&&!impl_->sealed,"file_checkpoint_not_writable");ins::require(FlushFileBuffers(impl_->file.get())!=FALSE,"file_checkpoint_flush_failed");}
ins::Handle FilePayloadLease::duplicate_transfer_handle(bool receiving)const{
    impl_->live();
    if(receiving){ins::require(impl_->created&&!impl_->sealed&&size(impl_->file.get())==0,"file_transfer_receive_requires_new_empty_object");impl_->receive_budget->require_write(impl_->directory->available(),impl_->limit);}
    else ins::require(!impl_->created||impl_->sealed,"file_transfer_send_requires_sealed_object");
    HANDLE duplicate{};const DWORD access=receiving?(GENERIC_WRITE|FILE_READ_ATTRIBUTES):GENERIC_READ;
    ins::require(DuplicateHandle(GetCurrentProcess(),impl_->file.get(),GetCurrentProcess(),&duplicate,access,TRUE,0)!=FALSE,"file_transfer_duplicate_failed");
    ins::Handle result(duplicate);verify_regular(result.get(),impl_->path);
    ins::require(ins::file_id(result.get())==ins::file_id(impl_->file.get())&&ins::volume_serial(result.get())==ins::volume_serial(impl_->file.get()),"file_transfer_duplicate_identity");
    return result;
}
app::FileObjectIdentity FilePayloadLease::object_identity()const{impl_->live();return impl_->logical_object;}
app::FileObjectIdentity FilePayloadLease::native_object_identity()const{impl_->live();return {std::to_string(ins::volume_serial(impl_->file.get())),std::to_string(ins::file_id(impl_->file.get()))};}
app::FilePayloadIdentity FilePayloadLease::identity(const std::function<bool(std::uint64_t,std::uint64_t)>& progress)const{
    impl_->live();if(impl_->created&&!impl_->sealed){ins::require(FlushFileBuffers(impl_->file.get())!=FALSE,"file_flush_failed");impl_->receive_budget->seal();impl_->sealed=true;}
    app::FilePayloadIdentity result;result.bytes=size(impl_->file.get());result.volume=impl_->logical_object.volume;result.file_id=impl_->logical_object.file_id;
    ins::require(!impl_->created||result.bytes<=impl_->limit,"file_external_actual_size_limit");
    crypto_hash_sha256_state hash{};crypto_hash_sha256_init(&hash);seek(impl_->file.get(),0);std::array<unsigned char,65536> buffer{};std::uint64_t total{};
    for(;;){ins::require(!progress||progress(total,result.bytes),"file_hash_cancelled");DWORD n{};ins::require(ReadFile(impl_->file.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&n,nullptr)!=FALSE,"file_hash_read_failed");if(!n)break;total+=n;ins::require(total<=result.bytes,"file_size_changed");crypto_hash_sha256_update(&hash,buffer.data(),n);}
    ins::require(total==result.bytes&&size(impl_->file.get())==total,"file_size_changed");crypto_hash_sha256_final(&hash,result.sha256.data());return result;
}
void FilePayloadLease::rename_to(const FilePayloadDirectory& destination,std::string_view name){
    impl_->live();ins::require(impl_->mutation&&!destination.impl_->import_only,"file_rename_not_authorized");const auto leaf=wide(name);const auto path=destination.path()/leaf;
    // An outside source may have a public ACL. Copy into the protected output
    // so its explicit move does not silently grant library write permissions.
    ins::require(!impl_->directory->import_only,"file_external_move_requires_private_copy");
    ins::require(ins::volume_serial(impl_->file.get())==ins::volume_serial(destination.impl_->ancestors.back().get()),"file_rename_cross_volume_requires_copy");
    if(impl_->created)identity();
    // Win32 path translation consumes a terminated string even though the
    // counted native rename length excludes its terminator. Retain both.
    const auto absolute=native_path(path);const auto bytes=absolute.native().size()*sizeof(wchar_t);std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+bytes,0);
    auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());info->ReplaceIfExists=FALSE;info->RootDirectory=nullptr;info->FileNameLength=static_cast<DWORD>(bytes);std::memcpy(info->FileName,absolute.c_str(),bytes);
    ins::require(SetFileInformationByHandle(impl_->file.get(),FileRenameInfo,info,static_cast<DWORD>(buffer.size()))!=FALSE,"file_rename_no_overwrite_failed");
    impl_->directory=destination.impl_;impl_->path=path;impl_->name=std::string(name);verify_regular(impl_->file.get(),path);
}
void FilePayloadLease::remove_exact(){
    impl_->live();ins::require(impl_->mutation,"file_delete_not_authorized");FILE_DISPOSITION_INFO info{};info.DeleteFile=TRUE;
    ins::require(SetFileInformationByHandle(impl_->file.get(),FileDispositionInfo,&info,sizeof(info))!=FALSE,"file_exact_delete_failed");
    // The handle denies delete sharing, so no other delete-pending open can
    // survive this close. Caller records success only after this returns.
    impl_->file.reset();impl_->removed=true;
    const auto attributes=GetFileAttributesW(native_path(impl_->path).c_str());const auto error=GetLastError();
    ins::require(attributes==INVALID_FILE_ATTRIBUTES&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND),"file_delete_absence_unconfirmed");
    record_portable_removed_file(impl_->path,impl_->logical_object);
}
#include "ftn_processed_expiry.inc"
} // namespace sfx::platform::win32
