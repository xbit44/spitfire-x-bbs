#include "sfx/application/menu.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace sfx::application {
namespace {
using Id = MenuId;
using Action = MenuAction;
const std::array<MenuDefinition,12> definitions{{
    {Id::main,"MAIN MENU","main",Id::main,{}, {
        {'M',"Message Menu",Action::open,Id::messages},
        {'F',"File Menu",Action::open,Id::files},
        {'D',"Door Menu",Action::open,Id::doors},
        {'B',"Bulletins",Action::open,Id::bulletins},
        {'C',"Chat Menu",Action::open,Id::chat},
        {'S',"Comment to Sysop",Action::comment_sysop}, {'L',"Locate a Caller",Action::locate_caller}, {'#',"List all Callers",Action::caller_directory},
        {'W',"Who's On-line",Action::online}, {'P',"Page Sysop",Action::page_sysop},
        {'Y',"Your Info / Settings",Action::open,Id::settings},
        {'N',"BBS Newsletter"}, {'?',"System Help",Action::help},
        {'O',"Other Bulletin Boards",Action::other_boards}, {'I',"This System's Info",Action::system_information},
        {'G',"Goodbye / Logoff",Action::goodbye},
        {'/',"Quick Goodbye / Logoff",Action::quick_goodbye},
        {'@',"Sysop Utilities",Action::sysop_utilities,Id::main,true}}},
    {Id::messages,"MESSAGE MENU","msg",Id::main,{}, {
        {'L',"QWK / Offline Mail",Action::open,Id::qwk}, {'C',"Change Conference",Action::message_change}, {'R',"Read Messages",Action::message_read},
        {'B',"Browse Messages",Action::message_browse}, {'S',"Specific Caller Messages",Action::message_addressed}, {'E',"Enter a Message",Action::message_enter},
        {'Y',"Your Message Scan",Action::message_personal},
        {'A',"Message Scan Settings",Action::message_settings}, {'T',"Search Messages",Action::message_search},
        {'F',"File Menu",Action::open,Id::files}, {'W',"Who's On-line",Action::online},
        {'?',"Help",Action::help}, {'Q',"Main Menu",Action::open,Id::main},
        {'G',"Goodbye / Logoff",Action::goodbye},
        {'/',"Quick Goodbye / Logoff",Action::quick_goodbye}}},
    {Id::files,"FILE MENU","file",Id::main,{}, {
        {'C',"Change File Area",Action::file_change}, {'L',"List Files",Action::file_list},
        {'N',"New File Scan",Action::file_new}, {'F',"Find Filename",Action::file_find}, {'T',"Search Descriptions",Action::file_search},
        {'R',"Read Text File",Action::file_read}, {'V',"View Archive",Action::file_archive}, {'D',"Download Files",Action::file_download_add},
        {'B',"Batch Transfer Menu",Action::open,Id::batch_transfer},
        {'U',"Upload Files",Action::file_upload_add},
        {'S',"Upload to Sysop",Action::file_private},
        {'W',"Who's On-line",Action::online}, {'?',"Help",Action::help},
        {'M',"Message Menu",Action::open,Id::messages}, {'Q',"Main Menu",Action::open,Id::main},
        {'G',"Goodbye / Logoff",Action::goodbye},
        {'/',"Quick Goodbye / Logoff",Action::quick_goodbye}}},
    {Id::doors,"DOOR MENU","door",Id::main,"No Doors are configured.", {
        {'X',"Door Bulletin Menu",Action::open,Id::door_bulletins},
        {'Q',"Main Menu",Action::open,Id::main}, {'G',"Goodbye / Logoff",Action::goodbye},
        {'/',"Quick Goodbye / Logoff",Action::quick_goodbye}}},
    {Id::bulletins,"BULLETIN MENU","bulletin",Id::main,"No Bulletins are configured.", {
        {'N',"New Bulletins"}, {'L',"List Bulletins"},
        {'Q',"Main Menu",Action::open,Id::main}}},
    {Id::door_bulletins,"DOOR BULLETIN MENU","dbul",Id::doors,"No Door Bulletins are configured.", {
        {'N',"Next Page"}, {'P',"Previous Page"},
        {'Q',"Return to Door Menu",Action::open,Id::doors}}},
    {Id::chat,"CHAT MENU","chat",Id::main,{}, {
        {'E',"Enter Chat Room",Action::chat_room}, {'L',"List / Change Chat Rooms",Action::chat_rooms}, {'W',"Who's On-line",Action::online},
        {'T',"Private Online Text",Action::chat_text},
        {'?',"System Help",Action::help}, {'Q',"Main Menu",Action::open,Id::main},
        {'G',"Goodbye / Logoff",Action::goodbye},
        {'/',"Quick Goodbye / Logoff",Action::quick_goodbye}}},
    {Id::settings,"YOUR INFO / SETTINGS MENU","your",Id::main,{}, {
        {'P',"Change Password",Action::password}, {'G',"Graphics",Action::graphics},
        {'F',"File Transfer",Action::transfer_preference}, {'H',"Hot-Key Mode",Action::hotkey}, {'S',"Scan Preferences",Action::file_settings},
        {'C',"Change Color",Action::color}, {'N',"Do Not Disturb",Action::chat_dnd},
        {'L',"Logon / Logoff Notices",Action::chat_notices}, {'E',"Edit Caller Profile",Action::caller_profile},
        {'?',"System Help",Action::help}, {'Q',"Main Menu",Action::open,Id::main}}},
    {Id::file_groups,"FILE GROUPS","fgrp",Id::files,"No File Groups are configured.", {
        {'Q',"Return to File Menu",Action::open,Id::files}}},
    {Id::file_areas,"FILE AREAS","farea",Id::file_groups,"No File Areas are configured.", {
        {'Q',"Return to File Groups",Action::open,Id::file_groups}}},
    {Id::batch_transfer,"BATCH TRANSFER MENU","batch",Id::files,{}, {
        {'A',"Add files to a queue",Action::file_both_add},
        {'U',"Upload files in upload queue",Action::file_upload_begin}, {'D',"Download files in download queue",Action::file_download_begin},
        {'L',"List files in batch queues",Action::file_both_view}, {'R',"Remove files from batch queues",Action::file_both_remove},
        {'C',"Clear all batch queues",Action::file_both_clear}, {'Q',"Return to File Menu",Action::open,Id::files},
        {'?',"Help",Action::help}}},
    {Id::qwk,"QWK / OFFLINE MAIL MENU","qwk",Id::messages,{}, {
        {'D',"Download QWK Packet",Action::qwk_download},
        {'U',"Upload REP Packet",Action::qwk_upload},
        {'P',"New-message Pointers",Action::qwk_pointers},
        {'Q',"Return to Message Menu",Action::open,Id::messages},
        {'S',"Packet Settings",Action::qwk_settings},
        {'A',"Message Scan Settings",Action::message_settings},
        {'?',"Help",Action::help}}}
}};
bool authenticated(const MenuAccess& access) {
    return access.security_level >= 1 && access.security_level <= 999;
}
std::string clean_line(std::string_view value) {
    std::string result;
    for (const unsigned char ch : value) {
        // Values are inert even if they contain @ syntax. Never interpret here.
        result.push_back(ch < 32 || ch == 127 ? ' ' : static_cast<char>(ch));
    }
    return result;
}
}

