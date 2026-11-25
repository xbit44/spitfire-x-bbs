#include "sfx/application/telgate.hpp"
#include "../../src/apps/config_ui/door_command_line.hpp"
#include <iostream>
#include <stdexcept>
#include <deque>
using namespace sfx::application;
namespace {
unsigned checks{};void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
std::string bytes(std::initializer_list<unsigned> values){std::string out;for(auto v:values)out+=char(v);return out;}
struct Bridge {
    std::uint64_t now{};std::deque<std::pair<unsigned,std::string>> caller,remote;
    std::string sent,shown;unsigned closed{1000},stop_at{2000},caller_closed{30000},lists{},selected{},texts{};std::string body;bool blocked_remote{},blocked_caller{};
    RloginBridgeResult run(bool ansi=false){
        RloginBridgePorts p;p.now=[&]{return now;};p.wait=[&]{now+=10;};p.stop=[&]()->std::optional<RloginEnd>{if(now>=stop_at)return RloginEnd::shutdown;return {};};
        const auto read=[&](auto& queue,std::size_t maximum,bool eof){
            if(!queue.empty()&&now>=queue.front().first){auto& s=queue.front().second;const auto n=std::min(maximum,s.size());std::vector<std::uint8_t> b(s.begin(),s.begin()+n);s.erase(0,n);if(s.empty())queue.pop_front();return InteractiveStreamReadResult{InteractiveStreamOutcome::completed,std::move(b)};}
            return InteractiveStreamReadResult{eof&&now>=closed?InteractiveStreamOutcome::disconnected:InteractiveStreamOutcome::timed_out,{}};
        };
        p.caller_read=[&](std::size_t n){if(now>=caller_closed)return InteractiveStreamReadResult{InteractiveStreamOutcome::disconnected,{}};return read(caller,n,false);};p.remote_read=[&](std::size_t n){return read(remote,n,true);};
        p.caller_write=[&](std::string_view s){if(blocked_caller)return InteractiveStreamWriteResult{InteractiveStreamOutcome::timed_out,0};shown+=s;return InteractiveStreamWriteResult{InteractiveStreamOutcome::completed,s.size()};};
        p.remote_write=[&](std::string_view s){if(blocked_remote)return InteractiveStreamWriteResult{InteractiveStreamOutcome::timed_out,0};const auto n=std::min<std::size_t>(7,s.size());sent+=s.substr(0,n);return InteractiveStreamWriteResult{InteractiveStreamOutcome::completed,n};};
        p.dimensions=[] {return std::pair<std::uint16_t,std::uint16_t>{std::uint16_t{25},std::uint16_t{40}};};
        TelgateLocalPorts local;local.ansi=ansi;local.callers=[&]{++lists;return "Node 2 Local caller\n";};local.select_recipient=[&](unsigned n){selected=n;return n==2;};local.private_text=[&](std::string_view s){++texts;body=s;return "Private text queued.";};
        return run_telgate_bridge(p,TelnetTarget{"127.0.0.1"},local);
    }
};
}
int main()try{
    using namespace sfx::config_ui;
    for(const auto command:{"telnet bbs.example","telnet 127.0.0.1:2323","telnet [::1]:23 -t ansi -T 1"}){const auto p=parse_telnet_command(command);check(bool(p)&&valid_telnet_target(p.target),"structured Telnet command");check(parse_telnet_command(format_door_command(p.target)).target.host==p.target.host,"Telnet canonical roundtrip");}
    check(parse_telnet_command("telnet host").target.port==23,"default23");
    for(const auto command:{"telnet host:0","telnet host:65536","telnet host -p","telnet host -h","telnet host -c account","telnet host -T 0","telnet host -T 121","telnet user@host","telnet https://host","telnet host & shell","telnet host\ncmd"})check(!parse_telnet_command(command),"invalid/credential/shell Telnet command refused");
    DoorDefinition d;d.id=6;d.name="Telnet test";d.kind=DoorExecutionKind::telnet;d.telnet={"127.0.0.1",2323,"ansi",10};
    const auto encoded=encode_door_definition(d);const auto decoded=decode_door_definition(encoded);check(encoded[0]==5&&decoded&&decoded->telnet.port==2323&&decoded->kind==DoorExecutionKind::telnet,"new kind version5 roundtrip");
    auto old=d;old.kind=DoorExecutionKind::rlogin;old.telnet={};old.rlogin.host="old.example";check(encode_door_definition(old)[0]==3,"unchanged RLogin record remains v3");
    old.kind=DoorExecutionKind::local;old.program.command_line="game.exe";old.program.drop_file=DoorDropFile::doordrop_ini;check(encode_door_definition(old)[0]==4,"unchanged local DOORDROP record remains v4");
    const auto input=bytes({255,251,1,255,253,24,255,250,24,1,255,240,65,255,255,13,0,66,13,10});
    std::string expected_display=bytes({65,255,13,66,13,10}),expected_reply;
    for(std::size_t split=0;split<=input.size();++split){TelgateCodec codec("ansi");codec.begin();std::string out,reply;check(codec.receive(std::string_view(input).substr(0,split),out,reply)&&codec.receive(std::string_view(input).substr(split),out,reply),"all split IAC and subnegotiation boundaries");check(out==expected_display&&codec.remote_echo(),"NVT and escaped FF decode");if(!split)expected_reply=reply;else check(reply==expected_reply,"split-independent replies");check(reply.find(bytes({255,250,24,0,97,110,115,105,255,240}))!=std::string::npos,"terminal response");}
    TelgateCodec c("ansi");const auto initial=c.begin();check(initial.find(bytes({255,253,0}))==std::string::npos&&initial.find(bytes({255,251,0}))==std::string::npos,"do not force binary at login");
    std::string out,reply;check(c.receive(bytes({255,251,99,255,251,99,255,253,100,255,253,100}),out,reply)&&reply==bytes({255,254,99,255,252,100}),"unknown option refusal once without loops");
    check(c.send("a\r")=="a"&&c.send("\nb\r")=="\r\nb"&&c.flush_cr()==std::string("\r\0",2),"split NVT CRLF and bare CR");
    reply.clear();check(c.receive(bytes({255,251,0,255,253,0}),out,reply)&&c.binary(),"peer-initiated binary accepted both ways");
    const auto binary=bytes({0,13,10,24,255,1});check(c.send(binary)==bytes({0,13,10,24,255,255,1}),"binary transparent except Telnet FF escaping");
    reply.clear();out.clear();check(c.receive(c.send(binary),out,reply)&&out==binary,"binary roundtrip includes CAN FF NUL CRLF");
    TelgateCodec bounded("ansi");out.clear();reply.clear();check(!bounded.receive(bytes({255,250})+std::string(1025,'x'),out,reply),"subnegotiation bounded");
    {TelgateCodec codec("ansi");codec.begin();codec.dimensions(255,511);std::string display,response;check(codec.receive(bytes({255,253,31}),display,response)&&response==bytes({255,250,31,1,255,255,0,255,255,255,240}),"NAWS escapes each FF in dimensions");check(codec.dimensions(255,511).empty(),"unchanged dimensions do not loop");}
    {TelgateCodec codec("ansi");codec.send("\r");std::string display,response;check(codec.receive(bytes({255,253,0}),display,response)&&response==bytes({13,0,255,251,0}),"pending NVT CR finishes before announcing binary");}
    {TelgateCodec codec("ansi");std::string display,response;check(codec.receive(bytes({13,255,251,0,255,252,0,0}),display,response)&&display==bytes({13,0}),"binary transition clears old NVT CR suppression");}
    for(bool ansi:{false,true}){
        Bridge b;b.caller={{10,bytes({24})+"ELP2\rhello local\rQ"},{300,"remote Q"},{500,bytes({24,24})},{700,bytes({24})+"D"}};
        check(b.run(ansi).end==RloginEnd::gateway_escape&&b.lists==1&&b.selected==2&&b.texts==1&&b.body=="hello local","all menu actions and explicit remote disconnect");
        check(b.sent.find("remote Q")!=std::string::npos&&b.sent.find(char(24))!=std::string::npos&&b.sent.find("hello local")==std::string::npos,"local action text never remote; literal Ctrl-X and ordinary Q pass");
        check(b.shown.find("[D:>")!=std::string::npos||ansi,"approved plain menu syntax");check(ansi||b.shown.find(char(27))==std::string::npos,"plain menu ANSI free");
    }
    {Bridge b;b.caller={{10,bytes({24})+"P2\rdraft"},{200,bytes({24,255,0,13,10})}};b.remote={{100,bytes({255,251,0,255,253,0})}};check(b.run().end==RloginEnd::remote_closed&&b.texts==0,"binary during composition cancels local parser");check(b.sent.find(bytes({24,255,255,0,13,10}))!=std::string::npos,"transition payload preserved");}
    {Bridge b;b.caller={{10,bytes({24})+"P"}};b.remote={{20,"last remote text"}};b.closed=100;check(b.run().end==RloginEnd::remote_closed&&b.now<300&&b.shown.find("last remote text")!=std::string::npos,"EOF during menu drains held text without waiting for user");}
    {Bridge b;b.remote={{0,bytes({255,251,0,255,253,0})}};b.caller={{50,bytes({24})+"D"}};check(b.run().end==RloginEnd::remote_closed&&b.sent.find(bytes({24})+"D")!=std::string::npos,"binary at login bypasses escape");}
    {Bridge b;b.caller={{10,bytes({24})}};b.stop_at=100;check(b.run().end==RloginEnd::shutdown&&b.now==100,"shutdown during menu remains supervised");}
    {Bridge b;b.caller={{10,bytes({24})+"EQ"},{200,"echo-test"}};b.run();check(b.shown.find("echo-test")!=std::string::npos,"optional local echo actually paints remote-bound input");}
    {Bridge b;b.remote={{0,bytes({255,251,1})}};b.caller={{10,bytes({24})+"EQ"},{200,"echo-test"}};b.run();check(b.shown.find("echo-test")==std::string::npos&&b.sent.find("echo-test")!=std::string::npos,"negotiated remote ECHO suppresses local duplicate");}
    {Bridge b;b.caller={{10,bytes({24})+"P9\r"},{100,bytes({27})},{200,bytes({24})+"P2\rdiscard"},{300,bytes({27})+"Q"}};b.run();check(b.texts==0&&b.shown.find("Caller unavailable")!=std::string::npos&&b.sent.find("discard")==std::string::npos,"invalid recipient and cancelled composition stay local");}
    {Bridge b;b.blocked_remote=true;b.closed=20000;b.stop_at=30000;const auto r=b.run();check(r.end==RloginEnd::backpressure_timeout&&b.now<5100&&r.outbound_high_water<=kRloginBridgeBufferBytes,"stalled remote write bounded in time and memory");}
    {Bridge b;b.blocked_caller=true;b.remote={{0,"queued remote display"}};b.closed=20000;b.stop_at=30000;const auto r=b.run();check(r.end==RloginEnd::backpressure_timeout&&b.now<5200&&r.inbound_high_water<=2*kRloginBridgeBufferBytes,"stalled caller output bounded in time and memory");}
    {Bridge b;b.caller={{10,bytes({24})+"P2\rdraft"}};b.caller_closed=100;check(b.run().end==RloginEnd::caller_disconnected&&b.now==100&&b.texts==0,"caller loss during private composition returns immediately without sending");}
    std::cout<<"PASS "<<checks<<" Telgate codec/model/bridge checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
