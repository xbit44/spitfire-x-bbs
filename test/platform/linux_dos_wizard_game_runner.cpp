// Real owner-supplied OGSF game through the wizard and default runtime selection.
// This is a synthetic caller, not an installed BBS/SSH/Telnet acceptance test.
#include "sfx/platform/linux/dos_setup.hpp"
#include "linux_web_dos_setup.hpp"
#include <sodium.h>
#include <unistd.h>
#include <poll.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <ctime>
#include <cerrno>
#include <array>
#include <algorithm>
namespace host=sfx::platform::linux_host;namespace app=sfx::application;
namespace web=sfx::apps::runtime::web;namespace fs=std::filesystem;
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void private_dir(const fs::path& p){fs::create_directories(p);fs::permissions(p,fs::perms::owner_all);}
std::string sha256(const fs::path& p){std::ifstream f(p,std::ios::binary);need(bool(f),"game_input_unreadable");std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(f),{}};std::array<unsigned char,32> hash{};need(sodium_init()>=0,"crypto_unavailable");crypto_hash_sha256(hash.data(),bytes.data(),bytes.size());std::array<char,65> hex{};sodium_bin2hex(hex.data(),hex.size(),hash.data(),hash.size());return hex.data();}
int main(int argc,char** argv){try{
    need(argc==7&&geteuid()!=0,"fresh root, runtime prefix, SFGUESS.EXE, mode, node and alias required as a normal account");
    const fs::path root=argv[1],prefix=argv[2],game=argv[3];const std::string mode=argv[4],alias=argv[6];const auto node=std::stoul(argv[5]);
    need((mode=="normal"||mode=="disconnect"||mode=="operator"||mode=="deadline")&&(node==1||node==2),"test_mode_or_node_invalid");
    need(!fs::exists(root)&&root.is_absolute(),"fresh_absolute_game_root_required");
    constexpr auto original="ced1703f126baf7322ccf3597767a1f7608d48f46233e26b83b6a8af6b6f6432";need(sha256(game)==original,"original_game_identity_mismatch");
    private_dir(root);for(const auto* folder:{"config","management","bin","games"})private_dir(root/folder);
    fs::copy_file(game,root/"games/SFGUESS.EXE");host::ensure_door_nodes(root,2);
    web::LinuxDosSetup wizard(root);
    const auto binary=prefix/"usr/libexec/dosemu2/dosemu2.bin";
    wizard.start({{"action","check"},{"runtime",{{"binary",binary.string()},{"root",prefix.string()}}}});
    const auto check_limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);auto status=wizard.status();
    while(status.at("active").get<bool>()){
        need(std::chrono::steady_clock::now()<check_limit,"wizard_runtime_check_timed_out");std::this_thread::sleep_for(std::chrono::milliseconds(20));status=wizard.status();
    }
    std::cerr<<"SFX_WIZARD_CHECK "<<status.dump()<<'\n'<<std::flush;
    need(status.at("checked").get<bool>()&&status.at("ok").get<bool>(),"wizard_runtime_check_failed");
    const auto saved=wizard.save({{"job",status.at("job")},{"revision",status.at("check_revision")}});
    const auto inspected=wizard.inspect_game({{"directory",(root/"games").string()}});
    wizard.prepare_game({{"directory",(root/"games").string()},{"driver_identity",inspected.at("driver_identity")},{"runtime_revision",saved.at("revision")}});
    std::cerr<<"SFX_WIZARD_PREPARED revision="<<saved.at("revision")<<"\n"<<std::flush;
    app::DoorDefinition door;door.id=1;door.code="OGSFGUESS";door.name="Original OGSF Guess a Number";door.enabled=true;
    door.kind=app::DoorExecutionKind::local;door.program.type=app::DoorProgramType::dos;door.program.io=app::DoorIo::uart_only;
    door.program.startup_directory=(root/"games").string();door.program.command_line="SFGUESS.EXE";
    door.program.drop_file=app::DoorDropFile::sfdoors_dat;door.program.drop_location=app::DoorDropLocation::startup;
    app::DoorDropContext caller;caller.account=42;caller.node=static_cast<std::uint16_t>(node);caller.nodes=2;caller.alias=alias;caller.user_key="42";
    caller.board="SFX Wizard Real Game Test";caller.security=10;caller.remaining_seconds=120;caller.ansi=true;
    const auto now=std::time(nullptr);const auto utc=*std::gmtime(&now);std::array<char,32> date{},clock{};std::strftime(date.data(),date.size(),"%m/%d/%y",&utc);std::strftime(clock.data(),clock.size(),"%H:%M",&utc);
    caller.date=date.data();caller.clock=clock.data();caller.logon_date=caller.date;caller.logon_clock=caller.clock;
    bool captured{};const auto began=std::chrono::steady_clock::now();app::DoorChannel channel;
    channel.read=[](std::size_t maximum){
        pollfd p{STDIN_FILENO,POLLIN,0};const auto ready=poll(&p,1,0);
        if(ready<0&&errno!=EINTR)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};
        if(ready<=0)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};
        std::vector<std::uint8_t> bytes(std::min<std::size_t>(maximum,4096));const auto n=read(STDIN_FILENO,bytes.data(),bytes.size());
        if(n<=0)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};
        bytes.resize(static_cast<std::size_t>(n));return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(bytes)};
    };
    channel.write=[&](std::string_view bytes){
        if(!captured&&fs::exists(root/"games/SFDOORS.DAT")){fs::copy_file(root/"games/SFDOORS.DAT",root/"observed-SFDOORS.DAT");captured=true;}
        const auto n=write(STDOUT_FILENO,bytes.data(),bytes.size());return app::InteractiveStreamWriteResult{n<0?app::InteractiveStreamOutcome::disconnected:app::InteractiveStreamOutcome::completed,n>0?static_cast<std::size_t>(n):0};
    };
    channel.stop=[&]()->std::optional<app::DoorEnd>{
        const auto elapsed=std::chrono::steady_clock::now()-began;
        if(mode=="operator"&&elapsed>std::chrono::seconds(3))return app::DoorEnd::operator_terminated;
        if(mode=="deadline"&&elapsed>std::chrono::seconds(2))return app::DoorEnd::calling_time;
        if(elapsed>std::chrono::seconds(45))return app::DoorEnd::calling_time;
        return {};
    };
    // Intentionally no runtime or serial-pacing override: this is the wizard's
    // saved selection and the installed caller's default adapter options.
    const auto result=host::run_local_door(root,door,caller,channel);
    const auto expected=mode=="operator"?app::DoorEnd::operator_terminated:mode=="deadline"?app::DoorEnd::calling_time:mode=="disconnect"?app::DoorEnd::caller_disconnected:app::DoorEnd::returned;
    const bool original_kept=sha256(root/"games/SFGUESS.EXE")==original;
    const bool drops_removed=!fs::exists(root/"games/SFDOORS.DAT")&&!fs::exists(root/"games/SFXDOOR.LCK");
    nlohmann::json report={{"end",app::door_result_code(result.end)},{"exit",result.exit_code},{"clean",result.cleanup_complete},{"captured_drop",captured},{"drops_removed",drops_removed},{"game_unchanged",original_kept},{"diagnostic",result.diagnostic},{"node",node},{"alias",alias},{"backend_pacing_ms",0},{"saved_runtime_used",true}};
    std::cerr<<"SFX_GAME_RESULT "<<report.dump()<<'\n'<<std::flush;
    std::ofstream(root/"runner-result.json")<<report.dump(2)<<'\n';
    return result.end==expected&&result.cleanup_complete&&captured&&drops_removed&&original_kept&&(mode!="normal"||result.exit_code==0)?0:1;
}catch(const std::exception& error){std::cerr<<"SFX_GAME_FAILURE "<<error.what()<<'\n';return 1;}}
