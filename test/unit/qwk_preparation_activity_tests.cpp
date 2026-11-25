#include "../../src/apps/runtime/qwk_preparation_activity.hpp"
#include <atomic>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
namespace run=sfx::apps::runtime;namespace arc=sfx::archive;
unsigned checks{};
void check(bool okay,const char* text){++checks;if(!okay)throw std::runtime_error(text);}
arc::QwkPreparedDownload prepared(arc::CancelCheck stop){arc::QwkPreparedDownload p;p.zip="unit sentinel";p.progress.emplace();p.progress->cancelled=std::move(stop);return p;}
int main()try{
    const auto caller=std::this_thread::get_id();std::vector<std::string> output;std::set<std::string> frames;
    bool live=true;unsigned phases{},proofs{};std::atomic<bool> off_thread{false};
    const auto proof=[&]{check(std::this_thread::get_id()==caller,"live proof runs only on caller thread");++proofs;return !live;};
    const auto phase=[&](const auto&){check(std::this_thread::get_id()==caller,"phase runs only on caller thread");++phases;return true;};
    const auto write=[&](std::string_view bytes){check(std::this_thread::get_id()==caller,"all output stays on caller thread");output.emplace_back(bytes);if(bytes.find('#')!=std::string_view::npos)frames.emplace(bytes);return true;};
    const auto long_work=[&](auto stop,auto progress){off_thread=std::this_thread::get_id()!=caller;if(stop()||!progress({arc::QwkPreparationStage::scanning,{},"Synthetic","Long work"}))return arc::QwkPreparedDownload{};std::this_thread::sleep_for(std::chrono::milliseconds(1250));if(stop()||!progress({arc::QwkPreparationStage::packing,{},{},{}}))return arc::QwkPreparedDownload{};std::this_thread::sleep_for(std::chrono::milliseconds(550));return prepared(stop);};
    auto p=run::prepare_qwk_with_activity(long_work,proof,phase,write,80);
    check(p.result.ok()&&p.progress&&p.zip=="unit sentinel"&&off_thread&&phases==2&&frames.size()>=3&&proofs>=5,"sustained animation while worker is blocked, without phase-only stepping");
    check(!output.empty()&&output.back()=="\r"+std::string(24,' ')+"\r","activity is fully cleared before helper returns");
    check(!p.progress->cancelled(),"returned cancellation proof survives destroyed worker bridge");live=false;check(p.progress->cancelled(),"returned proof retains actual caller authority, not a frozen worker flag");live=true;
    const auto written=output.size();std::this_thread::sleep_for(std::chrono::milliseconds(550));check(output.size()==written,"joined worker emits no bytes after return/prompt/binary handoff");
    for(const std::size_t columns:{0U,1U,4U,6U,20U,25U,80U})for(const std::uint64_t elapsed:{0U,500U,9500U,19000U}){
        const auto f=run::qwk_activity_frame(elapsed,columns);check(f.size()<columns||columns==0,"narrow terminal never reaches wrapping column");check(std::count(f.begin(),f.end(),'#')==static_cast<std::ptrdiff_t>(std::min<std::size_t>(5,f.size())),"narrow frame keeps five hashes where they fit");if(columns>=25)check(f==sfx::application::file_scan_activity(elapsed),"full-width animation exactly reuses upload scanner pattern");
    }
    p=run::prepare_qwk_with_activity([](auto stop,auto){return prepared(stop);},proof,phase,write,20);check(p.result.ok()&&p.progress&&output.back()=="\r"+std::string(19,' ')+"\r","fast completion and plain/narrow clearing");
    p=run::prepare_qwk_with_activity([](auto,auto)->arc::QwkPreparedDownload{throw std::runtime_error("owned unit exception");},proof,phase,write,80);check(p.result.error==arc::Error::archive_io&&!p.progress&&p.zip.empty(),"worker exception joins and cannot claim prepared success");
    p=run::prepare_qwk_with_activity([](auto,auto){arc::QwkPreparedDownload r;r.result={arc::Error::archive_invalid,"unit error"};return r;},proof,phase,write,80);check(p.result.error==arc::Error::archive_invalid&&output.back()=="\r"+std::string(24,' ')+"\r","preparation error retains result and clears before notice");
    const auto cooperative=[](auto stop,auto){while(!stop())std::this_thread::sleep_for(std::chrono::milliseconds(5));return prepared(stop);};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
    p=run::prepare_qwk_with_activity(cooperative,[&]{check(std::this_thread::get_id()==caller,"deadline remains caller-owned");return std::chrono::steady_clock::now()>=deadline;},phase,write,80);check(p.result.error==arc::Error::cancelled&&p.zip.empty()&&!p.progress,"deadline wins even if worker returns stale success during cancellation");
    const auto completion_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
    p=run::prepare_qwk_with_activity([](auto stop,auto){std::this_thread::sleep_for(std::chrono::milliseconds(100));return prepared(stop);},[&]{check(std::this_thread::get_id()==caller,"final completion proof caller thread");return std::chrono::steady_clock::now()>=completion_deadline;},phase,write,80);check(p.result.error==arc::Error::cancelled&&p.zip.empty()&&!p.progress,"completion before first timer tick still refuses stale success after deadline");
    unsigned writes{};p=run::prepare_qwk_with_activity(cooperative,proof,phase,[&](auto){check(std::this_thread::get_id()==caller,"disconnect output caller thread");return ++writes<2;},80);check(p.result.error==arc::Error::cancelled&&!p.progress&&writes==2,"disconnect stops output, wakes pending worker and joins");
    p=run::prepare_qwk_with_activity([](auto stop,auto progress){if(!progress({arc::QwkPreparationStage::packing,{},{},{}}))return prepared(stop);return prepared(stop);},proof,[](const auto&){return false;},write,80);check(p.result.error==arc::Error::cancelled&&!p.progress,"phase refusal cancels without recursive proxy callback");
    p=run::prepare_qwk_with_activity(cooperative,proof,phase,[](auto)->bool{throw std::runtime_error("owned output exception");},80);check(p.result.error==arc::Error::archive_io&&!p.progress,"output exception is contained");
    unsigned rendered{};p=run::prepare_qwk_with_activity([](auto stop,auto progress){(void)progress({arc::QwkPreparationStage::packing,{},{},{}});return prepared(stop);},proof,phase,[&](auto){if(++rendered==3)throw std::runtime_error("owned active-phase exception");return true;},80);check(p.result.error==arc::Error::archive_io&&!p.progress,"active phase output exception wakes and joins waiting worker");
    live=false;const auto before=output.size();p=run::prepare_qwk_with_activity(long_work,proof,phase,write,80);check(p.result.error==arc::Error::cancelled&&output.size()==before,"initial cancellation starts neither animation nor work");
    std::cout<<"PASS QWK activity ownership/lifetime/deadline/narrow/clear "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL QWK activity after "<<checks<<": "<<e.what()<<'\n';return 1;}
