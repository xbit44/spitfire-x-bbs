#include "sfx/application/message_editor.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
void MessageEditorKeyDecoder::reset() noexcept {sequence_.clear();lf_after_cr_=false;string_escape_=false;string_tail_=false;discard_sequence_=false;}
std::optional<MessageEditorInput> MessageEditorKeyDecoder::idle(){
    if(sequence_=="\x1b"){sequence_.clear();return MessageEditorInput{{},MessageEditorCommand::actions};}return {};
}
std::optional<MessageEditorInput> MessageEditorKeyDecoder::feed(unsigned char byte){
    const auto key=[](MessageEditorKey k,unsigned char c=0){return MessageEditorInput{MessageEditorEvent{k,c},MessageEditorCommand::none};};
    const auto command=[](MessageEditorCommand c){return MessageEditorInput{{},c};};
    if(discard_sequence_){if(byte>=0x40 && byte<=0x7e){discard_sequence_=false;return command(MessageEditorCommand::invalid);}return {};}
    if(string_escape_){
        if(byte==7 || (string_tail_ && byte=='\\')){string_escape_=false;string_tail_=false;}
        else string_tail_=byte==27;
        return {};
    }
    if(!sequence_.empty()){
        if(sequence_=="\x1b" && (byte==']' || byte=='P' || byte=='X' || byte=='^' || byte=='_')){sequence_.clear();string_escape_=true;return {};}
        sequence_.push_back(static_cast<char>(byte));
        if(sequence_=="\x1b[" || sequence_=="\x1bO")return {};
        if(sequence_.size()>32){sequence_.clear();if(byte>=0x40 && byte<=0x7e)return command(MessageEditorCommand::invalid);discard_sequence_=true;return {};}
        if(sequence_.size()==2 && sequence_[1]!='[' && sequence_[1]!='O'){
            sequence_.clear();switch(byte>='a' && byte<='z'?byte-32:byte){
            case 'S':return command(MessageEditorCommand::save);case 'A':return command(MessageEditorCommand::abort);
            case 'Q':return command(MessageEditorCommand::quote);case 'T':return command(MessageEditorCommand::subject);
            case 'E':case 27:return key(MessageEditorKey::resume);default:return command(MessageEditorCommand::invalid);}
        }
        if(byte<0x40 || byte>0x7e)return {};
        const auto sequence=std::move(sequence_);sequence_.clear();
        if(sequence=="\x1b[A" || sequence=="\x1bOA")return key(MessageEditorKey::up);
        if(sequence=="\x1b[B" || sequence=="\x1bOB")return key(MessageEditorKey::down);
        if(sequence=="\x1b[C" || sequence=="\x1bOC")return key(MessageEditorKey::right);
        if(sequence=="\x1b[D" || sequence=="\x1bOD")return key(MessageEditorKey::left);
        if(sequence=="\x1b[H" || sequence=="\x1bOH" || sequence=="\x1b[1~" || sequence=="\x1b[7~")return key(MessageEditorKey::home);
        if(sequence=="\x1b[F" || sequence=="\x1bOF" || sequence=="\x1b[K" || sequence=="\x1b[4~" || sequence=="\x1b[8~")return key(MessageEditorKey::end);
        if(sequence=="\x1b[2~" || sequence=="\x1b[@")return key(MessageEditorKey::insert_mode);
        if(sequence=="\x1b[3~")return key(MessageEditorKey::erase);
        if(sequence=="\x1b[5~" || sequence=="\x1b[V")return key(MessageEditorKey::page_up);
        if(sequence=="\x1b[6~" || sequence=="\x1b[U")return key(MessageEditorKey::page_down);
        return command(MessageEditorCommand::invalid);
    }
    if(lf_after_cr_){lf_after_cr_=false;if(byte=='\n')return {};}
    if(byte==27){sequence_="\x1b";return {};}
    switch(byte){
    case 26:return command(MessageEditorCommand::save);case 1:return command(MessageEditorCommand::abort);
    case 17:return command(MessageEditorCommand::quote);case 19:return command(MessageEditorCommand::subject);
    case 12:return command(MessageEditorCommand::help);
    case '\r':lf_after_cr_=true;return key(MessageEditorKey::enter,byte);case '\n':return key(MessageEditorKey::enter,byte);
    case 8:case 127:return key(MessageEditorKey::backspace);
    default:if(byte>=32 || byte=='\t')return key(MessageEditorKey::text,byte);return command(MessageEditorCommand::invalid);
    }
}
MessageLineCommand message_line_command(std::string_view text){
    if(text.empty() || text.front()!='/')return {};
    std::string upper(text);for(auto& c:upper)if(c>='a' && c<='z')c=static_cast<char>(c-32);
    if(upper=="/S")return {MessageEditorCommand::save,{}};
    if(upper=="/A")return {MessageEditorCommand::abort,{}};
    if(upper=="/Q")return {MessageEditorCommand::quote,{}};
    if(upper=="/T")return {MessageEditorCommand::subject,{}};
    if(upper=="/?")return {MessageEditorCommand::help,{}};
    if(upper=="/L")return {MessageEditorCommand::list,{}};
    if(upper=="/UPLOAD" || upper=="/UL")return {MessageEditorCommand::upload,{}};
    if(upper.size()<2 || (upper[1]!='E' && upper[1]!='D' && upper[1]!='I'))return {};
    const auto command=upper[1]=='E'?MessageEditorCommand::edit_line:upper[1]=='D'?MessageEditorCommand::delete_line:MessageEditorCommand::insert_line;
    if(upper.size()==2)return {command,{}};
    std::size_t value{};const auto number=std::string_view(upper).substr(2);
    const auto parsed=std::from_chars(number.data(),number.data()+number.size(),value);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !value || value>kMaximumMessageLines+1)return {MessageEditorCommand::invalid,{}};
    return {command,value};
}
void MessageEditorState::resize(bool ansi,unsigned columns,unsigned rows) noexcept {
    if(!columns)columns=80;
    if(!rows)rows=24;
    full_screen_=ansi && columns>=80 && rows>=24;rows_=std::clamp(rows,24U,200U);reveal();
}
std::size_t MessageEditorState::cursor_row() const noexcept {
    const auto& body=draft_.body().canonical_lf;return static_cast<std::size_t>(std::count(body.begin(),body.begin()+std::min(cursor_,body.size()),'\n'));
}
std::size_t MessageEditorState::cursor_column() const noexcept {
    const auto& body=draft_.body().canonical_lf;const auto end=std::min(cursor_,body.size());
    const auto previous=end?body.rfind('\n',end-1):std::string::npos;return previous==std::string::npos?end:end-previous-1;
}
std::size_t MessageEditorState::offset(std::size_t row,std::size_t column) const noexcept {
    std::size_t start{};const auto& lines=draft_.body().rows;
    for(std::size_t i=0;i<row && i<lines.size();++i)start+=lines[i].size()+1;
    return row>=lines.size()?draft_.body().canonical_lf.size():start+std::min(column,lines[row].size());
}
void MessageEditorState::reveal() noexcept {
    cursor_=std::min(cursor_,draft_.body().canonical_lf.size());const auto row=cursor_row();
    if(row<top_)top_=row;else if(row>=top_+body_rows())top_=row-body_rows()+1;
}
void MessageEditorState::vertical(std::int64_t count) noexcept {
    const auto row=static_cast<std::int64_t>(cursor_row());
    const auto last=static_cast<std::int64_t>(draft_.body().rows.size());
    cursor_=offset(static_cast<std::size_t>(std::clamp(row+count,std::int64_t{},last)),preferred_column_);reveal();
}
MessageValidation MessageEditorState::insert_data(std::string_view data){
    const auto at=cursor_;const auto before=draft_.body().canonical_lf;
    auto merged=before;merged.insert(at,data);
    // Full-source mapping locates insertion after suffix-sensitive wrapping. Data
    // cannot dispatch /S or a control-key action through this entry point.
    const auto mapped=message_body_cursor_offset(merged,draft_.maximum_lines(),at+data.size());
    const auto result=draft_.replace_body(merged);if(result!=MessageValidation::valid)return result;
    cursor_=mapped.value_or(draft_.body().canonical_lf.size());
    preferred_column_=cursor_column();reveal();return result;
}
MessageValidation MessageEditorState::quote(const std::vector<std::string>& rows,const std::vector<std::size_t>& selected){
    if(selected.empty())return MessageValidation::invalid_selection;
    std::string text;std::size_t previous{};bool first=true;
    for(const auto index:selected){if(index>=rows.size() || (!first && index<=previous))return MessageValidation::invalid_selection;text+="> "+rows[index]+"\n";previous=index;first=false;}
    return insert_data(text);
}
MessageValidation MessageEditorState::apply(MessageEditorEvent event){
    const auto erase_at=[&](std::size_t at){
        auto body=draft_.body().canonical_lf;body.erase(at,1);
        const auto mapped=message_body_cursor_offset(body,draft_.maximum_lines(),at);
        const auto result=draft_.replace_body(body);
        if(result==MessageValidation::valid){cursor_=mapped.value_or(body.size());preferred_column_=cursor_column();reveal();}
        return result;
    };
    switch(event.key){
    case MessageEditorKey::text:{
        if((event.byte<32 && event.byte!='\t') || event.byte==127)return MessageValidation::invalid_control;
        if(!inserting_ && cursor_<draft_.body().canonical_lf.size() && draft_.body().canonical_lf[cursor_]!='\n'){
            auto body=draft_.body().canonical_lf;body.replace(cursor_,1,std::string(1,static_cast<char>(event.byte)));
            const auto mapped=message_body_cursor_offset(body,draft_.maximum_lines(),cursor_+1);
            const auto result=draft_.replace_body(body);if(result==MessageValidation::valid){cursor_=mapped.value_or(body.size());preferred_column_=cursor_column();reveal();}return result;
        }return insert_data(std::string(1,static_cast<char>(event.byte)));
    }
    case MessageEditorKey::enter:return insert_data("\n");
    case MessageEditorKey::backspace:if(cursor_)return erase_at(cursor_-1);break;
    case MessageEditorKey::erase:if(cursor_<draft_.body().canonical_lf.size())return erase_at(cursor_);break;
    case MessageEditorKey::left:if(cursor_)--cursor_;break;
    case MessageEditorKey::right:cursor_=std::min(cursor_+1,draft_.body().canonical_lf.size());break;
    case MessageEditorKey::up:vertical(-1);return MessageValidation::valid;
    case MessageEditorKey::down:vertical(1);return MessageValidation::valid;
    case MessageEditorKey::home:cursor_=offset(cursor_row(),0);break;
    case MessageEditorKey::end:cursor_=offset(cursor_row(),80);break;
    case MessageEditorKey::page_up:vertical(-static_cast<std::int64_t>(body_rows()));return MessageValidation::valid;
    case MessageEditorKey::page_down:vertical(body_rows());return MessageValidation::valid;
    case MessageEditorKey::insert_mode:inserting_=!inserting_;break;
    case MessageEditorKey::resume:break;
    }
    preferred_column_=cursor_column();reveal();return MessageValidation::valid;
}
std::string MessageEditorState::frame(std::string_view to,std::string_view from) const {
    MessageEditorScreen screen;return update_frame(to,from,screen);
}
std::string MessageEditorState::update_frame(std::string_view to,std::string_view from,MessageEditorScreen& previous) const {
    if(!full_screen_)return {};
    const auto safe=[](std::string_view s){std::string r;for(unsigned char c:s)r.push_back(c<32 || c==127?' ':static_cast<char>(c));return r;};
    const auto position=[](std::size_t row,std::size_t column){return "\x1b["+std::to_string(row)+";"+std::to_string(column)+"H";};
    // Disable last-column autowrap only during fixed frame emission; restore it
    // before handing terminal ownership back to the lifetime/input adapter.
    // DECBKM reset makes SyncTERM Delete send CSI 3 ~ and Backspace DEL.
    // Keep DEL as Backspace for Unix-style clients; do not globally remap it.
    std::vector<std::string> screen(rows_);
    const auto line=[&](std::size_t row,std::string text){if(text.size()>80)text.resize(80);text.resize(80,' ');screen[row-1]="\x1b[0;37;40m"+text;};
    const auto header=[&](unsigned row,std::string label,std::string value){
        value.resize(std::min<std::size_t>(value.size(),74));
        value.resize(74,' ');screen[row-1]="\x1b[1;37;40m"+label+"\x1b[1;30;40m:\x1b[0;37;40m "+value;
    };
    header(1,"Subj",safe(draft_.subject()));header(2,"To  ",safe(to));header(3,"From",safe(from));
    for(unsigned column=0;column<80;++column){
        const bool tick=column==0 || column==16 || column==34 || column==52 || column==70;
        const bool block=column==8 || column==25 || column==43 || column==61 || column==79;
        screen[3]+=tick || block?"\x1b[1;30;40m":"\x1b[0;37;40m";
        screen[3]+=tick?'|':block?static_cast<char>(0xfe):static_cast<char>(0xfa);
    }
    for(std::size_t row=0;row<body_rows();++row){const auto source=top_+row;line(row+5,source<draft_.body().rows.size()?safe(draft_.body().rows[source]):std::string{});}
    screen[rows_-2]="\x1b[1;30;40m"+std::string(80,static_cast<char>(0xfa));
    std::string footer=inserting_?" SFXEditor [INS]":" SFXEditor [OVR]";footer.resize(67,' ');footer+="ESC for Help";line(rows_,footer);
    const auto row=cursor_row()-top_+5,column=std::min<std::size_t>(cursor_column()+1,80);
    const bool full=previous.rows.size()!=screen.size();
    std::string changed;
    const std::string plain="\x1b[0;37;40m";
    for(std::size_t i=0;i<screen.size();++i)if(full||screen[i]!=previous.rows[i]){
        if(!full&&screen[i].size()==plain.size()+80&&previous.rows[i].size()==screen[i].size()&&screen[i].rfind(plain,0)==0&&previous.rows[i].rfind(plain,0)==0){
            std::size_t first=plain.size(),last=screen[i].size();
            while(first<last&&screen[i][first]==previous.rows[i][first])++first;
            while(last>first&&screen[i][last-1]==previous.rows[i][last-1])--last;
            changed+=position(i+1,first-plain.size()+1)+plain+screen[i].substr(first,last-first);
        }else changed+=position(i+1,1)+screen[i];
    }
    if(changed.empty()&&row==previous.cursor_row&&column==previous.cursor_column)return {};
    std::string out="\x1b[?7l\x1b[?67l";
    if(full)out+="\x1b[0;37;40m\x1b[2J";
    out+=changed+position(row,column)+"\x1b[0;37;40m\x1b[?7h";
    previous.rows=std::move(screen);previous.cursor_row=row;previous.cursor_column=column;return out;
}
} // namespace sfx::application
