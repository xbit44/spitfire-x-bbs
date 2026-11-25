#include "sfx/application/telgate.hpp"
#include <algorithm>

namespace sfx::application {
namespace {
constexpr unsigned IAC=255, DO=253, DONT=254, WILL=251, WONT=252, SB=250, SE=240;
void command(std::string& out,unsigned verb,unsigned option){out+=char(IAC);out+=char(verb);out+=char(option);}
void escaped(std::string& out,unsigned char byte){out+=char(byte);if(byte==IAC)out+=char(byte);}
}
TelgateCodec::TelgateCodec(std::string terminal):terminal_(std::move(terminal)){}
std::string TelgateCodec::begin(){
    std::string out;
    // Preserve ordinary interactive NVT until the peer requests BINARY.
    for(unsigned option:{1U,3U}){peer_pending_[option]=true;command(out,DO,option);}
    for(unsigned option:{3U,24U,31U}){us_pending_[option]=true;command(out,WILL,option);}
    return out;
}
std::string TelgateCodec::window() const {
    std::string out;out+=char(IAC);out+=char(SB);out+=char(31);
    for(unsigned char b:{static_cast<unsigned char>(columns_>>8),static_cast<unsigned char>(columns_),static_cast<unsigned char>(rows_>>8),static_cast<unsigned char>(rows_)})escaped(out,b);
    out+=char(IAC);out+=char(SE);return out;
}
std::string TelgateCodec::dimensions(std::uint16_t rows,std::uint16_t columns){
    rows=std::max<std::uint16_t>(1,rows);columns=std::max<std::uint16_t>(1,columns);
    if(rows_==rows&&columns_==columns)return {};
    rows_=rows;columns_=columns;return us_[31]?window():std::string{};
}
void TelgateCodec::negotiate(unsigned verb,unsigned option,std::string& out){
    const bool remote=verb==WILL||verb==WONT;
    auto& enabled=remote?peer_[option]:us_[option];auto& pending=remote?peer_pending_[option]:us_pending_[option];auto& refused=remote?peer_refused_[option]:us_refused_[option];
    const bool yes=verb==WILL||verb==DO;
    const bool supported=remote?(option==0||option==1||option==3):(option==0||option==3||option==24||option==31);
    if(yes&&supported){
        const bool changed=!enabled;
        if(!remote&&option==0&&changed)out+=flush_cr();
        if(remote&&option==0&&changed)incoming_cr_=false;
        if(!enabled&&!pending)command(out,remote?DO:WILL,option);
        enabled=true;pending=false;refused=false;
        if(!remote&&option==31&&changed)out+=window();
    }else if(yes){if(!refused){command(out,remote?DONT:WONT,option);refused=true;}}
    else {if(enabled)command(out,remote?DONT:WONT,option);enabled=false;pending=false;refused=false;}
}
void TelgateCodec::payload(unsigned char b,std::string& out){
    if(!peer_[0]){if(incoming_cr_){incoming_cr_=false;if(!b)return;}if(b=='\r')incoming_cr_=true;}
    else incoming_cr_=false;
    out+=char(b);
}
bool TelgateCodec::receive(std::string_view bytes,std::string& display,std::string& reply){
    for(unsigned char b:bytes)switch(state_){
    case 0:if(b==IAC)state_=1;else payload(b,display);break;
    case 1:
        if(b==IAC){payload(b,display);state_=0;}
        else if(b==DO||b==DONT||b==WILL||b==WONT){command_=b;state_=2;}
        else if(b==SB){sub_.clear();state_=3;}
        else state_=0;
        break;
    case 2:negotiate(command_,b,reply);state_=0;break;
    case 3:if(b==IAC)state_=4;else {if(sub_.size()>=1024)return false;sub_+=char(b);}break;
    case 4:
        if(b==IAC){if(sub_.size()>=1024)return false;sub_+=char(b);state_=3;}
        else if(b==SE){
            if(sub_.size()==2&&static_cast<unsigned char>(sub_[0])==24&&sub_[1]==1&&us_[24]){
                reply+=char(IAC);reply+=char(SB);reply+=char(24);reply+=char(0);
                for(unsigned char c:terminal_)escaped(reply,c);
                reply+=char(IAC);reply+=char(SE);
            }
            sub_.clear();state_=0;
        }else return false;
        break;
    }
    return true;
}
std::string TelgateCodec::flush_cr(){if(!outgoing_cr_)return {};outgoing_cr_=false;return std::string("\r\0",2);}
std::string TelgateCodec::send(std::string_view bytes){
    std::string out;
    for(unsigned char b:bytes){
        if(outgoing_cr_){out+='\r';if(b!='\n')out+='\0';outgoing_cr_=false;}
        if(!us_[0]&&b=='\r'){outgoing_cr_=true;continue;}
        escaped(out,b);
    }
    return out;
}

RloginBridgeResult run_telgate_bridge(const RloginBridgePorts& p,const TelnetTarget& target,
    const TelgateLocalPorts& local,std::vector<std::uint8_t> prefix,bool discard_lf){
    using O=InteractiveStreamOutcome;RloginBridgeResult result;TelgateCodec codec(target.terminal_type);
    std::string to_remote=codec.begin(),to_caller,held;std::size_t prefix_at{};
    std::uint64_t remote_blocked{},caller_blocked{},cr_at{};
    bool menu{},echo{},skip_menu_lf{},remote_eof{};unsigned compose{};std::string draft;
    const auto width=[&]{return p.dimensions?std::max<unsigned>(2,p.dimensions().second):80U;};
    const auto show=[&](std::string_view text,unsigned initial_column=0){
        // Local text is bounded, control-clean, and wrapped without relying on autowrap.
        if(local.ansi)to_caller+="\x1b[0;37;40m";
        unsigned column=initial_column;for(unsigned char b:text.substr(0,4096)){if(b=='\r')continue;if(b=='\n'){to_caller+="\r\n";column=0;continue;}if(b<32||b==127)continue;if(column>=width()-1){to_caller+="\r\n";column=0;}to_caller+=char(b);++column;}
        if(local.ansi)to_caller+="\x1b[0m";
    };
    const auto menu_text=[&]{
        show("\nTelnet Gate menu\n");
        for(const auto& item:{std::pair{'D',"Disconnect remote"}, {'E',"Toggle local echo"},{'L',"Local callers"},{'P',"Private text"},{'Q',"Return to remote"}}){
            to_caller+=local.ansi?"\x1b[0;37;40m[\x1b[1;37m":"[";to_caller+=item.first;to_caller+=local.ansi?"\x1b[0;37;40m:> ":":> ";show(item.second,5);to_caller+="\r\n";
        }
        show("Enter/Esc resumes; Ctrl-X sends Ctrl-X.\nCommand: ");
    };
    const auto resume=[&]{menu=false;compose=0;draft.clear();show("\nReturning to remote.\n");};
    const auto end=[&](RloginEnd why){result.end=why;return result;};
    for(;;){
        if(p.stop)if(const auto why=p.stop())return end(*why);
        const auto now=p.now();
        if(p.dimensions){const auto d=p.dimensions();to_remote+=codec.dimensions(d.first,d.second);}
        if(cr_at&&now-cr_at>=50){to_remote+=codec.flush_cr();cr_at=0;}
        if(!menu&&!held.empty()&&to_caller.size()+held.size()<=kRloginBridgeBufferBytes){to_caller+=held;held.clear();}
        const auto drain=[&](std::string& buffer,const auto& write,std::uint64_t& blocked)->int{
            if(buffer.empty()){blocked=0;return 0;}const auto w=write(buffer);
            if(!valid_interactive_stream_write_result(buffer.size(),w))return -1;
            buffer.erase(0,w.bytes_written);
            if(w.outcome!=O::completed&&w.outcome!=O::timed_out)return -1;
            if(w.bytes_written)blocked=0;else if(!blocked)blocked=now?now:1;
            return blocked&&now>=blocked&&now-blocked>=5000?-2:0;
        };
        const int remote=remote_eof?0:drain(to_remote,p.remote_write,remote_blocked);if(remote)return end(remote==-2?RloginEnd::backpressure_timeout:RloginEnd::remote_closed);
        const int caller=drain(to_caller,p.caller_write,caller_blocked);if(caller)return end(caller==-2?RloginEnd::backpressure_timeout:RloginEnd::caller_disconnected);
        // Leave space for escaping and negotiation replies. Hold remote payload
        // while local actions run; continue decoding commands and servicing writes.
        if(remote_eof&&held.empty()&&to_caller.empty())return end(RloginEnd::remote_closed);
        if(!remote_eof&&held.size()<kRloginBridgeBufferBytes-4096&&to_remote.size()<kRloginBridgeBufferBytes-4096){
            const auto read=p.remote_read(1024);
            if(!valid_interactive_stream_read_result(1024,read))return end(RloginEnd::transport_failed);
            if(!read.bytes.empty()){
                std::string reply;if(!codec.receive({reinterpret_cast<const char*>(read.bytes.data()),read.bytes.size()},held,reply))return end(RloginEnd::transport_failed);
                to_remote+=reply;result.remote_bytes+=read.bytes.size();
                // Binary takes precedence immediately, including a negotiation
                // received while composing. No subsequent payload is a menu key.
                if(menu&&codec.binary()){menu=false;compose=0;draft.clear();skip_menu_lf=false;to_caller.clear();}
            }else if(read.outcome!=O::completed&&read.outcome!=O::timed_out){remote_eof=true;menu=false;compose=0;draft.clear();to_remote.clear();}
        }
        if(to_remote.size()<kRloginBridgeBufferBytes-8192&&to_caller.size()<kRloginBridgeBufferBytes-8192){
            InteractiveStreamReadResult read{O::timed_out,{}};
            const bool cached=prefix_at<prefix.size();
            if(cached){const auto n=std::min<std::size_t>(1024,prefix.size()-prefix_at);read={O::completed,{prefix.begin()+prefix_at,prefix.begin()+prefix_at+n}};prefix_at+=n;}
            else read=p.caller_read(1024);
            if(!valid_interactive_stream_read_result(1024,read))return end(RloginEnd::transport_failed);
            if(!cached&&!read.bytes.empty()&&p.keyboard)p.keyboard();
            for(unsigned char b:read.bytes){
                if(to_remote.size()>kRloginBridgeBufferBytes-8192||to_caller.size()>kRloginBridgeBufferBytes-8192)return end(RloginEnd::backpressure_timeout);
                if(discard_lf){discard_lf=false;if(b=='\n')continue;}
                if(skip_menu_lf){skip_menu_lf=false;if(b=='\n')continue;}
                ++result.caller_bytes;
                if(menu){
                    if(compose){
                        if(b==27){compose=0;draft.clear();menu_text();continue;}
                        if(b==8||b==127){if(!draft.empty()){draft.pop_back();to_caller+="\b \b";}continue;}
                        if(b=='\r'||b=='\n'){
                            skip_menu_lf=b=='\r';
                            if(compose==1){unsigned node{};for(char c:draft)node=node*10+unsigned(c-'0');draft.clear();if(!node||!local.select_recipient||!local.select_recipient(node)){show("\nCaller unavailable.\n");compose=0;menu_text();}else{compose=2;show("\nText (Esc cancels): ");}}
                            else{show("\n");show(local.private_text?local.private_text(draft):"Private text unavailable.");draft.clear();compose=0;menu_text();}
                        }else if(b>=32&&b!=127&&draft.size()<(compose==1?3U:512U)&&(compose!=1||(b>='0'&&b<='9'))){draft+=char(b);to_caller+=char(b);}
                        continue;
                    }
                    const auto key=b>='a'&&b<='z'?b-'a'+'A':b;
                    if(key=='D')return end(RloginEnd::gateway_escape);
                    if(key==24){to_remote+=codec.send(std::string(1,char(24)));resume();}
                    else if(key=='Q'||key==27||key=='\r'||key=='\n'){skip_menu_lf=key=='\r';resume();}
                    else if(key=='E'){echo=!echo;show(echo?"\nLocal echo On (suppressed while remote ECHO is active).\n":"\nLocal echo Off.\n");menu_text();}
                    else if(key=='L'){show("\n");show(local.callers?local.callers():"Callers unavailable.");menu_text();}
                    else if(key=='P'){compose=1;draft.clear();show("\nLocal node (Esc cancels): ");}
                    continue;
                }
                if(b==24&&!codec.binary()){menu=true;menu_text();continue;}
                to_remote+=codec.send(std::string(1,char(b)));if(b=='\r')cr_at=now?now:1;
                if(echo&&!codec.remote_echo()&&!codec.binary())to_caller+=char(b);
            }
            if(read.bytes.empty()&&read.outcome!=O::completed&&read.outcome!=O::timed_out)return end(RloginEnd::caller_disconnected);
        }
        if(to_remote.size()>kRloginBridgeBufferBytes||to_caller.size()>kRloginBridgeBufferBytes||held.size()>kRloginBridgeBufferBytes)return end(RloginEnd::backpressure_timeout);
        result.outbound_high_water=std::max(result.outbound_high_water,to_remote.size());result.inbound_high_water=std::max(result.inbound_high_water,to_caller.size()+held.size());
        if(p.wait)p.wait();
    }
}
}
