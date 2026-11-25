std::optional<std::vector<MessageArea>> MessageCallerSession::conferences(char scope){
    if(scope=='T')return selected_?std::vector<MessageArea>{*selected_}:std::vector<MessageArea>{};
    auto& p=ports_.compose;
    const auto actor=p.actor();
    if(!actor)return {};
    const auto groups=p.storage.message_groups(*actor);
    if(!groups || !groups.value)return {};
    std::vector<MessageArea> result;
    for(const auto& group:*groups.value){
        const auto echoes=p.storage.local_echoes(*actor,group.id);
        if(!echoes || !echoes.value)return {};
        for(const auto& echo:*echoes.value){
            if(scope=='O'){
                const auto pref=p.storage.message_scan_preference(*actor,echo.id);
                if(!pref || !pref.value){
                    if(pref.status.code==StorageErrorCode::not_found)continue;
                    return {};
                }
                if(!pref.value->unread && !pref.value->to_you)continue;
            }
            if(result.size()>=999)return {};
            const auto area=p.storage.message_area(*actor,echo.id);
            if(!area || !area.value)return {};
            result.push_back(*area.value);
        }
    }
    return result;
}

std::optional<char> MessageCallerSession::scan_scope(){
    auto& p=ports_.compose;
    if(!p.write("\r\n"+system_menu_text(">>>>> CONFERENCE SCAN MENU <<<<<",p.dimensions().ansi)+"\r\n") ||
        !present_message_options(p.terminal,p.dimensions().ansi,"message.specific.scope.options",
            {{'T',"This Message Conference"},{'A',"All Message Conferences"},{'O',"Only Queued Conferences"},{'Q',"Quit"}},p.dimensions().columns))return {};
    const auto choice=p.terminal.read_key({"message.specific.scope","CONFERENCE SCAN MENU - [T A O Q]? [T] ",{'T','A','O','Q'},'T'});
    if(choice.outcome==TerminalOutcome::cancelled)return 'Q';
    return choice.has_key()?choice.key:std::nullopt;
}

std::optional<std::string> MessageCallerSession::read_menu(){
    auto& p=ports_.compose;
    const bool ansi=p.dimensions().ansi;
    const auto color=p.text_color?p.text_color():std::uint8_t{7};
    const auto current=selected_?"#"+std::to_string(selected_->echo.conference_number)+" - \""+
        selected_->group.name+": "+selected_->echo.name+"\"":"None";
    if(!p.write("\r\n"+system_menu_text(">>>>>> READ MESSAGES MENU <<<<<<",ansi)+"\r\n") ||
        !present_message_read_options(p.terminal,ansi,p.dimensions().columns) ||
        !present_message_reader_field(p.terminal,ansi,color,"Current Message Conference is ",current,p.dimensions().columns))return {};
    const auto choice=p.terminal.read_key({"message.read.mode","READ MESSAGES MENU - [T A O N Q]? ",{'T','A','O','N','Q'},'Q'});
    if(choice.outcome==TerminalOutcome::cancelled || choice.key=='Q')return "";
    if(!choice.has_key())return {};
    if(choice.key=='T')return selected_?result_text(read_conference(selected_->echo.id)):
        "No selected accessible conference. Use C to choose one.";
    if(choice.key=='N'){
        const auto scope=p.terminal.read_key({"message.scan.scope",
            "[E] Current echo, [G] Current group, [A] All accessible groups, [Q] Cancel [E]: ",{'E','G','A','Q'},'E'});
        if(scope.outcome==TerminalOutcome::cancelled || scope.key=='Q')return "";
        if(!scope.has_key())return {};
        MessageQueryOptions options;
        options.mode=MessageQueryMode::regular_new;
        options.scope=scope.key=='G'?MessageQueryScope::current_group:scope.key=='A'?
            MessageQueryScope::all_accessible:MessageQueryScope::current_echo;
        if(selected_){options.echo=selected_->echo.id;options.group=selected_->group.id;}
        else if(options.scope!=MessageQueryScope::all_accessible)return "No selected accessible conference.";
        return result_text(read(options));
    }
    const auto areas=conferences(*choice.key);
    if(!areas)return {};
    if(areas->empty())return "No eligible readable conferences.";
    for(const auto& area:*areas){
        const auto outcome=read_conference(area.echo.id,true);
        if(outcome==MessageReaderOutcome::failed)return result_text(outcome);
        if(outcome==MessageReaderOutcome::cancelled)return "";
    }
    return "End of eligible conferences.";
}

