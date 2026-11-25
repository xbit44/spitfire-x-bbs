#include "bridge_model.hpp"
#include <iostream>
using namespace sfxdos;
static unsigned checks;
static void test(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(...){rejected=true;}test(rejected,"malformed value was admitted");}
int main(){try{
    const std::string log="SFXLOG8|100|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|7|9|1|12|42|0|2|127.0.0.1|1|2|17|17|bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|00000000000000000000000000000000|Test%7CCaller|10|125|80|43|6|Outpost%25Test|17|door_entered||";
    Context c;test(parse_context(log,c),"typed current Runtime record");test(c.node==12&&c.generation==42&&c.account==17&&c.rows==43&&c.caller=="Test|Caller"&&c.name=="Outpost%Test","actual caller/node/account mapping");
    test(eligible({c},c.caller,c.name,{},100).size()==1,"fresh test session");test(eligible({c},c.caller,c.name,{},106).empty(),"stale entry rejected");
    test(eligible({c},c.caller,c.name,{c.key()},100).empty(),"used entry rejected");test(eligible({c},"Other",c.name,{},100).empty(),"other caller rejected");
    auto ended=c;ended.sequence=8;ended.diagnostic="door_operator_terminated";
    test(eligible({c,ended},c.caller,c.name,{},100).empty(),"cancelled session cannot be relaunched");ended.generation++;
    test(eligible({c,ended},c.caller,c.name,{},100).size()==1,"different node generation cannot revoke current session");
    auto second=c;second.sequence++;test(eligible({c,second},c.caller,c.name,{},100).size()==2,"duplicate test logins remain ambiguous");
    auto fields=split(log,'|');for(auto index:{6u,7u,13u,15u,22u}){auto copy=fields;copy[index]="0";std::string joined;for(auto& f:copy){if(!joined.empty())joined+='|';joined+=f;}Context invalid;test(!parse_context(joined,invalid),"invalid ownership field rejected");}
    for(auto code:{"door_remote_closed","door_gateway_escape","door_operator_terminated","door_caller_disconnected","door_shutdown","door_authority_lost","door_calling_time","door_inactivity","door_connection_failed","door_handshake_failed","door_transport_failed","door_backpressure_timeout"}){
        auto copy=log;copy.replace(copy.find("door_entered"),12,code);copy.replace(copy.find("|9|1|"),5,"|9|2|");Context e;test(parse_context(copy,e),"real Runtime end diagnostic recognized");}
    test(!parse_context(log+"|extra",c),"extra typed field rejected");rejects([]{number("18446744073709551616");});rejects([]{number("999",128);});rejects([]{unescape("%00");});rejects([]{config("a=1\na=2\n");});
    auto profile=config("# test only\r\na=one\r\nb=two\r\n");test(profile.size()==2&&profile["b"]=="two","bounded CRLF profile");
    parse_context(log,c);Drop d{c,10,5,"C:\\SFXDOS\\NODE\\12\\","Sysop","Test location","10/24/26","10:00",36000};
    auto door=split(doorsys(d),'\n'),sf=split(sfdoors(d),'\n');test(door.size()==53&&sf.size()==33,"52-line DOOR.SYS and32-line SFDOORS");
    test(door[3]=="12\r"&&door[9]=="Test|Caller\r"&&door[25]=="17\r"&&door[20]=="43\r"&&door[13]=="\r","DOOR.SYS real identity and blank password");
    test(sf[0]=="17\r"&&sf[1]=="Test|Caller\r"&&sf[2]=="\r"&&sf[23]=="12\r"&&sf[8]==d.nodepath+"\r","SFDOORS real identity and private node");
    for(unsigned n:{0u,129u})rejects([&]{name(std::string(32,'a'),n,"hungup");});
    std::set<std::string> names;for(unsigned n:{1u,12u,128u})for(char t:{'a','b'})for(auto kind:{"to_dos","from_dos","hungup","hangup","carrier","to_credit","from_credit"})test(names.insert(name(std::string(32,t),n,kind)).second,"all session/node/direction identities distinct");
    rejects([]{name(std::string(32,'a'),1,"sbbsexec");});std::cout<<"PASS "<<checks<<" bridge model checks; native DOS bridge PENDING\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
