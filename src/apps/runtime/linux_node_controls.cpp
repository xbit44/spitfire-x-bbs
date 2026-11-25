#include "linux_node_controls.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sys/stat.h>
#include <fcntl.h>

namespace sfx::apps::runtime {
namespace {
namespace host=platform::linux_host;
bool exists(int dir,const char* name){
    struct stat value{};
    if(fstatat(dir,name,&value,AT_SYMLINK_NOFOLLOW)==0)return true;
    if(errno==ENOENT)return false;
    throw std::runtime_error("node_controls_stat_failed");
}
std::string text(int dir,const char* name){const auto bytes=host::read_private_file(dir,name,4096);return {bytes.begin(),bytes.end()};}
std::string serialize(const application::NodeEnablement& enabled){
    std::string result="SFX_NODE_CONTROL=3\n";
    for(std::size_t i=0;i<enabled.size();++i)result+="node"+std::to_string(i+1)+"_enabled="+(enabled[i]?"1\n":"0\n");
    return result;
}
}
application::NodeEnablement read_linux_node_controls(const std::filesystem::path& root){
    const auto dir=host::open_owned_directory(root/"config");auto enabled=application::default_node_enablement();
    if(exists(dir.get(),"sfxnodes.state")){
        const auto value=text(dir.get(),"sfxnodes.state");constexpr std::string_view current="SFX_NODE_CONTROL=3\n",legacy="SFX_NODE_CONTROL=2\n";
        const bool old=value.compare(0,legacy.size(),legacy)==0;
        if(!old&&value.compare(0,current.size(),current)!=0)throw std::runtime_error("node_controls_invalid");
        std::size_t offset=old?legacy.size():current.size();const auto count=old?std::size_t{4}:enabled.size();
        for(std::size_t i=0;i<count;++i){
            const auto prefix="node"+std::to_string(i+1)+"_enabled=";
            if(value.compare(offset,prefix.size(),prefix)!=0)throw std::runtime_error("node_controls_invalid");
            offset+=prefix.size();
            if(offset+2>value.size()||(value[offset]!='0'&&value[offset]!='1')||value[offset+1]!='\n')throw std::runtime_error("node_controls_invalid");
            enabled[i]=value[offset]=='1';offset+=2;
        }
        if(offset!=value.size())throw std::runtime_error("node_controls_trailing_data");
    }else if(exists(dir.get(),"sfxnode1.state")){
        const auto value=text(dir.get(),"sfxnode1.state");
        if(value!="SFX_NODE_CONTROL=1\nnode1_enabled=1\n"&&value!="SFX_NODE_CONTROL=1\nnode1_enabled=0\n")throw std::runtime_error("legacy_node_controls_invalid");
        enabled.fill(value=="SFX_NODE_CONTROL=1\nnode1_enabled=1\n");
    }
    return enabled;
}
void publish_linux_node_controls(const std::filesystem::path& root,const application::NodeEnablement& enabled){
    const auto dir=host::open_owned_directory(root/"config");const auto value=serialize(enabled);
    host::publish_private_file(dir.get(),"sfxnodes.state",{value.begin(),value.end()},true);
    if(text(dir.get(),"sfxnodes.state")!=value)throw std::runtime_error("node_controls_verify_failed");
}
}
