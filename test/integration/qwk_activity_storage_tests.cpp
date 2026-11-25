#include "sfx/application/menu_session.hpp"
#define main retained_qwk_storage_main
#include "qwk_storage_tests.cpp"
#undef main
#include "../../src/apps/runtime/qwk_download_preparation.hpp"
#include "../../src/apps/runtime/qwk_preparation_activity.hpp"
#include "../../src/apps/runtime/qwk_runtime_authority.hpp"
#include <fstream>
#include <set>
int main(int argc,char** argv)try{
    check(argc==3,"existing new synthetic fixture and new result directory required");const auto root=std::filesystem::absolute(argv[1]),output=std::filesystem::absolute(argv[2]);check(root.generic_string().find("beta3-20261112-r1")!=std::string::npos&&std::filesystem::is_regular_file(root/"SFX.DB")&&!std::filesystem::exists(output),"only preserved beta3 synthetic fixture family");std::filesystem::create_directories(output);
    storage::SqliteStorageSettings settings;settings.data_directory=root;settings.utc_now=[]{return std::int64_t{1794484800};};sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(settings,paths);check(db.start().succeeded(),"current production storage opens synthetic FTN fixture");
    const app::MessageActor actor{id(3),3,1,1};const auto policy=db.qwk_caller_policy(actor);check(bool(policy),"fresh ordinary caller policy");const app::QwkTransferRequest request{actor,policy.value->board_id,policy.value->revision};
    const auto caller=std::this_thread::get_id();const auto cancelled=[&]{check(std::this_thread::get_id()==caller,"real live authorization stays on caller thread");return !sfx::apps::runtime::qwk_runtime_authorized(db,actor.session,actor.account,request);};
    app::QwkPacketMetadata meta;meta.board_name="Synthetic profile";meta.sysop="Synthetic";meta.caller_name="Bob";meta.created="11-12-2026,12:00:00";const auto date=[](std::int64_t){return sfx::archive::QwkRecordDate{"11-12-26","12:00","1794484800"};};
    const auto prepare=[&](auto stop,auto progress){return sfx::apps::runtime::prepare_caller_qwk_download(db,actor,id(40000),meta,date,std::move(stop),std::move(progress));};
    const auto before_read=scalar(root/"SFX.DB","SELECT count(*) FROM sfx_caller_echo_state"),before_delivery=scalar(root/"SFX.DB","SELECT count(*) FROM sfx_qwk_continuation");
    const auto start=std::chrono::steady_clock::now();const auto plain=prepare(cancelled,sfx::archive::QwkPreparationObserver{});const auto middle=std::chrono::steady_clock::now();check(plain.result.ok()&&plain.progress,"unanimated current core baseline ready");
    std::string wire;std::set<std::string> frames;unsigned phases{};
    const auto active=sfx::apps::runtime::prepare_qwk_with_activity(prepare,cancelled,[&](const auto&){check(std::this_thread::get_id()==caller,"real phase callback caller thread");++phases;return true;},[&](std::string_view bytes){check(std::this_thread::get_id()==caller,"real activity output caller thread");wire+=bytes;if(bytes.find('#')!=std::string_view::npos)frames.emplace(bytes);return true;},80);
    const auto end=std::chrono::steady_clock::now();check(active.result.ok()&&active.progress&&active.progress->delivered.size()==2335&&active.zip==plain.zip,"real FTN packet bytes/order/references unchanged by activity pump");check(frames.size()>=3&&phases>=8&&wire.size()>=26&&wire.substr(wire.size()-26)=="\r"+std::string(24,' ')+"\r","sustained actual work activity and complete final clearing");check(!active.progress->cancelled(),"real packet has restored caller lifetime proof after worker join");
    check(scalar(root/"SFX.DB","SELECT count(*) FROM sfx_caller_echo_state")==before_read&&scalar(root/"SFX.DB","SELECT count(*) FROM sfx_qwk_continuation")==before_delivery,"preparation/decline still changes no read or delivery pointers");
    std::ofstream(output/"activity.bin",std::ios::binary)<<wire;std::ofstream(output/"packet.qwk",std::ios::binary)<<active.zip;std::ofstream record(output/"result.json");record<<"{\"result\":\"PASS\",\"messages\":2335,\"packet_identical\":true,\"frames\":"<<frames.size()<<",\"phases\":"<<phases<<",\"baseline_seconds\":"<<std::chrono::duration<double>(middle-start).count()<<",\"animated_seconds\":"<<std::chrono::duration<double>(end-middle).count()<<"}\n";record.close();check(db.shutdown().succeeded(),"synthetic fixture closes cleanly");std::cout<<"PASS QWK activity actual SQLite/FTN2335 "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL QWK activity storage after "<<checks<<": "<<e.what()<<'\n';return 1;}
