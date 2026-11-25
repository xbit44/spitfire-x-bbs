#include "sfx/platform/win32/installation_security.hpp"
#include <algorithm>
#include <iostream>
namespace inst=sfx::platform::win32::installation;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
int main()try{
    check(inst::current_program_count==12&&inst::maintenance_program_count==10&&inst::file_program_count==9&&inst::packet_program_count==7&&inst::message_program_count==6&&inst::file_transfer_program_index==7&&inst::file_manager_program_index==8&&inst::maintenance_program_index==9&&inst::dos_host_program_index==10&&inst::dos_launcher_program_index==11,"appended DOS roles preserve all accepted legacy counts");
    for(const auto count:{inst::legacy_program_count,inst::archive_program_count,inst::configuration_program_count,inst::message_program_count,inst::packet_program_count,inst::file_program_count,inst::maintenance_program_count,inst::current_program_count}){
        inst::Receipt r;r.id=L"ABCDEF0123456789ABCDEF0123456789";r.root=L"C:\\ReceiptUnitOnly";r.owner=inst::operator_sid();r.generation=9;r.committed=true;r.volume=123;r.root_id=5;r.bin_id=6;r.program_count=count;
        for(std::size_t n=0;n<count;++n){r.program_ids[n]=n+10;r.program_hashes[n]=std::string(64,'A');}
        const auto bytes=inst::encode_receipt(r);const auto read=inst::decode_receipt(bytes);check(inst::encode_receipt(read)==bytes&&read.program_count==count&&read.program_ids[count-1]==r.program_ids[count-1],"exact canonical receipt bytes preserved for each legacy/current image count");
        const std::string text(bytes.begin(),bytes.end());check(std::count(text.begin(),text.end(),'\n')==static_cast<int>(9+count*2+(count==12?7:0)),"exact version-specific field count");
        const auto version=count==2?1:count==4?2:count==5?3:count==6?4:count==7?5:count==9?6:count==10?7:8;const auto header="SFX-DESKTOP-INSTALL-"+std::to_string(version)+"\n";check(text.substr(0,header.size())==header,"legacy versions1..7 retain exact image counts; twelve images use version8");
        auto bad=bytes;bad.pop_back();bool refused=false;try{(void)inst::decode_receipt(bad);}catch(...){refused=true;}check(refused,"truncated receipt refused");
        bad=bytes;bad.insert(bad.end(),{'0','\n'});refused=false;try{(void)inst::decode_receipt(bad);}catch(...){refused=true;}check(refused,"extra receipt field cannot imply another image count");
        if(count==12){r.door_component={true,true,L"C:\\Windows\\System32\\SFXEXEC.DLL",123,111,std::string(64,'B'),"SFXEXEC-PRIVATE20-21-1"};const auto component=inst::decode_receipt(inst::encode_receipt(r));check(component.door_component.registered&&component.door_component.introduced&&component.door_component.file_id==111,"v8 retains exact System32 identity and introduced ownership");}
    }
    std::cout<<"PASS "<<checks<<" installation receipt compatibility units; no install/registry write/program launch\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL receipt after "<<checks<<": "<<e.what()<<'\n';return 1;}