std::optional<std::string> MessageCallerSession::personal_menu(){
    auto& p=ports_.compose;
    const auto actor=p.actor();
    if(!actor)return {};
    const auto summary=p.storage.login_message_scan_summary(*actor,true);
    if(!summary || !summary.value)return "Your Message Scan could not be completed.";
    if(!present_message_login_summary(p.terminal,*summary.value,p.dimensions().ansi,p.dimensions().columns))return {};
    const auto review=p.terminal.read_key({"message.personal.review","Review your messages now? [Y/n] ",{'Y','N'},'Y'});
    if(review.outcome==TerminalOutcome::cancelled || review.key=='N')return "";
    if(!review.has_key())return {};
    for(;;){
        const auto mode=p.terminal.read_key({"message.personal.mode",
            "[T] To You, [F] From You, [B] Both, [L] List, [Q] Quit [T]: ",{'T','F','B','L','Q'},'T'});
        if(mode.outcome==TerminalOutcome::cancelled || mode.key=='Q')return "";
        if(!mode.has_key())return {};
        const bool list=mode.key=='L';
        auto filter=*mode.key;
        if(list){
            const auto choice=p.terminal.read_key({"message.personal.list.filter",
                "List [T] To You, [F] From You, [B] Both, [I] Private Inbox\n[S] Sent Private/NetMail, [Q] Quit [B]: ",
                {'T','F','B','I','S','Q'},'B'});
            if(choice.key=='Q')return "";
            if(choice.outcome==TerminalOutcome::cancelled)continue;
            if(!choice.has_key())return {};
            filter=*choice.key;
        }
        MessageQueryOptions options;
        options.scope=MessageQueryScope::all_accessible;
        options.mode=filter=='I'?MessageQueryMode::inbox:filter=='S'?MessageQueryMode::sent:
            filter=='T' && !list?MessageQueryMode::personal:MessageQueryMode::correspondence;
        if(filter=='F')options.caller_direction=MessageCallerDirection::from;
        if(filter=='B')options.caller_direction=MessageCallerDirection::both;
        const auto outcome=read(options,list,{},MessageReadNavigation{false,list});
        if(outcome==MessageReaderOutcome::failed)return result_text(outcome);
        if(list && outcome==MessageReaderOutcome::cancelled)return "";
    }
}

