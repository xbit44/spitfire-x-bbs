#include "linux_web_ftn_import.hpp"
#include "linux_web_ftn_records.hpp"
#include "sfx/application/qwk.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace sfx::apps::runtime::web {
WebFtnListJob::WebFtnListJob(application::FtnTransferToken token,std::string network,std::string source,bool url,bool cp437,FtnListReader reader,std::string selected_source){
    current_.token=token;current_.network=std::move(network);current_.source=source;current_.url=url;
    current_.selected_source=selected_source.empty()?source:std::move(selected_source);
    thread_=std::thread([this,source=std::move(source),url,cp437,reader=std::move(reader)]{
        WebFtnListSnapshot result;{std::lock_guard<std::mutex> lock(mutex_);result=current_;}
        try{
            auto input=reader(source,url,cancelled_);
            if(cancelled_.load())result.diagnostic="Echo list load cancelled. The previous list and FTN settings remain.";
            else if(input.code!=FtnEchoListDownloadCode::ok)result.diagnostic=std::string(ftn_echo_list_download_message(input.code));
            else{
                if(input.bytes.compare(0,3,"\xEF\xBB\xBF")==0)input.bytes.erase(0,3);
                if(cp437)input.bytes=application::qwk_cp437_to_utf8(input.bytes);
                result.list=application::parse_ftn_echo_list(input.bytes);result.ready=result.list.complete;
                result.diagnostic=result.ready?"List loaded for review. Nothing has been saved or subscribed.":"The list exceeds supported size or line limits. The previous list remains.";
            }
        }catch(...){result.diagnostic="Echo list load failed. The previous list and FTN settings remain.";}
        result.active=false;std::lock_guard<std::mutex> lock(mutex_);current_=std::move(result);
    });
}
WebFtnListJob::~WebFtnListJob(){cancel();if(thread_.joinable())thread_.join();}
void WebFtnListJob::cancel() noexcept {cancelled_.store(true);}
WebFtnListSnapshot WebFtnListJob::snapshot()const{std::lock_guard<std::mutex> lock(mutex_);return current_;}
application::FtnEchoImportPlan web_ftn_import_plan(const application::FtnConfiguration& configuration,const WebFtnListSnapshot& source,const WebFtnImportCatalog& catalog,const std::vector<std::string>& selected,application::FtnEchoImportRequirements requirements,bool update_text){
    if(!source.ready||source.active||selected.empty()||selected.size()>16384||catalog.revision!=configuration.message_catalog_revision)throw std::runtime_error("ftn_import_source_or_catalog_changed");
    auto list=source.list;std::set<std::string> remaining;
    const auto fold=[](std::string s){for(auto& c:s)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return s;};
    for(const auto& name:selected)if(!application::valid_ftn_echo_tag(name)||!remaining.insert(fold(name)).second)throw std::runtime_error("ftn_import_selection_invalid");
    for(auto& entry:list.entries)entry.selected=remaining.erase(fold(entry.tag))!=0;
    if(!remaining.empty())throw std::runtime_error("ftn_import_selection_changed");
    const auto destination=application::resolve_ftn_echo_import_destination(configuration,source.network,catalog.groups,catalog.echoes);
    if(!destination.issues.empty())throw std::runtime_error("ftn_import_destination_invalid");
    auto result=application::plan_ftn_echo_import(configuration,source.network,list,std::vector<application::FtnEchoImportRequirements>(list.entries.size(),requirements),catalog.echoes.size(),destination.group,destination.group?std::string{}:destination.name,update_text);
    for(auto& network:result.configuration.networks)if(web_ftn_same_id(network.code,source.network)){
        network.echo_list_source=source.url?application::FtnEchoListSource::url:application::FtnEchoListSource::local_file;
        if(source.url)network.echo_list_url=source.selected_source;else network.echo_list_file=source.selected_source;
    }
    return result;
}
}
