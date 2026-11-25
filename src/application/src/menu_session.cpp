#include "sfx/application/menu_session.hpp"
#include "sfx/application/caller_help.hpp"
#include "sfx/application/caller_inactivity.hpp"
#include "sfx/application/classic_information.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
TerminalDisplayProgram trim_menu_artwork_padding(TerminalDisplayProgram program) {
    // Keep the native prompt under the artwork, above any unused canvas rows.
    while(!program.operations.empty()) {
        const auto kind=program.operations.back().kind;
        if(kind!=TerminalDisplayOperationKind::paging_newline && kind!=TerminalDisplayOperationKind::logical_newline) break;
        program.operations.pop_back();
        program.output_byte_count-=2;
    }
    return program;
}

std::string caller_inactivity_music(GraphicsMode graphics) {
    // SI restores the charset but ANSI-BBS terminals display its CP437 sun.
    // Erase that cell and return to the original cursor position.
    return graphics==GraphicsMode::ansi?"\x1b[|MBT180O4MLL16GP32C#P32G\x0e\x0f\b \b":std::string{};
}
std::string caller_inactivity_warning(GraphicsMode graphics,const MenuPreferences& preferences,
    std::uint8_t default_color,std::uint8_t input_color) {
    const auto text=std::string(kCallerInactivityWarning);
    if(graphics==GraphicsMode::ascii) return text;
    (void)preferences;(void)default_color;
    return menu_text_sgr(12)+text+menu_text_sgr(input_color);
}
TrustedDisplayIdentity menu_display_identity(MenuId id) noexcept {
    switch(id) {
        case MenuId::main: return TrustedDisplayIdentity::menu_main;
        case MenuId::messages: return TrustedDisplayIdentity::menu_messages;
        case MenuId::files: return TrustedDisplayIdentity::menu_files;
        case MenuId::doors: return TrustedDisplayIdentity::menu_doors;
        case MenuId::bulletins: return TrustedDisplayIdentity::menu_bulletins;
        case MenuId::door_bulletins: return TrustedDisplayIdentity::menu_door_bulletins;
        case MenuId::chat: return TrustedDisplayIdentity::menu_chat;
        case MenuId::settings: return TrustedDisplayIdentity::menu_settings;
        case MenuId::file_groups: return TrustedDisplayIdentity::menu_file_groups;
        case MenuId::file_areas: return TrustedDisplayIdentity::menu_file_areas;
        case MenuId::batch_transfer: return TrustedDisplayIdentity::menu_batch_transfer;
        case MenuId::qwk: return TrustedDisplayIdentity::menu_qwk;
    }
    return TrustedDisplayIdentity::menu_main;
}
std::string menu_duration_words(std::uint64_t seconds) {
    if(seconds<60) return std::to_string(seconds)+(seconds==1?" second":" seconds");
    const auto minutes=seconds/60;
    if(minutes<60) return std::to_string(minutes)+(minutes==1?" minute":" minutes");
    const auto hours=minutes/60;
    return std::to_string(hours)+(hours==1?" hour":" hours")+" "+
        std::to_string(minutes%60)+(minutes%60==1?" minute":" minutes");
}
bool CallerTimeWarnings::pending(std::optional<std::int64_t> seconds) const noexcept {
    if (!seconds || *seconds <= 0 || *seconds > 300) return false;
    return (*seconds <= 60 ? 60 : 300) < last_threshold_;
}
std::optional<std::string> CallerTimeWarnings::take(std::optional<std::int64_t> seconds) {
    if (!pending(seconds)) return {};
    last_threshold_ = *seconds <= 60 ? 60 : 300;
    auto text = menu_duration_words(*seconds);
    const auto remainder = *seconds % 60;
    if (*seconds > 60 && remainder) text += " " + menu_duration_words(remainder);
    return "Time warning: " + text + " remaining.";
}
namespace {
const std::array<std::string_view,5> transfer_names{{"XMODEM","YMODEM","YMODEM-G","ZMODEM","Select at time of transfer"}};
std::string duration(const std::optional<std::string>& value) {
    if(!value) return "unavailable";
    if(*value=="Unlimited") return "unlimited time";
    std::uint64_t seconds{};
    const auto parsed=std::from_chars(value->data(),value->data()+value->size(),seconds);
    return parsed.ec==std::errc{} && parsed.ptr==value->data()+value->size()
        ? menu_duration_words(seconds) : "unavailable";
}
bool same_access(const MenuSessionSnapshot& a,const MenuSessionSnapshot& b) {
    return a.access.revision==b.access.revision && a.access.security_level==b.access.security_level &&
        a.access.sysop_authorized==b.access.sysop_authorized &&
        a.configuration_revision==b.configuration_revision && a.preferences.revision==b.preferences.revision;
}
std::string footer_clock(const std::optional<std::string>& value) {
    if(!value || value->size()!=8 || (*value)[2]!=':' || (*value)[5]!=':') return "unavailable";
    unsigned hour{};
    const auto parsed=std::from_chars(value->data(),value->data()+2,hour);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value->data()+2 || hour>23) return "unavailable";
    return std::to_string(hour%12?hour%12:12)+value->substr(2)+(hour<12?" AM":" PM");
}
std::string location_status(const MenuSessionSnapshot& state,MenuId menu){
    if(menu==MenuId::chat)return "Current Chat Room: "+state.context.chat_room.value_or("None");
    return "It is presently "+footer_clock(state.context.time)+" on "+state.context.date.value_or("unavailable")+".";
}
bool finish_goodbye(const MenuSessionPorts& ports,const MenuSessionSnapshot& state) {
    if(ports.door_event&&!ports.door_event(DoorAutomaticEvent::logoff))return false;
    TrustedDisplaySelectionRequest request;
    request.identity=TrustedDisplayIdentity::goodbye;request.graphics=state.graphics;
    request.context=state.context;
    const auto custom=ports.artwork(request);
    if(custom.available())return ports.display(custom.program->terminal_program);
    return ports.write("\r\n"+render_menu_rows({{MenuTextRole::feedback,"Goodbye from Spitfire-X BBS."}},
        state.graphics==GraphicsMode::ansi,state.preferences.preferences,state.default_color));
}
}
std::vector<MenuTextRow> menu_footer_rows(const MenuSessionSnapshot& state,std::string prompt,MenuId menu) {
    std::vector<MenuTextRow> rows;
    if(!state.compact_footer) {
        rows.push_back({MenuTextRole::status,location_status(state,menu),true});
        rows.push_back({MenuTextRole::status,"You have been on "+duration(state.context.time_on)+" with "+duration(state.context.time_left)+" remaining.",true});
    }
    rows.push_back({MenuTextRole::prompt,std::move(prompt),true});return rows;
}
bool run_menu_session(const MenuSessionPorts& ports, MenuId initial_menu,bool login_information) {
    if(!ports.snapshot || !ports.artwork || !ports.display || !ports.write || !ports.read ||
        !ports.save_preferences || !ports.action || !ports.utc_now) return false;
    if(static_cast<unsigned>(initial_menu)>static_cast<unsigned>(MenuId::qwk)||
       static_cast<unsigned>(initial_menu)==3||static_cast<unsigned>(initial_menu)==4) return false;
    MenuId location=initial_menu;
    std::optional<MenuId> published_location;
    MenuInputDecoder input;
    std::string event;
    bool color_picker=false,transfer_picker=false,confirming_logoff=false;
    bool helping=false;
    std::optional<char> help_key;
    if(login_information) {
        const auto bulletins=run_bulletin_session(ports,ports.previous_login_utc,true);
        if(bulletins==BulletinOutcome::failed) return false;
        confirming_logoff=bulletins==BulletinOutcome::goodbye;
        if(!confirming_logoff && !offer_login_newsletter(ports,ports.previous_login_utc)) return false;
        if(!confirming_logoff && !show_login_door_bulletin_updates(ports))return false;
        if(!confirming_logoff && ports.login_message_scan && !ports.login_message_scan())return false;
        if(!confirming_logoff && ports.login_file_scan && !ports.login_file_scan())return false;
        if(!confirming_logoff && ports.door_event && !ports.door_event(DoorAutomaticEvent::logon))return false;
    }
    std::optional<TransferPreferenceRecord> shown_transfer;
    unsigned highlighted=1;
    std::vector<std::string> color_screen;
    unsigned color_columns{};
    for(;;) {
        auto state=ports.snapshot();
        if(!state || !state->access.security_level || state->access.security_level>999 ||
            !valid_menu_preferences(state->preferences.preferences)) return false;
        auto shown=menu_snapshot(location,state->access,state->configuration_revision);
        const auto door_commands=ports.door_commands?ports.door_commands(location):std::optional<MenuDoorCommands>(MenuDoorCommands{});
        if(!door_commands)return false;
        for(const auto& command:door_commands->commands)
            if(available_door_command_key(location,command.key) && !(location==MenuId::messages && command.key=='Y'))
                shown.commands.push_back({command.key,command.name});
        const auto custom_command=[&](unsigned key)->const MenuDoorCommand*{
            for(const auto& command:door_commands->commands)if(static_cast<unsigned char>(command.key)==key&&available_door_command_key(location,command.key))return &command;
            return nullptr;
        };
        if(published_location!=location){
            if(ports.menu_entered)ports.menu_entered(location);
            published_location=location;
        }
        const bool ansi=state->graphics==GraphicsMode::ansi;
        const bool lightbar=color_picker && ansi && state->lightbars_enabled && state->preferences.preferences.lightbar;
        if(!color_picker||!ansi||color_columns!=state->columns)color_screen.clear();
        color_columns=state->columns;
        if(transfer_picker) {
            shown_transfer=ports.transfer_preference?ports.transfer_preference():std::nullopt;
            if(!shown_transfer || shown_transfer->account_id!=state->preferences.account_id ||
                shown_transfer->revision<1 || storage_code(shown_transfer->preference).empty()) {
                transfer_picker=false;event="Default transfer preference is unavailable. Nothing changed.";
            }
        }
        input.reset(helping || confirming_logoff || state->preferences.preferences.hotkey,color_picker || transfer_picker,
            !confirming_logoff && !color_picker && !transfer_picker &&
            (location==MenuId::main || location==MenuId::messages || location==MenuId::files || location==MenuId::chat || location==MenuId::doors));
        bool initial_draw=true;
        const auto draw=[&]() {
            if(!initial_draw){
                // An asynchronous notice redraw must not repeat the clock,
                // elapsed time or current conference from menu entry. Retain
                // the original authority snapshot for the later route gate.
                const auto live=ports.snapshot();if(!live)return false;
                state->context=live->context;state->message_group=live->message_group;
                state->message_echo=live->message_echo;state->columns=live->columns;
            }
            initial_draw=false;
            if(confirming_logoff) {
                auto rows=ports.notifications?ports.notifications():std::vector<MenuTextRow>{};
                rows.push_back({MenuTextRole::feedback,"Logoff the BBS? [Y/n]"});
                return ports.write("\r\n"+render_menu_rows(rows,ansi,state->preferences.preferences,state->default_color));
            }
            // Opening the chooser appends below the echoed menu command. Only
            // the explanation and the approved return redraw clear the screen.
            if((!color_picker||!ansi||color_screen.empty())&&!ports.write(helping && !help_key ? "\r\n" :
                ansi?"\x1b[0m\x1b[2J\x1b[H":"\r\n")) return false;
            if(helping){
                std::vector<MenuTextRow> rows{{MenuTextRole::heading,std::string(menu_definition(location).title)+" HELP..."},
                    {MenuTextRole::heading,{}}};
                const auto topic=help_key?menu_help_topic(location,*help_key):nullptr;
                if(help_key&&custom_command(static_cast<unsigned char>(*help_key))){
                    const auto& command=*custom_command(static_cast<unsigned char>(*help_key));
                    const CallerHelpTopic explanation{static_cast<int>(location),command.key,command.name,
                        location==MenuId::messages && command.key=='Y'?
                            "Y opens a choice between Your Message Scan and this Door. Choose Y or Enter for the scan, D for the Door, or Q to return.":
                            "Launch this Door and return to this menu when it ends. The Door's access and execution requirements apply."};
                    const auto text=caller_help_rows(explanation,state->columns);rows.insert(rows.end(),text.begin(),text.end());
                }
                else if(topic){const auto explanation=caller_help_rows(*topic,state->columns);rows.insert(rows.end(),explanation.begin(),explanation.end());}
                else rows.push_back({MenuTextRole::prompt,caller_help_question(shown)});
                if(ports.operational_notifications){
                    const auto required=ports.operational_notifications();
                    rows.insert(rows.end()-1,required.begin(),required.end());
                }
                return ports.write(render_menu_rows(rows,ansi,state->preferences.preferences,state->default_color)+
                    (ansi?menu_text_sgr(state->input_color):std::string{}));
            }
            if(transfer_picker) {
                std::vector<MenuTextRow> rows{{MenuTextRole::heading,"DEFAULT TRANSFER PROTOCOL"},
                    {MenuTextRole::heading,{}},
                    {MenuTextRole::feedback,"Saved default for File transfers."},
                    {MenuTextRole::feedback,"Current: "+std::string(transfer_names[static_cast<std::size_t>(shown_transfer->preference)])},
                    {MenuTextRole::feedback,{}}};
                for(std::size_t index=0;index<transfer_names.size();++index)
                    rows.push_back({MenuTextRole::feedback,std::to_string(index+1)+". "+std::string(transfer_names[index])});
                if(!event.empty()) rows.push_back({MenuTextRole::feedback,event});
                if(ports.notifications){const auto notices=ports.notifications();rows.insert(rows.end(),notices.begin(),notices.end());}
                rows.push_back({MenuTextRole::feedback,{}});
                rows.push_back({MenuTextRole::prompt,"Choose 1-5, [Q]/Escape to cancel: "});
                return ports.write(render_menu_rows(rows,ansi,state->preferences.preferences,state->default_color)+
                    (ansi?menu_text_sgr(state->input_color):std::string{}));
            }
            if(color_picker) {
                std::vector<std::string> lines{system_menu_text(">>>>>>>> MENU COLOR <<<<<<<<",ansi),""};
                for(unsigned index=0;index<16;++index) {
                    const auto& color=menu_colors()[index];
                    const auto number=index+1;
                    std::string row=std::to_string(number)+". "+std::string(color.name);
                    row.append(25-row.size(),' ');
                    if(lightbar && number==highlighted) row=menu_text_sgr(state->selected_foreground,state->selected_background)+row+"\x1b[0m";
                    if(ansi) row+=menu_text_sgr(color.value?color.value:state->default_color)+"AaBb"+"\x1b[0m";
                    else row+="AaBb";
                    lines.push_back(std::move(row));
                }
                lines.push_back({});
                if(state->lightbars_enabled)lines.push_back(system_menu_text(std::string("[L] Light bars: ")+(state->preferences.preferences.lightbar?"On":"Off"),ansi));
                const auto row_text=[&](const MenuTextRow& row){auto value=render_menu_rows({row},ansi,state->preferences.preferences,state->default_color);const auto end=value.find("\r\n");if(end!=std::string::npos)value.erase(end,2);return value;};
                if(!event.empty())lines.push_back(row_text({MenuTextRole::feedback,event}));
                if(ports.notifications)for(const auto& notice:ports.notifications())lines.push_back(row_text(notice));
                const std::string prompt="Choose 1-16, [Q] to return: ";
                lines.push_back(system_menu_text(prompt,ansi));
                std::string output;
                if(ansi){
                    const bool full=color_screen.size()!=lines.size();
                    for(std::size_t i=0;i<lines.size();++i)if(full||lines[i]!=color_screen[i])output+="\x1b["+std::to_string(i+1)+";1H"+menu_text_sgr(7)+lines[i]+"\x1b[0m\x1b[K";
                    for(std::size_t i=lines.size();i<color_screen.size();++i)output+="\x1b["+std::to_string(i+1)+";1H\x1b[0m\x1b[K";
                    output+="\x1b["+std::to_string(lines.size())+";"+std::to_string(prompt.size()+1)+"H"+menu_text_sgr(state->input_color);
                    color_screen=std::move(lines);
                }else for(std::size_t i=0;i<lines.size();++i)output+=lines[i]+(i+1==lines.size()?"":"\r\n");
                return ports.write(output);
            }
            std::vector<MenuTextRow> rows=builtin_menu_rows(shown,
                location_status(*state,location),
                "You have been on "+duration(state->context.time_on)+" with "+duration(state->context.time_left)+" remaining.",
                event,state->columns);
            // System feedback is separate from the footer status and prompt.
            for(auto& row:rows) if(row.role==MenuTextRole::event) row.role=MenuTextRole::feedback;
            if(state->compact_footer) {
                // The last three rows are the two live lines and command prompt.
                // Keep any event/empty-menu notice and the single separator.
                rows.resize(rows.size()-3U);
                rows.push_back({MenuTextRole::prompt,"Command: ",true});
            }
            if(!state->compact_footer && (location==MenuId::files || location==MenuId::file_groups || location==MenuId::file_areas)) {
                auto position=std::find_if(rows.begin(),rows.end(),[](const MenuTextRow& row){return row.role==MenuTextRole::prompt;});
                rows.insert(position,{MenuTextRole::status,"File Group: "+(state->file_group.empty()?"None":state->file_group)+"   File Area: "+(state->file_area_number?"#"+std::to_string(state->file_area_number)+" ":"")+(state->file_area.empty()?"None":state->file_area),true});
            }
            if(location==MenuId::messages || location==MenuId::files){
                // These menus retain context/time remaining, not a clock row.
                rows.erase(std::remove_if(rows.begin(),rows.end(),[](const MenuTextRow& row){
                    return row.role==MenuTextRole::status && row.text.rfind("It is presently ",0)==0;
                }),rows.end());
            }
            if(location==MenuId::messages){
                auto position=std::find_if(rows.begin(),rows.end(),[](const MenuTextRow& row){return row.role==MenuTextRole::prompt;});
                const auto number=state->context.message_conference_number;
                rows.insert(position,{MenuTextRole::status,"Current Message Conference is "+
                    (number?"#"+*number+" - \""+state->message_group+": "+state->message_echo+"\"":"None"),true});
            }
            {
                TrustedDisplaySelectionRequest request;
                request.identity=menu_display_identity(location); request.graphics=state->graphics;
                request.authenticated_security_level=state->access.security_level; request.context=state->context;
                const auto artwork=ports.artwork(request);
                const bool message_art_fits=(location!=MenuId::messages && location!=MenuId::qwk) || state->columns>=78;
                if(artwork.available() && message_art_fits) {
                    const auto program=trim_menu_artwork_padding(artwork.program->terminal_program);
                    if(!program.operations.empty()) {
                        if(!ports.display(program) || !ports.write(ansi?"\x1b[0m\r\n":"\r\n")) return false;
                        rows.erase(std::remove_if(rows.begin(),rows.end(),[](const MenuTextRow& row) {
                            return row.role==MenuTextRole::heading || row.role==MenuTextRole::command;
                        }),rows.end());
                    }
                }
            }
            if(ports.notifications){
                const auto notices=ports.notifications();
                if(!notices.empty()) {
                    // Replace the native art-to-footer spacer, don't push it
                    // below notifications. Never strip authored artwork rows.
                    auto at=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.role==MenuTextRole::status && row.text.empty();});
                    if(at!=rows.end())at=rows.erase(at);
                    else at=std::find_if(rows.begin(),rows.end(),[](const auto& row){return row.role==MenuTextRole::prompt;});
                    rows.insert(at,notices.begin(),notices.end());
                }
            }
            if (!confirming_logoff && ports.calling_time_warning) {
                if (auto warning = ports.calling_time_warning()) {
                    const auto at = std::find_if(rows.begin(), rows.end(), [](const MenuTextRow& row) {
                        return row.role == MenuTextRole::prompt ||
                            (row.role == MenuTextRole::status && !row.text.empty());
                    });
                    rows.insert(at, {MenuTextRole::feedback, std::move(*warning), true});
                }
            }
            const bool file_menu=location==MenuId::files||location==MenuId::batch_transfer||location==MenuId::file_groups||location==MenuId::file_areas;
            return ports.write((file_menu?render_file_menu_rows(rows,ansi,state->preferences.preferences,state->default_color):render_menu_rows(rows,ansi,state->preferences.preferences,state->default_color))+
                (ansi?menu_text_sgr(state->input_color):std::string{}));
        };
        if(!draw()) return false;
        if(!helping && !confirming_logoff && !color_picker && !transfer_picker && ports.safe_boundary){
            const auto prompt=render_menu_rows({{MenuTextRole::prompt,state->compact_footer?"Command: ":menu_prompt(shown),true}},ansi,state->preferences.preferences,state->default_color)+
                (ansi?menu_text_sgr(state->input_color):std::string{});
            if(!ports.safe_boundary(prompt))return false;
        }
        const auto choice=ports.read(input,draw);
        if(!choice) return false;
        const auto current=ports.snapshot();
        if(!current) return false;
        if(!same_access(*state,*current)) { confirming_logoff=false;help_key.reset();event="Settings or access changed. Please choose again."; continue; }
        if(ports.door_commands){
            const auto live=ports.door_commands(location);if(!live)return false;
            if(live->revision!=door_commands->revision||live->door_revision!=door_commands->door_revision){help_key.reset();event="Door commands changed. Please choose again.";continue;}
        }
        if(helping){
            if(help_key){
                // Other commands neither dismiss this acknowledgement nor run.
                if(choice->kind==MenuInputKind::redraw){helping=false;help_key.reset();}
                input.discard_pending();
            }else if(choice->kind==MenuInputKind::back){helping=false;input.discard_pending();}
            else if(choice->kind==MenuInputKind::command || choice->kind==MenuInputKind::quick_goodbye){
                const auto key=static_cast<unsigned char>(choice->value);
                // Reauthorize before explaining; never dispatch this route.
                if(custom_command(key)||(route_menu(shown,key,current->access,current->configuration_revision).code==MenuRouteCode::command &&
                    menu_help_topic(location,static_cast<char>(key)))){help_key=static_cast<char>(key);input.discard_pending();}
            }
            continue;
        }
        if(confirming_logoff) {
            if(choice->kind==MenuInputKind::back || (choice->kind==MenuInputKind::command && choice->value=='N')) {
                confirming_logoff=false;event.clear();continue;
            }
            if(choice->kind==MenuInputKind::redraw || (choice->kind==MenuInputKind::command && choice->value=='Y')) {
                return finish_goodbye(ports,*current);
            }
            continue;
        }
        if(transfer_picker) {
            if(choice->kind==MenuInputKind::back || (choice->kind==MenuInputKind::command && choice->value=='Q')) {
                transfer_picker=false;event="Default transfer preference unchanged.";continue;
            }
            if(choice->kind!=MenuInputKind::number || choice->value<1 || choice->value>transfer_names.size()) {
                event="Choose a listed number or Q.";continue;
            }
            const auto selected=static_cast<TransferProtocolPreference>(choice->value-1);
            if(ports.save_transfer_preference && ports.save_transfer_preference({shown_transfer->account_id,
                shown_transfer->revision,selected,ports.utc_now()})) {
                transfer_picker=false;
                event="Saved. Default transfer protocol: "+std::string(transfer_names[choice->value-1])+".";
            } else event="Not saved. Settings may have changed; please choose again.";
            continue;
        }
        if(color_picker) {
            if(choice->kind==MenuInputKind::back || (choice->kind==MenuInputKind::command && choice->value=='Q')) {
                color_picker=false; event.clear(); continue;
            }
            if(lightbar && (choice->kind==MenuInputKind::up || choice->kind==MenuInputKind::down)) {
                highlighted=choice->kind==MenuInputKind::up ? (highlighted==1?16:highlighted-1) : (highlighted==16?1:highlighted+1);
                continue;
            }
            auto preferences=current->preferences.preferences;
            const auto number=choice->kind==MenuInputKind::number ? choice->value :
                lightbar && choice->kind==MenuInputKind::redraw ? highlighted : 0U;
            if(number>=1 && number<=16) preferences.color=static_cast<std::uint8_t>(number-1);
            else if(state->lightbars_enabled && choice->kind==MenuInputKind::command && choice->value=='L')
                preferences.lightbar=!preferences.lightbar;
            else {event="Choose a listed number or Q."; continue;}
            const bool saved=ports.save_preferences({current->preferences.account_id,current->preferences.revision,
                preferences,ports.utc_now()});
            event=saved?"Saved.":"Not saved. Settings may have changed; please try again.";
            continue;
        }
        if(choice->kind==MenuInputKind::up || choice->kind==MenuInputKind::down) continue;
        if(choice->kind==MenuInputKind::invalid ||
            choice->kind==MenuInputKind::number) {event="Please choose one of the listed commands.";continue;}
        const auto key=choice->kind==MenuInputKind::back ? 27U : choice->kind==MenuInputKind::redraw ? 13U : choice->value;
        const auto custom=custom_command(key);
        bool run_door=custom && ports.door_command;
        if(run_door && location==MenuId::messages && key=='Y'){
            input.discard_pending();
            input.reset(true,false,false);
            const auto draw_choices=[&](){
                return ports.write("\r\n"+system_menu_text("[Y] Your Message Scan  [D] Door: "+custom->name+"  [Q] Return: ",ansi));
            };
            if(!draw_choices())return false;
            const auto selection=ports.read(input,draw_choices);
            if(!selection)return false;
            input.discard_pending();
            const auto live=ports.snapshot();
            if(!live)return false;
            if(!same_access(*current,*live)){event="Settings or access changed. Please choose again.";continue;}
            if(selection->kind==MenuInputKind::back ||
                (selection->kind==MenuInputKind::command && selection->value=='Q'))continue;
            if(selection->kind!=MenuInputKind::redraw && (selection->kind!=MenuInputKind::command ||
                (selection->value!='Y' && selection->value!='D'))){event="Choose Y for Your Message Scan or D for the Door.";continue;}
            run_door=selection->kind==MenuInputKind::command && selection->value=='D';
        }
        if(run_door){
            const auto result=ports.door_command({location,static_cast<char>(key),door_commands->revision,door_commands->door_revision});
            if(!result)return false;
            event=*result;input.discard_pending();
            if(ports.menu_entered)ports.menu_entered(location);
            continue;
        }
        const auto route=route_menu(shown,static_cast<unsigned char>(key),current->access,current->configuration_revision);
        event.clear();
        if(route.code==MenuRouteCode::redraw || route.code==MenuRouteCode::refresh) continue;
        if(route.code==MenuRouteCode::denied) {event="Access denied.";continue;}
        if(route.code!=MenuRouteCode::command) {event="Please choose one of the listed commands.";continue;}
        switch(route.action) {
            case MenuAction::open:
                if(ports.file_navigation){const auto allowed=ports.file_navigation(location,route.destination);if(!allowed)return false;if(!*allowed)break;}
                if(route.destination==MenuId::doors && ports.doors){
                    const auto result=ports.doors();if(!result)return false;
                    if(*result==DoorMenuExit::quick_goodbye){input.discard_pending();return !ports.door_event||ports.door_event(DoorAutomaticEvent::logoff);}
                    if(*result==DoorMenuExit::confirmed_goodbye){
                        input.discard_pending();const auto latest=ports.snapshot();
                        return latest&&finish_goodbye(ports,*latest);
                    }
                    confirming_logoff=*result==DoorMenuExit::goodbye;location=MenuId::main;
                    // The nested Door menu owns its own decoder. Do not carry
                    // the main menu's old D-hotkey Enter suppression into the
                    // logoff question or the returning main menu.
                    input.discard_pending();
                }else if(route.destination==MenuId::bulletins) {
                    const auto outcome=run_bulletin_session(ports,ports.previous_login_utc);
                    if(outcome==BulletinOutcome::failed) return false;
                    confirming_logoff=outcome==BulletinOutcome::goodbye;
                } else location=route.destination;
                break;
            case MenuAction::chat_room: case MenuAction::chat_rooms: case MenuAction::chat_text: {
                if(!ports.chat){event="Chat is unavailable.";break;}
                const auto result=ports.chat(route.action);if(!result)return false;input.discard_pending();
                if(*result==ChatExit::quick_goodbye)return !ports.door_event||ports.door_event(DoorAutomaticEvent::logoff);
                confirming_logoff=*result==ChatExit::goodbye;
                location=*result==ChatExit::main_menu?MenuId::main:MenuId::chat;
                break;
            }
            case MenuAction::help: helping=true;help_key.reset();break;
            case MenuAction::unavailable: {
                if(location==MenuId::main && key=='N') {
                    TrustedDisplaySelectionRequest request;
                    request.identity=TrustedDisplayIdentity::sfnewsletter;request.graphics=current->graphics;
                    request.context=current->context;
                    auto newsletter=ports.artwork(request);
                    if(newsletter.available() && prepare_acknowledged_display(newsletter.program->terminal_program,current->graphics,
                        current->preferences.preferences.color?current->preferences.preferences.color:current->default_color)) {
                        if(!ports.display(newsletter.program->terminal_program)) return false;
                        break;
                    }
                    event=newsletter.code==TrustedDisplaySelectionCode::not_found
                        ? "No news to report at this time - check back later"
                        : "Newsletter is unavailable at this time. Please try again later.";
                    break;
                }
                event=std::string(unavailable_menu_feature); break;
            }
            case MenuAction::goodbye: confirming_logoff=true;input.discard_pending();break;
            case MenuAction::quick_goodbye: input.discard_pending();return !ports.door_event||ports.door_event(DoorAutomaticEvent::logoff);
            case MenuAction::color: color_picker=true;highlighted=current->preferences.preferences.color+1U;break;
            case MenuAction::transfer_preference:
                if(ports.transfer_preference && ports.save_transfer_preference) transfer_picker=true;
                else event="Default transfer preference is unavailable. Nothing changed.";
                break;
            case MenuAction::hotkey: {
                auto preferences=current->preferences.preferences;preferences.hotkey=!preferences.hotkey;
                event=ports.save_preferences({current->preferences.account_id,current->preferences.revision,preferences,ports.utc_now()})
                    ? std::string("Saved. Hot keys ")+(preferences.hotkey?"On.":"Off.") : "Not saved. Please try again.";
                break;
            }
            case MenuAction::file_change:case MenuAction::file_list:case MenuAction::file_new:
            case MenuAction::file_find:case MenuAction::file_search:case MenuAction::file_read:case MenuAction::file_archive:
            case MenuAction::file_download_add:case MenuAction::file_upload_add:case MenuAction::file_both_add:
            case MenuAction::file_upload_view:case MenuAction::file_download_view:case MenuAction::file_both_view:
            case MenuAction::file_upload_remove:case MenuAction::file_download_remove:case MenuAction::file_both_remove:
            case MenuAction::file_upload_clear:case MenuAction::file_download_clear:case MenuAction::file_both_clear:
            case MenuAction::file_upload_begin:case MenuAction::file_download_begin:case MenuAction::file_private:case MenuAction::file_settings:{
                if(!ports.file_action){event="File service unavailable.";break;}
                const auto result=ports.file_action(route.action,location);if(!result)return false;
                event=result->notice;if(result->goodbye)return !ports.door_event||ports.door_event(DoorAutomaticEvent::logoff);if(result->destination)location=*result->destination;break;
            }
            case MenuAction::chat_dnd: case MenuAction::chat_notices: case MenuAction::page_sysop:
            case MenuAction::caller_profile: case MenuAction::caller_directory: case MenuAction::locate_caller:
            case MenuAction::message_change: case MenuAction::message_read: case MenuAction::message_browse:
            case MenuAction::message_addressed: case MenuAction::message_enter: case MenuAction::message_personal:
            case MenuAction::message_settings: case MenuAction::message_search: case MenuAction::comment_sysop:
            case MenuAction::qwk_download: case MenuAction::qwk_upload:
            case MenuAction::qwk_settings: case MenuAction::qwk_pointers:
            case MenuAction::other_boards: case MenuAction::system_information:
            case MenuAction::sysop_utilities:
            case MenuAction::password: case MenuAction::graphics: case MenuAction::online: {
                const auto result=ports.action(route.action);if(!result)return false;event=*result;break;
            }
        }
    }
}
} // namespace sfx::application
