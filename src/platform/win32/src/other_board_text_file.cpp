#include "sfx/platform/win32/other_board_text_file.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"
#include <algorithm>
#include <cstring>

namespace sfx::platform::win32 {
namespace {
namespace app=application;namespace ins=installation;
using Code=app::StorageErrorCode;
std::vector<ins::Handle> pin(const std::filesystem::path& root,const std::wstring& owner) {
    std::vector<std::filesystem::path> paths;
    for(auto p=root/L"Config";!p.empty();) {paths.push_back(p);auto parent=p.parent_path();if(parent==p)break;p=parent;}
    std::reverse(paths.begin(),paths.end());std::vector<ins::Handle> held;
    for(const auto& p:paths)held.push_back(ins::open_object(p,true,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE));
    ins::require(WindowsStorageDaclPolicy(owner).assess(root/L"Config",StorageDaclObjectKind::directory).status.succeeded(),"otherbbs_directory_security_invalid");
    return held;
}
struct Snapshot {ins::Handle held;std::string bytes;std::int64_t revision{};std::vector<app::OtherBoardValues> rows;};
Snapshot load(const std::filesystem::path& path,const std::wstring& owner) {
    // Deny in-place writers while reading. Replacement-aware CAS below also
    // catches editor saves that replace the file rather than truncate it.
    Snapshot result;result.held=ins::open_object(path,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_DELETE);
    ins::require(WindowsStorageDaclPolicy(owner).assess(path,StorageDaclObjectKind::file,true).status.succeeded(),"otherbbs_file_security_invalid");
    LARGE_INTEGER size{};ins::require(GetFileSizeEx(result.held.get(),&size) && size.QuadPart>=0 &&
        static_cast<unsigned long long>(size.QuadPart)<=app::kOtherBoardTextMaximum,"otherbbs_file_size_invalid");
    result.bytes.resize(static_cast<std::size_t>(size.QuadPart));DWORD count{};
    ins::require(ReadFile(result.held.get(),result.bytes.data(),static_cast<DWORD>(result.bytes.size()),&count,nullptr) && count==result.bytes.size(),"otherbbs_file_read_failed");
    const auto parsed=app::parse_other_board_text(result.bytes);ins::require(parsed.has_value(),"otherbbs_text_invalid_check_Config_otherbbs_txt");
    result.rows=*parsed;
    const auto hash=ins::sha256({result.bytes.begin(),result.bytes.end()});
    // Bounded positive whole-file revision compatible with the existing IPC.
    result.revision=std::stoll(hash.substr(0,15),nullptr,16)+1;
    return result;
}
void save(const std::filesystem::path& path,const std::wstring& owner,Snapshot& expected,std::string_view bytes) {
    const auto temp=path.parent_path()/(L"otherbbs-pending-"+ins::unique_id()+L".tmp");
    auto descriptor=ins::mutable_descriptor(owner);ins::create_file(temp,{bytes.begin(),bytes.end()},descriptor.get());
    ins::require(WindowsStorageDaclPolicy(owner).apply_for_publication(temp,StorageDaclObjectKind::file,path).succeeded(),"otherbbs_temp_security_failed");
    auto staged=ins::open_object(temp,false,GENERIC_WRITE|DELETE|READ_CONTROL,FILE_SHARE_READ);
    ins::require(FlushFileBuffers(staged.get())!=FALSE,"otherbbs_flush_failed_pending_preserved");
    auto current=load(path,owner);
    ins::require(current.bytes==expected.bytes && ins::file_id(current.held.get())==ins::file_id(expected.held.get()) &&
        ins::volume_serial(current.held.get())==ins::volume_serial(expected.held.get()),"otherbbs_selection_changed_pending_preserved");
    // Win7-compatible replacement requires the destination handles closed.
    // Parent paths remain pinned and in-process writers remain serialized.
    // Recheck immediately before replacement, refusing stale selections.
    current.held.reset();expected.held.reset();
    const auto name=path.native();std::vector<std::uint8_t> data(sizeof(FILE_RENAME_INFO)+(name.size()+1)*sizeof(wchar_t));
    auto rename=reinterpret_cast<FILE_RENAME_INFO*>(data.data());rename->ReplaceIfExists=TRUE;
    rename->FileNameLength=static_cast<DWORD>(name.size()*sizeof(wchar_t));std::copy(name.begin(),name.end(),rename->FileName);
    if(!SetFileInformationByHandle(staged.get(),FileRenameInfo,rename,static_cast<DWORD>(data.size())))
        throw std::runtime_error("otherbbs_publish_failed_pending_preserved_"+std::to_string(GetLastError()));
}
}
void initialize_other_board_text(const std::filesystem::path& root,const std::wstring& owner) {
    auto held=pin(root,owner);const auto path=root/L"Config"/L"otherbbs.txt";
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES){
        auto file=ins::open_object(path,false);
        ins::require(WindowsStorageDaclPolicy(owner).assess(path,StorageDaclObjectKind::file,true).status.succeeded(),"otherbbs_file_security_invalid");
        return; // Bad text disables this list, not unrelated caller services.
    }
    ins::require(GetLastError()==ERROR_FILE_NOT_FOUND,"otherbbs_initialization_failed");
    auto descriptor=ins::mutable_descriptor(owner);ins::create_file(path,{},descriptor.get());
    ins::require(WindowsStorageDaclPolicy(owner).apply(path,StorageDaclObjectKind::file).succeeded(),"otherbbs_initialization_security_failed");
}
app::StorageResult<app::OtherBoardPage> WindowsOtherBoardTextStorage::read(std::int64_t after) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        if(after<0 || after>99999)return {{Code::invalid_argument,"otherbbs_cursor_invalid"},{}};
        auto held=pin(root_,owner_);const auto file=load(root_/L"Config"/L"otherbbs.txt",owner_);
        app::OtherBoardPage page;
        const auto begin=std::min<std::size_t>(static_cast<std::size_t>(after),file.rows.size());
        const auto end=std::min(file.rows.size(),begin+app::kOtherBoardPageSize);
        for(auto n=begin;n<end;++n)page.entries.push_back({static_cast<std::int64_t>(n+1),file.revision,file.rows[n]});
        page.has_more=end<file.rows.size();return {{},page};
    }catch(const std::exception& e){return {{Code::io,e.what()},{}};}
}
app::StorageResult<app::OtherBoardLogRecord> WindowsOtherBoardTextStorage::change(
    const app::ClassicInformationChange& change,const app::ClassicInformationActor& actor,std::string_view actor_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        if(!app::valid_classic_information_change(change) || change.action==app::ClassicInformationAction::opening_date)
            return {{Code::invalid_argument,"otherbbs_change_invalid"},{}};
        auto held=pin(root_,owner_);const auto path=root_/L"Config"/L"otherbbs.txt";auto snapshot=load(path,owner_);
        auto rows=snapshot.rows;std::string name;
        if(change.action==app::ClassicInformationAction::add_board) {
            if(rows.size()>=app::kOtherBoardMaximum)return {{Code::full,"otherbbs_list_full"},{}};
            rows.push_back(change.values);name=change.values.name;
        }else {
            if(change.expected_revision!=snapshot.revision || change.id<1 || static_cast<std::size_t>(change.id)>rows.size())
                return {{Code::conflict,"otherbbs_selection_changed"},{}};
            const auto at=static_cast<std::size_t>(change.id-1);name=rows[at].name;
            if(change.action==app::ClassicInformationAction::remove_board)rows.erase(rows.begin()+at);
            else {rows[at]=change.values;name=change.values.name;}
        }
        save(path,owner_,snapshot,app::encode_other_board_text(rows));
        return {{},app::OtherBoardLogRecord{change.action,name,std::string(actor_name),actor.caller,actor.now_utc,actor.event_id}};
    }catch(const std::exception& e){return {{Code::io,e.what()},{}};}
}
}