std::optional<std::string> MessageCallerSession::browse_results(const std::vector<MessageArea>& areas,
    const MessageQueryOptions& filter,bool allow_skip){
    auto& p=ports_.compose;
    const bool filtered=!filter.search.empty() || filter.mode==MessageQueryMode::addressed;
    std::vector<MessageQueryContext> results;
    std::uint64_t matches{};
    auto next_poll=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    if(filtered && !present_message_text(p.terminal,"message.search.start","Scanning messages. Press Q to cancel."))return {};
    for(const auto& area:areas){
        if(ports_.safe_boundary && !ports_.safe_boundary())return {};
        auto options=filter;
        options.echo=area.echo.id;
        options.group=area.group.id;
        const auto actor=p.actor();
        if(!actor)return {};
        if(filtered && !present_message_text(p.terminal,"message.search.progress",
            "Searching #"+std::to_string(area.echo.conference_number)+" - "+area.group.name+": "+area.echo.name))return {};
        auto query=p.storage.begin_message_query(*actor,options);
        if(!query || !query.value)return "Message scan interrupted. No complete result was recorded.";
        std::optional<MessageQueryCursor> cursor{MessageQueryCursor{}};
        bool found{};
        while(cursor){
            if(ports_.safe_boundary && !ports_.safe_boundary())return {};
            if(filtered && p.byte && std::chrono::steady_clock::now()>=next_poll){
                next_poll=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                const auto input=p.byte();
                if(input.byte && (*input.byte=='Q' || *input.byte=='q' || *input.byte==27)){
                    if(p.accepted_key)p.accepted_key(false);
                    return "Message scan cancelled.";
                }
                if(!input.byte && input.outcome!=TerminalOutcome::timed_out)return {};
            }
            auto page=p.storage.query_message_headers(*actor,*query.value,*cursor,filtered?kMessagePageMaximum:1);
            if(!page || !page.value)return "Message scan interrupted. No complete result was recorded.";
            found=found || !page.value->entries.empty();
            matches+=page.value->entries.size();
            if(!filtered && found)break;
            cursor=page.value->next;
        }
        if(found)results.push_back(std::move(*query.value));
    }
    if(results.empty())return filtered?"No matching messages in the selected conferences.":"No messages in the selected conferences.";
    const auto summary=filtered?std::to_string(matches)+(matches==1?" matching message":" matching messages"):std::string{};
    std::size_t at{};
    bool from_next{};
    while(at<results.size()){
        const auto& result=results[at];
        const auto outcome=read(result.options,true,{},MessageReadNavigation{
            allow_skip,true,at>0,at+1<results.size(),from_next,summary},result);
        if(outcome==MessageReaderOutcome::failed)return result_text(outcome);
        if(outcome==MessageReaderOutcome::cancelled)return "";
        from_next=outcome==MessageReaderOutcome::previous_conference;
        if(from_next)--at;
        else ++at;
    }
    return allow_skip?"End of eligible conferences.":"";
}

namespace {
std::string fold_header_name(std::string_view name){
    std::string result(name);
    for(auto& c:result)if(c>='A' && c<='Z')c=static_cast<char>(c+('a'-'A'));
    return result;
}
struct HeaderParticipant {
    std::string name,network,address;
};
}

