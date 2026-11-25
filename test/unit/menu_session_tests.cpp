#include "sfx/application/menu_session.hpp"
#include "sfx/application/caller_help.hpp"
#include <iostream>
#include <stdexcept>
#include <tuple>

namespace app=sfx::application;
std::string visible(std::string_view s){std::string out;for(std::size_t i=0;i<s.size();){if(s[i]=='\x1b'&&i+1<s.size()&&s[i+1]=='['){i+=2;while(i<s.size()&&!(s[i]>=0x40&&s[i]<=0x7e))++i;if(i<s.size())++i;}else out+=s[i++];}return out;}
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
struct Fixture {
    app::MenuSessionSnapshot state;
    std::string input,output;
    std::size_t position=0;
    unsigned frames=0,saves=0,actions=0,art_frames=0;
    bool art=false,fail_save=false,change_access=false,echo_keys=false;
    std::optional<app::TransferPreferenceRecord> transfer{app::TransferPreferenceRecord{
        1,app::TransferProtocolPreference::select_at_transfer,1,1788990000}};
    unsigned transfer_saves=0;
    bool transfer_conflict=false;
    app::MenuSessionPorts ports;
    Fixture(std::string keys,bool ansi=false):input(std::move(keys)+"\r") {
        state.access={10,false,1};state.configuration_revision=1;
        state.preferences={1,{},1,1788990000};
        state.graphics=ansi?app::GraphicsMode::ansi:app::GraphicsMode::ascii;
        state.context.time="12:00:00";state.context.date="2026-09-27";
        state.context.time_on="60";state.context.time_left="Unlimited";
        ports.snapshot=[&]{return std::optional<app::MenuSessionSnapshot>{state};};
        ports.artwork=[&](const app::TrustedDisplaySelectionRequest& request) {
            if(!art || request.graphics!=app::GraphicsMode::ansi || app::trusted_menu_basename(request.identity).empty())
                return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::not_found,{}};
            ++art_frames;
            app::RenderedTrustedDisplayProgram value;
            value.identity=request.identity;value.graphics=request.graphics;
            value.selected_logical_asset=std::string(app::trusted_menu_basename(request.identity))+".ANS";
            value.aggregate_source_bytes=32;
            value.terminal_program.operations={{app::TerminalDisplayOperationKind::ansi_sequence,"\x1b[31m"},
                {app::TerminalDisplayOperationKind::cp437_text,"Authored [Z]"},
                {app::TerminalDisplayOperationKind::ansi_sequence,"\x1b[0m"},
                {app::TerminalDisplayOperationKind::paging_newline,{}}};
            value.terminal_program.output_byte_count=0;
            for(const auto& op:value.terminal_program.operations)
                value.terminal_program.output_byte_count+=op.kind==app::TerminalDisplayOperationKind::paging_newline?2U:op.bytes.size();
            return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::available,value};
        };
        ports.write=[&](std::string_view bytes){output+=bytes;return true;};
        ports.display=[&](const app::TerminalDisplayProgram& program) {
            for(const auto& op:program.operations) output+=op.bytes;
            return true;
        };
        ports.read=[&](app::MenuInputDecoder& decoder,const std::function<bool()>&) -> std::optional<app::MenuInput> {
            check(++frames<100,"bounded scripted session");
            while(position<input.size()) {
                auto choice=decoder.feed(static_cast<unsigned char>(input[position++]));
                if(choice.kind!=app::MenuInputKind::pending) {
                    if(echo_keys && choice.kind==app::MenuInputKind::command)output+=static_cast<char>(choice.value);
                    if(echo_keys && (choice.kind==app::MenuInputKind::command || choice.kind==app::MenuInputKind::redraw))output+="\r\n";
                    if(change_access){change_access=false;++state.access.revision;state.access.sysop_authorized=false;}
                    return choice;
                }
            }
            return std::nullopt;
        };
        ports.save_preferences=[&](const app::UpdateMenuPreferenceRequest& request) {
            ++saves;check(request.expected_revision==state.preferences.revision,"preference revision supplied");
            if(fail_save)return false;
            state.preferences.preferences=request.preferences;++state.preferences.revision;return true;
        };
        ports.action=[&](app::MenuAction)->std::optional<std::string>{++actions;return "Native action completed.";};
        ports.transfer_preference=[&]{return transfer;};
        ports.save_transfer_preference=[&](const app::UpdateTransferPreferenceRequest& request) {
            ++transfer_saves;
            check(request.account_id==state.preferences.account_id,"transfer write targets authenticated identity");
            check(transfer && request.expected_revision==transfer->revision,"transfer write carries shown revision");
            if(transfer_conflict) {++transfer->revision;return false;}
            transfer->preference=request.preference;++transfer->revision;return true;
        };
        ports.utc_now=[] {return std::int64_t{1788990001};};
    }
};
int main(){try {
    for(bool ansi:{false,true})for(bool hotkey:{false,true}){
        Fixture f("",ansi);f.input=hotkey?"C/g\r":"C\r/g\r";f.state.preferences.preferences.hotkey=hotkey;
        unsigned entered{},chat_calls{},logoffs{};f.ports.menu_entered=[&](auto menu){if(menu==app::MenuId::chat)++entered;};
        f.ports.chat=[&](app::MenuAction)->std::optional<app::ChatExit>{++chat_calls;return app::ChatExit::chat_menu;};
        f.ports.door_event=[&](auto){++logoffs;return true;};
        check(app::run_menu_session(f.ports)&&entered==1&&chat_calls==0&&logoffs==1,"Main C shows Chat Menu before any room action; menu slash g disconnects");
        check(visible(f.output).find("CHAT MENU - [")!=std::string::npos&&f.output.find("Logoff the BBS?")==std::string::npos,"Chat Menu prompt and quick exit correct");
        Fixture choices("C\rE\rL\rT\rQ\r/G\r",ansi);std::vector<app::MenuAction> actions;
        choices.ports.chat=[&](app::MenuAction action)->std::optional<app::ChatExit>{actions.push_back(action);return app::ChatExit::chat_menu;};
        check(app::run_menu_session(choices.ports)&&actions==std::vector<app::MenuAction>{app::MenuAction::chat_room,app::MenuAction::chat_rooms,app::MenuAction::chat_text},"Chat Menu E L T dispatch and return without leaving menu");
    }
    for(bool ansi:{false,true})for(bool custom_art:{false,true}){
        Fixture chat_footer("/G\r",ansi);chat_footer.art=custom_art;
        chat_footer.state.context.chat_room="Games";
        check(app::run_menu_session(chat_footer.ports,app::MenuId::chat),"Chat footer session completes");
        check(chat_footer.output.find("Current Chat Room: Games")!=std::string::npos&&chat_footer.output.find("It is presently")==std::string::npos,"Chat room replaces clock in native/custom-art footer");
        check(chat_footer.output.find("You have been on")!=std::string::npos&&visible(chat_footer.output).find("CHAT MENU - [E L W T ? Q G /G]")!=std::string::npos,"elapsed time and E/L command prompt retained");
        Fixture main_footer("/G\r",ansi);main_footer.state.context.chat_room="Games";
        check(app::run_menu_session(main_footer.ports)&&main_footer.output.find("It is presently")!=std::string::npos&&main_footer.output.find("Current Chat Room:")==std::string::npos,"other menu clock unchanged");
    }
    for(const auto menu:{app::MenuId::main,app::MenuId::messages,app::MenuId::files})for(bool custom_art:{false,true}){
        Fixture command("K\r/G\r",true);command.art=custom_art;unsigned launches{};
        command.ports.door_commands=[](app::MenuId){return std::optional<app::MenuDoorCommands>({7,9,{{'K',"Configured Door"}}});};
        command.ports.door_command=[&](const app::MenuDoorSelection& selected)->std::optional<std::string>{++launches;check(selected.menu==menu&&selected.key=='K'&&selected.revision==7&&selected.door_revision==9,"exact origin and revisions passed to shared launch");return "Returned from Configured Door.";};
        check(app::run_menu_session(command.ports,menu)&&launches==1,"one shared Door launch then original menu");
        check(command.output.find("Returned from Configured Door.")!=std::string::npos,"launch return feedback");
        check(visible(command.output).find(" K]")!=std::string::npos,"actual prompt includes configured key even with custom art");
        if(custom_art)check(command.output.find("Authored [Z]")!=std::string::npos,"custom artwork retained");
        Fixture help("?\rK\r\r/G\r",true);help.ports.door_commands=command.ports.door_commands;help.ports.door_command=command.ports.door_command;
        check(app::run_menu_session(help.ports,menu)&&launches==1&&help.output.find("Launch this Door and return to this menu")!=std::string::npos,"custom Help explains without launching");
    }
    for(char key:std::string("BDNPRQGX"))check(!app::available_door_command_key(app::MenuId::doors,key),"numbered-browser commands protected");
    for (const auto& [selection, scan, door] : std::vector<std::tuple<std::string, unsigned, unsigned>>{
            {"\r", 1, 0}, {"Y\r", 1, 0}, {"D\r", 0, 1}, {"Q\r", 0, 0}}) {
        Fixture conflict("Y\r" + selection + "/G\r", true);
        unsigned scans = 0;
        unsigned launches = 0;
        conflict.ports.door_commands=[](app::MenuId) {
            return std::optional<app::MenuDoorCommands>({7, 9, {{'Y', "Configured Door"}}});
        };
        conflict.ports.door_command=[&](const app::MenuDoorSelection& selected)->std::optional<std::string> {
            check(selected.menu==app::MenuId::messages && selected.key=='Y' &&
                  selected.revision==7 && selected.door_revision==9,"Y Door keeps its original route and revisions");
            ++launches;
            return "Returned from Configured Door.";
        };
        conflict.ports.action=[&](app::MenuAction action)->std::optional<std::string> {
            check(action==app::MenuAction::message_personal,"Y chooses the personal scan");
            ++scans;
            return "Scan complete.";
        };
        check(app::run_menu_session(conflict.ports,app::MenuId::messages) && scans==scan && launches==door,
            "Configured Y Door and Your Message Scan both stay reachable, with Enter defaulting to scan");
    }
    unsigned topics=0;
    for(const auto retired:{3,4}){Fixture closed("");check(!app::run_menu_session(closed.ports,static_cast<app::MenuId>(retired)),"retired menu IDs rejected without throwing");}
    for(unsigned id=0;id<=static_cast<unsigned>(app::MenuId::batch_transfer);++id){
        if(id==3||id==4)continue;
        const auto menu=static_cast<app::MenuId>(id);
        const auto shown=app::menu_snapshot(menu,{999,true,1},1);
        const bool has_help=app::menu_help_topic(menu,'?')!=nullptr;
        for(const auto& command:shown.commands){
            const auto topic=app::menu_help_topic(menu,command.key);
            check(has_help==(topic!=nullptr),"approved help covers only existing help areas");
            if(!topic)continue;
            ++topics;
            const auto key=command.key=='/'?std::string("/G"):std::string(1,command.key);
            Fixture help("?\r"+key+"\r",true);help.state.access={999,true,1};
            check(!app::run_menu_session(help.ports,menu),"help does not logoff or navigate its selected command");
            check(help.actions==0 && help.saves==0 && help.transfer_saves==0,"help has no action or preference side effects");
            check(help.output.find(std::string(topic->title))!=std::string::npos &&
                visible(help.output).find("< ENTER > To Continue")!=std::string::npos,"approved command explanation and acknowledgement");
            for(const auto& row:app::caller_help_rows(*topic,40))if(row.role==app::MenuTextRole::feedback)
                check(row.text.size()<=40,"help wraps at terminal width");
        }
    }
    check(topics==77,"77 Help topics including Your Message Scan and Chat Menu quick logoff");
    Fixture chat_settings("Y\rN\rL\rQ\r/G\r",true);check(app::run_menu_session(chat_settings.ports)&&chat_settings.actions==2,"DND and notice keys dispatch their operations");
    Fixture page_sysop("P\r/G\r",true);check(app::run_menu_session(page_sysop.ports)&&page_sysop.actions==1,"Page Sysop key reaches its action handler");
    Fixture help_ack("?\rMZ\r/G\r",true);
    help_ack.ports.operational_notifications=[] {return std::vector<app::MenuTextRow>{{app::MenuTextRole::notification,"Operational shutdown requires Enter"}};};
    check(app::run_menu_session(help_ack.ports) && help_ack.actions==0,"ordinary help topic/ack keys never dispatch actions");
    check(help_ack.output.find("Operational shutdown requires Enter")!=std::string::npos,"required operational notice visible during help");
    check(app::caller_help_question(app::menu_snapshot(app::MenuId::main,{10,false,1},1)).find('@')==std::string::npos,"non-Sysop help hides restricted command");
    for(auto menu:{app::MenuId::file_groups,app::MenuId::file_areas}){
        const auto shown=app::menu_snapshot(menu,{999,true,1},1);
        check(app::route_menu(shown,'?',{999,true,1},1).code==app::MenuRouteCode::invalid,
            "inline file selectors have no hidden Help route");
    }
    for(bool ansi:{false,true}) for(bool compact:{false,true}) for(bool art:{false,true}){
        Fixture append("?\r",ansi);append.art=art;append.state.compact_footer=compact;append.echo_keys=true;
        check(!app::run_menu_session(append.ports),"chooser does not execute anything at end of input");
        const auto display=visible(append.output);
        const auto heading=display.find("MAIN MENU HELP...");
        const auto prompt=display.find(compact?"Command: ":"MAIN MENU - [");
        check(prompt!=std::string::npos && heading>prompt,"Help appears after existing footer");
        const auto first=append.output.find("\x1b[2J");
        check(!ansi || append.output.find("\x1b[2J",first+1)==std::string::npos,
            "opening or repeating chooser never clears preserved menu");
        check(display.substr(prompt,heading-prompt).find("?\r\n\r\n")!=std::string::npos && display.substr(prompt,heading-prompt).find("?\r\n\r\n\r\n")==std::string::npos,
            "Help chooser opens one blank row below prompt");
        check(append.art_frames==(ansi && art?1U:0U),"opening Help never reloads artwork");
    }
    for(bool ansi:{false,true}) for(bool hot:{false,true})
        for(auto menu:{app::MenuId::main,app::MenuId::messages,app::MenuId::files}) {
            Fixture quick("/g\r",ansi);quick.art=ansi;
            quick.state.preferences.preferences.hotkey=hot;
            unsigned goodbye_requests=0;
            auto original=quick.ports.artwork;
            quick.ports.artwork=[&](const app::TrustedDisplaySelectionRequest& request) {
                if(request.identity==app::TrustedDisplayIdentity::goodbye)++goodbye_requests;
                return original(request);
            };
            check(app::run_menu_session(quick.ports,menu),"quick logoff uses normal session completion");
            check(goodbye_requests==0 && quick.output.find("Logoff the BBS?")==std::string::npos &&
                quick.output.find("Goodbye from")==std::string::npos,"quick logoff skips confirmation and all goodbye output");
            check(quick.output.find("[O] Other Bulletin Boards")==std::string::npos &&
                quick.output.find("[I] This System")==std::string::npos,"custom and native menus have no injected hints");
        }
    {Fixture f("D\r");unsigned events{};f.ports.doors=[]()->std::optional<app::DoorMenuExit>{return app::DoorMenuExit::quick_goodbye;};f.ports.door_event=[&](auto event){if(event==app::DoorAutomaticEvent::logoff)++events;return true;};check(app::run_menu_session(f.ports)&&events==1&&f.output.find("Logoff the BBS?")==std::string::npos,"Door quick exit performs one normal logoff event without confirmation");}
    for(auto menu:{app::MenuId::door_bulletins,app::MenuId::file_groups,app::MenuId::file_areas}){const auto snapshot=app::menu_snapshot(menu,{10,false,1},1);for(const auto& command:snapshot.commands)check(command.action!=app::MenuAction::goodbye&&command.action!=app::MenuAction::quick_goodbye,"selection menus expose no logoff actions");}
    Fixture stale_quick("/G\r/G\r");stale_quick.change_access=true;
    check(app::run_menu_session(stale_quick.ports) && stale_quick.output.find("Settings or access changed.")!=std::string::npos,
        "quick logoff reauthorizes stale selection before accepting a new one");
    for(bool ansi:{false,true})for(bool compact:{false,true})for(bool art:{false,true})
        for(auto menu:{app::MenuId::main,app::MenuId::messages,app::MenuId::files,app::MenuId::doors,app::MenuId::bulletins,app::MenuId::door_bulletins,app::MenuId::chat,app::MenuId::settings,app::MenuId::file_groups,app::MenuId::file_areas,app::MenuId::batch_transfer}){
        Fixture notice("/G\r",ansi);notice.art=art;notice.state.compact_footer=compact;
        notice.state.preferences.preferences.color=2;notice.state.columns=40;
        notice.ports.notifications=[] {return std::vector<app::MenuTextRow>{{app::MenuTextRole::notification,"You have a new message."},{app::MenuTextRole::inactivity_warning,"AFK warning"}};};
        notice.ports.read=[](app::MenuInputDecoder&,const std::function<bool()>&)->std::optional<app::MenuInput>{return {};};
        check(!app::run_menu_session(notice.ports,menu),"shared notifications render on every menu surface before transport end");
        const auto start=notice.output.find("You have a new message.");const auto afk=notice.output.find("AFK warning");
        const auto footer=notice.output.find(compact?(menu==app::MenuId::messages?"Current Message Conference is None":"Command: "):(menu==app::MenuId::messages || menu==app::MenuId::files)?"You have been on ":menu==app::MenuId::chat?"Current Chat Room: ":"It is presently ");
        check(start!=std::string::npos && start<afk && afk<footer,"notifications precede the entire native footer, including compact/art/plain");
        const auto immediately=notice.output.substr(afk+std::string("AFK warning").size(),footer-afk-std::string("AFK warning").size());
        const auto footer_color=compact&&menu!=app::MenuId::messages?std::uint8_t{7}:std::uint8_t{2};
        check(immediately==(ansi?"\r\n"+app::menu_text_sgr(footer_color):"\r\n"),"notification occupies spacer; no extra blank row beneath it");
        const auto prompt=compact?std::string("Command: "):app::builtin_menu_rows(
            app::menu_snapshot(menu,notice.state.access,notice.state.configuration_revision),"","",{},notice.state.columns).back().text;
        const auto display=visible(notice.output);
        const auto command=display.find(prompt);
        check(command!=std::string::npos&&display.find(prompt,command+prompt.size())==std::string::npos,
            "notification does not duplicate the command prompt");
        if(ansi)check(notice.output.find(app::menu_text_sgr(15)+"You have a new message.")==start-app::menu_text_sgr(15).size() && notice.output.find(app::menu_text_sgr(12)+"AFK warning")!=std::string::npos,"white/red notification colors ignore caller preference");
        else check(notice.output.find('\x1b')==std::string::npos,"plain notifications contain no ANSI");
    }
    Fixture malformed_quick("/\r/Q\r/G\r");
    check(app::run_menu_session(malformed_quick.ports) && malformed_quick.output.find("Please choose one of the listed commands.")!=std::string::npos,
        "incomplete and unrelated slash commands cannot prematurely log off");
    const std::array<std::string,5> transfer_labels{{"XMODEM","YMODEM","YMODEM-G","ZMODEM","Select at time of transfer"}};
    for(bool ansi:{false,true}) for(unsigned number=1;number<=5;++number) {
        Fixture saved_transfer("Y\rF\r"+std::to_string(number)+"\rQ\rG\rY",ansi);
        saved_transfer.state.preferences.preferences.color=2;
        check(app::run_menu_session(saved_transfer.ports),"transfer picker returns through Settings and normal Goodbye");
        check(saved_transfer.transfer_saves==1 && saved_transfer.transfer->preference==
            static_cast<app::TransferProtocolPreference>(number-1),"all approved transfer preferences save");
        check(saved_transfer.output.find("Saved. Default transfer protocol: "+transfer_labels[number-1]+".")!=std::string::npos,
            "saved result uses approved protocol name");
        check(saved_transfer.output.find("Saved default for File transfers.")!=std::string::npos&&saved_transfer.output.find("Unavailable protocols are never substituted.")==std::string::npos,
            "preference does not advertise a working transfer engine");
        check(saved_transfer.saves==0 && saved_transfer.state.preferences.preferences.color==2,
            "transfer save preserves accepted menu choices");
        check(ansi?saved_transfer.output.find(app::menu_text_sgr(7)+"Saved.")!=std::string::npos:
            saved_transfer.output.find('\x1b')==std::string::npos,"transfer result uses system color or plain ASCII");
    }
    Fixture cancelled_transfer("Y\rF\rQ\rQ\rG\rY");
    check(app::run_menu_session(cancelled_transfer.ports) && cancelled_transfer.transfer_saves==0,
        "transfer cancellation preserves saved preference");
    for(bool ansi:{false,true}){
        Fixture transfer_spacing("Y\rF\rQ\rQ\r/G\r",ansi);
        check(app::run_menu_session(transfer_spacing.ports),"beta6 protocol spacing session returns normally");
        const auto text=visible(transfer_spacing.output);
        check(text.find("Unavailable protocols are never substituted.")==text.npos,"protocol explanatory sentence removed without a selection change");
        check(text.find("Current: Select at time of transfer\r\n\r\n1. XMODEM")!=text.npos&&text.find("5. Select at time of transfer\r\n\r\nChoose 1-5")!=text.npos,"protocol current and choice prompt each have their requested blank row");
        Fixture color_spacing("Y\rC\rQ\rQ\r/G\r",ansi);
        check(app::run_menu_session(color_spacing.ports),"beta6 color spacing session returns normally");
        if(ansi)check(color_spacing.output.find("\x1b[19;1H")!=text.npos&&color_spacing.output.find("\x1b[20;1H")!=text.npos,"ANSI color picker reserves blank physical row before lightbars");
        else check(color_spacing.output.find("AaBb\r\n\r\n[L] Light bars")!=text.npos,"plain color picker has blank row before lightbars");
    }
    Fixture conflict_transfer("Y\rF\r1\rQ\rQ\rG\rY");conflict_transfer.transfer_conflict=true;
    check(app::run_menu_session(conflict_transfer.ports) && conflict_transfer.transfer->preference==
        app::TransferProtocolPreference::select_at_transfer && conflict_transfer.output.find("Not saved.")!=std::string::npos,
        "transfer conflict cannot overwrite concurrent change");
    Fixture invalid_transfer("Y\rF\r0\r6\rQ\rQ\rG\rY");
    check(app::run_menu_session(invalid_transfer.ports) && invalid_transfer.transfer_saves==0,
        "transfer choices outside approved set never write");
    Fixture unavailable_transfer("Y\rF\rQ\rG\rY");unavailable_transfer.transfer.reset();
    check(app::run_menu_session(unavailable_transfer.ports) && unavailable_transfer.transfer_saves==0 &&
        unavailable_transfer.output.find("unavailable. Nothing changed.")!=std::string::npos,
        "failed transfer read cannot invent a saved preference");
    Fixture wrong_identity_transfer("Y\rF\rQ\rG\rY");wrong_identity_transfer.transfer->account_id=2;
    check(app::run_menu_session(wrong_identity_transfer.ports) && wrong_identity_transfer.transfer_saves==0,
        "transfer snapshot for another account cannot be used");
    Fixture basic("M\rR\rQ\rF\rC\rQ\rQ\rY\rC\r16\rQ\rH\rQ\rG\r");
    check(app::run_menu_session(basic.ports),"navigation and settings reach normal Goodbye");
    check(basic.output.find("MESSAGE MENU")!=std::string::npos && basic.output.find("FILE MENU")!=std::string::npos,
        "approved submenu routes connected");
    check(basic.output.find("Native action completed.")!=std::string::npos && basic.actions==1,"implemented message read dispatches to the authoritative host");
    Fixture offline_mail("M\rL\rQ\r/G\r");
    check(app::run_menu_session(offline_mail.ports) && offline_mail.actions==0 &&
        offline_mail.output.find("QWK / OFFLINE MAIL MENU")!=std::string::npos,"QWK navigation returns without executing a transfer");
    check(basic.output.find("File service unavailable.")!=std::string::npos,"missing native file port reports unavailable without fabricated selectors");
    check(basic.output.find("File Group: None   File Area: None")!=std::string::npos,"file selection status explicit");
    check(basic.saves==2 && basic.state.preferences.preferences.color==15 && basic.state.preferences.preferences.hotkey,
        "color and hotkeys are saved through host");
    check(basic.output.find('\x1b')==std::string::npos,"ASCII navigation and color previews contain no escapes");
    check(basic.output.find("You have been on 1 minute with unlimited time remaining.")!=std::string::npos,"live status grammar and unlimited");
    Fixture artwork("Z\rG\r",true);artwork.art=true;
    artwork.state.preferences.preferences.color=2;
    check(app::run_menu_session(artwork.ports),"custom art session exits normally");
    check(artwork.output.find("\x1b[31mAuthored [Z]")!=std::string::npos,"caller green does not recolor authored red");
    check(artwork.output.find("Please choose one of the listed commands.")!=std::string::npos,"artwork cannot add command Z");
    Fixture stale("@\r@\rG\r");stale.state.access.sysop_authorized=true;stale.change_access=true;
    check(app::run_menu_session(stale.ports),"live access-change session survives safely");
    check(stale.output.find("Settings or access changed. Please choose again.")!=std::string::npos &&
        stale.output.find("Access denied.")!=std::string::npos,"stale command discarded and new unauthorized command denied");
    Fixture failure("Y\rC\r2\rQ\rQ\rG\r");failure.fail_save=true;
    check(app::run_menu_session(failure.ports) && failure.state.preferences.preferences.color==0,"failed save retains prior choice");
    check(failure.output.find("Not saved.")!=std::string::npos,"failed save reported honestly");
    Fixture bar("Y\rC\r\x1b[B\rQ\rQ\rG\r",true);
    check(app::run_menu_session(bar.ports) && bar.state.preferences.preferences.color==1,"arrow then Enter selects named color");
    const auto picker=bar.output.find("MENU COLOR");const auto selected=bar.output.find("Saved.",picker);
    check(visible(bar.output).find(">>>>>>>> MENU COLOR <<<<<<<<")!=std::string::npos &&
        picker!=std::string::npos && selected!=std::string::npos &&
        bar.output.substr(picker,selected-picker).find("\x1b[2J")==std::string::npos,
        "arrow and selection update the color picker without full-screen clears");
    Fixture native("W\rY\rP\rG\rQ\rG\r");
    check(app::run_menu_session(native.ports) && native.actions==3,"online password graphics dispatch native actions");
    Fixture ended("M\r");check(!app::run_menu_session(ended.ports),"transport end is not normal Goodbye");
    Fixture message_footer("Q\rG\r");message_footer.state.message_group="Local";message_footer.state.message_echo="Sysop Notices";
    message_footer.state.context.message_conference_number="1";
    check(app::run_menu_session(message_footer.ports,app::MenuId::messages),"message-only status navigation");
    const auto first_main=message_footer.output.find("MAIN MENU");
    const auto message_only=message_footer.output.substr(0,first_main);
    check(message_only.find("It is presently")==std::string::npos && message_only.find("You have been on 1 minute with unlimited time remaining.")!=std::string::npos,"Message footer no date retains actual online and unlimited time");
    check(message_only.find("Current Message Conference is #1 - \"Local: Sysop Notices\"")!=std::string::npos,"Message footer uses sentence case, real conference and names");
    for(bool ansi:{false,true})for(bool art:{false,true})for(bool compact:{false,true}) {
        Fixture file_footer("\r",ansi);file_footer.art=art;file_footer.state.compact_footer=compact;
        check(!app::run_menu_session(file_footer.ports,app::MenuId::files),"File footer redraw then transport end in both formats/art modes");
        const auto file_only=visible(file_footer.output);
        check(file_only.find("It is presently")==std::string::npos,"File footer omits clock including redraw");
        if(!compact){
            const std::string footer="You have been on 1 minute with unlimited time remaining.";
            const auto start=file_only.find(footer);
            check(start!=std::string::npos && file_only.find("File Group: None   File Area: None",start)!=std::string::npos,"File retains elapsed/remaining and group/area context");
            check(file_only.substr(start,footer.size()+2)==footer+"\r\n" && file_only.find("FILE MENU - [",start)!=std::string::npos,"File footer has no replacement blank row and retains prompt");
        }else check(file_only.find("Command: ")!=std::string::npos && file_only.find("You have been on")==std::string::npos,"File compact mode unchanged");
        Fixture file_return("Q\r/G\r",ansi);file_return.art=art;file_return.state.compact_footer=compact;
        check(app::run_menu_session(file_return.ports,app::MenuId::files),"File returns to Main in both formats/art modes");
        if(!compact)check(file_return.output.find("It is presently")!=std::string::npos,"Main still has its date/time row after File return");
    }
    for(bool ansi:{false,true}) for(bool art:{false,true}) {
        Fixture compact("M\rR\rQ\rG\r",ansi);compact.art=art;compact.state.compact_footer=true;
        compact.state.preferences.preferences.color=12;
        check(app::run_menu_session(compact.ports),"compact footer keeps navigation in both formats/art modes");
        check(compact.output.find("Command: ")!=std::string::npos && compact.output.find("It is presently")==std::string::npos &&
            compact.output.find("You have been on")==std::string::npos,"compact footer is one command line");
        check(compact.output.find("Native action completed.")!=std::string::npos,"compact footer does not hide implemented action feedback");
        check(!ansi || compact.output.find(app::menu_text_sgr(7)+"Command: ")!=std::string::npos,"compact command uses fixed light gray");
    }
    check(app::menu_duration_words(1)=="1 second" && app::menu_duration_words(3600)=="1 hour 0 minutes",
        "human-readable duration units");
    Fixture redraw("?\r@\rM\r?\rQ\rG\rN\rG\rY\r",true);redraw.art=true;
    redraw.state.access={999,true,1};redraw.state.preferences.preferences.color=12;
    check(app::run_menu_session(redraw.ports) && redraw.art_frames>=4,"help return, unavailable and cancelled logoff retain artwork");
    check(redraw.output.find("Which Command - [")!=std::string::npos && visible(redraw.output).find("< ENTER > To Continue")!=std::string::npos,
        "context help explains instead of running chosen commands");
    check(redraw.output.find(app::menu_text_sgr(7)+"Goodbye from Spitfire-X BBS.")!=std::string::npos,
        "native goodbye uses system color");
    check(visible(redraw.output).find("Logoff the BBS? [Y/n]")!=std::string::npos,"explicit logoff confirmation");
    Fixture save_color("Y\rH\rQ\rG\rY",true);save_color.state.preferences.preferences.color=2;
    check(app::run_menu_session(save_color.ports) && save_color.output.find(app::menu_text_sgr(7)+"Saved. Hot keys On.")!=std::string::npos,
        "saved feedback uses system color");
    Fixture no_confirm("G\r");no_confirm.input="G\r";
    check(!app::run_menu_session(no_confirm.ports) && no_confirm.output.find("Goodbye from")==std::string::npos,
        "disconnect at confirmation is not caller-requested goodbye");
    Fixture hot_enter("G\r",true);hot_enter.state.preferences.preferences.hotkey=true;
    check(app::run_menu_session(hot_enter.ports),"hotkey G then Enter confirms without extra keypress");
    for(const auto& sample:std::vector<std::pair<std::string,std::string>>{{"00:00:00","12:00:00 AM"},
        {"12:00:00","12:00:00 PM"},{"16:20:57","4:20:57 PM"},{"23:59:59","11:59:59 PM"}}) {
        Fixture clock("G\r",true);clock.state.context.time=sample.first;
        check(app::run_menu_session(clock.ports) && clock.output.find("It is presently "+sample.second+" on")!=std::string::npos,
            "Original footer uses 12-hour clock without changing date");
        check(clock.output.find("\r\n"+app::menu_text_sgr(7)+"Logoff")!=std::string::npos,
            "confirmation begins after a CRLF rather than beside command prompt");
    }
    for(unsigned color=0;color<16;++color) {
        app::MenuPreferences preferences;preferences.color=static_cast<std::uint8_t>(color);
        const auto warning=app::caller_inactivity_warning(app::GraphicsMode::ansi,preferences,2,11);
        check(warning.find(app::menu_text_sgr(12))==0U,"AFK warning is fixed bright red independent of caller/BBS color");
        check(app::caller_inactivity_warning(app::GraphicsMode::ascii,preferences,2,11).find('\x1b')==std::string::npos,
            "ASCII AFK warning contains no color controls");
    }
    check(app::caller_inactivity_music(app::GraphicsMode::ascii).empty(),"ASCII AFK has no music or BEL fallback");
    check(app::caller_inactivity_music(app::GraphicsMode::ansi)=="\x1b[|MBT180O4MLL16GP32C#P32G\x0e\x0f\b \b","selected B background cue with charset restoration and glyph erase, no setting override");
    for(bool ansi:{false,true}) for(std::uint8_t saved:{std::uint8_t{0},std::uint8_t{13}}) {
        Fixture news("N\rG\r",ansi);news.state.preferences.preferences.color=saved;news.state.default_color=2;
        auto original=news.ports.artwork;
        news.ports.artwork=[&](const app::TrustedDisplaySelectionRequest& request) {
            if(request.identity!=app::TrustedDisplayIdentity::sfnewsletter) return original(request);
            app::RenderedTrustedDisplayProgram value;value.identity=request.identity;value.graphics=request.graphics;
            value.selected_logical_asset="SFNWSLTR.ASC";value.aggregate_source_bytes=10;
            value.terminal_program.operations={{app::TerminalDisplayOperationKind::cp437_text,"Plain news"}};
            value.terminal_program.output_byte_count=10;
            return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::available,value};
        };
        auto display=news.ports.display;bool framed=false;
        news.ports.display=[&](const app::TerminalDisplayProgram& program) {
            framed=true;const auto color=saved?saved:news.state.default_color;
            check(ansi ? program.paging_prompt_color==color : !program.paging_prompt_color,"newsletter resolves saved/BBS color only for ANSI");
            return display(program);
        };
        check(app::run_menu_session(news.ports) && framed,"Main N uses real newsletter framing without changing menu routes");
    }
    for(bool ansi:{false,true}) for(auto code:{app::TrustedDisplaySelectionCode::not_found,
        app::TrustedDisplaySelectionCode::invalid,app::TrustedDisplaySelectionCode::unavailable}) {
        Fixture news("N\rG\r",ansi);news.state.preferences.preferences.color=12;
        auto original=news.ports.artwork;
        news.ports.artwork=[&](const app::TrustedDisplaySelectionRequest& request) {
            return request.identity==app::TrustedDisplayIdentity::sfnewsletter
                ? app::TrustedDisplaySelectionResult{code,{}} : original(request);
        };
        const std::string message=code==app::TrustedDisplaySelectionCode::not_found
            ? "No news to report at this time - check back later"
            : "Newsletter is unavailable at this time. Please try again later.";
        check(app::run_menu_session(news.ports),"missing/invalid newsletter returns to usable Main menu");
        check(news.output.find((ansi?app::menu_text_sgr(7):std::string{})+message)!=std::string::npos,
            "newsletter absence/refusal feedback is precise and system-colored");
        check(news.output.find(app::unavailable_menu_feature)==std::string::npos,"newsletter no longer reports feature unimplemented");
        check(ansi || news.output.find('\x1b')==std::string::npos,"ASCII newsletter fallback feedback stays plain");
    }
    {
        Fixture files("Q\rQ\rQ\r");unsigned navigation{},dispatch{};
        files.state.file_group="Main";files.state.file_area="Uploads";files.state.file_area_number=2;
        files.ports.file_navigation=[&](app::MenuId from,app::MenuId to)->std::optional<bool>{++navigation;if(from==app::MenuId::batch_transfer&&to==app::MenuId::files)return navigation>1;return true;};
        check(!app::run_menu_session(files.ports,app::MenuId::batch_transfer)&&navigation==3,"file queue navigation can cancel first return and then proceed");
        check(files.output.find("File Group: Main   File Area: #2 Uploads")!=std::string::npos,"current file context from native snapshot");
        Fixture transfer("U\r");
        transfer.ports.file_action=[&](app::MenuAction action,app::MenuId menu)->std::optional<app::FileSessionEffect>{++dispatch;check(action==app::MenuAction::file_upload_begin&&menu==app::MenuId::batch_transfer,"typed file action dispatch includes current menu");return app::FileSessionEffect{{},{},true};};
        check(app::run_menu_session(transfer.ports,app::MenuId::batch_transfer)&&dispatch==1,"completed batch logoff uses established normal lifetime owner");
    }
    for(const auto menu:{app::MenuId::files,app::MenuId::batch_transfer,app::MenuId::settings}){
        const auto snapshot=app::menu_snapshot(menu,{10,false,1},1);
        for(const auto& command:snapshot.commands){
            if(command.action<app::MenuAction::file_change||command.action>app::MenuAction::file_settings)continue;
            Fixture dispatch_file(std::string(1,command.key)+"\r");unsigned dispatched{};
            dispatch_file.ports.file_action=[&](app::MenuAction action,app::MenuId location)->std::optional<app::FileSessionEffect>{
                ++dispatched;check(action==command.action&&location==menu,"every advertised file action reaches the exact typed file handler");return app::FileSessionEffect{{},{},true};};
            check(app::run_menu_session(dispatch_file.ports,menu)&&dispatched==1,"no displayed file command silently falls through the menu dispatcher");
        }
    }
    {Fixture f("G\rY\r");std::vector<app::DoorAutomaticEvent> events;unsigned scan_order{};
        f.ports.login_message_scan=[&]{check(scan_order++==0,"message scan precedes automatic Doors");return true;};
        f.ports.login_file_scan=[&]{check(scan_order++==1,"file scan precedes automatic Doors");return true;};
        f.ports.door_event=[&](auto event){check(scan_order==2,"event occurs after login scans");events.push_back(event);check(f.output.find("Goodbye from Spitfire-X BBS.")==std::string::npos,"logoff event precedes goodbye output");return true;};
        check(app::run_menu_session(f.ports,app::MenuId::main,true)&&events==std::vector<app::DoorAutomaticEvent>({app::DoorAutomaticEvent::logon,app::DoorAutomaticEvent::logoff}),"logon and confirmed logoff events each run once");
    }
    {Fixture f("G\rN\r");unsigned events{};f.ports.door_event=[&](auto){++events;return true;};check(!app::run_menu_session(f.ports)&&events==0,"cancelled goodbye and later input loss never dispatch logoff event");}
    {Fixture f("G\rY\r");unsigned events{};f.ports.door_event=[&](auto){++events;return false;};check(!app::run_menu_session(f.ports)&&events==1&&f.output.find("Goodbye from Spitfire-X BBS.")==std::string::npos,"event caller loss stops exactly once before goodbye");}
    for(bool hotkey:{false,true})for(const auto answer:{"\r","\r\n","Y","N","\x1b"}){
        Fixture f("");f.input=hotkey?"D":"D\r";f.state.preferences.preferences.hotkey=hotkey;
        unsigned nested{},events{};bool asked{};
        f.ports.doors=[&]()->std::optional<app::DoorMenuExit>{++nested;return app::DoorMenuExit::goodbye;};
        f.ports.write=[&](std::string_view bytes){f.output+=bytes;if(!asked&&visible(bytes).find("Logoff the BBS? [Y/n]")!=std::string::npos){asked=true;f.input+=answer;}return true;};
        f.ports.door_event=[&](auto event){check(event==app::DoorAutomaticEvent::logoff,"Door confirmation dispatches only logoff");++events;return true;};
        const bool yes=answer[0]=='\r'||answer[0]=='Y';
        check(app::run_menu_session(f.ports)==yes,"Door return accepts first Enter or Y; N/Escape never log off");
        check(asked&&nested==1&&events==(yes?1u:0u),"Door confirmation event occurs exactly once only after acceptance");
    }
    for(bool ansi:{false,true})for(bool event_ok:{false,true}){
        Fixture f("D\r",ansi);unsigned events{},goodbye_art{};const auto art=f.ports.artwork;
        f.ports.doors=[]()->std::optional<app::DoorMenuExit>{return app::DoorMenuExit::confirmed_goodbye;};
        f.ports.door_event=[&](auto event){check(event==app::DoorAutomaticEvent::logoff,"confirmed Door exit dispatches only logoff");++events;return event_ok;};
        f.ports.artwork=[&](const auto& request){if(request.identity==app::TrustedDisplayIdentity::goodbye)++goodbye_art;return art(request);};
        check(app::run_menu_session(f.ports)==event_ok&&events==1,"Door-confirmed Goodbye finalizes exactly once and honors event failure");
        check(f.output.find("Logoff the BBS?")==std::string::npos&&goodbye_art==(event_ok?1u:0u),"Door-confirmed exit never asks twice or shows Goodbye before its event");
    }
    {Fixture f("");f.input="D";f.state.preferences.preferences.hotkey=true;unsigned events{};
        f.ports.doors=[]()->std::optional<app::DoorMenuExit>{return app::DoorMenuExit::goodbye;};f.ports.door_event=[&](auto){++events;return true;};
        check(!app::run_menu_session(f.ports)&&events==0,"Door goodbye with no answer never confirms itself");}
    for(bool hotkey:{false,true})for(bool ansi:{false,true})for(bool event_ok:{false,true}){
        Fixture f("",ansi);f.input=hotkey?"CE":"C\rE\r";f.state.preferences.preferences.hotkey=hotkey;
        unsigned events{},goodbye_art{};auto chat_artwork=f.ports.artwork;
        f.ports.artwork=[&](const auto& request){if(request.identity==app::TrustedDisplayIdentity::goodbye)++goodbye_art;return chat_artwork(request);};
        f.ports.chat=[](app::MenuAction)->std::optional<app::ChatExit>{return app::ChatExit::quick_goodbye;};
        f.ports.door_event=[&](auto event){check(event==app::DoorAutomaticEvent::logoff,"Chat quick exit uses normal logoff event");++events;return event_ok;};
        check(app::run_menu_session(f.ports)==event_ok,"Chat quick exit returns normal event result without another input");
        check(events==1&&goodbye_art==0&&f.output.find("Logoff the BBS?")==std::string::npos&&f.output.find("Goodbye from Spitfire-X BBS.")==std::string::npos,"Chat quick exit omits confirmation and goodbye art/text");
    }
    for(bool hotkey:{false,true})for(const auto answer:{"\r","Y","N"}){
        Fixture f("",true);f.input=hotkey?"CE":"C\rE\r";f.state.preferences.preferences.hotkey=hotkey;
        unsigned events{},goodbye_art{};bool asked{};auto chat_artwork=f.ports.artwork;
        f.ports.artwork=[&](const auto& request){
            if(request.identity!=app::TrustedDisplayIdentity::goodbye)return chat_artwork(request);
            ++goodbye_art;app::RenderedTrustedDisplayProgram value;value.identity=request.identity;value.graphics=request.graphics;
            value.selected_logical_asset="GOODBYE.ANS";value.aggregate_source_bytes=16;
            value.terminal_program.operations={{app::TerminalDisplayOperationKind::cp437_text,"TEST GOODBYE ART"}};
            value.terminal_program.output_byte_count=16;
            return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::available,value};
        };
        f.ports.chat=[](app::MenuAction)->std::optional<app::ChatExit>{return app::ChatExit::goodbye;};
        f.ports.write=[&](std::string_view bytes){f.output+=bytes;if(!asked&&visible(bytes).find("Logoff the BBS? [Y/n]")!=std::string::npos){asked=true;f.input+=answer;}return true;};
        f.ports.door_event=[&](auto){++events;return true;};const bool yes=answer[0]!='N';
        check(app::run_menu_session(f.ports)==yes,"Chat G respects Enter/Y acceptance and N refusal");
        check(asked&&events==(yes?1u:0u)&&goodbye_art==(yes?1u:0u),"Chat G confirms before one logoff event and goodbye display");
        check((f.output.find("TEST GOODBYE ART")!=std::string::npos)==yes,"Chat G renders goodbye artwork only after acceptance");
    }
    for(bool lost:{false,true}){
        Fixture f("C\rE\r");unsigned events{};f.ports.chat=[&](app::MenuAction)->std::optional<app::ChatExit>{return lost?std::nullopt:std::optional<app::ChatExit>{app::ChatExit::main_menu};};
        f.ports.door_event=[&](auto){++events;return true;};
        check(!app::run_menu_session(f.ports)&&events==0&&f.output.find("Logoff the BBS?")==std::string::npos,"Chat Main return or lost caller never invents a voluntary logoff");
    }
    {
        app::CallerTimeWarnings warning;
        check(!warning.pending({}) && !warning.pending(301), "unlimited and more than five minutes do not warn");
        check(warning.take(294) == "Time warning: 4 minutes 54 seconds remaining.", "warning reports time at first safe boundary");
        check(!warning.take(290), "five-minute warning appears once");
        check(warning.take(60) == "Time warning: 1 minute remaining.", "one-minute warning");
        check(!warning.take(300) && !warning.take(58), "extended limits do not reissue a threshold");
        app::CallerTimeWarnings short_call;
        check(short_call.take(25) == "Time warning: 25 seconds remaining." && !short_call.take(61),
            "a caller starting under a minute receives one warning");
        Fixture f("");
        f.ports.calling_time_warning = [] {return std::optional<std::string>{"Time warning: 5 minutes remaining."};};
        (void)app::run_menu_session(f.ports);
        const auto output = visible(f.output);
        check(output.find("Time warning:") < output.find("You have been on"),"time warning is above the time display");
    }
    std::cout<<"PASS menu session: "<<checks<<" checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
