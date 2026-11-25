// Exact fresh RC18 installation; real shipping Panel/Runtime, loopback peer.
#include <winsock2.h>
#include "sfx/application/ftn_wire.hpp"
#include "sfx/platform/win32/ftn_crypto.hpp"
#define main retained_rc17_binkp_ui_main
#include "ftn_installed_ui_tests.cpp"
#undef main
#include "network_setup_fixture.hpp"
#include <fstream>
int main(int argc,char** argv){Product panel;try{
    check(argc==2,"fresh shipping BinkP evidence directory");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"retain earlier BinkP evidence");std::filesystem::create_directories(output);
    const auto root=win32::installation::canonical_root(network_setup_qualification_root());const auto owner=win32::installation::operator_sid();check(!win32::installation::elevated(),"ordinary shipping fixture owner");win32::installation::verify_runtime_installation(root,owner);
    const auto before=win32::offline_ftn_configuration(root,owner);check(before&&before.value,"starting isolated configuration readable");const auto original=*before.value;auto staged=original;
    staged.settings.mode=app::FtnMailerMode::native;staged.settings.listener_enabled=true;staged.settings.bind_address="127.0.0.1";staged.settings.port=26537;staged.settings.tls_listener_enabled=false;for(auto& n:staged.networks)n.enabled=false;
    app::FtnNetwork network;network.code="PG15";network.name="Owner fix regression";network.address="42:100/1@own-test";network.enabled=true;staged.networks.push_back(network);app::FtnLink link;link.network="PG15";link.code="PEER15";link.name="Synthetic peer";link.address="42:100/2@own-test";link.host="127.0.0.1";link.port=26538;link.enabled=true;link.contact_on_mail=false;link.credentials.session=*win32::protect_ftn_secret(root.u8string(),link.network,link.code,win32::FtnCredentialKind::session,"OnlySession123");staged.links.push_back(link);
    const auto operation=[] {GUID id{};check(SUCCEEDED(CoCreateGuid(&id)),"fresh synthetic owner operation");app::FtnTransferToken out{};memcpy(out.data(),&id,out.size());return out;};
    check(bool(win32::offline_change_ftn_configuration(root,owner,operation(),staged)),"bounded loopback configuration saved through owner writer");const auto raw=app::encode_ftn_configuration(original);{std::ofstream saved(output/L"before-ftn.bin",std::ios::binary);saved.write(reinterpret_cast<const char*>(raw.data()),raw.size());}
    panel=launch(root/L"Bin/SFXControlPanel.exe",L"");panel.window=wait_window(panel.pid,kWindowClass);const auto ready=GetTickCount64()+30000;while(!visible_text_contains(panel.window,L"READY")&&GetTickCount64()<ready)Sleep(20);check(visible_text_contains(panel.window,L"READY"),"shipping Runtime ready");const auto tab=GetDlgItem(panel.window,IDC_MAIN_TAB);SendMessageW(tab,WM_KEYDOWN,VK_HOME,0);SendMessageW(tab,WM_KEYDOWN,VK_RIGHT,0);check(TabCtrl_GetCurSel(tab)==1,"shipping BinkP tab selected");const auto active=GetTickCount64()+30000;while(!visible_text_contains(panel.window,L"Classic 26537")&&GetTickCount64()<active)Sleep(20);check(visible_text_contains(panel.window,L"Classic 26537"),"only isolated loopback listener active");
    WSADATA winsock{};check(WSAStartup(MAKEWORD(2,2),&winsock)==0,"bounded native socket lifetime");
#include "rc15_binkp_progress_checks.inc"
    WSACleanup();close_panel(panel);
    const auto current=win32::offline_ftn_configuration(root,owner);check(current&&current.value,"post-transfer configuration readable");staged.revision=current.value->revision;check(app::encode_ftn_configuration(staged)==app::encode_ftn_configuration(*current.value),"peer traffic changed no saved configuration");auto restored=original;restored.revision=current.value->revision;check(bool(win32::offline_change_ftn_configuration(root,owner,operation(),restored)),"test-only settings restored through owner writer; all payload/log evidence retained");
    std::cout<<"PASS "<<checks<<" shipping BinkP authenticated loopback/progress/duplicate/flat-original/log checks; no live hub contact\n";return 0;
}catch(const std::exception& error){if(panel.window){try{render_foreign_png(panel.window,std::filesystem::path(argv[1])/L"failed-panel.png");}catch(...){}try{close_panel(panel);}catch(...){}}std::cerr<<"FAIL "<<checks<<": "<<error.what()<<"; retained synthetic settings/evidence require review\n";return 1;}}