const MenuDefinition& menu_definition(MenuId id) {
    const auto at=std::find_if(definitions.begin(),definitions.end(),[id](const auto& menu){return menu.id==id;});
    if(at==definitions.end())throw std::invalid_argument("unknown menu");
    return *at;
}
bool primary_door_menu(MenuId id) noexcept {
    return id==Id::main||id==Id::messages||id==Id::files||id==Id::chat||id==Id::doors;
}
bool available_door_command_key(MenuId id,char key) noexcept {
    if(!primary_door_menu(id)||key<'A'||key>'Z')return false;
    // Existing Message Menu Y shortcuts are explicitly disambiguated at launch.
    if(id==Id::messages && key=='Y')return true;
    for(const auto& command:menu_definition(id).commands)if(command.key==key)return false;
    // The numbered Door browser has Back, root, paging and redraw commands.
    if(id==Id::doors&&(key=='B'||key=='D'||key=='N'||key=='P'||key=='R'))return false;
    return true;
}
MenuSnapshot menu_snapshot(MenuId id, const MenuAccess& access, std::uint64_t revision) {
    MenuSnapshot snapshot{id,access.revision,revision,{}};
    for (const auto& command : menu_definition(id).commands)
        if (authenticated(access) && (!command.sysop_only || access.sysop_authorized))
            snapshot.commands.push_back(command);
    return snapshot;
}
MenuRoute route_menu(const MenuSnapshot& shown, unsigned char key, const MenuAccess& current,
                     std::uint64_t revision) {
    if (!authenticated(current)) return {MenuRouteCode::denied};
    if (key >= 'a' && key <= 'z') key = static_cast<unsigned char>(key-'a'+'A');
    const auto& definition = menu_definition(shown.menu);
    // Reauthorize the command before considering stale display state. Neither a
    // stale Sysop frame nor attacker-supplied artwork grants SY authority.
    if (key == '@' && shown.menu == Id::main && !current.sysop_authorized)
        return {MenuRouteCode::denied};
    if (shown.access_revision != current.revision || shown.configuration_revision != revision)
        return {MenuRouteCode::refresh};
    if (key == '\r' || key == '\n') {
        if (shown.menu == Id::doors)
            return {MenuRouteCode::command,Action::open,Id::main};
        return {MenuRouteCode::redraw};
    }
    if (key == 27) return shown.menu == Id::main ? MenuRoute{MenuRouteCode::redraw}
        : MenuRoute{MenuRouteCode::command,Action::open,definition.back};
    for (const auto& command : menu_snapshot(shown.menu,current,revision).commands) {
        if (key != static_cast<unsigned char>(command.key)) continue;
        const auto previous = std::find_if(shown.commands.begin(),shown.commands.end(),
            [&](const MenuCommand& old) { return old.key == command.key && old.action == command.action &&
                old.destination == command.destination; });
        if (previous == shown.commands.end()) return {MenuRouteCode::refresh};
        return {MenuRouteCode::command,command.action,
            command.action == Action::open ? command.destination : shown.menu};
    }
    return {MenuRouteCode::invalid};
}
std::string menu_prompt(const MenuSnapshot& snapshot) {
    std::string keys;
    for (const auto& command : snapshot.commands) {
        if(!keys.empty()) keys.push_back(' ');
        if(command.action==Action::quick_goodbye) keys+="/G";
        else keys.push_back(command.key);
    }
    return std::string(menu_definition(snapshot.menu).title)+" - ["+keys+"]? ";
}
std::vector<MenuTextRow> builtin_menu_rows(const MenuSnapshot& snapshot,
    std::string_view date_time_line, std::string_view session_time_line,
    std::string_view event_line, unsigned columns) {
    const auto& definition = menu_definition(snapshot.menu);
    columns = std::clamp(columns,40U,200U);
    std::vector<MenuTextRow> rows{{MenuTextRole::heading,
        ">>>>>>>> "+std::string(definition.title)+" <<<<<<<<"}};
    rows.push_back({MenuTextRole::heading,{}}); // removed with native heading when custom artwork is used
    if (!definition.empty_notice.empty())
        rows.push_back({MenuTextRole::status,std::string(definition.empty_notice)});
    std::vector<std::string> commands;
    for (const auto& command : snapshot.commands)
        commands.push_back(std::string("[")+(command.action==Action::quick_goodbye?std::string("/G"):std::string(1,command.key)+":")+">......... "+std::string(command.label));
    const std::size_t left_count = (commands.size()+1)/2;
    const std::size_t width = columns/2;
    const bool two_columns = columns >= 80 && std::all_of(commands.begin(),commands.end(),
        [&](const std::string& text) { return text.size()+2 <= width; });
    for (std::size_t i=0; i<(two_columns ? left_count : commands.size()); ++i) {
        auto row = commands[i];
        if (two_columns && i+left_count < commands.size()) {
            row.append(width-row.size(),' '); row += commands[i+left_count];
        }
        rows.push_back({MenuTextRole::command,std::move(row)});
    }
    rows.push_back({MenuTextRole::status,{}}); // exactly one separator before live information
    if (!event_line.empty()) {
        const auto text=clean_line(event_line);
        std::string_view remaining=text;
        const auto limit=columns-1U;
        while(!remaining.empty()){
            auto take=std::min<std::size_t>(limit,remaining.size());
            if(remaining.size()>limit){
                const auto space=remaining.rfind(' ',limit);
                if(space!=remaining.npos && space>0)take=space;
            }
            rows.push_back({MenuTextRole::event,std::string(remaining.substr(0,take))});
            remaining.remove_prefix(take);
            if(!remaining.empty()&&remaining.front()==' ')remaining.remove_prefix(1);
        }
    }
    rows.push_back({MenuTextRole::status,clean_line(date_time_line),true});
    rows.push_back({MenuTextRole::status,clean_line(session_time_line),true});
    rows.push_back({MenuTextRole::prompt,menu_prompt(snapshot),true});
    for(auto& row:rows) if(row.text.size()>columns) {
        row.text.resize(columns-3); row.text+="...";
    }
    return rows;
}
const std::array<MenuColor,16>& menu_colors() noexcept {
    static constexpr std::array<MenuColor,16> palette{{
        {0,"Use BBS default"},{1,"Blue"},{2,"Green"},{3,"Cyan"},{4,"Red"},
        {5,"Magenta"},{6,"Brown"},{7,"Light Gray"},{8,"Dark Gray"},{9,"Light Blue"},
        {10,"Light Green"},{11,"Light Cyan"},{12,"Light Red"},{13,"Light Magenta"},
        {14,"Yellow"},{15,"White"}}};
    return palette;
}
bool valid_menu_preferences(const MenuPreferences& preferences) noexcept { return preferences.color <= 15; }
std::string menu_text_sgr(std::uint8_t foreground, std::uint8_t background) {
    if (foreground > 15 || background > 7) return {};
    constexpr std::array<unsigned,8> ansi{{0,4,2,6,1,5,3,7}};
    return "\x1b[0;" + std::string(foreground >= 8 ? "1;" : "") +
        std::to_string(30U+ansi[foreground&7U]) + ";" + std::to_string(40U+ansi[background]) + "m";
}
std::string system_menu_text(std::string_view text,bool ansi) {
    if(!ansi)return std::string(text);
    const auto gray=menu_text_sgr(7),white=menu_text_sgr(15);
    std::string result=gray;
    for(std::size_t i=0;i<text.size();){
        if((text[i]=='>' || text[i]=='<') && i+2<text.size() && text[i+1]==text[i] && text[i+2]==text[i]){
            auto end=i+3;
            while(end<text.size() && text[end]==text[i])++end;
            result+=menu_text_sgr(8);result+=text.substr(i,end-i);result+=gray;
            i=end;continue;
        }
        const char open=text[i],close=open=='['?']':open=='<'?'>':0;
        auto end=close?text.find(close,i+1):std::string_view::npos;
        // Native fallback menus use [K:> and [/G> markers.
        if(open=='[' && i+2<text.size() && text[i+2]==':')end=i+2;
        if(open=='[' && i+3<text.size() && text.substr(i,4)=="[/G>")end=i+3;
        bool keys=end!=std::string_view::npos && end>i+1 && end-i<=64;
        const auto body=keys?text.substr(i+1,end-i-1):std::string_view{};
        const bool enter=body=="ENT"||body=="ENTER"||body==" ENTER ";
        if(keys&&!enter){
            bool previous=false;
            for(const char c:body){
                const bool key=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='?'||c=='-'||c=='@'||c=='#';
                if((key&&previous)||(!key&&c!=' '&&c!='/')){keys=false;break;}
                previous=key;
            }
        }
        if(keys){
            result+=open;
            for(const char c:body){if(c==' '||c=='/')result+=c;else{result+=white;result+=c;result+=gray;}}
            result+=text[end];i=end+1;
        }else result+=text[i++];
    }
    return result;
}
static std::string footer_command_text(std::string_view text) {
    const auto gray=menu_text_sgr(7), white=menu_text_sgr(15);
    std::string result=gray;
    const auto divider=text.find(" - [");
    if(divider==text.npos){result+=text;return result;}
    const auto keys=divider+4;
    const auto end=text.find(']',keys);
    result+=text.substr(0,divider);
    result+=menu_text_sgr(8)+" - "+white+"["+gray;
    result+=text.substr(keys,end==text.npos?text.npos:end-keys);
    if(end!=text.npos){result+=white+"]"+gray;result+=text.substr(end+1);}
    return result;
}
std::string render_menu_rows(const std::vector<MenuTextRow>& rows, bool ansi,
    const MenuPreferences& preferences, std::uint8_t board_color) {
    if (!valid_menu_preferences(preferences) || board_color == 0 || board_color > 15) return {};
    std::string result;
    const auto caller_color = preferences.color ? preferences.color : board_color;
    for (const auto& row : rows) {
        const auto text=clean_line(row.text);
        const bool system=!row.main_footer && (row.role==MenuTextRole::heading||row.role==MenuTextRole::command||row.role==MenuTextRole::prompt||row.role==MenuTextRole::feedback);
        if(row.main_footer&&row.role==MenuTextRole::prompt)result+=ansi?footer_command_text(text):text;
        else if(system)result+=system_menu_text(text,ansi);
        else {
            if(ansi)result+=menu_text_sgr(row.main_footer?caller_color:row.role==MenuTextRole::notification?std::uint8_t{15}:row.role==MenuTextRole::inactivity_warning?std::uint8_t{12}:row.role==MenuTextRole::event?std::uint8_t{14}:std::uint8_t{7});
            result+=text;
        }
        if (row.role != MenuTextRole::prompt) result += "\r\n";
    }
    if (ansi) result += "\x1b[0m";
    return result;
}

