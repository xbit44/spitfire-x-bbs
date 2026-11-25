#include "sfx/application/message_compose.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/application/message_editor_help.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
namespace {
bool completed(TerminalOutcome o){return o==TerminalOutcome::completed || o==TerminalOutcome::empty;}
bool say(const MessageComposePorts& p,std::string text){return present_message_text(p.terminal,"message.feedback",text);}
bool acknowledge_upload_feedback(const MessageComposePorts& p,std::string text){
    // Synchronized refusal is not a successful post. Keep its explanation on
    // screen until Enter; a full-screen editor redraw must not erase it first.
    if(!say(p,std::move(text)))return false;
    for(;;){
        const auto input=p.terminal.read_visible({"message.upload.refused.enter","<enter>",1,EmptyInputPolicy::use_default,{},true});
        if(input.outcome==TerminalOutcome::empty ||
            (input.outcome==TerminalOutcome::completed && input.value.empty()))return true;
        if(input.outcome!=TerminalOutcome::completed && input.outcome!=TerminalOutcome::cancelled)return false;
    }
}
std::optional<std::size_t> number(std::string_view text){std::size_t n{};const auto r=std::from_chars(text.data(),text.data()+text.size(),n);if(r.ec!=std::errc{} || r.ptr!=text.data()+text.size() || !n)return std::nullopt;return n;}
struct EditorChoice {char key;std::string label;};

class EditorPopupFrame {
public:
    EditorPopupFrame(const MessageComposePorts& ports,const MessageEditorState& editor,
        std::string_view to,std::string_view from):ports_(ports),editor_(editor),to_(to),from_(from){}

    void clear(){painted_.clear();}