std::optional<std::string> MessageCallerSession::specific_caller(){
    auto& p=ports_.compose;
    const auto scope=scan_scope();
    if(!scope)return {};
    if(*scope=='Q')return "";
    const auto areas=conferences(*scope);
    if(!areas)return {};
    if(areas->empty())return "No eligible readable conferences.";
    MessageQueryOptions filter;
    filter.mode=MessageQueryMode::addressed;
    const auto kind=p.terminal.read_key({"message.specific.identity",
        "Find [L] Local caller, [H] FTN/header name, [Q] Quit [L]: ",{'L','H','Q'},'L'});
    if(kind.outcome==TerminalOutcome::cancelled || kind.key=='Q')return "";
    if(!kind.has_key())return {};
    if(kind.key=='L'){
        const auto chosen=select_local_message_recipient({p.storage,p.terminal,p.actor});
        if(chosen.outcome==MessageSelectionOutcome::failed)return {};
        if(!chosen.recipient)return "";
        filter.addressed_to=chosen.recipient->account;
    }else{
        for(;;){
        const auto input=p.terminal.read_visible({"message.specific.remote.name",
            "Full or partial FTN/header name (Enter cancels): ",128,EmptyInputPolicy::use_default,{}});
        if(cancelled(input) || input.outcome==TerminalOutcome::empty)return "";
        if(!input.has_value())return {};
        const auto needle=fold_header_name(input.value);
        std::vector<HeaderParticipant> offered;
        bool too_many{};
        const auto offer=[&](std::string name,std::string network,std::string address){
            if(name.empty() || fold_header_name(name).find(needle)==std::string::npos)return;
            if(std::any_of(offered.begin(),offered.end(),[&](const auto& entry){
                return same_network(entry.name,name) && same_network(entry.network,network) && entry.address==address;
            }))return;
            if(offered.size()==128){too_many=true;return;}
            offered.push_back({std::move(name),std::move(network),std::move(address)});
        };
        for(const auto& area:*areas){
            if(ports_.safe_boundary && !ports_.safe_boundary())return {};
            if(!present_message_text(p.terminal,"message.specific.discovery","Checking names in "+area.group.name+": "+area.echo.name))return {};
            const auto actor=p.actor();
            if(!actor)return {};
            MessageQueryOptions options;options.echo=area.echo.id;options.group=area.group.id;
            const auto query=p.storage.begin_message_query(*actor,options);
            if(!query || !query.value)return "Caller lookup could not be completed.";
            std::optional<MessageQueryCursor> cursor{MessageQueryCursor{}};
            while(cursor && !too_many){
                if(ports_.safe_boundary && !ports_.safe_boundary())return {};
                const auto page=p.storage.query_message_headers(*actor,*query.value,*cursor,kMessagePageMaximum);
                if(!page || !page.value)return "Caller lookup could not be completed.";
                for(const auto& h:page.value->entries){
                    if(!h.author && h.remote_author)offer(h.from,h.remote_author->network,h.remote_author->address);
                    if(!h.target.recipient && h.to!="All" && h.to!="All Callers")
                        offer(h.to,h.remote_author?h.remote_author->network:std::string{},{});
                    if(too_many)break;
                }
                cursor=page.value->next;
            }
            if(too_many)return "Too many matching header names. Try a longer name.";
        }
        if(offered.empty()){
            if(!present_message_text(p.terminal,"message.specific.no_name","No matching FTN/header names in the selected conferences."))return {};
            const auto retry=p.terminal.read_key({"message.specific.retry","[R] Try another name, [Q] Quit [R]: ",{'R','Q'},'R'});
            if(retry.outcome==TerminalOutcome::cancelled || retry.key=='Q')return "";
            if(!retry.has_key())return {};
            continue;
        }
        for(const auto& entry:offered){
            const auto source=entry.network.empty()?"header name":entry.network+
                (entry.address.empty()?" header name":" "+entry.address);
            if(!present_message_text(p.terminal,"message.specific.candidate",entry.name+" ("+source+")"))return {};
            const auto use=p.terminal.read_key({"message.specific.remote.confirm","Use this header name? [Y]es [N]ext [Q]uit [N]: ",{'Y','N','Q'},'N'});
            if(use.outcome==TerminalOutcome::cancelled || use.key=='Q')return "";
            if(!use.has_key())return {};
            if(use.key=='Y'){
                filter.remote_name=entry.name;
                filter.remote_network=entry.network;
                filter.remote_address=entry.address;
                break;
            }
        }
        if(!filter.remote_name.empty())break;
        }
    }
    if(!p.write("\r\n"+system_menu_text(">>> SPECIFIC CALLER MESSAGES MENU <<<",p.dimensions().ansi)+"\r\n") ||
        !present_message_options(p.terminal,p.dimensions().ansi,"message.specific.direction.options",
            {{'T',"To"},{'F',"From"},{'B',"Both"},{'Q',"Quit"}},p.dimensions().columns))return {};
    const auto direction=p.terminal.read_key({"message.specific.direction",
        "SPECIFIC CALLER MESSAGES MENU - [T F B Q]? [B] ",{'T','F','B','Q'},'B'});
    if(direction.outcome==TerminalOutcome::cancelled || direction.key=='Q')return "";
    if(!direction.has_key())return {};
    filter.caller_direction=direction.key=='T'?MessageCallerDirection::to:
        direction.key=='F'?MessageCallerDirection::from:MessageCallerDirection::both;
    return browse_results(*areas,filter,*scope!='T');
}
