#include "sfx/application/menu.hpp"
#include "sfx/application/menu_session.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>

namespace app = sfx::application;
using Id = app::MenuId;
using Action = app::MenuAction;
using Code = app::MenuRouteCode;
unsigned checks=0;
void require(bool ok,const char* text) { ++checks; if(!ok) throw std::runtime_error(text); }

int main() {
    try {
        const app::MenuAccess ordinary{10,false,5}, sysop{999,true,5};
        constexpr std::array<std::string_view,12> expected{{
            "MFDBCSL#WPYN?OIG/", "LCRBSEYATFW?QG/", "CLNFTRVDBUSW?MQG/",
            "XQG/", "NLQ", "NPQ",
            "ELWT?QG/", "PGFHSCNLE?Q", "Q", "Q", "AUDLRCQ?", "DUPQSA?"
        }};
        for(unsigned number=0;number<expected.size();++number) {
            const auto id=static_cast<Id>(number<3?number:number+2);
            const auto snapshot=app::menu_snapshot(id,ordinary,12);
            std::string keys;
            std::set<char> unique;
            for(const auto& command:snapshot.commands) {
                keys += command.key;
                require(unique.insert(command.key).second,"no duplicate command keys");
                auto route=app::route_menu(snapshot,static_cast<unsigned char>(command.key),ordinary,12);
                require(route.code==Code::command && route.action==command.action,"every displayed command routes");
                require(route.destination==(command.action==Action::open?command.destination:id),
                    "non-navigation actions preserve current menu");
                if(command.key>='A' && command.key<='Z')
                    require(app::route_menu(snapshot,static_cast<unsigned char>(command.key-'A'+'a'),ordinary,12).action==command.action,
                        "case-insensitive command keys");
            }
            require(keys==expected[number],"exact approved map/order");
            std::string prompt_keys;
            for(const auto& command:snapshot.commands) {
                if(!prompt_keys.empty())prompt_keys+=' ';
                if(command.action==Action::quick_goodbye)prompt_keys+="/G";else prompt_keys+=command.key;
            }
            require(app::menu_prompt(snapshot).find("["+prompt_keys+"]?")!=std::string::npos,
                "prompt is generated from same authoritative commands");
            for(unsigned key=0;key<256;++key) {
                auto normalized=key>='a' && key<='z'?key-'a'+'A':key;
                const auto result=app::route_menu(snapshot,static_cast<unsigned char>(key),ordinary,12);
                if(unique.count(static_cast<char>(normalized))) require(result.code==Code::command,"accepted displayed key");
                else if(key==27 || key=='\r' || key=='\n')
                    require(result.code==Code::redraw || result.code==Code::command,"safe back/redraw");
                else if(key=='@' && id==Id::main) require(result.code==Code::denied,"unauthorized Sysop is denied not unavailable");
                else require(result.code==Code::invalid,"unmapped byte is not a command");
            }
            for(unsigned columns:{80U,100U,132U}) {
                const auto rows=app::builtin_menu_rows(snapshot,"It is presently 12:00 on September 27.",
                    "You have been on 1 minute with unlimited time remaining.",{},columns);
                require(rows.front().text==">>>>>>>> "+std::string(app::menu_definition(id).title)+" <<<<<<<<",
                    "exact unboxed heading");
                require(rows.back().role==app::MenuTextRole::prompt && rows.back().text==app::menu_prompt(snapshot),
                    "authoritative prompt last");
                unsigned empty=0;
                for(const auto& row:rows) {
                    require(row.text.find('\x1b')==std::string::npos,"layout never emits raw ANSI");
                    require(row.text.size()<=columns,"generated rows fit supported width");
                    if(row.text.empty()) ++empty;
                }
                require(empty==2 && rows[1].text.empty() && rows[rows.size()-4].text.empty(),
                    "one blank line after heading and one before live status");
                for(char key:keys) {
                    const auto marker=std::string("[")+(key=='/'?std::string("/G"):std::string(1,key)+":")+">......... ";
                    require(std::count_if(rows.begin(),rows.end(),[&](const app::MenuTextRow& row){
                        return row.role==app::MenuTextRole::command && row.text.find(marker)!=std::string::npos;})==1,
                        "each command represented once in unboxed rows");
                }
            }
            require(app::route_menu(snapshot,'?',app::MenuAccess{10,false,6},12).code==Code::refresh,
                "access revision cannot silently change an in-progress selection");
            require(app::route_menu(snapshot,'?',ordinary,13).code==Code::refresh,
                "configuration revision requires redraw/new selection");
            require(app::route_menu(snapshot,'Q',app::MenuAccess{0,false,5},12).code==Code::denied,
                "invalid/deactivated authentication cannot route");
        }
        const auto main=app::menu_snapshot(Id::main,ordinary,12);
        require(app::route_menu(main,'F',ordinary,12).destination==Id::files,"Main F File");
        require(app::route_menu(main,'L',ordinary,12).action==Action::locate_caller,"Main L remains Locate Caller, never QWK");
        const auto offline=app::menu_snapshot(Id::qwk,ordinary,12);
        require(app::route_menu(app::menu_snapshot(Id::messages,ordinary,12),'L',ordinary,12).destination==Id::qwk,"Message Menu L opens QWK only");
        require(app::route_menu(offline,'Q',ordinary,12).destination==Id::messages &&
                app::route_menu(offline,27,ordinary,12).destination==Id::messages,"QWK Q/Escape returns Message Menu, not Main");
        require(app::menu_definition(Id::qwk).asset_base=="qwk","both display companions use the QWK asset base");
        require(app::menu_display_identity(Id::qwk)==app::TrustedDisplayIdentity::menu_qwk,"actual trusted artwork route is QWK, never Main");
        require(app::trusted_menu_basename(app::menu_display_identity(Id::qwk))=="QWK","actual QWK trusted companion basename");
        require(app::route_menu(offline,'A',ordinary,12).action==Action::message_settings,"QWK A reuses existing shared scan settings");
        require(app::route_menu(offline,'D',ordinary,12).action==Action::qwk_download &&
                app::route_menu(offline,'U',ordinary,12).action==Action::qwk_upload &&
                app::route_menu(offline,'S',ordinary,12).action==Action::qwk_settings &&
                app::route_menu(offline,'P',ordinary,12).action==Action::qwk_pointers,"approved packet workflow routes are distinct");
        require(app::route_menu(app::menu_snapshot(Id::files,ordinary,12),'B',ordinary,12).destination==Id::batch_transfer,
            "File B opens shared Batch Transfer Menu");
        for(const auto& command:app::menu_snapshot(Id::batch_transfer,ordinary,12).commands)if(command.key!='Q'&&command.key!='?')require(command.action>=Action::file_change&&command.action<=Action::file_settings,
            "shared Batch options use native file actions");
        require(app::route_menu(app::menu_snapshot(Id::files,ordinary,12),'S',ordinary,12).action==Action::file_private,
            "private submission directly on File Menu");
        require(app::route_menu(app::menu_snapshot(Id::batch_transfer,ordinary,12),'G',ordinary,12).code==Code::invalid &&
            app::route_menu(app::menu_snapshot(Id::batch_transfer,ordinary,12),'S',ordinary,12).code==Code::invalid,
            "private command is only on File Menu, not combined Batch");
        for(const auto retired:{3,4}){bool refused=false;try{(void)app::menu_definition(static_cast<Id>(retired));}catch(const std::invalid_argument&){refused=true;}require(refused,"retired menus cannot be opened");}
        require(app::route_menu(app::menu_snapshot(Id::files,ordinary,12),'Q',ordinary,12).destination==Id::main,
            "File Q Main");
        require(app::route_menu(app::menu_snapshot(Id::files,ordinary,12),'U',ordinary,12).action==Action::file_upload_add,
            "File U starts direct upload selection");
        require(app::route_menu(app::menu_snapshot(Id::batch_transfer,ordinary,12),'A',ordinary,12).action==Action::file_both_add,
            "shared Batch A adds to either queue without starting a transfer");
        require(app::route_menu(app::menu_snapshot(Id::settings,ordinary,12),'G',ordinary,12).action==Action::graphics,
            "Your Settings G changes graphics, not logoff");
        require(app::route_menu(app::menu_snapshot(Id::door_bulletins,ordinary,12),'Q',ordinary,12).destination==Id::doors,
            "Door Bulletin Q returns to Door Menu");
        require(app::route_menu(app::menu_snapshot(Id::doors,ordinary,12),'\r',ordinary,12).destination==Id::main,
            "root Door Enter returns Main");
        require(app::route_menu(main,'H',ordinary,12).code==Code::invalid,"retired Main H stays unassigned");
        require(app::route_menu(app::menu_snapshot(Id::bulletins,ordinary,12),'G',ordinary,12).code==Code::invalid,
            "Bulletin G is not a logoff route");
        const auto privileged=app::menu_snapshot(Id::main,sysop,12);
        require(privileged.commands.back().key=='@',"authorized Sysop sees @");
        require(app::route_menu(privileged,'@',sysop,12).action==Action::sysop_utilities,
            "authorized Sysop Utilities routes to the scoped controller");
        require(app::route_menu(privileged,'@',ordinary,12).code==Code::denied,
            "live demotion checked even if epoch publication lags");
        require(app::menu_snapshot(Id::main,{999,false,5},12).commands.size()==main.commands.size(),
            "level999 alone does not substitute for independent Sysop authorization");
        require(app::menu_snapshot(Id::main,{10,true,5},12).commands.back().key=='@',
            "do not hardcode999 when Runtime authorizes a different threshold");
        auto forged=main;
        forged.commands.push_back({'Z',"Injected",Action::goodbye});
        require(app::route_menu(forged,'Z',ordinary,12).code==Code::invalid,"snapshot/artwork cannot introduce commands");
        require(app::menu_snapshot(Id::main,{1000,true,5},12).commands.empty(),"invalid security has no menu authority");
        auto rows=app::builtin_menu_rows(main,"@NAME@\x1b[2J", "status", "event\r\nattack");
        require(rows[rows.size()-3].text=="@NAME@ [2J","context text is inert and cannot emit terminal controls");
        require(rows[rows.size()-4].text=="event  attack","event is one inert row");
        require(app::unavailable_menu_feature=="This feature is not available yet.","approved unavailable wording");
        const auto palette=app::menu_colors();
        require(palette.size()==16 && palette.front().name=="Use BBS default","15 named colors plus default");
        for (unsigned color=0;color<16;++color) {
            app::MenuPreferences preferences{static_cast<std::uint8_t>(color),false,true};
            require(palette[color].value==color && !palette[color].name.empty(),"fixed named palette indexes");
            const std::vector<app::MenuTextRow> sample{{app::MenuTextRole::heading,"Heading"},{app::MenuTextRole::command,"Command"},
                {app::MenuTextRole::event,"Warning"},{app::MenuTextRole::status,"Status"},
                {app::MenuTextRole::feedback,"Try again"}};
            const auto plain=app::render_menu_rows(sample,false,preferences);
            require(plain=="Heading\r\nCommand\r\nWarning\r\nStatus\r\nTry again\r\n","ASCII is independent of colors");
            const auto colored=app::render_menu_rows(sample,true,preferences);
            require(colored.find(app::menu_text_sgr(7)+"Heading")==0,"every native heading matches light-gray footer, not bright white");
            require(colored.find(app::menu_text_sgr(7)+"Command")!=std::string::npos,
                "generated commands use system gray");
            require(colored.find(app::menu_text_sgr(7)+"Try again")!=std::string::npos,
                "feedback uses system gray");
            require(colored.find(app::menu_text_sgr(14)+"Warning")!=std::string::npos &&
                colored.find(app::menu_text_sgr(7)+"Status")!=std::string::npos,"semantic colors remain separate");
        }
        require(app::menu_text_sgr(4)=="\x1b[0;31;40m" && app::menu_text_sgr(9)=="\x1b[0;1;34;40m",
            "VGA indexes translated to ANSI, not treated as ANSI indexes");
        require(app::menu_text_sgr(16).empty() && !app::valid_menu_preferences({16,false,true}),"invalid color fails closed");
        using Input=app::MenuInputKind;
        for(bool hot:{false,true}) {
            app::MenuInputDecoder quick(hot,false,true);
            require(quick.feed('/').kind==Input::pending,"slash alone never logs off");
            auto result=quick.feed('g');
            if(!hot) {require(result.kind==Input::pending,"line-mode quick waits for Enter");result=quick.feed('\r');}
            require(result.kind==Input::quick_goodbye && result.value=='/',"explicit case-insensitive /G");
            require(quick.feed('\r').kind==(hot?Input::pending:Input::redraw),"quick hot-key Enter suppression");
            quick.reset(hot,false,true);
            quick.feed('/'); require(quick.feed('\r').kind==Input::invalid,"incomplete slash rejected");
            quick.feed('/'); quick.feed('Q'); require(quick.feed('\r').kind==Input::invalid,"wrong slash command rejected");
            quick.feed('/'); quick.feed(8);
            result=quick.feed('M');if(!hot)result=quick.feed('\r');
            require(result.kind==Input::command && result.value=='M',"prefix can be cancelled with Backspace");
            quick.reset(hot,false,true);quick.feed(27);
            require(quick.feed('/').kind==Input::back,"Escape tail cannot start quick logoff");
        }
        app::MenuInputDecoder no_quick(false,false);
        no_quick.feed('/');no_quick.feed('G');
        require(no_quick.feed('\r').kind==Input::invalid,"quick logoff unavailable outside approved menus");
        require(app::route_menu(app::menu_snapshot(Id::doors,ordinary,12),'/',ordinary,12).action==Action::quick_goodbye,
            "Door menu preserves approved quick logoff");
        app::MenuInputDecoder input(false,true);
        require(input.feed('1').kind==Input::pending && input.feed('2').kind==Input::pending,"numbers wait for Enter");
        auto selection=input.feed('\r');
        require(selection.kind==Input::number && selection.value==12,"multi-digit number is unambiguous");
        require(input.feed('\n').kind==Input::pending,"CRLF is one submission");
        input.feed('m'); input.feed(8); input.feed('q'); selection=input.feed('\r');
        require(selection.kind==Input::command && selection.value=='Q',"line mode editable case-insensitive command");
        input.reset(true,true);
        require(input.feed('f').value=='F' && input.feed('\r').kind==Input::pending,"hotkey immediate with coalesced Enter suppressed");
        input.feed('9'); input.feed('9');
        selection=input.feed('\r'); require(selection.kind==Input::number && selection.value==99,"hotkey numbers still await Enter");
        input.feed(27); input.feed('['); require(input.feed('A').kind==Input::up,"split up-arrow decoded");
        input.feed(27); input.feed('O'); require(input.feed('B').kind==Input::down,"SS3 down-arrow decoded");
        input.feed(27); input.feed('['); input.feed('1'); input.feed(';'); input.feed('5');
        require(input.feed('A').kind==Input::pending,"modified arrow ignored, never command A");
        for(const auto sequence:{"\x1b[C","\x1b[D","\x1bOC","\x1bOD","\x1b[1;2D"}){
            for(unsigned char byte:std::string_view(sequence))require(input.feed(byte).kind==Input::pending,"nonselecting arrow silently consumed");
        }
        input.feed(27); require(input.idle().kind==Input::back,"bare Escape returns Back");
        input.feed(27); require(input.feed('G').kind==Input::back,"Escape tail cannot trigger Goodbye");
        for(bool hotkey:{false,true}){
            app::MenuInputDecoder ordinary_input(hotkey,false);
            for(unsigned char byte:std::string("\x1b[A\x1b[B\x1b[C\x1b[D\x1bOA\x1bOB\x1bOC\x1bOD"))
                require(ordinary_input.feed(byte).kind==Input::pending,"ordinary menu ignores all arrows without commands or redraw");
        }
        for (char c:std::string("12345")) input.feed(static_cast<unsigned char>(c));
        require(input.feed('\r').kind==Input::invalid,"oversized numeric input rejected");
        input.feed('0'); require(input.feed('\r').kind==Input::invalid,"zero is not a numbered entry");
        const auto echoed=[](bool hot,std::vector<unsigned> numbers,std::string_view bytes){
            app::MenuInputDecoder decoder(hot,true,true,true);decoder.numeric_choices(std::move(numbers));
            std::string output;
            for(const unsigned char byte:bytes){const auto before=std::string(decoder.pending_text());
                const auto decoded=decoder.feed(byte);output+=app::menu_input_echo(byte,before,decoded,decoder.pending_text());}
            return output;
        };
        require(echoed(true,{1},"1")=="1\r\n","immediate numeric hotkey echoes its digit once");
        require(echoed(true,{1,12},"1\r\n")=="1\r\n","ambiguous numeric prefix plus Enter is not duplicated");
        require(echoed(true,{1,10,11},"10\r\n")=="10\r\n","numeric ten echoes raw final zero, never decoded LF");
        require(echoed(true,{1,10,11},"11\r\n")=="11\r\n","numeric eleven echoes raw final one, never decoded control");
        require(echoed(false,{1,12},"12\r\n")=="12\r\n","line mode retains exactly one echo per digit and newline");
        require(echoed(true,{1,12},"1\b12")=="1\b \b12\r\n","backspace erases a displayed prefix without losing completion");
        require(echoed(true,{1},"\x1b[A\x1b[B\x1b[C\x01").empty(),"CSI navigation and controls are never echoed as commands");
        require(echoed(true,{1},"/g\r\n")=="/G\r\n","quick Goodbye prefix is echoed once");
        for(unsigned number=1;number<=99;++number)for(bool hot:{false,true})
            require(echoed(hot,{number},std::to_string(number)+"\r\n")==std::to_string(number)+"\r\n",
                "all Door numbers echo exactly once in hotkey and line modes");
        app::NumberedMenuSnapshot numbered{4,9,{{1,"stable-a","First"},{22,"stable-b","Second"}}};
        require(app::select_numbered_menu(numbered,22,numbered)=="stable-b","direct number resolves stable identity");
        auto changed=numbered; changed.entries[1].stable_id="replacement";
        require(!app::select_numbered_menu(numbered,22,changed),"same number never reinterpreted as replacement item");
        changed=numbered; changed.access_revision++;
        require(!app::select_numbered_menu(numbered,1,changed),"access epoch forces new numbered choice");
        changed=numbered; std::swap(changed.entries[0],changed.entries[1]);
        require(!app::select_numbered_menu(numbered,22,changed),"reordered lightbar forces redraw");
        changed=numbered; changed.entries[1].number=1;
        require(!app::valid_numbered_menu(changed),"duplicate menu numbers rejected");
        std::cout<<"MENU FOUNDATION PASS checks="<<checks<<" failed=0 skipped=0\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"MENU FOUNDATION FAIL checks="<<checks<<" "<<error.what()<<'\n'; return 1;
    }
}
