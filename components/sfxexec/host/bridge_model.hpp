// SFX-owned bounded candidate model. No GPL driver implementation is linked here.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
namespace sfxdos {
inline void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline std::vector<std::string> split(const std::string& s,char delimiter){std::vector<std::string> v;std::size_t at=0;
    for(;;){auto end=s.find(delimiter,at);v.push_back(s.substr(at,end-at));if(end==s.npos)break;at=end+1;}return v;}
inline bool text(const std::string& s,std::size_t maximum,bool empty=false){return (empty||!s.empty())&&s.size()<=maximum&&
    std::all_of(s.begin(),s.end(),[](unsigned char c){return c>=32&&c<127;});}
inline std::uint64_t number(const std::string& s,std::uint64_t maximum=UINT64_MAX){require(!s.empty()&&s.size()<=20,"missing/oversized numeric field");std::uint64_t n=0;
    for(char c:s){require(c>='0'&&c<='9'&&n<=maximum/10,"invalid numeric field");auto d=unsigned(c-'0');require(d<=maximum-n*10,"numeric field out of range");n=n*10+d;}return n;}
inline bool hex32(const std::string& s){return s.size()==32&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
inline std::string unescape(const std::string& s){std::string out;for(std::size_t i=0;i<s.size();++i){if(s[i]!='%')out+=s[i];
    else if(s.substr(i,3)=="%25"){out+='%';i+=2;}else if(s.substr(i,3)=="%7C"){out+='|';i+=2;}else throw std::runtime_error("invalid typed log escape");}return out;}
struct Context {std::uint64_t utc{},sequence{},generation{},account{},door{},duration{};unsigned node{},rows{};
    std::string epoch,session,caller,name,diagnostic;std::string key()const{return epoch+":"+std::to_string(sequence);}};
inline bool parse_context(std::string line,Context& c){try{
    if(!line.empty()&&line.back()=='\r')line.pop_back();if(!text(line,1280))return false;
    auto f=split(line,'|');if(f.size()!=28||(f[0]!="SFXLOG6"&&f[0]!="SFXLOG7"&&f[0]!="SFXLOG8"))return false;
    c.utc=number(f[1]);c.epoch=f[2];c.sequence=number(f[3]);c.node=unsigned(number(f[6],128));
    c.generation=number(f[7]);c.account=number(f[13],UINT32_MAX);c.session=f[15];c.caller=unescape(f[17]);
    c.duration=number(f[19]);c.rows=unsigned(number(f[21],255));c.door=number(f[22],999);c.name=unescape(f[23]);c.diagnostic=f[25];
    static const std::vector<std::string> ends={"door_remote_closed","door_gateway_escape","door_operator_terminated","door_caller_disconnected","door_shutdown","door_authority_lost","door_calling_time","door_inactivity","door_connection_failed","door_handshake_failed","door_transport_failed","door_backpressure_timeout"};
    bool entered=c.diagnostic=="door_entered"&&f[5]=="1";
    bool ended=std::find(ends.begin(),ends.end(),c.diagnostic)!=ends.end();
    return f[4]=="9"&&(entered||ended)&&c.node&&c.generation&&c.account&&c.door&&hex32(c.epoch)&&hex32(c.session)&&
        c.session!=std::string(32,'0')&&text(c.caller,30)&&text(c.name,64)&&
        number(f[5],32)>0;
}catch(...){return false;}}
inline std::vector<Context> eligible(const std::vector<Context>& entries,const std::string& alias,const std::string& door,const std::set<std::string>& claimed,std::uint64_t now){
    std::vector<Context> out;for(const auto& c:entries){
        if(c.diagnostic!="door_entered"||c.caller!=alias||c.name!=door||claimed.count(c.key())||c.utc>now+1||now>c.utc+5)continue;
        bool ended=false;for(const auto& e:entries)if(e.epoch==c.epoch&&e.session==c.session&&e.generation==c.generation&&e.node==c.node&&e.door==c.door&&e.sequence>c.sequence&&e.diagnostic!="door_entered")ended=true;
        if(!ended)out.push_back(c);
    }return out;}
inline std::map<std::string,std::string> config(const std::string& bytes){require(bytes.size()<=8192,"profile too large");std::map<std::string,std::string> result;
    for(auto line:split(bytes,'\n')){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;
        require(text(line,512),"profile must use printable ASCII");auto equal=line.find('=');require(equal!=line.npos&&equal>0,"profile requires key=value");
        auto key=line.substr(0,equal);require(result.emplace(key,line.substr(equal+1)).second,"duplicate profile key");}
    return result;}
inline std::string lines(const std::vector<std::string>& fields){std::string out;for(auto& f:fields){require(text(f,255,true),"unsafe drop-file field");out+=f+"\r\n";}return out;}
struct Drop {Context caller;unsigned security{},minutes{};std::string nodepath,sysop,location,date,clock;unsigned seconds_now{};};
inline std::string doorsys(const Drop& d){require(d.caller.node>=1&&d.caller.node<=128&&d.security>=1&&d.security<=999&&d.minutes>=1&&d.minutes<=60,"invalid drop-file context");
    std::vector<std::string> f(52);f[0]="COM1:";f[1]="38400";f[2]="8";f[3]=std::to_string(d.caller.node);f[4]="38400";
    f[5]="N";f[6]="N";f[7]="N";f[8]="N";f[9]=d.caller.caller;f[10]=d.location;f[13]="";
    f[14]=std::to_string(d.security);f[15]="1";f[16]=d.date;f[17]=std::to_string(d.minutes*60);f[18]=std::to_string(d.minutes);
    f[19]="GR";f[20]=std::to_string(d.caller.rows?d.caller.rows:25);f[21]="N";f[23]="0";f[24]="12/31/99";
    f[25]=std::to_string(d.caller.account);f[26]="Z";for(unsigned n=27;n<=30;++n)f[n]="0";
    f[32]=d.nodepath;f[33]=d.nodepath;f[34]=d.sysop;f[35]=d.caller.caller;f[36]="00:00";f[37]="Y";f[38]="N";f[39]="Y";f[40]="7";
    f[41]="0";f[42]=d.date;f[43]=d.clock;f[44]=d.clock;for(unsigned n=45;n<=48;++n)f[n]="0";f[50]="0";f[51]="0";return lines(f);}
inline std::string sfdoors(const Drop& d){require(d.caller.node>=1&&d.caller.node<=128&&d.security>=1&&d.security<=999&&d.minutes>=1&&d.minutes<=60,"invalid drop-file context");
    std::vector<std::string> f(32,"0");f[0]=std::to_string(d.caller.account);f[1]=d.caller.caller;f[2]="";f[3]=d.caller.caller.substr(0,d.caller.caller.find(' '));
    f[4]="38400";f[5]="1";f[6]=std::to_string(d.minutes);f[7]=std::to_string(d.seconds_now);f[8]=d.nodepath;f[9]="TRUE";
    f[10]=std::to_string(d.security);f[13]=std::to_string(d.minutes);f[14]=std::to_string((d.seconds_now+86400-unsigned(d.caller.duration%86400))%86400);
    f[16]=f[17]=f[18]="FALSE";f[19]="38400";f[20]="FALSE";f[23]=std::to_string(d.caller.node);f[30]="";f[31]=d.location;return lines(f);}
inline std::string name(const std::string& session,unsigned node,const std::string& kind){require(hex32(session)&&node>=1&&node<=128,"invalid private channel identity");
    require(kind=="to_dos"||kind=="from_dos"||kind=="hungup"||kind=="hangup"||kind=="carrier"||kind=="to_credit"||kind=="from_credit","unknown private channel kind");
    if(kind=="to_dos"||kind=="from_dos")return "\\\\.\\mailslot\\sfxexec\\v1\\"+session+"\\n"+std::to_string(node)+"\\"+kind;
    return "Local\\SFXEXEC.v1."+session+".n"+std::to_string(node)+"."+kind;}
}
