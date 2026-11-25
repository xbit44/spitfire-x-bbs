#include "sfx/application/door_session.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <regex>
namespace app=sfx::application;
namespace {
unsigned checks{};void check(bool c,const char* why){++checks;if(!c)throw std::runtime_error(why);}
struct Storage final:app::IDoorStorage {
    app::DoorCatalog c;app::DoorAuthority authority{10,90};app::DoorBulletinListing bulletins{1,{}};
    app::StorageResult<app::DoorBulletinListing> door_bulletins(const app::MessageActor&)override{return {{},bulletins};}
    app::StorageResult<app::DoorCatalog> door_configuration(const app::MessageConfigurationActor&)override{return {{},c};}
    app::StorageResult<app::DoorChangeOutcome> change_door_configuration(const app::MessageConfigurationActor&,const app::DoorConfigurationChange&)override{return {{app::StorageErrorCode::invalid_argument,"unused synthetic port"},{}};}
    app::StorageResult<app::DoorCallerMenu> door_menu(const app::MessageActor&,app::DoorMenuId id)override{
        const auto* m=app::find_door_menu(c,id);if(!m)return {{app::StorageErrorCode::not_found,"test missing menu"},{}};
        if(!app::meets_door_requirements(m->access,authority))return {{app::StorageErrorCode::not_found,"test hidden category"},{}};
        if(!app::meets_door_requirements(m->execution,authority))return {{app::StorageErrorCode::permission_denied,"test category execution denied"},{}};
        auto entries=app::visible_door_entries(c,id,authority);const auto all=static_cast<std::size_t>(std::count_if(c.entries.begin(),c.entries.end(),[id](const auto& e){return e.menu==id;}));const bool hidden=entries.size()!=all;
        return {{},app::DoorCallerMenu{c.revision,*m,std::move(entries),hidden}};
    }
    app::StorageResult<app::DoorLaunch> door_launch(const app::MessageActor& a,const app::DoorLaunchRequest& r)override{
        auto menu=door_menu(a,r.menu);if(!menu||menu.value->revision!=r.expected_revision)return {{app::StorageErrorCode::conflict,"test stale launch"},{}};
        for(const auto& e:menu.value->entries)if(e.number==r.number&&e.kind==app::DoorEntryKind::door){const auto* d=app::find_door(c,e.target);if(!app::door_executable(*d,authority))return {{app::StorageErrorCode::permission_denied,"test Door execution denied"},{}};return {{},app::DoorLaunch{c.revision,*d,menu.value->menu,e}};}
        return {{app::StorageErrorCode::not_found,"test unavailable"},{}};
    }
    app::StorageResult<app::DoorEventList> door_events(const app::MessageActor&,app::DoorAutomaticEvent event)override{
        app::DoorEventList result{c.revision,{}};for(const auto& d:c.doors)if(d.automatic_event==event&&app::door_executable(d,authority))result.doors.push_back(d.id);return {{},result};
    }
    app::StorageResult<app::DoorLaunch> door_event_launch(const app::MessageActor&,const app::DoorEventLaunchRequest& r)override{
        const auto* d=app::find_door(c,r.door);if(!d||c.revision!=r.expected_revision||d->automatic_event!=r.event||!app::door_executable(*d,authority))return {{app::StorageErrorCode::conflict,"test changed event"},{}};
        return {{},app::DoorLaunch{c.revision,*d,{0,"Events"},{}}};
    }
};
struct Session {
    Storage storage;app::MenuSessionPorts menu;std::vector<app::MenuInput> input;std::string output;std::vector<std::string> paths;
    std::size_t cursor{};unsigned launches{},locations{},pauses{},paging{},menu_art{};bool ansi{},pre{},mutate{},art{};
    std::vector<unsigned> launch_numbers;
    app::DoorEnd end{app::DoorEnd::returned};app::DoorBulletinRefresh refresh{};
    Session(){app::DoorDefinition d;d.id=1;d.name="Example Door";d.enabled=true;d.rlogin.host="example.invalid";storage.c.doors={d};
        menu.snapshot=[&]()->std::optional<app::MenuSessionSnapshot>{app::MenuSessionSnapshot s;s.access={10,false,1};s.configuration_revision=1;s.graphics=ansi?app::GraphicsMode::ansi:app::GraphicsMode::ascii;s.rows=25;return s;};
        menu.write=[&](std::string_view s){output+=s;return true;};menu.menu_entered=[&](app::MenuId id){check(id==app::MenuId::doors||id==app::MenuId::door_bulletins,"typed Door/Bulletin location");++locations;};
        menu.artwork=[&](const app::TrustedDisplaySelectionRequest& r){
            if(r.identity==app::TrustedDisplayIdentity::door_prelaunch){if(!pre)return app::TrustedDisplaySelectionResult{};app::RenderedTrustedDisplayProgram p;p.identity=r.identity;p.graphics=r.graphics;p.door_asset_number=r.door_asset_number;p.selected_logical_asset="DP001.ASC";p.aggregate_source_bytes=32;p.terminal_program.operations={{app::TerminalDisplayOperationKind::paging_newline,{}},{app::TerminalDisplayOperationKind::pause,{}}};p.terminal_program.output_byte_count=4;check(app::valid_rendered_trusted_display_program(p),"synthetic authored pre-door display obeys shipping selector contract");return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::available,std::move(p)};}
            ++menu_art;paths.push_back(r.context.door_menu.value_or(""));if(!art)return app::TrustedDisplaySelectionResult{};app::RenderedTrustedDisplayProgram p;p.identity=r.identity;p.terminal_program.operations={{app::TerminalDisplayOperationKind::cp437_text,"Hidden art title"}};return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::available,std::move(p)};
        };
        menu.display=[&](const app::TerminalDisplayProgram& p){for(const auto& op:p.operations){if(op.kind==app::TerminalDisplayOperationKind::pause)++pauses;if(op.kind==app::TerminalDisplayOperationKind::paging_newline)++paging;}return true;};
        menu.read=[&](app::MenuInputDecoder&,const std::function<bool()>&)->std::optional<app::MenuInput>{if(cursor==input.size())throw std::runtime_error("unexpected Door prompt");auto choice=input[cursor++];if(mutate&&cursor==1){++storage.c.revision;storage.c.doors[0].enabled=false;}return choice;};
    }
    std::optional<app::DoorMenuExit> run(){app::DoorSessionPorts p{storage,menu,[]()->std::optional<app::MessageActor>{return app::MessageActor{};},[&](const app::DoorLaunch& l)->std::optional<app::DoorEnd>{check(l.door.id==1,"shared permanent target");++launches;launch_numbers.push_back(l.entry.number);return end;},{},refresh};return app::run_door_session(p);}
};
app::MenuInput n(unsigned v){return {app::MenuInputKind::number,v};}app::MenuInput k(char v){return {app::MenuInputKind::command,static_cast<unsigned>(v)};}
}
int main(){try{
    {Session s;for(unsigned i=1;i<=11;++i)s.storage.c.entries.push_back({0,static_cast<std::uint8_t>(i),app::DoorEntryKind::door,1,"Numbered "+std::to_string(i),""});
        s.menu.door_commands=[](app::MenuId){return std::optional<app::MenuDoorCommands>({1,1,{{'K',"Late command one"},{'L',"Late command two"},{'M',"Late command three"}}});};
        unsigned reads{};s.menu.read=[&](auto&,const auto&)->std::optional<app::MenuInput>{++reads;
            if(reads==1){check(s.output.find("Late command one")!=std::string::npos&&s.output.find("Late command two")==std::string::npos,"generated command rows share bounded first Door page");s.output.clear();return k('N');}
            check(s.output.find("Late command two")!=std::string::npos&&s.output.find("Late command three")!=std::string::npos,"remaining command rows reachable on next Door page");return k('Q');};
        check(s.run().has_value()&&reads==2,"Door commands preserve numeric menu paging");}
    {Session s;s.storage.c.menus[0].execution.level=50;const auto result=s.run();check(result&&*result==app::DoorMenuExit::main_menu&&!s.launches&&s.output.find("security clearance")!=std::string::npos,"root clearance denial returns to BBS Main Menu without Goodbye");}
    for(const bool ansi:{false,true}){
        {Session s;s.ansi=ansi;s.storage.c.doors[0].execution.level=50;s.storage.c.entries={{0,1,app::DoorEntryKind::door,1,"Visible Game",""}};s.input={n(1),k('Q')};const auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&!s.launches&&s.output.find("Visible Game")!=std::string::npos&&s.output.find("security clearance to run this Door")!=std::string::npos,"visible execution-denied Door receives SFX notification and keeps session");}
        {Session s;s.ansi=ansi;s.storage.c.menus.push_back({1,"Visible Category"});s.storage.c.menus.back().execution.level=50;s.storage.c.entries={{0,1,app::DoorEntryKind::submenu,1,"Visible Category",""}};s.input={n(1),k('Q')};const auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&!s.launches&&s.output.find("Visible Category")!=std::string::npos&&s.output.find("security clearance to enter this category")!=std::string::npos,"visible execution-denied category receives SFX notification");}
        {Session s;s.ansi=ansi;s.art=true;s.storage.c.menus.push_back({1,"Hidden Category"});s.storage.c.menus.back().access.level=50;s.storage.c.entries={{0,1,app::DoorEntryKind::submenu,1,"Hidden Category",""}};s.input={n(1),k('Q')};const auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&!s.launches&&!s.menu_art&&s.output.find("Hidden Category")==std::string::npos,"hidden category stays absent including custom artwork and numeric selection");}
    }
    for(const auto ending:{app::DoorEnd::returned,app::DoorEnd::escaped,app::DoorEnd::operator_terminated,app::DoorEnd::launch_failed}){Session s;s.end=ending;s.storage.c.menus.push_back({1,"Games"});s.storage.c.entries={{0,7,app::DoorEntryKind::submenu,1,"Games",""},{1,42,app::DoorEntryKind::door,1,"Play",""}};s.input={n(7),n(42),k('Q'),k('Q')};auto result=s.run();check(result&&*result==app::DoorMenuExit::main_menu&&s.launches==1&&s.locations==1,"recoverable exit retains caller");check(s.paths.size()==4&&s.paths[1]=="Door Menu > Games"&&s.paths[2]==s.paths[1],"exact launching menu after return");}
    {Session s;s.storage.c.menus={{0,"Door Menu"},{1,"A"},{2,"B"},{3,"Shared"}};s.storage.c.entries={{0,1,app::DoorEntryKind::submenu,1,"A",""},{0,2,app::DoorEntryKind::submenu,2,"B",""},{1,5,app::DoorEntryKind::submenu,3,"Shared",""},{2,5,app::DoorEntryKind::submenu,3,"Shared",""},{3,99,app::DoorEntryKind::door,1,"Play",""}};s.input={n(1),n(5),n(99),{app::MenuInputKind::back,0},{app::MenuInputKind::back,0},n(2),n(5),n(99),{app::MenuInputKind::back,0},{app::MenuInputKind::back,0},{app::MenuInputKind::back,0}};const auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&s.launches==2,"shared child from both parents");check(s.output.find("Door Menu > A > Shared")!=std::string::npos&&s.output.find("Door Menu > B > Shared")!=std::string::npos,"Back uses actual parent");}
    {Session s;s.storage.c.entries={{0,1,app::DoorEntryKind::door,1,"Play",""}};s.mutate=true;s.input={n(1),k('Q')};auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&!s.launches&&s.output.find("settings changed")!=std::string::npos,"stale selection cannot launch");}
    {Session s;s.art=true;s.storage.c.doors[0].sysop_only=true;s.storage.c.entries={{0,1,app::DoorEntryKind::door,1,"Hidden Door",""}};s.input={n(1),k('Q')};auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&!s.launches&&!s.menu_art&&s.output.find("Hidden")==std::string::npos,"ineligible custom-art labels hidden");}
    {Session s;s.pre=true;s.storage.c.entries={{0,1,app::DoorEntryKind::door,1,"Play",""}};s.input={n(1),k('Q')};auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&s.pauses==1&&!s.paging,"only authored pre-door pause");}
    {Session s;s.ansi=true;s.storage.c.entries={{0,1,app::DoorEntryKind::door,1,"Play",""}};s.input={{app::MenuInputKind::down,0},{app::MenuInputKind::activate,0},{app::MenuInputKind::redraw,0}};auto r=s.run();check(r&&*r==app::DoorMenuExit::main_menu&&s.launches==1&&s.output.find("Right opens; Enter goes back")==std::string::npos,"lightbar Right opens, Enter backs without visible help clutter");}
    {app::MenuInputDecoder old(false,true);old.feed(27);old.feed('[');check(old.feed('C').kind==app::MenuInputKind::pending,"retained menus ignore Right");app::MenuInputDecoder door(false,true,false,true);door.feed(27);door.feed('[');check(door.feed('C').kind==app::MenuInputKind::activate,"Door-only Right activation");}
    {Session s;s.input={k('G'),k('Y')};auto r=s.run();check(r&&*r==app::DoorMenuExit::confirmed_goodbye,"Door confirmation explicitly reports accepted normal Goodbye");}
    for(bool ansi:{false,true})for(bool hot:{false,true})for(bool nested:{false,true})
        for(const auto& answer:{std::string("\r"),std::string("\r\n"),std::string("Y"),std::string("N"),std::string("N\r\n"),std::string("\x1b")}){
        Session s;s.ansi=ansi;const auto snapshot=s.menu.snapshot;s.menu.snapshot=[&](){auto value=snapshot();value->preferences.preferences.hotkey=hot;return value;};
        s.storage.c.menus.push_back({1,"Games"});s.storage.c.entries={{0,7,app::DoorEntryKind::submenu,1,"Games",""}};
        std::string raw=(nested?std::string("7\r\n"):std::string{})+(hot?"G":"G\r\n");std::size_t offset{};bool asked{};
        s.menu.write=[&](std::string_view bytes){s.output+=bytes;if(!asked&&bytes.find("Logoff the BBS? ")!=bytes.npos){asked=true;raw+=answer;}return true;};
        s.menu.read=[&](app::MenuInputDecoder& decoder,const auto&)->std::optional<app::MenuInput>{
            while(offset<raw.size()){const auto value=decoder.feed(raw[offset++]);if(value.kind!=app::MenuInputKind::pending)return value;}
            const auto idle=decoder.idle();return idle.kind==app::MenuInputKind::pending?std::nullopt:std::optional<app::MenuInput>{idle};};
        const auto result=s.run();const bool yes=answer.front()=='Y'||answer.front()=='\r';
        check(asked&&((yes&&result&&*result==app::DoorMenuExit::confirmed_goodbye)||(!yes&&!result)),"actual Door decoder accepts first Enter/Y and resumes after N/Escape");
        check(!s.launches&&s.paths.back()==(nested?"Door Menu > Games":"Door Menu"),"cancel retains the exact category before later caller input loss");
        check(ansi||s.output.find('\x1b')==s.output.npos,"Door confirmation and cancel remain ANSI-free in plain mode");
    }
    for(const auto cancel:{k('N'),app::MenuInput{app::MenuInputKind::back,0}}){
        Session s;s.ansi=true;s.storage.c.menus.push_back({1,"Games"});s.storage.c.entries={{0,7,app::DoorEntryKind::submenu,1,"Games",""}};
        for(unsigned number=1;number<=14;++number)s.storage.c.entries.push_back({1,static_cast<std::uint8_t>(number),app::DoorEntryKind::door,1,"Game "+std::to_string(number),""});
        s.input={n(7),k('N'),{app::MenuInputKind::down,0},{app::MenuInputKind::down,0},k('G'),cancel,{app::MenuInputKind::activate,0},k('Q'),k('Q')};
        const auto result=s.run();
        check(result&&*result==app::DoorMenuExit::main_menu&&s.launch_numbers==std::vector<unsigned>{14},"cancelled Goodbye retains page2 and exact lightbar target14");
        check(std::count(s.paths.begin(),s.paths.end(),"Door Menu")==2,"cancel does not re-enter or redraw Door root between category visits");
    }
    for(bool preference_change:{false,true}){Session s;std::uint64_t revision=1;const auto snapshot=s.menu.snapshot;s.menu.snapshot=[&](){auto value=snapshot();if(preference_change)value->preferences.revision=revision;else value->configuration_revision=revision;return value;};
        s.input={k('G'),k('Y'),k('Q')};const auto read=s.menu.read;s.menu.read=[&](auto& decoder,const auto& redraw){const auto choice=read(decoder,redraw);if(choice&&choice->value=='Y')++revision;return choice;};
        const auto result=s.run();check(result&&*result==app::DoorMenuExit::main_menu&&s.output.find("Settings or access changed")!=s.output.npos,"changed authority/configuration cannot accept a stale Door Goodbye question");}
    {Session s;s.input={k('G'),k('Y')};unsigned actor_reads{};
        app::DoorSessionPorts ports{s.storage,s.menu,[&]()->std::optional<app::MessageActor>{return ++actor_reads<=2?std::optional<app::MessageActor>{app::MessageActor{}}:std::nullopt;},[&](const auto&)->std::optional<app::DoorEnd>{++s.launches;return app::DoorEnd::returned;}};
        check(!app::run_door_session(ports)&&actor_reads==3&&!s.launches,"actor loss at Door confirmation cannot become an accepted Goodbye");}
    for(bool hot:{false,true})for(bool ansi:{false,true})for(bool nested:{false,true})for(const auto* quick:{"/G\r","/g\r"}){
        Session s;s.ansi=ansi;auto snapshot=s.menu.snapshot;s.menu.snapshot=[&](){auto state=snapshot();state->preferences.preferences.hotkey=hot;return state;};
        s.storage.c.menus.push_back({1,"Games"});s.storage.c.entries={{0,7,app::DoorEntryKind::submenu,1,"Games",""}};
        std::string bytes=(nested?std::string("7\r"):std::string{})+quick;std::size_t offset{};
        s.menu.read=[&](app::MenuInputDecoder& decoder,const auto&)->std::optional<app::MenuInput>{while(offset<bytes.size()){const auto value=decoder.feed(bytes[offset++]);if(value.kind!=app::MenuInputKind::pending)return value;}return std::nullopt;};
        const auto result=s.run();check(result&&*result==app::DoorMenuExit::quick_goodbye,"actual Door decoder accepts /G and /g at root and child with hotkeys on/off");check(!s.launches&&s.output.find("Quick Goodbye")!=std::string::npos,"quick command is presented and never launches a Door");
    }
    for(auto ending:{app::DoorEnd::caller_disconnected,app::DoorEnd::shutdown,app::DoorEnd::authority_lost,app::DoorEnd::calling_time,app::DoorEnd::inactivity}){Session s;s.end=ending;s.storage.c.entries={{0,1,app::DoorEntryKind::door,1,"Play",""}};s.input={n(1)};check(!s.run(),"caller lifetime endings do not revive a menu");}
    for(const bool ansi:{false,true})for(const unsigned branch:{1U,2U}){
        Session s;s.ansi=ansi;s.storage.c.menus={{0,"Door Menu"},{1,"Games A"},{2,"Games B"},{3,"Maps"}};
        s.storage.c.entries={{0,8,app::DoorEntryKind::submenu,1,"Games A",""},{0,27,app::DoorEntryKind::submenu,2,"Games B",""},{1,4,app::DoorEntryKind::submenu,3,"Maps",""},{2,4,app::DoorEntryKind::submenu,3,"Maps",""},{3,19,app::DoorEntryKind::door,1,"Play",""}};
        s.input={n(branch==1?8:27),n(4),n(19),k('Q'),n(4),k('B'),k('D'),k('Q')};
        const auto result=s.run();const auto main=std::string("Door Menu > Games ")+(branch==1?"A":"B");
        check(result&&*result==app::DoorMenuExit::main_menu&&s.launches==1,"Q category return does not disconnect caller or launch a game command");
        check(s.paths==std::vector<std::string>({"Door Menu",main,main+" > Maps",main+" > Maps",main,main+" > Maps",main,"Door Menu"}),"Q uses entered branch main list; B parent and D root preserved");
        check(s.output.find(std::string("Back to Games ")+(branch==1?"A":"B"))!=std::string::npos,"Q generated label names actual branch destination");
        check(ansi||s.output.find('\x1b')==std::string::npos,"category Q plain output has no ANSI");
    }
    for(unsigned first=1;first<=99;++first)for(unsigned second=1;second<=99;++second){
        app::MenuInputDecoder decoder(true,true);decoder.numeric_choices({first,second});app::MenuInput result;
        const auto a=std::to_string(first),b=std::to_string(second);
        for(const auto digit:a)result=decoder.feed(digit);
        const bool prefix=b.size()>a.size()&&b.compare(0,a.size(),a)==0;
        check(result.kind==(prefix?app::MenuInputKind::pending:app::MenuInputKind::number),"numeric hotkeys wait for every possible longer valid number");
        if(prefix)result=decoder.feed('\r');
        check(result.kind==app::MenuInputKind::number&&result.value==first,"Enter explicitly resolves a valid prefix number");
    }
    {app::MenuInputDecoder decoder(true,true);decoder.numeric_choices({1,12,123});check(decoder.feed('1').kind==app::MenuInputKind::pending&&decoder.feed('2').kind==app::MenuInputKind::pending&&decoder.feed('3').value==123,"arbitrary prefix depth, not hardcoded digits");}
    for(const bool hotkey:{false,true}){app::MenuInputDecoder decoder(hotkey,true);if(!hotkey)decoder.numeric_choices({8});check(decoder.feed('8').kind==app::MenuInputKind::pending&&decoder.feed('\r').value==8,"disabled hotkeys and menus without numeric opt-in retain Enter");}
    for(const bool ansi:{false,true}){
        Session s;s.ansi=ansi;const auto original=s.menu.snapshot;s.menu.snapshot=[&](){auto value=original();value->preferences.preferences.hotkey=true;value->context.time_on="59";value->context.time_left="59640";return value;};
        s.storage.c.entries={{0,2,app::DoorEntryKind::door,1,"Two",""},{0,20,app::DoorEntryKind::door,1,"Twenty",""}};
        bool selected{};s.menu.read=[&](app::MenuInputDecoder& decoder,const auto&)->std::optional<app::MenuInput>{if(selected)return k('Q');selected=true;check(decoder.feed('2').kind==app::MenuInputKind::pending,"Door session supplies complete numeric choice set");return decoder.feed('0');};
        check(s.run().has_value()&&s.launches==1,"actual Door session numeric hotkey launches complete choice");
        const auto visible=std::regex_replace(s.output,std::regex("\x1b\\[[0-9;]*m"),"");
        check(visible.find("You have been on 0 minutes with 994 remaining.")!=std::string::npos&&visible.find("ENTER COMMAND - [")!=std::string::npos,"minute footer uses actual session times, ignoring ANSI color changes");
        for(const auto* removed:{"Remote Door:","Up/Down select", "It is presently", "Door number or command"})check(s.output.find(removed)==std::string::npos,"old Door footer removed");
        check(ansi||s.output.find('\x1b')==std::string::npos,"ASCII Door footer stays ANSI-free");
    }
    {Session s;auto& d=s.storage.c.doors.front();d.automatic_event=app::DoorAutomaticEvent::logon;d.event_only=true;s.storage.c.entries={{0,1,app::DoorEntryKind::door,1,"Event",""}};
        check(s.storage.door_menu({},0).value->entries.empty(),"event-only absent from caller choices");
        auto second=d;second.id=2;second.name="Second event";s.storage.c.doors.push_back(second);std::vector<unsigned> seen;unsigned outcomes{};
        app::DoorSessionPorts p{s.storage,s.menu,[]()->std::optional<app::MessageActor>{return app::MessageActor{};},[&](const app::DoorLaunch& l)->std::optional<app::DoorEnd>{seen.push_back(l.door.id);return app::DoorEnd::returned;},[&](auto,auto,auto){++outcomes;}};
        check(app::run_door_events(p,app::DoorAutomaticEvent::logon)&&seen==std::vector<unsigned>({1,2})&&outcomes==2,"events run once each in permanent-ID order without requiring a menu placement");
        seen.clear();check(app::run_door_events(p,app::DoorAutomaticEvent::logoff)&&seen.empty(),"only matching event runs");
        p.launch=[&](const auto&)->std::optional<app::DoorEnd>{++s.launches;return app::DoorEnd::caller_disconnected;};
        check(!app::run_door_events(p,app::DoorAutomaticEvent::logon)&&s.launches==1,"carrier loss prevents later event launch and does not recurse into logoff");
        p.launch=[&](const auto&)->std::optional<app::DoorEnd>{++s.launches;++s.storage.c.revision;return app::DoorEnd::returned;};s.launches=0;
        check(app::run_door_events(p,app::DoorAutomaticEvent::logon)&&s.launches==1,"catalog change refuses remaining captured events");
        p.launch=[&](const auto&)->std::optional<app::DoorEnd>{++s.launches;return app::DoorEnd::busy;};s.launches=0;
        check(app::run_door_events(p,app::DoorAutomaticEvent::logon)&&s.launches==2,"busy event skips without failing caller logon");
    }
    for(const bool ansi:{false,true})for(const bool hot:{false,true})for(const bool nested:{false,true}){
        Session s;s.ansi=ansi;const auto snapshot=s.menu.snapshot;s.menu.snapshot=[&](){auto v=snapshot();v->preferences.preferences.hotkey=hot;return v;};
        s.storage.c.menus.push_back({1,"Games"});s.storage.c.entries={{0,7,app::DoorEntryKind::submenu,1,"Games",""}};
        s.storage.bulletins.entries={{{1},"Door news",1,app::DoorBulletinHealth::available},{{999},"Permanent last ID",1,app::DoorBulletinHealth::unavailable}};
        std::string raw=(nested?std::string("7\r\n"):std::string{})+"X\r\n999\r\nG\r\n/G\r\nQ\r\n"+(nested?"Q\r\n":"")+"Q\r\n";std::size_t offset{};unsigned refreshes{};
        s.menu.read=[&](app::MenuInputDecoder& decoder,const auto&)->std::optional<app::MenuInput>{while(offset<raw.size()){const auto r=decoder.feed(raw[offset++]);if(r.kind!=app::MenuInputKind::pending)return r;}return std::nullopt;};
        s.refresh=[&](app::DoorBulletinId id,const auto&,auto){++refreshes;check(id.value==999,"full-width999 selected from raw bytes");return app::DoorBulletinRefreshResult{};};
        const auto result=s.run();check(result&&*result==app::DoorMenuExit::main_menu&&refreshes==1&&!s.launches,"X/Q CRLF coalesces across shared nested decoder; G and /G never log off from bulletin reader");check(s.output.find(app::kDoorBulletinUnavailable)!=s.output.npos,"unavailable enabled source has exact path-free caller message");check(s.paths.size()>=2&&s.paths.back()=="Door Menu","return from bulletin preserves parent then Q returns root");check(ansi||s.output.find('\x1b')==s.output.npos,"plain Bulletin list and controls contain no ANSI");
    }
    {Session s;s.art=true;s.input={k('X'),k('Q'),k('Q')};check(s.run().has_value()&&s.output.find("No Door Bulletins are enabled.")!=s.output.npos&&s.output.find("Door Bulletins")!=s.output.npos,"fixed X remains usable with custom art and empty root/index");}
    {Session s;for(unsigned id=1;id<=20;++id)s.storage.bulletins.entries.push_back({{static_cast<std::uint16_t>(id)},"News "+std::to_string(id),1,{}});s.input={k('X'),k('N'),k('P'),k('Q'),k('Q')};check(s.run().has_value()&&s.output.find("2/2")!=s.output.npos&&s.output.find("1/2")!=s.output.npos,"bulletin N/P paging follows terminal height and returns to invoker");}
    std::cout<<"PASS "<<checks<<" Door session checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
