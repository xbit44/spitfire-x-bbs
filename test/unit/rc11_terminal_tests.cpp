#include "caller_runtime_helpers.hpp"
#include "linux_startup_message.hpp"
#include "sfx/application/newsletter_editor.hpp"
#include "sfx/application/sysop_session.hpp"
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
struct Wire final:app::IInteractiveSessionByteStream {
    std::deque<int> events;std::string output;unsigned polls{};
    std::size_t writes{};
    std::vector<std::string> controls;
    void bytes(std::string_view text){for(unsigned char c:text)events.push_back(c);}
    void idle(){for(unsigned n=0;n<8;++n)events.push_back(-1);}
    app::InteractiveStreamReadResult read_some(std::size_t)override{
        if(events.empty())return {app::InteractiveStreamOutcome::disconnected,{}};
        const int next=events.front();events.pop_front();
        if(next<0){++polls;return {app::InteractiveStreamOutcome::timed_out,{}};}
        return {app::InteractiveStreamOutcome::completed,{static_cast<std::uint8_t>(next)}};
    }
    app::InteractiveStreamWriteResult write_some(std::string_view text)override{
        ++writes;
        if (text.find('\x1b') != text.npos || text.find('\r') != text.npos) controls.emplace_back(text);
        output+=text;return {app::InteractiveStreamOutcome::completed,text.size()};
    }
};
void newsletter(bool ansi,unsigned columns,unsigned scenario){
    Wire wire;bool live=true;unsigned ticks{},screens{},saves{};
    rt::MenuModalStream modal(wire,[&]{++ticks;return live;},[]{});
    app::StreamCallerTerminal terminal(modal);
    terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
    app::SysopSessionPorts sysop{terminal};
    sysop.authorized=[&]{return live;};sysop.ansi=[&]{return ansi;};sysop.hotkeys=[]{return false;};
    sysop.columns=[&]{return columns;};sysop.rows=[]{return 25U;};
    sysop.write=[&](std::string_view text){return wire.write_some(text).bytes_written==text.size();};
    sysop.artwork=[&]{++screens;return sysop.write("SYSOP LOCATION\r\n");};
    std::string saved;const std::string original="Owner\ttext  \r\nlast";
    sysop.newsletter=[&]{
        return app::edit_sysop_newsletter({terminal,sysop.authorized,sysop.ansi,sysop.columns,sysop.rows,sysop.write,
            [&]{return modal.read_editor_byte(terminal);},[&](bool cr){terminal.finish_editor_key(cr);},
            [&]{return app::NewsletterSnapshot{true,true,original,"stable",{}};},
            [&](const auto&,std::string_view value){++saves;saved=value;return app::NewsletterSaveResult{true,value==original?"Newsletter unchanged; no file was rewritten.":"SFX Newsletter saved."};}});
    };
    wire.bytes("N\r");wire.idle();
    if(ansi){
        if(scenario!=0)wire.bytes("X");
        if(scenario==2||scenario==3){wire.bytes("\x01");wire.idle();wire.bytes(scenario==2?"Y":"N");}
        if(scenario==4||scenario==5){wire.bytes("\x1b");wire.idle();wire.bytes(scenario==4?"E":"S");}
        if(scenario!=2&&scenario!=5){wire.idle();wire.bytes("\x1a");}
    }else{
        if(scenario!=0){wire.bytes("/E1\r");wire.idle();wire.bytes("XOwner text  \r");wire.idle();}
        if(scenario==2){wire.bytes("/A\r");wire.idle();wire.bytes("Y");}
        else wire.bytes("/S\r");
    }
    wire.idle();wire.bytes("\r");wire.idle();wire.bytes("Q\r");
    check(app::SysopCallerSession(sysop).run(),"delayed real stream prompts complete");
    check(screens==2&&wire.events.empty(),"returns to enclosing Sysop before Q exits");
    check(wire.polls>=24&&ticks>wire.polls,"idle polls still check caller lifetime");
    check(saves==(scenario==2?0U:1U),"Cancel never invokes Save");
    if(saves)check(saved==(scenario==0?original:ansi?"X"+original:"XOwner text  \r\nlast"),"help/discard refusal preserves exact draft bytes");
    check(wire.output.find("access")==std::string::npos,"normal prompts do not fabricate access warning");
    if(scenario==2)check(wire.output.find("saved.")==std::string::npos,"Cancel result never claims saved");
    live=false;check(modal.read_some(1).outcome==app::InteractiveStreamOutcome::cancelled,"real revocation still cancels stream");
}
void display_playback() {
    using namespace std::chrono_literals;
    using Kind = app::TerminalDisplayOperationKind;
    const std::string body(480, 'X');
    const std::string escape = "\x1b[31m";
    app::TerminalDisplayProgram program;
    program.operations = {{Kind::ansi_sequence,escape,19200}, {Kind::cp437_text,body,19200},
                          {Kind::paging_newline,{},19200}};
    program.output_byte_count = escape.size() + body.size() + 2;
    Wire paced;
    app::StreamCallerTerminal terminal(paced);
    const auto started = std::chrono::steady_clock::now();
    check(terminal.present_display({"pace", &program}) == app::TerminalOutcome::completed,
          "paced ANSI display completes");
    check(std::chrono::steady_clock::now() - started >= 190ms,
          "19200 bps produces caller-visible elapsed time");
    check(paced.output == escape + body + "\r\n" && paced.writes > 3 &&
          paced.controls == std::vector<std::string>{escape,"\r\n"},
          "pacing preserves bytes and never splits ANSI or CRLF");

    Wire cancelled;
    rt::MenuModalStream modal(cancelled,[&] { return cancelled.output.size() < 40; },[] {});
    app::StreamCallerTerminal guarded(modal);
    check(guarded.present_display({"pace", &program}) == app::TerminalOutcome::cancelled &&
          cancelled.output.size() < program.output_byte_count,
          "session guard cancels a paced display before its remaining artwork is sent");

    for (auto& operation : program.operations) operation.playback_bps = 0;
    Wire normal;
    app::StreamCallerTerminal unpaced(normal);
    check(unpaced.present_display({"pace", &program}) == app::TerminalOutcome::completed &&
          normal.writes == 3 && normal.output == paced.output,
          "full-speed display keeps the original bytes and writes");
}
}
int main(){try{
    display_playback();
    for(unsigned width:{40U,80U})for(bool ansi:{false,true})for(unsigned scenario=0;scenario<(ansi?6U:3U);++scenario)newsletter(ansi,width,scenario);
    for(unsigned width:{40U,60U,80U}){
        const std::string warning="Sysop Utilities closed; refresh your access and selected records before continuing.";
        const auto rows=app::builtin_menu_rows(app::MenuSnapshot{},"","",warning,width);
        std::string joined;
        for(const auto& row:rows)if(row.role==app::MenuTextRole::event){
            check(row.text.size()<width,"warning stays below right margin");
            if(!joined.empty())joined+=' ';
            joined+=row.text;
        }
        check(joined==warning,"warning wrapping preserves complete message");
    }
    check(rt::linux_startup_message("telnet_port_in_use:127.0.0.1:2323").find("127.0.0.1:2323")!=std::string::npos,"Telnet conflict identifies endpoint");
    check(rt::linux_startup_message("ssh_port_in_use:0.0.0.0:2222").find("already in use")!=std::string::npos,"SSH conflict gives actual cause");
    check(rt::linux_startup_message("ssh_port_permission_denied:0.0.0.0:22").find("permission")!=std::string::npos,"permission failure is distinct");
    check(rt::linux_startup_message("secret /private/path").find("secret")==std::string::npos,"unknown startup failure does not echo private internals");
    check(rt::linux_startup_message("telnet_port_in_use:secret:23").find("secret")==std::string::npos,"malformed endpoint never echoed");
    std::cout<<"PASS rc11 terminal "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL rc11 terminal "<<checks<<": "<<e.what()<<'\n';return 1;}}
