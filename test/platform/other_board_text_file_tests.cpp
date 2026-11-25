#include "sfx/platform/win32/other_board_text_file.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>

namespace app=sfx::application;namespace win=sfx::platform::win32;namespace ins=win::installation;
unsigned checks=0;
void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
std::string bytes(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void fixture_write(const std::filesystem::path& path,std::string_view data) {
    std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(data.data(),data.size());check(file.good(),"owner edit fixture saved");
}
int main(int argc,char** argv){try {
    check(argc==2,"isolated parent required");
    const auto root=std::filesystem::absolute(argv[1])/std::filesystem::path(ins::unique_id());
    std::filesystem::create_directories(root/L"Config");
    const auto owner=win::current_process_user_sid_string();win::WindowsStorageDaclPolicy security(owner);
    check(security.apply(root/L"Config",win::StorageDaclObjectKind::directory).succeeded(),"private conf");
    win::initialize_other_board_text(root,owner);const auto path=root/L"Config"/L"otherbbs.txt";
    check(bytes(path).empty(),"transition creates exact empty file, no import");
    std::vector<app::OtherBoardLogRecord> logs;
    win::WindowsOtherBoardTextStorage storage(root,owner,[&](const auto& log){logs.push_back(log);return true;});
    app::ClassicInformationActor actor{true,0,0,1791150000,{}};actor.event_id.bytes[0]=1;
    app::ClassicInformationChange add;add.values={"One","bbs.example:23",""};
    for(unsigned n=1;n<=4;++n){add.values.name="Board "+std::to_string(n);const auto changed=storage.change(add,actor,"Installation owner");
        if(!changed)throw std::runtime_error(changed.status.operation);
        check(changed.value && storage.publish_log(*changed.value),"add and log captured");}
    const auto listed=storage.read(0);check(listed && listed.value->entries.size()==4 && listed.value->entries.back().id==4,"initial consecutive entries");
    const auto old=listed.value->entries.back();
    for(unsigned n=0;n<3;++n){const auto current=storage.read(0).value->entries.front();app::ClassicInformationChange remove;
        remove.action=app::ClassicInformationAction::remove_board;remove.id=current.id;remove.expected_revision=current.revision;
        check(storage.change(remove,actor,"Installation owner").succeeded(),"remove first current entry");}
    const auto remaining=storage.read(0).value->entries;
    check(remaining.size()==1 && remaining.front().id==1 && remaining.front().values.name=="Board 4","old number4 becomes1");
    app::ClassicInformationChange stale{app::ClassicInformationAction::remove_board,old.id,old.revision};
    const auto before=bytes(path);check(!storage.change(stale,actor,"Installation owner") && bytes(path)==before,"stale numbered selection does not remove another entry");
    win::initialize_other_board_text(root,owner);check(bytes(path)==before,"future initialization preserves existing list");
    {
        auto reader=ins::open_object(path,false,GENERIC_READ,FILE_SHARE_READ);
        const auto refusal=storage.change(add,actor,"Installation owner");
        check(!refusal && bytes(path)==before,"replacement blocked by another reader preserves original list");
    }
    fixture_write(path,"Name: Manually edited\r\nTelnet: edited.example:2323\r\n\r\n");
    check(storage.read(0).value->entries.front().values.name=="Manually edited","manual edit reloads without restart");
    stale.id=1;stale.expected_revision=remaining.front().revision;
    check(!storage.change(stale,actor,"Installation owner"),"manual edit invalidates delete selection");
    fixture_write(path,"Name: Invalid without address\r\n");const auto invalid=bytes(path);
    check(!storage.read(0) && !storage.change(add,actor,"Installation owner") && bytes(path)==invalid,"invalid file reported and preserved, not emptied");
    win::initialize_other_board_text(root,owner);check(bytes(path)==invalid,"repair initializer preserves malformed owner content");
    fixture_write(path,"");
    std::vector<std::thread> callers;std::atomic<bool> all_saved{true};std::mutex errors;std::string error;
    for(unsigned n=0;n<4;++n)callers.emplace_back([&]{for(unsigned k=0;k<9;++k){auto change=add;change.values.name="Concurrent";
        const auto saved=storage.change(change,actor,"Installation owner");
        if(!saved){all_saved=false;std::lock_guard<std::mutex> lock(errors);error=saved.status.operation;}}});
    for(auto& caller:callers)caller.join();
    if(!all_saved)throw std::runtime_error("concurrent add: "+error);
    check(all_saved,"all concurrent additions committed");
    const auto first=storage.read(0);const auto second=storage.read(32);
    check(first.value->entries.size()==32 && first.value->has_more && second.value->entries.size()==4 &&
        second.value->entries.front().id==33 && !second.value->has_more,"serialized caller additions, consecutive cross-page numbers");
    std::cout<<"PASS "<<checks<<" Other BBS text file checks; fixture retained "<<root.string()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
