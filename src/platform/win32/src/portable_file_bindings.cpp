#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include <sddl.h>
#include <sodium.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
namespace sfx::platform::win32 {
namespace {
namespace fs=std::filesystem;namespace inst=installation;using inst::require;using inst::Handle;
constexpr std::size_t max_bytes=128U*1024U*1024U,max_records=1000000;
constexpr auto snapshot_leaf=L"file-identities.v1";
using ObjectKey=std::tuple<DWORD,std::uint64_t,std::uint64_t>;
struct Record {std::string relative,logical_volume,logical_id;DWORD volume{};std::uint64_t native_id{},created{},written{},bytes{};bool active{true};std::array<unsigned char,32> digest{};};
struct Snapshot {fs::path root;DWORD volume{};std::uint64_t root_id{};std::vector<fs::path> roots;std::vector<Record> records;};
struct Context {Snapshot snapshot;std::map<ObjectKey,application::FileObjectIdentity> objects;};
std::mutex context_mutex,publication_mutex;std::shared_ptr<const Context> active_context;
std::map<fs::path,std::set<ObjectKey>> created_objects;
std::map<fs::path,std::set<std::pair<std::string,std::string>>> removed_objects;
std::uint64_t ticks(FILETIME value){return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
std::string hex(const unsigned char* bytes,std::size_t count){constexpr char digits[]="0123456789ABCDEF";std::string out;out.reserve(count*2);for(std::size_t n=0;n<count;++n){out+=digits[bytes[n]>>4];out+=digits[bytes[n]&15];}return out;}
std::string hex(std::string_view value){return hex(reinterpret_cast<const unsigned char*>(value.data()),value.size());}
std::vector<unsigned char> unhex(std::string_view value){require(value.size()%2==0&&value.size()<=8192,"file_identity_hex_invalid");const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("file_identity_hex_invalid");};std::vector<unsigned char> bytes;bytes.reserve(value.size()/2);for(std::size_t n=0;n<value.size();n+=2)bytes.push_back(static_cast<unsigned char>((digit(value[n])<<4)|digit(value[n+1])));return bytes;}
std::string decode_text(std::string_view value){const auto bytes=unhex(value);return {bytes.begin(),bytes.end()};}
std::uint64_t number(std::string_view value){std::uint64_t out{};const auto r=std::from_chars(value.data(),value.data()+value.size(),out);require(!value.empty()&&r.ec==std::errc{}&&r.ptr==value.data()+value.size(),"file_identity_number_invalid");return out;}
bool same(fs::path a,fs::path b){a.make_preferred();b.make_preferred();return !_wcsicmp(a.c_str(),b.c_str());}
bool inside(fs::path path,fs::path root){path.make_preferred();root.make_preferred();const auto& a=path.native();const auto& b=root.native();return a.size()>b.size()&&!_wcsnicmp(a.c_str(),b.c_str(),b.size())&&a[b.size()]==L'\\';}
fs::path normalized(fs::path path){path=path.lexically_normal();path.make_preferred();return path;}
bool safe_relative(const fs::path& path){if(path.empty()||path.is_absolute()||path.has_root_name()||normalized(path)!=path)return false;for(const auto& part:path){if(part==L"."||part==L".."||part.native().find_first_of(L":*?\"<>|")!=std::wstring::npos)return false;for(const auto c:part.native())if(c<L' '||c==127)return false;}return true;}
fs::path relative_under(const fs::path& path,const fs::path& root){
    require(inside(path,root),"file_identity_path_outside_root");
    // Windows prefix admission and suffix extraction use the same comparison.
    // path::lexically_relative compares component spelling case-sensitively.
    auto relative=fs::path(path.native().substr(root.native().size()+1));relative.make_preferred();
    require(safe_relative(relative),"file_identity_relative_path_invalid");return relative;
}
bool owned_relative(const fs::path& path){if(!safe_relative(path))return false;auto first=path.begin();if(first==path.end())return false;if(!_wcsicmp(first->c_str(),L"Files")||!_wcsicmp(first->c_str(),L"FTN"))return true;if(_wcsicmp(first->c_str(),L"Data"))return false;++first;return first!=path.end()&&(!_wcsicmp(first->c_str(),L"FileIntake")||!_wcsicmp(first->c_str(),L"FileRemoved"));}
bool owned_path(const fs::path& path){if(!safe_relative(path))return false;const auto first=path.begin();for(const auto* role:inst::mutable_directories)if(!_wcsicmp(first->c_str(),role))return true;return !_wcsicmp(first->c_str(),L"FTN");}
std::wstring native(const fs::path& path){return L"\\\\?\\"+path.native();}
Handle open(const fs::path& path,bool directory,DWORD access=GENERIC_READ,DWORD sharing=FILE_SHARE_READ){if(directory)access|=FILE_LIST_DIRECTORY;Handle h(CreateFileW(native(path).c_str(),access,sharing,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr));require(h.get()!=INVALID_HANDLE_VALUE,"file_identity_object_unavailable");BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(h.get(),&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory&&(directory||info.nNumberOfLinks==1),"file_identity_redirected_object_preserved");std::vector<wchar_t> final(32768);const auto count=GetFinalPathNameByHandleW(h.get(),final.data(),static_cast<DWORD>(final.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);require(count&&count<final.size()&&!_wcsicmp(std::wstring(final.data(),count).c_str(),native(path).c_str()),"file_identity_final_path_changed");return h;}
std::vector<Handle> pin_path(const fs::path& root,const fs::path& relative){std::vector<Handle> pins;auto path=root;pins.push_back(open(path,true,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE));for(auto i=relative.begin();i!=relative.end();){const auto part=*i++;if(i==relative.end())break;path/=part;pins.push_back(open(path,true,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE));}return pins;}
std::pair<DWORD,std::uint64_t> root_identity(const fs::path& root){auto h=open(root,true,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE);return {inst::volume_serial(h.get()),inst::file_id(h.get())};}
struct MacKey {std::array<unsigned char,32> value{};explicit MacKey(const PortableBoardKey& key){require(crypto_kdf_derive_from_key(value.data(),value.size(),1,"SFXFILES",key.key.bytes.data())==0,"file_identity_mac_key_unavailable");}~MacKey(){sodium_memzero(value.data(),value.size());}};
std::string encode(const Snapshot& snapshot,const PortableBoardKey& key){
    std::ostringstream out;out<<"SFX-FILE-IDENTITIES-1\n"<<hex(key.board.data(),key.board.size())<<'\n'<<hex(snapshot.root.u8string())<<'\n'<<snapshot.volume<<'\n'<<snapshot.root_id<<'\n'<<snapshot.roots.size()<<'\n'<<snapshot.records.size()<<'\n';
    for(const auto& root:snapshot.roots)out<<hex(root.u8string())<<'\n';
    for(const auto& record:snapshot.records){out<<record.logical_volume<<' '<<record.logical_id<<' '<<record.volume<<' '<<record.native_id<<' '<<record.created<<' '<<record.written<<' '<<record.bytes<<' '<<(record.active?1:0)<<' '<<hex(record.digest.data(),record.digest.size())<<' '<<hex(record.relative)<<'\n';require(static_cast<std::uint64_t>(out.tellp())<=max_bytes-69,"file_identity_snapshot_size_limit");}
    auto body=out.str();MacKey derived(key);std::array<unsigned char,32> mac{};crypto_auth_hmacsha256(mac.data(),reinterpret_cast<const unsigned char*>(body.data()),body.size(),derived.value.data());body+="MAC:"+hex(mac.data(),mac.size())+'\n';return body;
}
Snapshot decode(const std::vector<std::uint8_t>& bytes,const PortableBoardKey& key){
    require(bytes.size()>=69&&bytes.size()<=max_bytes,"file_identity_snapshot_size_invalid");const std::string_view all(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto body=all.substr(0,all.size()-69),trailer=all.substr(all.size()-69);require(trailer.substr(0,4)=="MAC:"&&trailer.back()=='\n',"file_identity_snapshot_authentication_invalid");const auto supplied=unhex(trailer.substr(4,64));MacKey derived(key);require(crypto_auth_hmacsha256_verify(supplied.data(),reinterpret_cast<const unsigned char*>(body.data()),body.size(),derived.value.data())==0,"file_identity_snapshot_authentication_failed_original_preserved");
    std::istringstream in{std::string(body)};std::string line;const auto next=[&]{require(static_cast<bool>(std::getline(in,line)),"file_identity_snapshot_truncated");return line;};require(next()=="SFX-FILE-IDENTITIES-1"&&next()==hex(key.board.data(),key.board.size()),"file_identity_snapshot_board_invalid");Snapshot result;result.root=normalized(fs::u8path(decode_text(next())));require(result.root.is_absolute()&&result.root.native().rfind(L"\\\\",0)!=0,"file_identity_source_root_invalid");const auto volume=number(next());require(volume<=MAXDWORD,"file_identity_volume_invalid");result.volume=static_cast<DWORD>(volume);result.root_id=number(next());const auto roots=number(next()),count=number(next());require(roots>0&&roots<=1024&&count<=max_records,"file_identity_snapshot_count_invalid");
    for(std::uint64_t n=0;n<roots;++n){auto root=normalized(fs::u8path(decode_text(next())));require(root.is_absolute()&&root.native().rfind(L"\\\\",0)!=0,"file_identity_source_root_invalid");result.roots.push_back(std::move(root));}
    std::set<std::string> paths;std::set<ObjectKey> objects;std::set<std::pair<std::string,std::string>> logical;
    for(std::uint64_t n=0;n<count;++n){std::istringstream row(next());Record record;std::string actual_volume,digest,relative,extra;unsigned active{};require(static_cast<bool>(row>>record.logical_volume>>record.logical_id>>actual_volume>>record.native_id>>record.created>>record.written>>record.bytes>>active>>digest>>relative)&&active<=1&&!(row>>extra),"file_identity_record_invalid");record.active=active!=0;require(number(record.logical_volume)<=MAXDWORD&&number(record.logical_id)>0,"file_identity_logical_id_invalid");const auto v=number(actual_volume);require(v<=MAXDWORD&&record.native_id>0,"file_identity_native_id_invalid");record.volume=static_cast<DWORD>(v);const auto hash=unhex(digest);require(hash.size()==record.digest.size(),"file_identity_digest_invalid");std::copy(hash.begin(),hash.end(),record.digest.begin());record.relative=decode_text(relative);const auto path=normalized(fs::u8path(record.relative));require(owned_relative(path)&&path.generic_u8string()==record.relative,"file_identity_relative_path_invalid");require(logical.insert({record.logical_volume,record.logical_id}).second,"file_identity_duplicate_logical_record");if(record.active)require(paths.insert(record.relative).second&&objects.insert({record.volume,record.native_id,record.created}).second,"file_identity_duplicate_active_record");result.records.push_back(std::move(record));}
    require(!std::getline(in,line),"file_identity_snapshot_trailing_data");return result;
}
std::optional<Snapshot> read_snapshot(const fs::path& root,const PortableBoardKey& key){auto pins=pin_path(root,fs::path(L"Config")/snapshot_leaf);const auto path=root/L"Config"/snapshot_leaf;const auto a=GetFileAttributesW(path.c_str());if(a==INVALID_FILE_ATTRIBUTES){require(GetLastError()==ERROR_FILE_NOT_FOUND,"file_identity_snapshot_unavailable");return {};}
    auto h=open(path,false);LARGE_INTEGER size{};require(GetFileSizeEx(h.get(),&size)&&size.QuadPart>=0&&size.QuadPart<=max_bytes,"file_identity_snapshot_size_invalid");std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));DWORD read{};require(ReadFile(h.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size(),"file_identity_snapshot_read_failed");return decode(bytes,key);}
void publish(const fs::path& root,const std::wstring& owner,const Snapshot& snapshot,const PortableBoardKey& key,const fs::path& output={}){
    const auto target=output.empty()?root/L"Config"/snapshot_leaf:output;require(output.empty()||(same(output.parent_path(),root/L"Work")&&output.filename().native().rfind(L"file-identities-",0)==0),"file_identity_output_scope_invalid");const auto text=encode(snapshot,key);auto pins=pin_path(root,target.lexically_relative(root));const auto staging=target.parent_path()/(L"file-identities-"+inst::unique_id()+L".pending");const auto sddl=L"O:"+owner+L"D:P(A;;FA;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)";PSECURITY_DESCRIPTOR raw{};require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"file_identity_output_security_failed");inst::Descriptor descriptor(raw);SECURITY_ATTRIBUTES security{sizeof(security),raw,FALSE};Handle file;
    {ServiceFileOwnerScope scope(owner);require(scope.valid(),"file_identity_output_owner_unavailable");file.reset(CreateFileW(native(staging).c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));}
    require(file.get()!=INVALID_HANDLE_VALUE,"file_identity_output_create_failed");DWORD written{};require(WriteFile(file.get(),text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(file.get()),"file_identity_output_incomplete_prior_preserved");const auto destination=native(target);std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+(destination.size()+1)*sizeof(wchar_t));auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());rename->ReplaceIfExists=output.empty()?TRUE:FALSE;rename->RootDirectory=nullptr;rename->FileNameLength=static_cast<DWORD>(destination.size()*sizeof(wchar_t));std::copy(destination.begin(),destination.end(),rename->FileName);require(SetFileInformationByHandle(file.get(),FileRenameInfo,rename,static_cast<DWORD>(buffer.size()))&&FlushFileBuffers(file.get()),"file_identity_output_publication_failed_prior_preserved");
}
void cancelled(const std::function<bool()>& check){require(!check||!check(),"file_identity_snapshot_cancelled_prior_preserved");}
Record inspect(const fs::path& root,const std::string& relative,Handle& held,const std::function<bool()>& check,const Record* previous=nullptr,std::vector<Handle>* retained_directories=nullptr){
    cancelled(check);const auto rel=normalized(fs::u8path(relative));require(owned_relative(rel),"file_identity_capture_path_invalid");auto pins=pin_path(root,rel);held=open(root/rel,false);BY_HANDLE_FILE_INFORMATION before{},after{};require(GetFileInformationByHandle(held.get(),&before),"file_identity_capture_info_unavailable");Record out;out.relative=relative;out.volume=before.dwVolumeSerialNumber;out.native_id=(std::uint64_t(before.nFileIndexHigh)<<32)|before.nFileIndexLow;out.created=ticks(before.ftCreationTime);out.written=ticks(before.ftLastWriteTime);out.bytes=(std::uint64_t(before.nFileSizeHigh)<<32)|before.nFileSizeLow;out.logical_volume=std::to_string(out.volume);out.logical_id=std::to_string(out.native_id);
    if(previous&&previous->volume==out.volume&&previous->native_id==out.native_id&&previous->created==out.created){out.logical_volume=previous->logical_volume;out.logical_id=previous->logical_id;if(previous->bytes==out.bytes&&previous->written==out.written){out.digest=previous->digest;if(retained_directories)*retained_directories=std::move(pins);return out;}}
    crypto_hash_sha256_state hash{};crypto_hash_sha256_init(&hash);std::array<unsigned char,65536> data{};std::uint64_t total{};for(;;){cancelled(check);DWORD n{};require(ReadFile(held.get(),data.data(),static_cast<DWORD>(data.size()),&n,nullptr),"file_identity_capture_read_failed");if(!n)break;total+=n;require(total<=out.bytes,"file_identity_capture_changed");crypto_hash_sha256_update(&hash,data.data(),n);}require(total==out.bytes&&GetFileInformationByHandle(held.get(),&after)&&after.dwVolumeSerialNumber==before.dwVolumeSerialNumber&&after.nFileIndexHigh==before.nFileIndexHigh&&after.nFileIndexLow==before.nFileIndexLow&&ticks(after.ftLastWriteTime)==out.written&&((std::uint64_t(after.nFileSizeHigh)<<32)|after.nFileSizeLow)==out.bytes,"file_identity_capture_changed");crypto_hash_sha256_final(&hash,out.digest.data());if(retained_directories)*retained_directories=std::move(pins);return out;
}
void note(PortableFileBindingResult& result,std::string code){++result.deferred;if(result.diagnostics.size()<32)result.diagnostics.push_back(std::move(code));}
}
PortableFileBindingResult configure_portable_file_bindings(const fs::path& root)noexcept{
    PortableFileBindingResult result;
    try{
        auto key=load_portable_board_key(root);if(!key)throw std::runtime_error("file_identity_board_key_unavailable");
        const auto snapshot=read_snapshot(root,*key.value);
        if(!snapshot)result.state=PortableFileBindingState::missing;
        else{
            const auto actual=root_identity(root);
            if(!same(snapshot->root,root)||snapshot->volume!=actual.first||snapshot->root_id!=actual.second)result.state=PortableFileBindingState::different_host;
            else{
                auto context=std::make_shared<Context>();context->snapshot=*snapshot;
                for(const auto& record:snapshot->records){
                    if(record.active){context->objects.emplace(ObjectKey{record.volume,record.native_id,record.created},application::FileObjectIdentity{record.logical_volume,record.logical_id});++result.recorded;}
                    else ++result.deferred;
                }
                result.state=PortableFileBindingState::ready;
                if(result.deferred)result.diagnostics.push_back("file_identity_unconfirmed_work_preserved");
                std::lock_guard<std::mutex> lock(context_mutex);active_context=std::move(context);return result;
            }
        }
    }catch(const std::exception& error){result.diagnostics.push_back(error.what());}
    std::lock_guard<std::mutex> lock(context_mutex);active_context.reset();return result;
}
application::FileObjectIdentity portable_logical_file_identity(const fs::path& path,HANDLE file){BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(file,&info),"file_identity_current_handle_unavailable");const auto id=(std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;application::FileObjectIdentity actual{std::to_string(info.dwVolumeSerialNumber),std::to_string(id)};std::shared_ptr<const Context> context;{std::lock_guard<std::mutex> lock(context_mutex);context=active_context;}if(!context||!inside(path,context->snapshot.root)||!owned_relative(relative_under(path,context->snapshot.root)))return actual;const auto found=context->objects.find({info.dwVolumeSerialNumber,id,ticks(info.ftCreationTime)});return found==context->objects.end()?actual:found->second;}
fs::path resolve_portable_owned_path(const fs::path& root,const fs::path& input){
    auto configured=input;configured.make_preferred();
    if(configured.is_relative()){auto relative=configured;relative.make_preferred();require(safe_relative(relative),"file_identity_configured_path_invalid");return normalized(root/relative);}
    std::shared_ptr<const Context> context;{std::lock_guard<std::mutex> lock(context_mutex);context=active_context;}
    if(!context||!same(context->snapshot.root,root))return configured;
    for(const auto& source:context->snapshot.roots){if(same(configured,source))return root;if(inside(configured,source)){const auto rel=relative_under(configured,source);if(owned_path(rel))return normalized(root/rel);}}return configured;
}
fs::path portable_logical_owned_path(const fs::path& root,const fs::path& path){
    const auto current=resolve_portable_owned_path(root,path);std::shared_ptr<const Context> context;{std::lock_guard<std::mutex> lock(context_mutex);context=active_context;}
    if(context&&same(context->snapshot.root,root)&&inside(current,root)){
        const auto relative=relative_under(current,root);if(owned_path(relative))return normalized(context->snapshot.roots.front()/relative);
    }return current;
}