MenuInputDecoder::MenuInputDecoder(bool hotkey, bool numbered, bool quick_goodbye, bool arrow_activate) noexcept
    : hotkey_(hotkey), numbered_(numbered), quick_goodbye_(quick_goodbye),arrow_activate_(arrow_activate) {}
void MenuInputDecoder::discard_pending() noexcept {
    text_.clear(); invalid_=false; escape_=0; discard_enter_=false; discard_lf_=true;
}
void MenuInputDecoder::completed_action() noexcept { discard_enter_=false; }
void MenuInputDecoder::reset(bool hotkey, bool numbered, bool quick_goodbye, bool arrow_activate) noexcept {
    hotkey_=hotkey; numbered_=numbered; quick_goodbye_=quick_goodbye;arrow_activate_=arrow_activate; text_.clear(); invalid_=false; escape_=0;
    numeric_choices_.clear();
    // Preserve coalesced CR/LF suppression across redraws and navigation.
}
void MenuInputDecoder::numeric_choices(std::vector<unsigned> choices) {
    numeric_choices_=std::move(choices);
}
MenuInput MenuInputDecoder::idle() {
    if (escape_ == 1) { escape_=0; text_.clear(); invalid_=false; return {MenuInputKind::back}; }
    return {};
}
std::string menu_input_echo(unsigned char byte,std::string_view before,
    MenuInput decoded,std::string_view after) {
    if(decoded.kind==MenuInputKind::pending) {
        if(before==after)return {};
        return after.size()<before.size()?std::string("\b \b"):std::string(after.substr(before.size()));
    }
    std::string echo;
    if(byte>=32&&byte<=126) {
        if(decoded.kind==MenuInputKind::command)
            echo+=static_cast<char>(decoded.value);
        else if(decoded.kind==MenuInputKind::quick_goodbye)echo+='G';
        else if(decoded.kind==MenuInputKind::number&&byte>='0'&&byte<='9')
            echo+=static_cast<char>(byte);
    }
    if(decoded.kind==MenuInputKind::command||decoded.kind==MenuInputKind::quick_goodbye||
       decoded.kind==MenuInputKind::number||decoded.kind==MenuInputKind::redraw)echo+="\r\n";
    return echo;
}
MenuInput MenuInputDecoder::feed(unsigned char byte) {
    if (escape_) {
        if (escape_ == 1) {
            if (byte == '[' || byte == 'O') { escape_=2; return {}; }
            escape_=0; text_.clear(); invalid_=false;
            return {MenuInputKind::back}; // also discard the non-CSI Escape tail
        }
        if (byte >= 0x40U && byte <= 0x7eU) {
            const bool clean = escape_ == 2;
            escape_=0;
            if (clean && numbered_ && byte=='A') return {MenuInputKind::up};
            if (clean && numbered_ && byte=='B') return {MenuInputKind::down};
            if (clean && numbered_ && arrow_activate_ && byte=='C') return {MenuInputKind::activate};
            if (byte>='A' && byte<='D') return {}; // ignored navigation, never a command
            return {MenuInputKind::invalid};
        }
        // Bounded CSI/SS3 body, discarded until its final byte.
        escape_=3;
        return {};
    }
    if (byte=='\n' && discard_lf_) { discard_lf_=false; return {}; }
    discard_lf_=false;
    if (byte=='\r' || byte=='\n') {
        discard_lf_=byte=='\r';
        if (discard_enter_) { discard_enter_=false; return {}; }
        MenuInput result{MenuInputKind::redraw};
        if (invalid_) result.kind=MenuInputKind::invalid;
        else if (!text_.empty()) {
            if(quick_goodbye_ && text_.front()=='/') result=text_=="/G"
                ? MenuInput{MenuInputKind::quick_goodbye,'/'} : MenuInput{MenuInputKind::invalid};
            else if (numbered_ && text_.front()>='0' && text_.front()<='9') {
                unsigned number=0;
                for (char ch:text_) {
                    if (ch<'0' || ch>'9') { invalid_=true; break; }
                    number=number*10U+static_cast<unsigned>(ch-'0');
                }
                result={invalid_ || number==0 ? MenuInputKind::invalid : MenuInputKind::number,number};
            } else result=text_.size()==1 ? MenuInput{MenuInputKind::command,
                static_cast<unsigned char>(text_.front())} : MenuInput{MenuInputKind::invalid};
        }
        text_.clear(); invalid_=false; return result;
    }
    discard_enter_=false;
    if (byte==27) { escape_=1; return {}; }
    if (byte==8 || byte==127) {
        if (!text_.empty()) text_.pop_back();
        if (text_.empty()) invalid_=false;
        return {};
    }
    if (byte<32 || byte>126) { invalid_=true; return {}; }
    if (byte>='a' && byte<='z') byte=static_cast<unsigned char>(byte-'a'+'A');
    if(quick_goodbye_ && hotkey_ && text_=="/" && !invalid_ && byte=='G') {
        text_.clear();discard_enter_=true;return {MenuInputKind::quick_goodbye,'/'};
    }
    if(quick_goodbye_ && text_.empty() && byte=='/') {text_.push_back('/');return {};}
    if (hotkey_ && text_.empty() && !invalid_ && !(numbered_ && byte>='0' && byte<='9')) {
        discard_enter_=true;
        return {MenuInputKind::command,byte};
    }
    if (text_.size()>=3) invalid_=true;
    else text_.push_back(static_cast<char>(byte));
    if(hotkey_ && numbered_ && !invalid_ && !text_.empty() &&
        std::all_of(text_.begin(),text_.end(),[](char c){return c>='0'&&c<='9';})) {
        unsigned complete{};bool longer{};
        for(const auto number:numeric_choices_) {
            const auto candidate=std::to_string(number);
            if(candidate==text_)complete=number;
            else if(candidate.size()>text_.size() && candidate.compare(0,text_.size(),text_)==0)longer=true;
        }
        if(complete && !longer){text_.clear();discard_enter_=true;return {MenuInputKind::number,complete};}
    }
    return {};
}
bool valid_numbered_menu(const NumberedMenuSnapshot& snapshot) noexcept {
    if (snapshot.entries.size()>999) return false;
    for (std::size_t i=0;i<snapshot.entries.size();++i) {
        const auto& entry=snapshot.entries[i];
        if (entry.number==0 || entry.number>999 || entry.stable_id.empty() || entry.stable_id.size()>128 ||
            entry.label.empty() || entry.label.size()>200) return false;
        for (unsigned char ch:entry.label) if (ch<32 || ch==127) return false;
        for (unsigned char ch:entry.stable_id) if (ch<33 || ch>126) return false;
        for (std::size_t j=0;j<i;++j) if (snapshot.entries[j].number==entry.number ||
            snapshot.entries[j].stable_id==entry.stable_id) return false;
    }
    return true;
}
std::optional<std::string> select_numbered_menu(const NumberedMenuSnapshot& shown,
    unsigned number, const NumberedMenuSnapshot& current) {
    if (!valid_numbered_menu(shown) || !valid_numbered_menu(current) ||
        shown.access_revision != current.access_revision ||
        shown.configuration_revision != current.configuration_revision ||
        shown.entries.size() != current.entries.size()) return std::nullopt;
    std::optional<std::string> selected;
    for (std::size_t i=0;i<shown.entries.size();++i) {
        const auto& before=shown.entries[i]; const auto& now=current.entries[i];
        if (before.number!=now.number || before.stable_id!=now.stable_id || before.label!=now.label)
            return std::nullopt;
        if (before.number==number) selected=before.stable_id;
    }
    return selected;
}
} // namespace sfx::application
