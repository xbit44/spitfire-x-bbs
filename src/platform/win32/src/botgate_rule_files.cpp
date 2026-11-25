#include "sfx/platform/win32/botgate_rule_files.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <algorithm>

namespace sfx::platform::win32 {
namespace {
namespace app=application;
namespace ins=installation;
bool missing(const std::filesystem::path& p) {
    if(GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES) return false;
    ins::require(GetLastError()==ERROR_FILE_NOT_FOUND,"botgate_file_access_failed"); return true;
}
std::vector<ins::Handle> pin(const std::filesystem::path& root,const std::wstring& owner,const wchar_t* folder) {
    std::vector<std::filesystem::path> paths;
    for(auto p=root/L"BotGate"/folder;!p.empty();) {paths.push_back(p);auto parent=p.parent_path();if(parent==p)break;p=parent;}
    std::reverse(paths.begin(),paths.end()); std::vector<ins::Handle> held;
    for(const auto& p:paths) held.push_back(ins::open_object(p,true,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE));
    WindowsStorageDaclPolicy policy(owner);
    ins::require(policy.assess(root/L"BotGate",StorageDaclObjectKind::directory).status.succeeded() &&
        policy.assess(root/L"BotGate"/folder,StorageDaclObjectKind::directory).status.succeeded(),"botgate_directory_security_invalid");
    return held;
}
std::string read(const std::filesystem::path& p,const std::wstring& owner,std::size_t max) {
    auto file=ins::open_object(p,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
    // Owner-dropped files may inherit the exact private parent rights. Still
    // require the designated owner, exact principals/rights and pinned parents.
    ins::require(WindowsStorageDaclPolicy(owner).assess(p,StorageDaclObjectKind::file,true).status.succeeded(),"botgate_file_security_invalid");
    LARGE_INTEGER size{}; ins::require(GetFileSizeEx(file.get(),&size) && size.QuadPart>=0 &&
        static_cast<unsigned long long>(size.QuadPart)<=max,"botgate_file_size_invalid");
    std::string text(static_cast<std::size_t>(size.QuadPart),'\0'); DWORD count{};
    ins::require(ReadFile(file.get(),text.data(),static_cast<DWORD>(text.size()),&count,nullptr) && count==text.size(),"botgate_file_read_failed");
    return text;
}
void write(const std::filesystem::path& p,const std::wstring& owner,std::string_view text) {
    if(!missing(p)) {auto held=ins::open_object(p,false);ins::require(WindowsStorageDaclPolicy(owner).assess(p,StorageDaclObjectKind::file,true).status.succeeded(),"botgate_file_security_invalid");}
    const auto temp=p.parent_path()/(L"pending-"+ins::unique_id());
    auto descriptor=ins::mutable_descriptor(owner);
    ins::create_file(temp,{text.begin(),text.end()},descriptor.get());
    ins::require(WindowsStorageDaclPolicy(owner).apply(temp,StorageDaclObjectKind::file).succeeded(),"botgate_write_security_failed");
    auto handle=ins::open_object(temp,false,GENERIC_WRITE|DELETE|READ_CONTROL,FILE_SHARE_READ);
    ins::require(FlushFileBuffers(handle.get())!=FALSE,"botgate_flush_failed");
    const auto target=p.native(); std::vector<std::uint8_t> data(sizeof(FILE_RENAME_INFO)+(target.size()+1)*sizeof(wchar_t));
    auto rename=reinterpret_cast<FILE_RENAME_INFO*>(data.data()); rename->ReplaceIfExists=TRUE;
    rename->FileNameLength=static_cast<DWORD>(target.size()*sizeof(wchar_t));std::copy(target.begin(),target.end(),rename->FileName);
    ins::require(SetFileInformationByHandle(handle.get(),FileRenameInfo,rename,static_cast<DWORD>(data.size()))!=FALSE,"botgate_publish_failed_pending_preserved");
}
}
std::string read_botgate_rule(const std::filesystem::path& root,const std::wstring& owner,unsigned index) {
    ins::require(index<app::kBotGateRuleNames.size(),"botgate_rule_index_invalid");
    auto held=pin(root,owner,L"can");const auto path=root/L"BotGate"/L"can"/app::kBotGateRuleNames[index];
    return missing(path) ? std::string{} : read(path,owner,1024*1024);
}
void save_botgate_rule(const std::filesystem::path& root,const std::wstring& owner,unsigned index,std::string_view text) {
    ins::require(index<app::kBotGateRuleNames.size() && index!=4,"botgate_rule_index_invalid");
    ins::require(app::parse_botgate_patterns(text,index==3).has_value(),"botgate_rules_invalid");
    auto held=pin(root,owner,L"can");write(root/L"BotGate"/L"can"/app::kBotGateRuleNames[index],owner,text);
}
void initialize_botgate_files(const std::filesystem::path& root,const std::wstring& owner,const app::BotGateConfiguration& legacy) {
    auto root_handle=ins::open_object(root/L"BotGate",true);
    ins::require(WindowsStorageDaclPolicy(owner).assess(root/L"BotGate",StorageDaclObjectKind::directory).status.succeeded(),"botgate_directory_security_invalid");
    auto descriptor=ins::mutable_descriptor(owner);
    for(const auto* dir:{L"can",L"geo",L"feeds"}) {
        const auto path=root/L"BotGate"/dir;
        if(missing(path)) ins::create_directory(path,descriptor.get());
    }
    auto held=pin(root,owner,L"can");
    const auto marker=root/L"BotGate"/L"can"/L"sfx-rules-v1.marker";
    if(!missing(marker)) {ins::require(read(marker,owner,64)=="SFX BotGate rules v1\n","botgate_migration_marker_invalid");return;}
    const std::array<std::string,6> old{{legacy.trusted_sources,legacy.manual_blocks,"-",legacy.domains,"-",legacy.feed_exceptions}};
    for(unsigned i=0;i<old.size();++i) {
        const auto path=root/L"BotGate"/L"can"/app::kBotGateRuleNames[i];
        std::string lines;
        if(old[i]!="-") {
            lines=old[i];std::replace(lines.begin(),lines.end(),';','\n');lines+='\n';
            if(i==3) { // Old domain rules included the domain and its subdomains.
                std::string expanded;std::size_t begin=0;
                while(begin<lines.size()) {const auto end=lines.find('\n',begin);const auto domain=lines.substr(begin,end-begin);expanded+=domain+"\n*."+domain+"\n";begin=end+1;}
                lines=std::move(expanded);
            }
        }
        if(!missing(path)) {
            const auto existing=read(path,owner,1024*1024);
            // A retry accepts only the exact already-published migrated list.
            ins::require(lines.empty() || existing==lines,"botgate_migration_conflict_preserved");
        } else write(path,owner,lines);
    }
    write(marker,owner,"SFX BotGate rules v1\n");
}
void save_botgate_temporary(const std::filesystem::path& root,const std::wstring& owner,std::string_view text) {
    ins::require(app::parse_botgate_patterns(text,false,true).has_value(),"botgate_temporary_rules_invalid");
    auto held=pin(root,owner,L"can");write(root/L"BotGate"/L"can"/L"temp_ip.can",owner,text);
}
app::BotGateRuleSnapshot load_botgate_files(const std::filesystem::path& root,const std::wstring& owner) {
    app::BotGateRuleSnapshot result;
    try {
        for(unsigned i=0;i<result.lists.size();++i) {
            auto text=read_botgate_rule(root,owner,i);
            auto parsed=app::parse_botgate_patterns(text,i==3,i==4);
            ins::require(parsed.has_value(),"botgate_rule_parse_failed");result.lists[i]=std::move(*parsed);
        }
        auto held=pin(root,owner,L"geo");std::size_t total=0;
        for(const auto& entry:std::filesystem::directory_iterator(root/L"BotGate"/L"geo")) {
            auto extension=entry.path().extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),::towlower);
            if(extension!=L".txt") continue;
            ins::require(result.geo.size()<64,"botgate_geo_file_limit");
            const auto name=entry.path().filename().string();
            ins::require(name.size()<=64 && std::all_of(name.begin(),name.end(),[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_';}),"botgate_geo_name_invalid");
            auto bytes=read(entry.path(),owner,16*1024*1024);total+=bytes.size();
            ins::require(total<=32*1024*1024,"botgate_geo_total_limit");
            auto parsed=app::parse_botgate_geo(name,bytes);ins::require(parsed.has_value(),"botgate_geo_parse_failed");
            result.geo.push_back(std::move(*parsed));
        }
        result.ready=true;result.status="rules_loaded";
    } catch(const std::exception& e) {result.status=e.what();}
    return result;
}
}
