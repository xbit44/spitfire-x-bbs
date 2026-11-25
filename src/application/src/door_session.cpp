#include "sfx/application/door_session.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
namespace {
std::string key_row(std::string_view key,std::string_view label,bool ansi){
    const auto gray=ansi?menu_text_sgr(7):std::string{};const auto white=ansi?menu_text_sgr(15):std::string{};
    return gray+"["+white+std::string(key)+gray+"] "+std::string(label);
}
TrustedDisplaySelectionRequest door_art(const MenuSessionSnapshot& s,TrustedDisplayIdentity identity,std::uint16_t id){
    TrustedDisplaySelectionRequest r;r.identity=identity;r.graphics=s.graphics;r.authenticated_security_level=s.access.security_level;r.context=s.context;r.door_asset_number=id;return r;
}
std::string door_minutes(const std::optional<std::string>& seconds) {
    if(!seconds)return "unavailable";
    if(*seconds=="Unlimited")return "unlimited time";
    std::uint64_t value{};
    const auto parsed=std::from_chars(seconds->data(),seconds->data()+seconds->size(),value);
    return parsed.ec==std::errc{} && parsed.ptr==seconds->data()+seconds->size()
        ?std::to_string(value/60):"unavailable";
}
std::string door_choices(const DoorCallerMenu& shown,bool nested,bool paged) {
    std::vector<unsigned> numbers;for(const auto& e:shown.entries)numbers.push_back(e.number);
    std::sort(numbers.begin(),numbers.end());std::string result;
    for(std::size_t i=0;i<numbers.size();++i){
        auto end=i;while(end+1<numbers.size() && numbers[end+1]==numbers[end]+1)++end;
        if(!result.empty())result+=' ';
        result+=std::to_string(numbers[i]);
        if(end>=i+2){result+='-';result+=std::to_string(numbers[end]);i=end;}
    }
    const auto append=[&](const char* keys){if(!result.empty())result+=' ';result+=keys;};
    if(nested)append("B D");
    if(paged)append("P N");
    append("X Q G /G");return result;
}
}
std::optional<DoorMenuExit> run_door_session(const DoorSessionPorts& p){
    const auto& ports=p.menu;if(!ports.snapshot||!ports.write||!ports.read||!ports.artwork||!ports.display||!p.actor||!p.launch)return {};
    if(ports.menu_entered)ports.menu_entered(MenuId::doors);
    std::vector<DoorMenuDefinition> path{{0,"Door Menu"}};std::string feedback;
    MenuInputDecoder input;std::size_t page{},highlight{};bool highlighted{},confirming_logoff{};
    for(;;){
        const auto actor=p.actor();auto state=ports.snapshot();if(!actor||!state)return {};
        auto queried=p.storage.door_menu(*actor,path.back().id);
        DoorCallerMenu shown;
        if(queried&&queried.value){shown=*queried.value;path.back()=shown.menu;}
        else if(queried.status.code==StorageErrorCode::not_found){shown.menu=path.back();feedback="This menu has changed. Choose Back or Main Door Menu.";}
        else if(queried.status.code==StorageErrorCode::permission_denied){
            if(path.size()==1){if(!ports.write("You do not have security clearance to enter this category.\r\n"))return {};return DoorMenuExit::main_menu;}
            path.pop_back();feedback="You do not have security clearance to enter this category.";page=highlight=0;highlighted=false;continue;
        }else return {};
        const bool ansi=state->graphics==GraphicsMode::ansi;
        const auto commands=ports.door_commands?ports.door_commands(MenuId::doors):std::optional<MenuDoorCommands>(MenuDoorCommands{});
        if(!commands)return {};
        const bool lightbar=ansi&&state->lightbars_enabled&&state->preferences.preferences.lightbar;
        const auto per_page=std::max<unsigned>(1,std::clamp<unsigned>(state->rows,14U,50U)-13U);
        const auto item_count=shown.entries.size()+commands->commands.size();
        const auto pages=std::max<std::size_t>(1,(item_count+per_page-1)/per_page);page=std::min(page,pages-1);
        if(highlight>=shown.entries.size()){highlight=0;highlighted=false;}
        std::string breadcrumb;for(const auto& menu:path){if(!breadcrumb.empty())breadcrumb+=" > ";breadcrumb+=menu.name;}
        auto request=door_art(*state,path.size()==1?TrustedDisplayIdentity::menu_doors:TrustedDisplayIdentity::door_submenu,path.size()==1?0:path.back().id);request.context.door_menu=breadcrumb;
        auto artwork=shown.has_hidden_entries?TrustedDisplaySelectionResult{}:ports.artwork(request);
        // Unmappable custom art uses direct stable numbers. Generated menus own
        // their lightbar positions; Enter remains Back in both presentations.
        const bool native_lightbar=lightbar&&!artwork.available();
        const auto draw=[&]{
            if(confirming_logoff){
                auto rows=ports.notifications?ports.notifications():std::vector<MenuTextRow>{};
                rows.push_back({MenuTextRole::feedback,"Logoff the BBS? [Y/n]"});
                return ports.write("\r\n"+render_menu_rows(rows,ansi,state->preferences.preferences,state->default_color));
            }
            if(!ports.write(ansi?"\x1b[0m\x1b[2J\x1b[H":"\r\n"))return false;
            if(artwork.available()){
                auto program=artwork.program->terminal_program;
                for(auto& op:program.operations)if(op.kind==TerminalDisplayOperationKind::paging_newline)op.kind=TerminalDisplayOperationKind::logical_newline;
                if(!ports.display(program)||!ports.write("\r\n"))return false;
            }else{
                if(!ports.write((ansi?menu_text_sgr(7):std::string{})+breadcrumb+"\r\n\r\n"))return false;
                const auto start=page*per_page,end=std::min(shown.entries.size(),start+per_page);
                for(auto i=start;i<end;++i){const auto& e=shown.entries[i];std::string row;
                    if(native_lightbar&&highlighted&&i==highlight)row=menu_text_sgr(state->selected_foreground,state->selected_background)+"["+std::to_string(e.number)+"] "+e.title+(ansi?"\x1b[0m":"");
                    else row=key_row(std::to_string(e.number),e.title,ansi);
                    if(!ports.write(row+"\r\n"))return false;
                }
                if(shown.entries.empty()&&!ports.write("No Doors are available.\r\n"))return false;
                const auto command_begin=start>shown.entries.size()?start-shown.entries.size():0;
                const auto command_end=std::min(commands->commands.size(),page*per_page+per_page>shown.entries.size()?page*per_page+per_page-shown.entries.size():0);
                for(auto i=command_begin;i<command_end;++i){const auto& command=commands->commands[i];if(!ports.write(key_row(std::string(1,command.key),command.name,ansi)+"\r\n"))return false;}
            }
            if(!ports.write("\r\n"))return false;
            if(!feedback.empty()&&!ports.write((ansi?menu_text_sgr(7):std::string{})+feedback+"\r\n"))return false;
            if(ports.notifications&&!ports.write(render_menu_rows(ports.notifications(),ansi,state->preferences.preferences,state->default_color)))return false;
            if(pages>1&&!ports.write(key_row("P","Previous page",ansi)+"  "+key_row("N","Next page",ansi)+"  "+std::to_string(page+1)+"/"+std::to_string(pages)+"\r\n"))return false;
            if(path.size()>1&&!ports.write(key_row("B","Back to "+path[path.size()-2].name,ansi)+"  "+key_row("D","Door Menu",ansi)+"\r\n"))return false;
            const auto quit_label=path.size()>2?"Back to "+path[1].name:path.size()>1?"Door Menu":std::string("BBS Main Menu");
            if(!ports.write(key_row("X","Door Bulletin Menu",ansi)+"\r\n"))return false;
            if(!ports.write(key_row("Q",quit_label,ansi)+"  "+key_row("G","Goodbye and Logoff",ansi)+"  "+key_row("/G","Quick Goodbye",ansi)+"\r\n"))return false;
            auto command_keys=door_choices(shown,path.size()>1,pages>1);
            for(const auto& command:commands->commands)command_keys+=" "+std::string(1,command.key);
            const auto time="You have been on "+door_minutes(state->context.time_on)+" minutes with "+door_minutes(state->context.time_left)+" remaining.";
            std::vector<MenuTextRow> footer;
            if(!state->compact_footer)footer.push_back({MenuTextRole::status,time,true});
            footer.push_back({MenuTextRole::prompt,state->compact_footer?"Command: ":"ENTER COMMAND - ["+command_keys+"]? ",true});
            return ports.write(render_menu_rows(footer,ansi,state->preferences.preferences,state->default_color))&&ports.write(ansi?menu_text_sgr(state->input_color):std::string{});
        };
        input.reset(confirming_logoff||state->preferences.preferences.hotkey,!confirming_logoff,
            !confirming_logoff,native_lightbar&&!confirming_logoff);
        std::vector<unsigned> numbers;for(const auto& e:shown.entries)numbers.push_back(e.number);
        input.numeric_choices(std::move(numbers));
        if(!draw())return {};
        const auto choice=ports.read(input,draw);if(!choice)return {};
        if(confirming_logoff){
            const auto latest=ports.snapshot();if(!latest||!p.actor())return {};
            if(latest->access.security_level!=state->access.security_level||
               latest->access.sysop_authorized!=state->access.sysop_authorized||
               latest->access.revision!=state->access.revision||
               latest->configuration_revision!=state->configuration_revision||
               latest->preferences.revision!=state->preferences.revision){
                confirming_logoff=false;feedback="Settings or access changed. Please choose again.";continue;
            }
            if(choice->kind==MenuInputKind::back||(choice->kind==MenuInputKind::command&&choice->value=='N')){
                // Keep N's trailing-Enter suppression when resuming this exact
                // Door path/page/highlight; Enter must not become Back.
                confirming_logoff=false;feedback.clear();continue;
            }
            if(choice->kind==MenuInputKind::redraw||(choice->kind==MenuInputKind::command&&choice->value=='Y'))
                return DoorMenuExit::confirmed_goodbye;
            continue;
        }
        if(choice->kind==MenuInputKind::quick_goodbye)return DoorMenuExit::quick_goodbye;
        if(choice->kind==MenuInputKind::back || choice->kind==MenuInputKind::redraw){if(path.size()==1)return DoorMenuExit::main_menu;path.pop_back();page=highlight=0;highlighted=false;continue;}
        if(native_lightbar&&(choice->kind==MenuInputKind::up||choice->kind==MenuInputKind::down)){
            if(!shown.entries.empty()){if(!highlighted){highlight=page*per_page;highlighted=true;}else if(choice->kind==MenuInputKind::up)highlight=highlight?highlight-1:shown.entries.size()-1;else highlight=(highlight+1)%shown.entries.size();page=highlight/per_page;}continue;
        }
        if(choice->kind==MenuInputKind::command){
            // Reserved at every level, including custom art and an empty root.
            // Returning leaves the invoker's path, page and highlight intact.
            if(choice->value=='X'){
                if(!run_door_bulletin_session(p.storage,ports,p.actor,p.bulletin_refresh,&input))return {};
                if(ports.menu_entered)ports.menu_entered(MenuId::doors);
                continue;
            }
            const auto custom=std::find_if(commands->commands.begin(),commands->commands.end(),[&](const auto& command){return static_cast<unsigned char>(command.key)==choice->value;});
            if(custom!=commands->commands.end()&&ports.door_command){
                const auto result=ports.door_command({MenuId::doors,custom->key,commands->revision,commands->door_revision});if(!result)return {};
                feedback=*result;input.discard_pending();if(ports.menu_entered)ports.menu_entered(MenuId::doors);continue;
            }
            switch(choice->value){
            case 'Q':if(path.size()==1)return DoorMenuExit::main_menu;path.resize(path.size()>2?2:1);page=highlight=0;highlighted=false;feedback.clear();continue;
            case 'G':confirming_logoff=true;input.completed_action();continue;
            case 'B':if(path.size()==1)return DoorMenuExit::main_menu;path.pop_back();page=highlight=0;highlighted=false;continue;
            case 'D':path.resize(1);page=highlight=0;highlighted=false;continue;
            case 'N':if(page+1<pages)++page;highlighted=false;continue;
            case 'P':if(page)--page;highlighted=false;continue;
            case 'R':feedback.clear();continue;
            default:feedback="Choose a listed number or command.";continue;
        }}
        unsigned number{};
        if(choice->kind==MenuInputKind::number)number=choice->value;
        else if(native_lightbar&&choice->kind==MenuInputKind::activate&&highlighted&&highlight<shown.entries.size())number=shown.entries[highlight].number;
        if(!number||number>99){feedback="Choose a listed number or command.";continue;}
        const auto current_actor=p.actor();if(!current_actor)return {};
        const auto current=p.storage.door_menu(*current_actor,path.back().id);
        if(!current||!current.value||current.value->revision!=shown.revision){feedback="Door settings changed. Review the refreshed menu and choose again.";continue;}
        const auto found=std::find_if(current.value->entries.begin(),current.value->entries.end(),[number](const auto& e){return e.number==number;});
        if(found==current.value->entries.end()){feedback="That entry is unavailable.";continue;}
        if(found->kind==DoorEntryKind::submenu){
            if(path.size()>=3){feedback="That entry is unavailable.";continue;}
            const auto child=p.storage.door_menu(*current_actor,found->target);if(!child||!child.value){feedback=child.status.code==StorageErrorCode::permission_denied?"You do not have security clearance to enter this category.":"That menu is unavailable.";continue;}
            path.push_back(child.value->menu);page=highlight=0;highlighted=false;feedback.clear();continue;
        }
        const auto launch=p.storage.door_launch(*current_actor,{path.back().id,static_cast<std::uint8_t>(number),shown.revision});
        if(!launch||!launch.value){feedback=launch.status.code==StorageErrorCode::permission_denied?"You do not have security clearance to run this Door.":"Door settings changed or access is no longer available. Choose again.";continue;}
        state=ports.snapshot();if(!state)return {};
        auto pre=door_art(*state,TrustedDisplayIdentity::door_prelaunch,launch.value->door.id);pre.context.door_name=launch.value->door.name;pre.context.door_number=std::to_string(launch.value->door.id);pre.context.door_menu=breadcrumb;
        const auto display=ports.artwork(pre);if(display.available()){
            auto program=display.program->terminal_program;
            for(auto& op:program.operations)if(op.kind==TerminalDisplayOperationKind::paging_newline)op.kind=TerminalDisplayOperationKind::logical_newline;
            if(!ports.display(program))return {};
        }
        const auto outcome=p.launch(*launch.value);if(!outcome)return {};
        switch(*outcome){case DoorEnd::caller_disconnected:case DoorEnd::game_disconnect:case DoorEnd::shutdown:case DoorEnd::authority_lost:case DoorEnd::calling_time:case DoorEnd::inactivity:return {};default:break;}
        feedback=*outcome==DoorEnd::busy?"This Door is busy. Try again after the current caller finishes.":
            *outcome==DoorEnd::unavailable?"This Door's program support is unavailable on this host.":
            *outcome==DoorEnd::launch_failed?"The Door could not be started. You are back in "+path.back().name+".":
            *outcome==DoorEnd::returned||*outcome==DoorEnd::escaped||*outcome==DoorEnd::operator_terminated?"Returned from "+launch.value->door.name+".":"The Door session ended. You are back in "+path.back().name+".";
        // Deliberately retain the exact path/page and launching Menu ID.
    }
}
bool run_door_events(const DoorSessionPorts& ports,DoorAutomaticEvent event){
    if(event==DoorAutomaticEvent::none||!ports.actor||!ports.launch)return false;
    const auto actor=ports.actor();if(!actor)return false;const auto candidates=ports.storage.door_events(*actor,event);
    if(!candidates||!candidates.value){if(ports.event_outcome)ports.event_outcome(0,event,DoorEnd::launch_failed);return true;}
    for(const auto id:candidates.value->doors){
        const auto current=ports.actor();if(!current)return false;
        const auto launch=ports.storage.door_event_launch(*current,{id,event,candidates.value->revision});
        if(!launch||!launch.value){if(ports.event_outcome)ports.event_outcome(id,event,DoorEnd::launch_failed);continue;}
        const auto result=ports.launch(*launch.value);if(!result)return false;if(ports.event_outcome)ports.event_outcome(id,event,*result);
        switch(*result){case DoorEnd::caller_disconnected:case DoorEnd::game_disconnect:case DoorEnd::shutdown:case DoorEnd::authority_lost:case DoorEnd::calling_time:case DoorEnd::inactivity:return false;default:break;}
    }return true;
}
} // namespace sfx::application
