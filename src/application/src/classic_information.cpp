#include "sfx/application/classic_information.hpp"
#include "sfx/application/file_presentation.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace sfx::application {
bool information_updated(const std::optional<std::int64_t>& modified,
    const std::optional<std::int64_t>& previous) noexcept {
    if(!previous) return true;
    if(!modified || *previous<0 || *previous>std::numeric_limits<std::int64_t>::max()/10000000) return false;
    return *modified>*previous*10000000;
}
namespace {
TrustedDisplaySelectionRequest request(TrustedDisplayIdentity identity,const MenuSessionSnapshot& state,unsigned number=1) {
    TrustedDisplaySelectionRequest r;r.identity=identity;r.graphics=state.graphics;r.context=state.context;
    r.bulletin_number=static_cast<std::uint8_t>(number);
    if(identity==TrustedDisplayIdentity::menu_bulletins) r.authenticated_security_level=state.access.security_level;
    return r;
}
bool rows(const MenuSessionPorts& ports,const MenuSessionSnapshot& state,std::vector<MenuTextRow> lines) {
    return ports.write(render_menu_rows(lines,state.graphics==GraphicsMode::ansi,state.preferences.preferences,state.default_color));
}
bool paged(const MenuSessionPorts& ports,const MenuSessionSnapshot& state,const std::vector<std::string>& lines,bool acknowledge,bool heading=false) {
    TerminalDisplayProgram program;
    if(state.graphics==GraphicsMode::ansi) {
        const auto sgr=menu_text_sgr(7);
        program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,sgr});
        program.output_byte_count+=sgr.size();program.paging_prompt_color=std::uint8_t{7};
    }
    std::vector<std::string> wrapped;
    const auto width=std::clamp<unsigned>(state.columns,40,200)-1;
    const auto heading_rows=lines.empty()?0U:std::max<std::size_t>(1,(lines.front().size()+width-1)/width);
    for(const auto& line:lines) {
        if(line.empty())wrapped.emplace_back();
        for(std::size_t start=0;start<line.size();start+=width)wrapped.push_back(line.substr(start,width));
    }
    for(std::size_t n=0;n<wrapped.size();++n) {
        if(heading && n==heading_rows && state.graphics==GraphicsMode::ansi) {
            const auto sgr=menu_text_sgr(7);
            program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,sgr});program.output_byte_count+=sgr.size();
        }
        const auto& row=wrapped[n];
        const auto first=row.find_first_not_of(' ');
        const auto opening=first==row.npos?row.npos:row.find_first_not_of('>',first);
        const auto closing=row.find_last_not_of('<');
        if(n==0 && state.graphics==GraphicsMode::ansi && first!=row.npos && opening!=row.npos &&
            opening-first>=3 && closing!=row.npos && row.size()-closing>3 && opening<=closing){
            const auto style=[&](std::uint8_t color){
                auto sgr=menu_text_sgr(color);program.output_byte_count+=sgr.size();
                program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,std::move(sgr)});
            };
            style(8);
            program.operations.push_back({TerminalDisplayOperationKind::cp437_text,row.substr(0,opening)});
            style(7);
            program.operations.push_back({TerminalDisplayOperationKind::cp437_text,row.substr(opening,closing+1-opening)});
            style(8);
            program.operations.push_back({TerminalDisplayOperationKind::cp437_text,row.substr(closing+1)});
            style(7);
        }else if(!row.empty())program.operations.push_back({TerminalDisplayOperationKind::cp437_text,row});
        program.operations.push_back({n+1==wrapped.size()?TerminalDisplayOperationKind::logical_newline:TerminalDisplayOperationKind::paging_newline,{}});
        program.output_byte_count+=wrapped[n].size()+2;
    }
    return (!acknowledge || prepare_acknowledged_display(program,state.graphics,std::uint8_t{7})) && ports.display(program);
}
std::vector<std::string> numbers(const std::vector<BulletinEntry>& entries,std::optional<std::int64_t> previous,bool only_new) {
    std::vector<std::string> result;
    std::string line;
    for(const auto& entry:entries) if(!only_new || information_updated(entry.modified_utc_100ns,previous)) {
        const auto next="#"+std::to_string(entry.number);
        if(line.size()+next.size()+3>72) {result.push_back(line);line.clear();}
        if(!line.empty()) line+="... ";
        line+=next;
    }
    if(!line.empty()) result.push_back(line);
    return result;
}
bool other_board_page(const MenuSessionPorts& ports,const MenuSessionSnapshot& state,
    const OtherBoardPage& page,bool numbered=false) {
    TerminalDisplayProgram program;
    const bool ansi=state.graphics==GraphicsMode::ansi;
    const auto width=std::clamp<unsigned>(state.columns,40,200)-1;
    unsigned column=0;
    const auto style=[&](std::uint8_t foreground) {
        if(!ansi)return;
        auto sgr=menu_text_sgr(foreground);program.output_byte_count+=sgr.size();
        program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,std::move(sgr)});
    };
    const auto newline=[&] {
        program.operations.push_back({TerminalDisplayOperationKind::paging_newline,{}});
        program.output_byte_count+=2;column=0;
    };
    const auto text=[&](const std::string& value) {
        for(std::size_t at=0;at<value.size();) {
            if(column==width)newline();
            const auto count=std::min<std::size_t>(width-column,value.size()-at);
            program.operations.push_back({TerminalDisplayOperationKind::cp437_text,value.substr(at,count)});
            program.output_byte_count+=count;column+=static_cast<unsigned>(count);at+=count;
        }
    };
    program.paging_prompt_color=std::uint8_t{7};
    style(8);text(">>>>>>>> ");style(7);
    text(numbered?"DELETE OTHER BBS ENTRY":"OTHER BULLETIN BOARDS");
    style(8);text(" <<<<<<<<");newline();newline();
    for(const auto& entry:page.entries) {
        style(15);text((numbered?std::to_string(entry.id)+". ":std::string{})+entry.values.name);newline();
        style(7);
        if(!entry.values.telnet.empty()){text("Telnet: "+entry.values.telnet);newline();}
        if(!entry.values.ssh.empty()){text("SSH:    "+entry.values.ssh);newline();}
        newline();
    }
    style(7);
    if(page.entries.empty()){text("No other BBS entries are listed.");newline();}
    return ports.write("\r\n") && ports.display(program);
}
}
std::vector<BulletinEntry> available_bulletins(const MenuSessionPorts& ports,const MenuSessionSnapshot& state) {
    std::vector<BulletinEntry> entries;
    for(unsigned n=1;n<=99;++n) {
        const auto selected=ports.artwork(request(TrustedDisplayIdentity::bulletin_page,state,n));
        if(selected.available()) entries.push_back({n,selected.program->modified_utc_100ns});
    }
    return entries;
}
BulletinOutcome run_bulletin_session(const MenuSessionPorts& ports,std::optional<std::int64_t> previous,bool skip_if_absent) {
    MenuInputDecoder input;
    std::string feedback;
    for(;;) {
        const auto state=ports.snapshot();
        if(!state || state->access.security_level<1 || state->access.security_level>999) return BulletinOutcome::failed;
        const auto pages=available_bulletins(ports,*state);
        const auto menu=ports.artwork(request(TrustedDisplayIdentity::menu_bulletins,*state));
        if(skip_if_absent && pages.empty() && !menu.available()) return BulletinOutcome::continued;
        skip_if_absent=false;
        input.reset(state->preferences.preferences.hotkey,true);
        std::vector<unsigned> choices;
        for(const auto& page:pages)choices.push_back(page.number);
        input.numeric_choices(std::move(choices));
        const auto draw=[&] {
            if(!ports.write(state->graphics==GraphicsMode::ansi?"\x1b[0m\x1b[2J\x1b[H":"\r\n")) return false;
            if(menu.available()) {
                auto program=menu.program->terminal_program;
                // Menu artwork is one uninterrupted surface. Content pages
                // retain their own MORE and completion operations below.
                for(auto& operation:program.operations)
                    if(operation.kind==TerminalDisplayOperationKind::paging_newline)
                        operation.kind=TerminalDisplayOperationKind::logical_newline;
                unsigned trailing=0;
                for(auto it=program.operations.rbegin();it!=program.operations.rend();++it) {
                    if(it->kind==TerminalDisplayOperationKind::ansi_sequence)continue;
                    if(it->kind==TerminalDisplayOperationKind::paging_newline || it->kind==TerminalDisplayOperationKind::logical_newline)++trailing;
                    else break;
                }
                if(!ports.write(state->graphics==GraphicsMode::ansi?menu_text_sgr(7):std::string{}) ||
                    !ports.display(program)) return false;
                for(unsigned n=trailing;n<2;++n)if(!ports.write("\r\n"))return false;
            } else {
                std::vector<std::string> lines{"  >>>>>>>> BULLETIN MENU <<<<<<<<",""};
                for(const auto& page:pages) lines.push_back("["+std::to_string(page.number)+(page.number<10?":>":">")+"......... Bulletin "+std::to_string(page.number));
                if(pages.empty()) lines.push_back("No bulletins are available at this time.");
                if(!paged(ports,*state,lines,false,true)) return false;
            }
            if(!menu.available() && !ports.write("\r\n"))return false;
            auto updates=numbers(pages,previous,true);
            if(!updates.empty()) {
                updates.front()="been updated since your last log-on... "+updates.front();
                updates.insert(updates.begin(),state->context.name.value_or("Caller")+", the following bulletin(s) have");
                updates.push_back("");
                std::vector<MenuTextRow> notification_rows;
                for(const auto& update:updates)notification_rows.push_back({MenuTextRole::feedback,update});
                if(!rows(ports,*state,std::move(notification_rows))) return false;
            }
            if(!feedback.empty() && !rows(ports,*state,{{MenuTextRole::feedback,feedback}})) return false;
            if(ports.notifications && !rows(ports,*state,ports.notifications()))return false;
            return rows(ports,*state,{{MenuTextRole::prompt,
                "Bulletin <#>, <L>ist Bulletins, <N>ew Bulletins, < ENTER > To Continue? ",true}}) &&
                ports.write(state->graphics==GraphicsMode::ansi?menu_text_sgr(state->input_color):std::string{});
        };
        if(!draw()) return BulletinOutcome::failed;
        const auto choice=ports.read(input,draw);
        if(!choice) return BulletinOutcome::failed;
        const auto current=ports.snapshot();if(!current) return BulletinOutcome::failed;
        if(choice->kind==MenuInputKind::back || choice->kind==MenuInputKind::redraw ||
            (choice->kind==MenuInputKind::command && choice->value=='Q')) return BulletinOutcome::continued;
        // Bulletin pages acknowledge through display(), and invalid/list choices
        // finish here. Their next menu Enter is a new action, not a hotkey tail.
        input.completed_action();
        if(choice->kind==MenuInputKind::command && choice->value=='L') {
            feedback.clear();continue; // Reload through the normal menu draw path.
        }
        if(choice->kind==MenuInputKind::command && choice->value=='N') {
            const auto fresh=available_bulletins(ports,*current);
            auto listing=numbers(fresh,previous,true);
            listing.insert(listing.begin(),"NEW BULLETINS");
            if(listing.size()==1) listing.push_back("No new bulletins.");
            listing.insert(listing.begin()+1,"");
            if(!paged(ports,*current,listing,true,true)) return BulletinOutcome::failed;
            feedback.clear();continue;
        }
        if(choice->kind==MenuInputKind::number && choice->value>=1 && choice->value<=99) {
            auto selected=ports.artwork(request(TrustedDisplayIdentity::bulletin_page,*current,choice->value));
            if(selected.available() && prepare_acknowledged_display(selected.program->terminal_program,current->graphics,std::uint8_t{7},true)) {
                if(!ports.display(selected.program->terminal_program)) return BulletinOutcome::failed;
                feedback.clear();
            } else feedback="Bulletin "+std::to_string(choice->value)+" is unavailable at this time.";
        } else feedback="Choose a bulletin number, L, N, or Enter.";
    }
}
bool show_login_door_bulletin_updates(const MenuSessionPorts& ports){
    if(!ports.previous_login_utc||!ports.door_bulletin_updates)return true;
    const auto notices=ports.door_bulletin_updates(*ports.previous_login_utc);if(!notices)return false;if(notices->empty())return true;
    const auto state=ports.snapshot();if(!state)return false;
    std::vector<std::string> lines{"Door Bulletins updated since your last log-on:",""};
    for(const auto& item:*notices){lines.push_back("["+std::to_string(item.id.value)+"] "+file_terminal_text(item.title,64));}
    lines.push_back("");
    return paged(ports,*state,lines,false,true);
}
bool offer_login_newsletter(const MenuSessionPorts& ports,std::optional<std::int64_t> previous) {
    const auto state=ports.snapshot();if(!state) return false;
    auto newsletter=ports.artwork(request(TrustedDisplayIdentity::sfnewsletter,*state));
    if(!newsletter.available() || !information_updated(newsletter.program->modified_utc_100ns,previous)) return true;
    MenuInputDecoder input(true,false);
    const auto draw=[&] {
        auto notices=ports.notifications?ports.notifications():std::vector<MenuTextRow>{};
        notices.push_back({MenuTextRole::feedback,"The newsletter has been updated!"});
        notices.push_back({MenuTextRole::feedback,"Would you like to review it? [Y/n]"});
        return ports.write("\r\n\r\n") && rows(ports,*state,notices);
    };
    for(;;) {
        if(!draw()) return false;
        const auto choice=ports.read(input,draw);if(!choice) return false;
        if(choice->kind==MenuInputKind::back || (choice->kind==MenuInputKind::command && choice->value=='N')) return true;
        if(choice->kind!=MenuInputKind::redraw && !(choice->kind==MenuInputKind::command && choice->value=='Y')) continue;
        const auto current=ports.snapshot();if(!current) return false;
        newsletter=ports.artwork(request(TrustedDisplayIdentity::sfnewsletter,*current));
        if(!newsletter.available()) return rows(ports,*current,{{MenuTextRole::feedback,"Newsletter is unavailable at this time. Please try again later."}});
        return prepare_acknowledged_display(newsletter.program->terminal_program,current->graphics,std::uint8_t{7}) &&
            ports.display(newsletter.program->terminal_program);
    }
}
std::optional<std::string> run_other_board_session(const MenuSessionPorts& ports,ICallerTerminal& terminal,
    const std::function<StorageResult<OtherBoardPage>(std::int64_t)>& query,
    const std::function<bool(const OtherBoardValues&)>& add,
    const std::function<bool(const OtherBoardRecord&)>& remove) {
    std::int64_t after=0;std::vector<std::int64_t> previous;
    for(;;) {
        const auto state=ports.snapshot();if(!state)return {};
        const auto page=query(after);if(!page || !page.value)return
            page.status.operation=="otherbbs_text_invalid_check_Config_otherbbs_txt"?
                "Other BBS list is invalid. Please ask the Sysop to check Config/otherbbs.txt.":
                "Other Bulletin Boards are unavailable. Please try again later.";
        if(!other_board_page(ports,*state,*page.value))return {};
        const bool may_delete=remove && state->access.security_level==999 && state->access.sysop_authorized;
        auto keys=std::vector<char>{'A','a','N','n','P','p','Q','q'};
        if(may_delete){keys.push_back('D');keys.push_back('d');}
        if(!ports.write(system_menu_text(may_delete?"[A]dd [D]elete [N]ext [P]revious [Q]uit:":"[A]dd [N]ext [P]revious [Q]uit:",state->graphics==GraphicsMode::ansi)))return {};
        const auto choice=terminal.read_key({"other.boards.choice"," ",keys,'Q'});
        if(!choice.has_key())return {};
        const auto key=*choice.key;
        if(key=='Q'||key=='q')return std::string{};
        if(key=='P'||key=='p') {if(!previous.empty()){after=previous.back();previous.pop_back();}continue;}
        if(key=='N'||key=='n') {
            if(page.value->has_more&&!page.value->entries.empty()){previous.push_back(after);after=page.value->entries.back().id;}continue;
        }
        if(key=='D'||key=='d') {
            const auto authorized=[&] {
                const auto current=ports.snapshot();return may_delete && current && current->access.security_level==999 &&
                    current->access.sysop_authorized && current->access.revision==state->access.revision;
            };
            if(!authorized())return "Entry not deleted. Sysop authority changed or is unavailable.";
            // Keep the public listing unnumbered. The Sysop sees stable entry
            // identifiers only when choosing an entry for confirmed deletion.
            if(!other_board_page(ports,*state,*page.value,true))return {};
            const auto number=terminal.read_visible({"other.boards.delete.number","Entry number to delete (Enter cancels): ",5,EmptyInputPolicy::use_default});
            if(number.outcome==TerminalOutcome::cancelled || number.outcome==TerminalOutcome::empty)continue;
            if(!number.has_value())return {};
            std::int64_t id{};const auto parsed=std::from_chars(number.value.data(),number.value.data()+number.value.size(),id);
            if(parsed.ec!=std::errc{} || parsed.ptr!=number.value.data()+number.value.size() || id<1 || id>99999)
                return "Entry not deleted. Choose a listed entry number.";
            std::optional<OtherBoardRecord> selected;
            for(const auto& row:page.value->entries)if(row.id==id)selected=row;
            if(!selected) {
                const auto found=query(id-1);
                if(!found || !found.value)return "Entry not deleted. Storage is unavailable.";
                if(!found.value->entries.empty() && found.value->entries.front().id==id)selected=found.value->entries.front();
            }
            if(!selected)return "Entry not deleted. That entry is unavailable.";
            if(!authorized())return "Entry not deleted. Sysop authority changed or is unavailable.";
            const auto confirm=terminal.read_key({"other.boards.delete.confirm",
                "Delete entry "+std::to_string(id)+" ("+selected->values.name+")? [y/N] ",{'Y','y','N','n'},'N'});
            if(confirm.outcome==TerminalOutcome::cancelled)continue;
            if(!confirm.has_key())return {};
            if(*confirm.key!='Y' && *confirm.key!='y')continue;
            if(!authorized())return "Entry not deleted. Sysop authority changed or is unavailable.";
            return remove(*selected)?"Other BBS entry deleted.":"Entry not deleted. The entry/account changed or storage is unavailable.";
        }
        if(key!='A' && key!='a')continue;
        const auto name=terminal.read_visible({"other.boards.name","BBS Name: ",kOtherBoardNameMaximum,EmptyInputPolicy::reject});
        if(name.outcome==TerminalOutcome::cancelled)continue;
        if(!name.has_value())return "Entry not added. A BBS name is required.";
        const auto telnet=terminal.read_visible({"other.boards.telnet","Telnet address (optional, host[:port]): ",kOtherBoardAddressMaximum,EmptyInputPolicy::use_default});
        if(telnet.outcome==TerminalOutcome::cancelled)continue;
        if(!telnet.has_value() && telnet.outcome!=TerminalOutcome::empty)return {};
        const auto ssh=terminal.read_visible({"other.boards.ssh","SSH address (optional, host[:port]): ",kOtherBoardAddressMaximum,EmptyInputPolicy::use_default});
        if(ssh.outcome==TerminalOutcome::cancelled)continue;
        if(!ssh.has_value() && ssh.outcome!=TerminalOutcome::empty)return {};
        OtherBoardValues values{name.value,telnet.value,ssh.value};
        if(!valid_other_board_values(values))return "Entry not added. Use a name and at least one valid Telnet or SSH address.";
        return add(values)?"Other BBS entry added and published.":"Entry not added. The account changed or storage is unavailable.";
    }
}
bool show_system_information(const MenuSessionPorts& ports,const SystemInformationCounters& counters,std::string opening_date) {
    const auto state=ports.snapshot();if(!state)return false;
    const auto& context=state->context;
    return ports.write("\r\n") && paged(ports,*state,{"  >>>>>>>> SYSTEM INFORMATION <<<<<<<<","",
        "BBS Name: "+context.bbs_name.value_or("Unavailable"),
        "System: Spitfire-X BBS "+context.version.value_or("Unavailable"),
        "Sysop: "+context.sysop.value_or("Not configured"),
        "This BBS began on: "+opening_date,
        "Total calls: "+std::to_string(counters.total_calls),
        "Calls today: "+std::to_string(counters.today_calls),
        "Active callers: "+std::to_string(counters.active_accounts),
        "Current node: "+context.node.value_or("Unavailable"),""},true,true);
}
} // namespace sfx::application
