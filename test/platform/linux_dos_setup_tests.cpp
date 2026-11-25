#include "sfx/platform/linux/dos_setup.hpp"
#include "linux_web_dos_setup.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace fs=std::filesystem;namespace host=sfx::platform::linux_host;namespace app=sfx::application;namespace web=sfx::apps::runtime::web;
unsigned checks{};
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F>void refused(F action,const char* message){bool caught{};try{action();}catch(const std::exception&){caught=true;}check(caught,message);}
void put(const fs::path& p,std::string_view s){std::ofstream f(p);f<<s;f.close();fs::permissions(p,fs::perms::owner_read|fs::perms::owner_write);}
std::string read(const fs::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
void directory(const fs::path& p){fs::create_directories(p);fs::permissions(p,fs::perms::owner_all);}
nlohmann::json finished(web::LinuxDosSetup& manager){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);for(;;){auto s=manager.status();if(!s.at("active").get<bool>())return s;check(std::chrono::steady_clock::now()<deadline,"background job bounded");std::this_thread::sleep_for(std::chrono::milliseconds(10));}}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"fresh unprivileged fixture arguments");const fs::path root=argv[1],fixture=argv[2];check(!fs::exists(root),"fixture must be fresh");directory(root);
    for(const auto* d:{"config","management","bin","games"})directory(root/d);
    fs::permissions(root/"games",fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
    const auto prefix=root/"Runtime";for(const auto* d:{"usr/libexec/dosemu2","usr/lib/x86_64-linux-gnu","usr/lib/dosemu","usr/share/dosemu","usr/share/comcom32","usr/share/fdpp"})directory(prefix/d);
    const auto binary=prefix/"usr/libexec/dosemu2/dosemu2.bin";fs::copy_file(fixture,binary);fs::permissions(binary,fs::perms::owner_all);
    host::DoorOptions proposed;proposed.dosemu_root=prefix;proposed.dosemu_binary=binary;
    check(host::read_dos_runtime_profile(root).revision==0,"legacy board has no forced selection");
    web::LinuxDosSetup manager(root);refused([&]{manager.start({{"action","check"},{"runtime",{{"binary",binary.string()},{"root",prefix.string()}}},{"extra",true}});},"unknown request fields refused");
    manager.start({{"action","check"},{"runtime",{{"binary",binary.string()},{"root",prefix.string()}}}});auto status=finished(manager);
    check(status.at("ok")==true&&status.at("checked")==true&&status.at("version").get<std::string>().find("test fixture")!=std::string::npos,"actual pinned ELF version probe completes off request path");
    refused([&]{manager.save({{"job","bad"},{"revision","0"}});},"save requires exact checked job");
    auto saved=manager.save({{"job",status.at("job")},{"revision","0"}});check(saved.at("revision")=="1","checked selection saved once");
    const auto profile=host::read_dos_runtime_profile(root);check(profile.revision==1&&profile.options.dosemu_binary==binary&&profile.options.dosemu_root==prefix&&profile.options.dosemu_sha256.size()==64,"normal caller reads exact persisted binary/prefix/identity");
    refused([&]{manager.save({{"job",status.at("job")},{"revision","0"}});},"replayed revision cannot rewrite selection");
    put(root/"games"/"GAME.EXE","game data, not executed");put(root/"games"/"sfxexec.ini","; Keep me\r\nYieldInterval=0.5\r\nCanDisconnect=true\r\n");
    fs::create_symlink("GAME.EXE",root/"games"/"LINK.EXE");auto game=host::inspect_dos_game(root,root/"games");check(game.driver_change&&game.programs.size()==1&&game.programs[0]=="GAME.EXE","game scan is bounded and ignores executable links");
    const auto before=read(root/"games"/"sfxexec.ini");refused([&]{host::prepare_dos_game_driver(root,root/"games","stale");},"changed driver identity refused");check(read(root/"games"/"sfxexec.ini")==before,"stale request leaves bytes exact");
    manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",game.driver_identity},{"runtime_revision","1"}});
    const auto prepared=host::inspect_dos_game(root,root/"games");const auto settings=app::parse_door_driver_settings("",read(root/"games"/"sfxexec.ini"));
    check(!prepared.driver_change&&settings&&!settings->can_disconnect&&settings->yield_interval_ms==0.5&&read(root/"games"/"sfxexec.ini").find("Keep me")!=std::string::npos,"required Linux setting prepared while preserving other settings/comments");
    refused([&]{host::inspect_dos_game(root,root/"games"/".."/"games");},"path aliases refused");
    refused([&]{manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",prepared.driver_identity},{"runtime_revision","0"}});},"game preparation binds runtime revision");
    const auto pacing_preview=manager.inspect_game({{"directory",(root/"games").string()},{"input_interval_ms",75}});
    check(pacing_preview.at("input_interval_ms")==75&&pacing_preview.at("driver_change")==true&&read(root/"games/sfxexec.ini").find("DosInputIntervalMs")==std::string::npos,"pacing preview is explicit and read-only");
    const auto paced_saved=manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",prepared.driver_identity},{"runtime_revision","1"},{"input_interval_ms",75}});
    const auto paced_text=read(root/"games/sfxexec.ini");const auto paced_settings=app::parse_door_driver_settings("",paced_text);
    check(paced_saved.at("input_interval_ms")==75&&paced_settings&&paced_settings->dos_input_interval_ms==75&&paced_settings->yield_interval_ms==0.5&&!paced_settings->can_disconnect&&paced_text.find("Keep me")!=std::string::npos,"per-game pacing saves while preserving unrelated settings and comments");
    refused([&]{manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",prepared.driver_identity},{"runtime_revision","1"},{"input_interval_ms",0}});},"stale driver revision cannot change pacing");
    check(read(root/"games/sfxexec.ini")==paced_text,"stale pacing save leaves exact bytes");
    const auto kept=manager.inspect_game({{"directory",(root/"games").string()}});
    check(kept.at("input_interval_ms")==75&&!kept.at("driver_change").get<bool>(),"omitted pacing field reads saved value without a rewrite");
    manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",kept.at("driver_identity")},{"runtime_revision","1"}});
    check(read(root/"games/sfxexec.ini")==paced_text,"omitted pacing field preserves saved bytes");
    directory(root/"OtherGame");put(root/"OtherGame/OTHER.EXE","different game");
    check(manager.inspect_game({{"directory",(root/"OtherGame").string()}}).at("input_interval_ms")==0,"one game's pacing does not change another game's default");
    for(const auto& bad:std::vector<nlohmann::json>{-1,101,75.5,"75",nullptr,true}){
        refused([&]{manager.inspect_game({{"directory",(root/"games").string()},{"input_interval_ms",bad}});},"invalid pacing preview refused");
        refused([&]{manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",kept.at("driver_identity")},{"runtime_revision","1"},{"input_interval_ms",bad}});},"invalid pacing save refused");
        check(read(root/"games/sfxexec.ini")==paced_text,"invalid pacing leaves exact game settings");
    }
    refused([&]{host::prepare_dos_game_driver(root,root/"games",kept.at("driver_identity").get<std::string>(),101);},"direct platform pacing preparation is bounded");
    const auto disabled=manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",kept.at("driver_identity")},{"runtime_revision","1"},{"input_interval_ms",0}});
    check(disabled.at("input_interval_ms")==0&&app::parse_door_driver_settings("",read(root/"games/sfxexec.ini"))->dos_input_interval_ms==0,"explicit zero disables saved pacing");
    const auto zero_text=read(root/"games/sfxexec.ini");
    check(manager.inspect_game({{"directory",(root/"games").string()},{"input_interval_ms",0}}).at("driver_change")==false,"same zero setting is a no-op");
    refused([&]{manager.prepare_game({{"directory",(root/"games").string()},{"driver_identity",disabled.at("driver_identity")},{"runtime_revision","0"},{"input_interval_ms",75}});},"pacing save still binds current runtime revision");
    check(read(root/"games/sfxexec.ini")==zero_text,"stale runtime keeps pacing exact");
    put(root/"bin/sfxexec.ini","CanDisconnect=false\nDosInputIntervalMs=75\n");
    const auto inherited=manager.inspect_game({{"directory",(root/"OtherGame").string()}});
    check(inherited.at("input_interval_ms")==75&&!inherited.at("driver_change").get<bool>(),"shared pacing is read without an unsolicited per-game write");
    manager.prepare_game({{"directory",(root/"OtherGame").string()},{"driver_identity",inherited.at("driver_identity")},{"runtime_revision","1"},{"input_interval_ms",75}});
    put(root/"bin/sfxexec.ini","CanDisconnect=false\nDosInputIntervalMs=0\n");
    check(manager.inspect_game({{"directory",(root/"OtherGame").string()}}).at("input_interval_ms")==75,"explicit per-game choice is pinned even when originally equal to shared pacing");
    // The installed caller route uses default DoorOptions, not test overrides.
    directory(root/"bin"/"dosemu");host::ensure_door_nodes(root,1);app::DoorDefinition door;door.id=1;door.code="DOSFIX";door.name="DOS fixture";door.enabled=true;door.kind=app::DoorExecutionKind::local;
    door.program.type=app::DoorProgramType::dos;door.program.io=app::DoorIo::standard;door.program.startup_directory=(root/"games").string();door.program.command_line="GAME.EXE";door.program.drop_file=app::DoorDropFile::none;
    app::DoorDropContext caller;caller.account=1;caller.node=1;caller.nodes=1;caller.alias="Fixture";caller.board="Fixture";caller.security=999;caller.remaining_seconds=60;caller.user_key="fixture";
    std::string output;app::DoorChannel channel;channel.read=[](std::size_t){return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};channel.write=[&](std::string_view bytes){output+=bytes;return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,bytes.size()};};const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);channel.stop=[&]()->std::optional<app::DoorEnd>{if(std::chrono::steady_clock::now()>deadline)return app::DoorEnd::calling_time;return {};};
    auto result=host::run_local_door(root,door,caller,channel);check(result.end==app::DoorEnd::returned&&result.cleanup_complete&&output.find("SFX SELECTED DOS FIXTURE")!=std::string::npos,"real default launcher uses saved runtime even when another Bin/Dosemu folder exists");
    const auto config_before=read(root/"config"/"dos-runtime-linux.v1");{std::ofstream f(binary,std::ios::app);f<<"changed";}output.clear();result=host::run_local_door(root,door,caller,channel);check(result.end==app::DoorEnd::launch_failed&&output.empty()&&result.diagnostic.find("identity_mismatch")!=std::string::npos,"changed runtime cannot execute under saved identity");
    check(read(root/"config"/"dos-runtime-linux.v1")==config_before,"failed launch preserves runtime selection");fs::copy_file(fixture,binary,fs::copy_options::overwrite_existing);fs::permissions(binary,fs::perms::owner_all);
    const auto hang=prefix/"usr/libexec/dosemu2/dosemu-hang.bin";fs::copy_file(fixture,hang);fs::permissions(hang,fs::perms::owner_all);
    manager.start({{"action","check"},{"runtime",{{"binary",hang.string()},{"root",prefix.string()}}}});std::this_thread::sleep_for(std::chrono::milliseconds(50));const auto began=std::chrono::steady_clock::now();manager.cancel();status=finished(manager);
    check(status.at("ok")==false&&!status.at("checked").get<bool>()&&std::chrono::steady_clock::now()-began<std::chrono::seconds(2),"cancelled probe reaps child promptly and cannot be saved");
    check(read(root/"config"/"dos-runtime-linux.v1")==config_before,"cancel leaves saved runtime and game files exact");
    put(prefix/"script","#!/bin/sh\necho DOSEMU2\n");fs::permissions(prefix/"script",fs::perms::owner_all);proposed.dosemu_binary=prefix/"script";refused([&]{host::check_dos_runtime(root,proposed);},"wrapper script cannot be passed off as checked ELF");
    manager.reset();check(!manager.status().at("checked").get<bool>(),"restore boundary clears previous check authority");
    std::cout<<"PASS "<<checks<<" isolated DOS setup checks; synthetic runtime only, no emulator/game compatibility claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
