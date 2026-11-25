#include "sfx/application/file_presentation.hpp"
#include "sfx/application/file.hpp"
#include "sfx/application/menu.hpp"
#include "sfx/application/qwk.hpp"
#include "sfx/application/message_source.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
std::optional<bool> file_transfer_logoff_countdown(const FileListPorts& p,
    const std::function<std::chrono::steady_clock::time_point()>& now){
    if(!p.byte||!p.write||!p.live||!now||!p.live())return {};
    const bool ansi=p.dimensions&&p.dimensions().ansi;
    const std::string style=ansi?"\x1b[0;37;40m":"";
    // Fence buffered transfer/Enter bytes before advertising any hotkey.
    // A sustained noisy peer cannot hold this phase or the countdown forever.
    const auto drain_deadline=now()+std::chrono::seconds(1);
    bool armed{};
    while(p.live()&&now()<drain_deadline){
        const auto input=p.byte();
        if(input.outcome==TerminalOutcome::timed_out){armed=true;break;}
        if(input.outcome!=TerminalOutcome::completed||!input.byte)return {};
    }
    if(!p.live())return {};
    const auto controls=[&]{return p.write(style+"\r\n"+file_option('A',"Abort logoff",ansi)+"  "+
        file_option('D',"Disconnect now",ansi)+"\r\n");};
    if(armed){if(!controls())return {};}
    else if(!p.write(style+"\r\nWaiting for transfer input to settle.\r\n"))return {};
    const auto deadline=now()+std::chrono::seconds(10);
    unsigned shown{};bool noise{};char command{};
    const auto finish=[&](bool disconnect,bool cr)->std::optional<bool>{
        if(p.accepted_key)p.accepted_key(cr);
        if(!p.write("\r\n"))return {};
        return disconnect;
    };
    while(p.live()){
        const auto remaining=deadline-now();
        if(remaining<=std::chrono::steady_clock::duration::zero())return finish(true,false);
        const auto seconds=static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()+999)/1000;
        if(seconds!=shown){
            if(!p.write(style+"\rLogging off in "+std::to_string(seconds)+" second(s).   "))return {};
            shown=seconds;
        }
        const auto input=p.byte();
        if(!p.live())return {};
        // Deadline wins even when a transport read finishes after expiry.
        if(now()>=deadline)return finish(true,false);
        if(input.outcome==TerminalOutcome::timed_out){
            if(armed&&!noise&&command)return finish(command=='D',false);
            if(!armed){if(!controls())return {};shown=0;}
            armed=true;noise=false;command=0;continue;
        }
        if(input.outcome!=TerminalOutcome::completed||!input.byte)return {};
        if(!armed)continue; // pending Enter, ZMODEM tail, or pretyped hotkeys
        const auto c=*input.byte;
        if(c=='\r'||c=='\n'){
            if(!noise&&command)return finish(command=='D',c=='\r');
            noise=false;command=0;continue;
        }
        // Only a standalone hotkey is actionable. Escape sequences and
        // protocol/control strings cannot contribute an embedded A or D.
        if(!noise&&!command&&(c=='A'||c=='a'||c=='D'||c=='d'))command=(c=='a'?'A':c=='d'?'D':static_cast<char>(c));
        else {noise=true;command=0;}
    }
    return {};
}

