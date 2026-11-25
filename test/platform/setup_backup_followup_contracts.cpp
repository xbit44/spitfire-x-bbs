// Real archive engine, isolated generated files only. No installation or network.
#include "sfx/archive/archive_engine.hpp"
#include "../../src/apps/setup/maintenance_progress.hpp"
#include "../../src/apps/setup/planning_task.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
namespace arc=sfx::archive;namespace fs=std::filesystem;
static unsigned checks{};
static void check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
static void put(const fs::path& path,const std::string& data){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out.write(data.data(),data.size());check(out.good(),"owned fixture write");}
static std::string get(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
static void progress_layout_checks(){
    for(const auto dpi:{96,144,192})for(const auto* face:{L"Segoe UI",L"Tahoma"}){
        HDC dc=CreateCompatibleDC(nullptr);check(dc!=nullptr,"owned progress measurement DC");
        HFONT font=CreateFontW(-MulDiv(9,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,face);
        check(font!=nullptr,"Setup font at diagnostic DPI");const auto old=SelectObject(dc,font);TEXTMETRICW metrics{};check(GetTextMetricsW(dc,&metrics)!=FALSE,"native progress font metrics");
        const auto measure=[&](const std::wstring& message){
            // The shipping resource's IDC_PROGRESS is340x37 dialog units. Both
            // font axes scale together, as in the native dialog manager.
            SIZE alphabet{};const wchar_t letters[]=L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            check(GetTextExtentPoint32W(dc,letters,52,&alphabet)!=FALSE,"native dialog horizontal units");
            const auto base=(alphabet.cx/26+1)/2;RECT bounds{0,0,MulDiv(340,base,4),0};
            check(DrawTextW(dc,message.c_str(),-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX)>0&&bounds.bottom<=MulDiv(37,metrics.tmHeight,8),"stage byte/file/elapsed text fits existing compact Setup area");
            check(message.find(L'%')==std::wstring::npos&&message.find(L"ETA")==std::wstring::npos,"progress never invents percentage or ETA");
        };
        for(const auto stage:{arc::BackupStage::inventory,arc::BackupStage::writing,arc::BackupStage::verification,arc::BackupStage::publication}){
            std::wstring output;sfx::apps::setup::MaintenanceProgress progress([&](const auto& value){output=value;});
            progress.backup({stage,138412032,stage==arc::BackupStage::inventory?0ULL:138412032ULL,1500,stage==arc::BackupStage::inventory?0ULL:1500ULL});
            check(output.find(L"Total since confirmation:")!=std::wstring::npos,"backup progress identifies whole operation elapsed scope");measure(output);
        }
        std::wstring output;sfx::apps::setup::MaintenanceProgress progress([&](const auto& value){output=value;});progress.retention(3,3);measure(output);
        check(output.find(L"Existing archives checked: 3 / 3")!=std::wstring::npos,"counter identifies only existing archives");
        for(auto stage:{sfx::platform::win32::BackupFinalizationStage::published_archive,sfx::platform::win32::BackupFinalizationStage::ownership,sfx::platform::win32::BackupFinalizationStage::dependencies,sfx::platform::win32::BackupFinalizationStage::retirement}){progress.finalization(stage);check(output.find(L"3 / 3")==output.npos&&output.find(L"Total since confirmation:")!=output.npos,"later real stage clears completed old-archive counter");measure(output);}
        for(auto stage:{sfx::apps::setup::PlanningPhase::embedded_payload,sfx::apps::setup::PlanningPhase::location,sfx::apps::setup::PlanningPhase::receipt,sfx::apps::setup::PlanningPhase::installed_files,sfx::apps::setup::PlanningPhase::source_recognition,sfx::apps::setup::PlanningPhase::details})measure(sfx::apps::setup::planning_status(stage,12000));
        progress.status(L"Update committed and verified. Final recovery or backup checks did not finish. Keep the remaining recovery and backup files; review them before another update.");measure(output);
        SelectObject(dc,old);check(DeleteObject(font)&&DeleteDC(dc),"progress GDI resources released");
    }
}
static arc::CreateBackupRequest request(const fs::path& root){
    arc::CreateBackupRequest r;r.source_root=root/"source";r.partial_path=root/"point.partial";r.destination_path=root/"point.zip";
    r.metadata={"setup-upgrade","0123456789ABCDEF0123456789ABCDEF","1.0.0-alpha.2","1",{"20261023-120000","2026-10-23T12:00:00","+00:00","2026-10-23T12:00:00Z"}};
    r.compression_level=1;r.store_clamav_definitions=true;
    put(r.source_root/"Data/ClamAV/daily.cvd",std::string(2*1024*1024,'x'));
    put(r.source_root/"Config/test.txt","settings retained");
    put(root/"selected-content.bin","selected File Area bytes");r.source_overrides={{"SFX-FILE-CONTENTS/1/test.bin",root/"selected-content.bin"}};return r;
}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--layout-only"){progress_layout_checks();std::cout<<"PASS setup feedback native font/layout and phase/counter checks="<<checks<<"; no window/installation\n";return 0;}
    check(argc==2,"fresh evidence directory argument");const auto root=fs::absolute(argv[1]);
    check(root.parent_path().filename()=="section11-owner-followup-20261023"&&!fs::exists(root),"fresh bounded fixture identity");fs::create_directories(root);
    progress_layout_checks();
    for(const auto stage:{arc::BackupStage::inventory,arc::BackupStage::writing,arc::BackupStage::verification,arc::BackupStage::publication}){
        auto r=request(root/("cancel-"+std::to_string(static_cast<unsigned>(stage))));bool cancel{};unsigned events{};
        r.cancelled=[&]{return cancel;};r.backup_progress=[&](const auto& event){++events;if(event.stage==stage)cancel=true;};
        const auto result=arc::create_backup(r);check(result.error==arc::Error::cancelled&&events,"cancellation reaches actual backup phase");
        check(!fs::exists(r.destination_path)&&!fs::exists(r.partial_path),"cancelled operation never publishes success or incomplete ZIP");
        check(get(r.source_root/"Config/test.txt")=="settings retained","backup cancellation leaves original data exact");
    }
    auto r=request(root/"complete");std::set<arc::BackupStage> stages;std::uint64_t written{},verified{};
    r.backup_progress=[&](const auto& event){stages.insert(event.stage);if(event.stage==arc::BackupStage::writing){check(event.completed_bytes>=written&&event.completed_bytes<=event.total_bytes,"writing counters truthful and monotonic");written=event.completed_bytes;}if(event.stage==arc::BackupStage::verification){check(event.completed_bytes>=verified&&event.completed_bytes<=event.total_bytes,"verification counters truthful and monotonic");verified=event.completed_bytes;}};
    check(arc::create_backup(r).ok()&&stages.size()==4&&written==verified,"all four production backup phases complete full payload");
    const auto inspection=arc::inspect_backup(r.destination_path);check(inspection.result.ok()&&inspection.files.size()==3,"definitions core and selected contents are fully covered");
    arc::ExtractRequest restore;restore.archive_path=r.destination_path;restore.destination_root=root/"restored";restore.skip_backup_control_files=true;
    check(arc::extract_zip(restore).ok(),"compatible ZIP restores with shipping extractor");
    check(get(restore.destination_root/"Data/ClamAV/daily.cvd")==std::string(2*1024*1024,'x')&&get(restore.destination_root/"SFX-FILE-CONTENTS/1/test.bin")=="selected File Area bytes","restored definitions and selected file contents exact");
    check(arc::create_backup(r).error==arc::Error::destination_exists,"existing backup cannot be replaced");
    auto changed=request(root/"source-changed");bool edited{};changed.backup_progress=[&](const auto& event){if(event.stage==arc::BackupStage::writing&&!edited){edited=true;put(changed.source_root/"Config/test.txt","source changed after hashing");}};
    check(!arc::create_backup(changed).ok()&&!fs::exists(changed.destination_path),"source mutation after inventory refuses publication");
    const auto damaged=root/"damaged.zip";fs::copy_file(r.destination_path,damaged);
    {std::fstream file(damaged,std::ios::in|std::ios::out|std::ios::binary);file.seekp(static_cast<std::streamoff>(fs::file_size(damaged)/2));file.put('!');}
    check(!arc::inspect_backup(damaged).result.ok(),"tampered ZIP fails full verification");
    bool cancel_verify{};const auto stopped=arc::inspect_backup(r.destination_path,{},[&]{return cancel_verify;},[&](const auto&){cancel_verify=true;});
    check(stopped.result.error==arc::Error::cancelled,"standalone published/retention verification is cancellable inside large members");
    auto blocked=request(root/"write-refused");fs::create_directory(blocked.partial_path);
    check(!arc::create_backup(blocked).ok()&&!fs::exists(blocked.destination_path)&&fs::is_directory(blocked.partial_path),"unavailable write destination is preserved with no false backup");
    std::cout<<"PASS "<<checks<<" native backup follow-up checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
