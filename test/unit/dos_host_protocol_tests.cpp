#include "sfx/core/dos_host_protocol.hpp"
#include <iostream>
#include <stdexcept>
namespace proto=sfx::core::dos_host;
unsigned checks{};
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
int main(){try{
    proto::Request r;r.parent_pid=123;r.account=42;r.node=128;r.mode=12;r.parent_birth=123456;r.generation=10;r.sequence=12;
    r.parent_process=4;r.input=8;r.output=12;r.error=16;r.control=20;r.status=24;r.root="C:\\SFX";r.startup="C:\\Games\\Example";r.node_directory="C:\\SFX\\Node\\128";r.drop_directory=r.node_directory;r.private_directory="C:\\SFX\\Node\\128\\run1";r.command="EXAMPLE.EXE /dC:\\SFX\\Node\\128";r.session="0123456789abcdef0123456789abcdef";r.driver_ini="YieldInterval=0.25\r\nCanDisconnect=true\r\n";
    const auto encoded=proto::encode(r);check(encoded.has_value(),"valid request");const auto decoded=proto::decode(*encoded);check(decoded&&decoded->generation==10&&decoded->sequence==12&&decoded->node==128&&decoded->input==8&&decoded->command==r.command,"captured identity/handles/command round trip");
    for(std::size_t n=0;n<encoded->size();++n)check(!proto::decode({encoded->begin(),encoded->begin()+n}),"every truncation refuses");
    auto extra=*encoded;extra.push_back(0);check(!proto::decode(extra),"strict one-envelope EOF");extra=*encoded;extra.insert(extra.end(),encoded->begin(),encoded->end());check(!proto::decode(extra),"replayed second envelope refuses");
    for(unsigned at:{0u,8u,12u,20u,24u,28u}){auto corrupt=*encoded;for(unsigned i=0;i<4;++i)corrupt.at(at+i)=at==20?0:255;check(!proto::decode(corrupt),"magic/version/bound/account/node/mode refuses");}
    auto invalid=r;invalid.status=invalid.input;check(!proto::encode(invalid),"aliased inherited handles refused");invalid=r;invalid.parent_process=UINT64_MAX;check(!proto::encode(invalid),"pseudo handle refused");invalid=r;invalid.private_directory="\\\\server\\share";check(!proto::encode(invalid),"network IPC staging refused");invalid=r;invalid.command="EXAMPLE.EXE\nBAD";check(!proto::encode(invalid),"command line injection refused");invalid=r;invalid.session[0]='G';check(!proto::encode(invalid),"session alphabet strict");invalid=r;invalid.generation=0;check(!proto::encode(invalid),"unowned generation refused");
    check(encoded->at(8)==1,"older launch keeps the original envelope version");
    r.doordrop_path="C:\\SFX\\NODE\\128\\DOORDROP.INI";
    const auto ini=proto::encode(r);
    check(ini&&ini->at(8)==2,"only DOORDROP.INI uses the new envelope version");
    const auto decoded_ini=proto::decode(*ini);
    check(decoded_ini&&decoded_ini->doordrop_path==r.doordrop_path&&
          proto::environment_file(*decoded_ini)==r.command+"\r\nDOORDROP_INI="+r.doordrop_path+"\r\n",
          "matched DOS host receives exact child environment bytes");
    for(std::size_t n=0;n<ini->size();++n)check(!proto::decode({ini->begin(),ini->begin()+n}),"truncated INI envelope refused");
    invalid=r;invalid.bbsdev_path="C:\\SFX\\NODE\\128\\BBSDEV.DRP";
    check(!proto::encode(invalid),"different drop formats cannot occupy one envelope");
    for(auto kind:{proto::Message::started,proto::Message::finished,proto::Message::failed,proto::Message::hangup,proto::Message::stop}){const auto frame=proto::encode_frame({kind,42,"bounded_diagnostic"});check(frame.has_value(),"valid status/control frame");const auto back=proto::decode_frame(*frame);check(back&&back->message==kind&&back->value==42,"frame roundtrip");for(std::size_t n=0;n<frame->size();++n)check(!proto::decode_frame({frame->begin(),frame->begin()+n}),"partial frame rejected");}
    check(!proto::encode_frame({proto::Message::failed,0,std::string(4096,'x')}),"frame bound enforced");check(!proto::encode_frame({proto::Message::failed,0,"newline\n"}),"diagnostic line injection refused");
    std::cout<<"PASS "<<checks<<" private request/status protocol checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