    bool draw(const MessageEditorDimensions& size,std::size_t first_row,
        std::vector<std::string> rows,std::size_t final_column){
        const bool full=painted_.size()!=rows.size() || columns_!=size.columns || rows_!=size.rows;
        if(!full && rows==painted_)return true;
        auto frame=full?editor_.frame(to_,from_):std::string{};
        const auto position=[](std::size_t row,std::size_t column){
            return "\x1b["+std::to_string(row)+";"+std::to_string(column)+"H";
        };
        frame+="\x1b[?7l";
        for(std::size_t i=0;i<rows.size();++i)
            if(full || rows[i]!=painted_[i])frame+=position(first_row+i,1)+rows[i];
        frame+=position(first_row+rows.size()-1,std::min<std::size_t>(final_column,size.columns));
        frame+="\x1b[0;37;40m\x1b[?7h";
        if(!ports_.write(frame))return false;
        painted_=std::move(rows);columns_=size.columns;rows_=size.rows;
        return true;
    }
private:
    const MessageComposePorts& ports_;
    const MessageEditorState& editor_;
    std::string_view to_,from_;
    unsigned columns_{},rows_{};
    std::vector<std::string> painted_;
};

std::optional<char> lightbar(const MessageComposePorts& p,MessageEditorState& editor,
    std::string_view to,std::string_view from,MessageEditorKeyDecoder& keys,
    std::string title,const std::vector<EditorChoice>& choices,char default_key,char cancel_key){
    auto selected=static_cast<std::size_t>(std::find_if(choices.begin(),choices.end(),[&](const EditorChoice& c){return c.key==default_key;})-choices.begin());
    EditorPopupFrame popup(p,editor,to,from);
    for(;;){
        const auto size=p.dimensions();editor.resize(size.ansi,size.columns,size.rows);
        if(!editor.full_screen()){
            std::vector<char> allowed;std::string prompt=title+" ";
            for(const auto& c:choices){allowed.push_back(c.key);prompt+="["+std::string(1,c.key)+"] "+c.label+"  ";}
            const auto answer=p.terminal.read_key({"message.editor.lightbar",prompt,allowed,default_key});
            if(answer.outcome==TerminalOutcome::cancelled)return cancel_key;
            return answer.has_key()?answer.key:std::nullopt;
        }
        const auto row=std::clamp(size.rows?size.rows:24U,24U,200U);
        const std::string gray="\x1b[0;37;40m";
        std::vector<std::string> lines;
        const auto split=title.find('\n');
        if(split!=std::string::npos){
            lines.push_back(gray+"\x1b[2K"+title.substr(0,std::min<std::size_t>(split,80)));
            lines.push_back(gray+"\x1b[2K"+title.substr(split+1,80));
        }else lines.push_back(gray+"\x1b[2K"+title.substr(0,80));
        std::string buttons=gray+"\x1b[2K";
        std::size_t cells{};
        for(std::size_t i=0;i<choices.size();++i){
            buttons+=i==selected?"\x1b[0;30;47m":"\x1b[0;37;40m";
            buttons+=" "+choices[i].label+" ";
            cells+=choices[i].label.size()+2;
        }
        lines.push_back(std::move(buttons));
        const auto first_row=row+1-lines.size();
        if(!popup.draw(size,first_row,std::move(lines),cells+1))return {};
        const auto byte=p.byte();std::optional<MessageEditorInput> input;
        if(byte.outcome==TerminalOutcome::timed_out)input=keys.idle();else if(byte.byte)input=keys.feed(*byte.byte);else return {};
        if(!input)continue;
        if(input->command==MessageEditorCommand::actions || (input->event && input->event->key==MessageEditorKey::resume))return cancel_key;
        if(input->command==MessageEditorCommand::help)return std::any_of(choices.begin(),choices.end(),[](const EditorChoice& c){return c.key=='H';})?'H':cancel_key;
        if(input->command==MessageEditorCommand::save && std::any_of(choices.begin(),choices.end(),[](const EditorChoice& c){return c.key=='S';}))return 'S';
        if(input->command==MessageEditorCommand::abort && std::any_of(choices.begin(),choices.end(),[](const EditorChoice& c){return c.key=='A';}))return 'A';
        if(!input->event)continue;
        const auto event=*input->event;
        if(event.key==MessageEditorKey::left || event.key==MessageEditorKey::up)selected=selected?selected-1:choices.size()-1;
        else if(event.key==MessageEditorKey::right || event.key==MessageEditorKey::down)selected=(selected+1)%choices.size();
        else if(event.key==MessageEditorKey::enter){if(p.accepted_key){p.accepted_key(event.byte=='\r');keys.reset();}return choices[selected].key;}
        else if(event.key==MessageEditorKey::text){
            const auto upper=event.byte>='a' && event.byte<='z'?event.byte-32:event.byte;
            const auto found=std::find_if(choices.begin(),choices.end(),[&](const EditorChoice& c){return c.key==upper;});
            if(found!=choices.end()){if(p.accepted_key){p.accepted_key(false);keys.reset();}return found->key;}
        }
    }
}
bool subject_prompt(const MessageComposePorts& p,MessageEditorState& editor){
    const auto input=p.terminal.read_visible({"message.subject","Subject: ",kMessageSubjectCells,EmptyInputPolicy::use_default,editor.draft().subject()});
    if(input.outcome==TerminalOutcome::cancelled)return true;
    return completed(input.outcome) && editor.draft().set_subject(input.value)==MessageValidation::valid;
}
std::optional<char> help_prompt(const MessageComposePorts& p,MessageEditorKeyDecoder& keys){
    std::string rendered;
    for(;;){
        const auto size=p.dimensions();
        if(!size.ansi || size.columns<80 || size.rows<24){
            if(!say(p,"Ctrl-Z Save, Ctrl-A Abort, Ctrl-Q Quote, Ctrl-S Subject, Ctrl-L Help.\n/S /A /Q /T /L /E# /D# /I# /?; replacement text is literal.\n/UPLOAD or /UL: confirm, upload one file, then it posts if valid."))return {};
            const auto answer=p.terminal.read_key({"message.editor.help","S Save, A Abort, Q Quote, T Subject, E/Escape Return: ",{'S','A','Q','T','E'},'E'});
            if(answer.outcome==TerminalOutcome::cancelled)return 'E';
            return answer.has_key()?answer.key:std::nullopt;
        }
        // Address authored rows explicitly so trailing padding cannot scroll
        // the approved Help heading off a 24-row terminal. Asset bytes stay exact.
        std::string frame="\x1b[?7l\x1b[0;37;40m\x1b[2J\x1b[H";
        auto art=message_editor_help_art();std::size_t start{},row=1;
        while(start<art.size()){
            const auto end=art.find("\r\n",start);
            const auto line=art.substr(start,end==std::string_view::npos?art.size()-start:end-start);
            if(!line.empty())frame+="\x1b["+std::to_string(row)+";1H"+std::string(line);
            if(end==std::string_view::npos)break;
            start=end+2;++row;
        }
        frame+="\x1b[0;37;40m\x1b[23;1H\x1b[2K /UPLOAD or /UL: confirm, upload one file; it posts if valid.\x1b["+std::to_string(std::min(size.rows,200U))+";1H\x1b[?7h";
        if(frame!=rendered){if(!p.write(frame))return {};rendered=frame;}
        const auto byte=p.byte();std::optional<MessageEditorInput> input;
        if(byte.outcome==TerminalOutcome::timed_out)input=keys.idle();
        else if(byte.byte)input=keys.feed(*byte.byte);else return {};
        if(!input)continue;
        if(input->command==MessageEditorCommand::actions)return 'E';
        if(input->event && input->event->key==MessageEditorKey::enter){
            if(p.accepted_key){p.accepted_key(input->event->byte=='\r');keys.reset();}return 'E';
        }
        if(input->event && input->event->key==MessageEditorKey::text){
            const char c=static_cast<char>(input->event->byte>='a' && input->event->byte<='z'?input->event->byte-32:input->event->byte);
            if(c=='S' || c=='A' || c=='Q' || c=='T' || c=='E'){
                if(p.accepted_key){p.accepted_key(false);keys.reset();}return c;
            }
        }
    }
}
bool quote_prompt(const MessageComposePorts& p,MessageEditorState& editor,MessageId parent,
    std::string_view to,std::string_view from,MessageEditorKeyDecoder& keys){
    const auto actor=p.actor();if(!actor)return false;const auto source=p.storage.quote_source(*actor,parent);
    if(!source || !source.value)return say(p,"Quote source is unavailable.");
    const auto insert=[&](const std::vector<std::size_t>& selected){
        const auto current_actor=p.actor();if(!current_actor)return MessageValidation::invalid_selection;
        const auto current=p.storage.quote_source(*current_actor,parent);
        if(!current || !current.value)return MessageValidation::invalid_selection;
        return editor.quote(current.value->body.rows,selected);
    };
    auto dimensions=p.dimensions();
    if(dimensions.ansi && dimensions.columns>=80 && dimensions.rows>=24 && !source.value->body.rows.empty()){
        std::vector<bool> marked(source.value->body.rows.size(),false);
        std::size_t cursor{},top{};std::string feedback;
        EditorPopupFrame popup(p,editor,to,from);
        for(;;){
            dimensions=p.dimensions();
            if(!dimensions.ansi || dimensions.columns<80 || dimensions.rows<24)break;
            const auto rows=std::clamp(dimensions.rows,24U,200U);
            const auto height=std::min<unsigned>(8,rows/3);
            const auto start=rows-height-1;
            if(cursor<top)top=cursor;else if(cursor>=top+height)top=cursor-height+1;
            editor.resize(dimensions.ansi,dimensions.columns,dimensions.rows);
            std::vector<std::string> lines;
            lines.push_back("\x1b[0;37;40m\x1b[2KQuote source: "+std::to_string(cursor+1)+"/"+
                std::to_string(marked.size())+"  "+(marked[cursor]?"Marked":"Unmarked"));
            for(std::size_t row=0;row<height;++row){
                const auto index=top+row;
                std::string line=index==cursor?"\x1b[0;30;47m":index<marked.size() && marked[index]?"\x1b[1;37;40m":"\x1b[0;37;40m";
                line+="\x1b[2K";
                if(index<marked.size())line+=source.value->body.rows[index];
                lines.push_back(std::move(line));
            }
            const std::string_view footer=feedback.empty()?
                std::string_view{"Space marks; A All; N ranges; ESC cancels; Enter returns"}:std::string_view{feedback};
            lines.push_back("\x1b[0;37;40m\x1b[2K"+std::string(footer));
            if(!popup.draw(dimensions,start,std::move(lines),footer.size()+1))return false;
            const auto byte=p.byte();std::optional<MessageEditorInput> input;
            if(byte.outcome==TerminalOutcome::timed_out)input=keys.idle();else if(byte.byte)input=keys.feed(*byte.byte);else return false;
            if(!input)continue;
            if(input->command==MessageEditorCommand::actions || input->command==MessageEditorCommand::help){return p.write("\x1b[0m\r\n");}
            if(!input->event)continue;
            const auto event=*input->event;
            switch(event.key){
            case MessageEditorKey::up:if(cursor)--cursor;break;
            case MessageEditorKey::down:cursor=std::min(cursor+1,marked.size()-1);break;
            case MessageEditorKey::home:cursor=0;break;
            case MessageEditorKey::end:cursor=marked.size()-1;break;
            case MessageEditorKey::page_up:cursor=cursor>height?cursor-height:0;break;
            case MessageEditorKey::page_down:cursor=std::min(cursor+height,marked.size()-1);break;
            case MessageEditorKey::text:
                if(event.byte==' ')marked[cursor]=!marked[cursor];
                else if(event.byte=='A' || event.byte=='a')std::fill(marked.begin(),marked.end(),true);
                else if(event.byte=='N' || event.byte=='n'){
                    popup.clear();
                    if(!p.write("\x1b[0m\r\n"))return false;
                    const auto chosen=p.terminal.read_visible({"message.quote.selection","All or line numbers/ranges (Escape cancels): ",4096,EmptyInputPolicy::use_default,{}});
                    if(chosen.outcome==TerminalOutcome::cancelled || chosen.outcome==TerminalOutcome::empty)continue;
                    if(!completed(chosen.outcome))return false;
                    const auto selected=message_quote_selection(chosen.value,marked.size());
                    if(!selected){if(!say(p,"Invalid selection; source and draft unchanged."))return false;continue;}
                    std::fill(marked.begin(),marked.end(),false);for(const auto n:*selected)marked[n]=true;
                }break;
            case MessageEditorKey::enter:{
                if(p.accepted_key){p.accepted_key(event.byte=='\r');keys.reset();}
                std::vector<std::size_t> selected;for(std::size_t n=0;n<marked.size();++n)if(marked[n])selected.push_back(n);
                if(selected.empty())selected.push_back(cursor);
                if(insert(selected)==MessageValidation::valid)return p.write("\x1b[0m\r\n");
                feedback="Quote refused; draft unchanged. Reduce selection or Escape.";break;
            }
            default:break;
            }
        }
    }
    // Numbered plain view is emitted in bounded chunks, never a body-sized
    // presentation plus potentially unbounded numeric-prefix overhead.
    for(std::size_t first=0;first<source.value->body.rows.size();first+=50){
        TerminalDisplayProgram program;
        for(std::size_t i=first;i<std::min(first+50,source.value->body.rows.size());++i){
            const auto row=std::to_string(i+1)+". "+source.value->body.rows[i];
            program.operations.push_back({TerminalDisplayOperationKind::cp437_text,row});program.operations.push_back({TerminalDisplayOperationKind::paging_newline,{}});program.output_byte_count+=row.size()+2;
        }
        const auto shown=p.terminal.present_display({"message.quote.source",&program,{true,std::uint16_t{24}}});
        if(shown==TerminalOutcome::cancelled)return true;
        if(shown!=TerminalOutcome::completed)return false;
    }
    for(;;){
        const auto chosen=p.terminal.read_visible({"message.quote.selection","All or line numbers/ranges (Escape cancels): ",4096,EmptyInputPolicy::use_default,{}});
        if(chosen.outcome==TerminalOutcome::cancelled || chosen.outcome==TerminalOutcome::empty)return true;
        if(!completed(chosen.outcome))return false;
        const auto selected=message_quote_selection(chosen.value,source.value->body.rows.size());
        if(!selected){if(!say(p,"Invalid quote selection; no text inserted."))return false;continue;}
        const auto result=insert(*selected);
        if(result==MessageValidation::valid)return true;
        if(!say(p,"Quote exceeds the message limit; select fewer lines or cancel. No text inserted."))return false;
    }
}
} // namespace
MessageComposeOutcome compose_local_message(const MessageComposePorts& p,const MessageTarget& target,
    std::string to,std::string from,std::optional<MessageId> parent,std::string subject){
    if(!p.actor || !p.operation || !p.dimensions || !p.byte || !p.write)return MessageComposeOutcome::failed;
    auto actor=p.actor();if(!actor)return MessageComposeOutcome::failed;
    auto context=p.storage.begin_message(*actor,target,parent);
    if(!context || !context.value){
        std::string_view notice=target.remote?"FTN destination is unavailable. Check the configured network, source address and route; no mail was saved.":"Message entry is unavailable. Check access or the System Sysop assignment.";
        if (context.status.operation == kEchoRealNameRequired) {
            notice = p.real_name_editable
                ? "Real names are required for this echo. From Main Menu, choose Your Info / Settings > Edit Caller Profile > Real Name."
                : "Real names are required for this echo. Ask the Sysop to help set your Real Name.";
        }
        const bool shown=p.unavailable_notice?p.unavailable_notice(notice):say(p,std::string(notice));
        return shown?MessageComposeOutcome::cancelled:MessageComposeOutcome::failed;
    }
    MessageEditorState editor(context.value->maximum_lines);editor.draft().set_subject(subject);
    struct InputModeRestore {
        const MessageComposePorts& ports;bool enabled{};
        ~InputModeRestore(){if(enabled)ports.write("\x1b[?67h");}
    } input_mode{p};
    auto current_target=target;
    if(target.remote&&!say(p,"Network: "+message_network_name(p.storage,*actor,target.remote->network)+"\nFrom: "+from+" ("+target.remote->source_aka+")\nTo: "+target.remote->name+" ("+target.remote->address+")"))return MessageComposeOutcome::failed;
    bool refresh_context=false;
    if(!subject_prompt(p,editor))return MessageComposeOutcome::failed;
    MessageEditorKeyDecoder decoder;
    std::optional<CommitMessageRequest> uncertain;
    std::optional<std::size_t> live_command_row;
    std::optional<MessageLineCommand> pending_command;
    MessageEditorScreen rendered;
    bool line_banner=false;
    const auto reset_terminal=[&]{return p.dimensions().ansi?p.write("\x1b[?7h\x1b[0m\r\n"):true;};
    const auto fresh_context=[&]()->std::optional<ComposeContext>{
        const auto current=p.actor();if(!current)return {};
        const auto refreshed=p.storage.begin_message(*current,current_target,parent);
        if(!refreshed || !refreshed.value)return {};
        return refreshed.value;
    };
    for(;;){
        const auto size=p.dimensions();editor.resize(size.ansi,size.columns,size.rows);
        if(editor.full_screen())input_mode.enabled=true;
        else if(input_mode.enabled){if(!p.write("\x1b[?67h"))return MessageComposeOutcome::failed;input_mode.enabled=false;}
        MessageLineCommand command;
        if(uncertain){
            const auto retry=p.terminal.read_key({"message.save.reconcile","Pending save outcome: [R] retries reconciliation (no new post): ",{'R'},'R'});
            if(!completed(retry.outcome) && retry.outcome!=TerminalOutcome::cancelled)return MessageComposeOutcome::failed;
            command.command=MessageEditorCommand::save;
        }else if(pending_command){command=*pending_command;pending_command.reset();}
        else if(editor.full_screen()){
            const auto frame=editor.update_frame(to,from,rendered);
            if(!frame.empty()&&!p.write(frame))return MessageComposeOutcome::failed;
            auto byte=p.byte();std::optional<MessageEditorInput> input;
            if(byte.outcome==TerminalOutcome::timed_out)input=decoder.idle();
            else if(byte.byte)input=decoder.feed(*byte.byte);
            else return MessageComposeOutcome::failed;
            if(!input)continue;
            command.command=input->command;
            if(input->event){
                const auto event=*input->event;const auto row=editor.cursor_row();
                if(event.key==MessageEditorKey::text && !live_command_row &&
                    (row>=editor.draft().body().rows.size() || editor.draft().body().rows[row].empty()))live_command_row=row;
                if(event.key==MessageEditorKey::enter && live_command_row==row && row<editor.draft().body().rows.size()){
                    command=message_line_command(editor.draft().body().rows[row]);
                    if(command.command!=MessageEditorCommand::none){editor.draft().delete_line(row+1);editor.resize(size.ansi,size.columns,size.rows);live_command_row.reset();}
                    if(command.command!=MessageEditorCommand::none && p.accepted_key){p.accepted_key(event.byte=='\r');decoder.reset();}
                }
                if(command.command==MessageEditorCommand::none){
                    const auto result=editor.apply(event);
                    if(result!=MessageValidation::valid && (!reset_terminal() || !say(p,"Edit exceeds a limit or contains unsupported controls; the draft was preserved.")))return MessageComposeOutcome::failed;
                }
                if(event.key!=MessageEditorKey::text && event.key!=MessageEditorKey::backspace)live_command_row.reset();
                if(command.command==MessageEditorCommand::none)continue;
            }
            if(!reset_terminal())return MessageComposeOutcome::failed;
        }else{
            rendered.clear();
            if(!line_banner){
                if(!say(p,"["+std::to_string(editor.draft().maximum_lines())+" Line Maximum] - Enter your message now") ||
                   !say(p,"/S Save, /A Abort, /? Help; empty Enter adds a blank line."))return MessageComposeOutcome::failed;
                line_banner=true;
            }
            const auto line=p.terminal.read_visible({"message.body.line",std::to_string(editor.cursor_row()+1)+": ",4096,EmptyInputPolicy::use_default,{}});
            if(line.outcome==TerminalOutcome::cancelled)command.command=MessageEditorCommand::help;
            else if(!completed(line.outcome))return MessageComposeOutcome::failed;
            else command=message_line_command(line.value);
            if(command.command==MessageEditorCommand::none){
                const auto result=editor.insert_data(line.value+"\n");
                if(result!=MessageValidation::valid && !say(p,"Message line/byte limit reached; text was not inserted."))return MessageComposeOutcome::failed;
                continue;
            }
        }
        rendered.clear();
#include "message_upload_compose.inc"
        if(command.command==MessageEditorCommand::save){
            if(uncertain){
                const auto known=p.storage.message_commit_outcome(*uncertain);
                if(!known || !known.value){if(!say(p,"Save outcome remains uncertain. Retry reconciliation; do not send a new operation."))return MessageComposeOutcome::failed;continue;}
                if(known.value->committed){say(p,"Message saved.");return MessageComposeOutcome::saved;}
                const bool was_upload=uncertain->original_source.has_value();
                uncertain.reset();refresh_context=true;
                // A rejected upload does not turn the retained old draft into
                // a new post. Re-enter ordinary editing after reconciliation.
                if(was_upload)continue;
            }
            if(refresh_context){
                actor=p.actor();if(!actor)return MessageComposeOutcome::failed;
                if(current_target.sysop_assignment_revision && p.current_sysop){
                    const auto replacement=p.current_sysop();
                    if(!replacement){if(!say(p,"System Sysop is unavailable; this draft is retained."))return MessageComposeOutcome::failed;continue;}
                    if(replacement->first.recipient!=current_target.recipient || replacement->first.sysop_assignment_revision!=current_target.sysop_assignment_revision){
                        if(!say(p,"System Sysop changed. New recipient: "+replacement->second))return MessageComposeOutcome::failed;
                        const auto answer=p.terminal.read_key({"message.sysop.refresh","Use this recipient for the retained draft? [y/N]",{'Y','N'},'N'});
                        if(answer.outcome==TerminalOutcome::cancelled || answer.key=='N')continue;
                        if(!answer.has_key())return MessageComposeOutcome::failed;
                        current_target=replacement->first;to=replacement->second;
                    }
                }
                const auto refreshed=p.storage.begin_message(*actor,current_target,parent);
                if(!refreshed || !refreshed.value){if(!say(p,"Current access or recipient is unavailable. Draft retained; no message saved."))return MessageComposeOutcome::failed;continue;}
                if(editor.draft().change_limit(refreshed.value->maximum_lines)!=MessageValidation::valid){
                    if(!say(p,"Current allowance is "+std::to_string(refreshed.value->maximum_lines)+" lines. Reduce this retained draft before saving."))return MessageComposeOutcome::failed;
                    continue;
                }
                if(current_target.recipient){const auto found=p.storage.resolve_message_recipient(*actor,std::to_string(*current_target.recipient));if(!found || !found.value || found.value->entries.size()!=1){if(!say(p,"Recipient unavailable; draft retained."))return MessageComposeOutcome::failed;continue;}to=found.value->entries.front().name;}
                context=refreshed;refresh_context=false;
            }
            if(editor.draft().save_validation()!=MessageValidation::valid || validate_message_subject(editor.draft().subject(),true)!=MessageValidation::valid){if(!say(p,"A nonblank message body within the message limits is required. The subject is optional."))return MessageComposeOutcome::failed;continue;}
            if(current_target.remote){
                if(p.terminal.clear_screen()!=TerminalOutcome::completed||!say(p,"Network: "+message_network_name(p.storage,*actor,current_target.remote->network)+"\nFrom: "+from+" ("+current_target.remote->source_aka+")\nTo: "+current_target.remote->name+" ("+current_target.remote->address+")"))return MessageComposeOutcome::failed;
            }
            if(!say(p,"To: "+to+"\nSubject: "+editor.draft().subject()))return MessageComposeOutcome::failed;
            const auto answer=editor.full_screen()&&!current_target.remote?TerminalKeyResult{TerminalOutcome::completed,
                lightbar(p,editor,to,from,decoder,"To: "+to+"\nSubject: "+editor.draft().subject(),{{'Y',"Yes - Save"},{'N',"No - Edit"}},'Y','N')}:
                p.terminal.read_key({"message.save.confirm","Save this message? [Y/n]",{'Y','N'},'Y'});
            if(answer.outcome==TerminalOutcome::cancelled || answer.key=='N')continue;
            if(!answer.has_key())return MessageComposeOutcome::failed;
            actor=p.actor();if(!actor)return MessageComposeOutcome::failed;
            CommitMessageRequest request{*actor,p.operation(),*context.value,editor.draft().subject(),editor.draft().body().canonical_lf};
            const auto saved=p.storage.commit_message(request);
            if(saved && saved.value){say(p,current_target.kind==MessageKind::private_ftn?"NetMail saved and queued. Link delivery is pending.":"Message saved.");return MessageComposeOutcome::saved;}
            const auto known=p.storage.message_commit_outcome(request);
            if(known && known.value && known.value->committed){say(p,current_target.kind==MessageKind::private_ftn?"NetMail saved and queued. Link delivery is pending.":"Message saved.");return MessageComposeOutcome::saved;}
            if(!known || !known.value)uncertain=request;
            else refresh_context=true;
            if(!say(p,uncertain?"Save outcome is uncertain; retain this draft and reconcile before any retry.":"Message was not saved. The connected draft is retained; access or limits may have changed."))return MessageComposeOutcome::failed;
        }else if(command.command==MessageEditorCommand::abort){
            if(uncertain){if(!say(p,"First reconcile the pending save with /S; discard cannot claim an uncertain commit was undone."))return MessageComposeOutcome::failed;continue;}
            const auto answer=editor.full_screen()?TerminalKeyResult{TerminalOutcome::completed,
                lightbar(p,editor,to,from,decoder,"Discard this message?",{{'Y',"Yes - Discard"},{'N',"No - Edit"}},'N','N')}:
                p.terminal.read_key({"message.abort.confirm","Discard this message? [y/N]",{'Y','N'},'N'});
            if(answer.key=='Y'){reset_terminal();return MessageComposeOutcome::cancelled;}
            if(!answer.has_key() && answer.outcome!=TerminalOutcome::cancelled)return MessageComposeOutcome::failed;
        }else if(command.command==MessageEditorCommand::subject){if(!subject_prompt(p,editor))return MessageComposeOutcome::failed;}
        else if(command.command==MessageEditorCommand::quote){if(parent){if(!quote_prompt(p,editor,*parent,to,from,decoder))return MessageComposeOutcome::failed;}else if(!say(p,"Quote is available only while replying."))return MessageComposeOutcome::failed;live_command_row.reset();}
        else if(command.command==MessageEditorCommand::help || command.command==MessageEditorCommand::actions){
            if(editor.full_screen() && command.command==MessageEditorCommand::actions){
                std::vector<EditorChoice> choices{{'S',"Save"},{'A',"Abort"},{'E',"Edit"},{'T',"Subject"}};
                if(parent)choices.push_back({'Q',"Quote"});
                choices.push_back({'H',"Help"});
                const auto selected=lightbar(p,editor,to,from,decoder,std::string("Editor actions - ")+(editor.inserting()?"Insert":"Overwrite"),choices,'E','E');
                if(!selected)return MessageComposeOutcome::failed;
                if(*selected!='H'){
                    if(*selected!='E')pending_command=MessageLineCommand{*selected=='S'?MessageEditorCommand::save:*selected=='A'?MessageEditorCommand::abort:*selected=='Q'?MessageEditorCommand::quote:MessageEditorCommand::subject,{}};
                    continue;
                }
            }
            const auto choice=help_prompt(p,decoder);
            if(!choice)return MessageComposeOutcome::failed;
            // Same confirmation/quote path on the next iteration; no automatic
            // save or discard and no fake insertion of a command into body text.
            if(*choice!='E')pending_command=MessageLineCommand{
                *choice=='S'?MessageEditorCommand::save:*choice=='A'?MessageEditorCommand::abort:
                *choice=='Q'?MessageEditorCommand::quote:MessageEditorCommand::subject,{}};
        }else if(command.command==MessageEditorCommand::list){
            for(std::size_t i=0;i<editor.draft().body().rows.size();++i)if(!say(p,std::to_string(i+1)+". "+editor.draft().body().rows[i]))return MessageComposeOutcome::failed;
        }else if(command.command==MessageEditorCommand::edit_line || command.command==MessageEditorCommand::delete_line || command.command==MessageEditorCommand::insert_line){
            auto line=command.number;if(!line){const auto input=p.terminal.read_visible({"message.line.number","Line number (Escape cancels): ",6,EmptyInputPolicy::use_default,{}});if(input.outcome==TerminalOutcome::cancelled || input.outcome==TerminalOutcome::empty)continue;if(!completed(input.outcome))return MessageComposeOutcome::failed;line=number(input.value);}
            if(!line || *line>editor.draft().body().rows.size()+(command.command==MessageEditorCommand::insert_line?1:0)){if(!say(p,"Invalid line number; draft unchanged."))return MessageComposeOutcome::failed;continue;}
            MessageValidation result;
            if(command.command==MessageEditorCommand::delete_line)result=editor.draft().delete_line(*line);
            else{const auto input=p.terminal.read_visible({"message.line.replacement","Replacement text (Escape cancels): ",4096,EmptyInputPolicy::use_default,{}});if(input.outcome==TerminalOutcome::cancelled)continue;if(!completed(input.outcome))return MessageComposeOutcome::failed;result=command.command==MessageEditorCommand::edit_line?editor.draft().replace_line(*line,input.value):editor.draft().insert_line(*line,input.value);}
            if(result!=MessageValidation::valid && !say(p,"Edit exceeds a limit; draft unchanged."))return MessageComposeOutcome::failed;
        }else if(command.command==MessageEditorCommand::invalid){if(!say(p,"Invalid editor command; draft unchanged."))return MessageComposeOutcome::failed;}
    }
}
} // namespace sfx::application
