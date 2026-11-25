#include "linux_web_help.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <algorithm>
#include <stdexcept>

namespace sfx::apps::runtime::web {
std::optional<HelpAsset> read_help_asset(const std::filesystem::path& root,std::string_view relative){
    if(relative.empty()||relative.size()>160)return std::nullopt;
    const auto slash=relative.find('/');
    const auto folder=slash==relative.npos?std::string_view{}:relative.substr(0,slash);
    const auto name=slash==relative.npos?relative:relative.substr(slash+1);
    const auto dot=name.rfind('.');
    if(dot==name.npos||dot==0||!std::all_of(name.begin(),name.begin()+static_cast<std::ptrdiff_t>(dot),[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_';}))return std::nullopt;
    const auto extension=name.substr(dot);std::string type;std::size_t maximum=1024*1024;
    if(folder.empty()&&extension==".html")type="text/html; charset=utf-8";
    else if(folder=="assets"&&extension==".css")type="text/css; charset=utf-8";
    else if(folder=="assets"&&extension==".js")type="text/javascript; charset=utf-8";
    else if(folder=="images"&&extension==".png"){type="image/png";maximum=8*1024*1024;}
    else return std::nullopt;
    try{
        const auto installation=platform::linux_host::open_owned_directory(root);
        const auto docs=platform::linux_host::open_owned_resource_directory(installation.get(),"docs");
        auto pin=platform::linux_host::open_owned_resource_directory(docs.get(),"sysop");
        if(!folder.empty())pin=platform::linux_host::open_owned_resource_directory(pin.get(),folder);
        auto bytes=platform::linux_host::read_owned_resource(pin.get(),name,maximum);
        if(extension==".png"){
            constexpr std::uint8_t signature[]={137,80,78,71,13,10,26,10};
            if(bytes.size()<8||!std::equal(std::begin(signature),std::end(signature),bytes.begin()))return std::nullopt;
        }
        return HelpAsset{std::move(bytes),std::move(type)};
    }catch(const std::exception&){return std::nullopt;}
}
}
