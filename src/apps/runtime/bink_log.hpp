#pragma once
#include "sfx/platform/win32/file_payload_store.hpp"
#include <chrono>
#include <ctime>
namespace sfx::apps::runtime {
// Called only by the sole Runtime under its FTN mutex. Keep two 1-MiB
// generations; never use peer strings as paths or emit protocol passwords.
inline void append_bink_log(const std::filesystem::path& root,const std::wstring& owner,
    std::uint64_t utc,std::uint64_t session,std::string_view network,std::string_view link,std::string_view message){
    namespace native=sfx::platform::win32;
    auto dir=native::FilePayloadDirectory::private_store(root/L"Logs",owner);
    const auto clean=[](std::string_view text){std::string out;for(const unsigned char c:text.substr(0,512))out+=c<32||c==127?' ':static_cast<char>(c);return out;};
    __time64_t stamp=utc;tm local{};native::installation::require(_localtime64_s(&local,&stamp)==0,"bink_log_time_failed");char date[32]{};strftime(date,sizeof(date),"%Y-%m-%d %H:%M:%S",&local);
    const auto line=std::string(date)+"  session="+std::to_string(session)+"  "+clean(network)+" / "+clean(link)+"  "+clean(message)+"\r\n";
    std::optional<native::FilePayloadLease> file;
    if(!dir.absent("bink.log")){
        std::optional<native::FilePayloadLease> current;current.emplace(dir.read("bink.log",true));native::installation::require(current->byte_count()<=1048576,"bink_log_existing_oversized_retained");const auto prefix=current->identity();
        if(prefix.bytes+line.size()>1048576){
            if(!dir.absent("bink.log.2")){auto oldest=dir.read("bink.log.2",true);oldest.remove_exact();}
            if(!dir.absent("bink.log.1")){auto previous=dir.read("bink.log.1",true);previous.rename_to(dir,"bink.log.2");}
            current->rename_to(dir,"bink.log.1");
        }else {current.reset();file.emplace(dir.resume_private("bink.log",1048576,prefix));}
    }
    if(!file)file.emplace(dir.create("bink.log",1048576));
    file->append(reinterpret_cast<const std::uint8_t*>(line.data()),line.size());file->flush();
}
}