void record_portable_new_file(const fs::path& path,HANDLE file){
    BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(file,&info),"file_identity_new_handle_unavailable");
    std::lock_guard<std::mutex> lock(context_mutex);
    if(active_context&&inside(path,active_context->snapshot.root)&&owned_relative(relative_under(path,active_context->snapshot.root)))
        created_objects[active_context->snapshot.root].emplace(info.dwVolumeSerialNumber,(std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow,ticks(info.ftCreationTime));
}
void record_portable_removed_file(const fs::path& path,const application::FileObjectIdentity& logical){
    std::lock_guard<std::mutex> lock(context_mutex);if(active_context&&inside(path,active_context->snapshot.root)&&owned_relative(relative_under(path,active_context->snapshot.root)))removed_objects[active_context->snapshot.root].emplace(logical.volume,logical.file_id);
}
bool portable_file_initialization_pending(const fs::path& root)noexcept{
    try{const auto proof=inst::read_file(root/L"Config"/L"file-identities.initialize",4096);const auto actual=root_identity(root);
        const auto expected=std::string("SFX-FILE-INITIALIZATION-1\n")+root.u8string()+'\n'+std::to_string(actual.first)+'\n'+std::to_string(actual.second)+'\n';
        return std::string(proof.begin(),proof.end())==expected;
    }catch(...){return false;}
}
PortableFileBindingResult refresh_portable_file_snapshot(const fs::path& root,const std::wstring& owner,bool initialize,const fs::path& output,const std::function<bool()>& check){
    std::lock_guard<std::mutex> lock(publication_mutex);PortableFileBindingResult result;
    try{
        auto key=load_portable_board_key(root);require(static_cast<bool>(key),"file_identity_board_key_unavailable");
        const auto prior=read_snapshot(root,*key.value);
        const bool initial_pending=portable_file_initialization_pending(root);
        if(!prior&&!initialize&&!initial_pending){result.state=PortableFileBindingState::missing;return result;}
        const auto actual=root_identity(root);
        if(prior)require(same(prior->root,root)&&prior->volume==actual.first&&prior->root_id==actual.second,"file_identity_copy_requires_preparation");
        Snapshot next;next.root=root;next.volume=actual.first;next.root_id=actual.second;next.roots=prior?prior->roots:std::vector<fs::path>{root};
        using Logical=std::pair<std::string,std::string>;
        std::map<ObjectKey,const Record*> known;std::set<Logical> used;std::set<std::string> unresolved_names;
        std::set<ObjectKey> fresh;std::set<Logical> retired;{std::lock_guard<std::mutex> context_lock(context_mutex);const auto scope=prior?prior->root:root;const auto c=created_objects.find(scope);if(c!=created_objects.end())fresh=c->second;const auto r=removed_objects.find(scope);if(r!=removed_objects.end())retired=r->second;}
        if(prior)for(const auto& record:prior->records){const ObjectKey object{record.volume,record.native_id,record.created};const auto found=known.find(object);if(found==known.end()||record.active)known[object]=&record;if(!record.active)unresolved_names.insert(record.relative);}
        std::vector<fs::path> pending{fs::path(L"Files"),fs::path(L"FTN"),fs::path(L"Data")/L"FileIntake",fs::path(L"Data")/L"FileRemoved"};
        std::vector<Handle> captured_files;std::vector<std::vector<Handle>> captured_directories;
        while(!pending.empty()){
            cancelled(check);const auto relative=pending.back();pending.pop_back();
            try{
                auto directory_pins=pin_path(root,relative/fs::path(L"placeholder"));WIN32_FIND_DATAW item{};
                const auto search=FindFirstFileW(native(root/relative/L"*").c_str(),&item);
                if(search==INVALID_HANDLE_VALUE){if(GetLastError()!=ERROR_FILE_NOT_FOUND)note(result,"file_identity_directory_unavailable:"+relative.generic_u8string());continue;}
                struct Find {HANDLE value;~Find(){FindClose(value);}} find{search};
                do{
                    cancelled(check);if(!wcscmp(item.cFileName,L".")||!wcscmp(item.cFileName,L".."))continue;
                    const auto child=relative/item.cFileName;
                    if(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT){note(result,"file_identity_redirected_work_preserved:"+child.generic_u8string());continue;}
                    if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){pending.push_back(child);continue;}
                    require(next.records.size()<max_records,"file_identity_snapshot_count_limit");
                    try{
                        auto metadata=open(root/child,false,FILE_READ_ATTRIBUTES,FILE_SHARE_READ);BY_HANDLE_FILE_INFORMATION info{};
                        require(GetFileInformationByHandle(metadata.get(),&info),"file_identity_capture_info_unavailable");
                        const ObjectKey object{info.dwVolumeSerialNumber,(std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow,ticks(info.ftCreationTime)};
                        const auto found=known.find(object);const auto name=child.generic_u8string();
                        const bool unresolved=unresolved_names.find(name)!=unresolved_names.end();
                        // An unavailable old object is not silently replaced by a
                        // similarly named foreign object. Keep its original proof
                        // for explicit preparation retry. Exclusive SFX creation
                        // proves a genuinely new namespace object, not an outcome.
                        if(found==known.end()&&unresolved&&fresh.find(object)==fresh.end()){note(result,"file_identity_unresolved_work_preserved:"+name);continue;}
                        Handle held;std::vector<Handle> directories;auto record=inspect(root,name,held,check,found==known.end()?nullptr:found->second,&directories);
                        used.emplace(record.logical_volume,record.logical_id);next.records.push_back(std::move(record));
                        captured_files.push_back(std::move(held));captured_directories.push_back(std::move(directories));
                    }catch(...){cancelled(check);note(result,"file_identity_work_unconfirmed_preserved:"+child.generic_u8string());}
                }while(FindNextFileW(search,&item));
                if(GetLastError()!=ERROR_NO_MORE_FILES)note(result,"file_identity_directory_scan_unconfirmed:"+relative.generic_u8string());
            }catch(...){cancelled(check);note(result,"file_identity_directory_unavailable:"+relative.generic_u8string());}
        }
        // Retain inactive expected-byte/logical-ID evidence through partial
        // scans and publication. Inactive entries never enter the live mapping.
        if(prior)for(const auto& record:prior->records)if(used.find({record.logical_volume,record.logical_id})==used.end()&&retired.find({record.logical_volume,record.logical_id})==retired.end()){
            if(record.active)note(result,"file_identity_previous_object_unconfirmed_preserved:"+record.relative);
            auto preserved=record;preserved.active=false;next.records.push_back(std::move(preserved));
        }
        require(next.records.size()<=max_records,"file_identity_snapshot_count_limit");cancelled(check);
        publish(root,owner,next,*key.value,output);result.state=PortableFileBindingState::ready;result.published=true;
        result.recorded=static_cast<std::size_t>(std::count_if(next.records.begin(),next.records.end(),[](const auto& record){return record.active;}));
        result.snapshot=output.empty()?root/L"Config"/snapshot_leaf:output;
        if(output.empty())require(configure_portable_file_bindings(root).state==PortableFileBindingState::ready,"file_identity_published_context_unavailable");
        if(output.empty()&&initial_pending&&!result.deferred){
            const auto completed=root/L"Config"/(L"file-identities-initialized-"+inst::unique_id()+L".confirmed");
            require(MoveFileExW((root/L"Config"/L"file-identities.initialize").c_str(),completed.c_str(),MOVEFILE_WRITE_THROUGH),"file_identity_initialization_acknowledgment_pending");
        }
    }catch(const std::exception& error){result.state=PortableFileBindingState::unavailable;result.diagnostics.push_back(error.what());}
    return result;
}
PortableFileBindingResult prepare_copied_file_bindings(const fs::path& root,const std::wstring& owner,const std::function<bool()>& check){
    std::lock_guard<std::mutex> lock(publication_mutex);PortableFileBindingResult result;
    try{
        auto key=load_portable_board_key(root);require(static_cast<bool>(key),"file_identity_board_key_unavailable");
        const auto source=read_snapshot(root,*key.value);
        if(!source){result.state=PortableFileBindingState::missing;note(result,"file_identity_snapshot_missing_affected_work_preserved");return result;}
        Snapshot next=*source;next.root=root;const auto actual=root_identity(root);next.volume=actual.first;next.root_id=actual.second;
        if(std::none_of(next.roots.begin(),next.roots.end(),[&](const auto& p){return same(p,root);}))next.roots.push_back(root);
        std::map<std::string,std::vector<std::size_t>> groups;
        for(std::size_t n=0;n<source->records.size();++n){groups[source->records[n].relative].push_back(n);next.records[n].active=false;}
        std::vector<Handle> pinned_files;std::vector<std::vector<Handle>> pinned_directories;
        for(const auto& group:groups){
            cancelled(check);
            try{
                Handle held;std::vector<Handle> directories;auto copied=inspect(root,group.first,held,check,nullptr,&directories);
                std::vector<std::size_t> active_matches,inactive_matches;
                for(const auto n:group.second){const auto& record=source->records[n];if(record.bytes==copied.bytes&&record.digest==copied.digest)(record.active?active_matches:inactive_matches).push_back(n);}
                const auto& matches=active_matches.empty()?inactive_matches:active_matches;
                if(matches.size()!=1){note(result,matches.empty()?"file_identity_changed_work_preserved:"+group.first:"file_identity_ambiguous_work_preserved:"+group.first);continue;}
                const auto selected=matches.front();copied.logical_volume=source->records[selected].logical_volume;copied.logical_id=source->records[selected].logical_id;
                next.records[selected]=std::move(copied);pinned_directories.push_back(std::move(directories));pinned_files.push_back(std::move(held));
            }catch(...){cancelled(check);note(result,"file_identity_unavailable_work_preserved:"+group.first);}
        }
        cancelled(check);publish(root,owner,next,*key.value);result.state=PortableFileBindingState::ready;result.published=true;
        result.recorded=static_cast<std::size_t>(std::count_if(next.records.begin(),next.records.end(),[](const auto& record){return record.active;}));
        result.snapshot=root/L"Config"/snapshot_leaf;
        require(configure_portable_file_bindings(root).state==PortableFileBindingState::ready,"file_identity_published_context_unavailable");
    }catch(const std::exception& error){result.state=PortableFileBindingState::unavailable;result.diagnostics.push_back(error.what());}
    return result;
}
}
