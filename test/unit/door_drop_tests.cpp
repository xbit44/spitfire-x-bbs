#include "sfx/application/door_drop.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <set>
namespace app=sfx::application;
unsigned checks{};
void check(bool pass,const char* text){++checks;if(!pass)throw std::runtime_error(text);}

void check_ini(app::DoorDropContext context) {
    app::DoorDefinition door;
    door.id=14;door.name="INI fixture";door.kind=app::DoorExecutionKind::local;
    door.program.command_line="TEST.EXE";
    const auto old=app::encode_door_definition(door);
    check(!old.empty()&&app::decode_door_definition(old).has_value(),"legacy door definition remains readable");
    door.program.drop_file=app::DoorDropFile::doordrop_ini;
    door.program.doordrop_utf8=true;
    const auto new_record=app::encode_door_definition(door);
    const auto decoded=app::decode_door_definition(new_record);
    check(decoded&&decoded->program.drop_file==app::DoorDropFile::doordrop_ini&&decoded->program.doordrop_utf8,
          "per-door new ID and text encoding survive serialization");
    door.allow_multiple=true;door.program.drop_location=app::DoorDropLocation::startup;
    check(!app::valid_door_definition(door),"concurrent callers cannot share one start-up DOORDROP.INI");
    context.software_version="1.0.0-alpha.2";
    context.file_time="2026-11-01T01:30:00-08:00";
    context.alias=u8" Caf\u00e9 \u2603\r\nUSER_ROLE=sysop ";
    context.remaining_seconds=65000;
    context.sysop_access=false;
    context.door_code="FIXTURE";
    for (bool utf8 : {false,true}) {
        context.doordrop_utf8=utf8;
        context.door_name=std::string(244,'x')+u8"\u2603";
        auto result=app::make_door_drop_files(app::DoorDropFile::doordrop_ini,context);
        check(result.valid()&&result.files.size()==1,"new format emits exactly one file");
        check(result.files.front().name=="DOORDROP.INI"&&
              result.environment.at("DOORDROP_INI")=="C:\\SFX\\Node\\2\\DOORDROP.INI","exact discovery path");
        const std::string bytes(result.files.front().bytes.begin(),result.files.front().bytes.end());
        check(bytes.rfind("[file]\r\n",0)==0&&bytes.find('\0')==bytes.npos&&bytes.find('\x1a')==bytes.npos,"strict INI envelope");
        std::map<std::string,std::string> parsed;
        std::string section;
        for (std::size_t begin=0;begin<bytes.size();) {
            const auto end=bytes.find("\r\n",begin);
            check(end!=bytes.npos&&end-begin<=255,"each line is bounded and CRLF-terminated");
            const auto line=bytes.substr(begin,end-begin);
            check(line.find_first_of("\r\n")==line.npos,"no injected newline");
            if (line.front()=='[') section=line.substr(1,line.size()-2);
            else {
                const auto equals=line.find('=');
                check(equals!=line.npos&&parsed.emplace(section+"/"+line.substr(0,equals),line.substr(equals+1)).second,"unique literal INI key");
            }
            begin=end+2;
        }
        for (const char* field : {"file/FILE_TIME","system/SYS_SOFTWARE","system/SYS_NAME","system/SYS_OP",
             "system/SYS_NODE_NUM","system/SYS_NODE_COUNT","comm/COMM_TYPE","comm/COMM_CHARSET",
             "user/USER_ALIAS","user/USER_NUMBER","user/USER_KEY","terminal/TERM_TYPE",
             "terminal/TERM_COLS","terminal/TERM_ROWS"}) check(parsed.count(field)==1,"active caller field present");
        check(parsed.count("file/FILE_UTF8")==static_cast<unsigned>(utf8),"UTF-8 opt-in only");
        check(parsed.at("user/USER_ALIAS")== (utf8 ? u8"Caf\u00e9 \u2603??USER_ROLE=sysop" : "Caf\x82 ???USER_ROLE=sysop"),"encoding and literal equals survive safe serialization");
        check(parsed.at("user/USER_ROLE")=="user"&&parsed.at("user/USER_KEY")==context.user_key,"actual identity and role");
        check(parsed.at("session/TIME_LEFT")=="65000"&&parsed.at("file/FILE_TIME")==context.file_time,"full time limit and local offset");
        check(parsed.at("door/DOOR_NAME").size()==(utf8?244U:245U),"encoded length never splits a Unicode character");
    }
    context.remaining_seconds.reset();
    context.dos=true;
    context.io=app::DoorIo::uart_only;
    auto result=app::make_door_drop_files(app::DoorDropFile::doordrop_ini,context);
    check(result.valid(),"UART context valid");
    const std::string uart(result.files.front().bytes.begin(),result.files.front().bytes.end());
    check(uart.find("COMM_TYPE=uart\r\nCOMM_PORT=1\r\nUART_BASE=1016\r\nUART_IRQ=4\r\n")!=uart.npos&&
          uart.find("TIME_LEFT")==uart.npos,"actual COM1 settings and unlimited time");
    context.dos=false;context.io=app::DoorIo::standard;context.drop_path="/home/sysop/sfx/Node/2";
    check(!app::make_door_drop_files(app::DoorDropFile::doordrop_ini,context).valid(),"Windows path contract still refuses POSIX input");
    context.path_style=app::DoorDropPathStyle::posix;
    result=app::make_door_drop_files(app::DoorDropFile::doordrop_ini,context);
    check(result.valid()&&result.environment.at("DOORDROP_INI")=="/home/sysop/sfx/Node/2/DOORDROP.INI","native POSIX INI discovery path exact");
    const std::string native(result.files.front().bytes.begin(),result.files.front().bytes.end());
    check(native.find("TIME_LEFT")==native.npos&&native.find("COMM_TYPE=stdio\r\n")!=native.npos,"native unlimited time and stdio preserved");
    context.dos=true;
    check(!app::make_door_drop_files(app::DoorDropFile::doordrop_ini,context).valid(),"DOS cannot receive native POSIX drop path");
    context.dos=false;
    for(const auto* path:{"relative/Node/2","C:\\SFX\\Node\\2","/home/sysop\nINJECT=1","/home/sysop\\Node"}){
        context.drop_path=path;check(!app::make_door_drop_files(app::DoorDropFile::doordrop_ini,context).valid(),"invalid POSIX discovery path refused");
    }
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh output path required");const std::filesystem::path root=argv[1];check(!std::filesystem::exists(root),"retain earlier fixtures");std::filesystem::create_directories(root);
    app::DoorDropContext context;context.account=42;context.node=2;context.nodes=4;context.security=25;context.alias="Fixture Caller";context.user_key="sfx-account-42";context.board="Fixture Board";context.sysop="Fixture Sysop";context.ansi=true;context.remaining_seconds=3601;context.node_path="C:\\SFX\\Node\\2";context.drop_path=context.node_path;context.startup_path="C:\\Games\\Example";context.date=context.logon_date="10/25/26";context.clock=context.logon_clock="12:34";context.deadline_utc="2026-10-25T21:34:01Z";context.seconds_today=context.logon_seconds_today=45240;context.elapsed_seconds=5;
    for(unsigned format=0;format<=13;++format){
        context.dos=format!=12;context.io=context.dos?app::DoorIo::fossil_or_uart:app::DoorIo::socket;context.socket=314;
        const auto result=app::make_door_drop_files(static_cast<app::DoorDropFile>(format),context);
        check(result.valid(),result.diagnostic.c_str());check(result.files.size()==(format==0?0U:format==4||format==6||format==10?2U:1U),"exact companion set");
        const auto directory=root/std::to_string(format);std::filesystem::create_directory(directory);
        for(const auto& file:result.files){check(!file.name.empty()&&file.name.find_first_of("/\\:")==file.name.npos,"bounded leaf filename");check(!file.bytes.empty()&&file.bytes.size()<16384,"bounded complete drop output");std::ofstream output(directory/file.name,std::ios::binary);output.write(reinterpret_cast<const char*>(file.bytes.data()),file.bytes.size());}
        if(format==13)check(result.environment.at("BBSDEV_DRP")=="C:\\SFX\\Node\\2\\BBSDEV.DRP","exact unquoted BBSDEV environment path");
    }
    context.elapsed_seconds=125;
    for(const auto format:{app::DoorDropFile::pcboard_sys,app::DoorDropFile::dorinfo1_def}){
        const auto result=app::make_door_drop_files(format,context);check(result.valid(),"elapsed caller context fixture");
        const auto directory=root/"elapsed";std::filesystem::create_directories(directory);
        for(const auto& file:result.files){std::ofstream output(directory/file.name,std::ios::binary);output.write(reinterpret_cast<const char*>(file.bytes.data()),file.bytes.size());}
    }
    context.elapsed_seconds=5;context.dos=false;context.io=app::DoorIo::standard;
    {const auto result=app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context);check(result.valid(),"native stdio BBSDEV reader fixture");std::filesystem::create_directory(root/"bbsdev-stdio");std::ofstream output(root/"bbsdev-stdio/BBSDEV.DRP",std::ios::binary);output.write(reinterpret_cast<const char*>(result.files[0].bytes.data()),result.files[0].bytes.size());}
    context.alias="bad\ncaller";check(!app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context).valid(),"line injection refused");context.alias="Fixture Caller";
    context.account=70000;check(!app::make_door_drop_files(app::DoorDropFile::pcboard_sys,context).valid(),"PCBoard narrow identity cannot truncate");check(!app::make_door_drop_files(app::DoorDropFile::dorinfo1_def,context).valid(),"EXITINFO narrow identity cannot alias another account");context.account=42;
    context.alias=u8"Caf\u00e9";check(app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context).valid(),"BBSDEV UTF8 identity");const auto legacy=app::make_door_drop_files(app::DoorDropFile::sfdoors_dat,context);check(legacy.valid()&&std::find(legacy.files[0].bytes.begin(),legacy.files[0].bytes.end(),0x82)!=legacy.files[0].bytes.end(),"legacy identity encoded CP437");
    context.alias=u8"\u2603";check(app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context).valid(),"BBSDEV outside CP437 remains UTF8");check(!app::make_door_drop_files(app::DoorDropFile::sfdoors_dat,context).valid(),"legacy unsupported name never silently collides");
    context.alias="Fixture Caller";context.remaining_seconds.reset();context.deadline_utc.clear();const auto unlimited=app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context);check(unlimited.valid(),"unlimited BBSDEV has no invented deadline");
    for(const auto bad:{"2026-02-29T12:00:00Z","2024-02-30T12:00:00Z","2026-00-01T12:00:00Z","2026-12-01T24:00:00Z","2026-12-01T12:60:00Z","2026-12-01T12:00:60Z","202x-10-25T12:00:00Z"}){context.deadline_utc=bad;check(!app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context).valid(),"invalid deadline refused");}
    context.deadline_utc="2024-02-29T23:59:59Z";check(app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context).valid(),"valid leap day");context.deadline_utc.clear();
    for(const auto bad:{"-","en-","en--US","1n","en-US-US","en-abc","en-123456789","de-1901-1901"}){context.language=bad;check(!app::make_door_drop_files(app::DoorDropFile::bbsdev_drp,context).valid(),"invalid language tag refused");}context.language="en-US";
    context.node=context.nodes=16;context.dos=true;check(!app::make_door_drop_files(app::DoorDropFile::dorinfo_node,context).valid(),"two digit DOS node filename refused");check(app::make_door_drop_files(app::DoorDropFile::dorinfo1_def,context).valid(),"DORINFO1 supports high actual node");context.node=2;context.nodes=4;
    const auto defaults=app::parse_door_driver_settings("","");check(defaults&&defaults->yield_interval_ms==1.0&&defaults->can_disconnect&&!defaults->allow_x64_dos,"driver defaults");
    check(!defaults->dos_input_interval_ms,"absent pacing preserves legacy adapter default");
    for(const unsigned interval:{0,75,100}){
        const auto parsed=app::parse_door_driver_settings("",std::string("[SFXEXEC]\nDosInputIntervalMs=")+std::to_string(interval)+" ; keep comment\n");
        check(parsed&&parsed->dos_input_interval_ms&&*parsed->dos_input_interval_ms==interval,"whole millisecond pacing boundaries and inline comments parse");
    }
    const auto paced_override=app::parse_door_driver_settings("DosInputIntervalMs=75\n","[SFXEXEC]\nDosInputIntervalMs=0\n");
    check(paced_override&&paced_override->dos_input_interval_ms&&*paced_override->dos_input_interval_ms==0,"explicit per-game zero overrides shared pacing");
    for(const auto invalid:{"DosInputIntervalMs=-1","DosInputIntervalMs=101","DosInputIntervalMs=75.0","DosInputIntervalMs=+75","DosInputIntervalMs=75ms","DosInputIntervalMs=","DosInputIntervalMs=999999999999999999999999","DosInputIntervalMs=0\nDosInputIntervalMs=75","[Runtime]\nDosInputIntervalMs=75","DosInputIntervaMs=75"})
        check(!app::parse_door_driver_settings("",invalid),"invalid pacing, wrong section, duplicates and unknown keys remain refused");
    const auto override=app::parse_door_driver_settings("; Shared\r\nYieldInterval=0.5\r\nCanDisconnect=true\r\n[Runtime]\r\nAllowX64DOS=false\r\n","[SFXEXEC]\nCanDisconnect=false\nYieldInterval=0\n");check(override&&override->yield_interval_ms==0&&!override->can_disconnect&&!override->allow_x64_dos,"captured startup override and zero yield");
    for(const auto invalid:{"YieldInterval=-1","YieldInterval=nan","YieldInterval=inf","YieldInterval=1e300","YieldInterval=1001","YieldInterval=1.2.3","CanDisconnect=yes","YieldInterval=1\nYieldInterval=2","[Unknown]\nCanDisconnect=true","[UART]\nComPort=2","Bogus=1"})check(!app::parse_door_driver_settings(invalid,""),"malformed or unsupported INI refused");
    check(app::parse_door_driver_settings("YieldInterval=0.25\n","")->yield_interval_ms==0.25,"fractional yield survives parsing");
    context.dos=false;context.io=app::DoorIo::standard;
    check_ini(context);
    std::cout<<"PASS "<<checks<<" drop/INI checks; all format images retained\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
