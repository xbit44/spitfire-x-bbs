#include "sfx/application/message_selection.hpp"
#include "sfx/application/message_presentation.hpp"
#include <algorithm>
#include <charconv>
#include <utility>

namespace sfx::application {
namespace {
bool ok(TerminalOutcome o){return o==TerminalOutcome::completed || o==TerminalOutcome::empty;}
bool message(const MessageSelectionPorts& p,std::string text){
    if(p.columns)text=wrap_message_prompt(text,p.columns(),0);
    return present_message_text(p.terminal,"message.selection.feedback",text);
}
std::optional<std::size_t> index(std::string_view s){std::size_t n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size() || !n)return {};return n;}
bool quit(const TerminalInputResult& input){return input.outcome==TerminalOutcome::cancelled || input.outcome==TerminalOutcome::empty || input.value=="Q" || input.value=="q";}
}
StorageResult<std::optional<MessageAreaSelection>> first_local_message_echo(IMessageStorage& store,const MessageActor& actor){
    MessageCatalogCursor group_cursor;
    for(;;){
        const auto groups=store.message_group_page(actor,group_cursor);if(!groups || !groups.value)return {groups.status,{}};
        for(const auto& group:groups.value->entries){
            MessageCatalogCursor echo_cursor;echo_cursor.configuration_revision=groups.value->configuration_revision;echo_cursor.page_size=1;
            const auto echoes=store.local_echo_page(actor,group.id,echo_cursor);if(!echoes || !echoes.value)return {echoes.status,{}};
            if(!echoes.value->entries.empty())return {StorageStatus::success(),std::optional<MessageAreaSelection>{MessageAreaSelection{group,echoes.value->entries.front(),groups.value->configuration_revision}}};
        }
        if(!groups.value->next)return {StorageStatus::success(),std::optional<MessageAreaSelection>{}};
        group_cursor=*groups.value->next;
    }
}
MessageAreaSelectionResult select_local_message_echo(const MessageSelectionPorts& p){
    if(!p.actor || !p.page_size || p.page_size>kMessagePageMaximum)return {MessageSelectionOutcome::failed,{}};
    const auto page_size=[&](unsigned reserved){
        const auto rows=p.rows?p.rows():24U;
        return std::min(p.page_size,static_cast<std::size_t>(rows>reserved?rows-reserved:1));
    };
    MessageCatalogCursor group_cursor;group_cursor.page_size=page_size(5);
    std::string group_feedback;
    for(;;){
        auto actor=p.actor();if(!actor)return {MessageSelectionOutcome::failed,{}};
        const auto groups=p.storage.message_group_page(*actor,group_cursor);
        if(!groups || !groups.value){message(p,"Group selection changed or is unavailable. Please choose again.");return {};}
        if(groups.value->entries.empty() && !groups.value->next){message(p,"No accessible message groups.");return {};}
        if(p.terminal.clear_screen()!=TerminalOutcome::completed)return {MessageSelectionOutcome::failed,{}};
        if(p.terminal.present({"message.selection.heading",">>>>> MESSAGE GROUPS <<<<<\r\n"})!=TerminalOutcome::completed)
            return {MessageSelectionOutcome::failed,{}};
        for(std::size_t i=0;i<groups.value->entries.size();++i)if(!message(p,std::to_string(i+1)+". "+groups.value->entries[i].name))return {MessageSelectionOutcome::failed,{}};
        if(!group_feedback.empty() && !message(p,std::exchange(group_feedback,{})))return {MessageSelectionOutcome::failed,{}};
        const auto input=p.terminal.read_visible({"message.group.choose",
            groups.value->next?"Group number, [N] Next, [Q]/Escape cancel: ":"Group number, [Q]/Escape cancel: ",
            8,EmptyInputPolicy::use_default,{}});
        if(quit(input))return {};
        if(!ok(input.outcome))return {MessageSelectionOutcome::failed,{}};
        if(input.value=="N" || input.value=="n"){if(groups.value->next)group_cursor=*groups.value->next;else group_feedback="No further group page.";continue;}
        const auto chosen=index(input.value);if(!chosen || *chosen>groups.value->entries.size()){group_feedback="Choose a listed group number.";continue;}
        const auto group=groups.value->entries[*chosen-1];MessageCatalogCursor echo_cursor;echo_cursor.configuration_revision=groups.value->configuration_revision;echo_cursor.page_size=page_size(6);
        std::string echo_feedback;
        for(;;){
            actor=p.actor();if(!actor)return {MessageSelectionOutcome::failed,{}};
            const auto echoes=p.storage.local_echo_page(*actor,group.id,echo_cursor);
            if(!echoes || !echoes.value){message(p,"Conference selection changed or is unavailable. Original selection retained.");return {};}
            if(p.terminal.clear_screen()!=TerminalOutcome::completed)return {MessageSelectionOutcome::failed,{}};
            if(p.terminal.present({"message.selection.heading",">>>>> MESSAGE CONFERENCES <<<<<\r\n"})!=TerminalOutcome::completed ||
                !message(p,"GROUP: "+group.name))return {MessageSelectionOutcome::failed,{}};
            for(const auto& echo:echoes.value->entries)if(!message(p,std::to_string(echo.conference_number)+". "+echo.name))return {MessageSelectionOutcome::failed,{}};
            if(echoes.value->entries.empty() && !message(p,"No accessible conferences in this group."))return {MessageSelectionOutcome::failed,{}};
            if(!echo_feedback.empty() && !message(p,std::exchange(echo_feedback,{})))return {MessageSelectionOutcome::failed,{}};
            const auto response=p.terminal.read_visible({"message.echo.choose",
                echoes.value->next?"Conference number, [N] Next, [Q]/Escape cancel: ":"Conference number, [Q]/Escape cancel: ",
                8,EmptyInputPolicy::use_default,{}});
            if(quit(response))return {};
            if(!ok(response.outcome))return {MessageSelectionOutcome::failed,{}};
            if(response.value=="N" || response.value=="n"){if(echoes.value->next)echo_cursor=*echoes.value->next;else echo_feedback="No further conference page.";continue;}
            const auto number=index(response.value);const auto selected=std::find_if(echoes.value->entries.begin(),echoes.value->entries.end(),[&](const LocalEcho& echo){return number && echo.conference_number==*number;});
            if(selected==echoes.value->entries.end()){echo_feedback="Choose a listed conference number.";continue;}
            actor=p.actor();if(!actor)return {MessageSelectionOutcome::failed,{}};
            echo_cursor.configuration_revision=echoes.value->configuration_revision;
            const auto current=p.storage.local_echo_page(*actor,group.id,echo_cursor);
            if(!current || !current.value || std::none_of(current.value->entries.begin(),current.value->entries.end(),[&](const LocalEcho& echo){return echo.id==selected->id && echo.revision==selected->revision;})){
                message(p,"Selection changed; original group/conference retained. Choose again.");return {};
            }
            return {MessageSelectionOutcome::selected,MessageAreaSelection{group,*selected,echoes.value->configuration_revision}};
        }
    }
}
MessageRecipientSelectionResult select_local_message_recipient(const MessageSelectionPorts& p,bool complete_name){
    const auto input=p.terminal.read_visible({"message.recipient.name",complete_name?"Enter complete name to search for: ":"Caller name ([Q]/Escape cancels): ",kAccountNameMaximum,EmptyInputPolicy::use_default,{}});
    if(quit(input))return {};
    if(!ok(input.outcome))return {MessageSelectionOutcome::failed,{}};
    std::optional<MessageRecipient> after;
    for(;;){
        const auto actor=p.actor();if(!actor)return {MessageSelectionOutcome::failed,{}};
        const auto page=p.storage.resolve_message_recipient(*actor,input.value,after,true);
        if(!page || !page.value){message(p,"Recipient selection is unavailable.");return {};}
        for(const auto& recipient:page.value->entries){
            if(complete_name){auto name=recipient.name,query=input.value;const auto fold=[](std::string& s){for(auto& c:s)if(c>='A' && c<='Z')c=static_cast<char>(c+32);};fold(name);fold(query);if(name!=query)continue;}
            if(!message(p,"Caller: "+recipient.name))return {MessageSelectionOutcome::failed,{}};
            const auto confirm=p.terminal.read_key({"message.recipient.confirm","Correct caller? [Y]es [N]o [Q]uit: ",{'Y','N','Q'},'N'});
            if(confirm.outcome==TerminalOutcome::cancelled || confirm.key=='Q')return {};
            if(!confirm.has_key())return {MessageSelectionOutcome::failed,{}};
            if(confirm.key=='Y'){
                const auto current_actor=p.actor();if(!current_actor)return {MessageSelectionOutcome::failed,{}};
                const auto current=p.storage.resolve_message_recipient(*current_actor,std::to_string(recipient.account),{});
                if(!current || !current.value || current.value->entries.size()!=1 || current.value->entries.front().revision!=recipient.revision || current.value->entries.front().name!=recipient.name){message(p,"Caller changed or became unavailable; choose again.");return {};}
                return {MessageSelectionOutcome::selected,recipient};
            }
        }
        if(!page.value->next_after){
            if(!message(p,"No such user in the caller database"))return {MessageSelectionOutcome::failed,{}};
            // This response must remain visible until Enter, not disappear into
            // the outer Message Menu redraw. Other keys and Escape do not ack it.
            for(;;){
                const auto acknowledgment=p.terminal.read_visible({"message.recipient.absent.enter","<enter>",1,EmptyInputPolicy::use_default,{},true});
                if(acknowledgment.outcome==TerminalOutcome::empty ||
                    (acknowledgment.outcome==TerminalOutcome::completed && acknowledgment.value.empty()))return {};
                if(acknowledgment.outcome!=TerminalOutcome::completed && acknowledgment.outcome!=TerminalOutcome::cancelled)
                    return {MessageSelectionOutcome::failed,{}};
            }
        }
        after=page.value->next_after;
    }
}
} // namespace sfx::application
