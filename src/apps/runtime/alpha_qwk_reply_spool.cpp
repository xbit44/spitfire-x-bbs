#include "alpha_qwk_reply_spool.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "qwk_private_file_descriptor.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <limits>
#include <sddl.h>

namespace sfx::apps::runtime {
namespace {
namespace app=sfx::application;
namespace inst=sfx::platform::win32::installation;
constexpr std::string_view magic="SFX-REP-SPOOL-1\n";
constexpr std::array<std::size_t,9> maximums{255,255,app::kMessageSubjectCells,app::kMessageBodyBytes,8,5,1024,1024,64};
std::array<const std::string*,9> fields(const app::QwkRecord& m){return {&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written};}
std::array<std::string*,9> fields(app::QwkRecord& m){return {&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written};}
bool same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&a.nFileSizeHigh==b.nFileSizeHigh&&a.nFileSizeLow==b.nFileSizeLow&&a.nNumberOfLinks==1&&b.nNumberOfLinks==1&&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime&&a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime&&
        !(b.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY));
}
void cancel_check(const std::function<bool()>& callback){inst::require(!callback||!callback(),"reply_spool_cancelled");}
void write(HANDLE file,std::string_view bytes,const std::function<bool()>& callback){
    for(std::size_t at=0;at<bytes.size();){cancel_check(callback);DWORD wrote{};const auto n=static_cast<DWORD>(std::min<std::size_t>(4096,bytes.size()-at));inst::require(WriteFile(file,bytes.data()+at,n,&wrote,nullptr)&&wrote>0&&wrote<=n,"reply_spool_write");at+=wrote;}
}
void number(HANDLE file,std::uint64_t value,unsigned width,const std::function<bool()>& callback){
    std::array<char,8> bytes{};for(unsigned n=0;n<width;++n)bytes[n]=static_cast<char>(value>>(n*8));write(file,{bytes.data(),width},callback);
}
struct Creation {
    inst::Handle file;
    // Partial sealing is failed/uncertain work, retained for review.
};
class SealedSource final:public app::IQwkReplySource {
public:
    // Destroyed LAST, after the pinned file/directory handles. The host's exact
    // nonrecursive job cleanup must not run while this source is writer-owned.
    std::shared_ptr<void> job_lifetime;
    inst::Handle directory,file;inst::Descriptor file_security,directory_security;BY_HANDLE_FILE_INFORMATION identity{};app::QwkReplySourceInfo metadata;
    std::function<bool()> cancellation;std::uint64_t position{},record_position{},size{};
    std::function<void()> confirmed_completion;bool disposable{};
    ~SealedSource()override{
        try{if(disposable&&file&&verify_identity()){FILE_DISPOSITION_INFO remove{TRUE};(void)SetFileInformationByHandle(file.get(),FileDispositionInfo,&remove,sizeof(remove));}}catch(...){/* retain confined object on uncertain identity/security */}
    }
    void confirm_completed()noexcept override{try{if(!disposable&&file&&verify_identity()){if(confirmed_completion)confirmed_completion();disposable=true;}}catch(...){}}
    app::QwkReplySourceInfo info()const override{return metadata;}
    bool cancelled()const noexcept override{try{return cancellation&&cancellation();}catch(...){return true;}}
    app::StorageStatus verify_identity()const override{
        BY_HANDLE_FILE_INFORMATION current{};
        if(!file||!GetFileInformationByHandle(file.get(),&current)||!same(identity,current))return {app::StorageErrorCode::io,"sealed reply file identity changed"};
        try{if(!file_security||!directory_security||!inst::exact_security(file.get(),file_security.get())||!inst::exact_security(directory.get(),directory_security.get()))return {app::StorageErrorCode::permission_denied,"sealed reply security changed"};}
        catch(...){return {app::StorageErrorCode::permission_denied,"sealed reply security unavailable"};}
        return app::StorageStatus::success();
    }
    std::string read(std::size_t n){
        inst::require(!cancelled()&&verify_identity()&&position<=size&&n<=size-position,"reply_spool_read_bounds");std::string bytes(n,'\0');
        for(std::size_t at=0;at<n;){inst::require(!cancelled(),"reply_spool_cancelled");DWORD actual{};const auto request=static_cast<DWORD>(std::min<std::size_t>(4096,n-at));inst::require(ReadFile(file.get(),bytes.data()+at,request,&actual,nullptr)&&actual>0&&actual<=request,"reply_spool_read");at+=actual;position+=actual;}
        return bytes;
    }
    std::uint64_t integer(unsigned width){const auto bytes=read(width);std::uint64_t value{};for(unsigned n=0;n<width;++n)value|=static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[n]))<<(n*8);return value;}
    app::StorageStatus rewind()override{
        try{
            LARGE_INTEGER zero{};inst::require(!cancelled()&&verify_identity()&&SetFilePointerEx(file.get(),zero,nullptr,FILE_BEGIN),"reply_spool_rewind");position=record_position=0;
            inst::require(read(magic.size())==magic,"reply_spool_magic");const auto board_length=integer(1);inst::require(board_length==metadata.board_id.size(),"reply_spool_board_length");inst::require(read(static_cast<std::size_t>(board_length))==metadata.board_id,"reply_spool_board");
            inst::require(integer(8)==metadata.records&&integer(8)==metadata.compressed_bytes&&integer(8)==metadata.expanded_bytes,"reply_spool_bounds_header");
            const auto digest=read(32);inst::require(sodium_memcmp(digest.data(),metadata.logical_sha256.data(),32)==0,"reply_spool_digest_header");return app::StorageStatus::success();
        }catch(...){return {cancelled()?app::StorageErrorCode::shutting_down:app::StorageErrorCode::io,"sealed reply rewind refused"};}
    }
    app::StorageResult<std::optional<app::QwkReplyRecord>> next()override{
        try{
            inst::require(!cancelled()&&verify_identity(),"reply_spool_identity");
            if(record_position==metadata.records){inst::require(position==size,"reply_spool_exact_eof");return {app::StorageStatus::success(),std::optional<app::QwkReplyRecord>{}};}
            app::QwkReplyRecord r;auto& m=r.message;m.conference=static_cast<std::uint16_t>(integer(2));m.reference=static_cast<std::uint32_t>(integer(4));
            const auto private_flag=integer(1),command=integer(1);inst::require(private_flag<=1&&command<=2,"reply_spool_flags");m.private_mail=private_flag==1;if(command)r.command=command==1?app::QwkScanCommand::add:app::QwkScanCommand::drop;
            const auto destinations=fields(m);
            for(std::size_t n=0;n<destinations.size();++n){const auto length=integer(4);inst::require(length<=maximums[n],"reply_spool_bounded_field");*destinations[n]=read(static_cast<std::size_t>(length));}
            ++record_position;return {app::StorageStatus::success(),std::optional<app::QwkReplyRecord>{std::move(r)}};
        }catch(...){return {{cancelled()?app::StorageErrorCode::shutting_down:app::StorageErrorCode::io,"sealed reply record refused"},std::nullopt};}
    }
};
} // namespace
app::StorageResult<std::shared_ptr<app::IQwkReplySource>> seal_qwk_reply_spool(
    const std::filesystem::path& job,const std::wstring& owner,const app::QwkReplyPacket& packet,
    std::uint64_t compressed,std::uint64_t expanded,std::function<bool()> cancelled,std::shared_ptr<void> job_lifetime,void* pinned_directory,std::function<void()> confirmed_completion){
    try{
        cancel_check(cancelled);inst::require(packet&&app::valid_qwk_board_id(packet.board_id)&&packet.board_id==app::canonical_qwk_board_id(packet.board_id)&&!packet.records.empty()&&packet.records.size()<=app::kQwkExpandedBytes/256&&
            compressed>0&&compressed<=app::kQwkCompressedBytes&&expanded>=128&&expanded<=app::kQwkExpandedBytes,"reply_spool_packet_bounds");
        bool canonical_job=false;
        if(job.native().size()<=160)canonical_job=job.is_absolute()&&inst::canonical_root(job)==job;
        else if(pinned_directory){
            // The accepted INSTALL root cap is160, not the length of its
            // host-generated Work/qwk-<32hex> child. Preserve that root policy;
            // allow only this fixed bounded descendant with a pinned handle.
            const auto root=job.parent_path().parent_path();const auto leaf=job.filename().native();
            canonical_job=root.is_absolute()&&inst::canonical_root(root)==root&&job.parent_path().filename()==L"Work"&&leaf.size()==36&&leaf.substr(0,4)==L"qwk-"&&
                std::all_of(leaf.begin()+4,leaf.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F');})&&root/L"Work"/leaf==job&&job.native().size()<=202;
        }
        inst::require(canonical_job,"reply_spool_host_path");
        auto source=std::make_shared<SealedSource>();source->job_lifetime=std::move(job_lifetime);source->cancellation=cancelled;source->confirmed_completion=std::move(confirmed_completion);source->metadata={packet.board_id,{},packet.records.size(),compressed,expanded};
        if(pinned_directory){
            // Duplicate the host's SAME pinned object, reducing access. A
            // name-based reopen without delete sharing conflicts with its
            // retained DELETE cleanup access. No sharing flag is broadened.
            std::array<wchar_t,32768> final_path{};const auto n=GetFinalPathNameByHandleW(pinned_directory,final_path.data(),static_cast<DWORD>(final_path.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
            const auto expected=L"\\\\?\\"+job.native();BY_HANDLE_FILE_INFORMATION info{};
            inst::require(n>0&&n<final_path.size()&&CompareStringOrdinal(final_path.data(),static_cast<int>(n),expected.data(),static_cast<int>(expected.size()),TRUE)==CSTR_EQUAL&&GetFileInformationByHandle(pinned_directory,&info)&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"reply_spool_host_directory_identity");
            HANDLE duplicate{};inst::require(DuplicateHandle(GetCurrentProcess(),pinned_directory,GetCurrentProcess(),&duplicate,FILE_READ_ATTRIBUTES|READ_CONTROL,FALSE,0),"reply_spool_duplicate_directory");source->directory.reset(duplicate);
        }else source->directory=inst::open_object(job,true,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        const auto assessment=sfx::platform::win32::WindowsStorageDaclPolicy(owner).assess(job,sfx::platform::win32::StorageDaclObjectKind::directory);
        inst::require(assessment.status&&assessment.effective_access&&assessment.private_access,"reply_spool_private_directory");
        source->directory_security=inst::mutable_descriptor(owner);source->file_security=qwk_native_detail::private_file_descriptor(owner);
        inst::require(inst::exact_security(source->directory.get(),source->directory_security.get()),"reply_spool_fixed_directory_descriptor");
        std::uint64_t field_bytes{};for(const auto& r:packet.records){const auto values=fields(r.message);for(std::size_t n=0;n<values.size();++n){inst::require(values[n]->size()<=maximums[n]&&values[n]->size()<=expanded-field_bytes,"reply_spool_field_bounds");field_bytes+=values[n]->size();}}
        const std::uint64_t normalized=magic.size()+1+packet.board_id.size()+24+32+packet.records.size()*44+field_bytes;
        inst::require(normalized<=app::kQwkExpandedBytes+packet.records.size()*44+128,"reply_spool_normalized_bounds");
        inst::require(sodium_init()>=0&&crypto_hash_sha256(source->metadata.logical_sha256.data(),reinterpret_cast<const unsigned char*>(packet.logical_identity.data()),packet.logical_identity.size())==0,"reply_spool_logical_digest");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),source->file_security.get(),FALSE};Creation creation;
        {sfx::platform::win32::ServiceFileOwnerScope scope(owner);inst::require(scope.valid(),"reply_spool_owner_scope_failed");
            creation.file.reset(CreateFileW((job/L"reply.spool").c_str(),GENERIC_READ|GENERIC_WRITE|DELETE|READ_CONTROL,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));}
        inst::require(creation.file.get()!=INVALID_HANDLE_VALUE,"reply_spool_create_new");
        BY_HANDLE_FILE_INFORMATION before{};inst::require(GetFileInformationByHandle(creation.file.get(),&before)&&before.nNumberOfLinks==1&&!(before.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),"reply_spool_new_regular_file");
        write(creation.file.get(),magic,source->cancellation);number(creation.file.get(),packet.board_id.size(),1,source->cancellation);write(creation.file.get(),packet.board_id,source->cancellation);
        number(creation.file.get(),packet.records.size(),8,source->cancellation);number(creation.file.get(),compressed,8,source->cancellation);number(creation.file.get(),expanded,8,source->cancellation);write(creation.file.get(),{reinterpret_cast<const char*>(source->metadata.logical_sha256.data()),32},source->cancellation);
        for(const auto& r:packet.records){const auto& m=r.message;number(creation.file.get(),m.conference,2,source->cancellation);number(creation.file.get(),m.reference,4,source->cancellation);number(creation.file.get(),m.private_mail?1:0,1,source->cancellation);number(creation.file.get(),r.command?(*r.command==app::QwkScanCommand::add?1:2):0,1,source->cancellation);
            for(const auto* v:fields(m)){number(creation.file.get(),v->size(),4,source->cancellation);write(creation.file.get(),*v,source->cancellation);}}
        LARGE_INTEGER length{};inst::require(GetFileSizeEx(creation.file.get(),&length)&&length.QuadPart==static_cast<LONGLONG>(normalized)&&FlushFileBuffers(creation.file.get()),"reply_spool_flush_exact_length");
        HANDLE read_handle{};inst::require(DuplicateHandle(GetCurrentProcess(),creation.file.get(),GetCurrentProcess(),&read_handle,GENERIC_READ|DELETE|READ_CONTROL,FALSE,0),"reply_spool_readonly_duplicate");source->file.reset(read_handle);
        // Close the last writing handle WITHOUT deleting the sealed object.
        creation.file.reset();inst::require(GetFileInformationByHandle(source->file.get(),&source->identity)&&source->identity.nNumberOfLinks==1,"reply_spool_sealed_identity");source->size=normalized;
        // Assess the pinned handle, not a name-based reopen that conflicts with
        // its intentional no-write/no-delete sharing. This checks the exact
        // protected owner/DACL descriptor without releasing file ownership.
        inst::require(inst::exact_security(source->file.get(),source->file_security.get()),"reply_spool_private_file");
        inst::require(static_cast<bool>(source->rewind()),"reply_spool_initial_readback");
        return {app::StorageStatus::success(),std::static_pointer_cast<app::IQwkReplySource>(source)};
    }catch(const std::exception& e){bool is_cancelled=true;try{is_cancelled=cancelled&&cancelled();}catch(...){}
        const std::string_view phase=e.what();const auto diagnostic=phase.size()<=64&&phase.substr(0,12)=="reply_spool_"?std::string(phase):std::string("protected reply spool could not be sealed");
        return {{is_cancelled?app::StorageErrorCode::shutting_down:app::StorageErrorCode::io,diagnostic},std::nullopt};
    }catch(...){bool is_cancelled=true;try{is_cancelled=cancelled&&cancelled();}catch(...){}
        return {{is_cancelled?app::StorageErrorCode::shutting_down:app::StorageErrorCode::io,"protected reply spool could not be sealed"},std::nullopt};}
}
} // namespace sfx::apps::runtime
