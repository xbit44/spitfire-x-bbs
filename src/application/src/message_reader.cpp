#include "sfx/application/message_reader.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/application/message_source.hpp"
#include "sfx/application/menu.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
namespace {
bool say(const MessageReaderPorts& p,std::string text){return present_message_text(p.terminal,"message.reader",text);}
bool quit(const TerminalInputResult& r){return r.outcome==TerminalOutcome::cancelled || r.outcome==TerminalOutcome::empty || r.value=="Q" || r.value=="q";}
std::optional<std::size_t> positive(std::string_view s){std::size_t n{};const auto result=std::from_chars(s.data(),s.data()+s.size(),n);if(!n || result.ec!=std::errc{} || result.ptr!=s.data()+s.size())return {};return n;}
bool failed_input(TerminalOutcome o){return o!=TerminalOutcome::completed && o!=TerminalOutcome::empty && o!=TerminalOutcome::cancelled;}
struct Chosen {
    bool failed{};
    std::optional<MessageHeader> header;
    bool cancelled{};
    MessageReaderOutcome navigation{MessageReaderOutcome::completed};
    unsigned footer_row{};
};
Chosen next_header(const MessageReaderPorts& p,const MessageQueryContext& c,MessageQueryCursor cursor){
    for(;;){
        if(p.safe_boundary && !p.safe_boundary())return {true,{}};
        const auto actor=p.actor();if(!actor)return {true,{}};
        const auto page=p.storage.query_message_headers(*actor,c,cursor,1);
        if(!page || !page.value)return {true,{}};
        if(!page.value->entries.empty())return {false,page.value->entries.front()};
        if(!page.value->next)return {};
        cursor=*page.value->next;
    }
}
std::string reference(const MessageHeader& h){return h.target.kind==MessageKind::private_ftn?"NetMail #"+std::to_string(h.identity.id.value):h.target.kind==MessageKind::private_local?(h.remote_author?"Private NetMail #":"Private local mail #")+std::to_string(h.identity.id.value):h.group_name+" / "+h.echo_name+" ("+std::to_string(h.conference_number)+") #"+std::to_string(h.identity.area_number);}
#include "message_reader_list.inc"
#include "message_reader_pager.inc"
TerminalOutcome display_body(const MessageReaderPorts& p,const OpenedMessage& message,ReaderPager& pager){
    const auto dimensions=p.dimensions?p.dimensions():MessageEditorDimensions{};
    const auto color=p.text_color?p.text_color():std::uint8_t{7};
    if(color>15)return TerminalOutcome::invalid;
    TerminalOutcome failure=TerminalOutcome::completed;
    const auto field=[&](std::string_view label,std::string_view value={}){
        TerminalDisplayProgram row;
        const auto add=[&](TerminalDisplayOperationKind kind,std::string bytes={}){row.output_byte_count+=kind==TerminalDisplayOperationKind::logical_newline?2:bytes.size();row.operations.push_back({kind,std::move(bytes)});};
        if(dimensions.ansi)add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m");
        if(!label.empty())add(TerminalDisplayOperationKind::cp437_text,std::string(label));
        if(dimensions.ansi)add(TerminalDisplayOperationKind::ansi_sequence,menu_text_sgr(color));
        if(!value.empty())add(TerminalDisplayOperationKind::cp437_text,std::string(value));
        add(TerminalDisplayOperationKind::logical_newline);failure=pager.show(row);return failure==TerminalOutcome::completed;
    };
    struct RestoreStyle {ICallerTerminal& terminal;bool ansi;~RestoreStyle(){
        if(ansi){TerminalDisplayProgram reset;reset.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,"\x1b[0m"});reset.output_byte_count=4;terminal.present_display({"message.source.restore",&reset});}
    }} restore{p.terminal,dimensions.ansi};

    const auto& h=message.header;
    const auto actor=p.actor();if(!actor)return TerminalOutcome::output_failure;
    std::string from=h.from;if(h.remote_author&&!h.remote_author->address.empty())from+=" ("+h.remote_author->address+")";
    else if(h.target.remote&&!h.target.remote->source_aka.empty())from+=" ("+h.target.remote->source_aka+")";
    std::string to=h.to;if(h.target.remote&&!h.target.remote->address.empty())to+=" ("+h.target.remote->address+")";
    if(!field("Message: "+reference(h)) || !field("Subject: ",h.subject) || !field("To: ",to) || !field("From: ",from) ||
        !field("Date: ",p.date?p.date(h.identity.utc_seconds):std::to_string(h.identity.utc_seconds)+" UTC") ||
        (h.permanent && !field("Permanent")) || !field(""))return failure;
    const auto rows=std::clamp<std::size_t>(p.page_length,kTerminalMinimumPageLength,kTerminalMaximumPageLength);
    const auto canvas_capable=dimensions.ansi && dimensions.columns>=80;
    const auto* body=&message.body;
    std::optional<MessageCanvas> artwork;
    const bool source_plain=message.original_source && message.original_source->format==MessageSourceFormat::plain;
    const bool synchronet_pipes=message.pipe_colors && !message.received_canvas &&
        (source_plain || message.plain_rendition_source) && (!p.legacy_pipe_colors || !p.legacy_pipe_colors());
    if(synchronet_pipes){
        const auto& source=source_plain?message.original_source->bytes:message.plain_rendition_source->canonical_lf;
        artwork=message_synchronet_pipe_canvas(source,color);
        if(!artwork && source_plain)artwork=message_source_rendition(*message.original_source,false,message.ansi_colors,color);
        if(artwork)body=&artwork->text;
        else if(message.plain_rendition_source)body=&*message.plain_rendition_source;
        else return TerminalOutcome::invalid;
    }else if(message.received_canvas && canvas_capable)artwork=*message.received_canvas;
    else if(message.original_source && canvas_capable){
        artwork=message_source_rendition(*message.original_source,message.pipe_colors,message.ansi_colors,color);
        if(!artwork)return TerminalOutcome::invalid;
    }
    else if(message.plain_rendition_source && canvas_capable){
        artwork=message_plain_canvas(*message.plain_rendition_source,message.pipe_colors,color);
        if(!artwork)return TerminalOutcome::invalid;
    }

    for(std::size_t first=0;first<body->rows.size();first+=rows){
        TerminalDisplayProgram program;const auto last=std::min(first+rows,body->rows.size());
        if(artwork && canvas_capable){const auto page=message_canvas_page(*artwork,first,last-first,true);if(!page)return TerminalOutcome::invalid;program=*page;}
        else {
            if(dimensions.ansi){const auto sgr=menu_text_sgr(color);program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,sgr});program.output_byte_count+=sgr.size();}
            for(auto i=first;i<last;++i){if(!body->rows[i].empty())program.operations.push_back({TerminalDisplayOperationKind::cp437_text,body->rows[i]});program.operations.push_back({TerminalDisplayOperationKind::logical_newline,{}});program.output_byte_count+=body->rows[i].size()+2;}
        }
        // Body data never enters the display-file interpreter or token engine.
        const auto output=pager.show(program,!message.original_source);if(output!=TerminalOutcome::completed)return output;
    }
    return pager.run();
}
}
MessageReaderOutcome read_local_messages(const MessageReaderPorts& p,const MessageQueryOptions& options,bool headers_only,std::optional<std::int64_t> number){
    if(!p.actor)return MessageReaderOutcome::failed;
    auto actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
    const auto begun=p.entry_context?StorageResult<MessageQueryContext>{StorageStatus::success(),p.entry_context}:p.storage.begin_message_query(*actor,options);
    if(!begun || !begun.value){say(p,"Messages are unavailable or the selected conference changed.");return MessageReaderOutcome::failed;}
    const auto context=*begun.value;
    HeaderListState list;
    Chosen selected;
    bool manual=headers_only || number.has_value();
    if(headers_only)selected=list_headers(p,context,list);
    else if(number){const auto h=p.storage.message_number_header(*actor,context,*number);if(h && h.value)selected.header=*h.value;else if(h.status.code!=StorageErrorCode::not_found)selected.failed=true;}
    else selected=next_header(p,context,{});
    if(selected.navigation!=MessageReaderOutcome::completed)return selected.navigation;
    if(selected.failed){say(p,"Message query failed; no empty result is claimed.");return MessageReaderOutcome::failed;}
    if(selected.cancelled)return MessageReaderOutcome::cancelled;
    if(!selected.header){if(!headers_only && !say(p,options.mode==MessageQueryMode::personal?(options.unread_only?"No new messages addressed to you.":"No messages addressed to you."):"No available message."))return MessageReaderOutcome::failed;return MessageReaderOutcome::completed;}
    for(;;){
        actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
        auto header=*selected.header;
        const auto preview=p.preview?p.preview(selected.footer_row):std::optional<bool>{false};
        if(!preview)return MessageReaderOutcome::failed;
        auto opened=p.storage.open_query_message(*actor,context,header.identity.id,!manual,header.position.history,*preview);
        if(!opened || !opened.value){
            if(opened.status.code!=StorageErrorCode::not_found){say(p,"Message context changed or storage is unavailable; return and refresh.");return MessageReaderOutcome::failed;}
            if(manual){
                if(!p.return_to_list){say(p,"That message is no longer available.");return MessageReaderOutcome::completed;}
                selected=list_headers(p,context,list);
                if(selected.failed)return MessageReaderOutcome::failed;
                if(selected.cancelled)return MessageReaderOutcome::cancelled;
                if(!selected.header)return selected.navigation;
                continue;
            }
            selected=next_header(p,context,header.position);if(selected.failed)return MessageReaderOutcome::failed;
            if(!selected.header){say(p,"No further available messages.");return MessageReaderOutcome::completed;}continue;
        }
        ReaderPager pager(p);
        const auto initial_authority=p.storage.message_delete_authority(*actor);if(!initial_authority||!initial_authority.value)return MessageReaderOutcome::failed;
        const auto d=p.dimensions?p.dimensions():MessageEditorDimensions{};
        const auto commands=message_reader_command_program(d.ansi,d.columns,*initial_authority.value||(opened.value->header.target.kind==MessageKind::private_local&&opened.value->header.target.recipient==actor->account&&!opened.value->header.inbox_removed),!p.sysop_review_only&&*initial_authority.value&&opened.value->header.target.kind==MessageKind::public_echo,!p.sysop_review_only,p.return_to_list,p.allow_skip,opened.value->header.permanent);
        pager.footer_rows(1+std::count_if(commands.operations.begin(),commands.operations.end(),[](const auto& op){return op.kind==TerminalDisplayOperationKind::logical_newline;}));
        const auto output=display_body(p,*opened.value,pager);
        if(failed_input(output))return MessageReaderOutcome::failed;
        for(;;){
            actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
            const auto authority=p.storage.message_delete_authority(*actor);
            if(!authority || !authority.value)return MessageReaderOutcome::failed;
            if(*authority.value && !p.operation)return MessageReaderOutcome::failed;
            const bool global_delete=*authority.value;
            const bool can_mark=!p.sysop_review_only && global_delete && opened.value->header.target.kind==MessageKind::public_echo;
            bool protected_message=opened.value->header.permanent;
            if(can_mark){
                const auto current=p.storage.message_permanent_state(*actor,header.identity.id);
                if(!current || !current.value){
                    if(current.status.code==StorageErrorCode::not_found){say(p,"Message no longer available.");return MessageReaderOutcome::completed;}
                    return MessageReaderOutcome::failed;
                }
                protected_message=*current.value;
            }
            const bool can_delete=global_delete || (opened.value->header.target.kind==MessageKind::private_local && opened.value->header.target.recipient==actor->account && !opened.value->header.inbox_removed);
            if(p.safe_boundary && !p.safe_boundary())return MessageReaderOutcome::failed;
            std::vector<char> allowed{'N','-','L','Q'};if(p.allow_skip)allowed.push_back('K');if(!p.sysop_review_only)allowed.push_back('R');if(can_delete)allowed.push_back('D');if(can_mark)allowed.push_back('P');
            const auto dimensions=p.dimensions?p.dimensions():MessageEditorDimensions{};
            TerminalKeyResult input;
            bool return_on_enter=false;
            if(pager.message_command){input={TerminalOutcome::completed,pager.message_command};pager.message_command.reset();}
            else {
                const auto footer=message_reader_command_program(dimensions.ansi,dimensions.columns,can_delete,can_mark,!p.sysop_review_only,p.return_to_list,p.allow_skip,protected_message);
                if(pager.command_footer(1+std::count_if(footer.operations.begin(),footer.operations.end(),[](const auto& op){return op.kind==TerminalDisplayOperationKind::logical_newline;}))!=TerminalOutcome::completed ||
                   p.terminal.present_display({"message.reader.command.options",&footer,{}})!=TerminalOutcome::completed)return MessageReaderOutcome::failed;
                input=p.terminal.read_key({"message.reader.command",dimensions.columns<4?"\n":" ",allowed,p.return_to_list?'L':'N',false,true,true});
                return_on_enter=p.return_to_list && input.outcome==TerminalOutcome::empty;
            }
            if(input.navigation==TerminalNavigationKey::up || input.navigation==TerminalNavigationKey::down){
                const auto moved=pager.scroll(input.navigation==TerminalNavigationKey::up?-1:1);
                if(failed_input(moved))return MessageReaderOutcome::failed;
                const auto continued=pager.wait(true);if(failed_input(continued))return MessageReaderOutcome::failed;continue;
            }
            if(input.navigation==TerminalNavigationKey::left)input={TerminalOutcome::completed,'-'};
            if(input.navigation==TerminalNavigationKey::right)input={TerminalOutcome::completed,'N'};
            if(input.outcome==TerminalOutcome::cancelled || input.key=='Q')return MessageReaderOutcome::cancelled;
            if(!input.has_key())return MessageReaderOutcome::failed;
            if(input.key=='K' && p.allow_skip)return MessageReaderOutcome::skipped;
            if(return_on_enter)input.key='L';
            if(std::find(allowed.begin(),allowed.end(),*input.key)==allowed.end())continue;
            if(input.key=='R'){if(!p.reply || !p.reply(*opened.value))return MessageReaderOutcome::failed;pager.invalidate();continue;}
            if(input.key=='P'){
                pager.invalidate();
                const bool desired=!protected_message;
                const auto confirm=p.terminal.read_key({"message.permanent.confirm",desired?"Mark this message Permanent [y/N]? ":"Remove Permanent status [y/N]? ",{'Y','N'},'N'});
                if(confirm.outcome==TerminalOutcome::cancelled || confirm.key=='N')continue;
                if(!confirm.has_key())return MessageReaderOutcome::failed;
                if(confirm.key!='Y')continue;
                actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
                const auto operation=p.operation();
                auto changed=p.storage.set_message_permanent(*actor,operation,header.identity.id,protected_message,desired);
                while(!changed && changed.status.code!=StorageErrorCode::conflict && changed.status.code!=StorageErrorCode::not_found && changed.status.code!=StorageErrorCode::permission_denied){
                    if(!say(p,"Permanent update not confirmed; no success is claimed."))return MessageReaderOutcome::failed;
                    const auto retry=p.terminal.read_key({"message.permanent.reconcile","[R] Reconcile the same update, [Q] Return: ",{'R','Q'},'Q'});
                    if(!retry.has_key() || retry.key!='R')return MessageReaderOutcome::completed;
                    actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
                    changed=p.storage.set_message_permanent(*actor,operation,header.identity.id,protected_message,desired);
                }
                if(!changed){pager.notice("Permanent status or authority changed; refresh before trying again.");continue;}
                opened.value->header.permanent=desired;
                pager.notice(desired?"Message marked Permanent.":"Permanent status removed.");
                continue;
            }
            if(input.key=='D'){
                if(global_delete && protected_message){pager.notice(p.sysop_review_only?"Deletion refused: this message is Permanent.":"Remove Permanent status before deleting this message.");continue;}
                pager.invalidate();
                const auto confirm=p.terminal.read_key({global_delete?"message.delete.confirm":"message.inbox.remove.confirm",
                    global_delete?"Permanently delete "+reference(header)+" for everyone [y/N]? ":"Delete this message [Y/n]? ",{'Y','N'},global_delete?'N':'Y'});
                if(confirm.outcome==TerminalOutcome::cancelled || confirm.key=='N')continue;
                if(!confirm.has_key())return MessageReaderOutcome::failed;
                if(confirm.key!='Y')continue;
                actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
                if(global_delete){
                    const auto operation=p.operation();
                    auto removed=p.storage.delete_message(*actor,operation,header.identity.id);
                    if(!removed && removed.status.code==StorageErrorCode::permission_denied){pager.notice(p.sysop_review_only?"Deletion refused: the message is Permanent or access changed.":"Deletion refused; remove Permanent status first.");continue;}
                    while(!removed){
                        if(!say(p,"Deletion not confirmed; no success is claimed."))return MessageReaderOutcome::failed;
                        const auto retry=p.terminal.read_key({"message.delete.reconcile","[R] Reconcile deletion, [Q] Return: ",{'R','Q'},'Q'});
                        if(!retry.has_key() || retry.key!='R')return MessageReaderOutcome::failed;
                        actor=p.actor();if(!actor)return MessageReaderOutcome::failed;
                        removed=p.storage.delete_message(*actor,operation,header.identity.id);
                        if(!removed && removed.status.code==StorageErrorCode::permission_denied)break;
                    }
                    if(!removed){pager.notice(p.sysop_review_only?"Deletion refused: the message is Permanent or access changed.":"Deletion refused; remove Permanent status first.");continue;}
                    if(!say(p,"Message permanently deleted."))return MessageReaderOutcome::failed;
                }else{
                    const auto removed=p.storage.remove_private_inbox_item(*actor,header.identity.id);if(!removed){pager.notice("Inbox removal failed; no removal is claimed.");continue;}
                    if(!say(p,"Removed from your inbox. Sender copy retained."))return MessageReaderOutcome::failed;
                }
                header.position.reverse=false;manual=false;selected=next_header(p,context,header.position);
            }else if(input.key=='L'){
                selected=list_headers(p,context,list);
                if(selected.navigation!=MessageReaderOutcome::completed)return selected.navigation;
                if(selected.cancelled && p.return_to_list)return MessageReaderOutcome::cancelled;
                if(!selected.failed && !selected.header){pager.invalidate();continue;}
                manual=true;
            }
            else {header.position.reverse=input.key=='-';manual=header.position.reverse;selected=next_header(p,context,header.position);}
            if(selected.failed){say(p,"Query changed or failed; refresh from the menu.");return MessageReaderOutcome::failed;}
            if(!selected.header){
                if(input.key=='-' || (p.return_to_list && input.key=='N')){
                    pager.notice(input.key=='-'?"No previous available message.":"No next available message.");
                    continue;
                }
                say(p,"End of messages.");return MessageReaderOutcome::completed;
            }
            break;
        }
    }
}
static MessageReaderOutcome edit_message_scan_settings_plain(const MessageScanSettingsPorts& p){
    if(!p.operation || !p.selection.actor)return MessageReaderOutcome::failed;
    for(;;){
    const auto area=select_local_message_echo(p.selection);if(area.outcome==MessageSelectionOutcome::failed)return MessageReaderOutcome::failed;if(!area.selection)return MessageReaderOutcome::cancelled;
    auto& terminal=p.selection.terminal;auto& store=p.selection.storage;
    const auto feedback=[&](std::string text){return present_message_text(terminal,"message.scan.settings",text);};
    for(;;){
        auto actor=p.selection.actor();if(!actor)return MessageReaderOutcome::failed;
        const auto pref=store.message_scan_preference(*actor,area.selection->echo.id);
        if(!pref || !pref.value){feedback("Scan settings changed or are unavailable.");return MessageReaderOutcome::failed;}
        if(!feedback(area.selection->group.name+" / "+area.selection->echo.name+": Unread "+(pref.value->unread?"On":"Off")+", To-you "+(pref.value->to_you?"On":"Off")))return MessageReaderOutcome::failed;
        const auto command=terminal.read_key({"message.scan.settings.command","[U] Unread, [T] To-you, [G] Group column, [Q] Return: ",{'U','T','G','Q'},'Q'});
        if(command.outcome==TerminalOutcome::cancelled || command.key=='Q')break;
        if(!command.has_key())return MessageReaderOutcome::failed;
        if(command.key=='G'){
            const auto column=terminal.read_key({"message.scan.group.column","Group column: [U] Unread, [T] To-you, [Q] Cancel: ",{'U','T','Q'},'Q'});
            if(column.outcome==TerminalOutcome::cancelled || column.key=='Q')continue;
            if(!column.has_key())return MessageReaderOutcome::failed;
            actor=p.selection.actor();if(!actor)return MessageReaderOutcome::failed;
            const auto preview=store.preview_group_scan_column(*actor,area.selection->group.id,column.key=='U'?MessageScanColumn::unread:MessageScanColumn::to_you);
            if(!preview || !preview.value){if(!feedback("Group scan preview changed or failed."))return MessageReaderOutcome::failed;continue;}
            if(!feedback(area.selection->group.name+": Set "+std::to_string(preview.value->entries.size())+" conferences' "+
                (column.key=='U'?"Unread":"To-you")+" setting "+(preview.value->enable?"On":"Off")+"?"))return MessageReaderOutcome::failed;
            const auto confirm=terminal.read_key({"message.scan.group.confirm","Apply group column [y/N]? ",{'Y','N'},'N'});
            if(confirm.outcome==TerminalOutcome::cancelled || confirm.key=='N')continue;
            if(!confirm.has_key())return MessageReaderOutcome::failed;
            const auto operation=p.operation();
            actor=p.selection.actor();if(!actor)return MessageReaderOutcome::failed;
            auto applied=store.toggle_group_scan_column(*actor,operation,*preview.value);
            // Explicit same-operation retry reconciles a possibly committed batch.
            if(!applied){const auto retry=terminal.read_key({"message.scan.group.reconcile","Outcome not confirmed. [R] Reconcile, [Q] Return: ",{'R','Q'},'Q'});if(retry.key=='R'){actor=p.selection.actor();if(!actor)return MessageReaderOutcome::failed;applied=store.toggle_group_scan_column(*actor,operation,*preview.value);}else return MessageReaderOutcome::failed;}
            if(!feedback(applied?"Group scan column saved.":"Group scan column not confirmed; refresh before another apply."))return MessageReaderOutcome::failed;
            if(!applied)return MessageReaderOutcome::failed;
        }else{
            actor=p.selection.actor();if(!actor)return MessageReaderOutcome::failed;
            const bool unread=command.key=='U';const auto result=store.set_message_scan_preference(*actor,area.selection->echo.id,pref.value->revision,unread?MessageScanColumn::unread:MessageScanColumn::to_you,unread?!pref.value->unread:!pref.value->to_you);
            if(!feedback(result?"Scan preference saved.":"Scan preference changed or failed; refreshing."))return MessageReaderOutcome::failed;
        }
    }
    }
}
namespace {
struct ScanGroup { MessageGroup group; std::size_t count{}; };
std::string scan_text(std::string_view text,std::size_t width){
    std::string out;for(unsigned char c:text){if(out.size()==width)break;out+=c<32 || c==127?' ':static_cast<char>(c);}
    out.resize(width,' ');return out;
}
bool scan_groups(const MessageScanSettingsPorts& p,std::vector<ScanGroup>& groups){
    groups.clear();MessageCatalogCursor cursor;cursor.page_size=kMessagePageMaximum;
    for(;;){
        const auto actor=p.selection.actor();if(!actor)return false;
        const auto page=p.selection.storage.message_group_page(*actor,cursor);
        if(!page || !page.value)return false;
        for(const auto& group:page.value->entries){
            const auto preview=p.selection.storage.preview_group_scan_column(*actor,group.id,MessageScanColumn::unread);
            if(!preview || !preview.value || preview.value->configuration_revision!=page.value->configuration_revision)return false;
            // Bound the presentation cache independently of storage continuations.
            if(groups.size()==65535)return false;
            groups.push_back({group,preview.value->entries.size()});
        }
        if(!page.value->next)return true;
        cursor=*page.value->next;
    }
}
void scan_move(MessageEditorKey key,std::size_t& row,std::size_t count,std::size_t height){
    if(!count){row=0;return;}
    if(key==MessageEditorKey::up && row)--row;
    else if(key==MessageEditorKey::down)row=std::min(row+1,count-1);
    else if(key==MessageEditorKey::home)row=0;
    else if(key==MessageEditorKey::end)row=count-1;
    else if(key==MessageEditorKey::page_up)row=row>height?row-height:0;
    else if(key==MessageEditorKey::page_down)row=std::min(row+height,count-1);
}
MessageReaderOutcome scan_lightbars(const MessageScanSettingsPorts& p){
    std::vector<ScanGroup> groups;
    auto& store=p.selection.storage;
    if(!scan_groups(p,groups))return MessageReaderOutcome::failed;
    std::optional<MessageGroup> group;
    std::size_t group_row{},echo_row{},top{};bool to_you{};
    std::vector<std::string> rendered;
    std::string feedback;
    MessageEditorKeyDecoder keys;
    std::optional<MessageGroupScanPreview> confirmation;
    std::optional<std::pair<Identifier128,MessageGroupScanPreview>> uncertain;
    std::int64_t catalog_revision=-1,account_revision=-1,policy_revision=-1;
    for(;;){
        const auto size=p.dimensions();
        if(!size.ansi || size.columns<80 || size.rows<24 || (p.lightbars_enabled && !p.lightbars_enabled())){
            if(!p.write("\x1b[?7h\x1b[0m\r\n"))return MessageReaderOutcome::failed;
            // Never abandon an uncertain operation and issue a fresh bulk write.
            if(uncertain)return MessageReaderOutcome::failed;
            return edit_message_scan_settings_plain(p);
        }
        const auto actor=p.selection.actor();if(!actor)return MessageReaderOutcome::failed;
        MessageCatalogCursor check_cursor;check_cursor.page_size=1;
        const auto catalog=store.message_group_page(*actor,check_cursor);
        if(!catalog || !catalog.value)return MessageReaderOutcome::failed;
        if(catalog_revision!=catalog.value->configuration_revision ||
           account_revision!=actor->account_revision || policy_revision!=actor->policy_revision){
            const auto selected_id=!groups.empty()?std::optional<MessageGroupId>{groups[std::min(group_row,groups.size()-1)].group.id}:std::nullopt;
            if(!scan_groups(p,groups))return MessageReaderOutcome::failed;
            if(selected_id){
                const auto found=std::find_if(groups.begin(),groups.end(),[&](const ScanGroup& g){return g.group.id==*selected_id;});
                if(found!=groups.end())group_row=static_cast<std::size_t>(found-groups.begin());
            }
            if(group){
                const auto found=std::find_if(groups.begin(),groups.end(),[&](const ScanGroup& g){return g.group.id==group->id;});
                if(found==groups.end()){if(uncertain)return MessageReaderOutcome::failed;group.reset();confirmation.reset();top=0;}
                else group=found->group;
            }
            catalog_revision=catalog.value->configuration_revision;
            account_revision=actor->account_revision;policy_revision=actor->policy_revision;
        }
        std::optional<MessageGroupScanPreview> preview;
        if(group){
            const auto current=store.preview_group_scan_column(*actor,group->id,MessageScanColumn::unread);
            if(!current || !current.value){
                if(uncertain)return MessageReaderOutcome::failed;
                group.reset();confirmation.reset();top=0;feedback="Group changed or unavailable; refresh selection.";
                if(!scan_groups(p,groups))return MessageReaderOutcome::failed;
            }else preview=*current.value;
        }
        const auto rows=std::clamp(size.rows,24U,200U);
        const auto height=static_cast<std::size_t>(rows-8);
        auto& selected=group?echo_row:group_row;
        const auto count=preview?preview->entries.size():groups.size();
        selected=count?std::min(selected,count-1):0;
        if(selected<top)top=selected;else if(selected>=top+height)top=selected-height+1;
        std::vector<std::string> frame(rows);
        const auto line=[&](std::size_t row,std::string_view text,std::string_view sgr){
            frame[row-1]=std::string(sgr)+scan_text(text,80);
        };
        line(1,group?group->name:"Choose a message group to configure","\x1b[1;37;40m");
        line(2,group?"U: Toggle group Unread    Y: Toggle group To-you":"Group number / description                                      Echo count","\x1b[0;37;40m");
        line(3,group?"  #  Echo                                                 Unread    To-you":"","\x1b[1;37;40m");
        line(4,"+------------------------------------------------------------------------------+","\x1b[1;30;40m");
        for(std::size_t i=0;i<height;++i){
            const auto index=top+i;const auto row=i+5;
            if(index>=count){line(row,i==0 && !count?"| No eligible entries                                                          |":"|                                                                              |","\x1b[0;37;40m");continue;}
            if(!group){
                const auto label="| "+scan_text(std::to_string(index+1),4)+scan_text(groups[index].group.name,63)+scan_text(std::to_string(groups[index].count),10)+"|";
                line(row,label,index==selected?"\x1b[0;30;47m":"\x1b[0;37;40m");
            }else{
                const auto& entry=preview->entries[index];
                const auto label="| "+scan_text(std::to_string(entry.echo.conference_number),4)+scan_text(entry.echo.name,50)+"   "+(entry.preference.unread?"[X]":"[ ]")+"       "+(entry.preference.to_you?"[X]":"[ ]")+"       |";
                line(row,label,"\x1b[0;37;40m");
                if(index==selected){
                    auto& text=frame[row-1];
                    text+="\x1b["+std::to_string(row)+";"+std::to_string(to_you?70:60)+"H\x1b[0;30;47m";
                    text+=to_you?(entry.preference.to_you?"[X]":"[ ]"):(entry.preference.unread?"[X]":"[ ]");
                }
            }
        }
        line(rows-3,"+------------------------------------------------------------------------------+","\x1b[1;30;40m");
        std::string status=uncertain?"Outcome not confirmed: [R] Reconcile, [Q] Return (no new apply)":confirmation?
            "Apply "+std::to_string(confirmation->entries.size())+" echoes "+(confirmation->column==MessageScanColumn::unread?"Unread":"To-you")+" "+(confirmation->enable?"On":"Off")+"? Y Yes / N No [N]":feedback;
        line(rows-2,status,"\x1b[1;37;40m");
        line(rows-1,group?"Up/Down Row  Left/Right Column  Enter Toggle  U/Y Group  Esc/Q Back":"Up/Down Select  Enter Open  Esc/[Q] Return","\x1b[0;37;40m");
        line(rows,"PgUp/PgDn Page  Home/End First/Last","\x1b[1;30;40m");
        if(frame!=rendered){
            const bool full=frame.size()!=rendered.size();
            std::string update="\x1b[?7l";
            if(full)update+="\x1b[0;37;40m\x1b[2J";
            for(std::size_t i=0;i<frame.size();++i){
                if(frame[i].empty() || (!full && frame[i]==rendered[i]))continue;
                update+="\x1b["+std::to_string(i+1)+";1H"+frame[i];
            }
            update+="\x1b[0;37;40m\x1b[?7h";
            if(!p.write(update))return MessageReaderOutcome::failed;
            rendered=std::move(frame);
        }
        const auto byte=p.byte();std::optional<MessageEditorInput> input;
        if(byte.outcome==TerminalOutcome::timed_out)input=keys.idle();else if(byte.byte)input=keys.feed(*byte.byte);else return MessageReaderOutcome::failed;
        if(!input)continue;
        const auto event=input->event;char c{};
        if(event && event->key==MessageEditorKey::text)c=static_cast<char>(event->byte>='a' && event->byte<='z'?event->byte-32:event->byte);
        const bool back=input->command==MessageEditorCommand::actions || c=='Q';
        if(uncertain){
            if(back)return MessageReaderOutcome::failed;
            if(c=='R'){
                const auto current_actor=p.selection.actor();if(!current_actor)return MessageReaderOutcome::failed;
                const auto result=store.toggle_group_scan_column(*current_actor,uncertain->first,uncertain->second);
                if(result){uncertain.reset();feedback="Group column saved.";}else feedback="Outcome still unconfirmed.";
            }continue;
        }
        if(confirmation){
            if(c=='Y'){
                const auto current_actor=p.selection.actor();if(!current_actor)return MessageReaderOutcome::failed;
                const auto operation=p.operation();
                const auto result=store.toggle_group_scan_column(*current_actor,operation,*confirmation);
                if(result)feedback="Group column saved.";
                else {uncertain=std::make_pair(operation,*confirmation);feedback="Outcome not confirmed.";}
                confirmation.reset();
            }else if(c=='N' || back || (event && event->key==MessageEditorKey::enter)){
                if(event && event->key==MessageEditorKey::enter && p.accepted_key){p.accepted_key(event->byte=='\r');keys.reset();}
                confirmation.reset();
            }
            continue;
        }
        if(back){
            if(!group)return MessageReaderOutcome::completed;
            group.reset();top=0;feedback.clear();if(!scan_groups(p,groups))return MessageReaderOutcome::failed;continue;
        }
        if(!event)continue;
        if(event->key==MessageEditorKey::enter){
            if(p.accepted_key){p.accepted_key(event->byte=='\r');keys.reset();}
            if(!group){
                if(count){group=groups[selected].group;echo_row=top=0;feedback.clear();}
            }else if(count){
                const auto& entry=preview->entries[selected];
                const auto current_actor=p.selection.actor();if(!current_actor)return MessageReaderOutcome::failed;
                const auto result=store.set_message_scan_preference(*current_actor,entry.echo.id,entry.preference.revision,to_you?MessageScanColumn::to_you:MessageScanColumn::unread,to_you?!entry.preference.to_you:!entry.preference.unread);
                feedback=result?"Preference saved.":"Update changed or failed; refreshing saved state.";
            }
        }else if(group && (c=='U' || c=='Y')){
            confirmation=*preview;confirmation->column=c=='U'?MessageScanColumn::unread:MessageScanColumn::to_you;
            confirmation->enable=std::any_of(confirmation->entries.begin(),confirmation->entries.end(),[&](const auto& e){return !(c=='U'?e.preference.unread:e.preference.to_you);});
        }else if(group && (event->key==MessageEditorKey::left || event->key==MessageEditorKey::right))to_you=event->key==MessageEditorKey::right;
        else scan_move(event->key,selected,count,height);
    }
}
} // namespace
MessageReaderOutcome edit_message_scan_settings(const MessageScanSettingsPorts& p){
    if(!p.operation || !p.selection.actor)return MessageReaderOutcome::failed;
    if(!p.dimensions || !p.byte || !p.write)return edit_message_scan_settings_plain(p);
    const auto size=p.dimensions();
    if(!size.ansi || size.columns<80 || size.rows<24 || (p.lightbars_enabled && !p.lightbars_enabled()))return edit_message_scan_settings_plain(p);
    const auto result=scan_lightbars(p);
    if(!p.write("\x1b[?7h\x1b[0m\r\n"))return MessageReaderOutcome::failed;
    return result;
}
} // namespace sfx::application
