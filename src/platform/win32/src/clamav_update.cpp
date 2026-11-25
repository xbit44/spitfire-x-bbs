#include "sfx/platform/win32/clamav_update.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/clamav_resources.hpp"
#include "sfx/platform/win32/clamav_status_gate.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>
#include <sddl.h>
#include <aclapi.h>
#include "sfx/platform/win32/storage_dacl_policy.hpp"

namespace sfx::platform::win32 {
namespace {
namespace ins=installation;using ins::require;using Path=std::filesystem::path;
struct Child {
    ins::Handle job,process,thread;bool* reaped{};
    ~Child(){if(process){if(WaitForSingleObject(process.get(),0)!=WAIT_OBJECT_0&&job)TerminateJobObject(job.get(),1);if(reaped)*reaped=WaitForSingleObject(process.get(),5000)==WAIT_OBJECT_0;}}
};
struct Pipe {
    ins::Handle read,write;bool eof{};std::string tail;
    Pipe(){SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE r{},w{};require(CreatePipe(&r,&w,&sa,4096)!=FALSE,"clamav_update_pipe");read.reset(r);write.reset(w);require(SetHandleInformation(read.get(),HANDLE_FLAG_INHERIT,0)!=FALSE,"clamav_update_pipe_private");}
    // Output is discarded, not accumulated. Bound each drain so a noisy vendor
    // cannot starve cancellation/deadline checks; normal progress is not an error.
    void drain(){std::size_t drained{};while(!eof&&drained<65536){DWORD available{};if(!PeekNamedPipe(read.get(),nullptr,0,nullptr,&available,nullptr)){require(GetLastError()==ERROR_BROKEN_PIPE,"clamav_update_pipe_read");eof=true;break;}if(!available)break;
        std::array<char,4096> data{};DWORD count{};require(ReadFile(read.get(),data.data(),std::min<DWORD>(available,4096),&count,nullptr)&&count&&count<=data.size(),"clamav_update_pipe_read");drained+=count;
        for(DWORD n=0;n<count;++n){const auto c=static_cast<unsigned char>(data[n]);tail+=c>='A'&&c<='Z'?static_cast<char>(c-'A'+'a'):c>=32&&c<127?static_cast<char>(c):' ';}if(tail.size()>8192)tail.erase(0,tail.size()-8192);}}
};
struct Attributes {std::vector<std::uint8_t> bytes;LPPROC_THREAD_ATTRIBUTE_LIST value{};~Attributes(){if(value)DeleteProcThreadAttributeList(value);}};
std::wstring quote(const Path& p){const auto& s=p.native();require(!s.empty()&&s.back()!=L'\\'&&s.find_first_of(L"\"\r\n")==std::wstring::npos,"clamav_update_argument");return L"\""+s+L"\"";}
void bounded_tree(const Path& root,unsigned depth,std::uint64_t& bytes,std::size_t& entries){
    require(depth<=16,"clamav_update_tree_depth");WIN32_FIND_DATAW item{};const auto find=FindFirstFileW((root/L"*").c_str(),&item);require(find!=INVALID_HANDLE_VALUE,"clamav_update_tree_read");
    struct Close{HANDLE h;~Close(){FindClose(h);}} close{find};
    do{const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;require(++entries<=8192&&!(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"clamav_update_tree_redirect_or_bound");
        if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)bounded_tree(root/name,depth+1,bytes,entries);
        else{const auto n=(std::uint64_t(item.nFileSizeHigh)<<32)|item.nFileSizeLow;require(n<=2ULL*1024*1024*1024-bytes,"clamav_update_disk_bound");bytes+=n;}
    }while(FindNextFileW(find,&item));require(GetLastError()==ERROR_NO_MORE_FILES,"clamav_update_tree_read");
}
bool admit_day(const Path& directory,const std::wstring& owner,std::int64_t day){
    const auto sd=ins::mutable_descriptor(owner);SECURITY_ATTRIBUTES sa{sizeof(sa),sd.get(),FALSE};
    ins::Handle state;bool existed{};
    {ServiceFileOwnerScope scope(owner);require(scope.valid(),"clamav_update_owner_scope");
        state.reset(CreateFileW((directory/L"update-state.dat").c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,&sa,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        existed=GetLastError()==ERROR_ALREADY_EXISTS;}
    require(state.get()!=INVALID_HANDLE_VALUE,"clamav_update_state_open");
    BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(state.get(),&info)&&info.nNumberOfLinks==1&&!info.nFileSizeHigh&&info.nFileSizeLow<=80&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&ins::exact_security(state.get(),sd.get()),"clamav_update_state_identity");
    const std::string prefix="SFX-CLAMAV-UPDATE-1\n";std::array<char,80> bytes{};DWORD got{};require(ReadFile(state.get(),bytes.data(),80,&got,nullptr),"clamav_update_state_read");
    if(existed){const std::string text(bytes.data(),got);require(text.size()>prefix.size()+1&&text.rfind(prefix,0)==0&&text.back()=='\n',"clamav_update_state_invalid");
        std::int64_t old{};const auto parsed=std::from_chars(text.data()+prefix.size(),text.data()+text.size()-1,old);require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()-1&&old>=0,"clamav_update_state_invalid");if(day<=old)return false;}
    const auto next=prefix+std::to_string(day)+"\n";LARGE_INTEGER zero{};DWORD written{};
    require(SetFilePointerEx(state.get(),zero,nullptr,FILE_BEGIN)&&WriteFile(state.get(),next.data(),static_cast<DWORD>(next.size()),&written,nullptr)&&written==next.size()&&SetEndOfFile(state.get())&&FlushFileBuffers(state.get()),"clamav_update_state_publish");return true;
}
bool database_name(const std::string& name){return name=="main.cvd"||name=="main.cld"||name=="daily.cvd"||name=="daily.cld"||name=="bytecode.cvd"||name=="bytecode.cld"||name=="freshclam.dat";}
constexpr const wchar_t* status_name=L"SFX-update-status.dat";
struct History {std::int64_t attempt{},success{},changed{};ClamAvUpdateMode mode{ClamAvUpdateMode::automatic};std::string result;std::array<std::string,3> fingerprints{{"-","-","-"}};ClamAvDefinitionChange change{ClamAvDefinitionChange::unknown};};
std::string encode_history(const History& h,bool legacy=false){std::string out=(legacy?"SFX-CLAMAV-STATUS-1\n":"SFX-CLAMAV-STATUS-2\n")+std::to_string(h.attempt)+"\n"+std::to_string(h.success)+"\n"+(h.mode==ClamAvUpdateMode::manual?"manual":"automatic")+"\n"+h.result+"\n";for(const auto& f:h.fingerprints)out+=f+"\n";if(!legacy)out+=std::to_string(h.changed)+"\n"+std::to_string(static_cast<unsigned>(h.change))+"\n";return out;}
History decode_history(const std::vector<std::uint8_t>& raw){
    require(!raw.empty()&&raw.size()<=1024&&raw.back()=='\n',"clamav_status_invalid");std::string text(raw.begin(),raw.end());
    require(text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-. |\n")==std::string::npos,"clamav_status_invalid");
    std::istringstream stream(text);std::vector<std::string> lines;std::string line;while(std::getline(stream,line))lines.push_back(line);
    const bool legacy=lines.size()==8&&lines[0]=="SFX-CLAMAV-STATUS-1";
    require(legacy||(lines.size()==10&&lines[0]=="SFX-CLAMAV-STATUS-2"),"clamav_status_invalid");History h;
    const auto number=[](const std::string& s){std::int64_t v{};const auto p=std::from_chars(s.data(),s.data()+s.size(),v);require(p.ec==std::errc{}&&p.ptr==s.data()+s.size()&&v>=0&&v<=32503680000LL&&std::to_string(v)==s,"clamav_status_invalid");return v;};
    h.attempt=number(lines[1]);h.success=number(lines[2]);require(h.attempt>0&&h.success<=h.attempt&&(lines[3]=="manual"||lines[3]=="automatic"),"clamav_status_invalid");h.mode=lines[3]=="manual"?ClamAvUpdateMode::manual:ClamAvUpdateMode::automatic;
    h.result=lines[4];require(!h.result.empty()&&h.result.size()<100&&h.result.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos,"clamav_status_invalid");
    const std::array<std::string,3> bases{{"main","daily","bytecode"}};
    for(std::size_t n=0;n<3;++n){h.fingerprints[n]=lines[5+n];const auto& f=h.fingerprints[n];if(!h.success){require(f=="-","clamav_status_invalid");continue;}
        const auto a=f.find('|'),b=f.find('|',a==std::string::npos?0:a+1);require(a!=std::string::npos&&b!=std::string::npos&&(f.substr(0,a)==bases[n]+".cvd"||f.substr(0,a)==bases[n]+".cld")&&number(f.substr(a+1,b-a-1))>0&&f.size()-b-1==64&&f.substr(b+1).find_first_not_of("0123456789ABCDEF")==std::string::npos,"clamav_status_invalid");}
    if(!legacy){h.changed=number(lines[8]);const auto change=number(lines[9]);require(h.changed<=h.success&&change<=3&&(h.success>0||(change==0&&h.changed==0)),"clamav_status_invalid");h.change=static_cast<ClamAvDefinitionChange>(change);}
    require(encode_history(h,legacy)==text,"clamav_status_invalid");return h;
}
std::optional<History> read_history(const FilePayloadDirectory& config){if(config.absent("SFX-update-status.dat"))return {};const auto file=config.read("SFX-update-status.dat");return decode_history(file.read(0,1025));}
std::array<std::string,3> definition_fingerprints(const FilePayloadDirectory& definitions,const std::function<bool()>& cancelled){
    std::array<std::string,3> out;const std::array<std::string,3> bases{{"main","daily","bytecode"}};
        for(std::size_t n=0;n<3;++n){unsigned found{};for(const auto* extension:{".cvd",".cld"}){const auto name=bases[n]+extension;if(definitions.absent(name))continue;++found;const auto file=definitions.read(name);const auto identity=file.identity([&](std::uint64_t,std::uint64_t){return !cancelled||!cancelled();});require(identity.bytes>0,"clamav_update_empty_database");std::string hash;for(const auto b:identity.sha256){hash+="0123456789ABCDEF"[b>>4];hash+="0123456789ABCDEF"[b&15];}out[n]=name+"|"+std::to_string(identity.bytes)+"|"+hash;}require(found==1,"clamav_update_database_missing");}return out;
}
std::array<std::optional<ClamAvDatabaseVersion>,3> database_versions(const FilePayloadDirectory& definitions){
    std::array<std::optional<ClamAvDatabaseVersion>,3> out;std::size_t n{};
    for(const auto* base:{"main","daily","bytecode"}){for(const auto* ext:{".cvd",".cld"}){const auto name=std::string(base)+ext;if(!definitions.absent(name)){const auto file=definitions.read(name);out[n]=clamav_database_header(file.read(0,512));}}++n;}return out;
}
void write_history(const FilePayloadDirectory& config,const std::wstring& owner,const History& history){
    const auto bytes=encode_history(history);(void)decode_history({bytes.begin(),bytes.end()});void* raw{};require(ConvertStringSecurityDescriptorToSecurityDescriptorW((L"O:"+owner+L"D:P(A;;0x13019f;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)").c_str(),SDDL_REVISION_1,&raw,nullptr),"clamav_status_security");ins::Descriptor sd(raw);SECURITY_ATTRIBUTES sa{sizeof(sa),sd.get(),FALSE};
    ins::Handle file;
    {ServiceFileOwnerScope scope(owner);require(scope.valid(),"clamav_status_owner_scope");
        file.reset(CreateFileW((config.path()/status_name).c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,&sa,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));}
    require(file.get()!=INVALID_HANDLE_VALUE,"clamav_status_open");
    BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(file.get(),&info)&&info.nNumberOfLinks==1&&!info.nFileSizeHigh&&info.nFileSizeLow<=1024&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&ins::exact_security(file.get(),sd.get()),"clamav_status_identity");
    DWORD written{};require(WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&SetEndOfFile(file.get())&&FlushFileBuffers(file.get()),"clamav_status_publish");
}
void copy_database(const FilePayloadDirectory& from,const FilePayloadDirectory& to,const std::string& name,const std::function<void()>& poll){
    const auto progress=[&](std::uint64_t,std::uint64_t){poll();return true;};
    const auto source=from.read(name);const auto identity=source.identity(progress);require(identity.bytes>0&&identity.bytes<=2ULL*1024*1024*1024,"clamav_update_database_size");
    auto destination=to.create(name,identity.bytes);
    for(std::uint64_t offset=0;offset<identity.bytes;){poll();const auto bytes=source.read(offset);require(!bytes.empty(),"clamav_update_database_copy");destination.append(bytes.data(),bytes.size());offset+=bytes.size();}
    require(destination.identity(progress).sha256==identity.sha256,"clamav_update_database_copy");
}
void publish_database(const FilePayloadDirectory& staged,const FilePayloadDirectory& persistent,const std::string& name,bool& retain_scratch){
    auto next=staged.read(name,true);require(!next.read(0,1).empty(),"clamav_update_empty_database");
    std::optional<FilePayloadLease> previous;
    if(!persistent.absent(name)){previous.emplace(persistent.read(name,true));previous->rename_to(staged,"previous-"+name);}
    try{next.rename_to(persistent,name);}catch(...){
        // No replacement API or path-based deletion: a late foreign collision
        // remains untouched. Preserve the old exact object if recovery cannot
        // restore its absent name, rather than cleaning this job's scratch.
        if(previous)try{if(persistent.absent(name))previous->rename_to(persistent,name);else retain_scratch=true;}catch(...){retain_scratch=true;}
        throw;
    }
}
bool private_log_handle(HANDLE file,const std::wstring& owner){
    PSID actual_owner{};PACL acl{};PSECURITY_DESCRIPTOR raw{};
    if(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&actual_owner,nullptr,&acl,nullptr,&raw)!=ERROR_SUCCESS||!raw)return false;
    ins::Descriptor held(raw);void* expected_raw{};
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW((L"O:"+owner+L"D:P(A;;0x13019f;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)").c_str(),SDDL_REVISION_1,&expected_raw,nullptr))return false;
    ins::Descriptor expected(expected_raw);PSID expected_owner{};BOOL defaulted{};PACL expected_acl{};BOOL present{};
    if(!GetSecurityDescriptorOwner(expected.get(),&expected_owner,&defaulted)||!actual_owner||!EqualSid(actual_owner,expected_owner)||!GetSecurityDescriptorDacl(expected.get(),&present,&expected_acl,&defaulted)||!acl||acl->AceCount!=3)return false;
    std::array<bool,3> found{};
    for(DWORD n=0;n<3;++n){void* ace_raw{};if(!GetAce(acl,n,&ace_raw))return false;const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(ace_raw);
        if(ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE||(ace->Header.AceFlags!=0&&ace->Header.AceFlags!=INHERITED_ACE))return false;
        bool match{};for(DWORD k=0;k<3;++k){void* e_raw{};if(!GetAce(expected_acl,k,&e_raw))return false;const auto* e=static_cast<ACCESS_ALLOWED_ACE*>(e_raw);
            if(!found[k]&&EqualSid(const_cast<DWORD*>(&ace->SidStart),const_cast<DWORD*>(&e->SidStart))&&(ace->Mask==e->Mask||((ace->Header.AceFlags&INHERITED_ACE)&&k==0&&ace->Mask==0x1301ff))){found[k]=true;match=true;break;}}
        if(!match)return false;
    }return true;
}
}
std::optional<ClamAvDatabaseVersion> clamav_database_header(const std::vector<std::uint8_t>& bytes)noexcept{
    try{
        if(bytes.size()!=512)return {};std::string head(bytes.begin(),bytes.end());
        while(!head.empty()&&(head.back()==' '||head.back()=='\n'||head.back()=='\r'))head.pop_back();
        if(head.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789: +-_/=.")!=std::string::npos)return {};
        std::vector<std::string> fields;std::istringstream stream(head);std::string field;while(std::getline(stream,field,':'))fields.push_back(field);
        if(fields.size()!=9||fields[0]!="ClamAV-VDB")return {};
        const auto number=[](const std::string& s,std::uint64_t maximum)->std::optional<std::uint64_t>{std::uint64_t n{};const auto p=std::from_chars(s.data(),s.data()+s.size(),n);if(p.ec!=std::errc{}||p.ptr!=s.data()+s.size()||n>maximum||std::to_string(n)!=s)return {};return n;};
        const auto version=number(fields[2],UINT32_MAX),sigs=number(fields[3],UINT32_MAX),date=number(fields[8],32503680000ULL);
        if(!version||!*version||!sigs||!date||!*date)return {};
        return ClamAvDatabaseVersion{static_cast<std::uint32_t>(*version),static_cast<std::uint32_t>(*sigs),static_cast<std::int64_t>(*date)};
    }catch(...){return {};}
}
std::string clamav_change_text(ClamAvDefinitionChange change){
    switch(change){case ClamAvDefinitionChange::initial:return "Initial definitions installed and verified";case ClamAvDefinitionChange::updated:return "Definitions updated and verified";case ClamAvDefinitionChange::current:return "Already current - definitions verified";default:return "Definitions verified - change history unavailable";}
}
ClamAvLogView read_clamav_update_log(const Path& root,const std::wstring& owner)noexcept{
    ClamAvLogView view;if(!clamav_available())return view;
    try{
        const auto logs=FilePayloadDirectory::private_store(root/L"Logs"/L"ClamAV",owner);const auto path=logs.path()/L"freshclam.log";
        ins::Handle file(CreateFileW(path.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(file.get()==INVALID_HANDLE_VALUE){view.missing=GetLastError()==ERROR_FILE_NOT_FOUND;return view;}
        BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(file.get(),&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"clamav_log_identity");
        std::array<wchar_t,32768> final{};const auto count=GetFinalPathNameByHandleW(file.get(),final.data(),static_cast<DWORD>(final.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        require(count&&count<final.size()&&!_wcsicmp(final.data(),(L"\\\\?\\"+path.native()).c_str())&&private_log_handle(file.get(),owner),"clamav_log_security");
        LARGE_INTEGER length{};require(GetFileSizeEx(file.get(),&length)&&length.QuadPart>=0,"clamav_log_size");constexpr DWORD maximum=65536;
        LARGE_INTEGER offset{};offset.QuadPart=std::max<LONGLONG>(0,length.QuadPart-maximum);view.truncated=offset.QuadPart!=0;require(SetFilePointerEx(file.get(),offset,nullptr,FILE_BEGIN),"clamav_log_seek");
        std::string raw(static_cast<std::size_t>(std::min<LONGLONG>(maximum,length.QuadPart)),'\0');DWORD got{};require(ReadFile(file.get(),raw.data(),static_cast<DWORD>(raw.size()),&got,nullptr),"clamav_log_read");raw.resize(got);
        if(view.truncated){const auto end=raw.find('\n');raw=end==std::string::npos?std::string{}:raw.substr(end+1);}
        for(unsigned char c:raw){if(c=='\r')continue;if(c=='\n')view.text+="\r\n";else view.text+=c=='\t'||(c>=32&&c<127)?static_cast<char>(c):'?';}
        view.available=true;return view;
    }catch(...){return view;}
}
bool valid_clamav_update_status(const std::vector<std::uint8_t>& bytes)noexcept{try{(void)decode_history(bytes);return true;}catch(...){return false;}}
ClamAvStatus inspect_clamav_status(const Path& root,const std::wstring& owner,const std::wstring& id,const std::function<bool()>& cancelled)noexcept{
    if(!clamav_available()){ClamAvStatus disabled;disabled.diagnostic="clamav_host_unavailable";return disabled;}
    ClamAvStatus s;try{s.installed=clamav_installed(root);if(!s.installed){s.diagnostic="clamav_not_installed";return s;}
        if(clamav_upgrade_required(root,owner,id)){s.diagnostic="clamav_engine_upgrade_required";return s;}
        verify_clamav_installation(root,owner,id);s.verified=true;s.version=kClamAvPortableVersion;
        const auto config=FilePayloadDirectory::private_store(root/L"Config"/L"ClamAV",owner);const auto definitions=FilePayloadDirectory::private_store(root/L"Data"/L"ClamAV",owner);
        try{const auto work=FilePayloadDirectory::private_store(root/L"Work"/L"ClamAV",owner);const auto gate=clamav_status_gate(work,owner,cancelled);
            // History and definition hashes must describe the SAME locked state.
            std::optional<History> history;try{history=read_history(config);}catch(...){s.diagnostic="clamav_status_unavailable";}
            if(history){s.history_available=true;s.last_attempt=history->attempt;s.last_success=history->success;s.last_change=history->changed;s.change=history->change;s.last_mode=history->mode;s.last_result=history->result;}
            const auto names=definitions.names();for(const auto& name:names)require(database_name(name),"clamav_update_unexpected_database_file");std::uint64_t bytes{};std::size_t entries{};bounded_tree(definitions.path(),0,bytes,entries);require(entries==names.size(),"clamav_update_database_subdirectory");const auto fingerprints=definition_fingerprints(definitions,cancelled);s.definitions_present=true;s.definitions_verified=history&&history->success>0&&history->fingerprints==fingerprints;if(s.definitions_verified)s.databases=database_versions(definitions);}catch(const std::exception& e){s.diagnostic=std::string(e.what())=="scan_admission_busy"?"clamav_status_operation_busy":"clamav_definitions_unavailable";}
    }catch(...){s.diagnostic="clamav_status_unavailable";}return s;
}
ClamAvUpdateResult update_clamav_definitions(const Path& root,const std::wstring& owner,const std::wstring& id,std::int64_t now,
    const std::function<bool()>& cancelled,const std::function<void(std::uint64_t)>& activity,std::uint32_t deadline,ClamAvUpdateMode mode){
    ClamAvUpdateResult result;std::unique_ptr<ClamAvScratch> scratch;std::unique_ptr<ClamAvResourceLease> resource;bool retain_scratch{};std::optional<FilePayloadDirectory> history_config;History history;
    if(!clamav_available()){result.diagnostic="clamav_host_unavailable";return result;}
    try{
        require((service_file_owner_enabled(owner)||(!ins::elevated()&&owner==ins::operator_sid()))&&now>0&&deadline>0&&deadline<=180000&&(mode==ClamAvUpdateMode::automatic||mode==ClamAvUpdateMode::manual),"clamav_update_policy");
        const auto started=GetTickCount64();
        const auto poll=[&]{if(cancelled&&cancelled()){result.cancelled=true;throw std::runtime_error("clamav_update_cancelled");}require(GetTickCount64()-started<deadline,"clamav_update_timeout");};
        poll();
        require(!clamav_upgrade_required(root,owner,id),"clamav_engine_upgrade_required");
        verify_clamav_installation(root,owner,id);initialize_clamav_data(root,owner);
        auto configuration=FilePayloadDirectory::private_store(root/L"Config"/L"ClamAV",owner);
        auto definitions=FilePayloadDirectory::private_store(root/L"Data"/L"ClamAV",owner);
        auto logs=FilePayloadDirectory::private_store(root/L"Logs"/L"ClamAV",owner);
        auto config=configuration.read("freshclam.conf");const auto expected=clamav_update_configuration(root);
        require(config.read(0,expected.size()+1)==std::vector<std::uint8_t>(expected.begin(),expected.end()),"clamav_update_configuration_changed");
        const auto work=FilePayloadDirectory::private_store(root/L"Work"/L"ClamAV",owner);
        // Serialize BEFORE touching the active vendor log or creating a job.
        // poll() preserves the total deadline/cancel result while queued.
        resource=ClamAvResourceLease::acquire_update(work,owner,2ULL*1024*1024*1024,[&]{poll();return false;},
            [&](std::uint64_t){poll();if(activity)activity(GetTickCount64()-started);},deadline);
        // Existing fixed log names may be rotated by FreshClam, but never a
        // reparse point/hardlink to some other owner file.
        for(const auto* name:{"freshclam.log","freshclam.log.1"})if(!logs.absent(name)){auto held=logs.read(name);(void)held.object_identity();}
        scratch=ClamAvScratch::create(root/L"Work"/L"ClamAV",owner,true);
        auto directory=FilePayloadDirectory::private_store(scratch->path(),owner);
        if(mode==ClamAvUpdateMode::manual||admit_day(configuration.path(),owner,now/86400)){
            result.attempted=true;
            history_config=configuration;try{history=read_history(configuration).value_or(History{});}catch(...){history=History{};}
            // Refuse unknown prior debris or linked definition files before
            // permitting the external updater to open any persistent filename.
            for(const auto& name:definitions.names()){
                require(database_name(name),"clamav_update_unexpected_database_file");
                const auto pin=definitions.read(name);(void)pin.object_identity();
            }
            {std::uint64_t bytes{};std::size_t entries{};bounded_tree(definitions.path(),0,bytes,entries);require(entries==definitions.names().size(),"clamav_update_database_subdirectory");}
            // FreshClam creates tmp.* under its database directory, regardless
            // of TEMP. Run it in this newly owned Work job, not persistent Data.
            // Failed downloads cannot strand temporary directories in Data.
            for(const auto& name:definitions.names())copy_database(definitions,directory,name,poll);
            bool previous_verified{};std::array<std::string,3> previous_fingerprints;
            const auto previous_versions=database_versions(definitions);
            const auto prior_names=definitions.names();const bool had_definitions=std::any_of(prior_names.begin(),prior_names.end(),[](const std::string& name){return name!="freshclam.dat";});
            if(history.success>0)try{previous_fingerprints=definition_fingerprints(definitions,cancelled);previous_verified=previous_fingerprints==history.fingerprints;}catch(...){poll();}
            const auto engine=root/L"Bin"/L"Tools"/L"ClamAV";std::vector<ins::Handle> engine_pins;
            for(const auto& member:clamav_program_members())engine_pins.push_back(ins::open_object(engine/member.name,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ));
            Pipe output,error;SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};ins::Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));require(input.get()!=INVALID_HANDLE_VALUE,"clamav_update_input");
            auto gate=resource->inherit_lock(),budget=resource->inherit_budget();HANDLE handles[]{input.get(),output.write.get(),error.write.get(),gate.get(),budget.get()};
            Attributes attributes;SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);require(size>0&&size<65536,"clamav_update_attributes_size");attributes.bytes.resize(size);attributes.value=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());
            require(InitializeProcThreadAttributeList(attributes.value,1,0,&size)&&UpdateProcThreadAttribute(attributes.value,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr),"clamav_update_handle_allowlist");
            Child child;child.reaped=&result.process_reaped;child.job.reset(CreateJobObjectW(nullptr,nullptr));require(bool(child.job),"clamav_update_job");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PROCESS_MEMORY;limits.BasicLimitInformation.ActiveProcessLimit=1;limits.ProcessMemoryLimit=static_cast<SIZE_T>(resource->child_memory_limit());
            require(SetInformationJobObject(child.job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"clamav_update_job_limits");
            std::array<wchar_t,32768> windows{};const auto count=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));require(count&&count<windows.size(),"clamav_update_windows_path");
            const Path system=Path(windows.data())/L"System32";std::wstring environment=L"PATH="+system.native()+L'\0'+L"SystemRoot="+windows.data()+L'\0'+L"TEMP="+directory.path().native()+L'\0'+L"TMP="+directory.path().native()+L'\0'+L'\0';
            const auto exe=engine/L"freshclam.exe";auto command=quote(exe)+L" --config-file="+quote(config.path())+L" --datadir="+quote(directory.path())+L" --stdout --quiet";
            STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.StartupInfo.hStdInput=input.get();startup.StartupInfo.hStdOutput=output.write.get();startup.StartupInfo.hStdError=error.write.get();startup.lpAttributeList=attributes.value;
            PROCESS_INFORMATION process{};require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),engine.c_str(),&startup.StartupInfo,&process),"clamav_update_create");
            child.process.reset(process.hProcess);child.thread.reset(process.hThread);result.process_reaped=false;
            if(!AssignProcessToJobObject(child.job.get(),child.process.get())){TerminateProcess(child.process.get(),1);WaitForSingleObject(child.process.get(),2000);throw std::runtime_error("clamav_update_job_assignment");}
            resource->bind_child(child.process.get());output.write.reset();error.write.reset();require(ResumeThread(child.thread.get())!=static_cast<DWORD>(-1),"clamav_update_resume");child.thread.reset();
            const auto start=started;std::uint64_t last=UINT64_MAX;
            for(;;){output.drain();error.drain();const auto elapsed=GetTickCount64()-start;
                if(elapsed/500!=last){last=elapsed/500;std::uint64_t bytes{};std::size_t entries{};bounded_tree(directory.path(),0,bytes,entries);bounded_tree(definitions.path(),0,bytes,entries);if(activity)activity(elapsed);}
                if(WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0){DWORD exit_code{};require(GetExitCodeProcess(child.process.get(),&exit_code),"clamav_update_exit");result.exit_code=exit_code;output.drain();error.drain();if(output.eof&&error.eof)break;}
                if(cancelled&&cancelled()){result.cancelled=true;throw std::runtime_error("clamav_update_cancelled");}require(elapsed<deadline,"clamav_update_timeout");
                const auto wait=WaitForSingleObject(child.process.get(),25);require(wait==WAIT_OBJECT_0||wait==WAIT_TIMEOUT,"clamav_update_wait");}
            // Keep the vendor's cooldown state even after a completed failed
            // request. It is data, never executed or shown as a scan pass.
            if(!directory.absent("freshclam.dat")){const auto state=directory.read("freshclam.dat");require(state.identity().bytes<=4096,"clamav_update_vendor_state_size");}
            if(!directory.absent("freshclam.dat"))publish_database(directory,definitions,"freshclam.dat",retain_scratch);
            if(result.exit_code!=0){const auto text=output.tail+error.tail;require(text.find("cool-down")==std::string::npos&&text.find("cooldown")==std::string::npos&&text.find("429")==std::string::npos&&text.find("403")==std::string::npos,"clamav_update_vendor_cooldown");throw std::runtime_error("clamav_update_vendor_failed");}
            const auto names=directory.names();
            {std::uint64_t bytes{};std::size_t entries{};bounded_tree(directory.path(),0,bytes,entries);require(entries==names.size(),"clamav_update_unfinished_scratch");}
            for(const auto& name:names)require(database_name(name)||name=="previous-freshclam.dat","clamav_update_unexpected_database_file");
            for(const auto* base:{"main","daily","bytecode"}){std::size_t n{};for(const auto* suffix:{".cvd",".cld"}){const auto name=std::string(base)+suffix;if(std::find(names.begin(),names.end(),name)!=names.end()){const auto held=directory.read(name);require(!held.read(0,1).empty(),"clamav_update_empty_database");++n;}}require(n==1,"clamav_update_database_missing");}
            for(const auto& name:names)if(database_name(name))publish_database(directory,definitions,name,retain_scratch);
            // A format change (.cvd -> .cld or back) retires only the exact old
            // vendor database after its replacement is present and verified.
            for(const auto* base:{"main","daily","bytecode"})for(const auto* suffix:{".cvd",".cld"}){const auto name=std::string(base)+suffix;if(std::find(names.begin(),names.end(),name)==names.end()&&!definitions.absent(name)){auto old=definitions.read(name,true);old.rename_to(directory,"previous-"+name);}}
            history.fingerprints=definition_fingerprints(definitions,cancelled);history.success=now;
            const auto next_versions=database_versions(definitions);
            result.change=ClamAvDefinitionChange::unknown;
            if(!previous_verified&&!had_definitions)result.change=ClamAvDefinitionChange::initial;
            else if(previous_verified&&previous_fingerprints==history.fingerprints)result.change=ClamAvDefinitionChange::current;
            else if(previous_verified){
                bool comparable=true,same=true;
                for(std::size_t n=0;n<3;++n){if(!previous_versions[n]||!next_versions[n]){comparable=false;break;}const auto& a=*previous_versions[n];const auto& b=*next_versions[n];same=same&&a.version==b.version&&a.signatures==b.signatures&&a.published==b.published;}
                if(comparable)result.change=same?ClamAvDefinitionChange::current:ClamAvDefinitionChange::updated;
            }
            history.change=result.change;if(result.change==ClamAvDefinitionChange::initial||result.change==ClamAvDefinitionChange::updated)history.changed=now;
            if(next_versions[1])result.daily_version=next_versions[1]->version;
            result.succeeded=true;result.diagnostic="clamav_definitions_verified";
        }else result.diagnostic="clamav_update_already_attempted_today";
    }catch(const std::exception& e){const std::string message=e.what();const bool known=message.rfind("clamav_",0)==0||message.rfind("scan_",0)==0||message.rfind("file_",0)==0;result.diagnostic=known&&message.size()<100&&message.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos?message:"clamav_update_unavailable";}catch(...){result.diagnostic="clamav_update_unavailable";}
    if(!result.process_reaped){result.succeeded=false;result.diagnostic="clamav_update_child_exit_unconfirmed";}
    // Definition-result history is published while the shared operation gate is
    // still held. A torn/invalid record is never read as success. Scratch cleanup
    // has its own separately reported result and must reacquire admission.
    if(history_config&&resource&&result.process_reaped){try{history.attempt=now;history.mode=mode;history.result=result.diagnostic;write_history(*history_config,owner,history);}catch(...){result.succeeded=false;result.diagnostic="clamav_update_status_unavailable";}}
    // Do not release admission between publication and exact scratch cleanup:
    // a queued manual updater must not steal the gate and strand this job.
    if(scratch){result.cleanup_complete=!retain_scratch&&resource&&scratch->cleanup(*resource);if(!result.cleanup_complete){result.succeeded=false;result.diagnostic="clamav_update_scratch_retained";}}
    resource.reset();
    return result;
}
} // namespace sfx::platform::win32
