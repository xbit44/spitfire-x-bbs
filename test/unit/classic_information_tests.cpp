#include "sfx/application/classic_information.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/terminal_stream.hpp"
#include "../../src/apps/runtime/caller_runtime_helpers.hpp"
#include <deque>
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
namespace app=sfx::application;
std::string visible(std::string_view s){std::string out;for(std::size_t i=0;i<s.size();){if(s[i]=='\x1b'&&i+1<s.size()&&s[i+1]=='['){i+=2;while(i<s.size()&&!(s[i]>=0x40&&s[i]<=0x7e))++i;if(i<s.size())++i;}else out+=s[i++];}return out;}
unsigned checks=0;
void check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
struct Reader:app::ITrustedDisplayAssetReader {
    std::map<std::string,std::pair<std::string,std::int64_t>> files;
    app::TrustedDisplayAssetReadResult read_asset(std::string_view name,std::size_t maximum) override {
        const auto it=files.find(std::string(name));
        if(it==files.end()) return {app::TrustedDisplayAssetReadCode::not_found,{},{},{}};
        if(it->second.first.size()>maximum) return {app::TrustedDisplayAssetReadCode::invalid,{},{},{}};
        return {app::TrustedDisplayAssetReadCode::available,it->first,it->first,it->second.first,it->second.second};
    }
};
struct Fixture {
    Reader reader;app::TrustedDisplaySelector selector{reader};app::MenuSessionSnapshot state;
    app::MenuSessionPorts ports;std::string input,output;unsigned frames=0,views=0;
    Fixture(std::string keys,bool ansi=false):input(std::move(keys)) {
        state.access={10,false,1};state.preferences={1,{5,true,true},1,0};state.graphics=ansi?app::GraphicsMode::ansi:app::GraphicsMode::ascii;
        state.context.name="Test Caller";
        ports.snapshot=[&]{return std::optional<app::MenuSessionSnapshot>{state};};
        ports.artwork=[&](const auto& r){return selector.select(r);};
        ports.write=[&](std::string_view value){output+=value;return true;};
        ports.display=[&](const app::TerminalDisplayProgram& program){
            check(app::valid_terminal_display_program(program),"valid bounded display");++views;
            for(const auto& op:program.operations) {output+=op.bytes;
                if(op.kind==app::TerminalDisplayOperationKind::logical_newline || op.kind==app::TerminalDisplayOperationKind::paging_newline) output+="\r\n";}
            return true;
        };
        ports.read=[&](app::MenuInputDecoder& decoder,const std::function<bool()>&)->std::optional<app::MenuInput>{
            check(++frames<30,"bounded native input");
            while(!input.empty()) {const auto key=static_cast<unsigned char>(input.front());input.erase(input.begin());
                const auto choice=decoder.feed(key);if(choice.kind!=app::MenuInputKind::pending)return choice;}
            return std::nullopt;
        };
    }
};
struct WireStream:app::IInteractiveSessionByteStream {
    Fixture& fixture;
    explicit WireStream(Fixture& value):fixture(value){}
    app::InteractiveStreamReadResult read_some(std::size_t maximum)override{
        if(!maximum||fixture.input.empty())return {app::InteractiveStreamOutcome::disconnected,{}};
        const auto byte=static_cast<std::uint8_t>(fixture.input.front());fixture.input.erase(fixture.input.begin());
        return {app::InteractiveStreamOutcome::completed,{byte}};
    }
    app::InteractiveStreamWriteResult write_some(std::string_view bytes)override{fixture.output+=bytes;return {app::InteractiveStreamOutcome::completed,bytes.size()};}
};
struct Terminal:app::ICallerTerminal {
    std::deque<char> keys;std::deque<std::string> values;std::string output;
    std::function<void(std::string_view)> on_key;
    app::TerminalOutcome set_text_style(app::TerminalTextStyle)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome clear_screen()override{return app::TerminalOutcome::completed;}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& request)override {
        output+=request.prompt;if(values.empty())return {app::TerminalOutcome::disconnected,{}};
        auto value=values.front();values.pop_front();return {value.empty()?app::TerminalOutcome::empty:app::TerminalOutcome::completed,value};
    }
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{return {app::TerminalOutcome::invalid,{}};}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest& request)override {
        output+=request.prompt;if(keys.empty())return {app::TerminalOutcome::disconnected,{}};
        if(on_key)on_key(request.identity);
        const auto key=keys.front();keys.pop_front();return {app::TerminalOutcome::completed,key};
    }
    app::TerminalOutcome present(const app::TerminalPresentationRequest&)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest&)override{return app::TerminalOutcome::invalid;}
    app::TerminalOutcome pause(const app::TerminalPauseRequest&)override{return app::TerminalOutcome::completed;}
};
int main(int argc,char** argv){try {
    if(argc==2 && std::string(argv[1])=="--boards-frame") {
        Fixture sample("",true);Terminal terminal;terminal.keys={'Q'};
        const auto result=app::run_other_board_session(sample.ports,terminal,[](std::int64_t){
            return app::StorageResult<app::OtherBoardPage>{{},app::OtherBoardPage{{
                {1,1,{"32-Bit BBS","x-bit.org:23230",""}},
                {2,1,{"uniX-Bit BBS","x-bit.org:1336","x-bit.org:1337"}},
                {3,1,{"X-Bit BBS","x-bit.org:23","x-bit.org:22222"}}},false}};
        },[](const auto&){return false;});
        std::cout<<sample.output;return result?0:1;
    }
    for(bool ansi:{false,true})for(bool hotkey:{false,true})for(const std::string newline:{"\r","\n","\r\n"}){
        for(const std::string action:{"1","N","L","Z","9"}){
            const bool acknowledge=action=="1"||action=="N";
            const auto submitted=[&](std::string key){return key+(!hotkey||key=="9"?newline:std::string{});};
            Fixture sample(submitted(action)+(acknowledge?newline:std::string{})+submitted("Z")+newline+"!",ansi);
            sample.state.preferences.preferences.hotkey=hotkey;sample.reader.files["BULLET1.ASC"]={"REAL ACKNOWLEDGED PAGE",1};
            WireStream stream(sample);app::StreamCallerTerminal terminal(stream);
            check(terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain)==app::TerminalOutcome::completed,"real terminal style selected");
            sample.ports.display=[&](const app::TerminalDisplayProgram& program){++sample.views;return terminal.present_display({"menu.artwork",&program,{true,std::uint16_t{24}}})==app::TerminalOutcome::completed;};
            sample.ports.read=[&](app::MenuInputDecoder& decoder,const std::function<bool()>&)->std::optional<app::MenuInput>{
                sfx::apps::runtime::MenuDelimiterStream menu(stream,terminal,decoder);
                for(unsigned attempt=0;attempt<100;++attempt){
                    const auto read=menu.read_some(1);if(read.outcome!=app::InteractiveStreamOutcome::completed)return std::nullopt;
                    for(const auto byte:read.bytes){const auto choice=decoder.feed(byte);if(choice.kind!=app::MenuInputKind::pending)return choice;}
                }return std::nullopt;
            };
            const auto result=app::run_bulletin_session(sample.ports,{});
            const auto context="actual modal page/menu Enter action="+action+", hotkey="+std::to_string(hotkey)+", newline="+std::to_string(static_cast<unsigned char>(newline[0]))+"/"+std::to_string(newline.size());
            check(result==app::BulletinOutcome::continued,(context+" returns after one independent menu Enter").c_str());
            check(sample.output.find("Choose a bulletin number, L, N, or Enter.")!=std::string::npos,(context+" does not treat page/selection CRLF tail as menu Enter").c_str());
            app::MenuInputDecoder next(true,false);const auto parent=sample.ports.read(next,[]{return true;});
            check(parent&&parent->kind==app::MenuInputKind::command&&parent->value=='!',(context+" preserves following parent-menu command").c_str());
        }
    }
    for(const std::string newline:{"\r","\n","\r\n"}){
        Fixture sample("M"+newline+"Alice"+newline+"Q"+newline+"!",false);WireStream stream(sample);
        sfx::apps::runtime::MenuModalStream modal(stream,[]{return true;},[]{});app::StreamCallerTerminal terminal(modal);
        const auto command=[&](app::MenuInputDecoder& decoder)->std::optional<app::MenuInput>{
            sfx::apps::runtime::MenuDelimiterStream menu(stream,terminal,decoder);
            for(unsigned i=0;i<40;++i){const auto read=menu.read_some(1);if(read.outcome!=app::InteractiveStreamOutcome::completed)return std::nullopt;
                const auto value=decoder.feed(read.bytes.front());if(value.kind!=app::MenuInputKind::pending)return value;}return std::nullopt;
        };
        app::MenuInputDecoder first(false,false);const auto m=command(first);check(m&&m->value=='M',"ordinary line menu consumes its own selection delimiter");
        const auto name=terminal.read_visible({"test.form","Name: ",20,app::EmptyInputPolicy::reject,{}});check(name.outcome==app::TerminalOutcome::completed&&name.value=="Alice","modal visible form receives full next input without menu CRLF tail");
        app::MenuInputDecoder second(false,false);const auto q=command(second);check(q&&q->value=='Q',"ordinary menu resumes after form without treating form CRLF tail as empty command");
        app::MenuInputDecoder parent(true,false);const auto next=command(parent);check(next&&next->value=='!',"following parent hotkey remains owned and readable");
    }
    check(app::information_updated({},{}),"first login treats available content as new");
    check(!app::information_updated({},100),"unknown timestamp cannot fabricate update");
    check(!app::information_updated(1000000000,100),"equal timestamp unchanged");
    check(app::information_updated(1000000001,100),"subsecond modification is later");
    for(bool ansi:{false,true}) {
        for(bool hotkey:{false,true})for(const std::string newline:{"\r","\n","\r\n"}) {
            for(const std::string action:{"","1","Z","L","N","9"}) {
                const auto keys=action+(!action.empty()&&(!hotkey||action=="9")?newline:std::string{})+newline;
                Fixture returned(keys,ansi);returned.state.preferences.preferences.hotkey=hotkey;
                returned.reader.files["BULLET1.ASC"]={"ACKNOWLEDGED PAGE",1};
                check(app::run_bulletin_session(returned.ports,{})==app::BulletinOutcome::continued,
                    ("one independent menu Enter exits after action="+action+", hotkey="+std::to_string(hotkey)+", newline="+std::to_string(static_cast<unsigned char>(newline[0]))+"/"+std::to_string(newline.size())).c_str());
            }
        }
        Fixture hotkeys("12L1Q",ansi);
        hotkeys.reader.files["BULLET1.ASC"]={"FIRST PAGE",1};
        hotkeys.reader.files["BULLET2.ASC"]={"SECOND PAGE",1};
        check(app::run_bulletin_session(hotkeys.ports,{})==app::BulletinOutcome::continued,"single bulletin digits open immediately and reload retains choices");
        check(hotkeys.output.find("FIRST PAGE")!=std::string::npos&&hotkeys.output.find("SECOND PAGE")!=std::string::npos,"both immediate bulletin choices display");
        Fixture prefix("1\r10Q",ansi);
        prefix.reader.files["BULLET1.ASC"]={"PREFIX ONE",1};
        prefix.reader.files["BULLET10.ASC"]={"TEN PAGE",1};
        check(app::run_bulletin_session(prefix.ports,{})==app::BulletinOutcome::continued&&prefix.output.find("TEN PAGE")!=std::string::npos,"ambiguous one waits for Enter while ten opens immediately");
        Fixture waiting("1",ansi);waiting.reader.files["BULLET1.ASC"]={"MUST WAIT",1};waiting.reader.files["BULLET10.ASC"]={"TEN",1};
        check(app::run_bulletin_session(waiting.ports,{})==app::BulletinOutcome::failed&&waiting.output.find("MUST WAIT")==std::string::npos,"ambiguous prefix never guessed at end of input");
        Fixture line("1",ansi);line.state.preferences.preferences.hotkey=false;line.reader.files["BULLET1.ASC"]={"MUST WAIT",1};
        check(app::run_bulletin_session(line.ports,{})==app::BulletinOutcome::failed&&line.output.find("MUST WAIT")==std::string::npos,"line mode requires Enter even for unique choice");
    }
    for(bool ansi:{false,true}) {
        Fixture empty("",ansi);
        check(app::run_bulletin_session(empty.ports,100,true)==app::BulletinOutcome::continued,"absent login bulletin bypasses only stage");
        check(empty.output.empty() && empty.frames==0,"no unsolicited absent-stage pause");
        Fixture menu("\r",ansi);menu.reader.files["BULLETIN.ASC"]={"CUSTOM ONLY",900000000};
        menu.reader.files["BULLETIN10.ASC"]={"MUST NOT OVERRIDE",2000000000};
        check(app::run_bulletin_session(menu.ports,100,true)==app::BulletinOutcome::continued,"custom menu without pages still shown");
        check(menu.output.find("CUSTOM ONLY")!=std::string::npos && menu.output.find("MUST NOT") ==std::string::npos,"regular artwork unsuffixed");
        Fixture pages("N\rL\r7\r8\r100\r\r",ansi);
        pages.state.preferences.preferences.hotkey=false;
        pages.reader.files["BULLET7.ASC"]={"SEVENTH",1000000001};
        pages.reader.files["BULLET99.ASC"]={"LAST",900000000};
        pages.reader.files["BULLET100.ASC"]={"NOT IN RANGE",2000000000};
        pages.reader.files["BULLET8.ASC"]={"@PASSWORD@",2000000000};
        check(app::run_bulletin_session(pages.ports,100)==app::BulletinOutcome::continued,"native lists/numbers/Enter");
        check(pages.output.find("Bulletin 7")!=std::string::npos && pages.output.find("Bulletin 99")!=std::string::npos,"gaps preserve fixed generic labels");
        check(pages.output.find("#7")!=std::string::npos && pages.output.find("LIST BULLETINS")==std::string::npos,"New lists numbers; L redraws menu");
        check(pages.output.find("Test Caller, the following bulletin(s) have\r\n")!=std::string::npos && pages.output.find("been updated since your last log-on... #7")!=std::string::npos,
            "OGSF update wording uses actual caller and current numbers");
        check(!ansi || pages.output.find(app::menu_text_sgr(7)+"Test Caller")!=std::string::npos,"bulletin update notice uses system color");
        check(!ansi || pages.output.find(app::menu_text_sgr(7)+"Bulletin <#>")!=std::string::npos,"bulletin command row remains light gray");
        check(pages.output.find("[G]oodbye")==std::string::npos && pages.output.find("It is presently")==std::string::npos &&
            pages.output.find("Bulletin <#>, <L>ist Bulletins, <N>ew Bulletins, < ENTER > To Continue? ")!=std::string::npos,
            "Bulletin has replacement prompt, no goodbye or live footer");
        check(pages.output.find("SEVENTH")!=std::string::npos && pages.output.find("LAST")==std::string::npos,"new list does not automatically open pages");
        check(pages.output.find("Bulletin 8 is unavailable")!=std::string::npos && pages.output.find("NOT IN RANGE")==std::string::npos,"invalid/missing/out-of-range safe");
        Fixture both("7\r\r",ansi);both.reader.files["BULLET7.ASC"]={"PLAIN",2000000000};both.reader.files["BULLET7.ANS"]={"\x1b[32mAUTHORED",900000000};
        const auto selected=app::available_bulletins(both.ports,both.state);
        check(selected.size()==1 && selected.front().modified_utc_100ns==(ansi?900000000:2000000000),"selected-format timestamp, not unrelated companion");
        check(app::run_bulletin_session(both.ports,{})==app::BulletinOutcome::continued,"first-login page viewing");
        check(both.output.find(ansi?"AUTHORED":"PLAIN")!=std::string::npos,"ANS preference and ASCII-only selection");
        check(ansi?both.output.find("\x1b[32m")!=std::string::npos:both.output.find('\x1b')==std::string::npos,"authored color preserved / ASCII escape-free");
        Fixture no("N",ansi);no.reader.files["SFNWSLTR.ASC"]={"NEWS",2000000000};
        check(app::offer_login_newsletter(no.ports,100),"newsletter independently offered");
        check(no.output.find("The newsletter has been updated!")!=std::string::npos && visible(no.output).find("Would you like to review it? [Y/n]")!=std::string::npos && no.output.find("NEWS")==std::string::npos,"exact prompt / No does not display");
        check(no.output.rfind("\r\n\r\n",0)==0,"newsletter exits prior prompt and leaves one blank row");
        check(!ansi || no.output.find(app::menu_text_sgr(7)+"The newsletter has been updated!\r\n")!=std::string::npos,"newsletter notice uses system color on its own row");
        check(!ansi || no.output.find(app::system_menu_text("Would you like to review it? [Y/n]",true))!=std::string::npos,"newsletter review question uses the system gray");
        Fixture yes("\r",ansi);yes.reader.files["SFNWSLTR.ASC"]={"NEWS",1000000000};
        check(app::offer_login_newsletter(yes.ports,{}) && yes.output.find("NEWS")!=std::string::npos,"first-login Enter defaults Yes and uses accepted viewer");
        Fixture unchanged("",ansi);unchanged.reader.files["SFNWSLTR.ASC"]={"NEWS",1000000000};
        check(app::offer_login_newsletter(unchanged.ports,100) && unchanged.output.empty(),"unchanged newsletter adds no offer");
        Fixture system("",ansi);system.state.context.bbs_name="Test BBS";system.state.context.sysop="Public Sysop";
        system.state.context.version="0.0.3-alpha";system.state.context.node="3";
        check(app::show_system_information(system.ports,{54,2,10},"1984-05-12"),"system info native bounded viewer");
        check(system.output.find("Total calls: 54")!=std::string::npos && system.output.find("Calls today: 2")!=std::string::npos && system.output.find("Active callers: 10")!=std::string::npos && system.output.find("Current node: 3")!=std::string::npos,"real counters/node not invented engine counts");
        check(system.output.find("This BBS began on: 1984-05-12")!=std::string::npos && system.output.find("Password")==std::string::npos,"date and public identity only");
        Fixture boards("",ansi);Terminal terminal;terminal.keys={'N','P','A'};terminal.values={"Example Board","bbs.example:2323",""};
        unsigned adds=0;std::vector<std::int64_t> cursors;
        const auto result=app::run_other_board_session(boards.ports,terminal,[&](std::int64_t after) {
            cursors.push_back(after);return app::StorageResult<app::OtherBoardPage>{{},app::OtherBoardPage{{{after?40:7,1,{after?"Second":"First","bbs.example",""}}},after==0}};
        },[&](const app::OtherBoardValues& value){++adds;return app::valid_other_board_values(value);});
        check(result && result->find("added and published")!=std::string::npos && adds==1 && cursors==std::vector<std::int64_t>{0,7,0},"Other BBS paging/back/add immediate publication");
        check(terminal.output.find("Edit")==std::string::npos && terminal.output.find("Remove")==std::string::npos,"no caller or submitter edit authority offered");
        check(boards.output.find("7. First")==std::string::npos && boards.output.find("40. Second")==std::string::npos,
            "ordinary Other BBS listing has no entry numbers");
        check(visible(boards.output).find("[D]elete")==std::string::npos,"ordinary caller never sees Delete");
        check(!ansi || (boards.output.find(app::menu_text_sgr(8)+">>>>>>>> "+app::menu_text_sgr(7)+"OTHER BULLETIN BOARDS")!=std::string::npos &&
            boards.output.find(app::menu_text_sgr(15)+"First")!=std::string::npos),"left-aligned gray heading, dark arrows and white names");
        Fixture aligned("",ansi);Terminal aligned_terminal;aligned_terminal.keys={'Q'};
        check(app::run_other_board_session(aligned.ports,aligned_terminal,[](std::int64_t){return app::StorageResult<app::OtherBoardPage>{{},app::OtherBoardPage{{{1,1,{"Board","host:23","host:22"}}},false}};},[](const auto&){return false;}).has_value(),"aligned address display completes");
        check(aligned.output.find("Telnet: host:23\r\nSSH:    host:22")!=std::string::npos,"Telnet and SSH addresses share column eight");
        check(ansi || aligned.output.find('\x1b')==std::string::npos,"plain Other BBS display remains escape-free");
        for(const auto level:{10,998,999}) {
            Fixture deletion("",ansi);deletion.state.access={static_cast<std::uint16_t>(level),true,1};
            Terminal deleting;deleting.keys={'D','Y','Q'};deleting.values={"7"};unsigned removals=0;
            const auto deleted=app::run_other_board_session(deletion.ports,deleting,[](std::int64_t){
                return app::StorageResult<app::OtherBoardPage>{{},app::OtherBoardPage{{{7,3,{"Shown Board","bbs.example",""}}},false}};
            },[](const auto&){return false;},[&](const app::OtherBoardRecord& shown){++removals;return shown.id==7 && shown.revision==3;});
            check(deleted.has_value() && removals==(level==999?1U:0U),"only level999 may invoke caller delete");
            check((visible(deletion.output).find("[D]elete")!=std::string::npos)==(level==999),"Delete is hidden below999 including lower Sysops");
            check(level!=999 || deletion.output.find("7. Shown Board")!=std::string::npos,"Sysop gets stable selection numbers only inside Delete");
            check(level!=999 || deleting.output.find("Delete entry 7 (Shown Board)? [y/N]")!=std::string::npos,"stable shown identity confirmed before removal");
        }
        Fixture stale("",ansi);stale.state.access={999,true,1};Terminal cancelling;cancelling.keys={'D','N','D','Y'};cancelling.values={"7","7"};unsigned calls=0;
        const auto refused=app::run_other_board_session(stale.ports,cancelling,[](std::int64_t){
            return app::StorageResult<app::OtherBoardPage>{{},app::OtherBoardPage{{{7,3,{"Board","bbs.example",""}}},false}};
        },[](const auto&){return false;},[&](const auto&){++calls;return false;});
        check(refused && refused->find("entry/account changed")!=std::string::npos && calls==1,"No cancels safely; stale CAS refusal never reports success");
        Fixture demoted("",ansi);demoted.state.access={999,true,1};Terminal revoking;revoking.keys={'D','Y'};revoking.values={"7"};calls=0;
        revoking.on_key=[&](std::string_view id){if(id=="other.boards.delete.confirm")demoted.state.access={10,false,2};};
        const auto revoked=app::run_other_board_session(demoted.ports,revoking,[](std::int64_t){
            return app::StorageResult<app::OtherBoardPage>{{},app::OtherBoardPage{{{7,3,{"Board","bbs.example",""}}},false}};
        },[](const auto&){return false;},[&](const auto&){++calls;return true;});
        check(revoked && revoked->find("authority changed")!=std::string::npos && calls==0,"demotion during confirmation cannot invoke mutation");
        Fixture unauthorized("",ansi);unauthorized.state.access={999,false,1};Terminal hidden;hidden.keys={'Q'};
        check(app::run_other_board_session(unauthorized.ports,hidden,[](std::int64_t){return app::StorageResult<app::OtherBoardPage>{{},app::OtherBoardPage{}};},
            [](const auto&){return false;},[](const auto&){return true;}).has_value() && visible(unauthorized.output).find("[D]elete")==std::string::npos,
            "999 without independent Sysop authority never exposes Delete");
        for(const auto& source:{std::string{"TAIL"},std::string{"TAIL\r\n"},std::string{"TAIL\r\n\r\n"}}) {
            Fixture spacing("7\r\r",ansi);spacing.reader.files["BULLET7.ASC"]={source,2000000000};
            check(app::run_bulletin_session(spacing.ports,100)==app::BulletinOutcome::continued,"bulletin framing with each newline ending");
            const auto at=spacing.output.find("TAIL");const auto pause=spacing.output.find("Press <ENTER>",at);
            auto between=spacing.output.substr(at+4,pause-at-4);
            const auto sgr=app::menu_text_sgr(5);if(ansi)for(auto s=between.find(sgr);s!=std::string::npos;s=between.find(sgr))between.erase(s,sgr.size());
            check(visible(between)=="\r\n\r\n","exact blank row before completion independent of final newline");
        }
        Fixture nog("G\r\r",ansi);nog.reader.files["BULLET7.ASC"]={"PAGE",2000000000};
        for(const auto& source:{std::string{"ART"},std::string{"ART\r\n"},std::string{"ART\r\n\r\n"}}) {
            Fixture redraw("L\r\r",ansi);redraw.reader.files["BULLETIN.ASC"]={source,900000000};
            check(app::run_bulletin_session(redraw.ports,100)==app::BulletinOutcome::continued,"L redraw remains at Bulletin until Enter");
            check(redraw.output.find("ART\r\n\r\n")!=std::string::npos,"blank row follows artwork independent of newline ending");
            check(redraw.output.find("ART",redraw.output.find("ART")+3)!=std::string::npos &&
                redraw.output.find("LIST BULLETINS")==std::string::npos && redraw.output.find("Press <ENTER>")==std::string::npos,
                "L redraws custom artwork without separate list or acknowledgment");
        }
        check(app::run_bulletin_session(nog.ports,100)==app::BulletinOutcome::continued && nog.output.find("Choose a bulletin")!=std::string::npos,"Bulletin G rejected; Enter still returns Main");
    }
    for(const app::OtherBoardValues& good: {app::OtherBoardValues{"Board","bbs.example",""},{"Board","","host.example:65535"},{"Board","telnet://127.0.0.1:2323","ssh://[::1]:2222"}})
        check(app::valid_other_board_values(good),"accepted one/both address forms and nondefault ports");
    for(const app::OtherBoardValues& bad: {app::OtherBoardValues{"","bbs.example",""},{"Board","",""},{"Board","bbs.example:0",""},{"Board","bbs.example:65536",""},{"Board","host;cmd.exe",""},{"Board\x1b[31m","bbs.example",""},{"Board","http://host",""}})
        check(!app::valid_other_board_values(bad),"bounded inert address/name validation");
    for(const auto* invalid:{"[::::]:23","host..example","bad-.example","host:bad"})
        check(!app::valid_other_board_values({"Board",invalid,""}),"invalid IPv6/DNS label/port refuses inert storage");
    const std::vector<app::OtherBoardValues> boards{{"One","bbs.example:23",""},{"Two","","ssh.example:2222"}};
    const auto encoded_boards=app::encode_other_board_text(boards);
    const auto parsed_boards=app::parse_other_board_text(encoded_boards);
    check(parsed_boards && parsed_boards->size()==2 && parsed_boards->back().name=="Two","text block roundtrip and ordered entries");
    check(app::parse_other_board_text("")->empty(),"empty text list is valid");
    check(app::parse_other_board_text("\xef\xbb\xbf# Comment\nName: One\nTelnet: bbs.example\n")->size()==1,"UTF8 BOM and ordinary LF accepted");
    for(const auto bad:{"Name: One\nName: Two\nTelnet: bbs.example","Name: Missing address\n","Number: 4\nName: One\nTelnet: bbs.example",
        "Name: One\nTelnet: bbs.example\nSSH: host;cmd.exe"})check(!app::parse_other_board_text(bad),"invalid text list is not silently emptied");
    Fixture line("L\r7\r\r",true);line.state.preferences.preferences.hotkey=false;
    line.reader.files["BULLET7.ASC"]={"LINE MODE PAGE",2000000000};
    check(app::run_bulletin_session(line.ports,100)==app::BulletinOutcome::continued && line.output.find("LINE MODE PAGE")!=std::string::npos,"saved line mode supports commands and numbered page selection");
    for(bool ansi:{false,true})for(bool notice:{false,true}) {
        Fixture menu("L\r\r",ansi);std::string artwork;
        for(unsigned row=0;row<40;++row)artwork+="FULL MENU ROW "+std::to_string(row)+"\r\n";
        menu.reader.files["BULLETIN.ASC"]={artwork,2000000000};
        if(ansi)menu.reader.files["BULLETIN.ANS"]={"\x1b[32m"+artwork,2000000000};
        if(notice)menu.reader.files["BULLET7.ASC"]={"CONTENT",2000000000};
        unsigned menu_frames=0;
        menu.ports.display=[&](const app::TerminalDisplayProgram& program){
            ++menu_frames;
            if(menu_frames%2==1 || !notice)check(std::none_of(program.operations.begin(),program.operations.end(),[](const auto& op){return op.kind==app::TerminalDisplayOperationKind::paging_newline;}),"full menu and L redraw have no paging boundaries");
            return true;
        };
        check(app::run_bulletin_session(menu.ports,100)==app::BulletinOutcome::continued,"full ANSI/ASC menu notice and Enter preserved");
    }
    for(bool ansi:{false,true})for(unsigned color=0;color<=15;++color){
        for(const auto board_color:{6U,14U}){
            Fixture bulletin("\r",ansi);bulletin.state.default_color=static_cast<std::uint8_t>(board_color);
            bulletin.state.preferences.preferences.color=static_cast<std::uint8_t>(color);
            bulletin.state.input_color=11;
            bulletin.reader.files["BULLETIN.ASC"]={"AUTHORED MENU",2000000000};
            bulletin.reader.files["BULLETIN.ANS"]={"\x1b[1;37mAUTHORED MENU",2000000000};
            bulletin.reader.files["BULLET7.ASC"]={"UPDATED PAGE",2000000000};
            check(app::run_bulletin_session(bulletin.ports,100)==app::BulletinOutcome::continued,"all saved colors and board-default fallbacks complete bulletin login stage");
            check(!ansi || bulletin.output.find(app::menu_text_sgr(7)+"Test Caller, the following bulletin(s) have")!=std::string::npos,"caller bulletin notice resets inherited artwork color to system gray");
            check(bulletin.output.find((ansi?app::menu_text_sgr(7):std::string{})+
                "Bulletin <#>, <L>ist Bulletins, <N>ew Bulletins, < ENTER > To Continue? ")!=std::string::npos,
                "all caller colors preserve system-gray bulletin question and exact plain command choices");
            check(ansi || bulletin.output.find('\x1b')==std::string::npos,"plain bulletin stage never emits SGR");
            Fixture newsletter("N",ansi);newsletter.state.default_color=static_cast<std::uint8_t>(board_color);
            newsletter.state.preferences.preferences.color=static_cast<std::uint8_t>(color);
            newsletter.state.input_color=11;newsletter.reader.files["SFNWSLTR.ASC"]={"NEWS",2000000000};
            check(app::offer_login_newsletter(newsletter.ports,100),"all saved colors and board-default fallbacks complete newsletter offer");
            check(!ansi || newsletter.output.find(app::menu_text_sgr(7)+"The newsletter has been updated!\r\n")!=std::string::npos,"newsletter update uses system color");
            check(!ansi || newsletter.output.find(app::system_menu_text("Would you like to review it? [Y/n]",true))!=std::string::npos,"newsletter question shares system gray, not fixed bright white");
            check(ansi || newsletter.output.find('\x1b')==std::string::npos,"plain newsletter stage never emits SGR");
        }
    }
    app::ClassicInformationChange change;change.values={"Board","bbs.example",""};
    const auto wire=app::encode_classic_information_change(change);check(!wire.empty() && app::decode_classic_information_change(wire).has_value(),"typed change wire roundtrip");
    for(std::size_t n=0;n<wire.size();++n)check(!app::decode_classic_information_change({wire.begin(),wire.begin()+n}),"every truncated mutation fails closed");
    auto extra=wire;extra.push_back(1);check(!app::decode_classic_information_change(extra),"extra authority/tail cannot enter typed request");
    app::LocalClassicInformationSnapshot snapshot{app::LocalAccountResultCode::ok,{{{1,1,change.values}},false},{1,1700000000,"1984-05-12"}};
    const auto encoded=app::encode_classic_information_snapshot(snapshot);
    check(!encoded.empty() && app::decode_classic_information_snapshot(encoded)->opening.configured_date=="1984-05-12","bounded snapshot and opening date roundtrip");
    snapshot.boards.entries.push_back(snapshot.boards.entries.front());check(app::encode_classic_information_snapshot(snapshot).empty(),"duplicate/unordered stable IDs refuse wire snapshot");
    check(app::decode_classic_information_query(app::encode_classic_information_query(99999))==99999 && app::encode_classic_information_query(-1).empty(),"bounded query cursor");
    std::cout<<"PASS "<<checks<<" classic information checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
