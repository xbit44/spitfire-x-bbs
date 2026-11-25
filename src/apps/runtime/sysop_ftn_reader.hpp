#pragma once
#include "alpha_ftn_engine.hpp"
#include "sfx/application/sysop_session.hpp"
namespace sfx::apps::runtime {
inline application::SysopTextPage read_sysop_ftn(application::IFtnMailStorage& storage,
    const std::function<std::optional<application::MessageActor>()>& actor,
    const std::shared_ptr<AlphaFtnEngine>& engine,std::string network,
    application::SysopFtnView kind,std::uint64_t page,unsigned rows){
    using namespace application;
    SysopTextPage out;out.title=kind==SysopFtnView::queues?"FTN Queues":kind==SysopFtnView::held?"Held Mail":"BinkP Activity";out.page=page;
    auto current=actor();if(!current){out.notice="Sysop access changed or read cancelled.";return out;}
    const auto config=storage.ftn_configuration({false,*current});if(!config||!config.value){out.notice="FTN configuration unavailable.";return out;}
    const auto selected=std::find_if(config.value->networks.begin(),config.value->networks.end(),[&](const auto& n){return n.code==network&&n.enabled;});
    if(selected==config.value->networks.end()){out.notice="Selected network changed or is disabled.";return out;}out.title+=" - "+selected->name;
    const auto link_name=[&](const std::string& code){const auto found=std::find_if(config.value->links.begin(),config.value->links.end(),[&](const auto& l){return l.network==network&&l.code==code;});return found==config.value->links.end()?std::string("Unavailable link"):found->name;};
    const auto limit=static_cast<std::uint64_t>(std::clamp(rows>8?rows-8:1U,1U,32U));
    if(page>1000000/limit){out.notice="Page is outside the supported view. Refresh to restart.";return out;}const auto first=page*limit;
    if(kind==SysopFtnView::activity){
        if(!engine){out.notice="BinkP activity unavailable.";return out;}std::vector<std::string> filtered;
        for(const auto& item:engine->snapshot().activity)if(item.network==network)filtered.push_back(link_name(item.link)+": "+item.message);
        if(first<filtered.size())out.lines.assign(filtered.begin()+static_cast<std::ptrdiff_t>(first),filtered.begin()+static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(filtered.size(),first+limit)));
        out.next=first+limit<filtered.size();out.notice="Recent activity for this network; bounded Runtime history.";
    }else if(kind==SysopFtnView::queues){
        const auto data=storage.sysop_ftn_queues(*current,network,static_cast<std::uint32_t>(first));if(!data||!data.value){out.notice="Queues unavailable or changed.";return out;}
        static const char* in[]={"Receiving","Received","Processing","Held","Processed"};static const char* sent[]={"Queued","Claimed","Awaiting acknowledgment","Delivered","Held","External handoff"};
        for(std::size_t i=0;i<std::min<std::size_t>(data.value->rows.size(),static_cast<std::size_t>(limit));++i){const auto& r=data.value->rows[i];const auto state=r.outbound?(r.state<6?sent[r.state]:"Unknown state"):(r.state<5?in[r.state]:"Unknown state");out.lines.push_back(std::string(r.outbound?"Out: ":"In: ")+r.file+" / "+link_name(r.link)+" / "+state+(r.reason.empty()?"":" / "+r.reason));}
        out.next=data.value->more||data.value->rows.size()>limit;
    }else{
        Identifier128 after;std::uint64_t skipped{};
        for(;;){current=actor();if(!current){out.lines.clear();out.notice="Sysop access changed or read cancelled.";return out;}
            const auto data=storage.sysop_ftn_held(*current,network,after);if(!data||!data.value){out.lines.clear();out.notice="Held mail unavailable or changed.";return out;}
            for(const auto& r:data.value->rows){if(skipped++<first)continue;if(out.lines.size()==limit){out.next=true;break;}out.lines.push_back(r.file+" / "+link_name(r.link)+" / "+(r.reason.empty()?"Held; inspect in SFXConfig for details":r.reason));}
            if(out.next||!data.value->more)break;if(data.value->rows.empty()){out.notice="Held-mail cursor unavailable.";return out;}const auto next=data.value->rows.back().ingress;if(next==after){out.notice="Held-mail cursor changed. Refresh.";return out;}after=next;
        }
    }
    out.previous=page>0;if(out.lines.empty())out.notice+=" No entries on this page.";return out;
}
}
