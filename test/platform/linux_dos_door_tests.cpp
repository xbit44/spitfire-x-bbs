#include "sfx/platform/linux/local_door.hpp"
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <future>
#include <atomic>

namespace app=sfx::application;namespace host=sfx::platform::linux_host;namespace fs=std::filesystem;
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
    check(argc==4&&geteuid()!=0,"unprivileged DOS fixture, runtime prefix and probe required");
    const fs::path root=argv[1];check(!fs::exists(root),"fresh DOS fixture required");
    fs::create_directories(root/"games");fs::create_directory(root/"bin");
    fs::copy_file(argv[3],root/"games/PROBE.COM");host::ensure_door_nodes(root,2);
    std::ofstream(root/"bin/sfxexec.ini")<<"CanDisconnect=false\n";
    app::DoorDefinition door;door.id=1;door.code="DOSPROBE";door.name="DOS Probe";door.enabled=true;
    door.kind=app::DoorExecutionKind::local;door.program.type=app::DoorProgramType::dos;
    door.program.io=app::DoorIo::uart_only;door.program.startup_directory="games";
    door.program.command_line="PROBE.COM U";door.program.drop_file=app::DoorDropFile::door_sys52;
    app::DoorDropContext caller;caller.account=42;caller.node=1;caller.nodes=2;caller.alias="DOS Caller One";
    caller.user_key="42";caller.board="SFX DOS Test";caller.security=10;caller.remaining_seconds=120;
    caller.ansi=true;caller.date="11/08/26";caller.clock="12:30";caller.logon_date=caller.date;
    caller.logon_clock=caller.clock;caller.deadline_utc="2026-11-08T12:32:00Z";
    host::DoorOptions options;options.dosemu_root=argv[2];
    struct Run {app::DoorProgramResult result;std::string output;};
    const auto run=[&](const app::DoorDefinition& selected,app::DoorDropContext person,const host::DoorOptions& runtime,
                       std::string input,app::DoorEnd forced=app::DoorEnd::returned,bool lose=false,std::atomic<unsigned>* barrier=nullptr,bool queued_stop=false){
        Run observed;bool sent{},arrived{};const auto start=std::chrono::steady_clock::now();app::DoorChannel channel;
        channel.read=[&](std::size_t maximum){
            const bool ready=observed.output.find(" READY\r\n")!=std::string::npos;
            if(ready&&barrier&&!arrived){++*barrier;arrived=true;}
            if(ready&&lose)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};
            if(sent||input.empty()||!ready||(barrier&&barrier->load()<2))return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};
            if(input.size()>maximum)throw std::runtime_error("bounded DOS input");
            sent=true;
            return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{input.begin(),input.end()}};
        };
        channel.write=[&](std::string_view bytes){const auto count=std::min<std::size_t>(37,bytes.size());observed.output.append(bytes.data(),count);return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,count};};
        channel.stop=[&]()->std::optional<app::DoorEnd>{
            if(forced!=app::DoorEnd::returned&&observed.output.find(queued_stop?" READY\r\nZ":" READY\r\n")!=std::string::npos)return forced;
            if(std::chrono::steady_clock::now()-start>std::chrono::seconds(20))return app::DoorEnd::calling_time;
            return {};
        };
        observed.result=host::run_local_door(root,selected,person,channel,runtime);
        return observed;
    };
    const auto report=[&](const Run& observed,const char* label){
        std::cout<<"CASE "<<label<<" end="<<app::door_result_code(observed.result.end)<<" exit="<<observed.result.exit_code<<" clean="<<observed.result.cleanup_complete<<" diagnostic="<<observed.result.diagnostic<<"\nOUTPUT\n"<<observed.output<<"\nEND OUTPUT\n"<<std::flush;
    };
    for(const auto io:{app::DoorIo::uart_only,app::DoorIo::fossil_only,app::DoorIo::fossil_or_uart,app::DoorIo::standard}){
        door.program.io=io;door.program.command_line=io==app::DoorIo::uart_only?"PROBE.COM U":io==app::DoorIo::fossil_only?"PROBE.COM F":"PROBE.COM S";
        if(io==app::DoorIo::fossil_or_uart)door.program.command_line="PROBE.COM F";
        const auto observed=run(door,caller,options,"hello q");report(observed,door.program.command_line.c_str());
        const auto& result=observed.result;const auto& output=observed.output;
        check(result.end==app::DoorEnd::returned&&result.exit_code==0&&result.cleanup_complete,"DOS returns and owned cleanup completes");
        check(output.find("DOS Caller One")!=std::string::npos,"DOS actually reads generated mapped drop file");
        check(output.find("hello DOS RETURN")!=std::string::npos,"actual DOS caller input and return output");
        check(output.find(std::string("\x1b[36mDOS ")+"\xda\xb3\xc0"+"\x1b[0m READY\r\n")!=std::string::npos,"DOS ANSI and CP437 bytes remain exact");
        check(!fs::exists(root/"node/1/DOOR.SYS")&&!fs::exists(root/"node/1/SFXDOOR.LCK"),"owned drop and reservation removed");
        check(fs::is_empty(root/"node/1"),"emulator configuration/state does not accumulate");
    }
    door.program.io=app::DoorIo::uart_only;door.program.command_line="PROBE.COM U";
    for(const auto placement:{app::DoorDropLocation::startup,app::DoorDropLocation::temporary}){
        door.program.drop_location=placement;const auto observed=run(door,caller,options,"q");report(observed,"drop placement");
        check(observed.result.end==app::DoorEnd::returned&&observed.result.cleanup_complete&&observed.output.find(caller.alias)!=std::string::npos,"actual DOS alternate drop placement");
        check(fs::is_empty(root/"node/1")&&!fs::exists(root/"games/DOOR.SYS")&&!fs::exists(root/"games/SFXDOOR.LCK"),"alternate placement owned cleanup");
    }
    door.program.drop_location=app::DoorDropLocation::node;
    for(const auto end:{app::DoorEnd::operator_terminated,app::DoorEnd::calling_time,app::DoorEnd::inactivity,app::DoorEnd::shutdown}){
        const auto observed=run(door,caller,options,"",end);report(observed,"host stop after DOS READY");
        check(observed.result.end==end&&observed.result.cleanup_complete&&fs::is_empty(root/"node/1"),"actual DOS host stop and cleanup");
    }
    for(const bool lose:{false,true}){
        const auto observed=run(door,caller,options,std::string(1,29)+"D",app::DoorEnd::returned,lose);report(observed,lose?"caller lost":"host escape");
        check(observed.result.end==(lose?app::DoorEnd::caller_disconnected:app::DoorEnd::escaped)&&observed.result.cleanup_complete&&fs::is_empty(root/"node/1"),"actual DOS disconnect/escape cleanup");
    }
    const auto failed=run(door,caller,options,"!");report(failed,"DOS exit seven");
    check(failed.result.end==app::DoorEnd::launch_failed&&failed.result.exit_code==7&&failed.result.cleanup_complete,"DOS nonzero exit retained");
    std::atomic<unsigned> barrier{};auto second=caller;second.node=2;second.alias="DOS Caller Two";second.account=43;
    auto a=std::async(std::launch::async,[&]{return run(door,caller,options,"node-one q",app::DoorEnd::returned,false,&barrier);});
    auto b=std::async(std::launch::async,[&]{return run(door,second,options,"node-two q",app::DoorEnd::returned,false,&barrier);});
    const auto one=a.get(),two=b.get();report(one,"concurrent node one");report(two,"concurrent node two");
    check(barrier==2&&one.result.end==app::DoorEnd::returned&&two.result.end==app::DoorEnd::returned,"two DOS processes actually overlap");
    check(one.output.find(caller.alias)!=std::string::npos&&one.output.find(second.alias)==std::string::npos&&two.output.find(second.alias)!=std::string::npos&&two.output.find(caller.alias)==std::string::npos,"concurrent DOS drop files isolated");
    check(one.output.find("node-one DOS RETURN")!=std::string::npos&&two.output.find("node-two DOS RETURN")!=std::string::npos,"concurrent DOS input isolated");
    check(one.result.cleanup_complete&&two.result.cleanup_complete&&fs::is_empty(root/"node/1")&&fs::is_empty(root/"node/2"),"both concurrent nodes cleaned");
    auto paced=options;paced.dos_serial_input_interval_ms=75;const std::string burst="0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    for(const auto io:{app::DoorIo::uart_only,app::DoorIo::fossil_only}){
        door.program.io=io;door.program.command_line=io==app::DoorIo::uart_only?"PROBE.COM U":"PROBE.COM F";
        const auto observed=run(door,caller,paced,burst+"q");report(observed,"paced 64-byte serial burst");
        check(observed.result.end==app::DoorEnd::returned&&observed.result.cleanup_complete&&observed.output.find(burst+"DOS RETURN")!=std::string::npos,"paced UART/FOSSIL burst exact");
        const auto began=std::chrono::steady_clock::now();
        const auto stopped=run(door,caller,paced,std::string(4096,'Z'),app::DoorEnd::operator_terminated,false,nullptr,true);report(stopped,"stop with queued serial input");
        check(stopped.result.end==app::DoorEnd::operator_terminated&&stopped.result.cleanup_complete&&std::chrono::steady_clock::now()-began<std::chrono::seconds(3),"queued pacing remains promptly cancellable");
    }
    std::ofstream(root/"games/sfxexec.ini")<<"[SFXEXEC]\nCanDisconnect=false\nDosInputIntervalMs=75\n";
    for(const auto io:{app::DoorIo::uart_only,app::DoorIo::fossil_only}){
        door.program.io=io;door.program.command_line=io==app::DoorIo::uart_only?"PROBE.COM U":"PROBE.COM F";
        const auto observed=run(door,caller,options,burst+"q");report(observed,"saved per-game 64-byte serial burst, no pacing override");
        check(observed.result.end==app::DoorEnd::returned&&observed.result.cleanup_complete&&observed.output.find(burst+"DOS RETURN")!=std::string::npos,"saved per-game UART/FOSSIL pacing is consumed by the normal adapter");
        const auto began=std::chrono::steady_clock::now();
        const auto stopped=run(door,caller,options,std::string(4096,'Z'),app::DoorEnd::operator_terminated,false,nullptr,true);report(stopped,"saved pacing stop with queued serial input");
        check(stopped.result.end==app::DoorEnd::operator_terminated&&stopped.result.cleanup_complete&&std::chrono::steady_clock::now()-began<std::chrono::seconds(3),"saved pacing remains promptly cancellable");
    }
    door.program.io=app::DoorIo::standard;door.program.command_line="PROBE.COM S";
    const auto standard_paced=run(door,caller,options,"hello q");report(standard_paced,"standard DOS with serial-only saved setting");
    check(standard_paced.result.end==app::DoorEnd::returned&&standard_paced.result.cleanup_complete&&standard_paced.output.find("hello DOS RETURN")!=std::string::npos,"saved serial setting preserves standard DOS input/output");
    std::ofstream(root/"games/sfxexec.ini")<<"CanDisconnect=false\nDosInputIntervalMs=101\n";
    const auto bad_saved=run(door,caller,options,"q");
    check(bad_saved.result.end==app::DoorEnd::launch_failed&&bad_saved.result.cleanup_complete&&bad_saved.result.diagnostic.find("dos_input_interval_range")!=std::string::npos,"bad saved pacing refuses launch instead of clamping");
    std::ofstream(root/"games/sfxexec.ini")<<"CanDisconnect=false\n";
    door.program.io=app::DoorIo::uart_only;door.program.command_line="PROBE.COM U";
    auto excessive=options;excessive.dos_serial_input_interval_ms=101;const auto invalid_interval=run(door,caller,excessive,"q");
    check(invalid_interval.result.end==app::DoorEnd::launch_failed&&invalid_interval.result.diagnostic.find("interval_out_of_range")!=std::string::npos,"input interval bounded");
    auto invalid=options;invalid.dosemu_sha256=std::string(64,'0');const auto mismatch=run(door,caller,invalid,"q");
    check(mismatch.result.end==app::DoorEnd::launch_failed&&mismatch.result.diagnostic.find("identity_mismatch")!=std::string::npos&&mismatch.result.cleanup_complete,"optional incorrect runtime pin refused");
    invalid=options;invalid.dosemu_binary=root/"missing-runtime";const auto missing=run(door,caller,invalid,"q");
    check(missing.result.end==app::DoorEnd::launch_failed&&missing.result.diagnostic.find("missing_prepare_Bin_Dosemu")!=std::string::npos&&missing.result.cleanup_complete,"missing runtime actionable refusal");
    // The pacing cases above set a startup override. Clear only this fresh
    // fixture's values so this case actually inherits the shared DTR promise.
    std::ofstream(root/"games/sfxexec.ini")<<"; Inherit shared settings for the DTR refusal test.\n";
    std::ofstream(root/"bin/sfxexec.ini")<<"CanDisconnect=true\n";const auto dtr=run(door,caller,options,"q");
    check(dtr.result.end==app::DoorEnd::launch_failed&&dtr.result.diagnostic.find("dtr_unavailable")!=std::string::npos,"unsupported DOS DTR promise refused");
    std::ofstream(root/"bin/sfxexec.ini")<<"CanDisconnect=false\n";
    fs::copy(argv[2],root/"bin/dosemu",fs::copy_options::recursive|fs::copy_options::copy_symlinks);
    const auto discovered=run(door,caller,host::DoorOptions{},"auto q");report(discovered,"default Bin/Dosemu discovery");
    check(discovered.result.end==app::DoorEnd::returned&&discovered.result.cleanup_complete&&discovered.output.find("auto DOS RETURN")!=std::string::npos,"default options actually execute discovered runtime");
    const auto retained=run(door,caller,options,"k");report(retained,"DOS-created private data");
    check(retained.result.end==app::DoorEnd::returned&&!retained.result.cleanup_complete,"unknown private file reports incomplete cleanup");
    unsigned kept{};for(const auto& entry:fs::recursive_directory_iterator(root/"node/1"))if(entry.is_regular_file()){
        check(entry.path().filename()=="keep.txt"||entry.path().filename()=="KEEP.TXT","only unknown DOS-created file remains");
        std::ifstream file(entry.path());std::string text((std::istreambuf_iterator<char>(file)),{});check(text=="Preserve DOS-created data\r\n","DOS-created file preserved exact");++kept;
    }
    check(kept==1&&!fs::exists(root/"node/1/DOOR.SYS")&&!fs::exists(root/"node/1/SFXDOOR.LCK"),"unknown file preserved while owned drop and lock removed");
    std::cout<<"PASS "<<checks<<" actual DOS DoorChannel checks; not a full Linux BBS or classic-game qualification\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