std::string file_size_label(std::uint64_t bytes){
    std::uint64_t unit=1024;char suffix='K';
    if(bytes>=1024ULL*1024){unit*=1024;suffix='M';}
    if(bytes>=1024ULL*1024*1024){unit*=1024;suffix='G';}
    // Integer rounding avoids locale changes, floating-point drift and overflow.
    auto tenths=(bytes/unit)*10+((bytes%unit)*10+unit/2)/unit;
    if(tenths>=10240&&suffix!='G'){unit*=1024;suffix=suffix=='K'?'M':'G';tenths=(bytes/unit)*10+((bytes%unit)*10+unit/2)/unit;}
    return std::to_string(tenths/10)+"."+std::to_string(tenths%10)+suffix;
}
std::string file_terminal_text(std::string_view input, std::size_t maximum) {
    maximum=std::min<std::size_t>(maximum,16384);
    const auto text=sanitize_file_text(input, maximum*4,true);
    std::string result;
    for(std::size_t i=0;i<text.size() && result.size()<maximum;){
        const auto c=static_cast<unsigned char>(text[i]);
        const std::size_t width=c<128?1:c<224?2:c<240?3:4;
        const auto encoded=qwk_utf8_to_cp437(std::string_view(text).substr(i,width));
        result+=encoded?*encoded:"?";i+=width;
    }
    return result;
}
std::string file_option(char key,std::string_view description,bool ansi){
    std::string result=ansi?"\x1b[0;37;40m[\x1b[1;37;40m":"[";
    result+=key;result+=ansi?"\x1b[0;37;40m] ":"] ";result+=file_terminal_text(description,512);return result;
}
#include "file_information_presentation.inc"
std::string render_file_menu_rows(const std::vector<MenuTextRow>& rows,bool ansi,const MenuPreferences& preferences,std::uint8_t default_color){
    if(!ansi)return render_menu_rows(rows,false,preferences,default_color);
    std::string result;
    for(const auto& row:rows){
        if(row.role!=MenuTextRole::command){auto unchanged=render_menu_rows({row},true,preferences,default_color);if(unchanged.size()>=4&&unchanged.substr(unchanged.size()-4)=="\x1b[0m")unchanged.resize(unchanged.size()-4);result+=unchanged;continue;}
        // Static native command rows only; never parse artwork or file metadata.
        result+="\x1b[0;37;40m";
        for(std::size_t i=0;i<row.text.size();++i){
            if(i+3<row.text.size()&&row.text[i]=='['&&row.text[i+2]==':'&&row.text[i+3]=='>'&&((row.text[i+1]>='A'&&row.text[i+1]<='Z')||row.text[i+1]=='?')){
                result+="[\x1b[1;37;40m";result+=row.text[++i];result+="\x1b[0;37;40m";
            }else if(i+3<row.text.size()&&row.text.substr(i,4)=="[/G>"){
                result+="[/\x1b[1;37;40mG\x1b[0;37;40m>";i+=3;
            }else result+=row.text[i];
        }result+="\r\n";
    }result+="\x1b[0m";return result;
}
bool present_file_text(ICallerTerminal& terminal,std::string_view identity,std::string_view utf8,unsigned columns,unsigned rows){
    columns=std::clamp(columns,20U,80U);rows=std::clamp(rows,10U,24U);
    const auto indentation=std::min<std::size_t>(utf8.find_first_not_of(' '),utf8.size());
    auto text=std::string(std::min<std::size_t>(indentation,16384),' ')+file_terminal_text(utf8.substr(indentation));TerminalDisplayProgram program;std::string line;
    const auto flush=[&](){
        if(!line.empty()){program.output_byte_count+=line.size();program.operations.push_back({TerminalDisplayOperationKind::cp437_text,std::move(line)});line.clear();}
        program.operations.push_back({TerminalDisplayOperationKind::paging_newline,{}});program.output_byte_count+=2;
    };
    for(const auto c:text){if(c=='\n')flush();else {line+=c;if(line.size()==columns)flush();}}
    if(!line.empty())flush();
    TerminalDisplayPresentationRequest request{std::string(identity),&program,{true,static_cast<std::uint16_t>(rows)}};request.system_ui=true;
    return terminal.present_display(request)==TerminalOutcome::completed;
}
bool present_file_transfer_text(ICallerTerminal& terminal,std::string_view identity,
                                const std::vector<std::string>& lines,MessageEditorDimensions size){
    const auto width=std::clamp<std::size_t>(size.columns,20,80)-1;
    const auto rows=std::clamp(size.rows,10U,24U);
    TerminalDisplayProgram program;
    for(const auto& line:lines){
        const auto indent=std::min({line.find_first_not_of(' '),line.size(),width-1});
        auto text=file_terminal_text(std::string_view(line).substr(indent),512);
        std::replace(text.begin(),text.end(),'\n',' ');
        for(const auto& part:wrap_message_line(text,width-indent,false)){
            const auto first=part.text.find_first_not_of(' ');
            if(first==std::string::npos&&!text.empty())continue;
            if(first!=std::string::npos){
                const auto last=part.text.find_last_not_of(' ');
                auto visible=std::string(indent,' ')+part.text.substr(first,last-first+1);
                program.output_byte_count+=visible.size();
                program.operations.push_back({TerminalDisplayOperationKind::cp437_text,std::move(visible)});
            }
            program.operations.push_back({TerminalDisplayOperationKind::paging_newline,{}});
            program.output_byte_count+=2;
        }
    }
    TerminalDisplayPresentationRequest request{std::string(identity),&program,{true,static_cast<std::uint16_t>(rows)}};
    request.system_ui=true;
    return terminal.present_display(request)==TerminalOutcome::completed;
}
std::string_view file_transfer_text_handoff(bool ansi) noexcept{
    return ansi?"\r\x1b[2K\x1b[?7h\x1b[0m":"\r\n";
}
bool present_file_login_scan_question(ICallerTerminal& terminal,bool ansi){
    TerminalDisplayProgram program;
    const auto add=[&](TerminalDisplayOperationKind kind,std::string bytes={}){
        program.output_byte_count+=kind==TerminalDisplayOperationKind::logical_newline?2:bytes.size();
        program.operations.push_back({kind,std::move(bytes)});
    };
    // The preceding accepted Enter already ends its line. Add one blank row.
    add(TerminalDisplayOperationKind::logical_newline);
    const auto style=[&](const char* value){if(ansi)add(TerminalDisplayOperationKind::ansi_sequence,value);};
    const auto square=[&](const char* color){style(color);add(TerminalDisplayOperationKind::cp437_text,"\xfe");};
    if(ansi){square("\x1b[1;30;40m");square("\x1b[0;37;40m");square("\x1b[1;37;40m");add(TerminalDisplayOperationKind::cp437_text," ");}
    style("\x1b[0;37;40m");add(TerminalDisplayOperationKind::cp437_text,"New File Scan");
    if(ansi){add(TerminalDisplayOperationKind::cp437_text," ");square("\x1b[1;37;40m");square("\x1b[0;37;40m");square("\x1b[1;30;40m");style("\x1b[0m");}
    add(TerminalDisplayOperationKind::logical_newline);
    add(TerminalDisplayOperationKind::logical_newline);
    style("\x1b[0;37;40m");add(TerminalDisplayOperationKind::cp437_text,"List new files since last logon? [Y/n]");
    style("\x1b[0m");
    return terminal.present_display({"files.login.scan.question",&program,{}})==TerminalOutcome::completed;
}
std::optional<TerminalDisplayProgram> prepare_file_view(std::string_view bytes,bool ansi){
    if(bytes.size()>kMessageSourceBytes)return {};
    if(bytes.find('\x1b')!=std::string_view::npos){
        const auto validated=validate_message_source({MessageSourceFormat::ansi_canvas,1,std::string(bytes)},kMessageCanvasRows);
        if(!validated)return {};
        TerminalDisplayProgram result;
        for(std::size_t first=0;first<validated.off.extent;first+=100){const auto page=message_canvas_page(validated.off,first,std::min<std::size_t>(100,validated.off.extent-first),ansi);if(!page)return {};
            for(auto op:page->operations){if(op.kind==TerminalDisplayOperationKind::logical_newline)op.kind=TerminalDisplayOperationKind::paging_newline;result.operations.push_back(std::move(op));}
            result.output_byte_count+=page->output_byte_count;if(result.operations.size()>kTerminalMaximumDisplayOperations||result.output_byte_count>kTerminalMaximumDisplayOutputBytes)return {};
        }
        return valid_terminal_display_program(result)?std::optional<TerminalDisplayProgram>{std::move(result)}:std::nullopt;
    }
    std::string decoded;
    if(valid_file_utf8(bytes)){const auto encoded=qwk_utf8_to_cp437(bytes);if(!encoded)return {};decoded=*encoded;bytes=decoded;}
    TerminalDisplayProgram program;std::string row;bool cr{};
    const auto flush=[&](){if(!row.empty()){program.output_byte_count+=row.size();program.operations.push_back({TerminalDisplayOperationKind::cp437_text,std::move(row)});row.clear();}program.operations.push_back({TerminalDisplayOperationKind::paging_newline,{}});program.output_byte_count+=2;};
    if(ansi){program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m"});program.output_byte_count+=10;}
    for(std::size_t i=0;i<bytes.size();++i){const auto c=static_cast<unsigned char>(bytes[i]);
        if(cr&&c=='\n'){cr=false;continue;}cr=false;
        if(c=='\r'||c=='\n'||c=='\f'){flush();cr=c=='\r';}
        else if(c=='\t'){const auto spaces=8-row.size()%8;row.append(spaces,' ');if(row.size()>=80)flush();}
        else if(c==26&&i+1==bytes.size())break;
        else {if(c<32||c==127)return {};row+=static_cast<char>(c);if(row.size()==80)flush();}
        if(program.operations.size()+2>=kTerminalMaximumDisplayOperations)return {};
    }
    if(!row.empty())flush();
    return valid_terminal_display_program(program)?std::optional<TerminalDisplayProgram>{std::move(program)}:std::nullopt;
}
namespace {
std::string fit(std::string_view text,std::size_t width){const auto indentation=std::min({text.find_first_not_of(' '),text.size(),width});auto value=std::string(indentation,' ')+file_terminal_text(text.substr(indentation),width-indentation);std::replace(value.begin(),value.end(),'\n',' ');value.resize(width,' ');return value;}
unsigned number(std::string_view text){unsigned result{};const auto n=std::from_chars(text.data(),text.data()+text.size(),result);return n.ec==std::errc{} && n.ptr==text.data()+text.size()?result:0;}
FileListChoice row_choice(const FileListPage& page,std::size_t row,FileListAction action){
    if(row>=page.rows.size())return {FileListAction::cancel};
    return {action,page.rows[row].identity,page.rows[row].revision};
}
FileListChoice command(const FileListPage& page,char key,std::size_t selected){
    switch(key){
    case 'Q':return {FileListAction::cancel};
    case 'N':if(page.next)return {FileListAction::next};break;
    case 'P':if(page.previous)return {FileListAction::previous};break;
    case 'S':if(page.sort)return {FileListAction::sort};break;
    case 'J':if(page.jump)return {FileListAction::jump};break;
    case 'T':if(page.tag && !page.rows.empty())return row_choice(page,selected,FileListAction::tag);break;
    }return {FileListAction::failed};
}
std::string options(const FileListPage& page,bool ansi){
    std::string result;
    const auto add=[&](char key,const char* label){if(!result.empty())result+="  ";result+=file_option(key,label,ansi);};
    if(page.tag)add('T',"Tag");
    if(page.sort)add('S',"Sort");
    if(page.jump)add('J',"Find");
    if(page.numbered||!ansi){if(page.previous)add('P',"Previous");if(page.next)add('N',"Next");}add('Q',"Return");if(!page.numbered)result+=ansi?"  \x1b[0;37;40m[\x1b[1;37;40mCR\x1b[0;37;40m] View file":"  [CR] View file";return result;
}
FileListChoice plain(const FileListPorts& p,const FileListPage& page,std::size_t& selected){
    const bool compact=!page.numbered&&page.title.empty()&&page.context.empty();
    std::string text;if(!page.title.empty())text+=page.title+"\n";if(!page.context.empty())text+=page.context+"\n";text+=(page.numbered||compact?"":"  ")+page.columns+"\n";
    if(compact)text+=std::string(79,'-')+"\n";
    for(std::size_t n=0;n<page.rows.size();++n){const auto& row=page.rows[n];text+=(page.numbered?std::to_string(row.number)+" ":n==selected?"> ":"  ")+row.text+"\n";}
    if(page.rows.empty())text+="No eligible entries.\n";
    if(!page.notice.empty())text+=page.notice+"\n";
    if(compact)text+=std::string(79,'-')+"\n";
    const auto size=p.dimensions?p.dimensions():MessageEditorDimensions{};
    if(size.ansi&&p.write&&!p.write("\x1b[0;37;40m"))return {};
    if(!p.live() || !present_file_text(p.terminal,"files.list",text,size.columns,size.rows))return {};
    if(p.emitted && !page.rows.empty() && !p.emitted(page.rows))return {};
    for(;;){
        if(!p.live())return {};
        const auto input=p.terminal.read_visible({"files.list.choice",options(page,false)+(page.numbered?" or number: ":" - Filename or command: "),page.numbered?8u:128u,EmptyInputPolicy::use_default,{},true});
        if(input.outcome==TerminalOutcome::cancelled)return {FileListAction::cancel};
        if(input.outcome==TerminalOutcome::empty)return !page.numbered&&!page.rows.empty()?row_choice(page,selected,FileListAction::open):FileListChoice{FileListAction::cancel};
        if(!input.has_value())return {};
        if(!p.live())return {};
        if(!page.numbered){
            const auto choose_name=[&](std::string_view name,FileListAction action)->FileListChoice{std::optional<std::size_t> match;for(std::size_t i=0;i<page.rows.size();++i)if(file_case_key(page.rows[i].filename)==file_case_key(qwk_cp437_to_utf8(name))){if(match)return {};match=i;}if(!match)return {};selected=*match;return row_choice(page,selected,action);};
            if(input.value.size()==1){auto c=input.value[0];if(c>='a'&&c<='z')c=static_cast<char>(c-32);const auto choice=command(page,c,selected);if(choice.action!=FileListAction::failed)return choice;}
            const auto choice=choose_name(input.value,FileListAction::open);if(choice.action!=FileListAction::failed)return choice;
            if(!present_file_text(p.terminal,"files.list.invalid","Enter one unambiguous displayed filename or command."))return {};
            continue;
        }
        const auto n=number(input.value);
        for(std::size_t i=0;i<page.rows.size();++i)if(page.rows[i].number==n){selected=i;return row_choice(page,i,FileListAction::open);}
        auto value=input.value;for(auto& c:value)if(c>='a'&&c<='z')c=static_cast<char>(c-32);
        if(value.size()==1){
            if(value[0]=='T' && page.tag){
                const auto which=p.terminal.read_visible({"files.list.tag","Tag displayed number (Enter cancels): ",6,EmptyInputPolicy::use_default,{},true});
                if(which.outcome==TerminalOutcome::empty||which.outcome==TerminalOutcome::cancelled)continue;
                if(!which.has_value()||!p.live())return {};
                const auto tag=number(which.value);
                for(std::size_t i=0;i<page.rows.size();++i)if(page.rows[i].number==tag){selected=i;return row_choice(page,i,FileListAction::tag);}
            }else {const auto choice=command(page,value[0],selected);if(choice.action!=FileListAction::failed)return choice;}
        }
        if(!present_file_text(p.terminal,"files.list.invalid","Choose a displayed number or command."))return {};
    }
}
}
FileListChoice choose_file_row(const FileListPorts& p,const FileListPage& page,std::size_t& selected){
    if(!p.live || page.rows.size()>kFilePageMaximum)return {};
    for(std::size_t i=0;i<page.rows.size();++i){const auto& row=page.rows[i];if(!row.number||row.number>999999||row.identity<=0||row.revision<=0||(!page.numbered&&!valid_file_name(row.filename)))return {};
        for(std::size_t j=0;j<i;++j)if(page.rows[j].number==row.number||page.rows[j].identity==row.identity)return {};
    }
    const auto dimensions=p.dimensions?p.dimensions():MessageEditorDimensions{};
    selected=page.rows.empty()?0:std::min(selected,page.rows.size()-1);
    if(!p.lightbars||(p.lightbars_enabled&&!p.lightbars_enabled())||!dimensions.ansi||dimensions.columns<80||dimensions.rows<24||!p.byte||!p.write)return plain(p,page,selected);
    selected=page.rows.empty()?0:std::min(selected,page.rows.size()-1);
    MessageEditorKeyDecoder decoder;std::string digits;std::size_t top{};
    std::vector<std::string> rendered;
    unsigned rendered_columns{};
    std::optional<std::size_t> emitted_top;
    std::size_t emitted_height{};
    const auto finish=[&](FileListChoice choice){if(!p.write("\x1b[0;37;40m\r\n"))return FileListChoice{};return choice;};
    for(;;){
        if(!p.live())return {};
        const auto size=p.dimensions();
        if(!size.ansi||size.columns<80||size.rows<24||(p.lightbars_enabled&&!p.lightbars_enabled())){if(!p.write(size.ansi?"\x1b[0;37;40m\r\n":"\r\n"))return {};return plain(p,page,selected);}
        const auto colors=p.selection_colors?p.selection_colors():std::make_pair(p.selected_foreground,p.selected_background);
        const bool compact=!page.numbered&&page.title.empty()&&page.context.empty();
        const auto rows=std::clamp(size.rows,24U,50U);const auto height=rows-(compact?5U:8U)-(compact&&!page.notice.empty()?1U:0U);
        if(selected<top)top=selected;else if(selected>=top+height)top=selected-height+1;
        std::vector<std::string> frame(rows);
        const auto line=[&](unsigned row,std::string_view text,std::string_view sgr){frame[row-1]=std::string(sgr)+fit(text,79);};
        if(!compact){line(1,page.title,"\x1b[0;37;40m");line(2,page.context,"\x1b[0;37;40m");}
        line(compact?1:3,page.columns,compact?"\x1b[1;37;40m":"\x1b[0;37;40m");
        line(compact?2:4,std::string(79,'-'),"\x1b[1;30;40m");
        for(unsigned i=0;i<height;++i){const auto at=top+i;
            const auto text=at<page.rows.size()?(page.numbered?std::to_string(page.rows[at].number)+" ":"")+page.rows[at].text:i==0?"No eligible entries.":"";
            line(i+(compact?3:5),text,at==selected&&!page.rows.empty()?menu_text_sgr(colors.first,colors.second):"\x1b[0;37;40m");
        }
        if(compact){if(!page.notice.empty())line(rows-3,page.notice,"\x1b[0;37;40m");line(rows-2,std::string(79,'-'),"\x1b[1;30;40m");}
        else line(rows-3,page.notice,"\x1b[0;37;40m");
        line(rows-(compact?1:2),"Arrows: select - PgUp/PgDn: page - Home/End: first/last row","\x1b[0;37;40m");
        frame[rows-(compact?1:2)]=options(page,true);
        if(page.numbered)line(rows,"Enter: open   Number: "+digits,"\x1b[0;37;40m");
        const bool full=rendered.size()!=frame.size() || rendered_columns!=size.columns;
        std::string update=full?"\x1b[0;37;40m\x1b[2J":"";
        for(std::size_t i=0;i<frame.size();++i){
            if((full && !frame[i].empty()) || (!full && frame[i]!=rendered[i]))
                update+="\x1b["+std::to_string(i+1)+";1H"+frame[i]+"\x1b[0;37;40m\x1b[K";
        }
        if(!update.empty()){
            if(!p.write(update))return {};
            const auto end=std::min<std::size_t>(top+height,page.rows.size());
            if(p.emitted && end>top && (emitted_top!=top || emitted_height!=height) &&
                !p.emitted({page.rows.begin()+static_cast<std::ptrdiff_t>(top),page.rows.begin()+static_cast<std::ptrdiff_t>(end)}))return {};
            rendered=std::move(frame);
            rendered_columns=size.columns;
            emitted_top=top;
            emitted_height=height;
        }
        const auto byte=p.byte();std::optional<MessageEditorInput> input;
        if(byte.outcome==TerminalOutcome::timed_out)input=decoder.idle();else if(byte.byte)input=decoder.feed(*byte.byte);else return {};
        if(!input)continue;
        if(!p.live())return {};
        if(input->command==MessageEditorCommand::actions)return finish({FileListAction::cancel});
        if(!input->event)continue;
        const auto event=*input->event;
        if(event.key==MessageEditorKey::enter){
            if(p.accepted_key)p.accepted_key(event.byte=='\r');
            decoder.reset();
            if(!digits.empty()){const auto n=number(digits);digits.clear();const auto at=std::find_if(page.rows.begin(),page.rows.end(),[&](const auto& row){return row.number==n;});if(at==page.rows.end())continue;selected=static_cast<std::size_t>(at-page.rows.begin());}
            if(!page.rows.empty())return finish(row_choice(page,selected,FileListAction::open));
            continue;
        }
        if(event.key==MessageEditorKey::text){
            auto c=static_cast<char>(event.byte);if(c>='0'&&c<='9'){if(page.numbered&&digits.size()<6)digits+=c;continue;}
            if(c>='a'&&c<='z')c=static_cast<char>(c-32);
            const auto choice=command(page,c,selected);if(choice.action!=FileListAction::failed)return finish(choice);
        }else if(event.key==MessageEditorKey::backspace){if(!digits.empty())digits.pop_back();}
        else {digits.clear();switch(event.key){
            case MessageEditorKey::up:if(selected)--selected;break;
            case MessageEditorKey::down:if(selected+1<page.rows.size())++selected;break;
            case MessageEditorKey::home:selected=0;break;
            case MessageEditorKey::end:if(!page.rows.empty())selected=page.rows.size()-1;break;
            case MessageEditorKey::page_up:if(top)selected=selected>height?selected-height:0;else if(page.previous)return finish({FileListAction::previous});break;
            case MessageEditorKey::page_down:if(top+height<page.rows.size())selected=std::min(selected+height,page.rows.size()-1);else if(page.next)return finish({FileListAction::next});break;
            default:break;
        }}
    }
}
} // namespace sfx::application
