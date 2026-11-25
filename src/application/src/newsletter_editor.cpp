#include "sfx/application/newsletter_editor.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/application/sysop_session.hpp"
#include <algorithm>

namespace sfx::application {
namespace {
bool valid_document(std::string_view bytes){
    if(bytes.size()>NewsletterDocument::maximum_bytes)return false;
    for(const unsigned char c:bytes)if(c<32&&c!='\t'&&c!='\r'&&c!='\n')return false;
    return bytes.find(char(127))==bytes.npos;
}
std::string visible(std::string_view bytes){
    std::string result;for(const unsigned char c:bytes){if(c=='\t')result.append(8-result.size()%8,' ');else result+=char(c);}return result;
}
}
bool NewsletterDocument::load(std::string bytes){
    if(!valid_document(bytes))return false;
    bytes_=std::move(bytes);cursor_=top_=left_=0;inserting_=true;newline_="\r\n";
    const auto at=bytes_.find_first_of("\r\n");if(at!=bytes_.npos)newline_=bytes_.substr(at,bytes_[at]=='\r'&&at+1<bytes_.size()&&bytes_[at+1]=='\n'?2:1);
    return true;
}
std::vector<NewsletterDocument::Line> NewsletterDocument::index()const{
    std::vector<Line> out;std::size_t at{};
    for(;;){const auto end=bytes_.find_first_of("\r\n",at);if(end==bytes_.npos){out.push_back({at,bytes_.size(),bytes_.size()});break;}
        const auto next=end+(bytes_[end]=='\r'&&end+1<bytes_.size()&&bytes_[end+1]=='\n'?2:1);out.push_back({at,end,next});at=next;}
    return out;
}
std::vector<std::string> NewsletterDocument::lines()const{std::vector<std::string> out;for(const auto& line:index())out.push_back(bytes_.substr(line.begin,line.end-line.begin));return out;}
std::size_t NewsletterDocument::row()const{const auto rows=index();for(std::size_t n=0;n<rows.size();++n)if(cursor_<=rows[n].end)return n;return rows.size()-1;}
std::size_t NewsletterDocument::visual_column(std::size_t begin,std::size_t end)const{std::size_t column{};for(auto at=begin;at<end;++at)column+=bytes_[at]=='\t'?8-column%8:1;return column;}
bool NewsletterDocument::splice(std::size_t at,std::size_t count,std::string_view text){
    if(at>bytes_.size()||count>bytes_.size()-at||text.size()>maximum_bytes-(bytes_.size()-count)||!valid_document(text))return false;
    bytes_.replace(at,count,text);cursor_=at+text.size();return true;
}
bool NewsletterDocument::replace_line(std::size_t n,std::string_view text){const auto rows=index();return n&&n<=rows.size()&&text.find_first_of("\r\n")==text.npos&&splice(rows[n-1].begin,rows[n-1].end-rows[n-1].begin,text);}
bool NewsletterDocument::insert_line(std::size_t n,std::string_view text){const auto rows=index();if(!n||n>rows.size()+1||text.find_first_of("\r\n")!=text.npos)return false;
    if(n<=rows.size())return splice(rows[n-1].begin,0,std::string(text)+newline_);
    return splice(bytes_.size(),0,newline_+std::string(text));
}
bool NewsletterDocument::delete_line(std::size_t n){const auto rows=index();if(!n||n>rows.size())return false;const auto& line=rows[n-1];return splice(line.begin,line.next-line.begin,{});}
bool NewsletterDocument::apply(MessageEditorEvent event){
    const auto rows=index();const auto current=row();const auto line=rows[current];
    switch(event.key){
    case MessageEditorKey::text:if(event.byte<32&&event.byte!='\t')return false;return splice(cursor_,!inserting_&&cursor_<line.end?1:0,std::string(1,char(event.byte)));
    case MessageEditorKey::enter:return splice(cursor_,0,newline_);
    case MessageEditorKey::left:if(cursor_){--cursor_;if(cursor_&&bytes_[cursor_]=='\n'&&bytes_[cursor_-1]=='\r')--cursor_;}break;
    case MessageEditorKey::right:if(cursor_<bytes_.size()){const auto step=bytes_[cursor_]=='\r'&&cursor_+1<bytes_.size()&&bytes_[cursor_+1]=='\n'?2:1;cursor_+=step;}break;
    case MessageEditorKey::backspace:if(cursor_){const auto count=cursor_>1&&bytes_[cursor_-1]=='\n'&&bytes_[cursor_-2]=='\r'?2:1;return splice(cursor_-count,count,{});}break;
    case MessageEditorKey::erase:if(cursor_<bytes_.size())return splice(cursor_,bytes_[cursor_]=='\r'&&cursor_+1<bytes_.size()&&bytes_[cursor_+1]=='\n'?2:1,{});break;
    case MessageEditorKey::home:cursor_=line.begin;break;
    case MessageEditorKey::end:cursor_=line.end;break;
    case MessageEditorKey::insert_mode:inserting_=!inserting_;break;
    case MessageEditorKey::up:case MessageEditorKey::down:case MessageEditorKey::page_up:case MessageEditorKey::page_down:{
        const auto distance=event.key==MessageEditorKey::page_up||event.key==MessageEditorKey::page_down?page_rows_:1;
        const auto target=event.key==MessageEditorKey::up||event.key==MessageEditorKey::page_up?(current>distance?current-distance:0):std::min(current+distance,rows.size()-1);
        const auto column=visual_column(line.begin,cursor_);cursor_=rows[target].begin;
        while(cursor_<rows[target].end&&visual_column(rows[target].begin,cursor_+1)<=column)++cursor_;
        break;
    }
    default:break;
    }return true;
}
std::string NewsletterDocument::frame(unsigned columns,unsigned height){
    MessageEditorScreen screen;return update_frame(columns,height,screen);
}
std::string NewsletterDocument::update_frame(unsigned columns,unsigned height,MessageEditorScreen& previous){
    columns=std::clamp(columns,40U,160U);height=std::clamp(height,10U,60U);page_rows_=height-4;
    const auto rows=index();const auto current=row();const auto column=visual_column(rows[current].begin,cursor_);
    if(current<top_)top_=current;
    if(current>=top_+page_rows_)top_=current-page_rows_+1;
    if(column<left_)left_=column;
    if(column>=left_+columns-1)left_=column-columns+2;
    const auto pos=[](std::size_t r,std::size_t c){return "\x1b["+std::to_string(r)+";"+std::to_string(c)+"H";};
    std::vector<std::string> screen(height);
    screen[0]=" SFXEditor - SF Newsletter";
    screen[1]="Ctrl-Z Save | Ctrl-A Cancel | Esc Help";
    for(std::size_t n=0;n<page_rows_;++n)if(top_+n<rows.size()){auto text=visible(std::string_view(bytes_).substr(rows[top_+n].begin,rows[top_+n].end-rows[top_+n].begin));if(left_<text.size())screen[n+2]=text.substr(left_,columns-1);}
    screen.back()=std::string(inserting_?"INS":"OVR")+"  Line "+std::to_string(current+1)+"  Column "+std::to_string(column+1)+"  Formatting preserved";
    for(auto& line:screen)line.resize(columns-1,' ');
    const bool full=previous.rows.size()!=screen.size()||(!previous.rows.empty()&&previous.rows.front().size()!=columns-1);
    std::string changed;
    for(std::size_t n=0;n<screen.size();++n){
        if(full){changed+=pos(n+1,1)+screen[n];continue;}
        if(screen[n]==previous.rows[n])continue;
        std::size_t first{},last=screen[n].size();while(first<last&&screen[n][first]==previous.rows[n][first])++first;
        while(last>first&&screen[n][last-1]==previous.rows[n][last-1])--last;
        changed+=pos(n+1,first+1)+screen[n].substr(first,last-first);
    }
    const auto cursor_row=current-top_+3,cursor_column=column-left_+1;
    if(changed.empty()&&previous.cursor_row==cursor_row&&previous.cursor_column==cursor_column)return {};
    auto out=std::string("\x1b[?7l\x1b[0;37;40m")+(full?"\x1b[2J":"")+changed+pos(cursor_row,cursor_column)+"\x1b[?7h";
    previous.rows=std::move(screen);previous.cursor_row=cursor_row;previous.cursor_column=cursor_column;return out;
}
bool edit_sysop_newsletter(NewsletterEditorPorts p){
    if(!p.authorized())return false;
    const auto say=[&](std::string_view text){
        std::string wrapped;
        for(const auto& row:wrap_sysop_log_text(text,p.columns(),false)){
            if(!wrapped.empty())wrapped+='\n';
            wrapped+=row;
        }
        return p.authorized()&&present_message_text(p.terminal,"sysop.newsletter",wrapped);
    };
    const auto report=[&](std::string_view text){
        if(p.ansi()&&p.terminal.clear_screen()!=TerminalOutcome::completed)return false;
        if(!say(text)||!say("Press <ENTER> to continue."))return false;
        const auto result=p.terminal.pause({"newsletter.result"});
        return result==TerminalOutcome::completed||result==TerminalOutcome::cancelled;
    };
    const auto snapshot=p.load();if(!snapshot.available)return report(snapshot.notice);
    NewsletterDocument document;if(!document.load(snapshot.bytes))return report("Newsletter cannot be edited: use plain ASCII/CP437 text without ANSI controls, up to 256 KiB. The file is unchanged.");
    MessageEditorKeyDecoder decoder;std::string previous;MessageEditorScreen rendered;
    for(;;){
        if(!p.authorized())return false;
        MessageEditorCommand command=MessageEditorCommand::none;
        if(p.ansi()&&p.columns()>=40&&p.rows()>=10){
            const auto frame=document.update_frame(p.columns(),p.rows(),rendered);if(!frame.empty()&&!p.write(frame))return false;
            const auto byte=p.byte();std::optional<MessageEditorInput> input;
            if(byte.outcome==TerminalOutcome::timed_out)input=decoder.idle();else if(byte.byte)input=decoder.feed(*byte.byte);else return false;
            if(!input)continue;
            command=input->command;
            if(input->event&&!document.apply(*input->event)){if(!say("Edit refused: document limit or unsupported control. Your draft is unchanged."))return false;previous.clear();rendered.clear();}
        }else{
            if(previous.empty()){if(!say("SFXEditor - SF Newsletter. /L List, /E# Edit, /I# Insert, /D# Delete, /S Save, /A Cancel."))return false;previous="plain";}
            const auto input=p.terminal.read_visible({"newsletter.command","Editor command: ",32,EmptyInputPolicy::reject,{},true});
            if(input.outcome==TerminalOutcome::cancelled)command=MessageEditorCommand::abort;
            else if(!input.has_value())return false;
            else{
                const auto parsed=message_line_command(input.value);command=parsed.command;
                if(command==MessageEditorCommand::list){TerminalDisplayProgram program;std::size_t n{};for(const auto& line:document.lines()){program.operations.push_back({TerminalDisplayOperationKind::cp437_text,std::to_string(++n)+": "+visible(line)});program.operations.push_back({TerminalDisplayOperationKind::paging_newline,{}});program.output_byte_count+=visible(line).size()+32;}
                    const auto listed=p.terminal.present_display({"newsletter.list",&program,{true,static_cast<std::uint16_t>(std::clamp(p.rows(),10U,24U))}});
                    if(listed!=TerminalOutcome::completed&&listed!=TerminalOutcome::cancelled)return false;
                    continue;}
                if(command==MessageEditorCommand::edit_line||command==MessageEditorCommand::insert_line||command==MessageEditorCommand::delete_line){
                    if(!parsed.number){if(!say("Add the line number, for example /E16."))return false;continue;}
                    bool ok=false;if(command==MessageEditorCommand::delete_line)ok=document.delete_line(*parsed.number);
                    else{const auto replacement=p.terminal.read_visible({"newsletter.line","Replacement line: ",4096,EmptyInputPolicy::use_default,{},true});if(replacement.outcome==TerminalOutcome::cancelled)continue;if(replacement.outcome!=TerminalOutcome::completed&&replacement.outcome!=TerminalOutcome::empty)return false;
                        ok=command==MessageEditorCommand::edit_line?document.replace_line(*parsed.number,replacement.value):document.insert_line(*parsed.number,replacement.value);}
                    if(!say(ok?"Draft updated.":"Line or size limit invalid. Draft unchanged."))return false;
                    continue;
                }
            }
        }
        if(command==MessageEditorCommand::save){
            if(p.accepted_key)p.accepted_key(false);
            if(!p.authorized())return false;
            const auto result=p.save(snapshot,document.bytes());if(!report(result.notice))return false;if(result.saved)return true;previous.clear();rendered.clear();
        }else if(command==MessageEditorCommand::abort){
            if(p.accepted_key)p.accepted_key(false);
            const auto answer=p.terminal.read_key({"newsletter.cancel","Discard newsletter edits? [y/N] ",{'Y','N'},'N'});
            if(answer.key=='Y')return report("Newsletter unchanged.");
            if(!answer.has_key()&&answer.outcome!=TerminalOutcome::cancelled)return false;
            decoder.reset();previous.clear();rendered.clear();
        }else if(command!=MessageEditorCommand::none){
            if(p.accepted_key)p.accepted_key(false);
            if(p.ansi()&&p.terminal.clear_screen()!=TerminalOutcome::completed)return false;
            if(!say("SFXEditor: arrows/Home/End/PgUp/PgDn move; Insert toggles overwrite. Ctrl-Z saves; Ctrl-A cancels. Tabs and line breaks stay as typed."))return false;
            const auto answer=p.terminal.read_key({"newsletter.help","[E] Edit [S] Save [A] Cancel: ",{'E','S','A'},'E'});
            if(!answer.has_key())return false;
            if(answer.key=='S'){if(!p.authorized())return false;const auto result=p.save(snapshot,document.bytes());if(!report(result.notice))return false;if(result.saved)return true;}
            if(answer.key=='A')return report("Newsletter unchanged.");
            decoder.reset();previous.clear();rendered.clear();
        }
    }
}
}
