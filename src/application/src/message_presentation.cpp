#include "sfx/application/message_presentation.hpp"
#include "sfx/application/message_storage.hpp"
#include "sfx/application/menu.hpp"
#include <algorithm>
namespace sfx::application {
std::string wrap_message_prompt(std::string_view text,unsigned columns,unsigned input_cells){
    const auto width=static_cast<std::size_t>(std::clamp(columns,2U,4096U)-1);
    const auto edit_width=width-std::min<std::size_t>(input_cells,width-1);
    std::string out;
    for(;;){
        const auto newline=text.find('\n');
        auto row=text.substr(0,newline);
        if(!row.empty() && row.back()=='\r')row.remove_suffix(1);
        const auto limit=newline==text.npos?edit_width:width;
        while(row.size()>limit){
            const auto space=row.rfind(' ',limit);
            const auto count=space==row.npos || space==0?limit:space;
            out.append(row.substr(0,count));out+='\n';
            row.remove_prefix(count);
            while(!row.empty() && row.front()==' ')row.remove_prefix(1);
        }
        out.append(row);
        if(newline==text.npos)break;
        out+='\n';
        text.remove_prefix(newline+1);
    }
    // The stream terminal requires a visible prompt even when the margin
    // pushed the complete label onto the preceding row.
    if(!out.empty() && out.back()=='\n')out+=' ';
    return out;
}
std::string message_network_name(IMessageStorage& storage,const MessageActor& actor,std::string_view code){
    const auto networks=storage.ftn_message_networks(actor);
    if(networks&&networks.value)for(const auto& n:*networks.value){
        if(n.code.size()==code.size()&&std::equal(n.code.begin(),n.code.end(),code.begin(),[](char a,char b){return (a>='A'&&a<='Z'?a+32:a)==(b>='A'&&b<='Z'?b+32:b);}))return n.name;
    }
    return "Unavailable network";
}
bool MessageQuestionTerminal::before(){return write_(line_start_?"\r\n":"\r\n\r\n");}
TerminalOutcome MessageQuestionTerminal::clear_screen(){
    line_start_=true;
    const bool ansi=ansi_ && ansi_();
    const auto style=terminal_.set_text_style(ansi?TerminalTextStyle::ogsf_ansi:TerminalTextStyle::plain);
    if(style!=TerminalOutcome::completed)return style;
    const auto cleared=terminal_.clear_screen();
    if(cleared!=TerminalOutcome::completed || ansi || !rows_)return cleared;
    const auto rows=std::min(rows_(),200U);
    if(rows<=25)return cleared;
    std::string padding;
    for(unsigned i=25;i<rows;++i)padding+="\r\n";
    return write_(padding)?cleared:TerminalOutcome::output_failure;
}
bool MessageQuestionTerminal::after(TerminalOutcome outcome){
    if(outcome!=TerminalOutcome::completed && outcome!=TerminalOutcome::empty && outcome!=TerminalOutcome::cancelled)return true;
    line_start_=true;
    return outcome!=TerminalOutcome::cancelled || write_("\r\n");
}
TerminalOutcome MessageQuestionTerminal::present_display(const TerminalDisplayPresentationRequest& r){
    // Built-in QWK text follows the normal gray menu presentation instead of
    // inheriting the bright input-echo color from the preceding command.
    if(r.identity=="qwk.notice" && ansi_() && !write_("\x1b[0;37;40m"))
        return TerminalOutcome::output_failure;
    // Only the built-in QWK option lists use this presentation identity. Keep
    // only the hotkey character bright white; both brackets and the complete
    // description/value stay gray. Never interpret caller text or add escapes
    // to the plain fallback.
    auto request=r;TerminalDisplayProgram options;
    if(r.identity=="qwk.options" && ansi_()){
        if(!r.program || !valid_terminal_display_program(*r.program))return TerminalOutcome::output_failure;
        const auto add=[&](TerminalDisplayOperationKind kind,std::string bytes){
            options.output_byte_count+=(kind==TerminalDisplayOperationKind::logical_newline || kind==TerminalDisplayOperationKind::paging_newline)?2:bytes.size();
            options.operations.push_back({kind,std::move(bytes)});
        };
        add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m");
        for(const auto& op:r.program->operations){
            if(op.kind==TerminalDisplayOperationKind::cp437_text && op.bytes.size()>=4 &&
                op.bytes[0]=='[' && op.bytes[1]>='A' && op.bytes[1]<='Z' && op.bytes[2]==']' && op.bytes[3]==' '){
                add(TerminalDisplayOperationKind::cp437_text,"[");
                add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[1;37;40m");
                add(TerminalDisplayOperationKind::cp437_text,op.bytes.substr(1,1));
                add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m");
                add(TerminalDisplayOperationKind::cp437_text,op.bytes.substr(2));
            }else add(op.kind,op.bytes);
        }
        request.program=&options;
        request.system_ui=false; // This built-in option program owns its styles.
    }
    const auto result=terminal_.present_display(request);
    if(result==TerminalOutcome::completed && r.program && !r.program->operations.empty()){
        const auto kind=r.program->operations.back().kind;
        line_start_=kind==TerminalDisplayOperationKind::logical_newline || kind==TerminalDisplayOperationKind::paging_newline;
    }
    return result;
}
TerminalInputResult MessageQuestionTerminal::read_visible(const TerminalInputRequest& r){
    // Numbered body entry is a continuous line-by-line transcript. Each
    // accepted input already emits its own CRLF; do not add blank rows here.
    if(r.identity!="message.body.line" && !before())return {TerminalOutcome::output_failure,{}};
    auto request=r;request.escape_cancels=true;
    if(columns_ && r.identity!="message.body.line")request.prompt=wrap_message_prompt(r.prompt,columns_(),1);
    auto result=terminal_.read_visible(request);if(!after(result.outcome))return {TerminalOutcome::output_failure,{}};return result;
}
TerminalInputResult MessageQuestionTerminal::read_secret(const TerminalInputRequest& r){
    if(!before())return {TerminalOutcome::output_failure,{}};
    auto request=r;request.escape_cancels=true;
    auto result=terminal_.read_secret(request);if(!after(result.outcome))return {TerminalOutcome::output_failure,{}};return result;
}
TerminalKeyResult MessageQuestionTerminal::read_key(const TerminalKeyRequest& r){
    const bool specific_footer=r.identity=="message.specific.scope" || r.identity=="message.specific.direction" || r.identity=="message.read.mode";
    // These two reviewed submenus place their command question immediately
    // below the Current Conference status. Other message questions retain the
    // normal separating blank row.
    // The reader has already emitted its styled, width-aware footer. Its inert
    // one-space input request must remain on that footer's final row.
    if(r.identity!="message.reader.command" && r.identity!="message.reader.more" && r.identity!="message.read.preview.footer" &&
        (!specific_footer || !line_start_) && !before())return {TerminalOutcome::output_failure,{}};
    auto request=r;
    request.escape_cancels=true;
    if(columns_ && r.identity!="message.reader.command" && r.identity!="message.reader.more" &&
        r.identity!="message.read.preview.footer")request.prompt=wrap_message_prompt(r.prompt,columns_());
    if(r.identity=="message.login.read_new" && ansi_()){
        if(!write_("\x1b[0;37;40m"+r.prompt+"\x1b[0m"))return {TerminalOutcome::output_failure,{}};
        request.prompt=" ";
    }
    if(r.identity=="message.login.scan"){
        const bool ansi=ansi_();
        const std::string left=ansi?"\x1b[1;30;40m\xfe\x1b[0;37;40m\xfe\x1b[1;37;40m\xfe\x1b[0;37;40m":"";
        const std::string right=ansi?"\x1b[1;37;40m\xfe\x1b[0;37;40m\xfe\x1b[1;30;40m\xfe\x1b[0m":"";
        if(!write_(left+"  Your Message Scan [Y/n]  "+right))return {TerminalOutcome::output_failure,{}};
        request.prompt=" "; // Keep the terminal's nonempty inert-prompt contract.
    }
    if(r.identity=="message.reader.more" && ansi_() && !r.prompt_visible){
        std::string styled="\x1b[0;37;40m";
        for(std::size_t at=0;at<r.prompt.size();){
            const char first=r.prompt[at];const char close=first=='<'?'>':first=='['?']':0;
            const auto end=close?r.prompt.find(close,at+1):std::string::npos;
            if(end!=std::string::npos){styled+=first;styled+="\x1b[1;37;40m";styled+=r.prompt.substr(at+1,end-at-1);styled+="\x1b[0;37;40m";styled+=close;at=end+1;}
            else styled+=r.prompt[at++];
        }
        if(!write_(styled))return {TerminalOutcome::output_failure,{}};
        request.prompt=" ";
    }
    auto result=terminal_.read_key(request);
    if(!request.advance_line)line_start_=false;
    else if(!after(result.outcome))return {TerminalOutcome::output_failure,{}};
    if(r.identity=="message.login.scan" && (result.has_key() || result.outcome==TerminalOutcome::cancelled) && !write_("\r\n"))return {TerminalOutcome::output_failure,{}};
    return result;
}
bool present_message_text(ICallerTerminal& terminal,std::string identity,std::string_view text){
    if(text.size()>4096)return false;
    TerminalDisplayProgram program;std::size_t start{};
    for(std::size_t i=0;i<=text.size();++i){
        if(i!=text.size() && text[i]!='\n'){
            const auto c=static_cast<unsigned char>(text[i]);if(c<32 || c==127)return false;continue;
        }
        if(i>start){const auto row=std::string(text.substr(start,i-start));program.operations.push_back({TerminalDisplayOperationKind::cp437_text,row});program.output_byte_count+=row.size();}
        program.operations.push_back({TerminalDisplayOperationKind::logical_newline,{}});program.output_byte_count+=2;start=i+1;
    }
    TerminalDisplayPresentationRequest request{std::move(identity),&program,{}};request.system_ui=true;
    return terminal.present_display(request)==TerminalOutcome::completed;
}
namespace {
struct MessageNativeDisplay {
    TerminalDisplayProgram program;
    void add(TerminalDisplayOperationKind kind,std::string text={}){
        program.output_byte_count+=kind==TerminalDisplayOperationKind::logical_newline?2:text.size();
        program.operations.push_back({kind,std::move(text)});
    }
    void text(std::string value){add(TerminalDisplayOperationKind::cp437_text,std::move(value));}
    void style(bool ansi,const char* value){if(ansi)add(TerminalDisplayOperationKind::ansi_sequence,value);}
    void newline(){add(TerminalDisplayOperationKind::logical_newline);}
    bool show(ICallerTerminal& terminal,std::string identity){return terminal.present_display({std::move(identity),&program,{}})==TerminalOutcome::completed;}
};
}
bool present_message_reader_field(ICallerTerminal& terminal,bool ansi,std::uint8_t color,std::string_view label,std::string_view value,unsigned columns){
    if(color>15 || label.size()+value.size()>4096)return false;
    for(const auto span:{label,value})for(const auto ch:span)if(static_cast<unsigned char>(ch)<32 || ch==127)return false;
    MessageNativeDisplay display;display.style(ansi,"\x1b[0;37;40m");if(!label.empty())display.text(std::string(label));
    if(!value.empty()){
        const auto sgr=menu_text_sgr(color);
        display.style(ansi,sgr.c_str());
        if(label.size()+value.size()>=columns)display.newline();
        const auto lines=wrap_message_prompt(value,columns,0);
        for(std::size_t first=0;;){
            const auto next=lines.find('\n',first);
            display.text(lines.substr(first,next==lines.npos?next:next-first));
            if(next==lines.npos)break;
            display.newline();first=next+1;
        }
    }
    display.newline();return display.show(terminal,"message.reader.field");
}
bool present_message_login_summary(ICallerTerminal& terminal,const MessageLoginScanSummary& summary,bool ansi,unsigned columns){
    if(summary.scanned<0 || summary.unread_to_you<0 || summary.read_to_you<0 || summary.from_you<0)return false;
    MessageNativeDisplay display;display.newline();display.newline();
    display.style(ansi,"\x1b[0;37;40m");
    if(ansi){display.style(true,"\x1b[1;30;40m");display.text("\xfe");display.style(true,"\x1b[0;37;40m");display.text("\xfe");display.style(true,"\x1b[1;37;40m");display.text("\xfe");display.style(true,"\x1b[0;37;40m");display.text("  ");}
    display.text("Your Message Scan");
    if(ansi){display.text("  ");display.style(true,"\x1b[1;37;40m");display.text("\xfe");display.style(true,"\x1b[0;37;40m");display.text("\xfe");display.style(true,"\x1b[1;30;40m");display.text("\xfe");}
    display.newline();display.newline();
    const auto count=[&](std::string label,std::int64_t value){
        const auto number=std::to_string(value);
        const std::size_t label_width=columns>=40?31:17;
        const auto number_width=std::max<std::size_t>(6,number.size());
        display.style(ansi,"\x1b[0;37;40m");display.text(label+std::string(label_width-label.size(),' ')+": ");
        if(label_width+2+number_width>=columns)display.newline();
        display.style(ansi,"\x1b[1;37;40m");display.text(std::string(number_width-number.size(),' ')+number);display.style(ansi,"\x1b[0m");display.newline();
    };
    count("Messages scanned",summary.scanned);
    count(columns>=40?"New messages to you":"New to you",summary.unread_to_you);
    count(columns>=40?"Previously read messages to you":"Read to you",summary.read_to_you);
    count(columns>=40?"Messages from you":"From you",summary.from_you);
    return display.show(terminal,"message.login.summary");
}
bool present_message_read_options(ICallerTerminal& terminal,bool ansi,unsigned columns){
    return present_message_options(terminal,ansi,"message.read.menu",{{'T',"This Message Conference"},{'A',"All Message Conferences"},{'O',"Only Queued Conferences"},{'N',"New Message Scan"},{'Q',"Quit"}},columns);
}
TerminalDisplayProgram message_reader_command_program(bool ansi,unsigned columns,bool can_delete,bool can_mark,bool can_reply,bool list_return,bool allow_skip,bool permanent){
    struct Option {std::string key,label;bool enter{},comma{true};};
    std::vector<Option> options{{"ENT",list_return?"List":"Next",true},{"-","Last"}};
    if(list_return)options.push_back({"N","Next"});
    if(can_reply)options.push_back({"R","Reply"});
    options.push_back({"L","List"});
    if(can_delete)options.push_back({"D","Delete"});
    if(can_mark)options.push_back({"P",permanent?"Unprotect":"Protect",false,false});
    if(allow_skip)options.push_back({"K","Skip Conference"});
    options.push_back({"Q","Quit"});
    const auto width=static_cast<std::size_t>(std::clamp(columns,2U,4096U)-1);
    const auto suffix=columns<4?std::size_t{1}:std::size_t{3}; // colon, prompt space, echoed key
    MessageNativeDisplay display;display.style(ansi,"\x1b[0;37;40m");display.newline();std::size_t used{};
    const auto newline=[&]{display.style(ansi,"\x1b[0;37;40m");display.newline();used=0;};
    for(std::size_t i=0;i<options.size();++i){const auto& option=options[i];const bool last=i+1==options.size();
        const auto text=(option.enter?"<":"[")+option.key+(option.enter?"> ":"] ")+option.label+(last||!option.comma?"":",");
        if(used){if(used+1+text.size()+(last?suffix:0)>width)newline();else{display.style(ansi,"\x1b[0;37;40m");display.text(" ");++used;}}
        const auto emit=[&](std::size_t begin,std::size_t end){while(begin<end){const bool hot=begin>=1&&begin<1+option.key.size();const auto stop=std::min(end,begin<1?std::size_t{1}:hot?1+option.key.size():text.size());display.style(ansi,hot?"\x1b[1;37;40m":"\x1b[0;37;40m");display.text(text.substr(begin,stop-begin));used+=stop-begin;begin=stop;}};
        std::size_t begin{};while(text.size()-begin>width-used){const auto available=width-used;const auto space=text.rfind(' ',begin+available-1);const auto count=space!=text.npos&&space>=begin?space-begin+1:available;emit(begin,begin+count);begin+=count;newline();}emit(begin,text.size());
    }
    if(used+suffix>width)newline();
    display.style(ansi,"\x1b[0;37;40m");display.text(":");
    return std::move(display.program);
}
bool present_message_reader_commands(ICallerTerminal& terminal,bool ansi,unsigned columns,bool can_delete,bool can_mark,bool permanent,bool can_reply,bool list_return,bool allow_skip){
    const auto program=message_reader_command_program(ansi,columns,can_delete,can_mark,can_reply,list_return,allow_skip,permanent);
    return terminal.present_display({"message.reader.command.options",&program,{}})==TerminalOutcome::completed;
}
bool present_message_options(ICallerTerminal& terminal,bool ansi,std::string identity,
    const std::vector<std::pair<char,std::string>>& options,unsigned columns){
    MessageNativeDisplay display;
    const auto width=std::clamp(columns,6U,46U)-1;
    for(const auto& option:options){
        display.style(ansi,"\x1b[0;37;40m");display.text("[");display.style(ansi,"\x1b[1;37;40m");display.text(std::string(1,option.first));
        display.style(ansi,"\x1b[0;37;40m");
        const bool dotted=identity=="message.read.menu" || identity=="message.specific.scope.options" || identity=="message.specific.direction.options" || identity=="message.entry.kind.options";
        const auto label_width=width-5;
        display.text(":>"+std::string(dotted && option.second.size()<label_width?label_width-option.second.size():0,'.')+" ");
        const auto lines=wrap_message_prompt(option.second,label_width+1,0);
        for(std::size_t first=0;;){
            const auto next=lines.find('\n',first);
            display.text(lines.substr(first,next==lines.npos?next:next-first));
            if(next==lines.npos)break;
            display.newline();display.text("     ");first=next+1;
        }
        display.style(ansi,"\x1b[0m");display.newline();
    }
    return display.show(terminal,std::move(identity));
}
}
