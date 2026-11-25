#include "sfx/application/message_session.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/application/ftn.hpp"
#include <charconv>
#include <algorithm>
#include <chrono>
namespace sfx::application {
namespace {
bool cancelled(const TerminalInputResult& input){return input.outcome==TerminalOutcome::cancelled || input.value=="Q" || input.value=="q";}
std::string result_text(MessageReaderOutcome result){return result==MessageReaderOutcome::failed?"Message operation unavailable or changed; no success is claimed.":"";}
bool is_message_action(MenuAction action){return action>=MenuAction::message_change && action<=MenuAction::comment_sysop;}
bool same_network(std::string a,std::string b){for(auto* v:{&a,&b})for(auto& c:*v)if(c>='A'&&c<='Z')c=static_cast<char>(c+32);return a==b;}
struct NetmailSelection {bool failed{};std::optional<MessageTarget> target;};
NetmailSelection netmail_target(const MessageComposePorts& p,const std::vector<MessageNetworkChoice>& networks){
    if(networks.empty())return {};
    std::size_t selected{};
    if(networks.size()>1){
        for(std::size_t i=0;i<networks.size();++i)if(!present_message_text(p.terminal,"message.netmail.network",std::to_string(i+1)+". "+networks[i].name))return {true,{}};
        for(;;){const auto input=p.terminal.read_visible({"message.netmail.network.select","Network number (Enter cancels): ",4,EmptyInputPolicy::use_default,{}});if(cancelled(input)||input.outcome==TerminalOutcome::empty)return {};if(!input.has_value())return {true,{}};unsigned value{};const auto parsed=std::from_chars(input.value.data(),input.value.data()+input.value.size(),value);if(parsed.ec==std::errc{}&&parsed.ptr==input.value.data()+input.value.size()&&value&&value<=networks.size()){selected=value-1;break;}if(!present_message_text(p.terminal,"message.netmail.network.invalid","Choose a listed network number."))return {true,{}};}
    }
    const auto& n=networks[selected];if(!present_message_text(p.terminal,"message.netmail.network.selected","Network: "+n.name+"\nFrom address: "+n.source_aka+(n.paused?"\nFTN is paused; valid mail will remain queued.":"")))return {true,{}};
    const auto name=p.terminal.read_visible({"message.netmail.name","Recipient name (Enter cancels): ",35,EmptyInputPolicy::use_default,{}});if(cancelled(name)||name.outcome==TerminalOutcome::empty)return {};if(!name.has_value())return {true,{}};
    for(;;){const auto address=p.terminal.read_visible({"message.netmail.address","Full FTN address (zone:net/node.point, Enter cancels): ",96,EmptyInputPolicy::use_default,{}});if(cancelled(address)||address.outcome==TerminalOutcome::empty)return {};if(!address.has_value())return {true,{}};const auto parsed=parse_ftn_address(address.value);if(parsed){MessageTarget target;target.kind=MessageKind::private_ftn;target.remote=MessageRemoteTarget{n.code,n.source_aka,format_ftn_address(*parsed),name.value};return {false,target};}if(!present_message_text(p.terminal,"message.netmail.address.invalid","Enter a complete FTN address including its zone."))return {true,{}};}
}
}
bool MessageCallerSession::initialize(){
    const auto actor=ports_.compose.actor();if(!actor)return false;
    const auto saved=ports_.compose.storage.saved_message_area(*actor);if(!saved||!saved.value)return false;
    if(*saved.value){selected_=**saved.value;return true;}
    const auto first=first_local_message_echo(ports_.compose.storage,*actor);if(!first || !first.value)return false;
    if(*first.value){const auto& area=**first.value;selected_=MessageArea{area.group,area.echo,area.configuration_revision};}return true;
}
bool MessageCallerSession::refresh_selection(){
    if(!selected_)return true;
    const auto actor=ports_.compose.actor();if(!actor)return false;
    const auto current=ports_.compose.storage.message_area(*actor,selected_->echo.id);
    if(current && current.value){selected_=*current.value;return true;}
    if(current.status.code==StorageErrorCode::not_found){selected_.reset();return true;}
    return false;
}
std::optional<bool> MessageCallerSession::preview(unsigned footer_row){
    if(preview_)return preview_;
    auto& p=ports_.compose;
    const auto actor=p.actor();
    if(!actor)return {};
    const auto assigned=p.storage.system_sysop_target(*actor);
    if(!assigned && assigned.status.code!=StorageErrorCode::not_found)return {};
    if(!assigned.value || assigned.value->recipient!=actor->account)return preview_=false;
    std::string prompt=ports_.caller_name()+", do you wish to preview messages? [y/N] ";
    if(footer_row){
        if(!p.write("\x1b["+std::to_string(footer_row)+";1H\x1b[0;37;40m\x1b[2K"))return {};
        if(prompt.size()+2>p.dimensions().columns)prompt="Preview messages? [y/N] ";
    }
    const auto choice=p.terminal.read_key({footer_row?"message.read.preview.footer":"message.read.preview",
        prompt,{'Y','N'},'N',false,true,false,!footer_row});
    if(choice.outcome==TerminalOutcome::cancelled)return preview_=false;
    if(!choice.has_key())return {};
    return preview_=choice.key=='Y';
}
MessageReaderOutcome MessageCallerSession::read(const MessageQueryOptions& options,bool headers,
    std::optional<std::int64_t> number,MessageReadNavigation navigation,std::optional<MessageQueryContext> context){
    auto& p=ports_.compose;
    const auto policy=ports_.paging?ports_.paging():TerminalDisplayPagingPolicy{true,std::uint16_t{24}};
    MessageReaderPorts reader{p.storage,p.terminal,p.actor,[this](const OpenedMessage& m){return reply(m);},
        ports_.date,ports_.safe_boundary,policy.page_length.value_or(std::uint16_t{24}),policy.more_prompt_enabled,
        std::move(context),p.operation,p.dimensions,p.text_color};
    reader.allow_skip=navigation.skip;
    reader.return_to_list=navigation.list_return;
    reader.adjacent_previous=navigation.previous;
    reader.adjacent_next=navigation.next;
    reader.start_at_end=navigation.start_at_end;
    reader.byte=p.byte;
    reader.write=p.write;
    reader.accepted_key=p.accepted_key;
    reader.preview=[this](unsigned row){return preview(row);};
    reader.lightbars_enabled=ports_.lightbars_enabled;
    reader.list_summary=std::move(navigation.list_summary);
    reader.legacy_pipe_colors=ports_.legacy_pipe_colors;
    return read_local_messages(reader,options,headers,number);
}
bool MessageCallerSession::reply(const OpenedMessage& original){
    auto& p=ports_.compose;const auto actor=p.actor();if(!actor)return false;
    const auto source=p.storage.quote_source(*actor,original.header.identity.id);
    if(!source || !source.value)return present_message_text(p.terminal,"message.reply.unavailable","Reply source is unavailable.");
    if(source.value->header.remote_author||source.value->header.target.kind==MessageKind::private_ftn){
        const auto& h=source.value->header;const auto networks=p.storage.ftn_message_networks(*actor);if(!networks||!networks.value)return present_message_text(p.terminal,"message.reply.unavailable","FTN network is unavailable; no reply target was guessed.");
        const auto code=h.remote_author?h.remote_author->network:h.target.remote?h.target.remote->network:std::string{};
        const auto n=std::find_if(networks.value->begin(),networks.value->end(),[&](const auto& value){return same_network(value.code,code);});if(n==networks.value->end())return present_message_text(p.terminal,"message.reply.unavailable","Original FTN network is disabled or removed; no reply target was guessed.");
        auto target=h.target;target.recipient.reset();target.sysop_assignment_revision.reset();
        if(h.remote_author){target.remote=MessageRemoteTarget{n->code,n->source_aka,h.remote_author->address,h.remote_author->name};if(target.kind==MessageKind::private_local)target.kind=MessageKind::private_ftn;}
        else{if(!target.remote)return present_message_text(p.terminal,"message.reply.unavailable","Original remote destination is missing.");target.remote->source_aka=n->source_aka;}
        return compose_local_message(p,target,target.remote->name,ports_.caller_name(),h.identity.id,h.subject)!=MessageComposeOutcome::failed;
    }
    if(!source.value->header.author)return present_message_text(p.terminal,"message.reply.unavailable","Original author identity is unavailable.");
    const auto recipient=p.storage.resolve_message_recipient(*actor,std::to_string(*source.value->header.author));
    if(!recipient || !recipient.value || recipient.value->entries.size()!=1)return present_message_text(p.terminal,"message.reply.unavailable","Original author is no longer an eligible recipient.");
    auto target=source.value->header.target;target.recipient=source.value->header.author;target.sysop_assignment_revision.reset();target.remote.reset();
    const auto result=compose_local_message(p,target,recipient.value->entries.front().name,ports_.caller_name(),source.value->header.identity.id,source.value->header.subject);
    return result!=MessageComposeOutcome::failed;
}
bool MessageCallerSession::login_scan(){
    auto& p=ports_.compose;const auto actor=p.actor();if(!actor)return false;
    const auto summary=p.storage.login_message_scan_summary(*actor);
    if(!summary || !summary.value){
        if(!present_message_text(p.terminal,"message.login.failure","Message scan is unavailable; no empty inbox is claimed."))return false;
    }else{
        if(!present_message_login_summary(p.terminal,*summary.value,p.dimensions().ansi))return false;
        if(summary.value->unread_to_you==0){
            if(!present_message_text(p.terminal,"message.login.empty","\nNo new messages addressed to you."))return false;
        }else{
            const auto answer=p.terminal.read_key({"message.login.read_new","Read your new messages now [Y/n]?",{'Y','N'},'Y'});
            if(answer.outcome==TerminalOutcome::cancelled || answer.key=='N')return true;
            if(!answer.has_key())return false;
            const auto policy=ports_.paging?ports_.paging():TerminalDisplayPagingPolicy{true,std::uint16_t{24}};
            MessageReaderPorts reader{p.storage,p.terminal,p.actor,[this](const OpenedMessage& m){return reply(m);},
                ports_.date,ports_.safe_boundary,policy.page_length.value_or(std::uint16_t{24}),
                policy.more_prompt_enabled,summary.value->query,p.operation,p.dimensions,p.text_color};
            reader.write=p.write;
            reader.legacy_pipe_colors=ports_.legacy_pipe_colors;
            const auto outcome=read_local_messages(reader,summary.value->query.options);
            if(outcome==MessageReaderOutcome::failed && !present_message_text(p.terminal,"message.login.failure","Message scan changed or failed; returning to Main."))return false;
        }
    }
    const auto continued=p.terminal.read_key({"message.login.scan.complete","<Enter> to continue ",{'C'},'C'});
    return continued.has_key() || continued.outcome==TerminalOutcome::cancelled;
}
MessageReaderOutcome MessageCallerSession::read_conference(LocalEchoId echo,bool allow_skip){
    auto& p=ports_.compose;auto actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
    const auto area=p.storage.message_area(*actor,echo);
    if(!area || !area.value)return area.status.code==StorageErrorCode::not_found?MessageReaderOutcome::completed:MessageReaderOutcome::failed;
    MessageQueryOptions options;options.echo=echo;options.group=area.value->group.id;
    const auto context=p.storage.begin_message_query(*actor,options);
    if(!context || !context.value)return context.status.code==StorageErrorCode::not_found?MessageReaderOutcome::completed:MessageReaderOutcome::failed;
    const auto say=[&](std::string text){return present_message_text(p.terminal,"message.read.entry",text);};
    if(!say("Loading MESSAGE CONFERENCE #"+std::to_string(area.value->echo.conference_number)+" - \""+area.value->group.name+": "+area.value->echo.name+"\""))return MessageReaderOutcome::failed;
    actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
    const auto first=p.storage.query_message_headers(*actor,*context.value,{},1);
    MessageQueryCursor reverse;reverse.reverse=true;
    const auto last=p.storage.query_message_headers(*actor,*context.value,reverse,1);
    const auto pref=p.storage.message_scan_preference(*actor,echo);
    if(!first || !first.value || !last || !last.value || !pref || !pref.value)return MessageReaderOutcome::failed;
    if(first.value->entries.empty() || last.value->entries.empty()){
        if(!say("No available messages in this conference."))return MessageReaderOutcome::failed;
        if(allow_skip)return MessageReaderOutcome::completed;
        const auto acknowledged=p.terminal.pause({"message.read.empty.ack"});
        return acknowledged==TerminalOutcome::cancelled?MessageReaderOutcome::cancelled:acknowledged==TerminalOutcome::completed?MessageReaderOutcome::completed:MessageReaderOutcome::failed;
    }
    const auto saved=pref.value->highest_opened;
    const auto available=saved?p.storage.message_number_header(*actor,*context.value,saved):StorageResult<MessageHeader>{};
    if(saved && !available && available.status.code!=StorageErrorCode::not_found)return MessageReaderOutcome::failed;
    const bool resumable=saved && available && available.value;
    // Separate sentences deliberately: even a 64-bit saved number fits the
    // supported 80-column caller baseline without terminal mid-word wrapping.
    if(!say(resumable?"Your saved last message read in this conference is #"+std::to_string(saved)+".\nOlder revisits do not move this position back.":"No saved available message can be resumed; select a message number."))return MessageReaderOutcome::failed;
    const auto range=std::to_string(first.value->entries.front().identity.area_number)+"-"+std::to_string(last.value->entries.front().identity.area_number);
    for(;;){
        const auto answer=p.terminal.read_visible({"message.read.start","Enter message # ["+range+"]"+(resumable?" or < ENTER > = last message read":"")+", [L] List"+(allow_skip?", [K] Skip":"")+", [Q] Return: ",19,EmptyInputPolicy::use_default,{}});
        if(cancelled(answer))return MessageReaderOutcome::cancelled;
        if(answer.outcome!=TerminalOutcome::completed && answer.outcome!=TerminalOutcome::empty)return MessageReaderOutcome::failed;
        if(allow_skip && (answer.value=="K" || answer.value=="k"))return MessageReaderOutcome::skipped;
        if(answer.value=="L" || answer.value=="l")return read(options,true,{},MessageReadNavigation{allow_skip,true},context.value);
        if(answer.outcome==TerminalOutcome::empty){
            if(resumable)return read(options,false,saved,MessageReadNavigation{allow_skip},context.value);
            continue;
        }
        std::int64_t number{};const auto parsed=std::from_chars(answer.value.data(),answer.value.data()+answer.value.size(),number);
        if(number>0 && parsed.ec==std::errc{} && parsed.ptr==answer.value.data()+answer.value.size()){
            actor=p.actor();
            if(!actor)return MessageReaderOutcome::failed;
            const auto found=p.storage.message_number_header(*actor,*context.value,number);
            if(found && found.value)return read(options,false,number,MessageReadNavigation{allow_skip},context.value);
            if(found.status.code!=StorageErrorCode::not_found)return MessageReaderOutcome::failed;
        }
        if(!say("Choose an available conference message number or L for the list."))return MessageReaderOutcome::failed;
    }
}
#include "message_session_reading.inc"
std::optional<std::string> MessageCallerSession::enter(bool sysop){
    auto& p=ports_.compose;MessageTarget target;std::string to="All Callers";
    MessageSelectionPorts selection{p.storage,p.terminal,p.actor};
    if(sysop){
        const auto actor=p.actor();if(!actor)return {};
        const auto assigned=p.storage.system_sysop_target(*actor);if(!assigned || !assigned.value)return "System Sysop is not available. Ask the Sysop to configure an eligible assignment.";
        const auto recipient=p.storage.resolve_message_recipient(*actor,std::to_string(*assigned.value->recipient));if(!recipient || !recipient.value || recipient.value->entries.size()!=1)return "System Sysop is unavailable.";
        target=*assigned.value;to=recipient.value->entries.front().name;
    }else{
        const auto actor=p.actor();if(!actor)return {};const auto networks=p.storage.ftn_message_networks(*actor);const bool netmail=networks&&networks.value&&!networks.value->empty();
        std::vector<std::pair<char,std::string>> options{{'P',"Public post"},{'M',"Private local mail"}};if(netmail)options.push_back({'N',"NetMail"});options.push_back({'Q',"Cancel"});
        if(!p.write("\r\n"+system_menu_text(">>>>> ENTER MESSAGE MENU <<<<<",p.dimensions().ansi)+"\r\n") ||
            !present_message_options(p.terminal,p.dimensions().ansi,"message.entry.kind.options",options,p.dimensions().columns) ||
            !present_message_text(p.terminal,"message.entry.conference","\nCurrent Message Conference is "+
                (selected_?"#"+std::to_string(selected_->echo.conference_number)+" - \""+selected_->group.name+": "+selected_->echo.name+"\"":"None")))return {};
        const auto kind=p.terminal.read_key({"message.entry.kind",netmail?"ENTER MESSAGE MENU - [P M N Q]? [P]: ":"ENTER MESSAGE MENU - [P M Q]? [P]: ",netmail?std::vector<char>{'P','M','N','Q'}:std::vector<char>{'P','M','Q'},'P'});
        if(kind.outcome==TerminalOutcome::cancelled || kind.key=='Q')return "";
        if(!kind.has_key())return {};
        if(kind.key=='N'){const auto chosen=netmail_target(p,*networks.value);if(chosen.failed)return {};if(!chosen.target)return "";return compose_local_message(p,*chosen.target,chosen.target->remote->name,ports_.caller_name())==MessageComposeOutcome::failed?std::optional<std::string>{"NetMail composition failed; no save is claimed."}:std::optional<std::string>{""};}
        if(kind.key=='M')target.kind=MessageKind::private_local;
        else{
            if(!selected_)return "Choose an accessible public conference first. Private mail remains available.";
            target.echo=selected_->echo.id;
            const auto recipient=p.terminal.read_key({"message.public.to","[A] All Callers, [N] Named caller, [Q] Cancel [A]: ",{'A','N','Q'},'A'});
            if(recipient.outcome==TerminalOutcome::cancelled || recipient.key=='Q')return "";
            if(!recipient.has_key())return {};
            if(recipient.key=='A')return compose_local_message(p,target,to,ports_.caller_name())==MessageComposeOutcome::failed?std::optional<std::string>{"Message composition failed; no save is claimed."}:std::optional<std::string>{""};
        }
        const auto chosen=select_local_message_recipient(selection);if(chosen.outcome==MessageSelectionOutcome::failed)return {};if(!chosen.recipient)return "";target.recipient=chosen.recipient->account;to=chosen.recipient->name;
    }
    return compose_local_message(p,target,to,ports_.caller_name())==MessageComposeOutcome::failed?std::optional<std::string>{"Message composition failed; no save is claimed."}:std::optional<std::string>{""};
}
std::optional<std::string> MessageCallerSession::action(MenuAction action){
    if(!is_message_action(action))return std::string(unavailable_menu_feature);
    auto& p=ports_.compose;MessageSelectionPorts selection{p.storage,p.terminal,p.actor};
    selection.columns=[&]{return p.dimensions().columns;};
    selection.rows=[&]{return p.dimensions().rows;};
    if(!refresh_selection())return "Message selection could not be refreshed.";
    preview_.reset();
    if(action==MenuAction::message_change){
        const auto area=select_local_message_echo(selection);
        if(area.outcome==MessageSelectionOutcome::failed)return {};
        if(!area.selection)return "";
        const auto& next=*area.selection;
        if(!selected_ || !(selected_->echo.id==next.echo.id)){
            const auto actor=p.actor();
            if(!actor || !p.storage.remember_message_area(*actor,next.echo.id))
                return "Conference selection could not be saved. Please try again.";
        }
        selected_=MessageArea{next.group,next.echo,next.configuration_revision};
        const auto choice=p.terminal.read_key({"message.change.read",
            "Read Msg Conference #"+std::to_string(next.echo.conference_number)+" - \""+next.group.name+": "+next.echo.name+"\"? [Y/n] ",
            {'Y','N'},'Y'});
        if(choice.outcome==TerminalOutcome::cancelled || choice.key=='N')return "";
        if(!choice.has_key())return {};
        return result_text(read_conference(next.echo.id));
    }
    if(action==MenuAction::message_enter || action==MenuAction::comment_sysop)return enter(action==MenuAction::comment_sysop);
    if(action==MenuAction::message_settings)return result_text(edit_message_scan_settings({selection,p.operation,p.dimensions,p.byte,p.write,p.accepted_key,ports_.lightbars_enabled}));
    if(action==MenuAction::message_read)return read_menu();
    if(action==MenuAction::message_addressed)return specific_caller();
    if(action==MenuAction::message_personal)return personal_menu();
    const auto scope=scan_scope();
    if(!scope)return {};
    if(*scope=='Q')return "";
    const auto areas=conferences(*scope);
    if(!areas)return {};
    if(areas->empty())return "No eligible readable conferences.";
    MessageQueryOptions options;
    if(action==MenuAction::message_search){
        const auto text=p.terminal.read_visible({"message.search.text","Literal Subject/body text (maximum 128 characters): ",
            128,EmptyInputPolicy::use_default,{}});
        if(cancelled(text) || text.outcome==TerminalOutcome::empty)return "";
        if(!text.has_value())return {};
        options.search=text.value;
    }
    return browse_results(*areas,options,*scope!='T');
}
}
