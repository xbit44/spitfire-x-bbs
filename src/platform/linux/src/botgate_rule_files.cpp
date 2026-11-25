#include "sfx/platform/linux/botgate_rule_files.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void owner_matches(const std::wstring& owner){require(geteuid()!=0&&owner==std::to_wstring(geteuid()),"botgate_owner_mismatch");}
bool exists(int directory,const char* name){struct stat info{};if(fstatat(directory,name,&info,AT_SYMLINK_NOFOLLOW)==0)return true;require(errno==ENOENT,"botgate_file_inspection_failed");return false;}
std::string read(int directory,const char* name,std::size_t bound){const auto bytes=read_private_file(directory,name,bound);return {bytes.begin(),bytes.end()};}
void write(int directory,const char* name,std::string_view text,bool replace){publish_private_file(directory,name,{text.begin(),text.end()},replace);}
}
std::string read_botgate_rule(const std::filesystem::path& root,const std::wstring& owner,unsigned index){
    owner_matches(owner);require(index<app::kBotGateRuleNames.size(),"botgate_rule_index_invalid");
    auto directory=open_owned_directory(root/"botgate/can");const auto* name=app::kBotGateRuleNames[index];
    return exists(directory.get(),name)?read(directory.get(),name,1024*1024):std::string{};
}
void initialize_botgate_files(const std::filesystem::path& root,const std::wstring& owner,const app::BotGateConfiguration& legacy){
    owner_matches(owner);auto parent=open_owned_directory(root);
    require(mkdirat(parent.get(),"botgate",0700)==0||errno==EEXIST,"botgate_directory_create_failed");
    auto botgate=open_owned_directory(root/"botgate");
    for(const auto* name:{"can","geo","feeds"})require(mkdirat(botgate.get(),name,0700)==0||errno==EEXIST,"botgate_directory_create_failed");
    require(fsync(parent.get())==0&&fsync(botgate.get())==0,"botgate_directory_sync_failed");
    for(const auto* name:{"can","geo","feeds"}){auto checked=open_owned_directory(root/"botgate"/name);(void)checked;}
    auto rules=open_owned_directory(root/"botgate/can");
    constexpr auto marker="sfx-rules-v1.marker";
    if(exists(rules.get(),marker)){require(read(rules.get(),marker,64)=="SFX BotGate rules v1\n","botgate_migration_marker_invalid");return;}
    const std::array<std::string,6> old{{legacy.trusted_sources,legacy.manual_blocks,"-",legacy.domains,"-",legacy.feed_exceptions}};
    for(unsigned i=0;i<old.size();++i){
        std::string lines;
        if(old[i]!="-"){
            lines=old[i];std::replace(lines.begin(),lines.end(),';','\n');lines+='\n';
            if(i==3){std::string expanded;std::size_t begin{};while(begin<lines.size()){
                const auto end=lines.find('\n',begin);const auto domain=lines.substr(begin,end-begin);expanded+=domain+"\n*."+domain+"\n";begin=end+1;}lines=std::move(expanded);}
        }
        require(app::parse_botgate_patterns(lines,i==3,i==4).has_value(),"botgate_migration_invalid");
        const auto* name=app::kBotGateRuleNames[i];
        if(exists(rules.get(),name)){const auto prior=read(rules.get(),name,1024*1024);require(lines.empty()||prior==lines,"botgate_migration_conflict_preserved");}
        else write(rules.get(),name,lines,false);
    }
    write(rules.get(),marker,"SFX BotGate rules v1\n",false);
}
void save_botgate_rule(const std::filesystem::path& root,const std::wstring& owner,unsigned index,std::string_view text){
    owner_matches(owner);require(index<app::kBotGateRuleNames.size()&&index!=4,"botgate_rule_index_invalid");
    require(text.size()<=1024*1024&&app::parse_botgate_patterns(text,index==3).has_value(),"botgate_rules_invalid");
    auto directory=open_owned_directory(root/"botgate/can");write(directory.get(),app::kBotGateRuleNames[index],text,true);
}
void save_botgate_temporary(const std::filesystem::path& root,const std::wstring& owner,std::string_view text){
    owner_matches(owner);require(text.size()<=1024*1024&&app::parse_botgate_patterns(text,false,true).has_value(),"botgate_temporary_rules_invalid");
    auto directory=open_owned_directory(root/"botgate/can");write(directory.get(),"temp_ip.can",text,true);
}
app::BotGateRuleSnapshot load_botgate_files(const std::filesystem::path& root,const std::wstring& owner){
    app::BotGateRuleSnapshot result;
    try{
        owner_matches(owner);
        for(unsigned i=0;i<result.lists.size();++i){const auto text=read_botgate_rule(root,owner,i);auto parsed=app::parse_botgate_patterns(text,i==3,i==4);
            require(parsed.has_value(),"botgate_rule_parse_failed");result.lists[i]=std::move(*parsed);}
        auto geo=open_owned_directory(root/"botgate/geo");
        const auto enumeration=openat(geo.get(),".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);require(enumeration>=0,"botgate_geo_scan_failed");
        DIR* raw=fdopendir(enumeration);if(!raw){close(enumeration);throw std::runtime_error("botgate_geo_scan_failed");}
        struct Close {void operator()(DIR* value)const{closedir(value);}};
        std::unique_ptr<DIR,Close> entries(raw);std::size_t total{};
        for(;;){errno=0;const auto* entry=readdir(entries.get());if(!entry){require(errno==0,"botgate_geo_scan_failed");break;}
            const std::string name=entry->d_name;if(name=="."||name=="..")continue;
            auto extension=std::filesystem::path(name).extension().string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(c>='A'&&c<='Z'?c-'A'+'a':c);});
            if(extension!=".txt")continue;
            require(result.geo.size()<64,"botgate_geo_file_limit");
            require(name.size()<=64&&std::all_of(name.begin(),name.end(),[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_';}),"botgate_geo_name_invalid");
            const auto bytes=read(geo.get(),name.c_str(),16*1024*1024);total+=bytes.size();require(total<=32*1024*1024,"botgate_geo_total_limit");
            auto parsed=app::parse_botgate_geo(name,bytes);require(parsed.has_value(),"botgate_geo_parse_failed");result.geo.push_back(std::move(*parsed));
        }
        result.ready=true;result.status="rules_loaded";
    }catch(const std::exception& error){result.status=error.what();}
    return result;
}
}
