#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include <fstream>
#include <iostream>

namespace app=sfx::application;namespace win=sfx::platform::win32;namespace ins=win::installation;
std::string bytes(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
int main(){try {
    const std::filesystem::path root(L"C:\\xtest-sfx-section7-followup-qualified-20261005");
    const auto owner=ins::operator_sid();
    ins::verify_runtime_installation(root,owner);
    check(ins::read_receipt(root,owner).generation==3,"completed disposable Setup fixture required");
    const auto path=root/L"Config"/L"otherbbs.txt";const auto original=bytes(path);
    const auto snapshot=win::offline_classic_information_snapshot(root,owner,0);
    check(snapshot.code==app::LocalAccountResultCode::ok && snapshot.boards.entries.size()==1 &&
        snapshot.boards.entries[0].values.name=="Lifecycle Board","offline Config reads authoritative preserved text");
    const auto saved=snapshot.boards.entries[0];
    app::ClassicInformationChange edit{app::ClassicInformationAction::edit_board,saved.id,saved.revision,{"Offline Lifecycle Board","lifecycle.example:2323",""},{}};
    check(win::offline_change_classic_information(root,owner,edit).committed,"offline Config edits text through ordinary owner boundary");
    check(!win::offline_change_classic_information(root,owner,edit).committed,"offline stale edit refused without retarget");
    app::ClassicInformationChange add;add.values={"Offline Disposable","offline.example:23",""};
    check(win::offline_change_classic_information(root,owner,add).committed,"offline Config adds text");
    auto current=win::offline_classic_information_snapshot(root,owner,0);
    check(current.boards.entries.size()==2 && current.boards.entries[1].id==2,"offline text uses consecutive numbers");
    const auto row=current.boards.entries[1];
    app::ClassicInformationChange remove{app::ClassicInformationAction::remove_board,row.id,row.revision,{},{}};
    check(win::offline_change_classic_information(root,owner,remove).committed,"offline Config removes text");
    current=win::offline_classic_information_snapshot(root,owner,0);
    edit.expected_revision=current.boards.entries[0].revision;edit.values=saved.values;
    check(win::offline_change_classic_information(root,owner,edit).committed && bytes(path)==original,"offline checks restore original fixture text exactly through approved saves");
    const auto callers=bytes(root/L"Logs"/L"CALLERS.LOG"),attention=bytes(root/L"Logs"/L"HEYSYSOP.LOG");
    check(callers.find("BBS: Offline Disposable")!=callers.npos && attention.find("BBS: Offline Disposable")!=attention.npos,"offline add/remove logs emitted to approved files");
    check(callers.find("Actor: Installation owner (installation owner)")!=callers.npos &&
        attention.find("Actor: Installation owner (installation owner)")!=attention.npos,"offline logs identify installation owner, not invented caller");
    std::cout<<"PASS "<<checks<<" offline Other BBS Config checks; original text restored; fixture stopped\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
